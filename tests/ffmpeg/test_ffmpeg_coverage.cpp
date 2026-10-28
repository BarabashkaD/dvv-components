// The `ffmpeg` provider cases the M4 test-coverage review found missing (Task 11-pre):
// damaged containers, and the allowlisted codecs without a test before (rawvideo in AVI, MOV
// and NUT, r210, qtrle, v410). Fixtures: tests/oracle/make-container-fixtures.cmake; the damaged
// files are made from them at run time (no binary fixture in the repository).

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/lifetime.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"
#include "ffmpeg/api.h"
#include "ffmpeg_harness.h"
#include "framemd5.h"
#include "sources/container_supplier.h"

namespace {

namespace media = dvv::media;
using dvv::components::testing::FfmpegHarness;
using dvv::components::testing::fixture;
using dvv::components::testing::fixtureUri;
using dvv::components::testing::md5Hex;
using dvv::components::testing::referenceHashes;
using dvv::components::testing::TemporaryDirectory;

using namespace std::chrono_literals;

// Longer than any open or read of these small files takes, short against the CTest timeout.
constexpr std::chrono::seconds LIMIT{30};

// `work()` on a helper thread, waited for at most LIMIT. Work that does not end is a hang: the
// process ends with a failure at once instead of blocking until the CTest timeout.
template <class Work>
std::invoke_result_t<Work&> bounded(const std::string& what, Work work) {
    using Value = std::invoke_result_t<Work&>;
    std::promise<Value> promise;
    std::future<Value> future = promise.get_future();
    // A failed REQUIRE inside `work()` throws: the future carries it to the test thread (an
    // exception that left the thread would end the process with std::terminate).
    std::thread worker([&] {
        try {
            promise.set_value(work());
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });
    if (future.wait_for(LIMIT) != std::future_status::ready) {
        std::fprintf(stderr, "HANG: %s did not end within %lld s\n", what.c_str(),
                     static_cast<long long>(LIMIT.count()));
        std::fflush(stderr);
        std::quick_exit(1);
    }
    worker.join();
    return future.get();
}

std::string formatName(const media::ISource& source) {
    return media::descriptor(source.info().desc.format).name;
}

std::shared_ptr<media::ISource> takeSource(
    dvv::Result<std::variant<std::unique_ptr<media::ISource>, media::Ambiguous>>& opened) {
    REQUIRE(opened.has_value());
    REQUIRE(std::holds_alternative<std::unique_ptr<media::ISource>>(*opened));
    return std::move(std::get<std::unique_ptr<media::ISource>>(*opened));
}

// What reading every frame of a damaged file gave.
struct DamagedRun {
    bool opened = false;
    std::int64_t frameCount = 0;
    std::vector<bool> matches;  // per frame: a value equal to the reference
    std::vector<bool> errors;   // per frame: an Error
    // per frame: the code and message of its first Error (nullopt and empty without one)
    std::vector<std::optional<dvv::Errc>> codes;
    std::vector<std::string> messages;
};

// Opens `uri` (a damaged copy of `reference_stem`.mkv) and reads every frame, each bounded. Open
// either succeeds or fails with Format or Io. With `strict`, a frame must be an Error or equal to
// its reference (a value with other bytes is wrong pixels); without, a mismatch is only recorded.
DamagedRun readDamaged(const std::string& uri, std::string_view reference_stem, bool strict) {
    const auto hashes = referenceHashes(std::string(reference_stem) + ".framemd5");
    DamagedRun run;
    FfmpegHarness harness;
    auto opened = bounded("open " + uri, [&] { return harness.tryOpen(uri); });
    if (!opened.has_value()) {
        CAPTURE(opened.error().describe());
        CHECK((opened.error().code == dvv::Errc::Format || opened.error().code == dvv::Errc::Io));
        return run;
    }
    run.opened = true;
    const std::shared_ptr<media::ISource> source = takeSource(opened);
    // Every container is indexed at open, so the count is known even for a damaged file.
    CHECK(source->info().frameCount.has_value());
    run.frameCount = source->info().frameCount.value_or(0);
    REQUIRE(run.frameCount >= 0);
    CHECK(run.frameCount <= static_cast<std::int64_t>(hashes.size()));
    // Forward, then backward (the restart and seek paths), then forward again.
    std::vector<std::int64_t> order;
    order.reserve(3 * static_cast<std::size_t>(run.frameCount));
    for (std::int64_t i = 0; i < run.frameCount; ++i) {
        order.push_back(i);
    }
    for (std::int64_t i = run.frameCount - 1; i >= 0; --i) {
        order.push_back(i);
    }
    for (std::int64_t i = 0; i < run.frameCount; ++i) {
        order.push_back(i);
    }
    run.matches.assign(static_cast<std::size_t>(run.frameCount), true);
    run.errors.assign(static_cast<std::size_t>(run.frameCount), false);
    run.codes.assign(static_cast<std::size_t>(run.frameCount), std::nullopt);
    run.messages.assign(static_cast<std::size_t>(run.frameCount), {});
    for (const std::int64_t index : order) {
        CAPTURE(index);
        const auto bytes = bounded("frame " + std::to_string(index) + " of " + uri,
                                   [&] { return harness.originalBytes(source, index); });
        const auto at = static_cast<std::size_t>(index);
        if (!bytes.has_value()) {
            if (!run.errors[at]) {
                run.codes[at] = bytes.error().code;
                run.messages[at] = bytes.error().message;
            }
            run.errors[at] = true;
            run.matches[at] = false;
            continue;
        }
        const std::string ours = md5Hex(*bytes);
        if (strict) {
            CHECK_MESSAGE(ours == hashes[at].md5,
                          "wrong pixels: ours " << ours << ", reference " << hashes[at].md5);
        }
        if (ours != hashes[at].md5) {
            run.matches[at] = false;
        }
    }
    // The frames the index lost (a cut file) are out of range: the ISource out-of-range Error
    // (Errc::Internal "frame <i> is outside [0, <frameCount>)"), never bytes or another Error.
    for (auto index = run.frameCount; index < static_cast<std::int64_t>(hashes.size()); ++index) {
        CAPTURE(index);
        const auto bytes = bounded("frame " + std::to_string(index) + " of " + uri,
                                   [&] { return harness.originalBytes(source, index); });
        REQUIRE_FALSE(bytes.has_value());
        CHECK(bytes.error().code == dvv::Errc::Internal);
        const std::string range = std::format("frame {} is outside [0, {})", index, run.frameCount);
        CHECK_MESSAGE(bytes.error().message.find(range) != std::string::npos,
                      bytes.error().message);
    }
    return run;
}

// The UTF-8 form of `path` (ContainerSupplier::open takes one).
std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

// The packet whose bytes flippedCopy damages: the packet of frame 4 (an FFV1 stream has one packet
// per frame, in frame order).
constexpr int DAMAGED_PACKET = 4;

// Where the packet of video frame `index` lies in the file `path`: its byte position and size, read
// with the demuxer (no FFmpeg build's header, packet or cue sizes are assumed).
std::pair<std::size_t, std::size_t> packetSpan(const std::filesystem::path& path, int index) {
    const dvv::components::ffmpeg::Api& api = dvv::components::ffmpeg::api();
    AVFormatContext* context = nullptr;
    REQUIRE(api.avformat_open_input(&context, utf8(path).c_str(), nullptr, nullptr) == 0);
    const dvv::components::ffmpeg::FormatContextPtr format(context);
    REQUIRE(api.avformat_find_stream_info(context, nullptr) >= 0);
    const int stream = api.av_find_best_stream(context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    REQUIRE(stream >= 0);
    const dvv::components::ffmpeg::PacketPtr packet(api.av_packet_alloc());
    REQUIRE(packet != nullptr);
    for (int seen = 0; api.av_read_frame(context, packet.get()) >= 0;) {
        const bool found = packet->stream_index == stream && seen++ == index;
        const std::int64_t pos = packet->pos;
        const int size = packet->size;
        api.av_packet_unref(packet.get());
        if (found) {
            REQUIRE(pos >= 0);
            REQUIRE(size > 0);
            return {static_cast<std::size_t>(pos), static_cast<std::size_t>(size)};
        }
    }
    FAIL("the file has no packet " << index);
    return {};
}

// <stem>.mkv with 64 bytes in the middle of the packet of frame DAMAGED_PACKET XORed with 0x5A,
// written to `folder` as flipped_<stem>.mkv: its name. In MKV, AVPacket::pos is the SimpleBlock
// payload start, so [pos, pos + size) begins with the block header (track number, timecode and
// flags, 4 bytes for track 1) and the frame bytes start a few bytes later. The window is centred
// about 4 bytes early; a packet of at least 128 bytes keeps it clear of that header.
std::string flippedCopy(const TemporaryDirectory& folder, std::string_view stem) {
    const std::string source = std::string(stem) + ".mkv";
    std::vector<std::byte> bytes = dvv::components::testing::readFile(fixture(source));
    const auto [pos, size] = packetSpan(fixture(source), DAMAGED_PACKET);
    REQUIRE(pos + size <= bytes.size());
    REQUIRE_MESSAGE(size >= 128,
                    "packet " << DAMAGED_PACKET << " is too small to damage safely: " << size);
    const std::size_t count = 64;
    const std::size_t first = pos + ((size - count) / 2);
    for (std::size_t i = first; i < first + count; ++i) {
        bytes[i] ^= std::byte{0x5A};
    }
    const std::string name = "flipped_" + source;
    dvv::components::testing::writeFile(folder.path() / name, bytes);
    return name;
}

// The damage of a flipped file with a key frame every `interval` frames (damaged packets): the
// frames before the first Error match, every frame from it to the next key frame is an Error
// "frame <i> is damaged", and every frame from that key frame on matches again (the damage ended,
// also after the seeks or restarts of the backward and second forward passes).
void checkDamageEnds(const DamagedRun& run, std::size_t interval) {
    const auto count = static_cast<std::size_t>(run.frameCount);
    std::size_t first = 0;
    while (first < count && !run.errors[first]) {
        ++first;
    }
    REQUIRE_MESSAGE(first < count, "no frame was reported as damaged");
    const std::size_t key = ((first / interval) + 1) * interval;
    REQUIRE_MESSAGE(key < count, "the damage lies in the last GOP: nothing checks that it ends");
    for (std::size_t index = 0; index < count; ++index) {
        CAPTURE(index);
        if (index < first || index >= key) {
            CHECK(run.matches[index]);
            continue;
        }
        CHECK(run.errors[index]);
        CHECK(run.codes[index] == dvv::Errc::Format);
        CHECK_MESSAGE(run.messages[index].find("is damaged") != std::string::npos,
                      run.messages[index]);
    }
}

// A StopCheck that never stops (no flag, the current generation).
dvv::components::detail::StopCheck running() {
    return {.flag = nullptr, .generation = dvv::components::ffmpeg::stopGeneration()};
}

}  // namespace

TEST_SUITE("ffmpeg") {
    TEST_CASE("ffmpeg: a truncated MKV gives Errors past the cut, no hang or crash") {
        const std::vector<std::byte> whole =
            dvv::components::testing::readFile(fixture("ffv1_yuv420p.mkv"));
        const TemporaryDirectory folder;
        std::vector<std::byte> cut = whole;
        cut.resize(whole.size() * 6 / 10);
        dvv::components::testing::writeFile(folder.path() / "truncated.mkv", cut);
        const DamagedRun run = readDamaged(folder.uri("truncated.mkv"), "ffv1_yuv420p", true);
        // Frame 0 lies well before the cut; the last frames lie past it.
        REQUIRE(run.opened);
        REQUIRE(run.frameCount >= 1);
        CHECK(run.matches[0]);
        // The cut removes the trailing clusters, so the index scan loses frames and the
        // past-the-cut reads in readDamaged run for each lost one.
        CHECK_MESSAGE(run.frameCount < 10,
                      "the cut file still indexes " << run.frameCount << " of 10 frames");
    }

    TEST_CASE("ffmpeg: flipped bytes in an FFV1 packet give no hang or crash") {
        const TemporaryDirectory folder;
        const DamagedRun run =
            readDamaged(folder.uri(flippedCopy(folder, "ffv1_yuv420p")), "ffv1_yuv420p", false);
        REQUIRE(run.opened);
        REQUIRE(run.frameCount == 10);
        // Frames 0 to 3 lie before the damaged packet (frame 4's) and decode unchanged.
        for (std::size_t index = 0; index < 4; ++index) {
            CAPTURE(index);
            CHECK(run.matches[index]);
        }
        // The damage reached the decoder: some frame from 4 on is no reference frame.
        CHECK_MESSAGE(std::any_of(run.matches.begin() + DAMAGED_PACKET, run.matches.end(),
                                  [](const bool match) { return !match; }),
                      "frames 4 to 9 all match: the flipped bytes damaged no frame");
    }

    // The FFV1 decoder reports "slice CRC mismatch" for frame 4 only in its log and
    // conceals it; frames 4 to 9 (one key frame here, so the damage carries on) must be Errors,
    // never concealed pixels (strict readDamaged), and frames 0 to 3 still decode unchanged.
    TEST_CASE("ffmpeg: flipped bytes in an FFV1 packet give an Error, not wrong pixels") {
        const TemporaryDirectory folder;
        const DamagedRun run =
            readDamaged(folder.uri(flippedCopy(folder, "ffv1_yuv420p")), "ffv1_yuv420p", true);
        REQUIRE(run.opened);  // frame 0 lies before the damage
        REQUIRE(run.frameCount == 10);
        for (std::size_t index = 0; index < 4; ++index) {
            CAPTURE(index);
            CHECK(run.matches[index]);
        }
        CHECK(run.errors[4]);
        CHECK(run.codes[4] == dvv::Errc::Format);
        CHECK_MESSAGE(run.messages[4] == "frame 4 is damaged (the decoder reported an error)",
                      run.messages[4]);
        CHECK_MESSAGE(run.messages[5] == "frame 5 is damaged (it depends on damaged frame 4)",
                      run.messages[5]);
    }

    // The damage ends at the next key frame, in an indexed file (key frames 0, 3, 6, 9;
    // the backward pass seeks) and in a Sequential one (seq.mkv: FFmpeg's default GOP of 12 gives
    // key frames 0, 12 and 24; the backward pass restarts).
    TEST_CASE("ffmpeg: the damage in an FFV1 file ends at the next key frame") {
        const std::array<std::pair<std::string_view, std::size_t>, 2> files{
            {{"ffv1_gop", 3}, {"seq", 12}}};
        for (const auto& [stem, interval] : files) {
            CAPTURE(stem);
            const TemporaryDirectory folder;
            const DamagedRun run = readDamaged(folder.uri(flippedCopy(folder, stem)), stem, true);
            REQUIRE(run.opened);
            checkDamageEnds(run, interval);
        }
    }

    // A damaged frame keeps the decoder in step, so the requests that
    // follow it decode on instead of seeking (indexed) or restarting from frame 0 (Sequential).
    TEST_CASE("ffmpeg: damaged frames keep the decoder position") {
        for (const std::string_view stem : {"ffv1_gop", "seq"}) {
            CAPTURE(stem);
            const TemporaryDirectory folder;
            const std::string name = flippedCopy(folder, stem);
            auto opened = dvv::components::detail::ContainerSupplier::open(
                utf8(folder.path() / name), running());
            REQUIRE_MESSAGE(opened.has_value(), opened.error().describe());
            dvv::components::detail::ContainerSupplier& supplier = **opened;
            CHECK(supplier.sequential() == (stem == "seq"));
            REQUIRE(supplier.resets() == 0);
            bool damaged = false;
            for (std::int64_t index = 1; index < supplier.frameCount(); ++index) {
                CAPTURE(index);
                const auto result = supplier.frame(index, running());
                if (!result) {
                    damaged = true;
                    CHECK(result.error().code == dvv::Errc::Format);
                    CHECK_MESSAGE(result.error().message.find("is damaged") != std::string::npos,
                                  result.error().message);
                }
            }
            CHECK(damaged);
            CHECK(supplier.resets() == 0);
        }
    }

    TEST_CASE("ffmpeg: rawvideo in AVI, MOV and NUT, r210 and qtrle match framemd5") {
        FfmpegHarness harness;
        const std::array<std::pair<std::string_view, std::string_view>, 5> files{{
            {"lossless_rawvideo_avi.avi", "yuv420p"},
            {"lossless_rawvideo_mov.mov", "uyvy422"},
            {"lossless_rawvideo_nut.nut", "yuv444p"},
            {"lossless_r210.mov", "gbrp10le"},
            {"lossless_qtrle.mov", "rgb24"},
        }};
        // Forward, then backward and random: qtrle's inter frames need the seek to a key frame.
        const std::array<std::int64_t, 16> order{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 9, 3, 7, 1, 0, 5};
        for (const auto& [file, format] : files) {
            CAPTURE(file);
            const std::string stem(file.substr(0, file.find('.')));
            const auto hashes = referenceHashes(stem + ".framemd5");
            REQUIRE(hashes.size() == 10);
            const auto source = harness.open(fixtureUri(file));
            const media::SourceInfo& info = source->info();
            CHECK(info.provider == "ffmpeg");
            CHECK(formatName(*source) == format);
            CHECK(info.desc.width == 192);
            CHECK(info.desc.height == 144);
            CHECK(info.frameCount == 10);
            CHECK(info.fps == media::Rational{25, 1});
            for (const std::int64_t index : order) {
                CAPTURE(index);
                auto bytes = harness.originalBytes(source, index);
                REQUIRE_MESSAGE(bytes.has_value(), bytes.error().describe());
                const std::string ours = md5Hex(*bytes);
                const std::string& reference = hashes[static_cast<std::size_t>(index)].md5;
                CHECK_MESSAGE(ours == reference, "ours " << ours << ", reference " << reference);
            }
        }
    }

    TEST_CASE("ffmpeg: v410 in MOV is refused with a clean Unsupported") {
        FfmpegHarness harness;
        auto& lifetimes = dvv::LifetimeRegistry::instance();
        const std::size_t live_before = lifetimes.liveCount();
        auto opened = harness.tryOpen(fixtureUri("lossless_v410.mov"));
        REQUIRE_FALSE(opened.has_value());
        const std::string text = opened.error().describe();
        CAPTURE(text);
        CHECK(opened.error().code == dvv::Errc::Unsupported);
        // FFmpeg 8 decodes v410 as rawvideo with output v30xle, outside the catalogue.
        CHECK(text.find("is not in the catalogue") != std::string::npos);
        CHECK(lifetimes.liveCount() == live_before);
    }
}

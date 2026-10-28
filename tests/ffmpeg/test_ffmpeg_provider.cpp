// The `ffmpeg` source provider against `ffmpeg -f framemd5` (M4 Task 5a; design v2 §4.4, §5.4, D5,
// D28; v1.0 codec scope). Fixtures: tests/oracle/make-container-fixtures.cmake.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/executors.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/log.h"
#include "dvv/base/task.h"
#include "dvv/base/uri.h"
#include "dvv/base/when_all.h"
#include "dvv/components/ffmpeg.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/media/types.h"
#include "ffmpeg/api.h"
#include "ffmpeg_harness.h"
#include "framemd5.h"
#include "sources/av_worker.h"
#include "sources/container_supplier.h"

namespace {

namespace media = dvv::media;
using dvv::components::testing::FfmpegHarness;
using dvv::components::testing::fixture;
using dvv::components::testing::fixturePath;
using dvv::components::testing::fixtureUri;
using dvv::components::testing::md5Hex;
using dvv::components::testing::PacedWorkers;
using dvv::components::testing::referenceHashes;
using dvv::components::testing::TemporaryDirectory;

using namespace std::chrono_literals;

constexpr std::string_view SEQ = "seq.mkv";    // Task 0: FFV1 in MKV with pairs of equal pts
constexpr std::string_view LONG = "long.mkv";  // 3000 frames, Sequential like SEQ

// The FFV1/MKV formats of make-container-fixtures.cmake (gbrp is FFVHuff in the same name).
constexpr std::array<std::string_view, 18> FFV1_FORMATS{
    "gray",        "gray10le",    "gray12le",    "gray16le",    "yuv420p",     "yuv422p",
    "yuv444p",     "yuv420p10le", "yuv422p10le", "yuv444p10le", "yuv420p12le", "yuv422p12le",
    "yuv444p12le", "yuv420p16le", "yuv444p16le", "gbrp",        "gbrp10le",    "gbrp16le"};

std::string formatName(const media::ISource& source) {
    return media::descriptor(source.info().desc.format).name;
}

// The MD5 of frame `index` of `source`; fails the test when it cannot be read.
std::string frameHash(FfmpegHarness& harness, const std::shared_ptr<media::ISource>& source,
                      std::int64_t index) {
    auto bytes = harness.originalBytes(source, index);
    REQUIRE_MESSAGE(bytes.has_value(), "frame " << index << ": " << bytes.error().describe());
    return md5Hex(*bytes);
}

// Reads `order` and compares every frame with the reference list.
void checkFrames(FfmpegHarness& harness, const std::shared_ptr<media::ISource>& source,
                 std::string_view file,
                 const std::vector<dvv::components::testing::FrameHash>& hashes,
                 std::span<const std::int64_t> order) {
    for (const std::int64_t index : order) {
        REQUIRE(index < static_cast<std::int64_t>(hashes.size()));
        const std::string ours = frameHash(harness, source, index);
        const std::string& reference = hashes[static_cast<std::size_t>(index)].md5;
        CHECK_MESSAGE(ours == reference, file << " frame " << index << ": ours " << ours
                                              << ", reference " << reference);
    }
}

std::vector<std::int64_t> range(std::int64_t first, std::int64_t end) {
    std::vector<std::int64_t> values;
    for (std::int64_t i = first; i < end; ++i) {
        values.push_back(i);
    }
    return values;
}

// The provider instance `ffmpeg` of the harness's registry.
std::unique_ptr<media::ISourceProvider> ffmpegProvider(FfmpegHarness& harness) {
    auto provider = harness.registry().createComponent<media::ISourceProvider>("ffmpeg");
    REQUIRE_MESSAGE(provider.has_value(), provider.error().describe());
    return std::move(*provider);
}

media::Match matchHead(const media::ISourceProvider& provider, std::string_view uri,
                       std::span<const std::byte> head) {
    auto parsed = dvv::parseUri(uri);
    REQUIRE(parsed.has_value());
    const media::SourceRequest request{.uri = std::move(*parsed),
                                       .hint = std::nullopt,
                                       .fileReads = dvv::platform::FileReadMode::Auto};
    return provider.match(request, head);
}

std::vector<std::byte> head4096(std::string_view name) {
    std::vector<std::byte> bytes = dvv::components::testing::readFile(fixture(name));
    bytes.resize(std::min<std::size_t>(bytes.size(), 4096));
    return bytes;
}

std::string errorText(const dvv::Error& error) {
    return error.describe();
}

// An open that must fail: its error.
dvv::Error openError(FfmpegHarness& harness, std::string_view uri) {
    INFO(std::string(uri));
    auto opened = harness.tryOpen(uri);
    REQUIRE_FALSE(opened.has_value());
    return opened.error();
}

// The resolved-URI prefix of a fixture: its path as formatUri writes it.
std::string resolvedBase(std::string_view name) {
    dvv::Uri uri;
    uri.scheme = "file";
    uri.path = fixturePath(name);
    return dvv::formatUri(uri);
}

// Every dvv log line written from the first call on (FFmpeg's log reaches the dvv logger at Warn).
const dvv::MemorySink& logLines() {
    static const std::shared_ptr<dvv::MemorySink> SINK = [] {
        auto sink = std::make_shared<dvv::MemorySink>();
        dvv::Logger::instance().addSink(sink);
        return sink;
    }();
    return *SINK;
}

constexpr std::string_view REFUSED = "codec mpeg4 is not supported (lossless codecs only)";

// The huffyuv AVI with every HFYU FourCC of its head replaced by XVID (the AVI demuxer then
// reports codec mpeg4), written to `folder`: its URI and its head.
std::pair<std::string, std::vector<std::byte>> relabelledAvi(const TemporaryDirectory& folder) {
    std::vector<std::byte> bytes =
        dvv::components::testing::readFile(fixture("lossless_huffyuv.avi"));
    const std::size_t head = std::min<std::size_t>(bytes.size(), 4096);
    int replaced = 0;
    for (std::size_t i = 0; i + 4 <= head; ++i) {
        if (bytes[i] == std::byte{'H'} && bytes[i + 1] == std::byte{'F'} &&
            bytes[i + 2] == std::byte{'Y'} && bytes[i + 3] == std::byte{'U'}) {
            bytes[i] = std::byte{'X'};
            bytes[i + 1] = std::byte{'V'};
            bytes[i + 2] = std::byte{'I'};
            bytes[i + 3] = std::byte{'D'};
            ++replaced;
        }
    }
    CHECK(replaced > 0);
    dvv::components::testing::writeFile(folder.path() / "patched.avi", bytes);
    bytes.resize(head);
    return {folder.uri("patched.avi"), std::move(bytes)};
}

// A StopCheck that never stops (no flag, the current generation).
dvv::components::detail::StopCheck running() {
    return {.flag = nullptr, .generation = dvv::components::ffmpeg::stopGeneration()};
}

// Field by field, so a failure names the field.
void checkColour(const media::ColourParams& actual, const media::ColourParams& expected) {
    CHECK(static_cast<int>(actual.matrix) == static_cast<int>(expected.matrix));
    CHECK(static_cast<int>(actual.range) == static_cast<int>(expected.range));
    CHECK(static_cast<int>(actual.transfer) == static_cast<int>(expected.transfer));
    CHECK(static_cast<int>(actual.primaries) == static_cast<int>(expected.primaries));
    CHECK(actual == expected);
}

}  // namespace

TEST_SUITE("ffmpeg") {
    TEST_CASE("ffmpeg: FFV1 MKV matches framemd5") {
        FfmpegHarness harness;
        const std::vector<std::int64_t> order = range(0, 10);
        for (const std::string_view format : FFV1_FORMATS) {
            const std::string file = "ffv1_" + std::string(format) + ".mkv";
            CAPTURE(file);
            const auto hashes = referenceHashes("ffv1_" + std::string(format) + ".framemd5");
            REQUIRE(hashes.size() == 10);
            const auto source = harness.open(fixtureUri(file));
            const media::SourceInfo& info = source->info();
            CHECK(info.provider == "ffmpeg");
            CHECK(info.reader == "ffmpeg");
            CHECK(formatName(*source) == format);
            CHECK(info.desc.width == 352);
            CHECK(info.desc.height == 288);
            CHECK(info.frameCount == 10);
            CHECK(info.access == media::SourceAccess::RandomCostly);
            CHECK(info.fps == media::Rational{25, 1});
            checkFrames(harness, source, file, hashes, order);
        }
    }

    TEST_CASE("ffmpeg: a Sequential stream matches framemd5 forward, backward and random") {
        FfmpegHarness harness;
        const auto hashes = referenceHashes("seq.framemd5");
        REQUIRE(hashes.size() == 30);
        const auto source = harness.open(fixtureUri(SEQ));
        CHECK(source->info().access == media::SourceAccess::Sequential);
        CHECK(source->info().frameCount == 30);
        std::vector<std::int64_t> order = range(0, 30);
        for (const std::int64_t index : {29, 3, 17, 0, 28, 5}) {
            order.push_back(index);
        }
        std::vector<std::int64_t> shuffled = range(0, 30);
        std::mt19937 random(1);
        std::ranges::shuffle(shuffled, random);
        order.insert(order.end(), shuffled.begin(), shuffled.end());
        checkFrames(harness, source, SEQ, hashes, order);
    }

    TEST_CASE("ffmpeg: seek and flush on the indexed path") {
        FfmpegHarness harness;
        const auto hashes = referenceHashes("ffv1_yuv420p.framemd5");
        const auto source = harness.open(fixtureUri("ffv1_yuv420p.mkv"));
        const std::array<std::int64_t, 5> order{9, 2, 8, 0, 5};
        checkFrames(harness, source, "ffv1_yuv420p.mkv", hashes, order);
    }

    TEST_CASE("ffmpeg: concurrent produce calls from one store") {
        FfmpegHarness harness;
        const auto hashes = referenceHashes("ffv1_yuv420p10le.framemd5");
        const auto source = harness.open(fixtureUri("ffv1_yuv420p10le.mkv"));
        const media::OriginId origin = harness.origin(source);
        std::vector<dvv::Task<dvv::Result<media::OriginalRead>>> tasks;
        for (std::int64_t index = 9; index >= 0; --index) {
            tasks.push_back(harness.store().acquireOriginal({origin, index}, media::Api::HostPlanes,
                                                            harness.store().hostDomain(),
                                                            harness.cpu(), true));
        }
        auto reads = dvv::syncWait(dvv::whenAll(std::move(tasks)));
        REQUIRE(reads.size() == 10);
        for (std::size_t i = 0; i < reads.size(); ++i) {
            const auto index = static_cast<std::int64_t>(9 - i);
            CAPTURE(index);
            REQUIRE_MESSAGE(reads[i].has_value(), reads[i].error().describe());
            const std::string ours = md5Hex(dvv::components::testing::originalReadBytes(*reads[i]));
            CHECK(ours == hashes[static_cast<std::size_t>(index)].md5);
        }
    }

    TEST_CASE(
        "ffmpeg: destroying a Sequential source with 4 queued far-back requests returns "
        "within 1 s") {
        const PacedWorkers paced;
        auto made = dvv::components::detail::makeContainerWorkerForTest(fixture(LONG));
        REQUIRE_MESSAGE(made.has_value(), made.error().describe());
        std::unique_ptr<dvv::components::detail::AvWorker> worker = std::move(*made);
        dvv::components::detail::AvWorker* const raw = worker.get();
        auto pool = dvv::ExecutorFactory::makeThreadPool(2);
        REQUIRE(pool.has_value());
        const std::array<std::int64_t, 4> indices{2999, 1500, 2999, 1500};
        std::array<std::optional<dvv::Result<void>>, 4> results;
        std::vector<std::thread> threads;
        threads.reserve(indices.size());
        for (std::size_t i = 0; i < indices.size(); ++i) {
            threads.emplace_back(
                [&, i] { results[i] = dvv::syncWait(raw->fetch(indices[i], **pool)); });
        }
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (raw->queued() < 4 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        if (raw->queued() != 4) {
            // A thread may not have entered fetch yet: destroying the worker now would be a
            // use-after-free. The paced requests finish on their own; then fail.
            const std::size_t queued = raw->queued();
            for (std::thread& thread : threads) {
                thread.join();
            }
            FAIL("only " << queued << " of 4 requests were queued within 10 s");
        }
        CHECK(paced.waitUntilDecoding());  // the first request is decoding, three are queued
        const auto t0 = std::chrono::steady_clock::now();
        worker.reset();
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        for (std::thread& thread : threads) {
            thread.join();
        }
        CHECK(elapsed < 1000ms);
        for (const auto& slot : results) {
            REQUIRE(slot.has_value());
            // A missing slot already failed above; the default (success) then fails below.
            const dvv::Result<void> result = slot.value_or(dvv::Result<void>{});
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == dvv::Errc::Internal);
            CHECK(result.error().message.find("source closed") != std::string::npos);
        }
    }

    TEST_CASE("ffmpeg: a frame that changes size is Format") {
        FfmpegHarness harness;
        const auto source = harness.open(fixtureUri("concat_sizes.mkv"));
        CHECK(formatName(*source) == "gray");
        for (std::int64_t index = 0; index < 5; ++index) {
            CAPTURE(index);
            const auto bytes = harness.originalBytes(source, index);
            CHECK_MESSAGE(bytes.has_value(), bytes.error().describe());
        }
        const auto changed = harness.originalBytes(source, 5);
        REQUIRE_FALSE(changed.has_value());
        CHECK(changed.error().code == dvv::Errc::Format);
        CAPTURE(errorText(changed.error()));
        CHECK(errorText(changed.error()).find("frame 5 changed to 176x144 gray") !=
              std::string::npos);
    }

    TEST_CASE("ffmpeg: colour from the container and the D28 override") {
        FfmpegHarness harness;
        const std::string uri = fixtureUri("ffv1_yuv420p.mkv");
        const std::string base = resolvedBase("ffv1_yuv420p.mkv");
        const auto tagged = harness.open(uri);
        const media::ColourParams container{.matrix = media::ColourMatrix::Bt709,
                                            .range = media::ColourRange::Limited,
                                            .transfer = media::TransferFn::Bt1886,
                                            .primaries = media::Primaries::Bt709,
                                            .kr = 0,
                                            .kb = 0};
        checkColour(media::colourParams(tagged->info().desc.colour), container);
        CHECK(tagged->info().resolvedUri == base + "?fmt=yuv420p&size=352x288&fps=25&impl=ffmpeg");

        // colour= (D5R): the standard replaces matrix, curve and primaries; the range stays.
        const auto overridden = harness.open(uri + "?colour=bt601-ntsc");
        const media::ColourParams bt601{.matrix = media::ColourMatrix::Bt601,
                                        .range = media::ColourRange::Limited,
                                        .transfer = media::TransferFn::Bt1886,
                                        .primaries = media::Primaries::Bt601_525,
                                        .kr = 0,
                                        .kb = 0};
        checkColour(media::colourParams(overridden->info().desc.colour), bt601);
        const std::string resolved = overridden->info().resolvedUri;
        CHECK(resolved == base + "?fmt=yuv420p&size=352x288&fps=25&colour=bt601-ntsc&impl=ffmpeg");

        // The in-process Reopen round trip.
        const auto reopened = harness.open(resolved);
        CHECK(reopened->info().desc == overridden->info().desc);
        const auto hashes = referenceHashes("ffv1_yuv420p.framemd5");
        CHECK(frameHash(harness, reopened, 3) == hashes[3].md5);
        CHECK(frameHash(harness, overridden, 3) == hashes[3].md5);

        // range= replaces the range with every standard; the form is kept.
        const auto preset = harness.open(uri + "?colour=bt2100-hlg&range=full");
        checkColour(media::colourParams(preset->info().desc.colour),
                    media::ColourParams{.matrix = media::ColourMatrix::Bt2020Ncl,
                                        .range = media::ColourRange::Full,
                                        .transfer = media::TransferFn::Hlg,
                                        .primaries = media::Primaries::Bt2020,
                                        .kr = 0,
                                        .kb = 0});
        CHECK(preset->info().resolvedUri ==
              base + "?fmt=yuv420p&size=352x288&fps=25&colour=bt2100-hlg&range=full&impl=ffmpeg");

        // The removed keys name colour=<standard> (D5R §3.1).
        const dvv::Error removed = openError(harness, uri + "?matrix=bt601");
        CHECK(removed.code == dvv::Errc::Format);
        CHECK(removed.message ==
              "unknown parameter 'matrix' (set the colour with colour=<standard>: bt601-ntsc, "
              "bt601-pal, bt709, bt2020, bt2100-pq, bt2100-hlg)");
    }

    TEST_CASE("ffmpeg: fmt, size and fps must equal the stream") {
        FfmpegHarness harness;
        // impl=ffmpeg: with fmt and size the raw provider would match the file too.
        const std::string uri = fixtureUri("ffv1_yuv420p.mkv");
        const auto source = harness.open(uri + "?fmt=yuv420p&size=352x288&fps=25&impl=ffmpeg");
        CHECK(source->info().provider == "ffmpeg");
        const std::array<std::pair<std::string_view, std::string_view>, 3> wrong{{
            {"?fmt=gray&impl=ffmpeg", "fmt is set by the container"},
            {"?size=176x144&impl=ffmpeg", "size is set by the container"},
            {"?fps=30&impl=ffmpeg", "fps is set by the container"},
        }};
        for (const auto& [query, text] : wrong) {
            CAPTURE(query);
            const dvv::Error error = openError(harness, uri + std::string(query));
            CHECK(error.code == dvv::Errc::Format);
            CAPTURE(errorText(error));
            CHECK(errorText(error).find(text) != std::string::npos);
        }
    }

    TEST_CASE("ffmpeg: match with a 1-byte head") {
        FfmpegHarness harness;
        const auto provider = ffmpegProvider(harness);
        const std::array<std::byte, 1> one{std::byte{0x1A}};
        const media::Match tiny = matchHead(*provider, "file:x.mkv", one);
        CHECK((tiny.confidence == 0 || tiny.confidence == 90));

        const std::vector<std::byte> avi = head4096("lossless_huffyuv.avi");
        const media::Match by_content = matchHead(*provider, "file:clip.avi", avi);
        CHECK(by_content.confidence == 90);
        CAPTURE(by_content.why);
        CHECK(by_content.why.find("FFmpeg demuxer avi") != std::string::npos);
        CHECK(matchHead(*provider, "file:clip.bin", avi).confidence == 90);

        const std::vector<std::byte> mov = head4096("lossless_v210.mov");
        CHECK(matchHead(*provider, "file:clip.mov", mov).confidence == 90);
        CHECK(matchHead(*provider, "file:clip.bin", mov).confidence == 90);

        const std::vector<std::byte> mkv = head4096("lossless_utvideo.mkv");
        CHECK(matchHead(*provider, "file:clip.mkv", mkv).confidence == 90);
        CHECK(matchHead(*provider, "file:clip.bin", mkv).confidence == 90);
    }

    TEST_CASE("ffmpeg: a raw .dat file is not claimed by its extension") {
        FfmpegHarness harness;
        const TemporaryDirectory folder;
        const std::vector<std::byte> frames(std::size_t{16} * 16 * 3, std::byte{0x40});
        dvv::components::testing::writeFile(folder.path() / "x_16x16_gray.dat", frames);
        const std::string uri = folder.uri("x_16x16_gray.dat");
        const media::Match match = matchHead(*ffmpegProvider(harness), uri, frames);
        CAPTURE(match.why);
        CHECK(match.confidence == 0);
        const auto raw = harness.open(uri);  // REQUIRE: not Ambiguous
        CHECK(raw->info().provider == "raw");
    }

    TEST_CASE("ffmpeg: raw and Y4M files stay with their providers") {
        FfmpegHarness harness;
        const TemporaryDirectory folder;
        const std::vector<std::byte> frames(std::size_t{16} * 16 * 3, std::byte{0x40});
        dvv::components::testing::writeFile(folder.path() / "x_16x16_gray.yuv", frames);
        const auto raw = harness.open(folder.uri("x_16x16_gray.yuv"));
        CHECK(raw->info().provider == "raw");

        std::string y4m = "YUV4MPEG2 W16 H16 F25:1 Cmono\n";
        for (int frame = 0; frame < 2; ++frame) {
            y4m += "FRAME\n" + std::string(std::size_t{16} * 16, '\x40');
        }
        const auto* data = reinterpret_cast<const std::byte*>(y4m.data());
        dvv::components::testing::writeFile(folder.path() / "clip.y4m",
                                            std::span<const std::byte>(data, y4m.size()));
        const auto y4m_source = harness.open(folder.uri("clip.y4m"));
        CHECK(y4m_source->info().provider == "y4m");

        // The exclusion itself: the factory alone would still pick y4m (100 against 90).
        const auto provider = ffmpegProvider(harness);
        const media::Match y4m_match =
            matchHead(*provider, folder.uri("clip.y4m"), std::span<const std::byte>(data, 64));
        CAPTURE(y4m_match.why);
        CHECK(y4m_match.confidence == 0);
        CHECK(y4m_match.why.find("left to its own provider") != std::string::npos);
        CHECK(matchHead(*provider, folder.uri("x_16x16_gray.yuv"), frames).confidence == 0);
    }

    TEST_CASE("ffmpeg: errors") {
        FfmpegHarness harness;
        const TemporaryDirectory folder;
        const dvv::Error missing = openError(harness, folder.uri("missing.mkv"));
        CHECK(missing.code == dvv::Errc::Io);
        // The factory fails on the head read above; the provider's own mapping (ENOENT -> Io):
        const auto missing_worker =
            dvv::components::detail::makeContainerWorkerForTest(folder.path() / "missing.mkv");
        REQUIRE_FALSE(missing_worker.has_value());
        CHECK(missing_worker.error().code == dvv::Errc::Io);

        const std::string text = "plain notes, not a video\n";
        const auto* data = reinterpret_cast<const std::byte*>(text.data());
        dvv::components::testing::writeFile(folder.path() / "notes.txt",
                                            std::span<const std::byte>(data, text.size()));
        const dvv::Error notes = openError(harness, folder.uri("notes.txt") + "?impl=ffmpeg");
        CHECK(notes.code == dvv::Errc::Unsupported);

        const dvv::Error audio = openError(harness, fixtureUri("audio_only.mka"));
        CHECK(audio.code == dvv::Errc::Unsupported);
        CAPTURE(errorText(audio));
        CHECK(errorText(audio).find("no video stream") != std::string::npos);
    }

    TEST_CASE("ffmpeg: stopFfmpegSources ends queued store requests within 1 s") {
        {
            FfmpegHarness harness;
            std::optional<PacedWorkers> paced(std::in_place);
            const auto source = harness.open(fixtureUri(LONG));
            REQUIRE(source->info().access == media::SourceAccess::Sequential);
            static_cast<void>(harness.origin(source));
            std::optional<dvv::Result<std::vector<std::byte>>> result;
            std::thread reader([&] { result = harness.originalBytes(source, 2999); });
            CHECK(paced->waitUntilDecoding());  // the request is decoding on the worker
            const auto t0 = std::chrono::steady_clock::now();
            dvv::components::stopFfmpegSources();
            reader.join();
            const auto elapsed = std::chrono::steady_clock::now() - t0;
            paced.reset();  // the source opened below decodes at full speed
            CHECK(elapsed < 1000ms);
            REQUIRE(result.has_value());
            // A missing result already failed above; the default (a value) then fails below.
            const auto read = result.value_or(dvv::Result<std::vector<std::byte>>{});
            REQUIRE_FALSE(read.has_value());
            CAPTURE(errorText(read.error()));
            CHECK(errorText(read.error()).find("source closed") != std::string::npos);

            // A source opened afterwards takes the new generation.
            const auto hashes = referenceHashes("seq.framemd5");
            const auto fresh = harness.open(fixtureUri(SEQ));
            CHECK(frameHash(harness, fresh, 0) == hashes[0].md5);
        }
        // The harness detached and destroyed everything.
    }

    TEST_CASE("ffmpeg: lossless codecs in AVI, MKV, MOV and NUT open and match framemd5") {
        FfmpegHarness harness;
        const std::array<std::pair<std::string_view, std::string_view>, 4> files{{
            {"lossless_huffyuv.avi", "yuv422p"},
            {"lossless_utvideo.mkv", "yuv420p"},
            {"lossless_v210.mov", "yuv422p10le"},
            {"lossless_magicyuv.nut", "yuv444p"},
        }};
        std::vector<std::int64_t> order = range(0, 10);
        for (const std::int64_t index : {7, 1, 0}) {
            order.push_back(index);
        }
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
            // Task 0: AVI and NUT packets carry pts too, so every container is indexed.
            CHECK(info.access == media::SourceAccess::RandomCostly);
            CHECK(info.fps == media::Rational{25, 1});
            checkFrames(harness, source, file, hashes, order);
        }
    }

    TEST_CASE("ffmpeg: a lossy codec is refused at open") {
        FfmpegHarness harness;
        const auto provider = ffmpegProvider(harness);
        const TemporaryDirectory folder;
        auto& lifetimes = dvv::LifetimeRegistry::instance();

        // (a) The relabelled huffyuv AVI (codec mpeg4), (b) a real mpeg4 MP4.
        const auto [patched, head] = relabelledAvi(folder);
        CHECK(matchHead(*provider, patched, head).confidence == 90);

        for (const std::string& uri : {patched, fixtureUri("lossy_mpeg4.mp4")}) {
            CAPTURE(uri);
            const std::size_t live_before = lifetimes.liveCount();
            const dvv::Error error = openError(harness, uri);
            CHECK(error.code == dvv::Errc::Unsupported);
            CAPTURE(errorText(error));
            CHECK(errorText(error).find(REFUSED) != std::string::npos);
            CHECK(lifetimes.liveCount() == live_before);
        }
    }

    TEST_CASE("ffmpeg: no lossy decoder opens while the streams are probed") {
        FfmpegHarness harness;
        const TemporaryDirectory folder;
        const auto [patched, head] = relabelledAvi(folder);
        for (const std::string& uri : {patched, fixtureUri("lossy_mpeg4.mp4")}) {
            CAPTURE(uri);
            const std::size_t before = logLines().lines().size();
            const dvv::Error error = openError(harness, uri);
            CHECK(error.code == dvv::Errc::Unsupported);
            CAPTURE(errorText(error));
            CHECK(errorText(error).find(REFUSED) != std::string::npos);
            // avcodec_open2 refuses a decoder outside codec_whitelist before it initialises it.
            const std::vector<std::string> lines = logLines().lines();
            const bool refused = std::any_of(
                lines.begin() + static_cast<std::ptrdiff_t>(before), lines.end(),
                [](const std::string& line) {
                    return line.find("Codec (mpeg4) not on whitelist") != std::string::npos;
                });
            CHECK_MESSAGE(refused, "FFmpeg was not stopped from opening the mpeg4 decoder");
        }
    }

    TEST_CASE("ffmpeg: a failed Sequential reopen leaves a defined error state") {
        auto opened = dvv::components::detail::ContainerSupplier::open(fixturePath(SEQ), running());
        REQUIRE_MESSAGE(opened.has_value(), opened.error().describe());
        dvv::components::detail::ContainerSupplier& supplier = **opened;
        REQUIRE(supplier.sequential());
        REQUIRE(supplier.frame(20, running()).has_value());  // the ring holds 13..20
        // The injected failure: the next restart reopens a file that does not exist.
        const TemporaryDirectory folder;
        supplier.setPathForTest(dvv::media::utf8FromPath(folder.path() / "gone.mkv"));
        // Behind the ring (a restart), then forward from the old position, then both again.
        for (const std::int64_t index : {1, 21, 25, 0, 29}) {
            CAPTURE(index);
            const auto result = supplier.frame(index, running());
            REQUIRE_FALSE(result.has_value());
            CAPTURE(result.error().describe());
            CHECK(result.error().code == dvv::Errc::Io);
        }
    }

    TEST_CASE("ffmpeg: open uses the container index of AVI and MOV") {
        // AVI (idx1) and MOV (sample tables) list every frame: open reads no packet for the index.
        // MKV cues and NUT syncpoints list only some frames, so those are scanned.
        const std::array<std::pair<std::string_view, bool>, 4> files{{
            {"lossless_huffyuv.avi", true},
            {"lossless_v210.mov", true},
            {"lossless_utvideo.mkv", false},
            {"lossless_magicyuv.nut", false},
        }};
        for (const auto& [file, indexed] : files) {
            CAPTURE(file);
            auto opened =
                dvv::components::detail::ContainerSupplier::open(fixturePath(file), running());
            REQUIRE_MESSAGE(opened.has_value(), opened.error().describe());
            CHECK((*opened)->frameCount() == 10);
            CHECK_FALSE((*opened)->sequential());
            if (indexed) {
                CHECK((*opened)->scannedPackets() == 0);
            } else {
                CHECK((*opened)->scannedPackets() >= 10);
            }
        }
    }

    TEST_CASE("ffmpeg: the open-time scan honours the stop flag") {
        // long.mkv is scanned (MKV). This proves the scan checks the flag; like the paced cases it
        // cannot tell a check per packet from one before the loop (review item).
        const std::atomic<bool> stopped{true};
        const dvv::components::detail::StopCheck stop{
            .flag = &stopped, .generation = dvv::components::ffmpeg::stopGeneration()};
        const auto opened =
            dvv::components::detail::ContainerSupplier::open(fixturePath(LONG), stop);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == dvv::Errc::Internal);
        CHECK(opened.error().message == "source closed");
    }

    TEST_CASE("ffmpeg: a frame that changes only its format is Format") {
        const dvv::components::ffmpeg::FramePtr frame(
            dvv::components::ffmpeg::api().av_frame_alloc());
        REQUIRE(frame);
        frame->width = 64;
        frame->height = 48;
        frame->format = AV_PIX_FMT_YUV422P;
        const media::FrameDesc desc{.format = 0, .colour = 0, .width = 64, .height = 48};
        const auto changed =
            dvv::components::detail::checkFrame(*frame, 7, desc, AV_PIX_FMT_YUV420P);
        REQUIRE_FALSE(changed.has_value());
        CHECK(changed.error().code == dvv::Errc::Format);
        CHECK(changed.error().message == "frame 7 changed to 64x48 yuv422p");
        CHECK(dvv::components::detail::checkFrame(*frame, 7, desc, AV_PIX_FMT_YUV422P).has_value());
    }
}

// Its own CTest entry, which the thread-sanitizer preset runs with no suppressions: the decoders
// start no FFmpeg threads, so every report would come from our own threading (the worker, the
// ring, the store and pools, the log callback).
TEST_SUITE("ffmpeg-single-thread") {
    TEST_CASE("ffmpeg: single-thread decoding has no races in our threading") {
        namespace detail = dvv::components::detail;
        struct Restore {
            Restore() { detail::setDecoderThreadsForTest(1); }
            ~Restore() { detail::setDecoderThreadsForTest(0); }
            Restore(const Restore&) = delete;
            Restore& operator=(const Restore&) = delete;
            Restore(Restore&&) = delete;
            Restore& operator=(Restore&&) = delete;
        } restore;
        for (const std::string_view file : {SEQ, std::string_view("ffv1_yuv420p10le.mkv")}) {
            CAPTURE(file);
            auto opened = detail::ContainerSupplier::open(fixturePath(file), running());
            REQUIRE_MESSAGE(opened.has_value(), opened.error().describe());
            CHECK((*opened)->decoderThreads() == 1);
        }

        FfmpegHarness harness;
        // Sequential: forward, then backward (each step back restarts the stream).
        const auto seq_hashes = referenceHashes("seq.framemd5");
        REQUIRE(seq_hashes.size() == 30);
        const auto seq = harness.open(fixtureUri(SEQ));
        std::vector<std::int64_t> order = range(0, 30);
        for (const std::int64_t index : {29, 3, 17, 0}) {
            order.push_back(index);
        }
        checkFrames(harness, seq, SEQ, seq_hashes, order);

        // Indexed: concurrent produce calls from one store on the cpu pool.
        const auto hashes = referenceHashes("ffv1_yuv420p10le.framemd5");
        const auto source = harness.open(fixtureUri("ffv1_yuv420p10le.mkv"));
        const media::OriginId origin = harness.origin(source);
        std::vector<dvv::Task<dvv::Result<media::OriginalRead>>> tasks;
        for (std::int64_t index = 9; index >= 0; --index) {
            tasks.push_back(harness.store().acquireOriginal({origin, index}, media::Api::HostPlanes,
                                                            harness.store().hostDomain(),
                                                            harness.cpu(), true));
        }
        auto reads = dvv::syncWait(dvv::whenAll(std::move(tasks)));
        REQUIRE(reads.size() == 10);
        for (std::size_t i = 0; i < reads.size(); ++i) {
            const auto index = static_cast<std::int64_t>(9 - i);
            CAPTURE(index);
            REQUIRE_MESSAGE(reads[i].has_value(), reads[i].error().describe());
            const std::string ours = md5Hex(dvv::components::testing::originalReadBytes(*reads[i]));
            CHECK(ours == hashes[static_cast<std::size_t>(index)].md5);
        }
    }
}

// The `lavfi` source provider against `ffmpeg -f lavfi … -f rawvideo` (M4 Task 5b; design v2 D6,
// §4.4 lavfi, §5.4; v1.0 codec scope). Fixtures: tests/oracle/make-lavfi-fixtures.cmake.

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "dvv/base/environment.h"
#include "dvv/base/error.h"
#include "dvv/base/executors.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/base/when_all.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"
#include "ffmpeg_harness.h"
#include "sources/av_worker.h"
#include "sources/ffmpeg_rules.h"
#include "sources/graph_supplier.h"

namespace {

namespace media = dvv::media;
using dvv::components::testing::FfmpegHarness;
using dvv::components::testing::PacedWorkers;

using namespace std::chrono_literals;

// One line of manifest.txt: "<k>|<graph>|<frames>".
struct LavfiFixture {
    int k = 0;
    std::string graph;
    std::int64_t frames = 0;
};

// The lavfi fixture folder of this FFmpeg build: <DVV_LAVFI_FIXTURES>/current.txt (REQUIRE).
std::filesystem::path lavfiDirectory() {
    const std::string root = dvv::getEnvironment("DVV_LAVFI_FIXTURES").value_or("");
    REQUIRE_MESSAGE(!root.empty(), "DVV_LAVFI_FIXTURES is not set");
    const std::vector<std::byte> bytes =
        dvv::components::testing::readFile(media::pathFromUtf8(root) / "current.txt");
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return media::pathFromUtf8(text);
}

std::vector<LavfiFixture> manifest() {
    const std::vector<std::byte> bytes =
        dvv::components::testing::readFile(lavfiDirectory() / "manifest.txt");
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::vector<LavfiFixture> lines;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        const std::size_t first = line.find('|');
        const std::size_t last = line.rfind('|');
        REQUIRE_MESSAGE((first != std::string_view::npos && last > first), std::string(line));
        lines.push_back({.k = std::stoi(std::string(line.substr(0, first))),
                         .graph = std::string(line.substr(first + 1, last - first - 1)),
                         .frames = std::stoll(std::string(line.substr(last + 1)))});
    }
    REQUIRE(lines.size() == 6);
    return lines;
}

// Frame `index` of lavfi_<k>.raw (frames of `frame_bytes` back to back).
std::vector<std::byte> referenceFrame(const std::vector<std::byte>& raw, std::size_t frame_bytes,
                                      std::int64_t index) {
    const std::size_t offset = static_cast<std::size_t>(index) * frame_bytes;
    REQUIRE(offset + frame_bytes <= raw.size());
    const auto begin = raw.begin() + static_cast<std::ptrdiff_t>(offset);
    return {begin, begin + static_cast<std::ptrdiff_t>(frame_bytes)};
}

std::size_t frameBytes(const media::ISource& source) {
    const media::FrameDesc& desc = source.info().desc;
    const auto layout = media::geometry(desc.format, desc.width, desc.height);
    REQUIRE_MESSAGE(layout.has_value(), layout.error().describe());
    return static_cast<std::size_t>(layout->bytes);
}

// Frame `index` of `source` equals `reference`; a failure names the graph, the frame and the first
// differing byte.
void checkFrame(FfmpegHarness& harness, const std::shared_ptr<media::ISource>& source,
                std::string_view graph, std::int64_t index,
                const std::vector<std::byte>& reference) {
    const auto ours = harness.originalBytes(source, index);
    REQUIRE_MESSAGE(ours.has_value(),
                    graph << " frame " << index << ": " << ours.error().describe());
    REQUIRE(ours->size() == reference.size());
    std::size_t differs = 0;
    while (differs < reference.size() && (*ours)[differs] == reference[differs]) {
        ++differs;
    }
    CHECK_MESSAGE(differs == reference.size(),
                  graph << " frame " << index << ": first differing byte at " << differs);
}

// An open that must fail: its error.
dvv::Error openError(FfmpegHarness& harness, std::string_view uri) {
    INFO(std::string(uri));
    auto opened = harness.tryOpen(uri);
    REQUIRE_FALSE(opened.has_value());
    return opened.error();
}

void checkError(const dvv::Error& error, dvv::Errc code, std::string_view text) {
    CAPTURE(error.describe());
    CHECK(error.code == code);
    CHECK(error.describe().find(text) != std::string::npos);
}

std::string formatName(const media::ISource& source) {
    return media::descriptor(source.info().desc.format).name;
}

constexpr std::string_view GRAPH_1 = "testsrc2=s=352x288:r=25,format=yuv420p";

}  // namespace

TEST_SUITE("ffmpeg") {
    TEST_CASE("lavfi: frames equal ffmpeg -f rawvideo") {
        FfmpegHarness harness;
        for (const LavfiFixture& fixture : manifest()) {
            CAPTURE(fixture.graph);
            const std::string uri = "lavfi:" + fixture.graph;
            const auto source = harness.open(uri);
            const media::SourceInfo& info = source->info();
            CHECK(info.provider == "lavfi");
            CHECK(info.reader == "lavfi");
            CHECK_FALSE(info.frameCount.has_value());
            CHECK(info.access == media::SourceAccess::Sequential);
            CHECK(info.desc.width == 352);
            CHECK(info.desc.height == 288);
            const std::string format = fixture.graph.substr(fixture.graph.rfind("format=") + 7);
            CHECK(formatName(*source) == format);
            CHECK(info.fps == media::Rational{25, 1});
            CHECK(info.resolvedUri == uri);
            const std::vector<std::byte> raw = dvv::components::testing::readFile(
                lavfiDirectory() / ("lavfi_" + std::to_string(fixture.k) + ".raw"));
            const std::size_t bytes = frameBytes(*source);
            REQUIRE(raw.size() == bytes * static_cast<std::size_t>(fixture.frames));
            for (std::int64_t index = 0; index < fixture.frames; ++index) {
                checkFrame(harness, source, fixture.graph, index,
                           referenceFrame(raw, bytes, index));
            }
        }
    }

    TEST_CASE("lavfi: a request behind the ring restarts the graph") {
        FfmpegHarness harness;
        const auto source = harness.open("lavfi:" + std::string(GRAPH_1));
        const std::vector<std::byte> raw =
            dvv::components::testing::readFile(lavfiDirectory() / "lavfi_1.raw");
        const std::size_t bytes = frameBytes(*source);
        for (const std::int64_t index : {20, 2, 23, 0}) {
            checkFrame(harness, source, GRAPH_1, index, referenceFrame(raw, bytes, index));
        }
    }

    TEST_CASE("lavfi: concurrent produce calls from one store") {
        FfmpegHarness harness;
        const auto source = harness.open("lavfi:" + std::string(GRAPH_1));
        const std::vector<std::byte> raw =
            dvv::components::testing::readFile(lavfiDirectory() / "lavfi_1.raw");
        const std::size_t bytes = frameBytes(*source);
        const media::OriginId origin = harness.origin(source);
        std::vector<dvv::Task<dvv::Result<media::OriginalRead>>> tasks;
        for (std::int64_t index = 11; index >= 0; --index) {
            tasks.push_back(harness.store().acquireOriginal({origin, index}, media::Api::HostPlanes,
                                                            harness.store().hostDomain(),
                                                            harness.cpu(), true));
        }
        auto reads = dvv::syncWait(dvv::whenAll(std::move(tasks)));
        REQUIRE(reads.size() == 12);
        for (std::size_t i = 0; i < reads.size(); ++i) {
            const auto index = static_cast<std::int64_t>(11 - i);
            CAPTURE(index);
            REQUIRE_MESSAGE(reads[i].has_value(), reads[i].error().describe());
            CHECK(dvv::components::testing::originalReadBytes(*reads[i]) ==
                  referenceFrame(raw, bytes, index));
        }
    }

    TEST_CASE("lavfi: a finite graph ends with Io") {
        FfmpegHarness harness;
        const auto source = harness.open("lavfi:testsrc2=s=64x48:r=25:d=0.4,format=yuv420p");
        std::vector<std::byte> first;
        for (std::int64_t index = 0; index < 10; ++index) {
            CAPTURE(index);
            auto bytes = harness.originalBytes(source, index);
            REQUIRE_MESSAGE(bytes.has_value(), bytes.error().describe());
            if (index == 0) {
                first = std::move(*bytes);
            }
        }
        for (const std::int64_t index : {10, 12}) {
            CAPTURE(index);
            const auto ended = harness.originalBytes(source, index);
            REQUIRE_FALSE(ended.has_value());
            checkError(ended.error(), dvv::Errc::Io, "the lavfi graph ended after 10 frames");
        }
        // Frame 0 lies behind the ring (2..9): the graph is rebuilt after its end.
        const auto again = harness.originalBytes(source, 0);
        REQUIRE_MESSAGE(again.has_value(), again.error().describe());
        CHECK(*again == first);
    }

    TEST_CASE("lavfi: colour from frame 0") {
        FfmpegHarness harness;
        const auto source = harness.open(
            "lavfi:testsrc2=s=64x48:r=25,format=yuv420p,setparams=colorspace=bt709:range=pc");
        const media::ColourParams colour = media::colourParams(source->info().desc.colour);
        CHECK(colour.matrix == media::ColourMatrix::Bt709);
        CHECK(colour.range == media::ColourRange::Full);
        CHECK(source->info().colourHint == media::FileHint{});
        CHECK(source->info().notes.empty());
    }

    TEST_CASE("lavfi: a non-standard tag sets the colour hint") {
        FfmpegHarness harness;
        // M/T/P = 4/2/2 (FCC matrix, the rest unspecified): signalled, but no field stands for it.
        const auto source =
            harness.open("lavfi:testsrc2=s=64x48:r=25,format=yuv420p,setparams=colorspace=fcc");
        const media::SourceInfo& info = source->info();
        CHECK(info.colourHint.nonStandard);
        CHECK(info.colourHint.bt601 == media::Bt601Hint::None);
        CHECK(media::colourParams(info.desc.colour).matrix == media::ColourMatrix::Unspecified);
        REQUIRE(info.notes.size() == 1);
        CHECK(info.notes[0].find("signalled but non-standard") != std::string::npos);
    }

    TEST_CASE("lavfi: a size change is Format") {
        FfmpegHarness harness;
        // The pinned FFmpeg (n8.1.3) evaluates scale's `n` one ahead of the output frame number
        // (showinfo: lt(n,3) changes the size at frame 2), so lt(n,4) changes it at frame 3.
        const auto source = harness.open(
            "lavfi:testsrc2=s=64x48:r=25,scale=w='if(lt(n,4),64,32)':h='if(lt(n,4),"
            "48,24)':eval=frame,format=yuv420p");
        for (std::int64_t index = 0; index < 3; ++index) {
            CAPTURE(index);
            const auto bytes = harness.originalBytes(source, index);
            CHECK_MESSAGE(bytes.has_value(), bytes.error().describe());
        }
        const auto changed = harness.originalBytes(source, 3);
        REQUIRE_FALSE(changed.has_value());
        checkError(changed.error(), dvv::Errc::Format, "frame 3 changed to 32x24 yuv420p");
    }

    TEST_CASE("lavfi: errors") {
        FfmpegHarness harness;
        // An allowed filter with an option it does not have: the graph does not apply.
        checkError(openError(harness, "lavfi:testsrc2=nosuchoption=1"), dvv::Errc::Format,
                   "the lavfi graph is invalid");
        // Unknown names and audio sources are not on the allowlist.
        checkError(openError(harness, "lavfi:nosuchfilter"), dvv::Errc::Unsupported,
                   "lavfi: filter nosuchfilter is not supported");
        // An input left open, and more than one output.
        checkError(openError(harness, "lavfi:null"), dvv::Errc::Format,
                   "the lavfi graph has an unconnected input");
        checkError(openError(harness, "lavfi:testsrc2=s=64x48:r=25;testsrc2=s=64x48:r=25"),
                   dvv::Errc::Format, "the lavfi graph needs exactly one output");
        checkError(openError(harness, "lavfi:testsrc2=s=64x48,format=bgr0"), dvv::Errc::Unsupported,
                   "not in the catalogue");
        CHECK(openError(harness, "lavfi:").code == dvv::Errc::Format);
        checkError(openError(harness, "lavfi:testsrc2?fmt=gray"), dvv::Errc::Format,
                   "take no query");
        checkError(openError(harness, "lavfi:sine"), dvv::Errc::Unsupported,
                   "lavfi: filter sine is not supported");
    }

    TEST_CASE("lavfi: destroying the source with queued requests stops it") {
        // Paced (as the ffmpeg provider's stop cases): frame 2999 takes at least 6 s on any
        // machine, so the stop must come from the supplier's own check while it pulls.
        const PacedWorkers paced;
        auto made =
            dvv::components::detail::makeGraphWorkerForTest("testsrc2=s=64x48:r=25,format=yuv420p");
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
        CHECK(paced.waitUntilDecoding());  // the first request is pulling, three are queued
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

    TEST_CASE("lavfi: graphs that open files or URLs are refused") {
        FfmpegHarness harness;
        auto& lifetimes = dvv::LifetimeRegistry::instance();
        // Relative with '/' separators: no drive letter, so no ':' to escape inside the graph.
        const std::filesystem::path clip =
            dvv::components::testing::fixture("lossless_huffyuv.avi");
        const std::u8string relative =
            std::filesystem::relative(clip, std::filesystem::current_path()).generic_u8string();
        const std::string rel(relative.begin(), relative.end());
        REQUIRE_FALSE(rel.empty());
        REQUIRE(rel.find(':') == std::string::npos);
        // Not on the allowlist (security review): file and URL sources, a QR source and drawtext
        // (font and text files). Without the check movie=<rel> opens and plays that file. The
        // check runs on the parsed names: a second chain, an instance name after '@'
        // and a quoted or escaped name all resolve to movie and are refused under that name.
        const std::array<std::pair<std::string, std::string_view>, 8> refused{{
            {"lavfi:movie=" + rel, "movie"},
            {"lavfi:amovie=" + rel, "amovie"},
            {"lavfi:qrencodesrc=text=x", "qrencodesrc"},
            {"lavfi:testsrc2=s=64x48:r=25,drawtext=textfile=x.txt,format=yuv420p", "drawtext"},
            {"lavfi:testsrc2=s=64x48:r=25,format=yuv420p;movie=" + rel, "movie"},
            {"lavfi:movie@x=" + rel, "movie"},
            {"lavfi:'mov''ie'=" + rel, "movie"},
            {"lavfi:mov\\ie=" + rel, "movie"},
        }};
        for (const auto& [uri, name] : refused) {
            CAPTURE(uri);
            const std::size_t live_before = lifetimes.liveCount();
            const dvv::Error error = openError(harness, uri);
            CHECK(error.code == dvv::Errc::Unsupported);
            CHECK(error.message == "lavfi: filter " + std::string(name) +
                                       " is not supported; open files with file: instead");
            CHECK(lifetimes.liveCount() == live_before);
        }
        const auto control = harness.open("lavfi:testsrc2=s=64x48:r=25,format=yuv420p");
        CHECK(control->info().provider == "lavfi");
    }

    TEST_CASE("lavfi: filters that can drop every frame are refused") {
        FfmpegHarness harness;
        // av_buffersink_get_frame runs the graph until a frame comes out, and nothing
        // can stop it. Over an unbounded source these never give a frame (select=0) or only after
        // an unbounded number of discarded frames, so the open or the close would hang.
        const std::array<std::pair<std::string_view, std::string_view>, 3> refused{{
            {"lavfi:testsrc2=s=64x48:r=25,select=0,format=yuv420p", "select"},
            {"lavfi:testsrc2=s=64x48:r=25,trim=start_frame=4000000000,format=yuv420p", "trim"},
            {"lavfi:testsrc2=s=64x48:r=25,fps=fps=1/100000000,format=yuv420p", "fps"},
        }};
        for (const auto& [uri, name] : refused) {
            CAPTURE(uri);
            const dvv::Error error = openError(harness, uri);
            CHECK(error.code == dvv::Errc::Unsupported);
            // None of them reads a file: no file: hint.
            CHECK(error.message == "lavfi: filter " + std::string(name) + " is not supported");
        }
    }

    TEST_CASE("lavfi: a filter with a second input is refused") {
        FfmpegHarness harness;
        // Scale's ref input (rw/rh) joins two generators by time; with this frame-rate
        // ratio every output frame discards 25 million ref frames, so the pull would not end.
        const dvv::Error error = openError(
            harness,
            "lavfi:testsrc2=s=64x48:r=1/1000000[m];testsrc2=s=32x24:r=25[r];[m][r]scale=w=rw:h=rh,"
            "format=yuv420p");
        CHECK(error.code == dvv::Errc::Unsupported);
        CHECK(error.message ==
              "lavfi: filter scale with 2 inputs is not supported (a graph is one chain)");
    }

    TEST_CASE("lavfi: random graphs are not deterministic") {
        FfmpegHarness harness;
        CHECK(harness.open("lavfi:testsrc2=s=64x48:r=25,format=yuv420p")->info().deterministic);
        // gradients' default seed and a "random" colour change on every build.
        for (const std::string_view uri : {"lavfi:gradients=s=64x48:r=25,format=yuv420p",
                                           "lavfi:color=c=random:s=64x48:r=25,format=yuv420p"}) {
            CAPTURE(uri);
            CHECK_FALSE(harness.open(uri)->info().deterministic);
        }
    }

    TEST_CASE("lavfi: every allowed generator opens") {
        FfmpegHarness harness;
        // The generators of LAVFI_ALLOWED_FILTERS; allrgb, allyuv and colorchart have a fixed size.
        const std::array<std::string_view, 16> generators{
            "testsrc",    "testsrc2",   "smptebars",  "smptehdbars",   "pal75bars", "pal100bars",
            "rgbtestsrc", "yuvtestsrc", "colorchart", "colorspectrum", "color",     "gradients",
            "allrgb",     "allyuv",     "nullsrc",    "mandelbrot"};
        for (const std::string_view generator : generators) {
            CAPTURE(generator);
            CHECK(dvv::components::detail::checkLavfiFilter(generator).has_value());
            const bool fixed_size =
                generator == "allrgb" || generator == "allyuv" || generator == "colorchart";
            const std::string uri = "lavfi:" + std::string(generator) +
                                    (fixed_size ? "=r=25" : "=s=64x48:r=25") + ",format=yuv420p";
            const auto source = harness.open(uri);
            CHECK(source->info().provider == "lavfi");
            CHECK(formatName(*source) == "yuv420p");
            for (const std::int64_t index : {0, 1}) {
                CAPTURE(index);
                const auto bytes = harness.originalBytes(source, index);
                CHECK_MESSAGE(bytes.has_value(), bytes.error().describe());
            }
        }
    }
}

// The FFmpeg binding's pure parts (M4 design D3, §7): library checks, search plan, file names and
// the AVERROR mapping, without FFmpeg loaded.

#include <doctest/doctest.h>

#include <cerrno>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/components/ffmpeg.h"
#include "dvv/media/types.h"
#include "dvv/registry/capabilities.h"
#include "ffmpeg/api.h"
#include "ffmpeg/loader_detail.h"

namespace {

using dvv::components::FfmpegLoadOptions;
using dvv::components::FfmpegStatus;
using dvv::components::detail::LibraryFacts;
using dvv::components::detail::libraryFileName;
using dvv::components::detail::planSearch;
using dvv::components::detail::refusalReason;
using dvv::components::detail::SearchSource;
using dvv::components::detail::sourceName;
using dvv::components::detail::versionText;

constexpr unsigned v(unsigned major, unsigned minor, unsigned micro) {
    return (major << 16) | (minor << 8) | micro;
}

std::vector<LibraryFacts> facts() {
    std::vector<LibraryFacts> libraries;
    for (const char* name :
         {"avutil", "avcodec", "avformat", "swscale", "swresample", "avfilter"}) {
        libraries.push_back(
            LibraryFacts{.name = name,
                         .runtimeVersion = v(60, 8, 100),
                         .compiledVersion = v(60, 8, 100),
                         .licence = "LGPL version 2.1 or later",
                         .configuration = "--prefix=/x --enable-shared --disable-static"});
    }
    return libraries;
}

LibraryFacts& library(std::vector<LibraryFacts>& libraries, const std::string& name) {
    for (LibraryFacts& entry : libraries) {
        if (entry.name == name) {
            return entry;
        }
    }
    FAIL("no library " << name);
    return libraries.front();
}

// An absolute directory on this platform.
std::filesystem::path absoluteDirectory() {
#if defined(_WIN32)
    return "X:/opt/ff";
#else
    return "/opt/ff";
#endif
}

std::vector<std::filesystem::path> normal(const std::vector<std::filesystem::path>& paths) {
    std::vector<std::filesystem::path> result;
    result.reserve(paths.size());
    for (const auto& path : paths) {
        result.push_back(path.lexically_normal());
    }
    return result;
}

}  // namespace

TEST_CASE("refusalReason: an LGPL build of the compiled versions passes") {
    CHECK(refusalReason(facts()) == std::nullopt);
    CHECK(refusalReason({}) == "has no libraries to check");
}

TEST_CASE("refusalReason: a GPL or nonfree build is refused") {
    {
        auto libraries = facts();
        library(libraries, "swscale").licence = "GPL version 2 or later";
        CHECK(refusalReason(libraries) ==
              "is a GPL build (swscale: GPL version 2 or later); rawreel uses only LGPL builds");
    }
    {
        auto libraries = facts();
        library(libraries, "avcodec").licence = "nonfree and unredistributable";
        CHECK(refusalReason(libraries) ==
              "is not an LGPL build (avcodec: nonfree and "
              "unredistributable); rawreel uses only LGPL builds");
    }
    {
        auto libraries = facts();
        library(libraries, "avformat").configuration = "--enable-shared --enable-gpl";
        CHECK(refusalReason(libraries) ==
              "is a GPL build (avformat configured with --enable-gpl); "
              "rawreel uses only LGPL builds");
    }
    {
        auto libraries = facts();
        library(libraries, "avfilter").configuration = "--enable-nonfree";
        CHECK(refusalReason(libraries) ==
              "is a nonfree build (avfilter configured with --enable-nonfree); rawreel uses only "
              "LGPL builds");
    }
}

TEST_CASE("refusalReason: versions must match the headers") {
    auto libraries = facts();
    LibraryFacts& avformat = library(libraries, "avformat");
    avformat.compiledVersion = v(62, 3, 100);
    avformat.runtimeVersion = v(61, 3, 100);
    CHECK(refusalReason(libraries) ==
          "has avformat 61.3.100, but rawreel was built for 62.3.100 (the major version must "
          "match)");
    avformat.runtimeVersion = v(62, 2, 100);
    CHECK(refusalReason(libraries) ==
          "has avformat 62.2.100, older than the 62.3.100 rawreel was built for");
    avformat.runtimeVersion = v(62, 4, 0);
    CHECK(refusalReason(libraries) == std::nullopt);
    avformat.runtimeVersion = v(62, 3, 99);
    CHECK(refusalReason(libraries) == std::nullopt);
}

TEST_CASE("refusalReason: the first problem in order is reported") {
    auto libraries = facts();
    library(libraries, "avutil").runtimeVersion = v(60, 7, 100);
    library(libraries, "swscale").licence = "GPL version 2 or later";
    CHECK(refusalReason(libraries) ==
          "has avutil 60.7.100, older than the 60.8.100 rawreel was built for");
}

TEST_CASE("planSearch: explicit directories are final and must be absolute") {
    const std::filesystem::path a = absoluteDirectory();
    {
        const auto plan = planSearch(FfmpegLoadOptions{.directory = a}, "/elsewhere", "");
        CHECK(plan.directories == std::vector<std::filesystem::path>{a});
        CHECK(plan.source == SearchSource::Options);
        CHECK(plan.error.empty());
    }
    {
        const auto plan = planSearch(
            FfmpegLoadOptions{.directory = std::filesystem::path("ffmpeg")}, std::nullopt, "");
        CHECK(plan.directories.empty());
        CHECK(plan.error == "the FFmpeg directory must be absolute (got 'ffmpeg')");
    }
    {
        const auto plan = planSearch(FfmpegLoadOptions{}, dvv::media::utf8FromPath(a), "");
        CHECK(plan.directories == std::vector<std::filesystem::path>{a});
        CHECK(plan.source == SearchSource::Environment);
        CHECK(plan.error.empty());
    }
    {
        const auto plan = planSearch(FfmpegLoadOptions{}, "rel/dir", "");
        CHECK(plan.directories.empty());
        CHECK(plan.error == "DVV_FFMPEG_DIR must be absolute (got 'rel/dir')");
    }
    CHECK(sourceName(SearchSource::Options) == "the given directory");
    CHECK(sourceName(SearchSource::Environment) == "DVV_FFMPEG_DIR");
    CHECK(sourceName(SearchSource::Default) == "default search");
}

TEST_CASE("planSearch: the default search looks next to the executable") {
    const std::filesystem::path a = absoluteDirectory();
    const std::filesystem::path e = a / "bin";
    const std::vector<std::filesystem::path> expected =
        normal({e / "ffmpeg", a / "lib" / "ffmpeg"});
    for (const std::optional<std::string>& environment :
         {std::optional<std::string>(), std::optional<std::string>("")}) {
        CAPTURE(environment.has_value());
        const auto plan = planSearch(FfmpegLoadOptions{}, environment, e);
        CHECK(normal(plan.directories) == expected);
        CHECK(plan.source == SearchSource::Default);
        CHECK(plan.error.empty());
    }
    const auto plan = planSearch(FfmpegLoadOptions{}, std::nullopt, "");
    CHECK(plan.directories.empty());
    CHECK(plan.source == SearchSource::Default);
    CHECK(plan.error.empty());
}

TEST_CASE("libraryFileName and versionText") {
#if defined(_WIN32)
    CHECK(libraryFileName("avutil", 60) == "avutil-60.dll");
    CHECK(libraryFileName("swresample", 6) == "swresample-6.dll");
#else
    CHECK(libraryFileName("avutil", 60) == "libavutil.so.60");
    CHECK(libraryFileName("swresample", 6) == "libswresample.so.6");
#endif
    CHECK(versionText(v(62, 3, 100)) == "62.3.100");
}

TEST_CASE("errcForAverror maps the codes of the design") {
    using dvv::Errc;
    using dvv::components::ffmpeg::errcForAverror;
    CHECK(errcForAverror(AVERROR(ENOMEM)) == Errc::OutOfMemory);
    CHECK(errcForAverror(AVERROR(ENOENT)) == Errc::Io);
    CHECK(errcForAverror(AVERROR(EIO)) == Errc::Io);
    CHECK(errcForAverror(AVERROR(EACCES)) == Errc::Io);
    CHECK(errcForAverror(AVERROR_INVALIDDATA) == Errc::Format);
    CHECK(errcForAverror(AVERROR_DECODER_NOT_FOUND) == Errc::Unsupported);
    CHECK(errcForAverror(AVERROR_DEMUXER_NOT_FOUND) == Errc::Unsupported);
    CHECK(errcForAverror(AVERROR_STREAM_NOT_FOUND) == Errc::Unsupported);
    CHECK(errcForAverror(AVERROR_PATCHWELCOME) == Errc::Unsupported);
    CHECK(errcForAverror(AVERROR(ENOSYS)) == Errc::Unsupported);
    CHECK(errcForAverror(AVERROR_EOF) == Errc::Internal);
    CHECK(errcForAverror(AVERROR_BUG) == Errc::Internal);
    CHECK(errcForAverror(-12345) == Errc::Internal);
}

TEST_CASE("ffmpegError without FFmpeg loaded uses the fallback text") {
    const dvv::Error error = dvv::components::ffmpeg::ffmpegError("x", AVERROR(ENOENT));
    CHECK(error.code == dvv::Errc::Io);
    CHECK(error.message == std::format("x failed (AVERROR {})", AVERROR(ENOENT)));
}

TEST_CASE("addFfmpegCapabilities follows the status") {
    using dvv::components::addFfmpegCapabilities;
    {
        dvv::Capabilities caps;
        addFfmpegCapabilities(FfmpegStatus{}, caps);
        CHECK(caps.names().empty());
    }
    FfmpegStatus status;
    status.core = true;
    {
        dvv::Capabilities caps;
        addFfmpegCapabilities(status, caps);
        CHECK(caps.names() == std::vector<std::string>{"ffmpeg"});
    }
    status.lavfi = true;
    {
        dvv::Capabilities caps;
        addFfmpegCapabilities(status, caps);
        CHECK(caps.names() == std::vector<std::string>{"ffmpeg", "ffmpeg.lavfi"});
    }
}

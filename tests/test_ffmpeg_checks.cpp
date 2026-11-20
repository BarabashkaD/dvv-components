// The FFmpeg binding's pure parts: library checks, search plan, file names and
// the AVERROR mapping, without FFmpeg loaded.

#include <doctest/doctest.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <type_traits>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/utf8.h"
#include "dvv/components/ffmpeg.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/module.h"
#include "dvv/registry/parameters.h"
#include "ffmpeg/api.h"
#include "ffmpeg/loader_detail.h"
#include "sources/av_worker.h"
#include "sources/ffmpeg_module.h"

namespace {

using dvv::components::FfmpegLoadOptions;
using dvv::components::FfmpegStatus;
using dvv::components::detail::LibraryInfo;
using dvv::components::detail::moduleLoadOf;
using dvv::components::detail::moduleOptions;
using dvv::components::detail::planSearch;
using dvv::components::detail::refusalReason;
using dvv::components::detail::SearchSource;
using dvv::components::detail::sourceName;
using dvv::components::detail::versionText;

// The old name stays until 1.0.0.
static_assert(
    std::is_same_v<dvv::components::detail::LibraryFacts, dvv::components::detail::LibraryInfo>);

constexpr unsigned v(unsigned major, unsigned minor, unsigned micro) {
    return (major << 16) | (minor << 8) | micro;
}

std::vector<LibraryInfo> facts() {
    std::vector<LibraryInfo> libraries;
    for (const char* name :
         {"avutil", "avcodec", "avformat", "swscale", "swresample", "avfilter"}) {
        libraries.push_back(
            LibraryInfo{.name = name,
                        .runtimeVersion = v(60, 8, 100),
                        .compiledVersion = v(60, 8, 100),
                        .licence = "LGPL version 2.1 or later",
                        .configuration = "--prefix=/x --enable-shared --disable-static"});
    }
    return libraries;
}

LibraryInfo& library(std::vector<LibraryInfo>& libraries, const std::string& name) {
    for (LibraryInfo& entry : libraries) {
        if (entry.name == name) {
            return entry;
        }
    }
    FAIL("no library " << name);
    return libraries.front();
}

// Both FFmpeg capabilities, in the module's produces order.
std::vector<std::string> bothCapabilities() {
    return {std::string(dvv::components::FFMPEG_CAPABILITY),
            std::string(dvv::components::FFMPEG_LAVFI_CAPABILITY)};
}

// The property the module adds when DVV_FFMPEG_DIR chose the folder.
dvv::ModuleProperty variableProperty() {
    return dvv::ModuleProperty{.key = std::string(dvv::MODULE_VARIABLE_PROPERTY),
                               .value = "DVV_FFMPEG_DIR"};
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
              "is a GPL build (swscale: GPL version 2 or later); only LGPL builds are accepted");
    }
    {
        auto libraries = facts();
        library(libraries, "avcodec").licence = "nonfree and unredistributable";
        CHECK(refusalReason(libraries) ==
              "is not an LGPL build (avcodec: nonfree and "
              "unredistributable); only LGPL builds are accepted");
    }
    {
        auto libraries = facts();
        library(libraries, "avformat").configuration = "--enable-shared --enable-gpl";
        CHECK(refusalReason(libraries) ==
              "is a GPL build (avformat configured with --enable-gpl); "
              "only LGPL builds are accepted");
    }
    {
        auto libraries = facts();
        library(libraries, "avfilter").configuration = "--enable-nonfree";
        CHECK(refusalReason(libraries) ==
              "is a nonfree build (avfilter configured with --enable-nonfree); only LGPL builds "
              "are accepted");
    }
}

TEST_CASE("refusalReason: versions must match the headers") {
    auto libraries = facts();
    LibraryInfo& avformat = library(libraries, "avformat");
    avformat.compiledVersion = v(62, 3, 100);
    avformat.runtimeVersion = v(61, 3, 100);
    CHECK(
        refusalReason(libraries) ==
        "has avformat 61.3.100, but dvv-components was built for 62.3.100 (the major version must "
        "match)");
    avformat.runtimeVersion = v(62, 2, 100);
    CHECK(refusalReason(libraries) ==
          "has avformat 62.2.100, older than the 62.3.100 dvv-components was built for");
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
          "has avutil 60.7.100, older than the 60.8.100 dvv-components was built for");
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
        const auto plan = planSearch(FfmpegLoadOptions{}, dvv::pathText(a), "");
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

#if !defined(_WIN32)
TEST_CASE("planSearch keeps a non-UTF-8 DVV_FFMPEG_DIR") {
    // A POSIX path is bytes; the search keeps them, and the messages show U+FFFD for them.
    const auto kept = planSearch({},
                                 "/tmp/a\xFF"
                                 "b",
                                 "");
    CHECK(kept.directories.size() == 1);
    if (!kept.directories.empty()) {
        CHECK(kept.directories.front().native().find('\xFF') != std::string::npos);
    }
    const auto relative = planSearch({},
                                     "a\xFF"
                                     "b",
                                     "");
    CHECK(relative.error ==
          "DVV_FFMPEG_DIR must be absolute (got 'a\xEF\xBF\xBD"
          "b')");
    FfmpegLoadOptions options;
    options.directory = dvv::pathFromText(
        "a\xFF"
        "b");
    const auto given = planSearch(options, std::nullopt, "");
    CHECK(given.error ==
          "the FFmpeg directory must be absolute (got 'a\xEF\xBF\xBD"
          "b')");
}
#endif

TEST_CASE("planSearch: the default search looks next to the executable") {
    const std::filesystem::path a = absoluteDirectory();
    const std::filesystem::path e = a / "bin";
#if defined(__APPLE__)
    const std::vector<std::filesystem::path> expected =
        normal({e / "ffmpeg", a / "lib" / "ffmpeg", a / "Frameworks" / "ffmpeg"});
#else
    const std::vector<std::filesystem::path> expected =
        normal({e / "ffmpeg", a / "lib" / "ffmpeg"});
#endif
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

TEST_CASE("versionText") {
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

TEST_CASE("moduleLoadOf maps the status") {
    FfmpegStatus status;
    status.reason = "r";
    {
        const dvv::ModuleLoad module_load = moduleLoadOf(status, SearchSource::Default);
        CHECK(module_load.provides.empty());
        CHECK(module_load.reason == "r");
        CHECK(module_load.properties.empty());
        CHECK(module_load.handle == nullptr);
    }
    {
        // The variable property also on a failure.
        const dvv::ModuleLoad module_load = moduleLoadOf(status, SearchSource::Environment);
        CHECK(module_load.provides.empty());
        CHECK(module_load.reason == "r");
        CHECK(module_load.properties == std::vector{variableProperty()});
        CHECK(module_load.handle == nullptr);
    }

    status.core = true;
    status.version = "n8.1.3";
    status.licence = "LGPL version 2.1 or later";
    status.directory = dvv::pathFromText("/opt/ffmpeg");
    status.reason = "lavfi off";
    std::vector<dvv::ModuleProperty> properties{
        dvv::ModuleProperty{.key = std::string(dvv::MODULE_VERSION_PROPERTY), .value = "n8.1.3"},
        dvv::ModuleProperty{.key = std::string(dvv::MODULE_LICENCE_PROPERTY),
                            .value = "LGPL version 2.1 or later"},
        dvv::ModuleProperty{.key = std::string(dvv::MODULE_DIRECTORY_PROPERTY),
                            .value = dvv::pathText(status.directory)},
    };
    {
        const dvv::ModuleLoad module_load = moduleLoadOf(status, SearchSource::Default);
        CHECK(module_load.provides ==
              std::vector<std::string>{std::string(dvv::components::FFMPEG_CAPABILITY)});
        CHECK(module_load.reason == "lavfi off");
        CHECK(module_load.properties == properties);
        CHECK(module_load.handle == nullptr);
    }

    status.lavfi = true;
    status.reason.clear();
    {
        const dvv::ModuleLoad module_load = moduleLoadOf(status, SearchSource::Options);
        CHECK(module_load.provides == bothCapabilities());
        CHECK(module_load.reason.empty());
        CHECK(module_load.properties == properties);
        CHECK(module_load.handle == nullptr);
    }
    properties.push_back(variableProperty());
    {
        const dvv::ModuleLoad module_load = moduleLoadOf(status, SearchSource::Environment);
        CHECK(module_load.provides == bothCapabilities());
        CHECK(module_load.reason.empty());
        CHECK(module_load.properties == properties);
        CHECK(module_load.handle == nullptr);
    }
}

TEST_CASE("moduleOptions reads the directory and refuses the rest") {
    const std::string directory_option(dvv::components::FFMPEG_DIRECTORY_OPTION);
    {
        const auto parsed = moduleOptions(dvv::ParameterValues{});
        CHECK(parsed.has_value());
        if (parsed) {
            CHECK_FALSE(parsed->directory.has_value());
        }
    }
    {
        const auto parsed =
            moduleOptions(dvv::ParameterValues{{directory_option, std::string("/opt/ffmpeg")}});
        CHECK(parsed.has_value());
        CHECK((parsed && parsed->directory.has_value()));
        if (parsed && parsed->directory) {
            CHECK(*parsed->directory == dvv::pathFromText("/opt/ffmpeg"));
        }
    }
    {
        const auto parsed =
            moduleOptions(dvv::ParameterValues{{directory_option, std::int64_t{1}}});
        CHECK_FALSE(parsed.has_value());
        if (!parsed) {
            CHECK(parsed.error() == "option 'directory' must be text (a folder)");
        }
    }
    {
        const auto parsed = moduleOptions(dvv::ParameterValues{{"folder", std::string("x")}});
        CHECK_FALSE(parsed.has_value());
        if (!parsed) {
            CHECK(parsed.error() ==
                  "unknown option 'folder' (the FFmpeg module takes 'directory')");
        }
    }
    {
        // An empty folder is kept; planSearch refuses it as relative (no default search).
        const auto parsed = moduleOptions(dvv::ParameterValues{{directory_option, std::string()}});
        CHECK(parsed.has_value());
        if (parsed) {
            CHECK(parsed->directory == std::optional{std::filesystem::path{}});
            const auto plan = planSearch(*parsed, std::nullopt, dvv::pathFromText("/opt/app"));
            CHECK(plan.error == "the FFmpeg directory must be absolute (got '')");
            CHECK(plan.directories.empty());
        }
    }
#if !defined(_WIN32)
    {
        // The folder is path text: a byte that is not UTF-8 stays.
        const std::string folder_text = std::string(
            "/tmp/a\xFF"
            "b");
        const auto parsed = moduleOptions(dvv::ParameterValues{{directory_option, folder_text}});
        CHECK(parsed.has_value());
        CHECK((parsed && parsed->directory.has_value()));
        if (parsed && parsed->directory) {
            CHECK(dvv::pathText(*parsed->directory) == folder_text);
        }
    }
#endif
}

TEST_CASE("averrorText without FFmpeg loaded is empty") {
    CHECK_FALSE(dvv::components::ffmpeg::averrorText(-22).has_value());
}

TEST_CASE("StopCheck: the token gives Cancelled, the flag and the generation give source closed") {
    using dvv::Errc;
    using dvv::components::detail::StopCheck;
    const std::uint64_t gen = dvv::components::ffmpeg::stopGeneration();
    std::stop_source source;
    source.request_stop();
    const std::stop_token stopped = source.get_token();
    const std::atomic<bool> set{true};

    const StopCheck running{.flag = nullptr, .generation = gen, .token = {}};
    CHECK_FALSE(running());

    const StopCheck token_only{.flag = nullptr, .generation = gen, .token = stopped};
    CHECK(token_only());
    CHECK(token_only.error().code == Errc::Cancelled);
    CHECK(token_only.error().message == "source request cancelled");

    const StopCheck flag_only{.flag = &set, .generation = gen, .token = {}};
    CHECK(flag_only());
    CHECK(flag_only.error().code == Errc::Internal);
    CHECK(flag_only.error().message == "source closed");

    const StopCheck generation_only{.flag = nullptr, .generation = gen + 1, .token = {}};
    CHECK(generation_only());
    CHECK(generation_only.error().code == Errc::Internal);
    CHECK(generation_only.error().message == "source closed");

    const StopCheck flag_and_token{.flag = &set, .generation = gen, .token = stopped};
    CHECK(flag_and_token());
    CHECK(flag_and_token.error().code == Errc::Cancelled);
    CHECK(flag_and_token.error().message == "source request cancelled");

    const StopCheck generation_and_token{.flag = nullptr, .generation = gen + 1, .token = stopped};
    CHECK(generation_and_token());
    CHECK(generation_and_token.error().code == Errc::Cancelled);
    CHECK(generation_and_token.error().message == "source request cancelled");
}

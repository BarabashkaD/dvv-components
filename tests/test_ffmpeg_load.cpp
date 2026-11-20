// The FFmpeg loader and the FFmpeg module against the prefix's FFmpeg and against directories
// without one. FFmpeg loads once per process, so each suite runs in its own
// CTest entry.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/log.h"
#include "dvv/base/utf8.h"
#include "dvv/components/ffmpeg.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_registry.h"
#include "dvv/registry/module.h"
#include "dvv/registry/parameters.h"
#include "ffmpeg/api.h"
#include "ffmpeg_support.h"

namespace {

using dvv::components::addFfmpegCapabilities;
using dvv::components::FFMPEG_CAPABILITY;
using dvv::components::FFMPEG_DIRECTORY_OPTION;
using dvv::components::FFMPEG_LAVFI_CAPABILITY;
using dvv::components::FFMPEG_MODULE;
using dvv::components::FfmpegLoadOptions;
using dvv::components::loadFfmpeg;

// The module's reason for a `directory` option that is not text.
constexpr std::string_view OPTION_NOT_TEXT = "option 'directory' must be text (a folder)";

bool anyLineContains(const std::vector<std::string>& lines, std::string_view text) {
    return std::ranges::any_of(
        lines, [&](const std::string& line) { return line.find(text) != std::string::npos; });
}

// The two capabilities of the FFmpeg module, in its produces order.
std::vector<std::string> bothCapabilities() {
    return {std::string(FFMPEG_CAPABILITY), std::string(FFMPEG_LAVFI_CAPABILITY)};
}

// The property the module adds when DVV_FFMPEG_DIR chose the folder.
dvv::ModuleProperty variableProperty() {
    return dvv::ModuleProperty{.key = std::string(dvv::MODULE_VARIABLE_PROPERTY),
                               .value = "DVV_FFMPEG_DIR"};
}

// The module's properties when FFmpeg loaded from DVV_FFMPEG_DIR, in their order.
std::vector<dvv::ModuleProperty> availableProperties() {
    const auto& status = loadFfmpeg();
    return {
        dvv::ModuleProperty{.key = std::string(dvv::MODULE_VERSION_PROPERTY),
                            .value = status.version},
        dvv::ModuleProperty{.key = std::string(dvv::MODULE_LICENCE_PROPERTY),
                            .value = status.licence},
        dvv::ModuleProperty{.key = std::string(dvv::MODULE_DIRECTORY_PROPERTY),
                            .value = dvv::pathText(status.directory)},
        variableProperty(),
    };
}

// A registry whose FFmpeg module gets `directory` as its one option.
std::unique_ptr<dvv::ComponentRegistry> loadWithDirectory(const dvv::ParameterValue& directory) {
    return dvv::ComponentRegistry::load(
        {.modules = {
             {std::string(FFMPEG_MODULE), {{std::string(FFMPEG_DIRECTORY_OPTION), directory}}}}});
}

// Two threads each load a registry at the same time.
std::array<std::unique_ptr<dvv::ComponentRegistry>, 2> loadFromTwoThreads() {
    std::array<std::unique_ptr<dvv::ComponentRegistry>, 2> registries;
    std::thread first([&registries] { registries[0] = dvv::ComponentRegistry::load(); });
    std::thread second([&registries] { registries[1] = dvv::ComponentRegistry::load(); });
    first.join();
    second.join();
    return registries;
}

// ModuleStatus has no operator==: compares what a load reports, field by field.
void checkSameStatus(const dvv::ModuleStatus& first, const dvv::ModuleStatus& second) {
    CHECK(first.provides == second.provides);
    CHECK(first.withheld == second.withheld);
    CHECK(first.reason == second.reason);
    CHECK(first.properties == second.properties);
}

}  // namespace

TEST_SUITE("ffmpeg") {
    TEST_CASE("loader: the prefix's FFmpeg loads and passes the checks") {
        const auto& status = loadFfmpeg();
        CHECK(status.core);
        CHECK(status.lavfi);
        CHECK(status.reason.empty());
        CHECK(status.licence.starts_with("LGPL"));
        CHECK(status.version == "n8.1.3");
        CHECK(std::filesystem::equivalent(status.directory,
                                          dvv::components::testing::ffmpegRuntimeDirectory()));
        const std::array<unsigned, 6> majors{
            LIBAVUTIL_VERSION_MAJOR,  LIBAVCODEC_VERSION_MAJOR,    LIBAVFORMAT_VERSION_MAJOR,
            LIBSWSCALE_VERSION_MAJOR, LIBSWRESAMPLE_VERSION_MAJOR, LIBAVFILTER_VERSION_MAJOR};
        for (std::size_t i = 0; i < majors.size(); ++i) {
            CAPTURE(i);
            CHECK((status.libraryVersions[i] >> 16) == majors[i]);
        }
        const dvv::components::ffmpeg::Api& api = dvv::components::ffmpeg::api();
        // NOLINTBEGIN(bugprone-macro-parentheses)
#define DVV_FFMPEG_CHECK_MEMBER(library_id, symbol) CHECK_MESSAGE(api.symbol != nullptr, #symbol);
        DVV_FFMPEG_FUNCTIONS(DVV_FFMPEG_CHECK_MEMBER)
#undef DVV_FFMPEG_CHECK_MEMBER
        // NOLINTEND(bugprone-macro-parentheses)
    }

    TEST_CASE("loader: later calls return the first result") {
        const auto& first = loadFfmpeg();
        const auto& second =
            loadFfmpeg(FfmpegLoadOptions{.directory = std::filesystem::path("relative")});
        CHECK(&first == &second);
        CHECK(second.core);
    }

    TEST_CASE("loader: FFmpeg's log reaches the dvv logger, capped at warning") {
        REQUIRE(loadFfmpeg().core);
        auto sink = std::make_shared<dvv::MemorySink>();
        dvv::Logger::instance().addSink(sink);
        const auto& api = dvv::components::ffmpeg::api();
        api.av_log(nullptr, AV_LOG_ERROR, "dvv-test-error %d\n", 1);
        api.av_log(nullptr, AV_LOG_WARNING, "dvv-test-warning\n");
        api.av_log(nullptr, AV_LOG_INFO, "dvv-test-info\n");
        // The sink stays attached but gets nothing more; the other sinks keep theirs.
        dvv::Logger::instance().setSinkLevel(*sink, dvv::LogLevel::Off);

        const std::vector<std::string> lines = sink->lines();
        const std::string_view warn = dvv::logLevelName(dvv::LogLevel::Warn);
        const auto has = [&](std::string_view text) {
            return std::ranges::any_of(lines, [&](const std::string& line) {
                return line.find(text) != std::string::npos && line.find(warn) != std::string::npos;
            });
        };
        CHECK(has("FFmpeg: dvv-test-error 1"));
        CHECK(has("FFmpeg: dvv-test-warning"));
        CHECK_FALSE(anyLineContains(lines, "dvv-test-info"));
        CHECK_FALSE(anyLineContains(lines, dvv::logLevelName(dvv::LogLevel::Error)));
    }

    TEST_CASE("loader: ffmpegError names the call and maps the code") {
        REQUIRE(loadFfmpeg().core);
        using dvv::components::ffmpeg::ffmpegError;
        const dvv::Error missing = ffmpegError("avformat_open_input", AVERROR(ENOENT));
        CHECK(missing.code == dvv::Errc::Io);
        const std::string_view prefix = "avformat_open_input failed: ";
        const std::string_view suffix = "(AVERROR -2)";
        CHECK(missing.message.starts_with(prefix));
        CHECK(missing.message.ends_with(suffix));
        CHECK(missing.message.size() > prefix.size() + suffix.size() + 1);
        const dvv::Error invalid = ffmpegError("avcodec_send_packet", AVERROR_INVALIDDATA);
        CHECK(invalid.code == dvv::Errc::Format);
        CHECK(invalid.message.ends_with(std::format("(AVERROR {})", AVERROR_INVALIDDATA)));
    }

    TEST_CASE("loader: the capabilities follow the status") {
        dvv::Capabilities caps;
        addFfmpegCapabilities(loadFfmpeg(), caps);
        CHECK(caps.has(FFMPEG_CAPABILITY));
        CHECK(caps.has(FFMPEG_LAVFI_CAPABILITY));
    }

    TEST_CASE("load() provides both capabilities") {
        const auto registry = dvv::ComponentRegistry::load();
        REQUIRE(registry != nullptr);
        const dvv::ModuleStatus* module = registry->module(FFMPEG_MODULE);
        CHECK(module != nullptr);
        if (module) {
            CHECK(module->provides == bothCapabilities());
            CHECK(module->withheld.empty());
            CHECK(module->reason.empty());
            const std::string directory = dvv::pathText(loadFfmpeg().directory);
            CHECK(module->property(dvv::MODULE_VERSION_PROPERTY) ==
                  std::optional<std::string_view>{loadFfmpeg().version});
            CHECK(module->property(dvv::MODULE_LICENCE_PROPERTY) ==
                  std::optional<std::string_view>{loadFfmpeg().licence});
            CHECK(module->property(dvv::MODULE_DIRECTORY_PROPERTY) ==
                  std::optional<std::string_view>{directory});
            CHECK(std::filesystem::equivalent(dvv::pathFromText(directory),
                                              dvv::components::testing::ffmpegRuntimeDirectory()));
            CHECK_FALSE(module->properties.empty());
            if (!module->properties.empty()) {
                CHECK(module->properties.back() == variableProperty());
            }
            CHECK(module->properties == availableProperties());
        }
        CHECK(registry->capabilities().has(FFMPEG_CAPABILITY));
        CHECK(registry->capabilities().has(FFMPEG_LAVFI_CAPABILITY));
    }

    TEST_CASE("load() from two threads reports one status") {
        const auto registries = loadFromTwoThreads();
        REQUIRE(registries[0] != nullptr);
        REQUIRE(registries[1] != nullptr);
        const dvv::ModuleStatus* first = registries[0]->module(FFMPEG_MODULE);
        const dvv::ModuleStatus* second = registries[1]->module(FFMPEG_MODULE);
        CHECK(first != nullptr);
        CHECK(second != nullptr);
        if (first && second) {
            checkSameStatus(*first, *second);
            CHECK(first->provides == bothCapabilities());
            CHECK(first->properties == availableProperties());
        }
    }

    TEST_CASE("a second load() keeps the first result") {
        const auto first = dvv::ComponentRegistry::load();
        const auto second = loadWithDirectory(std::string("/opt/other-ffmpeg"));
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        const dvv::ModuleStatus* first_module = first->module(FFMPEG_MODULE);
        const dvv::ModuleStatus* second_module = second->module(FFMPEG_MODULE);
        CHECK(first_module != nullptr);
        CHECK(second_module != nullptr);
        if (first_module && second_module) {
            checkSameStatus(*first_module, *second_module);
            CHECK(second_module->provides == bothCapabilities());
            CHECK(second_module->properties == availableProperties());
        }
    }

    TEST_CASE("a bad option provides nothing; a valid load after it provides both") {
        const auto refused = loadWithDirectory(std::int64_t{1});
        REQUIRE(refused != nullptr);
        const dvv::ModuleStatus* refused_module = refused->module(FFMPEG_MODULE);
        CHECK(refused_module != nullptr);
        if (refused_module) {
            CHECK(refused_module->provides.empty());
            CHECK(refused_module->withheld == bothCapabilities());
            CHECK(refused_module->reason == OPTION_NOT_TEXT);
            CHECK(refused_module->properties.empty());
        }
        CHECK_FALSE(refused->capabilities().has(FFMPEG_CAPABILITY));

        const auto loaded = dvv::ComponentRegistry::load();
        REQUIRE(loaded != nullptr);
        const dvv::ModuleStatus* module = loaded->module(FFMPEG_MODULE);
        CHECK(module != nullptr);
        if (module) {
            CHECK(module->provides == bothCapabilities());
        }
    }

    TEST_CASE("load() logs no folder") {
        const std::string folder = dvv::pathText(loadFfmpeg().directory);
        REQUIRE_FALSE(folder.empty());
        auto sink = std::make_shared<dvv::MemorySink>();
        dvv::Logger::instance().addSink(sink);
        const auto registry = loadWithDirectory(folder);
        // The sink stays attached but gets nothing more; the other sinks keep theirs.
        dvv::Logger::instance().setSinkLevel(*sink, dvv::LogLevel::Off);

        REQUIRE(registry != nullptr);
        const dvv::ModuleStatus* module = registry->module(FFMPEG_MODULE);
        CHECK(module != nullptr);
        if (module) {
            CHECK(module->property(dvv::MODULE_DIRECTORY_PROPERTY) ==
                  std::optional<std::string_view>{folder});
        }
        const std::vector<std::string> lines = sink->lines();
        // The registry logged the module, by its id only.
        CHECK(anyLineContains(lines, std::format("module '{}'", FFMPEG_MODULE)));
        CHECK_FALSE(anyLineContains(lines, folder));
    }
}

TEST_SUITE("ffmpeg-absent") {
    TEST_CASE("load() from two threads reports one status (first load)") {
        // main loads nothing in this mode, so the two threads make the process's first load.
        const auto registries = loadFromTwoThreads();
        REQUIRE(registries[0] != nullptr);
        REQUIRE(registries[1] != nullptr);
        const dvv::ModuleStatus* first = registries[0]->module(FFMPEG_MODULE);
        const dvv::ModuleStatus* second = registries[1]->module(FFMPEG_MODULE);
        CHECK(first != nullptr);
        CHECK(second != nullptr);
        if (first && second) {
            checkSameStatus(*first, *second);
            CHECK(first->provides.empty());
            CHECK(first->withheld == bothCapabilities());
            CHECK(first->properties == std::vector{variableProperty()});
        }
    }

    TEST_CASE("load() withholds both with the reason") {
        const auto registry = dvv::ComponentRegistry::load();
        REQUIRE(registry != nullptr);
        const dvv::ModuleStatus* module = registry->module(FFMPEG_MODULE);
        CHECK(module != nullptr);
        if (module) {
            CHECK(module->provides.empty());
            CHECK(module->withheld == bothCapabilities());
            CHECK(module->reason == loadFfmpeg().reason);
            CHECK(module->properties == std::vector{variableProperty()});
        }
        CHECK_FALSE(registry->capabilities().has(FFMPEG_CAPABILITY));
    }

    TEST_CASE("loader: an empty FFmpeg directory leaves FFmpeg off with the reason") {
        const auto& status = loadFfmpeg();
        CHECK_FALSE(status.core);
        CHECK_FALSE(status.lavfi);
        CAPTURE(status.reason);
        CHECK(status.reason.find("(DVV_FFMPEG_DIR)") != std::string::npos);
        CHECK(status.reason.find("no-ffmpeg") != std::string::npos);
        CHECK(status.reason.find("could not be loaded") != std::string::npos);
        CHECK(status.version.empty());
        dvv::Capabilities caps;
        addFfmpegCapabilities(status, caps);
        CHECK(caps.names().empty());
        CHECK_THROWS_AS(static_cast<void>(dvv::components::ffmpeg::api()), std::logic_error);
    }
}

TEST_SUITE("ffmpeg-relative") {
    TEST_CASE("a bad option does not take the once-per-process load") {
        const auto refused = loadWithDirectory(std::int64_t{1});
        REQUIRE(refused != nullptr);
        const dvv::ModuleStatus* refused_module = refused->module(FFMPEG_MODULE);
        CHECK(refused_module != nullptr);
        if (refused_module) {
            CHECK(refused_module->reason == OPTION_NOT_TEXT);
            CHECK(refused_module->properties.empty());
        }
        // The bad option returned before the load, so this load still reads DVV_FFMPEG_DIR.
        const auto loaded = dvv::ComponentRegistry::load();
        REQUIRE(loaded != nullptr);
        const dvv::ModuleStatus* module = loaded->module(FFMPEG_MODULE);
        CHECK(module != nullptr);
        if (module) {
            CHECK(module->provides.empty());
            CHECK(module->reason.find("must be absolute") != std::string::npos);
            CHECK(module->properties == std::vector{variableProperty()});
        }
    }

    TEST_CASE("load() refuses a relative DVV_FFMPEG_DIR") {
        const auto registry = dvv::ComponentRegistry::load();
        REQUIRE(registry != nullptr);
        const dvv::ModuleStatus* module = registry->module(FFMPEG_MODULE);
        CHECK(module != nullptr);
        if (module) {
            CHECK(module->provides.empty());
            CHECK(module->withheld == bothCapabilities());
            CHECK(module->reason == loadFfmpeg().reason);
            CHECK(module->reason.find("must be absolute") != std::string::npos);
            CHECK(module->properties == std::vector{variableProperty()});
        }
    }

    TEST_CASE("loader: a relative DVV_FFMPEG_DIR is refused") {
        const auto& status = loadFfmpeg();
        CHECK(status.reason == "DVV_FFMPEG_DIR must be absolute (got 'ffmpeg-relative')");
        CHECK_FALSE(status.core);
    }
}

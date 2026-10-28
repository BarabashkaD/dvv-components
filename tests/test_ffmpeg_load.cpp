// The FFmpeg loader against the prefix's FFmpeg and against directories without one (M4 design D3,
// §8.1). FFmpeg loads once per process, so each suite runs in its own CTest entry.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/log.h"
#include "dvv/components/ffmpeg.h"
#include "dvv/registry/capabilities.h"
#include "ffmpeg/api.h"
#include "ffmpeg_support.h"

namespace {

using dvv::components::addFfmpegCapabilities;
using dvv::components::FfmpegLoadOptions;
using dvv::components::loadFfmpeg;

bool anyLineContains(const std::vector<std::string>& lines, std::string_view text) {
    return std::ranges::any_of(
        lines, [&](const std::string& line) { return line.find(text) != std::string::npos; });
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
        dvv::Logger::instance().clearSinks();

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
        CHECK(caps.has("ffmpeg"));
        CHECK(caps.has("ffmpeg.lavfi"));
    }
}

TEST_SUITE("ffmpeg-absent") {
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
    TEST_CASE("loader: a relative DVV_FFMPEG_DIR is refused") {
        const auto& status = loadFfmpeg();
        CHECK(status.reason == "DVV_FFMPEG_DIR must be absolute (got 'ffmpeg-relative')");
        CHECK_FALSE(status.core);
    }
}

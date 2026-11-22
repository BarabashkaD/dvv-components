#pragma once

// Pure parts of the FFmpeg loader, unit-tested without FFmpeg.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dvv::components::detail {

// What the loader learned about one library; pure checks, unit-tested without FFmpeg.
struct LibraryInfo {
    std::string name;              // "avutil", "avcodec", ...
    unsigned runtimeVersion = 0;   // <lib>_version() (AV_VERSION_INT packing)
    unsigned compiledVersion = 0;  // LIB<NAME>_VERSION_INT of the headers
    std::string licence;           // <lib>_license()
    std::string configuration;     // <lib>_configuration()
};
// The first problem in `libraries` (in order), worded to follow "FFmpeg in <dir> (<source>) ", or
// nullopt when every library passes: major == compiled major, minor >= compiled minor, licence
// starts with "LGPL", configuration contains neither "--enable-gpl" nor "--enable-nonfree".
[[nodiscard]] std::optional<std::string> refusalReason(std::span<const LibraryInfo> libraries);

// The result of the process-wide load.
struct FfmpegStatus {
    bool core = false;    // FFMPEG_CAPABILITY: avutil, avcodec, avformat, swscale, swresample
    bool lavfi = false;   // FFMPEG_LAVFI_CAPABILITY: core and avfilter
    std::string reason;   // why not; empty when both are available (with core only: why not lavfi)
    std::string version;  // av_version_info(), e.g. "n8.1.3"
    std::string licence;  // avutil_license()
    std::filesystem::path directory;  // where the libraries were loaded from
};
// An explicit FFmpeg directory: absolute, final like DVV_FFMPEG_DIR (no fallthrough).
struct FfmpegLoadOptions {
    std::optional<std::filesystem::path> directory;
};

enum class SearchSource : std::uint8_t { Options, Environment, Default };

struct SearchPlan {
    std::vector<std::filesystem::path> directories;  // in order; exactly one for an explicit source
    SearchSource source = SearchSource::Default;
    std::string error;  // non-empty: FFmpeg stays off with this reason, nothing is searched
};

// options.directory, else `environment` (DVV_FFMPEG_DIR; nullopt or "" counts as unset), else
// exe_dir/ffmpeg and exe_dir/../lib/ffmpeg (macOS also exe_dir/../Frameworks/ffmpeg), lexically
// normal; none when exe_dir is empty. An
// explicit source must be absolute: "the FFmpeg directory must be absolute (got '<p>')",
// "DVV_FFMPEG_DIR must be absolute (got '<value>')".
[[nodiscard]] SearchPlan planSearch(const FfmpegLoadOptions& options,
                                    const std::optional<std::string>& environment,
                                    const std::filesystem::path& exe_dir);
// "the given directory", "DVV_FFMPEG_DIR", "default search".
[[nodiscard]] std::string_view sourceName(SearchSource source) noexcept;
// AV_VERSION_INT packing as "<major>.<minor>.<micro>", e.g. "62.3.100".
[[nodiscard]] std::string versionText(unsigned packed);

// The process-wide load: the first result and the source of its search, whatever later options
// say. Loads once per process and never unloads; installs the av_log callback; thread-safe
// (call_once).
struct LoadOutcome {
    const FfmpegStatus& status;
    SearchSource source;
};
[[nodiscard]] LoadOutcome loadOnce(const FfmpegLoadOptions& options);

}  // namespace dvv::components::detail

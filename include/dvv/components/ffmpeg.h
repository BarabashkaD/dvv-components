#pragma once

// FFmpeg status and loading (spec §2, §13.2; M4 design D3, §4.4): the only public FFmpeg header of
// dvv-components, with no FFmpeg type. FFmpeg is never linked: loadFfmpeg() opens the six libraries
// of an LGPL shared build at run time, checks them, resolves every function the components use and
// never unloads them.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include "dvv/registry/capabilities.h"

namespace dvv::components {

struct FfmpegStatus {
    bool core = false;    // capability "ffmpeg": avutil, avcodec, avformat, swscale, swresample
    bool lavfi = false;   // capability "ffmpeg.lavfi": core and avfilter
    std::string reason;   // why not; empty when both are available (with core only: why not lavfi)
    std::string version;  // av_version_info(), e.g. "n8.1.3"
    std::string licence;  // avutil_license()
    std::filesystem::path directory;                 // where the libraries were loaded from
    std::array<std::uint32_t, 6> libraryVersions{};  // <lib>_version() of avutil, avcodec,
                                                     // avformat, swscale, swresample, avfilter
};

// An explicit FFmpeg directory: absolute, final like DVV_FFMPEG_DIR (no fallthrough). The v1.0
// executables pass {} (a command-line option is v1.x).
struct FfmpegLoadOptions {
    std::optional<std::filesystem::path> directory;
};

// Loads once per process (later calls return the first result, whatever their options;
// thread-safe) and never unloads. Search: options.directory, else DVV_FFMPEG_DIR (an empty value
// counts as unset), else <exe dir>/ffmpeg, then <exe dir>/../lib/ffmpeg. An explicit directory is
// final: a failure there leaves FFmpeg off with the reason. Every library passes the checks of
// design D3 (refusalReason) and every function of the table resolves. Installs the av_log callback.
// Adds nothing to a registry.
[[nodiscard]] const FfmpegStatus& loadFfmpeg(const FfmpegLoadOptions& options = {});
// Adds "ffmpeg" when status.core and "ffmpeg.lavfi" when status.lavfi.
void addFfmpegCapabilities(const FfmpegStatus& status, Capabilities& caps);

// Ends the current and queued work of every FFmpeg and lavfi source (design v2 D5, §5.4: exit is
// bounded): from now on their workers complete each request with Internal "source closed". Sources
// created afterwards work normally. The applications call it before the Session or CLI shutdown,
// because the store's queued Produce steps keep their sources alive (M2 R11) and the Session waits
// for in-flight acquisitions. Thread-safe; increments a process-wide generation.
void stopFfmpegSources() noexcept;

namespace detail {
// What the loader learned about one library; pure checks, unit-tested without FFmpeg.
struct LibraryFacts {
    std::string name;              // "avutil", "avcodec", ...
    unsigned runtimeVersion = 0;   // <lib>_version() (AV_VERSION_INT packing)
    unsigned compiledVersion = 0;  // LIB<NAME>_VERSION_INT of the headers
    std::string licence;           // <lib>_license()
    std::string configuration;     // <lib>_configuration()
};
// The first problem in `libraries` (in order), worded to follow "FFmpeg in <dir> (<source>) ", or
// nullopt when every library passes: major == compiled major, minor >= compiled minor, licence
// starts with "LGPL", configuration contains neither "--enable-gpl" nor "--enable-nonfree".
[[nodiscard]] std::optional<std::string> refusalReason(std::span<const LibraryFacts> libraries);
}  // namespace detail

}  // namespace dvv::components

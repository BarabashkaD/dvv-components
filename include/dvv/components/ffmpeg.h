#pragma once

// FFmpeg in dvv-components: the module constants, and the status and loading functions of 0.2.x
// (deprecated). FFmpeg is never linked: the module opens the six libraries of an LGPL shared build
// at run time, checks them, resolves every function the components use and never unloads them.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "dvv/registry/capabilities.h"

namespace dvv::components {

// The capabilities of the FFmpeg module. Each name is defined here once; a component that needs
// FFmpeg probes for these constants. The values are a contract: they never change.
inline constexpr std::string_view FFMPEG_CAPABILITY = "ffmpeg";              // avutil, avcodec,
                                                                             // avformat, swscale,
                                                                             // swresample
inline constexpr std::string_view FFMPEG_LAVFI_CAPABILITY = "ffmpeg.lavfi";  // also avfilter

// The FFmpeg module: ComponentRegistry::load() loads FFmpeg (once per process; it is never
// unloaded) and adds FFMPEG_CAPABILITY and FFMPEG_LAVFI_CAPABILITY as far as they are available.
// registry.module(FFMPEG_MODULE) is its status: the reason when a capability is withheld, and the
// properties MODULE_VERSION_PROPERTY, MODULE_LICENCE_PROPERTY, MODULE_DIRECTORY_PROPERTY and,
// when DVV_FFMPEG_DIR chose the folder, MODULE_VARIABLE_PROPERTY. The reason and the directory
// property may hold a path: the reason shows folders as dvv::pathDisplayText, the directory
// property is dvv::pathText (dvv::pathFromText gives the folder back).
// The module id and the capability name are both "ffmpeg" by naming only; they are separate
// namespaces (module() versus capabilities()).
inline constexpr std::string_view FFMPEG_MODULE = "ffmpeg";
// The module's one option: an explicit FFmpeg folder as text (a std::string), absolute and final
// like DVV_FFMPEG_DIR. Without it the module searches as loadFfmpeg() does.
// FFmpeg loads once per process: a later load() with another folder gets the first result.
inline constexpr std::string_view FFMPEG_DIRECTORY_OPTION = "directory";

// Deprecated (0.3.0): use ComponentRegistry::load() and registry.module(FFMPEG_MODULE); removed in
// 1.0.0.
struct FfmpegStatus {
    bool core = false;    // FFMPEG_CAPABILITY: avutil, avcodec, avformat, swscale, swresample
    bool lavfi = false;   // FFMPEG_LAVFI_CAPABILITY: core and avfilter
    std::string reason;   // why not; empty when both are available (with core only: why not lavfi)
    std::string version;  // av_version_info(), e.g. "n8.1.3"
    std::string licence;  // avutil_license()
    std::filesystem::path directory;                 // where the libraries were loaded from
    std::array<std::uint32_t, 6> libraryVersions{};  // <lib>_version() of avutil, avcodec,
                                                     // avformat, swscale, swresample, avfilter
};

// Deprecated (0.3.0): use ComponentRegistry::load() and registry.module(FFMPEG_MODULE); removed in
// 1.0.0.
// An explicit FFmpeg directory: absolute, final like DVV_FFMPEG_DIR (no fallthrough). The v1.0
// executables pass {} (a command-line option is v1.x).
struct FfmpegLoadOptions {
    std::optional<std::filesystem::path> directory;
};

// Deprecated (0.3.0): use ComponentRegistry::load() and registry.module(FFMPEG_MODULE); removed in
// 1.0.0.
// Loads once per process (later calls return the first result, whatever their options;
// thread-safe) and never unloads. Search: options.directory, else DVV_FFMPEG_DIR (an empty value
// counts as unset), else <exe dir>/ffmpeg, then <exe dir>/../lib/ffmpeg. An explicit directory is
// final: a failure there leaves FFmpeg off with the reason. Every library passes the checks of
// refusalReason and every function of the table resolves. Installs the av_log callback.
// Adds nothing to a registry.
[[nodiscard]] const FfmpegStatus& loadFfmpeg(const FfmpegLoadOptions& options = {});
// Deprecated (0.3.0): use ComponentRegistry::load() and registry.module(FFMPEG_MODULE); removed in
// 1.0.0.
// Adds FFMPEG_CAPABILITY when status.core and FFMPEG_LAVFI_CAPABILITY when status.lavfi.
void addFfmpegCapabilities(const FfmpegStatus& status, Capabilities& caps);

// Deprecated (0.3.0): detach the origin (dvv-media FrameStore::detach) or stop the open's token
// (SourceFactory::open(request, stop)); removed in 1.0.0.
// Ends the current and queued work of every FFmpeg and lavfi source: from now on their workers
// complete each request with Internal "source closed". Sources created afterwards work normally.
// Thread-safe; increments a process-wide generation.
void stopFfmpegSources() noexcept;

namespace detail {
// What the loader learned about one library; pure checks, unit-tested without FFmpeg.
// Deprecated name (0.3.0): use LibraryInfo; at 1.0.0 the struct is LibraryInfo and this name goes.
struct LibraryFacts {
    std::string name;              // "avutil", "avcodec", ...
    unsigned runtimeVersion = 0;   // <lib>_version() (AV_VERSION_INT packing)
    unsigned compiledVersion = 0;  // LIB<NAME>_VERSION_INT of the headers
    std::string licence;           // <lib>_license()
    std::string configuration;     // <lib>_configuration()
};
using LibraryInfo = LibraryFacts;  // the name to use
// The first problem in `libraries` (in order), worded to follow "FFmpeg in <dir> (<source>) ", or
// nullopt when every library passes: major == compiled major, minor >= compiled minor, licence
// starts with "LGPL", configuration contains neither "--enable-gpl" nor "--enable-nonfree".
[[nodiscard]] std::optional<std::string> refusalReason(std::span<const LibraryInfo> libraries);
}  // namespace detail

}  // namespace dvv::components

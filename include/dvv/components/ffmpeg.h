#pragma once

// FFmpeg in dvv-components: the module constants. FFmpeg is never linked: the module opens the six
// libraries of an LGPL shared build at run time, checks them, resolves every function the
// components use and never unloads them.

#include <string_view>

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
// properties MODULE_VERSION_PROPERTY, MODULE_LICENCE_PROPERTY and MODULE_DIRECTORY_PROPERTY
// (present only when the core capability is provided) and MODULE_VARIABLE_PROPERTY (present only
// when DVV_FFMPEG_DIR chose the folder). The reason and the directory property may hold a path: the
// reason shows folders as dvv::pathDisplayText, the directory property is dvv::pathText
// (dvv::pathFromText gives the folder back). The module id and the capability name are both
// "ffmpeg" by naming only; they are separate namespaces (module() versus capabilities()).
inline constexpr std::string_view FFMPEG_MODULE = "ffmpeg";
// The module's one option: an explicit FFmpeg folder as text, absolute and final
// like DVV_FFMPEG_DIR. Without it ComponentRegistry::load() searches DVV_FFMPEG_DIR, then the
// folders next to the executable.
// FFmpeg loads once per process: a later load() with another folder gets the first result.
inline constexpr std::string_view FFMPEG_DIRECTORY_OPTION = "directory";

}  // namespace dvv::components

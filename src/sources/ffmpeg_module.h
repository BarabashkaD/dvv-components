#pragma once

// The FFmpeg module: ComponentRegistry::load() loads FFmpeg through it once per process and adds
// the FFmpeg capabilities FFmpeg provides. Private to dvv-components; the pure parts are
// unit-tested without FFmpeg.

#include <expected>
#include <string>

#include "dvv/components/ffmpeg.h"
#include "dvv/registry/module.h"
#include "dvv/registry/parameters.h"
#include "ffmpeg/loader_detail.h"

namespace dvv::components {

struct FfmpegModule {
    [[nodiscard]] static ModuleInfo info() noexcept;
    [[nodiscard]] static ModuleLoad load(const ParameterValues& options);
};

namespace detail {
// The module's options as load options; the reason text when they are not valid.
[[nodiscard]] std::expected<FfmpegLoadOptions, std::string> moduleOptions(
    const ParameterValues& options);
// What the module reports for a load result (pure; unit-tested without FFmpeg).
[[nodiscard]] ModuleLoad moduleLoadOf(const FfmpegStatus& status, SearchSource source);
}  // namespace detail

}  // namespace dvv::components

#include <cstddef>
#include <cstdio>

#include "dvv/components/ffmpeg.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_info.h"
#include "dvv/registry/component_registry.h"

int main() {
    auto registry = dvv::ComponentRegistry::create(dvv::Capabilities{});
    const std::size_t sources = registry->list(dvv::Kind::Source).size();
    registry.reset();
    // The installed dvv::ffmpeg links (no FFmpeg is loaded).
    const dvv::components::FfmpegStatus status;
    dvv::Capabilities caps;
    dvv::components::addFfmpegCapabilities(status, caps);
    if (!caps.names().empty()) {
        std::fprintf(stderr, "addFfmpegCapabilities added a capability without FFmpeg\n");
        return 1;
    }
    std::printf("dvv_components package works (%zu source components)\n", sources);
    return 0;
}

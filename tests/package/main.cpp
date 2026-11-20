#include <cstddef>
#include <cstdio>

#include "dvv/components/ffmpeg.h"
#include "dvv/media/source_factory.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_info.h"
#include "dvv/registry/component_registry.h"

int main() {
    auto registry = dvv::ComponentRegistry::create(dvv::Capabilities{});
    const std::size_t sources = registry->list<dvv::media::ISourceProvider>().size();
    registry.reset();
    // The installed FFmpeg module is linked: load() lists it, whether FFmpeg loads or not.
    const auto loaded = dvv::ComponentRegistry::load();
    if (loaded->module(dvv::components::FFMPEG_MODULE) == nullptr) {
        std::fprintf(stderr, "ComponentRegistry::load() does not list the FFmpeg module\n");
        return 1;
    }
    std::printf("dvv_components package works (%zu source components)\n", sources);
    return 0;
}

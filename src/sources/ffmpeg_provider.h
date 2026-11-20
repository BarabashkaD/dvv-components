#pragma once

// The `ffmpeg` source provider (design v2 §4.4; v1.0 codec scope): files of any container FFmpeg
// demuxes, lossless video codecs only, decoded on one worker thread per source. Private to
// dvv-components (also used by the compile-fail test).

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string_view>

#include "dvv/base/error.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_info.h"
#include "dvv/registry/component_registry.h"

namespace dvv::components {

class FfmpegSourceProvider final : public media::ISourceProvider,
                                   public Managed<FfmpegSourceProvider> {
public:
    explicit FfmpegSourceProvider(CreationKey<ComponentRegistry> key) noexcept;

    static ComponentInfo info() noexcept {
        return {.id = "ffmpeg", .name = "FFmpeg container files", .version = 1};
    }
    static std::expected<void, Unavailable> probe(const Capabilities& caps) {
        return require(caps, {"ffmpeg"});
    }

    [[nodiscard]] std::span<const std::string_view> schemes() const noexcept override;  // file
    // No I/O: av_probe_input_format3 on a padded copy of the head (design v2 §4.4 match rule).
    [[nodiscard]] media::Match match(const media::SourceRequest& request,
                                     std::span<const std::byte> head) const override;
    [[nodiscard]] Task<Result<std::unique_ptr<media::ISource>>> open(
        media::OpenTicket ticket, const media::SourceRequest& request,
        const media::FormatGuess& guess, const media::OpenContext& context) override;
};

}  // namespace dvv::components

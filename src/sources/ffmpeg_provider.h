#pragma once

// The `ffmpeg` source provider (design v2 §4.4; v1.0 codec scope): files of any container FFmpeg
// demuxes, lossless video codecs only, decoded on one worker thread per source. Private to
// dvv-components (also used by the compile-fail test).

#include <cstddef>
#include <cstdint>
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
#include "dvv/registry/parameters.h"
#include "sources/av_worker.h"

namespace dvv::components {

class FfmpegSourceProvider final : public media::ISourceProvider,
                                   public Managed<FfmpegSourceProvider> {
public:
    FfmpegSourceProvider(CreationKey<ComponentRegistry> key,
                         const ParameterValues& values) noexcept;

    static ComponentInfo info() noexcept {
        return {
            .id = "ffmpeg", .name = "FFmpeg container files", .kind = Kind::Source, .version = 1};
    }
    static const ParameterSchema& schema();  // empty
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

// One opened container file; produce() checks the index, then forwards to the worker.
class FfmpegSource final : public media::ISource, public Managed<FfmpegSource> {
public:
    FfmpegSource(CreationKey<media::SourceFactory> key, media::SourceInfo info,
                 std::unique_ptr<detail::AvWorker> worker) noexcept;

    [[nodiscard]] const media::SourceInfo& info() const noexcept override { return info_; }
    [[nodiscard]] Task<Result<media::FrameRef>> produce(std::int64_t index,
                                                        media::ProduceContext& ctx) override;

private:
    media::SourceInfo info_;
    std::unique_ptr<detail::AvWorker> worker_;
};

}  // namespace dvv::components

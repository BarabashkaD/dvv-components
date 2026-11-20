// The `lavfi` source provider: lavfi:<graph> URIs
// build an in-process libavfilter graph (FFmpeg loaded at run time) whose single video output
// ends in a buffersink; frames are pulled in order on one worker thread per source.

#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/components/ffmpeg.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_info.h"
#include "dvv/registry/component_registry.h"
#include "sources/av_source.h"
#include "sources/graph_supplier.h"

namespace dvv::components {

class LavfiSourceProvider final : public media::ISourceProvider,
                                  public Managed<LavfiSourceProvider> {
public:
    explicit LavfiSourceProvider(CreationKey<ComponentRegistry> key) noexcept
        : Managed(std::move(key)) {}

    static ComponentInfo info() noexcept {
        return {.id = "lavfi", .name = "FFmpeg filter graphs (lavfi)", .version = 1};
    }
    static std::expected<void, Unavailable> probe(const Capabilities& caps) {
        return require(caps, {FFMPEG_LAVFI_CAPABILITY});
    }

    [[nodiscard]] std::span<const std::string_view> schemes() const noexcept override {
        static constexpr std::array<std::string_view, 1> SCHEMES{"lavfi"};
        return SCHEMES;
    }
    // No validation: open reports the graph's errors. The guess stays empty.
    [[nodiscard]] media::Match match(const media::SourceRequest& request,
                                     std::span<const std::byte> /*head*/) const override {
        if (request.uri.scheme != "lavfi") {
            return media::Match{.confidence = 0, .guess = {}, .why = "not a lavfi graph"};
        }
        return media::Match{.confidence = 100, .guess = {}, .why = "lavfi filter graph"};
    }
    [[nodiscard]] Task<Result<std::unique_ptr<media::ISource>>> open(
        media::OpenTicket ticket, const media::SourceRequest& request,
        const media::FormatGuess& guess, const media::OpenContext& context) override;
};

Task<Result<std::unique_ptr<media::ISource>>> LavfiSourceProvider::open(
    media::OpenTicket ticket, const media::SourceRequest& request,
    const media::FormatGuess& /*guess*/, const media::OpenContext& context) {
    co_return co_await detail::openAvSource(std::move(ticket), request.uri, context.io,
                                            context.stop, &detail::openGraph);
}

DVV_REGISTER(media::ISourceProvider, LavfiSourceProvider);

}  // namespace dvv::components

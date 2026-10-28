// The `lavfi` source provider (design v2 D6, §4.4 lavfi; v1.0 codec scope): lavfi:<graph> URIs
// build an in-process libavfilter graph (FFmpeg loaded at run time, D3) whose single video output
// ends in a buffersink; frames are pulled in order on one worker thread per source.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

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
#include "sources/graph_supplier.h"

namespace dvv::components {

class LavfiSourceProvider final : public media::ISourceProvider,
                                  public Managed<LavfiSourceProvider> {
public:
    LavfiSourceProvider(CreationKey<ComponentRegistry> key,
                        const ParameterValues& /*values*/) noexcept
        : Managed(std::move(key)) {}

    static ComponentInfo info() noexcept {
        return {.id = "lavfi",
                .name = "FFmpeg filter graphs (lavfi)",
                .kind = Kind::Source,
                .version = 1};
    }
    static const ParameterSchema& schema() {
        static const ParameterSchema SCHEMA;
        return SCHEMA;
    }
    static std::expected<void, Unavailable> probe(const Capabilities& caps) {
        return require(caps, {"ffmpeg.lavfi"});
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

// One lavfi graph; produce() checks the index, then forwards to the worker.
class LavfiSource final : public media::ISource, public Managed<LavfiSource> {
public:
    LavfiSource(CreationKey<media::SourceFactory> key, media::SourceInfo info,
                std::unique_ptr<detail::AvWorker> worker) noexcept
        : Managed(std::move(key)), info_(std::move(info)), worker_(std::move(worker)) {}

    [[nodiscard]] const media::SourceInfo& info() const noexcept override { return info_; }
    [[nodiscard]] Task<Result<media::FrameRef>> produce(std::int64_t index,
                                                        media::ProduceContext& ctx) override {
        if (index < 0) {
            co_return makeError(Errc::Internal,
                                std::format("frame {} is outside [0, unbounded)", index));
        }
        // Concurrent calls (the store's prefetch) queue on the worker (§5.4).
        co_return co_await worker_->produce(index, ctx);
    }

private:
    media::SourceInfo info_;
    std::unique_ptr<detail::AvWorker> worker_;
};

Task<Result<std::unique_ptr<media::ISource>>> LavfiSourceProvider::open(
    media::OpenTicket ticket, const media::SourceRequest& request,
    const media::FormatGuess& /*guess*/, const media::OpenContext& context) {
    co_await resumeOn(context.io);  // all FFmpeg work of open runs on the I/O pool (§5.1)
    detail::OpenedGraph opened = co_try(detail::openGraph(request.uri));
    std::unique_ptr<media::ISource> source = std::make_unique<LavfiSource>(
        media::SourceFactory::issueKey<LavfiSource>(std::move(ticket)), std::move(opened.info),
        std::move(opened.worker));
    co_return std::move(source);
}

DVV_REGISTER(media::ISourceProvider, LavfiSourceProvider);

}  // namespace dvv::components

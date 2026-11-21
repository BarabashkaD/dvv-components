// The source class and the open body that the `ffmpeg` and `lavfi` providers share.

#include "sources/av_source.h"

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/base/uri.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "sources/av_worker.h"

namespace dvv::components {

AvSource::AvSource(CreationKey<media::SourceFactory> key, media::SourceInfo info,
                   std::unique_ptr<detail::AvWorker> worker) noexcept
    : Managed(std::move(key)), info_(std::move(info)), worker_(std::move(worker)) {}

Task<Result<media::FrameRef>> AvSource::produce(std::int64_t index, media::ProduceContext& ctx) {
    co_try(detail::checkFrameIndex(index, info_.frameCount));
    // Concurrent calls (the store's prefetch) queue on the worker.
    co_return co_await worker_->produce(index, ctx);
}

namespace detail {

Task<Result<std::unique_ptr<media::ISource>>> openAvSource(
    media::OpenTicket ticket,
    Uri uri,  // NOLINT(performance-unnecessary-value-param): the task is lazy; it owns its
              // arguments
    IExecutor& io, std::stop_token stop, OpenSteps steps) {
    co_await resumeOn(io);  // all FFmpeg work of open runs on the I/O pool
    // A stop that came before the open starts pays for no FFmpeg call.
    if (stop.stop_requested()) {
        co_return makeError(Errc::Cancelled, "source request cancelled");
    }
    OpenedSource opened = co_try(steps(uri, std::move(stop)));
    std::unique_ptr<media::ISource> source =
        std::make_unique<AvSource>(media::SourceFactory::issueKey<AvSource>(std::move(ticket)),
                                   std::move(opened.info), std::move(opened.worker));
    co_return std::move(source);
}

Result<void> checkFrameIndex(std::int64_t index, std::optional<std::int64_t> frame_count) {
    if (!frame_count) {
        if (index < 0) {
            return makeError(Errc::Internal,
                             std::format("frame {} is outside [0, unbounded)", index));
        }
        return {};
    }
    if (index < 0 || index >= *frame_count) {
        return makeError(Errc::Internal,
                         std::format("frame {} is outside [0, {})", index, *frame_count));
    }
    return {};
}

}  // namespace detail

}  // namespace dvv::components

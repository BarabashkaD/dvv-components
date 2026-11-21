#pragma once

// The source class and the open body that the `ffmpeg` and `lavfi` providers share. Private to
// dvv-components (also used by the compile-fail test).

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>

#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/base/uri.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "sources/av_worker.h"

namespace dvv::components {
// One opened FFmpeg or lavfi source: produce() checks the index against info().frameCount
// (unbounded when it has none), then forwards to the worker.
class AvSource final : public media::ISource, public Managed<AvSource> {
public:
    AvSource(CreationKey<media::SourceFactory> key, media::SourceInfo info,
             std::unique_ptr<detail::AvWorker> worker) noexcept;
    [[nodiscard]] const media::SourceInfo& info() const noexcept override { return info_; }
    [[nodiscard]] Task<Result<media::FrameRef>> produce(std::int64_t index,
                                                        media::ProduceContext& ctx) override;

private:
    media::SourceInfo info_;
    std::unique_ptr<detail::AvWorker> worker_;
};
namespace detail {
// What an open returns: the source's info and its worker (which owns the supplier).
struct OpenedSource {
    media::SourceInfo info;
    std::unique_ptr<AvWorker> worker;
};
using OpenSteps = Result<OpenedSource> (*)(const Uri& uri, std::stop_token stop);
// The providers' open body: resumes on `io` (every FFmpeg call of an open runs on the I/O pool),
// ends with Cancelled when `stop` is already stopped (before any FFmpeg call), else runs `steps`
// with the open's token and wraps the result in an AvSource. Arguments by value (the task is
// lazy); `io` is the factory's executor, which outlives every open.
[[nodiscard]] Task<Result<std::unique_ptr<media::ISource>>> openAvSource(media::OpenTicket ticket,
                                                                         Uri uri, IExecutor& io,
                                                                         std::stop_token stop,
                                                                         OpenSteps steps);
// AvSource::produce's index check: Errc::Internal "frame <i> is outside [0, <count>)" when the
// source has a frame count, "frame <i> is outside [0, unbounded)" when it has none.
[[nodiscard]] Result<void> checkFrameIndex(std::int64_t index,
                                           std::optional<std::int64_t> frame_count);
}  // namespace detail
}  // namespace dvv::components

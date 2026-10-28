#pragma once

// One worker thread per FFmpeg or lavfi source (design v2 §5.1, §5.4, D5): the stop condition, the
// supplier interface, the frame check and the copy into a catalogue original.

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/base/task.h"
#include "dvv/media/frame.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"
#include "ffmpeg/api.h"

namespace dvv::components::detail {

// The stop condition of one worker (design v2 D5, §5.4): its own flag, set by ~AvWorker, or a
// process-wide stopFfmpegSources() since the worker was created (ffmpeg::stopGeneration() changed).
struct StopCheck {
    const std::atomic<bool>* flag = nullptr;
    std::uint64_t generation = 0;
    // flag->load(std::memory_order_acquire) || ffmpeg::stopGeneration() != generation
    [[nodiscard]] bool operator()() const noexcept;
};

// The decode or filter state of one source. Called only on the worker thread.
class IFrameSupplier {
public:
    IFrameSupplier() = default;
    IFrameSupplier(const IFrameSupplier&) = delete;
    IFrameSupplier& operator=(const IFrameSupplier&) = delete;
    IFrameSupplier(IFrameSupplier&&) = delete;
    IFrameSupplier& operator=(IFrameSupplier&&) = delete;
    virtual ~IFrameSupplier() = default;
    // The frame of `index`, owned by the supplier's ring and valid until the next call. Checks
    // `stopped()` before each packet read, decode and filter step (Errc::Internal "source closed").
    // Every frame it decodes or pulls passes `checkFrame` before it enters the ring.
    [[nodiscard]] virtual Result<const AVFrame*> frame(std::int64_t index,
                                                       const StopCheck& stopped) = 0;
};

// §5.4 frame check: width, height and format equal the descriptor's, else Errc::Format
// "frame <index> changed to <w>x<h> <pix_fmt name>".
[[nodiscard]] Result<void> checkFrame(const AVFrame& frame, std::int64_t index,
                                      const media::FrameDesc& desc, int expected_av_format);

// Copies the planes of `frame` row by row (linesize may be padded or negative) into a HostFrame of
// the tight catalogue geometry, allocated through ctx, and publishes it. Errc::Internal when the
// catalogue plane count differs from av_pix_fmt_count_planes.
[[nodiscard]] Result<media::FrameRef> copyAndPublish(const AVFrame& frame,
                                                     const media::FrameDesc& desc,
                                                     media::ProduceContext& ctx);

// Test seam (the stop tests): while a wrapper is set, every AvWorker created passes its supplier
// through it, so a test can pace the real decoding (e.g. one frame at a time with a pause) and stop
// it while it is in progress, whatever the machine's speed. Empty by default; an empty
// wrapper clears it. Thread-safe.
using SupplierWrapper =
    std::function<std::unique_ptr<IFrameSupplier>(std::unique_ptr<IFrameSupplier>)>;
void setSupplierWrapperForTest(SupplierWrapper wrapper);

class AvWorker {
public:
    // Starts the worker thread (ExecutorFactory::makeThreadPool(1)); throws std::runtime_error
    // when it cannot be created. `generation`: ffmpeg::stopGeneration() when the source's open
    // began, so a stopFfmpegSources() during the open stops the new source too.
    AvWorker(std::unique_ptr<IFrameSupplier> supplier, media::FrameDesc desc,
             std::uint64_t generation);
    // Sets `stopping`, then destroys the pool: every queued request runs, sees the flag and
    // completes with Internal "source closed"; then the supplier (and its AV objects) dies. Must
    // not run on the worker thread (produce and fetch never return there). No produce or fetch may
    // start once destruction begins: post_mutex_ only makes queued() a safe signal that every
    // counted request is already posted.
    ~AvWorker();
    AvWorker(const AvWorker&) = delete;
    AvWorker& operator=(const AvWorker&) = delete;
    AvWorker(AvWorker&&) = delete;
    AvWorker& operator=(AvWorker&&) = delete;

    // ISource::produce body: hop to the worker, supplier->frame(index), copyAndPublish, then hop
    // to ctx.io() before returning (never return on the worker thread: the caller may drop the
    // last reference to the source there, and the destructor would join its own thread).
    [[nodiscard]] Task<Result<media::FrameRef>> produce(std::int64_t index,
                                                        media::ProduceContext& ctx);
    // Test seam (stop test): the same path without a ProduceContext; resumes on `resume_on`.
    [[nodiscard]] Task<Result<void>> fetch(std::int64_t index, IExecutor& resume_on);
    // Requests that entered produce/fetch and have not left the worker yet (tests).
    [[nodiscard]] std::size_t queued() const noexcept;

private:
    // co_await toWorker(): counts the request and posts its resumption to the worker, both under
    // post_mutex_, so ~AvWorker (which sets the flag under the same mutex) sees every request that
    // was counted already queued on the pool.
    struct ToWorker {
        AvWorker* worker;
        [[nodiscard]] bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle) const;
        void await_resume() const noexcept {}
    };
    [[nodiscard]] ToWorker toWorker() noexcept { return ToWorker{this}; }
    // On the worker: Internal "source closed" when stopped, else the supplier's frame.
    [[nodiscard]] Result<const AVFrame*> frameOnWorker(std::int64_t index);

    std::mutex post_mutex_;              // orders posting against the stop flag
    std::atomic<bool> stopping_{false};  // design v2 D5/§5.4: the stop flag
    StopCheck stop_;                     // the flag or stopFfmpegSources()
    std::atomic<std::size_t> queued_{0};
    media::FrameDesc desc_;
    std::unique_ptr<IFrameSupplier> supplier_;  // declared before pool_: destroyed after it
    std::shared_ptr<IExecutor> pool_;           // ExecutorFactory::makeThreadPool(1)
};

}  // namespace dvv::components::detail

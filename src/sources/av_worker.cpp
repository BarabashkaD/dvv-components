// One worker thread per FFmpeg or lavfi source (design v2 §5.1, §5.4, D5).

#include "sources/av_worker.h"

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/base/executor.h"
#include "dvv/base/executors.h"
#include "dvv/base/task.h"
#include "dvv/media/frame.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"
#include "ffmpeg/api.h"

namespace dvv::components::detail {
namespace {

constexpr std::size_t HOST_ALIGNMENT = 64;

std::shared_ptr<IExecutor> makeWorkerPool() {
    auto pool = ExecutorFactory::makeThreadPool(1);
    if (!pool) {
        throw std::runtime_error(pool.error().describe());
    }
    return std::move(*pool);
}

// An exception on the worker must not complete the request there (the caller would continue on
// the worker thread): it becomes an error.
Error errorFromException() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return Error{
            .code = Errc::OutOfMemory, .message = "out of memory", .where = {}, .cause = nullptr};
    } catch (const std::exception& error) {
        return Error{.code = Errc::Internal,
                     .message = std::string("source worker failed: ") + error.what(),
                     .where = {},
                     .cause = nullptr};
    } catch (...) {
        return Error{.code = Errc::Internal,
                     .message = "source worker failed",
                     .where = {},
                     .cause = nullptr};
    }
}

// The test seam's wrapper (setSupplierWrapperForTest) and its lock.
std::mutex& wrapperMutex() {
    static std::mutex mutex;
    return mutex;
}
SupplierWrapper& wrapperSlot() {
    static SupplierWrapper wrapper;
    return wrapper;
}

std::unique_ptr<IFrameSupplier> wrapForTest(std::unique_ptr<IFrameSupplier> supplier) {
    SupplierWrapper wrapper;
    {
        const std::lock_guard lock(wrapperMutex());
        wrapper = wrapperSlot();
    }
    return wrapper ? wrapper(std::move(supplier)) : std::move(supplier);
}

}  // namespace

void setSupplierWrapperForTest(SupplierWrapper wrapper) {
    const std::lock_guard lock(wrapperMutex());
    wrapperSlot() = std::move(wrapper);
}

bool StopCheck::operator()() const noexcept {
    return (flag != nullptr && flag->load(std::memory_order_acquire)) ||
           ffmpeg::stopGeneration() != generation;
}

Result<void> checkFrame(const AVFrame& frame, std::int64_t index, const media::FrameDesc& desc,
                        int expected_av_format) {
    if (std::cmp_equal(frame.width, desc.width) && std::cmp_equal(frame.height, desc.height) &&
        frame.format == expected_av_format) {
        return {};
    }
    const char* name = ffmpeg::api().av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame.format));
    return makeError(Errc::Format, std::format("frame {} changed to {}x{} {}", index, frame.width,
                                               frame.height, name != nullptr ? name : "unknown"));
}

Result<media::FrameRef> copyAndPublish(const AVFrame& frame, const media::FrameDesc& desc,
                                       media::ProduceContext& ctx) {
    DVV_TRY(const media::FrameGeometry layout,
            media::geometry(desc.format, desc.width, desc.height));
    const int av_planes =
        ffmpeg::api().av_pix_fmt_count_planes(static_cast<AVPixelFormat>(frame.format));
    if (av_planes != layout.planeCount) {
        return makeError(Errc::Internal, std::format("{} has {} planes, the decoder's frame {}",
                                                     media::descriptor(desc.format).name,
                                                     layout.planeCount, av_planes));
    }
    DVV_TRY(const std::shared_ptr<media::HostFrame> host,
            ctx.allocateHost(desc, layout.bytes, HOST_ALIGNMENT, 0));
    const std::span<std::byte> out = host->data();
    for (std::size_t p = 0; p < layout.planeCount; ++p) {
        const media::PlaneGeometry& plane = layout.planes[p];
        const auto* source = reinterpret_cast<const std::byte*>(frame.data[p]);
        const std::ptrdiff_t source_stride = frame.linesize[p];
        for (std::uint32_t row = 0; row < plane.rows; ++row) {
            std::memcpy(
                out.data() + plane.offset + (static_cast<std::uint64_t>(row) * plane.stride),
                source + (static_cast<std::ptrdiff_t>(row) * source_stride), plane.rowBytes);
        }
    }
    return ctx.publish(host);
}

AvWorker::AvWorker(std::unique_ptr<IFrameSupplier> supplier, media::FrameDesc desc,
                   std::uint64_t generation)
    : stop_{.flag = &stopping_, .generation = generation},
      desc_(desc),
      supplier_(wrapForTest(std::move(supplier))),
      pool_(makeWorkerPool()) {}  // the source's own worker (D5)

AvWorker::~AvWorker() {
    {
        // §5.4: before joining the worker; every request counted so far is already posted.
        const std::lock_guard lock(post_mutex_);
        stopping_.store(true, std::memory_order_release);
    }
    pool_.reset();      // runs every queued resumption (each sees stopping_), then joins
    supplier_.reset();  // the AV objects die after the worker thread is gone
}

void AvWorker::ToWorker::await_suspend(std::coroutine_handle<> handle) const {
    const std::lock_guard lock(worker->post_mutex_);
    worker->queued_.fetch_add(1, std::memory_order_relaxed);
    try {
        worker->pool_->post([handle] { handle.resume(); });
    } catch (...) {
        worker->queued_.fetch_sub(1, std::memory_order_relaxed);
        throw;
    }
}

Result<const AVFrame*> AvWorker::frameOnWorker(std::int64_t index) {
    if (stop_()) {
        return makeError(Errc::Internal, "source closed");
    }
    return supplier_->frame(index, stop_);
}

Task<Result<media::FrameRef>> AvWorker::produce(std::int64_t index, media::ProduceContext& ctx) {
    co_await toWorker();
    Result<media::FrameRef> result = [&]() -> Result<media::FrameRef> {
        try {
            auto frame = frameOnWorker(index);
            if (!frame) {
                return std::unexpected(std::move(frame).error());
            }
            return copyAndPublish(**frame, desc_, ctx);
        } catch (...) {
            return std::unexpected(errorFromException());
        }
    }();
    queued_.fetch_sub(1, std::memory_order_relaxed);
    // Leave the worker before returning: the store may release the last reference to the source
    // in our continuation, and ~AvWorker on the worker thread would join itself.
    co_await resumeOn(ctx.io());
    co_return result;
}

Task<Result<void>> AvWorker::fetch(std::int64_t index, IExecutor& resume_on) {
    co_await toWorker();
    Result<void> result = [&]() -> Result<void> {
        try {
            auto frame = frameOnWorker(index);
            if (!frame) {
                return std::unexpected(std::move(frame).error());
            }
            return {};
        } catch (...) {
            return std::unexpected(errorFromException());
        }
    }();
    queued_.fetch_sub(1, std::memory_order_relaxed);
    co_await resumeOn(resume_on);
    co_return result;
}

std::size_t AvWorker::queued() const noexcept {
    return queued_.load(std::memory_order_relaxed);
}

}  // namespace dvv::components::detail

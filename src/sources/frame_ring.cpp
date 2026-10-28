#include "sources/frame_ring.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>

#include "ffmpeg/api.h"

namespace dvv::components::detail {

FrameRing::FrameRing(std::size_t capacity) : slots_(std::max<std::size_t>(capacity, 1)) {
    for (Slot& slot : slots_) {
        slot.frame.reset(ffmpeg::api().av_frame_alloc());
        if (!slot.frame) {
            throw std::bad_alloc();
        }
    }
}

const AVFrame* FrameRing::find(std::int64_t index) const noexcept {
    for (const Slot& slot : slots_) {
        if (slot.index == index) {
            return slot.frame.get();
        }
    }
    return nullptr;
}

void FrameRing::put(std::int64_t index, AVFrame& frame) {
    Slot* target = nullptr;
    for (Slot& slot : slots_) {
        if (slot.index == index) {
            target = &slot;
            break;
        }
        if (!slot.index && target == nullptr) {
            target = &slot;
        }
    }
    if (target == nullptr) {
        // Full: the lowest index goes.
        target = &*std::ranges::min_element(
            slots_, {}, [](const Slot& slot) { return slot.index.value_or(0); });
    }
    const ffmpeg::Api& api = ffmpeg::api();
    api.av_frame_unref(target->frame.get());
    api.av_frame_move_ref(target->frame.get(), &frame);
    target->index = index;
}

void FrameRing::clear() noexcept {
    for (Slot& slot : slots_) {
        if (slot.index) {
            ffmpeg::api().av_frame_unref(slot.frame.get());
            slot.index.reset();
        }
    }
}

std::optional<std::int64_t> FrameRing::oldest() const noexcept {
    std::optional<std::int64_t> result;
    for (const Slot& slot : slots_) {
        if (slot.index && (!result || *slot.index < *result)) {
            result = slot.index;
        }
    }
    return result;
}

std::optional<std::int64_t> FrameRing::newest() const noexcept {
    std::optional<std::int64_t> result;
    for (const Slot& slot : slots_) {
        if (slot.index && (!result || *slot.index > *result)) {
            result = slot.index;
        }
    }
    return result;
}

}  // namespace dvv::components::detail

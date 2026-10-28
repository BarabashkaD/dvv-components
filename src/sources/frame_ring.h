#pragma once

// The last decoded frames of one FFmpeg or lavfi source (design v2 D5); used only on its worker.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ffmpeg/api.h"

namespace dvv::components::detail {

class FrameRing {
public:
    // capacity >= 1 (0 counts as 1). Allocates its AVFrames (FFmpeg must be loaded); throws
    // std::bad_alloc when FFmpeg cannot allocate one.
    explicit FrameRing(std::size_t capacity);

    // The frame of `index`, or nullptr when absent.
    [[nodiscard]] const AVFrame* find(std::int64_t index) const noexcept;
    // Takes the frame's references (av_frame_move_ref into a ring-owned AVFrame; `frame` is left
    // blank). An entry of the same index is replaced; when full, the entry with the lowest index is
    // dropped first.
    void put(std::int64_t index, AVFrame& frame);
    void clear() noexcept;
    [[nodiscard]] std::optional<std::int64_t> oldest() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> newest() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

private:
    struct Slot {
        std::optional<std::int64_t> index;  // nullopt: free
        ffmpeg::FramePtr frame;
    };
    std::vector<Slot> slots_;
};

}  // namespace dvv::components::detail

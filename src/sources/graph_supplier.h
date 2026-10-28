#pragma once

// The `lavfi` provider's filter graph (design v2 D6, §4.4 lavfi, §5.4): build, pull, restart and
// end of one libavfilter graph whose single video output ends in a buffersink.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "dvv/base/error.h"
#include "dvv/base/uri.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"
#include "ffmpeg/api.h"
#include "sources/av_worker.h"
#include "sources/frame_ring.h"

namespace dvv::components::detail {

// One lavfi graph (design v2 D6, §4.4 lavfi). Built on open and on every restart; used only on the
// source's worker after open. Frames are numbered 0, 1, 2… in pull order since the last build.
class GraphSupplier final : public IFrameSupplier {
public:
    // Builds the graph. Errors: Errc::Unsupported for a filter not in LAVFI_ALLOWED_FILTERS
    // (checked on the parsed names, before any filter is created); Errc::Format "the lavfi graph is
    // invalid: <text> (AVERROR <n>)" for a parse or configure error; Errc::Format "the lavfi graph
    // has an unconnected input" / "the lavfi graph needs exactly one output"; Errc::Unsupported
    // "lavfi: filter <name> with <n> inputs is not supported (a graph is one chain)";
    // Errc::OutOfMemory for ENOMEM; any other error of the sink's creation or link is
    // ffmpegError's (all allowed filters are video, so the link does not fail on the media type).
    [[nodiscard]] static Result<std::unique_ptr<GraphSupplier>> create(std::string graph,
                                                                       std::size_t ring_capacity);
    ~GraphSupplier() override = default;
    GraphSupplier(const GraphSupplier&) = delete;
    GraphSupplier& operator=(const GraphSupplier&) = delete;
    GraphSupplier(GraphSupplier&&) = delete;
    GraphSupplier& operator=(GraphSupplier&&) = delete;

    // Pulls frame 0 (open only, before setExpected): its size, format and colour. The frame stays
    // in the ring as index 0. Errc::Format "the lavfi graph produced no frame" at an immediate end.
    [[nodiscard]] Result<const AVFrame*> first();
    [[nodiscard]] AVRational frameRate() const;  // av_buffersink_get_frame_rate
    // The descriptor and AVPixelFormat every later frame must match (checkFrame).
    void setExpected(const media::FrameDesc& desc, int av_format);
    // Replaces the ring by one of `capacity` frames, keeping frame 0 (open only, after first()).
    void resizeRing(std::size_t capacity);

    [[nodiscard]] Result<const AVFrame*> frame(std::int64_t index,
                                               const StopCheck& stopped) override;

private:
    GraphSupplier(std::string graph, std::size_t ring_capacity);
    // Builds (or rebuilds) the graph from text_: produced_ = 0, ended_ = false.
    [[nodiscard]] Result<void> build();
    // Pulls the next frame into the ring as produced_; checks `stopped` first.
    [[nodiscard]] Result<void> pull(const StopCheck& stopped);

    std::string text_;
    ffmpeg::FilterGraphPtr graph_;
    AVFilterContext* sink_ = nullptr;  // owned by graph_
    ffmpeg::FramePtr scratch_;         // the sink's frame before it enters the ring
    FrameRing ring_;
    std::int64_t produced_ = 0;  // the number of the next frame pulled
    bool ended_ = false;         // the sink reported AVERROR_EOF
    media::FrameDesc expected_{};
    std::optional<int> expected_format_;  // nullopt until setExpected (first() checks nothing)
};

// The result of open steps 1-5 for one `lavfi` URI: the source's info and its worker (which owns
// the supplier, with frame 0 in its ring).
struct OpenedGraph {
    media::SourceInfo info;
    std::unique_ptr<AvWorker> worker;
};
// LavfiSourceProvider::open's work (it runs on the I/O pool).
[[nodiscard]] Result<OpenedGraph> openGraph(const Uri& uri);

// Test seam (stop test): an AvWorker over GraphSupplier for `graph`, with the same open code.
[[nodiscard]] Result<std::unique_ptr<AvWorker>> makeGraphWorkerForTest(std::string graph);

}  // namespace dvv::components::detail

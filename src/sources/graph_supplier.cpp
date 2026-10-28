// The `lavfi` provider's filter graph (design v2 D6, §4.4 lavfi, §5.4; v1.0 codec scope).

#include "sources/graph_supplier.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/uri.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"
#include "ffmpeg/api.h"
#include "sources/av_worker.h"
#include "sources/ffmpeg_rules.h"
#include "sources/frame_ring.h"

namespace dvv::components::detail {
namespace {

constexpr std::size_t OPEN_RING = 8;  // open step 2; resized in step 4
constexpr media::Rational DEFAULT_FPS{25, 1};

using ffmpeg::api;

Error sourceClosed() {
    return Error{.code = Errc::Internal, .message = "source closed", .where = {}, .cause = nullptr};
}

// av_strerror's text, or "AVERROR <n>" when it has none.
std::string errorText(int averror) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    if (api().av_strerror(averror, text.data(), text.size()) == 0 && text.front() != '\0') {
        return text.data();
    }
    return std::format("AVERROR {}", averror);
}

// A parse or configure error of the graph: AVERROR(ENOMEM) -> OutOfMemory (ffmpegError of the
// call); any other -> Format "the lavfi graph is invalid: <text> (AVERROR <n>)" (plan choice: a
// typo in a user graph is an input error).
Error graphError(std::string_view call, int averror) {
    if (averror == AVERROR(ENOMEM)) {
        return ffmpeg::ffmpegError(call, averror);
    }
    return Error{.code = Errc::Format,
                 .message = std::format("the lavfi graph is invalid: {} (AVERROR {})",
                                        errorText(averror), averror),
                 .where = {},
                 .cause = nullptr};
}

Error graphEnded(std::int64_t produced) {
    return Error{.code = Errc::Io,
                 .message = std::format("the lavfi graph ended after {} frames", produced),
                 .where = {},
                 .cause = nullptr};
}

}  // namespace

GraphSupplier::GraphSupplier(std::string graph, std::size_t ring_capacity)
    : text_(std::move(graph)), scratch_(api().av_frame_alloc()), ring_(ring_capacity) {}

Result<std::unique_ptr<GraphSupplier>> GraphSupplier::create(std::string graph,
                                                             std::size_t ring_capacity) {
    std::unique_ptr<GraphSupplier> supplier(new GraphSupplier(std::move(graph), ring_capacity));
    if (!supplier->scratch_) {
        return makeError(Errc::OutOfMemory, "FFmpeg could not allocate a frame");
    }
    DVV_TRY_VOID(supplier->build());
    return supplier;
}

Result<void> GraphSupplier::build() {
    // The old graph (and the sink it owns) goes first; a failed build leaves none.
    sink_ = nullptr;
    graph_.reset(api().avfilter_graph_alloc());
    if (!graph_) {
        return std::unexpected(ffmpeg::ffmpegError("avfilter_graph_alloc", AVERROR(ENOMEM)));
    }
    AVFilterInOut* inputs = nullptr;
    AVFilterInOut* outputs = nullptr;
    const auto free_lists = [&] {
        api().avfilter_inout_free(&inputs);
        api().avfilter_inout_free(&outputs);
    };
    // Parse only (no filter created yet), check every filter name against LAVFI_ALLOWED_FILTERS
    // (the security review's allowlist: no filter that opens a file or URL;
    // movie/amovie would open their input in their init), then apply.
    AVFilterGraphSegment* segment = nullptr;
    int parsed = api().avfilter_graph_segment_parse(graph_.get(), text_.c_str(), 0, &segment);
    if (parsed < 0) {
        api().avfilter_graph_segment_free(&segment);
        return std::unexpected(graphError("avfilter_graph_segment_parse", parsed));
    }
    for (std::size_t c = 0; c < segment->nb_chains; ++c) {
        const AVFilterChain* chain = segment->chains[c];
        for (std::size_t f = 0; f < chain->nb_filters; ++f) {
            const char* name = chain->filters[f]->filter_name;
            if (auto allowed = checkLavfiFilter(name != nullptr ? name : ""); !allowed) {
                api().avfilter_graph_segment_free(&segment);
                return std::unexpected(std::move(allowed).error());
            }
        }
    }
    // The same result as avfilter_graph_parse_ptr with empty open lists (create, options, init,
    // link): the unconnected pads come back in inputs/outputs.
    parsed = api().avfilter_graph_segment_apply(segment, 0, &inputs, &outputs);
    api().avfilter_graph_segment_free(&segment);
    if (parsed < 0) {
        free_lists();
        return std::unexpected(graphError("avfilter_graph_segment_apply", parsed));
    }
    if (inputs != nullptr) {
        free_lists();
        return makeError(Errc::Format, "the lavfi graph has an unconnected input");
    }
    if (outputs == nullptr || outputs->next != nullptr) {
        free_lists();
        return makeError(Errc::Format, "the lavfi graph needs exactly one output");
    }
    // One chain only: a filter with a second input (scale's ref input) joins two
    // generators by time and can discard an unbounded number of frames per output frame. The
    // input count is known only now (it can depend on the options); no frame has run yet.
    for (unsigned i = 0; i < graph_->nb_filters; ++i) {
        const AVFilterContext* filter = graph_->filters[i];
        if (filter->nb_inputs > 1) {
            free_lists();
            return makeError(Errc::Unsupported,
                             std::format("lavfi: filter {} with {} inputs is not supported "
                                         "(a graph is one chain)",
                                         filter->filter->name, filter->nb_inputs));
        }
    }
    // Every allowed filter is video, so the link cannot fail on the media type.
    AVFilterContext* sink = nullptr;
    int result = api().avfilter_graph_create_filter(&sink, api().avfilter_get_by_name("buffersink"),
                                                    "out", nullptr, nullptr, graph_.get());
    const char* call = "avfilter_graph_create_filter";
    if (result >= 0) {
        result = api().avfilter_link(outputs->filter_ctx, static_cast<unsigned>(outputs->pad_idx),
                                     sink, 0);
        call = "avfilter_link";
    }
    free_lists();
    if (result < 0) {
        return std::unexpected(ffmpeg::ffmpegError(call, result));
    }
    result = api().avfilter_graph_config(graph_.get(), nullptr);
    if (result < 0) {
        return std::unexpected(graphError("avfilter_graph_config", result));
    }
    sink_ = sink;  // owned by graph_
    produced_ = 0;
    ended_ = false;
    return {};
}

Result<const AVFrame*> GraphSupplier::first() {
    const StopCheck never{.flag = nullptr, .generation = ffmpeg::stopGeneration()};
    if (auto pulled = pull(never); !pulled) {
        if (ended_) {
            return makeError(Errc::Format, "the lavfi graph produced no frame");
        }
        return std::unexpected(std::move(pulled).error());
    }
    return ring_.find(0);
}

AVRational GraphSupplier::frameRate() const {
    return api().av_buffersink_get_frame_rate(sink_);
}

void GraphSupplier::setExpected(const media::FrameDesc& desc, int av_format) {
    expected_ = desc;
    expected_format_ = av_format;
}

void GraphSupplier::resizeRing(std::size_t capacity) {
    if (capacity == ring_.capacity()) {
        return;
    }
    FrameRing next(capacity);
    if (const AVFrame* zero = ring_.find(0)) {
        // The ring hands out const frames; this one moves into the new ring and the old one dies.
        next.put(0, *const_cast<AVFrame*>(zero));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }
    ring_ = std::move(next);
}

Result<void> GraphSupplier::pull(const StopCheck& stopped) {
    if (stopped()) {
        return std::unexpected(sourceClosed());
    }
    if (ended_) {
        return std::unexpected(graphEnded(produced_));
    }
    AVFrame* const frame = scratch_.get();
    const int result = api().av_buffersink_get_frame(sink_, frame);
    if (result == AVERROR_EOF) {
        ended_ = true;
        return std::unexpected(graphEnded(produced_));
    }
    if (result < 0) {
        sink_ = nullptr;  // the numbering would shift: the next request rebuilds the graph
        return std::unexpected(ffmpeg::ffmpegError("av_buffersink_get_frame", result));
    }
    if (expected_format_) {
        if (auto checked = checkFrame(*frame, produced_, expected_, *expected_format_); !checked) {
            api().av_frame_unref(frame);
            sink_ = nullptr;  // as above
            return std::unexpected(std::move(checked).error());
        }
    }
    ring_.put(produced_, *frame);
    ++produced_;
    return {};
}

Result<const AVFrame*> GraphSupplier::frame(std::int64_t index, const StopCheck& stopped) {
    if (const AVFrame* held = ring_.find(index)) {
        return held;
    }
    if (ended_ && index >= produced_) {
        return std::unexpected(graphEnded(produced_));
    }
    // Behind the ring (every pulled frame not in it is older than its oldest), or no usable graph
    // after a failed build or pull: rebuild and pull from frame 0 again.
    if (sink_ == nullptr || index < produced_) {
        ring_.clear();
        DVV_TRY_VOID(build());
    }
    while (produced_ <= index) {
        DVV_TRY_VOID(pull(stopped));
    }
    return ring_.find(index);
}

// ---- open steps 1-5 ----

Result<OpenedGraph> openGraph(const Uri& uri) {
    // 1. The graph is the URI's body exactly as given (lavfi: is an opaque scheme, so parseUri
    // keeps it whole); a '?' would start a query, which lavfi: URIs do not take (D28).
    const std::string& graph = uri.body;
    if (graph.empty()) {
        return makeError(Errc::Format, "lavfi: URI without a graph");
    }
    if (graph.find('?') != std::string::npos || !uri.params.empty()) {
        return makeError(Errc::Format,
                         "lavfi: URIs take no query (the rest of the URI is the graph)");
    }
    // One stop generation for the open and the worker (as the ffmpeg provider).
    const std::uint64_t generation = ffmpeg::stopGeneration();

    // 2. Build and frame 0.
    DVV_TRY(std::unique_ptr<GraphSupplier> supplier, GraphSupplier::create(graph, OPEN_RING));
    DVV_TRY(const AVFrame* const first, supplier->first());

    // 3. Frame 0 decides the format, the size and the colour; the sink the frame rate.
    const int av_format = first->format;
    const char* pix_fmt = api().av_get_pix_fmt_name(static_cast<AVPixelFormat>(av_format));
    const std::string pix_fmt_name = pix_fmt != nullptr ? pix_fmt : "none";
    const std::optional<CatalogueFormat> format = catalogueFormat(pix_fmt_name);
    if (!format) {
        return makeError(Errc::Unsupported,
                         std::format("graph output {} is not in the catalogue (append ,format=<a "
                                     "catalogue name>)",
                                     pix_fmt_name));
    }
    const auto width = static_cast<std::uint32_t>(first->width);
    const auto height = static_cast<std::uint32_t>(first->height);
    // The frame's tags with the PAL/NTSC hint applied (D5R Task 0); a non-standard set is logged.
    const MappedColour mapped =
        mapColour(first->colorspace, first->color_range, first->color_trc, first->color_primaries);
    std::vector<std::string> notes;
    if (mapped.hint.nonStandard) {
        notes.push_back(nonStandardNote(ColourTags{.space = first->colorspace,
                                                   .range = first->color_range,
                                                   .transfer = first->color_trc,
                                                   .primaries = first->color_primaries}));
    }
    media::ColourParams colour =
        withBt601Hint(mapped.params, mapped.hint.bt601, media::descriptor(format->format).model);
    if (format->fullRange && colour.range == media::ColourRange::Unspecified) {
        colour.range = media::ColourRange::Full;
    }
    DVV_TRY_VOID(media::validateColour(colour));
    const media::FrameDesc desc{.format = format->format,
                                .colour = media::internColour(colour),
                                .width = width,
                                .height = height};
    DVV_TRY(const media::FrameGeometry layout, media::geometry(desc.format, width, height));
    std::vector<std::string> warnings;
    const AVRational rate = supplier->frameRate();
    media::Rational fps = reduced({rate.num, rate.den});
    if (fps.num <= 0 || fps.den <= 0) {
        fps = DEFAULT_FPS;
        warnings.emplace_back("frame rate unknown, 25 fps assumed");
    }

    // 4. Every later frame is checked against frame 0; the ring gets its D5 size (frame 0 stays).
    supplier->setExpected(desc, av_format);
    supplier->resizeRing(ringCapacity(layout.bytes));

    // 5. The source's info and its worker (the thread starts here).
    OpenedGraph opened;
    media::SourceInfo& info = opened.info;
    info.desc = desc;
    info.fps = fps;
    info.frameCount = std::nullopt;
    info.access = media::SourceAccess::Sequential;
    info.deterministic = lavfiDeterministic(graph);  // gradients and random colours are not (D6)
    info.resolvedUri = "lavfi:" + graph;
    info.provider = "lavfi";
    info.warnings = std::move(warnings);
    info.notes = std::move(notes);
    // The hint from frame 0's tags, for the labels (D5R F1).
    info.colourHint = mapped.hint;
    info.reader = "lavfi";  // no file is read (no file read mode applies)
    opened.worker = std::make_unique<AvWorker>(std::move(supplier), desc, generation);
    return opened;
}

Result<std::unique_ptr<AvWorker>> makeGraphWorkerForTest(std::string graph) {
    Uri uri;
    uri.scheme = "lavfi";
    uri.body = std::move(graph);
    DVV_TRY(OpenedGraph opened, openGraph(uri));
    return std::move(opened.worker);
}

}  // namespace dvv::components::detail

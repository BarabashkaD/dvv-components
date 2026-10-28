// The `ffmpeg` provider's container path (design v2 §4.4, D5, D28; v1.0 codec scope).

#include "sources/container_supplier.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/log.h"
#include "dvv/base/uri.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/source.h"
#include "dvv/media/types.h"
#include "dvv/media/uri_colour.h"
#include "ffmpeg/api.h"
#include "sources/av_worker.h"
#include "sources/ffmpeg_rules.h"
#include "sources/frame_ring.h"

namespace dvv::components::detail {
namespace {

constexpr media::Rational DEFAULT_FPS{25, 1};

Error sourceClosed() {
    return Error{.code = Errc::Internal, .message = "source closed", .where = {}, .cause = nullptr};
}

Error notProduced(std::int64_t index) {
    return Error{.code = Errc::Format,
                 .message = std::format("frame {} not produced by the decoder", index),
                 .where = {},
                 .cause = nullptr};
}

// `from`: the frame the decoder reported the damage for (nullopt when that frame was dropped).
Error damagedFrame(std::int64_t index, std::optional<std::int64_t> from) {
    std::string reason = "it depends on a damaged frame";
    if (from == index) {
        reason = "the decoder reported an error";
    } else if (from) {
        reason = std::format("it depends on damaged frame {}", *from);
    }
    return Error{.code = Errc::Format,
                 .message = std::format("frame {} is damaged ({})", index, reason),
                 .where = {},
                 .cause = nullptr};
}

// Only `stream` is demuxed (D5: every other stream is discarded).
void discardOthers(AVFormatContext& format, int stream) noexcept {
    for (unsigned i = 0; i < format.nb_streams; ++i) {
        if (static_cast<int>(i) != stream) {
            format.streams[i]->discard = AVDISCARD_ALL;
        }
    }
}

// D5 stream choice: the best video stream; when it is an attached picture, the first video stream
// that is not. -1 when there is none.
int chooseStream(AVFormatContext& format) {
    const int best =
        ffmpeg::api().av_find_best_stream(&format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (best >= 0 && (format.streams[best]->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
        return best;
    }
    for (unsigned i = 0; i < format.nb_streams; ++i) {
        const AVStream& stream = *format.streams[i];
        if (stream.codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            (stream.disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The D5 stream choice and the lossless codec allowlist on the codec id the demuxer reports.
Result<int> chooseLosslessStream(AVFormatContext& format) {
    const int stream = chooseStream(format);
    if (stream < 0) {
        return makeError(Errc::Unsupported, "no video stream");
    }
    DVV_TRY_VOID(checkLosslessCodec(
        ffmpeg::api().avcodec_get_name(format.streams[stream]->codecpar->codec_id)));
    return stream;
}

// LOSSLESS_CODECS as FFmpeg's comma-separated codec_whitelist.
std::string codecWhitelist() {
    std::string list;
    for (const std::string_view codec : LOSSLESS_CODECS) {
        if (!list.empty()) {
            list += ',';
        }
        list += codec;
    }
    return list;
}

// avformat_open_input with protocol_whitelist=file (D3) and codec_whitelist = the lossless
// allowlist (FFmpeg opens no other decoder, not even while avformat_find_stream_info probes
// the streams), then avformat_find_stream_info.
Result<ffmpeg::FormatContextPtr> openFormat(const std::string& url) {
    const ffmpeg::Api& api = ffmpeg::api();
    static const std::string CODECS = codecWhitelist();
    const std::array<std::pair<const char*, const char*>, 2> settings{{
        {"protocol_whitelist", "file"},
        {"codec_whitelist", CODECS.c_str()},
    }};
    AVDictionary* options = nullptr;
    for (const auto& [key, value] : settings) {
        if (const int result = api.av_dict_set(&options, key, value, 0); result < 0) {
            api.av_dict_free(&options);
            return std::unexpected(ffmpeg::ffmpegError("av_dict_set", result));
        }
    }
    AVFormatContext* context = nullptr;
    const int opened = api.avformat_open_input(&context, url.c_str(), nullptr, &options);
    api.av_dict_free(&options);
    if (opened < 0) {
        return std::unexpected(
            ffmpeg::ffmpegError("avformat_open_input", opened));  // freed by FFmpeg
    }
    ffmpeg::FormatContextPtr format(context);
    if (const int result = api.avformat_find_stream_info(context, nullptr); result < 0) {
        // Without its refused decoder a lossy stream may lack parameters: the allowlist error is
        // the clear one.
        if (auto chosen = chooseLosslessStream(*context); !chosen) {
            return std::unexpected(std::move(chosen).error());
        }
        return std::unexpected(ffmpeg::ffmpegError("avformat_find_stream_info", result));
    }
    return format;
}

}  // namespace

// ---- open (steps 2-8) ----

Result<std::unique_ptr<ContainerSupplier>> ContainerSupplier::open(const std::string& path_utf8,
                                                                   const StopCheck& stopped) {
    const ffmpeg::Api& api = ffmpeg::api();
    std::unique_ptr<ContainerSupplier> supplier(new ContainerSupplier());
    supplier->url_ = "file:" + path_utf8;  // a Windows drive letter is not a protocol

    // 2. Open (codec_whitelist: no decoder outside the allowlist runs, not even for probing).
    DVV_TRY(ffmpeg::FormatContextPtr scan, openFormat(supplier->url_));
    // 3. Stream choice and 3b. the lossless codec allowlist.
    DVV_TRY(const int stream, chooseLosslessStream(*scan));
    supplier->stream_ = stream;
    discardOthers(*scan, stream);

    // 4. The frame index: the container's own when it lists every frame, else a demux-only scan.
    supplier->packet_.reset(api.av_packet_alloc());
    supplier->scratch_.reset(api.av_frame_alloc());
    if (!supplier->packet_ || !supplier->scratch_) {
        return makeError(Errc::OutOfMemory, "FFmpeg could not allocate a packet or frame");
    }
    if (!supplier->indexFromContainer(*scan->streams[stream])) {
        DVV_TRY_VOID(supplier->scanIndex(*scan, stopped));
    }
    if (supplier->entries_.empty()) {
        return makeError(Errc::Format, "no video frames");
    }
    // 5. Mode.
    std::vector<std::int64_t> pts;
    pts.reserve(supplier->entries_.size());
    std::ranges::transform(supplier->entries_, std::back_inserter(pts), &Entry::pts);
    supplier->sequential_ = sequentialMode(pts);
    if (!supplier->sequential_) {
        std::ranges::stable_sort(supplier->entries_, {}, &Entry::pts);
    }
    supplier->frame_count_ = static_cast<std::int64_t>(supplier->entries_.size());
    scan.reset();

    // 6. Reopen, so decoding starts at the beginning.
    DVV_TRY_VOID(supplier->reopen());

    // 7. Decoder (software; FFmpeg's own threads).
    const AVStream& chosen = *supplier->format_->streams[stream];
    const AVCodec* decoder = api.avcodec_find_decoder(chosen.codecpar->codec_id);
    if (decoder == nullptr) {
        return makeError(
            Errc::Unsupported,
            std::format("no decoder for {}", api.avcodec_get_name(chosen.codecpar->codec_id)));
    }
    supplier->codec_.reset(api.avcodec_alloc_context3(decoder));
    if (!supplier->codec_) {
        return makeError(Errc::OutOfMemory, "FFmpeg could not allocate a decoder");
    }
    if (const int result =
            api.avcodec_parameters_to_context(supplier->codec_.get(), chosen.codecpar);
        result < 0) {
        return std::unexpected(ffmpeg::ffmpegError("avcodec_parameters_to_context", result));
    }
    supplier->codec_->thread_count = 0;
    supplier->codec_->pkt_timebase = chosen.time_base;
    if (chosen.codecpar->codec_id == AV_CODEC_ID_FFV1 && chosen.codecpar->extradata_size > 0) {
        // FFV1 reports a damaged slice (slice CRC mismatch) only in its log. With slice
        // threads each packet is decoded inside the send call, so a raised mark belongs to the next
        // received frame. Only FFV1 v2+ has extradata (its configuration record) and only v3 has
        // slice CRCs; v0/v1 have no CRC to report, so they keep FFmpeg's default (frame) threads.
        supplier->codec_->thread_type = FF_THREAD_SLICE;
        supplier->codec_->opaque = &supplier->decode_error_;
    }
    if (const int result = api.avcodec_open2(supplier->codec_.get(), decoder, nullptr);
        result < 0) {
        return std::unexpected(ffmpeg::ffmpegError("avcodec_open2", result));
    }
    // An error FFmpeg logged while it opened the decoder belongs to no frame.
    supplier->decode_error_.raised.store(false, std::memory_order_relaxed);

    // 8. Frame 0 (into the ring).
    DVV_TRY_VOID(supplier->frame(0, stopped));
    return supplier;
}

bool ContainerSupplier::indexFromContainer(AVStream& stream) {
    const ffmpeg::Api& api = ffmpeg::api();
    const int count = api.avformat_index_get_entries_count(&stream);
    if (stream.nb_frames <= 0 || count != stream.nb_frames) {
        return false;
    }
    std::vector<Entry> entries;
    entries.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const AVIndexEntry* entry = api.avformat_index_get_entry(&stream, i);
        if (entry == nullptr || (entry->flags & AVINDEX_DISCARD_FRAME) != 0 ||
            entry->timestamp == AV_NOPTS_VALUE ||
            (!entries.empty() && entry->timestamp <= entries.back().pts)) {
            return false;
        }
        entries.push_back({.pts = entry->timestamp, .key = (entry->flags & AVINDEX_KEYFRAME) != 0});
    }
    // The entries hold decode timestamps: a first pts that differs (composition offsets) means
    // they are not the frames' pts.
    if (stream.start_time != AV_NOPTS_VALUE && stream.start_time != entries.front().pts) {
        return false;
    }
    entries_ = std::move(entries);
    return true;
}

Result<void> ContainerSupplier::scanIndex(AVFormatContext& format, const StopCheck& stopped) {
    const ffmpeg::Api& api = ffmpeg::api();
    AVPacket* const packet = packet_.get();
    while (true) {
        if (stopped()) {
            return std::unexpected(sourceClosed());
        }
        const int result = api.av_read_frame(&format, packet);
        if (result == AVERROR_EOF) {
            return {};
        }
        if (result < 0) {
            return std::unexpected(ffmpeg::ffmpegError("av_read_frame", result));
        }
        ++scanned_;
        if (packet->stream_index == stream_ && (packet->flags & AV_PKT_FLAG_DISCARD) == 0) {
            entries_.push_back({.pts = packet->pts, .key = (packet->flags & AV_PKT_FLAG_KEY) != 0});
        }
        api.av_packet_unref(packet);
    }
}

Result<void> ContainerSupplier::reopen() {
    // Until the reopen succeeds every request restarts (and retries it): a failed reopen leaves no
    // format context, or one of a changed file, that a forward request must not read.
    needs_reset_ = true;
    format_.reset();
    DVV_TRY(format_, openFormat(url_));
    if (stream_ >= static_cast<int>(format_->nb_streams)) {
        return makeError(Errc::Format, "the file changed while open");
    }
    discardOthers(*format_, stream_);
    last_ = -1;
    floor_ = 0;
    demuxer_end_ = false;
    needs_reset_ = false;
    return {};
}

const AVFrame& ContainerSupplier::firstFrame() const noexcept {
    // open() decoded frame 0 into the ring, and nothing ran on the supplier since.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): open() created the ring with frame 0
    return *ring_->find(0);
}

AVRational ContainerSupplier::guessedFrameRate() const noexcept {
    return ffmpeg::api().av_guess_frame_rate(format_.get(), format_->streams[stream_], nullptr);
}

const AVCodecParameters& ContainerSupplier::codecParameters() const noexcept {
    return *format_->streams[stream_]->codecpar;
}

// ---- decode (on the worker) ----

Result<const AVFrame*> ContainerSupplier::frame(std::int64_t index, const StopCheck& stopped) {
    if (ring_) {
        if (const AVFrame* held = ring_->find(index)) {
            return held;
        }
    }
    if (sequential_) {
        // D5/D6: forward from the newest frame; anything behind it restarts from the beginning.
        if (needs_reset_ || !last_ || index <= *last_) {
            DVV_TRY_VOID(restart());
        }
    } else {
        const auto capacity = static_cast<std::int64_t>(ring_ ? ring_->capacity() : 1);
        const bool forward = !needs_reset_ && last_ && *last_ < index && index <= *last_ + capacity;
        if (!forward) {
            DVV_TRY_VOID(seekTo(index));
        }
    }
    kept_position_ = false;
    Result<const AVFrame*> decoded = decodeUntil(index, stopped);
    // The decoder stopped mid-stream: Sequential numbers would shift. A damaged frame `index`
    // keeps the decoder in step instead: the next request decodes on.
    if (!decoded && !kept_position_ && !stopped()) {
        needs_reset_ = true;
    }
    return decoded;
}

Result<void> ContainerSupplier::restart() {
    ++resets_;
    DVV_TRY_VOID(reopen());
    ffmpeg::api().avcodec_flush_buffers(codec_.get());
    decode_error_.raised.store(false, std::memory_order_relaxed);
    damaged_ = false;
    if (ring_) {
        ring_->clear();
    }
    return {};
}

Result<void> ContainerSupplier::seekTo(std::int64_t index) {
    ++resets_;
    // The last keyframe entry at or before `index` (entry 0 when none is flagged).
    std::int64_t key = 0;
    for (std::int64_t entry = index; entry >= 0; --entry) {
        if (entries_[static_cast<std::size_t>(entry)].key) {
            key = entry;
            break;
        }
    }
    const ffmpeg::Api& api = ffmpeg::api();
    const std::int64_t target = entries_[static_cast<std::size_t>(key)].pts;
    if (const int result = api.avformat_seek_file(
            format_.get(), stream_, std::numeric_limits<std::int64_t>::min(), target, target, 0);
        result < 0) {
        needs_reset_ = true;
        return std::unexpected(ffmpeg::ffmpegError("avformat_seek_file", result));
    }
    api.avcodec_flush_buffers(codec_.get());  // after every avformat_seek_file
    decode_error_.raised.store(false, std::memory_order_relaxed);
    damaged_ = false;  // decoding starts again at a key frame
    if (ring_) {
        ring_->clear();
    }
    last_.reset();
    floor_ = key;
    demuxer_end_ = false;
    needs_reset_ = false;
    return {};
}

std::optional<std::int64_t> ContainerSupplier::indexOf(const AVFrame& frame) {
    if (sequential_) {
        return last_.value_or(-1) + 1;
    }
    if (frame.pts == AV_NOPTS_VALUE) {
        DVV_LOG_DEBUG("ffmpeg: a decoded frame without pts was dropped");
        return std::nullopt;
    }
    const auto found = std::ranges::lower_bound(entries_, frame.pts, {}, &Entry::pts);
    if (found == entries_.end() || found->pts != frame.pts) {
        DVV_LOG_DEBUG("ffmpeg: a decoded frame with the unknown pts {} was dropped", frame.pts);
        return std::nullopt;
    }
    const auto index = static_cast<std::int64_t>(std::distance(entries_.begin(), found));
    if (index < floor_) {
        // An open-GOP leading frame after the seek: it may reference the previous GOP.
        return std::nullopt;
    }
    return index;
}

void ContainerSupplier::createRing(const AVFrame& frame) {
    std::uint64_t bytes = 0;
    const char* name = ffmpeg::api().av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame.format));
    if (const auto format = catalogueFormat(name != nullptr ? name : "")) {
        if (const auto layout =
                media::geometry(format->format, static_cast<std::uint32_t>(frame.width),
                                static_cast<std::uint32_t>(frame.height))) {
            bytes = layout->bytes;
        }
    }
    // A format outside the catalogue fails open; one frame is enough until then.
    ring_.emplace(bytes > 0 ? ringCapacity(bytes) : 1);
}

Result<const AVFrame*> ContainerSupplier::decodeUntil(std::int64_t index,
                                                      const StopCheck& stopped) {
    const ffmpeg::Api& api = ffmpeg::api();
    AVFrame* const decoded = scratch_.get();
    while (true) {
        if (stopped()) {
            return std::unexpected(sourceClosed());
        }
        const int result = api.avcodec_receive_frame(codec_.get(), decoded);
        if (result == 0) {
            // Damage the decoder reported for this frame carries on to the frames
            // until the next key frame (they reference it).
            const bool reported = decode_error_.raised.exchange(false, std::memory_order_acq_rel) ||
                                  decoded->decode_error_flags != 0 ||
                                  (decoded->flags & AV_FRAME_FLAG_CORRUPT) != 0;
            damaged_ = reported || (damaged_ && (decoded->flags & AV_FRAME_FLAG_KEY) == 0);
            const std::optional<std::int64_t> number = indexOf(*decoded);
            if (reported) {
                damaged_from_ = number;
            }
            if (!number) {
                api.av_frame_unref(decoded);
                continue;
            }
            if (!expected_) {
                expected_ = Expected{.width = static_cast<std::uint32_t>(decoded->width),
                                     .height = static_cast<std::uint32_t>(decoded->height),
                                     .format = decoded->format};
                createRing(*decoded);
            }
            const media::FrameDesc size{
                .format = 0, .colour = 0, .width = expected_->width, .height = expected_->height};
            last_ = number;
            if (damaged_) {  // never in the ring: its pixels are concealed, not decoded
                api.av_frame_unref(decoded);
                if (*number < index) {
                    continue;
                }
                if (*number == index) {
                    kept_position_ = true;  // received and consumed: the decoder is in step
                    return std::unexpected(damagedFrame(index, damaged_from_));
                }
                // Frame `index` was dropped, and this damaged frame is not in the ring: a request
                // for it must decode it again, so frame() resets.
                return std::unexpected(notProduced(index));
            }
            if (auto checked = checkFrame(*decoded, *number, size, expected_->format); !checked) {
                api.av_frame_unref(decoded);
                return std::unexpected(std::move(checked).error());
            }
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): createRing ran with expected_
            FrameRing& ring = *ring_;
            ring.put(*number, *decoded);
            if (*number == index) {
                return ring.find(index);
            }
            if (*number > index) {
                return std::unexpected(notProduced(index));
            }
            continue;
        }
        if (result == AVERROR(EAGAIN) && !demuxer_end_) {
            DVV_TRY_VOID(sendNextPacket(stopped));
            continue;
        }
        if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) {
            needs_reset_ = true;  // the drained decoder needs a flush: the next request seeks
            return std::unexpected(notProduced(index));
        }
        return std::unexpected(ffmpeg::ffmpegError("avcodec_receive_frame", result));
    }
}

Result<void> ContainerSupplier::sendNextPacket(const StopCheck& stopped) {
    const ffmpeg::Api& api = ffmpeg::api();
    AVPacket* const packet = packet_.get();
    while (true) {
        if (stopped()) {
            return std::unexpected(sourceClosed());
        }
        const int read = api.av_read_frame(format_.get(), packet);
        if (read == AVERROR_EOF) {
            demuxer_end_ = true;
            if (const int sent = api.avcodec_send_packet(codec_.get(), nullptr);
                sent < 0 && sent != AVERROR_EOF) {
                return std::unexpected(ffmpeg::ffmpegError("avcodec_send_packet", sent));
            }
            return {};
        }
        if (read < 0) {
            return std::unexpected(ffmpeg::ffmpegError("av_read_frame", read));
        }
        if (packet->stream_index != stream_ || (packet->flags & AV_PKT_FLAG_DISCARD) != 0) {
            api.av_packet_unref(packet);
            continue;
        }
        if (stopped()) {
            api.av_packet_unref(packet);
            return std::unexpected(sourceClosed());
        }
        const int sent = api.avcodec_send_packet(codec_.get(), packet);
        api.av_packet_unref(packet);
        if (sent < 0) {
            return std::unexpected(ffmpeg::ffmpegError("avcodec_send_packet", sent));
        }
        return {};
    }
}

// ---- open steps 8-12 ----

Result<OpenedContainer> openContainer(const Uri& uri) {
    const ffmpeg::Api& api = ffmpeg::api();
    // One stop generation for the open and the worker: a source whose open overlapped
    // stopFfmpegSources() is born stopped.
    const std::uint64_t generation = ffmpeg::stopGeneration();
    const StopCheck opening{.flag = nullptr, .generation = generation};
    DVV_TRY(std::unique_ptr<ContainerSupplier> supplier,
            ContainerSupplier::open(uri.path, opening));

    // 8. The catalogue format of the decoder's output.
    const AVFrame& first = supplier->firstFrame();
    const char* pix_fmt = api.av_get_pix_fmt_name(static_cast<AVPixelFormat>(first.format));
    const std::string pix_fmt_name = pix_fmt != nullptr ? pix_fmt : "none";
    const std::optional<CatalogueFormat> format = catalogueFormat(pix_fmt_name);
    if (!format) {
        return makeError(Errc::Unsupported,
                         std::format("decoder output {} is not in the catalogue", pix_fmt_name));
    }
    const auto width = static_cast<std::uint32_t>(first.width);
    const auto height = static_cast<std::uint32_t>(first.height);

    // 9. Frame rate and the D28 query check.
    std::vector<std::string> warnings;
    const AVRational guessed = supplier->guessedFrameRate();
    media::Rational fps = reduced({guessed.num, guessed.den});
    if (fps.num <= 0 || fps.den <= 0) {
        fps = DEFAULT_FPS;
        warnings.emplace_back("frame rate unknown, 25 fps assumed");
    }
    DVV_TRY(const media::ColourSpec query,
            checkContainerQuery(
                uri, ContainerFacts{
                         .format = format->format, .width = width, .height = height, .fps = fps}));

    // 10. Colour: the frame's tags, then the stream's (per field; both logged when they differ),
    // then the alias range, the PAL/NTSC hint, then the query (D28, D5R §3.1).
    const AVCodecParameters& parameters = supplier->codecParameters();
    const ColourTags frame_tags{.space = first.colorspace,
                                .range = first.color_range,
                                .transfer = first.color_trc,
                                .primaries = first.color_primaries};
    const ColourTags stream_tags{.space = parameters.color_space,
                                 .range = parameters.color_range,
                                 .transfer = parameters.color_trc,
                                 .primaries = parameters.color_primaries};
    const ColourTags tags = mergeTags(frame_tags, stream_tags);
    std::vector<std::string> notes = tagConflicts(frame_tags, stream_tags);
    const MappedColour mapped = mapColour(tags.space, tags.range, tags.transfer, tags.primaries);
    if (mapped.hint.nonStandard) {
        notes.push_back(nonStandardNote(tags));
    }
    const media::ColourModel model = media::descriptor(format->format).model;
    media::ColourParams file_colour = withBt601Hint(mapped.params, mapped.hint.bt601, model);
    if (format->fullRange && file_colour.range == media::ColourRange::Unspecified) {
        file_colour.range = media::ColourRange::Full;
    }
    DVV_TRY(const media::ColourParams colour, overrideColour(file_colour, query, model));
    const media::FrameDesc desc{.format = format->format,
                                .colour = media::internColour(colour),
                                .width = width,
                                .height = height};
    DVV_TRY_VOID(media::geometry(desc.format, width, height));

    // 11. The resolved URI: the stream's facts and only the query's colour keys.
    Uri resolved;
    resolved.scheme = "file";
    resolved.path = uri.path;
    resolved.params = {
        {"fmt", media::descriptor(format->format).name},
        {"size", std::format("{}x{}", width, height)},
        {"fps", fpsText(fps)},
    };
    std::ranges::move(media::colourParameters(query), std::back_inserter(resolved.params));
    resolved.params.emplace_back("impl", "ffmpeg");

    // 12. The source's info and its worker (the thread starts here).
    OpenedContainer opened;
    media::SourceInfo& info = opened.info;
    info.desc = desc;
    info.fps = fps;
    info.frameCount = supplier->frameCount();
    info.access = supplier->sequential() ? media::SourceAccess::Sequential
                                         : media::SourceAccess::RandomCostly;
    info.deterministic = true;
    info.resolvedUri = formatUri(resolved);
    info.provider = "ffmpeg";
    info.warnings = std::move(warnings);
    info.notes = std::move(notes);
    // The hint from the file's tags, for the labels (D5R F1).
    info.colourHint = mapped.hint;
    info.reader = "ffmpeg";  // FFmpeg's own file I/O (no file read mode applies)
    opened.worker = std::make_unique<AvWorker>(std::move(supplier), desc, generation);
    return opened;
}

Result<std::unique_ptr<AvWorker>> makeContainerWorkerForTest(const std::filesystem::path& path) {
    Uri uri;
    uri.scheme = "file";
    uri.path = media::utf8FromPath(path);
    DVV_TRY(OpenedContainer opened, openContainer(uri));
    return std::move(opened.worker);
}

}  // namespace dvv::components::detail

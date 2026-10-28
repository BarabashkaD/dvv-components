#pragma once

// The FFmpeg function table of dvv_ffmpeg (M4 design D3, §4.4): every FFmpeg function
// dvv-components calls, resolved at load time from the six libraries; RAII handles; the AVERROR
// mapping (§7). Internal to dvv-components; never installed.

#include <atomic>
#include <cstdint>
#include <memory>
#include <source_location>
#include <string_view>

#include "dvv/base/error.h"
#include "ffmpeg/ffmpeg_headers.h"

namespace dvv::components::ffmpeg {

enum class Library : std::uint8_t { Avutil, Avcodec, Avformat, Swscale, Swresample, Avfilter };

// X(<Library>, <function>): every FFmpeg function dvv-components calls. Add a line when a provider
// needs another function; it must be exported by that library. Avfilter's entries are null unless
// FfmpegStatus::lavfi.
// NOLINTBEGIN(bugprone-macro-parentheses)
#define DVV_FFMPEG_FUNCTIONS(X)                   \
    X(Avutil, avutil_version)                     \
    X(Avutil, avutil_license)                     \
    X(Avutil, avutil_configuration)               \
    X(Avutil, av_version_info)                    \
    X(Avutil, av_log)                             \
    X(Avutil, av_log_set_callback)                \
    X(Avutil, av_log_set_level)                   \
    X(Avutil, av_log_format_line2)                \
    X(Avutil, av_strerror)                        \
    X(Avutil, av_frame_alloc)                     \
    X(Avutil, av_frame_free)                      \
    X(Avutil, av_frame_unref)                     \
    X(Avutil, av_frame_move_ref)                  \
    X(Avutil, av_dict_set)                        \
    X(Avutil, av_dict_free)                       \
    X(Avutil, av_get_pix_fmt_name)                \
    X(Avutil, av_pix_fmt_desc_get)                \
    X(Avutil, av_pix_fmt_count_planes)            \
    X(Avutil, av_md5_sum)                         \
    X(Avutil, av_opt_set)                         \
    X(Avutil, av_opt_set_bin)                     \
    X(Avutil, av_rescale_q)                       \
    X(Avcodec, avcodec_version)                   \
    X(Avcodec, avcodec_license)                   \
    X(Avcodec, avcodec_configuration)             \
    X(Avcodec, avcodec_get_name)                  \
    X(Avcodec, avcodec_get_class)                 \
    X(Avcodec, avcodec_find_decoder)              \
    X(Avcodec, avcodec_alloc_context3)            \
    X(Avcodec, avcodec_free_context)              \
    X(Avcodec, avcodec_parameters_to_context)     \
    X(Avcodec, avcodec_open2)                     \
    X(Avcodec, avcodec_send_packet)               \
    X(Avcodec, avcodec_receive_frame)             \
    X(Avcodec, avcodec_flush_buffers)             \
    X(Avcodec, av_packet_alloc)                   \
    X(Avcodec, av_packet_free)                    \
    X(Avcodec, av_packet_unref)                   \
    X(Avformat, avformat_version)                 \
    X(Avformat, avformat_license)                 \
    X(Avformat, avformat_configuration)           \
    X(Avformat, avformat_open_input)              \
    X(Avformat, avformat_close_input)             \
    X(Avformat, avformat_find_stream_info)        \
    X(Avformat, av_find_best_stream)              \
    X(Avformat, av_guess_frame_rate)              \
    X(Avformat, av_read_frame)                    \
    X(Avformat, avformat_seek_file)               \
    X(Avformat, avformat_index_get_entries_count) \
    X(Avformat, avformat_index_get_entry)         \
    X(Avformat, av_probe_input_format3)           \
    X(Avformat, av_match_ext)                     \
    X(Swscale, swscale_version)                   \
    X(Swscale, swscale_license)                   \
    X(Swscale, swscale_configuration)             \
    X(Swresample, swresample_version)             \
    X(Swresample, swresample_license)             \
    X(Swresample, swresample_configuration)       \
    X(Avfilter, avfilter_version)                 \
    X(Avfilter, avfilter_license)                 \
    X(Avfilter, avfilter_configuration)           \
    X(Avfilter, avfilter_graph_alloc)             \
    X(Avfilter, avfilter_graph_free)              \
    X(Avfilter, avfilter_graph_parse_ptr)         \
    X(Avfilter, avfilter_graph_config)            \
    X(Avfilter, avfilter_graph_create_filter)     \
    X(Avfilter, avfilter_get_by_name)             \
    X(Avfilter, avfilter_inout_alloc)             \
    X(Avfilter, avfilter_inout_free)              \
    X(Avfilter, avfilter_graph_segment_parse)     \
    X(Avfilter, avfilter_graph_segment_apply)     \
    X(Avfilter, avfilter_graph_segment_free)      \
    X(Avfilter, avfilter_link)                    \
    X(Avfilter, av_buffersink_get_frame)          \
    X(Avfilter, av_buffersink_get_frame_rate)

// One pointer per function, typed from the headers and named like the function.
struct Api {
#define DVV_FFMPEG_MEMBER(library_id, symbol) decltype(&::symbol) symbol = nullptr;
    DVV_FFMPEG_FUNCTIONS(DVV_FFMPEG_MEMBER)
#undef DVV_FFMPEG_MEMBER
};
// NOLINTEND(bugprone-macro-parentheses)

// The loaded table. Throws std::logic_error("FFmpeg is not loaded") unless FfmpegStatus::core;
// callers exist only where the capability does (a provider's probe requires it).
[[nodiscard]] const Api& api();

// The process-wide stop generation (design v2 D5, §5.4): read with acquire; stopFfmpegSources()
// increments it with release. A source worker remembers the value it was created with and ends its
// work once the value differs.
[[nodiscard]] std::uint64_t stopGeneration() noexcept;

// Some decoders report a damaged packet only in their log (FFmpeg 8.1.3 FFV1
// logs "slice CRC mismatch", conceals the slice and sets neither AVFrame::decode_error_flags nor
// AV_FRAME_FLAG_CORRUPT, also with AV_EF_EXPLODE). When an AVCodecContext's `opaque` points to a
// DecodeErrorMark, the FFmpeg log callback raises it for every message of level AV_LOG_ERROR or
// worse that FFmpeg logs for that context (on any FFmpeg thread). It names the frame being decoded
// only when the decoder decodes each packet inside the caller's send call (no frame threads).
// The `opaque` of every AVCodecContext dvv-components creates is reserved for a DecodeErrorMark
// (or null); the callback also checks the tag before it raises the mark.
struct DecodeErrorMark {
    static constexpr std::uint32_t TAG = 0x44454D4B;  // "DEMK"
    std::uint32_t tag = TAG;
    std::atomic<bool> raised{false};
};

// RAII handles; each deleter calls FFmpeg's free function through api(), null is a no-op. Pass a
// handle to a T** parameter (avformat_open_input, av_dict_set) as std::inout_ptr(handle).
struct FormatContextDeleter {
    void operator()(AVFormatContext* context) const noexcept;
};
struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const noexcept;
};
struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept;
};
struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept;
};
struct FilterGraphDeleter {
    void operator()(AVFilterGraph* graph) const noexcept;
};
struct FilterInOutDeleter {
    void operator()(AVFilterInOut* inout) const noexcept;
};
struct DictionaryDeleter {
    void operator()(AVDictionary* dictionary) const noexcept;
};
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FilterGraphPtr = std::unique_ptr<AVFilterGraph, FilterGraphDeleter>;
using FilterInOutPtr = std::unique_ptr<AVFilterInOut, FilterInOutDeleter>;
using DictionaryPtr = std::unique_ptr<AVDictionary, DictionaryDeleter>;

// M4 design §7: AVERROR(ENOMEM) -> OutOfMemory; AVERROR(ENOENT, EIO, EACCES, EPERM, EISDIR,
// ENOTDIR, ENXIO, ENODEV, EROFS, ENOSPC, EFBIG, ENAMETOOLONG, EMFILE) -> Io; AVERROR_INVALIDDATA ->
// Format; AVERROR_DECODER_NOT_FOUND, AVERROR_DEMUXER_NOT_FOUND, AVERROR_STREAM_NOT_FOUND,
// AVERROR_PATCHWELCOME, AVERROR(ENOSYS) -> Unsupported; anything else -> Internal.
[[nodiscard]] Errc errcForAverror(int averror) noexcept;
// errcForAverror(averror) with "<call> failed: <av_strerror text> (AVERROR <averror>)", or
// "<call> failed (AVERROR <averror>)" when FFmpeg is not loaded or av_strerror has no text.
[[nodiscard]] Error ffmpegError(std::string_view call, int averror,
                                std::source_location where = std::source_location::current());

}  // namespace dvv::components::ffmpeg

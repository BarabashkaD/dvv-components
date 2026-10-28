// The AVERROR mapping (M4 design v2 §7) and the RAII deleters of the FFmpeg binding.

#include "dvv/base/error.h"

#include <array>
#include <cerrno>
#include <format>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ffmpeg/api.h"

namespace dvv::components::ffmpeg {
namespace {

// The table, or nullptr when FFmpeg is not loaded (error paths must not throw).
const Api* loadedApi() noexcept {
    try {
        return &api();
    } catch (const std::logic_error&) {
        return nullptr;
    }
}

}  // namespace

void FormatContextDeleter::operator()(AVFormatContext* context) const noexcept {
    if (context != nullptr) {
        api().avformat_close_input(&context);
    }
}
void CodecContextDeleter::operator()(AVCodecContext* context) const noexcept {
    if (context != nullptr) {
        api().avcodec_free_context(&context);
    }
}
void FrameDeleter::operator()(AVFrame* frame) const noexcept {
    if (frame != nullptr) {
        api().av_frame_free(&frame);
    }
}
void PacketDeleter::operator()(AVPacket* packet) const noexcept {
    if (packet != nullptr) {
        api().av_packet_free(&packet);
    }
}
void FilterGraphDeleter::operator()(AVFilterGraph* graph) const noexcept {
    if (graph != nullptr) {
        api().avfilter_graph_free(&graph);
    }
}
void FilterInOutDeleter::operator()(AVFilterInOut* inout) const noexcept {
    if (inout != nullptr) {
        api().avfilter_inout_free(&inout);
    }
}
void DictionaryDeleter::operator()(AVDictionary* dictionary) const noexcept {
    if (dictionary != nullptr) {
        api().av_dict_free(&dictionary);
    }
}

Errc errcForAverror(int averror) noexcept {
    switch (averror) {
        case AVERROR(ENOMEM): return Errc::OutOfMemory;
        case AVERROR(ENOENT):
        case AVERROR(EIO):
        case AVERROR(EACCES):
        case AVERROR(EPERM):
        case AVERROR(EISDIR):
        case AVERROR(ENOTDIR):
        case AVERROR(ENXIO):
        case AVERROR(ENODEV):
        case AVERROR(EROFS):
        case AVERROR(ENOSPC):
        case AVERROR(EFBIG):
        case AVERROR(ENAMETOOLONG):
        case AVERROR(EMFILE): return Errc::Io;
        case AVERROR_INVALIDDATA: return Errc::Format;
        case AVERROR_DECODER_NOT_FOUND:
        case AVERROR_DEMUXER_NOT_FOUND:
        case AVERROR_STREAM_NOT_FOUND:
        case AVERROR_PATCHWELCOME:
        case AVERROR(ENOSYS): return Errc::Unsupported;
        default: return Errc::Internal;
    }
}

Error ffmpegError(std::string_view call, int averror, std::source_location where) {
    std::string message;
    if (const Api* table = loadedApi(); table != nullptr) {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
        if (table->av_strerror(averror, text.data(), text.size()) == 0 && text.front() != '\0') {
            message = std::format("{} failed: {} (AVERROR {})", call, text.data(), averror);
        }
    }
    if (message.empty()) {
        message = std::format("{} failed (AVERROR {})", call, averror);
    }
    return Error{.code = errcForAverror(averror),
                 .message = std::move(message),
                 .where = where,
                 .cause = nullptr};
}

}  // namespace dvv::components::ffmpeg

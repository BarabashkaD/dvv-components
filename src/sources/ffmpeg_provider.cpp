// The `ffmpeg` source provider (design v2 §4.4, §5.1; v1.0 codec scope): files of any container
// FFmpeg demuxes, lossless video codecs only, through FFmpeg loaded at run time (D3).

#include "sources/ffmpeg_provider.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/registry/component_registry.h"
#include "dvv/registry/parameters.h"
#include "ffmpeg/api.h"
#include "sources/av_worker.h"
#include "sources/container_supplier.h"
#include "sources/ffmpeg_rules.h"

namespace dvv::components {
namespace {

constexpr std::array<std::string_view, 1> SCHEMES{"file"};

}  // namespace

FfmpegSourceProvider::FfmpegSourceProvider(CreationKey<ComponentRegistry> key,
                                           const ParameterValues& /*values*/) noexcept
    : Managed(std::move(key)) {}

const ParameterSchema& FfmpegSourceProvider::schema() {
    static const ParameterSchema SCHEMA;
    return SCHEMA;
}

std::span<const std::string_view> FfmpegSourceProvider::schemes() const noexcept {
    return SCHEMES;
}

media::Match FfmpegSourceProvider::match(const media::SourceRequest& request,
                                         std::span<const std::byte> head) const {
    if (request.uri.scheme != "file") {
        return media::Match{.confidence = 0, .guess = {}, .why = "not a file"};
    }
    // av_probe_input_format3 reads up to AVPROBE_PADDING_SIZE bytes past buf_size.
    std::vector<unsigned char> buffer(head.size() + AVPROBE_PADDING_SIZE, 0);
    std::ranges::transform(head, buffer.begin(),
                           [](std::byte value) { return static_cast<unsigned char>(value); });
    AVProbeData probe{};
    probe.filename = request.uri.path.c_str();
    probe.buf = buffer.data();
    probe.buf_size = static_cast<int>(head.size());
    probe.mime_type = nullptr;
    int score = 0;
    const AVInputFormat* format = ffmpeg::api().av_probe_input_format3(&probe, 1, &score);
    if (format == nullptr) {
        return media::Match{
            .confidence = 0, .guess = {}, .why = "FFmpeg does not recognise the file"};
    }
    const std::string_view name = format->name != nullptr ? format->name : "";
    const int confidence = detail::matchConfidence(score, name);
    std::string why;
    if (confidence > 0) {
        why = std::format("FFmpeg demuxer {} (score {})", name, score);
    } else if (detail::excludedDemuxer(name)) {
        why = std::format("FFmpeg demuxer {} left to its own provider", name);
    } else {
        why = std::format("FFmpeg probe score {} ({}) is not a content match", score, name);
    }
    // The guess stays empty: the format is known only after decoding.
    return media::Match{.confidence = confidence, .guess = {}, .why = std::move(why)};
}

Task<Result<std::unique_ptr<media::ISource>>> FfmpegSourceProvider::open(
    media::OpenTicket ticket, const media::SourceRequest& request,
    const media::FormatGuess& /*guess*/, const media::OpenContext& context) {
    co_await resumeOn(context.io);  // all FFmpeg work of open runs on the I/O pool (§5.1)
    detail::OpenedContainer opened = co_try(detail::openContainer(request.uri));
    std::unique_ptr<media::ISource> source = std::make_unique<FfmpegSource>(
        media::SourceFactory::issueKey<FfmpegSource>(std::move(ticket)), std::move(opened.info),
        std::move(opened.worker));
    co_return std::move(source);
}

FfmpegSource::FfmpegSource(CreationKey<media::SourceFactory> key, media::SourceInfo info,
                           std::unique_ptr<detail::AvWorker> worker) noexcept
    : Managed(std::move(key)), info_(std::move(info)), worker_(std::move(worker)) {}

Task<Result<media::FrameRef>> FfmpegSource::produce(std::int64_t index,
                                                    media::ProduceContext& ctx) {
    const std::int64_t frames = info_.frameCount.value_or(0);
    if (index < 0 || index >= frames) {
        co_return makeError(Errc::Internal,
                            std::format("frame {} is outside [0, {})", index, frames));
    }
    // Concurrent calls (the store's prefetch) queue on the worker (§5.4).
    co_return co_await worker_->produce(index, ctx);
}

DVV_REGISTER(media::ISourceProvider, FfmpegSourceProvider);

}  // namespace dvv::components

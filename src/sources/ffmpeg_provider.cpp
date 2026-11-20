// The `ffmpeg` source provider (design v2 §4.4, §5.1; v1.0 codec scope): files of any container
// FFmpeg demuxes, lossless video codecs only, through FFmpeg loaded at run time (D3).

#include "sources/ffmpeg_provider.h"

#include <algorithm>
#include <array>
#include <cstddef>
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
#include "ffmpeg/api.h"
#include "sources/av_source.h"
#include "sources/container_supplier.h"
#include "sources/ffmpeg_rules.h"

namespace dvv::components {
namespace {

constexpr std::array<std::string_view, 1> SCHEMES{"file"};

}  // namespace

FfmpegSourceProvider::FfmpegSourceProvider(CreationKey<ComponentRegistry> key) noexcept
    : Managed(std::move(key)) {}

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
    co_return co_await detail::openAvSource(std::move(ticket), request.uri, context.io,
                                            context.stop, &detail::openContainer);
}

DVV_REGISTER(media::ISourceProvider, FfmpegSourceProvider);

}  // namespace dvv::components

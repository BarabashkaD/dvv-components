// Pure rules of the FFmpeg providers (M4 design v2 §4.4, D5, D28; v1.0 codec scope).

#include "sources/ffmpeg_rules.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/uri.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/rate_size_text.h"
#include "dvv/media/types.h"
#include "dvv/media/uri_colour.h"

namespace dvv::components::detail {
namespace {

constexpr std::uint64_t RING_BYTES = 256ULL << 20U;
constexpr std::size_t MAX_RING = 8;
// AVPROBE_SCORE_EXTENSION: a score above it comes from the content, not from the name.
constexpr int CONTENT_SCORE = 50;
constexpr int CONTENT_CONFIDENCE = 90;
constexpr std::array<std::string_view, 3> EXCLUDED_DEMUXERS{"rawvideo", "yuv4mpegpipe",
                                                            "image2pipe"};
constexpr std::array<std::string_view, 3> OWN_PARAMETERS{"fmt", "size", "fps"};
// Calls `visit` for every comma-separated item of `list` until it returns true.
template <class F>
bool anyItem(std::string_view list, F visit) {
    while (true) {
        const std::size_t comma = list.find(',');
        if (visit(list.substr(0, comma))) {
            return true;
        }
        if (comma == std::string_view::npos) {
            return false;
        }
        list.remove_prefix(comma + 1);
    }
}

bool sameFormat(std::string_view fmt, media::FormatId stream_format) {
    if (fmt.starts_with('@')) {
        return false;
    }
    if (const auto format = media::findFormat(fmt)) {
        return *format == stream_format;
    }
    if (const auto alias = media::formatAlias(fmt)) {
        return alias->format == stream_format;
    }
    return false;
}

}  // namespace

std::size_t ringCapacity(std::uint64_t frame_bytes) noexcept {
    const std::uint64_t frames = RING_BYTES / std::max<std::uint64_t>(frame_bytes, 1);
    return static_cast<std::size_t>(std::clamp<std::uint64_t>(frames, 1, MAX_RING));
}

Result<void> checkLosslessCodec(std::string_view codec_name) {
    if (std::ranges::find(LOSSLESS_CODECS, codec_name) != LOSSLESS_CODECS.end()) {
        return {};
    }
    return makeError(Errc::Unsupported,
                     std::format("codec {} is not supported (lossless codecs only)", codec_name));
}

Result<void> checkLavfiFilter(std::string_view filter_name) {
    if (std::ranges::find(LAVFI_ALLOWED_FILTERS, filter_name) != LAVFI_ALLOWED_FILTERS.end()) {
        return {};
    }
    // The file: hint only where it helps: a filter that reads a file.
    const bool reads_files =
        std::ranges::find(LAVFI_FILE_FILTERS, filter_name) != LAVFI_FILE_FILTERS.end();
    return makeError(Errc::Unsupported,
                     std::format("lavfi: filter {} is not supported{}", filter_name,
                                 reads_files ? "; open files with file: instead" : ""));
}

bool lavfiDeterministic(std::string_view graph) {
    std::string text;
    text.reserve(graph.size());
    for (const char c : graph) {
        if (c != '\'' && c != '\\') {
            text.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return std::ranges::none_of(std::array<std::string_view, 3>{"gradients", "random", "bikeshed"},
                                [&](std::string_view word) { return text.contains(word); });
}

bool sequentialMode(std::span<const std::int64_t> pts) {
    if (std::ranges::find(pts, AV_NOPTS_VALUE) != pts.end()) {
        return true;
    }
    std::vector<std::int64_t> sorted(pts.begin(), pts.end());
    std::ranges::sort(sorted);
    return std::ranges::adjacent_find(sorted) != sorted.end();
}

bool excludedDemuxer(std::string_view demuxer_name) noexcept {
    return anyItem(demuxer_name, [](std::string_view name) {
        return std::ranges::find(EXCLUDED_DEMUXERS, name) != EXCLUDED_DEMUXERS.end();
    });
}

int matchConfidence(int score, std::string_view demuxer_name) noexcept {
    return score > CONTENT_SCORE && !excludedDemuxer(demuxer_name) ? CONTENT_CONFIDENCE : 0;
}

Result<media::ColourSpec> checkContainerQuery(const Uri& uri, const ContainerInfo& facts) {
    for (const auto& [name, value] : uri.params) {
        const bool own = std::ranges::find(OWN_PARAMETERS, name) != OWN_PARAMETERS.end();
        const bool colour =
            std::ranges::find(media::COLOUR_PARAMETERS, name) != media::COLOUR_PARAMETERS.end();
        if (!own && !colour && name != "impl") {
            return makeError(Errc::Format, media::unknownParameterText(name));
        }
    }
    DVV_TRY(const media::ColourSpec colour, media::colourSpecFromQuery(uri));
    if (const auto fmt = uri.param("fmt"); fmt && !sameFormat(*fmt, facts.format)) {
        return makeError(Errc::Format, std::format("fmt is set by the container ({})",
                                                   media::formatDesc(facts.format).name));
    }
    if (const auto size = uri.param("size")) {
        const auto parsed = media::parseSize(*size);
        if (!parsed || parsed->first != facts.width || parsed->second != facts.height) {
            return makeError(Errc::Format, std::format("size is set by the container ({}x{})",
                                                       facts.width, facts.height));
        }
    }
    if (const auto fps = uri.param("fps")) {
        const media::Rational stream_fps = reduced(facts.fps);
        const auto parsed = media::parseFps(*fps);
        if (!parsed || *parsed != stream_fps) {
            return makeError(Errc::Format, std::format("fps is set by the container ({})",
                                                       media::formatFps(stream_fps)));
        }
    }
    return colour;
}

Result<media::ColourParams> overrideColour(const media::ColourParams& mapped,
                                           const media::ColourSpec& query,
                                           media::ColourModel model) {
    media::ColourParams colour = mapped;
    if (query.preset) {
        const media::ColourParams standard = media::merged(query, model);
        // A YUV-model stream tagged with the GBR matrix keeps it: its planes hold G, B and R
        // (design §3.3 rule 5), so only the curve and the primaries follow the standard.
        if (standard.matrix != media::ColourMatrix::Unspecified &&
            mapped.matrix != media::ColourMatrix::Rgb) {
            colour.matrix = standard.matrix;
            colour.kr = standard.kr;
            colour.kb = standard.kb;
        }
        colour.transfer = standard.transfer;
        colour.primaries = standard.primaries;
    }
    if (query.range != media::ColourRange::Unspecified) {
        colour.range = query.range;
    }
    DVV_TRY_VOID(media::validateColour(colour));
    return colour;
}

namespace {

// Whether code 0 is reserved (H.273: for the primaries and the transfer) or a valid code (the
// matrix: 0 is GBR).
enum class ZeroCode : std::uint8_t { Reserved, Valid };

// Unspecified and reserved codes (H.273: 3 is reserved for all three; 0 per `zero`) are not
// signalled.
bool signalled(int code, int unspecified, ZeroCode zero) noexcept {
    return code != unspecified && code != 3 && !(zero == ZeroCode::Reserved && code == 0);
}

template <typename Code>
Code firstSignalled(Code frame, Code stream, int unspecified, ZeroCode zero) noexcept {
    return signalled(static_cast<int>(frame), unspecified, zero) ? frame : stream;
}

}  // namespace

ColourTags mergeTags(const ColourTags& frame, const ColourTags& stream) noexcept {
    return ColourTags{
        .space = firstSignalled(frame.space, stream.space, AVCOL_SPC_UNSPECIFIED, ZeroCode::Valid),
        .range = frame.range != AVCOL_RANGE_UNSPECIFIED ? frame.range : stream.range,
        .transfer = firstSignalled(frame.transfer, stream.transfer, AVCOL_TRC_UNSPECIFIED,
                                   ZeroCode::Reserved),
        .primaries = firstSignalled(frame.primaries, stream.primaries, AVCOL_PRI_UNSPECIFIED,
                                    ZeroCode::Reserved)};
}

std::vector<std::string> tagConflicts(const ColourTags& frame, const ColourTags& stream) {
    std::vector<std::string> lines;
    const auto check = [&lines](std::string_view field, int frame_code, int stream_code,
                                int unspecified, ZeroCode zero) {
        if (signalled(frame_code, unspecified, zero) && signalled(stream_code, unspecified, zero) &&
            frame_code != stream_code) {
            lines.push_back(
                std::format("colour: the frame's {} {} differs from the stream's {} (the frame's "
                            "is used)",
                            field, frame_code, stream_code));
        }
    };
    check("matrix", frame.space, stream.space, AVCOL_SPC_UNSPECIFIED, ZeroCode::Valid);
    check("range", frame.range, stream.range, AVCOL_RANGE_UNSPECIFIED, ZeroCode::Valid);
    check("transfer", frame.transfer, stream.transfer, AVCOL_TRC_UNSPECIFIED, ZeroCode::Reserved);
    check("primaries", frame.primaries, stream.primaries, AVCOL_PRI_UNSPECIFIED,
          ZeroCode::Reserved);
    return lines;
}

std::string tagsText(const ColourTags& tags) {
    return std::format("P/T/M = {}/{}/{}", static_cast<int>(tags.primaries),
                       static_cast<int>(tags.transfer), static_cast<int>(tags.space));
}

std::string nonStandardNote(const ColourTags& tags) {
    return std::format(
        "colour: signalled but non-standard ({}); no standard, the given fields are used as they "
        "are",
        tagsText(tags));
}

media::ColourParams withBt601Hint(media::ColourParams params, media::Bt601Hint hint,
                                  media::ColourModel model) noexcept {
    if (model == media::ColourModel::Yuv && params.matrix == media::ColourMatrix::Bt601 &&
        params.primaries == media::Primaries::Unspecified) {
        if (hint == media::Bt601Hint::Pal) {
            params.primaries = media::Primaries::Bt601_625;
        } else if (hint == media::Bt601Hint::Ntsc) {
            params.primaries = media::Primaries::Bt601_525;
        }
    }
    return params;
}

MappedColour mapColour(const ColourTags& tags) noexcept {
    const auto [space, range, transfer, primaries] = tags;
    MappedColour mapped;
    media::ColourParams& colour = mapped.params;
    media::FileHint& hint = mapped.hint;
    switch (space) {
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: colour.matrix = media::ColourMatrix::Bt601; break;
        case AVCOL_SPC_BT709: colour.matrix = media::ColourMatrix::Bt709; break;
        case AVCOL_SPC_BT2020_NCL: colour.matrix = media::ColourMatrix::Bt2020Ncl; break;
        case AVCOL_SPC_RGB: colour.matrix = media::ColourMatrix::Rgb; break;
        case AVCOL_SPC_FCC:
        case AVCOL_SPC_SMPTE240M:
        case AVCOL_SPC_YCGCO: hint.nonStandard = true; break;
        default:
            hint.nonStandard = static_cast<int>(space) >= static_cast<int>(AVCOL_SPC_BT2020_CL);
    }
    switch (range) {
        case AVCOL_RANGE_MPEG: colour.range = media::ColourRange::Limited; break;
        case AVCOL_RANGE_JPEG: colour.range = media::ColourRange::Full; break;
        default: break;
    }
    switch (transfer) {
        case AVCOL_TRC_BT709:
        case AVCOL_TRC_GAMMA22:
        case AVCOL_TRC_GAMMA28:
        case AVCOL_TRC_SMPTE170M:
        case AVCOL_TRC_BT2020_10:
        case AVCOL_TRC_BT2020_12: colour.transfer = media::TransferFn::Bt1886; break;
        case AVCOL_TRC_IEC61966_2_1: colour.transfer = media::TransferFn::Srgb; break;
        case AVCOL_TRC_SMPTE2084: colour.transfer = media::TransferFn::Pq; break;
        case AVCOL_TRC_ARIB_STD_B67: colour.transfer = media::TransferFn::Hlg; break;
        case AVCOL_TRC_LINEAR: colour.transfer = media::TransferFn::Linear; break;
        case AVCOL_TRC_SMPTE240M:
        case AVCOL_TRC_LOG:
        case AVCOL_TRC_LOG_SQRT:
        case AVCOL_TRC_IEC61966_2_4:
        case AVCOL_TRC_BT1361_ECG:
        case AVCOL_TRC_SMPTE428: hint.nonStandard = true; break;
        default: break;
    }
    switch (primaries) {
        case AVCOL_PRI_BT709: colour.primaries = media::Primaries::Bt709; break;
        case AVCOL_PRI_BT470BG:
            colour.primaries = media::Primaries::Bt601_625;
            hint.bt601 = media::Bt601Hint::Pal;
            break;
        case AVCOL_PRI_SMPTE170M:
        case AVCOL_PRI_SMPTE240M:
            colour.primaries = media::Primaries::Bt601_525;
            hint.bt601 = media::Bt601Hint::Ntsc;
            break;
        case AVCOL_PRI_BT2020: colour.primaries = media::Primaries::Bt2020; break;
        case AVCOL_PRI_BT470M:
        case AVCOL_PRI_FILM:
        case AVCOL_PRI_SMPTE428:
        case AVCOL_PRI_SMPTE431:
        case AVCOL_PRI_SMPTE432:
        case AVCOL_PRI_EBU3213: hint.nonStandard = true; break;
        case AVCOL_PRI_UNSPECIFIED:
            // The matrix code is only a weak hint, used when the primaries are not given.
            if (space == AVCOL_SPC_BT470BG) {
                hint.bt601 = media::Bt601Hint::Pal;
            } else if (space == AVCOL_SPC_SMPTE170M) {
                hint.bt601 = media::Bt601Hint::Ntsc;
            }
            break;
        default: break;
    }
    return mapped;
}

std::optional<CatalogueFormat> catalogueFormat(std::string_view pix_fmt_name) {
    if (const auto format = media::findFormat(pix_fmt_name);
        format && std::ranges::find(media::catalogue(), *format) != media::catalogue().end()) {
        return CatalogueFormat{.format = *format, .fullRange = false};
    }
    if (const auto alias = media::formatAlias(pix_fmt_name)) {
        return CatalogueFormat{.format = alias->format, .fullRange = true};
    }
    return std::nullopt;
}

media::Rational reduced(media::Rational fps) noexcept {
    const std::int64_t divisor = std::gcd(fps.num, fps.den);
    if (divisor == 0) {
        return fps;
    }
    media::Rational result{.num = fps.num / divisor, .den = fps.den / divisor};
    if (result.den < 0) {
        result.num = -result.num;
        result.den = -result.den;
    }
    return result;
}

}  // namespace dvv::components::detail

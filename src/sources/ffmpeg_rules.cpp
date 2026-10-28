// Pure rules of the FFmpeg providers (M4 design v2 §4.4, D5, D28; v1.0 codec scope).

#include "sources/ffmpeg_rules.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/uri.h"
#include "dvv/media/pixel_format.h"
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
// I.F frame rates: at most this many digits after the dot (10^9 fits an int64 denominator with
// room for the integer part).
constexpr std::size_t MAX_FRACTION_DIGITS = 9;
constexpr std::uint64_t MAX_FPS_INTEGER = 1'000'000'000;

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

std::optional<std::uint64_t> digits(std::string_view text) {
    std::uint64_t value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();  // from_chars reads [begin, end) only
    const auto [ptr, error] = std::from_chars(begin, end, value);
    if (text.empty() || error != std::errc() || ptr != end) {
        return std::nullopt;
    }
    return value;
}

// WxH with both dimensions in 1..65535 (M2's size form).
std::optional<std::pair<std::uint32_t, std::uint32_t>> parseSize(std::string_view text) {
    const std::size_t x = text.find('x');
    if (x == std::string_view::npos) {
        return std::nullopt;
    }
    const auto width = digits(text.substr(0, x));
    const auto height = digits(text.substr(x + 1));
    constexpr std::uint64_t MAX_DIMENSION = 65535;
    if (!width || !height || *width == 0 || *height == 0 || *width > MAX_DIMENSION ||
        *height > MAX_DIMENSION) {
        return std::nullopt;
    }
    return std::pair{static_cast<std::uint32_t>(*width), static_cast<std::uint32_t>(*height)};
}

// N, N/D or I.F (M2's fps forms), reduced; nullopt for anything else or a zero value.
std::optional<media::Rational> parseFps(std::string_view text) {
    std::uint64_t num = 0;
    std::uint64_t den = 1;
    if (const std::size_t slash = text.find('/'); slash != std::string_view::npos) {
        const auto n = digits(text.substr(0, slash));
        const auto d = digits(text.substr(slash + 1));
        if (!n || !d) {
            return std::nullopt;
        }
        num = *n;
        den = *d;
    } else if (const std::size_t dot = text.find('.'); dot != std::string_view::npos) {
        const std::string_view fraction = text.substr(dot + 1);
        const auto i = digits(text.substr(0, dot));
        const auto f = digits(fraction);
        if (!i || !f || *i > MAX_FPS_INTEGER || fraction.size() > MAX_FRACTION_DIGITS) {
            return std::nullopt;
        }
        for (std::size_t digit = 0; digit < fraction.size(); ++digit) {
            den *= 10;
        }
        num = *i * den + *f;
    } else if (const auto n = digits(text)) {
        num = *n;
    } else {
        return std::nullopt;
    }
    constexpr auto MAX_INT64 = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (num == 0 || den == 0 || num > MAX_INT64 || den > MAX_INT64) {
        return std::nullopt;
    }
    return reduced({static_cast<std::int64_t>(num), static_cast<std::int64_t>(den)});
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

Result<media::ColourSpec> checkContainerQuery(const Uri& uri, const ContainerFacts& facts) {
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
                                                   media::descriptor(facts.format).name));
    }
    if (const auto size = uri.param("size")) {
        const auto parsed = parseSize(*size);
        if (!parsed || parsed->first != facts.width || parsed->second != facts.height) {
            return makeError(Errc::Format, std::format("size is set by the container ({}x{})",
                                                       facts.width, facts.height));
        }
    }
    if (const auto fps = uri.param("fps")) {
        const media::Rational stream_fps = reduced(facts.fps);
        const auto parsed = parseFps(*fps);
        if (!parsed || *parsed != stream_fps) {
            return makeError(Errc::Format,
                             std::format("fps is set by the container ({})", fpsText(stream_fps)));
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

// Unspecified and reserved codes (H.273: 0 is reserved for the primaries and the transfer, 3 for
// all three) are not signalled.
bool signalled(int code, int unspecified, bool zero_reserved) noexcept {
    return code != unspecified && code != 3 && !(zero_reserved && code == 0);
}

template <typename Code>
Code firstSignalled(Code frame, Code stream, int unspecified, bool zero_reserved) noexcept {
    return signalled(static_cast<int>(frame), unspecified, zero_reserved) ? frame : stream;
}

}  // namespace

ColourTags mergeTags(const ColourTags& frame, const ColourTags& stream) noexcept {
    return ColourTags{
        .space = firstSignalled(frame.space, stream.space, AVCOL_SPC_UNSPECIFIED, false),
        .range = frame.range != AVCOL_RANGE_UNSPECIFIED ? frame.range : stream.range,
        .transfer = firstSignalled(frame.transfer, stream.transfer, AVCOL_TRC_UNSPECIFIED, true),
        .primaries =
            firstSignalled(frame.primaries, stream.primaries, AVCOL_PRI_UNSPECIFIED, true)};
}

std::vector<std::string> tagConflicts(const ColourTags& frame, const ColourTags& stream) {
    std::vector<std::string> lines;
    const auto check = [&lines](std::string_view field, int frame_code, int stream_code,
                                int unspecified, bool zero_reserved) {
        if (signalled(frame_code, unspecified, zero_reserved) &&
            signalled(stream_code, unspecified, zero_reserved) && frame_code != stream_code) {
            lines.push_back(
                std::format("colour: the frame's {} {} differs from the stream's {} (the frame's "
                            "is used)",
                            field, frame_code, stream_code));
        }
    };
    check("matrix", frame.space, stream.space, AVCOL_SPC_UNSPECIFIED, false);
    check("range", frame.range, stream.range, AVCOL_RANGE_UNSPECIFIED, false);
    check("transfer", frame.transfer, stream.transfer, AVCOL_TRC_UNSPECIFIED, true);
    check("primaries", frame.primaries, stream.primaries, AVCOL_PRI_UNSPECIFIED, true);
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

MappedColour mapColour(AVColorSpace space, AVColorRange range,
                       AVColorTransferCharacteristic transfer,
                       AVColorPrimaries primaries) noexcept {
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

std::string fpsText(media::Rational fps) {
    if (fps.den == 1) {
        return std::format("{}", fps.num);
    }
    return std::format("{}/{}", fps.num, fps.den);
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

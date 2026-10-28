#pragma once

// Pure rules of the FFmpeg providers (M4 design v2 §4.4, D5, D28; v1.0 codec scope): no FFmpeg
// calls, unit-tested without FFmpeg. FFmpeg enum types come from the dvv_ffmpeg wrapper header.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/uri.h"
#include "dvv/media/colour_preset.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "dvv/media/uri_colour.h"
#include "ffmpeg/ffmpeg_headers.h"

namespace dvv::components::detail {

// D5: max(1, min(8, 256 MiB / frame_bytes)) frames; frame_bytes 0 counts as 1.
[[nodiscard]] std::size_t ringCapacity(std::uint64_t frame_bytes) noexcept;

// The v1.0 codec scope (binding): the lossless video codecs v1.0 decodes, by FFmpeg codec name
// (avcodec_get_name of the stream's codec_id). MPEG-family and other lossy codecs are out of scope.
inline constexpr std::array<std::string_view, 11> LOSSLESS_CODECS{
    "rawvideo", "v210",    "v410",     "r210", "ffv1", "huffyuv",
    "ffvhuff",  "utvideo", "magicyuv", "png",  "qtrle"};
// Ok when `codec_name` is in LOSSLESS_CODECS (exact match); else Errc::Unsupported
// "codec <codec_name> is not supported (lossless codecs only)".
[[nodiscard]] Result<void> checkLosslessCodec(std::string_view codec_name);

// The filters a lavfi graph may use (the result of the security review, replacing the
// earlier denylist of file filters, which could not be complete): video generators and simple
// processing filters of the pinned FFmpeg n8.1.3 build, none of which has an option that takes a
// path or URL (cellauto was dropped: its filename option reads a pattern file). drawtext is not on
// it (font files). So no file is read or decoded inside a graph and the codec allowlist cannot be
// bypassed. No filter on it drops frames: select, trim and fps were removed, because
// over an unbounded source they can drop every frame or an unbounded number of them, and one
// av_buffersink_get_frame call cannot be stopped. Every processing filter here gives one output
// frame per input frame (loop and tpad only add frames), so one pull costs one generated frame.
// scale's optional second input (ref), which joins two streams, is refused by GraphSupplier.
// clang-format off
inline constexpr std::array<std::string_view, 31> LAVFI_ALLOWED_FILTERS{
    // generators
    "testsrc", "testsrc2", "smptebars", "smptehdbars", "pal75bars", "pal100bars", "rgbtestsrc",
    "yuvtestsrc", "colorchart", "colorspectrum", "color", "gradients", "allrgb", "allyuv",
    "nullsrc", "mandelbrot",
    // processing
    "format", "scale", "setparams", "setsar", "setdar", "crop", "pad", "hflip", "vflip",
    "transpose", "null", "loop", "tpad", "drawbox", "drawgrid"};
// clang-format on
// Every filter of the pinned FFmpeg n8.1.3 tree with an option that takes a file, directory, URL,
// plugin, model, weights, font, text, LUT, shader or stats/log/dump path, by exact name (the 51
// names checked in Task 5b, qrencodesrc and qrencode, plus drawvg, nnedi, flite, asr, fieldhint
// and ocio found by a later review, plus xfade_opencl and paletteuse found by its
// follow-up check). Only for the refusal's hint: the allowlist above is the security check
// (checkLavfiFilter refuses every name off it), so this is the one list the hint uses and a name
// missing here is still refused, only without the hint.
// clang-format off
inline constexpr std::array<std::string_view, 61> LAVFI_FILE_FILTERS{
    // container and subtitle readers
    "movie", "amovie", "subtitles", "ass",
    // command, metadata and message files
    "sendcmd", "asendcmd", "zmq", "azmq", "metadata", "ametadata",
    // text and font files
    "drawtext", "qrencodesrc", "qrencode", "drawvg", "showcqt", "flite",
    // LUT, curve, colour and shader files
    "lut1d", "lut3d", "curves", "selectivecolor", "libplacebo", "program_opencl", "openclsrc",
    "ocio", "lensfun", "xfade_opencl",
    // plugins
    "frei0r", "frei0r_src", "ladspa", "lv2",
    // models and weights
    "arnndn", "dnn_classify", "dnn_detect", "dnn_processing", "sr", "derain", "nnedi", "whisper",
    "ocr", "libvmaf", "asr", "sofalizer",
    // stats, log and dump files
    "psnr", "ssim", "ssim360", "xpsnr", "vmafmotion", "signature", "vidstabdetect",
    "vidstabtransform", "deshake", "firequalizer", "loudnorm", "fsync", "fieldhint", "paletteuse",
    // image and pattern files
    "find_rect", "cover_rect", "removelogo", "cellauto", "life"};
// clang-format on
// Ok when `filter_name` (exact, case-sensitive) is in LAVFI_ALLOWED_FILTERS; else
// Errc::Unsupported "lavfi: filter <name> is not supported", with "; open files with file:
// instead" appended when the name is in LAVFI_FILE_FILTERS.
[[nodiscard]] Result<void> checkLavfiFilter(std::string_view filter_name);
// SourceInfo::deterministic of a lavfi graph (D6 "seeded generators only"): false when the text,
// lower-cased and without the quote and escape characters ' and \, contains "gradients" (a random
// seed and colours by default) or a random colour, "random" or "bikeshed" (av_parse_color).
// Conservative: an explicit seed or an expression's random() also count as random.
[[nodiscard]] bool lavfiDeterministic(std::string_view graph);

// D5 mode of the index scan: true (Sequential) when any pts is AV_NOPTS_VALUE or two pts are equal
// (any order); false (indexed) otherwise, also for an empty list.
[[nodiscard]] bool sequentialMode(std::span<const std::int64_t> pts);

// §4.4 match: 90 when score > 50 (AVPROBE_SCORE_EXTENSION) and the demuxer
// is not excluded; else 0. FFmpeg's extension match alone scores at most 50, so the provider claims
// a file only when FFmpeg's content probe recognises it (a raw *.dat stays with the raw provider).
// Excluded demuxer names (any of the comma-separated names): "rawvideo", "yuv4mpegpipe",
// "image2pipe" (raw and Y4M stay with their providers, spec §7.2).
[[nodiscard]] int matchConfidence(int score, std::string_view demuxer_name) noexcept;
// True when one of the comma-separated names of `demuxer_name` is excluded (see matchConfidence).
[[nodiscard]] bool excludedDemuxer(std::string_view demuxer_name) noexcept;

// What the container decides (D28); fps already reduced.
struct ContainerFacts {
    media::FormatId format = 0;
    std::uint32_t width = 0, height = 0;
    media::Rational fps{25, 1};
};
// D28 query check of an `ffmpeg` URI. Allowed names: fmt, size, fps, COLOUR_PARAMETERS (colour,
// range; D5R) and impl. Errc::Format: media::unknownParameterText(name) (the removed keys matrix,
// kr, kb, transfer and primaries name colour=<standard>); colourSpecFromQuery's errors; "fmt is set
// by the container (<stream format name>)" when fmt names another format (findFormat for catalogue
// names, formatAlias for yuvj*; an unknown name or "@<json>" also differs); "size is set by the
// container (<W>x<H>)" for another or unparsable WxH; "fps is set by the container (<N or N/D>)"
// for another or unparsable fps (N, N/D or I.F, compared as reduced rationals). Returns the query's
// colour as written (merged() gives the D28 override, whose Unspecified fields are not overridden;
// colourParameters() gives the resolved URI's colour keys).
[[nodiscard]] Result<media::ColourSpec> checkContainerQuery(const Uri& uri,
                                                            const ContainerFacts& facts);
// D5R §3.1: the query's standard (colour=) replaces the matrix (with kr/kb), the curve and the
// primaries of `mapped` (an RGB or gray model keeps the mapped matrix: the standard has none
// there, and a YUV model tagged with the GBR matrix keeps it, design §3.3 rule 5); the query's
// range (range=) replaces the range. Then validateColour (Errc::Format with its message).
[[nodiscard]] Result<media::ColourParams> overrideColour(const media::ColourParams& mapped,
                                                         const media::ColourSpec& query,
                                                         media::ColourModel model);

// The four colour codes (H.273 values as FFmpeg stores them) of a decoded frame or of a stream.
struct ColourTags {
    AVColorSpace space = AVCOL_SPC_UNSPECIFIED;
    AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
    AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
    bool operator==(const ColourTags&) const = default;
};
// Frame first, per field (D5R Task 0 rule 4: the frame carries e.g. the HLG ATC SEI): the frame's
// code when it is signalled, else the stream's. Unspecified and the reserved codes (0 for the
// primaries and the transfer, 3 for all three) count as not signalled.
[[nodiscard]] ColourTags mergeTags(const ColourTags& frame, const ColourTags& stream) noexcept;
// One log line per field that the frame and the stream both signal with different codes, e.g.
// "colour: the frame's transfer 18 differs from the stream's 1 (the frame's is used)".
[[nodiscard]] std::vector<std::string> tagConflicts(const ColourTags& frame,
                                                    const ColourTags& stream);
// "P/T/M = <primaries>/<transfer>/<matrix>", the codes as numbers.
[[nodiscard]] std::string tagsText(const ColourTags& tags);

// mapColour's result: the fields and what the codes say beyond them (D5R Task 0).
struct MappedColour {
    media::ColourParams params;
    media::FileHint hint;
    bool operator==(const MappedColour&) const = default;
};
// The binding colour mapping table (design v1 §4.4, aligned by D5R Task 0): matrix
// BT470BG/SMPTE170M -> Bt601, BT709 -> Bt709, BT2020_NCL -> Bt2020Ncl, RGB -> Rgb; MPEG -> Limited,
// JPEG -> Full; trc BT709/GAMMA22/GAMMA28/SMPTE170M/BT2020_10/BT2020_12 -> Bt1886 (the SDR curve),
// IEC61966_2_1 -> Srgb, SMPTE2084 -> Pq, ARIB_STD_B67 -> Hlg, LINEAR -> Linear; primaries BT709 ->
// Bt709, BT470BG -> Bt601_625, SMPTE170M/SMPTE240M -> Bt601_525, BT2020 -> Bt2020; anything else
// -> Unspecified.
// hint.bt601: primaries 5 -> Pal, 6/7 -> Ntsc; with unspecified primaries the matrix code is the
// weak hint (5 -> Pal, 6 -> Ntsc). hint.nonStandard ("signalled but non-standard", not
// "unspecified"): primaries 4/8/10/11/12/22, transfer 7/9/10/11/12/17, matrix 4/7/8/10 and above.
[[nodiscard]] MappedColour mapColour(AVColorSpace space, AVColorRange range,
                                     AVColorTransferCharacteristic transfer,
                                     AVColorPrimaries primaries) noexcept;
// The hint applied to the fields, because dvv-media's resolveColour() gets only the FrameDesc:
// on a YUV model with the BT.601 matrix and unspecified primaries, hint.bt601 fills the primaries
// (Pal -> Bt601_625, Ntsc -> Bt601_525), so standardOf() names the same BT.601 standard. Anything
// else is returned unchanged.
[[nodiscard]] media::ColourParams withBt601Hint(media::ColourParams params, media::Bt601Hint hint,
                                                media::ColourModel model) noexcept;
// The log line of a non-standard tag set: "colour: signalled but non-standard (P/T/M = x/y/z);
// no standard, the given fields are used as they are".
[[nodiscard]] std::string nonStandardNote(const ColourTags& tags);

// A decoder/graph output pix_fmt name -> catalogue format: a catalogue name (findFormat), or
// formatAlias (yuvj*: the yuv*p entry with fullRange = true). nullopt when neither knows the name.
struct CatalogueFormat {
    media::FormatId format = 0;
    bool fullRange = false;
};
[[nodiscard]] std::optional<CatalogueFormat> catalogueFormat(std::string_view pix_fmt_name);

// "N" when den == 1, else "N/D" (M2's resolved-URI form).
[[nodiscard]] std::string fpsText(media::Rational fps);
// num/den reduced by the gcd; den > 0 (0/0 is returned unchanged).
[[nodiscard]] media::Rational reduced(media::Rational fps) noexcept;

}  // namespace dvv::components::detail

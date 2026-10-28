// The pure rules of the FFmpeg providers (M4 design v2 §4.4, D5, D28), without FFmpeg.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/uri.h"
#include "dvv/media/colour_preset.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/types.h"
#include "dvv/media/uri_colour.h"
#include "sources/ffmpeg_rules.h"

namespace {

namespace media = dvv::media;
namespace detail = dvv::components::detail;

media::FormatId formatId(std::string_view name) {
    const auto format = media::findFormat(name);
    REQUIRE_MESSAGE(format.has_value(), std::string(name));
    return format.value_or(media::FormatId{});
}

// Every field set (partial designated initializers warn with clang's -Wextra).
media::ColourParams colour(media::ColourMatrix matrix, media::ColourRange range,
                           media::TransferFn transfer = media::TransferFn::Unspecified,
                           media::Primaries primaries = media::Primaries::Unspecified, float kr = 0,
                           float kb = 0) {
    return {.matrix = matrix,
            .range = range,
            .transfer = transfer,
            .primaries = primaries,
            .kr = kr,
            .kb = kb};
}

dvv::Result<media::ColourSpec> checkQuery(std::string_view query) {
    const auto uri = dvv::parseUri(std::string("file:x.mkv") + std::string(query));
    REQUIRE_MESSAGE(uri.has_value(), std::string(query));
    const detail::ContainerFacts facts{
        .format = formatId("yuv420p10le"), .width = 64, .height = 48, .fps = {25, 1}};
    return detail::checkContainerQuery(*uri, facts);
}

void checkQueryFails(std::string_view query, std::string_view text) {
    CAPTURE(query);
    const auto result = checkQuery(query);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == dvv::Errc::Format);
    CAPTURE(result.error().message);
    CHECK(result.error().message.find(text) != std::string::npos);
}

}  // namespace

TEST_SUITE("ffmpeg_rules") {
    TEST_CASE("ffmpeg rules: ring capacity") {
        CHECK(detail::ringCapacity(352ULL * 288 * 3 / 2) == 8);
        CHECK(detail::ringCapacity(0) == 8);
        CHECK(detail::ringCapacity(40ULL << 20U) == 6);
        CHECK(detail::ringCapacity(7680ULL * 4320 * 6) == 1);
        CHECK(detail::ringCapacity(256ULL << 20U) == 1);
    }

    TEST_CASE("ffmpeg rules: match confidence") {
        CHECK(detail::matchConfidence(60, "matroska,webm") == 90);
        CHECK(detail::matchConfidence(100, "avi") == 90);
        CHECK(detail::matchConfidence(51, "nut") == 90);
        // FFmpeg's extension-only scores (1, 24, 50) never claim a file (e.g. raw *.dat).
        CHECK(detail::matchConfidence(1, "luodat") == 0);
        CHECK(detail::matchConfidence(25, "image2") == 0);
        CHECK(detail::matchConfidence(50, "image2") == 0);
        CHECK(detail::matchConfidence(0, "image2") == 0);
        CHECK(detail::matchConfidence(100, "rawvideo") == 0);
        CHECK(detail::matchConfidence(100, "yuv4mpegpipe") == 0);
        CHECK(detail::matchConfidence(60, "image2pipe") == 0);
    }

    TEST_CASE("ffmpeg rules: container query") {
        {
            const auto parsed = checkQuery("?range=full");
            REQUIRE_MESSAGE(parsed.has_value(), parsed.error().describe());
            CHECK(*parsed ==
                  media::ColourSpec{.preset = std::nullopt, .range = media::ColourRange::Full});
        }
        {
            // colour= and range= (D5R §3.1): as written; range= with every standard.
            const auto parsed = checkQuery("?colour=bt2100-pq&range=full");
            REQUIRE_MESSAGE(parsed.has_value(), parsed.error().describe());
            CHECK(*parsed == media::ColourSpec{.preset = media::ColourPreset::Bt2100Pq,
                                               .range = media::ColourRange::Full});
            using Parameters = std::vector<std::pair<std::string, std::string>>;
            CHECK(media::colourParameters(*parsed) ==
                  Parameters{{"colour", "bt2100-pq"}, {"range", "full"}});
            const auto pal = checkQuery("?colour=bt601-pal&range=full");
            REQUIRE_MESSAGE(pal.has_value(), pal.error().describe());
            CHECK(*pal == media::ColourSpec{.preset = media::ColourPreset::Bt601Pal,
                                            .range = media::ColourRange::Full});
        }
        checkQueryFails("?colour=hdr10",
                        "bad colour 'hdr10' (bt601-ntsc, bt601-pal, bt709, bt2020, "
                        "bt2100-pq, bt2100-hlg)");
        checkQueryFails("?colour=jpeg", "bad colour 'jpeg'");
        checkQueryFails("?range=tv", "bad range 'tv' (full, limited)");
        // The removed keys (D5R §3.1) name colour=<standard>.
        for (const std::string_view key : {"matrix", "kr", "kb", "transfer", "primaries"}) {
            CAPTURE(key);
            const auto removed = checkQuery(std::string("?") + std::string(key) + "=bt709");
            REQUIRE_FALSE(removed.has_value());
            CHECK(removed.error().code == dvv::Errc::Format);
            CHECK(removed.error().message ==
                  std::string("unknown parameter '") + std::string(key) +
                      "' (set the colour with colour=<standard>: bt601-ntsc, bt601-pal, bt709, "
                      "bt2020, bt2100-pq, bt2100-hlg)");
        }
        for (const std::string_view query :
             {"?fmt=yuv420p10le&size=64x48&fps=25", "?fps=50/2", "?fps=25.0", "?impl=ffmpeg"}) {
            CAPTURE(query);
            const auto parsed = checkQuery(query);
            REQUIRE_MESSAGE(parsed.has_value(), parsed.error().describe());
            CHECK(*parsed == media::ColourSpec{});
        }
        checkQueryFails("?fmt=gray", "fmt is set by the container (yuv420p10le)");
        checkQueryFails("?fmt=@x.json", "fmt is set by the container");
        checkQueryFails("?size=32x24", "size is set by the container (64x48)");
        checkQueryFails("?size=64*48", "size is set by the container");
        checkQueryFails("?fps=30", "fps is set by the container (25)");
        {
            const auto unknown = checkQuery("?w=64");
            REQUIRE_FALSE(unknown.has_value());
            CHECK(unknown.error().code == dvv::Errc::Format);
            CHECK(unknown.error().message == "unknown parameter 'w'");
        }
        {
            // A yuvj alias names its yuv*p format (the range stays the container's or `range`).
            const auto uri = dvv::parseUri("file:x.mkv?fmt=yuvj420p");
            REQUIRE(uri.has_value());
            const detail::ContainerFacts facts{
                .format = formatId("yuv420p"), .width = 64, .height = 48, .fps = {25, 1}};
            const auto alias = detail::checkContainerQuery(*uri, facts);
            REQUIRE_MESSAGE(alias.has_value(), alias.error().describe());
            CHECK(*alias == media::ColourSpec{});
        }
    }

    TEST_CASE("ffmpeg rules: colour override") {
        using media::ColourMatrix;
        using media::ColourModel;
        using media::ColourRange;
        using media::Primaries;
        using media::TransferFn;
        const media::ColourParams mapped =
            colour(ColourMatrix::Bt709, ColourRange::Full, TransferFn::Bt1886, Primaries::Bt709);
        {
            // The standard replaces matrix, curve and primaries; the file's range stays.
            const auto result = detail::overrideColour(
                mapped, media::ColourSpec{.preset = media::ColourPreset::Bt2100Hlg, .range = {}},
                ColourModel::Yuv);
            REQUIRE(result.has_value());
            CHECK(*result == colour(ColourMatrix::Bt2020Ncl, ColourRange::Full, TransferFn::Hlg,
                                    Primaries::Bt2020));
        }
        {
            // range= replaces the range only.
            const auto result = detail::overrideColour(
                mapped, media::ColourSpec{.preset = std::nullopt, .range = ColourRange::Limited},
                ColourModel::Yuv);
            REQUIRE(result.has_value());
            media::ColourParams want = mapped;
            want.range = ColourRange::Limited;
            CHECK(*result == want);
        }
        {
            // A standard on an RGB format keeps the mapped matrix (the standard has none there).
            const media::ColourParams rgb = colour(ColourMatrix::Rgb, ColourRange::Unspecified);
            const auto result =
                detail::overrideColour(rgb,
                                       media::ColourSpec{.preset = media::ColourPreset::Bt601Ntsc,
                                                         .range = ColourRange::Limited},
                                       ColourModel::Rgb);
            REQUIRE(result.has_value());
            CHECK(*result == colour(ColourMatrix::Rgb, ColourRange::Limited, TransferFn::Bt1886,
                                    Primaries::Bt601_525));
        }
        {
            // A YUV-model stream tagged with the GBR matrix keeps it (design §3.3 rule 5): the
            // planes hold G, B and R, so a standard replaces only the curve and the primaries.
            const media::ColourParams gbr = colour(ColourMatrix::Rgb, ColourRange::Full);
            const auto result = detail::overrideColour(
                gbr, media::ColourSpec{.preset = media::ColourPreset::Bt2020, .range = {}},
                ColourModel::Yuv);
            REQUIRE(result.has_value());
            CHECK(*result == colour(ColourMatrix::Rgb, ColourRange::Full, TransferFn::Bt1886,
                                    Primaries::Bt2020));
        }
        {
            const auto result =
                detail::overrideColour(mapped, media::ColourSpec{}, ColourModel::Yuv);
            REQUIRE(result.has_value());
            CHECK(*result == mapped);
        }
    }

    TEST_CASE("ffmpeg rules: frame-first tags") {
        const detail::ColourTags frame{.space = AVCOL_SPC_UNSPECIFIED,
                                       .range = AVCOL_RANGE_UNSPECIFIED,
                                       .transfer = AVCOL_TRC_ARIB_STD_B67,
                                       .primaries = AVCOL_PRI_RESERVED0};
        const detail::ColourTags stream{.space = AVCOL_SPC_BT2020_NCL,
                                        .range = AVCOL_RANGE_MPEG,
                                        .transfer = AVCOL_TRC_BT2020_10,
                                        .primaries = AVCOL_PRI_BT2020};
        CHECK(detail::mergeTags(frame, stream) ==
              detail::ColourTags{.space = AVCOL_SPC_BT2020_NCL,
                                 .range = AVCOL_RANGE_MPEG,
                                 .transfer = AVCOL_TRC_ARIB_STD_B67,
                                 .primaries = AVCOL_PRI_BT2020});
        CHECK(detail::tagConflicts(frame, stream) ==
              std::vector<std::string>{
                  "colour: the frame's transfer 18 differs from the stream's 14 (the frame's is "
                  "used)"});
        CHECK(detail::tagConflicts(stream, stream).empty());
        CHECK(detail::mergeTags(detail::ColourTags{}, stream) == stream);
        CHECK(detail::tagsText(stream) == "P/T/M = 9/14/9");
        CHECK(detail::nonStandardNote(detail::ColourTags{.space = AVCOL_SPC_SMPTE240M,
                                                         .range = AVCOL_RANGE_UNSPECIFIED,
                                                         .transfer = AVCOL_TRC_SMPTE240M,
                                                         .primaries = AVCOL_PRI_SMPTE240M}) ==
              "colour: signalled but non-standard (P/T/M = 7/7/7); no standard, the given fields "
              "are used as they are");
    }

    TEST_CASE("ffmpeg rules: the PAL/NTSC hint and non-standard tags") {
        using media::Bt601Hint;
        const auto hint = [](AVColorSpace space, AVColorPrimaries primaries) {
            return detail::mapColour(space, AVCOL_RANGE_UNSPECIFIED, AVCOL_TRC_UNSPECIFIED,
                                     primaries)
                .hint;
        };
        const auto bt601 = [&](AVColorSpace space, AVColorPrimaries primaries) {
            return hint(space, primaries).bt601;
        };
        CHECK(bt601(AVCOL_SPC_UNSPECIFIED, AVCOL_PRI_BT470BG) == Bt601Hint::Pal);
        CHECK(bt601(AVCOL_SPC_UNSPECIFIED, AVCOL_PRI_SMPTE170M) == Bt601Hint::Ntsc);
        CHECK(bt601(AVCOL_SPC_UNSPECIFIED, AVCOL_PRI_SMPTE240M) == Bt601Hint::Ntsc);
        // The primaries choose; the matrix code is only a weak hint when they are unspecified.
        CHECK(bt601(AVCOL_SPC_SMPTE170M, AVCOL_PRI_BT470BG) == Bt601Hint::Pal);
        CHECK(bt601(AVCOL_SPC_BT470BG, AVCOL_PRI_SMPTE170M) == Bt601Hint::Ntsc);
        CHECK(bt601(AVCOL_SPC_SMPTE170M, AVCOL_PRI_UNSPECIFIED) == Bt601Hint::Ntsc);
        CHECK(bt601(AVCOL_SPC_BT470BG, AVCOL_PRI_UNSPECIFIED) == Bt601Hint::Pal);
        CHECK(bt601(AVCOL_SPC_BT470BG, AVCOL_PRI_BT709) == Bt601Hint::None);
        CHECK(bt601(AVCOL_SPC_BT709, AVCOL_PRI_UNSPECIFIED) == Bt601Hint::None);
        CHECK(bt601(AVCOL_SPC_UNSPECIFIED, AVCOL_PRI_UNSPECIFIED) == Bt601Hint::None);
        CHECK(hint(AVCOL_SPC_BT709, AVCOL_PRI_BT709) == media::FileHint{});

        const auto non_standard = [](AVColorSpace space, AVColorTransferCharacteristic transfer,
                                     AVColorPrimaries primaries) {
            return detail::mapColour(space, AVCOL_RANGE_UNSPECIFIED, transfer, primaries)
                .hint.nonStandard;
        };
        for (const int code : {4, 8, 10, 11, 12, 22}) {
            CAPTURE(code);
            CHECK(non_standard(AVCOL_SPC_UNSPECIFIED, AVCOL_TRC_UNSPECIFIED,
                               static_cast<AVColorPrimaries>(code)));
        }
        for (const int code : {7, 9, 10, 11, 12, 17}) {
            CAPTURE(code);
            CHECK(non_standard(AVCOL_SPC_UNSPECIFIED,
                               static_cast<AVColorTransferCharacteristic>(code),
                               AVCOL_PRI_UNSPECIFIED));
        }
        for (const int code : {4, 7, 8, 10, 11, 12, 13, 14}) {
            CAPTURE(code);
            CHECK(non_standard(static_cast<AVColorSpace>(code), AVCOL_TRC_UNSPECIFIED,
                               AVCOL_PRI_UNSPECIFIED));
        }
        // Standard, unspecified and reserved codes are not "signalled but non-standard".
        for (const int code : {0, 1, 2, 3, 5, 6, 9}) {
            CAPTURE(code);
            CHECK_FALSE(non_standard(AVCOL_SPC_UNSPECIFIED, AVCOL_TRC_UNSPECIFIED,
                                     static_cast<AVColorPrimaries>(code)));
        }
        for (const int code : {0, 1, 2, 3, 4, 5, 6, 8, 13, 14, 15, 16, 18}) {
            CAPTURE(code);
            CHECK_FALSE(non_standard(AVCOL_SPC_UNSPECIFIED,
                                     static_cast<AVColorTransferCharacteristic>(code),
                                     AVCOL_PRI_UNSPECIFIED));
        }
        for (const int code : {0, 1, 2, 3, 5, 6, 9}) {
            CAPTURE(code);
            CHECK_FALSE(non_standard(static_cast<AVColorSpace>(code), AVCOL_TRC_UNSPECIFIED,
                                     AVCOL_PRI_UNSPECIFIED));
        }

        // The hint applied to the fields (YUV, BT.601 matrix, no primaries only).
        using media::ColourMatrix;
        using media::ColourModel;
        using media::Primaries;
        const media::ColourParams ntsc =
            detail::mapColour(AVCOL_SPC_SMPTE170M, AVCOL_RANGE_UNSPECIFIED, AVCOL_TRC_UNSPECIFIED,
                              AVCOL_PRI_UNSPECIFIED)
                .params;
        CHECK(detail::withBt601Hint(ntsc, Bt601Hint::Ntsc, ColourModel::Yuv).primaries ==
              Primaries::Bt601_525);
        CHECK(detail::withBt601Hint(ntsc, Bt601Hint::Pal, ColourModel::Yuv).primaries ==
              Primaries::Bt601_625);
        CHECK(detail::withBt601Hint(ntsc, Bt601Hint::None, ColourModel::Yuv) == ntsc);
        CHECK(detail::withBt601Hint(ntsc, Bt601Hint::Ntsc, ColourModel::Rgb) == ntsc);
        media::ColourParams tagged = ntsc;
        tagged.primaries = Primaries::Bt709;
        CHECK(detail::withBt601Hint(tagged, Bt601Hint::Ntsc, ColourModel::Yuv) == tagged);
        media::ColourParams bt709 = ntsc;
        bt709.matrix = ColourMatrix::Bt709;
        CHECK(detail::withBt601Hint(bt709, Bt601Hint::Ntsc, ColourModel::Yuv) == bt709);
        // With the hint applied, dvv-media names the NTSC standard (no FileHint needed).
        CHECK(media::standardOf(detail::withBt601Hint(ntsc, Bt601Hint::Ntsc, ColourModel::Yuv),
                                ColourModel::Yuv,
                                media::FileHint{}) == media::ColourPreset::Bt601Ntsc);
    }

    TEST_CASE("ffmpeg rules: colour mapping table") {
        const auto matrix = [](AVColorSpace space) {
            return detail::mapColour(space, AVCOL_RANGE_UNSPECIFIED, AVCOL_TRC_UNSPECIFIED,
                                     AVCOL_PRI_UNSPECIFIED)
                .params.matrix;
        };
        const auto range = [](AVColorRange value) {
            return detail::mapColour(AVCOL_SPC_UNSPECIFIED, value, AVCOL_TRC_UNSPECIFIED,
                                     AVCOL_PRI_UNSPECIFIED)
                .params.range;
        };
        const auto transfer = [](AVColorTransferCharacteristic value) {
            return detail::mapColour(AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_UNSPECIFIED, value,
                                     AVCOL_PRI_UNSPECIFIED)
                .params.transfer;
        };
        const auto primaries = [](AVColorPrimaries value) {
            return detail::mapColour(AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_UNSPECIFIED,
                                     AVCOL_TRC_UNSPECIFIED, value)
                .params.primaries;
        };
        using media::ColourMatrix;
        using media::ColourRange;
        using media::Primaries;
        using media::TransferFn;
        CHECK(matrix(AVCOL_SPC_BT470BG) == ColourMatrix::Bt601);
        CHECK(matrix(AVCOL_SPC_SMPTE170M) == ColourMatrix::Bt601);
        CHECK(matrix(AVCOL_SPC_BT709) == ColourMatrix::Bt709);
        CHECK(matrix(AVCOL_SPC_BT2020_NCL) == ColourMatrix::Bt2020Ncl);
        CHECK(matrix(AVCOL_SPC_RGB) == ColourMatrix::Rgb);
        CHECK(range(AVCOL_RANGE_MPEG) == ColourRange::Limited);
        CHECK(range(AVCOL_RANGE_JPEG) == ColourRange::Full);
        CHECK(transfer(AVCOL_TRC_BT709) == TransferFn::Bt1886);
        CHECK(transfer(AVCOL_TRC_SMPTE170M) == TransferFn::Bt1886);
        CHECK(transfer(AVCOL_TRC_BT2020_10) == TransferFn::Bt1886);
        CHECK(transfer(AVCOL_TRC_BT2020_12) == TransferFn::Bt1886);
        CHECK(transfer(AVCOL_TRC_IEC61966_2_1) == TransferFn::Srgb);
        CHECK(transfer(AVCOL_TRC_SMPTE2084) == TransferFn::Pq);
        CHECK(transfer(AVCOL_TRC_ARIB_STD_B67) == TransferFn::Hlg);
        CHECK(transfer(AVCOL_TRC_LINEAR) == TransferFn::Linear);
        CHECK(primaries(AVCOL_PRI_BT709) == Primaries::Bt709);
        CHECK(primaries(AVCOL_PRI_BT470BG) == Primaries::Bt601_625);
        CHECK(primaries(AVCOL_PRI_SMPTE170M) == Primaries::Bt601_525);
        CHECK(primaries(AVCOL_PRI_BT2020) == Primaries::Bt2020);

        CHECK(matrix(AVCOL_SPC_FCC) == ColourMatrix::Unspecified);
        CHECK(range(AVCOL_RANGE_UNSPECIFIED) == ColourRange::Unspecified);
        CHECK(transfer(AVCOL_TRC_GAMMA22) == TransferFn::Bt1886);
        CHECK(transfer(AVCOL_TRC_GAMMA28) == TransferFn::Bt1886);
        CHECK(transfer(AVCOL_TRC_SMPTE240M) == TransferFn::Unspecified);
        CHECK(primaries(AVCOL_PRI_SMPTE240M) == Primaries::Bt601_525);
        CHECK(primaries(AVCOL_PRI_BT470M) == Primaries::Unspecified);
        const media::ColourParams all =
            detail::mapColour(AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, AVCOL_TRC_BT709, AVCOL_PRI_BT709)
                .params;
        CHECK(all == colour(ColourMatrix::Bt709, ColourRange::Limited, TransferFn::Bt1886,
                            Primaries::Bt709));
    }

    TEST_CASE("ffmpeg rules: catalogue format") {
        const auto yuv420p = detail::catalogueFormat("yuv420p");
        REQUIRE(yuv420p.has_value());
        CHECK(yuv420p.value_or(detail::CatalogueFormat{}).format == formatId("yuv420p"));
        CHECK_FALSE(yuv420p.value_or(detail::CatalogueFormat{}).fullRange);
        const auto yuvj420p = detail::catalogueFormat("yuvj420p");
        REQUIRE(yuvj420p.has_value());
        CHECK(yuvj420p.value_or(detail::CatalogueFormat{}).format == formatId("yuv420p"));
        CHECK(yuvj420p.value_or(detail::CatalogueFormat{}).fullRange);
        const auto yuvj444p = detail::catalogueFormat("yuvj444p");
        REQUIRE(yuvj444p.has_value());
        CHECK(yuvj444p.value_or(detail::CatalogueFormat{}).format == formatId("yuv444p"));
        CHECK(yuvj444p.value_or(detail::CatalogueFormat{}).fullRange);
        const auto gbrp10le = detail::catalogueFormat("gbrp10le");
        REQUIRE(gbrp10le.has_value());
        CHECK(gbrp10le.value_or(detail::CatalogueFormat{}).format == formatId("gbrp10le"));
        CHECK_FALSE(detail::catalogueFormat("bgr0").has_value());
    }

    TEST_CASE("ffmpeg rules: fps text") {
        CHECK(detail::fpsText({25, 1}) == "25");
        CHECK(detail::fpsText({30000, 1001}) == "30000/1001");
        CHECK(detail::reduced({50, 2}) == media::Rational{25, 1});
        CHECK(detail::reduced({60000, 2002}) == media::Rational{30000, 1001});
    }

    TEST_CASE("ffmpeg rules: lossless codec allowlist") {
        for (const std::string_view codec : detail::LOSSLESS_CODECS) {
            CAPTURE(codec);
            CHECK(detail::checkLosslessCodec(codec).has_value());
        }
        for (const std::string_view codec : {"h264", "hevc", "mpeg1video", "mpeg2video", "mpeg4",
                                             "av1", "mjpeg", "prores", "FFV1", ""}) {
            CAPTURE(codec);
            const auto result = detail::checkLosslessCodec(codec);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == dvv::Errc::Unsupported);
        }
        const auto h264 = detail::checkLosslessCodec("h264");
        REQUIRE_FALSE(h264.has_value());
        CHECK(h264.error().message.find("codec h264 is not supported (lossless codecs only)") !=
              std::string::npos);
    }

    TEST_CASE("lavfi filter allowlist") {
        for (const std::string_view name : detail::LAVFI_ALLOWED_FILTERS) {
            CAPTURE(name);
            REQUIRE_FALSE(name.empty());
            CHECK(detail::checkLavfiFilter(name).has_value());
        }
        // Filters that read files or URLs (movie, amovie, sendcmd, qrencodesrc, drawtext, the
        // dropped cellauto, ...) are refused with the hint to open the file as a file: URI.
        CHECK_FALSE(detail::LAVFI_FILE_FILTERS.empty());
        for (const std::string_view name : detail::LAVFI_FILE_FILTERS) {
            CAPTURE(name);
            CHECK(std::ranges::find(detail::LAVFI_ALLOWED_FILTERS, name) ==
                  detail::LAVFI_ALLOWED_FILTERS.end());
            const auto result = detail::checkLavfiFilter(name);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == dvv::Errc::Unsupported);
            CHECK(result.error().message == "lavfi: filter " + std::string(name) +
                                                " is not supported; open files with file: "
                                                "instead");
        }
        // Every file-reading filter of the pinned FFmpeg n8.1.3 build, pinned by family (a
        // trimmed list lost the hint for 29 names). Each name must get the hint, and the list
        // must hold exactly these names, so dropping or adding one fails here.
        // clang-format off
        constexpr std::array<std::string_view, 61> PINNED_FILE_FILTERS{
            // container and subtitle readers
            "movie", "amovie", "subtitles", "ass",
            // command, metadata and message files
            "sendcmd", "asendcmd", "zmq", "azmq", "metadata", "ametadata",
            // text and font files
            "drawtext", "qrencodesrc", "qrencode", "drawvg", "showcqt", "flite",
            // LUT, curve, colour and shader files
            "lut1d", "lut3d", "curves", "selectivecolor", "libplacebo", "program_opencl",
            "openclsrc", "ocio", "lensfun", "xfade_opencl",
            // plugins
            "frei0r", "frei0r_src", "ladspa", "lv2",
            // models and weights
            "arnndn", "dnn_classify", "dnn_detect", "dnn_processing", "sr", "derain", "nnedi",
            "whisper", "ocr", "libvmaf", "asr", "sofalizer",
            // stats, log and dump files
            "psnr", "ssim", "ssim360", "xpsnr", "vmafmotion", "signature", "vidstabdetect",
            "vidstabtransform", "deshake", "firequalizer", "loudnorm", "fsync", "fieldhint",
            "paletteuse",
            // image and pattern files
            "find_rect", "cover_rect", "removelogo", "cellauto", "life"};
        // clang-format on
        CHECK(detail::LAVFI_FILE_FILTERS.size() == PINNED_FILE_FILTERS.size());
        for (const std::string_view name : PINNED_FILE_FILTERS) {
            CAPTURE(name);
            CHECK(std::ranges::count(PINNED_FILE_FILTERS, name) == 1);
            CHECK(std::ranges::find(detail::LAVFI_FILE_FILTERS, name) !=
                  detail::LAVFI_FILE_FILTERS.end());
            const auto result = detail::checkLavfiFilter(name);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().message == "lavfi: filter " + std::string(name) +
                                                " is not supported; open files with file: "
                                                "instead");
        }
        // Audio, filters that can drop every frame of an unbounded graph (select, trim, fps:
        // the pull could not be stopped), case and the empty name are refused without
        // the file: hint (none of them reads a file).
        for (const std::string_view name :
             {"sine", "select", "trim", "fps", "Testsrc2", "TESTSRC2", "Movie", ""}) {
            CAPTURE(name);
            const auto result = detail::checkLavfiFilter(name);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == dvv::Errc::Unsupported);
            CHECK(result.error().message ==
                  "lavfi: filter " + std::string(name) + " is not supported");
        }
    }
    TEST_CASE("lavfi graph determinism") {
        CHECK(detail::lavfiDeterministic("testsrc2=s=64x48:r=25,format=yuv420p"));
        CHECK(detail::lavfiDeterministic("mandelbrot=s=64x48,drawbox=c=red"));
        // gradients picks a random seed and colours; any colour option takes "random" or
        // "bikeshed" (av_parse_color, any case, optional @alpha); quotes and escapes are removed
        // first. Conservative: gradients with a seed counts as random too.
        for (const std::string_view graph :
             {"gradients=s=64x48", "gradients=s=64x48:seed=1", "'grad''ients'=s=64x48",
              "color=c=random", "color=c=Random@0.5", "testsrc2,drawbox=c=BIKESHED",
              "testsrc2,pad=w=80:color='ran''dom'", "testsrc2,tpad=start=2:color=ran\\dom"}) {
            CAPTURE(graph);
            CHECK_FALSE(detail::lavfiDeterministic(graph));
        }
    }
    TEST_CASE("ffmpeg rules: sequential mode") {
        const auto mode = [](std::vector<std::int64_t> pts) { return detail::sequentialMode(pts); };
        CHECK_FALSE(mode({0, 1, 2}));
        CHECK_FALSE(mode({}));
        CHECK_FALSE(mode({2, 0, 1}));
        CHECK(mode({0, AV_NOPTS_VALUE, 2}));
        CHECK(mode({0, 0, 1, 1}));
        CHECK(mode({3, 1, 3}));
    }
}

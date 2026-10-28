// The FFmpeg oracle for display colour (M4 design v2 §8.3, E2): toDisplayReference with
// encodeSrgb8 stays within 1 code value of swscale's accurate_rnd+full_chroma_int+bitexact rgb24.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include "dvv/media/display.h"
#include "dvv/media/pixel_format.h"
#include "dvv/media/reference.h"
#include "dvv/media/types.h"
#include "oracle/oracle_harness.h"

namespace {

namespace media = dvv::media;
namespace oracle = dvv::components::oracle;

constexpr int TOLERANCE = 1;

media::ColourMatrix matrixOf(const std::string& name) {
    if (name == "bt601") {
        return media::ColourMatrix::Bt601;
    }
    if (name == "bt709") {
        return media::ColourMatrix::Bt709;
    }
    REQUIRE_MESSAGE(name == "-", "unknown matrix " << name);
    return media::ColourMatrix::Rgb;
}

media::ColourRange rangeOf(const std::string& name) {
    if (name == "limited") {
        return media::ColourRange::Limited;
    }
    REQUIRE_MESSAGE(name == "full", "unknown range " << name);
    return media::ColourRange::Full;
}

// One display line: every pixel of every frame of the format's clip against swscale's rgb24.
void compareDisplay(oracle::OracleHarness& harness, const oracle::OracleLine& line,
                    const oracle::OracleLine& clip) {
    const auto found = media::findFormat(line.format);
    REQUIRE_MESSAGE(found.has_value(), "unknown format " << line.format);
    const media::FormatId format = found.value_or(media::FormatId{});
    const std::size_t components =
        std::min<std::size_t>(media::descriptor(format).componentCount, 3);
    const media::ColourParams params{.matrix = matrixOf(line.matrix),
                                     .range = rangeOf(line.range),
                                     .transfer = media::TransferFn::Srgb,
                                     .primaries = media::Primaries::Bt709};
    const std::vector<std::byte> rgb = oracle::readOracleFile(oracle::oracleFile(line.file));
    const std::size_t pixels = std::size_t{clip.width} * clip.height;
    REQUIRE_MESSAGE(rgb.size() == pixels * 3 * clip.frames,
                    "display reference " << line.file << " has " << rgb.size()
                                         << " bytes, expected " << pixels * 3 * clip.frames);

    const std::string uri = oracle::clipUri(clip);
    std::size_t misses = 0;
    int largest = 0;
    std::string first_miss;
    for (std::uint32_t frame = 0; frame < clip.frames; ++frame) {
        const auto original = harness.originalBytes(uri, frame);
        REQUIRE_MESSAGE(original.has_value(), original.error().describe());
        const auto codes = media::unpack(format, clip.width, clip.height, *original);
        REQUIRE_MESSAGE(codes.has_value(), codes.error().describe());
        for (std::size_t c = 0; c < components; ++c) {
            REQUIRE(codes->components[c].size() == pixels);
        }
        const std::byte* reference = rgb.data() + pixels * 3 * frame;
        std::array<std::uint16_t, 3> values{};
        for (std::size_t i = 0; i < pixels; ++i) {
            for (std::size_t c = 0; c < components; ++c) {
                values[c] = codes->components[c][i];
            }
            const std::array<float, 4> linear = media::toDisplayReference(
                format, params, std::span<const std::uint16_t>(values.data(), components));
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const int ours = media::encodeSrgb8(linear[channel]);
                const int theirs = std::to_integer<int>(reference[i * 3 + channel]);
                const int difference = std::abs(ours - theirs);
                largest = std::max(largest, difference);
                if (difference <= TOLERANCE) {
                    continue;
                }
                if (misses++ == 0) {
                    first_miss = "frame " + std::to_string(frame) + " x " +
                                 std::to_string(i % clip.width) + " y " +
                                 std::to_string(i / clip.width) + " channel " +
                                 std::to_string(channel) + ": ours " + std::to_string(ours) +
                                 ", swscale " + std::to_string(theirs);
                }
            }
        }
    }
    CHECK_MESSAGE(misses == 0, line.file << ": " << misses << " channel values differ by more than "
                                         << TOLERANCE << "; first: " << first_miss
                                         << "; largest difference " << largest);
    MESSAGE(line.file << ": largest difference " << largest);
}

}  // namespace

TEST_SUITE("oracle") {
    TEST_CASE("oracle: display colour within 1 of swscale") {
        const std::vector<oracle::OracleLine> lines = oracle::readManifest();
        std::size_t count = 0;
        for (const auto& line : lines) {
            if (line.kind != "display") {
                continue;
            }
            ++count;
            const std::string name = line.format + " " + line.matrix + " " + line.range;
            SUBCASE(name.c_str()) {
                const auto clip = std::ranges::find_if(lines, [&](const oracle::OracleLine& other) {
                    return other.kind == "unpack" && other.format == line.format;
                });
                REQUIRE_MESSAGE(clip != lines.end(), "no clip for " << line.format);
                oracle::OracleHarness harness;
                compareDisplay(harness, line, *clip);
            }
        }
        CHECK(count == 11);
    }
}

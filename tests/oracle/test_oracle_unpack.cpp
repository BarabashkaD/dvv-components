// The FFmpeg oracle for unpack (M4 design v2 §8.3, E2): for every catalogue format, our CPU unpack
// (unpackToPlanes) equals FFmpeg's extractplanes bit for bit.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/media/pixel_format.h"
#include "dvv/media/reference.h"
#include "oracle/oracle_harness.h"

namespace {

namespace media = dvv::media;
namespace oracle = dvv::components::oracle;

// Catalogue formats the oracle cannot check, each with its reason (design R2); empty: every
// catalogue format is covered.
constexpr std::array<std::pair<std::string_view, std::string_view>, 0> NOT_COVERED{};

char letterOf(media::ComponentKind kind) {
    switch (kind) {
        case media::ComponentKind::Y: return 'y';
        case media::ComponentKind::U: return 'u';
        case media::ComponentKind::V: return 'v';
        case media::ComponentKind::R: return 'r';
        case media::ComponentKind::G: return 'g';
        case media::ComponentKind::B: return 'b';
        case media::ComponentKind::A: return 'a';
    }
    return '?';
}

std::string joined(const std::set<std::string>& names) {
    std::string text;
    for (const auto& name : names) {
        text += text.empty() ? name : " " + name;
    }
    return text;
}

// The value of one sample of the unpacked planes (LE 8- or 16-bit).
std::uint32_t sampleAt(std::span<const std::byte> bytes, std::size_t offset,
                       std::size_t sample_bytes) {
    std::uint32_t value = std::to_integer<std::uint32_t>(bytes[offset]);
    if (sample_bytes == 2) {
        value |= std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U;
    }
    return value;
}

// The fixture guard: a component deeper than 8 bits is only checked bit for bit
// when every bit position below the top 8 is set in some sample of its reference (each
// position, not just one). testsrc2 alone draws 8-bit colours shifted to the
// depth, so without this guard a broken low bit could pass unseen.
void checkLowBits(const oracle::OracleLine& line, std::size_t k, unsigned bits,
                  std::span<const std::byte> reference) {
    if (bits <= 8) {
        return;
    }
    const std::uint32_t low_mask = (std::uint32_t{1} << (bits - 8U)) - 1U;
    std::uint32_t seen = 0;
    for (std::size_t offset = 0; offset + 1 < reference.size() && seen != low_mask; offset += 2) {
        seen |= sampleAt(reference, offset, 2) & low_mask;
    }
    CHECK_MESSAGE(seen == low_mask,
                  line.format << " component " << k << " (" << bits << " bits): low bits 0x"
                              << std::hex << (low_mask & ~seen) << std::dec
                              << " are zero in every reference sample, so the fixture cannot "
                                 "check them");
}

// One unpack line: every frame of the clip against the extractplanes planes.
void compareUnpack(oracle::OracleHarness& harness, const oracle::OracleLine& line) {
    const auto found = media::findFormat(line.format);
    REQUIRE_MESSAGE(found.has_value(), "unknown format " << line.format);
    const media::FormatId format = found.value_or(media::FormatId{});
    const media::PixelFormatDescriptor& desc = media::descriptor(format);
    const auto geometry = media::geometry(media::unpackedFormat(format), line.width, line.height);
    REQUIRE_MESSAGE(geometry.has_value(), geometry.error().describe());
    REQUIRE(geometry->planeCount == desc.componentCount);
    REQUIRE_MESSAGE(line.letters.size() == desc.componentCount,
                    line.format << ": manifest letters " << line.letters);

    // The reference planes in our component order, found by component kind.
    std::vector<std::vector<std::byte>> references;
    std::vector<std::uint64_t> slices;
    for (std::size_t k = 0; k < desc.componentCount; ++k) {
        const char letter = letterOf(desc.components[k].kind);
        REQUIRE_MESSAGE(line.letters.find(letter) != std::string::npos,
                        line.format << ": no reference plane for component " << k);
        const std::string file = line.format + "_" + letter + ".raw";
        std::vector<std::byte> bytes = oracle::readOracleFile(oracle::oracleFile(file));
        const media::PlaneGeometry& plane = geometry->planes[k];
        const std::uint64_t slice = std::uint64_t{plane.stride} * plane.rows;
        const std::uint64_t expected = slice * line.frames;
        REQUIRE_MESSAGE(bytes.size() == expected, "reference plane "
                                                      << file << " has " << bytes.size()
                                                      << " bytes, expected " << expected);
        checkLowBits(line, k, desc.components[k].bits, bytes);
        references.push_back(std::move(bytes));
        slices.push_back(slice);
    }

    const std::string uri = oracle::clipUri(line);
    std::vector<std::byte> ours(geometry->bytes);
    std::vector<std::byte> reference;
    reference.reserve(geometry->bytes);
    for (std::uint32_t frame = 0; frame < line.frames; ++frame) {
        const auto original = harness.originalBytes(uri, frame);
        REQUIRE_MESSAGE(original.has_value(), original.error().describe());
        // A sample unpackToPlanes leaves unwritten must not keep the previous frame's value.
        std::ranges::fill(ours, std::byte{0xA5});
        const auto unpacked =
            media::unpackToPlanes(format, line.width, line.height, *original, ours);
        REQUIRE_MESSAGE(unpacked.has_value(), unpacked.error().describe());
        reference.clear();
        for (std::size_t k = 0; k < references.size(); ++k) {
            const auto first =
                references[k].begin() + static_cast<std::ptrdiff_t>(slices[k] * frame);
            reference.insert(reference.end(), first,
                             first + static_cast<std::ptrdiff_t>(slices[k]));
        }
        REQUIRE(reference.size() == ours.size());
        if (reference == ours) {
            continue;
        }
        // The first difference: its component, position and both values.
        std::size_t offset = 0;
        while (ours[offset] == reference[offset]) {
            ++offset;
        }
        std::size_t k = 0;
        while (k + 1 < desc.componentCount && offset >= geometry->planes[k + 1].offset) {
            ++k;
        }
        const media::PlaneGeometry& plane = geometry->planes[k];
        const std::uint32_t component_width =
            media::componentSize(format, k, line.width, line.height).first;
        const std::size_t sample_bytes = plane.rowBytes / component_width;
        const std::size_t inside = offset - plane.offset;
        const std::size_t y = inside / plane.stride;
        const std::size_t x = (inside % plane.stride) / sample_bytes;
        const std::size_t sample = plane.offset + y * plane.stride + x * sample_bytes;
        FAIL_CHECK(line.format << " frame " << frame << " component " << k << " x " << x << " y "
                               << y << ": ours " << sampleAt(ours, sample, sample_bytes)
                               << ", extractplanes " << sampleAt(reference, sample, sample_bytes));
        return;
    }
}

}  // namespace

TEST_SUITE("oracle") {
    TEST_CASE("oracle: every catalogue format is covered or listed") {
        std::set<std::string> listed;
        for (const auto& line : oracle::readManifest()) {
            if (line.kind == "unpack") {
                CHECK_MESSAGE(listed.insert(line.format).second, "listed twice: " << line.format);
            }
        }
        for (const auto& [name, reason] : NOT_COVERED) {
            CHECK_MESSAGE(!reason.empty(), "no reason for " << name);
            CHECK_MESSAGE(listed.insert(std::string(name)).second,
                          "covered and not covered: " << name);
        }
        std::set<std::string> names;
        for (const media::FormatId format : media::catalogue()) {
            names.insert(media::descriptor(format).name);
        }
        CHECK(names.size() == 37);
        std::set<std::string> missing;
        std::set<std::string> extra;
        for (const auto& name : names) {
            if (!listed.contains(name)) {
                missing.insert(name);
            }
        }
        for (const auto& name : listed) {
            if (!names.contains(name)) {
                extra.insert(name);
            }
        }
        CHECK_MESSAGE(missing.empty(), "neither covered nor listed: " << joined(missing));
        CHECK_MESSAGE(extra.empty(), "not in the catalogue: " << joined(extra));
    }

    TEST_CASE("oracle: unpack equals extractplanes") {
        const std::vector<oracle::OracleLine> lines = oracle::readManifest();
        std::size_t count = 0;
        for (const auto& line : lines) {
            if (line.kind != "unpack") {
                continue;
            }
            ++count;
            SUBCASE(line.format.c_str()) {
                oracle::OracleHarness harness;
                compareUnpack(harness, line);
            }
        }
        CHECK(count > 0);
    }
}

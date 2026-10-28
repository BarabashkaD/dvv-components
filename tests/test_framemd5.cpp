// The framemd5 parser of the FFmpeg oracle (M4 design v2 §8.1), without FFmpeg.

#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "dvv/base/error.h"
#include "framemd5.h"

namespace {

using dvv::components::testing::parseFramemd5;

constexpr std::string_view HEADER =
    "#format: frame checksums\n"
    "#version: 2\n"
    "#hash: MD5\n"
    "#tb 0: 1/25\n"
    "#media_type 0: video\n"
    "#codec_id 0: rawvideo\n"
    "#dimensions 0: 352x288\n"
    "#sar 0: 1/1\n"
    "#stream#, dts,        pts, duration,     size, hash\n";

void checkFormatError(std::string_view text) {
    const auto parsed = parseFramemd5(text);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == dvv::Errc::Format);
}

}  // namespace

TEST_SUITE("framemd5") {
    TEST_CASE("framemd5: the parser reads stream 0 and refuses bad lines") {
        const std::string text =
            std::string(HEADER) +
            "0,          0,          0,        1,   152064, 0123456789abcdef0123456789abcdef\n"
            "1,          0,          0,        1,     4096, ffffffffffffffffffffffffffffffff\n"
            "0,          1,          1,        1,   152064, 00112233445566778899aabbccddeeff\n"
            "0,          2,          2,        1,   152064, fedcba9876543210fedcba9876543210\n";
        const auto parsed = parseFramemd5(text);
        REQUIRE_MESSAGE(parsed.has_value(), parsed.error().describe());
        REQUIRE(parsed->size() == 3);
        CHECK((*parsed)[0].pts == 0);
        CHECK((*parsed)[1].pts == 1);
        CHECK((*parsed)[2].pts == 2);
        for (const auto& entry : *parsed) {
            CHECK(entry.size == 152064);
        }
        CHECK((*parsed)[0].md5 == "0123456789abcdef0123456789abcdef");
        CHECK((*parsed)[1].md5 == "00112233445566778899aabbccddeeff");
        CHECK((*parsed)[2].md5 == "fedcba9876543210fedcba9876543210");

        SUBCASE("a line with 5 fields") {
            checkFormatError(std::string(HEADER) + "0, 0, 0, 1, 152064\n");
        }
        SUBCASE("a hash that is not 32 hex digits") {
            checkFormatError(std::string(HEADER) +
                             "0, 0, 0, 1, 152064, 0123456789abcdef0123456789abcde\n");
            checkFormatError(std::string(HEADER) +
                             "0, 0, 0, 1, 152064, 0123456789abcdef0123456789abcdeg\n");
        }
        SUBCASE("a field that is not a number") {
            checkFormatError(std::string(HEADER) +
                             "0, 0, x, 1, 152064, 0123456789abcdef0123456789abcdef\n");
        }
    }
}

#pragma once

// The reference side of the FFmpeg oracle (M4 design v2 §8.1): `ffmpeg -f framemd5` output and the
// MD5 of our own frame bytes.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/base/error.h"

namespace dvv::components::testing {

struct FrameHash {
    std::int64_t pts = 0;
    std::uint64_t size = 0;
    std::string md5;  // 32 lower-case hex digits
};

// The entries of stream 0, in file order. Lines starting with '#' and empty lines are skipped;
// every other line has six comma-separated fields (stream, dts, pts, duration, size, hash), each
// trimmed. Errc::Format "framemd5 line <n>: <reason>" for another field count, a field that is not
// a number, or a hash that is not 32 hex digits.
[[nodiscard]] Result<std::vector<FrameHash>> parseFramemd5(std::string_view text);

// The MD5 of `bytes` as 32 lower-case hex digits, through api().av_md5_sum (FFmpeg must be loaded).
[[nodiscard]] std::string md5Hex(std::span<const std::byte> bytes);

}  // namespace dvv::components::testing

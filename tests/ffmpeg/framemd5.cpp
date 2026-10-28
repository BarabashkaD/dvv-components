#include "framemd5.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "dvv/base/error.h"
#include "ffmpeg/api.h"

namespace dvv::components::testing {
namespace {

constexpr std::size_t FIELDS = 6;
constexpr std::size_t MD5_DIGITS = 32;

std::string_view trimmed(std::string_view text) {
    constexpr std::string_view SPACE = " \t\r";
    const std::size_t first = text.find_first_not_of(SPACE);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(SPACE);
    return text.substr(first, last - first + 1);
}

template <class T>
bool parseNumber(std::string_view text, T& value) {
    const char* const begin = text.data();
    const char* const end = begin + text.size();  // from_chars reads [begin, end) only
    const auto [ptr, error] = std::from_chars(begin, end, value);
    return error == std::errc() && ptr == end && !text.empty();
}

bool isHash(std::string_view text) {
    if (text.size() != MD5_DIGITS) {
        return false;
    }
    for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool hex = c >= 'a' && c <= 'f';
        if (!digit && !hex) {
            return false;
        }
    }
    return true;
}

}  // namespace

Result<std::vector<FrameHash>> parseFramemd5(std::string_view text) {
    std::vector<FrameHash> entries;
    std::size_t line_number = 0;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line =
            trimmed(end == std::string_view::npos ? text : text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        ++line_number;
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        std::array<std::string_view, FIELDS> fields{};
        std::size_t count = 0;
        std::string_view rest = line;
        while (true) {
            const std::size_t comma = rest.find(',');
            const std::string_view field =
                trimmed(comma == std::string_view::npos ? rest : rest.substr(0, comma));
            if (count < FIELDS) {
                fields[count] = field;
            }
            ++count;
            if (comma == std::string_view::npos) {
                break;
            }
            rest = rest.substr(comma + 1);
        }
        if (count != FIELDS) {
            return makeError(Errc::Format, std::format("framemd5 line {}: {} fields instead of {}",
                                                       line_number, count, FIELDS));
        }
        int stream = 0;
        std::int64_t dts = 0;
        FrameHash entry;
        std::int64_t duration = 0;
        if (!parseNumber(fields[0], stream) || !parseNumber(fields[1], dts) ||
            !parseNumber(fields[2], entry.pts) || !parseNumber(fields[3], duration) ||
            !parseNumber(fields[4], entry.size)) {
            return makeError(Errc::Format,
                             std::format("framemd5 line {}: a field is not a number", line_number));
        }
        if (!isHash(fields[5])) {
            return makeError(Errc::Format, std::format("framemd5 line {}: '{}' is not an MD5 hash",
                                                       line_number, fields[5]));
        }
        if (stream != 0) {
            continue;
        }
        entry.md5 = std::string(fields[5]);
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::string md5Hex(std::span<const std::byte> bytes) {
    std::array<std::uint8_t, 16> digest{};
    ffmpeg::api().av_md5_sum(digest.data(), reinterpret_cast<const std::uint8_t*>(bytes.data()),
                             bytes.size());
    std::string text;
    text.reserve(MD5_DIGITS);
    for (const std::uint8_t byte : digest) {
        text += std::format("{:02x}", byte);
    }
    return text;
}

}  // namespace dvv::components::testing

#include "oracle/oracle_harness.h"

#include <doctest/doctest.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "dvv/base/environment.h"
#include "dvv/base/error.h"
#include "dvv/base/executors.h"
#include "dvv/base/task.h"
#include "dvv/base/uri.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/media/types.h"
#include "dvv/platform/file_reader.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_registry.h"

namespace dvv::components::oracle {
namespace {

constexpr std::size_t POOL_THREADS = 2;

// UTF-8 with '/' separators (URIs and messages).
std::string genericUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) {
        return {};
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(separator, start);
        if (end == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
}

std::optional<std::uint32_t> parseNumber(std::string_view text) {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::optional<std::filesystem::path> oracleFixtureDirectory() {
    const std::string root = getEnvironment("DVV_ORACLE_FIXTURES").value_or("");
    if (root.empty()) {
        return std::nullopt;
    }
    std::string text = readText(media::pathFromUtf8(root) / "current.txt");
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::filesystem::path directory = media::pathFromUtf8(text);
    std::error_code error;
    if (!std::filesystem::is_regular_file(directory / "done.stamp", error)) {
        return std::nullopt;
    }
    return directory;
}

std::vector<OracleLine> readManifest() {
    const auto found = oracleFixtureDirectory();
    REQUIRE_MESSAGE(found.has_value(), "no oracle fixtures (DVV_ORACLE_FIXTURES)");
    const std::filesystem::path directory = found.value_or(std::filesystem::path{});
    const std::string text = readText(directory / "manifest.txt");
    REQUIRE_MESSAGE(!text.empty(), "empty or missing " << genericUtf8(directory / "manifest.txt"));
    std::vector<OracleLine> lines;
    for (std::string_view row : split(text, '\n')) {
        if (!row.empty() && row.back() == '\r') {
            row.remove_suffix(1);
        }
        if (row.empty()) {
            continue;
        }
        const std::vector<std::string_view> fields = split(row, '|');
        OracleLine line;
        if (fields.size() == 7 && fields[0] == "unpack") {
            const auto width = parseNumber(fields[2]);
            const auto height = parseNumber(fields[3]);
            const auto frames = parseNumber(fields[4]);
            REQUIRE_MESSAGE((width && height && frames), "bad manifest line: " << row);
            line = {.kind = std::string(fields[0]),
                    .format = std::string(fields[1]),
                    .matrix = {},
                    .range = {},
                    .file = std::string(fields[5]),
                    .letters = std::string(fields[6]),
                    .width = width.value_or(0U),
                    .height = height.value_or(0U),
                    .frames = frames.value_or(0U)};
        } else if (fields.size() == 5 && fields[0] == "display") {
            line = {.kind = std::string(fields[0]),
                    .format = std::string(fields[1]),
                    .matrix = std::string(fields[2]),
                    .range = std::string(fields[3]),
                    .file = std::string(fields[4]),
                    .letters = {}};
        } else {
            FAIL("bad manifest line: " << row);
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::filesystem::path oracleFile(std::string_view name) {
    const auto found = oracleFixtureDirectory();
    REQUIRE_MESSAGE(found.has_value(), "no oracle fixtures (DVV_ORACLE_FIXTURES)");
    const std::filesystem::path path =
        found.value_or(std::filesystem::path{}) / media::pathFromUtf8(name);
    std::error_code error;
    REQUIRE_MESSAGE(std::filesystem::is_regular_file(path, error),
                    "missing fixture " << genericUtf8(path));
    return path;
}

std::vector<std::byte> readOracleFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE_MESSAGE(file.good(), "cannot read " << genericUtf8(path));
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    return {data, data + text.size()};
}

std::string clipUri(const OracleLine& line) {
    return std::format("file:{}?fmt={}&size={}x{}&impl=raw", genericUtf8(oracleFile(line.file)),
                       line.format, line.width, line.height);
}

OracleHarness::OracleHarness() {
    auto cpu = ExecutorFactory::makeThreadPool(POOL_THREADS);
    REQUIRE_MESSAGE(cpu.has_value(), cpu.error().describe());
    cpu_ = std::move(*cpu);
    auto io = ExecutorFactory::makeThreadPool(POOL_THREADS);
    REQUIRE_MESSAGE(io.has_value(), io.error().describe());
    io_ = std::move(*io);
    registry_ = ComponentRegistry::create(Capabilities{});
    REQUIRE(registry_ != nullptr);
    auto factory = media::SourceFactory::create(*registry_, io_);
    REQUIRE_MESSAGE(factory.has_value(), factory.error().describe());
    factory_ = std::move(*factory);
    auto store = media::FrameStore::create({.cpu = cpu_, .io = io_});
    REQUIRE_MESSAGE(store.has_value(), store.error().describe());
    store_ = std::move(*store);
}

OracleHarness::~OracleHarness() {
    for (const auto& [uri, origin] : origins_) {
        store_->detach(origin);
    }
}

Result<media::OriginId> OracleHarness::origin(std::string_view uri) {
    if (const auto found = origins_.find(uri); found != origins_.end()) {
        return found->second;
    }
    DVV_TRY(Uri parsed, parseUri(uri));
    const media::SourceRequest request{
        .uri = std::move(parsed), .hint = std::nullopt, .fileReads = platform::FileReadMode::Auto};
    DVV_TRY(auto opened, syncWait(factory_->open(request)));
    auto* source = std::get_if<std::unique_ptr<media::ISource>>(&opened);
    if (source == nullptr || *source == nullptr) {
        return makeError(Errc::Internal, std::format("{}: the open was ambiguous", uri));
    }
    DVV_TRY(const media::OriginId id,
            store_->attach(std::shared_ptr<media::ISource>(std::move(*source))));
    origins_.emplace(std::string(uri), id);
    return id;
}

Result<std::vector<std::byte>> OracleHarness::originalBytes(std::string_view uri,
                                                            std::int64_t index) {
    DVV_TRY(const media::OriginId id, origin(uri));
    Result<std::vector<std::byte>> bytes;
    {
        auto read = syncWait(store_->acquireOriginal({id, index}, media::Api::HostPlanes,
                                                     store_->hostDomain(), *cpu_, true));
        if (read) {
            const auto* planes = std::get_if<media::HostPlanesB>(&read->lease().binding());
            REQUIRE(planes != nullptr);
            std::vector<std::byte> copy;
            for (std::size_t p = 0; p < planes->planeCount; ++p) {
                copy.insert(copy.end(), planes->planes[p].begin(), planes->planes[p].end());
            }
            bytes = std::move(copy);
        } else {
            bytes = std::unexpected(std::move(read).error());
        }
    }
    // The lease is gone: drop the frame, so the store does not keep every frame of every clip.
    store_->trim(store_->hostDomain(), 0);
    return bytes;
}

}  // namespace dvv::components::oracle

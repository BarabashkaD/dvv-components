#include "ffmpeg_harness.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "dvv/base/environment.h"
#include "dvv/base/error.h"
#include "dvv/base/executors.h"
#include "dvv/base/task.h"
#include "dvv/base/uri.h"
#include "dvv/components/ffmpeg.h"
#include "dvv/media/frame.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/media/types.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_registry.h"
#include "ffmpeg/api.h"
#include "framemd5.h"
#include "sources/av_worker.h"

namespace dvv::components::testing {
namespace {

constexpr std::size_t POOL_THREADS = 4;

// UTF-8 with '/' separators (URIs and messages).
std::string genericUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

}  // namespace

FfmpegHarness::FfmpegHarness() {
    auto cpu = ExecutorFactory::makeThreadPool(POOL_THREADS);
    REQUIRE_MESSAGE(cpu.has_value(), cpu.error().describe());
    cpu_ = std::move(*cpu);
    auto io = ExecutorFactory::makeThreadPool(POOL_THREADS);
    REQUIRE_MESSAGE(io.has_value(), io.error().describe());
    io_ = std::move(*io);
    Capabilities caps;
    addFfmpegCapabilities(loadFfmpeg(), caps);
    REQUIRE(caps.has("ffmpeg"));
    registry_ = ComponentRegistry::create(caps);
    REQUIRE(registry_ != nullptr);
    auto factory = media::SourceFactory::create(*registry_, io_);
    REQUIRE_MESSAGE(factory.has_value(), factory.error().describe());
    factory_ = std::move(*factory);
    auto store = media::FrameStore::create({.cpu = cpu_, .io = io_});
    REQUIRE_MESSAGE(store.has_value(), store.error().describe());
    store_ = std::move(*store);
}

FfmpegHarness::~FfmpegHarness() {
    for (const auto& [source, origin] : origins_) {
        store_->detach(origin);
    }
}

std::shared_ptr<media::ISource> FfmpegHarness::open(std::string_view uri) {
    INFO(std::string(uri));
    auto opened = tryOpen(uri);
    REQUIRE_MESSAGE(opened.has_value(), opened.error().describe());
    auto* source = std::get_if<std::unique_ptr<media::ISource>>(&*opened);
    REQUIRE_MESSAGE(source != nullptr, "the open was ambiguous");
    REQUIRE(*source != nullptr);
    return std::shared_ptr<media::ISource>(std::move(*source));
}

Result<std::variant<std::unique_ptr<media::ISource>, media::Ambiguous>> FfmpegHarness::tryOpen(
    std::string_view uri) {
    DVV_TRY(Uri parsed, parseUri(uri));
    const media::SourceRequest request{
        .uri = std::move(parsed), .hint = std::nullopt, .fileReads = platform::FileReadMode::Auto};
    return syncWait(factory_->open(request));
}

media::OriginId FfmpegHarness::origin(const std::shared_ptr<media::ISource>& source) {
    if (const auto found = origins_.find(source.get()); found != origins_.end()) {
        return found->second;
    }
    auto attached = store_->attach(source);
    REQUIRE_MESSAGE(attached.has_value(), attached.error().describe());
    origins_.emplace(source.get(), *attached);
    return *attached;
}

Result<std::vector<std::byte>> FfmpegHarness::originalBytes(
    const std::shared_ptr<media::ISource>& source, std::int64_t index) {
    const media::OriginId id = origin(source);
    Result<std::vector<std::byte>> bytes;
    {
        auto read = syncWait(store_->acquireOriginal({id, index}, media::Api::HostPlanes,
                                                     store_->hostDomain(), *cpu_, true));
        if (read) {
            bytes = originalReadBytes(*read);
        } else {
            bytes = std::unexpected(std::move(read).error());
        }
    }
    // The lease is gone: drop the frame, so the next read asks the source again.
    store_->trim(store_->hostDomain(), 0);
    return bytes;
}

std::vector<std::byte> originalReadBytes(const media::OriginalRead& read) {
    const auto* planes = std::get_if<media::HostPlanesB>(&read.lease().binding());
    REQUIRE(planes != nullptr);
    std::vector<std::byte> copy;
    for (std::size_t p = 0; p < planes->planeCount; ++p) {
        copy.insert(copy.end(), planes->planes[p].begin(), planes->planes[p].end());
    }
    return copy;
}

std::filesystem::path fixtureDirectory() {
    const std::string root = getEnvironment("DVV_FFMPEG_FIXTURES").value_or("");
    REQUIRE_MESSAGE(!root.empty(), "DVV_FFMPEG_FIXTURES is not set");
    const std::filesystem::path current = media::pathFromUtf8(root) / "current.txt";
    std::ifstream file(current, std::ios::binary);
    REQUIRE_MESSAGE(file.good(), "no fixtures: " << genericUtf8(current));
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return media::pathFromUtf8(text);
}

std::filesystem::path fixture(std::string_view name) {
    const std::filesystem::path path = fixtureDirectory() / media::pathFromUtf8(name);
    std::error_code error;
    REQUIRE_MESSAGE(std::filesystem::is_regular_file(path, error),
                    "missing fixture " << genericUtf8(path));
    return path;
}

std::string fixturePath(std::string_view name) {
    return genericUtf8(fixture(name));
}

std::string fixtureUri(std::string_view name) {
    return "file:" + fixturePath(name);
}

std::vector<FrameHash> referenceHashes(std::string_view framemd5_name) {
    const std::vector<std::byte> bytes = readFile(fixture(framemd5_name));
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    auto hashes = parseFramemd5(text);
    REQUIRE_MESSAGE(hashes.has_value(), hashes.error().describe());
    return std::move(*hashes);
}

std::vector<std::byte> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE_MESSAGE(file.good(), "cannot read " << genericUtf8(path));
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    return {data, data + text.size()};
}

void writeFile(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE_MESSAGE(file.good(), "cannot write " << genericUtf8(path));
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}

PacedSupplier::PacedSupplier(std::unique_ptr<IFrameSupplier> inner,
                             std::shared_ptr<std::atomic<std::int64_t>> stepped)
    : inner_(std::move(inner)), stepped_(std::move(stepped)) {}

Result<const AVFrame*> PacedSupplier::frame(std::int64_t index, const detail::StopCheck& stopped) {
    if (index < next_) {
        return inner_->frame(index, stopped);  // the ring, a seek or a restart: not paced
    }
    while (true) {
        const std::int64_t step = next_;
        auto result = inner_->frame(step, stopped);
        if (!result || step == index) {
            if (result) {
                next_ = step + 1;
            }
            return result;
        }
        next_ = step + 1;
        stepped_->fetch_add(1, std::memory_order_release);
        std::this_thread::sleep_for(PACE);
    }
}

PacedWorkers::PacedWorkers() {
    detail::setSupplierWrapperForTest(
        [stepped = stepped_](std::unique_ptr<detail::IFrameSupplier> inner) {
            return std::make_unique<PacedSupplier>(std::move(inner), stepped);
        });
}

PacedWorkers::~PacedWorkers() {
    detail::setSupplierWrapperForTest({});
}

bool PacedWorkers::waitUntilDecoding() const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (stepped_->load(std::memory_order_acquire) < STEPS_BEFORE_STOP) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

TemporaryDirectory::TemporaryDirectory() {
    std::random_device random;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            std::format("dvv-ffmpeg-test-{:x}-{:x}", random(), stamp);
    std::filesystem::create_directories(path_);
}

TemporaryDirectory::~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
}

std::string TemporaryDirectory::uri(std::string_view name) const {
    return "file:" + genericUtf8(path_ / media::pathFromUtf8(name));
}

}  // namespace dvv::components::testing

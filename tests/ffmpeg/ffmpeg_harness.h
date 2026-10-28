#pragma once

// The fixture of the `ffmpeg` provider tests (as dvv-media's source_harness): a registry with the
// loaded FFmpeg's capabilities and every linked component, a SourceFactory and a host-only
// FrameStore; the container fixtures of make-container-fixtures.cmake and their framemd5 lists.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/executors.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/media/types.h"
#include "dvv/registry/component_registry.h"
#include "ffmpeg/api.h"
#include "framemd5.h"
#include "sources/av_worker.h"

namespace dvv::components::testing {

// cpu and io pools of 4 threads, the registry (capabilities from
// addFfmpegCapabilities(loadFfmpeg()) and every linked component), a SourceFactory and a host-only
// FrameStore. The destructor detaches every attached source, then destroys the store, the factory,
// the registry and the pools.
class FfmpegHarness {
public:
    FfmpegHarness();
    ~FfmpegHarness();
    FfmpegHarness(const FfmpegHarness&) = delete;
    FfmpegHarness& operator=(const FfmpegHarness&) = delete;
    FfmpegHarness(FfmpegHarness&&) = delete;
    FfmpegHarness& operator=(FfmpegHarness&&) = delete;

    // Opens a URI; fails the test (REQUIRE) on an error or Ambiguous.
    [[nodiscard]] std::shared_ptr<media::ISource> open(std::string_view uri);
    // The raw result, for error tests. A URI that does not parse gives its error.
    [[nodiscard]] Result<std::variant<std::unique_ptr<media::ISource>, media::Ambiguous>> tryOpen(
        std::string_view uri);
    // The source's origin in the store, attached on first use (once per source).
    [[nodiscard]] media::OriginId origin(const std::shared_ptr<media::ISource>& source);
    // acquireOriginal(requireExact, HostPlanes, host) of the source's origin and a copy of the
    // tight bytes. The store's host tier is then trimmed to 0, so the next read of any frame runs
    // the source's produce() again (the provider's ring, seek and restart paths are what the tests
    // check, not the store's cache).
    [[nodiscard]] Result<std::vector<std::byte>> originalBytes(
        const std::shared_ptr<media::ISource>& source, std::int64_t index);

    [[nodiscard]] media::FrameStore& store() { return *store_; }
    [[nodiscard]] IExecutor& cpu() { return *cpu_; }
    [[nodiscard]] ComponentRegistry& registry() { return *registry_; }

private:
    // Declared in creation order, so destruction runs the other way round.
    std::shared_ptr<ThreadPool> cpu_;
    std::shared_ptr<ThreadPool> io_;
    std::unique_ptr<ComponentRegistry> registry_;
    std::unique_ptr<media::SourceFactory> factory_;
    std::shared_ptr<media::FrameStore> store_;
    std::map<const media::ISource*, media::OriginId> origins_;
};

// The tight bytes of an original read (the planes back to back).
[[nodiscard]] std::vector<std::byte> originalReadBytes(const media::OriginalRead& read);

// The fixture folder of this FFmpeg build: <DVV_FFMPEG_FIXTURES>/current.txt (REQUIRE).
[[nodiscard]] std::filesystem::path fixtureDirectory();
// A fixture file (REQUIRE: it exists).
[[nodiscard]] std::filesystem::path fixture(std::string_view name);
// Its UTF-8 path with '/' separators, and "file:" + that path.
[[nodiscard]] std::string fixturePath(std::string_view name);
[[nodiscard]] std::string fixtureUri(std::string_view name);
// The entries of <stem>.framemd5 next to the fixtures (REQUIRE: it parses).
[[nodiscard]] std::vector<FrameHash> referenceHashes(std::string_view framemd5_name);
// The whole file (REQUIRE: it can be read).
[[nodiscard]] std::vector<std::byte> readFile(const std::filesystem::path& path);
void writeFile(const std::filesystem::path& path, std::span<const std::byte> bytes);

// The stop tests (the ffmpeg provider's cases 5 and 12, the lavfi stop case) must stop a decode or
// a graph pull that is in progress, on any machine. PacedSupplier steps the real supplier
// one frame at a time and pauses between frames, so a request for frame 2999 takes at least
// 3000 * PACE (6 s) however fast the decoder or the graph is. The supplier's own stop checks end
// it: without them the request would finish after seconds.
// Limit: each paced step is a separate one-frame frame() call, so these cases prove a
// stop check per call, not one per decode-loop iteration; a check moved out of the loop (e.g. to
// the entry of frame()) would still pass. Keep the per-iteration check a review item.
inline constexpr std::chrono::milliseconds PACE{2};
// Frames stepped before the test stops the source: the decode is under way by then.
inline constexpr std::int64_t STEPS_BEFORE_STOP = 20;

class PacedSupplier final : public detail::IFrameSupplier {
public:
    PacedSupplier(std::unique_ptr<IFrameSupplier> inner,
                  std::shared_ptr<std::atomic<std::int64_t>> stepped);
    Result<const AVFrame*> frame(std::int64_t index, const detail::StopCheck& stopped) override;

private:
    std::unique_ptr<IFrameSupplier> inner_;
    std::shared_ptr<std::atomic<std::int64_t>> stepped_;  // shared with PacedWorkers
    std::int64_t next_ = 0;                               // the next frame to step to
};

// Wraps the supplier of every AvWorker created in its scope in a PacedSupplier.
class PacedWorkers {
public:
    PacedWorkers();
    ~PacedWorkers();
    PacedWorkers(const PacedWorkers&) = delete;
    PacedWorkers& operator=(const PacedWorkers&) = delete;
    PacedWorkers(PacedWorkers&&) = delete;
    PacedWorkers& operator=(PacedWorkers&&) = delete;

    // Waits (up to 30 s) until the paced suppliers have stepped STEPS_BEFORE_STOP frames.
    [[nodiscard]] bool waitUntilDecoding() const;

private:
    std::shared_ptr<std::atomic<std::int64_t>> stepped_ =
        std::make_shared<std::atomic<std::int64_t>>(0);
};

// A new empty folder under the system temporary folder, removed with its content at the end.
class TemporaryDirectory {
public:
    TemporaryDirectory();
    ~TemporaryDirectory();
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    // "file:" + the UTF-8 path of `name` inside the folder, with '/' separators.
    [[nodiscard]] std::string uri(std::string_view name) const;

private:
    std::filesystem::path path_;
};

}  // namespace dvv::components::testing

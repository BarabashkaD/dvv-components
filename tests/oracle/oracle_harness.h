#pragma once

// The FFmpeg oracle (M4 design v2 §8.3, Task 6): the fixtures of oracle/make-fixtures.cmake and a
// host-only reader of their clips through the raw provider. No FFmpeg library is loaded.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/base/executors.h"
#include "dvv/media/frame_store.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/registry/component_registry.h"

namespace dvv::components::oracle {

// One manifest line: "unpack|<F>|<W>|<H>|<frames>|<clip>|<letters>" or
// "display|<F>|<M or ->|<limited or full>|<rgb file>". `file` is relative to the fixture folder.
struct OracleLine {
    std::string kind, format, matrix, range, file, letters;
    std::uint32_t width = 0, height = 0, frames = 0;
};

// The fixture folder named by <DVV_ORACLE_FIXTURES>/current.txt when it holds done.stamp; nullopt
// when the variable is unset or the fixtures are incomplete.
[[nodiscard]] std::optional<std::filesystem::path> oracleFixtureDirectory();
// The lines of manifest.txt in the fixture folder (REQUIRE: the folder exists, every line parses).
[[nodiscard]] std::vector<OracleLine> readManifest();
// A file of the fixture folder (REQUIRE: it exists).
[[nodiscard]] std::filesystem::path oracleFile(std::string_view name);
// The whole file (REQUIRE: it is readable).
[[nodiscard]] std::vector<std::byte> readOracleFile(const std::filesystem::path& path);
// The raw-provider URI of an unpack line's clip: file:<path>?fmt=<F>&size=<W>x<H>&impl=raw.
[[nodiscard]] std::string clipUri(const OracleLine& line);

// A registry with every linked component (dvv::media_sources), cpu and io pools of 2 threads, a
// SourceFactory and a host-only FrameStore. Each URI is opened and attached once; the destructor
// detaches them, then destroys the store, the factory, the registry and the pools.
class OracleHarness {
public:
    OracleHarness();
    ~OracleHarness();
    OracleHarness(const OracleHarness&) = delete;
    OracleHarness& operator=(const OracleHarness&) = delete;

    // The original bytes of frame `index` of the URI's source (HostPlanes, exact, planes back to
    // back).
    [[nodiscard]] Result<std::vector<std::byte>> originalBytes(std::string_view uri,
                                                               std::int64_t index);

private:
    [[nodiscard]] Result<media::OriginId> origin(std::string_view uri);

    // Declared in creation order, so destruction runs the other way round.
    std::shared_ptr<ThreadPool> cpu_;
    std::shared_ptr<ThreadPool> io_;
    std::unique_ptr<ComponentRegistry> registry_;
    std::unique_ptr<media::SourceFactory> factory_;
    std::shared_ptr<media::FrameStore> store_;
    std::map<std::string, media::OriginId, std::less<>> origins_;
};

}  // namespace dvv::components::oracle

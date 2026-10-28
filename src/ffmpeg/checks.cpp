// Pure parts of the FFmpeg loader (M4 design D3): the library checks and the search plan.

#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "dvv/components/ffmpeg.h"
#include "dvv/media/types.h"
#include "ffmpeg/loader_detail.h"

namespace dvv::components::detail {
namespace {

constexpr std::string_view LGPL_ONLY = "rawreel uses only LGPL builds";

unsigned majorOf(unsigned packed) noexcept {
    return packed >> 16;
}
unsigned minorOf(unsigned packed) noexcept {
    return (packed >> 8) & 0xffU;
}

std::optional<std::string> libraryProblem(const LibraryFacts& library) {
    if (majorOf(library.runtimeVersion) != majorOf(library.compiledVersion)) {
        return std::format("has {} {}, but rawreel was built for {} (the major version must match)",
                           library.name, versionText(library.runtimeVersion),
                           versionText(library.compiledVersion));
    }
    if (minorOf(library.runtimeVersion) < minorOf(library.compiledVersion)) {
        return std::format("has {} {}, older than the {} rawreel was built for", library.name,
                           versionText(library.runtimeVersion),
                           versionText(library.compiledVersion));
    }
    if (!library.licence.starts_with("LGPL")) {
        if (library.licence.starts_with("GPL")) {
            return std::format("is a GPL build ({}: {}); {}", library.name, library.licence,
                               LGPL_ONLY);
        }
        return std::format("is not an LGPL build ({}: {}); {}", library.name, library.licence,
                           LGPL_ONLY);
    }
    if (library.configuration.find("--enable-gpl") != std::string::npos) {
        return std::format("is a GPL build ({} configured with --enable-gpl); {}", library.name,
                           LGPL_ONLY);
    }
    if (library.configuration.find("--enable-nonfree") != std::string::npos) {
        return std::format("is a nonfree build ({} configured with --enable-nonfree); {}",
                           library.name, LGPL_ONLY);
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> refusalReason(std::span<const LibraryFacts> libraries) {
    if (libraries.empty()) {
        return "has no libraries to check";
    }
    for (const LibraryFacts& library : libraries) {
        if (auto problem = libraryProblem(library)) {
            return problem;
        }
    }
    return std::nullopt;
}

SearchPlan planSearch(const FfmpegLoadOptions& options,
                      const std::optional<std::string>& environment,
                      const std::filesystem::path& exe_dir) {
    SearchPlan plan;
    if (options.directory) {
        plan.source = SearchSource::Options;
        if (!options.directory->is_absolute()) {
            plan.error = std::format("the FFmpeg directory must be absolute (got '{}')",
                                     media::utf8FromPath(*options.directory));
            return plan;
        }
        plan.directories.push_back(*options.directory);
        return plan;
    }
    if (environment && !environment->empty()) {
        plan.source = SearchSource::Environment;
        std::filesystem::path directory = media::pathFromUtf8(*environment);
        if (!directory.is_absolute()) {
            plan.error = std::format("DVV_FFMPEG_DIR must be absolute (got '{}')", *environment);
            return plan;
        }
        plan.directories.push_back(std::move(directory));
        return plan;
    }
    plan.source = SearchSource::Default;
    if (!exe_dir.empty()) {
        plan.directories.push_back((exe_dir / "ffmpeg").lexically_normal());
        plan.directories.push_back((exe_dir / ".." / "lib" / "ffmpeg").lexically_normal());
    }
    return plan;
}

std::string_view sourceName(SearchSource source) noexcept {
    switch (source) {
        case SearchSource::Options: return "the given directory";
        case SearchSource::Environment: return "DVV_FFMPEG_DIR";
        case SearchSource::Default: break;
    }
    return "default search";
}

std::string libraryFileName(std::string_view library, unsigned major) {
#if defined(_WIN32)
    return std::format("{}-{}.dll", library, major);
#else
    return std::format("lib{}.so.{}", library, major);
#endif
}

std::string versionText(unsigned packed) {
    return std::format("{}.{}.{}", majorOf(packed), minorOf(packed), packed & 0xffU);
}

}  // namespace dvv::components::detail

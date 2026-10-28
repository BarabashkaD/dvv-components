#pragma once

// Pure parts of the FFmpeg loader (M4 design D3), unit-tested without FFmpeg.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/components/ffmpeg.h"

namespace dvv::components::detail {

enum class SearchSource : std::uint8_t { Options, Environment, Default };

struct SearchPlan {
    std::vector<std::filesystem::path> directories;  // in order; exactly one for an explicit source
    SearchSource source = SearchSource::Default;
    std::string error;  // non-empty: FFmpeg stays off with this reason, nothing is searched
};

// options.directory, else `environment` (DVV_FFMPEG_DIR; nullopt or "" counts as unset), else
// exe_dir/ffmpeg and exe_dir/../lib/ffmpeg (lexically normal; none when exe_dir is empty). An
// explicit source must be absolute: "the FFmpeg directory must be absolute (got '<p>')",
// "DVV_FFMPEG_DIR must be absolute (got '<value>')".
[[nodiscard]] SearchPlan planSearch(const FfmpegLoadOptions& options,
                                    const std::optional<std::string>& environment,
                                    const std::filesystem::path& exe_dir);
// "the given directory", "DVV_FFMPEG_DIR", "default search".
[[nodiscard]] std::string_view sourceName(SearchSource source) noexcept;
// Windows "<library>-<major>.dll", Linux "lib<library>.so.<major>".
[[nodiscard]] std::string libraryFileName(std::string_view library, unsigned major);
// AV_VERSION_INT packing as "<major>.<minor>.<micro>", e.g. "62.3.100".
[[nodiscard]] std::string versionText(unsigned packed);

}  // namespace dvv::components::detail

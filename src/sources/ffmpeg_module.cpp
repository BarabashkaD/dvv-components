// The FFmpeg module (spec §6.2): parses its option, loads FFmpeg once per process and reports the
// capabilities, the reason and the properties of that load.

#include "sources/ffmpeg_module.h"

#include <array>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "dvv/base/utf8.h"
#include "dvv/components/ffmpeg.h"
#include "dvv/registry/module.h"
#include "dvv/registry/parameters.h"
#include "ffmpeg/loader_detail.h"

namespace dvv::components {
namespace {
// Static storage: ModuleInfo::produces is a span into it.
constexpr std::array<std::string_view, 2> PRODUCES{FFMPEG_CAPABILITY, FFMPEG_LAVFI_CAPABILITY};
}  // namespace

ModuleInfo FfmpegModule::info() noexcept {
    return {.id = FFMPEG_MODULE, .name = "FFmpeg", .produces = PRODUCES};
}

ModuleLoad FfmpegModule::load(const ParameterValues& options) {
    std::expected<FfmpegLoadOptions, std::string> parsed = detail::moduleOptions(options);
    if (!parsed) {
        // A bad option loads nothing, so a later valid load() still decides the process's load.
        return ModuleLoad{
            .provides = {}, .reason = std::move(parsed.error()), .properties = {}, .handle = {}};
    }
    const detail::LoadOutcome outcome = detail::loadOnce(*parsed);
    return detail::moduleLoadOf(outcome.status, outcome.source);
}

namespace detail {

std::expected<FfmpegLoadOptions, std::string> moduleOptions(const ParameterValues& options) {
    FfmpegLoadOptions load_options;
    for (const auto& [key, value] : options) {
        if (key != FFMPEG_DIRECTORY_OPTION) {
            return std::unexpected(std::format("unknown option '{}' (the FFmpeg module takes '{}')",
                                               key, FFMPEG_DIRECTORY_OPTION));
        }
        const std::string* folder = std::get_if<std::string>(&value);
        if (folder == nullptr) {
            return std::unexpected(
                std::format("option '{}' must be text (a folder)", FFMPEG_DIRECTORY_OPTION));
        }
        load_options.directory = pathFromText(*folder);
    }
    return load_options;
}

ModuleLoad moduleLoadOf(const FfmpegStatus& status, SearchSource source) {
    ModuleLoad result;
    if (status.core) {
        result.provides.emplace_back(FFMPEG_CAPABILITY);
    }
    if (status.lavfi) {
        result.provides.emplace_back(FFMPEG_LAVFI_CAPABILITY);
    }
    result.reason = status.reason;
    if (status.core) {
        result.properties.push_back(
            ModuleProperty{.key = std::string(MODULE_VERSION_PROPERTY), .value = status.version});
        result.properties.push_back(
            ModuleProperty{.key = std::string(MODULE_LICENCE_PROPERTY), .value = status.licence});
        result.properties.push_back(ModuleProperty{.key = std::string(MODULE_DIRECTORY_PROPERTY),
                                                   .value = pathText(status.directory)});
    }
    if (source == SearchSource::Environment) {
        result.properties.push_back(ModuleProperty{.key = std::string(MODULE_VARIABLE_PROPERTY),
                                                   .value = "DVV_FFMPEG_DIR"});
    }
    return result;  // handle: null, FFmpeg keeps no per-registry state and is never unloaded
}

}  // namespace detail

DVV_REGISTER_MODULE(FfmpegModule);  // at namespace scope, as DVV_REGISTER is used today
}  // namespace dvv::components

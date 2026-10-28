// The FFmpeg run-time loader (M4 design D3, §4.4, §5.5, §6): finds, opens, resolves and checks the
// six libraries once per process and never unloads them; routes FFmpeg's log to the dvv logger.

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "dvv/base/environment.h"
#include "dvv/base/log.h"
#include "dvv/base/paths.h"
#include "dvv/components/ffmpeg.h"
#include "dvv/media/types.h"
#include "dvv/platform/dynamic_library.h"
#include "ffmpeg/api.h"
#include "ffmpeg/loader_detail.h"

namespace dvv::components {
namespace {

using ffmpeg::Api;
using ffmpeg::Library;

struct LibraryInfo {
    Library library;
    std::string_view name;
    unsigned compiledVersion;
};
// The FfmpegStatus::libraryVersions order.
constexpr std::array<LibraryInfo, 6> LIBRARIES{{
    {Library::Avutil, "avutil", LIBAVUTIL_VERSION_INT},
    {Library::Avcodec, "avcodec", LIBAVCODEC_VERSION_INT},
    {Library::Avformat, "avformat", LIBAVFORMAT_VERSION_INT},
    {Library::Swscale, "swscale", LIBSWSCALE_VERSION_INT},
    {Library::Swresample, "swresample", LIBSWRESAMPLE_VERSION_INT},
    {Library::Avfilter, "avfilter", LIBAVFILTER_VERSION_INT},
}};
// Each library after the FFmpeg libraries it depends on.
constexpr std::array<Library, 6> LOAD_ORDER{Library::Avutil,   Library::Swresample,
                                            Library::Swscale,  Library::Avcodec,
                                            Library::Avformat, Library::Avfilter};

using OpenedLibraries = std::array<std::optional<platform::DynamicLibrary>, 6>;

constexpr std::size_t indexOf(Library library) noexcept {
    return static_cast<std::size_t>(library);
}
static_assert(indexOf(Library::Avfilter) + 1 == LIBRARIES.size());

const LibraryInfo& infoOf(Library library) noexcept {
    return LIBRARIES[indexOf(library)];
}

// The file name the loader opens, e.g. "avutil-60.dll".
std::string libraryFile(Library library) {
    const LibraryInfo& info = infoOf(library);
    return detail::libraryFileName(info.name, info.compiledVersion >> 16);
}

// Process infrastructure (design §6): created once, never destroyed, like the Logger.
struct LoaderState {
    std::once_flag once;
    FfmpegStatus status;
    Api api;
    std::atomic<bool> loaded{false};
    std::vector<platform::DynamicLibrary> kept;  // keepLoaded(); never unloaded
};
LoaderState& loaderState() {
    static auto* const STATE = new LoaderState;
    return *STATE;
}

// Raises the DecodeErrorMark of the AVCodecContext `object` (av_log's object: null or a struct
// whose first member is its AVClass pointer), if it has one.
void markDecodeError(void* object) noexcept {
    if (object == nullptr) {
        return;
    }
    const Api& api = loaderState().api;
    const AVClass* const object_class = *static_cast<const AVClass* const*>(object);
    if (api.avcodec_get_class == nullptr || object_class != api.avcodec_get_class()) {
        return;
    }
    if (auto* const mark =
            static_cast<ffmpeg::DecodeErrorMark*>(static_cast<AVCodecContext*>(object)->opaque);
        mark != nullptr && mark->tag == ffmpeg::DecodeErrorMark::TAG) {
        mark->raised.store(true, std::memory_order_release);
    }
}

// FFmpeg's log goes to the dvv logger, capped at warning; nothing reaches stderr (design D3).
// Errors for a codec context with a DecodeErrorMark also raise it.
// Called from FFmpeg's C frames (maybe its worker threads): no exception may escape, so a line that
// cannot be formatted or written is dropped.
void logCallback(void* object, int level, const char* format, va_list args) noexcept {
    // Levels may carry a colour tint above the low byte (AV_LOG_C); FFmpeg masks it the same way.
    const int base_level = level >= 0 ? (level & 0xff) : level;
    if (base_level < AV_LOG_PANIC || base_level > AV_LOG_WARNING) {
        return;
    }
    if (base_level <= AV_LOG_ERROR) {
        markDecodeError(object);
    }
    thread_local int print_prefix = 1;
    std::array<char, 1024> line{};
    loaderState().api.av_log_format_line2(object, level, format, args, line.data(),
                                          static_cast<int>(line.size()), &print_prefix);
    std::string_view text(line.data());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return;
    }
    try {
        Logger::instance().write(LogLevel::Warn, std::format("FFmpeg: {}", text));
    } catch (...) {  // NOLINT(bugprone-empty-catch): logging must never break decoding
    }
}

// Resolves every table entry from the opened libraries (index = Library). A missing function is
// appended to `missing_core` or, for avfilter, `missing_lavfi`.
void resolve(Api& api, const OpenedLibraries& opened, std::vector<std::string>& missing_core,
             std::vector<std::string>& missing_lavfi) {
// NOLINTBEGIN(bugprone-macro-parentheses)
#define DVV_FFMPEG_RESOLVE(library_id, symbol)                                               \
    if (const auto& handle = opened[indexOf(Library::library_id)]; handle) {                 \
        api.symbol = handle->function<decltype(api.symbol)>(#symbol);                        \
        if (api.symbol == nullptr) {                                                         \
            (Library::library_id == Library::Avfilter ? missing_lavfi : missing_core)        \
                .push_back(                                                                  \
                    std::format("{} has no {}", libraryFile(Library::library_id), #symbol)); \
        }                                                                                    \
    }
    DVV_FFMPEG_FUNCTIONS(DVV_FFMPEG_RESOLVE)
#undef DVV_FFMPEG_RESOLVE
    // NOLINTEND(bugprone-macro-parentheses)
}

// Sets every avfilter member back to null (lavfi off).
void clearAvfilter(Api& api) noexcept {
// NOLINTBEGIN(bugprone-macro-parentheses)
#define DVV_FFMPEG_CLEAR(library_id, symbol)                  \
    if constexpr (Library::library_id == Library::Avfilter) { \
        api.symbol = nullptr;                                 \
    }
    DVV_FFMPEG_FUNCTIONS(DVV_FFMPEG_CLEAR)
#undef DVV_FFMPEG_CLEAR
    // NOLINTEND(bugprone-macro-parentheses)
}

std::string text(const char* value) {
    return value != nullptr ? std::string(value) : std::string();
}

// <lib>_version(), <lib>_license() and <lib>_configuration() of one opened library.
detail::LibraryFacts factsOf(const Api& api, const LibraryInfo& info) {
    detail::LibraryFacts facts{.name = std::string(info.name),
                               .runtimeVersion = 0,
                               .compiledVersion = info.compiledVersion,
                               .licence = {},
                               .configuration = {}};
    switch (info.library) {
        case Library::Avutil:
            facts.runtimeVersion = api.avutil_version();
            facts.licence = text(api.avutil_license());
            facts.configuration = text(api.avutil_configuration());
            break;
        case Library::Avcodec:
            facts.runtimeVersion = api.avcodec_version();
            facts.licence = text(api.avcodec_license());
            facts.configuration = text(api.avcodec_configuration());
            break;
        case Library::Avformat:
            facts.runtimeVersion = api.avformat_version();
            facts.licence = text(api.avformat_license());
            facts.configuration = text(api.avformat_configuration());
            break;
        case Library::Swscale:
            facts.runtimeVersion = api.swscale_version();
            facts.licence = text(api.swscale_license());
            facts.configuration = text(api.swscale_configuration());
            break;
        case Library::Swresample:
            facts.runtimeVersion = api.swresample_version();
            facts.licence = text(api.swresample_license());
            facts.configuration = text(api.swresample_configuration());
            break;
        case Library::Avfilter:
            facts.runtimeVersion = api.avfilter_version();
            facts.licence = text(api.avfilter_license());
            facts.configuration = text(api.avfilter_configuration());
            break;
    }
    return facts;
}

// The directory to load from, or the reason FFmpeg stays off.
struct Located {
    std::filesystem::path directory;
    detail::SearchSource source = detail::SearchSource::Default;
    std::string error;
};

Located locate(const FfmpegLoadOptions& options) {
    std::filesystem::path exe_dir;
    if (auto exe = paths::executablePath(); exe) {
        exe_dir = exe->parent_path();
    }
    detail::SearchPlan plan =
        detail::planSearch(options, getEnvironment("DVV_FFMPEG_DIR"), exe_dir);
    if (!plan.error.empty()) {
        return {.directory = {}, .source = plan.source, .error = std::move(plan.error)};
    }
    if (plan.source != detail::SearchSource::Default) {
        return {.directory = plan.directories.front(), .source = plan.source, .error = {}};
    }
    constexpr std::string_view HINT =
        "set DVV_FFMPEG_DIR to the folder of an LGPL FFmpeg shared build";
    if (plan.directories.empty()) {
        return {.directory = {},
                .source = plan.source,
                .error =
                    std::format("FFmpeg not found (the executable's folder is unknown; {})", HINT)};
    }
    const std::string avutil_file = libraryFile(Library::Avutil);
    for (const std::filesystem::path& directory : plan.directories) {
        std::error_code error;
        if (std::filesystem::is_regular_file(directory / avutil_file, error)) {
            return {.directory = directory, .source = plan.source, .error = {}};
        }
    }
    std::string looked;
    for (std::size_t i = 0; i < plan.directories.size(); ++i) {
        if (i > 0) {
            looked += i + 1 == plan.directories.size() ? " and " : ", ";
        }
        looked += media::utf8FromPath(plan.directories[i]);
    }
    return {.directory = {},
            .source = plan.source,
            .error = std::format("FFmpeg not found (looked in {}; {})", looked, HINT)};
}

// Behaviour of Task 4: plan, directory, open, resolve, facts, refusal, success.
void load(LoaderState& state, const FfmpegLoadOptions& options) {
    FfmpegStatus& status = state.status;
    status = {};  // a retry after a throwing attempt (call_once runs again) starts clean
    Located located = locate(options);
    if (!located.error.empty()) {
        status.reason = std::move(located.error);
        return;
    }
    const std::filesystem::path directory = located.directory.lexically_normal();
    const std::string where = std::format("FFmpeg in {} ({})", media::utf8FromPath(directory),
                                          detail::sourceName(located.source));

    OpenedLibraries opened;
    std::string lavfi_reason;
    for (const Library library : LOAD_ORDER) {
        auto handle =
            platform::DynamicLibrary::open(media::utf8FromPath(directory / libraryFile(library)),
                                           platform::LibrarySearch::DllDirectory);
        if (!handle) {
            if (library == Library::Avfilter) {
                lavfi_reason = std::format("{}: libavfilter is not available: {}", where,
                                           handle.error().message);
                continue;
            }
            status.reason = std::format("{}: {}", where, handle.error().message);
            return;
        }
        opened[indexOf(library)].emplace(std::move(*handle));
    }

    Api api;
    std::vector<std::string> missing_core;
    std::vector<std::string> missing_lavfi;
    resolve(api, opened, missing_core, missing_lavfi);
    if (!missing_core.empty()) {
        status.reason = std::format("{}: {}", where, missing_core.front());
        return;
    }
    if (!missing_lavfi.empty()) {
        lavfi_reason = std::format("{}: {}", where, missing_lavfi.front());
    }
    if (!lavfi_reason.empty()) {
        clearAvfilter(api);
        opened[indexOf(Library::Avfilter)].reset();  // unloaded; never kept
    }

    std::vector<detail::LibraryFacts> facts;
    std::array<std::uint32_t, 6> versions{};
    for (std::size_t i = 0; i < LIBRARIES.size(); ++i) {
        if (opened[indexOf(LIBRARIES[i].library)]) {
            facts.push_back(factsOf(api, LIBRARIES[i]));
            versions[i] = facts.back().runtimeVersion;
        }
    }
    if (auto refusal = detail::refusalReason(facts)) {
        status.reason = std::format("{} {}", where, *refusal);
        return;
    }

    for (std::optional<platform::DynamicLibrary>& handle : opened) {
        if (handle) {
            handle->keepLoaded();
            state.kept.push_back(std::move(*handle));
        }
    }
    state.api = api;
    state.api.av_log_set_level(AV_LOG_WARNING);
    state.api.av_log_set_callback(&logCallback);
    status.core = true;
    status.lavfi = lavfi_reason.empty();
    status.reason = std::move(lavfi_reason);
    status.version = text(state.api.av_version_info());
    status.licence = text(state.api.avutil_license());
    status.directory = directory;
    status.libraryVersions = versions;
}

}  // namespace

const FfmpegStatus& loadFfmpeg(const FfmpegLoadOptions& options) {
    LoaderState& state = loaderState();
    std::call_once(state.once, [&] {
        load(state, options);
        if (state.status.core) {
            state.loaded.store(true, std::memory_order_release);
        }
    });
    return state.status;
}

void addFfmpegCapabilities(const FfmpegStatus& status, Capabilities& caps) {
    if (status.core) {
        caps.add("ffmpeg");
    }
    if (status.lavfi) {
        caps.add("ffmpeg.lavfi");
    }
}

namespace {
// Constant-initialised and never destroyed before use ends (a trivially destructible atomic).
std::atomic<std::uint64_t>& stopCounter() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}
}  // namespace

void stopFfmpegSources() noexcept {
    stopCounter().fetch_add(1, std::memory_order_release);
}

namespace ffmpeg {
std::uint64_t stopGeneration() noexcept {
    return stopCounter().load(std::memory_order_acquire);
}

const Api& api() {
    LoaderState& state = loaderState();
    if (!state.loaded.load(std::memory_order_acquire)) {
        throw std::logic_error("FFmpeg is not loaded");
    }
    return state.api;
}
}  // namespace ffmpeg

}  // namespace dvv::components

// The FFmpeg run-time loader: finds, opens, resolves and checks the
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
#include <type_traits>
#include <utility>
#include <vector>

#include "dvv/base/environment.h"
#include "dvv/base/log.h"
#include "dvv/base/paths.h"
#include "dvv/base/utf8.h"
#include "dvv/components/ffmpeg.h"
#include "dvv/platform/dynamic_library.h"
#include "ffmpeg/api.h"
#include "ffmpeg/loader_detail.h"

namespace dvv::components {
namespace {

using ffmpeg::Api;
using ffmpeg::Library;

struct LibraryEntry {
    Library library;
    std::string_view name;
    unsigned compiledVersion;
    decltype(Api::avutil_version) Api::* version;              // <lib>_version
    decltype(Api::avutil_license) Api::* licence;              // <lib>_license
    decltype(Api::avutil_configuration) Api::* configuration;  // <lib>_configuration
};
// The library prefixes, in the FfmpegStatus::libraryVersions order.
#define DVV_FFMPEG_LIBRARY_TABLE(X)                      \
    X(Avutil, avutil, LIBAVUTIL_VERSION_INT)             \
    X(Avcodec, avcodec, LIBAVCODEC_VERSION_INT)          \
    X(Avformat, avformat, LIBAVFORMAT_VERSION_INT)       \
    X(Swscale, swscale, LIBSWSCALE_VERSION_INT)          \
    X(Swresample, swresample, LIBSWRESAMPLE_VERSION_INT) \
    X(Avfilter, avfilter, LIBAVFILTER_VERSION_INT)
// NOLINTBEGIN(bugprone-macro-parentheses)
// The three function types are equal in every library's headers; a header change fails the build
// here instead of the call.
#define DVV_FFMPEG_LIBRARY_CHECK(library_id, prefix, version_int)                                  \
    static_assert(std::is_same_v<decltype(Api::prefix##_version), decltype(Api::avutil_version)>); \
    static_assert(std::is_same_v<decltype(Api::prefix##_license), decltype(Api::avutil_license)>); \
    static_assert(std::is_same_v<decltype(Api::prefix##_configuration),                            \
                                 decltype(Api::avutil_configuration)>);
DVV_FFMPEG_LIBRARY_TABLE(DVV_FFMPEG_LIBRARY_CHECK)
#undef DVV_FFMPEG_LIBRARY_CHECK
#define DVV_FFMPEG_LIBRARY_ENTRY(library_id, prefix, version_int) \
    {Library::library_id,                                         \
     #prefix,                                                     \
     version_int,                                                 \
     &Api::prefix##_version,                                      \
     &Api::prefix##_license,                                      \
     &Api::prefix##_configuration},
constexpr std::array<LibraryEntry, 6> LIBRARIES{
    {DVV_FFMPEG_LIBRARY_TABLE(DVV_FFMPEG_LIBRARY_ENTRY)}};
#undef DVV_FFMPEG_LIBRARY_ENTRY
#undef DVV_FFMPEG_LIBRARY_TABLE
// NOLINTEND(bugprone-macro-parentheses)
// Each library after the FFmpeg libraries it depends on.
constexpr std::array<Library, 6> LOAD_ORDER{Library::Avutil,   Library::Swresample,
                                            Library::Swscale,  Library::Avcodec,
                                            Library::Avformat, Library::Avfilter};

using OpenedLibraries = std::array<std::optional<platform::DynamicLibrary>, 6>;

constexpr std::size_t indexOf(Library library) noexcept {
    return static_cast<std::size_t>(library);
}
static_assert(indexOf(Library::Avfilter) + 1 == LIBRARIES.size());

const LibraryEntry& entryOf(Library library) noexcept {
    return LIBRARIES[indexOf(library)];
}

// The file name the loader opens, e.g. "avutil-60.dll".
std::string libraryFile(Library library) {
    const LibraryEntry& entry = entryOf(library);
    return platform::libraryFileName(entry.name, entry.compiledVersion >> 16);
}

// Process infrastructure: created once, never destroyed, like the Logger.
struct LoaderState {
    std::once_flag once;
    FfmpegStatus status;
    detail::SearchSource source = detail::SearchSource::Default;  // where the search came from
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

// FFmpeg's log goes to the dvv logger, capped at warning; nothing reaches stderr.
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
detail::LibraryInfo infoOf(const Api& api, const LibraryEntry& entry) {
    return detail::LibraryInfo{.name = std::string(entry.name),
                               .runtimeVersion = (api.*entry.version)(),
                               .compiledVersion = entry.compiledVersion,
                               .licence = text((api.*entry.licence)()),
                               .configuration = text((api.*entry.configuration)())};
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
        looked += pathDisplayText(plan.directories[i]);
    }
    return {.directory = {},
            .source = plan.source,
            .error = std::format("FFmpeg not found (looked in {}; {})", looked, HINT)};
}

// Behaviour: plan, directory, open, resolve, library info, refusal, success.
void load(LoaderState& state, const FfmpegLoadOptions& options) {
    FfmpegStatus& status = state.status;
    status = {};  // a retry after a throwing attempt (call_once runs again) starts clean
    Located located = locate(options);
    state.source = located.source;  // before the first return: every result carries it
    if (!located.error.empty()) {
        status.reason = std::move(located.error);
        return;
    }
    const std::filesystem::path directory = located.directory.lexically_normal();
    const std::string where = std::format("FFmpeg in {} ({})", pathDisplayText(directory),
                                          detail::sourceName(located.source));

    OpenedLibraries opened;
    std::string lavfi_reason;
    for (const Library library : LOAD_ORDER) {
        auto handle = platform::DynamicLibrary::open(pathText(directory / libraryFile(library)),
                                                     platform::LibrarySearch::LibraryDirectory);
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

    std::vector<detail::LibraryInfo> infos;
    std::array<std::uint32_t, 6> versions{};
    for (std::size_t i = 0; i < LIBRARIES.size(); ++i) {
        if (opened[indexOf(LIBRARIES[i].library)]) {
            infos.push_back(infoOf(api, LIBRARIES[i]));
            versions[i] = infos.back().runtimeVersion;
        }
    }
    if (auto refusal = detail::refusalReason(infos)) {
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

detail::LoadOutcome detail::loadOnce(const FfmpegLoadOptions& options) {
    LoaderState& state = loaderState();
    std::call_once(state.once, [&] {
        load(state, options);
        if (state.status.core) {
            state.loaded.store(true, std::memory_order_release);
        }
    });
    return {.status = state.status, .source = state.source};
}

const FfmpegStatus& loadFfmpeg(const FfmpegLoadOptions& options) {
    return detail::loadOnce(options).status;
}

void addFfmpegCapabilities(const FfmpegStatus& status, Capabilities& caps) {
    if (status.core) {
        caps.add(FFMPEG_CAPABILITY);
    }
    if (status.lavfi) {
        caps.add(FFMPEG_LAVFI_CAPABILITY);
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

std::optional<std::string> averrorText(int averror) {
    LoaderState& state = loaderState();
    if (!state.loaded.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (state.api.av_strerror(averror, buffer.data(), buffer.size()) != 0 ||
        buffer.front() == '\0') {
        return std::nullopt;
    }
    return std::string(buffer.data());
}
}  // namespace ffmpeg

}  // namespace dvv::components

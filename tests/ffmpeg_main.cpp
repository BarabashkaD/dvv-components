#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include "dvv/base/environment.h"
#include "dvv/base/leak_check.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/utf8.h"
#include "dvv/components/ffmpeg.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_registry.h"
#include "dvv/registry/module.h"

namespace {

// Loads FFmpeg through the FFmpeg module. Why it does not provide both capabilities; nullopt when
// it does. The registry ends here, before any case runs.
std::optional<std::string> ffmpegUnavailableReason() {
    const auto registry = dvv::ComponentRegistry::load();
    const dvv::ModuleStatus* module = registry->module(dvv::components::FFMPEG_MODULE);
    if (module == nullptr) {
        return "the FFmpeg module is not linked";
    }
    const dvv::Capabilities& caps = registry->capabilities();
    if (caps.has(dvv::components::FFMPEG_CAPABILITY) &&
        caps.has(dvv::components::FFMPEG_LAVFI_CAPABILITY)) {
        return std::nullopt;
    }
    return module->reason;
}

}  // namespace

// DVV_FFMPEG_TEST_MODE "available" (default): FFmpeg must load from DVV_FFMPEG_DIR; without it the
// executable exits 77 (skipped) unless DVV_REQUIRE_FFMPEG=1. "absent" and "relative" run the cases
// that expect FFmpeg to stay off.
int main(int argc, char** argv) {
    dvv::LifetimeRegistry::instance().setDetailed(true);
    const std::string mode = dvv::getEnvironment("DVV_FFMPEG_TEST_MODE").value_or("available");
    if (mode == "available") {
        if (const std::optional<std::string> reason = ffmpegUnavailableReason()) {
            std::fprintf(stderr, "FFmpeg is not available: %s\n", reason->c_str());
            return dvv::getEnvironment("DVV_REQUIRE_FFMPEG") == "1" ? 1 : 77;
        }
        // The container and lavfi fixtures (make-container-fixtures.cmake,
        // make-lavfi-fixtures.cmake) exist only when the prefix has the ffmpeg program; without
        // them the provider cases cannot run.
        for (const char* variable : {"DVV_FFMPEG_FIXTURES", "DVV_LAVFI_FIXTURES"}) {
            if (const auto fixtures = dvv::getEnvironment(variable)) {
                std::error_code error;
                if (!std::filesystem::is_regular_file(dvv::pathFromText(*fixtures) / "current.txt",
                                                      error)) {
                    std::fprintf(stderr, "FFmpeg fixtures are missing in %s\n", fixtures->c_str());
                    return dvv::getEnvironment("DVV_REQUIRE_FFMPEG") == "1" ? 1 : 77;
                }
            }
        }
    }
    doctest::Context context(argc, argv);
    const int result = context.run();
    if (context.shouldExit()) {
        return result;
    }
    return dvv::finishWithLeakCheck(result);
}

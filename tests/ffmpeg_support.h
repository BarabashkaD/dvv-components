#pragma once

// FFmpeg of the third-party prefix for the FFmpeg tests, from the CTest environment
// (tests/CMakeLists.txt, ffmpeg_test_environment).

#include <filesystem>
#include <string_view>

#include "dvv/base/environment.h"
#include "dvv/media/types.h"

namespace dvv::components::testing {

// The path in the environment variable `name`; empty when unset.
inline std::filesystem::path pathFromTestEnvironment(std::string_view name) {
    const auto value = getEnvironment(name);
    return value ? media::pathFromUtf8(*value) : std::filesystem::path();
}

// The prefix's FFmpeg library folder (DVV_TEST_FFMPEG_RUNTIME_DIR); empty when unset.
inline std::filesystem::path ffmpegRuntimeDirectory() {
    return pathFromTestEnvironment("DVV_TEST_FFMPEG_RUNTIME_DIR");
}
// The prefix's ffmpeg program (DVV_TEST_FFMPEG_PROGRAM); empty when unset.
inline std::filesystem::path ffmpegProgram() {
    return pathFromTestEnvironment("DVV_TEST_FFMPEG_PROGRAM");
}

}  // namespace dvv::components::testing

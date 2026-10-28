// Compile-fail cases for the components of dvv-components (spec §9.4). DVV_COMPILE_FAIL_CASE
// selects one:
//   0  control: the components' headers compile
//   1  constructing a component with a forged registry key (CreationKey's constructor is private)
//   2  the same for FfmpegSourceProvider (M4 Task 5a)
#include <type_traits>

#include "dvv/base/lifetime.h"
#include "echo_provider.h"
#include "sources/ffmpeg_provider.h"

using dvv::components::testing::TestEchoProvider;

int main() {
#if DVV_COMPILE_FAIL_CASE == 0
    static_assert(std::is_final_v<TestEchoProvider>);
    static_assert(!std::is_copy_constructible_v<TestEchoProvider>);
    static_assert(std::is_final_v<dvv::components::FfmpegSourceProvider>);
    static_assert(!std::is_copy_constructible_v<dvv::components::FfmpegSourceProvider>);
    static_assert(std::is_final_v<dvv::components::FfmpegSource>);
    return 0;
#elif DVV_COMPILE_FAIL_CASE == 1
    TestEchoProvider provider(dvv::CreationKey<dvv::ComponentRegistry>(
                                  dvv::LifetimeRegistry::instance().issue<TestEchoProvider>()),
                              dvv::ParameterValues{});
    return 0;
#elif DVV_COMPILE_FAIL_CASE == 2
    dvv::components::FfmpegSourceProvider provider(
        dvv::CreationKey<dvv::ComponentRegistry>(
            dvv::LifetimeRegistry::instance().issue<dvv::components::FfmpegSourceProvider>()),
        dvv::ParameterValues{});
    return 0;
#endif
}
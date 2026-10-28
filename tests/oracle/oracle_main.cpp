#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>

#include "dvv/base/environment.h"
#include "dvv/base/leak_check.h"
#include "dvv/base/lifetime.h"
#include "oracle/oracle_harness.h"

// The oracle needs the fixtures of oracle/make-fixtures.cmake, which exist only when the prefix has
// the ffmpeg program (design v2 §3): without them the executable exits 77 (skipped) unless
// DVV_REQUIRE_FFMPEG=1, which makes it a failure.
int main(int argc, char** argv) {
    dvv::LifetimeRegistry::instance().setDetailed(true);
    if (!dvv::components::oracle::oracleFixtureDirectory()) {
        if (dvv::getEnvironment("DVV_REQUIRE_FFMPEG") == "1") {
            std::fprintf(stderr, "oracle fixtures missing (DVV_REQUIRE_FFMPEG=1)\n");
            return 1;
        }
        std::fprintf(stderr, "oracle fixtures missing: skipped\n");
        return 77;
    }
    doctest::Context context(argc, argv);
    const int result = context.run();
    if (context.shouldExit()) {
        return result;
    }
    return dvv::finishWithLeakCheck(result);
}

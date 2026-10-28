// Component conformance (spec §12): the registry holds exactly the enabled components (the CMake
// set of dvv_add_component() and the M2 source providers), and every component is valid, available
// with its capabilities, owned by the registry and destroyed without a leak.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/base/lifetime.h"
#include "dvv/media/source_factory.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_info.h"
#include "dvv/registry/component_registry.h"
#include "dvv/registry/parameters.h"
#include "expected_components.h"

namespace {

using dvv::components::testing::CMAKE_COMPONENTS;
using dvv::components::testing::ExpectedComponent;

// Every capability a v1.0 component probes for; later components add theirs here.
const dvv::Capabilities& allCapabilities() {
    static const dvv::Capabilities ALL_CAPABILITIES{"ffmpeg", "ffmpeg.lavfi", "test.echo"};
    return ALL_CAPABILITIES;
}

// The source providers of dvv::media_sources (M2), linked into the test as well.
constexpr std::array<ExpectedComponent, 3> M2_PROVIDERS{{
    {"raw", dvv::Kind::Source},
    {"synthetic", dvv::Kind::Source},
    {"y4m", dvv::Kind::Source},
}};

// CMAKE_COMPONENTS and M2_PROVIDERS, sorted by id.
std::vector<ExpectedComponent> expected() {
    std::vector<ExpectedComponent> all(CMAKE_COMPONENTS.begin(), CMAKE_COMPONENTS.end());
    all.insert(all.end(), M2_PROVIDERS.begin(), M2_PROVIDERS.end());
    std::ranges::sort(all, {}, &ExpectedComponent::id);
    return all;
}

std::string describe(const std::vector<ExpectedComponent>& components) {
    std::string text;
    for (const ExpectedComponent& component : components) {
        text += std::string(component.id) + "=" + std::string(dvv::kindName(component.kind)) + " ";
    }
    return text;
}

std::vector<ExpectedComponent> listed(const std::vector<dvv::ComponentStatus>& statuses) {
    std::vector<ExpectedComponent> components;
    components.reserve(statuses.size());
    for (const dvv::ComponentStatus& status : statuses) {
        components.push_back({status.info.id, status.info.kind});
    }
    return components;
}

bool sameIdsAndKinds(const std::vector<ExpectedComponent>& a,
                     const std::vector<ExpectedComponent>& b) {
    return std::ranges::equal(a, b, [](const ExpectedComponent& x, const ExpectedComponent& y) {
        return x.id == y.id && x.kind == y.kind;
    });
}

bool sameIds(const std::vector<ExpectedComponent>& a, const std::vector<ExpectedComponent>& b) {
    return std::ranges::equal(a, b, {}, &ExpectedComponent::id, &ExpectedComponent::id);
}

bool cmakeHas(std::string_view id) {
    return std::ranges::any_of(
        CMAKE_COMPONENTS, [id](const ExpectedComponent& component) { return component.id == id; });
}

}  // namespace

TEST_CASE("conformance: the registry holds exactly the enabled components") {
    const auto registry = dvv::ComponentRegistry::create(allCapabilities());
    const std::vector<ExpectedComponent> got = listed(registry->list());
    const std::vector<ExpectedComponent> want = expected();
    INFO("registry: " << describe(got));
    INFO("expected: " << describe(want));
    CHECK(sameIdsAndKinds(got, want));
}

TEST_CASE("conformance: every component is available, valid, created and destroyed cleanly") {
    auto& lifetimes = dvv::LifetimeRegistry::instance();
    const auto registry = dvv::ComponentRegistry::create(allCapabilities());
    for (const dvv::ComponentStatus& status : registry->list()) {
        const std::string id(status.info.id);
        CAPTURE(id);
        if (status.unavailable.has_value()) {
            const std::string reason = status.unavailable->reason;
            FAIL_CHECK("unavailable: " << reason);
            continue;
        }
        REQUIRE(status.schema != nullptr);
        CHECK(status.schema->resolve({}).has_value());
        CHECK(!status.info.name.empty());
        CHECK(status.info.version >= 1);
        if (status.info.kind != dvv::Kind::Source) {
            // v1.0 has source components only; another kind needs its interface here (v1.x).
            FAIL_CHECK("no interface for kind " << std::string(dvv::kindName(status.info.kind)));
            continue;
        }

        const std::size_t live_before = lifetimes.liveCount();
        {
            auto created = registry->createComponent<dvv::media::ISourceProvider>(status.info.id);
            REQUIRE(created.has_value());
            REQUIRE(*created != nullptr);
            const auto& managed = dynamic_cast<const dvv::ManagedObject&>(**created);
            CHECK(managed.ownerId() == registry->objectId());
            CHECK(!(*created)->schemes().empty());
        }
        CHECK(lifetimes.liveCount() == live_before);
    }
}

TEST_CASE(
    "conformance: without capabilities every component is listed and either available or "
    "explains why") {
    const auto registry = dvv::ComponentRegistry::create(dvv::Capabilities{});
    const std::vector<dvv::ComponentStatus> statuses = registry->list();
    const std::vector<ExpectedComponent> got = listed(statuses);
    const std::vector<ExpectedComponent> want = expected();
    INFO("registry: " << describe(got));
    INFO("expected: " << describe(want));
    CHECK(sameIds(got, want));

    for (const dvv::ComponentStatus& status : statuses) {
        const std::string id(status.info.id);
        CAPTURE(id);
        // Available, or unavailable with a reason.
        if (status.unavailable.has_value()) {
            CHECK(!status.unavailable->reason.empty());
        }
        const bool m2_provider =
            std::ranges::any_of(M2_PROVIDERS, [&status](const ExpectedComponent& component) {
                return component.id == status.info.id;
            });
        if (m2_provider) {
            CHECK(status.available());
        }
        // Components that probe for a capability are unavailable without it and name it.
        for (const std::string_view needs : {"test.echo", "ffmpeg", "lavfi"}) {
            if (status.info.id == needs && cmakeHas(needs)) {
                CHECK(!status.available());
                if (status.unavailable.has_value()) {
                    CHECK(status.unavailable->reason.find(needs) != std::string::npos);
                }
            }
        }
    }
    // The M2 providers are listed even when the CMake set is empty (a dependent build with
    // DVV_TEST_DEPENDENCIES).
    for (const ExpectedComponent& provider : M2_PROVIDERS) {
        const std::string id(provider.id);
        CAPTURE(id);
        CHECK(std::ranges::any_of(statuses, [&provider](const dvv::ComponentStatus& status) {
            return status.info.id == provider.id;
        }));
    }
}

TEST_CASE("conformance: the ffmpeg provider is available with the FFmpeg capabilities") {
    if (!cmakeHas("ffmpeg")) {
        MESSAGE("the ffmpeg component is off (DVV_COMPONENT_FFMPEG)");
        return;
    }
    auto& lifetimes = dvv::LifetimeRegistry::instance();
    const auto registry =
        dvv::ComponentRegistry::create(dvv::Capabilities{"ffmpeg", "ffmpeg.lavfi"});
    const std::vector<dvv::ComponentStatus> statuses = registry->list();
    const auto status = std::ranges::find_if(
        statuses, [](const dvv::ComponentStatus& entry) { return entry.info.id == "ffmpeg"; });
    REQUIRE(status != statuses.end());
    CHECK(status->available());
    CHECK(status->info.kind == dvv::Kind::Source);

    const std::size_t live_before = lifetimes.liveCount();
    {
        auto created = registry->createComponent<dvv::media::ISourceProvider>("ffmpeg");
        REQUIRE_MESSAGE(created.has_value(), created.error().describe());
        REQUIRE(*created != nullptr);
        const auto& managed = dynamic_cast<const dvv::ManagedObject&>(**created);
        CHECK(managed.ownerId() == registry->objectId());
        CHECK(lifetimes.liveCount() == live_before + 1);
    }
    CHECK(lifetimes.liveCount() == live_before);
}

TEST_CASE("conformance: the lavfi provider needs the ffmpeg.lavfi capability") {
    if (!cmakeHas("lavfi")) {
        MESSAGE("the ffmpeg component is off (DVV_COMPONENT_FFMPEG)");
        return;
    }
    const auto find = [](const std::vector<dvv::ComponentStatus>& statuses) {
        const auto status = std::ranges::find_if(
            statuses, [](const dvv::ComponentStatus& entry) { return entry.info.id == "lavfi"; });
        REQUIRE(status != statuses.end());
        return *status;
    };
    {
        const auto registry = dvv::ComponentRegistry::create(dvv::Capabilities{"ffmpeg"});
        const dvv::ComponentStatus status = find(registry->list());
        CHECK_FALSE(status.available());
        REQUIRE(status.unavailable.has_value());
        if (status.unavailable) {
            CHECK(status.unavailable->reason.find("ffmpeg.lavfi") != std::string::npos);
        }
    }
    auto& lifetimes = dvv::LifetimeRegistry::instance();
    const auto registry =
        dvv::ComponentRegistry::create(dvv::Capabilities{"ffmpeg", "ffmpeg.lavfi"});
    const dvv::ComponentStatus status = find(registry->list());
    CHECK(status.available());
    CHECK(status.info.kind == dvv::Kind::Source);
    const std::size_t live_before = lifetimes.liveCount();
    {
        auto created = registry->createComponent<dvv::media::ISourceProvider>("lavfi");
        REQUIRE_MESSAGE(created.has_value(), created.error().describe());
        REQUIRE(*created != nullptr);
        const auto& managed = dynamic_cast<const dvv::ManagedObject&>(**created);
        CHECK(managed.ownerId() == registry->objectId());
        CHECK(lifetimes.liveCount() == live_before + 1);
    }
    CHECK(lifetimes.liveCount() == live_before);
}

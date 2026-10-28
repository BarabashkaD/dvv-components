#include "echo_provider.h"

#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/base/lifetime.h"
#include "dvv/base/task.h"
#include "dvv/media/source.h"
#include "dvv/media/source_factory.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_info.h"
#include "dvv/registry/component_registry.h"
#include "dvv/registry/parameters.h"

namespace dvv::components::testing {

namespace {

constexpr std::array<std::string_view, 1> SCHEMES{"test-echo"};

}  // namespace

TestEchoProvider::TestEchoProvider(CreationKey<ComponentRegistry> key,
                                   const ParameterValues& /*values*/) noexcept
    : Managed(std::move(key)) {}

ComponentInfo TestEchoProvider::info() noexcept {
    return {
        .id = "test.echo", .name = "Conformance test source", .kind = Kind::Source, .version = 1};
}

const ParameterSchema& TestEchoProvider::schema() {
    static const ParameterSchema SCHEMA;
    return SCHEMA;
}

std::expected<void, Unavailable> TestEchoProvider::probe(const Capabilities& caps) {
    return require(caps, {"test.echo"});
}

std::span<const std::string_view> TestEchoProvider::schemes() const noexcept {
    return SCHEMES;
}

media::Match TestEchoProvider::match(const media::SourceRequest& /*request*/,
                                     std::span<const std::byte> /*head*/) const {
    return media::Match{.confidence = 0, .guess = {}, .why = "test component"};
}

Task<Result<std::unique_ptr<media::ISource>>> TestEchoProvider::open(
    media::OpenTicket /*ticket*/, const media::SourceRequest& /*request*/,
    const media::FormatGuess& /*guess*/, const media::OpenContext& /*context*/) {
    co_return makeError(Errc::Unsupported, "test component");
}

}  // namespace dvv::components::testing

DVV_REGISTER(dvv::media::ISourceProvider, dvv::components::testing::TestEchoProvider);

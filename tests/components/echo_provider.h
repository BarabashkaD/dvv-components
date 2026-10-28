#pragma once

// The conformance test component test.echo (library test_echo): a Kind::Source provider for the
// scheme "test-echo" that needs the capability "test.echo", matches every request with confidence
// 0 and opens nothing. Built only when dvv-components is the top-level project, so it never reaches
// an application.

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string_view>

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

class TestEchoProvider final : public media::ISourceProvider, public Managed<TestEchoProvider> {
public:
    TestEchoProvider(CreationKey<ComponentRegistry> key, const ParameterValues& values) noexcept;

    static ComponentInfo info() noexcept;
    static const ParameterSchema& schema();
    static std::expected<void, Unavailable> probe(const Capabilities& caps);

    [[nodiscard]] std::span<const std::string_view> schemes() const noexcept override;
    [[nodiscard]] media::Match match(const media::SourceRequest& request,
                                     std::span<const std::byte> head) const override;
    [[nodiscard]] Task<Result<std::unique_ptr<media::ISource>>> open(
        media::OpenTicket ticket, const media::SourceRequest& request,
        const media::FormatGuess& guess, const media::OpenContext& context) override;
};

}  // namespace dvv::components::testing

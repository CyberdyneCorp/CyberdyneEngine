// SPDX-License-Identifier: MIT
// `integration.play_host_without_vulkan` — the play host as a build without the Vulkan renderer
// links it: play_host.cpp against presenter_unavailable.cpp (samples/play/CMakeLists.txt). That
// this binary links at all is most of the claim; the cases are what such a build does when run.

#include <cy/core/memory/system_allocator.h>
#include <cy/platform/sdl3_display_server.h>
#include <cy/test/test.h>

#include <string_view>

#include "play_host.h"
#include "presenter.h"

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

}  // namespace

CY_TEST_CASE("without the Vulkan renderer the presenter refuses to start, naming --no-draw") {
    cy::Sdl3DisplayServer display;
    cy::play::Presenter presenter(allocator());
    const cy::Status started =
        presenter.start(display, cy::kInvalidWindow, nullptr, cy::play::PresenterOptions{});
    CY_REQUIRE_FALSE(started.has_value());
    CY_CHECK(started.error().code == cy::ErrorCode::Unsupported);
    CY_CHECK(std::string_view(started.error().message).find("--no-draw") != std::string_view::npos);
    CY_CHECK(presenter.device() == nullptr);
    CY_CHECK_EQ(presenter.validation_errors(), 0U);
}

CY_TEST_CASE("without the Vulkan renderer the host opens a headless window and closes it") {
    cy::play::PlayHost host(allocator());
    cy::play::PlayOptions options;
    options.title = "play_host_without_vulkan";
    options.headless = true;
    const cy::Status opened = host.open(options);
    CY_REQUIRE_MESSAGE(opened.has_value(), (opened ? "" : opened.error().message));
    CY_CHECK(host.device() == nullptr);
    host.close();
}

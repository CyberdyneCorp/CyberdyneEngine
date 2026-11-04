// SPDX-License-Identifier: MIT
// presenter_unavailable.cpp — the presenter of a build without the Vulkan renderer (presenter.h).
//
// The play host owns a Presenter whatever the build, so the class is defined in every build: this
// one has no device, `start` refuses by name, and a game is played with `--no-draw`. Compiled in
// place of presenter.cpp (samples/play/CMakeLists.txt), never beside it.

#include "presenter.h"

namespace cy::play {

struct Presenter::State {};

// NOLINTBEGIN(readability-convert-member-functions-to-static) — the members presenter.h declares,
// defined without the state presenter.cpp's read: they cannot become static in one build only.

Presenter::Presenter(Allocator& allocator) noexcept : allocator_(&allocator) {}

Presenter::~Presenter() = default;

Status Presenter::start(DisplayServer& /*display*/, WindowId /*window*/, PlayUi* /*ui*/,
                        const PresenterOptions& /*options*/) noexcept {
    return fail(ErrorCode::Unsupported,
                "this build has no Vulkan renderer, so a played game cannot be drawn; "
                "--no-draw runs it without a picture");
}

rhi::SwapchainInfo Presenter::info() const noexcept {
    return rhi::SwapchainInfo{};
}

Expected<PresentResult, Error> Presenter::present(const PresentSource& /*source*/,
                                                  bool /*capture*/) noexcept {
    return fail(ErrorCode::Unsupported, "this build has no Vulkan renderer to present with");
}

Status Presenter::resize(Extent /*extent*/) noexcept {
    return ok();
}

u64 Presenter::validation_errors() const noexcept {
    return 0U;
}

const char* Presenter::device_name() const noexcept {
    return "";
}

void Presenter::release() noexcept {}

void Presenter::shutdown() noexcept {}

// NOLINTEND(readability-convert-member-functions-to-static)

}  // namespace cy::play

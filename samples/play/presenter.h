// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_PLAY_PRESENTER_H
#define CY_SAMPLE_PLAY_PRESENTER_H
// presenter.h — the window's half of a played frame: a Vulkan device, the window's surface and
// swapchain, and each frame the game's picture scaled into the swapchain image, the interface drawn
// over it at the window's own resolution, and the image presented.
//
// THE GAME DRAWS INTO ITS OWN IMAGE, AT ITS OWN EXTENT, on this presenter's device. Its frame —
// the GI, the upscaler, the shadow maps — is sized once, at start; the window is resized by the
// player whenever they like. So the game's picture is a texture this presenter samples, and only
// the swapchain and the interface follow the window: a resize recreates those two and nothing of
// the game's (`resize`). The picture keeps its aspect inside a letterbox.
//
// ONE FRAME (present):
//
//   begin the device frame; acquire the next swapchain image   (out of date: resize, skip)
//   graph: the picture imported as the game left it (sampled)
//          "present"   the full-screen triangle into the letterbox, the bars cleared black
//          "ui"        CyberUI's stream over it, at the swapchain's extent (play_ui.h)
//          "capture"   the swapchain image copied to the host, when asked — what a video is
//          "present"   the image handed back to the presentation engine
//   execute, waiting on the acquire and signalling the present; present; wait for the device
//
// V-SYNC is the presentation mode: FIFO with it, immediate without — and the swapchain reports
// what it got, which `info()` passes on.
//
// Vulkan only: Metal and D3D12 compile the shader (`just build-shaders --strict`) and have no
// surface in this host yet. Without a device, `start` answers Unavailable naming the machine, as
// the samples' `--render` does.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/platform/display_server.h>

#include <memory>
#include <vector>

namespace cy::play {

class PlayUi;

struct PresenterOptions {
    bool vsync = true;
    bool validation = false;
    const char* application = "cy_play";
};

/// The game's picture this frame: the texture its frame wrote, its extent, and what it was left in.
struct PresentSource {
    rhi::TextureHandle texture;
    rhi::Format format = rhi::Format::Rgba8Unorm;
    u32 width = 0;
    u32 height = 0;
    rhi::ImageUse use = rhi::ImageUse::SampledRead;
};

/// What one `present` did.
struct PresentResult {
    bool presented = false;
    /// The swapchain was out of date (a resize) and was recreated; nothing was presented.
    bool resized = false;
    /// The CPU's milliseconds from the acquire to the device being idle again — the presenter's
    /// share of a frame, v-sync wait included when FIFO blocks.
    f64 present_ms = 0.0;
};

class Presenter {
public:
    explicit Presenter(Allocator& allocator) noexcept;
    ~Presenter();

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    /// The device, the surface of `window`, the swapchain at the window's size, the present
    /// pipeline and, with `ui`, the interface renderer.
    [[nodiscard]] Status start(DisplayServer& display, WindowId window, PlayUi* ui,
                               const PresenterOptions& options) noexcept;
    /// The device a game's renderer draws with. Null before `start`.
    [[nodiscard]] rhi::Device* device() const noexcept { return device_; }
    [[nodiscard]] rhi::SwapchainInfo info() const noexcept;

    /// Present `source`, the interface over it; with `capture`, copy the presented image to
    /// `captured()` as RGBA8 rows.
    [[nodiscard]] Expected<PresentResult, Error> present(const PresentSource& source,
                                                         bool capture) noexcept;
    /// The window is now `extent`: the swapchain and the interface renderer follow it.
    [[nodiscard]] Status resize(Extent extent) noexcept;

    /// The last captured image, RGBA8, `info().extent` wide.
    [[nodiscard]] const std::vector<u32>& captured() const noexcept { return captured_; }
    [[nodiscard]] u32 captured_width() const noexcept { return captured_width_; }
    [[nodiscard]] u32 captured_height() const noexcept { return captured_height_; }
    [[nodiscard]] u64 validation_errors() const noexcept;
    [[nodiscard]] const char* device_name() const noexcept;

    /// Release everything but the device, which a game's renderer may still hold resources on;
    /// `shutdown` releases the device last. The destructor calls both.
    void release() noexcept;
    void shutdown() noexcept;

    struct State;

private:
    Allocator* allocator_;
    std::unique_ptr<State> state_;
    rhi::Device* device_ = nullptr;
    std::vector<u32> captured_;
    u32 captured_width_ = 0;
    u32 captured_height_ = 0;
};

}  // namespace cy::play

#endif  // CY_SAMPLE_PLAY_PRESENTER_H

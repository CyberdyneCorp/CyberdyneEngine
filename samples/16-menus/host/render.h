// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_MENUS_RENDER_H
#define CY_SAMPLE_MENUS_RENDER_H
// render.h — the menus drawn on a device, at the display settings the game applied.
//
//   cy_sample_menus --render <dir>    every frame drawn; a script's `shot <name>` writes it
//   --shot-size <w>x<h>               the size a shot is written at, averaged down from the output
//   --record                          every frame written too, as frame-00000.png onwards: a video
//
// The frame is the pipeline suites' one scene (`pipeline_test::FrameScene`) with the interface
// over it at the frame's `UiAndDebug` stage, drawn by `ui::render::UiRenderer` from the host's
// store. THE SETTINGS ARE THE RENDERER'S: when the game applies new display settings
// (`VideoSettingsAdapter::revision` moves) the frame is made again at the new OUTPUT resolution,
// rendered at the upscaler's INTERNAL extent and reconstructed by `upscale::TemporalUpscalePass`
// (native is the scene's own antialiasing), with bloom by the quality preset — off at Low, a
// four-level chain at Medium, six at High and Ultra. The interface scales with the output: it is
// designed at 1280x720.
//
// What it prints at the end, one line a test reads: the frames drawn, the mean and worst wall time
// of a frame (game, interface and device together), the interface's own submit time, and every
// extent the frame was drawn at.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>

#include <memory>
#include <string>
#include <vector>

namespace sample::menus {

class MenusHost;

class MenusRenderer {
public:
    explicit MenusRenderer(cy::Allocator& allocator) noexcept;
    ~MenusRenderer();

    MenusRenderer(const MenusRenderer&) = delete;
    MenusRenderer& operator=(const MenusRenderer&) = delete;

    /// A Vulkan device, and the frame at the host's display settings. Unavailable, naming it, on a
    /// machine with no device.
    /// Frames are written at `shot_width` x `shot_height`, averaged down from the output — a
    /// golden a repository can hold — or at the output's own size when either is 0.
    [[nodiscard]] cy::Status start(MenusHost& host, std::string directory, cy::u32 shot_width = 0,
                                   cy::u32 shot_height = 0) noexcept;
    /// Draw the frame the host just ran, and write it under each of `shots`' names.
    [[nodiscard]] cy::Status frame(MenusHost& host, const std::vector<std::string>& shots) noexcept;
    void print_report() const noexcept;

private:
    struct State;
    cy::Allocator* allocator_;
    std::unique_ptr<State> state_;
};

}  // namespace sample::menus

#endif  // CY_SAMPLE_MENUS_RENDER_H

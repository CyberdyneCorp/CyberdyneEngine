// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_THIRD_PERSON_RENDER_H
#define CY_SAMPLE_THIRD_PERSON_RENDER_H
// render.h — the third-person run drawn by the engine's frame.
//
//   cy_sample_third-person --render <dir>
//
// Everything is the engine's own frame, as samples/13-rts-api/host/map_render.h draws the RTS test
// map: `FrameAssembly` and `FrameRecorder` with the standard material and its base-colour texture
// table, the cooked hillside (its terrain and painted layers, its foliage, its kit), the
// courtyard's pieces, and the hero skinned on the device by `skinning::SkinnedScene` from the
// matrices the engine's animation system published — the blend space's pose, feet placed. Lit by
// the sun's 2048-texel shadow map with the percentage-closer soft filter, ambient occlusion
// (`occlusion::AmbientOcclusionPass`) and the device GI over the level's `IlluminationSystem`
// (`gi_gpu::GpuGiFrame`), and drawn below the output and reconstructed by the temporal upscaler
// (`upscale::TemporalUpscalePass`, Quality by default).
//
// The camera is the game's: the rig the Swift camera director frames, pulled in by its collision
// node. The shadow map follows the hero.
//
// What it prints: one line with the frame's wall time over the timed frames — recorded, submitted
// and waited for — the worst frame, the halves, the GI's device time, and what the frame drew. It
// writes the stills listed in render.cpp (`third-person-<frame>.png`), and every frame to
// `--video <dir>` when asked.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/upscale/upscale.h>

#include <memory>
#include <string>

namespace sample::rts::map {
struct MapContent;
}

namespace sample::tps {

class ThirdPersonHost;

struct RenderOptions {
    /// Where the stills are written.
    std::string directory;
    /// Every `video_every`th frame, as `frame-<n>.png`, for a video. Empty writes none.
    std::string video_directory;
    cy::u32 video_every = 1;
    /// A still every `stills_every` frames besides the listed ones; zero for the listed ones only.
    cy::u32 stills_every = 0;
    cy::u32 width = 1920;
    cy::u32 height = 1080;
    /// The frame's features. Each off is that feature's control.
    bool gi = true;
    bool ambient_occlusion = true;
    bool shadows = true;
    /// The temporal upscaler and its mode. Off draws at the output extent with temporal
    /// antialiasing, the frame before it.
    bool upscale = true;
    cy::rendering::upscale::UpscaleMode upscale_mode = cy::rendering::upscale::UpscaleMode::Quality;
    /// The graphics API's validation layer. Off is for measuring what a frame costs.
    bool validation = true;
    /// PLAYED: draw on this device — the play host's, whose swapchain presents the picture — and
    /// leave each frame's output sampled for it, writing no stills. The renderer does not own it.
    cy::rhi::Device* device = nullptr;
};

/// The frame's numbers, over the timed frames.
struct RenderReport {
    cy::u64 frames = 0;
    cy::u64 timed = 0;
    cy::f64 frame_ms = 0.0;
    cy::f64 worst_ms = 0.0;
    cy::f64 game_ms = 0.0;
    cy::f64 record_ms = 0.0;
    cy::f64 device_wait_ms = 0.0;
    cy::f64 gi_device_ms = 0.0;
    cy::u32 render_width = 0;
    cy::u32 render_height = 0;
    cy::u32 draws = 0;
    cy::u32 skinned_draws = 0;
    cy::u32 instances = 0;
    cy::u32 triangles = 0;
    cy::u32 plants = 0;
    cy::u32 stills = 0;
    cy::u32 validation_errors = 0;
};

class Renderer {
public:
    explicit Renderer(cy::Allocator& allocator) noexcept;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// A Vulkan device, the level's geometry, materials and textures made resident, the hero's
    /// skinned mesh, and the frame's stages. Unavailable, naming it, where the machine has no
    /// device. `content` is copied to the device and not kept.
    [[nodiscard]] cy::Status start(ThirdPersonHost& host,
                                   const sample::rts::map::MapContent& content,
                                   const RenderOptions& options) noexcept;
    /// Draw the frame `host` just ran.
    [[nodiscard]] cy::Status frame(ThirdPersonHost& host) noexcept;
    [[nodiscard]] const RenderReport& report() const noexcept;
    void print_report() const noexcept;

    /// The picture the last frame drew, sampled-ready when played: its texture and extent.
    [[nodiscard]] cy::rhi::TextureHandle output() const noexcept;
    [[nodiscard]] cy::u32 width() const noexcept;
    [[nodiscard]] cy::u32 height() const noexcept;

private:
    struct State;
    cy::Allocator* allocator_;
    std::unique_ptr<State> state_;
};

}  // namespace sample::tps

#endif  // CY_SAMPLE_THIRD_PERSON_RENDER_H

// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_FIRST_PERSON_RENDER_H
#define CY_SAMPLE_FIRST_PERSON_RENDER_H
// render.h — the first-person session drawn every frame on a device, through the engine's own
// frame.
//
//   cy_sample_first-person --render <dir>     1920x1080, the temporal upscaler at Quality
//
// Everything is `FrameAssembly` and `FrameRecorder` with the standard material: the arena's boxes,
// the props where their bodies are and the targets where the AI put them; the sun's shadow map with
// the percentage-closer soft filter; ambient occlusion; the device GI over the arena's
// `IlluminationSystem` (illumination.h); the scorch marks the game spawned (ABI 1.15) applied by
// the forward pass from the frame's decal table; the muzzle bursts and sparks the game played
// (ABI 1.15) simulated by `cy::vfx` and drawn by the particle renderer in the transparent stage;
// the muzzle flashes the game spawned (ABI 1.11) lighting the frame as lights; the arms and the
// weapon drawn through the WEAPON VIEW the game configured (ABI 1.13) — their own lens, never into
// a wall — and the temporal upscaler reconstructing the 1280x720 render extent to the 1080p output.
//
// What it prints, one line `tests/test_first_person_render.cpp` reads: the frame's wall time over
// the timed frames (recording, submission and the wait for the device), the worst, the device's own
// time from the first pass to the post-process by timestamps, the GI's device time, what the frame
// drew, and how many validation errors the layer reported. It writes the stills at `kStills`
// (`fps-<frame>.png`) and, with `--video`, every n-th frame (`frame-<n>.png`).

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>

#include <memory>
#include <string>

namespace sample::fps {

class FpsHost;

struct RenderOptions {
    std::string directory;
    std::string video_directory;
    cy::u32 video_every = 1;
    cy::u32 width = 1920;
    cy::u32 height = 1080;
    /// `off`, or an `upscale::UpscaleMode` name: the scene below the output, reconstructed.
    std::string upscale = "quality";
    /// The frame's features. Each off is that feature's control.
    bool gi = true;
    bool ambient_occlusion = true;
    bool shadows = true;
    bool weapon_view = true;
    bool decals = true;
    bool particles = true;
    bool flashes = true;
    /// The graphics API's validation layer, on unless a run measures what a frame costs.
    bool validation = true;
    /// PLAYED: draw on this device — the play host's, whose swapchain presents the picture — and
    /// leave each frame's output sampled for it, writing no stills. The renderer does not own it.
    cy::rhi::Device* device = nullptr;
};

/// The frames the stills are written at: the walk to the beam, the first shot on T1, the crate,
/// the wall and the floor struck, T0 hit down the sights, and the aftermath.
inline constexpr cy::u64 kStills[] = {150, 318, 396, 415, 428, 450, 600};

class FpsRenderer {
public:
    explicit FpsRenderer(cy::Allocator& allocator) noexcept;
    ~FpsRenderer();

    FpsRenderer(const FpsRenderer&) = delete;
    FpsRenderer& operator=(const FpsRenderer&) = delete;

    /// A Vulkan device, the arena's geometry and materials, and the frame's stages. Unavailable,
    /// naming it, where the machine has no device.
    [[nodiscard]] cy::Status start(FpsHost& host, const RenderOptions& options) noexcept;
    /// Draw the frame `host` just ran.
    [[nodiscard]] cy::Status frame(FpsHost& host) noexcept;
    void print_report() const noexcept;

    /// The picture the last frame drew, sampled-ready when played: its texture and extent.
    [[nodiscard]] cy::rhi::TextureHandle output() const noexcept;
    [[nodiscard]] cy::u32 width() const noexcept;
    [[nodiscard]] cy::u32 height() const noexcept;
    /// The last timed frame's milliseconds: its recording, submission and the wait for the device.
    [[nodiscard]] cy::f64 last_frame_ms() const noexcept;

private:
    struct State;
    cy::Allocator* allocator_;
    std::unique_ptr<State> state_;
};

}  // namespace sample::fps

#endif  // CY_SAMPLE_FIRST_PERSON_RENDER_H

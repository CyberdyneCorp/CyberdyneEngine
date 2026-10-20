// SPDX-License-Identifier: MIT
#pragma once
// samples/12-beauty — the shot's global illumination. Issue #35, stages 4 and 5.
//
// ================================================================================================
// WHAT `--gi on` DRAWS
// ================================================================================================
//
// The published still lights every surface's shadowed side with one number: the sky's mean
// radiance, weighted by how far the normal faces up. That is the M11.c provenance's own words — "a
// one-bounce approximation ... no global illumination pass ran" — and it is why the shadowed faces
// of the colonnade are a flat grey and the ground under a pillar is lit by a sky it cannot see.
//
// With `--gi on` the shot is lit by the engine's GI, every part of it the engine's:
//
//   the GI scene   built from the shot's own meshes and instances (`gi::append_mesh_cards`,
//                  `gi::MeshDistanceField`): a card every half metre over every surface, and each
//                  mesh's signed distance grid placed by its instance's transform into the
//                  clipmapped field. The cards' albedo is the material's mean — its albedo map's
//                  mean in linear light times the authored base colour, less its metallic share —
//                  because a card is half a metre of surface and the map's detail is below it.
//   the caches     `gi::IlluminationSystem`: the surface cache's cards lit by the shot's sun
//   through
//                  the field and bounced card to card (`SurfaceBounce::Cards`), the radiance
//                  cache's probes gathered from them, and the sky term fitted to the engine's own
//                  atmosphere.
//   the frame      `gi_gpu::GpuScreenResolve`, the frame's `ScreenSpaceGi` stage: the depth and
//                  normal prepass unpacked, a final-gather ray a pixel through the hardware tier
//                  where the device has ray queries (the software field otherwise), combined with
//                  the probes by confidence, denoised, and published as two targets — indirect
//                  diffuse and indirect specular — which `sceneFragmentGi` reads where the flat
//                  ambient sat.
//
// The sun, the shadows, the sky dome and everything after the scene pass are the published frame's.
// With `--gi off`, the default, none of this is created and the frame is M11.c's, byte for byte —
// `just capture-beauty-gi` checks that against the committed still.
//
// ================================================================================================
// THE GROUND TRUTH
// ================================================================================================
//
// `measure_reference` path traces the same scene — the same meshes, through
// `lightmap_bake::MeshSceneTracer`'s exact triangles rather than the field the real-time tiers
// trace — at pixels spread over the frame, and reports how far the published indirect diffuse is
// from it, and how far the flat ambient it replaces was.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/import/mesh.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi_gpu/gpu_resolve.h>
#include <cy/rendering/graph/graph.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "shot.h"

namespace cy::sample::beauty {

/// What the GI is lit by: the shot's sun and the engine's sky, as `build_sky` answered them.
struct BeautyGiLight {
    /// Towards the sun.
    Vec3 to_sun{0.0F, 1.0F, 0.0F};
    Vec3 sun_illuminance{0.0F, 0.0F, 0.0F};
    f32 sun_angular_radius = 0.004625F;
    /// The sky's radiance straight up and averaged around the horizon, and its mean.
    Vec3 zenith{0.0F, 0.0F, 0.0F};
    Vec3 horizon{0.0F, 0.0F, 0.0F};
    Vec3 mean_sky{0.0F, 0.0F, 0.0F};
};

/// One frame's view, in the space the geometry is baked into.
struct BeautyGiView {
    Vec3 eye{0.0F, 0.0F, 0.0F};
    Mat4 camera = Mat4::identity();
    Mat4 projection = Mat4::identity();
    Mat4 world_to_clip = Mat4::identity();
};

class BeautyGi {
public:
    explicit BeautyGi(Allocator& allocator) noexcept;
    ~BeautyGi();

    BeautyGi(const BeautyGi&) = delete;
    BeautyGi& operator=(const BeautyGi&) = delete;
    BeautyGi(BeautyGi&&) = delete;
    BeautyGi& operator=(BeautyGi&&) = delete;

    /// Build the GI scene from the shot's meshes and instances, configure the caches, and create
    /// the device half at the frame's extent.
    [[nodiscard]] Status build(const Shot& shot,
                               const std::vector<std::pair<std::string, import::MeshData>>& meshes,
                               const BeautyGiLight& light, const BeautyGiSettings& settings,
                               rhi::Device& device, u32 width, u32 height) noexcept;
    void destroy() noexcept;

    /// Frame `frame`: the caches updated on the host, and the resolve's stage attached to the
    /// assembly's view and sinks.
    [[nodiscard]] Status prepare(rendering::RenderGraph& graph,
                                 rendering::assembly::AssemblyView& view,
                                 rendering::assembly::FrameSinks& sinks, const BeautyGiView& frame,
                                 u64 frame_index) noexcept;

    /// With `BeautyGiSettings::readback`: whether the frames prepared from now on copy the GI's
    /// buffers back for the reference (`GpuScreenResolve::set_readback_frames`).
    void set_readback_frames(bool copy) noexcept;
    /// Whether the frames prepared from now on leave the host's caches as they are: the GI's
    /// device stages run as before over the cache the last update left. For timing a converged
    /// still view, where an update changes nothing and costs tens of milliseconds of host time.
    void set_hold_cache(bool hold) noexcept { hold_cache_ = hold; }

    [[nodiscard]] rhi::TextureViewHandle diffuse_view() const noexcept;
    [[nodiscard]] rhi::TextureViewHandle specular_view() const noexcept;
    /// The device time of the last frame's resolve, per stage, and the host time of its cache
    /// update.
    [[nodiscard]] rendering::gi_gpu::GpuResolveTimes last_times() const noexcept;
    [[nodiscard]] f64 last_update_ms() const noexcept { return update_ms_; }
    /// What converged mode took before the first frame, when the settings asked for it.
    [[nodiscard]] u32 converged_frames() const noexcept { return converged_frames_; }
    [[nodiscard]] f64 converge_ms() const noexcept { return converge_ms_; }
    [[nodiscard]] const char* tier_name() const noexcept;
    [[nodiscard]] u32 card_count() const noexcept;
    /// Cards an emissive decal lit (`BeautyGiSettings::emissive_decals`).
    [[nodiscard]] u32 emissive_card_count() const noexcept;
    [[nodiscard]] u32 placements() const noexcept;

    /// After a frame rendered with `readback`: the path tracer at `pixels` pixels spread over the
    /// frame, `samples` paths of `bounces` each.
    [[nodiscard]] Expected<BeautyGiReference, Error> measure_reference(u32 pixels, u32 samples,
                                                                       u32 bounces) noexcept;

    /// After a frame rendered with `readback` on the full-resolution grid: the path tracer's
    /// indirect diffuse at every `step`-th pixel each way, `samples` paths of `bounces` each, over
    /// the exact triangles, upsampled to every pixel; and the frame's published GI beside it.
    /// `threads` tracers run at once, each over its own copy of the triangles.
    [[nodiscard]] Status reference_image(u32 step, u32 samples, u32 bounces, u32 threads,
                                         BeautyGiReferenceImage& out) noexcept;

private:
    struct State;
    Allocator* allocator_;
    std::unique_ptr<State> state_;
    f64 update_ms_ = 0.0;
    bool hold_cache_ = false;
    u32 converged_frames_ = 0;
    f64 converge_ms_ = 0.0;
};

}  // namespace cy::sample::beauty

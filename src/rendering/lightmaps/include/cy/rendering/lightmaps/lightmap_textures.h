// SPDX-License-Identifier: MIT
#pragma once
// A baked lightmap in the forward frame: the atlas planes as textures the forward pass samples at
// the ambient term, the view-block words that describe them, and the one rule for which ambient
// source a surface takes.
//
// `rendering-global-illumination` — "Lightmap baking". The bake — packing, tracing, denoising,
// dilation, seams — is `lightmap_bake::bake_lightmaps`, which has no device. This is the half that
// has one, and it is a separate module for the reason `src/rendering/light_probes/` is: the bake
// runs every case headless, and `cy::rendering-pipeline` owns the frame and knows nothing of GI.
//
// ================================================================================================
// WHERE A DRAW FINDS ITS RECTANGLE
// ================================================================================================
//
// In `GpuDrawInstance::gi_address`, which the forward draw list has always carried and nothing
// filled: `BakedLightmap::addresses[instance]` is the word for a scene instance, and a caller puts
// it in `DrawSurface::gi_address` (or the draw record it uploads). The mesh's UV2 reaches the
// vertex shader as the forward passes' fourth stream, `GeometrySource::lightmap_uvs`. A draw whose
// address is zero is lit exactly as it was.
//
// ================================================================================================
// THE TEXTURES
// ================================================================================================
//
// One `Rgba16Sfloat` texture per plane — one for irradiance, two for directional, three for SH L1 —
// each `page_size` wide and `page_size * pages` tall, the texels exactly as the bake rounded them,
// and one more for the SHADOW MASK when the bake wrote one (a channel per stationary light). Half
// floats because every device filters them, and the frame's one sampler is linear: a texel is read
// bilinearly and the gutter the atlas leaves around every rectangle is what keeps that read inside
// its own object. Uploaded only when the lightmap changed, counted by `uploads()`.
//
// EVERY LEVEL OF THE MIP CHAIN: `lightmap_bake::build_lightmap_mips` filtered the levels the gutter
// and the chart padding protect, per chart, and each texture carries all of them, so a minified
// surface reads a coarse level that holds only its own chart.
//
// ================================================================================================
// THE LIGHTS
// ================================================================================================
//
// `write_lightmaps` also tells the frame which of ITS lights the lightmap speaks for, matching the
// bake's `gi::GiLight::id`s against the stable ids of the frame's lights: a stationary light's
// direct term on a lightmapped surface goes through its shadow-mask channel, and a light whose
// direct term the texels already hold (`BakedLightmap::direct_lights`: every static light) is not
// shaded there at all. Its intensity and colour stay the frame's, so a stationary light dimmed at
// run time is dimmed with its baked shadow intact.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/lightmap_bake/bake.h>
#include <cy/rendering/pipeline/frame_pipelines.h>
#include <cy/servers/render/types.h>

namespace cy::rendering::lightmaps {

inline constexpr u32 kMaxPlanes = 3;

class LightmapTextures {
public:
    LightmapTextures() noexcept = default;
    ~LightmapTextures();

    LightmapTextures(const LightmapTextures&) = delete;
    LightmapTextures& operator=(const LightmapTextures&) = delete;
    LightmapTextures(LightmapTextures&&) = delete;
    LightmapTextures& operator=(LightmapTextures&&) = delete;

    /// Remember the device. Nothing is created until the first `upload`.
    void initialize(rhi::Device& device, Allocator& allocator) noexcept;
    void shutdown() noexcept;

    /// Copy the lightmap's planes to the device, in a device frame of its own: call it OUTSIDE the
    /// host's `begin_frame`/`end_frame`, as `ProbeVolumeTexture::upload` is. Recreates the textures
    /// when the atlas's size or encoding changed; copies nothing when `lightmap` is the one it last
    /// copied, unchanged. KNOWING THAT IS NOT FREE: it hashes every texel of every level of every
    /// plane and of the mask, about half a second for a 2048 x 2048 SH L1 atlas with its mask and
    /// chain on an M2 Max. Call it when a lightmap was baked or loaded, not every frame.
    [[nodiscard]] Status upload(const lightmap_bake::BakedLightmap& lightmap) noexcept;

    [[nodiscard]] u32 planes() const noexcept { return planes_; }
    /// The (slot, view) pair the frame's set 0 needs for one plane, at a slot the caller chose.
    [[nodiscard]] pipeline::MaterialTextureSlot slot(u32 plane,
                                                     rhi::BindlessIndex index) const noexcept;
    /// Whether the lightmap carried a shadow mask, and the (slot, view) pair for it.
    [[nodiscard]] bool has_shadow_mask() const noexcept { return masked_; }
    [[nodiscard]] pipeline::MaterialTextureSlot shadow_mask_slot(
        rhi::BindlessIndex index) const noexcept;
    /// Levels each texture holds: the base and the chain below it.
    [[nodiscard]] u32 mip_levels() const noexcept { return mip_levels_; }
    [[nodiscard]] bool ready() const noexcept { return planes_ != 0; }
    [[nodiscard]] u32 uploads() const noexcept { return uploads_; }
    /// Bytes the planes and the shadow mask occupy on the device, every level.
    [[nodiscard]] u64 device_bytes() const noexcept;
    /// The texture of one plane — `kMaxPlanes` for the shadow mask — for a caller that reads a
    /// level back.
    [[nodiscard]] rhi::TextureHandle texture(u32 plane) const noexcept;

private:
    [[nodiscard]] Status recreate(u32 width, u32 height, u32 planes, bool masked,
                                  u32 levels) noexcept;
    [[nodiscard]] Status make_texture(u32 index) noexcept;
    void release_textures() noexcept;

    rhi::Device* device_ = nullptr;
    Allocator* allocator_ = nullptr;
    /// The planes, then the shadow mask at `kMaxPlanes`.
    rhi::TextureHandle textures_[kMaxPlanes + 1];
    rhi::TextureViewHandle views_[kMaxPlanes + 1];
    u32 width_ = 0;
    u32 height_ = 0;
    u32 planes_ = 0;
    bool masked_ = false;
    /// The base level and every level below it: `BakedLightmap::mip_levels + 1`.
    u32 mip_levels_ = 0;
    /// What was last copied, so an unchanged lightmap costs no transfer.
    u64 fingerprint_ = 0;
    u32 uploads_ = 0;
};

/// Which ambient source a surface takes in the frame.
enum class AmbientSource : u8 {
    /// The frame's flat ambient: nothing baked covers the surface.
    Sky = 0,
    IrradianceVolume,
    Lightmap,
};

/// The frame's ambient source for a surface, READ OFF `gi::exclusion_for()`.
///
/// The frame's ambient is one source, where the resolve's is a confidence-weighted mean of several:
/// the lightmap and the volume each already hold the sky and the bounces, so the frame takes the
/// highest-priority source the mask admits — lightmap, then volume, then the sky — rather than a
/// second copy of the exclusion table. `Probe` excludes lightmaps and so draws none, and every mode
/// that admits a lightmap excludes the volume for the surface that has one.
[[nodiscard]] AmbientSource frame_ambient_source(gi::GiMode mode, bool surface_has_lightmap,
                                                 bool surface_has_irradiance_volume) noexcept;

/// The frame's set 0 slots a lightmap is read through: one per plane, and the shadow mask's.
struct LightmapSlots {
    rhi::BindlessIndex planes[kMaxPlanes] = {
        pipeline::kNoMaterialTexture, pipeline::kNoMaterialTexture, pipeline::kNoMaterialTexture};
    /// `kNoMaterialTexture` when the lightmap has no mask, or the caller bound none.
    rhi::BindlessIndex shadow_mask = pipeline::kNoMaterialTexture;
};

/// Write the lightmap's words into the frame's view block: its planes' slots, its encoding, its
/// page layout, its shadow mask, and which of the frame's lights it speaks for. Writes nothing —
/// the frame draws every surface as it was — when `mode` does not admit lightmaps.
///
/// `frame_light_ids` is the stable id of every light the frame shades, in the frame's order
/// (`AssemblyView::lights`, `render::LightDescription::stable_id`); each is matched against the
/// bake's `gi::GiLight::id`s. Fails with `InvalidArgument` when a plane has no slot, when a
/// stationary light's mask has no slot, or when a light whose direct term is baked sits past the
/// first `pipeline::kMaxLightmapDirectLights` of the frame's lights — shading it twice is the
/// failure this refuses rather than draws. A refusal writes nothing: the view is left as it was.
[[nodiscard]] Status write_lightmaps(const LightmapSlots& slots,
                                     const lightmap_bake::BakedLightmap& lightmap, gi::GiMode mode,
                                     Span<const u64> frame_light_ids,
                                     pipeline::FrameViewData& view) noexcept;

/// Switch the frame to the lightmap texel-density view: every lightmapped surface drawn as a
/// checker of its own texels, green at `target_texels_per_metre`, toward blue at half of it and
/// red at twice it; every other surface flat grey. The engine half of the editor's
/// `viewport.view-mode.lightmap-density`. Needs `write_lightmaps` for the same frame.
void write_lightmap_density_view(f32 target_texels_per_metre,
                                 pipeline::FrameViewData& view) noexcept;

/// The engine half of a viewport's debug view, for the views this module draws: writes the
/// density view's words for `render::DebugViewMode::LightmapDensity` and returns true; writes
/// nothing and returns false for every other mode.
[[nodiscard]] bool write_lightmap_debug_view(render::DebugViewMode mode,
                                             f32 target_texels_per_metre,
                                             pipeline::FrameViewData& view) noexcept;

}  // namespace cy::rendering::lightmaps

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
// each `page_size` wide and `page_size * pages` tall, the texels exactly as the bake rounded them.
// Half floats because every device filters them, and the frame's one sampler is linear: a texel is
// read bilinearly and the gutter the atlas leaves around every rectangle is what keeps that read
// inside its own object. Uploaded only when the lightmap changed, counted by `uploads()`.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/lightmap_bake/bake.h>
#include <cy/rendering/pipeline/frame_pipelines.h>

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
    /// copied, unchanged.
    [[nodiscard]] Status upload(const lightmap_bake::BakedLightmap& lightmap) noexcept;

    [[nodiscard]] u32 planes() const noexcept { return planes_; }
    /// The (slot, view) pair the frame's set 0 needs for one plane, at a slot the caller chose.
    [[nodiscard]] pipeline::MaterialTextureSlot slot(u32 plane,
                                                     rhi::BindlessIndex index) const noexcept;
    [[nodiscard]] bool ready() const noexcept { return planes_ != 0; }
    [[nodiscard]] u32 uploads() const noexcept { return uploads_; }
    /// Bytes the planes occupy on the device.
    [[nodiscard]] u64 device_bytes() const noexcept {
        return u64{width_} * height_ * planes_ * 8U;
    }

private:
    [[nodiscard]] Status recreate(u32 width, u32 height, u32 planes) noexcept;
    void release_textures() noexcept;

    rhi::Device* device_ = nullptr;
    Allocator* allocator_ = nullptr;
    rhi::TextureHandle textures_[kMaxPlanes];
    rhi::TextureViewHandle views_[kMaxPlanes];
    u32 width_ = 0;
    u32 height_ = 0;
    u32 planes_ = 0;
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

/// Write the lightmap's words into the frame's view block: its planes' slots, its encoding and its
/// page layout. Writes nothing — the frame draws every surface as it was — when `mode` does not
/// admit lightmaps. `slots` holds one slot per plane of the lightmap.
[[nodiscard]] Status write_lightmaps(Span<const rhi::BindlessIndex> slots,
                                     const lightmap_bake::BakedLightmap& lightmap,
                                     gi::GiMode mode, pipeline::FrameViewData& view) noexcept;

}  // namespace cy::rendering::lightmaps

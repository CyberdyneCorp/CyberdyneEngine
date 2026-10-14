// SPDX-License-Identifier: MIT
#pragma once
// A GI debug view in the forward frame: the image `gi::render_gi_debug_view` drew for the frame's
// camera, as a texture the forward pass draws wherever the image is opaque.
//
// `rendering-global-illumination` — "GI debug visualisation". The views are drawn by
// `cy::rendering-gi`, which has no device, from the GI state they show; this is the half with one,
// here for the reason the lightmap's textures are: `cy::rendering-pipeline` owns the frame and
// knows nothing of GI. `Rgba8Unorm`, the frame's size, sampled at texel centres by
// `cy/frame.slang`'s `kCyGiDebugOverlayView` branch, so a fragment reads exactly its own texel.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/pipeline/frame_pipelines.h>
#include <cy/servers/render/types.h>

namespace cy::rendering::lightmaps {

class GiDebugOverlayTexture {
public:
    GiDebugOverlayTexture() noexcept = default;
    ~GiDebugOverlayTexture();

    GiDebugOverlayTexture(const GiDebugOverlayTexture&) = delete;
    GiDebugOverlayTexture& operator=(const GiDebugOverlayTexture&) = delete;
    GiDebugOverlayTexture(GiDebugOverlayTexture&&) = delete;
    GiDebugOverlayTexture& operator=(GiDebugOverlayTexture&&) = delete;

    void initialize(rhi::Device& device, Allocator& allocator) noexcept;
    void shutdown() noexcept;

    /// Copy one view's image, `width * height` `Rgba8Unorm` texels from the top-left, in a device
    /// frame of its own: call it outside the host's `begin_frame`/`end_frame`. Recreates the
    /// texture when the size changed.
    [[nodiscard]] Status upload(Span<const u32> texels, u32 width, u32 height) noexcept;

    [[nodiscard]] pipeline::MaterialTextureSlot slot(rhi::BindlessIndex index) const noexcept {
        return pipeline::MaterialTextureSlot{index, view_};
    }
    [[nodiscard]] bool ready() const noexcept { return !view_.is_null(); }

private:
    [[nodiscard]] Status recreate(u32 width, u32 height) noexcept;
    void release_texture() noexcept;

    rhi::Device* device_ = nullptr;
    Allocator* allocator_ = nullptr;
    rhi::TextureHandle texture_;
    rhi::TextureViewHandle view_;
    u32 width_ = 0;
    u32 height_ = 0;
};

/// The engine half of a viewport's GI debug view: for a `render::DebugViewMode` that names a GI
/// view (`gi::gi_debug_view_from_name` knows it), writes the overlay's words for the image at
/// `texture_slot` and returns true; writes nothing and returns false for any other mode.
[[nodiscard]] bool write_gi_debug_view(render::DebugViewMode mode, rhi::BindlessIndex texture_slot,
                                       pipeline::FrameViewData& view) noexcept;

}  // namespace cy::rendering::lightmaps

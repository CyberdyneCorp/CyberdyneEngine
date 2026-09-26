// SPDX-License-Identifier: MIT
#pragma once
// An irradiance volume on the device: its probes as a texture the forward frame samples, and the
// view-block words that tell the frame where the volume is.
//
// `rendering-global-illumination` — "Irradiance volumes and light probes". The volume itself — the
// capture, the update policy, the sampling rule — is `gi::IrradianceVolume`, which has no device.
// This is the half that has one, and it is a separate module for the reason
// `src/rendering/sky_illumination/` is: `cy::rendering-gi` runs every case headless and links no
// device, and `cy::rendering-pipeline` owns the frame and knows nothing of GI. This module is the
// one place the two meet.
//
// ================================================================================================
// THE TEXTURE
// ================================================================================================
//
// `Rgba16Sfloat`, `pack_texels`'s layout: five texels per probe, probe (x, y, z) starting at
// (5x, y + count_y * z). Half floats because every device filters them — the frame's one sampler
// is linear, and a 32-bit float texture is not filterable everywhere — and the coefficients are
// stored divided by the layout's scale so the brightest fits. It is sampled at texel centres, so
// the filter returns the texel and the shader does the trilinear blend itself: the visibility term
// weighs each of the eight probes separately, which a hardware blend cannot.
//
// UPLOADED ONLY WHEN THE VOLUME CHANGED. `upload` compares the volume's generation with the one it
// last copied, so a volume under `OnInvalidation` that nothing invalidated costs no transfer.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/gi/irradiance_volume.h>
#include <cy/rendering/pipeline/frame_pipelines.h>

namespace cy::rendering::light_probes {

class ProbeVolumeTexture {
public:
    ProbeVolumeTexture() noexcept = default;
    ~ProbeVolumeTexture();

    ProbeVolumeTexture(const ProbeVolumeTexture&) = delete;
    ProbeVolumeTexture& operator=(const ProbeVolumeTexture&) = delete;
    ProbeVolumeTexture(ProbeVolumeTexture&&) = delete;
    ProbeVolumeTexture& operator=(ProbeVolumeTexture&&) = delete;

    /// Remember the device. Nothing is created until the first `upload`.
    void initialize(rhi::Device& device, Allocator& allocator) noexcept;
    void shutdown() noexcept;

    /// Copy the volume's probes to the device if they changed since the last upload, in a device
    /// frame of its own: call it OUTSIDE the host's `begin_frame`/`end_frame`, as
    /// `MaterialTextureTable::upload` is. Recreates the texture when the grid's size changed.
    [[nodiscard]] Status upload(const gi::IrradianceVolume& volume) noexcept;

    /// The (slot, view) pair the frame's set 0 needs, at a slot the caller chose.
    [[nodiscard]] pipeline::MaterialTextureSlot slot(rhi::BindlessIndex index) const noexcept {
        return pipeline::MaterialTextureSlot{index, view_};
    }
    [[nodiscard]] bool ready() const noexcept { return !view_.is_null(); }
    [[nodiscard]] const gi::VolumeTextureLayout& layout() const noexcept { return layout_; }
    /// How many uploads actually copied. A diagnostic, and what the "unchanged costs nothing" claim
    /// is checked against.
    [[nodiscard]] u32 uploads() const noexcept { return uploads_; }

private:
    [[nodiscard]] Status recreate(const gi::VolumeTextureLayout& layout) noexcept;
    void release_texture() noexcept;

    rhi::Device* device_ = nullptr;
    Allocator* allocator_ = nullptr;
    rhi::TextureHandle texture_;
    rhi::TextureViewHandle view_;
    gi::VolumeTextureLayout layout_{};
    u64 generation_ = 0;
    bool uploaded_ = false;
    u32 uploads_ = 0;
};

/// Write the volume's words into the frame's view block: its slot, its grid, its origin RELATIVE TO
/// `camera` (the frame has no world space), and the texture's coefficient scale. A caller that
/// never calls this uploads `kNoMaterialTexture` and draws the flat ambient.
void write_probe_volume(rhi::BindlessIndex slot, const gi::IrradianceVolume& volume,
                        const gi::VolumeTextureLayout& layout, Vec3 camera,
                        pipeline::FrameViewData& view) noexcept;

/// IEEE half from float, round to nearest even. The engine has no half type; the one other
/// conversion (`pack_normal_stream`'s) is private to the pipeline module.
[[nodiscard]] u16 half_from_float(f32 value) noexcept;

}  // namespace cy::rendering::light_probes

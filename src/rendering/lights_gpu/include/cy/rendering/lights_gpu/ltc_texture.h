// SPDX-License-Identifier: MIT
#pragma once
// The frame's LTC table, on the device: `LtcTable`'s entries as a texture of the frame's set 0.
//
// `rendering-lighting-and-shadows` — "Area light evaluation": "linearly transformed cosines (LTC),
// using precomputed lookup tables for the GGX BRDF". The table is `LtcTable` (lighting/area.h),
// fitted once on the host; this uploads it where `cy/frame.slang`'s `frameAreaLtc` reads it.
//
// ONE `Rgba32Sfloat` IMAGE, `kLtcSize` WIDE AND TWICE THAT TALL. Rows 0 to 23 are the inverse
// transform's four free terms (a, b, c, d) — `Minv = [a 0 b; 0 c 0; d 0 1]` — and rows 24 to 47 the
// split sum's (magnitude, fresnel, 0, 0). Full floats, because a near-mirror's `a` is 0.02 and its
// `b` a hundred times that, and half floats would put the error of the fit's own storage beside the
// error of the fit. The shader reads texel centres and interpolates in f32, as `LtcTable::sample`
// does, so the device and the host are the same arithmetic.
//
//     LtcTableTexture ltc;
//     ltc.initialize(device, allocator);
//     ltc.upload(table);                          // outside the host's frames, once
//     slots.push_back(ltc.slot(index));           // the frame's set 0 texture table
//     write_ltc_slot(index, upload.view);         // `lighting_control[0]`

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/lighting/area.h>
#include <cy/rendering/pipeline/frame_pipelines.h>

namespace cy::rendering::lights_gpu {

/// The texture's width; its height is twice this.
inline constexpr u32 kLtcTextureWidth = kLtcTableSize;
inline constexpr u32 kLtcTextureHeight = 2U * kLtcTableSize;

/// The texels, row-major from the top, four floats each: what `upload` copies. Exposed so a test
/// can state the layout without a device.
[[nodiscard]] Status pack_ltc_texels(const LtcTable& table, Array<f32>& out) noexcept;

class LtcTableTexture {
public:
    LtcTableTexture() noexcept = default;
    ~LtcTableTexture();

    LtcTableTexture(const LtcTableTexture&) = delete;
    LtcTableTexture& operator=(const LtcTableTexture&) = delete;
    LtcTableTexture(LtcTableTexture&&) = delete;
    LtcTableTexture& operator=(LtcTableTexture&&) = delete;

    void initialize(rhi::Device& device, Allocator& allocator) noexcept;
    void shutdown() noexcept;

    /// Copy `table` to the device in a device frame of its own: call it OUTSIDE the host's
    /// `begin_frame`/`end_frame`, as `DecalTableTexture::upload` is. Refuses an unbuilt table.
    [[nodiscard]] Status upload(const LtcTable& table) noexcept;

    /// The (slot, view) pair the frame's set 0 needs, at a slot the caller chose.
    [[nodiscard]] pipeline::MaterialTextureSlot slot(rhi::BindlessIndex index) const noexcept {
        return pipeline::MaterialTextureSlot{index, view_};
    }
    [[nodiscard]] bool ready() const noexcept { return !view_.is_null(); }

private:
    void release() noexcept;

    rhi::Device* device_ = nullptr;
    Allocator* allocator_ = nullptr;
    rhi::TextureHandle texture_;
    rhi::TextureViewHandle view_;
};

}  // namespace cy::rendering::lights_gpu

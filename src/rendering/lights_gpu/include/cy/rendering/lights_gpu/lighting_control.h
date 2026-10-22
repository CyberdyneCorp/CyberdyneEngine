// SPDX-License-Identifier: MIT
#pragma once
// The frame's lighting words: `pipeline::FrameViewData::lighting_control`, which `cy/frame.slang`
// reads as `lightingControl`. One place writes each word, so a caller cannot set the stochastic
// flag with no reservoir to read or name a reservoir the shader then ignores.

#include <cy/backends/rhi/types.h>
#include <cy/core/base/types.h>
#include <cy/rendering/pipeline/frame_pipelines.h>

namespace cy::rendering::lights_gpu {

/// `lighting_control[3]`: the stochastic path is the frame's. `cy/frame.slang`'s
/// `kCyStochasticLighting`.
inline constexpr u32 kStochasticLighting = 1U;

/// Name the LTC table's set 0 slot (`LtcTableTexture::slot`). Without it an area light shades its
/// diffuse term alone.
inline void write_ltc_slot(rhi::BindlessIndex slot, pipeline::FrameViewData& view) noexcept {
    view.lighting_control[0] = slot;
}

/// Make the stochastic path the frame's: the denoised irradiance and the resolved reservoir at
/// these slots replace the cluster loop for every local light.
inline void write_stochastic_lighting(rhi::BindlessIndex irradiance, rhi::BindlessIndex reservoir,
                                      pipeline::FrameViewData& view) noexcept {
    view.lighting_control[1] = irradiance;
    view.lighting_control[2] = reservoir;
    view.lighting_control[3] |= kStochasticLighting;
}

/// The clustered frame: no stochastic slots, no flag. The LTC slot is kept.
inline void clear_stochastic_lighting(pipeline::FrameViewData& view) noexcept {
    view.lighting_control[1] = pipeline::kNoMaterialTexture;
    view.lighting_control[2] = pipeline::kNoMaterialTexture;
    view.lighting_control[3] &= ~kStochasticLighting;
}

}  // namespace cy::rendering::lights_gpu

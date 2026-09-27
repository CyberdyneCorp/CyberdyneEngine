// SPDX-License-Identifier: MIT
// Auto-exposure, dispatch 2 of 3: the luminance histogram. `rendering-post-processing` —
// "Exposure": "auto-exposure driven by a luminance histogram computed by compute shaders over the
// scene colour", with "histogram percentile-based target selection to reject outliers".
//
// ================================================================================================
// THE METERING CHAIN
// ================================================================================================
//
//   exposure_clear.slang       zero the 256 bins
//   exposure_histogram.slang   this file: every pixel's Rec. 709 luminance, as EV100, into a bin
//                              log-spaced over [histogram.x, histogram.y]; a group accumulates in
//                              shared memory and flushes each non-empty bin with one atomic
//   exposure_adapt.slang       the percentile mean of the bins, the compensation curve, the clamps
//                              and one step of adaptation at the direction's own speed, into the
//                              state the graded resolve reads
//
// The scene colour metered is the frame's SCENE-REFERRED colour before bloom (step 6 of the chain),
// and the result is applied by the NEXT frame's resolve (step 10): a frame never waits on its own
// metering, which is how every engine with histogram metering keeps the pass off the critical path,
// and at sixty frames a second the latency is a sixtieth of the adaptation it feeds.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   python3 src/rendering/grading/shaders/regenerate.py --slangc <build>/Development/bin/slangc
//
// which runs, from the repository root, for each of the three metering files and the resolve:
//
//   slangc src/rendering/grading/shaders/exposure_histogram.slang -I src/rendering/shaders \
//          -entry cyExposureHistogram -stage compute -target spirv -profile spirv_1_5 -o ...
//   slangc ... -target metal -o ...
//
// plus `fullscreenVertex` from src/rendering/shaders/cy/fullscreen.slang, then embed_spirv.py and
// embed_msl.py into ../src/grading_spirv.h and ../src/grading_msl.h, and clang-format over both.

#include "exposure_common.slang"

struct CyExposureHistogramSet
{
    Texture2D<float4> source;
    RWStructuredBuffer<uint> histogram;
};

[[vk::binding(0, 0)]] ParameterBlock<CyExposureHistogramSet> cyExposureHistogramSet;
[[vk::push_constant]] CyExposureConstants cyExposureConstants;

groupshared uint cyExposureLocalBins[kCyExposureBins];

[shader("compute")]
[numthreads(16, 16, 1)]
void cyExposureHistogram(uint3 pixel: SV_DispatchThreadID, uint index: SV_GroupIndex)
{
    cyExposureLocalBins[index] = 0;
    GroupMemoryBarrierWithGroupSync();

    let extent = cyExposureConstants.extent.xy;
    if (pixel.x < extent.x && pixel.y < extent.y)
    {
        let colour = cyExposureHistogramSet.source.Load(int3(int2(pixel.xy), 0)).rgb;
        let luminance = dot(colour, float3(0.2126, 0.7152, 0.0722));
        let bin = cyExposureBin(luminance, cyExposureConstants.histogram.x,
                                cyExposureConstants.histogram.y);
        InterlockedAdd(cyExposureLocalBins[bin], 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    let count = cyExposureLocalBins[index];
    if (count != 0u)
    {
        InterlockedAdd(cyExposureHistogramSet.histogram[index], count);
    }
}

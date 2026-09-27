// SPDX-License-Identifier: MIT
// Auto-exposure, dispatch 1 of 3: zero the histogram. `rendering-post-processing` — "Exposure".
//
// A dispatch of its own rather than a job the adapt pass does after reading, because the first frame
// must start from zero too, and a buffer's contents before anything wrote it are nobody's. One group,
// one thread a bin. See exposure_histogram.slang for the whole metering chain and for how the three
// files are compiled.

#include "exposure_common.slang"

struct CyExposureClearSet
{
    RWStructuredBuffer<uint> histogram;
};

[[vk::binding(0, 0)]] ParameterBlock<CyExposureClearSet> cyExposureClearSet;
[[vk::push_constant]] CyExposureConstants cyExposureConstants;

[shader("compute")]
[numthreads(256, 1, 1)]
void cyExposureClear(uint index: SV_GroupIndex)
{
    cyExposureClearSet.histogram[index] = 0;
}

// SPDX-License-Identifier: MIT
// Auto-exposure, dispatch 3 of 3: meter, compensate, clamp, adapt. `rendering-post-processing` —
// "Exposure": percentile target selection, minimum and maximum EV clamps, separate adaptation speeds
// for brightening and darkening, and a compensation curve keyed on measured luminance.
//
// ONE THREAD, deliberately. The work is two walks over 256 bins — a few microseconds — and a
// parallel prefix sum would be the same answer in more code, with an f32 summation order the host
// twin could no longer follow. `metered_ev100` and `adapt_ev100` in src/rendering/post/src/exposure.cpp
// are that twin, statement for statement.
//
// The state is one float4: x the EV100 in force, y the metered target this frame adapted toward,
// z the metered EV100 before the compensation curve, w how many frames have adapted since the last
// start. See exposure_histogram.slang for the chain and for how this file is compiled.

#include "exposure_common.slang"

struct CyExposureAdaptSet
{
    StructuredBuffer<uint> histogram;
    RWStructuredBuffer<float4> state;
};

[[vk::binding(0, 0)]] ParameterBlock<CyExposureAdaptSet> cyExposureAdaptSet;
[[vk::push_constant]] CyExposureConstants cyExposureConstants;

/// `ExposureCompensationCurve::at`.
float compensationAt(float metered)
{
    let points = cyExposureConstants.curveEv;
    let added = cyExposureConstants.curveCompensation;
    if (metered <= points.x)
    {
        return added.x;
    }
    for (uint index = 1; index < 4; ++index)
    {
        if (metered <= points[index])
        {
            let span = points[index] - points[index - 1];
            let t = span > 1e-6 ? (metered - points[index - 1]) / span : 0.0;
            return lerp(added[index - 1], added[index], t);
        }
    }
    return added.w;
}

/// `metered_ev100`, before the compensation and the clamps. False when nothing was metered.
bool meteredMean(out float metered)
{
    metered = 0.0;
    float total = 0.0;
    for (uint bin = 0; bin < kCyExposureBins; ++bin)
    {
        total += float(cyExposureAdaptSet.histogram[bin]);
    }
    if (total <= 0.0)
    {
        return false;
    }
    let lowFraction = clamp(cyExposureConstants.histogram.z, 0.0, 1.0);
    let highFraction = clamp(cyExposureConstants.histogram.w, lowFraction, 1.0);
    let lowCount = total * lowFraction;
    let highCount = total * highFraction;
    let minEv = cyExposureConstants.histogram.x;
    let binWidth = (cyExposureConstants.histogram.y - minEv) / float(kCyExposureBins);

    // The percentiles split the bins they fall inside, as on the host, rather than discarding whole
    // bins and quantising the answer to a bin's width.
    float seen = 0.0;
    float weighted = 0.0;
    float weight = 0.0;
    for (uint bin = 0; bin < kCyExposureBins; ++bin)
    {
        let begin = seen;
        let end = seen + float(cyExposureAdaptSet.histogram[bin]);
        seen = end;
        let taken = min(end, highCount) - max(begin, lowCount);
        if (taken > 0.0)
        {
            weighted += taken * (minEv + binWidth * (float(bin) + 0.5));
            weight += taken;
        }
    }
    metered = weight > 0.0 ? weighted / weight : minEv;
    return true;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void cyExposureAdapt()
{
    let limits = cyExposureConstants.limits;
    let frame = cyExposureConstants.frame;
    let previous = cyExposureAdaptSet.state[0];
    let current = frame.y > 0.5 ? frame.z : previous.x;
    let frames = frame.y > 0.5 ? 0.0 : previous.w;

    float metered = limits.x;
    float target = limits.x;
    if (meteredMean(metered))
    {
        target = clamp(metered + compensationAt(metered), limits.x, limits.y);
    }

    // `adapt_ev100`: exponential toward the target at the direction's own speed. Brightening means
    // the scene got brighter, so the exposure value RISES.
    float adapted = current;
    if (frame.x > 0.0)
    {
        let speed = target > current ? limits.z : limits.w;
        let blend = clamp(1.0 - exp(-max(speed, 0.0) * frame.x), 0.0, 1.0);
        adapted = lerp(current, target, blend);
    }
    cyExposureAdaptSet.state[0] = float4(adapted, target, metered, frames + 1.0);
}

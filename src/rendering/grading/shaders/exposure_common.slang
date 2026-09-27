// SPDX-License-Identifier: MIT
// What the three metering dispatches share: the constants they are pushed and the binning. Included
// by exposure_clear.slang, exposure_histogram.slang and exposure_adapt.slang.
//
// Every expression here has a C++ twin in src/rendering/post/src/exposure.cpp —
// `ev100_from_luminance`, `luminance_histogram_bin`, `metered_ev100`, `adapt_ev100` — written in the
// same order, because the device suite compares the device's histogram and exposure with the host's
// rather than judging a picture.

/// `cy::rendering::grading::ExposureConstants`, word for word. 96 bytes.
struct CyExposureConstants
{
    /// x: the histogram's lowest EV100, y: its highest, z: the low percentile, w: the high one.
    float4 histogram;
    /// x, y: the metered EV100's clamps. z: EV per second brightening, w: darkening.
    float4 limits;
    /// The compensation curve: metered EV100 at its four points, then the EV added at each.
    float4 curveEv;
    float4 curveCompensation;
    /// x: seconds since the last frame. y: 1 to start this frame's adaptation from z rather than
    /// from the state — the first frame, and any cut. z: that starting EV100.
    float4 frame;
    /// xy: the metered image's extent in pixels.
    uint4 extent;
};

/// Bins. The histogram dispatch's group is 16 x 16 threads and each clears, and flushes, one bin.
static const uint kCyExposureBins = 256;

/// `ev100_from_luminance`: 12.5 is the reflected-light meter calibration constant.
float cyExposureEv100(float luminance)
{
    return log2(max(luminance, 1e-6) * 100.0 / 12.5);
}

/// `luminance_histogram_bin`: below the range is bin 0 and above it the last bin.
uint cyExposureBin(float luminance, float minEv, float maxEv)
{
    let span = max(maxEv - minEv, 1e-6);
    let position = (cyExposureEv100(luminance) - minEv) / span;
    let scaled = clamp(position * float(kCyExposureBins), 0.0, float(kCyExposureBins - 1));
    return uint(scaled);
}

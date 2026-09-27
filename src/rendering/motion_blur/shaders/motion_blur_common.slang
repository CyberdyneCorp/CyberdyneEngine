// SPDX-License-Identifier: MIT
// What the three motion blur dispatches share: the constants, the blur vector, the depth ordering.
// Included by motion_blur_tile_max.slang, motion_blur_neighbour_max.slang and
// motion_blur_gather.slang.
//
// Every expression here has a C++ twin in src/rendering/motion_blur/src/motion_blur.cpp, written in
// the same order, because the suites compare the device's image against the host's.

/// `cy::rendering::motion_blur::MotionBlurConstants`, word for word. 128 bytes: exactly the
/// push-constant size every backend guarantees.
struct CyMotionBlurConstants
{
    /// This frame's clip space (unjittered NDC and depth) to last frame's, as four rows.
    float4 currentToPrevious0;
    float4 currentToPrevious1;
    float4 currentToPrevious2;
    float4 currentToPrevious3;
    /// xy: the extent in pixels, zw: its reciprocal.
    float4 extent;
    /// x: the camera's blur radius per pixel of motion, y: an object's, z: the longest radius in
    /// pixels, w: the soft depth in metres.
    float4 scales;
    /// x: m22, y: m32. z: 1 when the camera's share is scaled apart from the objects'.
    float4 depth;
    /// x: gather samples, y: tile edge in pixels, z: tiles across, w: tiles down.
    uint4 control;
};

[[vk::push_constant]] CyMotionBlurConstants cyMotionBlur;

/// Reversed-Z depth to positive view depth in metres: -z = m32 / (d + m22).
float cyMotionBlurViewDepth(float depth)
{
    return cyMotionBlur.depth.y / (depth + cyMotionBlur.depth.x);
}

/// Where last frame's camera saw this pixel's own depth, as a motion in pixels pointing at the
/// past — the prepass's convention.
float2 cyMotionBlurCameraMotion(uint2 pixel, float depth)
{
    let uv = (float2(pixel) + 0.5) * cyMotionBlur.extent.zw;
    let clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    let previous = float4(dot(cyMotionBlur.currentToPrevious0, clip),
                          dot(cyMotionBlur.currentToPrevious1, clip),
                          dot(cyMotionBlur.currentToPrevious2, clip),
                          dot(cyMotionBlur.currentToPrevious3, clip));
    let previousNdc = previous.xy / previous.w;
    return float2((previousNdc.x - clip.x) * 0.5 * cyMotionBlur.extent.x,
                  (clip.y - previousNdc.y) * 0.5 * cyMotionBlur.extent.y);
}

/// A pixel's blur vector in pixels: the shutter-scaled half-motion, the camera's share scaled apart
/// where the two scales differ, clamped to the longest radius.
/// A pixel no surface covered is the sky: the far plane, moving with the camera alone.
float2 cyMotionBlurVector(uint2 pixel, float2 velocity, float depth)
{
    let motion = depth > 0.0 ? velocity * cyMotionBlur.extent.xy
                             : cyMotionBlurCameraMotion(pixel, 0.0);
    float2 blur = motion * cyMotionBlur.scales.x;
    if (cyMotionBlur.depth.z > 0.5 && depth > 0.0)
    {
        let camera = cyMotionBlurCameraMotion(pixel, depth);
        blur = camera * cyMotionBlur.scales.x + (motion - camera) * cyMotionBlur.scales.y;
    }
    let size = length(blur);
    if (size > cyMotionBlur.scales.z)
    {
        blur = blur * (cyMotionBlur.scales.z / size);
    }
    return blur;
}

/// The longer of two vectors; the first on a tie, so a walk's order decides and not the hardware's.
float2 cyMotionBlurLonger(float2 longest, float2 candidate)
{
    return dot(candidate, candidate) > dot(longest, longest) ? candidate : longest;
}

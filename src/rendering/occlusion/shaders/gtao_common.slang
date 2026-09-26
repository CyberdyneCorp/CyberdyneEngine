// SPDX-License-Identifier: MIT
// What the horizon search and the filter cascade share: depth linearisation, view-position
// reconstruction and the normal decode. Included by gtao.slang and gtao_filter.slang.
//
// Every expression here has a C++ twin in src/rendering/occlusion/src/gtao.cpp, written in the same
// order, because the device suite compares the two buffers rather than a picture.

import cy.packing;

static const float kCyGtaoPi = 3.14159265358979;
static const float kCyGtaoHalfPi = 1.57079632679490;

/// Reversed-Z depth in [0, 1] to positive view depth in metres. `projection.z` and `.w` are the
/// projection's (2,2) and (3,2) terms, so this is exact for the finite and the infinite form alike:
/// d = (m22 * z + m32) / -z  gives  -z = m32 / (d + m22). Zero is the cleared far plane, the sky.
float cyGtaoViewDepth(float4 projection, float depth)
{
    return projection.w / (depth + projection.z);
}

/// A pixel centre and its view depth to a view-space position. -Z forward, +Y up; pixel row zero is
/// the top of the image, which is NDC y = +1 on every backend this engine records for.
float3 cyGtaoViewPosition(float4 projection, float4 extent, float2 pixelCentre, float viewDepth)
{
    let uv = pixelCentre * extent.zw;
    let ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3(ndc.x * viewDepth / projection.x, ndc.y * viewDepth / projection.y, -viewDepth);
}

/// The prepass normal, camera-relative, from the octahedral pair in `.xy`.
float3 cyGtaoNormal(float4 packed)
{
    return decodeOctahedral(packed.xy);
}

/// The eight slice directions, in pixels, y down, spread over half a turn: 0, 26.6, 45, 63.4, 90,
/// 116.6, 135 and 153.4 degrees. INTEGER STEPS, which is the point: a tap at `k * direction` from
/// a pixel centre IS a pixel centre, so every tap lies exactly on the screen line through the pixel
/// and its reconstructed position lies exactly in the slice plane. A rotated direction snapped to
/// the nearest pixel puts a tap half a pixel off that plane, and on a flat surface seen at an angle
/// the tap then rises above the slice's tangent and occludes the surface it lies on — the error
/// that made an open floor read 0.71 before this table replaced a continuous rotation.
static const int2 kCyGtaoDirections[8] = { int2(1, 0), int2(2, 1), int2(1, 1), int2(1, 2),
                                           int2(0, 1), int2(-1, 2), int2(-1, 1), int2(-2, 1) };

/// The 4x4 ordered-dither index at a pixel, in [0, 16). The whole of the pass's noise, and it is
/// FIXED IN SCREEN SPACE: nothing here reads a frame index, so a still camera gets the same term on
/// every frame and the denoiser's cascade — whose 5x5 base covers one period of this pattern — is
/// what removes it.
uint cyGtaoDither(uint2 pixel)
{
    static const uint kBayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    return kBayer[(pixel.y & 3u) * 4u + (pixel.x & 3u)];
}

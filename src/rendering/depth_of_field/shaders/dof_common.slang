// SPDX-License-Identifier: MIT
// What the five depth of field dispatches share: the constants, the circle of confusion in pixels,
// the aperture's shape and the gather's taps. Included by every dof_*.slang.
//
// Every function here has a C++ twin of the same name in
// src/rendering/depth_of_field/src/focus.cpp, written in the same order; unit.render_dof holds the
// twins to `circle_of_confusion` and to the projection, and render.depth_of_field measures what the
// device made of them.

/// `cy::rendering::depth_of_field::DofConstants`, word for word. 80 bytes.
struct CyDofConstants
{
    /// The projection's m22, m23, m32, m33: d = (m23 - z m33) / (m22 - z m32).
    float4 depth;
    /// x: K, the radius in pixels at infinity. y: the focus distance. z: the largest radius
    /// gathered, in pixels. w: the focus band, in pixels.
    float4 lens;
    /// x: blades (0 for a circle). y: their rotation. z: the aperture's area at unit circumradius.
    /// w: the most rings a gather takes.
    float4 aperture;
    /// The target's extent, then the half-resolution layers'.
    uint4 extent;
    /// x: the tile edge in half-resolution texels. yz: the tile grid.
    uint4 tiles;
};

[[vk::push_constant]] CyDofConstants cyDof;

static const float kCyDofTwoPi = 6.28318530717958647692;
static const float kCyDofPi = 3.14159265358979323846;
static const float kCyDofMinimumDistance = 1.0e-4;

/// The distance along the optical axis a reversed-Z depth encodes. The cleared sky (0) is at
/// infinity, which `cyDofRadiusAtDepth` takes as the limit K rather than dividing by zero.
float cyDofViewDistance(float depth)
{
    return (cyDof.depth.y - depth * cyDof.depth.w) / (cyDof.depth.x - depth * cyDof.depth.z);
}

/// The signed blur radius in pixels at a distance: K (d - F) / d. Negative in front of the focus.
float cyDofRadius(float distance)
{
    let clamped = max(distance, kCyDofMinimumDistance);
    return cyDof.lens.x * (clamped - cyDof.lens.y) / clamped;
}

/// The signed radius at a depth-buffer value, clamped to the largest radius gathered.
float cyDofRadiusAtDepth(float depth)
{
    let radius = depth > 0.0 ? cyDofRadius(cyDofViewDistance(depth)) : cyDof.lens.x;
    return clamp(radius, -cyDof.lens.z, cyDof.lens.z);
}

/// How far the aperture reaches in a direction, as a fraction of its circumradius.
float cyDofApertureExtent(float angle)
{
    let blades = cyDof.aperture.x;
    if (blades < 1.0)
    {
        return 1.0;
    }
    let sector = kCyDofTwoPi / blades;
    let turned = angle - cyDof.aperture.y;
    let within = turned - sector * floor(turned / sector);
    return cos(0.5 * sector) / cos(within - 0.5 * sector);
}

/// Rings for a radius in texels: one a texel of radius, up to the constants' limit.
uint cyDofRings(float radius)
{
    return min(uint(ceil(max(radius, 1.0))), uint(cyDof.aperture.w));
}

/// Tap `index` of a gather of `rings` rings over `radius`: xy the offset, z the area it stands for.
float3 cyDofTap(uint index, uint rings, float radius)
{
    let spacing = radius / (float(rings) + 0.5);
    if (index == 0)
    {
        return float3(0.0, 0.0, 0.25 * kCyDofPi * spacing * spacing);
    }
    uint ring = 1;
    while (index >= 1 + 3 * ring * (ring + 1))
    {
        ++ring;
    }
    let first = 1 + 3 * ring * (ring - 1);
    let count = 6 * ring;
    let turn = (float(index - first) + ((ring & 1) != 0 ? 0.0 : 0.5)) / float(count);
    let angle = kCyDofTwoPi * turn;
    let distance = float(ring) * spacing;
    return float3(distance * cos(angle), distance * sin(angle), (kCyDofPi / 3.0) * spacing * spacing);
}

/// Whether a texel whose blur has radius `radius` texels, `offset` texels from the pixel, reaches
/// the pixel: 1 inside the aperture's shape, 0 outside, a one-texel ramp across its edge so a
/// disc's rim is antialiased rather than stair-stepped.
float cyDofReach(float2 offset, float radius)
{
    let distance = length(offset);
    // The direction from the texel TO the pixel: the aperture's image is centred on the texel.
    let extent = distance > 0.0 ? cyDofApertureExtent(atan2(-offset.y, -offset.x)) : 1.0;
    return saturate(radius * extent - distance + 0.5);
}

/// How much of a blurred texel is blurred: 0 inside the focus band, 1 a pixel beyond it.
float cyDofDefocus(float radiusPixels)
{
    return saturate(abs(radiusPixels) - cyDof.lens.w);
}

/// The taps of a gather of `rings` rings: 1 + 3 k (k + 1).
uint cyDofTapCount(uint rings)
{
    return 1 + 3 * rings * (rings + 1);
}

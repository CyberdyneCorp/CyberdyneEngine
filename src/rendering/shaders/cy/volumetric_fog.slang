// SPDX-License-Identifier: MIT
// Volumetric fog, read from the froxel volume `fog::FogPass` fills. Part of the engine's Slang
// standard library.
//
// `rendering-post-processing` — "Volumetric fog": the volume is filled and integrated front to back
// by src/rendering/fog/shaders/fog_march.slang; this module is the other end, the lookup a
// shading pass makes to apply it to a surface. There is no fog colour and no fog distance here: the
// transmittance and in-scattering are what the medium between the eye and the point did.
//
// ================================================================================================
// THE LAYOUT, RESTATED FROM `cy/rendering/fog/volume.h`
// ================================================================================================
//
// One float4 texture, 2W by 1 + H D. Row 0 is the header:
//
//   texel 0   forward.xyz, enabled (1 or 0)
//   texel 1   right.xyz,   tan of half the horizontal field of view
//   texel 2   up.xyz,      tan of half the vertical field of view
//   texel 3   width, height, depth, the slice distribution's exponent
//   texel 4   near plane, far plane, sub-steps a slice, 0
//   texel 5   the eye the columns start at, in the frame's relative space
//
// and row 1 + slice H + y holds column x's transmittance at texel x and its in-scattering at texel
// W + x, for the slice's far edge, y counted UP the frame. The first five texels are
// `cy/aerial_perspective.slang`'s header word for word — the fog is the same froxel distribution
// as the atmosphere's table, `froxel_slice_depth`, and the sampler below has the same shape.
//
// ================================================================================================
// THE SOURCE IS AN INTERFACE
// ================================================================================================
//
// Every consumer binds the texture its own way — the frame through a slot of its global texture
// table, a sample through a binding of its own — so the lookup is written once against
// `ICyFogVolumeSource` and each consumer supplies one `texel` fetch. `fog_at` in
// src/rendering/fog/src/volume.cpp is this module on the processor, and the device suite compares
// the two.

module volumetric_fog;

/// One texel of the volume, by integer coordinate, level 0.
public interface ICyFogVolumeSource
{
    float4 texel(int2 coordinate);
};

/// What the medium between the eye and a point does to it: multiply by `transmittance`, then add
/// `inScattering`, which is in the frame's own radiance units.
public struct CyFogAtPoint
{
    public float3 transmittance;
    public float3 inScattering;
};

/// Whether the volume was filled at all. A header whose `enabled` texel is zero leaves every
/// surface exactly as it was lit.
public bool cyFogEnabled<S : ICyFogVolumeSource>(S source)
{
    return source.texel(int2(0, 0)).w > 0.5;
}

/// `a + (b - a) t`, the processor's `lerp`, rather than the intrinsic: the intrinsic may be
/// evaluated as `a (1 - t) + b t`, which does not return exactly one between two texels that both
/// hold one. A volume with nothing in it must leave a surface's radiance bit for bit as it was, and
/// that is the form that does.
float3 cyFogLerp(float3 a, float3 b, float t)
{
    return a + ((b - a) * t);
}

float cyFogSliceDepth(float4 shape, float4 planes, float slice)
{
    const float t = min(slice + 1.0, shape.z) / shape.z;
    return lerp(planes.x, planes.y, pow(t, max(shape.w, 1.0)));
}

float3 cyFogPlane<S : ICyFogVolumeSource>(S source, float4 shape, uint slice, float2 texel,
                                          uint which)
{
    const uint width = uint(shape.x);
    const uint height = uint(shape.y);
    const float2 clamped = clamp(texel, float2(0.0), float2(shape.x - 1.0, shape.y - 1.0));
    const uint x0 = uint(clamped.x);
    const uint y0 = uint(clamped.y);
    const uint x1 = min(x0 + 1u, width - 1u);
    const uint y1 = min(y0 + 1u, height - 1u);
    const float tx = clamped.x - float(x0);
    const float ty = clamped.y - float(y0);
    const int column = int(which * width);
    const int row0 = int(1u + (slice * height) + y0);
    const int row1 = int(1u + (slice * height) + y1);
    const float3 bottom = cyFogLerp(source.texel(int2(column + int(x0), row0)).xyz,
                               source.texel(int2(column + int(x1), row0)).xyz, tx);
    const float3 top = cyFogLerp(source.texel(int2(column + int(x0), row1)).xyz,
                            source.texel(int2(column + int(x1), row1)).xyz, tx);
    return cyFogLerp(bottom, top, ty);
}

/// What the medium does to a point at `relativePosition`, in the frame's relative space. A point
/// behind the eye, or a volume that is off, is returned untouched.
public CyFogAtPoint cyFogAt<S : ICyFogVolumeSource>(S source, float3 relativePosition)
{
    CyFogAtPoint result;
    result.transmittance = float3(1.0);
    result.inScattering = float3(0.0);

    const float4 forward = source.texel(int2(0, 0));
    const float4 right = source.texel(int2(1, 0));
    const float4 up = source.texel(int2(2, 0));
    const float4 shape = source.texel(int2(3, 0));
    const float4 planes = source.texel(int2(4, 0));
    const float3 offset = relativePosition - source.texel(int2(5, 0)).xyz;
    const float depth = dot(offset, forward.xyz);
    if (forward.w < 0.5 || depth <= 0.0)
    {
        return result;
    }

    const float ndcX = dot(offset, right.xyz) / (depth * right.w);
    const float ndcY = dot(offset, up.xyz) / (depth * up.w);
    const float2 texel = float2(((ndcX * 0.5 + 0.5) * shape.x) - 0.5,
                                ((ndcY * 0.5 + 0.5) * shape.y) - 0.5);

    // Where the depth falls: before the first slice's far edge the near end is the eye itself.
    const uint last = uint(shape.z) - 1u;
    const float first = cyFogSliceDepth(shape, planes, 0.0);
    float3 nearT = float3(1.0);
    float3 nearS = float3(0.0);
    uint farSlice = 0u;
    float fraction = saturate(depth / max(first, 1e-6));
    if (depth > first)
    {
        const float span = max(planes.y - planes.x, 1e-6);
        const float normalised = saturate((depth - planes.x) / span);
        const float continuous = pow(normalised, 1.0 / max(shape.w, 1.0)) * shape.z - 1.0;
        const uint slice = min(uint(max(continuous, 0.0)), last);
        nearT = cyFogPlane(source, shape, slice, texel, 0u);
        nearS = cyFogPlane(source, shape, slice, texel, 1u);
        farSlice = min(slice + 1u, last);
        const float nearEdge = cyFogSliceDepth(shape, planes, float(slice));
        const float farEdge = cyFogSliceDepth(shape, planes, float(slice + 1u));
        fraction = slice == last ? 1.0 : saturate((depth - nearEdge) / max(farEdge - nearEdge, 1e-6));
    }
    const float3 farT = cyFogPlane(source, shape, farSlice, texel, 0u);
    const float3 farS = cyFogPlane(source, shape, farSlice, texel, 1u);
    result.transmittance = cyFogLerp(nearT, farT, fraction);
    result.inScattering = cyFogLerp(nearS, farS, fraction);
    return result;
}

/// A lit surface's radiance as the eye receives it through the fog: attenuated, then added to.
public float3 cyApplyFog<S : ICyFogVolumeSource>(S source, float3 radiance, float3 relativePosition)
{
    const CyFogAtPoint fog = cyFogAt(source, relativePosition);
    return radiance * fog.transmittance + fog.inScattering;
}

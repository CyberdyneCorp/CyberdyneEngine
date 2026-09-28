// SPDX-License-Identifier: MIT
// Volumetric fog: the froxel volume filled with the medium, lit by the sun through the shadow map
// and by the ambient term, and integrated front to back. `rendering-post-processing` — "Volumetric
// fog".
//
// ================================================================================================
// WHAT THIS COMPUTES
// ================================================================================================
//
// One thread per (x, y) column of the volume, marching its ray from the eye through every slice
// and `steps` sub-steps a slice. At each sub-step's midpoint: the medium (the height fog and the
// authored volumes, `fog/medium.h`), the shadow map's visibility of the sun, the source term
// `J = sun_scattering * E_sun * V + scattering * L_ambient + emission`, and one step of
// `rendering-post-processing`'s analytic integral. At each slice's far edge the column writes the
// transmittance and the in-scattering it has accumulated from the eye. `fog/volume.h` says why the
// four stages the requirement names are one loop, and `integrate_fog_column` in
// src/rendering/fog/src/volume.cpp is this function on the processor, statement for statement.
//
// ================================================================================================
// OUTPUT
// ================================================================================================
//
//   row 0                 the header, six texels: forward and enabled, right and tan x, up and
//                         tan y, (W, H, D, exponent), (near, far, steps, 0), the eye — written by
//                         column (0, 0), copied from the constants
//   row 1 + slice H + y   texel x: transmittance.rgb, 1;  texel W + x: in-scattering.rgb, 1
//
// `cy/volumetric_fog.slang` reads it.
//
// ================================================================================================
// THE SECOND OUTPUT: THE ATMOSPHERE'S OWN TABLE, WITH THE FOG IN IT
// ================================================================================================
//
// Compiled again with `CY_FOG_AIR_TABLE`, the march writes `sky::pack_aerial_perspective`'s layout
// instead — five header words, then (transmittance, in-scattering) per froxel, slice-major then
// row-major — into a buffer, so a consumer that already applies aerial perspective through
// `cy/aerial_perspective.slang` applies the fog with it and changes nothing else. "Aerial
// perspective from the physical atmosphere SHALL be composited consistently with fog rather than
// applied separately": when the atmosphere's table is bound (`words[4].w`), each slice's stretch of
// air is read out of it — what it transmits and what it adds between the slice's two edges — and
// marched WITH the fog as one more homogeneous medium, per channel. The air is then in front of
// the fog and behind it in the proportion they share each slice, rather than all of one in front of
// all of the other.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   python3 src/rendering/fog/shaders/regenerate.py --slangc <build>/Development/bin/slangc
//
// which runs, from the repository root:
//
//   slangc src/rendering/fog/shaders/fog_march.slang -I src/rendering/shaders \
//          -entry cyVolumetricFog -stage compute -target spirv -profile spirv_1_5 -o fog.spv
//   slangc ... -target metal -o fog.metal
//
// and the same two again with `-D CY_FOG_AIR_TABLE=1` for the table variant, then embed_spirv.py
// and embed_msl.py into ../src/fog_spirv.h and ../src/fog_msl.h, and clang-format over both.

#if defined(CY_FOG_AIR_TABLE)
import cy.aerial_perspective;
#endif

static const float kCyFogPi = 3.14159265358979;
static const uint kCyFogHeaderWords = 17u;
static const uint kCyFogVolumeWords = 4u;
static const uint kCyFogHeaderTexels = 6u;

struct CyFogSet
{
    /// The directional shadow map's stored depths, reversed Z. A one-texel placeholder when the
    /// frame has none, and then never read: `words[7].w` is zero.
    Texture2D<float> shadowMap;
    /// `fog::FogConstants`, word for word — `pack_fog_constants` is the layout.
    StructuredBuffer<float4> words;
#if defined(CY_FOG_AIR_TABLE)
    /// `sky::pack_aerial_perspective`'s layout, with the fog in it.
    RWStructuredBuffer<float4> table;
    /// The atmosphere's own table, read when `words[4].w` is one.
    StructuredBuffer<float4> air;
#else
    [format("rgba32f")] RWTexture2D<float4> output;
#endif
};

[[vk::binding(0, 0)]] ParameterBlock<CyFogSet> cyFogSet;

float4 fogWord(uint index)
{
    return cyFogSet.words[index];
}

/// `froxel_slice_depth`: slice i's far edge, metres along the forward axis.
float fogSliceDepth(float4 shape, float4 planes, uint slice)
{
    const float t = min(float(slice) + 1.0, shape.z) / shape.z;
    return lerp(planes.x, planes.y, pow(t, max(shape.w, 1.0)));
}

/// `henyey_greenstein`, with its clamp.
float fogPhase(float g, float cosTheta)
{
    const float clamped = clamp(g, -0.99, 0.99);
    const float squared = clamped * clamped;
    const float denominator = 1.0 + squared - (2.0 * clamped * cosTheta);
    return (1.0 - squared) / (4.0 * kCyFogPi * denominator * sqrt(max(denominator, 1e-6)));
}

/// `inside_distance` in medium.cpp.
float fogInsideDistance(float shape, float3 size, float3 local)
{
    const float radial = sqrt(local.x * local.x + local.z * local.z);
    if (shape < 0.5)
    {
        return min(size.x - abs(local.x), min(size.y - abs(local.y), size.z - abs(local.z)));
    }
    if (shape < 1.5)
    {
        return size.x - length(local);
    }
    if (shape < 2.5)
    {
        return min(size.x - radial, size.y - abs(local.y));
    }
    const float height = max(size.y, 1e-6);
    const float radius = size.x * (1.0 - (local.y / height));
    return min(min(local.y, height - local.y), radius - radial);
}

struct FogMediumSample
{
    float extinction;
    float3 scattering;
    float3 sunScattering;
    float3 emission;
};

void fogAddMedium(inout FogMediumSample sample, float extinction, float3 albedo, float g,
                  float3 emission, float cosToSun)
{
    sample.emission += emission;
    if (extinction <= 0.0)
    {
        return;
    }
    const float3 scattering = albedo * extinction;
    sample.extinction += extinction;
    sample.scattering += scattering;
    sample.sunScattering += scattering * fogPhase(g, cosToSun);
}

/// `sample_medium`.
FogMediumSample fogSampleMedium(float3 world, float cosToSun)
{
    FogMediumSample sample;
    sample.extinction = 0.0;
    sample.scattering = float3(0.0);
    sample.sunScattering = float3(0.0);
    sample.emission = float3(0.0);

    const float4 height = fogWord(15u);
    if (height.x > 0.0)
    {
        const float above = max(world.y - height.y, 0.0);
        const float extinction = height.x * exp(-above / max(height.z, 1e-3));
        fogAddMedium(sample, extinction, fogWord(16u).xyz, height.w, float3(0.0), cosToSun);
    }
    const uint count = min(uint(fogWord(6u).w), 8u);
    for (uint index = 0u; index < count; ++index)
    {
        const uint base = kCyFogHeaderWords + index * kCyFogVolumeWords;
        const float4 v0 = fogWord(base);
        const float4 v1 = fogWord(base + 1u);
        const float4 v2 = fogWord(base + 2u);
        const float4 v3 = fogWord(base + 3u);
        const float inside = fogInsideDistance(v0.w, v1.xyz, world - v0.xyz);
        if (inside <= 0.0)
        {
            continue;
        }
        const float coverage = v1.w <= 0.0 ? 1.0 : saturate(inside / v1.w);
        fogAddMedium(sample, v2.w * coverage, v2.xyz, v3.w, v3.xyz * coverage, cosToSun);
    }
    return sample;
}

float fogLit(float reference, int2 texel, int extent)
{
    const int2 clamped = clamp(texel, int2(0, 0), int2(extent - 1, extent - 1));
    return reference >= cyFogSet.shadowMap.Load(int3(clamped, 0)) ? 1.0 : 0.0;
}

/// `fog_shadow_visibility`: a bilinear blend of four comparisons, 1 outside the map.
float fogShadowVisibility(float3 relative)
{
    if (fogWord(7u).w < 0.5)
    {
        return 1.0;
    }
    const float4 p = float4(relative, 1.0);
    const float4 q = float4(dot(fogWord(10u), p), dot(fogWord(11u), p), dot(fogWord(12u), p),
                            dot(fogWord(13u), p));
    if (q.w <= 0.0)
    {
        return 1.0;
    }
    const float2 uv = q.xy / q.w;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    {
        return 1.0;
    }
    const float4 control = fogWord(14u);
    const float reference = (q.z / q.w) + control.x;
    const float size = control.y;
    const int extent = int(size);
    const float2 t = (uv * size) - 0.5;
    const float2 f = floor(t);
    const int2 base = int2(f);
    const float2 a = t - f;
    const float top = lerp(fogLit(reference, base, extent), fogLit(reference, base + int2(1, 0), extent),
                           a.x);
    const float bottom = lerp(fogLit(reference, base + int2(0, 1), extent),
                              fogLit(reference, base + int2(1, 1), extent), a.x);
    return lerp(top, bottom, a.y);
}

#if defined(CY_FOG_AIR_TABLE)
/// The air between two distances along a column's ray, out of the atmosphere's table: an
/// extinction per channel and a source term such that a homogeneous medium of the two over the
/// stretch transmits and adds exactly what the table says it does.
///
///   T = T(far) / T(near),   S = (S(far) - S(near)) / T(near),
///   sigma = -ln(T) / length,   J = S / (length * (1 - T) / (sigma * length))
///
/// with `(1 - exp(-x)) / x` taken as `1 - x / 2` below 1e-3, where the quotient loses its digits.
void fogAirStretch(float3 direction, float nearDistance, float farDistance, out float3 extinction,
                   out float3 source)
{
    CyAerialPerspectiveTable table;
    table.words = cyFogSet.air;
    const CyAerialPerspective nearAir = cyAerialPerspectiveAt(table, direction * nearDistance);
    const CyAerialPerspective farAir = cyAerialPerspectiveAt(table, direction * farDistance);
    const float length = max(farDistance - nearDistance, 1e-6);
    const float3 nearT = max(nearAir.transmittance, float3(1e-6));
    const float3 transmitted = saturate(farAir.transmittance / nearT);
    const float3 added = max(farAir.inScattering - nearAir.inScattering, float3(0.0)) / nearT;
    extinction = -log(max(transmitted, float3(1e-6))) / length;
    const float3 x = extinction * length;
    const float3 fraction = float3(x.x < 1e-3 ? 1.0 - (0.5 * x.x) : (1.0 - exp(-x.x)) / x.x,
                                   x.y < 1e-3 ? 1.0 - (0.5 * x.y) : (1.0 - exp(-x.y)) / x.y,
                                   x.z < 1e-3 ? 1.0 - (0.5 * x.z) : (1.0 - exp(-x.z)) / x.z);
    source = added / (length * fraction);
}
#endif

[shader("compute")]
[numthreads(8, 8, 1)]
void cyVolumetricFog(uint3 id : SV_DispatchThreadID)
{
    const float4 forward = fogWord(0u);
    const float4 right = fogWord(1u);
    const float4 up = fogWord(2u);
    const float4 shape = fogWord(3u);
    const float4 planes = fogWord(4u);
    const uint width = uint(shape.x);
    const uint height = uint(shape.y);
    const uint depth = uint(shape.z);
    if (id.x == 0u && id.y == 0u)
    {
#if defined(CY_FOG_AIR_TABLE)
        // The atmosphere table's five header words: the eye is not one of them, because its
        // sampler is handed offsets from the eye.
        for (uint word = 0u; word < 4u; ++word)
        {
            cyFogSet.table[word] = fogWord(word);
        }
        cyFogSet.table[4u] = float4(planes.x, planes.y, 1.0, 0.0);
#else
        for (uint texel = 0u; texel < kCyFogHeaderTexels; ++texel)
        {
            cyFogSet.output[uint2(texel, 0u)] = fogWord(texel);
        }
#endif
    }
    if (id.x >= width || id.y >= height)
    {
        return;
    }

    const float ndcX = (((float(id.x) + 0.5) / float(width)) * 2.0) - 1.0;
    const float ndcY = (((float(id.y) + 0.5) / float(height)) * 2.0) - 1.0;
    const float3 direction =
        normalize(forward.xyz + (right.xyz * (ndcX * right.w)) + (up.xyz * (ndcY * up.w)));
    const float forwardCosine = max(dot(direction, forward.xyz), 1.0e-3);
    const float4 sun = fogWord(7u);
    const float cosToSun = dot(direction, sun.xyz);
    const float3 eye = fogWord(5u).xyz;
    const float3 worldOffset = fogWord(6u).xyz;
    const float3 sunIlluminance = fogWord(8u).xyz;
    const float3 ambient = fogWord(9u).xyz;
    const uint steps = max(uint(planes.z), 1u);

    float previous = 0.0;
#if defined(CY_FOG_AIR_TABLE)
    const bool withAir = planes.w > 0.5;
    float3 carried = float3(1.0);
#else
    float carried = 1.0;
#endif
    float3 scattered = float3(0.0);
    for (uint slice = 0u; slice < depth; ++slice)
    {
        const float distance = fogSliceDepth(shape, planes, slice) / forwardCosine;
        const float step = max(distance - previous, 0.0) / float(steps);
#if defined(CY_FOG_AIR_TABLE)
        float3 airExtinction = float3(0.0);
        float3 airSource = float3(0.0);
        if (withAir)
        {
            fogAirStretch(direction, previous, distance, airExtinction, airSource);
        }
#endif
        for (uint sub = 0u; sub < steps; ++sub)
        {
            const float along = previous + ((float(sub) + 0.5) * step);
            const float3 point = eye + (direction * along);
            const FogMediumSample medium = fogSampleMedium(point + worldOffset, cosToSun);
            const float visibility = fogShadowVisibility(point);
            const float3 source = (medium.sunScattering * sunIlluminance * visibility) +
                                  (medium.scattering * ambient) + medium.emission;
#if defined(CY_FOG_AIR_TABLE)
            // Fog and air as one medium over the sub-step, per channel.
            const float3 extinction3 = (max(medium.extinction, 0.0) + airExtinction) * step;
            const float3 transmitted = exp(-extinction3);
            const float3 sigma = max(float3(medium.extinction) + airExtinction, float3(1e-6));
            const float3 integrated =
                float3(extinction3.x > 1e-6 ? (1.0 - transmitted.x) / sigma.x : step,
                       extinction3.y > 1e-6 ? (1.0 - transmitted.y) / sigma.y : step,
                       extinction3.z > 1e-6 ? (1.0 - transmitted.z) / sigma.z : step);
            scattered += (source + airSource) * integrated * carried;
            carried *= transmitted;
#else
            // `integrate_froxel`: the analytic integral of a homogeneous sub-step.
            const float extinction = max(medium.extinction, 0.0) * step;
            const float sliceTransmittance = exp(-extinction);
            const float integrated = extinction > 1e-6
                                         ? (1.0 - sliceTransmittance) / max(medium.extinction, 1e-6)
                                         : step;
            scattered += source * integrated * carried;
            carried *= sliceTransmittance;
#endif
        }
        previous = distance;
#if defined(CY_FOG_AIR_TABLE)
        const uint froxel = (slice * height * width) + (id.y * width) + id.x;
        cyFogSet.table[5u + (froxel * 2u)] = float4(carried, 0.0);
        cyFogSet.table[5u + (froxel * 2u) + 1u] = float4(scattered, 0.0);
#else
        const uint row = 1u + (slice * height) + id.y;
        cyFogSet.output[uint2(id.x, row)] = float4(carried, carried, carried, 1.0);
        cyFogSet.output[uint2(width + id.x, row)] = float4(scattered, 1.0);
#endif
    }
}

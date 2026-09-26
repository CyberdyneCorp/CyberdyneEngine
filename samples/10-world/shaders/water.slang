// SPDX-License-Identifier: MIT
// The water surface samples/10-world draws when water shading is on: reflection, refraction with
// absorption over the water column, shoreline foam and caustics on the bed.
//
// ================================================================================================
// WHAT THIS SHADER IS, AND WHAT IT READS
// ================================================================================================
//
// `water` — "Water surface shading": the closure "SHALL account for: reflection, refraction,
// wavelength-dependent absorption and scattering through the water column, surface roughness,
// normals, and foam coverage", and "Colour with depth SHALL follow physically-inspired attenuation
// over the water column thickness". This is the fragment stage that evaluates those terms for the
// one water body the world has. Its inputs are two pictures the stage draws before the frame's
// opaque pass, with `world.slang`'s own pipeline, and one block of numbers:
//
//   the refraction   the terrain alone, drawn with the frame's camera, colour and depth. What lies
//                    under the surface at every pixel, and how far below it lies.
//   the reflection   the sky, the terrain, the stars and the plants drawn MIRRORED in the water
//                    plane, with a near plane moved onto the water so nothing below it is drawn.
//   the parameters   the body's optics from `water::WaterOptics` — extinction and in-scatter per
//                    channel and the Fresnel reflectance at normal incidence — the inverse of the
//                    frame's world-to-clip matrix, and the wave trains the caustics are derived from.
//
// Both pictures are written by `world.slang`'s fragment stage, which tonemaps before it stores. So
// every read is taken back to radiance through `untonemap()` before any light is added, and the
// result goes through the same `tonemap()` on the way out: absorption is multiplicative on
// radiance, not on a display value.
//
// ================================================================================================
// WHY THE REFLECTION IS PLANAR AND NOT SCREEN-SPACE
// ================================================================================================
//
// The one body this frame draws is a sea at one mean level, which is the case a planar reflection
// is exact for: every reflected ray is the eye's ray mirrored in one plane. A screen-space trace
// cannot reflect what is off the top of the screen — the hilltops and the sky above the frame,
// which is most of what a fjord reflects from a low camera — and would need the opaque pass's depth,
// which this frame's transparent stage has no way to read without a copy the assembly does not
// declare. The mirrored picture costs one extra draw of the world and needs no fallback: the sky
// dome is in it, so wherever no hill is reflected the sky already is.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc water.slang -I ../../../src/rendering/shaders -target spirv -profile spirv_1_5 \
//          -entry waterVertex -stage vertex   -o water_vertex.spv
//   slangc water.slang -I ../../../src/rendering/shaders -target spirv -profile spirv_1_5 \
//          -entry waterFragment -stage fragment -o water_fragment.spv
//   python3 embed_spirv.py water_spirv.h \
//          kWaterVertexSpirv=water_vertex.spv \
//          kWaterFragmentSpirv=water_fragment.spv
//   slangc water.slang -I ../../../src/rendering/shaders -target metal \
//          -entry waterVertex -stage vertex -o water_vertex.metal
//   slangc water.slang -I ../../../src/rendering/shaders -target metal \
//          -entry waterFragment -stage fragment -o water_fragment.metal
//   python3 embed_msl.py water_msl.h \
//          kWaterVertexMsl=water_vertex.metal \
//          kWaterFragmentMsl=water_fragment.metal
//   clang-format -i water_spirv.h water_msl.h

import cy.cloud_shadow;
import cy.field;
import cy.noise;

/// `WorldPush` in world.slang, the same 128 bytes: the stage hands the water run the push block
/// every other run of the frame is drawn with.
struct WorldPush
{
    float4 row0;
    float4 row1;
    float4 row2;
    float4 row3;
    float4 light;
    float4 eye;
    float4 sun;
    float4 ambient;
};

/// The most wave trains the caustics sum. The stage keeps the ones that bend light the most.
static const uint kCausticTrains = 16;

/// One element. Everything the surface needs that is not per vertex, as float4s so its layout is
/// the same on every target.
struct WaterParams
{
    /// Rows of the inverse of the frame's world-to-clip matrix: clip space back to world-relative.
    float4 inverse0;
    float4 inverse1;
    float4 inverse2;
    float4 inverse3;
    /// x, y: the target's size in pixels. z: how far, in pixels, a wave normal of slope one moves
    /// the refracted read. w: the same for the reflected read.
    float4 viewport;
    /// rgb: extinction per metre, absorption plus scattering. w: the depth, in metres, below which
    /// the shoreline foams; zero draws no shoreline foam.
    float4 extinction;
    /// rgb: the in-scattered colour a column returns as it becomes opaque, the single-scattering
    /// albedo times the body's scatter colour. w: the Fresnel reflectance at normal incidence.
    float4 inScatter;
    /// x: the water's mean level, world-relative metres. y: seconds, for the foam's breakup.
    /// z: caustic strength, zero for none. w: 1 - 1/n, how much a refracted ray bends per unit slope.
    float4 surface;
    /// x: wave trains in use. y: the brightest a caustic may focus to, times the unfocused light.
    float4 caustic;
    /// Per train: xy its direction of travel, z its wavenumber, w its amplitude.
    float4 trains[kCausticTrains];
    /// Per train, four to an element: its phase at this frame's time and at the world-relative
    /// origin, reduced on the processor so that no large number reaches the device.
    float4 phases[kCausticTrains / 4];
};

/// Set 0: the cloud shadow field, exactly as world.slang declares it, so the water's sun is the one
/// the rest of the world is lit by. DECLARED FIRST, for the Metal buffer order world.slang explains.
struct WaterCloudShadow
{
    StructuredBuffer<uint> field;
    StructuredBuffer<float4> placement;
};

/// Set 1: what only the water reads.
struct WaterSurface
{
    Texture2D<float4> refraction;
    Texture2D<float> refractionDepth;
    Texture2D<float4> reflection;
    StructuredBuffer<WaterParams> params;
};

[[vk::binding(0, 0)]]
ParameterBlock<WaterCloudShadow> cloudShadow;

[[vk::binding(0, 1)]]
ParameterBlock<WaterSurface> water;

[[vk::push_constant]]
ConstantBuffer<WorldPush> push;

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float3 color : COLOR;
};

struct VertexOutput
{
    float4 clip : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 color : TEXCOORD2;
};

[shader("vertex")]
VertexOutput waterVertex(VertexInput input)
{
    let point = float4(input.position, 1.0);
    VertexOutput output;
    output.clip = float4(dot(push.row0, point), dot(push.row1, point), dot(push.row2, point),
                         dot(push.row3, point));
    output.world = input.position;
    output.normal = input.normal;
    output.color = input.color;
    return output;
}

/// world.slang's `tonemap()`, the same two lines.
float3 tonemap(float3 radiance)
{
    let mapped = radiance / (1.0 + radiance);
    return pow(saturate(mapped), 1.0 / 2.2);
}

/// The inverse of `tonemap()`: a value world.slang stored, back to the radiance it was given.
/// Clamped short of one, where Reinhard's inverse has its pole.
float3 untonemap(float3 display)
{
    let mapped = min(pow(saturate(display), 2.2), 0.999);
    return mapped / (1.0 - mapped);
}

float sunThroughClouds(float3 world)
{
    const float4 placement = cloudShadow.placement[0];
    if (placement.z < 0.5)
    {
        return 1.0;
    }
    CyFieldImage image;
    image.words = cloudShadow.field;
    return cyCloudShadowAtImage(image, world.x + placement.x, world.y, world.z + placement.y);
}

/// The world-relative position a pixel of the refraction picture holds, from its stored depth.
float3 worldAt(WaterParams params, float2 pixelCentre, float depth)
{
    let ndc = float4((pixelCentre.x * 2.0 / params.viewport.x) - 1.0,
                     1.0 - (pixelCentre.y * 2.0 / params.viewport.y), depth, 1.0);
    let point = float4(dot(params.inverse0, ndc), dot(params.inverse1, ndc),
                       dot(params.inverse2, ndc), dot(params.inverse3, ndc));
    return point.xyz / point.w;
}

int2 clampPixel(WaterParams params, int2 pixel)
{
    return clamp(pixel, int2(0, 0), int2(params.viewport.xy) - int2(1, 1));
}

/// How much the surface above `xz` concentrates the sun on a bed `depth` metres below it.
///
/// SURFACE-DERIVED, the tier `water` makes the default: a refracted ray leaves a surface of slope
/// grad(h) displaced by depth * (1 - 1/n) * grad(h) by the time it reaches the bed, so the bed's
/// area element is scaled by the Jacobian 1 + depth * (1 - 1/n) * laplacian(h) and the light on it
/// by the reciprocal. A crest's negative curvature focuses; a trough's spreads. The Laplacian is the
/// analytic one of the very trains `water::evaluate_trains` displaces the surface by.
///
/// FILTERED BY THE PIXEL'S FOOTPRINT ON THE BED. A train whose wavelength is under two pixels of bed
/// cannot be drawn, only aliased — the shortest trains curve the surface most and would otherwise
/// turn a distant shallow into moire. Each train fades out between a quarter and a half of a
/// wavelength per pixel, which is what a band-limited evaluation of the same sum would keep.
float causticFocus(WaterParams params, float2 xz, float depth, float footprint)
{
    float laplacian = 0.0;
    const uint count = min(uint(params.caustic.x), kCausticTrains);
    for (uint index = 0; index < count; ++index)
    {
        let train = params.trains[index];
        let phase = params.phases[index / 4][index % 4];
        let theta = dot(train.xy, xz) * train.z + phase;
        let resolved = saturate(2.0 - (train.z * footprint * (2.0 / 3.14159265)));
        laplacian -= resolved * train.w * train.z * train.z * cos(theta);
    }
    let jacobian = 1.0 + depth * params.surface.w * laplacian;
    return min(1.0 / max(abs(jacobian), 1e-4), params.caustic.y);
}

/// Foam's breakup: two octaves of value noise drifting slowly, never below half. A band of
/// shoreline foam of one uniform white reads as a painted stripe.
float foamBreakup(float2 xz, float seconds)
{
    let coarse = valueNoise(float3(xz * 0.35, seconds * 0.15));
    let fine = valueNoise(float3(xz * 1.3 + 17.0, seconds * 0.4));
    return 0.5 + 0.5 * saturate(coarse * 0.65 + fine * 0.35);
}

[shader("fragment")]
float4 waterFragment(VertexOutput input) : SV_Target
{
    let params = water.params[0];

    float3 normal = normalize(input.normal);
    let toEye = normalize(push.eye.xyz - input.world);
    if (dot(normal, toEye) < 0.0)
    {
        normal = -normal;
    }

    let direct = push.sun.rgb * sunThroughClouds(input.world);
    let lambert = saturate(dot(normal, -push.light.xyz));
    // The light the column scatters back towards the eye: the sky's and the sun's, as they reach the
    // surface. The column's in-scatter colour is an albedo, like the colour it replaces was.
    let columnLight = push.ambient.rgb + direct * saturate(-push.light.y);

    // --- WHAT IS UNDER THE SURFACE. The bed straight behind this pixel first, which is what the
    // shoreline and the foam are measured against, and then the bed the wave's slope bends the ray
    // to — unless that read lands on something in front of the water, a bank, where the straight
    // one is kept.
    let pixel = int2(input.clip.xy);
    let straightDepth = water.refractionDepth.Load(int3(pixel, 0));
    let hasBed = straightDepth > 0.0;
    let straightBed = worldAt(params, float2(pixel) + 0.5, straightDepth);
    let column = hasBed ? max(input.world.y - straightBed.y, 0.0) : 1.0e4;
    // Metres of bed one pixel covers, taken here in uniform control flow where derivatives exist.
    let footprint = max(length(ddx(straightBed.xz)), length(ddy(straightBed.xz)));

    let bend = normal.xz * params.viewport.z * saturate(column * 0.5);
    int2 bentPixel = clampPixel(params, pixel + int2(round(bend)));
    float bentDepth = water.refractionDepth.Load(int3(bentPixel, 0));
    if (bentDepth > input.clip.z || (hasBed && bentDepth <= 0.0))
    {
        bentPixel = pixel;
        bentDepth = straightDepth;
    }
    let bed = worldAt(params, float2(bentPixel) + 0.5, bentDepth);
    let path = bentDepth > 0.0 ? length(bed - input.world) : 1.0e4;

    // --- THE COLUMN: Beer-Lambert over the path, plus what the column scatters back. The same
    // arithmetic as `water::water_column_colour()`, with the body's own coefficients.
    let transmittance = exp(-params.extinction.rgb * path);
    float3 bedRadiance = untonemap(water.refraction.Load(int3(bentPixel, 0)).rgb);
    if (params.surface.z > 0.0 && bentDepth > 0.0)
    {
        // Caustics move only the SUN's share of the bed's light, weakened by the water the sun
        // came down through — `water::underwater_state()`'s strength, on green for luminance.
        let bedDepth = max(params.surface.x - bed.y, 0.0);
        let sunOnBed = direct * saturate(-push.light.y);
        let share = dot(sunOnBed, 1.0) / max(dot(sunOnBed + push.ambient.rgb, 1.0), 1e-6);
        let reach = exp(-params.extinction.g * bedDepth);
        let focus = causticFocus(params, bed.xz, bedDepth, footprint);
        bedRadiance *= 1.0 + params.surface.z * share * reach * (focus - 1.0);
    }
    let refracted =
        bedRadiance * transmittance + (1.0 - transmittance) * params.inScatter.rgb * columnLight;

    // --- THE SURFACE: Schlick's Fresnel between the refracted column and the mirrored world.
    let reflectedPixel = clampPixel(params, pixel + int2(round(normal.xz * params.viewport.w)));
    let reflected = untonemap(water.reflection.Load(int3(reflectedPixel, 0)).rgb);
    let f0 = params.inScatter.w;
    let fresnel = f0 + (1.0 - f0) * pow(1.0 - saturate(dot(normal, toEye)), 5.0);
    float3 lit = lerp(refracted, reflected, fresnel);

    // world.slang's glitter lobe, unchanged, so the sun's path on the sea is the one it always was.
    let half = normalize(-push.light.xyz + toEye);
    lit += direct * pow(saturate(dot(normal, half)), 120.0) * push.sun.a;

    // --- FOAM: the shoreline's, from the depth of the column under this pixel, and the open sea's
    // breaking foam the device foam pass already wrote into the vertex colour.
    float shore = 0.0;
    if (params.extinction.w > 0.0 && hasBed)
    {
        shore = saturate(1.0 - column / params.extinction.w) *
                foamBreakup(input.world.xz, params.surface.y);
    }
    let open = saturate((input.color.r - 0.02) / 0.83);
    let foam = max(shore, open);
    let foamRadiance = 0.9 * (push.ambient.rgb + direct * lambert);
    lit = lerp(lit, foamRadiance, foam);

    return float4(tonemap(lit), 1.0);
}

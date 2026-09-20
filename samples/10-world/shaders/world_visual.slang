// Device-owned visual producers for the integrated world frame.

import cy.field;
import cy.noise;
import cy.terrain_shade;

struct VisualPush
{
    uint terrainCount;
    uint skyStart;
    uint skyCount;
    uint waterStart;
    uint waterCount;
    uint foamResolution;
    uint frameIndex;
    uint cloudSeed;
    float timeSeconds;
    float deltaSeconds;
    float wetness;
    float snowDepth;
    float cloudCoverage;
    float sunHeight;
    float exposure;
    float unusedFloat;
    float4 fieldOrigin;
};

struct VisualResources
{
    ByteAddressBuffer terrainVertices;
    RWByteAddressBuffer terrainColours;
    ByteAddressBuffer dynamicVertices;
    RWByteAddressBuffer dynamicColours;
    StructuredBuffer<float> foamPrevious;
    RWStructuredBuffer<float> foamNext;
    StructuredBuffer<uint> waterDistanceField;
    StructuredBuffer<uint> wetnessField;
    StructuredBuffer<uint> snowDepthField;
    StructuredBuffer<uint> vegetationField;
};

[[vk::binding(0, 0)]] ParameterBlock<VisualResources> resources;

[[vk::push_constant]] ConstantBuffer<VisualPush> visual;

float3 loadFloat3(ByteAddressBuffer input, uint address)
{
    return asfloat(input.Load3(address));
}

void storeFloat3(RWByteAddressBuffer output, uint address, float3 value)
{
    output.Store3(address, asuint(value));
}

float cloudFbm(float3 position, float3 seedOffset)
{
    const float coarse = valueNoise(position + seedOffset);
    const float detail = valueNoise((position * 2.03) + seedOffset + float3(17.0, 31.0, 47.0));
    return (coarse + detail * 0.5) / 1.5;
}

float cloudDensity(float3 position, float coverage, float3 seedOffset)
{
    const float heightFraction = saturate((position.y - 900.0) / 3400.0);
    const float lower = smoothstep(0.0, 0.18, heightFraction);
    const float upper = 1.0 - smoothstep(0.68, 1.0, heightFraction);
    const float profile = lower * upper;
    const float3 advected = position - float3(visual.timeSeconds * 9.0, 0.0,
                                               visual.timeSeconds * 2.5);
    const float base = cloudFbm(advected / 5200.0, seedOffset);
    const float threshold = 0.70 - coverage * 0.38;
    float shape = saturate((base - threshold) * 4.5);
    const float erosion = valueNoise((advected / 780.0) + seedOffset + float3(71.0, 11.0, 29.0));
    shape *= 0.62 + erosion * 0.58;
    return shape * profile;
}

float3 composeClouds(float3 direction)
{
    const float horizon = saturate(direction.y * 0.5 + 0.5);
    const float daylight = saturate(visual.sunHeight * 3.0 + 0.25);
    float3 clear = lerp(float3(0.002, 0.005, 0.018), float3(0.16, 0.43, 0.92), daylight);
    clear *= 0.16 + 0.84 * horizon;
    if (direction.y <= 0.025)
        return clear;

    const float seed = float(visual.cloudSeed & 255u);
    const float3 seedOffset = float3(seed * 0.37, seed * 0.19, seed * 0.53);
    const float3 origin = float3(visual.fieldOrigin.z, 0.0, visual.fieldOrigin.w);
    const float start = 900.0 / direction.y;
    const float stepLength = 3400.0 / (direction.y * 12.0);
    float transmittance = 1.0;
    float3 scattering = float3(0.0);
    [ForceUnroll]
    for (uint step = 0; step < 12u; ++step)
    {
        const float distance = start + stepLength * (float(step) + 0.5);
        const float3 position = origin + direction * distance;
        const float density = cloudDensity(position, visual.cloudCoverage, seedOffset);
        const float extinction = 1.0 - exp(-density * 0.48);
        const float heightFraction = saturate((position.y - 900.0) / 3400.0);
        const float3 dark = float3(0.14, 0.17, 0.22);
        const float3 bright = float3(1.00, 0.94, 0.84) * (0.30 + daylight * 0.70);
        const float3 source = lerp(dark, bright, 0.25 + heightFraction * 0.75);
        scattering += source * (transmittance * extinction);
        transmittance *= 1.0 - extinction;
        if (transmittance < 0.015)
            break;
    }
    return clear * transmittance + scattering;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void shadeTerrain(uint index : SV_DispatchThreadID)
{
    if (index >= visual.terrainCount)
        return;
    const uint vertexAddress = index * 24u;
    const float3 position = loadFloat3(resources.terrainVertices, vertexAddress);
    const float3 normal = loadFloat3(resources.terrainVertices, vertexAddress + 12u);
    const float slope = 1.0 - saturate(normal.y);
    CyFieldImage waterDistance;
    waterDistance.words = resources.waterDistanceField;
    CyFieldImage wetness;
    wetness.words = resources.wetnessField;
    CyFieldImage snowDepth;
    snowDepth.words = resources.snowDepthField;
    CyFieldImage vegetation;
    vegetation.words = resources.vegetationField;
    CyTerrainPalette palette;
    palette.rock = float3(0.33, 0.31, 0.30);
    palette.sand = float3(0.62, 0.56, 0.42);
    palette.grass = float3(0.24, 0.34, 0.17);
    palette.alpine = float3(0.44, 0.43, 0.40);
    palette.snow = float3(0.86, 0.88, 0.92);
    const float3 colour = cyTerrainShadeAtImages(
        waterDistance, wetness, snowDepth, vegetation, palette,
        position.x + visual.fieldOrigin.x, position.y,
        position.z + visual.fieldOrigin.y, slope, position.y, 132.0);
    storeFloat3(resources.terrainColours, index * 12u, colour);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void shadeClouds(uint index : SV_DispatchThreadID)
{
    if (index >= visual.skyCount)
        return;
    const uint vertex = visual.skyStart + index;
    const float3 direction = normalize(loadFloat3(resources.dynamicVertices, vertex * 24u + 12u));
    storeFloat3(resources.dynamicColours, vertex * 12u, composeClouds(direction));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void evolveFoam(uint index : SV_DispatchThreadID)
{
    const uint cells = visual.foamResolution * visual.foamResolution;
    if (index < cells)
    {
        const uint x = index % visual.foamResolution;
        const uint y = index / visual.foamResolution;
        const uint left = y * visual.foamResolution +
                          ((x + visual.foamResolution - 1u) % visual.foamResolution);
        const float injected = saturate(sin(float(x) * 0.17 + visual.timeSeconds * 0.7) *
                                          cos(float(y) * 0.11 - visual.timeSeconds * 0.4) - 0.72);
        resources.foamNext[index] =
            max(resources.foamPrevious[left] * exp(-visual.deltaSeconds / 9.0), injected);
    }
    if (index < visual.waterCount)
    {
        const uint vertex = visual.waterStart + index;
        const float3 position = loadFloat3(resources.dynamicVertices, vertex * 24u);
        const uint x = uint(abs(position.x) * 0.25) % visual.foamResolution;
        const uint y = uint(abs(position.z) * 0.25) % visual.foamResolution;
        const float foam = saturate(sin(float(x) * 0.17 + visual.timeSeconds * 0.7) *
                                    cos(float(y) * 0.11 - visual.timeSeconds * 0.4) - 0.72);
        const float3 deep = float3(0.02, 0.05, 0.07);
        storeFloat3(resources.dynamicColours, vertex * 12u,
                    lerp(deep, float3(0.85, 0.88, 0.92), saturate(foam)));
    }
}

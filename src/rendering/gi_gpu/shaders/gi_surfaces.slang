// SPDX-License-Identifier: MIT
// CyberGI on the device, stage 4: the frame's prepass as the resolve's surfaces. Issue #35.
//
//   cyGiUnpackSurfaces   the depth and the octahedral normal the `DepthNormal` prepass wrote, as a
//                        world position, a view depth, a unit normal and a roughness a pixel — the
//                        record `ScreenSurfaces` describes — for every pixel on the resolve's grid
//                        (`ScreenGrid`: each `divisor`-th pixel, the block's top-left), and the
//                        normal and view depth of EVERY pixel, which the publish upsamples against.
//
// The reconstruction is `cy::rendering-occlusion`'s, the one GTAO is measured with: reversed-Z
// depth to a positive view depth through the projection's (2,2) and (3,2) terms, a pixel centre to
// a view-space point (pixel row zero is NDC y = +1) with the prepass's jitter undone, then the
// view's rotation back to the camera-relative frame the forward pass shades in, and the camera's
// world position added. The normal is camera-relative already, which is world-oriented.

import cy.packing;

static const uint kGsCounts = 0u;          // grid width, grid height, grid pixels, current slot
static const uint kGsCamera = 1u;          // camera xyz (world)
static const uint kGsUnpack = 14u;         // P00, P11, m22, m32
static const uint kGsViewToRelative = 15u; // three rows
static const uint kGsJitter = 19u;         // xy: what to add to a jittered pixel centre
static const uint kGsFull = 36u;           // frame width, frame height, grid divisor, flags

struct GiSurfaceSet {
    StructuredBuffer<uint4> constants;
    /// A depth texture on Metal (`depth2d`), which is what the prepass target IS there.
    DepthTexture2D depth;
    Texture2D<float4> normals;
    RWStructuredBuffer<float4> surfaces;
    /// One float4 a frame pixel: [unit normal, view depth (zero on the sky)].
    RWStructuredBuffer<float4> guidance;
};

[[vk::binding(0, 0)]] ParameterBlock<GiSurfaceSet> giSurfaces;

[shader("compute")]
[numthreads(8, 8, 1)]
void cyGiUnpackSurfaces(uint3 thread : SV_DispatchThreadID) {
    const uint4 full = giSurfaces.constants[kGsFull];
    if (thread.x >= full.x || thread.y >= full.y) {
        return;
    }
    const uint4 counts = giSurfaces.constants[kGsCounts];
    const uint divisor = max(full.z, 1u);
    const uint pixel = (thread.y * full.x) + thread.x;
    const bool onGrid = (thread.x % divisor) == 0u && (thread.y % divisor) == 0u;
    const uint cell = ((thread.y / divisor) * counts.x) + (thread.x / divisor);
    const uint record = ((counts.w * counts.z) + cell) * 2u;
    const float depth = giSurfaces.depth.Load(int3(int2(thread.xy), 0));
    if (depth <= 0.0f) {
        giSurfaces.guidance[pixel] = float4(0.0f, 1.0f, 0.0f, 0.0f);
        if (onGrid) {
            giSurfaces.surfaces[record] = float4(0.0f, 0.0f, 0.0f, 0.0f);
            giSurfaces.surfaces[record + 1u] = float4(0.0f, 1.0f, 0.0f, 1.0f);
        }
        return;
    }
    const float4 projection = asfloat(giSurfaces.constants[kGsUnpack]);
    const float viewDepth = projection.w / (depth + projection.z);
    const float4 packed = giSurfaces.normals.Load(int3(int2(thread.xy), 0));
    const float3 normal = decodeOctahedral(packed.xy);
    giSurfaces.guidance[pixel] = float4(normal, viewDepth);
    if (!onGrid) {
        return;
    }
    const float2 jitter = asfloat(giSurfaces.constants[kGsJitter].xy);
    const float2 uv = (float2(thread.xy) + 0.5f + jitter) / float2(float(full.x), float(full.y));
    const float2 ndc = float2((uv.x * 2.0f) - 1.0f, 1.0f - (uv.y * 2.0f));
    const float3 view =
        float3(ndc.x * viewDepth / projection.x, ndc.y * viewDepth / projection.y, -viewDepth);
    const float3 row0 = asfloat(giSurfaces.constants[kGsViewToRelative].xyz);
    const float3 row1 = asfloat(giSurfaces.constants[kGsViewToRelative + 1u].xyz);
    const float3 row2 = asfloat(giSurfaces.constants[kGsViewToRelative + 2u].xyz);
    const float3 relative = float3(dot(row0, view), dot(row1, view), dot(row2, view));
    const float3 camera = asfloat(giSurfaces.constants[kGsCamera].xyz);
    giSurfaces.surfaces[record] = float4(relative + camera, viewDepth);
    giSurfaces.surfaces[record + 1u] = float4(normal, packed.z);
}

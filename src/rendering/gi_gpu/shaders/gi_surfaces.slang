// SPDX-License-Identifier: MIT
// CyberGI on the device, stage 4: the frame's prepass as the resolve's surfaces. Issue #35.
//
//   cyGiUnpackSurfaces   the depth and the octahedral normal the `DepthNormal` prepass wrote, as a
//                        world position, a view depth, a unit normal and a roughness a pixel —
//                        the record `ScreenSurfaces` describes and the resolve and the denoiser read.
//
// The reconstruction is `cy::rendering-occlusion`'s, the one GTAO is measured with: reversed-Z
// depth to a positive view depth through the projection's (2,2) and (3,2) terms, a pixel centre to
// a view-space point (pixel row zero is NDC y = +1), then the view's rotation back to the
// camera-relative frame the forward pass shades in, and the camera's world position added. The
// normal is camera-relative already, which is world-oriented.

import cy.packing;

static const uint kGsCounts = 0u;          // width, height, pixels, current slot
static const uint kGsCamera = 1u;          // camera xyz (world)
static const uint kGsUnpack = 14u;         // P00, P11, m22, m32
static const uint kGsViewToRelative = 15u; // three rows

struct GiSurfaceSet {
    StructuredBuffer<uint4> constants;
    /// A depth texture on Metal (`depth2d`), which is what the prepass target IS there.
    DepthTexture2D depth;
    Texture2D<float4> normals;
    RWStructuredBuffer<float4> surfaces;
};

[[vk::binding(0, 0)]] ParameterBlock<GiSurfaceSet> giSurfaces;

[shader("compute")]
[numthreads(8, 8, 1)]
void cyGiUnpackSurfaces(uint3 thread : SV_DispatchThreadID) {
    const uint4 counts = giSurfaces.constants[kGsCounts];
    if (thread.x >= counts.x || thread.y >= counts.y) {
        return;
    }
    const uint pixel = (thread.y * counts.x) + thread.x;
    const uint record = ((counts.w * counts.z) + pixel) * 2u;
    const float depth = giSurfaces.depth.Load(int3(int2(thread.xy), 0));
    if (depth <= 0.0f) {
        giSurfaces.surfaces[record] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        giSurfaces.surfaces[record + 1u] = float4(0.0f, 1.0f, 0.0f, 1.0f);
        return;
    }
    const float4 projection = asfloat(giSurfaces.constants[kGsUnpack]);
    const float viewDepth = projection.w / (depth + projection.z);
    const float2 uv = (float2(thread.xy) + 0.5f) / float2(float(counts.x), float(counts.y));
    const float2 ndc = float2((uv.x * 2.0f) - 1.0f, 1.0f - (uv.y * 2.0f));
    const float3 view =
        float3(ndc.x * viewDepth / projection.x, ndc.y * viewDepth / projection.y, -viewDepth);
    const float3 row0 = asfloat(giSurfaces.constants[kGsViewToRelative].xyz);
    const float3 row1 = asfloat(giSurfaces.constants[kGsViewToRelative + 1u].xyz);
    const float3 row2 = asfloat(giSurfaces.constants[kGsViewToRelative + 2u].xyz);
    const float3 relative = float3(dot(row0, view), dot(row1, view), dot(row2, view));
    const float3 camera = asfloat(giSurfaces.constants[kGsCamera].xyz);
    const float4 packed = giSurfaces.normals.Load(int3(int2(thread.xy), 0));
    giSurfaces.surfaces[record] = float4(relative + camera, viewDepth);
    giSurfaces.surfaces[record + 1u] = float4(decodeOctahedral(packed.xy), packed.z);
}

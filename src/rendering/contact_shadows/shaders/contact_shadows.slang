// SPDX-License-Identifier: MIT
// Screen-space contact shadows: a short trace toward the directional light through the prepass
// depth. `virtual-shadows` — "Contact and traced refinement".
//
// ================================================================================================
// WHAT THIS COMPUTES
// ================================================================================================
//
// For each pixel: reconstruct its view-space position from the depth, lift it off its own surface by
// one pixel's footprint along the prepass normal, and march `steps` taps toward the light over
// `length` metres. A tap that projects BEHIND the surface the depth buffer holds at its pixel, by
// less than `thickness`, is inside an occluder. `contact.h` says why the start is lifted and why the
// term fades over the second half of the trace. Every expression has a C++ twin in
// src/rendering/contact_shadows/src/contact.cpp, in the same order, because the device suite
// compares the two images rather than a picture.
//
// ================================================================================================
// OUTPUT
// ================================================================================================
//
//   r    visibility in [0, 1]: 1 lit, 0 in contact shadow. gba: 1.
//
// A sky pixel, a pixel beyond `control.z` and a pixel facing away from the light write 1: nothing
// here is darker than the shadow map says unless the trace found an occluder.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   python3 src/rendering/contact_shadows/shaders/regenerate.py --slangc <build>/Development/bin/slangc
//
// which runs, from the repository root:
//
//   slangc src/rendering/contact_shadows/shaders/contact_shadows.slang -I src/rendering/shaders \
//          -entry cyContactShadows -stage compute -target spirv -profile spirv_1_5 -o contact.spv
//   slangc ... -target metal -o contact.metal
//
// then embed_spirv.py and embed_msl.py into ../src/contact_spirv.h and ../src/contact_msl.h, and
// clang-format over both.

import cy.packing;

/// `cy::rendering::contact_shadows::ContactShadowConstants`, word for word. 128 bytes: exactly the
/// push-constant size every backend guarantees.
struct CyContactConstants
{
    /// Camera-relative to view space, the rotation's three rows.
    float4 viewRow0;
    float4 viewRow1;
    float4 viewRow2;
    /// x: P00, y: P11, z: m22, w: m32.
    float4 projection;
    /// xy: the extent in pixels, zw: its reciprocal.
    float4 extent;
    /// xyz: the view-space direction to the light. w: the trace length in metres.
    float4 light;
    /// x: steps, y: thickness in metres, z: the largest traced view depth, w: the start's lift
    /// along the normal in pixel footprints.
    float4 control;
    /// xy: what to add to a pixel centre of the JITTERED depth to reach the unjittered screen
    /// position the projection above reconstructs. zw: unused.
    float4 jitter;
};

struct CyContactSet
{
    /// A depth texture on Metal (`depth2d`), which is what the prepass target IS there.
    DepthTexture2D depth;
    Texture2D<float4> normals;
    [format("rgba8")] RWTexture2D<float4> output;
};

[[vk::binding(0, 0)]] ParameterBlock<CyContactSet> cyContactSet;
[[vk::push_constant]] CyContactConstants cyContactConstants;

/// Reversed-Z depth to positive view depth: -z = m32 / (d + m22). `cyGtaoViewDepth`'s arithmetic.
float viewDepthOf(float depth)
{
    return cyContactConstants.projection.w / (depth + cyContactConstants.projection.z);
}

float3 toView(float3 relative)
{
    return float3(dot(cyContactConstants.viewRow0.xyz, relative),
                  dot(cyContactConstants.viewRow1.xyz, relative),
                  dot(cyContactConstants.viewRow2.xyz, relative));
}

/// Interleaved gradient noise at the pixel centre: the fraction of a step every tap is offset by.
float cyContactNoise(uint2 pixel)
{
    let centre = float2(pixel) + 0.5;
    return frac(52.9829189 * frac(dot(centre, float2(0.06711056, 0.00583715))));
}

float traceVisibility(uint2 pixel)
{
    let depth = cyContactSet.depth.Load(int3(int2(pixel), 0));
    if (depth <= 0.0)
    {
        return 1.0;
    }
    let projection = cyContactConstants.projection;
    let viewDistance = viewDepthOf(depth);
    if (viewDistance > cyContactConstants.control.z)
    {
        return 1.0;
    }
    let light = cyContactConstants.light.xyz;
    let normal = normalize(toView(decodeOctahedral(cyContactSet.normals.Load(int3(int2(pixel), 0)).xy)));
    if (dot(normal, light) <= 0.0)
    {
        return 1.0;
    }
    let uv = (float2(pixel) + 0.5 + cyContactConstants.jitter.xy) * cyContactConstants.extent.zw;
    let position = float3((uv.x * 2.0 - 1.0) * viewDistance / projection.x,
                          (1.0 - uv.y * 2.0) * viewDistance / projection.y, -viewDistance);
    let footprint = 2.0 * viewDistance / (projection.y * cyContactConstants.extent.y);
    let start = position + normal * (cyContactConstants.control.w * footprint);

    let steps = uint(cyContactConstants.control.x);
    let offset = cyContactNoise(pixel);
    for (uint index = 0; index < steps; ++index)
    {
        let t = (float(index) + offset) / float(steps);
        let tap = start + light * (t * cyContactConstants.light.w);
        let tapDepth = -tap.z;
        if (tapDepth <= 0.0)
        {
            break;
        }
        let ndcX = projection.x * tap.x / tapDepth;
        let ndcY = projection.y * tap.y / tapDepth;
        let screenX = (ndcX * 0.5 + 0.5) * cyContactConstants.extent.x - cyContactConstants.jitter.x;
        let screenY = (0.5 - ndcY * 0.5) * cyContactConstants.extent.y - cyContactConstants.jitter.y;
        if (screenX < 0.0 || screenY < 0.0 || screenX >= cyContactConstants.extent.x ||
            screenY >= cyContactConstants.extent.y)
        {
            break;
        }
        let stored = cyContactSet.depth.Load(int3(int(screenX), int(screenY), 0));
        if (stored <= 0.0)
        {
            continue;
        }
        let behind = tapDepth - viewDepthOf(stored);
        if (behind > 0.0 && behind < cyContactConstants.control.y)
        {
            return saturate((t - 0.5) * 2.0);
        }
    }
    return 1.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cyContactShadows(uint3 thread : SV_DispatchThreadID)
{
    let size = uint2(cyContactConstants.extent.xy);
    if (thread.x >= size.x || thread.y >= size.y)
    {
        return;
    }
    cyContactSet.output[int2(thread.xy)] = float4(traceVisibility(thread.xy), 1.0, 1.0, 1.0);
}

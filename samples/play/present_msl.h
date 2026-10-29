// SPDX-License-Identifier: MIT
#pragma once
// Compiled MSL for the play host's present shader. GENERATED — do not edit by hand.
//
// Produced by samples/play/shaders/regenerate.py from present.slang. Checked in rather than
// compiled by the build because a played game must present in a build with no shader compiler.

#include <cy/core/base/types.h>

namespace cy::play {

/// cyPresentVertex.metal, 869 bytes.
inline constexpr char kPresentVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 43 "samples/play/shaders/present.slang"
struct cyPresentVertex_Result_0
{
    float4 position_0 [[position]];
    float2 uv_0 [[user(TEXCOORD)]];
};


#line 43
struct CyPresentVertex_0
{
    float4 position_1;
    float2 uv_1;
};


#line 43
[[vertex]] cyPresentVertex_Result_0 cyPresentVertex(uint vertexId_0 [[vertex_id]])
{

#line 54
    thread CyPresentVertex_0 output_0;
    float2 _S1 = float2(float((vertexId_0 << 1U) & 2U), float(vertexId_0 & 2U));

#line 55
    (&output_0)->uv_1 = _S1;
    (&output_0)->position_1 = float4(_S1 * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

#line 56
    thread cyPresentVertex_Result_0 _S2;

#line 56
    (&_S2)->position_0 = output_0.position_1;

#line 56
    (&_S2)->uv_0 = output_0.uv_1;

#line 56
    return _S2;
}

)cy_msl";

/// cyPresentFragment.metal, 1923 bytes.
inline constexpr char kPresentFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 61 "samples/play/shaders/present.slang"
float3 decodeSrgb_0(float3 encoded_0)
{


    return select(pow((encoded_0 + float3(0.05499999970197678) ) / float3(1.0549999475479126) , float3(2.40000009536743164, 2.40000009536743164, 2.40000009536743164)), encoded_0 / float3(12.92000007629394531) , encoded_0 <= float3(0.04044999927282333, 0.04044999927282333, 0.04044999927282333));
}


#line 90 "core"
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 90
struct pixelInput_0
{
    float2 uv_0 [[user(TEXCOORD)]];
};


#line 90
struct CyPresentSet_default_0
{
    texture2d<float, access::sample> picture_0;
    sampler linearClamp_0;
};


#line 23 "samples/play/shaders/present.slang"
struct CyPresentPush_0
{
    uint flags_0;
    uint pad0_0;
    uint pad1_0;
    uint pad2_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    CyPresentSet_default_0 constant* cyPresent_0;
    CyPresentPush_0 constant* cyPresentPush_0;
};


#line 69 "samples/play/shaders/present.slang"
[[fragment]] pixelOutput_0 cyPresentFragment(pixelInput_0 _S1 [[stage_in]], float4 position_0 [[position]], CyPresentSet_default_0 constant* cyPresent_1 [[buffer(0)]], CyPresentPush_0 constant* cyPresentPush_1 [[buffer(1)]])
{

#line 69
    thread KernelContext_0 kernelContext_0;

#line 69
    (&kernelContext_0)->cyPresent_0 = cyPresent_1;

#line 69
    (&kernelContext_0)->cyPresentPush_0 = cyPresentPush_1;

    float3 colour_0 = ((cyPresent_1->picture_0).sample((cyPresent_1->linearClamp_0), (_S1.uv_0))).xyz;

#line 71
    float3 colour_1;
    if(((cyPresentPush_1->flags_0) & 1U) != 0U)
    {

#line 72
        colour_1 = decodeSrgb_0(colour_0);

#line 72
    }
    else
    {

#line 72
        colour_1 = colour_0;

#line 72
    }

#line 72
    pixelOutput_0 _S2 = { float4(colour_1, 1.0) };



    return _S2;
}

)cy_msl";

}  // namespace cy::play

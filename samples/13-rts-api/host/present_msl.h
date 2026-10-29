#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for samples/13-rts-api's present pass. GENERATED — do not edit by hand.
//
// Written by samples/13-rts-api/shaders/regenerate.py from present.slang.

#include <cy/core/base/types.h>

namespace sample::rts::present {

/// cyRtsPresentVertex.metal, 720 bytes.
inline constexpr char kPresentVertexMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 42 "samples/13-rts-api/shaders/present.slang"
struct cyRtsPresentVertex_Result_0
{
    float4 position_0 [[position]];
};


#line 42
struct CyRtsPresentVertex_0
{
    float4 position_1;
};


#line 42
[[vertex]] cyRtsPresentVertex_Result_0 cyRtsPresentVertex(uint vertex_0 [[vertex_id]])
{

#line 50
    thread CyRtsPresentVertex_0 output_0;

    (&output_0)->position_1 = float4(float2(float((vertex_0 << 1U) & 2U), float(vertex_0 & 2U)) * float2(2.0)  - float2(1.0) , 0.0, 1.0);

#line 52
    thread cyRtsPresentVertex_Result_0 _S1;

#line 52
    (&_S1)->position_0 = output_0.position_1;

#line 52
    return _S1;
}

)cy_msl";

/// cyRtsPresentFragment.metal, 3230 bytes.
inline constexpr char kPresentFragmentMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 19 "samples/13-rts-api/shaders/present.slang"
struct CyRtsPresentConstants_0
{
    uint2 source_0;
    float2 origin_0;
    float2 size_0;
    uint flags_0;
    uint pad_0;
};


#line 5319 "core.meta.slang"
struct CyRtsPresentSet_default_0
{
    texture2d<float, access::sample> frame_0;
};


#line 5319
struct KernelContext_0
{
    CyRtsPresentConstants_0 constant* cyRtsPresentConstants_0;
    CyRtsPresentSet_default_0 constant* cyRtsPresent_0;
};


#line 56 "samples/13-rts-api/shaders/present.slang"
float4 load_0(int2 texel_0, KernelContext_0 thread* kernelContext_0)
{

    int3 _S1 = int3(clamp(texel_0, int2(int(0), int(0)), int2(kernelContext_0->cyRtsPresentConstants_0->source_0) - int2(int(1)) ), int(0));

#line 59
    return ((kernelContext_0->cyRtsPresent_0->frame_0).read(vec<uint,2>(((_S1)).xy), uint(((_S1)).z)));
}

float3 decode_0(float3 encoded_0)
{


    return select(pow((encoded_0 + float3(0.05499999970197678) ) / float3(1.0549999475479126) , float3(2.40000009536743164, 2.40000009536743164, 2.40000009536743164)), encoded_0 / float3(12.92000007629394531) , encoded_0 <= (float3(0.04044999927282333) ));
}


#line 66
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 70
[[fragment]] pixelOutput_0 cyRtsPresentFragment(float4 position_0 [[position]], CyRtsPresentConstants_0 constant* cyRtsPresentConstants_1 [[buffer(1)]], CyRtsPresentSet_default_0 constant* cyRtsPresent_1 [[buffer(0)]])
{

#line 70
    thread KernelContext_0 kernelContext_1;

#line 70
    (&kernelContext_1)->cyRtsPresentConstants_0 = cyRtsPresentConstants_1;

#line 70
    (&kernelContext_1)->cyRtsPresent_0 = cyRtsPresent_1;

    float2 local_0 = position_0.xy - cyRtsPresentConstants_1->origin_0;

#line 72
    bool _S2;
    if(any(local_0 < (float2(0.0) )))
    {

#line 73
        _S2 = true;

#line 73
    }
    else
    {

#line 73
        _S2 = any(local_0 >= ((&kernelContext_1)->cyRtsPresentConstants_0->size_0));

#line 73
    }

#line 73
    if(_S2)
    {

#line 73
        pixelOutput_0 _S3 = { float4(0.0, 0.0, 0.0, 1.0) };

        return _S3;
    }


    float2 at_0 = local_0 / (&kernelContext_1)->cyRtsPresentConstants_0->size_0 * float2((&kernelContext_1)->cyRtsPresentConstants_0->source_0) - float2(0.5) ;
    float2 _S4 = floor(at_0);

#line 80
    int2 _S5 = int2(_S4);
    float2 t_0 = at_0 - _S4;

#line 81
    float4 _S6 = load_0(_S5, &kernelContext_1);

#line 81
    float4 _S7 = load_0(_S5 + int2(int(1), int(0)), &kernelContext_1);
    float4 _S8 = float4(t_0.x) ;

#line 82
    float4 top_0 = mix(_S6, _S7, _S8);

#line 82
    float4 _S9 = load_0(_S5 + int2(int(0), int(1)), &kernelContext_1);

#line 82
    float4 _S10 = load_0(_S5 + int2(int(1), int(1)), &kernelContext_1);

    float3 colour_0 = mix(top_0, mix(_S9, _S10, _S8), float4(t_0.y) ).xyz;

#line 84
    float3 colour_1;
    if((((&kernelContext_1)->cyRtsPresentConstants_0->flags_0) & 1U) != 0U)
    {

#line 85
        colour_1 = decode_0(colour_0);

#line 85
    }
    else
    {

#line 85
        colour_1 = colour_0;

#line 85
    }

#line 85
    pixelOutput_0 _S11 = { float4(colour_1, 1.0) };



    return _S11;
}

)cy_msl";

}  // namespace sample::rts::present

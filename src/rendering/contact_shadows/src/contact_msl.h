#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the contact shadow dispatch. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::contact_shadows {

/// contact_shadows.metal, 6965 bytes.
inline constexpr char kContactShadowsMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 45 "src/rendering/contact_shadows/shaders/contact_shadows.slang"
struct CyContactConstants_0
{
    float4 viewRow0_0;
    float4 viewRow1_0;
    float4 viewRow2_0;
    float4 projection_0;
    float4 extent_0;
    float4 light_0;
    float4 control_0;
    float4 jitter_0;
};


#line 5163 "hlsl.meta.slang"
struct CyContactSet_default_0
{
    depth2d<float, access::sample> depth_0;
    texture2d<float, access::sample> normals_0;
    texture2d<float, access::read_write> output_0;
};


#line 5163
struct KernelContext_0
{
    CyContactConstants_0 constant* cyContactConstants_0;
    CyContactSet_default_0 constant* cyContactSet_0;
};


#line 77 "src/rendering/contact_shadows/shaders/contact_shadows.slang"
float viewDepthOf_0(float depth_1, KernelContext_0 thread* kernelContext_0)
{
    return kernelContext_0->cyContactConstants_0->projection_0.w / (depth_1 + kernelContext_0->cyContactConstants_0->projection_0.z);
}


#line 23 "src/rendering/shaders/cy/packing.slang"
float3 decodeOctahedral_0(float2 encoded_0)
{
    float2 _S1 = encoded_0 * float2(2.0)  - float2(1.0) ;
    float _S2 = _S1.x;

#line 26
    float _S3 = _S1.y;

#line 26
    float _S4 = 1.0 - abs(_S2) - abs(_S3);

#line 26
    thread float3 normal_0 = float3(_S2, _S3, _S4);
    float _S5 = saturate(- _S4);
    float2 _S6 = float2(_S2, _S3);

#line 28
    normal_0.xy = _S6 + select(float2(_S5) , float2(- _S5) , _S6 >= (float2(0.0) ));
    return normalize(normal_0);
}


#line 82 "src/rendering/contact_shadows/shaders/contact_shadows.slang"
float3 toView_0(float3 relative_0, KernelContext_0 thread* kernelContext_1)
{
    return float3(dot(kernelContext_1->cyContactConstants_0->viewRow0_0.xyz, relative_0), dot(kernelContext_1->cyContactConstants_0->viewRow1_0.xyz, relative_0), dot(kernelContext_1->cyContactConstants_0->viewRow2_0.xyz, relative_0));
}




float cyContactNoise_0(uint2 pixel_0)
{

    return fract(52.98291778564453125 * fract(dot(float2(pixel_0) + float2(0.5) , float2(0.06711056083440781, 0.00583714991807938))));
}

float traceVisibility_0(uint2 pixel_1, KernelContext_0 thread* kernelContext_2)
{
    int3 _S7 = int3(int2(pixel_1), int(0));

#line 98
    float _S8 = ((kernelContext_2->cyContactSet_0->depth_0).read(vec<uint,2>(((_S7)).xy), uint(((_S7)).z)));
    if(_S8 <= 0.0)
    {
        return 1.0;
    }
    float4 _S9 = kernelContext_2->cyContactConstants_0->projection_0;

#line 103
    float _S10 = viewDepthOf_0(_S8, kernelContext_2);

    if(_S10 > (kernelContext_2->cyContactConstants_0->control_0.z))
    {
        return 1.0;
    }
    float3 _S11 = kernelContext_2->cyContactConstants_0->light_0.xyz;

#line 109
    float3 _S12 = toView_0(decodeOctahedral_0(((kernelContext_2->cyContactSet_0->normals_0).read(vec<uint,2>(((_S7)).xy), uint(((_S7)).z))).xy), kernelContext_2);
    float3 _S13 = normalize(_S12);
    if((dot(_S13, _S11)) <= 0.0)
    {
        return 1.0;
    }
    float2 _S14 = (float2(pixel_1) + float2(0.5)  + kernelContext_2->cyContactConstants_0->jitter_0.xy) * kernelContext_2->cyContactConstants_0->extent_0.zw;
    float _S15 = _S9.x;
    float _S16 = _S9.y;

    float3 _S17 = float3((_S14.x * 2.0 - 1.0) * _S10 / _S15, (1.0 - _S14.y * 2.0) * _S10 / _S16, - _S10) + _S13 * float3((kernelContext_2->cyContactConstants_0->control_0.w * (2.0 * _S10 / (_S16 * kernelContext_2->cyContactConstants_0->extent_0.y)))) ;

    uint _S18 = uint(kernelContext_2->cyContactConstants_0->control_0.x);
    float _S19 = cyContactNoise_0(pixel_1);

#line 122
    uint index_0 = 0U;
    for(;;)
    {

#line 123
        if(index_0 < _S18)
        {
        }
        else
        {

#line 123
            break;
        }
        float _S20 = (float(index_0) + _S19) / float(_S18);
        float3 _S21 = _S17 + _S11 * float3((_S20 * kernelContext_2->cyContactConstants_0->light_0.w)) ;
        float _S22 = - _S21.z;
        if(_S22 <= 0.0)
        {
            break;
        }


        float _S23 = (_S15 * _S21.x / _S22 * 0.5 + 0.5) * kernelContext_2->cyContactConstants_0->extent_0.x - kernelContext_2->cyContactConstants_0->jitter_0.x;
        float _S24 = (0.5 - _S16 * _S21.y / _S22 * 0.5) * kernelContext_2->cyContactConstants_0->extent_0.y - kernelContext_2->cyContactConstants_0->jitter_0.y;

#line 135
        bool _S25;
        if(_S23 < 0.0)
        {

#line 136
            _S25 = true;

#line 136
        }
        else
        {

#line 136
            _S25 = _S24 < 0.0;

#line 136
        }

#line 136
        bool _S26;

#line 136
        if(_S25)
        {

#line 136
            _S26 = true;

#line 136
        }
        else
        {

#line 136
            _S26 = _S23 >= (kernelContext_2->cyContactConstants_0->extent_0.x);

#line 136
        }

#line 136
        bool _S27;

#line 136
        if(_S26)
        {

#line 136
            _S27 = true;

#line 136
        }
        else
        {

#line 136
            _S27 = _S24 >= (kernelContext_2->cyContactConstants_0->extent_0.y);

#line 136
        }

#line 136
        if(_S27)
        {

            break;
        }
        int3 _S28 = int3(int(_S23), int(_S24), int(0));

#line 141
        float _S29 = ((kernelContext_2->cyContactSet_0->depth_0).read(vec<uint,2>(((_S28)).xy), uint(((_S28)).z)));
        if(_S29 <= 0.0)
        {
            index_0 = index_0 + 1U;

#line 123
            continue;
        }

#line 123
        float _S30 = viewDepthOf_0(_S29, kernelContext_2);

#line 146
        float _S31 = _S22 - _S30;

#line 146
        bool _S32;
        if(_S31 > 0.0)
        {

#line 147
            _S32 = _S31 < (kernelContext_2->cyContactConstants_0->control_0.y);

#line 147
        }
        else
        {

#line 147
            _S32 = false;

#line 147
        }

#line 147
        if(_S32)
        {
            return saturate((_S20 - 0.5) * 2.0);
        }

#line 123
        index_0 = index_0 + 1U;

#line 123
    }

#line 152
    return 1.0;
}



[[kernel]] void cyContactShadows(uint3 thread_0 [[thread_position_in_grid]], CyContactConstants_0 constant* cyContactConstants_1 [[buffer(1)]], CyContactSet_default_0 constant* cyContactSet_1 [[buffer(0)]])
{

#line 157
    thread KernelContext_0 kernelContext_3;

#line 157
    (&kernelContext_3)->cyContactConstants_0 = cyContactConstants_1;

#line 157
    (&kernelContext_3)->cyContactSet_0 = cyContactSet_1;

    uint2 _S33 = uint2(cyContactConstants_1->extent_0.xy);

#line 159
    bool _S34;
    if((thread_0.x) >= (_S33.x))
    {

#line 160
        _S34 = true;

#line 160
    }
    else
    {

#line 160
        _S34 = (thread_0.y) >= (_S33.y);

#line 160
    }

#line 160
    if(_S34)
    {
        return;
    }
    uint2 _S35 = thread_0.xy;

)cy_msl"
    R"cy_msl(#line 164
    uint2 _S36 = uint2(int2(_S35));

#line 164
    float _S37 = traceVisibility_0(_S35, &kernelContext_3);

#line 164
    (&kernelContext_3)->cyContactSet_0->output_0.write(float4(_S37, 1.0, 1.0, 1.0),_S36);
    return;
}

)cy_msl";

}  // namespace cy::rendering::contact_shadows

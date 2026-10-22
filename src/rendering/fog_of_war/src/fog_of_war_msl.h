#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the fog of war dispatch. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::fog_of_war {

/// cyFogOfWar.metal, 6874 bytes.
inline constexpr char kFogOfWarMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 5163 "hlsl.meta.slang"
struct CyFogOfWarSet_default_0
{
    texture2d<float, access::sample> source_0;
    depth2d<float, access::sample> depth_0;
    texture2d<float, access::sample> visibility_0;
    float4 device* words_0;
    texture2d<float, access::read_write> target_0;
};


#line 5163
struct KernelContext_0
{
    CyFogOfWarSet_default_0 constant* cyFogOfWarSet_0;
};


#line 39 "src/rendering/fog_of_war/shaders/fog_of_war.slang"
float4 fogOfWarWord_0(uint index_0, KernelContext_0 thread* kernelContext_0)
{
    return kernelContext_0->cyFogOfWarSet_0->words_0[index_0];
}


#line 39
float4 fogOfWarWord_1(uint index_1, KernelContext_0 thread* kernelContext_1)
{
    return kernelContext_1->cyFogOfWarSet_0->words_0[index_1];
}


#line 51
float2 fogOfWarBilinear_0(float2 position_0, KernelContext_0 thread* kernelContext_2)
{

#line 51
    float4 _S1 = fogOfWarWord_0(8U, kernelContext_2);


    float2 _S2 = floor(position_0);
    float2 _S3 = position_0 - _S2;
    float2 _S4 = _S1.zw - float2(1.0) ;
    float _S5 = _S2.x;

#line 57
    float _S6 = _S4.x;

#line 57
    int _S7 = int(clamp(_S5, 0.0, _S6));
    int _S8 = int(clamp(_S5 + 1.0, 0.0, _S6));
    float _S9 = _S2.y;

#line 59
    float _S10 = _S4.y;

#line 59
    int _S11 = int(clamp(_S9, 0.0, _S10));
    int _S12 = int(clamp(_S9 + 1.0, 0.0, _S10));
    int3 _S13 = int3(_S7, _S11, int(0));
    int3 _S14 = int3(_S8, _S11, int(0));
    int3 _S15 = int3(_S7, _S12, int(0));
    int3 _S16 = int3(_S8, _S12, int(0));
    float2 _S17 = float2(_S3.x) ;

    return mix(mix(((kernelContext_2->cyFogOfWarSet_0->visibility_0).read(vec<uint,2>(((_S13)).xy), uint(((_S13)).z))).xy, ((kernelContext_2->cyFogOfWarSet_0->visibility_0).read(vec<uint,2>(((_S14)).xy), uint(((_S14)).z))).xy, _S17), mix(((kernelContext_2->cyFogOfWarSet_0->visibility_0).read(vec<uint,2>(((_S15)).xy), uint(((_S15)).z))).xy, ((kernelContext_2->cyFogOfWarSet_0->visibility_0).read(vec<uint,2>(((_S16)).xy), uint(((_S16)).z))).xy, _S17), float2(_S3.y) );
}


#line 44
float fogOfWarSmooth_0(float value_0)
{
    float _S18 = clamp(value_0, 0.0, 1.0);
    return _S18 * _S18 * (3.0 - 2.0 * _S18);
}


#line 73
float2 fogOfWarSample_0(float2 world_0, KernelContext_0 thread* kernelContext_3)
{

#line 73
    float4 _S19 = fogOfWarWord_1(5U, kernelContext_3);

#line 73
    float4 _S20 = fogOfWarWord_1(8U, kernelContext_3);



    float2 _S21 = (world_0 - _S19.xy) * float2(_S19.z) ;


    float _S22 = _S21.x;

#line 80
    bool _S23;

#line 80
    if(_S22 < -0.0625)
    {

#line 80
        _S23 = true;

#line 80
    }
    else
    {

#line 80
        _S23 = (_S21.y) < -0.0625;

#line 80
    }

#line 80
    if(_S23)
    {

#line 80
        _S23 = true;

#line 80
    }
    else
    {

#line 80
        _S23 = _S22 > (_S20.z + 0.0625);

#line 80
    }

#line 80
    if(_S23)
    {

#line 80
        _S23 = true;

#line 80
    }
    else
    {

#line 80
        _S23 = (_S21.y) > (_S20.w + 0.0625);

#line 80
    }

#line 80
    if(_S23)
    {
        return float2(0.0) ;
    }
    float2 _S24 = _S21 - float2(0.5) ;
    float2 _S25 = float2(0.0) ;

#line 85
    uint tap_0 = 0U;

#line 85
    float2 level_0 = _S25;
    for(;;)
    {

#line 86
        if(tap_0 < 4U)
        {
        }
        else
        {

#line 86
            break;
        }

#line 86
        float _S26;

        if((tap_0 & 1U) != 0U)
        {

#line 88
            _S26 = 0.5;

#line 88
        }
        else
        {

#line 88
            _S26 = -0.5;

#line 88
        }

#line 88
        float _S27;

#line 88
        if((tap_0 & 2U) != 0U)
        {

#line 88
            _S27 = 0.5;

#line 88
        }
        else
        {

#line 88
            _S27 = -0.5;

#line 88
        }

#line 88
        float2 _S28 = fogOfWarBilinear_0(_S24 + float2(_S26, _S27), kernelContext_3);
        float2 level_1 = level_0 + _S28;

#line 86
        tap_0 = tap_0 + 1U;

#line 86
        level_0 = level_1;

#line 86
    }

#line 91
    float2 level_2 = level_0 * float2(0.25) ;
    return float2(fogOfWarSmooth_0(level_2.x), fogOfWarSmooth_0(level_2.y));
}


float3 fogOfWarShade_0(float3 colour_0, float2 levels_0, KernelContext_0 thread* kernelContext_4)
{

#line 96
    float4 _S29 = fogOfWarWord_1(6U, kernelContext_4);

#line 101
    float3 _S30 = mix(float3((0.2125999927520752 * colour_0.x + 0.71520000696182251 * colour_0.y + 0.07220000028610229 * colour_0.z)) , colour_0, float3(_S29.y) ) * float3(_S29.x) ;

#line 101
    float4 _S31 = fogOfWarWord_1(7U, kernelContext_4);

    return mix(mix(_S31.xyz, _S30, float3(levels_0.y) ), colour_0, float3(levels_0.x) );
}



[[kernel]] void cyFogOfWar(uint3 thread_0 [[thread_position_in_grid]], CyFogOfWarSet_default_0 constant* cyFogOfWarSet_1 [[buffer(0)]])
{

#line 108
    thread KernelContext_0 kernelContext_5;

#line 108
    (&kernelContext_5)->cyFogOfWarSet_0 = cyFogOfWarSet_1;

#line 108
    float4 _S32 = fogOfWarWord_0(8U, &kernelContext_5);

#line 108
    bool _S33;


    if(float(thread_0.x) >= (_S32.x))
    {

#line 111
        _S33 = true;

#line 111
    }
    else
    {

#line 111
        _S33 = float(thread_0.y) >= (_S32.y);

#line 111
    }

#line 111
    if(_S33)
    {
        return;
    }
    uint2 _S34 = thread_0.xy;

#line 115
    int2 _S35 = int2(_S34);
    int3 _S36 = int3(_S35, int(0));

)cy_msl"
    R"cy_msl(#line 116
    float4 _S37 = (((&kernelContext_5)->cyFogOfWarSet_0->source_0).read(vec<uint,2>(((_S36)).xy), uint(((_S36)).z)));
    float _S38 = (((&kernelContext_5)->cyFogOfWarSet_0->depth_0).read(vec<uint,2>(((_S36)).xy), uint(((_S36)).z)));
    if(_S38 <= 0.0)
    {
        (&kernelContext_5)->cyFogOfWarSet_0->target_0.write(_S37,_S34);
        return;
    }

    float2 _S39 = (float2(_S35) + float2(0.5) ) / _S32.xy;
    float _S40 = _S39.x * 2.0 - 1.0;

#line 125
    float _S41 = 1.0 - _S39.y * 2.0;

#line 125
    float4 _S42 = fogOfWarWord_0(0U, &kernelContext_5);
    float4 _S43 = _S42 * float4(_S40) ;

#line 126
    float4 _S44 = fogOfWarWord_0(1U, &kernelContext_5);

#line 126
    float4 _S45 = _S43 + _S44 * float4(_S41) ;

#line 126
    float4 _S46 = fogOfWarWord_0(2U, &kernelContext_5);

#line 126
    float4 _S47 = _S45 + _S46 * float4(_S38) ;

#line 126
    float4 _S48 = fogOfWarWord_0(3U, &kernelContext_5);

#line 126
    float4 _S49 = _S47 + _S48;

    float3 _S50 = _S49.xyz / float3(_S49.w) ;

#line 128
    float4 _S51 = fogOfWarWord_0(4U, &kernelContext_5);

#line 128
    float2 _S52 = fogOfWarSample_0((_S50 + _S51.xyz).xz, &kernelContext_5);

    if((_S52.x) >= 1.0)
    {
        (&kernelContext_5)->cyFogOfWarSet_0->target_0.write(_S37,_S34);
        return;
    }

#line 133
    float3 _S53 = fogOfWarShade_0(_S37.xyz, _S52, &kernelContext_5);

    (&kernelContext_5)->cyFogOfWarSet_0->target_0.write(float4(_S53, _S37.w),_S34);
    return;
}

)cy_msl";

}  // namespace cy::rendering::fog_of_war

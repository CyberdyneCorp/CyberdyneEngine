#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the fog of war dispatch. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::fog_of_war {

/// cyFogOfWar.metal, 6180 bytes.
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


#line 38 "src/rendering/fog_of_war/shaders/fog_of_war.slang"
float4 fogOfWarWord_0(uint index_0, KernelContext_0 thread* kernelContext_0)
{
    return kernelContext_0->cyFogOfWarSet_0->words_0[index_0];
}


#line 38
float4 fogOfWarWord_1(uint index_1, KernelContext_0 thread* kernelContext_1)
{
    return kernelContext_1->cyFogOfWarSet_0->words_0[index_1];
}


#line 50
float2 fogOfWarBilinear_0(float2 position_0, KernelContext_0 thread* kernelContext_2)
{

#line 50
    float4 _S1 = fogOfWarWord_0(8U, kernelContext_2);


    float2 _S2 = floor(position_0);
    float2 _S3 = position_0 - _S2;
    float2 _S4 = _S1.zw - float2(1.0) ;
    float _S5 = _S2.x;

#line 56
    float _S6 = _S4.x;

#line 56
    int _S7 = int(clamp(_S5, 0.0, _S6));
    int _S8 = int(clamp(_S5 + 1.0, 0.0, _S6));
    float _S9 = _S2.y;

#line 58
    float _S10 = _S4.y;

#line 58
    int _S11 = int(clamp(_S9, 0.0, _S10));
    int _S12 = int(clamp(_S9 + 1.0, 0.0, _S10));
    int3 _S13 = int3(_S7, _S11, int(0));
    int3 _S14 = int3(_S8, _S11, int(0));
    int3 _S15 = int3(_S7, _S12, int(0));
    int3 _S16 = int3(_S8, _S12, int(0));
    float2 _S17 = float2(_S3.x) ;

    return mix(mix(((kernelContext_2->cyFogOfWarSet_0->visibility_0).read(vec<uint,2>(((_S13)).xy), uint(((_S13)).z))).xy, ((kernelContext_2->cyFogOfWarSet_0->visibility_0).read(vec<uint,2>(((_S14)).xy), uint(((_S14)).z))).xy, _S17), mix(((kernelContext_2->cyFogOfWarSet_0->visibility_0).read(vec<uint,2>(((_S15)).xy), uint(((_S15)).z))).xy, ((kernelContext_2->cyFogOfWarSet_0->visibility_0).read(vec<uint,2>(((_S16)).xy), uint(((_S16)).z))).xy, _S17), float2(_S3.y) );
}


#line 43
float fogOfWarSmooth_0(float value_0)
{
    float _S18 = clamp(value_0, 0.0, 1.0);
    return _S18 * _S18 * (3.0 - 2.0 * _S18);
}


#line 71
float2 fogOfWarSample_0(float2 world_0, KernelContext_0 thread* kernelContext_3)
{

#line 71
    float4 _S19 = fogOfWarWord_1(5U, kernelContext_3);


    float2 _S20 = (world_0 - _S19.xy) * float2(_S19.z)  - float2(0.5) ;
    float2 _S21 = float2(0.0) ;

#line 75
    uint tap_0 = 0U;

#line 75
    float2 level_0 = _S21;
    for(;;)
    {

#line 76
        if(tap_0 < 4U)
        {
        }
        else
        {

#line 76
            break;
        }

#line 76
        float _S22;

        if((tap_0 & 1U) != 0U)
        {

#line 78
            _S22 = 0.5;

#line 78
        }
        else
        {

#line 78
            _S22 = -0.5;

#line 78
        }

#line 78
        float _S23;

#line 78
        if((tap_0 & 2U) != 0U)
        {

#line 78
            _S23 = 0.5;

#line 78
        }
        else
        {

#line 78
            _S23 = -0.5;

#line 78
        }

#line 78
        float2 _S24 = fogOfWarBilinear_0(_S20 + float2(_S22, _S23), kernelContext_3);
        float2 level_1 = level_0 + _S24;

#line 76
        tap_0 = tap_0 + 1U;

#line 76
        level_0 = level_1;

#line 76
    }

#line 81
    float2 level_2 = level_0 * float2(0.25) ;
    return float2(fogOfWarSmooth_0(level_2.x), fogOfWarSmooth_0(level_2.y));
}


float3 fogOfWarShade_0(float3 colour_0, float2 levels_0, KernelContext_0 thread* kernelContext_4)
{

#line 86
    float4 _S25 = fogOfWarWord_1(6U, kernelContext_4);

#line 91
    float3 _S26 = mix(float3((0.2125999927520752 * colour_0.x + 0.71520000696182251 * colour_0.y + 0.07220000028610229 * colour_0.z)) , colour_0, float3(_S25.y) ) * float3(_S25.x) ;

#line 91
    float4 _S27 = fogOfWarWord_1(7U, kernelContext_4);

    return mix(mix(_S27.xyz, _S26, float3(levels_0.y) ), colour_0, float3(levels_0.x) );
}



[[kernel]] void cyFogOfWar(uint3 thread_0 [[thread_position_in_grid]], CyFogOfWarSet_default_0 constant* cyFogOfWarSet_1 [[buffer(0)]])
{

#line 98
    thread KernelContext_0 kernelContext_5;

#line 98
    (&kernelContext_5)->cyFogOfWarSet_0 = cyFogOfWarSet_1;

#line 98
    float4 _S28 = fogOfWarWord_0(8U, &kernelContext_5);

#line 98
    bool _S29;


    if(float(thread_0.x) >= (_S28.x))
    {

#line 101
        _S29 = true;

#line 101
    }
    else
    {

#line 101
        _S29 = float(thread_0.y) >= (_S28.y);

#line 101
    }

#line 101
    if(_S29)
    {
        return;
    }
    uint2 _S30 = thread_0.xy;

#line 105
    int2 _S31 = int2(_S30);
    int3 _S32 = int3(_S31, int(0));

#line 106
    float4 _S33 = (((&kernelContext_5)->cyFogOfWarSet_0->source_0).read(vec<uint,2>(((_S32)).xy), uint(((_S32)).z)));
    float _S34 = (((&kernelContext_5)->cyFogOfWarSet_0->depth_0).read(vec<uint,2>(((_S32)).xy), uint(((_S32)).z)));
    if(_S34 <= 0.0)
    {
        (&kernelContext_5)->cyFogOfWarSet_0->target_0.write(_S33,_S30);
        return;
    }

    float2 _S35 = (float2(_S31) + float2(0.5) ) / _S28.xy;
    float _S36 = _S35.x * 2.0 - 1.0;

#line 115
    float _S37 = 1.0 - _S35.y * 2.0;

#line 115
    float4 _S38 = fogOfWarWord_0(0U, &kernelContext_5);
    float4 _S39 = _S38 * float4(_S36) ;

#line 116
    float4 _S40 = fogOfWarWord_0(1U, &kernelContext_5);

#line 116
    float4 _S41 = _S39 + _S40 * float4(_S37) ;

#line 116
    float4 _S42 = fogOfWarWord_0(2U, &kernelContext_5);

#line 116
    float4 _S43 = _S41 + _S42 * float4(_S34) ;

#line 116
    float4 _S44 = fogOfWarWord_0(3U, &kernelContext_5);

#line 116
    float4 _S45 = _S43 + _S44;

    float3 _S46 = _S45.xyz / float3(_S45.w) ;

#line 118
    float4 _S47 = fogOfWarWord_0(4U, &kernelContext_5);

#line 118
    float2 _S48 = fogOfWarSample_0((_S46 + _S47.xyz).xz, &kernelContext_5);

    if((_S48.x) >= 1.0)
    {
        (&kernelContext_5)->cyFogOfWarSet_0->target_0.write(_S33,_S30);
        return;
    }

#line 123
    float3 _S49 = fogOfWarShade_0(_S33.xyz, _S48, &kernelContext_5);

    (&kernelContext_5)->cyFogOfWarSet_0->target_0.write(float4(_S49, _S33.w),_S30);
    return;
}

)cy_msl";

}  // namespace cy::rendering::fog_of_war

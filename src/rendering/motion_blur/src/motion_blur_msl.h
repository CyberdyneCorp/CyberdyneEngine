#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the motion blur dispatches. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::motion_blur {

/// motion_blur_tile_max.metal, 5545 bytes.
inline constexpr char kMotionBlurTileMaxMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 11 "src/rendering/motion_blur/shaders/motion_blur_common.slang"
struct CyMotionBlurConstants_0
{
    float4 currentToPrevious0_0;
    float4 currentToPrevious1_0;
    float4 currentToPrevious2_0;
    float4 currentToPrevious3_0;
    float4 extent_0;
    float4 scales_0;
    float4 depth_0;
    uint4 control_0;
};


#line 5163 "hlsl.meta.slang"
struct CyMotionBlurTileSet_default_0
{
    texture2d<float, access::sample> velocity_0;
    depth2d<float, access::sample> depth_1;
    texture2d<float, access::read_write> tiles_0;
};


#line 5163
struct KernelContext_0
{
    CyMotionBlurConstants_0 constant* cyMotionBlur_0;
    CyMotionBlurTileSet_default_0 constant* cyMotionBlurTileSet_0;
};


#line 45 "src/rendering/motion_blur/shaders/motion_blur_common.slang"
float2 cyMotionBlurCameraMotion_0(uint2 pixel_0, float depth_2, KernelContext_0 thread* kernelContext_0)
{
    float2 _S1 = (float2(pixel_0) + float2(0.5) ) * kernelContext_0->cyMotionBlur_0->extent_0.zw;
    float _S2 = _S1.x * 2.0 - 1.0;

#line 48
    float _S3 = 1.0 - _S1.y * 2.0;

#line 48
    float4 _S4 = float4(_S2, _S3, depth_2, 1.0);

#line 53
    float2 _S5 = float2(dot(kernelContext_0->cyMotionBlur_0->currentToPrevious0_0, _S4), dot(kernelContext_0->cyMotionBlur_0->currentToPrevious1_0, _S4)) / float2(dot(kernelContext_0->cyMotionBlur_0->currentToPrevious3_0, _S4)) ;
    return float2((_S5.x - _S2) * 0.5 * kernelContext_0->cyMotionBlur_0->extent_0.x, (_S3 - _S5.y) * 0.5 * kernelContext_0->cyMotionBlur_0->extent_0.y);
}


#line 61
float2 cyMotionBlurVector_0(uint2 pixel_1, float2 velocity_1, float depth_3, KernelContext_0 thread* kernelContext_1)
{
    bool _S6 = depth_3 > 0.0;

#line 63
    float2 blur_0;

#line 63
    if(_S6)
    {

#line 63
        blur_0 = velocity_1 * kernelContext_1->cyMotionBlur_0->extent_0.xy;

#line 63
    }
    else
    {

#line 63
        float2 _S7 = cyMotionBlurCameraMotion_0(pixel_1, 0.0, kernelContext_1);

#line 63
        blur_0 = _S7;

#line 63
    }

    float2 _S8 = blur_0 * float2(kernelContext_1->cyMotionBlur_0->scales_0.x) ;

#line 65
    bool _S9;
    if((kernelContext_1->cyMotionBlur_0->depth_0.z) > 0.5)
    {

#line 66
        _S9 = _S6;

#line 66
    }
    else
    {

#line 66
        _S9 = false;

#line 66
    }

#line 66
    if(_S9)
    {

#line 66
        float2 _S10 = cyMotionBlurCameraMotion_0(pixel_1, depth_3, kernelContext_1);

#line 66
        blur_0 = _S10 * float2(kernelContext_1->cyMotionBlur_0->scales_0.x)  + (blur_0 - _S10) * float2(kernelContext_1->cyMotionBlur_0->scales_0.y) ;

#line 66
    }
    else
    {

#line 66
        blur_0 = _S8;

#line 66
    }

#line 71
    float _S11 = length(blur_0);
    if(_S11 > (kernelContext_1->cyMotionBlur_0->scales_0.z))
    {

#line 72
        blur_0 = blur_0 * float2((kernelContext_1->cyMotionBlur_0->scales_0.z / _S11)) ;

#line 72
    }



    return blur_0;
}


float2 cyMotionBlurLonger_0(float2 longest_0, float2 candidate_0)
{

#line 80
    float2 _S12;

    if((dot(candidate_0, candidate_0)) > (dot(longest_0, longest_0)))
    {

#line 82
        _S12 = candidate_0;

#line 82
    }
    else
    {

#line 82
        _S12 = longest_0;

#line 82
    }

#line 82
    return _S12;
}


#line 29 "src/rendering/motion_blur/shaders/motion_blur_tile_max.slang"
[[kernel]] void cyMotionBlurTileMax(uint3 thread_0 [[thread_position_in_grid]], CyMotionBlurConstants_0 constant* cyMotionBlur_1 [[buffer(0)]], CyMotionBlurTileSet_default_0 constant* cyMotionBlurTileSet_1 [[buffer(1)]])
{

#line 29
    thread KernelContext_0 kernelContext_2;

#line 29
    (&kernelContext_2)->cyMotionBlur_0 = cyMotionBlur_1;

#line 29
    (&kernelContext_2)->cyMotionBlurTileSet_0 = cyMotionBlurTileSet_1;

#line 29
    bool _S13;

    if((thread_0.x) >= (cyMotionBlur_1->control_0.z))
    {

#line 31
        _S13 = true;

#line 31
    }
    else
    {

#line 31
        _S13 = (thread_0.y) >= (cyMotionBlur_1->control_0.w);

#line 31
    }

#line 31
    if(_S13)
    {
        return;
    }


    uint2 _S14 = thread_0.xy;

#line 37
    uint2 _S15 = uint2(cyMotionBlur_1->control_0.y) ;

#line 37
    uint2 _S16 = _S14 * _S15;
    uint2 _S17 = min(_S16 + _S15, uint2((&kernelContext_2)->cyMotionBlur_0->extent_0.xy));

    uint _S18 = _S16.y;

#line 40
    float2 longest_1 = float2(0.0, 0.0);

#line 40
    uint y_0 = _S18;

#line 40
    for(;;)
    {

#line 40
        if(y_0 < (_S17.y))
        {
        }
        else
        {

#line 40
            break;
        }

#line 40
        uint x_0 = _S16.x;

        for(;;)
        {

#line 42
            if(x_0 < (_S17.x))
            {
            }
            else
            {

#line 42
                break;
            }
            uint2 _S19 = uint2(x_0, y_0);
            int3 _S20 = int3(int2(_S19), int(0));

#line 45
            float2 _S21 = cyMotionBlurVector_0(_S19, (((&kernelContext_2)->cyMotionBlurTileSet_0->velocity_0).read(vec<uint,2>(((_S20)).xy), uint(((_S20)).z)).xy), (((&kernelContext_2)->cyMotionBlurTileSet_0->depth_1).read(vec<uint,2>(((_S20)).xy), uint(((_S20)).z))), &kernelContext_2);

            float2 _S22 = cyMotionBlurLonger_0(longest_1, _S21);

#line 42
            uint x_1 = x_0 + 1U;

#line 42
            longest_1 = _S22;

#line 42
            x_0 = x_1;

#line 42
        }

#line 40
        y_0 = y_0 + 1U;

#line 40
    }

#line 50
    (&kernelContext_2)->cyMotionBlurTileSet_0->tiles_0.write(float4(longest_1, 0.0, 0.0),uint2(int2(_S14)));
    return;
}

)cy_msl";

/// motion_blur_neighbour_max.metal, 3932 bytes.
inline constexpr char kMotionBlurNeighbourMaxMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 80 "src/rendering/motion_blur/shaders/motion_blur_common.slang"
float2 cyMotionBlurLonger_0(float2 longest_0, float2 candidate_0)
{

#line 80
    float2 _S1;

    if((dot(candidate_0, candidate_0)) > (dot(longest_0, longest_0)))
    {

#line 82
        _S1 = candidate_0;

#line 82
    }
    else
    {

#line 82
        _S1 = longest_0;

#line 82
    }

#line 82
    return _S1;
}


#line 11
struct CyMotionBlurConstants_0
{
    float4 currentToPrevious0_0;
    float4 currentToPrevious1_0;
    float4 currentToPrevious2_0;
    float4 currentToPrevious3_0;
    float4 extent_0;
    float4 scales_0;
    float4 depth_0;
    uint4 control_0;
};


#line 5163 "hlsl.meta.slang"
struct CyMotionBlurNeighbourSet_default_0
{
    texture2d<float, access::sample> tiles_0;
    texture2d<float, access::read_write> neighbours_0;
};


#line 5163
struct KernelContext_0
{
    CyMotionBlurConstants_0 constant* cyMotionBlur_0;
    CyMotionBlurNeighbourSet_default_0 constant* cyMotionBlurNeighbourSet_0;
};


#line 23 "src/rendering/motion_blur/shaders/motion_blur_neighbour_max.slang"
[[kernel]] void cyMotionBlurNeighbourMax(uint3 thread_0 [[thread_position_in_grid]], CyMotionBlurConstants_0 constant* cyMotionBlur_1 [[buffer(0)]], CyMotionBlurNeighbourSet_default_0 constant* cyMotionBlurNeighbourSet_1 [[buffer(1)]])
{

#line 23
    thread KernelContext_0 kernelContext_0;

#line 23
    (&kernelContext_0)->cyMotionBlur_0 = cyMotionBlur_1;

#line 23
    (&kernelContext_0)->cyMotionBlurNeighbourSet_0 = cyMotionBlurNeighbourSet_1;

    int _S2 = int(cyMotionBlur_1->control_0.z);
    int _S3 = int(cyMotionBlur_1->control_0.w);
    int2 _S4 = int2(thread_0.xy);

#line 27
    bool _S5;
    if((_S4.x) >= _S2)
    {

#line 28
        _S5 = true;

#line 28
    }
    else
    {

#line 28
        _S5 = (_S4.y) >= _S3;

#line 28
    }

#line 28
    if(_S5)
    {
        return;
    }

#line 30
    float2 longest_1 = float2(0.0, 0.0);

#line 30
    int dy_0 = int(-1);


    for(;;)
    {

#line 33
        if(dy_0 <= int(1))
        {
        }
        else
        {

#line 33
            break;
        }

#line 33
        float2 longest_2 = longest_1;

#line 33
        int dx_0 = int(-1);

        for(;;)
        {

#line 35
            if(dx_0 <= int(1))
            {
            }
            else
            {

#line 35
                break;
            }
            int2 _S6 = _S4 + int2(dx_0, dy_0);
            int _S7 = _S6.x;

#line 38
            if(_S7 < int(0))
            {

#line 38
                _S5 = true;

#line 38
            }
            else
            {

#line 38
                _S5 = (_S6.y) < int(0);

#line 38
            }

#line 38
            bool _S8;

#line 38
            if(_S5)
            {

#line 38
                _S8 = true;

#line 38
            }
            else
            {

#line 38
                _S8 = _S7 >= _S2;

#line 38
            }

#line 38
            bool _S9;

#line 38
            if(_S8)
            {

#line 38
                _S9 = true;

#line 38
            }
            else
            {

#line 38
                _S9 = (_S6.y) >= _S3;

#line 38
            }

#line 38
            if(_S9)
            {
                dx_0 = dx_0 + int(1);

#line 35
                continue;
            }

#line 42
            int3 _S10 = int3(_S6, int(0));

#line 42
            longest_2 = cyMotionBlurLonger_0(longest_2, (((&kernelContext_0)->cyMotionBlurNeighbourSet_0->tiles_0).read(vec<uint,2>(((_S10)).xy), uint(((_S10)).z))).xy);

#line 35
            dx_0 = dx_0 + int(1);

#line 35
        }

#line 33
        int dy_1 = dy_0 + int(1);

#line 33
        longest_1 = longest_2;

#line 33
        dy_0 = dy_1;

#line 33
    }

#line 46
    (&kernelContext_0)->cyMotionBlurNeighbourSet_0->neighbours_0.write(float4(longest_1, 0.0, 0.0),uint2(_S4));
    return;
}

)cy_msl";

/// motion_blur_gather.metal, 8900 bytes.
inline constexpr char kMotionBlurGatherMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 11 "src/rendering/motion_blur/shaders/motion_blur_common.slang"
struct CyMotionBlurConstants_0
{
    float4 currentToPrevious0_0;
    float4 currentToPrevious1_0;
    float4 currentToPrevious2_0;
    float4 currentToPrevious3_0;
    float4 extent_0;
    float4 scales_0;
    float4 depth_0;
    uint4 control_0;
};


#line 5163 "hlsl.meta.slang"
struct CyMotionBlurGatherSet_default_0
{
    texture2d<float, access::sample> color_0;
    texture2d<float, access::sample> velocity_0;
    depth2d<float, access::sample> depth_1;
    texture2d<float, access::sample> neighbours_0;
    texture2d<float, access::read_write> output_0;
};


#line 5163
struct KernelContext_0
{
    CyMotionBlurConstants_0 constant* cyMotionBlur_0;
    CyMotionBlurGatherSet_default_0 constant* cyMotionBlurGatherSet_0;
};


#line 45 "src/rendering/motion_blur/shaders/motion_blur_common.slang"
float2 cyMotionBlurCameraMotion_0(uint2 pixel_0, float depth_2, KernelContext_0 thread* kernelContext_0)
{
    float2 _S1 = (float2(pixel_0) + float2(0.5) ) * kernelContext_0->cyMotionBlur_0->extent_0.zw;
    float _S2 = _S1.x * 2.0 - 1.0;

#line 48
    float _S3 = 1.0 - _S1.y * 2.0;

#line 48
    float4 _S4 = float4(_S2, _S3, depth_2, 1.0);

#line 53
    float2 _S5 = float2(dot(kernelContext_0->cyMotionBlur_0->currentToPrevious0_0, _S4), dot(kernelContext_0->cyMotionBlur_0->currentToPrevious1_0, _S4)) / float2(dot(kernelContext_0->cyMotionBlur_0->currentToPrevious3_0, _S4)) ;
    return float2((_S5.x - _S2) * 0.5 * kernelContext_0->cyMotionBlur_0->extent_0.x, (_S3 - _S5.y) * 0.5 * kernelContext_0->cyMotionBlur_0->extent_0.y);
}


#line 61
float2 cyMotionBlurVector_0(uint2 pixel_1, float2 velocity_1, float depth_3, KernelContext_0 thread* kernelContext_1)
{
    bool _S6 = depth_3 > 0.0;

#line 63
    float2 blur_0;

#line 63
    if(_S6)
    {

#line 63
        blur_0 = velocity_1 * kernelContext_1->cyMotionBlur_0->extent_0.xy;

#line 63
    }
    else
    {

#line 63
        float2 _S7 = cyMotionBlurCameraMotion_0(pixel_1, 0.0, kernelContext_1);

#line 63
        blur_0 = _S7;

#line 63
    }

    float2 _S8 = blur_0 * float2(kernelContext_1->cyMotionBlur_0->scales_0.x) ;

#line 65
    bool _S9;
    if((kernelContext_1->cyMotionBlur_0->depth_0.z) > 0.5)
    {

#line 66
        _S9 = _S6;

#line 66
    }
    else
    {

#line 66
        _S9 = false;

#line 66
    }

#line 66
    if(_S9)
    {

#line 66
        float2 _S10 = cyMotionBlurCameraMotion_0(pixel_1, depth_3, kernelContext_1);

#line 66
        blur_0 = _S10 * float2(kernelContext_1->cyMotionBlur_0->scales_0.x)  + (blur_0 - _S10) * float2(kernelContext_1->cyMotionBlur_0->scales_0.y) ;

#line 66
    }
    else
    {

#line 66
        blur_0 = _S8;

#line 66
    }

#line 71
    float _S11 = length(blur_0);
    if(_S11 > (kernelContext_1->cyMotionBlur_0->scales_0.z))
    {

#line 72
        blur_0 = blur_0 * float2((kernelContext_1->cyMotionBlur_0->scales_0.z / _S11)) ;

#line 72
    }



    return blur_0;
}


#line 71 "src/rendering/motion_blur/shaders/motion_blur_gather.slang"
float cyMotionBlurRadiusAt_0(uint2 pixel_2, KernelContext_0 thread* kernelContext_2)
{
    int3 _S12 = int3(int2(pixel_2), int(0));

#line 73
    float2 _S13 = cyMotionBlurVector_0(pixel_2, ((kernelContext_2->cyMotionBlurGatherSet_0->velocity_0).read(vec<uint,2>(((_S12)).xy), uint(((_S12)).z)).xy), ((kernelContext_2->cyMotionBlurGatherSet_0->depth_1).read(vec<uint,2>(((_S12)).xy), uint(((_S12)).z))), kernelContext_2);

    return max(length(_S13), 0.5);
}


#line 36 "src/rendering/motion_blur/shaders/motion_blur_common.slang"
float cyMotionBlurViewDepth_0(float depth_4, KernelContext_0 thread* kernelContext_3)
{
    float _S14 = depth_4 + kernelContext_3->cyMotionBlur_0->depth_0.x;

#line 38
    float _S15;
    if(_S14 > 0.0)
    {

#line 39
        _S15 = min(kernelContext_3->cyMotionBlur_0->depth_0.y / _S14, 1.00000001504746622e+30);

#line 39
    }
    else
    {

#line 39
        _S15 = 1.00000001504746622e+30;

#line 39
    }

#line 39
    return _S15;
}


#line 46 "src/rendering/motion_blur/shaders/motion_blur_gather.slang"
float cyMotionBlurNoise_0(uint2 pixel_3)
{
    float2 _S16 = float2(pixel_3) + float2(0.5) ;
    return fract(52.98291778564453125 * fract(_S16.x * 0.06711056083440781 + _S16.y * 0.00583714991807938));
}


#line 66
float cyMotionBlurSoftDepthCompare_0(float a_0, float b_0, KernelContext_0 thread* kernelContext_4)
{
    return saturate(1.0 - (b_0 - a_0) / kernelContext_4->cyMotionBlur_0->scales_0.w);
}


#line 52
float cyMotionBlurCone_0(float distance_0, float radius_0)
{
    return saturate(1.0 - distance_0 / radius_0);
}

float cyMotionBlurCylinder_0(float distance_1, float radius_1)
{
    float _S17 = 0.94999998807907104 * radius_1;

    float _S18 = saturate((distance_1 - _S17) / (1.04999995231628418 * radius_1 - _S17));
    return 1.0 - _S18 * _S18 * (3.0 - 2.0 * _S18);
}


#line 78
float4 cyMotionBlurReconstruct_0(uint2 pixel_4, KernelContext_0 thread* kernelContext_5)
{
    int3 _S19 = int3(int2(pixel_4), int(0));

#line 80
    float4 _S20 = ((kernelContext_5->cyMotionBlurGatherSet_0->color_0).read(vec<uint,2>(((_S19)).xy), uint(((_S19)).z)));

#line 80
    texture2d<float, access::sample> _S21 = kernelContext_5->cyMotionBlurGatherSet_0->neighbours_0;

    uint2 _S22 = pixel_4 / uint2(kernelContext_5->cyMotionBlur_0->control_0.y) ;

#line 82
    int3 _S23 = int3(int2(_S22), int(0));

#line 82
    float2 _S24 = ((_S21).read(vec<uint,2>(((_S23)).xy), uint(((_S23)).z))).xy;
    if((dot(_S24, _S24)) < 0.25)
    {
        return _S20;
    }

#line 85
    float _S25 = cyMotionBlurRadiusAt_0(pixel_4, kernelContext_5);

#line 85
    float _S26 = cyMotionBlurViewDepth_0(((kernelContext_5->cyMotionBlurGatherSet_0->depth_1).read(vec<uint,2>(((_S19)).xy), uint(((_S19)).z))), kernelContext_5);



    float2 _S27 = kernelContext_5->cyMotionBlur_0->extent_0.xy - float2(1.0) ;

    float weight_0 = 1.0 / _S25;
    float3 _S28 = _S20.xyz * float3(weight_0) ;
    float _S29 = cyMotionBlurNoise_0(pixel_4) - 0.5;
    uint _S30 = kernelContext_5->cyMotionBlur_0->control_0.x;
    uint _S31 = (_S30 - 1U) / 2U;

#line 95
    uint index_0 = 0U;

#line 95
    float weight_1 = weight_0;

#line 95
    float3 sum_0 = _S28;
    for(;;)
    {

#line 96
        if(index_0 < _S30)
        {
        }
        else
        {

#line 96
            break;
        }
        if(index_0 == _S31)
        {
            index_0 = index_0 + 1U;

#line 96
            continue;
        }

#line 104
        float2 _S32 = float2(pixel_4);
        uint2 _S33 = uint2(clamp(floor(_S32 + float2(0.5)  + _S24 * float2((-1.0 + 2.0 * (float(index_0) + _S29 + 1.0) / (float(_S30) + 1.0))) ), float2(0.0, 0.0), _S27));


        float _S34 = length(float2(_S33) - _S32);
        int3 _S35 = int3(int2(_S33), int(0));

#line 109
        float _S36 = cyMotionBlurViewDepth_0(((kernelContext_5->cyMotionBlurGatherSet_0->depth_1).read(vec<uint,2>(((_S35)).xy), uint(((_S35)).z))), kernelContext_5);

#line 109
        float _S37 = cyMotionBlurRadiusAt_0(_S33, kernelContext_5);

#line 109
        float _S38 = cyMotionBlurSoftDepthCompare_0(_S26, _S36, kernelContext_5);

#line 109
        float _S39 = cyMotionBlurSoftDepthCompare_0(_S36, _S26, kernelContext_5);

)cy_msl"
    R"cy_msl(#line 114
        float _S40 = _S38 * cyMotionBlurCone_0(_S34, _S37) + _S39 * cyMotionBlurCone_0(_S34, _S25) + cyMotionBlurCylinder_0(_S34, _S37) * cyMotionBlurCylinder_0(_S34, _S25) * 2.0;



        float3 sum_1 = sum_0 + ((kernelContext_5->cyMotionBlurGatherSet_0->color_0).read(vec<uint,2>(((_S35)).xy), uint(((_S35)).z))).xyz * float3(_S40) ;

#line 118
        weight_1 = weight_1 + _S40;

#line 118
        sum_0 = sum_1;

#line 96
        index_0 = index_0 + 1U;

#line 96
    }

#line 120
    return float4(sum_0 / float3(weight_1) , _S20.w);
}



[[kernel]] void cyMotionBlurGather(uint3 thread_0 [[thread_position_in_grid]], CyMotionBlurConstants_0 constant* cyMotionBlur_1 [[buffer(0)]], CyMotionBlurGatherSet_default_0 constant* cyMotionBlurGatherSet_1 [[buffer(1)]])
{

#line 125
    thread KernelContext_0 kernelContext_6;

#line 125
    (&kernelContext_6)->cyMotionBlur_0 = cyMotionBlur_1;

#line 125
    (&kernelContext_6)->cyMotionBlurGatherSet_0 = cyMotionBlurGatherSet_1;

    uint2 _S41 = uint2(cyMotionBlur_1->extent_0.xy);

#line 127
    bool _S42;
    if((thread_0.x) >= (_S41.x))
    {

#line 128
        _S42 = true;

#line 128
    }
    else
    {

#line 128
        _S42 = (thread_0.y) >= (_S41.y);

#line 128
    }

#line 128
    if(_S42)
    {
        return;
    }
    uint2 _S43 = thread_0.xy;

#line 132
    uint2 _S44 = uint2(int2(_S43));

#line 132
    float4 _S45 = cyMotionBlurReconstruct_0(_S43, &kernelContext_6);

#line 132
    (&kernelContext_6)->cyMotionBlurGatherSet_0->output_0.write(_S45,_S44);
    return;
}

)cy_msl";

/// motion_blur_copy.metal, 1798 bytes.
inline constexpr char kMotionBlurCopyMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 11 "src/rendering/motion_blur/shaders/motion_blur_common.slang"
struct CyMotionBlurConstants_0
{
    float4 currentToPrevious0_0;
    float4 currentToPrevious1_0;
    float4 currentToPrevious2_0;
    float4 currentToPrevious3_0;
    float4 extent_0;
    float4 scales_0;
    float4 depth_0;
    uint4 control_0;
};


#line 5163 "hlsl.meta.slang"
struct CyMotionBlurCopySet_default_0
{
    texture2d<float, access::sample> color_0;
    texture2d<float, access::read_write> copy_0;
};


#line 5163
struct KernelContext_0
{
    CyMotionBlurConstants_0 constant* cyMotionBlur_0;
    CyMotionBlurCopySet_default_0 constant* cyMotionBlurCopySet_0;
};


#line 27 "src/rendering/motion_blur/shaders/motion_blur_copy.slang"
[[kernel]] void cyMotionBlurCopy(uint3 thread_0 [[thread_position_in_grid]], CyMotionBlurConstants_0 constant* cyMotionBlur_1 [[buffer(0)]], CyMotionBlurCopySet_default_0 constant* cyMotionBlurCopySet_1 [[buffer(1)]])
{

#line 27
    thread KernelContext_0 kernelContext_0;

#line 27
    (&kernelContext_0)->cyMotionBlur_0 = cyMotionBlur_1;

#line 27
    (&kernelContext_0)->cyMotionBlurCopySet_0 = cyMotionBlurCopySet_1;

    uint2 _S1 = uint2(cyMotionBlur_1->extent_0.xy);

#line 29
    bool _S2;
    if((thread_0.x) >= (_S1.x))
    {

#line 30
        _S2 = true;

#line 30
    }
    else
    {

#line 30
        _S2 = (thread_0.y) >= (_S1.y);

#line 30
    }

#line 30
    if(_S2)
    {
        return;
    }
    int2 _S3 = int2(thread_0.xy);
    int3 _S4 = int3(_S3, int(0));

#line 34
    (&kernelContext_0)->cyMotionBlurCopySet_0->copy_0.write((((&kernelContext_0)->cyMotionBlurCopySet_0->color_0).read(vec<uint,2>(((_S4)).xy), uint(((_S4)).z))),uint2(_S3));

    return;
}

)cy_msl";

}  // namespace cy::rendering::motion_blur

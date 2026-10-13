#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the device GI dispatches. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::gi_gpu {

/// cyGiTraceRays.metal, 11476 bytes.
inline constexpr char kGiTraceRaysMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 77 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiDispatch_0
{
    uint count_0;
    uint frame_0;
    uint reserved0_0;
    uint reserved1_0;
};


#line 48
struct GiSceneSet_default_0
{
    uint4 device* constants_0;
    uint device* pageTable_0;
    float device* bricks_0;
    float4 device* cards_0;
    float4 device* cardState_0;
    uint device* gridRanges_0;
    uint device* gridItems_0;
    float4 device* lights_0;
    float device* shadowDepths_0;
    uint device* selection_0;
    float4 device* results_0;
    float4 device* rays_0;
    float4 device* hits_0;
};


#line 48
struct KernelContext_0
{
    GiDispatch_0 constant* giDispatch_0;
    GiSceneSet_default_0 constant* giScene_0;
};


#line 111
uint giLevelCount_0(KernelContext_0 thread* kernelContext_0)
{

#line 112
    return kernelContext_0->giScene_0->constants_0[0U].x;
}


#line 119
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_1)
{

#line 120
    return (as_type<float>((kernelContext_1->giScene_0->constants_0[9U].y)));
}


#line 115
uint giWindowBricks_0(KernelContext_0 thread* kernelContext_2)
{

#line 116
    return kernelContext_2->giScene_0->constants_0[0U].y;
}


#line 163
int giWrap_0(int value_0, int modulus_0)
{

#line 164
    uint _S1 = uint(modulus_0);
    uint _S2 = (uint(value_0) + (_S1 << 20U)) % _S1;

#line 165
    return int(_S2);
}


#line 91
float giLerp_0(float a_0, float b_0, float t_0)
{

#line 92
    return a_0 + (b_0 - a_0) * t_0;
}


#line 124
float giTrilinear_0(uint slot_0, float fx_0, float fy_0, float fz_0, KernelContext_0 thread* kernelContext_3)
{
    int x0_0 = clamp(int(floor(fx_0)), int(0), int(3));
    int y0_0 = clamp(int(floor(fy_0)), int(0), int(3));
    int z0_0 = clamp(int(floor(fz_0)), int(0), int(3));



    float tx_0 = clamp(fx_0 - float(x0_0), 0.0, 1.0);
    float ty_0 = clamp(fy_0 - float(y0_0), 0.0, 1.0);

    uint base_0 = slot_0 * 64U;

    uint _S3 = uint(z0_0) * 4U;

#line 137
    uint _S4 = uint(y0_0);

#line 137
    uint _S5 = base_0 + (_S3 + _S4) * 4U;

#line 137
    uint _S6 = uint(x0_0);
    uint _S7 = uint(clamp(x0_0 + int(1), int(0), int(3)));
    uint _S8 = uint(clamp(y0_0 + int(1), int(0), int(3)));

#line 139
    uint _S9 = base_0 + (_S3 + _S8) * 4U;

    uint _S10 = uint(clamp(z0_0 + int(1), int(0), int(3))) * 4U;

#line 141
    uint _S11 = base_0 + (_S10 + _S4) * 4U;

    uint _S12 = base_0 + (_S10 + _S8) * 4U;

#line 149
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S5 + _S6], kernelContext_3->giScene_0->bricks_0[_S5 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S9 + _S6], kernelContext_3->giScene_0->bricks_0[_S9 + _S7], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S11 + _S6], kernelContext_3->giScene_0->bricks_0[_S11 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S12 + _S6], kernelContext_3->giScene_0->bricks_0[_S12 + _S7], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 171
float giSampleLevel_0(uint level_0, float3 p_0, float thread* farDistance_0, KernelContext_0 thread* kernelContext_4)
{

#line 172
    uint _S13 = 16U + level_0 * 2U;

#line 172
    float4 shape_0 = (as_type<float4>((kernelContext_4->giScene_0->constants_0[_S13])));
    int4 origin_0 = (as_type<int4>((kernelContext_4->giScene_0->constants_0[_S13 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_0.x / brick_0));
    int by_0 = int(floor(p_0.y / brick_0));
    int bz_0 = int(floor(p_0.z / brick_0));

#line 179
    uint _S14 = giWindowBricks_0(kernelContext_4);
    int dim_0 = int(_S14);
    int _S15 = origin_0.x;

#line 181
    bool _S16;

#line 181
    if(bx_0 < _S15)
    {

#line 181
        _S16 = true;

#line 181
    }
    else
    {

#line 181
        _S16 = by_0 < (origin_0.y);

#line 181
    }

#line 181
    if(_S16)
    {

#line 181
        _S16 = true;

#line 181
    }
    else
    {

#line 181
        _S16 = bz_0 < (origin_0.z);

#line 181
    }

#line 181
    if(_S16)
    {

#line 181
        _S16 = true;

#line 181
    }
    else
    {

#line 181
        _S16 = bx_0 >= (_S15 + dim_0);

#line 181
    }

#line 181
    if(_S16)
    {

#line 181
        _S16 = true;

#line 181
    }
    else
    {

#line 181
        _S16 = by_0 >= (origin_0.y + dim_0);

#line 181
    }
    if(_S16)
    {

#line 182
        _S16 = true;

#line 182
    }
    else
    {

#line 182
        _S16 = bz_0 >= (origin_0.z + dim_0);

#line 182
    }

#line 181
    if(_S16)
    {
        return *farDistance_0;
    }
    int _S17 = giWrap_0(bz_0, dim_0);

#line 185
    uint _S18 = uint(dim_0);

#line 185
    uint _S19 = uint(_S17) * _S18;

#line 185
    int _S20 = giWrap_0(by_0, dim_0);

#line 185
    uint _S21 = (_S19 + uint(_S20)) * _S18;
    int _S22 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_4->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S21 + uint(_S22))];
    if(slot_1 == 4294967295U)
    {

#line 189
        return *farDistance_0;
    }

    float3 local_0 = (p_0 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 192
    float _S23 = giTrilinear_0(slot_1, local_0.x, local_0.y, local_0.z, kernelContext_4);
    return _S23;
}


float giDistance_0(float3 p_1, KernelContext_0 thread* kernelContext_5)
{

#line 197
    uint _S24 = giLevelCount_0(kernelContext_5);

#line 197
    float answer_0 = 1.0e+06;

#line 197
    uint level_1 = 0U;


    for(;;)
    {

#line 200
        if(level_1 < _S24)
        {
        }
        else
        {

)cy_msl"
    R"cy_msl(#line 200
            break;
        }

#line 201
        thread float farDistance_1 = 0.0;

#line 201
        float _S25 = giSampleLevel_0(level_1, p_1, &farDistance_1, kernelContext_5);

        float _S26 = min(answer_0, _S25);
        if(_S25 < farDistance_1)
        {

#line 205
            return _S25;
        }

#line 200
        uint level_2 = level_1 + 1U;

#line 200
        answer_0 = _S26;

#line 200
        level_1 = level_2;

#line 200
    }

#line 208
    return answer_0;
}


#line 95
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 96
    float squared_0 = dot(v_0, v_0);

#line 96
    float3 _S27;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 97
        _S27 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 97
    }
    else
    {

#line 97
        _S27 = fallback_0;

#line 97
    }

#line 97
    return _S27;
}


#line 211
struct GiTraceHit_0
{
    bool hit_0;
    bool exhausted_0;
    float t_1;
    float closest_0;
    float3 position_0;
    float3 normal_0;
};

GiTraceHit_0 giSphereTrace_0(float3 origin_1, float3 direction_0, float maxDistance_0, float tMin_0, KernelContext_0 thread* kernelContext_6)
{

#line 222
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_0 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_1 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_0 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_0 = float3(0.0, 1.0, 0.0);

#line 228
    uint _S28 = giLevelCount_0(kernelContext_6);
    if(_S28 == 0U)
    {

#line 230
        return result_0;
    }

#line 230
    float _S29 = giFinestVoxel_0(kernelContext_6);


    float surface_0 = _S29 * 0.5;
    float _S30 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 235
    recent_0[int(0)] = 1.0e+06;

#line 235
    recent_0[int(1)] = 1.0e+06;

#line 235
    recent_0[int(2)] = 1.0e+06;

#line 235
    float closest_1 = 1.0e+06;

#line 235
    uint recentSlot_0 = 0U;

#line 235
    uint step_0 = 0U;

#line 235
    float t_2 = _S30;


    for(;;)
    {

#line 238
        if(step_0 < 192U)
        {
        }
        else
        {

#line 238
            break;
        }

#line 239
        if(t_2 > maxDistance_0)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_1 = origin_1 + direction_0 * float3(t_2) ;

#line 244
        float _S31 = giDistance_0(position_1, kernelContext_6);

        if(_S31 < surface_0)
        {

#line 247
            (&result_0)->hit_0 = true;
            (&result_0)->t_1 = t_2;
            (&result_0)->position_0 = position_1;


            float3 _S32 = float3(_S29, 0.0, 0.0);

#line 252
            float _S33 = giDistance_0(position_1 + _S32, kernelContext_6);

#line 252
            float _S34 = giDistance_0(position_1 - _S32, kernelContext_6);

#line 252
            float _S35 = _S33 - _S34;

            float3 _S36 = float3(0.0, _S29, 0.0);

#line 254
            float _S37 = giDistance_0(position_1 + _S36, kernelContext_6);

#line 254
            float _S38 = giDistance_0(position_1 - _S36, kernelContext_6);

#line 254
            float _S39 = _S37 - _S38;

            float3 _S40 = float3(0.0, 0.0, _S29);

#line 256
            float _S41 = giDistance_0(position_1 + _S40, kernelContext_6);

#line 256
            float _S42 = giDistance_0(position_1 - _S40, kernelContext_6);

            (&result_0)->normal_0 = giNormalisedOr_0(float3(_S35, _S39, _S41 - _S42), - direction_0);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S43 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S31;
        uint _S44 = (recentSlot_0 + 1U) % 3U;
        float t_3 = t_2 + max(_S31, surface_0);

#line 238
        uint step_1 = step_0 + 1U;

#line 238
        closest_1 = _S43;

#line 238
        recentSlot_0 = _S44;

#line 238
        step_0 = step_1;

#line 238
        t_2 = t_3;

#line 238
    }

#line 267
    (&result_0)->exhausted_0 = true;

    (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
    return result_0;
}


#line 24 "src/rendering/gi_gpu/shaders/gi_trace.slang"
[[kernel]] void cyGiTraceRays(uint3 thread_0 [[thread_position_in_grid]], GiDispatch_0 constant* giDispatch_1 [[buffer(1)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]])
{

#line 24
    thread KernelContext_0 kernelContext_7;

#line 24
    (&kernelContext_7)->giDispatch_0 = giDispatch_1;

#line 24
    (&kernelContext_7)->giScene_0 = giScene_1;
    uint ray_0 = thread_0.x;
    if(ray_0 >= (giDispatch_1->count_0))
    {

#line 27
        return;
    }
    uint _S45 = ray_0 * 2U;

#line 29
    float4 origin_2 = (&kernelContext_7)->giScene_0->rays_0[_S45];
    float4 direction_1 = (&kernelContext_7)->giScene_0->rays_0[_S45 + 1U];

#line 30
    GiTraceHit_0 _S46 = giSphereTrace_0(origin_2.xyz, direction_1.xyz, origin_2.w, direction_1.w, &kernelContext_7);

    uint _S47 = ray_0 * 3U;

#line 32
    float4 device* _S48 = (&kernelContext_7)->giScene_0->hits_0+_S47;

#line 32
    float _S49;
    if(_S46.hit_0)
    {

#line 33
        _S49 = 1.0;

#line 33
    }
    else
    {

#line 33
        _S49 = 0.0;

#line 33
    }

#line 33
    float _S50;

#line 33
    if(_S46.exhausted_0)
    {

#line 33
        _S50 = 1.0;

#line 33
    }
    else
    {

#line 33
        _S50 = 0.0;

#line 33
    }

#line 32
    *_S48 = float4(_S49, _S46.t_1, _S46.closest_0, _S50);

)cy_msl"
    R"cy_msl(    *((&kernelContext_7)->giScene_0->hits_0+(_S47 + 1U)) = float4(_S46.position_0, 0.0);
    *((&kernelContext_7)->giScene_0->hits_0+(_S47 + 2U)) = float4(_S46.normal_0, 0.0);
    return;
}

)cy_msl";

/// cyGiShadeCards.metal, 31436 bytes.
inline constexpr char kGiShadeCardsMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 95 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 96
    float squared_0 = dot(v_0, v_0);

#line 96
    float3 _S1;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 97
        _S1 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 97
    }
    else
    {

#line 97
        _S1 = fallback_0;

#line 97
    }

#line 97
    return _S1;
}


#line 77
struct GiDispatch_0
{
    uint count_0;
    uint frame_0;
    uint reserved0_0;
    uint reserved1_0;
};


#line 48
struct GiSceneSet_default_0
{
    uint4 device* constants_0;
    uint device* pageTable_0;
    float device* bricks_0;
    float4 device* cards_0;
    float4 device* cardState_0;
    uint device* gridRanges_0;
    uint device* gridItems_0;
    float4 device* lights_0;
    float device* shadowDepths_0;
    uint device* selection_0;
    float4 device* results_0;
    float4 device* rays_0;
    float4 device* hits_0;
};


#line 48
struct KernelContext_0
{
    GiDispatch_0 constant* giDispatch_0;
    GiSceneSet_default_0 constant* giScene_0;
};


#line 342
float3 giDirectRadiance_0(uint light_0, float3 position_0, float3 normal_0, KernelContext_0 thread* kernelContext_0)
{

#line 343
    uint _S2 = light_0 * 4U;

#line 343
    float4 a_0 = kernelContext_0->giScene_0->lights_0[_S2];
    float4 b_0 = kernelContext_0->giScene_0->lights_0[_S2 + 1U];
    float4 c_0 = kernelContext_0->giScene_0->lights_0[_S2 + 2U];
    if((c_0.w) != 0.0)
    {

        return c_0.xyz * float3((a_0.w * max(0.0, dot(normal_0, - giNormalisedOr_0(b_0.xyz, float3(0.0, 1.0, 0.0)))))) ;
    }
    float3 offset_0 = a_0.xyz - position_0;
    float _S3 = max(dot(offset_0, offset_0), 9.99999997475242708e-07);
    float _S4 = b_0.w;

#line 353
    bool _S5;

#line 353
    if(_S4 > 0.0)
    {

#line 353
        _S5 = _S3 > (_S4 * _S4);

#line 353
    }
    else
    {

#line 353
        _S5 = false;

#line 353
    }

#line 353
    if(_S5)
    {

#line 354
        return float3(0.0, 0.0, 0.0);
    }


    return c_0.xyz * float3((a_0.w * max(0.0, dot(normal_0, offset_0 / float3(sqrt(_S3)) )) / _S3)) ;
}


#line 291
bool giShadowMapShadowed_0(float3 p_0, KernelContext_0 thread* kernelContext_1)
{

#line 292
    float4 centre_0 = (as_type<float4>((kernelContext_1->giScene_0->constants_0[5U])));
    float4 dir_0 = (as_type<float4>((kernelContext_1->giScene_0->constants_0[6U])));

    float4 up_0 = (as_type<float4>((kernelContext_1->giScene_0->constants_0[8U])));
    uint resolution_0 = kernelContext_1->giScene_0->constants_0[7U].w;
    float extent_0 = centre_0.w;
    float3 relative_0 = p_0 - centre_0.xyz;
    float u_0 = dot(relative_0, (as_type<float4>((kernelContext_1->giScene_0->constants_0[7U]))).xyz);
    float v_1 = dot(relative_0, up_0.xyz);
    float _S6 = dir_0.w;

#line 301
    float depth_0 = dot(relative_0, dir_0.xyz) + _S6 * 0.5;

#line 301
    bool _S7;
    if((abs(u_0)) >= extent_0)
    {

#line 302
        _S7 = true;

#line 302
    }
    else
    {

#line 302
        _S7 = (abs(v_1)) >= extent_0;

#line 302
    }

#line 302
    if(_S7)
    {

#line 302
        _S7 = true;

#line 302
    }
    else
    {

#line 302
        _S7 = depth_0 < 0.0;

#line 302
    }

#line 302
    if(_S7)
    {

#line 302
        _S7 = true;

#line 302
    }
    else
    {

#line 302
        _S7 = depth_0 > _S6;

#line 302
    }

#line 302
    if(_S7)
    {

#line 303
        return false;
    }
    int last_0 = int(resolution_0) - int(1);
    float _S8 = float(resolution_0);


    return kernelContext_1->giScene_0->shadowDepths_0[uint(clamp(int(floor((v_1 / extent_0 * 0.5 + 0.5) * _S8)), int(0), last_0)) * resolution_0 + uint(clamp(int(floor((u_0 / extent_0 * 0.5 + 0.5) * _S8)), int(0), last_0))] < (depth_0 - up_0.w);
}


#line 119
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_2)
{

#line 120
    return (as_type<float>((kernelContext_2->giScene_0->constants_0[9U].y)));
}


#line 119
float giFinestVoxel_1(KernelContext_0 thread* kernelContext_3)
{

#line 120
    return (as_type<float>((kernelContext_3->giScene_0->constants_0[9U].y)));
}


#line 111
uint giLevelCount_0(KernelContext_0 thread* kernelContext_4)
{

#line 112
    return kernelContext_4->giScene_0->constants_0[0U].x;
}

uint giWindowBricks_0(KernelContext_0 thread* kernelContext_5)
{

#line 116
    return kernelContext_5->giScene_0->constants_0[0U].y;
}


#line 163
int giWrap_0(int value_0, int modulus_0)
{

#line 164
    uint _S9 = uint(modulus_0);
    uint _S10 = (uint(value_0) + (_S9 << 20U)) % _S9;

#line 165
    return int(_S10);
}


#line 91
float giLerp_0(float a_1, float b_1, float t_0)
{

#line 92
    return a_1 + (b_1 - a_1) * t_0;
}


#line 124
float giTrilinear_0(uint slot_0, float fx_0, float fy_0, float fz_0, KernelContext_0 thread* kernelContext_6)
{
    int x0_0 = clamp(int(floor(fx_0)), int(0), int(3));
    int y0_0 = clamp(int(floor(fy_0)), int(0), int(3));
    int z0_0 = clamp(int(floor(fz_0)), int(0), int(3));



    float tx_0 = clamp(fx_0 - float(x0_0), 0.0, 1.0);
    float ty_0 = clamp(fy_0 - float(y0_0), 0.0, 1.0);

    uint base_0 = slot_0 * 64U;

    uint _S11 = uint(z0_0) * 4U;

#line 137
    uint _S12 = uint(y0_0);

#line 137
    uint _S13 = base_0 + (_S11 + _S12) * 4U;

#line 137
    uint _S14 = uint(x0_0);
    uint _S15 = uint(clamp(x0_0 + int(1), int(0), int(3)));
    uint _S16 = uint(clamp(y0_0 + int(1), int(0), int(3)));

#line 139
    uint _S17 = base_0 + (_S11 + _S16) * 4U;

)cy_msl"
    R"cy_msl(    uint _S18 = uint(clamp(z0_0 + int(1), int(0), int(3))) * 4U;

#line 141
    uint _S19 = base_0 + (_S18 + _S12) * 4U;

    uint _S20 = base_0 + (_S18 + _S16) * 4U;

#line 149
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_6->giScene_0->bricks_0[_S13 + _S14], kernelContext_6->giScene_0->bricks_0[_S13 + _S15], tx_0), giLerp_0(kernelContext_6->giScene_0->bricks_0[_S17 + _S14], kernelContext_6->giScene_0->bricks_0[_S17 + _S15], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_6->giScene_0->bricks_0[_S19 + _S14], kernelContext_6->giScene_0->bricks_0[_S19 + _S15], tx_0), giLerp_0(kernelContext_6->giScene_0->bricks_0[_S20 + _S14], kernelContext_6->giScene_0->bricks_0[_S20 + _S15], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 171
float giSampleLevel_0(uint level_0, float3 p_1, float thread* farDistance_0, KernelContext_0 thread* kernelContext_7)
{

#line 172
    uint _S21 = 16U + level_0 * 2U;

#line 172
    float4 shape_0 = (as_type<float4>((kernelContext_7->giScene_0->constants_0[_S21])));
    int4 origin_0 = (as_type<int4>((kernelContext_7->giScene_0->constants_0[_S21 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_1.x / brick_0));
    int by_0 = int(floor(p_1.y / brick_0));
    int bz_0 = int(floor(p_1.z / brick_0));

#line 179
    uint _S22 = giWindowBricks_0(kernelContext_7);
    int dim_0 = int(_S22);
    int _S23 = origin_0.x;

#line 181
    bool _S24;

#line 181
    if(bx_0 < _S23)
    {

#line 181
        _S24 = true;

#line 181
    }
    else
    {

#line 181
        _S24 = by_0 < (origin_0.y);

#line 181
    }

#line 181
    if(_S24)
    {

#line 181
        _S24 = true;

#line 181
    }
    else
    {

#line 181
        _S24 = bz_0 < (origin_0.z);

#line 181
    }

#line 181
    if(_S24)
    {

#line 181
        _S24 = true;

#line 181
    }
    else
    {

#line 181
        _S24 = bx_0 >= (_S23 + dim_0);

#line 181
    }

#line 181
    if(_S24)
    {

#line 181
        _S24 = true;

#line 181
    }
    else
    {

#line 181
        _S24 = by_0 >= (origin_0.y + dim_0);

#line 181
    }
    if(_S24)
    {

#line 182
        _S24 = true;

#line 182
    }
    else
    {

#line 182
        _S24 = bz_0 >= (origin_0.z + dim_0);

#line 182
    }

#line 181
    if(_S24)
    {
        return *farDistance_0;
    }
    int _S25 = giWrap_0(bz_0, dim_0);

#line 185
    uint _S26 = uint(dim_0);

#line 185
    uint _S27 = uint(_S25) * _S26;

#line 185
    int _S28 = giWrap_0(by_0, dim_0);

#line 185
    uint _S29 = (_S27 + uint(_S28)) * _S26;
    int _S30 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_7->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S29 + uint(_S30))];
    if(slot_1 == 4294967295U)
    {

#line 189
        return *farDistance_0;
    }

    float3 local_0 = (p_1 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 192
    float _S31 = giTrilinear_0(slot_1, local_0.x, local_0.y, local_0.z, kernelContext_7);
    return _S31;
}


float giDistance_0(float3 p_2, KernelContext_0 thread* kernelContext_8)
{

#line 197
    uint _S32 = giLevelCount_0(kernelContext_8);

#line 197
    float answer_0 = 1.0e+06;

#line 197
    uint level_1 = 0U;


    for(;;)
    {

#line 200
        if(level_1 < _S32)
        {
        }
        else
        {

#line 200
            break;
        }

#line 201
        thread float farDistance_1 = 0.0;

#line 201
        float _S33 = giSampleLevel_0(level_1, p_2, &farDistance_1, kernelContext_8);

        float _S34 = min(answer_0, _S33);
        if(_S33 < farDistance_1)
        {

#line 205
            return _S33;
        }

#line 200
        uint level_2 = level_1 + 1U;

#line 200
        answer_0 = _S34;

#line 200
        level_1 = level_2;

#line 200
    }

#line 208
    return answer_0;
}

struct GiTraceHit_0
{
    bool hit_0;
    bool exhausted_0;
    float t_1;
    float closest_0;
    float3 position_1;
    float3 normal_1;
};

GiTraceHit_0 giSphereTrace_0(float3 origin_1, float3 direction_0, float maxDistance_0, float tMin_0, KernelContext_0 thread* kernelContext_9)
{

#line 222
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_0 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_1 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_1 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_1 = float3(0.0, 1.0, 0.0);

#line 228
    uint _S35 = giLevelCount_0(kernelContext_9);
    if(_S35 == 0U)
    {

#line 230
        return result_0;
    }

#line 230
    float _S36 = giFinestVoxel_0(kernelContext_9);


    float surface_0 = _S36 * 0.5;
    float _S37 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 235
    recent_0[int(0)] = 1.0e+06;

#line 235
    recent_0[int(1)] = 1.0e+06;

#line 235
    recent_0[int(2)] = 1.0e+06;

#line 235
    float closest_1 = 1.0e+06;

#line 235
    uint recentSlot_0 = 0U;

#line 235
    uint step_0 = 0U;

#line 235
    float t_2 = _S37;


    for(;;)
    {

#line 238
        if(step_0 < 192U)
        {
        }
        else
        {

#line 238
            break;
        }

#line 239
        if(t_2 > maxDistance_0)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_2 = origin_1 + direction_0 * float3(t_2) ;

#line 244
        float _S38 = giDistance_0(position_2, kernelContext_9);

)cy_msl"
    R"cy_msl(        if(_S38 < surface_0)
        {

#line 247
            (&result_0)->hit_0 = true;
            (&result_0)->t_1 = t_2;
            (&result_0)->position_1 = position_2;


            float3 _S39 = float3(_S36, 0.0, 0.0);

#line 252
            float _S40 = giDistance_0(position_2 + _S39, kernelContext_9);

#line 252
            float _S41 = giDistance_0(position_2 - _S39, kernelContext_9);

#line 252
            float _S42 = _S40 - _S41;

            float3 _S43 = float3(0.0, _S36, 0.0);

#line 254
            float _S44 = giDistance_0(position_2 + _S43, kernelContext_9);

#line 254
            float _S45 = giDistance_0(position_2 - _S43, kernelContext_9);

#line 254
            float _S46 = _S44 - _S45;

            float3 _S47 = float3(0.0, 0.0, _S36);

#line 256
            float _S48 = giDistance_0(position_2 + _S47, kernelContext_9);

#line 256
            float _S49 = giDistance_0(position_2 - _S47, kernelContext_9);

            (&result_0)->normal_1 = giNormalisedOr_0(float3(_S42, _S46, _S48 - _S49), - direction_0);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S50 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S38;
        uint _S51 = (recentSlot_0 + 1U) % 3U;
        float t_3 = t_2 + max(_S38, surface_0);

#line 238
        uint step_1 = step_0 + 1U;

#line 238
        closest_1 = _S50;

#line 238
        recentSlot_0 = _S51;

#line 238
        step_0 = step_1;

#line 238
        t_2 = t_3;

#line 238
    }

#line 267
    (&result_0)->exhausted_0 = true;

    (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
    return result_0;
}


#line 221
GiTraceHit_0 giSphereTrace_1(float3 origin_2, float3 direction_1, float maxDistance_1, float tMin_1, KernelContext_0 thread* kernelContext_10)
{

#line 222
    thread GiTraceHit_0 result_1;
    (&result_1)->hit_0 = false;
    (&result_1)->exhausted_0 = false;
    (&result_1)->t_1 = 0.0;
    (&result_1)->closest_0 = 0.0;
    (&result_1)->position_1 = float3(0.0, 0.0, 0.0);
    (&result_1)->normal_1 = float3(0.0, 1.0, 0.0);

#line 228
    uint _S52 = giLevelCount_0(kernelContext_10);
    if(_S52 == 0U)
    {

#line 230
        return result_1;
    }

#line 230
    float _S53 = giFinestVoxel_0(kernelContext_10);


    float surface_1 = _S53 * 0.5;
    float _S54 = max(tMin_1, surface_1);
    thread array<float, int(3)> recent_1;

#line 235
    recent_1[int(0)] = 1.0e+06;

#line 235
    recent_1[int(1)] = 1.0e+06;

#line 235
    recent_1[int(2)] = 1.0e+06;

#line 235
    float closest_2 = 1.0e+06;

#line 235
    uint recentSlot_1 = 0U;

#line 235
    uint step_2 = 0U;

#line 235
    float t_4 = _S54;


    for(;;)
    {

#line 238
        if(step_2 < 192U)
        {
        }
        else
        {

#line 238
            break;
        }

#line 239
        if(t_4 > maxDistance_1)
        {
            (&result_1)->closest_0 = min(closest_2, min(recent_1[int(0)], min(recent_1[int(1)], recent_1[int(2)])));
            return result_1;
        }
        float3 position_3 = origin_2 + direction_1 * float3(t_4) ;

#line 244
        float _S55 = giDistance_0(position_3, kernelContext_10);

        if(_S55 < surface_1)
        {

#line 247
            (&result_1)->hit_0 = true;
            (&result_1)->t_1 = t_4;
            (&result_1)->position_1 = position_3;


            float3 _S56 = float3(_S53, 0.0, 0.0);

#line 252
            float _S57 = giDistance_0(position_3 + _S56, kernelContext_10);

#line 252
            float _S58 = giDistance_0(position_3 - _S56, kernelContext_10);

#line 252
            float _S59 = _S57 - _S58;

            float3 _S60 = float3(0.0, _S53, 0.0);

#line 254
            float _S61 = giDistance_0(position_3 + _S60, kernelContext_10);

#line 254
            float _S62 = giDistance_0(position_3 - _S60, kernelContext_10);

#line 254
            float _S63 = _S61 - _S62;

            float3 _S64 = float3(0.0, 0.0, _S53);

#line 256
            float _S65 = giDistance_0(position_3 + _S64, kernelContext_10);

#line 256
            float _S66 = giDistance_0(position_3 - _S64, kernelContext_10);

            (&result_1)->normal_1 = giNormalisedOr_0(float3(_S59, _S63, _S65 - _S66), - direction_1);
            (&result_1)->closest_0 = closest_2;
            return result_1;
        }
        float _S67 = min(closest_2, recent_1[recentSlot_1]);
        recent_1[recentSlot_1] = _S55;
        uint _S68 = (recentSlot_1 + 1U) % 3U;
        float t_5 = t_4 + max(_S55, surface_1);

#line 238
        uint step_3 = step_2 + 1U;

#line 238
        closest_2 = _S67;

#line 238
        recentSlot_1 = _S68;

#line 238
        step_2 = step_3;

#line 238
        t_4 = t_5;

#line 238
    }

#line 267
    (&result_1)->exhausted_0 = true;

    (&result_1)->closest_0 = min(closest_2, min(recent_1[int(0)], min(recent_1[int(1)], recent_1[int(2)])));
    return result_1;
}


#line 313
bool giFieldOccluded_0(float3 from_0, float3 to_0, KernelContext_0 thread* kernelContext_11)
{

#line 314
    float3 offset_1 = to_0 - from_0;
    float span_0 = length(offset_1);
    if(span_0 <= 0.00009999999747379)
    {

#line 317
        return false;
    }

#line 317
    float _S69 = giFinestVoxel_1(kernelContext_11);

    float bias_0 = _S69 * 2.0;
    if(span_0 <= bias_0)
    {

#line 321
        return false;
    }

#line 321
    GiTraceHit_0 _S70 = giSphereTrace_1(from_0, offset_1 / float3(span_0) , span_0 * 0.99900001287460327, bias_0, kernelContext_11);

    return _S70.hit_0;
}


bool giOccluded_0(float3 from_1, float3 to_1, KernelContext_0 thread* kernelContext_12)
{

#line 328
    float3 offset_2 = to_1 - from_1;
    float span_1 = length(offset_2);
    if(span_1 <= 0.00009999999747379)
    {

#line 331
        return false;
    }
    float3 dir_1 = (as_type<float3>((kernelContext_12->giScene_0->constants_0[6U].xyz)));

#line 333
    bool _S71;
    if((kernelContext_12->giScene_0->constants_0[9U].x) != 0U)
    {

#line 334
        _S71 = (dot(offset_2 / float3(span_1) , - dir_1)) > 0.99989998340606689;

#line 334
    }
    else
    {

#line 334
        _S71 = false;

#line 334
    }

#line 334
    if(_S71)
    {

#line 334
        bool _S72 = giShadowMapShadowed_0(from_1, kernelContext_12);
        return _S72;
    }

#line 335
    bool _S73 = giFieldOccluded_0(from_1, to_1, kernelContext_12);

)cy_msl"
    R"cy_msl(    return _S73;
}


#line 361
float3 giShadedDirect_0(float3 position_4, float3 normal_2, KernelContext_0 thread* kernelContext_13)
{
    uint _S74 = kernelContext_13->giScene_0->constants_0[0U].z;

#line 363
    float3 total_0 = float3(0.0, 0.0, 0.0);

#line 363
    uint light_1 = 0U;
    for(;;)
    {

#line 364
        if(light_1 < _S74)
        {
        }
        else
        {

#line 364
            break;
        }

#line 364
        float3 _S75 = giDirectRadiance_0(light_1, position_4, normal_2, kernelContext_13);

#line 364
        bool _S76;

        if((_S75.x) <= 0.0)
        {

#line 366
            _S76 = (_S75.y) <= 0.0;

#line 366
        }
        else
        {

#line 366
            _S76 = false;

#line 366
        }

#line 366
        bool _S77;

#line 366
        if(_S76)
        {

#line 366
            _S77 = (_S75.z) <= 0.0;

#line 366
        }
        else
        {

#line 366
            _S77 = false;

#line 366
        }

#line 366
        if(_S77)
        {

#line 367
            light_1 = light_1 + 1U;

#line 364
            continue;
        }



        uint _S78 = light_1 * 4U;

#line 369
        float4 a_2 = kernelContext_13->giScene_0->lights_0[_S78];
        float4 b_2 = kernelContext_13->giScene_0->lights_0[_S78 + 1U];

#line 370
        float3 target_0;


        if((kernelContext_13->giScene_0->lights_0[_S78 + 2U].w) != 0.0)
        {

#line 373
            target_0 = position_4 - giNormalisedOr_0(b_2.xyz, float3(0.0, 1.0, 0.0)) * float3(1000.0) ;

#line 373
        }
        else
        {

#line 373
            target_0 = a_2.xyz;

#line 373
        }

#line 373
        bool _S79 = giOccluded_0(position_4 + normal_2 * float3(0.00999999977648258) , target_0, kernelContext_13);

        if(_S79)
        {

#line 376
            light_1 = light_1 + 1U;

#line 364
            continue;
        }

#line 364
        total_0 = total_0 + _S75;

#line 364
        light_1 = light_1 + 1U;

#line 364
    }

#line 380
    return total_0;
}


#line 274
float3 giHemisphereDirection_0(float3 normal_3, uint index_0, uint count_1)
{
    float _S80 = float(index_0);
    float radius_0 = sqrt((_S80 + 0.5) / float(count_1));
    float angle_0 = 2.3999631404876709 * _S80;
    float x_0 = radius_0 * cos(angle_0);
    float y_0 = radius_0 * sin(angle_0);
    float z_0 = sqrt(max(0.0, 1.0 - x_0 * x_0 - y_0 * y_0));

#line 281
    float3 tangent_0;
    if((abs(normal_3.y)) < 0.99000000953674316)
    {

#line 282
        tangent_0 = cross(float3(0.0, 1.0, 0.0), normal_3);

#line 282
    }
    else
    {

#line 282
        tangent_0 = cross(float3(1.0, 0.0, 0.0), normal_3);

#line 282
    }

    float3 tangent_1 = giNormalisedOr_0(tangent_0, float3(1.0, 0.0, 0.0));

    return giNormalisedOr_0(tangent_1 * float3(x_0)  + cross(normal_3, tangent_1) * float3(y_0)  + normal_3 * float3(z_0) , normal_3);
}


#line 385
float3 giSky_0(float3 direction_2, KernelContext_0 thread* kernelContext_14)
{

#line 386
    float4 zenith_0 = (as_type<float4>((kernelContext_14->giScene_0->constants_0[2U])));
    float3 horizon_0 = (as_type<float3>((kernelContext_14->giScene_0->constants_0[3U].xyz)));
    float3 ground_0 = (as_type<float3>((kernelContext_14->giScene_0->constants_0[4U].xyz)));

    float _S81 = giNormalisedOr_0(direction_2, float3(0.0, 1.0, 0.0)).y;

#line 390
    if(_S81 < 0.0)
    {

#line 391
        return ground_0 * float3(zenith_0.w) ;
    }

    return (horizon_0 + (zenith_0.xyz - horizon_0) * float3(clamp(pow(_S81, 0.44999998807907104), 0.0, 1.0)) ) * float3(zenith_0.w) ;
}



int giCell_0(float value_1, float cell_0)
{

#line 400
    return int(floor(value_1 / cell_0));
}

uint giBucket_0(int x_1, int y_1, int z_1, uint buckets_0)
{
    return (((uint(x_1) * 73856093U) ^ (uint(y_1) * 19349663U)) ^ (uint(z_1) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_5, float3 normal_4, KernelContext_0 thread* kernelContext_15)
{

#line 410
    uint4 grid_0 = kernelContext_15->giScene_0->constants_0[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 413
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_1 = (as_type<float>((grid_0.z)));
    float _S82 = radius_1 * radius_1;
    float _S83 = position_5.x;

#line 418
    int _S84 = giCell_0(_S83 - radius_1, cell_1);

#line 418
    float _S85 = position_5.y;

#line 418
    int _S86 = giCell_0(_S85 - radius_1, cell_1);
    float _S87 = position_5.z;

#line 419
    int _S88 = giCell_0(_S87 - radius_1, cell_1);
    int _S89 = giCell_0(_S83 + radius_1, cell_1);

#line 420
    int _S90 = giCell_0(_S85 + radius_1, cell_1);
    int _S91 = giCell_0(_S87 + radius_1, cell_1);

#line 421
    float bestScore_0 = 1.0e+09;

#line 421
    float bestAlignment_0 = 0.0;

#line 421
    uint best_0 = 4294967295U;

#line 421
    int z_2 = _S88;



    for(;;)
    {

#line 425
        if(z_2 <= _S91)
        {
        }
        else
        {

#line 425
            break;
        }

#line 425
        int y_2 = _S86;
        for(;;)
        {

#line 426
            if(y_2 <= _S90)
            {
            }
            else
            {

#line 426
                break;
            }

#line 426
            float bestScore_1 = bestScore_0;

#line 426
            float bestAlignment_1 = bestAlignment_0;

#line 426
            uint best_1 = best_0;

#line 426
            int x_2 = _S84;
            for(;;)
            {

)cy_msl"
    R"cy_msl(#line 427
                if(x_2 <= _S89)
                {
                }
                else
                {

#line 427
                    break;
                }
                uint _S92 = giBucket_0(x_2, y_2, z_2, buckets_1) * 2U;

#line 429
                uint first_0 = kernelContext_15->giScene_0->gridRanges_0[_S92];
                uint _S93 = kernelContext_15->giScene_0->gridRanges_0[_S92 + 1U];

#line 430
                float bestScore_2 = bestScore_1;

#line 430
                float bestAlignment_2 = bestAlignment_1;

#line 430
                uint best_2 = best_1;

#line 430
                uint item_0 = first_0;
                for(;;)
                {

#line 431
                    if(item_0 < (first_0 + _S93))
                    {
                    }
                    else
                    {

#line 431
                        break;
                    }

#line 432
                    uint handle_0 = kernelContext_15->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_15->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 434
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }



                    uint _S94 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_15->giScene_0->cards_0[_S94 + 1U].xyz, normal_4);
                    if(alignment_0 <= 0.25)
                    {

#line 439
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }

#line 441
                    float3 offset_3 = kernelContext_15->giScene_0->cards_0[_S94].xyz - position_5;
                    float squared_1 = dot(offset_3, offset_3);
                    if(squared_1 > _S82)
                    {

#line 444
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }

#line 447
                    float score_0 = squared_1 / alignment_0;

#line 447
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 448
                        better_0 = score_0 < bestScore_2;

#line 448
                    }
                    else
                    {

#line 449
                        if(alignment_0 != bestAlignment_2)
                        {

#line 449
                            better_0 = alignment_0 > bestAlignment_2;

#line 449
                        }
                        else
                        {

#line 449
                            better_0 = handle_0 < best_2;

#line 449
                        }

#line 448
                    }

#line 448
                    float bestScore_3;

#line 448
                    float bestAlignment_3;

#line 448
                    uint best_3;


                    if(better_0)
                    {

#line 451
                        bestScore_3 = score_0;

#line 451
                        bestAlignment_3 = alignment_0;

#line 451
                        best_3 = handle_0;

#line 451
                    }
                    else
                    {

#line 451
                        bestScore_3 = bestScore_2;

#line 451
                        bestAlignment_3 = bestAlignment_2;

#line 451
                        best_3 = best_2;

#line 451
                    }

#line 451
                    bestScore_2 = bestScore_3;

#line 451
                    bestAlignment_2 = bestAlignment_3;

#line 451
                    best_2 = best_3;

#line 431
                    item_0 = item_0 + 1U;

#line 431
                }

#line 427
                int x_3 = x_2 + int(1);

#line 427
                bestScore_1 = bestScore_2;

#line 427
                bestAlignment_1 = bestAlignment_2;

#line 427
                best_1 = best_2;

#line 427
                x_2 = x_3;

#line 427
            }

#line 426
            int y_3 = y_2 + int(1);

#line 426
            bestScore_0 = bestScore_1;

#line 426
            bestAlignment_0 = bestAlignment_1;

#line 426
            best_0 = best_1;

#line 426
            y_2 = y_3;

#line 426
        }

#line 425
        z_2 = z_2 + int(1);

#line 425
    }

#line 460
    return best_0;
}



float3 giGather_0(float3 position_6, float3 normal_5, KernelContext_0 thread* kernelContext_16)
{

#line 466
    uint rays_1 = kernelContext_16->giScene_0->constants_0[0U].w;

#line 466
    bool _S95;
    if(rays_1 == 0U)
    {

#line 467
        _S95 = true;

#line 467
    }
    else
    {

#line 467
        uint _S96 = giLevelCount_0(kernelContext_16);

#line 467
        _S95 = _S96 == 0U;

#line 467
    }

#line 467
    if(_S95)
    {

#line 468
        return float3(0.0, 0.0, 0.0);
    }
    float _S97 = (as_type<float>((kernelContext_16->giScene_0->constants_0[1U].w)));

#line 470
    float _S98 = giFinestVoxel_0(kernelContext_16);

    float _S99 = _S98 * 2.0;
    float3 _S100 = position_6 + normal_5 * float3(_S98) ;

#line 473
    float3 total_1 = float3(0.0, 0.0, 0.0);

#line 473
    uint index_1 = 0U;

    for(;;)
    {

#line 475
        if(index_1 < rays_1)
        {
        }
        else
        {

#line 475
            break;
        }

#line 476
        float3 direction_3 = giHemisphereDirection_0(normal_5, index_1, rays_1);

#line 476
        GiTraceHit_0 _S101 = giSphereTrace_0(_S100, direction_3, _S97, _S99, kernelContext_16);

        if(!_S101.hit_0)
        {

#line 478
            float3 _S102 = giSky_0(direction_3, kernelContext_16);

#line 478
            total_1 = total_1 + _S102;

            index_1 = index_1 + 1U;

#line 475
            continue;
        }

)cy_msl"
    R"cy_msl(#line 475
        uint _S103 = giFindCard_0(_S101.position_1, _S101.normal_1, kernelContext_16);

#line 475
        float3 total_2;

#line 483
        if(_S103 != 4294967295U)
        {

#line 483
            total_2 = total_1 + (*(kernelContext_16->giScene_0->cardState_0+_S103 * 2U)).xyz;

#line 483
        }
        else
        {

#line 483
            total_2 = total_1;

#line 483
        }

#line 483
        total_1 = total_2;

#line 475
        index_1 = index_1 + 1U;

#line 475
    }

#line 487
    return total_1 / float3(float(rays_1)) ;
}


#line 101
float giRelativeChange_0(float3 before_0, float3 after_0)
{

#line 102
    float magnitude_0 = length(before_0) + length(after_0);
    if(magnitude_0 <= 0.00000999999974738)
    {

#line 104
        return 0.0;
    }
    return length(after_0 - before_0) / magnitude_0;
}


#line 32 "src/rendering/gi_gpu/shaders/gi_cards.slang"
[[kernel]] void cyGiShadeCards(uint3 thread_0 [[thread_position_in_grid]], GiDispatch_0 constant* giDispatch_1 [[buffer(1)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]])
{

#line 32
    thread KernelContext_0 kernelContext_17;

#line 32
    (&kernelContext_17)->giDispatch_0 = giDispatch_1;

#line 32
    (&kernelContext_17)->giScene_0 = giScene_1;
    uint index_2 = thread_0.x;
    if(index_2 >= (giDispatch_1->count_0))
    {

#line 35
        return;
    }
    uint handle_1 = (&kernelContext_17)->giScene_0->selection_0[index_2];
    uint _S104 = handle_1 * 4U;

    float4 albedo_0 = (&kernelContext_17)->giScene_0->cards_0[_S104 + 2U];
    float4 emission_0 = (&kernelContext_17)->giScene_0->cards_0[_S104 + 3U];
    uint _S105 = handle_1 * 2U;

#line 42
    float3 before_1 = (*((&kernelContext_17)->giScene_0->cardState_0+_S105)).xyz;

    float3 _S106 = (&kernelContext_17)->giScene_0->cards_0[_S104].xyz;

#line 44
    float3 _S107 = (&kernelContext_17)->giScene_0->cards_0[_S104 + 1U].xyz;

#line 44
    float3 _S108 = giShadedDirect_0(_S106, _S107, &kernelContext_17);
    float3 _S109 = (*((&kernelContext_17)->giScene_0->cardState_0+(_S105 + 1U))).xyz;

#line 45
    float3 accumulated_0;
    if(((&kernelContext_17)->giScene_0->constants_0[0U].w) != 0U)
    {

#line 46
        float3 _S110 = giGather_0(_S106, _S107, &kernelContext_17);

#line 46
        accumulated_0 = albedo_0.xyz * _S110;

#line 46
    }
    else
    {

#line 46
        accumulated_0 = _S109;

#line 46
    }

#line 51
    float3 after_1 = emission_0.xyz + albedo_0.xyz * _S108 + accumulated_0;
    uint _S111 = index_2 * 3U;

#line 52
    *((&kernelContext_17)->giScene_0->results_0+_S111) = float4(_S108, giRelativeChange_0(before_1, after_1));
    *((&kernelContext_17)->giScene_0->results_0+(_S111 + 1U)) = float4(accumulated_0, (as_type<float>((handle_1))));
    *((&kernelContext_17)->giScene_0->results_0+(_S111 + 2U)) = float4(after_1, 1.0);
    return;
}

)cy_msl";

/// cyGiCommitCards.metal, 1865 bytes.
inline constexpr char kGiCommitCardsMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 77 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiDispatch_0
{
    uint count_0;
    uint frame_0;
    uint reserved0_0;
    uint reserved1_0;
};


#line 64 "src/rendering/gi_gpu/shaders/gi_cards.slang"
struct GiSceneSet_default_0
{
    uint4 device* constants_0;
    uint device* pageTable_0;
    float device* bricks_0;
    float4 device* cards_0;
    float4 device* cardState_0;
    uint device* gridRanges_0;
    uint device* gridItems_0;
    float4 device* lights_0;
    float device* shadowDepths_0;
    uint device* selection_0;
    float4 device* results_0;
    float4 device* rays_0;
    float4 device* hits_0;
};


#line 64
struct KernelContext_0
{
    GiDispatch_0 constant* giDispatch_0;
    GiSceneSet_default_0 constant* giScene_0;
};


#line 59
[[kernel]] void cyGiCommitCards(uint3 thread_0 [[thread_position_in_grid]], GiDispatch_0 constant* giDispatch_1 [[buffer(1)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]])
{

#line 59
    thread KernelContext_0 kernelContext_0;

#line 59
    (&kernelContext_0)->giDispatch_0 = giDispatch_1;

#line 59
    (&kernelContext_0)->giScene_0 = giScene_1;
    uint index_0 = thread_0.x;
    if(index_0 >= (giDispatch_1->count_0))
    {

#line 62
        return;
    }
    uint _S1 = index_0 * 3U;

#line 64
    float4 device* _S2 = (&kernelContext_0)->giScene_0->results_0+(_S1 + 1U);

#line 64
    float4 accumulated_0 = *_S2;

    uint _S3 = (as_type<uint>(((*_S2).w))) * 2U;

#line 66
    *((&kernelContext_0)->giScene_0->cardState_0+_S3) = float4((*((&kernelContext_0)->giScene_0->results_0+(_S1 + 2U))).xyz, 1.0);

    *((&kernelContext_0)->giScene_0->cardState_0+(_S3 + 1U)) = float4(accumulated_0.xyz, (as_type<float>(((&kernelContext_0)->giDispatch_0->frame_0))));
    return;
}

)cy_msl";

/// cyGiGatherProbes.metal, 37696 bytes.
inline constexpr char kGiGatherProbesMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 67 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiProbeSet_default_0
{
    uint4 device* constants_0;
    float4 device* probes_0;
    float device* payloads_0;
    uint device* selection_0;
    float4 device* directions_0;
    float4 device* results_0;
    uint device* layout_0;
    float4 device* queries_0;
    float4 device* samples_0;
    float4 device* tierRays_0;
    float4 device* tierResults_0;
    float device* screenDepth_0;
    float4 device* screenColour_0;
    float device* screenHzb_0;
    uint device* counters_0;
};


#line 74 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiSceneSet_default_0
{
    uint4 device* constants_1;
    uint device* pageTable_0;
    float device* bricks_0;
    float4 device* cards_0;
    float4 device* cardState_0;
    uint device* gridRanges_0;
    uint device* gridItems_0;
    float4 device* lights_0;
    float device* shadowDepths_0;
    uint device* selection_1;
    float4 device* results_1;
    float4 device* rays_0;
    float4 device* hits_0;
};


#line 77
struct GiDispatch_0
{
    uint count_0;
    uint frame_0;
    uint reserved0_0;
    uint reserved1_0;
};


#line 375 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct KernelContext_0
{
    GiProbeSet_default_0 constant* giProbes_0;
    GiSceneSet_default_0 constant* giScene_0;
    GiDispatch_0 constant* giDispatch_0;
    array<float3, int(64)> threadgroup* gpRayRadiance_0;
    array<float, int(64)> threadgroup* gpRayDistance_0;
};


#line 95 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 96
    float squared_0 = dot(v_0, v_0);

#line 96
    float3 _S1;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 97
        _S1 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 97
    }
    else
    {

#line 97
        _S1 = fallback_0;

#line 97
    }

#line 97
    return _S1;
}


#line 106 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
float gpClamp_0(float value_0, float lo_0, float hi_0)
{

#line 106
    float _S2;
    if(value_0 < lo_0)
    {

#line 107
        _S2 = lo_0;

#line 107
    }
    else
    {

#line 107
        if(value_0 > hi_0)
        {

#line 107
            _S2 = hi_0;

#line 107
        }
        else
        {

#line 107
            _S2 = value_0;

#line 107
        }

#line 107
    }

#line 107
    return _S2;
}


#line 121
float3 gpSky_0(float3 direction_0, KernelContext_0 thread* kernelContext_0)
{

#line 122
    float4 zenith_0 = (as_type<float4>((kernelContext_0->giProbes_0->constants_0[2U])));
    float3 horizon_0 = (as_type<float3>((kernelContext_0->giProbes_0->constants_0[3U].xyz)));
    float3 ground_0 = (as_type<float3>((kernelContext_0->giProbes_0->constants_0[4U].xyz)));

    float _S3 = giNormalisedOr_0(direction_0, float3(0.0, 1.0, 0.0)).y;

#line 126
    if(_S3 < 0.0)
    {

#line 127
        return ground_0 * float3(zenith_0.w) ;
    }

    return (horizon_0 + (zenith_0.xyz - horizon_0) * float3(gpClamp_0(pow(_S3, 0.44999998807907104), 0.0, 1.0)) ) * float3(zenith_0.w) ;
}


#line 281
struct GiWorldHit_0
{
    bool hit_0;
    float t_0;
    float3 position_0;
    float3 normal_0;
    float confidence_0;
};


#line 211 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiTraceHit_0
{
    bool hit_1;
    bool exhausted_0;
    float t_1;
    float closest_0;
    float3 position_1;
    float3 normal_1;
};


#line 111
uint giLevelCount_0(KernelContext_0 thread* kernelContext_1)
{

#line 112
    return kernelContext_1->giScene_0->constants_1[0U].x;
}


#line 119
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_2)
{

#line 120
    return (as_type<float>((kernelContext_2->giScene_0->constants_1[9U].y)));
}


#line 115
uint giWindowBricks_0(KernelContext_0 thread* kernelContext_3)
{

#line 116
    return kernelContext_3->giScene_0->constants_1[0U].y;
}


#line 163
int giWrap_0(int value_1, int modulus_0)
{

#line 164
    uint _S4 = uint(modulus_0);
    uint _S5 = (uint(value_1) + (_S4 << 20U)) % _S4;

#line 165
    return int(_S5);
}


#line 91
float giLerp_0(float a_0, float b_0, float t_2)
{

#line 92
    return a_0 + (b_0 - a_0) * t_2;
}


#line 124
float giTrilinear_0(uint slot_0, float fx_0, float fy_0, float fz_0, KernelContext_0 thread* kernelContext_4)
{
    int x0_0 = clamp(int(floor(fx_0)), int(0), int(3));
    int y0_0 = clamp(int(floor(fy_0)), int(0), int(3));
    int z0_0 = clamp(int(floor(fz_0)), int(0), int(3));



    float tx_0 = clamp(fx_0 - float(x0_0), 0.0, 1.0);
    float ty_0 = clamp(fy_0 - float(y0_0), 0.0, 1.0);

    uint base_0 = slot_0 * 64U;

    uint _S6 = uint(z0_0) * 4U;

#line 137
    uint _S7 = uint(y0_0);

#line 137
    uint _S8 = base_0 + (_S6 + _S7) * 4U;

#line 137
    uint _S9 = uint(x0_0);
    uint _S10 = uint(clamp(x0_0 + int(1), int(0), int(3)));
    uint _S11 = uint(clamp(y0_0 + int(1), int(0), int(3)));

#line 139
    uint _S12 = base_0 + (_S6 + _S11) * 4U;

    uint _S13 = uint(clamp(z0_0 + int(1), int(0), int(3))) * 4U;

#line 141
    uint _S14 = base_0 + (_S13 + _S7) * 4U;

    uint _S15 = base_0 + (_S13 + _S11) * 4U;

#line 149
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_4->giScene_0->bricks_0[_S8 + _S9], kernelContext_4->giScene_0->bricks_0[_S8 + _S10], tx_0), giLerp_0(kernelContext_4->giScene_0->bricks_0[_S12 + _S9], kernelContext_4->giScene_0->bricks_0[_S12 + _S10], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_4->giScene_0->bricks_0[_S14 + _S9], kernelContext_4->giScene_0->bricks_0[_S14 + _S10], tx_0), giLerp_0(kernelContext_4->giScene_0->bricks_0[_S15 + _S9], kernelContext_4->giScene_0->bricks_0[_S15 + _S10], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 171
float giSampleLevel_0(uint level_0, float3 p_0, float thread* farDistance_0, KernelContext_0 thread* kernelContext_5)
{

#line 172
    uint _S16 = 16U + level_0 * 2U;

#line 172
    float4 shape_0 = (as_type<float4>((kernelContext_5->giScene_0->constants_1[_S16])));
    int4 origin_0 = (as_type<int4>((kernelContext_5->giScene_0->constants_1[_S16 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_0.x / brick_0));
    int by_0 = int(floor(p_0.y / brick_0));
    int bz_0 = int(floor(p_0.z / brick_0));

#line 179
    uint _S17 = giWindowBricks_0(kernelContext_5);
    int dim_0 = int(_S17);
    int _S18 = origin_0.x;

#line 181
    bool _S19;

)cy_msl"
    R"cy_msl(#line 181
    if(bx_0 < _S18)
    {

#line 181
        _S19 = true;

#line 181
    }
    else
    {

#line 181
        _S19 = by_0 < (origin_0.y);

#line 181
    }

#line 181
    if(_S19)
    {

#line 181
        _S19 = true;

#line 181
    }
    else
    {

#line 181
        _S19 = bz_0 < (origin_0.z);

#line 181
    }

#line 181
    if(_S19)
    {

#line 181
        _S19 = true;

#line 181
    }
    else
    {

#line 181
        _S19 = bx_0 >= (_S18 + dim_0);

#line 181
    }

#line 181
    if(_S19)
    {

#line 181
        _S19 = true;

#line 181
    }
    else
    {

#line 181
        _S19 = by_0 >= (origin_0.y + dim_0);

#line 181
    }
    if(_S19)
    {

#line 182
        _S19 = true;

#line 182
    }
    else
    {

#line 182
        _S19 = bz_0 >= (origin_0.z + dim_0);

#line 182
    }

#line 181
    if(_S19)
    {
        return *farDistance_0;
    }
    int _S20 = giWrap_0(bz_0, dim_0);

#line 185
    uint _S21 = uint(dim_0);

#line 185
    uint _S22 = uint(_S20) * _S21;

#line 185
    int _S23 = giWrap_0(by_0, dim_0);

#line 185
    uint _S24 = (_S22 + uint(_S23)) * _S21;
    int _S25 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_5->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S24 + uint(_S25))];
    if(slot_1 == 4294967295U)
    {

#line 189
        return *farDistance_0;
    }

    float3 local_0 = (p_0 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 192
    float _S26 = giTrilinear_0(slot_1, local_0.x, local_0.y, local_0.z, kernelContext_5);
    return _S26;
}


float giDistance_0(float3 p_1, KernelContext_0 thread* kernelContext_6)
{

#line 197
    uint _S27 = giLevelCount_0(kernelContext_6);

#line 197
    float answer_0 = 1.0e+06;

#line 197
    uint level_1 = 0U;


    for(;;)
    {

#line 200
        if(level_1 < _S27)
        {
        }
        else
        {

#line 200
            break;
        }

#line 201
        thread float farDistance_1 = 0.0;

#line 201
        float _S28 = giSampleLevel_0(level_1, p_1, &farDistance_1, kernelContext_6);

        float _S29 = min(answer_0, _S28);
        if(_S28 < farDistance_1)
        {

#line 205
            return _S28;
        }

#line 200
        uint level_2 = level_1 + 1U;

#line 200
        answer_0 = _S29;

#line 200
        level_1 = level_2;

#line 200
    }

#line 208
    return answer_0;
}


#line 221
GiTraceHit_0 giSphereTrace_0(float3 origin_1, float3 direction_1, float maxDistance_0, float tMin_0, KernelContext_0 thread* kernelContext_7)
{

#line 222
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_1 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_1 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_1 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_1 = float3(0.0, 1.0, 0.0);

#line 228
    uint _S30 = giLevelCount_0(kernelContext_7);
    if(_S30 == 0U)
    {

#line 230
        return result_0;
    }

#line 230
    float _S31 = giFinestVoxel_0(kernelContext_7);


    float surface_0 = _S31 * 0.5;
    float _S32 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 235
    recent_0[int(0)] = 1.0e+06;

#line 235
    recent_0[int(1)] = 1.0e+06;

#line 235
    recent_0[int(2)] = 1.0e+06;

#line 235
    float closest_1 = 1.0e+06;

#line 235
    uint recentSlot_0 = 0U;

#line 235
    uint step_0 = 0U;

#line 235
    float t_3 = _S32;


    for(;;)
    {

#line 238
        if(step_0 < 192U)
        {
        }
        else
        {

#line 238
            break;
        }

#line 239
        if(t_3 > maxDistance_0)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_2 = origin_1 + direction_1 * float3(t_3) ;

#line 244
        float _S33 = giDistance_0(position_2, kernelContext_7);

        if(_S33 < surface_0)
        {

#line 247
            (&result_0)->hit_1 = true;
            (&result_0)->t_1 = t_3;
            (&result_0)->position_1 = position_2;


            float3 _S34 = float3(_S31, 0.0, 0.0);

#line 252
            float _S35 = giDistance_0(position_2 + _S34, kernelContext_7);

#line 252
            float _S36 = giDistance_0(position_2 - _S34, kernelContext_7);

#line 252
            float _S37 = _S35 - _S36;

            float3 _S38 = float3(0.0, _S31, 0.0);

#line 254
            float _S39 = giDistance_0(position_2 + _S38, kernelContext_7);

#line 254
            float _S40 = giDistance_0(position_2 - _S38, kernelContext_7);

#line 254
            float _S41 = _S39 - _S40;

            float3 _S42 = float3(0.0, 0.0, _S31);

#line 256
            float _S43 = giDistance_0(position_2 + _S42, kernelContext_7);

#line 256
            float _S44 = giDistance_0(position_2 - _S42, kernelContext_7);

            (&result_0)->normal_1 = giNormalisedOr_0(float3(_S37, _S41, _S43 - _S44), - direction_1);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S45 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S33;
        uint _S46 = (recentSlot_0 + 1U) % 3U;
        float t_4 = t_3 + max(_S33, surface_0);

)cy_msl"
    R"cy_msl(#line 238
        uint step_1 = step_0 + 1U;

#line 238
        closest_1 = _S45;

#line 238
        recentSlot_0 = _S46;

#line 238
        step_0 = step_1;

#line 238
        t_3 = t_4;

#line 238
    }

#line 267
    (&result_0)->exhausted_0 = true;

    (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
    return result_0;
}


#line 290 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiWorldHit_0 gpMiss_0()
{

#line 291
    thread GiWorldHit_0 miss_0;
    (&miss_0)->hit_0 = false;
    (&miss_0)->t_0 = 0.0;
    (&miss_0)->position_0 = float3(0.0, 0.0, 0.0);
    (&miss_0)->normal_0 = float3(0.0, 1.0, 0.0);
    (&miss_0)->confidence_0 = 1.0;
    return miss_0;
}


#line 309
GiWorldHit_0 GiSoftwareTier_trace_0(float3 origin_2, float3 direction_2, float maxDistance_1, KernelContext_0 thread* kernelContext_8)
{

#line 309
    GiTraceHit_0 _S47 = giSphereTrace_0(origin_2, direction_2, maxDistance_1, 0.0, kernelContext_8);

    if(!_S47.hit_1)
    {

#line 312
        return gpMiss_0();
    }
    thread GiWorldHit_0 hit_2;
    (&hit_2)->hit_0 = true;
    (&hit_2)->t_0 = _S47.t_1;
    (&hit_2)->position_0 = _S47.position_1;
    (&hit_2)->normal_0 = _S47.normal_1;



    (&hit_2)->confidence_0 = (as_type<float4>((kernelContext_8->giProbes_0->constants_0[25U]))).y * gpClamp_0(_S47.closest_0 / max((as_type<float>((kernelContext_8->giScene_0->constants_1[9U].z))), 0.00009999999747379), 0.25, 1.0);
    return hit_2;
}


#line 399 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_2, float cell_0)
{

#line 400
    return int(floor(value_2 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_3, float3 normal_2, KernelContext_0 thread* kernelContext_9)
{

#line 410
    uint4 grid_0 = kernelContext_9->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 413
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S48 = radius_0 * radius_0;
    float _S49 = position_3.x;

#line 418
    int _S50 = giCell_0(_S49 - radius_0, cell_1);

#line 418
    float _S51 = position_3.y;

#line 418
    int _S52 = giCell_0(_S51 - radius_0, cell_1);
    float _S53 = position_3.z;

#line 419
    int _S54 = giCell_0(_S53 - radius_0, cell_1);
    int _S55 = giCell_0(_S49 + radius_0, cell_1);

#line 420
    int _S56 = giCell_0(_S51 + radius_0, cell_1);
    int _S57 = giCell_0(_S53 + radius_0, cell_1);

#line 421
    float bestScore_0 = 1.0e+09;

#line 421
    float bestAlignment_0 = 0.0;

#line 421
    uint best_0 = 4294967295U;

#line 421
    int z_1 = _S54;



    for(;;)
    {

#line 425
        if(z_1 <= _S57)
        {
        }
        else
        {

#line 425
            break;
        }

#line 425
        int y_1 = _S52;
        for(;;)
        {

#line 426
            if(y_1 <= _S56)
            {
            }
            else
            {

#line 426
                break;
            }

#line 426
            float bestScore_1 = bestScore_0;

#line 426
            float bestAlignment_1 = bestAlignment_0;

#line 426
            uint best_1 = best_0;

#line 426
            int x_1 = _S50;
            for(;;)
            {

#line 427
                if(x_1 <= _S55)
                {
                }
                else
                {

#line 427
                    break;
                }
                uint _S58 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 429
                uint first_0 = kernelContext_9->giScene_0->gridRanges_0[_S58];
                uint _S59 = kernelContext_9->giScene_0->gridRanges_0[_S58 + 1U];

#line 430
                float bestScore_2 = bestScore_1;

#line 430
                float bestAlignment_2 = bestAlignment_1;

#line 430
                uint best_2 = best_1;

#line 430
                uint item_0 = first_0;
                for(;;)
                {

#line 431
                    if(item_0 < (first_0 + _S59))
                    {
                    }
                    else
                    {

#line 431
                        break;
                    }

#line 432
                    uint handle_0 = kernelContext_9->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_9->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 434
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }



                    uint _S60 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_9->giScene_0->cards_0[_S60 + 1U].xyz, normal_2);
                    if(alignment_0 <= 0.25)
                    {

#line 439
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }

#line 441
                    float3 offset_0 = kernelContext_9->giScene_0->cards_0[_S60].xyz - position_3;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S48)
                    {

#line 444
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }

#line 447
                    float score_0 = squared_1 / alignment_0;

#line 447
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 448
                        better_0 = score_0 < bestScore_2;

#line 448
                    }
                    else
                    {

#line 449
                        if(alignment_0 != bestAlignment_2)
                        {

#line 449
                            better_0 = alignment_0 > bestAlignment_2;

#line 449
                        }
                        else
                        {

#line 449
                            better_0 = handle_0 < best_2;

#line 449
                        }

#line 448
                    }

#line 448
                    float bestScore_3;

#line 448
                    float bestAlignment_3;

#line 448
                    uint best_3;


)cy_msl"
    R"cy_msl(                    if(better_0)
                    {

#line 451
                        bestScore_3 = score_0;

#line 451
                        bestAlignment_3 = alignment_0;

#line 451
                        best_3 = handle_0;

#line 451
                    }
                    else
                    {

#line 451
                        bestScore_3 = bestScore_2;

#line 451
                        bestAlignment_3 = bestAlignment_2;

#line 451
                        best_3 = best_2;

#line 451
                    }

#line 451
                    bestScore_2 = bestScore_3;

#line 451
                    bestAlignment_2 = bestAlignment_3;

#line 451
                    best_2 = best_3;

#line 431
                    item_0 = item_0 + 1U;

#line 431
                }

#line 427
                int x_2 = x_1 + int(1);

#line 427
                bestScore_1 = bestScore_2;

#line 427
                bestAlignment_1 = bestAlignment_2;

#line 427
                best_1 = best_2;

#line 427
                x_1 = x_2;

#line 427
            }

#line 426
            int y_2 = y_1 + int(1);

#line 426
            bestScore_0 = bestScore_1;

#line 426
            bestAlignment_0 = bestAlignment_1;

#line 426
            best_0 = best_1;

#line 426
            y_1 = y_2;

#line 426
        }

#line 425
        z_1 = z_1 + int(1);

#line 425
    }

#line 460
    return best_0;
}


#line 273 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
void gpLoadPayload_0(uint probe_0, uint floats_0, array<float, int(48)> thread* payload_0, KernelContext_0 thread* kernelContext_10)
{

#line 273
    uint index_0 = 0U;
    for(;;)
    {

#line 274
        if(index_0 < 48U)
        {
        }
        else
        {

#line 274
            break;
        }

#line 274
        float _S61;
        if(index_0 < floats_0)
        {

#line 275
            _S61 = *(kernelContext_10->giProbes_0->payloads_0+(probe_0 * floats_0 + index_0));

#line 275
        }
        else
        {

#line 275
            _S61 = 0.0;

#line 275
        }

#line 275
        (*payload_0)[index_0] = _S61;

#line 274
        index_0 = index_0 + 1U;

#line 274
    }


    return;
}


#line 114
constant array<float3, int(6)> kGpAxes_0 = { float3(1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, -1.0, 0.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, -1.0) };

#line 110
float3 gpMax0_0(float3 v_1)
{

#line 111
    return float3(max(0.0, v_1.x), max(0.0, v_1.y), max(0.0, v_1.z));
}


#line 154
void gpOctahedralUv_0(float3 direction_3, float thread* u_0, float thread* v_2)
{

#line 155
    float sum_0 = abs(direction_3.x) + abs(direction_3.y) + abs(direction_3.z);

#line 155
    float3 p_2;
    if(sum_0 > 0.0)
    {

#line 156
        p_2 = direction_3 / float3(sum_0) ;

#line 156
    }
    else
    {

#line 156
        p_2 = float3(0.0, 0.0, 1.0);

#line 156
    }
    float _S62 = p_2.x;
    float _S63 = p_2.y;

#line 158
    float px_0;

#line 158
    float py_0;
    if((p_2.z) < 0.0)
    {

#line 160
        float _S64 = 1.0 - abs(_S63);

#line 160
        if(_S62 >= 0.0)
        {

#line 160
            px_0 = 1.0;

#line 160
        }
        else
        {

#line 160
            px_0 = -1.0;

#line 160
        }

#line 160
        float fx_1 = _S64 * px_0;
        float _S65 = 1.0 - abs(_S62);

#line 161
        if(_S63 >= 0.0)
        {

#line 161
            px_0 = 1.0;

#line 161
        }
        else
        {

#line 161
            px_0 = -1.0;

#line 161
        }

#line 161
        float fy_1 = _S65 * px_0;

#line 161
        px_0 = fx_1;

#line 161
        py_0 = fy_1;

#line 159
    }
    else
    {

#line 159
        px_0 = _S62;

#line 159
        py_0 = _S63;

#line 159
    }

#line 165
    *u_0 = gpClamp_0(px_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    *v_2 = gpClamp_0(py_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    return;
}


#line 220
float3 gpDecode_0(uint encoding_0, const array<float, int(48)> thread* payload_1, float3 direction_4)
{

#line 220
    uint c_0;
    if(encoding_0 == 0U)
    {



        array<float, int(4)> _S66 = { 0.28209498524665833, 0.32573533058166504 * direction_4.y, 0.32573533058166504 * direction_4.z, 0.32573533058166504 * direction_4.x };

        thread float3 result_1 = float3(0.0, 0.0, 0.0);

#line 228
        c_0 = 0U;
        for(;;)
        {

#line 229
            if(c_0 < 4U)
            {
            }
            else
            {

#line 229
                break;
            }

#line 230
            uint _S67 = c_0 * 3U;

#line 230
            result_1.x = result_1.x + (*payload_1)[_S67] * _S66[c_0];
            result_1.y = result_1.y + (*payload_1)[_S67 + 1U] * _S66[c_0];
            result_1.z = result_1.z + (*payload_1)[_S67 + 2U] * _S66[c_0];

)cy_msl"
    R"cy_msl(#line 229
            c_0 = c_0 + 1U;

#line 229
        }

#line 234
        return gpMax0_0(result_1);
    }
    if(encoding_0 == 1U)
    {

#line 237
        thread float u_1 = 0.0;
        thread float v_3 = 0.0;
        gpOctahedralUv_0(direction_4, &u_1, &v_3);
        uint x0_1 = uint(u_1);
        uint y0_1 = uint(v_3);
        uint _S68 = min(x0_1 + 1U, 3U);



        uint _S69 = y0_1 * 4U;

        uint _S70 = min(y0_1 + 1U, 3U) * 4U;

        uint _S71 = (_S69 + x0_1) * 3U;

#line 250
        float3 f00_0 = float3((*payload_1)[_S71], (*payload_1)[_S71 + 1U], (*payload_1)[_S71 + 2U]);
        uint _S72 = (_S69 + _S68) * 3U;
        uint _S73 = (_S70 + x0_1) * 3U;

#line 252
        float3 f01_0 = float3((*payload_1)[_S73], (*payload_1)[_S73 + 1U], (*payload_1)[_S73 + 2U]);
        uint _S74 = (_S70 + _S68) * 3U;

#line 253
        float3 _S75 = float3((u_1 - float(x0_1))) ;
        float3 top_0 = f00_0 + (float3((*payload_1)[_S72], (*payload_1)[_S72 + 1U], (*payload_1)[_S72 + 2U]) - f00_0) * _S75;

        return top_0 + (f01_0 + (float3((*payload_1)[_S74], (*payload_1)[_S74 + 1U], (*payload_1)[_S74 + 2U]) - f01_0) * _S75 - top_0) * float3((v_3 - float(y0_1))) ;
    }
    float3 _S76 = float3(0.0, 0.0, 0.0);

#line 258
    float3 result_2 = _S76;

#line 258
    c_0 = 0U;

#line 258
    float total_0 = 0.0;

    for(;;)
    {

#line 260
        if(c_0 < 6U)
        {
        }
        else
        {

#line 260
            break;
        }

#line 261
        float _S77 = max(0.0, dot(kGpAxes_0[c_0], direction_4));
        if(_S77 <= 0.0)
        {

#line 263
            c_0 = c_0 + 1U;

#line 260
            continue;
        }



        uint _S78 = c_0 * 3U;

        float total_1 = total_0 + _S77;

#line 267
        result_2 = result_2 + float3((*payload_1)[_S78], (*payload_1)[_S78 + 1U], (*payload_1)[_S78 + 2U]) * float3(_S77) ;

#line 267
        total_0 = total_1;

#line 260
        c_0 = c_0 + 1U;

#line 260
    }

#line 269
    if(total_0 > 0.0)
    {

#line 269
        result_2 = result_2 / float3(total_0) ;

#line 269
    }
    else
    {

#line 269
        result_2 = _S76;

#line 269
    }

#line 269
    return result_2;
}


#line 138
float3 gpOctahedralDirection_0(uint texel_0)
{

    float u_2 = float(texel_0 % 4U) / 3.0 * 2.0 - 1.0;
    float v_4 = float(texel_0 / 4U) / 3.0 * 2.0 - 1.0;
    float _S79 = 1.0 - abs(u_2);

#line 143
    float _S80 = abs(v_4);

#line 143
    float _S81 = _S79 - _S80;

#line 143
    thread float3 direction_5 = float3(u_2, v_4, _S81);
    if(_S81 < 0.0)
    {

#line 145
        float _S82 = 1.0 - _S80;

#line 145
        float _S83;

#line 145
        if(u_2 >= 0.0)
        {

#line 145
            _S83 = 1.0;

#line 145
        }
        else
        {

#line 145
            _S83 = -1.0;

#line 145
        }

#line 145
        float fx_2 = _S82 * _S83;
        if(v_4 >= 0.0)
        {

#line 146
            _S83 = 1.0;

#line 146
        }
        else
        {

#line 146
            _S83 = -1.0;

#line 146
        }

#line 146
        float fy_2 = _S79 * _S83;
        direction_5.x = fx_2;
        direction_5.y = fy_2;

#line 144
    }

#line 150
    return giNormalisedOr_0(direction_5, float3(0.0, 0.0, 1.0));
}


#line 169
void gpEncode_0(uint encoding_1, array<float, int(48)> thread* payload_2, float3 direction_6, float3 radiance_0)
{

#line 169
    uint c_1;
    if(encoding_1 == 0U)
    {

        array<float, int(4)> _S84 = { 0.28209498524665833, 0.48860299587249756 * direction_6.y, 0.48860299587249756 * direction_6.z, 0.48860299587249756 * direction_6.x };

#line 173
        c_1 = 0U;
        for(;;)
        {

#line 174
            if(c_1 < 4U)
            {
            }
            else
            {

#line 174
                break;
            }
            uint _S85 = c_1 * 3U;

#line 176
            (*payload_2)[_S85] = (*payload_2)[_S85] + radiance_0.x * _S84[c_1];
            (*payload_2)[_S85 + 1U] = (*payload_2)[_S85 + 1U] + radiance_0.y * _S84[c_1];
            (*payload_2)[_S85 + 2U] = (*payload_2)[_S85 + 2U] + radiance_0.z * _S84[c_1];

#line 174
            c_1 = c_1 + 1U;

#line 174
        }

#line 180
        return;
    }
    if(encoding_1 == 1U)
    {

#line 182
        c_1 = 0U;
        for(;;)
        {

#line 183
            if(c_1 < 16U)
            {
            }
            else
            {

#line 183
                break;
            }

#line 184
            float _S86 = max(0.0, dot(gpOctahedralDirection_0(c_1), direction_6));
            if(_S86 <= 0.0)
            {

#line 186
                c_1 = c_1 + 1U;

#line 183
                continue;
            }

#line 189
            uint _S87 = c_1 * 3U;

#line 189
            (*payload_2)[_S87] = (*payload_2)[_S87] + radiance_0.x * _S86;
            (*payload_2)[_S87 + 1U] = (*payload_2)[_S87 + 1U] + radiance_0.y * _S86;
            (*payload_2)[_S87 + 2U] = (*payload_2)[_S87 + 2U] + radiance_0.z * _S86;

#line 183
            c_1 = c_1 + 1U;

#line 183
        }

#line 193
        return;
    }

#line 193
    c_1 = 0U;

    for(;;)
    {

#line 195
        if(c_1 < 6U)
        {
        }
        else
        {

#line 195
            break;
        }

)cy_msl"
    R"cy_msl(#line 196
        float _S88 = max(0.0, dot(kGpAxes_0[c_1], direction_6));
        if(_S88 <= 0.0)
        {

#line 198
            c_1 = c_1 + 1U;

#line 195
            continue;
        }

#line 201
        uint _S89 = c_1 * 3U;

#line 201
        (*payload_2)[_S89] = (*payload_2)[_S89] + radiance_0.x * _S88;
        (*payload_2)[_S89 + 1U] = (*payload_2)[_S89 + 1U] + radiance_0.y * _S88;
        (*payload_2)[_S89 + 2U] = (*payload_2)[_S89 + 2U] + radiance_0.z * _S88;

#line 195
        c_1 = c_1 + 1U;

#line 195
    }

#line 205
    return;
}


#line 207
void gpNormalise_0(uint encoding_2, uint floats_1, array<float, int(48)> thread* payload_3, float totalWeight_0)
{

#line 208
    if(totalWeight_0 <= 0.0)
    {

#line 209
        return;
    }

#line 209
    float _S90;


    if(encoding_2 == 0U)
    {

#line 212
        _S90 = 12.56637096405029297 / totalWeight_0;

#line 212
    }
    else
    {

#line 212
        _S90 = 3.14159274101257324 / totalWeight_0;

#line 212
    }

#line 212
    uint index_1 = 0U;

    for(;;)
    {

#line 214
        if(index_1 < floats_1)
        {
        }
        else
        {

#line 214
            break;
        }

#line 215
        (*payload_3)[index_1] = (*payload_3)[index_1] * _S90;

#line 214
        index_1 = index_1 + 1U;

#line 214
    }


    return;
}


#line 349
void gpGatherProbe_0(uint index_2, uint lane_0, KernelContext_0 thread* kernelContext_11)
{

#line 350
    uint4 counts_0 = kernelContext_11->giProbes_0->constants_0[0U];
    if(index_2 >= (counts_0.w))
    {

#line 352
        return;
    }
    uint encoding_3 = counts_0.x;
    uint floats_2 = counts_0.y;
    uint _S91 = min(max(1U, counts_0.z), 64U);
    float maxDistance_2 = (as_type<float>((kernelContext_11->giProbes_0->constants_0[1U].x)));
    uint probe_1 = kernelContext_11->giProbes_0->selection_0[index_2];
    uint _S92 = probe_1 * 4U;

#line 359
    float3 position_4 = (*(kernelContext_11->giProbes_0->probes_0+_S92)).xyz;

#line 359
    float3 previous_0;

#line 359
    float totalWeight_1;

    if(lane_0 < _S91)
    {

#line 362
        uint _S93 = lane_0 * 2U;
        float3 unit_0 = kernelContext_11->giProbes_0->directions_0[_S93 + 1U].xyz;

#line 363
        float3 _S94 = gpSky_0(kernelContext_11->giProbes_0->directions_0[_S93].xyz, kernelContext_11);

#line 363
        GiWorldHit_0 _S95 = GiSoftwareTier_trace_0(position_4, unit_0, maxDistance_2, kernelContext_11);



        if(_S95.hit_0)
        {

#line 367
            uint _S96 = giFindCard_0(_S95.position_0, _S95.normal_0, kernelContext_11);



            if(_S96 != 4294967295U)
            {

#line 371
                previous_0 = (*(kernelContext_11->giScene_0->cardState_0+_S96 * 2U)).xyz;

#line 371
            }
            else
            {

#line 371
                previous_0 = float3(0.0, 0.0, 0.0);

#line 371
            }

#line 371
            totalWeight_1 = _S95.t_0;

#line 367
        }
        else
        {

#line 367
            previous_0 = _S94;

#line 367
            totalWeight_1 = maxDistance_2;

#line 367
        }

#line 374
        (*kernelContext_11->gpRayRadiance_0)[lane_0] = previous_0;
        (*kernelContext_11->gpRayDistance_0)[lane_0] = totalWeight_1;

#line 361
    }

#line 377
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if(lane_0 != 0U)
    {

#line 379
        return;
    }

    thread array<float, int(48)> data_0;

#line 382
    gpLoadPayload_0(probe_1, floats_2, &data_0, kernelContext_11);

    float3 _S97 = float3(0.0, 0.0, 0.0);

#line 384
    previous_0 = _S97;

#line 384
    uint axis_0 = 0U;
    for(;;)
    {

#line 385
        if(axis_0 < 6U)
        {
        }
        else
        {

#line 385
            break;
        }

#line 385
        thread array<float, int(48)> _S98 = data_0;

#line 385
        float3 _S99 = gpDecode_0(encoding_3, &_S98, kGpAxes_0[axis_0]);
        float3 _S100 = previous_0 + _S99;

#line 385
        uint axis_1 = axis_0 + 1U;

#line 385
        previous_0 = _S100;

#line 385
        axis_0 = axis_1;

#line 385
    }

#line 385
    uint i_0 = 0U;


    for(;;)
    {

#line 388
        if(i_0 < 48U)
        {
        }
        else
        {

#line 388
            break;
        }

#line 389
        data_0[i_0] = 0.0;

#line 388
        i_0 = i_0 + 1U;

#line 388
    }


    thread array<float, int(6)> axisDistance_0;
    thread array<float, int(6)> axisWeight_0;

#line 392
    uint a_1 = 0U;
    for(;;)
    {

#line 393
        if(a_1 < 6U)
        {
        }
        else
        {

#line 393
            break;
        }

#line 394
        axisDistance_0[a_1] = 0.0;
        axisWeight_0[a_1] = 0.0;

#line 393
        a_1 = a_1 + 1U;

#line 393
    }

#line 393
    uint ray_0 = 0U;

)cy_msl"
    R"cy_msl(#line 393
    totalWeight_1 = 0.0;

#line 399
    for(;;)
    {

#line 399
        if(ray_0 < _S91)
        {
        }
        else
        {

#line 399
            break;
        }

#line 400
        float3 direction_7 = kernelContext_11->giProbes_0->directions_0[ray_0 * 2U].xyz;
        gpEncode_0(encoding_3, &data_0, direction_7, (*kernelContext_11->gpRayRadiance_0)[ray_0]);
        float totalWeight_2 = totalWeight_1 + 1.0;

#line 402
        a_1 = 0U;
        for(;;)
        {

#line 403
            if(a_1 < 6U)
            {
            }
            else
            {

#line 403
                break;
            }

#line 404
            float _S101 = max(0.0, dot(kGpAxes_0[a_1], direction_7));
            axisDistance_0[a_1] = axisDistance_0[a_1] + (*kernelContext_11->gpRayDistance_0)[ray_0] * _S101;
            axisWeight_0[a_1] = axisWeight_0[a_1] + _S101;

#line 403
            a_1 = a_1 + 1U;

#line 403
        }

#line 399
        ray_0 = ray_0 + 1U;

#line 399
        totalWeight_1 = totalWeight_2;

#line 399
    }

#line 409
    gpNormalise_0(encoding_3, floats_2, &data_0, totalWeight_1);

#line 409
    float3 current_0 = _S97;

#line 409
    axis_0 = 0U;


    for(;;)
    {

#line 412
        if(axis_0 < 6U)
        {
        }
        else
        {

#line 412
            break;
        }

#line 412
        thread array<float, int(48)> _S102 = data_0;

#line 412
        float3 _S103 = gpDecode_0(encoding_3, &_S102, kGpAxes_0[axis_0]);
        float3 _S104 = current_0 + _S103;

#line 412
        uint axis_2 = axis_0 + 1U;

#line 412
        current_0 = _S104;

#line 412
        axis_0 = axis_2;

#line 412
    }


    float magnitude_0 = length(previous_0) + length(current_0);

#line 415
    float error_0;
    if(magnitude_0 > 0.00000999999974738)
    {

#line 416
        error_0 = length(current_0 - previous_0) / magnitude_0;

#line 416
    }
    else
    {

#line 416
        error_0 = 0.0;

#line 416
    }

    uint record_0 = index_2 * 14U;

#line 418
    uint v_5 = 0U;
    for(;;)
    {

#line 419
        if(v_5 < 12U)
        {
        }
        else
        {

#line 419
            break;
        }
        uint _S105 = v_5 * 4U;

#line 420
        *(kernelContext_11->giProbes_0->results_0+(record_0 + v_5)) = float4(data_0[_S105], data_0[_S105 + 1U], data_0[_S105 + 2U], data_0[_S105 + 3U]);

#line 419
        v_5 = v_5 + 1U;

#line 419
    }



    thread array<float, int(6)> distances_0;

#line 423
    a_1 = 0U;
    for(;;)
    {

#line 424
        if(a_1 < 6U)
        {
        }
        else
        {

#line 424
            break;
        }

#line 425
        if((axisWeight_0[a_1]) > 0.0)
        {

#line 425
            totalWeight_1 = axisDistance_0[a_1] / axisWeight_0[a_1];

#line 425
        }
        else
        {

#line 425
            totalWeight_1 = maxDistance_2;

#line 425
        }

#line 425
        distances_0[a_1] = totalWeight_1;

#line 424
        a_1 = a_1 + 1U;

#line 424
    }


    *(kernelContext_11->giProbes_0->results_0+(record_0 + 12U)) = float4(distances_0[int(0)], distances_0[int(1)], distances_0[int(2)], distances_0[int(3)]);
    *(kernelContext_11->giProbes_0->results_0+(record_0 + 13U)) = float4(distances_0[int(4)], distances_0[int(5)], error_0, (as_type<float>((probe_1))));

#line 428
    uint f_0 = 0U;
    for(;;)
    {

#line 429
        if(f_0 < floats_2)
        {
        }
        else
        {

#line 429
            break;
        }

#line 430
        *(kernelContext_11->giProbes_0->payloads_0+(probe_1 * floats_2 + f_0)) = data_0[f_0];

#line 429
        f_0 = f_0 + 1U;

#line 429
    }


    *(kernelContext_11->giProbes_0->probes_0+(_S92 + 1U)) = float4((as_type<float>((kernelContext_11->giDispatch_0->frame_0))), 1.0, 1.0, 0.0);
    *(kernelContext_11->giProbes_0->probes_0+(_S92 + 2U)) = float4(distances_0[int(0)], distances_0[int(1)], distances_0[int(2)], distances_0[int(3)]);

    *(kernelContext_11->giProbes_0->probes_0+(_S92 + 3U)) = float4(distances_0[int(4)], distances_0[int(5)], 0.0, 0.0);
    return;
}


#line 18 "src/rendering/gi_gpu/shaders/gi_radiance.slang"
[[kernel]] void cyGiGatherProbes(uint3 group_0 [[threadgroup_position_in_grid]], uint3 lane_1 [[thread_position_in_threadgroup]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiDispatch_0 constant* giDispatch_1 [[buffer(1)]])
{

#line 18
    thread KernelContext_0 kernelContext_12;

#line 18
    (&kernelContext_12)->giProbes_0 = giProbes_1;

#line 18
    (&kernelContext_12)->giScene_0 = giScene_1;

#line 18
    (&kernelContext_12)->giDispatch_0 = giDispatch_1;

#line 18
    threadgroup array<float3, int(64)> gpRayRadiance_1;

#line 18
    (&kernelContext_12)->gpRayRadiance_0 = &gpRayRadiance_1;

#line 18
    threadgroup array<float, int(64)> gpRayDistance_1;

#line 18
    (&kernelContext_12)->gpRayDistance_0 = &gpRayDistance_1;

#line 18
    gpGatherProbe_0(group_0.x, lane_1.x, &kernelContext_12);

    return;
}

)cy_msl";

/// cyGiSampleProbes.metal, 17393 bytes.
inline constexpr char kGiSampleProbesMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 114 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
constant array<float3, int(6)> kGpAxes_0 = { float3(1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, -1.0, 0.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, -1.0) };

#line 440
int gpFloorDiv_0(float value_0, float divisor_0)
{

#line 441
    return int(floor(value_0 / divisor_0));
}


#line 163 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giWrap_0(int value_1, int modulus_0)
{

#line 164
    uint _S1 = uint(modulus_0);
    uint _S2 = (uint(value_1) + (_S1 << 20U)) % _S1;

#line 165
    return int(_S2);
}


#line 77
struct GiDispatch_0
{
    uint count_0;
    uint frame_0;
    uint reserved0_0;
    uint reserved1_0;
};


#line 67 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiProbeSet_default_0
{
    uint4 device* constants_0;
    float4 device* probes_0;
    float device* payloads_0;
    uint device* selection_0;
    float4 device* directions_0;
    float4 device* results_0;
    uint device* layout_0;
    float4 device* queries_0;
    float4 device* samples_0;
    float4 device* tierRays_0;
    float4 device* tierResults_0;
    float device* screenDepth_0;
    float4 device* screenColour_0;
    float device* screenHzb_0;
    uint device* counters_0;
};


#line 67
struct KernelContext_0
{
    GiDispatch_0 constant* giDispatch_0;
    GiProbeSet_default_0 constant* giProbes_0;
};


#line 476
uint gpLayoutProbe_0(uint level_0, int3 cell_0, KernelContext_0 thread* kernelContext_0)
{

#line 477
    int3 centre_0 = (as_type<int3>((kernelContext_0->giProbes_0->constants_0[6U + level_0].xyz)));
    int half_0 = int(kernelContext_0->giProbes_0->constants_0[1U].w);
    int3 _S3 = int3(half_0, half_0, half_0);

#line 479
    bool _S4;

#line 479
    if(any(cell_0 < (centre_0 - _S3)))
    {

#line 479
        _S4 = true;

#line 479
    }
    else
    {

#line 479
        _S4 = any(cell_0 > (centre_0 + _S3));

#line 479
    }

#line 479
    if(_S4)
    {

#line 480
        return 4294967295U;
    }
    int edge_0 = int(kernelContext_0->giProbes_0->constants_0[5U].z);
    uint cells_0 = uint(edge_0 * edge_0 * edge_0);
    int _S5 = giWrap_0(cell_0.z, edge_0);

#line 484
    uint _S6 = uint(edge_0);

#line 484
    uint _S7 = uint(_S5) * _S6;

#line 484
    int _S8 = giWrap_0(cell_0.y, edge_0);

#line 484
    uint _S9 = (_S7 + uint(_S8)) * _S6;

    int _S10 = giWrap_0(cell_0.x, edge_0);
    return kernelContext_0->giProbes_0->layout_0[level_0 * cells_0 + (_S9 + uint(_S10))];
}


#line 106
float gpClamp_0(float value_2, float lo_0, float hi_0)
{

#line 106
    float _S11;
    if(value_2 < lo_0)
    {

#line 107
        _S11 = lo_0;

#line 107
    }
    else
    {

#line 107
        if(value_2 > hi_0)
        {

#line 107
            _S11 = hi_0;

#line 107
        }
        else
        {

#line 107
            _S11 = value_2;

#line 107
        }

#line 107
    }

#line 107
    return _S11;
}


#line 445
float gpVisibilityWeight_0(uint probe_0, float3 position_0, float3 query_0, KernelContext_0 thread* kernelContext_1)
{

#line 446
    float3 offset_0 = query_0 - position_0;
    float span_0 = length(offset_0);
    if(span_0 <= 0.00009999999747379)
    {

#line 449
        return 1.0;
    }
    float3 _S12 = offset_0 / float3(span_0) ;
    uint _S13 = probe_0 * 4U;

#line 452
    float4 device* _S14 = kernelContext_1->giProbes_0->probes_0+(_S13 + 2U);
    float4 device* _S15 = kernelContext_1->giProbes_0->probes_0+(_S13 + 3U);
    array<float, int(6)> _S16 = { (*_S14).x, (*_S14).y, (*_S14).z, (*_S14).w, (*_S15).x, (*_S15).y };

#line 454
    uint axis_0 = 0U;

#line 454
    float expected_0 = 0.0;

#line 454
    float weight_0 = 0.0;


    for(;;)
    {

#line 457
        if(axis_0 < 6U)
        {
        }
        else
        {

#line 457
            break;
        }

#line 458
        float _S17 = max(0.0, dot(kGpAxes_0[axis_0], _S12));
        float expected_1 = expected_0 + _S16[axis_0] * _S17;
        float weight_1 = weight_0 + _S17;

#line 457
        axis_0 = axis_0 + 1U;

#line 457
        expected_0 = expected_1;

#line 457
        weight_0 = weight_1;

#line 457
    }

#line 462
    if(weight_0 <= 0.0)
    {

#line 463
        return 1.0;
    }



    float slack_0 = (as_type<float>((kernelContext_1->giProbes_0->constants_0[1U].y)));
    float _S18 = expected_0 / weight_0 + slack_0;

#line 469
    if(span_0 > _S18)
    {

#line 470
        return 0.0;
    }
    return gpClamp_0((_S18 - span_0) / max(slack_0, 0.00009999999747379), 0.0, 1.0);
}


#line 273
void gpLoadPayload_0(uint probe_1, uint floats_0, array<float, int(48)> thread* payload_0, KernelContext_0 thread* kernelContext_2)
{

#line 273
    uint index_0 = 0U;
    for(;;)
    {

#line 274
        if(index_0 < 48U)
        {
        }
        else
        {

#line 274
            break;
        }

#line 274
        float _S19;
        if(index_0 < floats_0)
        {

#line 275
            _S19 = *(kernelContext_2->giProbes_0->payloads_0+(probe_1 * floats_0 + index_0));

#line 275
        }
        else
        {

#line 275
            _S19 = 0.0;

#line 275
        }

#line 275
        (*payload_0)[index_0] = _S19;

#line 274
        index_0 = index_0 + 1U;

)cy_msl"
    R"cy_msl(#line 274
    }


    return;
}


#line 110
float3 gpMax0_0(float3 v_0)
{

#line 111
    return float3(max(0.0, v_0.x), max(0.0, v_0.y), max(0.0, v_0.z));
}


#line 154
void gpOctahedralUv_0(float3 direction_0, float thread* u_0, float thread* v_1)
{

#line 155
    float sum_0 = abs(direction_0.x) + abs(direction_0.y) + abs(direction_0.z);

#line 155
    float3 p_0;
    if(sum_0 > 0.0)
    {

#line 156
        p_0 = direction_0 / float3(sum_0) ;

#line 156
    }
    else
    {

#line 156
        p_0 = float3(0.0, 0.0, 1.0);

#line 156
    }
    float _S20 = p_0.x;
    float _S21 = p_0.y;

#line 158
    float px_0;

#line 158
    float py_0;
    if((p_0.z) < 0.0)
    {

#line 160
        float _S22 = 1.0 - abs(_S21);

#line 160
        if(_S20 >= 0.0)
        {

#line 160
            px_0 = 1.0;

#line 160
        }
        else
        {

#line 160
            px_0 = -1.0;

#line 160
        }

#line 160
        float fx_0 = _S22 * px_0;
        float _S23 = 1.0 - abs(_S20);

#line 161
        if(_S21 >= 0.0)
        {

#line 161
            px_0 = 1.0;

#line 161
        }
        else
        {

#line 161
            px_0 = -1.0;

#line 161
        }

#line 161
        float fy_0 = _S23 * px_0;

#line 161
        px_0 = fx_0;

#line 161
        py_0 = fy_0;

#line 159
    }
    else
    {

#line 159
        px_0 = _S20;

#line 159
        py_0 = _S21;

#line 159
    }

#line 165
    *u_0 = gpClamp_0(px_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    *v_1 = gpClamp_0(py_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    return;
}


#line 220
float3 gpDecode_0(uint encoding_0, const array<float, int(48)> thread* payload_1, float3 direction_1)
{

#line 220
    uint c_0;
    if(encoding_0 == 0U)
    {



        array<float, int(4)> _S24 = { 0.28209498524665833, 0.32573533058166504 * direction_1.y, 0.32573533058166504 * direction_1.z, 0.32573533058166504 * direction_1.x };

        thread float3 result_0 = float3(0.0, 0.0, 0.0);

#line 228
        c_0 = 0U;
        for(;;)
        {

#line 229
            if(c_0 < 4U)
            {
            }
            else
            {

#line 229
                break;
            }

#line 230
            uint _S25 = c_0 * 3U;

#line 230
            result_0.x = result_0.x + (*payload_1)[_S25] * _S24[c_0];
            result_0.y = result_0.y + (*payload_1)[_S25 + 1U] * _S24[c_0];
            result_0.z = result_0.z + (*payload_1)[_S25 + 2U] * _S24[c_0];

#line 229
            c_0 = c_0 + 1U;

#line 229
        }

#line 234
        return gpMax0_0(result_0);
    }
    if(encoding_0 == 1U)
    {

#line 237
        thread float u_1 = 0.0;
        thread float v_2 = 0.0;
        gpOctahedralUv_0(direction_1, &u_1, &v_2);
        uint x0_0 = uint(u_1);
        uint y0_0 = uint(v_2);
        uint _S26 = min(x0_0 + 1U, 3U);



        uint _S27 = y0_0 * 4U;

        uint _S28 = min(y0_0 + 1U, 3U) * 4U;

        uint _S29 = (_S27 + x0_0) * 3U;

#line 250
        float3 f00_0 = float3((*payload_1)[_S29], (*payload_1)[_S29 + 1U], (*payload_1)[_S29 + 2U]);
        uint _S30 = (_S27 + _S26) * 3U;
        uint _S31 = (_S28 + x0_0) * 3U;

#line 252
        float3 f01_0 = float3((*payload_1)[_S31], (*payload_1)[_S31 + 1U], (*payload_1)[_S31 + 2U]);
        uint _S32 = (_S28 + _S26) * 3U;

#line 253
        float3 _S33 = float3((u_1 - float(x0_0))) ;
        float3 top_0 = f00_0 + (float3((*payload_1)[_S30], (*payload_1)[_S30 + 1U], (*payload_1)[_S30 + 2U]) - f00_0) * _S33;

        return top_0 + (f01_0 + (float3((*payload_1)[_S32], (*payload_1)[_S32 + 1U], (*payload_1)[_S32 + 2U]) - f01_0) * _S33 - top_0) * float3((v_2 - float(y0_0))) ;
    }
    float3 _S34 = float3(0.0, 0.0, 0.0);

#line 258
    float3 result_1 = _S34;

#line 258
    c_0 = 0U;

#line 258
    float total_0 = 0.0;

    for(;;)
    {

#line 260
        if(c_0 < 6U)
        {
        }
        else
        {

#line 260
            break;
        }

#line 261
        float _S35 = max(0.0, dot(kGpAxes_0[c_0], direction_1));
        if(_S35 <= 0.0)
        {

#line 263
            c_0 = c_0 + 1U;

#line 260
            continue;
        }



        uint _S36 = c_0 * 3U;

        float total_1 = total_0 + _S35;

#line 267
        result_1 = result_1 + float3((*payload_1)[_S36], (*payload_1)[_S36 + 1U], (*payload_1)[_S36 + 2U]) * float3(_S35) ;

#line 267
        total_0 = total_1;

#line 260
        c_0 = c_0 + 1U;

#line 260
    }

#line 269
    if(total_0 > 0.0)
    {

#line 269
        result_1 = result_1 / float3(total_0) ;

#line 269
    }
    else
    {

#line 269
        result_1 = _S34;

#line 269
    }

#line 269
    return result_1;
}


#line 495
float4 gpSample_0(float3 position_1, float3 normal_0, uint frame_1, KernelContext_0 thread* kernelContext_3)
{

#line 496
    uint4 counts_0 = kernelContext_3->giProbes_0->constants_0[0U];
    uint _S37 = counts_0.x;
    uint _S38 = counts_0.y;
    uint _S39 = kernelContext_3->giProbes_0->constants_0[1U].z;
    float _S40 = (as_type<float>((kernelContext_3->giProbes_0->constants_0[5U].x)));

#line 500
    float3 total_2 = float3(0.0, 0.0, 0.0);

#line 500
    float oldest_0 = 0.0;

)cy_msl"
    R"cy_msl(#line 500
    uint level_1 = 0U;

#line 500
    float totalWeight_0 = 0.0;



    for(;;)
    {

#line 504
        if(level_1 < _S39)
        {
        }
        else
        {

#line 504
            break;
        }

#line 505
        float spacing_0 = _S40 * float(1U << level_1);
        float radius_0 = spacing_0 * 1.5;
        float _S41 = position_1.x;

#line 507
        int _S42 = gpFloorDiv_0(_S41 - radius_0, spacing_0);
        float _S43 = position_1.y;

#line 508
        int _S44 = gpFloorDiv_0(_S43 - radius_0, spacing_0);
        float _S45 = position_1.z;
        int _S46 = gpFloorDiv_0(_S41 + radius_0, spacing_0);
        int _S47 = gpFloorDiv_0(_S43 + radius_0, spacing_0);
        int _S48 = gpFloorDiv_0(_S45 + radius_0, spacing_0);

#line 512
        int z_0 = gpFloorDiv_0(_S45 - radius_0, spacing_0);
        for(;;)
        {

#line 513
            if(z_0 <= _S48)
            {
            }
            else
            {

#line 513
                break;
            }

#line 513
            float3 total_3 = total_2;

#line 513
            float oldest_1 = oldest_0;

#line 513
            int y_0 = _S44;

#line 513
            float totalWeight_1 = totalWeight_0;
            for(;;)
            {

#line 514
                if(y_0 <= _S47)
                {
                }
                else
                {

#line 514
                    break;
                }

#line 514
                float3 total_4 = total_3;

#line 514
                float oldest_2 = oldest_1;

#line 514
                int x_0 = _S42;

#line 514
                float totalWeight_2 = totalWeight_1;
                for(;;)
                {

#line 515
                    if(x_0 <= _S46)
                    {
                    }
                    else
                    {

#line 515
                        break;
                    }

#line 515
                    uint _S49 = gpLayoutProbe_0(level_1, int3(x_0, y_0, z_0), kernelContext_3);

                    if(_S49 == 4294967295U)
                    {

#line 518
                        x_0 = x_0 + int(1);

#line 515
                        continue;
                    }



                    uint _S50 = _S49 * 4U;

#line 520
                    float4 device* _S51 = kernelContext_3->giProbes_0->probes_0+(_S50 + 1U);

#line 520
                    float4 record_0 = *_S51;

#line 520
                    bool _S52;
                    if(((*_S51).y) == 0.0)
                    {

#line 521
                        _S52 = true;

#line 521
                    }
                    else
                    {

#line 521
                        _S52 = (record_0.z) == 0.0;

#line 521
                    }

#line 521
                    if(_S52)
                    {

#line 522
                        x_0 = x_0 + int(1);

#line 515
                        continue;
                    }

#line 524
                    float3 at_0 = (*(kernelContext_3->giProbes_0->probes_0+_S50)).xyz;
                    float3 offset_1 = position_1 - at_0;
                    float span_1 = length(offset_1);
                    if(span_1 > radius_0)
                    {

#line 528
                        x_0 = x_0 + int(1);

#line 515
                        continue;
                    }

#line 530
                    float weight_2 = 1.0 - gpClamp_0(span_1 / radius_0, 0.0, 1.0);

#line 530
                    float weight_3;
                    if(span_1 > 0.00009999999747379)
                    {

#line 531
                        weight_3 = weight_2 * gpClamp_0((dot(normal_0, - offset_1 / float3(span_1) ) + 1.0) * 0.5, 0.0, 1.0);

#line 531
                    }
                    else
                    {

#line 531
                        weight_3 = weight_2;

#line 531
                    }

#line 531
                    float _S53 = gpVisibilityWeight_0(_S49, at_0, position_1, kernelContext_3);



                    float weight_4 = weight_3 * _S53;
                    if(weight_4 <= 0.0)
                    {

#line 537
                        x_0 = x_0 + int(1);

#line 515
                        continue;
                    }

#line 539
                    thread array<float, int(48)> data_0;

#line 539
                    gpLoadPayload_0(_S49, _S38, &data_0, kernelContext_3);

#line 539
                    thread array<float, int(48)> _S54 = data_0;

#line 539
                    float3 _S55 = gpDecode_0(_S37, &_S54, normal_0);

                    float3 _S56 = total_4 + _S55 * float3(weight_4) ;
                    float totalWeight_3 = totalWeight_2 + weight_4;
                    uint last_0 = (as_type<uint>((record_0.x)));

#line 543
                    float age_0;
                    if(frame_1 > last_0)
                    {

#line 544
                        age_0 = float(frame_1 - last_0);

#line 544
                    }
                    else
                    {

#line 544
                        age_0 = 0.0;

#line 544
                    }
                    float _S57 = max(oldest_2, age_0);

#line 545
                    total_4 = _S56;

#line 545
                    oldest_2 = _S57;

#line 545
                    totalWeight_2 = totalWeight_3;

#line 515
                    x_0 = x_0 + int(1);

#line 515
                }

#line 514
                int y_1 = y_0 + int(1);

#line 514
                total_3 = total_4;

#line 514
                oldest_1 = oldest_2;

#line 514
                y_0 = y_1;

#line 514
                totalWeight_1 = totalWeight_2;

#line 514
            }

#line 513
            int z_1 = z_0 + int(1);

#line 513
            total_2 = total_3;

#line 513
            oldest_0 = oldest_1;

#line 513
            z_0 = z_1;

#line 513
            totalWeight_0 = totalWeight_1;

#line 513
        }

#line 504
        level_1 = level_1 + 1U;

#line 504
    }

)cy_msl"
    R"cy_msl(#line 550
    if(totalWeight_0 <= 0.0)
    {

#line 551
        return float4(0.0, 0.0, 0.0, 0.0);
    }



    return float4(total_2 / float3(totalWeight_0) , gpClamp_0(1.0 - gpClamp_0(oldest_0 / 240.0, 0.0, 1.0) * 0.75, 0.0, 1.0));
}


#line 24 "src/rendering/gi_gpu/shaders/gi_radiance.slang"
[[kernel]] void cyGiSampleProbes(uint3 thread_0 [[thread_position_in_grid]], GiDispatch_0 constant* giDispatch_1 [[buffer(1)]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]])
{

#line 24
    thread KernelContext_0 kernelContext_4;

#line 24
    (&kernelContext_4)->giDispatch_0 = giDispatch_1;

#line 24
    (&kernelContext_4)->giProbes_0 = giProbes_1;
    uint query_1 = thread_0.x;
    if(query_1 >= (giDispatch_1->count_0))
    {

#line 27
        return;
    }
    uint _S58 = query_1 * 2U;

    float4 device* _S59 = (&kernelContext_4)->giProbes_0->samples_0+query_1;

#line 31
    float4 _S60 = gpSample_0((&kernelContext_4)->giProbes_0->queries_0[_S58].xyz, (&kernelContext_4)->giProbes_0->queries_0[_S58 + 1U].xyz, (&kernelContext_4)->giDispatch_0->frame_0, &kernelContext_4);

#line 31
    *_S59 = _S60;
    return;
}

)cy_msl";

/// cyGiBuildScreenHzb.metal, 2995 bytes.
inline constexpr char kGiBuildScreenHzbMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 77 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiDispatch_0
{
    uint count_0;
    uint frame_0;
    uint reserved0_0;
    uint reserved1_0;
};


#line 67 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiProbeSet_default_0
{
    uint4 device* constants_0;
    float4 device* probes_0;
    float device* payloads_0;
    uint device* selection_0;
    float4 device* directions_0;
    float4 device* results_0;
    uint device* layout_0;
    float4 device* queries_0;
    float4 device* samples_0;
    float4 device* tierRays_0;
    float4 device* tierResults_0;
    float device* screenDepth_0;
    float4 device* screenColour_0;
    float device* screenHzb_0;
    uint device* counters_0;
};


#line 67
struct KernelContext_0
{
    GiDispatch_0 constant* giDispatch_0;
    GiProbeSet_default_0 constant* giProbes_0;
};


#line 36 "src/rendering/gi_gpu/shaders/gi_radiance.slang"
[[kernel]] void cyGiBuildScreenHzb(uint3 thread_0 [[thread_position_in_grid]], GiDispatch_0 constant* giDispatch_1 [[buffer(1)]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]])
{

#line 36
    thread KernelContext_0 kernelContext_0;

#line 36
    (&kernelContext_0)->giDispatch_0 = giDispatch_1;

#line 36
    (&kernelContext_0)->giProbes_0 = giProbes_1;


    uint level_0 = giDispatch_1->frame_0;
    uint _S1 = 26U + giDispatch_1->frame_0;

#line 40
    uint4 shape_0 = giProbes_1->constants_0[_S1];
    uint texel_0 = thread_0.x;
    uint _S2 = shape_0.y;

#line 42
    if(texel_0 >= (_S2 * shape_0.z))
    {

#line 43
        return;
    }
    uint x_0 = texel_0 % _S2;
    uint y_0 = texel_0 / _S2;
    if(level_0 == 0U)
    {

#line 48
        float depth_0 = (&kernelContext_0)->giProbes_0->screenDepth_0[texel_0];
        float device* _S3 = (&kernelContext_0)->giProbes_0->screenHzb_0+(shape_0.x + texel_0);

#line 49
        float _S4;

#line 49
        if(depth_0 > 0.0)
        {

#line 49
            _S4 = depth_0;

#line 49
        }
        else
        {

#line 49
            _S4 = 1.00000001504746622e+30;

#line 49
        }

#line 49
        *_S3 = _S4;
        return;
    }
    uint4 below_0 = giProbes_1->constants_0[_S1 - 1U];
    uint _S5 = x_0 * 2U;

#line 53
    uint _S6 = below_0.y;

#line 53
    uint _S7 = _S6 - 1U;

#line 53
    uint _S8 = min(_S5, _S7);
    uint _S9 = min(_S5 + 1U, _S7);
    uint _S10 = y_0 * 2U;

#line 55
    uint _S11 = below_0.z - 1U;

    uint _S12 = below_0.x;

#line 57
    uint _S13 = _S12 + min(_S10, _S11) * _S6;

    uint _S14 = _S12 + min(_S10 + 1U, _S11) * _S6;

    *((&kernelContext_0)->giProbes_0->screenHzb_0+(shape_0.x + texel_0)) = min(min(*((&kernelContext_0)->giProbes_0->screenHzb_0+(_S13 + _S8)), *((&kernelContext_0)->giProbes_0->screenHzb_0+(_S13 + _S9))), min(*((&kernelContext_0)->giProbes_0->screenHzb_0+(_S14 + _S8)), *((&kernelContext_0)->giProbes_0->screenHzb_0+(_S14 + _S9))));
    return;
}

)cy_msl";

/// cyGiTraceScreen.metal, 15221 bytes.
inline constexpr char kGiTraceScreenMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 678 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiTierSample_0
{
    float3 radiance_0;
    float confidence_0;
    float hitDistance_0;
    uint source_0;
};


#line 685
GiTierSample_0 gpTierSample_0(uint source_1)
{

#line 686
    thread GiTierSample_0 sample_0;
    (&sample_0)->radiance_0 = float3(0.0, 0.0, 0.0);
    (&sample_0)->confidence_0 = 0.0;
    (&sample_0)->hitDistance_0 = -1.0;
    (&sample_0)->source_0 = source_1;
    return sample_0;
}


#line 77 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiDispatch_0
{
    uint count_0;
    uint frame_0;
    uint reserved0_0;
    uint reserved1_0;
};


#line 67 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiProbeSet_default_0
{
    uint4 device* constants_0;
    float4 device* probes_0;
    float device* payloads_0;
    uint device* selection_0;
    float4 device* directions_0;
    float4 device* results_0;
    uint device* layout_0;
    float4 device* queries_0;
    float4 device* samples_0;
    float4 device* tierRays_0;
    float4 device* tierResults_0;
    float device* screenDepth_0;
    float4 device* screenColour_0;
    float device* screenHzb_0;
    uint device* counters_0;
};


#line 67
struct KernelContext_0
{
    GiDispatch_0 constant* giDispatch_0;
    GiProbeSet_default_0 constant* giProbes_0;
};


#line 577
float3 gpTransformPoint_0(uint first_0, float3 p_0, KernelContext_0 thread* kernelContext_0)
{



    return ((as_type<float4>((kernelContext_0->giProbes_0->constants_0[first_0]))) * float4(p_0.x)  + (as_type<float4>((kernelContext_0->giProbes_0->constants_0[first_0 + 1U]))) * float4(p_0.y)  + (as_type<float4>((kernelContext_0->giProbes_0->constants_0[first_0 + 2U]))) * float4(p_0.z)  + (as_type<float4>((kernelContext_0->giProbes_0->constants_0[first_0 + 3U])))).xyz;
}

float3 gpProjectPoint_0(uint first_1, float3 p_1, KernelContext_0 thread* kernelContext_1)
{



    float4 clip_0 = (as_type<float4>((kernelContext_1->giProbes_0->constants_0[first_1]))) * float4(p_1.x)  + (as_type<float4>((kernelContext_1->giProbes_0->constants_0[first_1 + 1U]))) * float4(p_1.y)  + (as_type<float4>((kernelContext_1->giProbes_0->constants_0[first_1 + 2U]))) * float4(p_1.z)  + (as_type<float4>((kernelContext_1->giProbes_0->constants_0[first_1 + 3U])));
    float _S1 = clip_0.w;

#line 591
    if(_S1 == 0.0)
    {

#line 592
        return clip_0.xyz;
    }
    return clip_0.xyz * float3((1.0 / _S1)) ;
}


#line 606
struct GiScreenStep_0
{
    bool outside_0;
    float x_0;
    float y_0;
    float depth_0;
};

GiScreenStep_0 gpScreenStep_0(float3 point_0, uint width_0, uint height_0, KernelContext_0 thread* kernelContext_2)
{

#line 614
    float3 _S2 = gpTransformPoint_0(16U, point_0, kernelContext_2);
    thread GiScreenStep_0 step_0;

    float _S3 = - _S2.z;

#line 617
    (&step_0)->depth_0 = _S3;

#line 617
    float3 _S4 = gpProjectPoint_0(20U, point_0, kernelContext_2);

    float _S5 = float(width_0);

#line 619
    (&step_0)->x_0 = (_S4.x * 0.5 + 0.5) * _S5;
    float _S6 = float(height_0);

#line 620
    (&step_0)->y_0 = (0.5 - _S4.y * 0.5) * _S6;

#line 620
    bool _S7;
    if(_S3 <= 0.0)
    {

#line 621
        _S7 = true;

#line 621
    }
    else
    {

#line 621
        _S7 = ((&step_0)->x_0) < 0.0;

#line 621
    }

#line 621
    if(_S7)
    {

#line 621
        _S7 = true;

#line 621
    }
    else
    {

#line 621
        _S7 = ((&step_0)->y_0) < 0.0;

#line 621
    }

#line 621
    if(_S7)
    {

#line 621
        _S7 = true;

#line 621
    }
    else
    {

#line 621
        _S7 = ((&step_0)->x_0) >= _S5;

#line 621
    }
    if(_S7)
    {

#line 622
        _S7 = true;

#line 622
    }
    else
    {

#line 622
        _S7 = ((&step_0)->y_0) >= _S6;

#line 622
    }

#line 621
    (&step_0)->outside_0 = _S7;

    return step_0;
}


#line 106
float gpClamp_0(float value_0, float lo_0, float hi_0)
{

#line 106
    float _S8;
    if(value_0 < lo_0)
    {

#line 107
        _S8 = lo_0;

#line 107
    }
    else
    {

#line 107
        if(value_0 > hi_0)
        {

#line 107
            _S8 = hi_0;

#line 107
        }
        else
        {

#line 107
            _S8 = value_0;

#line 107
        }

#line 107
    }

#line 107
    return _S8;
}


#line 598
float gpEdgeConfidence_0(float x_1, float y_1, uint width_1, uint height_1)
{
    float u_0 = x_1 / float(width_1);
    float v_0 = y_1 / float(height_1);

    return gpClamp_0(min(min(u_0, 1.0 - u_0), min(v_0, 1.0 - v_0)) / 0.10000000149011612, 0.0, 1.0);
}


#line 628
float gpNearestDepth_0(int x0_0, int y0_0, int x1_0, int y1_0, uint thread* reads_0, KernelContext_0 thread* kernelContext_3)
{

#line 629
    uint _S9 = kernelContext_3->giProbes_0->constants_0[15U].x;
    int _S10 = max(x1_0 - x0_0, y1_0 - y0_0);

#line 630
    uint level_0 = 0U;

    for(;;)
    {

#line 632
        uint _S11 = level_0 + 1U;

#line 632
        bool _S12;

#line 632
        if(_S11 < _S9)
        {

#line 632
            _S12 = (int(1) << uint(int(level_0))) <= _S10;

#line 632
        }
        else
        {

#line 632
            _S12 = false;

#line 632
        }

#line 632
        if(_S12)
        {
        }
        else
        {

)cy_msl"
    R"cy_msl(#line 632
            break;
        }

#line 632
        level_0 = _S11;

#line 632
    }


    uint4 shape_0 = kernelContext_3->giProbes_0->constants_0[26U + level_0];

#line 635
    uint _S13 = uint(int(level_0));
    int _S14 = x0_0 >> _S13;
    int ly0_0 = y0_0 >> _S13;
    uint _S15 = shape_0.y;

#line 638
    int _S16 = min(x1_0 >> _S13, int(_S15) - int(1));
    int _S17 = min(y1_0 >> _S13, int(shape_0.z) - int(1));

#line 639
    float nearest_0 = 1.00000001504746622e+30;

#line 639
    int y_2 = ly0_0;

    for(;;)
    {

#line 641
        if(y_2 <= _S17)
        {
        }
        else
        {

#line 641
            break;
        }

#line 641
        int x_2 = _S14;
        for(;;)
        {

#line 642
            if(x_2 <= _S16)
            {
            }
            else
            {

#line 642
                break;
            }

#line 643
            float _S18 = min(nearest_0, *(kernelContext_3->giProbes_0->screenHzb_0+(shape_0.x + uint(y_2) * _S15 + uint(x_2))));
            *reads_0 = *reads_0 + 1U;

#line 642
            int x_3 = x_2 + int(1);

#line 642
            nearest_0 = _S18;

#line 642
            x_2 = x_3;

#line 642
        }

#line 641
        y_2 = y_2 + int(1);

#line 641
    }

#line 647
    return nearest_0;
}




bool gpScreenRunEmpty_0(float3 origin_0, float3 direction_0, float step_1, uint index_0, uint run_0, const GiScreenStep_0 thread* here_0, uint width_2, uint height_2, uint thread* reads_1, KernelContext_0 thread* kernelContext_4)
{

#line 654
    GiScreenStep_0 _S19 = gpScreenStep_0(origin_0 + direction_0 * float3((step_1 * float(index_0 + run_0))) , width_2, height_2, kernelContext_4);

#line 654
    bool inside_0;

#line 659
    if(!_S19.outside_0)
    {

#line 659
        inside_0 = (_S19.x_0) >= 1.0;

#line 659
    }
    else
    {

#line 659
        inside_0 = false;

#line 659
    }

#line 659
    if(inside_0)
    {

#line 659
        inside_0 = (_S19.y_0) >= 1.0;

#line 659
    }
    else
    {

#line 659
        inside_0 = false;

#line 659
    }

#line 659
    if(inside_0)
    {

#line 659
        inside_0 = (_S19.x_0) < (float(width_2) - 1.0);

#line 659
    }
    else
    {

#line 659
        inside_0 = false;

#line 659
    }
    if(inside_0)
    {

#line 660
        inside_0 = (_S19.y_0) < (float(height_2) - 1.0);

#line 660
    }
    else
    {

#line 660
        inside_0 = false;

#line 660
    }

#line 660
    if(inside_0)
    {

#line 660
        inside_0 = (here_0->x_0) >= 1.0;

#line 660
    }
    else
    {

#line 660
        inside_0 = false;

#line 660
    }
    if(inside_0)
    {

#line 661
        inside_0 = (here_0->y_0) >= 1.0;

#line 661
    }
    else
    {

#line 661
        inside_0 = false;

#line 661
    }

#line 661
    if(inside_0)
    {

#line 661
        inside_0 = (here_0->x_0) < (float(width_2) - 1.0);

#line 661
    }
    else
    {

#line 661
        inside_0 = false;

#line 661
    }

#line 661
    if(inside_0)
    {

#line 661
        inside_0 = (here_0->y_0) < (float(height_2) - 1.0);

#line 661
    }
    else
    {

#line 661
        inside_0 = false;

#line 661
    }

    if(!inside_0)
    {

#line 664
        return false;
    }

#line 664
    float _S20 = gpNearestDepth_0(max(int(floor(min(here_0->x_0, _S19.x_0))) - int(1), int(0)), max(int(floor(min(here_0->y_0, _S19.y_0))) - int(1), int(0)), min(int(floor(max(here_0->x_0, _S19.x_0))) + int(1), int(width_2) - int(1)), min(int(floor(max(here_0->y_0, _S19.y_0))) + int(1), int(height_2) - int(1)), reads_1, kernelContext_4);

#line 675
    return (max(here_0->depth_0, _S19.depth_0)) < (_S20 - abs(_S20) * 0.00009999999747379 - 0.00009999999747379);
}


#line 695
GiTierSample_0 gpScreenTrace_0(float3 origin_1, float3 direction_1, float maxDistance_0, bool accelerated_0, uint thread* reads_2, KernelContext_0 thread* kernelContext_5)
{
    thread GiTierSample_0 sample_1 = gpTierSample_0(5U);
    uint4 screen_0 = kernelContext_5->giProbes_0->constants_0[14U];
    uint width_3 = screen_0.x;
    uint height_3 = screen_0.y;

#line 700
    bool _S21;
    if(width_3 == 0U)
    {

#line 701
        _S21 = true;

#line 701
    }
    else
    {

#line 701
        _S21 = height_3 == 0U;

#line 701
    }

#line 701
    if(_S21)
    {

#line 702
        return sample_1;
    }
    float _S22 = (as_type<float>((screen_0.w)));
    uint _S23 = max(1U, screen_0.z);
    float _S24 = maxDistance_0 / float(_S23);

#line 706
    uint run_1 = 8U;

#line 706
    uint index_1 = 1U;

#line 711
    for(;;)
    {

#line 711
        if(index_1 <= _S23)
        {
        }
        else
        {

)cy_msl"
    R"cy_msl(#line 711
            break;
        }

#line 712
        float t_0 = _S24 * float(index_1);

#line 712
        GiScreenStep_0 _S25 = gpScreenStep_0(origin_1 + direction_1 * float3(t_0) , width_3, height_3, kernelContext_5);


        if(_S25.outside_0)
        {

            (&sample_1)->confidence_0 = 0.0;
            return sample_1;
        }
        uint pixel_0 = uint(_S25.y_0) * width_3 + uint(_S25.x_0);
        float bufferDepth_0 = kernelContext_5->giProbes_0->screenDepth_0[pixel_0];
        *reads_2 = *reads_2 + 1U;


        bool _S26 = bufferDepth_0 > 0.0;

#line 726
        float gap_0;

#line 726
        if(_S26)
        {

#line 726
            gap_0 = bufferDepth_0 - _S25.depth_0;

#line 726
        }
        else
        {

#line 726
            gap_0 = 1.00000001504746622e+30;

#line 726
        }
        if(_S26)
        {

#line 728
            float behind_0 = _S25.depth_0 - bufferDepth_0;
            if(behind_0 > 0.0)
            {

#line 730
                if(behind_0 > _S22)
                {
                    (&sample_1)->confidence_0 = 0.0;
                    return sample_1;
                }
                (&sample_1)->radiance_0 = kernelContext_5->giProbes_0->screenColour_0[pixel_0].xyz;
                (&sample_1)->hitDistance_0 = t_0;

                (&sample_1)->confidence_0 = gpClamp_0(gpEdgeConfidence_0(_S25.x_0, _S25.y_0, width_3, height_3) * (1.0 - behind_0 / max(_S22, 0.00009999999747379)) * 0.92000001668930054, 0.0, 1.0);

                return sample_1;
            }

#line 727
        }

#line 744
        if(accelerated_0)
        {

#line 744
            _S21 = (index_1 + 1U) < _S23;

#line 744
        }
        else
        {

#line 744
            _S21 = false;

#line 744
        }

#line 744
        bool _S27;

#line 744
        if(_S21)
        {

#line 744
            _S27 = gap_0 > (_S22 * 2.0);

#line 744
        }
        else
        {

#line 744
            _S27 = false;

#line 744
        }

#line 744
        uint skip_0;

#line 744
        if(_S27)
        {

#line 745
            uint _S28 = min(run_1, _S23 - index_1);

#line 745
            thread GiScreenStep_0 _S29 = _S25;

#line 745
            bool _S30 = gpScreenRunEmpty_0(origin_1, direction_1, _S24, index_1, _S28, &_S29, width_3, height_3, reads_2, kernelContext_5);

#line 745
            uint run_2;
            if(_S30)
            {

                uint _S31 = min(run_1 * 2U, 32U);

#line 749
                skip_0 = _S28;

#line 749
                run_2 = _S31;

#line 746
            }
            else
            {


                uint _S32 = max(run_1 / 2U, 2U);

#line 751
                skip_0 = 0U;

#line 751
                run_2 = _S32;

#line 746
            }

#line 746
            run_1 = run_2;

#line 744
        }
        else
        {

#line 744
            skip_0 = 0U;

#line 744
        }

#line 744
        index_1 = index_1 + (1U + skip_0);

#line 711
    }

#line 757
    (&sample_1)->confidence_0 = 0.20000000298023224;
    (&sample_1)->hitDistance_0 = -1.0;
    return sample_1;
}


#line 803
void gpCount_0(uint counter_0, uint amount_0, KernelContext_0 thread* kernelContext_6)
{

#line 804
    uint total_0 = simd_sum(amount_0);
    bool _S33 = simd_is_first();

#line 805
    bool _S34;

#line 805
    if(_S33)
    {

#line 805
        _S34 = total_0 != 0U;

#line 805
    }
    else
    {

#line 805
        _S34 = false;

#line 805
    }

#line 805
    if(_S34)
    {

#line 806
        uint _S35 = atomic_fetch_add_explicit(((atomic_uint device*)(kernelContext_6->giProbes_0->counters_0+counter_0)), total_0, memory_order_relaxed);

#line 805
    }


    return;
}


#line 872
void gpWriteTierResult_0(uint ray_0, const GiTierSample_0 thread* sample_2, uint reads_3, KernelContext_0 thread* kernelContext_7)
{

#line 873
    uint _S36 = ray_0 * 2U;

#line 873
    *(kernelContext_7->giProbes_0->tierResults_0+_S36) = float4(sample_2->radiance_0, sample_2->confidence_0);
    *(kernelContext_7->giProbes_0->tierResults_0+(_S36 + 1U)) = float4(sample_2->hitDistance_0, (as_type<float>((sample_2->source_0))), (as_type<float>((reads_3))), 0.0);

    return;
}


#line 66 "src/rendering/gi_gpu/shaders/gi_radiance.slang"
[[kernel]] void cyGiTraceScreen(uint3 thread_0 [[thread_position_in_grid]], GiDispatch_0 constant* giDispatch_1 [[buffer(1)]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]])
{

#line 66
    thread KernelContext_0 kernelContext_8;

#line 66
    (&kernelContext_8)->giDispatch_0 = giDispatch_1;

#line 66
    (&kernelContext_8)->giProbes_0 = giProbes_1;
    uint ray_1 = thread_0.x;
    if(ray_1 >= (giDispatch_1->count_0))
    {

#line 69
        return;
    }
    uint _S37 = ray_1 * 2U;

#line 71
    float4 origin_2 = (&kernelContext_8)->giProbes_0->tierRays_0[_S37];
    float4 direction_2 = (&kernelContext_8)->giProbes_0->tierRays_0[_S37 + 1U];
    bool accelerated_1 = ((&kernelContext_8)->giProbes_0->constants_0[15U].y) != 0U;
    thread uint reads_4 = 0U;

#line 74
    GiTierSample_0 _S38 = gpScreenTrace_0(origin_2.xyz, direction_2.xyz, origin_2.w, accelerated_1, &reads_4, &kernelContext_8);

#line 74
    gpCount_0(3U, reads_4, &kernelContext_8);

#line 74
    thread GiTierSample_0 _S39 = _S38;

#line 74
    gpWriteTierResult_0(ray_1, &_S39, reads_4, &kernelContext_8);

#line 79
    return;
}

)cy_msl";

/// cyGiTraceSoftware.metal, 23465 bytes.
inline constexpr char kGiTraceSoftwareMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 678 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiTierSample_0
{
    float3 radiance_0;
    float confidence_0;
    float hitDistance_0;
    uint source_0;
};


#line 77 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiDispatch_0
{
    uint count_0;
    uint frame_0;
    uint reserved0_0;
    uint reserved1_0;
};


#line 67 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiProbeSet_default_0
{
    uint4 device* constants_0;
    float4 device* probes_0;
    float device* payloads_0;
    uint device* selection_0;
    float4 device* directions_0;
    float4 device* results_0;
    uint device* layout_0;
    float4 device* queries_0;
    float4 device* samples_0;
    float4 device* tierRays_0;
    float4 device* tierResults_0;
    float device* screenDepth_0;
    float4 device* screenColour_0;
    float device* screenHzb_0;
    uint device* counters_0;
};


#line 74 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiSceneSet_default_0
{
    uint4 device* constants_1;
    uint device* pageTable_0;
    float device* bricks_0;
    float4 device* cards_0;
    float4 device* cardState_0;
    uint device* gridRanges_0;
    uint device* gridItems_0;
    float4 device* lights_0;
    float device* shadowDepths_0;
    uint device* selection_1;
    float4 device* results_1;
    float4 device* rays_0;
    float4 device* hits_0;
};


#line 74
struct KernelContext_0
{
    GiDispatch_0 constant* giDispatch_0;
    GiProbeSet_default_0 constant* giProbes_0;
    GiSceneSet_default_0 constant* giScene_0;
};


#line 281 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiWorldHit_0
{
    bool hit_0;
    float t_0;
    float3 position_0;
    float3 normal_0;
    float confidence_1;
};


#line 211 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiTraceHit_0
{
    bool hit_1;
    bool exhausted_0;
    float t_1;
    float closest_0;
    float3 position_1;
    float3 normal_1;
};


#line 111
uint giLevelCount_0(KernelContext_0 thread* kernelContext_0)
{

#line 112
    return kernelContext_0->giScene_0->constants_1[0U].x;
}


#line 119
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_1)
{

#line 120
    return (as_type<float>((kernelContext_1->giScene_0->constants_1[9U].y)));
}


#line 115
uint giWindowBricks_0(KernelContext_0 thread* kernelContext_2)
{

#line 116
    return kernelContext_2->giScene_0->constants_1[0U].y;
}


#line 163
int giWrap_0(int value_0, int modulus_0)
{

#line 164
    uint _S1 = uint(modulus_0);
    uint _S2 = (uint(value_0) + (_S1 << 20U)) % _S1;

#line 165
    return int(_S2);
}


#line 91
float giLerp_0(float a_0, float b_0, float t_2)
{

#line 92
    return a_0 + (b_0 - a_0) * t_2;
}


#line 124
float giTrilinear_0(uint slot_0, float fx_0, float fy_0, float fz_0, KernelContext_0 thread* kernelContext_3)
{
    int x0_0 = clamp(int(floor(fx_0)), int(0), int(3));
    int y0_0 = clamp(int(floor(fy_0)), int(0), int(3));
    int z0_0 = clamp(int(floor(fz_0)), int(0), int(3));



    float tx_0 = clamp(fx_0 - float(x0_0), 0.0, 1.0);
    float ty_0 = clamp(fy_0 - float(y0_0), 0.0, 1.0);

    uint base_0 = slot_0 * 64U;

    uint _S3 = uint(z0_0) * 4U;

#line 137
    uint _S4 = uint(y0_0);

#line 137
    uint _S5 = base_0 + (_S3 + _S4) * 4U;

#line 137
    uint _S6 = uint(x0_0);
    uint _S7 = uint(clamp(x0_0 + int(1), int(0), int(3)));
    uint _S8 = uint(clamp(y0_0 + int(1), int(0), int(3)));

#line 139
    uint _S9 = base_0 + (_S3 + _S8) * 4U;

    uint _S10 = uint(clamp(z0_0 + int(1), int(0), int(3))) * 4U;

#line 141
    uint _S11 = base_0 + (_S10 + _S4) * 4U;

    uint _S12 = base_0 + (_S10 + _S8) * 4U;

#line 149
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S5 + _S6], kernelContext_3->giScene_0->bricks_0[_S5 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S9 + _S6], kernelContext_3->giScene_0->bricks_0[_S9 + _S7], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S11 + _S6], kernelContext_3->giScene_0->bricks_0[_S11 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S12 + _S6], kernelContext_3->giScene_0->bricks_0[_S12 + _S7], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 171
float giSampleLevel_0(uint level_0, float3 p_0, float thread* farDistance_0, KernelContext_0 thread* kernelContext_4)
{

#line 172
    uint _S13 = 16U + level_0 * 2U;

#line 172
    float4 shape_0 = (as_type<float4>((kernelContext_4->giScene_0->constants_1[_S13])));
    int4 origin_0 = (as_type<int4>((kernelContext_4->giScene_0->constants_1[_S13 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_0.x / brick_0));
    int by_0 = int(floor(p_0.y / brick_0));
    int bz_0 = int(floor(p_0.z / brick_0));

#line 179
    uint _S14 = giWindowBricks_0(kernelContext_4);
    int dim_0 = int(_S14);
    int _S15 = origin_0.x;

#line 181
    bool _S16;

#line 181
    if(bx_0 < _S15)
    {

#line 181
        _S16 = true;

#line 181
    }
    else
    {

#line 181
        _S16 = by_0 < (origin_0.y);

#line 181
    }

#line 181
    if(_S16)
    {

#line 181
        _S16 = true;

#line 181
    }
    else
    {

#line 181
        _S16 = bz_0 < (origin_0.z);

#line 181
    }

#line 181
    if(_S16)
    {

#line 181
        _S16 = true;

#line 181
    }
    else
    {

#line 181
        _S16 = bx_0 >= (_S15 + dim_0);

#line 181
    }

#line 181
    if(_S16)
    {

#line 181
        _S16 = true;

#line 181
    }
    else
    {

#line 181
        _S16 = by_0 >= (origin_0.y + dim_0);

#line 181
    }
    if(_S16)
    {

#line 182
        _S16 = true;

#line 182
    }
    else
    {

#line 182
        _S16 = bz_0 >= (origin_0.z + dim_0);

#line 182
    }

)cy_msl"
    R"cy_msl(#line 181
    if(_S16)
    {
        return *farDistance_0;
    }
    int _S17 = giWrap_0(bz_0, dim_0);

#line 185
    uint _S18 = uint(dim_0);

#line 185
    uint _S19 = uint(_S17) * _S18;

#line 185
    int _S20 = giWrap_0(by_0, dim_0);

#line 185
    uint _S21 = (_S19 + uint(_S20)) * _S18;
    int _S22 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_4->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S21 + uint(_S22))];
    if(slot_1 == 4294967295U)
    {

#line 189
        return *farDistance_0;
    }

    float3 local_0 = (p_0 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 192
    float _S23 = giTrilinear_0(slot_1, local_0.x, local_0.y, local_0.z, kernelContext_4);
    return _S23;
}


float giDistance_0(float3 p_1, KernelContext_0 thread* kernelContext_5)
{

#line 197
    uint _S24 = giLevelCount_0(kernelContext_5);

#line 197
    float answer_0 = 1.0e+06;

#line 197
    uint level_1 = 0U;


    for(;;)
    {

#line 200
        if(level_1 < _S24)
        {
        }
        else
        {

#line 200
            break;
        }

#line 201
        thread float farDistance_1 = 0.0;

#line 201
        float _S25 = giSampleLevel_0(level_1, p_1, &farDistance_1, kernelContext_5);

        float _S26 = min(answer_0, _S25);
        if(_S25 < farDistance_1)
        {

#line 205
            return _S25;
        }

#line 200
        uint level_2 = level_1 + 1U;

#line 200
        answer_0 = _S26;

#line 200
        level_1 = level_2;

#line 200
    }

#line 208
    return answer_0;
}


#line 95
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 96
    float squared_0 = dot(v_0, v_0);

#line 96
    float3 _S27;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 97
        _S27 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 97
    }
    else
    {

#line 97
        _S27 = fallback_0;

#line 97
    }

#line 97
    return _S27;
}


#line 221
GiTraceHit_0 giSphereTrace_0(float3 origin_1, float3 direction_0, float maxDistance_0, float tMin_0, KernelContext_0 thread* kernelContext_6)
{

#line 222
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_1 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_1 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_1 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_1 = float3(0.0, 1.0, 0.0);

#line 228
    uint _S28 = giLevelCount_0(kernelContext_6);
    if(_S28 == 0U)
    {

#line 230
        return result_0;
    }

#line 230
    float _S29 = giFinestVoxel_0(kernelContext_6);


    float surface_0 = _S29 * 0.5;
    float _S30 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 235
    recent_0[int(0)] = 1.0e+06;

#line 235
    recent_0[int(1)] = 1.0e+06;

#line 235
    recent_0[int(2)] = 1.0e+06;

#line 235
    float closest_1 = 1.0e+06;

#line 235
    uint recentSlot_0 = 0U;

#line 235
    uint step_0 = 0U;

#line 235
    float t_3 = _S30;


    for(;;)
    {

#line 238
        if(step_0 < 192U)
        {
        }
        else
        {

#line 238
            break;
        }

#line 239
        if(t_3 > maxDistance_0)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_2 = origin_1 + direction_0 * float3(t_3) ;

#line 244
        float _S31 = giDistance_0(position_2, kernelContext_6);

        if(_S31 < surface_0)
        {

#line 247
            (&result_0)->hit_1 = true;
            (&result_0)->t_1 = t_3;
            (&result_0)->position_1 = position_2;


            float3 _S32 = float3(_S29, 0.0, 0.0);

#line 252
            float _S33 = giDistance_0(position_2 + _S32, kernelContext_6);

#line 252
            float _S34 = giDistance_0(position_2 - _S32, kernelContext_6);

#line 252
            float _S35 = _S33 - _S34;

            float3 _S36 = float3(0.0, _S29, 0.0);

#line 254
            float _S37 = giDistance_0(position_2 + _S36, kernelContext_6);

#line 254
            float _S38 = giDistance_0(position_2 - _S36, kernelContext_6);

#line 254
            float _S39 = _S37 - _S38;

            float3 _S40 = float3(0.0, 0.0, _S29);

#line 256
            float _S41 = giDistance_0(position_2 + _S40, kernelContext_6);

#line 256
            float _S42 = giDistance_0(position_2 - _S40, kernelContext_6);

            (&result_0)->normal_1 = giNormalisedOr_0(float3(_S35, _S39, _S41 - _S42), - direction_0);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S43 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S31;
        uint _S44 = (recentSlot_0 + 1U) % 3U;
        float t_4 = t_3 + max(_S31, surface_0);

#line 238
        uint step_1 = step_0 + 1U;

#line 238
        closest_1 = _S43;

#line 238
        recentSlot_0 = _S44;

#line 238
        step_0 = step_1;

#line 238
        t_3 = t_4;

#line 238
    }

#line 267
    (&result_0)->exhausted_0 = true;

    (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
    return result_0;
}


#line 290 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiWorldHit_0 gpMiss_0()
{

#line 291
    thread GiWorldHit_0 miss_0;
    (&miss_0)->hit_0 = false;
    (&miss_0)->t_0 = 0.0;
    (&miss_0)->position_0 = float3(0.0, 0.0, 0.0);
    (&miss_0)->normal_0 = float3(0.0, 1.0, 0.0);
    (&miss_0)->confidence_1 = 1.0;
    return miss_0;
}


#line 106
float gpClamp_0(float value_1, float lo_0, float hi_0)
{

#line 106
    float _S45;
    if(value_1 < lo_0)
    {

#line 107
        _S45 = lo_0;

)cy_msl"
    R"cy_msl(#line 107
    }
    else
    {

#line 107
        if(value_1 > hi_0)
        {

#line 107
            _S45 = hi_0;

#line 107
        }
        else
        {

#line 107
            _S45 = value_1;

#line 107
        }

#line 107
    }

#line 107
    return _S45;
}


#line 309
GiWorldHit_0 GiSoftwareTier_trace_0(float3 origin_2, float3 direction_1, float maxDistance_1, KernelContext_0 thread* kernelContext_7)
{

#line 309
    GiTraceHit_0 _S46 = giSphereTrace_0(origin_2, direction_1, maxDistance_1, 0.0, kernelContext_7);

    if(!_S46.hit_1)
    {

#line 312
        return gpMiss_0();
    }
    thread GiWorldHit_0 hit_2;
    (&hit_2)->hit_0 = true;
    (&hit_2)->t_0 = _S46.t_1;
    (&hit_2)->position_0 = _S46.position_1;
    (&hit_2)->normal_0 = _S46.normal_1;



    (&hit_2)->confidence_1 = (as_type<float4>((kernelContext_7->giProbes_0->constants_0[25U]))).y * gpClamp_0(_S46.closest_0 / max((as_type<float>((kernelContext_7->giScene_0->constants_1[9U].z))), 0.00009999999747379), 0.25, 1.0);
    return hit_2;
}


#line 325
uint GiSoftwareTier_source_0()
{

#line 326
    return 6U;
}


#line 685
GiTierSample_0 gpTierSample_0(uint source_1)
{

#line 686
    thread GiTierSample_0 sample_0;
    (&sample_0)->radiance_0 = float3(0.0, 0.0, 0.0);
    (&sample_0)->confidence_0 = 0.0;
    (&sample_0)->hitDistance_0 = -1.0;
    (&sample_0)->source_0 = source_1;
    return sample_0;
}


#line 121
float3 gpSky_0(float3 direction_2, KernelContext_0 thread* kernelContext_8)
{

#line 122
    float4 zenith_0 = (as_type<float4>((kernelContext_8->giProbes_0->constants_0[2U])));
    float3 horizon_0 = (as_type<float3>((kernelContext_8->giProbes_0->constants_0[3U].xyz)));
    float3 ground_0 = (as_type<float3>((kernelContext_8->giProbes_0->constants_0[4U].xyz)));

    float _S47 = giNormalisedOr_0(direction_2, float3(0.0, 1.0, 0.0)).y;

#line 126
    if(_S47 < 0.0)
    {

#line 127
        return ground_0 * float3(zenith_0.w) ;
    }

    return (horizon_0 + (zenith_0.xyz - horizon_0) * float3(gpClamp_0(pow(_S47, 0.44999998807907104), 0.0, 1.0)) ) * float3(zenith_0.w) ;
}


#line 399 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_2, float cell_0)
{

#line 400
    return int(floor(value_2 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_3, float3 normal_2, KernelContext_0 thread* kernelContext_9)
{

#line 410
    uint4 grid_0 = kernelContext_9->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 413
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S48 = radius_0 * radius_0;
    float _S49 = position_3.x;

#line 418
    int _S50 = giCell_0(_S49 - radius_0, cell_1);

#line 418
    float _S51 = position_3.y;

#line 418
    int _S52 = giCell_0(_S51 - radius_0, cell_1);
    float _S53 = position_3.z;

#line 419
    int _S54 = giCell_0(_S53 - radius_0, cell_1);
    int _S55 = giCell_0(_S49 + radius_0, cell_1);

#line 420
    int _S56 = giCell_0(_S51 + radius_0, cell_1);
    int _S57 = giCell_0(_S53 + radius_0, cell_1);

#line 421
    float bestScore_0 = 1.0e+09;

#line 421
    float bestAlignment_0 = 0.0;

#line 421
    uint best_0 = 4294967295U;

#line 421
    int z_1 = _S54;



    for(;;)
    {

#line 425
        if(z_1 <= _S57)
        {
        }
        else
        {

#line 425
            break;
        }

#line 425
        int y_1 = _S52;
        for(;;)
        {

#line 426
            if(y_1 <= _S56)
            {
            }
            else
            {

#line 426
                break;
            }

#line 426
            float bestScore_1 = bestScore_0;

#line 426
            float bestAlignment_1 = bestAlignment_0;

#line 426
            uint best_1 = best_0;

#line 426
            int x_1 = _S50;
            for(;;)
            {

#line 427
                if(x_1 <= _S55)
                {
                }
                else
                {

#line 427
                    break;
                }
                uint _S58 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 429
                uint first_0 = kernelContext_9->giScene_0->gridRanges_0[_S58];
                uint _S59 = kernelContext_9->giScene_0->gridRanges_0[_S58 + 1U];

#line 430
                float bestScore_2 = bestScore_1;

#line 430
                float bestAlignment_2 = bestAlignment_1;

#line 430
                uint best_2 = best_1;

#line 430
                uint item_0 = first_0;
                for(;;)
                {

#line 431
                    if(item_0 < (first_0 + _S59))
                    {
                    }
                    else
                    {

#line 431
                        break;
                    }

#line 432
                    uint handle_0 = kernelContext_9->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_9->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 434
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }



                    uint _S60 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_9->giScene_0->cards_0[_S60 + 1U].xyz, normal_2);
                    if(alignment_0 <= 0.25)
                    {

#line 439
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }

#line 441
                    float3 offset_0 = kernelContext_9->giScene_0->cards_0[_S60].xyz - position_3;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S48)
                    {

#line 444
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }

#line 447
                    float score_0 = squared_1 / alignment_0;

#line 447
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 448
                        better_0 = score_0 < bestScore_2;

)cy_msl"
    R"cy_msl(#line 448
                    }
                    else
                    {

#line 449
                        if(alignment_0 != bestAlignment_2)
                        {

#line 449
                            better_0 = alignment_0 > bestAlignment_2;

#line 449
                        }
                        else
                        {

#line 449
                            better_0 = handle_0 < best_2;

#line 449
                        }

#line 448
                    }

#line 448
                    float bestScore_3;

#line 448
                    float bestAlignment_3;

#line 448
                    uint best_3;


                    if(better_0)
                    {

#line 451
                        bestScore_3 = score_0;

#line 451
                        bestAlignment_3 = alignment_0;

#line 451
                        best_3 = handle_0;

#line 451
                    }
                    else
                    {

#line 451
                        bestScore_3 = bestScore_2;

#line 451
                        bestAlignment_3 = bestAlignment_2;

#line 451
                        best_3 = best_2;

#line 451
                    }

#line 451
                    bestScore_2 = bestScore_3;

#line 451
                    bestAlignment_2 = bestAlignment_3;

#line 451
                    best_2 = best_3;

#line 431
                    item_0 = item_0 + 1U;

#line 431
                }

#line 427
                int x_2 = x_1 + int(1);

#line 427
                bestScore_1 = bestScore_2;

#line 427
                bestAlignment_1 = bestAlignment_2;

#line 427
                best_1 = best_2;

#line 427
                x_1 = x_2;

#line 427
            }

#line 426
            int y_2 = y_1 + int(1);

#line 426
            bestScore_0 = bestScore_1;

#line 426
            bestAlignment_0 = bestAlignment_1;

#line 426
            best_0 = best_1;

#line 426
            y_1 = y_2;

#line 426
        }

#line 425
        z_1 = z_1 + int(1);

#line 425
    }

#line 460
    return best_0;
}


#line 765 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpWorldRadiance_0(const GiWorldHit_0 thread* hit_3, float3 direction_3, uint source_2, uint cacheFrame_0, KernelContext_0 thread* kernelContext_10)
{

#line 766
    thread GiTierSample_0 sample_1 = gpTierSample_0(source_2);
    if(!hit_3->hit_0)
    {

#line 768
        (&sample_1)->source_0 = 9U;

#line 768
        float3 _S61 = gpSky_0(direction_3, kernelContext_10);
        (&sample_1)->radiance_0 = _S61;
        (&sample_1)->confidence_0 = 1.0;
        (&sample_1)->hitDistance_0 = -1.0;
        return sample_1;
    }
    (&sample_1)->hitDistance_0 = hit_3->t_0;

#line 774
    uint _S62 = giFindCard_0(hit_3->position_0, hit_3->normal_0, kernelContext_10);

    if(_S62 != 4294967295U)
    {

#line 777
        uint _S63 = _S62 * 2U;

#line 777
        (&sample_1)->radiance_0 = (*(kernelContext_10->giScene_0->cardState_0+_S63)).xyz;
        uint last_0 = (as_type<uint>(((*(kernelContext_10->giScene_0->cardState_0+(_S63 + 1U))).w)));

#line 778
        float age_0;
        if(cacheFrame_0 > last_0)
        {

#line 779
            age_0 = float(cacheFrame_0 - last_0);

#line 779
        }
        else
        {

#line 779
            age_0 = 0.0;

#line 779
        }


        (&sample_1)->confidence_0 = hit_3->confidence_1 * (1.0 - gpClamp_0(age_0 / 240.0, 0.0, 1.0) * 0.60000002384185791);
        return sample_1;
    }
    (&sample_1)->confidence_0 = hit_3->confidence_1 * 0.20000000298023224;
    return sample_1;
}


GiTierSample_0 gpWorldTrace_0(float3 origin_3, float3 unit_0, float reach_0, KernelContext_0 thread* kernelContext_11)
{

#line 791
    uint cacheFrame_1 = kernelContext_11->giProbes_0->constants_0[24U].w;

#line 791
    GiWorldHit_0 _S64 = GiSoftwareTier_trace_0(origin_3, unit_0, reach_0, kernelContext_11);

    uint _S65 = GiSoftwareTier_source_0();

#line 793
    thread GiWorldHit_0 _S66 = _S64;

#line 793
    GiTierSample_0 _S67 = gpWorldRadiance_0(&_S66, unit_0, _S65, cacheFrame_1, kernelContext_11);

#line 793
    return _S67;
}


#line 872
void gpWriteTierResult_0(uint ray_0, const GiTierSample_0 thread* sample_2, uint reads_0, KernelContext_0 thread* kernelContext_12)
{

#line 873
    uint _S68 = ray_0 * 2U;

#line 873
    *(kernelContext_12->giProbes_0->tierResults_0+_S68) = float4(sample_2->radiance_0, sample_2->confidence_0);
    *(kernelContext_12->giProbes_0->tierResults_0+(_S68 + 1U)) = float4(sample_2->hitDistance_0, (as_type<float>((sample_2->source_0))), (as_type<float>((reads_0))), 0.0);

    return;
}


#line 83 "src/rendering/gi_gpu/shaders/gi_radiance.slang"
[[kernel]] void cyGiTraceSoftware(uint3 thread_0 [[thread_position_in_grid]], GiDispatch_0 constant* giDispatch_1 [[buffer(1)]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]])
{

#line 83
    thread KernelContext_0 kernelContext_13;

#line 83
    (&kernelContext_13)->giDispatch_0 = giDispatch_1;

#line 83
    (&kernelContext_13)->giProbes_0 = giProbes_1;

#line 83
    (&kernelContext_13)->giScene_0 = giScene_1;
    uint ray_1 = thread_0.x;
    if(ray_1 >= (giDispatch_1->count_0))
    {

#line 86
        return;
    }
    uint _S69 = ray_1 * 2U;

#line 88
    float4 origin_4 = (&kernelContext_13)->giProbes_0->tierRays_0[_S69];

#line 88
    GiTierSample_0 _S70 = gpWorldTrace_0(origin_4.xyz, giNormalisedOr_0((&kernelContext_13)->giProbes_0->tierRays_0[_S69 + 1U].xyz, float3(0.0, 0.0, -1.0)), origin_4.w, &kernelContext_13);

#line 88
    thread GiTierSample_0 _S71 = _S70;

#line 88
    gpWriteTierResult_0(ray_1, &_S71, 0U, &kernelContext_13);



    return;
}

)cy_msl";

/// cyGiTraceTiered.metal, 39314 bytes.
inline constexpr char kGiTraceTieredMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 678 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiTierSample_0
{
    float3 radiance_0;
    float confidence_0;
    float hitDistance_0;
    uint source_0;
};


#line 77 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiDispatch_0
{
    uint count_0;
    uint frame_0;
    uint reserved0_0;
    uint reserved1_0;
};


#line 67 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiProbeSet_default_0
{
    uint4 device* constants_0;
    float4 device* probes_0;
    float device* payloads_0;
    uint device* selection_0;
    float4 device* directions_0;
    float4 device* results_0;
    uint device* layout_0;
    float4 device* queries_0;
    float4 device* samples_0;
    float4 device* tierRays_0;
    float4 device* tierResults_0;
    float device* screenDepth_0;
    float4 device* screenColour_0;
    float device* screenHzb_0;
    uint device* counters_0;
};


#line 74 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiSceneSet_default_0
{
    uint4 device* constants_1;
    uint device* pageTable_0;
    float device* bricks_0;
    float4 device* cards_0;
    float4 device* cardState_0;
    uint device* gridRanges_0;
    uint device* gridItems_0;
    float4 device* lights_0;
    float device* shadowDepths_0;
    uint device* selection_1;
    float4 device* results_1;
    float4 device* rays_0;
    float4 device* hits_0;
};


#line 74
struct KernelContext_0
{
    GiDispatch_0 constant* giDispatch_0;
    GiProbeSet_default_0 constant* giProbes_0;
    GiSceneSet_default_0 constant* giScene_0;
};


#line 281 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiWorldHit_0
{
    bool hit_0;
    float t_0;
    float3 position_0;
    float3 normal_0;
    float confidence_1;
};


#line 211 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiTraceHit_0
{
    bool hit_1;
    bool exhausted_0;
    float t_1;
    float closest_0;
    float3 position_1;
    float3 normal_1;
};


#line 111
uint giLevelCount_0(KernelContext_0 thread* kernelContext_0)
{

#line 112
    return kernelContext_0->giScene_0->constants_1[0U].x;
}


#line 119
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_1)
{

#line 120
    return (as_type<float>((kernelContext_1->giScene_0->constants_1[9U].y)));
}


#line 115
uint giWindowBricks_0(KernelContext_0 thread* kernelContext_2)
{

#line 116
    return kernelContext_2->giScene_0->constants_1[0U].y;
}


#line 163
int giWrap_0(int value_0, int modulus_0)
{

#line 164
    uint _S1 = uint(modulus_0);
    uint _S2 = (uint(value_0) + (_S1 << 20U)) % _S1;

#line 165
    return int(_S2);
}


#line 91
float giLerp_0(float a_0, float b_0, float t_2)
{

#line 92
    return a_0 + (b_0 - a_0) * t_2;
}


#line 124
float giTrilinear_0(uint slot_0, float fx_0, float fy_0, float fz_0, KernelContext_0 thread* kernelContext_3)
{
    int x0_0 = clamp(int(floor(fx_0)), int(0), int(3));
    int y0_0 = clamp(int(floor(fy_0)), int(0), int(3));
    int z0_0 = clamp(int(floor(fz_0)), int(0), int(3));



    float tx_0 = clamp(fx_0 - float(x0_0), 0.0, 1.0);
    float ty_0 = clamp(fy_0 - float(y0_0), 0.0, 1.0);

    uint base_0 = slot_0 * 64U;

    uint _S3 = uint(z0_0) * 4U;

#line 137
    uint _S4 = uint(y0_0);

#line 137
    uint _S5 = base_0 + (_S3 + _S4) * 4U;

#line 137
    uint _S6 = uint(x0_0);
    uint _S7 = uint(clamp(x0_0 + int(1), int(0), int(3)));
    uint _S8 = uint(clamp(y0_0 + int(1), int(0), int(3)));

#line 139
    uint _S9 = base_0 + (_S3 + _S8) * 4U;

    uint _S10 = uint(clamp(z0_0 + int(1), int(0), int(3))) * 4U;

#line 141
    uint _S11 = base_0 + (_S10 + _S4) * 4U;

    uint _S12 = base_0 + (_S10 + _S8) * 4U;

#line 149
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S5 + _S6], kernelContext_3->giScene_0->bricks_0[_S5 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S9 + _S6], kernelContext_3->giScene_0->bricks_0[_S9 + _S7], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S11 + _S6], kernelContext_3->giScene_0->bricks_0[_S11 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S12 + _S6], kernelContext_3->giScene_0->bricks_0[_S12 + _S7], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 171
float giSampleLevel_0(uint level_0, float3 p_0, float thread* farDistance_0, KernelContext_0 thread* kernelContext_4)
{

#line 172
    uint _S13 = 16U + level_0 * 2U;

#line 172
    float4 shape_0 = (as_type<float4>((kernelContext_4->giScene_0->constants_1[_S13])));
    int4 origin_0 = (as_type<int4>((kernelContext_4->giScene_0->constants_1[_S13 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_0.x / brick_0));
    int by_0 = int(floor(p_0.y / brick_0));
    int bz_0 = int(floor(p_0.z / brick_0));

#line 179
    uint _S14 = giWindowBricks_0(kernelContext_4);
    int dim_0 = int(_S14);
    int _S15 = origin_0.x;

#line 181
    bool _S16;

#line 181
    if(bx_0 < _S15)
    {

#line 181
        _S16 = true;

#line 181
    }
    else
    {

#line 181
        _S16 = by_0 < (origin_0.y);

#line 181
    }

#line 181
    if(_S16)
    {

#line 181
        _S16 = true;

#line 181
    }
    else
    {

#line 181
        _S16 = bz_0 < (origin_0.z);

#line 181
    }

#line 181
    if(_S16)
    {

#line 181
        _S16 = true;

#line 181
    }
    else
    {

#line 181
        _S16 = bx_0 >= (_S15 + dim_0);

#line 181
    }

#line 181
    if(_S16)
    {

#line 181
        _S16 = true;

#line 181
    }
    else
    {

#line 181
        _S16 = by_0 >= (origin_0.y + dim_0);

#line 181
    }
    if(_S16)
    {

#line 182
        _S16 = true;

#line 182
    }
    else
    {

#line 182
        _S16 = bz_0 >= (origin_0.z + dim_0);

#line 182
    }

)cy_msl"
    R"cy_msl(#line 181
    if(_S16)
    {
        return *farDistance_0;
    }
    int _S17 = giWrap_0(bz_0, dim_0);

#line 185
    uint _S18 = uint(dim_0);

#line 185
    uint _S19 = uint(_S17) * _S18;

#line 185
    int _S20 = giWrap_0(by_0, dim_0);

#line 185
    uint _S21 = (_S19 + uint(_S20)) * _S18;
    int _S22 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_4->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S21 + uint(_S22))];
    if(slot_1 == 4294967295U)
    {

#line 189
        return *farDistance_0;
    }

    float3 local_0 = (p_0 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 192
    float _S23 = giTrilinear_0(slot_1, local_0.x, local_0.y, local_0.z, kernelContext_4);
    return _S23;
}


float giDistance_0(float3 p_1, KernelContext_0 thread* kernelContext_5)
{

#line 197
    uint _S24 = giLevelCount_0(kernelContext_5);

#line 197
    float answer_0 = 1.0e+06;

#line 197
    uint level_1 = 0U;


    for(;;)
    {

#line 200
        if(level_1 < _S24)
        {
        }
        else
        {

#line 200
            break;
        }

#line 201
        thread float farDistance_1 = 0.0;

#line 201
        float _S25 = giSampleLevel_0(level_1, p_1, &farDistance_1, kernelContext_5);

        float _S26 = min(answer_0, _S25);
        if(_S25 < farDistance_1)
        {

#line 205
            return _S25;
        }

#line 200
        uint level_2 = level_1 + 1U;

#line 200
        answer_0 = _S26;

#line 200
        level_1 = level_2;

#line 200
    }

#line 208
    return answer_0;
}


#line 95
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 96
    float squared_0 = dot(v_0, v_0);

#line 96
    float3 _S27;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 97
        _S27 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 97
    }
    else
    {

#line 97
        _S27 = fallback_0;

#line 97
    }

#line 97
    return _S27;
}


#line 221
GiTraceHit_0 giSphereTrace_0(float3 origin_1, float3 direction_0, float maxDistance_0, float tMin_0, KernelContext_0 thread* kernelContext_6)
{

#line 222
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_1 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_1 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_1 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_1 = float3(0.0, 1.0, 0.0);

#line 228
    uint _S28 = giLevelCount_0(kernelContext_6);
    if(_S28 == 0U)
    {

#line 230
        return result_0;
    }

#line 230
    float _S29 = giFinestVoxel_0(kernelContext_6);


    float surface_0 = _S29 * 0.5;
    float _S30 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 235
    recent_0[int(0)] = 1.0e+06;

#line 235
    recent_0[int(1)] = 1.0e+06;

#line 235
    recent_0[int(2)] = 1.0e+06;

#line 235
    float closest_1 = 1.0e+06;

#line 235
    uint recentSlot_0 = 0U;

#line 235
    uint step_0 = 0U;

#line 235
    float t_3 = _S30;


    for(;;)
    {

#line 238
        if(step_0 < 192U)
        {
        }
        else
        {

#line 238
            break;
        }

#line 239
        if(t_3 > maxDistance_0)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_2 = origin_1 + direction_0 * float3(t_3) ;

#line 244
        float _S31 = giDistance_0(position_2, kernelContext_6);

        if(_S31 < surface_0)
        {

#line 247
            (&result_0)->hit_1 = true;
            (&result_0)->t_1 = t_3;
            (&result_0)->position_1 = position_2;


            float3 _S32 = float3(_S29, 0.0, 0.0);

#line 252
            float _S33 = giDistance_0(position_2 + _S32, kernelContext_6);

#line 252
            float _S34 = giDistance_0(position_2 - _S32, kernelContext_6);

#line 252
            float _S35 = _S33 - _S34;

            float3 _S36 = float3(0.0, _S29, 0.0);

#line 254
            float _S37 = giDistance_0(position_2 + _S36, kernelContext_6);

#line 254
            float _S38 = giDistance_0(position_2 - _S36, kernelContext_6);

#line 254
            float _S39 = _S37 - _S38;

            float3 _S40 = float3(0.0, 0.0, _S29);

#line 256
            float _S41 = giDistance_0(position_2 + _S40, kernelContext_6);

#line 256
            float _S42 = giDistance_0(position_2 - _S40, kernelContext_6);

            (&result_0)->normal_1 = giNormalisedOr_0(float3(_S35, _S39, _S41 - _S42), - direction_0);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S43 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S31;
        uint _S44 = (recentSlot_0 + 1U) % 3U;
        float t_4 = t_3 + max(_S31, surface_0);

#line 238
        uint step_1 = step_0 + 1U;

#line 238
        closest_1 = _S43;

#line 238
        recentSlot_0 = _S44;

#line 238
        step_0 = step_1;

#line 238
        t_3 = t_4;

#line 238
    }

#line 267
    (&result_0)->exhausted_0 = true;

    (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
    return result_0;
}


#line 290 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiWorldHit_0 gpMiss_0()
{

#line 291
    thread GiWorldHit_0 miss_0;
    (&miss_0)->hit_0 = false;
    (&miss_0)->t_0 = 0.0;
    (&miss_0)->position_0 = float3(0.0, 0.0, 0.0);
    (&miss_0)->normal_0 = float3(0.0, 1.0, 0.0);
    (&miss_0)->confidence_1 = 1.0;
    return miss_0;
}


#line 106
float gpClamp_0(float value_1, float lo_0, float hi_0)
{

#line 106
    float _S45;
    if(value_1 < lo_0)
    {

#line 107
        _S45 = lo_0;

)cy_msl"
    R"cy_msl(#line 107
    }
    else
    {

#line 107
        if(value_1 > hi_0)
        {

#line 107
            _S45 = hi_0;

#line 107
        }
        else
        {

#line 107
            _S45 = value_1;

#line 107
        }

#line 107
    }

#line 107
    return _S45;
}


#line 309
GiWorldHit_0 GiSoftwareTier_trace_0(float3 origin_2, float3 direction_1, float maxDistance_1, KernelContext_0 thread* kernelContext_7)
{

#line 309
    GiTraceHit_0 _S46 = giSphereTrace_0(origin_2, direction_1, maxDistance_1, 0.0, kernelContext_7);

    if(!_S46.hit_1)
    {

#line 312
        return gpMiss_0();
    }
    thread GiWorldHit_0 hit_2;
    (&hit_2)->hit_0 = true;
    (&hit_2)->t_0 = _S46.t_1;
    (&hit_2)->position_0 = _S46.position_1;
    (&hit_2)->normal_0 = _S46.normal_1;



    (&hit_2)->confidence_1 = (as_type<float4>((kernelContext_7->giProbes_0->constants_0[25U]))).y * gpClamp_0(_S46.closest_0 / max((as_type<float>((kernelContext_7->giScene_0->constants_1[9U].z))), 0.00009999999747379), 0.25, 1.0);
    return hit_2;
}


#line 325
uint GiSoftwareTier_source_0()
{

#line 326
    return 6U;
}


#line 685
GiTierSample_0 gpTierSample_0(uint source_1)
{

#line 686
    thread GiTierSample_0 sample_0;
    (&sample_0)->radiance_0 = float3(0.0, 0.0, 0.0);
    (&sample_0)->confidence_0 = 0.0;
    (&sample_0)->hitDistance_0 = -1.0;
    (&sample_0)->source_0 = source_1;
    return sample_0;
}


#line 121
float3 gpSky_0(float3 direction_2, KernelContext_0 thread* kernelContext_8)
{

#line 122
    float4 zenith_0 = (as_type<float4>((kernelContext_8->giProbes_0->constants_0[2U])));
    float3 horizon_0 = (as_type<float3>((kernelContext_8->giProbes_0->constants_0[3U].xyz)));
    float3 ground_0 = (as_type<float3>((kernelContext_8->giProbes_0->constants_0[4U].xyz)));

    float _S47 = giNormalisedOr_0(direction_2, float3(0.0, 1.0, 0.0)).y;

#line 126
    if(_S47 < 0.0)
    {

#line 127
        return ground_0 * float3(zenith_0.w) ;
    }

    return (horizon_0 + (zenith_0.xyz - horizon_0) * float3(gpClamp_0(pow(_S47, 0.44999998807907104), 0.0, 1.0)) ) * float3(zenith_0.w) ;
}


#line 399 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_2, float cell_0)
{

#line 400
    return int(floor(value_2 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_3, float3 normal_2, KernelContext_0 thread* kernelContext_9)
{

#line 410
    uint4 grid_0 = kernelContext_9->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 413
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S48 = radius_0 * radius_0;
    float _S49 = position_3.x;

#line 418
    int _S50 = giCell_0(_S49 - radius_0, cell_1);

#line 418
    float _S51 = position_3.y;

#line 418
    int _S52 = giCell_0(_S51 - radius_0, cell_1);
    float _S53 = position_3.z;

#line 419
    int _S54 = giCell_0(_S53 - radius_0, cell_1);
    int _S55 = giCell_0(_S49 + radius_0, cell_1);

#line 420
    int _S56 = giCell_0(_S51 + radius_0, cell_1);
    int _S57 = giCell_0(_S53 + radius_0, cell_1);

#line 421
    float bestScore_0 = 1.0e+09;

#line 421
    float bestAlignment_0 = 0.0;

#line 421
    uint best_0 = 4294967295U;

#line 421
    int z_1 = _S54;



    for(;;)
    {

#line 425
        if(z_1 <= _S57)
        {
        }
        else
        {

#line 425
            break;
        }

#line 425
        int y_1 = _S52;
        for(;;)
        {

#line 426
            if(y_1 <= _S56)
            {
            }
            else
            {

#line 426
                break;
            }

#line 426
            float bestScore_1 = bestScore_0;

#line 426
            float bestAlignment_1 = bestAlignment_0;

#line 426
            uint best_1 = best_0;

#line 426
            int x_1 = _S50;
            for(;;)
            {

#line 427
                if(x_1 <= _S55)
                {
                }
                else
                {

#line 427
                    break;
                }
                uint _S58 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 429
                uint first_0 = kernelContext_9->giScene_0->gridRanges_0[_S58];
                uint _S59 = kernelContext_9->giScene_0->gridRanges_0[_S58 + 1U];

#line 430
                float bestScore_2 = bestScore_1;

#line 430
                float bestAlignment_2 = bestAlignment_1;

#line 430
                uint best_2 = best_1;

#line 430
                uint item_0 = first_0;
                for(;;)
                {

#line 431
                    if(item_0 < (first_0 + _S59))
                    {
                    }
                    else
                    {

#line 431
                        break;
                    }

#line 432
                    uint handle_0 = kernelContext_9->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_9->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 434
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }



                    uint _S60 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_9->giScene_0->cards_0[_S60 + 1U].xyz, normal_2);
                    if(alignment_0 <= 0.25)
                    {

#line 439
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }

#line 441
                    float3 offset_0 = kernelContext_9->giScene_0->cards_0[_S60].xyz - position_3;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S48)
                    {

#line 444
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }

#line 447
                    float score_0 = squared_1 / alignment_0;

#line 447
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 448
                        better_0 = score_0 < bestScore_2;

)cy_msl"
    R"cy_msl(#line 448
                    }
                    else
                    {

#line 449
                        if(alignment_0 != bestAlignment_2)
                        {

#line 449
                            better_0 = alignment_0 > bestAlignment_2;

#line 449
                        }
                        else
                        {

#line 449
                            better_0 = handle_0 < best_2;

#line 449
                        }

#line 448
                    }

#line 448
                    float bestScore_3;

#line 448
                    float bestAlignment_3;

#line 448
                    uint best_3;


                    if(better_0)
                    {

#line 451
                        bestScore_3 = score_0;

#line 451
                        bestAlignment_3 = alignment_0;

#line 451
                        best_3 = handle_0;

#line 451
                    }
                    else
                    {

#line 451
                        bestScore_3 = bestScore_2;

#line 451
                        bestAlignment_3 = bestAlignment_2;

#line 451
                        best_3 = best_2;

#line 451
                    }

#line 451
                    bestScore_2 = bestScore_3;

#line 451
                    bestAlignment_2 = bestAlignment_3;

#line 451
                    best_2 = best_3;

#line 431
                    item_0 = item_0 + 1U;

#line 431
                }

#line 427
                int x_2 = x_1 + int(1);

#line 427
                bestScore_1 = bestScore_2;

#line 427
                bestAlignment_1 = bestAlignment_2;

#line 427
                best_1 = best_2;

#line 427
                x_1 = x_2;

#line 427
            }

#line 426
            int y_2 = y_1 + int(1);

#line 426
            bestScore_0 = bestScore_1;

#line 426
            bestAlignment_0 = bestAlignment_1;

#line 426
            best_0 = best_1;

#line 426
            y_1 = y_2;

#line 426
        }

#line 425
        z_1 = z_1 + int(1);

#line 425
    }

#line 460
    return best_0;
}


#line 765 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpWorldRadiance_0(const GiWorldHit_0 thread* hit_3, float3 direction_3, uint source_2, uint cacheFrame_0, KernelContext_0 thread* kernelContext_10)
{

#line 766
    thread GiTierSample_0 sample_1 = gpTierSample_0(source_2);
    if(!hit_3->hit_0)
    {

#line 768
        (&sample_1)->source_0 = 9U;

#line 768
        float3 _S61 = gpSky_0(direction_3, kernelContext_10);
        (&sample_1)->radiance_0 = _S61;
        (&sample_1)->confidence_0 = 1.0;
        (&sample_1)->hitDistance_0 = -1.0;
        return sample_1;
    }
    (&sample_1)->hitDistance_0 = hit_3->t_0;

#line 774
    uint _S62 = giFindCard_0(hit_3->position_0, hit_3->normal_0, kernelContext_10);

    if(_S62 != 4294967295U)
    {

#line 777
        uint _S63 = _S62 * 2U;

#line 777
        (&sample_1)->radiance_0 = (*(kernelContext_10->giScene_0->cardState_0+_S63)).xyz;
        uint last_0 = (as_type<uint>(((*(kernelContext_10->giScene_0->cardState_0+(_S63 + 1U))).w)));

#line 778
        float age_0;
        if(cacheFrame_0 > last_0)
        {

#line 779
            age_0 = float(cacheFrame_0 - last_0);

#line 779
        }
        else
        {

#line 779
            age_0 = 0.0;

#line 779
        }


        (&sample_1)->confidence_0 = hit_3->confidence_1 * (1.0 - gpClamp_0(age_0 / 240.0, 0.0, 1.0) * 0.60000002384185791);
        return sample_1;
    }
    (&sample_1)->confidence_0 = hit_3->confidence_1 * 0.20000000298023224;
    return sample_1;
}


GiTierSample_0 gpWorldTrace_0(float3 origin_3, float3 unit_0, float reach_0, KernelContext_0 thread* kernelContext_11)
{

#line 791
    uint cacheFrame_1 = kernelContext_11->giProbes_0->constants_0[24U].w;

#line 791
    GiWorldHit_0 _S64 = GiSoftwareTier_trace_0(origin_3, unit_0, reach_0, kernelContext_11);

    uint _S65 = GiSoftwareTier_source_0();

#line 793
    thread GiWorldHit_0 _S66 = _S64;

#line 793
    GiTierSample_0 _S67 = gpWorldRadiance_0(&_S66, unit_0, _S65, cacheFrame_1, kernelContext_11);

#line 793
    return _S67;
}


#line 803
void gpCount_0(uint counter_0, uint amount_0, KernelContext_0 thread* kernelContext_12)
{

#line 804
    uint total_0 = simd_sum(amount_0);
    bool _S68 = simd_is_first();

#line 805
    bool _S69;

#line 805
    if(_S68)
    {

#line 805
        _S69 = total_0 != 0U;

#line 805
    }
    else
    {

#line 805
        _S69 = false;

#line 805
    }

#line 805
    if(_S69)
    {

#line 806
        uint _S70 = atomic_fetch_add_explicit(((atomic_uint device*)(kernelContext_12->giProbes_0->counters_0+counter_0)), total_0, memory_order_relaxed);

#line 805
    }


    return;
}


#line 606
struct GiScreenStep_0
{
    bool outside_0;
    float x_3;
    float y_3;
    float depth_0;
};


#line 577
float3 gpTransformPoint_0(uint first_1, float3 p_2, KernelContext_0 thread* kernelContext_13)
{



    return ((as_type<float4>((kernelContext_13->giProbes_0->constants_0[first_1]))) * float4(p_2.x)  + (as_type<float4>((kernelContext_13->giProbes_0->constants_0[first_1 + 1U]))) * float4(p_2.y)  + (as_type<float4>((kernelContext_13->giProbes_0->constants_0[first_1 + 2U]))) * float4(p_2.z)  + (as_type<float4>((kernelContext_13->giProbes_0->constants_0[first_1 + 3U])))).xyz;
}

float3 gpProjectPoint_0(uint first_2, float3 p_3, KernelContext_0 thread* kernelContext_14)
{



    float4 clip_0 = (as_type<float4>((kernelContext_14->giProbes_0->constants_0[first_2]))) * float4(p_3.x)  + (as_type<float4>((kernelContext_14->giProbes_0->constants_0[first_2 + 1U]))) * float4(p_3.y)  + (as_type<float4>((kernelContext_14->giProbes_0->constants_0[first_2 + 2U]))) * float4(p_3.z)  + (as_type<float4>((kernelContext_14->giProbes_0->constants_0[first_2 + 3U])));
    float _S71 = clip_0.w;

#line 591
    if(_S71 == 0.0)
    {

#line 592
        return clip_0.xyz;
    }
    return clip_0.xyz * float3((1.0 / _S71)) ;
}


#line 614
GiScreenStep_0 gpScreenStep_0(float3 point_0, uint width_0, uint height_0, KernelContext_0 thread* kernelContext_15)
{

#line 614
    float3 _S72 = gpTransformPoint_0(16U, point_0, kernelContext_15);
    thread GiScreenStep_0 step_2;

)cy_msl"
    R"cy_msl(    float _S73 = - _S72.z;

#line 617
    (&step_2)->depth_0 = _S73;

#line 617
    float3 _S74 = gpProjectPoint_0(20U, point_0, kernelContext_15);

    float _S75 = float(width_0);

#line 619
    (&step_2)->x_3 = (_S74.x * 0.5 + 0.5) * _S75;
    float _S76 = float(height_0);

#line 620
    (&step_2)->y_3 = (0.5 - _S74.y * 0.5) * _S76;

#line 620
    bool _S77;
    if(_S73 <= 0.0)
    {

#line 621
        _S77 = true;

#line 621
    }
    else
    {

#line 621
        _S77 = ((&step_2)->x_3) < 0.0;

#line 621
    }

#line 621
    if(_S77)
    {

#line 621
        _S77 = true;

#line 621
    }
    else
    {

#line 621
        _S77 = ((&step_2)->y_3) < 0.0;

#line 621
    }

#line 621
    if(_S77)
    {

#line 621
        _S77 = true;

#line 621
    }
    else
    {

#line 621
        _S77 = ((&step_2)->x_3) >= _S75;

#line 621
    }
    if(_S77)
    {

#line 622
        _S77 = true;

#line 622
    }
    else
    {

#line 622
        _S77 = ((&step_2)->y_3) >= _S76;

#line 622
    }

#line 621
    (&step_2)->outside_0 = _S77;

    return step_2;
}


#line 598
float gpEdgeConfidence_0(float x_4, float y_4, uint width_1, uint height_1)
{
    float u_0 = x_4 / float(width_1);
    float v_1 = y_4 / float(height_1);

    return gpClamp_0(min(min(u_0, 1.0 - u_0), min(v_1, 1.0 - v_1)) / 0.10000000149011612, 0.0, 1.0);
}


#line 628
float gpNearestDepth_0(int x0_1, int y0_1, int x1_0, int y1_0, uint thread* reads_0, KernelContext_0 thread* kernelContext_16)
{

#line 629
    uint _S78 = kernelContext_16->giProbes_0->constants_0[15U].x;
    int _S79 = max(x1_0 - x0_1, y1_0 - y0_1);

#line 630
    uint level_3 = 0U;

    for(;;)
    {

#line 632
        uint _S80 = level_3 + 1U;

#line 632
        bool _S81;

#line 632
        if(_S80 < _S78)
        {

#line 632
            _S81 = (int(1) << uint(int(level_3))) <= _S79;

#line 632
        }
        else
        {

#line 632
            _S81 = false;

#line 632
        }

#line 632
        if(_S81)
        {
        }
        else
        {

#line 632
            break;
        }

#line 632
        level_3 = _S80;

#line 632
    }


    uint4 shape_1 = kernelContext_16->giProbes_0->constants_0[26U + level_3];

#line 635
    uint _S82 = uint(int(level_3));
    int _S83 = x0_1 >> _S82;
    int ly0_0 = y0_1 >> _S82;
    uint _S84 = shape_1.y;

#line 638
    int _S85 = min(x1_0 >> _S82, int(_S84) - int(1));
    int _S86 = min(y1_0 >> _S82, int(shape_1.z) - int(1));

#line 639
    float nearest_0 = 1.00000001504746622e+30;

#line 639
    int y_5 = ly0_0;

    for(;;)
    {

#line 641
        if(y_5 <= _S86)
        {
        }
        else
        {

#line 641
            break;
        }

#line 641
        int x_5 = _S83;
        for(;;)
        {

#line 642
            if(x_5 <= _S85)
            {
            }
            else
            {

#line 642
                break;
            }

#line 643
            float _S87 = min(nearest_0, *(kernelContext_16->giProbes_0->screenHzb_0+(shape_1.x + uint(y_5) * _S84 + uint(x_5))));
            *reads_0 = *reads_0 + 1U;

#line 642
            int x_6 = x_5 + int(1);

#line 642
            nearest_0 = _S87;

#line 642
            x_5 = x_6;

#line 642
        }

#line 641
        y_5 = y_5 + int(1);

#line 641
    }

#line 647
    return nearest_0;
}




bool gpScreenRunEmpty_0(float3 origin_4, float3 direction_4, float step_3, uint index_0, uint run_0, const GiScreenStep_0 thread* here_0, uint width_2, uint height_2, uint thread* reads_1, KernelContext_0 thread* kernelContext_17)
{

#line 654
    GiScreenStep_0 _S88 = gpScreenStep_0(origin_4 + direction_4 * float3((step_3 * float(index_0 + run_0))) , width_2, height_2, kernelContext_17);

#line 654
    bool inside_0;

#line 659
    if(!_S88.outside_0)
    {

#line 659
        inside_0 = (_S88.x_3) >= 1.0;

#line 659
    }
    else
    {

#line 659
        inside_0 = false;

#line 659
    }

#line 659
    if(inside_0)
    {

#line 659
        inside_0 = (_S88.y_3) >= 1.0;

#line 659
    }
    else
    {

#line 659
        inside_0 = false;

#line 659
    }

#line 659
    if(inside_0)
    {

#line 659
        inside_0 = (_S88.x_3) < (float(width_2) - 1.0);

#line 659
    }
    else
    {

#line 659
        inside_0 = false;

#line 659
    }
    if(inside_0)
    {

)cy_msl"
    R"cy_msl(#line 660
        inside_0 = (_S88.y_3) < (float(height_2) - 1.0);

#line 660
    }
    else
    {

#line 660
        inside_0 = false;

#line 660
    }

#line 660
    if(inside_0)
    {

#line 660
        inside_0 = (here_0->x_3) >= 1.0;

#line 660
    }
    else
    {

#line 660
        inside_0 = false;

#line 660
    }
    if(inside_0)
    {

#line 661
        inside_0 = (here_0->y_3) >= 1.0;

#line 661
    }
    else
    {

#line 661
        inside_0 = false;

#line 661
    }

#line 661
    if(inside_0)
    {

#line 661
        inside_0 = (here_0->x_3) < (float(width_2) - 1.0);

#line 661
    }
    else
    {

#line 661
        inside_0 = false;

#line 661
    }

#line 661
    if(inside_0)
    {

#line 661
        inside_0 = (here_0->y_3) < (float(height_2) - 1.0);

#line 661
    }
    else
    {

#line 661
        inside_0 = false;

#line 661
    }

    if(!inside_0)
    {

#line 664
        return false;
    }

#line 664
    float _S89 = gpNearestDepth_0(max(int(floor(min(here_0->x_3, _S88.x_3))) - int(1), int(0)), max(int(floor(min(here_0->y_3, _S88.y_3))) - int(1), int(0)), min(int(floor(max(here_0->x_3, _S88.x_3))) + int(1), int(width_2) - int(1)), min(int(floor(max(here_0->y_3, _S88.y_3))) + int(1), int(height_2) - int(1)), reads_1, kernelContext_17);

#line 675
    return (max(here_0->depth_0, _S88.depth_0)) < (_S89 - abs(_S89) * 0.00009999999747379 - 0.00009999999747379);
}


#line 695
GiTierSample_0 gpScreenTrace_0(float3 origin_5, float3 direction_5, float maxDistance_2, bool accelerated_0, uint thread* reads_2, KernelContext_0 thread* kernelContext_18)
{
    thread GiTierSample_0 sample_2 = gpTierSample_0(5U);
    uint4 screen_0 = kernelContext_18->giProbes_0->constants_0[14U];
    uint width_3 = screen_0.x;
    uint height_3 = screen_0.y;

#line 700
    bool _S90;
    if(width_3 == 0U)
    {

#line 701
        _S90 = true;

#line 701
    }
    else
    {

#line 701
        _S90 = height_3 == 0U;

#line 701
    }

#line 701
    if(_S90)
    {

#line 702
        return sample_2;
    }
    float _S91 = (as_type<float>((screen_0.w)));
    uint _S92 = max(1U, screen_0.z);
    float _S93 = maxDistance_2 / float(_S92);

#line 706
    uint run_1 = 8U;

#line 706
    uint index_1 = 1U;

#line 711
    for(;;)
    {

#line 711
        if(index_1 <= _S92)
        {
        }
        else
        {

#line 711
            break;
        }

#line 712
        float t_5 = _S93 * float(index_1);

#line 712
        GiScreenStep_0 _S94 = gpScreenStep_0(origin_5 + direction_5 * float3(t_5) , width_3, height_3, kernelContext_18);


        if(_S94.outside_0)
        {

            (&sample_2)->confidence_0 = 0.0;
            return sample_2;
        }
        uint pixel_0 = uint(_S94.y_3) * width_3 + uint(_S94.x_3);
        float bufferDepth_0 = kernelContext_18->giProbes_0->screenDepth_0[pixel_0];
        *reads_2 = *reads_2 + 1U;


        bool _S95 = bufferDepth_0 > 0.0;

#line 726
        float gap_0;

#line 726
        if(_S95)
        {

#line 726
            gap_0 = bufferDepth_0 - _S94.depth_0;

#line 726
        }
        else
        {

#line 726
            gap_0 = 1.00000001504746622e+30;

#line 726
        }
        if(_S95)
        {

#line 728
            float behind_0 = _S94.depth_0 - bufferDepth_0;
            if(behind_0 > 0.0)
            {

#line 730
                if(behind_0 > _S91)
                {
                    (&sample_2)->confidence_0 = 0.0;
                    return sample_2;
                }
                (&sample_2)->radiance_0 = kernelContext_18->giProbes_0->screenColour_0[pixel_0].xyz;
                (&sample_2)->hitDistance_0 = t_5;

                (&sample_2)->confidence_0 = gpClamp_0(gpEdgeConfidence_0(_S94.x_3, _S94.y_3, width_3, height_3) * (1.0 - behind_0 / max(_S91, 0.00009999999747379)) * 0.92000001668930054, 0.0, 1.0);

                return sample_2;
            }

#line 727
        }

#line 744
        if(accelerated_0)
        {

#line 744
            _S90 = (index_1 + 1U) < _S92;

#line 744
        }
        else
        {

#line 744
            _S90 = false;

#line 744
        }

#line 744
        bool _S96;

#line 744
        if(_S90)
        {

#line 744
            _S96 = gap_0 > (_S91 * 2.0);

#line 744
        }
        else
        {

#line 744
            _S96 = false;

#line 744
        }

#line 744
        uint skip_0;

#line 744
        if(_S96)
        {

#line 745
            uint _S97 = min(run_1, _S92 - index_1);

#line 745
            thread GiScreenStep_0 _S98 = _S94;

#line 745
            bool _S99 = gpScreenRunEmpty_0(origin_5, direction_5, _S93, index_1, _S97, &_S98, width_3, height_3, reads_2, kernelContext_18);

#line 745
            uint run_2;
            if(_S99)
            {

                uint _S100 = min(run_1 * 2U, 32U);

#line 749
                skip_0 = _S97;

#line 749
                run_2 = _S100;

#line 746
            }
            else
            {


                uint _S101 = max(run_1 / 2U, 2U);

#line 751
                skip_0 = 0U;

)cy_msl"
    R"cy_msl(#line 751
                run_2 = _S101;

#line 746
            }

#line 746
            run_1 = run_2;

#line 744
        }
        else
        {

#line 744
            skip_0 = 0U;

#line 744
        }

#line 744
        index_1 = index_1 + (1U + skip_0);

#line 711
    }

#line 757
    (&sample_2)->confidence_0 = 0.20000000298023224;
    (&sample_2)->hitDistance_0 = -1.0;
    return sample_2;
}


#line 812
uint gpTicket_0(uint counter_1, KernelContext_0 thread* kernelContext_19)
{
    uint count_1 = simd_sum(1U);
    uint rank_0 = simd_prefix_exclusive_sum(1U);

    bool _S102 = simd_is_first();

#line 817
    uint base_1;

#line 817
    if(_S102)
    {

#line 818
        uint _S103 = atomic_fetch_add_explicit(((atomic_uint device*)(kernelContext_19->giProbes_0->counters_0+counter_1)), count_1, memory_order_relaxed);

#line 818
        base_1 = _S103;

#line 817
    }
    else
    {

#line 817
        base_1 = 0U;

#line 817
    }


    uint _S104 = simd_broadcast_first(base_1);

#line 820
    return _S104 + rank_0;
}


#line 827
GiTierSample_0 gpTieredTrace_0(float3 origin_6, float3 direction_6, float maxDistance_3, bool screenPresent_0, uint thread* reads_3, KernelContext_0 thread* kernelContext_20)
{
    float3 unit_1 = giNormalisedOr_0(direction_6, float3(0.0, 0.0, -1.0));
    float4 hzb_0 = (as_type<float4>((kernelContext_20->giProbes_0->constants_0[15U])));
    float _S105 = min(maxDistance_3, hzb_0.z);
    float threshold_0 = hzb_0.w;
    bool accelerated_1 = (kernelContext_20->giProbes_0->constants_0[15U].y) != 0U;

    GiTierSample_0 _S106 = gpTierSample_0(0U);

#line 835
    GiTierSample_0 screen_1;
    if(screenPresent_0)
    {

#line 836
        gpCount_0(4U, 1U, kernelContext_20);

#line 836
        GiTierSample_0 _S107 = gpScreenTrace_0(origin_6, unit_1, _S105, accelerated_1, reads_3, kernelContext_20);


        if((_S107.confidence_0) >= threshold_0)
        {

#line 840
            return _S107;
        }

#line 840
        screen_1 = _S107;

#line 836
    }
    else
    {

#line 836
        screen_1 = _S106;

#line 836
    }

#line 847
    uint4 budget_0 = kernelContext_20->giProbes_0->constants_0[24U];

#line 847
    uint limit_0;
    if(screenPresent_0)
    {

#line 848
        limit_0 = min(budget_0.x, budget_0.y);

#line 848
    }
    else
    {

#line 848
        limit_0 = budget_0.x;

#line 848
    }

#line 848
    uint _S108 = gpTicket_0(0U, kernelContext_20);

    if(_S108 >= limit_0)
    {

#line 850
        gpCount_0(2U, 1U, kernelContext_20);

        return screen_1;
    }
    if(screenPresent_0)
    {

#line 854
        gpCount_0(1U, 1U, kernelContext_20);

#line 854
    }


    if((GiSoftwareTier_source_0()) == 7U)
    {

#line 857
        limit_0 = 6U;

#line 857
    }
    else
    {

#line 857
        limit_0 = 5U;

#line 857
    }

#line 857
    gpCount_0(limit_0, 1U, kernelContext_20);

#line 857
    GiTierSample_0 _S109 = gpWorldTrace_0(origin_6, unit_1, _S105, kernelContext_20);

    if((_S109.source_0) == 9U)
    {

#line 859
        limit_0 = 1U;

#line 859
    }
    else
    {

#line 859
        limit_0 = 0U;

#line 859
    }

#line 859
    gpCount_0(7U, limit_0, kernelContext_20);

#line 859
    bool _S110;
    if(!screenPresent_0)
    {

#line 860
        _S110 = true;

#line 860
    }
    else
    {

#line 860
        _S110 = (screen_1.confidence_0) <= 0.0;

#line 860
    }

#line 860
    if(_S110)
    {

#line 861
        return _S109;
    }
    thread GiTierSample_0 blended_0;
    (&blended_0)->radiance_0 = _S109.radiance_0 + (screen_1.radiance_0 - _S109.radiance_0) * float3(screen_1.confidence_0) ;
    (&blended_0)->confidence_0 = max(screen_1.confidence_0, _S109.confidence_0);
    bool _S111 = (screen_1.confidence_0) >= (_S109.confidence_0);

#line 866
    if(_S111)
    {

#line 866
        limit_0 = screen_1.source_0;

#line 866
    }
    else
    {

#line 866
        limit_0 = _S109.source_0;

#line 866
    }

#line 866
    (&blended_0)->source_0 = limit_0;

#line 866
    float _S112;

    if(_S111)
    {

#line 868
        _S112 = screen_1.hitDistance_0;

#line 868
    }
    else
    {

#line 868
        _S112 = _S109.hitDistance_0;

#line 868
    }

#line 867
    (&blended_0)->hitDistance_0 = _S112;

    return blended_0;
}

void gpWriteTierResult_0(uint ray_0, const GiTierSample_0 thread* sample_3, uint reads_4, KernelContext_0 thread* kernelContext_21)
{

#line 873
    uint _S113 = ray_0 * 2U;

#line 873
    *(kernelContext_21->giProbes_0->tierResults_0+_S113) = float4(sample_3->radiance_0, sample_3->confidence_0);
    *(kernelContext_21->giProbes_0->tierResults_0+(_S113 + 1U)) = float4(sample_3->hitDistance_0, (as_type<float>((sample_3->source_0))), (as_type<float>((reads_4))), 0.0);

    return;
}


#line 96 "src/rendering/gi_gpu/shaders/gi_radiance.slang"
[[kernel]] void cyGiTraceTiered(uint3 thread_0 [[thread_position_in_grid]], GiDispatch_0 constant* giDispatch_1 [[buffer(1)]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]])
{

#line 96
    thread KernelContext_0 kernelContext_22;

)cy_msl"
    R"cy_msl(#line 96
    (&kernelContext_22)->giDispatch_0 = giDispatch_1;

#line 96
    (&kernelContext_22)->giProbes_0 = giProbes_1;

#line 96
    (&kernelContext_22)->giScene_0 = giScene_1;
    uint ray_1 = thread_0.x;
    if(ray_1 >= (giDispatch_1->count_0))
    {

#line 99
        return;
    }
    uint _S114 = ray_1 * 2U;

#line 101
    float4 origin_7 = (&kernelContext_22)->giProbes_0->tierRays_0[_S114];
    float4 direction_7 = (&kernelContext_22)->giProbes_0->tierRays_0[_S114 + 1U];
    bool screenPresent_1 = ((&kernelContext_22)->giProbes_0->constants_0[14U].x) != 0U;
    thread uint reads_5 = 0U;

#line 104
    GiTierSample_0 _S115 = gpTieredTrace_0(origin_7.xyz, direction_7.xyz, origin_7.w, screenPresent_1, &reads_5, &kernelContext_22);

#line 104
    gpCount_0(3U, reads_5, &kernelContext_22);

#line 104
    thread GiTierSample_0 _S116 = _S115;

#line 104
    gpWriteTierResult_0(ray_1, &_S116, reads_5, &kernelContext_22);

#line 109
    return;
}

)cy_msl";

/// cyGiGatherProbesHardware.metal, 33977 bytes.
inline constexpr char kGiGatherProbesHardwareMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

            raytracing::intersection_params _slang_ray_flags_to_intersection_params(uint flags){
                raytracing::intersection_params params;
                if (flags & 0x01) /* RAY_FLAG_FORCE_OPAQUE */
                    params.force_opacity(raytracing::forced_opacity::opaque);
                if (flags & 0x02) /* RAY_FLAG_FORCE_NON_OPAQUE */
                    params.force_opacity(raytracing::forced_opacity::non_opaque);
                if (flags & 0x10) /* RAY_FLAG_CULL_BACK_FACING_TRIANGLES */
                    params.set_triangle_cull_mode(raytracing::triangle_cull_mode::back);
                if (flags & 0x20) /* RAY_FLAG_CULL_FRONT_FACING_TRIANGLES */
                    params.set_triangle_cull_mode(raytracing::triangle_cull_mode::front);
                if (flags & 0x40) /* RAY_FLAG_CULL_OPAQUE */
                    params.set_opacity_cull_mode(raytracing::opacity_cull_mode::opaque);
                if (flags & 0x80) /* RAY_FLAG_CULL_NON_OPAQUE */
                    params.set_opacity_cull_mode(raytracing::opacity_cull_mode::non_opaque);
                if (flags & 0x100) /* RAY_FLAG_SKIP_TRIANGLES */
                    params.set_geometry_cull_mode(raytracing::geometry_cull_mode::triangle);
                if (flags & 0x200) /* RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES */
                    params.set_geometry_cull_mode(raytracing::geometry_cull_mode::bounding_box);
                return params;
            }
        

                uint _slang_committed_intersection_type_to_uint(raytracing::intersection_type type){
                    switch(type){
                        case raytracing::intersection_type::none:
                            return 0; /* COMMITTED_NOTHING */
                        case raytracing::intersection_type::triangle:
                            return 1; /* COMMITTED_TRIANGLE_HIT */
                        case raytracing::intersection_type::bounding_box:
                            return 2; /* COMMITTED_PROCEDURAL_PRIMITIVE_HIT */
                        default:
                            return 0xFFFFFFFF; // Invalid value to indicate error case, should never be returned by the API
                    }
                }
            

#line 67 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiProbeSet_default_0
{
    uint4 device* constants_0;
    float4 device* probes_0;
    float device* payloads_0;
    uint device* selection_0;
    float4 device* directions_0;
    float4 device* results_0;
    uint device* layout_0;
    float4 device* queries_0;
    float4 device* samples_0;
    float4 device* tierRays_0;
    float4 device* tierResults_0;
    float device* screenDepth_0;
    float4 device* screenColour_0;
    float device* screenHzb_0;
    uint device* counters_0;
};


#line 74 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiSceneSet_default_0
{
    uint4 device* constants_1;
    uint device* pageTable_0;
    float device* bricks_0;
    float4 device* cards_0;
    float4 device* cardState_0;
    uint device* gridRanges_0;
    uint device* gridItems_0;
    float4 device* lights_0;
    float device* shadowDepths_0;
    uint device* selection_1;
    float4 device* results_1;
    float4 device* rays_0;
    float4 device* hits_0;
};


#line 77
struct GiDispatch_0
{
    uint count_0;
    uint frame_0;
    uint reserved0_0;
    uint reserved1_0;
};


#line 5319 "core.meta.slang"
struct GiHardwareSet_default_0
{
    metal::raytracing::acceleration_structure<metal::raytracing::instancing> topLevel_0;
    uint4 device* instances_0;
    float device* positions_0;
    uint device* indices_0;
};


#line 375 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct KernelContext_0
{
    GiProbeSet_default_0 constant* giProbes_0;
    GiSceneSet_default_0 constant* giScene_0;
    GiDispatch_0 constant* giDispatch_0;
    GiHardwareSet_default_0 constant* giHardware_0;
    array<float3, int(64)> threadgroup* gpRayRadiance_0;
    array<float, int(64)> threadgroup* gpRayDistance_0;
};


#line 95 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 96
    float squared_0 = dot(v_0, v_0);

#line 96
    float3 _S1;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 97
        _S1 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 97
    }
    else
    {

#line 97
        _S1 = fallback_0;

#line 97
    }

#line 97
    return _S1;
}


#line 106 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
float gpClamp_0(float value_0, float lo_0, float hi_0)
{

#line 106
    float _S2;
    if(value_0 < lo_0)
    {

#line 107
        _S2 = lo_0;

#line 107
    }
    else
    {

#line 107
        if(value_0 > hi_0)
        {

#line 107
            _S2 = hi_0;

#line 107
        }
        else
        {

#line 107
            _S2 = value_0;

#line 107
        }

#line 107
    }

#line 107
    return _S2;
}


#line 121
float3 gpSky_0(float3 direction_0, KernelContext_0 thread* kernelContext_0)
{

#line 122
    float4 zenith_0 = (as_type<float4>((kernelContext_0->giProbes_0->constants_0[2U])));
    float3 horizon_0 = (as_type<float3>((kernelContext_0->giProbes_0->constants_0[3U].xyz)));
    float3 ground_0 = (as_type<float3>((kernelContext_0->giProbes_0->constants_0[4U].xyz)));

    float _S3 = giNormalisedOr_0(direction_0, float3(0.0, 1.0, 0.0)).y;

#line 126
    if(_S3 < 0.0)
    {

#line 127
        return ground_0 * float3(zenith_0.w) ;
    }

    return (horizon_0 + (zenith_0.xyz - horizon_0) * float3(gpClamp_0(pow(_S3, 0.44999998807907104), 0.0, 1.0)) ) * float3(zenith_0.w) ;
}


#line 281
struct GiWorldHit_0
{
    bool hit_0;
    float t_0;
    float3 position_0;
    float3 normal_0;
    float confidence_0;
};


#line 19393 "hlsl.meta.slang"
struct RayDesc_0
{
    float3 Origin_0;
    float TMin_0;
    float3 Direction_0;
    float TMax_0;
};


#line 290 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiWorldHit_0 gpMiss_0()
{

#line 291
    thread GiWorldHit_0 miss_0;
    (&miss_0)->hit_0 = false;
    (&miss_0)->t_0 = 0.0;
    (&miss_0)->position_0 = float3(0.0, 0.0, 0.0);
    (&miss_0)->normal_0 = float3(0.0, 1.0, 0.0);
    (&miss_0)->confidence_0 = 1.0;
    return miss_0;
}


#line 32 "src/rendering/gi_gpu/shaders/gi_hardware.slang"
float3 ghVertex_0(uint vertex_0, KernelContext_0 thread* kernelContext_1)
{

#line 33
    uint _S4 = vertex_0 * 3U;

#line 33
    return float3(kernelContext_1->giHardware_0->positions_0[_S4], kernelContext_1->giHardware_0->positions_0[_S4 + 1U], kernelContext_1->giHardware_0->positions_0[_S4 + 2U]);
}



GiWorldHit_0 GiHardwareTier_trace_0(float3 origin_0, float3 direction_1, float maxDistance_0, KernelContext_0 thread* kernelContext_2)
{

#line 39
    thread RayDesc_0 ray_0;
    (&ray_0)->Origin_0 = origin_0;
    (&ray_0)->Direction_0 = direction_1;
    (&ray_0)->TMin_0 = 0.00100000004749745;
    (&ray_0)->TMax_0 = maxDistance_0;
    thread raytracing::intersection_query<raytracing::triangle_data, raytracing::instancing> query_0;
    (&query_0)->reset({ (ray_0.Origin_0), (ray_0.Direction_0), (ray_0.TMin_0), (ray_0.TMax_0) }, (kernelContext_2->giHardware_0->topLevel_0), (255U), _slang_ray_flags_to_intersection_params((1U)));
    for(;;)
    {

#line 46
        bool _S5 = ((&query_0)->next());

#line 46
        if(_S5)
        {
        }
        else
        {

#line 46
            break;
        }

#line 46
    }

    uint _S6 = (_slang_committed_intersection_type_to_uint((&query_0)->get_committed_intersection_type()));

#line 48
    if(_S6 != 1U)
    {

#line 49
        return gpMiss_0();
    }
    float t_1 = ((&query_0)->get_committed_distance());
    uint instance_0 = ((&query_0)->get_committed_user_instance_id());
    uint primitive_0 = ((&query_0)->get_committed_primitive_id());
    uint _S7 = instance_0 * 4U;

)cy_msl"
    R"cy_msl(#line 54
    uint4 record_0 = kernelContext_2->giHardware_0->instances_0[_S7];
    uint first_0 = record_0.y + primitive_0 * 3U;
    uint _S8 = record_0.x;

#line 56
    float3 _S9 = ghVertex_0(_S8 + kernelContext_2->giHardware_0->indices_0[first_0], kernelContext_2);

#line 56
    float3 _S10 = ghVertex_0(_S8 + kernelContext_2->giHardware_0->indices_0[first_0 + 1U], kernelContext_2);

#line 56
    float3 _S11 = ghVertex_0(_S8 + kernelContext_2->giHardware_0->indices_0[first_0 + 2U], kernelContext_2);


    float4 c0_0 = (as_type<float4>((kernelContext_2->giHardware_0->instances_0[_S7 + 1U])));


    float3 local_0 = cross(_S10 - _S9, _S11 - _S9);
    float3 normal_1 = giNormalisedOr_0(c0_0.xyz * float3(local_0.x)  + (as_type<float4>((kernelContext_2->giHardware_0->instances_0[_S7 + 2U]))).xyz * float3(local_0.y)  + (as_type<float4>((kernelContext_2->giHardware_0->instances_0[_S7 + 3U]))).xyz * float3(local_0.z) , float3(0.0, 1.0, 0.0));

#line 63
    float3 normal_2;

    if((dot(normal_1, direction_1)) > 0.0)
    {

#line 65
        normal_2 = - normal_1;

#line 65
    }
    else
    {

#line 65
        normal_2 = normal_1;

#line 65
    }


    thread GiWorldHit_0 hit_1;
    (&hit_1)->hit_0 = true;
    (&hit_1)->t_0 = t_1;
    (&hit_1)->position_0 = origin_0 + direction_1 * float3(t_1) ;
    (&hit_1)->normal_0 = normal_2;


    (&hit_1)->confidence_0 = (as_type<float4>((kernelContext_2->giProbes_0->constants_0[25U]))).z - gpClamp_0(c0_0.w * 2.0, 0.0, 0.40000000596046448);
    return hit_1;
}


#line 399 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_1, float cell_0)
{

#line 400
    return int(floor(value_1 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_1, float3 normal_3, KernelContext_0 thread* kernelContext_3)
{

#line 410
    uint4 grid_0 = kernelContext_3->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 413
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S12 = radius_0 * radius_0;
    float _S13 = position_1.x;

#line 418
    int _S14 = giCell_0(_S13 - radius_0, cell_1);

#line 418
    float _S15 = position_1.y;

#line 418
    int _S16 = giCell_0(_S15 - radius_0, cell_1);
    float _S17 = position_1.z;

#line 419
    int _S18 = giCell_0(_S17 - radius_0, cell_1);
    int _S19 = giCell_0(_S13 + radius_0, cell_1);

#line 420
    int _S20 = giCell_0(_S15 + radius_0, cell_1);
    int _S21 = giCell_0(_S17 + radius_0, cell_1);

#line 421
    float bestScore_0 = 1.0e+09;

#line 421
    float bestAlignment_0 = 0.0;

#line 421
    uint best_0 = 4294967295U;

#line 421
    int z_1 = _S18;



    for(;;)
    {

#line 425
        if(z_1 <= _S21)
        {
        }
        else
        {

#line 425
            break;
        }

#line 425
        int y_1 = _S16;
        for(;;)
        {

#line 426
            if(y_1 <= _S20)
            {
            }
            else
            {

#line 426
                break;
            }

#line 426
            float bestScore_1 = bestScore_0;

#line 426
            float bestAlignment_1 = bestAlignment_0;

#line 426
            uint best_1 = best_0;

#line 426
            int x_1 = _S14;
            for(;;)
            {

#line 427
                if(x_1 <= _S19)
                {
                }
                else
                {

#line 427
                    break;
                }
                uint _S22 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 429
                uint first_1 = kernelContext_3->giScene_0->gridRanges_0[_S22];
                uint _S23 = kernelContext_3->giScene_0->gridRanges_0[_S22 + 1U];

#line 430
                float bestScore_2 = bestScore_1;

#line 430
                float bestAlignment_2 = bestAlignment_1;

#line 430
                uint best_2 = best_1;

#line 430
                uint item_0 = first_1;
                for(;;)
                {

#line 431
                    if(item_0 < (first_1 + _S23))
                    {
                    }
                    else
                    {

#line 431
                        break;
                    }

#line 432
                    uint handle_0 = kernelContext_3->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_3->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 434
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }



                    uint _S24 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_3->giScene_0->cards_0[_S24 + 1U].xyz, normal_3);
                    if(alignment_0 <= 0.25)
                    {

#line 439
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }

#line 441
                    float3 offset_0 = kernelContext_3->giScene_0->cards_0[_S24].xyz - position_1;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S12)
                    {

#line 444
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }

#line 447
                    float score_0 = squared_1 / alignment_0;

#line 447
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 448
                        better_0 = score_0 < bestScore_2;

#line 448
                    }
                    else
                    {

#line 449
                        if(alignment_0 != bestAlignment_2)
                        {

#line 449
                            better_0 = alignment_0 > bestAlignment_2;

#line 449
                        }
                        else
                        {

#line 449
                            better_0 = handle_0 < best_2;

#line 449
                        }

#line 448
                    }

#line 448
                    float bestScore_3;

#line 448
                    float bestAlignment_3;

#line 448
                    uint best_3;


                    if(better_0)
                    {

#line 451
                        bestScore_3 = score_0;

#line 451
                        bestAlignment_3 = alignment_0;

#line 451
                        best_3 = handle_0;

#line 451
                    }
                    else
                    {

)cy_msl"
    R"cy_msl(#line 451
                        bestScore_3 = bestScore_2;

#line 451
                        bestAlignment_3 = bestAlignment_2;

#line 451
                        best_3 = best_2;

#line 451
                    }

#line 451
                    bestScore_2 = bestScore_3;

#line 451
                    bestAlignment_2 = bestAlignment_3;

#line 451
                    best_2 = best_3;

#line 431
                    item_0 = item_0 + 1U;

#line 431
                }

#line 427
                int x_2 = x_1 + int(1);

#line 427
                bestScore_1 = bestScore_2;

#line 427
                bestAlignment_1 = bestAlignment_2;

#line 427
                best_1 = best_2;

#line 427
                x_1 = x_2;

#line 427
            }

#line 426
            int y_2 = y_1 + int(1);

#line 426
            bestScore_0 = bestScore_1;

#line 426
            bestAlignment_0 = bestAlignment_1;

#line 426
            best_0 = best_1;

#line 426
            y_1 = y_2;

#line 426
        }

#line 425
        z_1 = z_1 + int(1);

#line 425
    }

#line 460
    return best_0;
}


#line 273 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
void gpLoadPayload_0(uint probe_0, uint floats_0, array<float, int(48)> thread* payload_0, KernelContext_0 thread* kernelContext_4)
{

#line 273
    uint index_0 = 0U;
    for(;;)
    {

#line 274
        if(index_0 < 48U)
        {
        }
        else
        {

#line 274
            break;
        }

#line 274
        float _S25;
        if(index_0 < floats_0)
        {

#line 275
            _S25 = *(kernelContext_4->giProbes_0->payloads_0+(probe_0 * floats_0 + index_0));

#line 275
        }
        else
        {

#line 275
            _S25 = 0.0;

#line 275
        }

#line 275
        (*payload_0)[index_0] = _S25;

#line 274
        index_0 = index_0 + 1U;

#line 274
    }


    return;
}


#line 114
constant array<float3, int(6)> kGpAxes_0 = { float3(1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, -1.0, 0.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, -1.0) };

#line 110
float3 gpMax0_0(float3 v_1)
{

#line 111
    return float3(max(0.0, v_1.x), max(0.0, v_1.y), max(0.0, v_1.z));
}


#line 154
void gpOctahedralUv_0(float3 direction_2, float thread* u_0, float thread* v_2)
{

#line 155
    float sum_0 = abs(direction_2.x) + abs(direction_2.y) + abs(direction_2.z);

#line 155
    float3 p_0;
    if(sum_0 > 0.0)
    {

#line 156
        p_0 = direction_2 / float3(sum_0) ;

#line 156
    }
    else
    {

#line 156
        p_0 = float3(0.0, 0.0, 1.0);

#line 156
    }
    float _S26 = p_0.x;
    float _S27 = p_0.y;

#line 158
    float px_0;

#line 158
    float py_0;
    if((p_0.z) < 0.0)
    {

#line 160
        float _S28 = 1.0 - abs(_S27);

#line 160
        if(_S26 >= 0.0)
        {

#line 160
            px_0 = 1.0;

#line 160
        }
        else
        {

#line 160
            px_0 = -1.0;

#line 160
        }

#line 160
        float fx_0 = _S28 * px_0;
        float _S29 = 1.0 - abs(_S26);

#line 161
        if(_S27 >= 0.0)
        {

#line 161
            px_0 = 1.0;

#line 161
        }
        else
        {

#line 161
            px_0 = -1.0;

#line 161
        }

#line 161
        float fy_0 = _S29 * px_0;

#line 161
        px_0 = fx_0;

#line 161
        py_0 = fy_0;

#line 159
    }
    else
    {

#line 159
        px_0 = _S26;

#line 159
        py_0 = _S27;

#line 159
    }

#line 165
    *u_0 = gpClamp_0(px_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    *v_2 = gpClamp_0(py_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    return;
}


#line 220
float3 gpDecode_0(uint encoding_0, const array<float, int(48)> thread* payload_1, float3 direction_3)
{

#line 220
    uint c_0;
    if(encoding_0 == 0U)
    {



        array<float, int(4)> _S30 = { 0.28209498524665833, 0.32573533058166504 * direction_3.y, 0.32573533058166504 * direction_3.z, 0.32573533058166504 * direction_3.x };

        thread float3 result_0 = float3(0.0, 0.0, 0.0);

#line 228
        c_0 = 0U;
        for(;;)
        {

#line 229
            if(c_0 < 4U)
            {
            }
            else
            {

#line 229
                break;
            }

#line 230
            uint _S31 = c_0 * 3U;

#line 230
            result_0.x = result_0.x + (*payload_1)[_S31] * _S30[c_0];
            result_0.y = result_0.y + (*payload_1)[_S31 + 1U] * _S30[c_0];
            result_0.z = result_0.z + (*payload_1)[_S31 + 2U] * _S30[c_0];

#line 229
            c_0 = c_0 + 1U;

#line 229
        }

#line 234
        return gpMax0_0(result_0);
    }
    if(encoding_0 == 1U)
    {

#line 237
        thread float u_1 = 0.0;
        thread float v_3 = 0.0;
        gpOctahedralUv_0(direction_3, &u_1, &v_3);
        uint x0_0 = uint(u_1);
        uint y0_0 = uint(v_3);
        uint _S32 = min(x0_0 + 1U, 3U);

)cy_msl"
    R"cy_msl(

        uint _S33 = y0_0 * 4U;

        uint _S34 = min(y0_0 + 1U, 3U) * 4U;

        uint _S35 = (_S33 + x0_0) * 3U;

#line 250
        float3 f00_0 = float3((*payload_1)[_S35], (*payload_1)[_S35 + 1U], (*payload_1)[_S35 + 2U]);
        uint _S36 = (_S33 + _S32) * 3U;
        uint _S37 = (_S34 + x0_0) * 3U;

#line 252
        float3 f01_0 = float3((*payload_1)[_S37], (*payload_1)[_S37 + 1U], (*payload_1)[_S37 + 2U]);
        uint _S38 = (_S34 + _S32) * 3U;

#line 253
        float3 _S39 = float3((u_1 - float(x0_0))) ;
        float3 top_0 = f00_0 + (float3((*payload_1)[_S36], (*payload_1)[_S36 + 1U], (*payload_1)[_S36 + 2U]) - f00_0) * _S39;

        return top_0 + (f01_0 + (float3((*payload_1)[_S38], (*payload_1)[_S38 + 1U], (*payload_1)[_S38 + 2U]) - f01_0) * _S39 - top_0) * float3((v_3 - float(y0_0))) ;
    }
    float3 _S40 = float3(0.0, 0.0, 0.0);

#line 258
    float3 result_1 = _S40;

#line 258
    c_0 = 0U;

#line 258
    float total_0 = 0.0;

    for(;;)
    {

#line 260
        if(c_0 < 6U)
        {
        }
        else
        {

#line 260
            break;
        }

#line 261
        float _S41 = max(0.0, dot(kGpAxes_0[c_0], direction_3));
        if(_S41 <= 0.0)
        {

#line 263
            c_0 = c_0 + 1U;

#line 260
            continue;
        }



        uint _S42 = c_0 * 3U;

        float total_1 = total_0 + _S41;

#line 267
        result_1 = result_1 + float3((*payload_1)[_S42], (*payload_1)[_S42 + 1U], (*payload_1)[_S42 + 2U]) * float3(_S41) ;

#line 267
        total_0 = total_1;

#line 260
        c_0 = c_0 + 1U;

#line 260
    }

#line 269
    if(total_0 > 0.0)
    {

#line 269
        result_1 = result_1 / float3(total_0) ;

#line 269
    }
    else
    {

#line 269
        result_1 = _S40;

#line 269
    }

#line 269
    return result_1;
}


#line 138
float3 gpOctahedralDirection_0(uint texel_0)
{

    float u_2 = float(texel_0 % 4U) / 3.0 * 2.0 - 1.0;
    float v_4 = float(texel_0 / 4U) / 3.0 * 2.0 - 1.0;
    float _S43 = 1.0 - abs(u_2);

#line 143
    float _S44 = abs(v_4);

#line 143
    float _S45 = _S43 - _S44;

#line 143
    thread float3 direction_4 = float3(u_2, v_4, _S45);
    if(_S45 < 0.0)
    {

#line 145
        float _S46 = 1.0 - _S44;

#line 145
        float _S47;

#line 145
        if(u_2 >= 0.0)
        {

#line 145
            _S47 = 1.0;

#line 145
        }
        else
        {

#line 145
            _S47 = -1.0;

#line 145
        }

#line 145
        float fx_1 = _S46 * _S47;
        if(v_4 >= 0.0)
        {

#line 146
            _S47 = 1.0;

#line 146
        }
        else
        {

#line 146
            _S47 = -1.0;

#line 146
        }

#line 146
        float fy_1 = _S43 * _S47;
        direction_4.x = fx_1;
        direction_4.y = fy_1;

#line 144
    }

#line 150
    return giNormalisedOr_0(direction_4, float3(0.0, 0.0, 1.0));
}


#line 169
void gpEncode_0(uint encoding_1, array<float, int(48)> thread* payload_2, float3 direction_5, float3 radiance_0)
{

#line 169
    uint c_1;
    if(encoding_1 == 0U)
    {

        array<float, int(4)> _S48 = { 0.28209498524665833, 0.48860299587249756 * direction_5.y, 0.48860299587249756 * direction_5.z, 0.48860299587249756 * direction_5.x };

#line 173
        c_1 = 0U;
        for(;;)
        {

#line 174
            if(c_1 < 4U)
            {
            }
            else
            {

#line 174
                break;
            }
            uint _S49 = c_1 * 3U;

#line 176
            (*payload_2)[_S49] = (*payload_2)[_S49] + radiance_0.x * _S48[c_1];
            (*payload_2)[_S49 + 1U] = (*payload_2)[_S49 + 1U] + radiance_0.y * _S48[c_1];
            (*payload_2)[_S49 + 2U] = (*payload_2)[_S49 + 2U] + radiance_0.z * _S48[c_1];

#line 174
            c_1 = c_1 + 1U;

#line 174
        }

#line 180
        return;
    }
    if(encoding_1 == 1U)
    {

#line 182
        c_1 = 0U;
        for(;;)
        {

#line 183
            if(c_1 < 16U)
            {
            }
            else
            {

#line 183
                break;
            }

#line 184
            float _S50 = max(0.0, dot(gpOctahedralDirection_0(c_1), direction_5));
            if(_S50 <= 0.0)
            {

#line 186
                c_1 = c_1 + 1U;

#line 183
                continue;
            }

#line 189
            uint _S51 = c_1 * 3U;

#line 189
            (*payload_2)[_S51] = (*payload_2)[_S51] + radiance_0.x * _S50;
            (*payload_2)[_S51 + 1U] = (*payload_2)[_S51 + 1U] + radiance_0.y * _S50;
            (*payload_2)[_S51 + 2U] = (*payload_2)[_S51 + 2U] + radiance_0.z * _S50;

#line 183
            c_1 = c_1 + 1U;

#line 183
        }

#line 193
        return;
    }

#line 193
    c_1 = 0U;

    for(;;)
    {

#line 195
        if(c_1 < 6U)
        {
        }
        else
        {

#line 195
            break;
        }

#line 196
        float _S52 = max(0.0, dot(kGpAxes_0[c_1], direction_5));
        if(_S52 <= 0.0)
        {

#line 198
            c_1 = c_1 + 1U;

#line 195
            continue;
        }

#line 201
        uint _S53 = c_1 * 3U;

#line 201
        (*payload_2)[_S53] = (*payload_2)[_S53] + radiance_0.x * _S52;
        (*payload_2)[_S53 + 1U] = (*payload_2)[_S53 + 1U] + radiance_0.y * _S52;
        (*payload_2)[_S53 + 2U] = (*payload_2)[_S53 + 2U] + radiance_0.z * _S52;

)cy_msl"
    R"cy_msl(#line 195
        c_1 = c_1 + 1U;

#line 195
    }

#line 205
    return;
}


#line 207
void gpNormalise_0(uint encoding_2, uint floats_1, array<float, int(48)> thread* payload_3, float totalWeight_0)
{

#line 208
    if(totalWeight_0 <= 0.0)
    {

#line 209
        return;
    }

#line 209
    float _S54;


    if(encoding_2 == 0U)
    {

#line 212
        _S54 = 12.56637096405029297 / totalWeight_0;

#line 212
    }
    else
    {

#line 212
        _S54 = 3.14159274101257324 / totalWeight_0;

#line 212
    }

#line 212
    uint index_1 = 0U;

    for(;;)
    {

#line 214
        if(index_1 < floats_1)
        {
        }
        else
        {

#line 214
            break;
        }

#line 215
        (*payload_3)[index_1] = (*payload_3)[index_1] * _S54;

#line 214
        index_1 = index_1 + 1U;

#line 214
    }


    return;
}


#line 349
void gpGatherProbe_0(uint index_2, uint lane_0, KernelContext_0 thread* kernelContext_5)
{

#line 350
    uint4 counts_0 = kernelContext_5->giProbes_0->constants_0[0U];
    if(index_2 >= (counts_0.w))
    {

#line 352
        return;
    }
    uint encoding_3 = counts_0.x;
    uint floats_2 = counts_0.y;
    uint _S55 = min(max(1U, counts_0.z), 64U);
    float maxDistance_1 = (as_type<float>((kernelContext_5->giProbes_0->constants_0[1U].x)));
    uint probe_1 = kernelContext_5->giProbes_0->selection_0[index_2];
    uint _S56 = probe_1 * 4U;

#line 359
    float3 position_2 = (*(kernelContext_5->giProbes_0->probes_0+_S56)).xyz;

#line 359
    float3 previous_0;

#line 359
    float totalWeight_1;

    if(lane_0 < _S55)
    {

#line 362
        uint _S57 = lane_0 * 2U;
        float3 unit_0 = kernelContext_5->giProbes_0->directions_0[_S57 + 1U].xyz;

#line 363
        float3 _S58 = gpSky_0(kernelContext_5->giProbes_0->directions_0[_S57].xyz, kernelContext_5);

#line 363
        GiWorldHit_0 _S59 = GiHardwareTier_trace_0(position_2, unit_0, maxDistance_1, kernelContext_5);



        if(_S59.hit_0)
        {

#line 367
            uint _S60 = giFindCard_0(_S59.position_0, _S59.normal_0, kernelContext_5);



            if(_S60 != 4294967295U)
            {

#line 371
                previous_0 = (*(kernelContext_5->giScene_0->cardState_0+_S60 * 2U)).xyz;

#line 371
            }
            else
            {

#line 371
                previous_0 = float3(0.0, 0.0, 0.0);

#line 371
            }

#line 371
            totalWeight_1 = _S59.t_0;

#line 367
        }
        else
        {

#line 367
            previous_0 = _S58;

#line 367
            totalWeight_1 = maxDistance_1;

#line 367
        }

#line 374
        (*kernelContext_5->gpRayRadiance_0)[lane_0] = previous_0;
        (*kernelContext_5->gpRayDistance_0)[lane_0] = totalWeight_1;

#line 361
    }

#line 377
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if(lane_0 != 0U)
    {

#line 379
        return;
    }

    thread array<float, int(48)> data_0;

#line 382
    gpLoadPayload_0(probe_1, floats_2, &data_0, kernelContext_5);

    float3 _S61 = float3(0.0, 0.0, 0.0);

#line 384
    previous_0 = _S61;

#line 384
    uint axis_0 = 0U;
    for(;;)
    {

#line 385
        if(axis_0 < 6U)
        {
        }
        else
        {

#line 385
            break;
        }

#line 385
        thread array<float, int(48)> _S62 = data_0;

#line 385
        float3 _S63 = gpDecode_0(encoding_3, &_S62, kGpAxes_0[axis_0]);
        float3 _S64 = previous_0 + _S63;

#line 385
        uint axis_1 = axis_0 + 1U;

#line 385
        previous_0 = _S64;

#line 385
        axis_0 = axis_1;

#line 385
    }

#line 385
    uint i_0 = 0U;


    for(;;)
    {

#line 388
        if(i_0 < 48U)
        {
        }
        else
        {

#line 388
            break;
        }

#line 389
        data_0[i_0] = 0.0;

#line 388
        i_0 = i_0 + 1U;

#line 388
    }


    thread array<float, int(6)> axisDistance_0;
    thread array<float, int(6)> axisWeight_0;

#line 392
    uint a_0 = 0U;
    for(;;)
    {

#line 393
        if(a_0 < 6U)
        {
        }
        else
        {

#line 393
            break;
        }

#line 394
        axisDistance_0[a_0] = 0.0;
        axisWeight_0[a_0] = 0.0;

#line 393
        a_0 = a_0 + 1U;

#line 393
    }

#line 393
    uint ray_1 = 0U;

#line 393
    totalWeight_1 = 0.0;

#line 399
    for(;;)
    {

#line 399
        if(ray_1 < _S55)
        {
        }
        else
        {

#line 399
            break;
        }

#line 400
        float3 direction_6 = kernelContext_5->giProbes_0->directions_0[ray_1 * 2U].xyz;
        gpEncode_0(encoding_3, &data_0, direction_6, (*kernelContext_5->gpRayRadiance_0)[ray_1]);
        float totalWeight_2 = totalWeight_1 + 1.0;

)cy_msl"
    R"cy_msl(#line 402
        a_0 = 0U;
        for(;;)
        {

#line 403
            if(a_0 < 6U)
            {
            }
            else
            {

#line 403
                break;
            }

#line 404
            float _S65 = max(0.0, dot(kGpAxes_0[a_0], direction_6));
            axisDistance_0[a_0] = axisDistance_0[a_0] + (*kernelContext_5->gpRayDistance_0)[ray_1] * _S65;
            axisWeight_0[a_0] = axisWeight_0[a_0] + _S65;

#line 403
            a_0 = a_0 + 1U;

#line 403
        }

#line 399
        ray_1 = ray_1 + 1U;

#line 399
        totalWeight_1 = totalWeight_2;

#line 399
    }

#line 409
    gpNormalise_0(encoding_3, floats_2, &data_0, totalWeight_1);

#line 409
    float3 current_0 = _S61;

#line 409
    axis_0 = 0U;


    for(;;)
    {

#line 412
        if(axis_0 < 6U)
        {
        }
        else
        {

#line 412
            break;
        }

#line 412
        thread array<float, int(48)> _S66 = data_0;

#line 412
        float3 _S67 = gpDecode_0(encoding_3, &_S66, kGpAxes_0[axis_0]);
        float3 _S68 = current_0 + _S67;

#line 412
        uint axis_2 = axis_0 + 1U;

#line 412
        current_0 = _S68;

#line 412
        axis_0 = axis_2;

#line 412
    }


    float magnitude_0 = length(previous_0) + length(current_0);

#line 415
    float error_0;
    if(magnitude_0 > 0.00000999999974738)
    {

#line 416
        error_0 = length(current_0 - previous_0) / magnitude_0;

#line 416
    }
    else
    {

#line 416
        error_0 = 0.0;

#line 416
    }

    uint record_1 = index_2 * 14U;

#line 418
    uint v_5 = 0U;
    for(;;)
    {

#line 419
        if(v_5 < 12U)
        {
        }
        else
        {

#line 419
            break;
        }
        uint _S69 = v_5 * 4U;

#line 420
        *(kernelContext_5->giProbes_0->results_0+(record_1 + v_5)) = float4(data_0[_S69], data_0[_S69 + 1U], data_0[_S69 + 2U], data_0[_S69 + 3U]);

#line 419
        v_5 = v_5 + 1U;

#line 419
    }



    thread array<float, int(6)> distances_0;

#line 423
    a_0 = 0U;
    for(;;)
    {

#line 424
        if(a_0 < 6U)
        {
        }
        else
        {

#line 424
            break;
        }

#line 425
        if((axisWeight_0[a_0]) > 0.0)
        {

#line 425
            totalWeight_1 = axisDistance_0[a_0] / axisWeight_0[a_0];

#line 425
        }
        else
        {

#line 425
            totalWeight_1 = maxDistance_1;

#line 425
        }

#line 425
        distances_0[a_0] = totalWeight_1;

#line 424
        a_0 = a_0 + 1U;

#line 424
    }


    *(kernelContext_5->giProbes_0->results_0+(record_1 + 12U)) = float4(distances_0[int(0)], distances_0[int(1)], distances_0[int(2)], distances_0[int(3)]);
    *(kernelContext_5->giProbes_0->results_0+(record_1 + 13U)) = float4(distances_0[int(4)], distances_0[int(5)], error_0, (as_type<float>((probe_1))));

#line 428
    uint f_0 = 0U;
    for(;;)
    {

#line 429
        if(f_0 < floats_2)
        {
        }
        else
        {

#line 429
            break;
        }

#line 430
        *(kernelContext_5->giProbes_0->payloads_0+(probe_1 * floats_2 + f_0)) = data_0[f_0];

#line 429
        f_0 = f_0 + 1U;

#line 429
    }


    *(kernelContext_5->giProbes_0->probes_0+(_S56 + 1U)) = float4((as_type<float>((kernelContext_5->giDispatch_0->frame_0))), 1.0, 1.0, 0.0);
    *(kernelContext_5->giProbes_0->probes_0+(_S56 + 2U)) = float4(distances_0[int(0)], distances_0[int(1)], distances_0[int(2)], distances_0[int(3)]);

    *(kernelContext_5->giProbes_0->probes_0+(_S56 + 3U)) = float4(distances_0[int(4)], distances_0[int(5)], 0.0, 0.0);
    return;
}


#line 85 "src/rendering/gi_gpu/shaders/gi_hardware.slang"
[[kernel]] void cyGiGatherProbesHardware(uint3 group_0 [[threadgroup_position_in_grid]], uint3 lane_1 [[thread_position_in_threadgroup]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiDispatch_0 constant* giDispatch_1 [[buffer(1)]], GiHardwareSet_default_0 constant* giHardware_1 [[buffer(3)]])
{

#line 85
    thread KernelContext_0 kernelContext_6;

#line 85
    (&kernelContext_6)->giProbes_0 = giProbes_1;

#line 85
    (&kernelContext_6)->giScene_0 = giScene_1;

#line 85
    (&kernelContext_6)->giDispatch_0 = giDispatch_1;

#line 85
    (&kernelContext_6)->giHardware_0 = giHardware_1;

#line 85
    threadgroup array<float3, int(64)> gpRayRadiance_1;

#line 85
    (&kernelContext_6)->gpRayRadiance_0 = &gpRayRadiance_1;

#line 85
    threadgroup array<float, int(64)> gpRayDistance_1;

#line 85
    (&kernelContext_6)->gpRayDistance_0 = &gpRayDistance_1;

#line 85
    gpGatherProbe_0(group_0.x, lane_1.x, &kernelContext_6);

    return;
}

)cy_msl";

/// cyGiTraceHardware.metal, 19960 bytes.
inline constexpr char kGiTraceHardwareMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

            raytracing::intersection_params _slang_ray_flags_to_intersection_params(uint flags){
                raytracing::intersection_params params;
                if (flags & 0x01) /* RAY_FLAG_FORCE_OPAQUE */
                    params.force_opacity(raytracing::forced_opacity::opaque);
                if (flags & 0x02) /* RAY_FLAG_FORCE_NON_OPAQUE */
                    params.force_opacity(raytracing::forced_opacity::non_opaque);
                if (flags & 0x10) /* RAY_FLAG_CULL_BACK_FACING_TRIANGLES */
                    params.set_triangle_cull_mode(raytracing::triangle_cull_mode::back);
                if (flags & 0x20) /* RAY_FLAG_CULL_FRONT_FACING_TRIANGLES */
                    params.set_triangle_cull_mode(raytracing::triangle_cull_mode::front);
                if (flags & 0x40) /* RAY_FLAG_CULL_OPAQUE */
                    params.set_opacity_cull_mode(raytracing::opacity_cull_mode::opaque);
                if (flags & 0x80) /* RAY_FLAG_CULL_NON_OPAQUE */
                    params.set_opacity_cull_mode(raytracing::opacity_cull_mode::non_opaque);
                if (flags & 0x100) /* RAY_FLAG_SKIP_TRIANGLES */
                    params.set_geometry_cull_mode(raytracing::geometry_cull_mode::triangle);
                if (flags & 0x200) /* RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES */
                    params.set_geometry_cull_mode(raytracing::geometry_cull_mode::bounding_box);
                return params;
            }
        

                uint _slang_committed_intersection_type_to_uint(raytracing::intersection_type type){
                    switch(type){
                        case raytracing::intersection_type::none:
                            return 0; /* COMMITTED_NOTHING */
                        case raytracing::intersection_type::triangle:
                            return 1; /* COMMITTED_TRIANGLE_HIT */
                        case raytracing::intersection_type::bounding_box:
                            return 2; /* COMMITTED_PROCEDURAL_PRIMITIVE_HIT */
                        default:
                            return 0xFFFFFFFF; // Invalid value to indicate error case, should never be returned by the API
                    }
                }
            

#line 678 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiTierSample_0
{
    float3 radiance_0;
    float confidence_0;
    float hitDistance_0;
    uint source_0;
};


#line 77 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiDispatch_0
{
    uint count_0;
    uint frame_0;
    uint reserved0_0;
    uint reserved1_0;
};


#line 67 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiProbeSet_default_0
{
    uint4 device* constants_0;
    float4 device* probes_0;
    float device* payloads_0;
    uint device* selection_0;
    float4 device* directions_0;
    float4 device* results_0;
    uint device* layout_0;
    float4 device* queries_0;
    float4 device* samples_0;
    float4 device* tierRays_0;
    float4 device* tierResults_0;
    float device* screenDepth_0;
    float4 device* screenColour_0;
    float device* screenHzb_0;
    uint device* counters_0;
};


#line 74 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiSceneSet_default_0
{
    uint4 device* constants_1;
    uint device* pageTable_0;
    float device* bricks_0;
    float4 device* cards_0;
    float4 device* cardState_0;
    uint device* gridRanges_0;
    uint device* gridItems_0;
    float4 device* lights_0;
    float device* shadowDepths_0;
    uint device* selection_1;
    float4 device* results_1;
    float4 device* rays_0;
    float4 device* hits_0;
};


#line 74
struct GiHardwareSet_default_0
{
    metal::raytracing::acceleration_structure<metal::raytracing::instancing> topLevel_0;
    uint4 device* instances_0;
    float device* positions_0;
    uint device* indices_0;
};


#line 74
struct KernelContext_0
{
    GiDispatch_0 constant* giDispatch_0;
    GiProbeSet_default_0 constant* giProbes_0;
    GiSceneSet_default_0 constant* giScene_0;
    GiHardwareSet_default_0 constant* giHardware_0;
};


#line 281 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiWorldHit_0
{
    bool hit_0;
    float t_0;
    float3 position_0;
    float3 normal_0;
    float confidence_1;
};


#line 19393 "hlsl.meta.slang"
struct RayDesc_0
{
    float3 Origin_0;
    float TMin_0;
    float3 Direction_0;
    float TMax_0;
};


#line 290 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiWorldHit_0 gpMiss_0()
{

#line 291
    thread GiWorldHit_0 miss_0;
    (&miss_0)->hit_0 = false;
    (&miss_0)->t_0 = 0.0;
    (&miss_0)->position_0 = float3(0.0, 0.0, 0.0);
    (&miss_0)->normal_0 = float3(0.0, 1.0, 0.0);
    (&miss_0)->confidence_1 = 1.0;
    return miss_0;
}


#line 32 "src/rendering/gi_gpu/shaders/gi_hardware.slang"
float3 ghVertex_0(uint vertex_0, KernelContext_0 thread* kernelContext_0)
{

#line 33
    uint _S1 = vertex_0 * 3U;

#line 33
    return float3(kernelContext_0->giHardware_0->positions_0[_S1], kernelContext_0->giHardware_0->positions_0[_S1 + 1U], kernelContext_0->giHardware_0->positions_0[_S1 + 2U]);
}


#line 95 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 96
    float squared_0 = dot(v_0, v_0);

#line 96
    float3 _S2;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 97
        _S2 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 97
    }
    else
    {

#line 97
        _S2 = fallback_0;

#line 97
    }

#line 97
    return _S2;
}


#line 106 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
float gpClamp_0(float value_0, float lo_0, float hi_0)
{

#line 106
    float _S3;
    if(value_0 < lo_0)
    {

#line 107
        _S3 = lo_0;

#line 107
    }
    else
    {

#line 107
        if(value_0 > hi_0)
        {

#line 107
            _S3 = hi_0;

#line 107
        }
        else
        {

#line 107
            _S3 = value_0;

#line 107
        }

#line 107
    }

#line 107
    return _S3;
}


#line 38 "src/rendering/gi_gpu/shaders/gi_hardware.slang"
GiWorldHit_0 GiHardwareTier_trace_0(float3 origin_0, float3 direction_0, float maxDistance_0, KernelContext_0 thread* kernelContext_1)
{

#line 39
    thread RayDesc_0 ray_0;
    (&ray_0)->Origin_0 = origin_0;
    (&ray_0)->Direction_0 = direction_0;
    (&ray_0)->TMin_0 = 0.00100000004749745;
    (&ray_0)->TMax_0 = maxDistance_0;
    thread raytracing::intersection_query<raytracing::triangle_data, raytracing::instancing> query_0;
    (&query_0)->reset({ (ray_0.Origin_0), (ray_0.Direction_0), (ray_0.TMin_0), (ray_0.TMax_0) }, (kernelContext_1->giHardware_0->topLevel_0), (255U), _slang_ray_flags_to_intersection_params((1U)));
    for(;;)
    {

#line 46
        bool _S4 = ((&query_0)->next());

#line 46
        if(_S4)
        {
        }
        else
        {

#line 46
            break;
        }

#line 46
    }

    uint _S5 = (_slang_committed_intersection_type_to_uint((&query_0)->get_committed_intersection_type()));

#line 48
    if(_S5 != 1U)
    {

#line 49
        return gpMiss_0();
    }
    float t_1 = ((&query_0)->get_committed_distance());
    uint instance_0 = ((&query_0)->get_committed_user_instance_id());
    uint primitive_0 = ((&query_0)->get_committed_primitive_id());
    uint _S6 = instance_0 * 4U;

#line 54
    uint4 record_0 = kernelContext_1->giHardware_0->instances_0[_S6];
    uint first_0 = record_0.y + primitive_0 * 3U;
    uint _S7 = record_0.x;

#line 56
    float3 _S8 = ghVertex_0(_S7 + kernelContext_1->giHardware_0->indices_0[first_0], kernelContext_1);

#line 56
    float3 _S9 = ghVertex_0(_S7 + kernelContext_1->giHardware_0->indices_0[first_0 + 1U], kernelContext_1);

#line 56
    float3 _S10 = ghVertex_0(_S7 + kernelContext_1->giHardware_0->indices_0[first_0 + 2U], kernelContext_1);

)cy_msl"
    R"cy_msl(
    float4 c0_0 = (as_type<float4>((kernelContext_1->giHardware_0->instances_0[_S6 + 1U])));


    float3 local_0 = cross(_S9 - _S8, _S10 - _S8);
    float3 normal_1 = giNormalisedOr_0(c0_0.xyz * float3(local_0.x)  + (as_type<float4>((kernelContext_1->giHardware_0->instances_0[_S6 + 2U]))).xyz * float3(local_0.y)  + (as_type<float4>((kernelContext_1->giHardware_0->instances_0[_S6 + 3U]))).xyz * float3(local_0.z) , float3(0.0, 1.0, 0.0));

#line 63
    float3 normal_2;

    if((dot(normal_1, direction_0)) > 0.0)
    {

#line 65
        normal_2 = - normal_1;

#line 65
    }
    else
    {

#line 65
        normal_2 = normal_1;

#line 65
    }


    thread GiWorldHit_0 hit_1;
    (&hit_1)->hit_0 = true;
    (&hit_1)->t_0 = t_1;
    (&hit_1)->position_0 = origin_0 + direction_0 * float3(t_1) ;
    (&hit_1)->normal_0 = normal_2;


    (&hit_1)->confidence_1 = (as_type<float4>((kernelContext_1->giProbes_0->constants_0[25U]))).z - gpClamp_0(c0_0.w * 2.0, 0.0, 0.40000000596046448);
    return hit_1;
}


#line 78
uint GiHardwareTier_source_0()
{

#line 79
    return 7U;
}


#line 685 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpTierSample_0(uint source_1)
{

#line 686
    thread GiTierSample_0 sample_0;
    (&sample_0)->radiance_0 = float3(0.0, 0.0, 0.0);
    (&sample_0)->confidence_0 = 0.0;
    (&sample_0)->hitDistance_0 = -1.0;
    (&sample_0)->source_0 = source_1;
    return sample_0;
}


#line 121
float3 gpSky_0(float3 direction_1, KernelContext_0 thread* kernelContext_2)
{

#line 122
    float4 zenith_0 = (as_type<float4>((kernelContext_2->giProbes_0->constants_0[2U])));
    float3 horizon_0 = (as_type<float3>((kernelContext_2->giProbes_0->constants_0[3U].xyz)));
    float3 ground_0 = (as_type<float3>((kernelContext_2->giProbes_0->constants_0[4U].xyz)));

    float _S11 = giNormalisedOr_0(direction_1, float3(0.0, 1.0, 0.0)).y;

#line 126
    if(_S11 < 0.0)
    {

#line 127
        return ground_0 * float3(zenith_0.w) ;
    }

    return (horizon_0 + (zenith_0.xyz - horizon_0) * float3(gpClamp_0(pow(_S11, 0.44999998807907104), 0.0, 1.0)) ) * float3(zenith_0.w) ;
}


#line 399 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_1, float cell_0)
{

#line 400
    return int(floor(value_1 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_1, float3 normal_3, KernelContext_0 thread* kernelContext_3)
{

#line 410
    uint4 grid_0 = kernelContext_3->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 413
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S12 = radius_0 * radius_0;
    float _S13 = position_1.x;

#line 418
    int _S14 = giCell_0(_S13 - radius_0, cell_1);

#line 418
    float _S15 = position_1.y;

#line 418
    int _S16 = giCell_0(_S15 - radius_0, cell_1);
    float _S17 = position_1.z;

#line 419
    int _S18 = giCell_0(_S17 - radius_0, cell_1);
    int _S19 = giCell_0(_S13 + radius_0, cell_1);

#line 420
    int _S20 = giCell_0(_S15 + radius_0, cell_1);
    int _S21 = giCell_0(_S17 + radius_0, cell_1);

#line 421
    float bestScore_0 = 1.0e+09;

#line 421
    float bestAlignment_0 = 0.0;

#line 421
    uint best_0 = 4294967295U;

#line 421
    int z_1 = _S18;



    for(;;)
    {

#line 425
        if(z_1 <= _S21)
        {
        }
        else
        {

#line 425
            break;
        }

#line 425
        int y_1 = _S16;
        for(;;)
        {

#line 426
            if(y_1 <= _S20)
            {
            }
            else
            {

#line 426
                break;
            }

#line 426
            float bestScore_1 = bestScore_0;

#line 426
            float bestAlignment_1 = bestAlignment_0;

#line 426
            uint best_1 = best_0;

#line 426
            int x_1 = _S14;
            for(;;)
            {

#line 427
                if(x_1 <= _S19)
                {
                }
                else
                {

#line 427
                    break;
                }
                uint _S22 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 429
                uint first_1 = kernelContext_3->giScene_0->gridRanges_0[_S22];
                uint _S23 = kernelContext_3->giScene_0->gridRanges_0[_S22 + 1U];

#line 430
                float bestScore_2 = bestScore_1;

#line 430
                float bestAlignment_2 = bestAlignment_1;

#line 430
                uint best_2 = best_1;

#line 430
                uint item_0 = first_1;
                for(;;)
                {

#line 431
                    if(item_0 < (first_1 + _S23))
                    {
                    }
                    else
                    {

#line 431
                        break;
                    }

#line 432
                    uint handle_0 = kernelContext_3->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_3->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 434
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }



                    uint _S24 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_3->giScene_0->cards_0[_S24 + 1U].xyz, normal_3);
                    if(alignment_0 <= 0.25)
                    {

#line 439
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }

#line 441
                    float3 offset_0 = kernelContext_3->giScene_0->cards_0[_S24].xyz - position_1;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S12)
                    {

#line 444
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }

#line 447
                    float score_0 = squared_1 / alignment_0;

#line 447
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 448
                        better_0 = score_0 < bestScore_2;

#line 448
                    }
                    else
                    {

#line 449
                        if(alignment_0 != bestAlignment_2)
                        {

#line 449
                            better_0 = alignment_0 > bestAlignment_2;

#line 449
                        }
                        else
                        {

)cy_msl"
    R"cy_msl(#line 449
                            better_0 = handle_0 < best_2;

#line 449
                        }

#line 448
                    }

#line 448
                    float bestScore_3;

#line 448
                    float bestAlignment_3;

#line 448
                    uint best_3;


                    if(better_0)
                    {

#line 451
                        bestScore_3 = score_0;

#line 451
                        bestAlignment_3 = alignment_0;

#line 451
                        best_3 = handle_0;

#line 451
                    }
                    else
                    {

#line 451
                        bestScore_3 = bestScore_2;

#line 451
                        bestAlignment_3 = bestAlignment_2;

#line 451
                        best_3 = best_2;

#line 451
                    }

#line 451
                    bestScore_2 = bestScore_3;

#line 451
                    bestAlignment_2 = bestAlignment_3;

#line 451
                    best_2 = best_3;

#line 431
                    item_0 = item_0 + 1U;

#line 431
                }

#line 427
                int x_2 = x_1 + int(1);

#line 427
                bestScore_1 = bestScore_2;

#line 427
                bestAlignment_1 = bestAlignment_2;

#line 427
                best_1 = best_2;

#line 427
                x_1 = x_2;

#line 427
            }

#line 426
            int y_2 = y_1 + int(1);

#line 426
            bestScore_0 = bestScore_1;

#line 426
            bestAlignment_0 = bestAlignment_1;

#line 426
            best_0 = best_1;

#line 426
            y_1 = y_2;

#line 426
        }

#line 425
        z_1 = z_1 + int(1);

#line 425
    }

#line 460
    return best_0;
}


#line 765 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpWorldRadiance_0(const GiWorldHit_0 thread* hit_2, float3 direction_2, uint source_2, uint cacheFrame_0, KernelContext_0 thread* kernelContext_4)
{

#line 766
    thread GiTierSample_0 sample_1 = gpTierSample_0(source_2);
    if(!hit_2->hit_0)
    {

#line 768
        (&sample_1)->source_0 = 9U;

#line 768
        float3 _S25 = gpSky_0(direction_2, kernelContext_4);
        (&sample_1)->radiance_0 = _S25;
        (&sample_1)->confidence_0 = 1.0;
        (&sample_1)->hitDistance_0 = -1.0;
        return sample_1;
    }
    (&sample_1)->hitDistance_0 = hit_2->t_0;

#line 774
    uint _S26 = giFindCard_0(hit_2->position_0, hit_2->normal_0, kernelContext_4);

    if(_S26 != 4294967295U)
    {

#line 777
        uint _S27 = _S26 * 2U;

#line 777
        (&sample_1)->radiance_0 = (*(kernelContext_4->giScene_0->cardState_0+_S27)).xyz;
        uint last_0 = (as_type<uint>(((*(kernelContext_4->giScene_0->cardState_0+(_S27 + 1U))).w)));

#line 778
        float age_0;
        if(cacheFrame_0 > last_0)
        {

#line 779
            age_0 = float(cacheFrame_0 - last_0);

#line 779
        }
        else
        {

#line 779
            age_0 = 0.0;

#line 779
        }


        (&sample_1)->confidence_0 = hit_2->confidence_1 * (1.0 - gpClamp_0(age_0 / 240.0, 0.0, 1.0) * 0.60000002384185791);
        return sample_1;
    }
    (&sample_1)->confidence_0 = hit_2->confidence_1 * 0.20000000298023224;
    return sample_1;
}


GiTierSample_0 gpWorldTrace_0(float3 origin_1, float3 unit_0, float reach_0, KernelContext_0 thread* kernelContext_5)
{

#line 791
    uint cacheFrame_1 = kernelContext_5->giProbes_0->constants_0[24U].w;

#line 791
    GiWorldHit_0 _S28 = GiHardwareTier_trace_0(origin_1, unit_0, reach_0, kernelContext_5);

    uint _S29 = GiHardwareTier_source_0();

#line 793
    thread GiWorldHit_0 _S30 = _S28;

#line 793
    GiTierSample_0 _S31 = gpWorldRadiance_0(&_S30, unit_0, _S29, cacheFrame_1, kernelContext_5);

#line 793
    return _S31;
}


#line 872
void gpWriteTierResult_0(uint ray_1, const GiTierSample_0 thread* sample_2, uint reads_0, KernelContext_0 thread* kernelContext_6)
{

#line 873
    uint _S32 = ray_1 * 2U;

#line 873
    *(kernelContext_6->giProbes_0->tierResults_0+_S32) = float4(sample_2->radiance_0, sample_2->confidence_0);
    *(kernelContext_6->giProbes_0->tierResults_0+(_S32 + 1U)) = float4(sample_2->hitDistance_0, (as_type<float>((sample_2->source_0))), (as_type<float>((reads_0))), 0.0);

    return;
}


#line 91 "src/rendering/gi_gpu/shaders/gi_hardware.slang"
[[kernel]] void cyGiTraceHardware(uint3 thread_0 [[thread_position_in_grid]], GiDispatch_0 constant* giDispatch_1 [[buffer(1)]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiHardwareSet_default_0 constant* giHardware_1 [[buffer(3)]])
{

#line 91
    thread KernelContext_0 kernelContext_7;

#line 91
    (&kernelContext_7)->giDispatch_0 = giDispatch_1;

#line 91
    (&kernelContext_7)->giProbes_0 = giProbes_1;

#line 91
    (&kernelContext_7)->giScene_0 = giScene_1;

#line 91
    (&kernelContext_7)->giHardware_0 = giHardware_1;
    uint ray_2 = thread_0.x;
    if(ray_2 >= (giDispatch_1->count_0))
    {

#line 94
        return;
    }
    uint _S33 = ray_2 * 2U;

#line 96
    float4 origin_2 = (&kernelContext_7)->giProbes_0->tierRays_0[_S33];

#line 96
    GiTierSample_0 _S34 = gpWorldTrace_0(origin_2.xyz, giNormalisedOr_0((&kernelContext_7)->giProbes_0->tierRays_0[_S33 + 1U].xyz, float3(0.0, 0.0, -1.0)), origin_2.w, &kernelContext_7);

#line 96
    thread GiTierSample_0 _S35 = _S34;

#line 96
    gpWriteTierResult_0(ray_2, &_S35, 0U, &kernelContext_7);



    return;
}

)cy_msl";

/// cyGiTraceTieredHardware.metal, 35819 bytes.
inline constexpr char kGiTraceTieredHardwareMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

            raytracing::intersection_params _slang_ray_flags_to_intersection_params(uint flags){
                raytracing::intersection_params params;
                if (flags & 0x01) /* RAY_FLAG_FORCE_OPAQUE */
                    params.force_opacity(raytracing::forced_opacity::opaque);
                if (flags & 0x02) /* RAY_FLAG_FORCE_NON_OPAQUE */
                    params.force_opacity(raytracing::forced_opacity::non_opaque);
                if (flags & 0x10) /* RAY_FLAG_CULL_BACK_FACING_TRIANGLES */
                    params.set_triangle_cull_mode(raytracing::triangle_cull_mode::back);
                if (flags & 0x20) /* RAY_FLAG_CULL_FRONT_FACING_TRIANGLES */
                    params.set_triangle_cull_mode(raytracing::triangle_cull_mode::front);
                if (flags & 0x40) /* RAY_FLAG_CULL_OPAQUE */
                    params.set_opacity_cull_mode(raytracing::opacity_cull_mode::opaque);
                if (flags & 0x80) /* RAY_FLAG_CULL_NON_OPAQUE */
                    params.set_opacity_cull_mode(raytracing::opacity_cull_mode::non_opaque);
                if (flags & 0x100) /* RAY_FLAG_SKIP_TRIANGLES */
                    params.set_geometry_cull_mode(raytracing::geometry_cull_mode::triangle);
                if (flags & 0x200) /* RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES */
                    params.set_geometry_cull_mode(raytracing::geometry_cull_mode::bounding_box);
                return params;
            }
        

                uint _slang_committed_intersection_type_to_uint(raytracing::intersection_type type){
                    switch(type){
                        case raytracing::intersection_type::none:
                            return 0; /* COMMITTED_NOTHING */
                        case raytracing::intersection_type::triangle:
                            return 1; /* COMMITTED_TRIANGLE_HIT */
                        case raytracing::intersection_type::bounding_box:
                            return 2; /* COMMITTED_PROCEDURAL_PRIMITIVE_HIT */
                        default:
                            return 0xFFFFFFFF; // Invalid value to indicate error case, should never be returned by the API
                    }
                }
            

#line 678 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiTierSample_0
{
    float3 radiance_0;
    float confidence_0;
    float hitDistance_0;
    uint source_0;
};


#line 77 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiDispatch_0
{
    uint count_0;
    uint frame_0;
    uint reserved0_0;
    uint reserved1_0;
};


#line 67 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiProbeSet_default_0
{
    uint4 device* constants_0;
    float4 device* probes_0;
    float device* payloads_0;
    uint device* selection_0;
    float4 device* directions_0;
    float4 device* results_0;
    uint device* layout_0;
    float4 device* queries_0;
    float4 device* samples_0;
    float4 device* tierRays_0;
    float4 device* tierResults_0;
    float device* screenDepth_0;
    float4 device* screenColour_0;
    float device* screenHzb_0;
    uint device* counters_0;
};


#line 74 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiSceneSet_default_0
{
    uint4 device* constants_1;
    uint device* pageTable_0;
    float device* bricks_0;
    float4 device* cards_0;
    float4 device* cardState_0;
    uint device* gridRanges_0;
    uint device* gridItems_0;
    float4 device* lights_0;
    float device* shadowDepths_0;
    uint device* selection_1;
    float4 device* results_1;
    float4 device* rays_0;
    float4 device* hits_0;
};


#line 27 "src/rendering/gi_gpu/shaders/gi_hardware.slang"
struct GiHardwareSet_default_0
{
    metal::raytracing::acceleration_structure<metal::raytracing::instancing> topLevel_0;
    uint4 device* instances_0;
    float device* positions_0;
    uint device* indices_0;
};


#line 27
struct KernelContext_0
{
    GiDispatch_0 constant* giDispatch_0;
    GiProbeSet_default_0 constant* giProbes_0;
    GiSceneSet_default_0 constant* giScene_0;
    GiHardwareSet_default_0 constant* giHardware_0;
};


#line 281 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiWorldHit_0
{
    bool hit_0;
    float t_0;
    float3 position_0;
    float3 normal_0;
    float confidence_1;
};


#line 19393 "hlsl.meta.slang"
struct RayDesc_0
{
    float3 Origin_0;
    float TMin_0;
    float3 Direction_0;
    float TMax_0;
};


#line 290 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiWorldHit_0 gpMiss_0()
{

#line 291
    thread GiWorldHit_0 miss_0;
    (&miss_0)->hit_0 = false;
    (&miss_0)->t_0 = 0.0;
    (&miss_0)->position_0 = float3(0.0, 0.0, 0.0);
    (&miss_0)->normal_0 = float3(0.0, 1.0, 0.0);
    (&miss_0)->confidence_1 = 1.0;
    return miss_0;
}


#line 32 "src/rendering/gi_gpu/shaders/gi_hardware.slang"
float3 ghVertex_0(uint vertex_0, KernelContext_0 thread* kernelContext_0)
{

#line 33
    uint _S1 = vertex_0 * 3U;

#line 33
    return float3(kernelContext_0->giHardware_0->positions_0[_S1], kernelContext_0->giHardware_0->positions_0[_S1 + 1U], kernelContext_0->giHardware_0->positions_0[_S1 + 2U]);
}


#line 95 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 96
    float squared_0 = dot(v_0, v_0);

#line 96
    float3 _S2;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 97
        _S2 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 97
    }
    else
    {

#line 97
        _S2 = fallback_0;

#line 97
    }

#line 97
    return _S2;
}


#line 106 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
float gpClamp_0(float value_0, float lo_0, float hi_0)
{

#line 106
    float _S3;
    if(value_0 < lo_0)
    {

#line 107
        _S3 = lo_0;

#line 107
    }
    else
    {

#line 107
        if(value_0 > hi_0)
        {

#line 107
            _S3 = hi_0;

#line 107
        }
        else
        {

#line 107
            _S3 = value_0;

#line 107
        }

#line 107
    }

#line 107
    return _S3;
}


#line 38 "src/rendering/gi_gpu/shaders/gi_hardware.slang"
GiWorldHit_0 GiHardwareTier_trace_0(float3 origin_0, float3 direction_0, float maxDistance_0, KernelContext_0 thread* kernelContext_1)
{

#line 39
    thread RayDesc_0 ray_0;
    (&ray_0)->Origin_0 = origin_0;
    (&ray_0)->Direction_0 = direction_0;
    (&ray_0)->TMin_0 = 0.00100000004749745;
    (&ray_0)->TMax_0 = maxDistance_0;
    thread raytracing::intersection_query<raytracing::triangle_data, raytracing::instancing> query_0;
    (&query_0)->reset({ (ray_0.Origin_0), (ray_0.Direction_0), (ray_0.TMin_0), (ray_0.TMax_0) }, (kernelContext_1->giHardware_0->topLevel_0), (255U), _slang_ray_flags_to_intersection_params((1U)));
    for(;;)
    {

#line 46
        bool _S4 = ((&query_0)->next());

#line 46
        if(_S4)
        {
        }
        else
        {

#line 46
            break;
        }

#line 46
    }

    uint _S5 = (_slang_committed_intersection_type_to_uint((&query_0)->get_committed_intersection_type()));

#line 48
    if(_S5 != 1U)
    {

#line 49
        return gpMiss_0();
    }
    float t_1 = ((&query_0)->get_committed_distance());
    uint instance_0 = ((&query_0)->get_committed_user_instance_id());
    uint primitive_0 = ((&query_0)->get_committed_primitive_id());
    uint _S6 = instance_0 * 4U;

#line 54
    uint4 record_0 = kernelContext_1->giHardware_0->instances_0[_S6];
    uint first_0 = record_0.y + primitive_0 * 3U;
    uint _S7 = record_0.x;

#line 56
    float3 _S8 = ghVertex_0(_S7 + kernelContext_1->giHardware_0->indices_0[first_0], kernelContext_1);

#line 56
    float3 _S9 = ghVertex_0(_S7 + kernelContext_1->giHardware_0->indices_0[first_0 + 1U], kernelContext_1);

#line 56
    float3 _S10 = ghVertex_0(_S7 + kernelContext_1->giHardware_0->indices_0[first_0 + 2U], kernelContext_1);

)cy_msl"
    R"cy_msl(
    float4 c0_0 = (as_type<float4>((kernelContext_1->giHardware_0->instances_0[_S6 + 1U])));


    float3 local_0 = cross(_S9 - _S8, _S10 - _S8);
    float3 normal_1 = giNormalisedOr_0(c0_0.xyz * float3(local_0.x)  + (as_type<float4>((kernelContext_1->giHardware_0->instances_0[_S6 + 2U]))).xyz * float3(local_0.y)  + (as_type<float4>((kernelContext_1->giHardware_0->instances_0[_S6 + 3U]))).xyz * float3(local_0.z) , float3(0.0, 1.0, 0.0));

#line 63
    float3 normal_2;

    if((dot(normal_1, direction_0)) > 0.0)
    {

#line 65
        normal_2 = - normal_1;

#line 65
    }
    else
    {

#line 65
        normal_2 = normal_1;

#line 65
    }


    thread GiWorldHit_0 hit_1;
    (&hit_1)->hit_0 = true;
    (&hit_1)->t_0 = t_1;
    (&hit_1)->position_0 = origin_0 + direction_0 * float3(t_1) ;
    (&hit_1)->normal_0 = normal_2;


    (&hit_1)->confidence_1 = (as_type<float4>((kernelContext_1->giProbes_0->constants_0[25U]))).z - gpClamp_0(c0_0.w * 2.0, 0.0, 0.40000000596046448);
    return hit_1;
}


#line 78
uint GiHardwareTier_source_0()
{

#line 79
    return 7U;
}


#line 685 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpTierSample_0(uint source_1)
{

#line 686
    thread GiTierSample_0 sample_0;
    (&sample_0)->radiance_0 = float3(0.0, 0.0, 0.0);
    (&sample_0)->confidence_0 = 0.0;
    (&sample_0)->hitDistance_0 = -1.0;
    (&sample_0)->source_0 = source_1;
    return sample_0;
}


#line 121
float3 gpSky_0(float3 direction_1, KernelContext_0 thread* kernelContext_2)
{

#line 122
    float4 zenith_0 = (as_type<float4>((kernelContext_2->giProbes_0->constants_0[2U])));
    float3 horizon_0 = (as_type<float3>((kernelContext_2->giProbes_0->constants_0[3U].xyz)));
    float3 ground_0 = (as_type<float3>((kernelContext_2->giProbes_0->constants_0[4U].xyz)));

    float _S11 = giNormalisedOr_0(direction_1, float3(0.0, 1.0, 0.0)).y;

#line 126
    if(_S11 < 0.0)
    {

#line 127
        return ground_0 * float3(zenith_0.w) ;
    }

    return (horizon_0 + (zenith_0.xyz - horizon_0) * float3(gpClamp_0(pow(_S11, 0.44999998807907104), 0.0, 1.0)) ) * float3(zenith_0.w) ;
}


#line 399 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_1, float cell_0)
{

#line 400
    return int(floor(value_1 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_1, float3 normal_3, KernelContext_0 thread* kernelContext_3)
{

#line 410
    uint4 grid_0 = kernelContext_3->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 413
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S12 = radius_0 * radius_0;
    float _S13 = position_1.x;

#line 418
    int _S14 = giCell_0(_S13 - radius_0, cell_1);

#line 418
    float _S15 = position_1.y;

#line 418
    int _S16 = giCell_0(_S15 - radius_0, cell_1);
    float _S17 = position_1.z;

#line 419
    int _S18 = giCell_0(_S17 - radius_0, cell_1);
    int _S19 = giCell_0(_S13 + radius_0, cell_1);

#line 420
    int _S20 = giCell_0(_S15 + radius_0, cell_1);
    int _S21 = giCell_0(_S17 + radius_0, cell_1);

#line 421
    float bestScore_0 = 1.0e+09;

#line 421
    float bestAlignment_0 = 0.0;

#line 421
    uint best_0 = 4294967295U;

#line 421
    int z_1 = _S18;



    for(;;)
    {

#line 425
        if(z_1 <= _S21)
        {
        }
        else
        {

#line 425
            break;
        }

#line 425
        int y_1 = _S16;
        for(;;)
        {

#line 426
            if(y_1 <= _S20)
            {
            }
            else
            {

#line 426
                break;
            }

#line 426
            float bestScore_1 = bestScore_0;

#line 426
            float bestAlignment_1 = bestAlignment_0;

#line 426
            uint best_1 = best_0;

#line 426
            int x_1 = _S14;
            for(;;)
            {

#line 427
                if(x_1 <= _S19)
                {
                }
                else
                {

#line 427
                    break;
                }
                uint _S22 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 429
                uint first_1 = kernelContext_3->giScene_0->gridRanges_0[_S22];
                uint _S23 = kernelContext_3->giScene_0->gridRanges_0[_S22 + 1U];

#line 430
                float bestScore_2 = bestScore_1;

#line 430
                float bestAlignment_2 = bestAlignment_1;

#line 430
                uint best_2 = best_1;

#line 430
                uint item_0 = first_1;
                for(;;)
                {

#line 431
                    if(item_0 < (first_1 + _S23))
                    {
                    }
                    else
                    {

#line 431
                        break;
                    }

#line 432
                    uint handle_0 = kernelContext_3->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_3->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 434
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }



                    uint _S24 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_3->giScene_0->cards_0[_S24 + 1U].xyz, normal_3);
                    if(alignment_0 <= 0.25)
                    {

#line 439
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }

#line 441
                    float3 offset_0 = kernelContext_3->giScene_0->cards_0[_S24].xyz - position_1;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S12)
                    {

#line 444
                        item_0 = item_0 + 1U;

#line 431
                        continue;
                    }

#line 447
                    float score_0 = squared_1 / alignment_0;

#line 447
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 448
                        better_0 = score_0 < bestScore_2;

#line 448
                    }
                    else
                    {

#line 449
                        if(alignment_0 != bestAlignment_2)
                        {

#line 449
                            better_0 = alignment_0 > bestAlignment_2;

#line 449
                        }
                        else
                        {

)cy_msl"
    R"cy_msl(#line 449
                            better_0 = handle_0 < best_2;

#line 449
                        }

#line 448
                    }

#line 448
                    float bestScore_3;

#line 448
                    float bestAlignment_3;

#line 448
                    uint best_3;


                    if(better_0)
                    {

#line 451
                        bestScore_3 = score_0;

#line 451
                        bestAlignment_3 = alignment_0;

#line 451
                        best_3 = handle_0;

#line 451
                    }
                    else
                    {

#line 451
                        bestScore_3 = bestScore_2;

#line 451
                        bestAlignment_3 = bestAlignment_2;

#line 451
                        best_3 = best_2;

#line 451
                    }

#line 451
                    bestScore_2 = bestScore_3;

#line 451
                    bestAlignment_2 = bestAlignment_3;

#line 451
                    best_2 = best_3;

#line 431
                    item_0 = item_0 + 1U;

#line 431
                }

#line 427
                int x_2 = x_1 + int(1);

#line 427
                bestScore_1 = bestScore_2;

#line 427
                bestAlignment_1 = bestAlignment_2;

#line 427
                best_1 = best_2;

#line 427
                x_1 = x_2;

#line 427
            }

#line 426
            int y_2 = y_1 + int(1);

#line 426
            bestScore_0 = bestScore_1;

#line 426
            bestAlignment_0 = bestAlignment_1;

#line 426
            best_0 = best_1;

#line 426
            y_1 = y_2;

#line 426
        }

#line 425
        z_1 = z_1 + int(1);

#line 425
    }

#line 460
    return best_0;
}


#line 765 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpWorldRadiance_0(const GiWorldHit_0 thread* hit_2, float3 direction_2, uint source_2, uint cacheFrame_0, KernelContext_0 thread* kernelContext_4)
{

#line 766
    thread GiTierSample_0 sample_1 = gpTierSample_0(source_2);
    if(!hit_2->hit_0)
    {

#line 768
        (&sample_1)->source_0 = 9U;

#line 768
        float3 _S25 = gpSky_0(direction_2, kernelContext_4);
        (&sample_1)->radiance_0 = _S25;
        (&sample_1)->confidence_0 = 1.0;
        (&sample_1)->hitDistance_0 = -1.0;
        return sample_1;
    }
    (&sample_1)->hitDistance_0 = hit_2->t_0;

#line 774
    uint _S26 = giFindCard_0(hit_2->position_0, hit_2->normal_0, kernelContext_4);

    if(_S26 != 4294967295U)
    {

#line 777
        uint _S27 = _S26 * 2U;

#line 777
        (&sample_1)->radiance_0 = (*(kernelContext_4->giScene_0->cardState_0+_S27)).xyz;
        uint last_0 = (as_type<uint>(((*(kernelContext_4->giScene_0->cardState_0+(_S27 + 1U))).w)));

#line 778
        float age_0;
        if(cacheFrame_0 > last_0)
        {

#line 779
            age_0 = float(cacheFrame_0 - last_0);

#line 779
        }
        else
        {

#line 779
            age_0 = 0.0;

#line 779
        }


        (&sample_1)->confidence_0 = hit_2->confidence_1 * (1.0 - gpClamp_0(age_0 / 240.0, 0.0, 1.0) * 0.60000002384185791);
        return sample_1;
    }
    (&sample_1)->confidence_0 = hit_2->confidence_1 * 0.20000000298023224;
    return sample_1;
}


GiTierSample_0 gpWorldTrace_0(float3 origin_1, float3 unit_0, float reach_0, KernelContext_0 thread* kernelContext_5)
{

#line 791
    uint cacheFrame_1 = kernelContext_5->giProbes_0->constants_0[24U].w;

#line 791
    GiWorldHit_0 _S28 = GiHardwareTier_trace_0(origin_1, unit_0, reach_0, kernelContext_5);

    uint _S29 = GiHardwareTier_source_0();

#line 793
    thread GiWorldHit_0 _S30 = _S28;

#line 793
    GiTierSample_0 _S31 = gpWorldRadiance_0(&_S30, unit_0, _S29, cacheFrame_1, kernelContext_5);

#line 793
    return _S31;
}


#line 803
void gpCount_0(uint counter_0, uint amount_0, KernelContext_0 thread* kernelContext_6)
{

#line 804
    uint total_0 = simd_sum(amount_0);
    bool _S32 = simd_is_first();

#line 805
    bool _S33;

#line 805
    if(_S32)
    {

#line 805
        _S33 = total_0 != 0U;

#line 805
    }
    else
    {

#line 805
        _S33 = false;

#line 805
    }

#line 805
    if(_S33)
    {

#line 806
        uint _S34 = atomic_fetch_add_explicit(((atomic_uint device*)(kernelContext_6->giProbes_0->counters_0+counter_0)), total_0, memory_order_relaxed);

#line 805
    }


    return;
}


#line 606
struct GiScreenStep_0
{
    bool outside_0;
    float x_3;
    float y_3;
    float depth_0;
};


#line 577
float3 gpTransformPoint_0(uint first_2, float3 p_0, KernelContext_0 thread* kernelContext_7)
{



    return ((as_type<float4>((kernelContext_7->giProbes_0->constants_0[first_2]))) * float4(p_0.x)  + (as_type<float4>((kernelContext_7->giProbes_0->constants_0[first_2 + 1U]))) * float4(p_0.y)  + (as_type<float4>((kernelContext_7->giProbes_0->constants_0[first_2 + 2U]))) * float4(p_0.z)  + (as_type<float4>((kernelContext_7->giProbes_0->constants_0[first_2 + 3U])))).xyz;
}

float3 gpProjectPoint_0(uint first_3, float3 p_1, KernelContext_0 thread* kernelContext_8)
{



    float4 clip_0 = (as_type<float4>((kernelContext_8->giProbes_0->constants_0[first_3]))) * float4(p_1.x)  + (as_type<float4>((kernelContext_8->giProbes_0->constants_0[first_3 + 1U]))) * float4(p_1.y)  + (as_type<float4>((kernelContext_8->giProbes_0->constants_0[first_3 + 2U]))) * float4(p_1.z)  + (as_type<float4>((kernelContext_8->giProbes_0->constants_0[first_3 + 3U])));
    float _S35 = clip_0.w;

#line 591
    if(_S35 == 0.0)
    {

#line 592
        return clip_0.xyz;
    }
    return clip_0.xyz * float3((1.0 / _S35)) ;
}


#line 614
GiScreenStep_0 gpScreenStep_0(float3 point_0, uint width_0, uint height_0, KernelContext_0 thread* kernelContext_9)
{

#line 614
    float3 _S36 = gpTransformPoint_0(16U, point_0, kernelContext_9);
    thread GiScreenStep_0 step_0;

    float _S37 = - _S36.z;

#line 617
    (&step_0)->depth_0 = _S37;

#line 617
    float3 _S38 = gpProjectPoint_0(20U, point_0, kernelContext_9);

    float _S39 = float(width_0);

#line 619
    (&step_0)->x_3 = (_S38.x * 0.5 + 0.5) * _S39;
    float _S40 = float(height_0);

#line 620
    (&step_0)->y_3 = (0.5 - _S38.y * 0.5) * _S40;

)cy_msl"
    R"cy_msl(#line 620
    bool _S41;
    if(_S37 <= 0.0)
    {

#line 621
        _S41 = true;

#line 621
    }
    else
    {

#line 621
        _S41 = ((&step_0)->x_3) < 0.0;

#line 621
    }

#line 621
    if(_S41)
    {

#line 621
        _S41 = true;

#line 621
    }
    else
    {

#line 621
        _S41 = ((&step_0)->y_3) < 0.0;

#line 621
    }

#line 621
    if(_S41)
    {

#line 621
        _S41 = true;

#line 621
    }
    else
    {

#line 621
        _S41 = ((&step_0)->x_3) >= _S39;

#line 621
    }
    if(_S41)
    {

#line 622
        _S41 = true;

#line 622
    }
    else
    {

#line 622
        _S41 = ((&step_0)->y_3) >= _S40;

#line 622
    }

#line 621
    (&step_0)->outside_0 = _S41;

    return step_0;
}


#line 598
float gpEdgeConfidence_0(float x_4, float y_4, uint width_1, uint height_1)
{
    float u_0 = x_4 / float(width_1);
    float v_1 = y_4 / float(height_1);

    return gpClamp_0(min(min(u_0, 1.0 - u_0), min(v_1, 1.0 - v_1)) / 0.10000000149011612, 0.0, 1.0);
}


#line 628
float gpNearestDepth_0(int x0_0, int y0_0, int x1_0, int y1_0, uint thread* reads_0, KernelContext_0 thread* kernelContext_10)
{

#line 629
    uint _S42 = kernelContext_10->giProbes_0->constants_0[15U].x;
    int _S43 = max(x1_0 - x0_0, y1_0 - y0_0);

#line 630
    uint level_0 = 0U;

    for(;;)
    {

#line 632
        uint _S44 = level_0 + 1U;

#line 632
        bool _S45;

#line 632
        if(_S44 < _S42)
        {

#line 632
            _S45 = (int(1) << uint(int(level_0))) <= _S43;

#line 632
        }
        else
        {

#line 632
            _S45 = false;

#line 632
        }

#line 632
        if(_S45)
        {
        }
        else
        {

#line 632
            break;
        }

#line 632
        level_0 = _S44;

#line 632
    }


    uint4 shape_0 = kernelContext_10->giProbes_0->constants_0[26U + level_0];

#line 635
    uint _S46 = uint(int(level_0));
    int _S47 = x0_0 >> _S46;
    int ly0_0 = y0_0 >> _S46;
    uint _S48 = shape_0.y;

#line 638
    int _S49 = min(x1_0 >> _S46, int(_S48) - int(1));
    int _S50 = min(y1_0 >> _S46, int(shape_0.z) - int(1));

#line 639
    float nearest_0 = 1.00000001504746622e+30;

#line 639
    int y_5 = ly0_0;

    for(;;)
    {

#line 641
        if(y_5 <= _S50)
        {
        }
        else
        {

#line 641
            break;
        }

#line 641
        int x_5 = _S47;
        for(;;)
        {

#line 642
            if(x_5 <= _S49)
            {
            }
            else
            {

#line 642
                break;
            }

#line 643
            float _S51 = min(nearest_0, *(kernelContext_10->giProbes_0->screenHzb_0+(shape_0.x + uint(y_5) * _S48 + uint(x_5))));
            *reads_0 = *reads_0 + 1U;

#line 642
            int x_6 = x_5 + int(1);

#line 642
            nearest_0 = _S51;

#line 642
            x_5 = x_6;

#line 642
        }

#line 641
        y_5 = y_5 + int(1);

#line 641
    }

#line 647
    return nearest_0;
}




bool gpScreenRunEmpty_0(float3 origin_2, float3 direction_3, float step_1, uint index_0, uint run_0, const GiScreenStep_0 thread* here_0, uint width_2, uint height_2, uint thread* reads_1, KernelContext_0 thread* kernelContext_11)
{

#line 654
    GiScreenStep_0 _S52 = gpScreenStep_0(origin_2 + direction_3 * float3((step_1 * float(index_0 + run_0))) , width_2, height_2, kernelContext_11);

#line 654
    bool inside_0;

#line 659
    if(!_S52.outside_0)
    {

#line 659
        inside_0 = (_S52.x_3) >= 1.0;

#line 659
    }
    else
    {

#line 659
        inside_0 = false;

#line 659
    }

#line 659
    if(inside_0)
    {

#line 659
        inside_0 = (_S52.y_3) >= 1.0;

#line 659
    }
    else
    {

#line 659
        inside_0 = false;

#line 659
    }

#line 659
    if(inside_0)
    {

#line 659
        inside_0 = (_S52.x_3) < (float(width_2) - 1.0);

#line 659
    }
    else
    {

#line 659
        inside_0 = false;

#line 659
    }
    if(inside_0)
    {

#line 660
        inside_0 = (_S52.y_3) < (float(height_2) - 1.0);

#line 660
    }
    else
    {

#line 660
        inside_0 = false;

#line 660
    }

#line 660
    if(inside_0)
    {

)cy_msl"
    R"cy_msl(#line 660
        inside_0 = (here_0->x_3) >= 1.0;

#line 660
    }
    else
    {

#line 660
        inside_0 = false;

#line 660
    }
    if(inside_0)
    {

#line 661
        inside_0 = (here_0->y_3) >= 1.0;

#line 661
    }
    else
    {

#line 661
        inside_0 = false;

#line 661
    }

#line 661
    if(inside_0)
    {

#line 661
        inside_0 = (here_0->x_3) < (float(width_2) - 1.0);

#line 661
    }
    else
    {

#line 661
        inside_0 = false;

#line 661
    }

#line 661
    if(inside_0)
    {

#line 661
        inside_0 = (here_0->y_3) < (float(height_2) - 1.0);

#line 661
    }
    else
    {

#line 661
        inside_0 = false;

#line 661
    }

    if(!inside_0)
    {

#line 664
        return false;
    }

#line 664
    float _S53 = gpNearestDepth_0(max(int(floor(min(here_0->x_3, _S52.x_3))) - int(1), int(0)), max(int(floor(min(here_0->y_3, _S52.y_3))) - int(1), int(0)), min(int(floor(max(here_0->x_3, _S52.x_3))) + int(1), int(width_2) - int(1)), min(int(floor(max(here_0->y_3, _S52.y_3))) + int(1), int(height_2) - int(1)), reads_1, kernelContext_11);

#line 675
    return (max(here_0->depth_0, _S52.depth_0)) < (_S53 - abs(_S53) * 0.00009999999747379 - 0.00009999999747379);
}


#line 695
GiTierSample_0 gpScreenTrace_0(float3 origin_3, float3 direction_4, float maxDistance_1, bool accelerated_0, uint thread* reads_2, KernelContext_0 thread* kernelContext_12)
{
    thread GiTierSample_0 sample_2 = gpTierSample_0(5U);
    uint4 screen_0 = kernelContext_12->giProbes_0->constants_0[14U];
    uint width_3 = screen_0.x;
    uint height_3 = screen_0.y;

#line 700
    bool _S54;
    if(width_3 == 0U)
    {

#line 701
        _S54 = true;

#line 701
    }
    else
    {

#line 701
        _S54 = height_3 == 0U;

#line 701
    }

#line 701
    if(_S54)
    {

#line 702
        return sample_2;
    }
    float _S55 = (as_type<float>((screen_0.w)));
    uint _S56 = max(1U, screen_0.z);
    float _S57 = maxDistance_1 / float(_S56);

#line 706
    uint run_1 = 8U;

#line 706
    uint index_1 = 1U;

#line 711
    for(;;)
    {

#line 711
        if(index_1 <= _S56)
        {
        }
        else
        {

#line 711
            break;
        }

#line 712
        float t_2 = _S57 * float(index_1);

#line 712
        GiScreenStep_0 _S58 = gpScreenStep_0(origin_3 + direction_4 * float3(t_2) , width_3, height_3, kernelContext_12);


        if(_S58.outside_0)
        {

            (&sample_2)->confidence_0 = 0.0;
            return sample_2;
        }
        uint pixel_0 = uint(_S58.y_3) * width_3 + uint(_S58.x_3);
        float bufferDepth_0 = kernelContext_12->giProbes_0->screenDepth_0[pixel_0];
        *reads_2 = *reads_2 + 1U;


        bool _S59 = bufferDepth_0 > 0.0;

#line 726
        float gap_0;

#line 726
        if(_S59)
        {

#line 726
            gap_0 = bufferDepth_0 - _S58.depth_0;

#line 726
        }
        else
        {

#line 726
            gap_0 = 1.00000001504746622e+30;

#line 726
        }
        if(_S59)
        {

#line 728
            float behind_0 = _S58.depth_0 - bufferDepth_0;
            if(behind_0 > 0.0)
            {

#line 730
                if(behind_0 > _S55)
                {
                    (&sample_2)->confidence_0 = 0.0;
                    return sample_2;
                }
                (&sample_2)->radiance_0 = kernelContext_12->giProbes_0->screenColour_0[pixel_0].xyz;
                (&sample_2)->hitDistance_0 = t_2;

                (&sample_2)->confidence_0 = gpClamp_0(gpEdgeConfidence_0(_S58.x_3, _S58.y_3, width_3, height_3) * (1.0 - behind_0 / max(_S55, 0.00009999999747379)) * 0.92000001668930054, 0.0, 1.0);

                return sample_2;
            }

#line 727
        }

#line 744
        if(accelerated_0)
        {

#line 744
            _S54 = (index_1 + 1U) < _S56;

#line 744
        }
        else
        {

#line 744
            _S54 = false;

#line 744
        }

#line 744
        bool _S60;

#line 744
        if(_S54)
        {

#line 744
            _S60 = gap_0 > (_S55 * 2.0);

#line 744
        }
        else
        {

#line 744
            _S60 = false;

#line 744
        }

#line 744
        uint skip_0;

#line 744
        if(_S60)
        {

#line 745
            uint _S61 = min(run_1, _S56 - index_1);

#line 745
            thread GiScreenStep_0 _S62 = _S58;

#line 745
            bool _S63 = gpScreenRunEmpty_0(origin_3, direction_4, _S57, index_1, _S61, &_S62, width_3, height_3, reads_2, kernelContext_12);

#line 745
            uint run_2;
            if(_S63)
            {

                uint _S64 = min(run_1 * 2U, 32U);

#line 749
                skip_0 = _S61;

#line 749
                run_2 = _S64;

#line 746
            }
            else
            {


                uint _S65 = max(run_1 / 2U, 2U);

#line 751
                skip_0 = 0U;

#line 751
                run_2 = _S65;

#line 746
            }

#line 746
            run_1 = run_2;

#line 744
        }
        else
        {

#line 744
            skip_0 = 0U;

)cy_msl"
    R"cy_msl(#line 744
        }

#line 744
        index_1 = index_1 + (1U + skip_0);

#line 711
    }

#line 757
    (&sample_2)->confidence_0 = 0.20000000298023224;
    (&sample_2)->hitDistance_0 = -1.0;
    return sample_2;
}


#line 812
uint gpTicket_0(uint counter_1, KernelContext_0 thread* kernelContext_13)
{
    uint count_1 = simd_sum(1U);
    uint rank_0 = simd_prefix_exclusive_sum(1U);

    bool _S66 = simd_is_first();

#line 817
    uint base_0;

#line 817
    if(_S66)
    {

#line 818
        uint _S67 = atomic_fetch_add_explicit(((atomic_uint device*)(kernelContext_13->giProbes_0->counters_0+counter_1)), count_1, memory_order_relaxed);

#line 818
        base_0 = _S67;

#line 817
    }
    else
    {

#line 817
        base_0 = 0U;

#line 817
    }


    uint _S68 = simd_broadcast_first(base_0);

#line 820
    return _S68 + rank_0;
}


#line 827
GiTierSample_0 gpTieredTrace_0(float3 origin_4, float3 direction_5, float maxDistance_2, bool screenPresent_0, uint thread* reads_3, KernelContext_0 thread* kernelContext_14)
{
    float3 unit_1 = giNormalisedOr_0(direction_5, float3(0.0, 0.0, -1.0));
    float4 hzb_0 = (as_type<float4>((kernelContext_14->giProbes_0->constants_0[15U])));
    float _S69 = min(maxDistance_2, hzb_0.z);
    float threshold_0 = hzb_0.w;
    bool accelerated_1 = (kernelContext_14->giProbes_0->constants_0[15U].y) != 0U;

    GiTierSample_0 _S70 = gpTierSample_0(0U);

#line 835
    GiTierSample_0 screen_1;
    if(screenPresent_0)
    {

#line 836
        gpCount_0(4U, 1U, kernelContext_14);

#line 836
        GiTierSample_0 _S71 = gpScreenTrace_0(origin_4, unit_1, _S69, accelerated_1, reads_3, kernelContext_14);


        if((_S71.confidence_0) >= threshold_0)
        {

#line 840
            return _S71;
        }

#line 840
        screen_1 = _S71;

#line 836
    }
    else
    {

#line 836
        screen_1 = _S70;

#line 836
    }

#line 847
    uint4 budget_0 = kernelContext_14->giProbes_0->constants_0[24U];

#line 847
    uint limit_0;
    if(screenPresent_0)
    {

#line 848
        limit_0 = min(budget_0.x, budget_0.y);

#line 848
    }
    else
    {

#line 848
        limit_0 = budget_0.x;

#line 848
    }

#line 848
    uint _S72 = gpTicket_0(0U, kernelContext_14);

    if(_S72 >= limit_0)
    {

#line 850
        gpCount_0(2U, 1U, kernelContext_14);

        return screen_1;
    }
    if(screenPresent_0)
    {

#line 854
        gpCount_0(1U, 1U, kernelContext_14);

#line 854
    }


    if((GiHardwareTier_source_0()) == 7U)
    {

#line 857
        limit_0 = 6U;

#line 857
    }
    else
    {

#line 857
        limit_0 = 5U;

#line 857
    }

#line 857
    gpCount_0(limit_0, 1U, kernelContext_14);

#line 857
    GiTierSample_0 _S73 = gpWorldTrace_0(origin_4, unit_1, _S69, kernelContext_14);

    if((_S73.source_0) == 9U)
    {

#line 859
        limit_0 = 1U;

#line 859
    }
    else
    {

#line 859
        limit_0 = 0U;

#line 859
    }

#line 859
    gpCount_0(7U, limit_0, kernelContext_14);

#line 859
    bool _S74;
    if(!screenPresent_0)
    {

#line 860
        _S74 = true;

#line 860
    }
    else
    {

#line 860
        _S74 = (screen_1.confidence_0) <= 0.0;

#line 860
    }

#line 860
    if(_S74)
    {

#line 861
        return _S73;
    }
    thread GiTierSample_0 blended_0;
    (&blended_0)->radiance_0 = _S73.radiance_0 + (screen_1.radiance_0 - _S73.radiance_0) * float3(screen_1.confidence_0) ;
    (&blended_0)->confidence_0 = max(screen_1.confidence_0, _S73.confidence_0);
    bool _S75 = (screen_1.confidence_0) >= (_S73.confidence_0);

#line 866
    if(_S75)
    {

#line 866
        limit_0 = screen_1.source_0;

#line 866
    }
    else
    {

#line 866
        limit_0 = _S73.source_0;

#line 866
    }

#line 866
    (&blended_0)->source_0 = limit_0;

#line 866
    float _S76;

    if(_S75)
    {

#line 868
        _S76 = screen_1.hitDistance_0;

#line 868
    }
    else
    {

#line 868
        _S76 = _S73.hitDistance_0;

#line 868
    }

#line 867
    (&blended_0)->hitDistance_0 = _S76;

    return blended_0;
}

void gpWriteTierResult_0(uint ray_1, const GiTierSample_0 thread* sample_3, uint reads_4, KernelContext_0 thread* kernelContext_15)
{

#line 873
    uint _S77 = ray_1 * 2U;

#line 873
    *(kernelContext_15->giProbes_0->tierResults_0+_S77) = float4(sample_3->radiance_0, sample_3->confidence_0);
    *(kernelContext_15->giProbes_0->tierResults_0+(_S77 + 1U)) = float4(sample_3->hitDistance_0, (as_type<float>((sample_3->source_0))), (as_type<float>((reads_4))), 0.0);

    return;
}


#line 104 "src/rendering/gi_gpu/shaders/gi_hardware.slang"
[[kernel]] void cyGiTraceTieredHardware(uint3 thread_0 [[thread_position_in_grid]], GiDispatch_0 constant* giDispatch_1 [[buffer(1)]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiHardwareSet_default_0 constant* giHardware_1 [[buffer(3)]])
{

#line 104
    thread KernelContext_0 kernelContext_16;

#line 104
    (&kernelContext_16)->giDispatch_0 = giDispatch_1;

#line 104
    (&kernelContext_16)->giProbes_0 = giProbes_1;

#line 104
    (&kernelContext_16)->giScene_0 = giScene_1;

#line 104
    (&kernelContext_16)->giHardware_0 = giHardware_1;
    uint ray_2 = thread_0.x;
    if(ray_2 >= (giDispatch_1->count_0))
    {

#line 107
        return;
    }
    uint _S78 = ray_2 * 2U;

)cy_msl"
    R"cy_msl(#line 109
    float4 origin_5 = (&kernelContext_16)->giProbes_0->tierRays_0[_S78];
    float4 direction_6 = (&kernelContext_16)->giProbes_0->tierRays_0[_S78 + 1U];
    bool screenPresent_1 = ((&kernelContext_16)->giProbes_0->constants_0[14U].x) != 0U;
    thread uint reads_5 = 0U;

#line 112
    GiTierSample_0 _S79 = gpTieredTrace_0(origin_5.xyz, direction_6.xyz, origin_5.w, screenPresent_1, &reads_5, &kernelContext_16);

#line 112
    gpCount_0(3U, reads_5, &kernelContext_16);

#line 112
    thread GiTierSample_0 _S80 = _S79;

#line 112
    gpWriteTierResult_0(ray_2, &_S80, reads_5, &kernelContext_16);

#line 117
    return;
}

)cy_msl";

}  // namespace cy::rendering::gi_gpu

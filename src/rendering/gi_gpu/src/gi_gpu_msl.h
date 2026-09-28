#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the device GI dispatches. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::gi_gpu {

/// cyGiTraceRays.metal, 11477 bytes.
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


#line 152
int giWrap_0(int value_0, int modulus_0)
{

#line 153
    int _S1 = value_0 % modulus_0;

#line 153
    int _S2 = (_S1 + modulus_0) % modulus_0;

#line 153
    return _S2;
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


#line 159
float giSampleLevel_0(uint level_0, float3 p_0, float thread* farDistance_0, KernelContext_0 thread* kernelContext_4)
{

#line 160
    uint _S13 = 16U + level_0 * 2U;

#line 160
    float4 shape_0 = (as_type<float4>((kernelContext_4->giScene_0->constants_0[_S13])));
    int4 origin_0 = (as_type<int4>((kernelContext_4->giScene_0->constants_0[_S13 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_0.x / brick_0));
    int by_0 = int(floor(p_0.y / brick_0));
    int bz_0 = int(floor(p_0.z / brick_0));

#line 167
    uint _S14 = giWindowBricks_0(kernelContext_4);
    int dim_0 = int(_S14);
    int _S15 = origin_0.x;

#line 169
    bool _S16;

#line 169
    if(bx_0 < _S15)
    {

#line 169
        _S16 = true;

#line 169
    }
    else
    {

#line 169
        _S16 = by_0 < (origin_0.y);

#line 169
    }

#line 169
    if(_S16)
    {

#line 169
        _S16 = true;

#line 169
    }
    else
    {

#line 169
        _S16 = bz_0 < (origin_0.z);

#line 169
    }

#line 169
    if(_S16)
    {

#line 169
        _S16 = true;

#line 169
    }
    else
    {

#line 169
        _S16 = bx_0 >= (_S15 + dim_0);

#line 169
    }

#line 169
    if(_S16)
    {

#line 169
        _S16 = true;

#line 169
    }
    else
    {

#line 169
        _S16 = by_0 >= (origin_0.y + dim_0);

#line 169
    }
    if(_S16)
    {

#line 170
        _S16 = true;

#line 170
    }
    else
    {

#line 170
        _S16 = bz_0 >= (origin_0.z + dim_0);

#line 170
    }

#line 169
    if(_S16)
    {
        return *farDistance_0;
    }
    int _S17 = giWrap_0(bz_0, dim_0);

#line 173
    uint _S18 = uint(dim_0);

#line 173
    uint _S19 = uint(_S17) * _S18;

#line 173
    int _S20 = giWrap_0(by_0, dim_0);

#line 173
    uint _S21 = (_S19 + uint(_S20)) * _S18;
    int _S22 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_4->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S21 + uint(_S22))];
    if(slot_1 == 4294967295U)
    {

#line 177
        return *farDistance_0;
    }

    float3 local_0 = (p_0 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 180
    float _S23 = giTrilinear_0(slot_1, local_0.x, local_0.y, local_0.z, kernelContext_4);
    return _S23;
}


float giDistance_0(float3 p_1, KernelContext_0 thread* kernelContext_5)
{

#line 185
    uint _S24 = giLevelCount_0(kernelContext_5);

#line 185
    float answer_0 = 1.0e+06;

#line 185
    uint level_1 = 0U;


    for(;;)
    {

#line 188
        if(level_1 < _S24)
        {
        }
        else
        {

)cy_msl"
    R"cy_msl(#line 188
            break;
        }

#line 189
        thread float farDistance_1 = 0.0;

#line 189
        float _S25 = giSampleLevel_0(level_1, p_1, &farDistance_1, kernelContext_5);

        float _S26 = min(answer_0, _S25);
        if(_S25 < farDistance_1)
        {

#line 193
            return _S25;
        }

#line 188
        uint level_2 = level_1 + 1U;

#line 188
        answer_0 = _S26;

#line 188
        level_1 = level_2;

#line 188
    }

#line 196
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


#line 199
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

#line 210
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_0 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_1 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_0 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_0 = float3(0.0, 1.0, 0.0);

#line 216
    uint _S28 = giLevelCount_0(kernelContext_6);
    if(_S28 == 0U)
    {

#line 218
        return result_0;
    }

#line 218
    float _S29 = giFinestVoxel_0(kernelContext_6);


    float surface_0 = _S29 * 0.5;
    float _S30 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 223
    recent_0[int(0)] = 1.0e+06;

#line 223
    recent_0[int(1)] = 1.0e+06;

#line 223
    recent_0[int(2)] = 1.0e+06;

#line 223
    float closest_1 = 1.0e+06;

#line 223
    uint recentSlot_0 = 0U;

#line 223
    uint step_0 = 0U;

#line 223
    float t_2 = _S30;


    for(;;)
    {

#line 226
        if(step_0 < 192U)
        {
        }
        else
        {

#line 226
            break;
        }

#line 227
        if(t_2 > maxDistance_0)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_1 = origin_1 + direction_0 * float3(t_2) ;

#line 232
        float _S31 = giDistance_0(position_1, kernelContext_6);

        if(_S31 < surface_0)
        {

#line 235
            (&result_0)->hit_0 = true;
            (&result_0)->t_1 = t_2;
            (&result_0)->position_0 = position_1;


            float3 _S32 = float3(_S29, 0.0, 0.0);

#line 240
            float _S33 = giDistance_0(position_1 + _S32, kernelContext_6);

#line 240
            float _S34 = giDistance_0(position_1 - _S32, kernelContext_6);

#line 240
            float _S35 = _S33 - _S34;

            float3 _S36 = float3(0.0, _S29, 0.0);

#line 242
            float _S37 = giDistance_0(position_1 + _S36, kernelContext_6);

#line 242
            float _S38 = giDistance_0(position_1 - _S36, kernelContext_6);

#line 242
            float _S39 = _S37 - _S38;

            float3 _S40 = float3(0.0, 0.0, _S29);

#line 244
            float _S41 = giDistance_0(position_1 + _S40, kernelContext_6);

#line 244
            float _S42 = giDistance_0(position_1 - _S40, kernelContext_6);

            (&result_0)->normal_0 = giNormalisedOr_0(float3(_S35, _S39, _S41 - _S42), - direction_0);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S43 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S31;
        uint _S44 = (recentSlot_0 + 1U) % 3U;
        float t_3 = t_2 + max(_S31, surface_0);

#line 226
        uint step_1 = step_0 + 1U;

#line 226
        closest_1 = _S43;

#line 226
        recentSlot_0 = _S44;

#line 226
        step_0 = step_1;

#line 226
        t_2 = t_3;

#line 226
    }

#line 255
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

/// cyGiShadeCards.metal, 30771 bytes.
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


#line 330
float3 giDirectRadiance_0(uint light_0, float3 position_0, float3 normal_0, KernelContext_0 thread* kernelContext_0)
{

#line 331
    uint _S2 = light_0 * 4U;

#line 331
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

#line 341
    bool _S5;

#line 341
    if(_S4 > 0.0)
    {

#line 341
        _S5 = _S3 > (_S4 * _S4);

#line 341
    }
    else
    {

#line 341
        _S5 = false;

#line 341
    }

#line 341
    if(_S5)
    {

#line 342
        return float3(0.0, 0.0, 0.0);
    }


    return c_0.xyz * float3((a_0.w * max(0.0, dot(normal_0, offset_0 / float3(sqrt(_S3)) )) / _S3)) ;
}


#line 279
bool giShadowMapShadowed_0(float3 p_0, KernelContext_0 thread* kernelContext_1)
{

#line 280
    float4 centre_0 = (as_type<float4>((kernelContext_1->giScene_0->constants_0[5U])));
    float4 dir_0 = (as_type<float4>((kernelContext_1->giScene_0->constants_0[6U])));

    float4 up_0 = (as_type<float4>((kernelContext_1->giScene_0->constants_0[8U])));
    uint resolution_0 = kernelContext_1->giScene_0->constants_0[7U].w;
    float extent_0 = centre_0.w;
    float3 relative_0 = p_0 - centre_0.xyz;
    float u_0 = dot(relative_0, (as_type<float4>((kernelContext_1->giScene_0->constants_0[7U]))).xyz);
    float v_1 = dot(relative_0, up_0.xyz);
    float _S6 = dir_0.w;

#line 289
    float depth_0 = dot(relative_0, dir_0.xyz) + _S6 * 0.5;

#line 289
    bool _S7;
    if((abs(u_0)) >= extent_0)
    {

#line 290
        _S7 = true;

#line 290
    }
    else
    {

#line 290
        _S7 = (abs(v_1)) >= extent_0;

#line 290
    }

#line 290
    if(_S7)
    {

#line 290
        _S7 = true;

#line 290
    }
    else
    {

#line 290
        _S7 = depth_0 < 0.0;

#line 290
    }

#line 290
    if(_S7)
    {

#line 290
        _S7 = true;

#line 290
    }
    else
    {

#line 290
        _S7 = depth_0 > _S6;

#line 290
    }

#line 290
    if(_S7)
    {

#line 291
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


#line 152
int giWrap_0(int value_0, int modulus_0)
{

#line 153
    int _S9 = value_0 % modulus_0;

#line 153
    int _S10 = (_S9 + modulus_0) % modulus_0;

#line 153
    return _S10;
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

)cy_msl"
    R"cy_msl(#line 139
    uint _S17 = base_0 + (_S11 + _S16) * 4U;

    uint _S18 = uint(clamp(z0_0 + int(1), int(0), int(3))) * 4U;

#line 141
    uint _S19 = base_0 + (_S18 + _S12) * 4U;

    uint _S20 = base_0 + (_S18 + _S16) * 4U;

#line 149
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_6->giScene_0->bricks_0[_S13 + _S14], kernelContext_6->giScene_0->bricks_0[_S13 + _S15], tx_0), giLerp_0(kernelContext_6->giScene_0->bricks_0[_S17 + _S14], kernelContext_6->giScene_0->bricks_0[_S17 + _S15], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_6->giScene_0->bricks_0[_S19 + _S14], kernelContext_6->giScene_0->bricks_0[_S19 + _S15], tx_0), giLerp_0(kernelContext_6->giScene_0->bricks_0[_S20 + _S14], kernelContext_6->giScene_0->bricks_0[_S20 + _S15], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 159
float giSampleLevel_0(uint level_0, float3 p_1, float thread* farDistance_0, KernelContext_0 thread* kernelContext_7)
{

#line 160
    uint _S21 = 16U + level_0 * 2U;

#line 160
    float4 shape_0 = (as_type<float4>((kernelContext_7->giScene_0->constants_0[_S21])));
    int4 origin_0 = (as_type<int4>((kernelContext_7->giScene_0->constants_0[_S21 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_1.x / brick_0));
    int by_0 = int(floor(p_1.y / brick_0));
    int bz_0 = int(floor(p_1.z / brick_0));

#line 167
    uint _S22 = giWindowBricks_0(kernelContext_7);
    int dim_0 = int(_S22);
    int _S23 = origin_0.x;

#line 169
    bool _S24;

#line 169
    if(bx_0 < _S23)
    {

#line 169
        _S24 = true;

#line 169
    }
    else
    {

#line 169
        _S24 = by_0 < (origin_0.y);

#line 169
    }

#line 169
    if(_S24)
    {

#line 169
        _S24 = true;

#line 169
    }
    else
    {

#line 169
        _S24 = bz_0 < (origin_0.z);

#line 169
    }

#line 169
    if(_S24)
    {

#line 169
        _S24 = true;

#line 169
    }
    else
    {

#line 169
        _S24 = bx_0 >= (_S23 + dim_0);

#line 169
    }

#line 169
    if(_S24)
    {

#line 169
        _S24 = true;

#line 169
    }
    else
    {

#line 169
        _S24 = by_0 >= (origin_0.y + dim_0);

#line 169
    }
    if(_S24)
    {

#line 170
        _S24 = true;

#line 170
    }
    else
    {

#line 170
        _S24 = bz_0 >= (origin_0.z + dim_0);

#line 170
    }

#line 169
    if(_S24)
    {
        return *farDistance_0;
    }
    int _S25 = giWrap_0(bz_0, dim_0);

#line 173
    uint _S26 = uint(dim_0);

#line 173
    uint _S27 = uint(_S25) * _S26;

#line 173
    int _S28 = giWrap_0(by_0, dim_0);

#line 173
    uint _S29 = (_S27 + uint(_S28)) * _S26;
    int _S30 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_7->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S29 + uint(_S30))];
    if(slot_1 == 4294967295U)
    {

#line 177
        return *farDistance_0;
    }

    float3 local_0 = (p_1 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 180
    float _S31 = giTrilinear_0(slot_1, local_0.x, local_0.y, local_0.z, kernelContext_7);
    return _S31;
}


float giDistance_0(float3 p_2, KernelContext_0 thread* kernelContext_8)
{

#line 185
    uint _S32 = giLevelCount_0(kernelContext_8);

#line 185
    float answer_0 = 1.0e+06;

#line 185
    uint level_1 = 0U;


    for(;;)
    {

#line 188
        if(level_1 < _S32)
        {
        }
        else
        {

#line 188
            break;
        }

#line 189
        thread float farDistance_1 = 0.0;

#line 189
        float _S33 = giSampleLevel_0(level_1, p_2, &farDistance_1, kernelContext_8);

        float _S34 = min(answer_0, _S33);
        if(_S33 < farDistance_1)
        {

#line 193
            return _S33;
        }

#line 188
        uint level_2 = level_1 + 1U;

#line 188
        answer_0 = _S34;

#line 188
        level_1 = level_2;

#line 188
    }

#line 196
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

#line 210
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_0 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_1 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_1 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_1 = float3(0.0, 1.0, 0.0);

#line 216
    uint _S35 = giLevelCount_0(kernelContext_9);
    if(_S35 == 0U)
    {

#line 218
        return result_0;
    }

#line 218
    float _S36 = giFinestVoxel_0(kernelContext_9);


    float surface_0 = _S36 * 0.5;
    float _S37 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 223
    recent_0[int(0)] = 1.0e+06;

#line 223
    recent_0[int(1)] = 1.0e+06;

#line 223
    recent_0[int(2)] = 1.0e+06;

#line 223
    float closest_1 = 1.0e+06;

#line 223
    uint recentSlot_0 = 0U;

#line 223
    uint step_0 = 0U;

#line 223
    float t_2 = _S37;


    for(;;)
    {

#line 226
        if(step_0 < 192U)
        {
        }
        else
        {

#line 226
            break;
        }

#line 227
        if(t_2 > maxDistance_0)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_2 = origin_1 + direction_0 * float3(t_2) ;

)cy_msl"
    R"cy_msl(#line 232
        float _S38 = giDistance_0(position_2, kernelContext_9);

        if(_S38 < surface_0)
        {

#line 235
            (&result_0)->hit_0 = true;
            (&result_0)->t_1 = t_2;
            (&result_0)->position_1 = position_2;


            float3 _S39 = float3(_S36, 0.0, 0.0);

#line 240
            float _S40 = giDistance_0(position_2 + _S39, kernelContext_9);

#line 240
            float _S41 = giDistance_0(position_2 - _S39, kernelContext_9);

#line 240
            float _S42 = _S40 - _S41;

            float3 _S43 = float3(0.0, _S36, 0.0);

#line 242
            float _S44 = giDistance_0(position_2 + _S43, kernelContext_9);

#line 242
            float _S45 = giDistance_0(position_2 - _S43, kernelContext_9);

#line 242
            float _S46 = _S44 - _S45;

            float3 _S47 = float3(0.0, 0.0, _S36);

#line 244
            float _S48 = giDistance_0(position_2 + _S47, kernelContext_9);

#line 244
            float _S49 = giDistance_0(position_2 - _S47, kernelContext_9);

            (&result_0)->normal_1 = giNormalisedOr_0(float3(_S42, _S46, _S48 - _S49), - direction_0);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S50 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S38;
        uint _S51 = (recentSlot_0 + 1U) % 3U;
        float t_3 = t_2 + max(_S38, surface_0);

#line 226
        uint step_1 = step_0 + 1U;

#line 226
        closest_1 = _S50;

#line 226
        recentSlot_0 = _S51;

#line 226
        step_0 = step_1;

#line 226
        t_2 = t_3;

#line 226
    }

#line 255
    (&result_0)->exhausted_0 = true;

    (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
    return result_0;
}


#line 209
GiTraceHit_0 giSphereTrace_1(float3 origin_2, float3 direction_1, float maxDistance_1, float tMin_1, KernelContext_0 thread* kernelContext_10)
{

#line 210
    thread GiTraceHit_0 result_1;
    (&result_1)->hit_0 = false;
    (&result_1)->exhausted_0 = false;
    (&result_1)->t_1 = 0.0;
    (&result_1)->closest_0 = 0.0;
    (&result_1)->position_1 = float3(0.0, 0.0, 0.0);
    (&result_1)->normal_1 = float3(0.0, 1.0, 0.0);

#line 216
    uint _S52 = giLevelCount_0(kernelContext_10);
    if(_S52 == 0U)
    {

#line 218
        return result_1;
    }

#line 218
    float _S53 = giFinestVoxel_0(kernelContext_10);


    float surface_1 = _S53 * 0.5;
    float _S54 = max(tMin_1, surface_1);
    thread array<float, int(3)> recent_1;

#line 223
    recent_1[int(0)] = 1.0e+06;

#line 223
    recent_1[int(1)] = 1.0e+06;

#line 223
    recent_1[int(2)] = 1.0e+06;

#line 223
    float closest_2 = 1.0e+06;

#line 223
    uint recentSlot_1 = 0U;

#line 223
    uint step_2 = 0U;

#line 223
    float t_4 = _S54;


    for(;;)
    {

#line 226
        if(step_2 < 192U)
        {
        }
        else
        {

#line 226
            break;
        }

#line 227
        if(t_4 > maxDistance_1)
        {
            (&result_1)->closest_0 = min(closest_2, min(recent_1[int(0)], min(recent_1[int(1)], recent_1[int(2)])));
            return result_1;
        }
        float3 position_3 = origin_2 + direction_1 * float3(t_4) ;

#line 232
        float _S55 = giDistance_0(position_3, kernelContext_10);

        if(_S55 < surface_1)
        {

#line 235
            (&result_1)->hit_0 = true;
            (&result_1)->t_1 = t_4;
            (&result_1)->position_1 = position_3;


            float3 _S56 = float3(_S53, 0.0, 0.0);

#line 240
            float _S57 = giDistance_0(position_3 + _S56, kernelContext_10);

#line 240
            float _S58 = giDistance_0(position_3 - _S56, kernelContext_10);

#line 240
            float _S59 = _S57 - _S58;

            float3 _S60 = float3(0.0, _S53, 0.0);

#line 242
            float _S61 = giDistance_0(position_3 + _S60, kernelContext_10);

#line 242
            float _S62 = giDistance_0(position_3 - _S60, kernelContext_10);

#line 242
            float _S63 = _S61 - _S62;

            float3 _S64 = float3(0.0, 0.0, _S53);

#line 244
            float _S65 = giDistance_0(position_3 + _S64, kernelContext_10);

#line 244
            float _S66 = giDistance_0(position_3 - _S64, kernelContext_10);

            (&result_1)->normal_1 = giNormalisedOr_0(float3(_S59, _S63, _S65 - _S66), - direction_1);
            (&result_1)->closest_0 = closest_2;
            return result_1;
        }
        float _S67 = min(closest_2, recent_1[recentSlot_1]);
        recent_1[recentSlot_1] = _S55;
        uint _S68 = (recentSlot_1 + 1U) % 3U;
        float t_5 = t_4 + max(_S55, surface_1);

#line 226
        uint step_3 = step_2 + 1U;

#line 226
        closest_2 = _S67;

#line 226
        recentSlot_1 = _S68;

#line 226
        step_2 = step_3;

#line 226
        t_4 = t_5;

#line 226
    }

#line 255
    (&result_1)->exhausted_0 = true;

    (&result_1)->closest_0 = min(closest_2, min(recent_1[int(0)], min(recent_1[int(1)], recent_1[int(2)])));
    return result_1;
}


#line 301
bool giFieldOccluded_0(float3 from_0, float3 to_0, KernelContext_0 thread* kernelContext_11)
{

#line 302
    float3 offset_1 = to_0 - from_0;
    float span_0 = length(offset_1);
    if(span_0 <= 0.00009999999747379)
    {

#line 305
        return false;
    }

#line 305
    float _S69 = giFinestVoxel_1(kernelContext_11);

    float bias_0 = _S69 * 2.0;
    if(span_0 <= bias_0)
    {

#line 309
        return false;
    }

#line 309
    GiTraceHit_0 _S70 = giSphereTrace_1(from_0, offset_1 / float3(span_0) , span_0 * 0.99900001287460327, bias_0, kernelContext_11);

    return _S70.hit_0;
}


bool giOccluded_0(float3 from_1, float3 to_1, KernelContext_0 thread* kernelContext_12)
{

#line 316
    float3 offset_2 = to_1 - from_1;
    float span_1 = length(offset_2);
    if(span_1 <= 0.00009999999747379)
    {

#line 319
        return false;
    }
    float3 dir_1 = (as_type<float3>((kernelContext_12->giScene_0->constants_0[6U].xyz)));

#line 321
    bool _S71;
    if((kernelContext_12->giScene_0->constants_0[9U].x) != 0U)
    {

#line 322
        _S71 = (dot(offset_2 / float3(span_1) , - dir_1)) > 0.99989998340606689;

#line 322
    }
    else
    {

#line 322
        _S71 = false;

#line 322
    }

#line 322
    if(_S71)
    {

#line 322
        bool _S72 = giShadowMapShadowed_0(from_1, kernelContext_12);
        return _S72;
    }

)cy_msl"
    R"cy_msl(#line 323
    bool _S73 = giFieldOccluded_0(from_1, to_1, kernelContext_12);

    return _S73;
}


#line 349
float3 giShadedDirect_0(float3 position_4, float3 normal_2, KernelContext_0 thread* kernelContext_13)
{
    uint _S74 = kernelContext_13->giScene_0->constants_0[0U].z;

#line 351
    float3 total_0 = float3(0.0, 0.0, 0.0);

#line 351
    uint light_1 = 0U;
    for(;;)
    {

#line 352
        if(light_1 < _S74)
        {
        }
        else
        {

#line 352
            break;
        }

#line 352
        float3 _S75 = giDirectRadiance_0(light_1, position_4, normal_2, kernelContext_13);

#line 352
        bool _S76;

        if((_S75.x) <= 0.0)
        {

#line 354
            _S76 = (_S75.y) <= 0.0;

#line 354
        }
        else
        {

#line 354
            _S76 = false;

#line 354
        }

#line 354
        bool _S77;

#line 354
        if(_S76)
        {

#line 354
            _S77 = (_S75.z) <= 0.0;

#line 354
        }
        else
        {

#line 354
            _S77 = false;

#line 354
        }

#line 354
        if(_S77)
        {

#line 355
            light_1 = light_1 + 1U;

#line 352
            continue;
        }



        uint _S78 = light_1 * 4U;

#line 357
        float4 a_2 = kernelContext_13->giScene_0->lights_0[_S78];
        float4 b_2 = kernelContext_13->giScene_0->lights_0[_S78 + 1U];

#line 358
        float3 target_0;


        if((kernelContext_13->giScene_0->lights_0[_S78 + 2U].w) != 0.0)
        {

#line 361
            target_0 = position_4 - giNormalisedOr_0(b_2.xyz, float3(0.0, 1.0, 0.0)) * float3(1000.0) ;

#line 361
        }
        else
        {

#line 361
            target_0 = a_2.xyz;

#line 361
        }

#line 361
        bool _S79 = giOccluded_0(position_4 + normal_2 * float3(0.00999999977648258) , target_0, kernelContext_13);

        if(_S79)
        {

#line 364
            light_1 = light_1 + 1U;

#line 352
            continue;
        }

#line 352
        total_0 = total_0 + _S75;

#line 352
        light_1 = light_1 + 1U;

#line 352
    }

#line 368
    return total_0;
}


#line 262
float3 giHemisphereDirection_0(float3 normal_3, uint index_0, uint count_1)
{
    float _S80 = float(index_0);
    float radius_0 = sqrt((_S80 + 0.5) / float(count_1));
    float angle_0 = 2.3999631404876709 * _S80;
    float x_0 = radius_0 * cos(angle_0);
    float y_0 = radius_0 * sin(angle_0);
    float z_0 = sqrt(max(0.0, 1.0 - x_0 * x_0 - y_0 * y_0));

#line 269
    float3 tangent_0;
    if((abs(normal_3.y)) < 0.99000000953674316)
    {

#line 270
        tangent_0 = cross(float3(0.0, 1.0, 0.0), normal_3);

#line 270
    }
    else
    {

#line 270
        tangent_0 = cross(float3(1.0, 0.0, 0.0), normal_3);

#line 270
    }

    float3 tangent_1 = giNormalisedOr_0(tangent_0, float3(1.0, 0.0, 0.0));

    return giNormalisedOr_0(tangent_1 * float3(x_0)  + cross(normal_3, tangent_1) * float3(y_0)  + normal_3 * float3(z_0) , normal_3);
}


#line 373
float3 giSky_0(float3 direction_2, KernelContext_0 thread* kernelContext_14)
{

#line 374
    float4 zenith_0 = (as_type<float4>((kernelContext_14->giScene_0->constants_0[2U])));
    float3 horizon_0 = (as_type<float3>((kernelContext_14->giScene_0->constants_0[3U].xyz)));
    float3 ground_0 = (as_type<float3>((kernelContext_14->giScene_0->constants_0[4U].xyz)));

    float _S81 = giNormalisedOr_0(direction_2, float3(0.0, 1.0, 0.0)).y;

#line 378
    if(_S81 < 0.0)
    {

#line 379
        return ground_0 * float3(zenith_0.w) ;
    }

    return (horizon_0 + (zenith_0.xyz - horizon_0) * float3(clamp(pow(_S81, 0.44999998807907104), 0.0, 1.0)) ) * float3(zenith_0.w) ;
}



int giCell_0(float value_1, float cell_0)
{

#line 388
    return int(floor(value_1 / cell_0));
}

uint giBucket_0(int x_1, int y_1, int z_1, uint buckets_0)
{
    return (((uint(x_1) * 73856093U) ^ (uint(y_1) * 19349663U)) ^ (uint(z_1) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_5, float3 normal_4, KernelContext_0 thread* kernelContext_15)
{

#line 398
    uint4 grid_0 = kernelContext_15->giScene_0->constants_0[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 401
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_1 = (as_type<float>((grid_0.z)));
    float _S82 = radius_1 * radius_1;
    float _S83 = position_5.x;

#line 406
    int _S84 = giCell_0(_S83 - radius_1, cell_1);

#line 406
    float _S85 = position_5.y;

#line 406
    int _S86 = giCell_0(_S85 - radius_1, cell_1);
    float _S87 = position_5.z;

#line 407
    int _S88 = giCell_0(_S87 - radius_1, cell_1);
    int _S89 = giCell_0(_S83 + radius_1, cell_1);

#line 408
    int _S90 = giCell_0(_S85 + radius_1, cell_1);
    int _S91 = giCell_0(_S87 + radius_1, cell_1);

#line 409
    float bestScore_0 = 1.0e+09;

#line 409
    uint best_0 = 4294967295U;

#line 409
    int z_2 = _S88;


    for(;;)
    {

#line 412
        if(z_2 <= _S91)
        {
        }
        else
        {

#line 412
            break;
        }

#line 412
        int y_2 = _S86;
        for(;;)
        {

#line 413
            if(y_2 <= _S90)
            {
            }
            else
            {

#line 413
                break;
            }

#line 413
            float bestScore_1 = bestScore_0;

#line 413
            uint best_1 = best_0;

#line 413
            int x_2 = _S84;
            for(;;)
            {

#line 414
                if(x_2 <= _S89)
                {
                }
                else
                {

)cy_msl"
    R"cy_msl(#line 414
                    break;
                }
                uint _S92 = giBucket_0(x_2, y_2, z_2, buckets_1) * 2U;

#line 416
                uint first_0 = kernelContext_15->giScene_0->gridRanges_0[_S92];
                uint _S93 = kernelContext_15->giScene_0->gridRanges_0[_S92 + 1U];

#line 417
                float bestScore_2 = bestScore_1;

#line 417
                uint best_2 = best_1;

#line 417
                uint item_0 = first_0;
                for(;;)
                {

#line 418
                    if(item_0 < (first_0 + _S93))
                    {
                    }
                    else
                    {

#line 418
                        break;
                    }

#line 419
                    uint handle_0 = kernelContext_15->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_15->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 421
                        item_0 = item_0 + 1U;

#line 418
                        continue;
                    }



                    uint _S94 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_15->giScene_0->cards_0[_S94 + 1U].xyz, normal_4);
                    if(alignment_0 <= 0.25)
                    {

#line 426
                        item_0 = item_0 + 1U;

#line 418
                        continue;
                    }

#line 428
                    float3 offset_3 = kernelContext_15->giScene_0->cards_0[_S94].xyz - position_5;
                    float squared_1 = dot(offset_3, offset_3);
                    if(squared_1 > _S82)
                    {

#line 431
                        item_0 = item_0 + 1U;

#line 418
                        continue;
                    }

#line 433
                    float score_0 = squared_1 / alignment_0;

#line 433
                    bool _S95;
                    if(score_0 < bestScore_2)
                    {

#line 434
                        _S95 = true;

#line 434
                    }
                    else
                    {

#line 434
                        if(score_0 == bestScore_2)
                        {

#line 434
                            _S95 = handle_0 < best_2;

#line 434
                        }
                        else
                        {

#line 434
                            _S95 = false;

#line 434
                        }

#line 434
                    }

#line 434
                    float bestScore_3;

#line 434
                    uint best_3;

#line 434
                    if(_S95)
                    {

#line 434
                        bestScore_3 = score_0;

#line 434
                        best_3 = handle_0;

#line 434
                    }
                    else
                    {

#line 434
                        bestScore_3 = bestScore_2;

#line 434
                        best_3 = best_2;

#line 434
                    }

#line 434
                    bestScore_2 = bestScore_3;

#line 434
                    best_2 = best_3;

#line 418
                    item_0 = item_0 + 1U;

#line 418
                }

#line 414
                int x_3 = x_2 + int(1);

#line 414
                bestScore_1 = bestScore_2;

#line 414
                best_1 = best_2;

#line 414
                x_2 = x_3;

#line 414
            }

#line 413
            int y_3 = y_2 + int(1);

#line 413
            bestScore_0 = bestScore_1;

#line 413
            best_0 = best_1;

#line 413
            y_2 = y_3;

#line 413
        }

#line 412
        z_2 = z_2 + int(1);

#line 412
    }

#line 442
    return best_0;
}



float3 giGather_0(float3 position_6, float3 normal_5, KernelContext_0 thread* kernelContext_16)
{

#line 448
    uint rays_1 = kernelContext_16->giScene_0->constants_0[0U].w;

#line 448
    bool _S96;
    if(rays_1 == 0U)
    {

#line 449
        _S96 = true;

#line 449
    }
    else
    {

#line 449
        uint _S97 = giLevelCount_0(kernelContext_16);

#line 449
        _S96 = _S97 == 0U;

#line 449
    }

#line 449
    if(_S96)
    {

#line 450
        return float3(0.0, 0.0, 0.0);
    }
    float _S98 = (as_type<float>((kernelContext_16->giScene_0->constants_0[1U].w)));

#line 452
    float _S99 = giFinestVoxel_0(kernelContext_16);
    float _S100 = _S99 * 2.0;

#line 453
    float3 total_1 = float3(0.0, 0.0, 0.0);

#line 453
    uint index_1 = 0U;

    for(;;)
    {

#line 455
        if(index_1 < rays_1)
        {
        }
        else
        {

#line 455
            break;
        }

#line 456
        float3 direction_3 = giHemisphereDirection_0(normal_5, index_1, rays_1);

#line 456
        GiTraceHit_0 _S101 = giSphereTrace_0(position_6, direction_3, _S98, _S100, kernelContext_16);

        if(!_S101.hit_0)
        {

#line 458
            float3 _S102 = giSky_0(direction_3, kernelContext_16);

#line 458
            total_1 = total_1 + _S102;

            index_1 = index_1 + 1U;

#line 455
            continue;
        }

#line 455
        uint _S103 = giFindCard_0(_S101.position_1, _S101.normal_1, kernelContext_16);

#line 455
        float3 total_2;

#line 463
        if(_S103 != 4294967295U)
        {

#line 463
            total_2 = total_1 + (*(kernelContext_16->giScene_0->cardState_0+_S103 * 2U)).xyz;

#line 463
        }
        else
        {

#line 463
            total_2 = total_1;

#line 463
        }

#line 463
        total_1 = total_2;

#line 455
        index_1 = index_1 + 1U;

)cy_msl"
    R"cy_msl(#line 455
    }

#line 467
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

/// cyGiCommitCards.metal, 1806 bytes.
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
    *((&kernelContext_0)->giScene_0->cardState_0+(_S3 + 1U)) = float4(accumulated_0.xyz, 0.0);
    return;
}

)cy_msl";

}  // namespace cy::rendering::gi_gpu

#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the device GI dispatches. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

#include <cstddef>
#include <initializer_list>

namespace cy::rendering::gi_gpu {

namespace detail {

/// One MSL module assembled from several raw-string chunks. See embed_msl.py for why.
template <std::size_t Size>
struct MslText {
    char text[Size];
};

template <std::size_t... Sizes>
consteval MslText<(Sizes + ...) - sizeof...(Sizes) + 1> join_msl(const char (&... chunks)[Sizes]) {
    MslText<(Sizes + ...) - sizeof...(Sizes) + 1> joined{};
    std::size_t at = 0;
    for (const char* chunk : {static_cast<const char*>(chunks)...}) {
        for (; *chunk != '\0'; ++chunk) {
            joined.text[at++] = *chunk;
        }
    }
    return joined;
}

}  // namespace detail

/// cyGiTraceRays.metal, 11415 bytes.
inline constexpr char kGiTraceRaysMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 81 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiDispatch_0
{
    uint count_0;
    uint frame_0;
    uint reserved0_0;
    uint reserved1_0;
};


#line 50
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


#line 50
struct KernelContext_0
{
    GiDispatch_0 constant* giDispatch_0;
    GiSceneSet_default_0 constant* giScene_0;
};


#line 115
uint giLevelCount_0(KernelContext_0 thread* kernelContext_0)
{

#line 116
    return kernelContext_0->giScene_0->constants_0[0U].x;
}


#line 123
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_1)
{

#line 124
    return (as_type<float>((kernelContext_1->giScene_0->constants_0[9U].y)));
}


#line 119
uint giWindowBricks_0(KernelContext_0 thread* kernelContext_2)
{

#line 120
    return kernelContext_2->giScene_0->constants_0[0U].y;
}


#line 167
int giWrap_0(int value_0, int modulus_0)
{

#line 168
    uint _S1 = uint(modulus_0);
    uint _S2 = (uint(value_0) + (_S1 << 20U)) % _S1;

#line 169
    return int(_S2);
}


#line 95
float giLerp_0(float a_0, float b_0, float t_0)
{

#line 96
    return a_0 + (b_0 - a_0) * t_0;
}


#line 128
float giTrilinear_0(uint slot_0, float fx_0, float fy_0, float fz_0, KernelContext_0 thread* kernelContext_3)
{
    int x0_0 = clamp(int(floor(fx_0)), int(0), int(3));
    int y0_0 = clamp(int(floor(fy_0)), int(0), int(3));
    int z0_0 = clamp(int(floor(fz_0)), int(0), int(3));



    float tx_0 = clamp(fx_0 - float(x0_0), 0.0, 1.0);
    float ty_0 = clamp(fy_0 - float(y0_0), 0.0, 1.0);

    uint base_0 = slot_0 * 125U;

    uint _S3 = uint(z0_0) * 5U;

#line 141
    uint _S4 = uint(y0_0);

#line 141
    uint _S5 = base_0 + (_S3 + _S4) * 5U;

#line 141
    uint _S6 = uint(x0_0);
    uint _S7 = uint(x0_0 + int(1));
    uint _S8 = uint(y0_0 + int(1));

#line 143
    uint _S9 = base_0 + (_S3 + _S8) * 5U;

    uint _S10 = uint(z0_0 + int(1)) * 5U;

#line 145
    uint _S11 = base_0 + (_S10 + _S4) * 5U;

    uint _S12 = base_0 + (_S10 + _S8) * 5U;

#line 153
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S5 + _S6], kernelContext_3->giScene_0->bricks_0[_S5 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S9 + _S6], kernelContext_3->giScene_0->bricks_0[_S9 + _S7], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S11 + _S6], kernelContext_3->giScene_0->bricks_0[_S11 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S12 + _S6], kernelContext_3->giScene_0->bricks_0[_S12 + _S7], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 175
float giSampleLevel_0(uint level_0, float3 p_0, float thread* farDistance_0, KernelContext_0 thread* kernelContext_4)
{

#line 176
    uint _S13 = 16U + level_0 * 2U;

#line 176
    float4 shape_0 = (as_type<float4>((kernelContext_4->giScene_0->constants_0[_S13])));
    int4 origin_0 = (as_type<int4>((kernelContext_4->giScene_0->constants_0[_S13 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_0.x / brick_0));
    int by_0 = int(floor(p_0.y / brick_0));
    int bz_0 = int(floor(p_0.z / brick_0));

#line 183
    uint _S14 = giWindowBricks_0(kernelContext_4);
    int dim_0 = int(_S14);
    int _S15 = origin_0.x;

#line 185
    bool _S16;

#line 185
    if(bx_0 < _S15)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = by_0 < (origin_0.y);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = bz_0 < (origin_0.z);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = bx_0 >= (_S15 + dim_0);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = by_0 >= (origin_0.y + dim_0);

#line 185
    }
    if(_S16)
    {

#line 186
        _S16 = true;

#line 186
    }
    else
    {

#line 186
        _S16 = bz_0 >= (origin_0.z + dim_0);

#line 186
    }

#line 185
    if(_S16)
    {
        return *farDistance_0;
    }
    int _S17 = giWrap_0(bz_0, dim_0);

#line 189
    uint _S18 = uint(dim_0);

#line 189
    uint _S19 = uint(_S17) * _S18;

#line 189
    int _S20 = giWrap_0(by_0, dim_0);

#line 189
    uint _S21 = (_S19 + uint(_S20)) * _S18;
    int _S22 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_4->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S21 + uint(_S22))];
    if(slot_1 == 4294967295U)
    {

#line 193
        return *farDistance_0;
    }

    float3 local_0 = (p_0 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 196
    float _S23 = giTrilinear_0(slot_1, local_0.x, local_0.y, local_0.z, kernelContext_4);
    return _S23;
}


float giDistance_0(float3 p_1, KernelContext_0 thread* kernelContext_5)
{

#line 201
    uint _S24 = giLevelCount_0(kernelContext_5);

#line 201
    float answer_0 = 1.0e+06;

#line 201
    uint level_1 = 0U;


    for(;;)
    {

#line 204
        if(level_1 < _S24)
        {
        }
        else
        {

)cy_msl"
    R"cy_msl(#line 204
            break;
        }

#line 205
        thread float farDistance_1 = 0.0;

#line 205
        float _S25 = giSampleLevel_0(level_1, p_1, &farDistance_1, kernelContext_5);

        float _S26 = min(answer_0, _S25);
        if(_S25 < farDistance_1)
        {

#line 209
            return _S25;
        }

#line 204
        uint level_2 = level_1 + 1U;

#line 204
        answer_0 = _S26;

#line 204
        level_1 = level_2;

#line 204
    }

#line 212
    return answer_0;
}


#line 99
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 100
    float squared_0 = dot(v_0, v_0);

#line 100
    float3 _S27;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 101
        _S27 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 101
    }
    else
    {

#line 101
        _S27 = fallback_0;

#line 101
    }

#line 101
    return _S27;
}


#line 215
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

#line 226
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_0 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_1 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_0 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_0 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S28 = giLevelCount_0(kernelContext_6);
    if(_S28 == 0U)
    {

#line 234
        return result_0;
    }

#line 234
    float _S29 = giFinestVoxel_0(kernelContext_6);


    float surface_0 = _S29 * 0.5;
    float _S30 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 239
    recent_0[int(0)] = 1.0e+06;

#line 239
    recent_0[int(1)] = 1.0e+06;

#line 239
    recent_0[int(2)] = 1.0e+06;

#line 239
    float closest_1 = 1.0e+06;

#line 239
    uint recentSlot_0 = 0U;

#line 239
    uint step_0 = 0U;

#line 239
    float t_2 = _S30;


    for(;;)
    {

#line 242
        if(step_0 < 192U)
        {
        }
        else
        {

#line 242
            break;
        }

#line 243
        if(t_2 > maxDistance_0)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_1 = origin_1 + direction_0 * float3(t_2) ;

#line 248
        float _S31 = giDistance_0(position_1, kernelContext_6);

        if(_S31 < surface_0)
        {

#line 251
            (&result_0)->hit_0 = true;
            (&result_0)->t_1 = t_2;
            (&result_0)->position_0 = position_1;


            float3 _S32 = float3(_S29, 0.0, 0.0);

#line 256
            float _S33 = giDistance_0(position_1 + _S32, kernelContext_6);

#line 256
            float _S34 = giDistance_0(position_1 - _S32, kernelContext_6);

#line 256
            float _S35 = _S33 - _S34;

            float3 _S36 = float3(0.0, _S29, 0.0);

#line 258
            float _S37 = giDistance_0(position_1 + _S36, kernelContext_6);

#line 258
            float _S38 = giDistance_0(position_1 - _S36, kernelContext_6);

#line 258
            float _S39 = _S37 - _S38;

            float3 _S40 = float3(0.0, 0.0, _S29);

#line 260
            float _S41 = giDistance_0(position_1 + _S40, kernelContext_6);

#line 260
            float _S42 = giDistance_0(position_1 - _S40, kernelContext_6);

            (&result_0)->normal_0 = giNormalisedOr_0(float3(_S35, _S39, _S41 - _S42), - direction_0);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S43 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S31;
        uint _S44 = (recentSlot_0 + 1U) % 3U;
        float t_3 = t_2 + max(_S31, surface_0);

#line 242
        uint step_1 = step_0 + 1U;

#line 242
        closest_1 = _S43;

#line 242
        recentSlot_0 = _S44;

#line 242
        step_0 = step_1;

#line 242
        t_2 = t_3;

#line 242
    }

#line 271
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

/// cyGiShadeCards.metal, 35673 bytes.
inline constexpr char kGiShadeCardsMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 99 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 100
    float squared_0 = dot(v_0, v_0);

#line 100
    float3 _S1;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 101
        _S1 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 101
    }
    else
    {

#line 101
        _S1 = fallback_0;

#line 101
    }

#line 101
    return _S1;
}


#line 349
void giEmitterBasis_0(float3 normal_0, float3 thread* tangent_0, float3 thread* bitangent_0)
{

#line 349
    float3 _S2;
    if((abs(normal_0.y)) < 0.99000000953674316)
    {

#line 350
        _S2 = cross(float3(0.0, 1.0, 0.0), normal_0);

#line 350
    }
    else
    {

#line 350
        _S2 = cross(float3(1.0, 0.0, 0.0), normal_0);

#line 350
    }

    float3 _S3 = giNormalisedOr_0(_S2, float3(1.0, 0.0, 0.0));

#line 352
    *tangent_0 = _S3;
    *bitangent_0 = cross(normal_0, _S3);
    return;
}

float giEmitterFormFactor_0(float3 centre_0, float3 emitterNormal_0, float area_0, float3 position_0, float3 normal_1)
{

#line 358
    bool _S4;
    if(area_0 <= 0.0)
    {

#line 359
        _S4 = true;

#line 359
    }
    else
    {

#line 359
        _S4 = (dot(position_0 - centre_0, emitterNormal_0)) <= 0.0;

#line 359
    }

#line 359
    if(_S4)
    {

#line 360
        return 0.0;
    }
    thread float3 tangent_1;
    thread float3 bitangent_1;
    giEmitterBasis_0(emitterNormal_0, &tangent_1, &bitangent_1);

#line 364
    float3 _S5 = float3((sqrt(area_0) * 0.5)) ;

    float3 u_0 = tangent_1 * _S5;
    float3 v_1 = bitangent_1 * _S5;
    float3 base_0 = centre_0 - position_0;
    float3 _S6 = base_0 - u_0;

#line 369
    float3 _S7 = base_0 + u_0;

#line 369
    array<float3, int(4)> _S8 = { _S6 - v_1, _S7 - v_1, _S7 + v_1, _S6 + v_1 };

    thread array<float3, int(5)> polygon_0;

#line 371
    uint index_0 = 0U;

#line 371
    uint count_0 = 0U;

    for(;;)
    {

#line 373
        if(index_0 < 4U)
        {
        }
        else
        {

#line 373
            break;
        }

#line 373
        uint _S9 = index_0;

        uint _S10 = index_0 + 1U;

#line 375
        uint _S11 = _S10 % 4U;
        float heightA_0 = dot(_S8[index_0], normal_1);
        float heightB_0 = dot(_S8[_S11], normal_1);
        bool _S12 = heightA_0 > 0.0;

#line 378
        uint count_1;

#line 378
        if(_S12)
        {

#line 379
            polygon_0[count_0] = _S8[_S9];

#line 379
            count_1 = count_0 + 1U;

#line 378
        }
        else
        {

#line 378
            count_1 = count_0;

#line 378
        }



        if(_S12 != (heightB_0 > 0.0))
        {

#line 383
            polygon_0[count_1] = _S8[_S9] + (_S8[_S11] - _S8[_S9]) * float3((heightA_0 / (heightA_0 - heightB_0))) ;

#line 383
            count_0 = count_1 + 1U;

#line 382
        }
        else
        {

#line 382
            count_0 = count_1;

#line 382
        }

#line 373
        index_0 = _S10;

#line 373
    }

#line 387
    if(count_0 < 3U)
    {

#line 388
        return 0.0;
    }

#line 388
    index_0 = 0U;

#line 388
    float sum_0 = 0.0;



    for(;;)
    {

#line 392
        if(index_0 < count_0)
        {
        }
        else
        {

#line 392
            break;
        }

#line 393
        float3 from_0 = giNormalisedOr_0(polygon_0[index_0], normal_1);
        uint _S13 = index_0 + 1U;

#line 394
        uint _S14 = _S13 % count_0;

#line 394
        float3 to_0 = giNormalisedOr_0(polygon_0[_S14], normal_1);
        float3 edge_0 = cross(from_0, to_0);
        float sine_0 = length(edge_0);
        if(sine_0 <= 9.999999960041972e-13)
        {

#line 398
            index_0 = _S13;

#line 392
            continue;
        }

#line 392
        sum_0 = sum_0 + atan2(sine_0, dot(from_0, to_0)) * (dot(edge_0, normal_1) / sine_0);

#line 392
        index_0 = _S13;

#line 392
    }

#line 402
    return abs(sum_0) * 0.15915493667125702;
}


#line 81
struct GiDispatch_0
{
    uint count_2;
    uint frame_0;
    uint reserved0_0;
    uint reserved1_0;
};


#line 50
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


#line 50
struct KernelContext_0
{
    GiDispatch_0 constant* giDispatch_0;
    GiSceneSet_default_0 constant* giScene_0;
};


)cy_msl"
    R"cy_msl(#line 407
float3 giDirectRadiance_0(uint light_0, float3 position_1, float3 normal_2, KernelContext_0 thread* kernelContext_0)
{

#line 408
    uint _S15 = light_0 * 4U;

#line 408
    float4 a_0 = kernelContext_0->giScene_0->lights_0[_S15];
    float4 b_0 = kernelContext_0->giScene_0->lights_0[_S15 + 1U];
    float4 c_0 = kernelContext_0->giScene_0->lights_0[_S15 + 2U];
    if((c_0.w) != 0.0)
    {

        return c_0.xyz * float3((a_0.w * max(0.0, dot(normal_2, - giNormalisedOr_0(b_0.xyz, float3(0.0, 1.0, 0.0)))))) ;
    }
    float3 _S16 = a_0.xyz;

#line 416
    float3 offset_0 = _S16 - position_1;
    float _S17 = max(dot(offset_0, offset_0), 9.99999997475242708e-07);
    float _S18 = b_0.w;

#line 418
    bool _S19;

#line 418
    if(_S18 > 0.0)
    {

#line 418
        _S19 = _S17 > (_S18 * _S18);

#line 418
    }
    else
    {

#line 418
        _S19 = false;

#line 418
    }

#line 418
    if(_S19)
    {

#line 419
        return float3(0.0, 0.0, 0.0);
    }
    float4 d_0 = kernelContext_0->giScene_0->lights_0[_S15 + 3U];
    float _S20 = d_0.w;

#line 422
    if(_S20 > 0.0)
    {

#line 423
        float3 _S21 = c_0.xyz;

#line 423
        float _S22 = a_0.w;

#line 423
        float _S23 = giEmitterFormFactor_0(_S16, d_0.xyz, _S20, position_1, normal_2);

#line 423
        return _S21 * float3((_S22 * _S23)) ;
    }


    return c_0.xyz * float3((a_0.w * max(0.0, dot(normal_2, offset_0 / float3(sqrt(_S17)) )) / _S17)) ;
}


#line 295
bool giShadowMapShadowed_0(float3 p_0, KernelContext_0 thread* kernelContext_1)
{

#line 296
    float4 centre_1 = (as_type<float4>((kernelContext_1->giScene_0->constants_0[5U])));
    float4 dir_0 = (as_type<float4>((kernelContext_1->giScene_0->constants_0[6U])));

    float4 up_0 = (as_type<float4>((kernelContext_1->giScene_0->constants_0[8U])));
    uint resolution_0 = kernelContext_1->giScene_0->constants_0[7U].w;
    float extent_0 = centre_1.w;
    float3 relative_0 = p_0 - centre_1.xyz;
    float u_1 = dot(relative_0, (as_type<float4>((kernelContext_1->giScene_0->constants_0[7U]))).xyz);
    float v_2 = dot(relative_0, up_0.xyz);
    float _S24 = dir_0.w;

#line 305
    float depth_0 = dot(relative_0, dir_0.xyz) + _S24 * 0.5;

#line 305
    bool _S25;
    if((abs(u_1)) >= extent_0)
    {

#line 306
        _S25 = true;

#line 306
    }
    else
    {

#line 306
        _S25 = (abs(v_2)) >= extent_0;

#line 306
    }

#line 306
    if(_S25)
    {

#line 306
        _S25 = true;

#line 306
    }
    else
    {

#line 306
        _S25 = depth_0 < 0.0;

#line 306
    }

#line 306
    if(_S25)
    {

#line 306
        _S25 = true;

#line 306
    }
    else
    {

#line 306
        _S25 = depth_0 > _S24;

#line 306
    }

#line 306
    if(_S25)
    {

#line 307
        return false;
    }
    int last_0 = int(resolution_0) - int(1);
    float _S26 = float(resolution_0);


    return kernelContext_1->giScene_0->shadowDepths_0[uint(clamp(int(floor((v_2 / extent_0 * 0.5 + 0.5) * _S26)), int(0), last_0)) * resolution_0 + uint(clamp(int(floor((u_1 / extent_0 * 0.5 + 0.5) * _S26)), int(0), last_0))] < (depth_0 - up_0.w);
}


#line 123
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_2)
{

#line 124
    return (as_type<float>((kernelContext_2->giScene_0->constants_0[9U].y)));
}


#line 123
float giFinestVoxel_1(KernelContext_0 thread* kernelContext_3)
{

#line 124
    return (as_type<float>((kernelContext_3->giScene_0->constants_0[9U].y)));
}


#line 115
uint giLevelCount_0(KernelContext_0 thread* kernelContext_4)
{

#line 116
    return kernelContext_4->giScene_0->constants_0[0U].x;
}

uint giWindowBricks_0(KernelContext_0 thread* kernelContext_5)
{

#line 120
    return kernelContext_5->giScene_0->constants_0[0U].y;
}


#line 167
int giWrap_0(int value_0, int modulus_0)
{

#line 168
    uint _S27 = uint(modulus_0);
    uint _S28 = (uint(value_0) + (_S27 << 20U)) % _S27;

#line 169
    return int(_S28);
}


#line 95
float giLerp_0(float a_1, float b_1, float t_0)
{

#line 96
    return a_1 + (b_1 - a_1) * t_0;
}


#line 128
float giTrilinear_0(uint slot_0, float fx_0, float fy_0, float fz_0, KernelContext_0 thread* kernelContext_6)
{
    int x0_0 = clamp(int(floor(fx_0)), int(0), int(3));
    int y0_0 = clamp(int(floor(fy_0)), int(0), int(3));
    int z0_0 = clamp(int(floor(fz_0)), int(0), int(3));



    float tx_0 = clamp(fx_0 - float(x0_0), 0.0, 1.0);
    float ty_0 = clamp(fy_0 - float(y0_0), 0.0, 1.0);

    uint base_1 = slot_0 * 125U;

    uint _S29 = uint(z0_0) * 5U;

#line 141
    uint _S30 = uint(y0_0);

#line 141
    uint _S31 = base_1 + (_S29 + _S30) * 5U;

#line 141
    uint _S32 = uint(x0_0);
    uint _S33 = uint(x0_0 + int(1));
    uint _S34 = uint(y0_0 + int(1));

#line 143
    uint _S35 = base_1 + (_S29 + _S34) * 5U;

    uint _S36 = uint(z0_0 + int(1)) * 5U;

#line 145
    uint _S37 = base_1 + (_S36 + _S30) * 5U;

    uint _S38 = base_1 + (_S36 + _S34) * 5U;

#line 153
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_6->giScene_0->bricks_0[_S31 + _S32], kernelContext_6->giScene_0->bricks_0[_S31 + _S33], tx_0), giLerp_0(kernelContext_6->giScene_0->bricks_0[_S35 + _S32], kernelContext_6->giScene_0->bricks_0[_S35 + _S33], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_6->giScene_0->bricks_0[_S37 + _S32], kernelContext_6->giScene_0->bricks_0[_S37 + _S33], tx_0), giLerp_0(kernelContext_6->giScene_0->bricks_0[_S38 + _S32], kernelContext_6->giScene_0->bricks_0[_S38 + _S33], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 175
float giSampleLevel_0(uint level_0, float3 p_1, float thread* farDistance_0, KernelContext_0 thread* kernelContext_7)
{

#line 176
    uint _S39 = 16U + level_0 * 2U;

#line 176
    float4 shape_0 = (as_type<float4>((kernelContext_7->giScene_0->constants_0[_S39])));
    int4 origin_0 = (as_type<int4>((kernelContext_7->giScene_0->constants_0[_S39 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_1.x / brick_0));
    int by_0 = int(floor(p_1.y / brick_0));
    int bz_0 = int(floor(p_1.z / brick_0));

#line 183
    uint _S40 = giWindowBricks_0(kernelContext_7);
    int dim_0 = int(_S40);
    int _S41 = origin_0.x;

#line 185
    bool _S42;

#line 185
    if(bx_0 < _S41)
    {

#line 185
        _S42 = true;

#line 185
    }
    else
    {

#line 185
        _S42 = by_0 < (origin_0.y);

)cy_msl"
    R"cy_msl(#line 185
    }

#line 185
    if(_S42)
    {

#line 185
        _S42 = true;

#line 185
    }
    else
    {

#line 185
        _S42 = bz_0 < (origin_0.z);

#line 185
    }

#line 185
    if(_S42)
    {

#line 185
        _S42 = true;

#line 185
    }
    else
    {

#line 185
        _S42 = bx_0 >= (_S41 + dim_0);

#line 185
    }

#line 185
    if(_S42)
    {

#line 185
        _S42 = true;

#line 185
    }
    else
    {

#line 185
        _S42 = by_0 >= (origin_0.y + dim_0);

#line 185
    }
    if(_S42)
    {

#line 186
        _S42 = true;

#line 186
    }
    else
    {

#line 186
        _S42 = bz_0 >= (origin_0.z + dim_0);

#line 186
    }

#line 185
    if(_S42)
    {
        return *farDistance_0;
    }
    int _S43 = giWrap_0(bz_0, dim_0);

#line 189
    uint _S44 = uint(dim_0);

#line 189
    uint _S45 = uint(_S43) * _S44;

#line 189
    int _S46 = giWrap_0(by_0, dim_0);

#line 189
    uint _S47 = (_S45 + uint(_S46)) * _S44;
    int _S48 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_7->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S47 + uint(_S48))];
    if(slot_1 == 4294967295U)
    {

#line 193
        return *farDistance_0;
    }

    float3 local_0 = (p_1 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 196
    float _S49 = giTrilinear_0(slot_1, local_0.x, local_0.y, local_0.z, kernelContext_7);
    return _S49;
}


float giDistance_0(float3 p_2, KernelContext_0 thread* kernelContext_8)
{

#line 201
    uint _S50 = giLevelCount_0(kernelContext_8);

#line 201
    float answer_0 = 1.0e+06;

#line 201
    uint level_1 = 0U;


    for(;;)
    {

#line 204
        if(level_1 < _S50)
        {
        }
        else
        {

#line 204
            break;
        }

#line 205
        thread float farDistance_1 = 0.0;

#line 205
        float _S51 = giSampleLevel_0(level_1, p_2, &farDistance_1, kernelContext_8);

        float _S52 = min(answer_0, _S51);
        if(_S51 < farDistance_1)
        {

#line 209
            return _S51;
        }

#line 204
        uint level_2 = level_1 + 1U;

#line 204
        answer_0 = _S52;

#line 204
        level_1 = level_2;

#line 204
    }

#line 212
    return answer_0;
}

struct GiTraceHit_0
{
    bool hit_0;
    bool exhausted_0;
    float t_1;
    float closest_0;
    float3 position_2;
    float3 normal_3;
};

GiTraceHit_0 giSphereTrace_0(float3 origin_1, float3 direction_0, float maxDistance_0, float tMin_0, KernelContext_0 thread* kernelContext_9)
{

#line 226
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_0 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_1 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_2 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_3 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S53 = giLevelCount_0(kernelContext_9);
    if(_S53 == 0U)
    {

#line 234
        return result_0;
    }

#line 234
    float _S54 = giFinestVoxel_0(kernelContext_9);


    float surface_0 = _S54 * 0.5;
    float _S55 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 239
    recent_0[int(0)] = 1.0e+06;

#line 239
    recent_0[int(1)] = 1.0e+06;

#line 239
    recent_0[int(2)] = 1.0e+06;

#line 239
    float closest_1 = 1.0e+06;

#line 239
    uint recentSlot_0 = 0U;

#line 239
    uint step_0 = 0U;

#line 239
    float t_2 = _S55;


    for(;;)
    {

#line 242
        if(step_0 < 192U)
        {
        }
        else
        {

#line 242
            break;
        }

#line 243
        if(t_2 > maxDistance_0)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_3 = origin_1 + direction_0 * float3(t_2) ;

#line 248
        float _S56 = giDistance_0(position_3, kernelContext_9);

        if(_S56 < surface_0)
        {

#line 251
            (&result_0)->hit_0 = true;
            (&result_0)->t_1 = t_2;
            (&result_0)->position_2 = position_3;


            float3 _S57 = float3(_S54, 0.0, 0.0);

#line 256
            float _S58 = giDistance_0(position_3 + _S57, kernelContext_9);

#line 256
            float _S59 = giDistance_0(position_3 - _S57, kernelContext_9);

#line 256
            float _S60 = _S58 - _S59;

            float3 _S61 = float3(0.0, _S54, 0.0);

#line 258
            float _S62 = giDistance_0(position_3 + _S61, kernelContext_9);

#line 258
            float _S63 = giDistance_0(position_3 - _S61, kernelContext_9);

#line 258
            float _S64 = _S62 - _S63;

            float3 _S65 = float3(0.0, 0.0, _S54);

#line 260
            float _S66 = giDistance_0(position_3 + _S65, kernelContext_9);

#line 260
            float _S67 = giDistance_0(position_3 - _S65, kernelContext_9);

            (&result_0)->normal_3 = giNormalisedOr_0(float3(_S60, _S64, _S66 - _S67), - direction_0);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S68 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S56;
        uint _S69 = (recentSlot_0 + 1U) % 3U;
        float t_3 = t_2 + max(_S56, surface_0);

#line 242
        uint step_1 = step_0 + 1U;

)cy_msl"
    R"cy_msl(#line 242
        closest_1 = _S68;

#line 242
        recentSlot_0 = _S69;

#line 242
        step_0 = step_1;

#line 242
        t_2 = t_3;

#line 242
    }

#line 271
    (&result_0)->exhausted_0 = true;

    (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
    return result_0;
}


#line 225
GiTraceHit_0 giSphereTrace_1(float3 origin_2, float3 direction_1, float maxDistance_1, float tMin_1, KernelContext_0 thread* kernelContext_10)
{

#line 226
    thread GiTraceHit_0 result_1;
    (&result_1)->hit_0 = false;
    (&result_1)->exhausted_0 = false;
    (&result_1)->t_1 = 0.0;
    (&result_1)->closest_0 = 0.0;
    (&result_1)->position_2 = float3(0.0, 0.0, 0.0);
    (&result_1)->normal_3 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S70 = giLevelCount_0(kernelContext_10);
    if(_S70 == 0U)
    {

#line 234
        return result_1;
    }

#line 234
    float _S71 = giFinestVoxel_0(kernelContext_10);


    float surface_1 = _S71 * 0.5;
    float _S72 = max(tMin_1, surface_1);
    thread array<float, int(3)> recent_1;

#line 239
    recent_1[int(0)] = 1.0e+06;

#line 239
    recent_1[int(1)] = 1.0e+06;

#line 239
    recent_1[int(2)] = 1.0e+06;

#line 239
    float closest_2 = 1.0e+06;

#line 239
    uint recentSlot_1 = 0U;

#line 239
    uint step_2 = 0U;

#line 239
    float t_4 = _S72;


    for(;;)
    {

#line 242
        if(step_2 < 192U)
        {
        }
        else
        {

#line 242
            break;
        }

#line 243
        if(t_4 > maxDistance_1)
        {
            (&result_1)->closest_0 = min(closest_2, min(recent_1[int(0)], min(recent_1[int(1)], recent_1[int(2)])));
            return result_1;
        }
        float3 position_4 = origin_2 + direction_1 * float3(t_4) ;

#line 248
        float _S73 = giDistance_0(position_4, kernelContext_10);

        if(_S73 < surface_1)
        {

#line 251
            (&result_1)->hit_0 = true;
            (&result_1)->t_1 = t_4;
            (&result_1)->position_2 = position_4;


            float3 _S74 = float3(_S71, 0.0, 0.0);

#line 256
            float _S75 = giDistance_0(position_4 + _S74, kernelContext_10);

#line 256
            float _S76 = giDistance_0(position_4 - _S74, kernelContext_10);

#line 256
            float _S77 = _S75 - _S76;

            float3 _S78 = float3(0.0, _S71, 0.0);

#line 258
            float _S79 = giDistance_0(position_4 + _S78, kernelContext_10);

#line 258
            float _S80 = giDistance_0(position_4 - _S78, kernelContext_10);

#line 258
            float _S81 = _S79 - _S80;

            float3 _S82 = float3(0.0, 0.0, _S71);

#line 260
            float _S83 = giDistance_0(position_4 + _S82, kernelContext_10);

#line 260
            float _S84 = giDistance_0(position_4 - _S82, kernelContext_10);

            (&result_1)->normal_3 = giNormalisedOr_0(float3(_S77, _S81, _S83 - _S84), - direction_1);
            (&result_1)->closest_0 = closest_2;
            return result_1;
        }
        float _S85 = min(closest_2, recent_1[recentSlot_1]);
        recent_1[recentSlot_1] = _S73;
        uint _S86 = (recentSlot_1 + 1U) % 3U;
        float t_5 = t_4 + max(_S73, surface_1);

#line 242
        uint step_3 = step_2 + 1U;

#line 242
        closest_2 = _S85;

#line 242
        recentSlot_1 = _S86;

#line 242
        step_2 = step_3;

#line 242
        t_4 = t_5;

#line 242
    }

#line 271
    (&result_1)->exhausted_0 = true;

    (&result_1)->closest_0 = min(closest_2, min(recent_1[int(0)], min(recent_1[int(1)], recent_1[int(2)])));
    return result_1;
}


#line 317
bool giFieldOccluded_0(float3 from_1, float3 to_1, KernelContext_0 thread* kernelContext_11)
{

#line 318
    float3 offset_1 = to_1 - from_1;
    float span_0 = length(offset_1);
    if(span_0 <= 0.00009999999747379)
    {

#line 321
        return false;
    }

#line 321
    float _S87 = giFinestVoxel_1(kernelContext_11);

    float bias_0 = _S87 * 2.0;
    if(span_0 <= bias_0)
    {

#line 325
        return false;
    }

#line 325
    GiTraceHit_0 _S88 = giSphereTrace_1(from_1, offset_1 / float3(span_0) , span_0 * 0.99900001287460327, bias_0, kernelContext_11);

    return _S88.hit_0;
}


bool giOccluded_0(float3 from_2, float3 to_2, KernelContext_0 thread* kernelContext_12)
{

#line 332
    float3 offset_2 = to_2 - from_2;
    float span_1 = length(offset_2);
    if(span_1 <= 0.00009999999747379)
    {

#line 335
        return false;
    }
    float3 dir_1 = (as_type<float3>((kernelContext_12->giScene_0->constants_0[6U].xyz)));

#line 337
    bool _S89;
    if((kernelContext_12->giScene_0->constants_0[9U].x) != 0U)
    {

#line 338
        _S89 = (dot(offset_2 / float3(span_1) , - dir_1)) > 0.99989998340606689;

#line 338
    }
    else
    {

#line 338
        _S89 = false;

#line 338
    }

#line 338
    if(_S89)
    {

#line 338
        bool _S90 = giShadowMapShadowed_0(from_2, kernelContext_12);
        return _S90;
    }

#line 339
    bool _S91 = giFieldOccluded_0(from_2, to_2, kernelContext_12);

    return _S91;
}


#line 430
float3 giShadedDirect_0(float3 position_5, float3 normal_4, KernelContext_0 thread* kernelContext_13)
{
    uint _S92 = kernelContext_13->giScene_0->constants_0[0U].z;

#line 432
    float3 total_0 = float3(0.0, 0.0, 0.0);

#line 432
    uint light_1 = 0U;
    for(;;)
    {

#line 433
        if(light_1 < _S92)
        {
        }
        else
        {

#line 433
            break;
        }

#line 433
        float3 _S93 = giDirectRadiance_0(light_1, position_5, normal_4, kernelContext_13);

#line 433
        bool _S94;

        if((_S93.x) <= 0.0)
        {

#line 435
            _S94 = (_S93.y) <= 0.0;

#line 435
        }
        else
        {

#line 435
            _S94 = false;

#line 435
        }

)cy_msl"
    R"cy_msl(#line 435
        bool _S95;

#line 435
        if(_S94)
        {

#line 435
            _S95 = (_S93.z) <= 0.0;

#line 435
        }
        else
        {

#line 435
            _S95 = false;

#line 435
        }

#line 435
        if(_S95)
        {

#line 436
            light_1 = light_1 + 1U;

#line 433
            continue;
        }



        uint _S96 = light_1 * 4U;

#line 438
        float4 a_2 = kernelContext_13->giScene_0->lights_0[_S96];
        float4 b_2 = kernelContext_13->giScene_0->lights_0[_S96 + 1U];

        float4 d_1 = kernelContext_13->giScene_0->lights_0[_S96 + 3U];

#line 441
        float3 target_0;


        if((kernelContext_13->giScene_0->lights_0[_S96 + 2U].w) != 0.0)
        {

#line 444
            target_0 = position_5 - giNormalisedOr_0(b_2.xyz, float3(0.0, 1.0, 0.0)) * float3(1000.0) ;

#line 444
        }
        else
        {

#line 445
            if((d_1.w) > 0.0)
            {

#line 445
                target_0 = a_2.xyz + d_1.xyz * float3(0.30000001192092896) ;

#line 445
            }
            else
            {

#line 445
                target_0 = a_2.xyz;

#line 445
            }

#line 444
        }

#line 444
        bool _S97 = giOccluded_0(position_5 + normal_4 * float3(0.00999999977648258) , target_0, kernelContext_13);


        if(_S97)
        {

#line 448
            light_1 = light_1 + 1U;

#line 433
            continue;
        }

#line 433
        total_0 = total_0 + _S93;

#line 433
        light_1 = light_1 + 1U;

#line 433
    }

#line 452
    return total_0;
}


#line 278
float3 giHemisphereDirection_0(float3 normal_5, uint index_1, uint count_3)
{
    float _S98 = float(index_1);
    float radius_0 = sqrt((_S98 + 0.5) / float(count_3));
    float angle_0 = 2.3999631404876709 * _S98;
    float x_0 = radius_0 * cos(angle_0);
    float y_0 = radius_0 * sin(angle_0);
    float z_0 = sqrt(max(0.0, 1.0 - x_0 * x_0 - y_0 * y_0));

#line 285
    float3 tangent_2;
    if((abs(normal_5.y)) < 0.99000000953674316)
    {

#line 286
        tangent_2 = cross(float3(0.0, 1.0, 0.0), normal_5);

#line 286
    }
    else
    {

#line 286
        tangent_2 = cross(float3(1.0, 0.0, 0.0), normal_5);

#line 286
    }

    float3 tangent_3 = giNormalisedOr_0(tangent_2, float3(1.0, 0.0, 0.0));

    return giNormalisedOr_0(tangent_3 * float3(x_0)  + cross(normal_5, tangent_3) * float3(y_0)  + normal_5 * float3(z_0) , normal_5);
}


#line 457
float3 giSky_0(float3 direction_2, KernelContext_0 thread* kernelContext_14)
{

#line 458
    float4 zenith_0 = (as_type<float4>((kernelContext_14->giScene_0->constants_0[2U])));
    float3 horizon_0 = (as_type<float3>((kernelContext_14->giScene_0->constants_0[3U].xyz)));
    float3 ground_0 = (as_type<float3>((kernelContext_14->giScene_0->constants_0[4U].xyz)));

    float _S99 = giNormalisedOr_0(direction_2, float3(0.0, 1.0, 0.0)).y;

#line 462
    if(_S99 < 0.0)
    {

#line 463
        return ground_0 * float3(zenith_0.w) ;
    }

    return (horizon_0 + (zenith_0.xyz - horizon_0) * float3(clamp(pow(_S99, 0.44999998807907104), 0.0, 1.0)) ) * float3(zenith_0.w) ;
}



int giCell_0(float value_1, float cell_0)
{

#line 472
    return int(floor(value_1 / cell_0));
}

uint giBucket_0(int x_1, int y_1, int z_1, uint buckets_0)
{
    return (((uint(x_1) * 73856093U) ^ (uint(y_1) * 19349663U)) ^ (uint(z_1) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_6, float3 normal_6, KernelContext_0 thread* kernelContext_15)
{

#line 482
    uint4 grid_0 = kernelContext_15->giScene_0->constants_0[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 485
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_1 = (as_type<float>((grid_0.z)));
    float _S100 = radius_1 * radius_1;
    float _S101 = position_6.x;

#line 490
    int _S102 = giCell_0(_S101 - radius_1, cell_1);

#line 490
    float _S103 = position_6.y;

#line 490
    int _S104 = giCell_0(_S103 - radius_1, cell_1);
    float _S105 = position_6.z;

#line 491
    int _S106 = giCell_0(_S105 - radius_1, cell_1);
    int _S107 = giCell_0(_S101 + radius_1, cell_1);

#line 492
    int _S108 = giCell_0(_S103 + radius_1, cell_1);
    int _S109 = giCell_0(_S105 + radius_1, cell_1);

#line 493
    float bestScore_0 = 1.0e+09;

#line 493
    float bestAlignment_0 = 0.0;

#line 493
    uint best_0 = 4294967295U;

#line 493
    int z_2 = _S106;



    for(;;)
    {

#line 497
        if(z_2 <= _S109)
        {
        }
        else
        {

#line 497
            break;
        }

#line 497
        int y_2 = _S104;
        for(;;)
        {

#line 498
            if(y_2 <= _S108)
            {
            }
            else
            {

#line 498
                break;
            }

#line 498
            float bestScore_1 = bestScore_0;

#line 498
            float bestAlignment_1 = bestAlignment_0;

#line 498
            uint best_1 = best_0;

#line 498
            int x_2 = _S102;
            for(;;)
            {

#line 499
                if(x_2 <= _S107)
                {
                }
                else
                {

#line 499
                    break;
                }
                uint _S110 = giBucket_0(x_2, y_2, z_2, buckets_1) * 2U;

#line 501
                uint first_0 = kernelContext_15->giScene_0->gridRanges_0[_S110];
                uint _S111 = kernelContext_15->giScene_0->gridRanges_0[_S110 + 1U];

#line 502
                float bestScore_2 = bestScore_1;

#line 502
                float bestAlignment_2 = bestAlignment_1;

#line 502
                uint best_2 = best_1;

#line 502
                uint item_0 = first_0;
                for(;;)
                {

)cy_msl"
    R"cy_msl(#line 503
                    if(item_0 < (first_0 + _S111))
                    {
                    }
                    else
                    {

#line 503
                        break;
                    }

#line 504
                    uint handle_0 = kernelContext_15->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_15->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 506
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }



                    uint _S112 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_15->giScene_0->cards_0[_S112 + 1U].xyz, normal_6);
                    if(alignment_0 <= 0.25)
                    {

#line 511
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 513
                    float3 offset_3 = kernelContext_15->giScene_0->cards_0[_S112].xyz - position_6;
                    float squared_1 = dot(offset_3, offset_3);
                    if(squared_1 > _S100)
                    {

#line 516
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 519
                    float score_0 = squared_1 / alignment_0;

#line 519
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 520
                        better_0 = score_0 < bestScore_2;

#line 520
                    }
                    else
                    {

#line 521
                        if(alignment_0 != bestAlignment_2)
                        {

#line 521
                            better_0 = alignment_0 > bestAlignment_2;

#line 521
                        }
                        else
                        {

#line 521
                            better_0 = handle_0 < best_2;

#line 521
                        }

#line 520
                    }

#line 520
                    float bestScore_3;

#line 520
                    float bestAlignment_3;

#line 520
                    uint best_3;


                    if(better_0)
                    {

#line 523
                        bestScore_3 = score_0;

#line 523
                        bestAlignment_3 = alignment_0;

#line 523
                        best_3 = handle_0;

#line 523
                    }
                    else
                    {

#line 523
                        bestScore_3 = bestScore_2;

#line 523
                        bestAlignment_3 = bestAlignment_2;

#line 523
                        best_3 = best_2;

#line 523
                    }

#line 523
                    bestScore_2 = bestScore_3;

#line 523
                    bestAlignment_2 = bestAlignment_3;

#line 523
                    best_2 = best_3;

#line 503
                    item_0 = item_0 + 1U;

#line 503
                }

#line 499
                int x_3 = x_2 + int(1);

#line 499
                bestScore_1 = bestScore_2;

#line 499
                bestAlignment_1 = bestAlignment_2;

#line 499
                best_1 = best_2;

#line 499
                x_2 = x_3;

#line 499
            }

#line 498
            int y_3 = y_2 + int(1);

#line 498
            bestScore_0 = bestScore_1;

#line 498
            bestAlignment_0 = bestAlignment_1;

#line 498
            best_0 = best_1;

#line 498
            y_2 = y_3;

#line 498
        }

#line 497
        z_2 = z_2 + int(1);

#line 497
    }

#line 532
    return best_0;
}



float3 giGather_0(float3 position_7, float3 normal_7, KernelContext_0 thread* kernelContext_16)
{

#line 538
    uint rays_1 = kernelContext_16->giScene_0->constants_0[0U].w;

#line 538
    bool _S113;
    if(rays_1 == 0U)
    {

#line 539
        _S113 = true;

#line 539
    }
    else
    {

#line 539
        uint _S114 = giLevelCount_0(kernelContext_16);

#line 539
        _S113 = _S114 == 0U;

#line 539
    }

#line 539
    if(_S113)
    {

#line 540
        return float3(0.0, 0.0, 0.0);
    }
    float _S115 = (as_type<float>((kernelContext_16->giScene_0->constants_0[1U].w)));

#line 542
    float _S116 = giFinestVoxel_0(kernelContext_16);

    float _S117 = _S116 * 2.0;
    float3 _S118 = position_7 + normal_7 * float3(_S116) ;

#line 545
    float3 total_1 = float3(0.0, 0.0, 0.0);

#line 545
    uint index_2 = 0U;

    for(;;)
    {

#line 547
        if(index_2 < rays_1)
        {
        }
        else
        {

#line 547
            break;
        }

#line 548
        float3 direction_3 = giHemisphereDirection_0(normal_7, index_2, rays_1);

#line 548
        GiTraceHit_0 _S119 = giSphereTrace_0(_S118, direction_3, _S115, _S117, kernelContext_16);

        if(!_S119.hit_0)
        {

#line 550
            float3 _S120 = giSky_0(direction_3, kernelContext_16);

#line 550
            total_1 = total_1 + _S120;

            index_2 = index_2 + 1U;

#line 547
            continue;
        }

#line 547
        uint _S121 = giFindCard_0(_S119.position_2, _S119.normal_3, kernelContext_16);

#line 547
        float3 total_2;

#line 555
        if(_S121 != 4294967295U)
        {

#line 555
            total_2 = total_1 + (*(kernelContext_16->giScene_0->cardState_0+_S121 * 2U)).xyz;

#line 555
        }
        else
        {

#line 555
            total_2 = total_1;

#line 555
        }

#line 555
        total_1 = total_2;

#line 547
        index_2 = index_2 + 1U;

)cy_msl"
    R"cy_msl(#line 547
    }

#line 559
    return total_1 / float3(float(rays_1)) ;
}


#line 105
float giRelativeChange_0(float3 before_0, float3 after_0)
{

#line 106
    float magnitude_0 = length(before_0) + length(after_0);
    if(magnitude_0 <= 0.00000999999974738)
    {

#line 108
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
    uint index_3 = thread_0.x;
    if(index_3 >= (giDispatch_1->count_2))
    {

#line 35
        return;
    }
    uint handle_1 = (&kernelContext_17)->giScene_0->selection_0[index_3];
    uint _S122 = handle_1 * 4U;

    float4 albedo_0 = (&kernelContext_17)->giScene_0->cards_0[_S122 + 2U];
    float4 emission_0 = (&kernelContext_17)->giScene_0->cards_0[_S122 + 3U];
    uint _S123 = handle_1 * 2U;

#line 42
    float3 before_1 = (*((&kernelContext_17)->giScene_0->cardState_0+_S123)).xyz;

    float3 _S124 = (&kernelContext_17)->giScene_0->cards_0[_S122].xyz;

#line 44
    float3 _S125 = (&kernelContext_17)->giScene_0->cards_0[_S122 + 1U].xyz;

#line 44
    float3 _S126 = giShadedDirect_0(_S124, _S125, &kernelContext_17);
    float3 _S127 = (*((&kernelContext_17)->giScene_0->cardState_0+(_S123 + 1U))).xyz;

#line 45
    float3 accumulated_0;
    if(((&kernelContext_17)->giScene_0->constants_0[0U].w) != 0U)
    {

#line 46
        float3 _S128 = giGather_0(_S124, _S125, &kernelContext_17);

#line 46
        accumulated_0 = albedo_0.xyz * _S128;

#line 46
    }
    else
    {

#line 46
        accumulated_0 = _S127;

#line 46
    }

#line 51
    float3 after_1 = emission_0.xyz + albedo_0.xyz * _S126 + accumulated_0;
    uint _S129 = index_3 * 3U;

#line 52
    *((&kernelContext_17)->giScene_0->results_0+_S129) = float4(_S126, giRelativeChange_0(before_1, after_1));
    *((&kernelContext_17)->giScene_0->results_0+(_S129 + 1U)) = float4(accumulated_0, (as_type<float>((handle_1))));
    *((&kernelContext_17)->giScene_0->results_0+(_S129 + 2U)) = float4(after_1, 1.0);
    return;
}

)cy_msl";

/// cyGiCommitCards.metal, 1865 bytes.
inline constexpr char kGiCommitCardsMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 81 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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

/// cyGiGatherProbes.metal, 37635 bytes.
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


#line 78 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 81
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


#line 99 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 100
    float squared_0 = dot(v_0, v_0);

#line 100
    float3 _S1;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 101
        _S1 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 101
    }
    else
    {

#line 101
        _S1 = fallback_0;

#line 101
    }

#line 101
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


#line 215 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiTraceHit_0
{
    bool hit_1;
    bool exhausted_0;
    float t_1;
    float closest_0;
    float3 position_1;
    float3 normal_1;
};


#line 115
uint giLevelCount_0(KernelContext_0 thread* kernelContext_1)
{

#line 116
    return kernelContext_1->giScene_0->constants_1[0U].x;
}


#line 123
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_2)
{

#line 124
    return (as_type<float>((kernelContext_2->giScene_0->constants_1[9U].y)));
}


#line 119
uint giWindowBricks_0(KernelContext_0 thread* kernelContext_3)
{

#line 120
    return kernelContext_3->giScene_0->constants_1[0U].y;
}


#line 167
int giWrap_0(int value_1, int modulus_0)
{

#line 168
    uint _S4 = uint(modulus_0);
    uint _S5 = (uint(value_1) + (_S4 << 20U)) % _S4;

#line 169
    return int(_S5);
}


#line 95
float giLerp_0(float a_0, float b_0, float t_2)
{

#line 96
    return a_0 + (b_0 - a_0) * t_2;
}


#line 128
float giTrilinear_0(uint slot_0, float fx_0, float fy_0, float fz_0, KernelContext_0 thread* kernelContext_4)
{
    int x0_0 = clamp(int(floor(fx_0)), int(0), int(3));
    int y0_0 = clamp(int(floor(fy_0)), int(0), int(3));
    int z0_0 = clamp(int(floor(fz_0)), int(0), int(3));



    float tx_0 = clamp(fx_0 - float(x0_0), 0.0, 1.0);
    float ty_0 = clamp(fy_0 - float(y0_0), 0.0, 1.0);

    uint base_0 = slot_0 * 125U;

    uint _S6 = uint(z0_0) * 5U;

#line 141
    uint _S7 = uint(y0_0);

#line 141
    uint _S8 = base_0 + (_S6 + _S7) * 5U;

#line 141
    uint _S9 = uint(x0_0);
    uint _S10 = uint(x0_0 + int(1));
    uint _S11 = uint(y0_0 + int(1));

#line 143
    uint _S12 = base_0 + (_S6 + _S11) * 5U;

    uint _S13 = uint(z0_0 + int(1)) * 5U;

#line 145
    uint _S14 = base_0 + (_S13 + _S7) * 5U;

    uint _S15 = base_0 + (_S13 + _S11) * 5U;

#line 153
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_4->giScene_0->bricks_0[_S8 + _S9], kernelContext_4->giScene_0->bricks_0[_S8 + _S10], tx_0), giLerp_0(kernelContext_4->giScene_0->bricks_0[_S12 + _S9], kernelContext_4->giScene_0->bricks_0[_S12 + _S10], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_4->giScene_0->bricks_0[_S14 + _S9], kernelContext_4->giScene_0->bricks_0[_S14 + _S10], tx_0), giLerp_0(kernelContext_4->giScene_0->bricks_0[_S15 + _S9], kernelContext_4->giScene_0->bricks_0[_S15 + _S10], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 175
float giSampleLevel_0(uint level_0, float3 p_0, float thread* farDistance_0, KernelContext_0 thread* kernelContext_5)
{

#line 176
    uint _S16 = 16U + level_0 * 2U;

#line 176
    float4 shape_0 = (as_type<float4>((kernelContext_5->giScene_0->constants_1[_S16])));
    int4 origin_0 = (as_type<int4>((kernelContext_5->giScene_0->constants_1[_S16 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_0.x / brick_0));
    int by_0 = int(floor(p_0.y / brick_0));
    int bz_0 = int(floor(p_0.z / brick_0));

#line 183
    uint _S17 = giWindowBricks_0(kernelContext_5);
    int dim_0 = int(_S17);
    int _S18 = origin_0.x;

#line 185
    bool _S19;

)cy_msl"
    R"cy_msl(#line 185
    if(bx_0 < _S18)
    {

#line 185
        _S19 = true;

#line 185
    }
    else
    {

#line 185
        _S19 = by_0 < (origin_0.y);

#line 185
    }

#line 185
    if(_S19)
    {

#line 185
        _S19 = true;

#line 185
    }
    else
    {

#line 185
        _S19 = bz_0 < (origin_0.z);

#line 185
    }

#line 185
    if(_S19)
    {

#line 185
        _S19 = true;

#line 185
    }
    else
    {

#line 185
        _S19 = bx_0 >= (_S18 + dim_0);

#line 185
    }

#line 185
    if(_S19)
    {

#line 185
        _S19 = true;

#line 185
    }
    else
    {

#line 185
        _S19 = by_0 >= (origin_0.y + dim_0);

#line 185
    }
    if(_S19)
    {

#line 186
        _S19 = true;

#line 186
    }
    else
    {

#line 186
        _S19 = bz_0 >= (origin_0.z + dim_0);

#line 186
    }

#line 185
    if(_S19)
    {
        return *farDistance_0;
    }
    int _S20 = giWrap_0(bz_0, dim_0);

#line 189
    uint _S21 = uint(dim_0);

#line 189
    uint _S22 = uint(_S20) * _S21;

#line 189
    int _S23 = giWrap_0(by_0, dim_0);

#line 189
    uint _S24 = (_S22 + uint(_S23)) * _S21;
    int _S25 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_5->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S24 + uint(_S25))];
    if(slot_1 == 4294967295U)
    {

#line 193
        return *farDistance_0;
    }

    float3 local_0 = (p_0 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 196
    float _S26 = giTrilinear_0(slot_1, local_0.x, local_0.y, local_0.z, kernelContext_5);
    return _S26;
}


float giDistance_0(float3 p_1, KernelContext_0 thread* kernelContext_6)
{

#line 201
    uint _S27 = giLevelCount_0(kernelContext_6);

#line 201
    float answer_0 = 1.0e+06;

#line 201
    uint level_1 = 0U;


    for(;;)
    {

#line 204
        if(level_1 < _S27)
        {
        }
        else
        {

#line 204
            break;
        }

#line 205
        thread float farDistance_1 = 0.0;

#line 205
        float _S28 = giSampleLevel_0(level_1, p_1, &farDistance_1, kernelContext_6);

        float _S29 = min(answer_0, _S28);
        if(_S28 < farDistance_1)
        {

#line 209
            return _S28;
        }

#line 204
        uint level_2 = level_1 + 1U;

#line 204
        answer_0 = _S29;

#line 204
        level_1 = level_2;

#line 204
    }

#line 212
    return answer_0;
}


#line 225
GiTraceHit_0 giSphereTrace_0(float3 origin_1, float3 direction_1, float maxDistance_0, float tMin_0, KernelContext_0 thread* kernelContext_7)
{

#line 226
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_1 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_1 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_1 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_1 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S30 = giLevelCount_0(kernelContext_7);
    if(_S30 == 0U)
    {

#line 234
        return result_0;
    }

#line 234
    float _S31 = giFinestVoxel_0(kernelContext_7);


    float surface_0 = _S31 * 0.5;
    float _S32 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 239
    recent_0[int(0)] = 1.0e+06;

#line 239
    recent_0[int(1)] = 1.0e+06;

#line 239
    recent_0[int(2)] = 1.0e+06;

#line 239
    float closest_1 = 1.0e+06;

#line 239
    uint recentSlot_0 = 0U;

#line 239
    uint step_0 = 0U;

#line 239
    float t_3 = _S32;


    for(;;)
    {

#line 242
        if(step_0 < 192U)
        {
        }
        else
        {

#line 242
            break;
        }

#line 243
        if(t_3 > maxDistance_0)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_2 = origin_1 + direction_1 * float3(t_3) ;

#line 248
        float _S33 = giDistance_0(position_2, kernelContext_7);

        if(_S33 < surface_0)
        {

#line 251
            (&result_0)->hit_1 = true;
            (&result_0)->t_1 = t_3;
            (&result_0)->position_1 = position_2;


            float3 _S34 = float3(_S31, 0.0, 0.0);

#line 256
            float _S35 = giDistance_0(position_2 + _S34, kernelContext_7);

#line 256
            float _S36 = giDistance_0(position_2 - _S34, kernelContext_7);

#line 256
            float _S37 = _S35 - _S36;

            float3 _S38 = float3(0.0, _S31, 0.0);

#line 258
            float _S39 = giDistance_0(position_2 + _S38, kernelContext_7);

#line 258
            float _S40 = giDistance_0(position_2 - _S38, kernelContext_7);

#line 258
            float _S41 = _S39 - _S40;

            float3 _S42 = float3(0.0, 0.0, _S31);

#line 260
            float _S43 = giDistance_0(position_2 + _S42, kernelContext_7);

#line 260
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
    R"cy_msl(#line 242
        uint step_1 = step_0 + 1U;

#line 242
        closest_1 = _S45;

#line 242
        recentSlot_0 = _S46;

#line 242
        step_0 = step_1;

#line 242
        t_3 = t_4;

#line 242
    }

#line 271
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


#line 471 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_2, float cell_0)
{

#line 472
    return int(floor(value_2 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_3, float3 normal_2, KernelContext_0 thread* kernelContext_9)
{

#line 482
    uint4 grid_0 = kernelContext_9->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 485
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S48 = radius_0 * radius_0;
    float _S49 = position_3.x;

#line 490
    int _S50 = giCell_0(_S49 - radius_0, cell_1);

#line 490
    float _S51 = position_3.y;

#line 490
    int _S52 = giCell_0(_S51 - radius_0, cell_1);
    float _S53 = position_3.z;

#line 491
    int _S54 = giCell_0(_S53 - radius_0, cell_1);
    int _S55 = giCell_0(_S49 + radius_0, cell_1);

#line 492
    int _S56 = giCell_0(_S51 + radius_0, cell_1);
    int _S57 = giCell_0(_S53 + radius_0, cell_1);

#line 493
    float bestScore_0 = 1.0e+09;

#line 493
    float bestAlignment_0 = 0.0;

#line 493
    uint best_0 = 4294967295U;

#line 493
    int z_1 = _S54;



    for(;;)
    {

#line 497
        if(z_1 <= _S57)
        {
        }
        else
        {

#line 497
            break;
        }

#line 497
        int y_1 = _S52;
        for(;;)
        {

#line 498
            if(y_1 <= _S56)
            {
            }
            else
            {

#line 498
                break;
            }

#line 498
            float bestScore_1 = bestScore_0;

#line 498
            float bestAlignment_1 = bestAlignment_0;

#line 498
            uint best_1 = best_0;

#line 498
            int x_1 = _S50;
            for(;;)
            {

#line 499
                if(x_1 <= _S55)
                {
                }
                else
                {

#line 499
                    break;
                }
                uint _S58 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 501
                uint first_0 = kernelContext_9->giScene_0->gridRanges_0[_S58];
                uint _S59 = kernelContext_9->giScene_0->gridRanges_0[_S58 + 1U];

#line 502
                float bestScore_2 = bestScore_1;

#line 502
                float bestAlignment_2 = bestAlignment_1;

#line 502
                uint best_2 = best_1;

#line 502
                uint item_0 = first_0;
                for(;;)
                {

#line 503
                    if(item_0 < (first_0 + _S59))
                    {
                    }
                    else
                    {

#line 503
                        break;
                    }

#line 504
                    uint handle_0 = kernelContext_9->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_9->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 506
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }



                    uint _S60 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_9->giScene_0->cards_0[_S60 + 1U].xyz, normal_2);
                    if(alignment_0 <= 0.25)
                    {

#line 511
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 513
                    float3 offset_0 = kernelContext_9->giScene_0->cards_0[_S60].xyz - position_3;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S48)
                    {

#line 516
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 519
                    float score_0 = squared_1 / alignment_0;

#line 519
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 520
                        better_0 = score_0 < bestScore_2;

#line 520
                    }
                    else
                    {

#line 521
                        if(alignment_0 != bestAlignment_2)
                        {

#line 521
                            better_0 = alignment_0 > bestAlignment_2;

#line 521
                        }
                        else
                        {

#line 521
                            better_0 = handle_0 < best_2;

#line 521
                        }

#line 520
                    }

#line 520
                    float bestScore_3;

#line 520
                    float bestAlignment_3;

#line 520
                    uint best_3;


)cy_msl"
    R"cy_msl(                    if(better_0)
                    {

#line 523
                        bestScore_3 = score_0;

#line 523
                        bestAlignment_3 = alignment_0;

#line 523
                        best_3 = handle_0;

#line 523
                    }
                    else
                    {

#line 523
                        bestScore_3 = bestScore_2;

#line 523
                        bestAlignment_3 = bestAlignment_2;

#line 523
                        best_3 = best_2;

#line 523
                    }

#line 523
                    bestScore_2 = bestScore_3;

#line 523
                    bestAlignment_2 = bestAlignment_3;

#line 523
                    best_2 = best_3;

#line 503
                    item_0 = item_0 + 1U;

#line 503
                }

#line 499
                int x_2 = x_1 + int(1);

#line 499
                bestScore_1 = bestScore_2;

#line 499
                bestAlignment_1 = bestAlignment_2;

#line 499
                best_1 = best_2;

#line 499
                x_1 = x_2;

#line 499
            }

#line 498
            int y_2 = y_1 + int(1);

#line 498
            bestScore_0 = bestScore_1;

#line 498
            bestAlignment_0 = bestAlignment_1;

#line 498
            best_0 = best_1;

#line 498
            y_1 = y_2;

#line 498
        }

#line 497
        z_1 = z_1 + int(1);

#line 497
    }

#line 532
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

/// cyGiSampleProbes.metal, 18006 bytes.
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


#line 167 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giWrap_0(int value_1, int modulus_0)
{

#line 168
    uint _S1 = uint(modulus_0);
    uint _S2 = (uint(value_1) + (_S1 << 20U)) % _S1;

#line 169
    return int(_S2);
}


#line 81
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


#line 487
uint gpLayoutProbe_0(uint level_0, int3 cell_0, KernelContext_0 thread* kernelContext_0)
{

#line 488
    int3 centre_0 = (as_type<int3>((kernelContext_0->giProbes_0->constants_0[6U + level_0].xyz)));
    int half_0 = int(kernelContext_0->giProbes_0->constants_0[1U].w);
    int3 _S3 = int3(half_0, half_0, half_0);

#line 490
    bool _S4;

#line 490
    if(any(cell_0 < (centre_0 - _S3)))
    {

#line 490
        _S4 = true;

#line 490
    }
    else
    {

#line 490
        _S4 = any(cell_0 > (centre_0 + _S3));

#line 490
    }

#line 490
    if(_S4)
    {

#line 491
        return 4294967295U;
    }
    int edge_0 = int(kernelContext_0->giProbes_0->constants_0[5U].z);
    uint cells_0 = uint(edge_0 * edge_0 * edge_0);
    int _S5 = giWrap_0(cell_0.z, edge_0);

#line 495
    uint _S6 = uint(edge_0);

#line 495
    uint _S7 = uint(_S5) * _S6;

#line 495
    int _S8 = giWrap_0(cell_0.y, edge_0);

#line 495
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
    float _S19 = max(slack_0, 0.00009999999747379);

#line 472
    float visible_0 = gpClamp_0((_S18 - span_0) / _S19, 0.0, 1.0);

#line 472
    axis_0 = 0U;

    for(;;)
    {

#line 474
        if(axis_0 < 6U)
        {
        }
        else
        {

#line 474
            break;
        }

#line 475
        float cosine_0 = dot(kGpAxes_0[axis_0], _S12);
        if(cosine_0 < 0.5)
        {

#line 477
            axis_0 = axis_0 + 1U;

#line 474
            continue;
        }

#line 474
        visible_0 = min(visible_0, gpClamp_0((_S16[axis_0] + slack_0 - span_0 * cosine_0) / _S19, 0.0, 1.0));

#line 474
        axis_0 = axis_0 + 1U;

#line 474
    }

#line 483
    return visible_0;
}


#line 273
void gpLoadPayload_0(uint probe_1, uint floats_0, array<float, int(48)> thread* payload_0, KernelContext_0 thread* kernelContext_2)
{

)cy_msl"
    R"cy_msl(#line 273
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
        float _S20;
        if(index_0 < floats_0)
        {

#line 275
            _S20 = *(kernelContext_2->giProbes_0->payloads_0+(probe_1 * floats_0 + index_0));

#line 275
        }
        else
        {

#line 275
            _S20 = 0.0;

#line 275
        }

#line 275
        (*payload_0)[index_0] = _S20;

#line 274
        index_0 = index_0 + 1U;

#line 274
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
    float _S21 = p_0.x;
    float _S22 = p_0.y;

#line 158
    float px_0;

#line 158
    float py_0;
    if((p_0.z) < 0.0)
    {

#line 160
        float _S23 = 1.0 - abs(_S22);

#line 160
        if(_S21 >= 0.0)
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
        float fx_0 = _S23 * px_0;
        float _S24 = 1.0 - abs(_S21);

#line 161
        if(_S22 >= 0.0)
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
        float fy_0 = _S24 * px_0;

#line 161
        px_0 = fx_0;

#line 161
        py_0 = fy_0;

#line 159
    }
    else
    {

#line 159
        px_0 = _S21;

#line 159
        py_0 = _S22;

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



        array<float, int(4)> _S25 = { 0.28209498524665833, 0.32573533058166504 * direction_1.y, 0.32573533058166504 * direction_1.z, 0.32573533058166504 * direction_1.x };

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
            uint _S26 = c_0 * 3U;

#line 230
            result_0.x = result_0.x + (*payload_1)[_S26] * _S25[c_0];
            result_0.y = result_0.y + (*payload_1)[_S26 + 1U] * _S25[c_0];
            result_0.z = result_0.z + (*payload_1)[_S26 + 2U] * _S25[c_0];

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
        uint _S27 = min(x0_0 + 1U, 3U);



        uint _S28 = y0_0 * 4U;

        uint _S29 = min(y0_0 + 1U, 3U) * 4U;

        uint _S30 = (_S28 + x0_0) * 3U;

#line 250
        float3 f00_0 = float3((*payload_1)[_S30], (*payload_1)[_S30 + 1U], (*payload_1)[_S30 + 2U]);
        uint _S31 = (_S28 + _S27) * 3U;
        uint _S32 = (_S29 + x0_0) * 3U;

#line 252
        float3 f01_0 = float3((*payload_1)[_S32], (*payload_1)[_S32 + 1U], (*payload_1)[_S32 + 2U]);
        uint _S33 = (_S29 + _S27) * 3U;

#line 253
        float3 _S34 = float3((u_1 - float(x0_0))) ;
        float3 top_0 = f00_0 + (float3((*payload_1)[_S31], (*payload_1)[_S31 + 1U], (*payload_1)[_S31 + 2U]) - f00_0) * _S34;

        return top_0 + (f01_0 + (float3((*payload_1)[_S33], (*payload_1)[_S33 + 1U], (*payload_1)[_S33 + 2U]) - f01_0) * _S34 - top_0) * float3((v_2 - float(y0_0))) ;
    }
    float3 _S35 = float3(0.0, 0.0, 0.0);

#line 258
    float3 result_1 = _S35;

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
        float _S36 = max(0.0, dot(kGpAxes_0[c_0], direction_1));
        if(_S36 <= 0.0)
        {

#line 263
            c_0 = c_0 + 1U;

#line 260
            continue;
        }



        uint _S37 = c_0 * 3U;

        float total_1 = total_0 + _S36;

#line 267
        result_1 = result_1 + float3((*payload_1)[_S37], (*payload_1)[_S37 + 1U], (*payload_1)[_S37 + 2U]) * float3(_S36) ;

#line 267
        total_0 = total_1;

#line 260
        c_0 = c_0 + 1U;

)cy_msl"
    R"cy_msl(#line 260
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
        result_1 = _S35;

#line 269
    }

#line 269
    return result_1;
}


#line 506
float4 gpSample_0(float3 position_1, float3 normal_0, uint frame_1, KernelContext_0 thread* kernelContext_3)
{

#line 507
    uint4 counts_0 = kernelContext_3->giProbes_0->constants_0[0U];
    uint _S38 = counts_0.x;
    uint _S39 = counts_0.y;
    uint _S40 = kernelContext_3->giProbes_0->constants_0[1U].z;
    float _S41 = (as_type<float>((kernelContext_3->giProbes_0->constants_0[5U].x)));

#line 511
    float3 total_2 = float3(0.0, 0.0, 0.0);

#line 511
    float oldest_0 = 0.0;

#line 511
    uint level_1 = 0U;

#line 511
    float totalWeight_0 = 0.0;



    for(;;)
    {

#line 515
        if(level_1 < _S40)
        {
        }
        else
        {

#line 515
            break;
        }

#line 516
        float spacing_0 = _S41 * float(1U << level_1);
        float radius_0 = spacing_0 * 1.5;
        float _S42 = position_1.x;

#line 518
        int _S43 = gpFloorDiv_0(_S42 - radius_0, spacing_0);
        float _S44 = position_1.y;

#line 519
        int _S45 = gpFloorDiv_0(_S44 - radius_0, spacing_0);
        float _S46 = position_1.z;
        int _S47 = gpFloorDiv_0(_S42 + radius_0, spacing_0);
        int _S48 = gpFloorDiv_0(_S44 + radius_0, spacing_0);
        int _S49 = gpFloorDiv_0(_S46 + radius_0, spacing_0);

#line 523
        int z_0 = gpFloorDiv_0(_S46 - radius_0, spacing_0);
        for(;;)
        {

#line 524
            if(z_0 <= _S49)
            {
            }
            else
            {

#line 524
                break;
            }

#line 524
            float3 total_3 = total_2;

#line 524
            float oldest_1 = oldest_0;

#line 524
            int y_0 = _S45;

#line 524
            float totalWeight_1 = totalWeight_0;
            for(;;)
            {

#line 525
                if(y_0 <= _S48)
                {
                }
                else
                {

#line 525
                    break;
                }

#line 525
                float3 total_4 = total_3;

#line 525
                float oldest_2 = oldest_1;

#line 525
                int x_0 = _S43;

#line 525
                float totalWeight_2 = totalWeight_1;
                for(;;)
                {

#line 526
                    if(x_0 <= _S47)
                    {
                    }
                    else
                    {

#line 526
                        break;
                    }

#line 526
                    uint _S50 = gpLayoutProbe_0(level_1, int3(x_0, y_0, z_0), kernelContext_3);

                    if(_S50 == 4294967295U)
                    {

#line 529
                        x_0 = x_0 + int(1);

#line 526
                        continue;
                    }



                    uint _S51 = _S50 * 4U;

#line 531
                    float4 device* _S52 = kernelContext_3->giProbes_0->probes_0+(_S51 + 1U);

#line 531
                    float4 record_0 = *_S52;

#line 531
                    bool _S53;
                    if(((*_S52).y) == 0.0)
                    {

#line 532
                        _S53 = true;

#line 532
                    }
                    else
                    {

#line 532
                        _S53 = (record_0.z) == 0.0;

#line 532
                    }

#line 532
                    if(_S53)
                    {

#line 533
                        x_0 = x_0 + int(1);

#line 526
                        continue;
                    }

#line 535
                    float3 at_0 = (*(kernelContext_3->giProbes_0->probes_0+_S51)).xyz;
                    float3 offset_1 = position_1 - at_0;
                    float span_1 = length(offset_1);
                    if(span_1 > radius_0)
                    {

#line 539
                        x_0 = x_0 + int(1);

#line 526
                        continue;
                    }

#line 541
                    float weight_2 = 1.0 - gpClamp_0(span_1 / radius_0, 0.0, 1.0);

#line 541
                    float weight_3;
                    if(span_1 > 0.00009999999747379)
                    {

#line 542
                        weight_3 = weight_2 * gpClamp_0((dot(normal_0, - offset_1 / float3(span_1) ) + 1.0) * 0.5, 0.0, 1.0);

#line 542
                    }
                    else
                    {

#line 542
                        weight_3 = weight_2;

#line 542
                    }

#line 542
                    float _S54 = gpVisibilityWeight_0(_S50, at_0, position_1, kernelContext_3);



                    float weight_4 = weight_3 * _S54;
                    if(weight_4 <= 0.0)
                    {

#line 548
                        x_0 = x_0 + int(1);

#line 526
                        continue;
                    }

#line 550
                    thread array<float, int(48)> data_0;

#line 550
                    gpLoadPayload_0(_S50, _S39, &data_0, kernelContext_3);

#line 550
                    thread array<float, int(48)> _S55 = data_0;

#line 550
                    float3 _S56 = gpDecode_0(_S38, &_S55, normal_0);

                    float3 _S57 = total_4 + _S56 * float3(weight_4) ;
                    float totalWeight_3 = totalWeight_2 + weight_4;
                    uint last_0 = (as_type<uint>((record_0.x)));

#line 554
                    float age_0;
                    if(frame_1 > last_0)
                    {

#line 555
                        age_0 = float(frame_1 - last_0);

#line 555
                    }
                    else
                    {

#line 555
                        age_0 = 0.0;

#line 555
                    }
                    float _S58 = max(oldest_2, age_0);

#line 556
                    total_4 = _S57;

#line 556
                    oldest_2 = _S58;

#line 556
                    totalWeight_2 = totalWeight_3;

#line 526
                    x_0 = x_0 + int(1);

#line 526
                }

)cy_msl"
    R"cy_msl(#line 525
                int y_1 = y_0 + int(1);

#line 525
                total_3 = total_4;

#line 525
                oldest_1 = oldest_2;

#line 525
                y_0 = y_1;

#line 525
                totalWeight_1 = totalWeight_2;

#line 525
            }

#line 524
            int z_1 = z_0 + int(1);

#line 524
            total_2 = total_3;

#line 524
            oldest_0 = oldest_1;

#line 524
            z_0 = z_1;

#line 524
            totalWeight_0 = totalWeight_1;

#line 524
        }

#line 515
        level_1 = level_1 + 1U;

#line 515
    }

#line 561
    if(totalWeight_0 <= 0.0)
    {

#line 562
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
    uint _S59 = query_1 * 2U;

    float4 device* _S60 = (&kernelContext_4)->giProbes_0->samples_0+query_1;

#line 31
    float4 _S61 = gpSample_0((&kernelContext_4)->giProbes_0->queries_0[_S59].xyz, (&kernelContext_4)->giProbes_0->queries_0[_S59 + 1U].xyz, (&kernelContext_4)->giDispatch_0->frame_0, &kernelContext_4);

#line 31
    *_S60 = _S61;
    return;
}

)cy_msl";

/// cyGiBuildScreenHzb.metal, 2995 bytes.
inline constexpr char kGiBuildScreenHzbMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 81 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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

#line 689 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiTierSample_0
{
    float3 radiance_0;
    float confidence_0;
    float hitDistance_0;
    uint source_0;
};


#line 696
GiTierSample_0 gpTierSample_0(uint source_1)
{

#line 697
    thread GiTierSample_0 sample_0;
    (&sample_0)->radiance_0 = float3(0.0, 0.0, 0.0);
    (&sample_0)->confidence_0 = 0.0;
    (&sample_0)->hitDistance_0 = -1.0;
    (&sample_0)->source_0 = source_1;
    return sample_0;
}


#line 81 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 588
float3 gpTransformPoint_0(uint first_0, float3 p_0, KernelContext_0 thread* kernelContext_0)
{



    return ((as_type<float4>((kernelContext_0->giProbes_0->constants_0[first_0]))) * float4(p_0.x)  + (as_type<float4>((kernelContext_0->giProbes_0->constants_0[first_0 + 1U]))) * float4(p_0.y)  + (as_type<float4>((kernelContext_0->giProbes_0->constants_0[first_0 + 2U]))) * float4(p_0.z)  + (as_type<float4>((kernelContext_0->giProbes_0->constants_0[first_0 + 3U])))).xyz;
}

float3 gpProjectPoint_0(uint first_1, float3 p_1, KernelContext_0 thread* kernelContext_1)
{



    float4 clip_0 = (as_type<float4>((kernelContext_1->giProbes_0->constants_0[first_1]))) * float4(p_1.x)  + (as_type<float4>((kernelContext_1->giProbes_0->constants_0[first_1 + 1U]))) * float4(p_1.y)  + (as_type<float4>((kernelContext_1->giProbes_0->constants_0[first_1 + 2U]))) * float4(p_1.z)  + (as_type<float4>((kernelContext_1->giProbes_0->constants_0[first_1 + 3U])));
    float _S1 = clip_0.w;

#line 602
    if(_S1 == 0.0)
    {

#line 603
        return clip_0.xyz;
    }
    return clip_0.xyz * float3((1.0 / _S1)) ;
}


#line 617
struct GiScreenStep_0
{
    bool outside_0;
    float x_0;
    float y_0;
    float depth_0;
};

GiScreenStep_0 gpScreenStep_0(float3 point_0, uint width_0, uint height_0, KernelContext_0 thread* kernelContext_2)
{

#line 625
    float3 _S2 = gpTransformPoint_0(16U, point_0, kernelContext_2);
    thread GiScreenStep_0 step_0;

    float _S3 = - _S2.z;

#line 628
    (&step_0)->depth_0 = _S3;

#line 628
    float3 _S4 = gpProjectPoint_0(20U, point_0, kernelContext_2);

    float _S5 = float(width_0);

#line 630
    (&step_0)->x_0 = (_S4.x * 0.5 + 0.5) * _S5;
    float _S6 = float(height_0);

#line 631
    (&step_0)->y_0 = (0.5 - _S4.y * 0.5) * _S6;

#line 631
    bool _S7;
    if(_S3 <= 0.0)
    {

#line 632
        _S7 = true;

#line 632
    }
    else
    {

#line 632
        _S7 = ((&step_0)->x_0) < 0.0;

#line 632
    }

#line 632
    if(_S7)
    {

#line 632
        _S7 = true;

#line 632
    }
    else
    {

#line 632
        _S7 = ((&step_0)->y_0) < 0.0;

#line 632
    }

#line 632
    if(_S7)
    {

#line 632
        _S7 = true;

#line 632
    }
    else
    {

#line 632
        _S7 = ((&step_0)->x_0) >= _S5;

#line 632
    }
    if(_S7)
    {

#line 633
        _S7 = true;

#line 633
    }
    else
    {

#line 633
        _S7 = ((&step_0)->y_0) >= _S6;

#line 633
    }

#line 632
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


#line 609
float gpEdgeConfidence_0(float x_1, float y_1, uint width_1, uint height_1)
{
    float u_0 = x_1 / float(width_1);
    float v_0 = y_1 / float(height_1);

    return gpClamp_0(min(min(u_0, 1.0 - u_0), min(v_0, 1.0 - v_0)) / 0.10000000149011612, 0.0, 1.0);
}


#line 639
float gpNearestDepth_0(int x0_0, int y0_0, int x1_0, int y1_0, uint thread* reads_0, KernelContext_0 thread* kernelContext_3)
{

#line 640
    uint _S9 = kernelContext_3->giProbes_0->constants_0[15U].x;
    int _S10 = max(x1_0 - x0_0, y1_0 - y0_0);

#line 641
    uint level_0 = 0U;

    for(;;)
    {

#line 643
        uint _S11 = level_0 + 1U;

#line 643
        bool _S12;

#line 643
        if(_S11 < _S9)
        {

#line 643
            _S12 = (int(1) << uint(int(level_0))) <= _S10;

#line 643
        }
        else
        {

#line 643
            _S12 = false;

#line 643
        }

#line 643
        if(_S12)
        {
        }
        else
        {

)cy_msl"
    R"cy_msl(#line 643
            break;
        }

#line 643
        level_0 = _S11;

#line 643
    }


    uint4 shape_0 = kernelContext_3->giProbes_0->constants_0[26U + level_0];

#line 646
    uint _S13 = uint(int(level_0));
    int _S14 = x0_0 >> _S13;
    int ly0_0 = y0_0 >> _S13;
    uint _S15 = shape_0.y;

#line 649
    int _S16 = min(x1_0 >> _S13, int(_S15) - int(1));
    int _S17 = min(y1_0 >> _S13, int(shape_0.z) - int(1));

#line 650
    float nearest_0 = 1.00000001504746622e+30;

#line 650
    int y_2 = ly0_0;

    for(;;)
    {

#line 652
        if(y_2 <= _S17)
        {
        }
        else
        {

#line 652
            break;
        }

#line 652
        int x_2 = _S14;
        for(;;)
        {

#line 653
            if(x_2 <= _S16)
            {
            }
            else
            {

#line 653
                break;
            }

#line 654
            float _S18 = min(nearest_0, *(kernelContext_3->giProbes_0->screenHzb_0+(shape_0.x + uint(y_2) * _S15 + uint(x_2))));
            *reads_0 = *reads_0 + 1U;

#line 653
            int x_3 = x_2 + int(1);

#line 653
            nearest_0 = _S18;

#line 653
            x_2 = x_3;

#line 653
        }

#line 652
        y_2 = y_2 + int(1);

#line 652
    }

#line 658
    return nearest_0;
}




bool gpScreenRunEmpty_0(float3 origin_0, float3 direction_0, float step_1, uint index_0, uint run_0, const GiScreenStep_0 thread* here_0, uint width_2, uint height_2, uint thread* reads_1, KernelContext_0 thread* kernelContext_4)
{

#line 665
    GiScreenStep_0 _S19 = gpScreenStep_0(origin_0 + direction_0 * float3((step_1 * float(index_0 + run_0))) , width_2, height_2, kernelContext_4);

#line 665
    bool inside_0;

#line 670
    if(!_S19.outside_0)
    {

#line 670
        inside_0 = (_S19.x_0) >= 1.0;

#line 670
    }
    else
    {

#line 670
        inside_0 = false;

#line 670
    }

#line 670
    if(inside_0)
    {

#line 670
        inside_0 = (_S19.y_0) >= 1.0;

#line 670
    }
    else
    {

#line 670
        inside_0 = false;

#line 670
    }

#line 670
    if(inside_0)
    {

#line 670
        inside_0 = (_S19.x_0) < (float(width_2) - 1.0);

#line 670
    }
    else
    {

#line 670
        inside_0 = false;

#line 670
    }
    if(inside_0)
    {

#line 671
        inside_0 = (_S19.y_0) < (float(height_2) - 1.0);

#line 671
    }
    else
    {

#line 671
        inside_0 = false;

#line 671
    }

#line 671
    if(inside_0)
    {

#line 671
        inside_0 = (here_0->x_0) >= 1.0;

#line 671
    }
    else
    {

#line 671
        inside_0 = false;

#line 671
    }
    if(inside_0)
    {

#line 672
        inside_0 = (here_0->y_0) >= 1.0;

#line 672
    }
    else
    {

#line 672
        inside_0 = false;

#line 672
    }

#line 672
    if(inside_0)
    {

#line 672
        inside_0 = (here_0->x_0) < (float(width_2) - 1.0);

#line 672
    }
    else
    {

#line 672
        inside_0 = false;

#line 672
    }

#line 672
    if(inside_0)
    {

#line 672
        inside_0 = (here_0->y_0) < (float(height_2) - 1.0);

#line 672
    }
    else
    {

#line 672
        inside_0 = false;

#line 672
    }

    if(!inside_0)
    {

#line 675
        return false;
    }

#line 675
    float _S20 = gpNearestDepth_0(max(int(floor(min(here_0->x_0, _S19.x_0))) - int(1), int(0)), max(int(floor(min(here_0->y_0, _S19.y_0))) - int(1), int(0)), min(int(floor(max(here_0->x_0, _S19.x_0))) + int(1), int(width_2) - int(1)), min(int(floor(max(here_0->y_0, _S19.y_0))) + int(1), int(height_2) - int(1)), reads_1, kernelContext_4);

#line 686
    return (max(here_0->depth_0, _S19.depth_0)) < (_S20 - abs(_S20) * 0.00009999999747379 - 0.00009999999747379);
}


#line 706
GiTierSample_0 gpScreenTrace_0(float3 origin_1, float3 direction_1, float maxDistance_0, bool accelerated_0, uint thread* reads_2, KernelContext_0 thread* kernelContext_5)
{
    thread GiTierSample_0 sample_1 = gpTierSample_0(5U);
    uint4 screen_0 = kernelContext_5->giProbes_0->constants_0[14U];
    uint width_3 = screen_0.x;
    uint height_3 = screen_0.y;

#line 711
    bool _S21;
    if(width_3 == 0U)
    {

#line 712
        _S21 = true;

#line 712
    }
    else
    {

#line 712
        _S21 = height_3 == 0U;

#line 712
    }

#line 712
    if(_S21)
    {

#line 713
        return sample_1;
    }
    float _S22 = (as_type<float>((screen_0.w)));
    uint _S23 = max(1U, screen_0.z);
    float _S24 = maxDistance_0 / float(_S23);

#line 717
    uint run_1 = 8U;

#line 717
    uint index_1 = 1U;

#line 722
    for(;;)
    {

#line 722
        if(index_1 <= _S23)
        {
        }
        else
        {

)cy_msl"
    R"cy_msl(#line 722
            break;
        }

#line 723
        float t_0 = _S24 * float(index_1);

#line 723
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

#line 737
        float gap_0;

#line 737
        if(_S26)
        {

#line 737
            gap_0 = bufferDepth_0 - _S25.depth_0;

#line 737
        }
        else
        {

#line 737
            gap_0 = 1.00000001504746622e+30;

#line 737
        }
        if(_S26)
        {

#line 739
            float behind_0 = _S25.depth_0 - bufferDepth_0;
            if(behind_0 > 0.0)
            {

#line 741
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

#line 738
        }

#line 755
        if(accelerated_0)
        {

#line 755
            _S21 = (index_1 + 1U) < _S23;

#line 755
        }
        else
        {

#line 755
            _S21 = false;

#line 755
        }

#line 755
        bool _S27;

#line 755
        if(_S21)
        {

#line 755
            _S27 = gap_0 > (_S22 * 2.0);

#line 755
        }
        else
        {

#line 755
            _S27 = false;

#line 755
        }

#line 755
        uint skip_0;

#line 755
        if(_S27)
        {

#line 756
            uint _S28 = min(run_1, _S23 - index_1);

#line 756
            thread GiScreenStep_0 _S29 = _S25;

#line 756
            bool _S30 = gpScreenRunEmpty_0(origin_1, direction_1, _S24, index_1, _S28, &_S29, width_3, height_3, reads_2, kernelContext_5);

#line 756
            uint run_2;
            if(_S30)
            {

                uint _S31 = min(run_1 * 2U, 32U);

#line 760
                skip_0 = _S28;

#line 760
                run_2 = _S31;

#line 757
            }
            else
            {


                uint _S32 = max(run_1 / 2U, 2U);

#line 762
                skip_0 = 0U;

#line 762
                run_2 = _S32;

#line 757
            }

#line 757
            run_1 = run_2;

#line 755
        }
        else
        {

#line 755
            skip_0 = 0U;

#line 755
        }

#line 755
        index_1 = index_1 + (1U + skip_0);

#line 722
    }

#line 768
    (&sample_1)->confidence_0 = 0.20000000298023224;
    (&sample_1)->hitDistance_0 = -1.0;
    return sample_1;
}


#line 814
void gpCount_0(uint counter_0, uint amount_0, KernelContext_0 thread* kernelContext_6)
{

#line 815
    uint total_0 = simd_sum(amount_0);
    bool _S33 = simd_is_first();

#line 816
    bool _S34;

#line 816
    if(_S33)
    {

#line 816
        _S34 = total_0 != 0U;

#line 816
    }
    else
    {

#line 816
        _S34 = false;

#line 816
    }

#line 816
    if(_S34)
    {

#line 817
        uint _S35 = atomic_fetch_add_explicit(((atomic_uint device*)(kernelContext_6->giProbes_0->counters_0+counter_0)), total_0, memory_order_relaxed);

#line 816
    }


    return;
}


#line 883
void gpWriteTierResult_0(uint ray_0, const GiTierSample_0 thread* sample_2, uint reads_3, KernelContext_0 thread* kernelContext_7)
{

#line 884
    uint _S36 = ray_0 * 2U;

#line 884
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

/// cyGiTraceSoftware.metal, 23404 bytes.
inline constexpr char kGiTraceSoftwareMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 689 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiTierSample_0
{
    float3 radiance_0;
    float confidence_0;
    float hitDistance_0;
    uint source_0;
};


#line 81 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 78 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 78
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


#line 215 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiTraceHit_0
{
    bool hit_1;
    bool exhausted_0;
    float t_1;
    float closest_0;
    float3 position_1;
    float3 normal_1;
};


#line 115
uint giLevelCount_0(KernelContext_0 thread* kernelContext_0)
{

#line 116
    return kernelContext_0->giScene_0->constants_1[0U].x;
}


#line 123
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_1)
{

#line 124
    return (as_type<float>((kernelContext_1->giScene_0->constants_1[9U].y)));
}


#line 119
uint giWindowBricks_0(KernelContext_0 thread* kernelContext_2)
{

#line 120
    return kernelContext_2->giScene_0->constants_1[0U].y;
}


#line 167
int giWrap_0(int value_0, int modulus_0)
{

#line 168
    uint _S1 = uint(modulus_0);
    uint _S2 = (uint(value_0) + (_S1 << 20U)) % _S1;

#line 169
    return int(_S2);
}


#line 95
float giLerp_0(float a_0, float b_0, float t_2)
{

#line 96
    return a_0 + (b_0 - a_0) * t_2;
}


#line 128
float giTrilinear_0(uint slot_0, float fx_0, float fy_0, float fz_0, KernelContext_0 thread* kernelContext_3)
{
    int x0_0 = clamp(int(floor(fx_0)), int(0), int(3));
    int y0_0 = clamp(int(floor(fy_0)), int(0), int(3));
    int z0_0 = clamp(int(floor(fz_0)), int(0), int(3));



    float tx_0 = clamp(fx_0 - float(x0_0), 0.0, 1.0);
    float ty_0 = clamp(fy_0 - float(y0_0), 0.0, 1.0);

    uint base_0 = slot_0 * 125U;

    uint _S3 = uint(z0_0) * 5U;

#line 141
    uint _S4 = uint(y0_0);

#line 141
    uint _S5 = base_0 + (_S3 + _S4) * 5U;

#line 141
    uint _S6 = uint(x0_0);
    uint _S7 = uint(x0_0 + int(1));
    uint _S8 = uint(y0_0 + int(1));

#line 143
    uint _S9 = base_0 + (_S3 + _S8) * 5U;

    uint _S10 = uint(z0_0 + int(1)) * 5U;

#line 145
    uint _S11 = base_0 + (_S10 + _S4) * 5U;

    uint _S12 = base_0 + (_S10 + _S8) * 5U;

#line 153
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S5 + _S6], kernelContext_3->giScene_0->bricks_0[_S5 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S9 + _S6], kernelContext_3->giScene_0->bricks_0[_S9 + _S7], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S11 + _S6], kernelContext_3->giScene_0->bricks_0[_S11 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S12 + _S6], kernelContext_3->giScene_0->bricks_0[_S12 + _S7], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 175
float giSampleLevel_0(uint level_0, float3 p_0, float thread* farDistance_0, KernelContext_0 thread* kernelContext_4)
{

#line 176
    uint _S13 = 16U + level_0 * 2U;

#line 176
    float4 shape_0 = (as_type<float4>((kernelContext_4->giScene_0->constants_1[_S13])));
    int4 origin_0 = (as_type<int4>((kernelContext_4->giScene_0->constants_1[_S13 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_0.x / brick_0));
    int by_0 = int(floor(p_0.y / brick_0));
    int bz_0 = int(floor(p_0.z / brick_0));

#line 183
    uint _S14 = giWindowBricks_0(kernelContext_4);
    int dim_0 = int(_S14);
    int _S15 = origin_0.x;

#line 185
    bool _S16;

#line 185
    if(bx_0 < _S15)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = by_0 < (origin_0.y);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = bz_0 < (origin_0.z);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = bx_0 >= (_S15 + dim_0);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = by_0 >= (origin_0.y + dim_0);

#line 185
    }
    if(_S16)
    {

#line 186
        _S16 = true;

#line 186
    }
    else
    {

#line 186
        _S16 = bz_0 >= (origin_0.z + dim_0);

#line 186
    }

)cy_msl"
    R"cy_msl(#line 185
    if(_S16)
    {
        return *farDistance_0;
    }
    int _S17 = giWrap_0(bz_0, dim_0);

#line 189
    uint _S18 = uint(dim_0);

#line 189
    uint _S19 = uint(_S17) * _S18;

#line 189
    int _S20 = giWrap_0(by_0, dim_0);

#line 189
    uint _S21 = (_S19 + uint(_S20)) * _S18;
    int _S22 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_4->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S21 + uint(_S22))];
    if(slot_1 == 4294967295U)
    {

#line 193
        return *farDistance_0;
    }

    float3 local_0 = (p_0 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 196
    float _S23 = giTrilinear_0(slot_1, local_0.x, local_0.y, local_0.z, kernelContext_4);
    return _S23;
}


float giDistance_0(float3 p_1, KernelContext_0 thread* kernelContext_5)
{

#line 201
    uint _S24 = giLevelCount_0(kernelContext_5);

#line 201
    float answer_0 = 1.0e+06;

#line 201
    uint level_1 = 0U;


    for(;;)
    {

#line 204
        if(level_1 < _S24)
        {
        }
        else
        {

#line 204
            break;
        }

#line 205
        thread float farDistance_1 = 0.0;

#line 205
        float _S25 = giSampleLevel_0(level_1, p_1, &farDistance_1, kernelContext_5);

        float _S26 = min(answer_0, _S25);
        if(_S25 < farDistance_1)
        {

#line 209
            return _S25;
        }

#line 204
        uint level_2 = level_1 + 1U;

#line 204
        answer_0 = _S26;

#line 204
        level_1 = level_2;

#line 204
    }

#line 212
    return answer_0;
}


#line 99
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 100
    float squared_0 = dot(v_0, v_0);

#line 100
    float3 _S27;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 101
        _S27 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 101
    }
    else
    {

#line 101
        _S27 = fallback_0;

#line 101
    }

#line 101
    return _S27;
}


#line 225
GiTraceHit_0 giSphereTrace_0(float3 origin_1, float3 direction_0, float maxDistance_0, float tMin_0, KernelContext_0 thread* kernelContext_6)
{

#line 226
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_1 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_1 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_1 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_1 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S28 = giLevelCount_0(kernelContext_6);
    if(_S28 == 0U)
    {

#line 234
        return result_0;
    }

#line 234
    float _S29 = giFinestVoxel_0(kernelContext_6);


    float surface_0 = _S29 * 0.5;
    float _S30 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 239
    recent_0[int(0)] = 1.0e+06;

#line 239
    recent_0[int(1)] = 1.0e+06;

#line 239
    recent_0[int(2)] = 1.0e+06;

#line 239
    float closest_1 = 1.0e+06;

#line 239
    uint recentSlot_0 = 0U;

#line 239
    uint step_0 = 0U;

#line 239
    float t_3 = _S30;


    for(;;)
    {

#line 242
        if(step_0 < 192U)
        {
        }
        else
        {

#line 242
            break;
        }

#line 243
        if(t_3 > maxDistance_0)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_2 = origin_1 + direction_0 * float3(t_3) ;

#line 248
        float _S31 = giDistance_0(position_2, kernelContext_6);

        if(_S31 < surface_0)
        {

#line 251
            (&result_0)->hit_1 = true;
            (&result_0)->t_1 = t_3;
            (&result_0)->position_1 = position_2;


            float3 _S32 = float3(_S29, 0.0, 0.0);

#line 256
            float _S33 = giDistance_0(position_2 + _S32, kernelContext_6);

#line 256
            float _S34 = giDistance_0(position_2 - _S32, kernelContext_6);

#line 256
            float _S35 = _S33 - _S34;

            float3 _S36 = float3(0.0, _S29, 0.0);

#line 258
            float _S37 = giDistance_0(position_2 + _S36, kernelContext_6);

#line 258
            float _S38 = giDistance_0(position_2 - _S36, kernelContext_6);

#line 258
            float _S39 = _S37 - _S38;

            float3 _S40 = float3(0.0, 0.0, _S29);

#line 260
            float _S41 = giDistance_0(position_2 + _S40, kernelContext_6);

#line 260
            float _S42 = giDistance_0(position_2 - _S40, kernelContext_6);

            (&result_0)->normal_1 = giNormalisedOr_0(float3(_S35, _S39, _S41 - _S42), - direction_0);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S43 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S31;
        uint _S44 = (recentSlot_0 + 1U) % 3U;
        float t_4 = t_3 + max(_S31, surface_0);

#line 242
        uint step_1 = step_0 + 1U;

#line 242
        closest_1 = _S43;

#line 242
        recentSlot_0 = _S44;

#line 242
        step_0 = step_1;

#line 242
        t_3 = t_4;

#line 242
    }

#line 271
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


#line 696
GiTierSample_0 gpTierSample_0(uint source_1)
{

#line 697
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


#line 471 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_2, float cell_0)
{

#line 472
    return int(floor(value_2 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_3, float3 normal_2, KernelContext_0 thread* kernelContext_9)
{

#line 482
    uint4 grid_0 = kernelContext_9->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 485
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S48 = radius_0 * radius_0;
    float _S49 = position_3.x;

#line 490
    int _S50 = giCell_0(_S49 - radius_0, cell_1);

#line 490
    float _S51 = position_3.y;

#line 490
    int _S52 = giCell_0(_S51 - radius_0, cell_1);
    float _S53 = position_3.z;

#line 491
    int _S54 = giCell_0(_S53 - radius_0, cell_1);
    int _S55 = giCell_0(_S49 + radius_0, cell_1);

#line 492
    int _S56 = giCell_0(_S51 + radius_0, cell_1);
    int _S57 = giCell_0(_S53 + radius_0, cell_1);

#line 493
    float bestScore_0 = 1.0e+09;

#line 493
    float bestAlignment_0 = 0.0;

#line 493
    uint best_0 = 4294967295U;

#line 493
    int z_1 = _S54;



    for(;;)
    {

#line 497
        if(z_1 <= _S57)
        {
        }
        else
        {

#line 497
            break;
        }

#line 497
        int y_1 = _S52;
        for(;;)
        {

#line 498
            if(y_1 <= _S56)
            {
            }
            else
            {

#line 498
                break;
            }

#line 498
            float bestScore_1 = bestScore_0;

#line 498
            float bestAlignment_1 = bestAlignment_0;

#line 498
            uint best_1 = best_0;

#line 498
            int x_1 = _S50;
            for(;;)
            {

#line 499
                if(x_1 <= _S55)
                {
                }
                else
                {

#line 499
                    break;
                }
                uint _S58 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 501
                uint first_0 = kernelContext_9->giScene_0->gridRanges_0[_S58];
                uint _S59 = kernelContext_9->giScene_0->gridRanges_0[_S58 + 1U];

#line 502
                float bestScore_2 = bestScore_1;

#line 502
                float bestAlignment_2 = bestAlignment_1;

#line 502
                uint best_2 = best_1;

#line 502
                uint item_0 = first_0;
                for(;;)
                {

#line 503
                    if(item_0 < (first_0 + _S59))
                    {
                    }
                    else
                    {

#line 503
                        break;
                    }

#line 504
                    uint handle_0 = kernelContext_9->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_9->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 506
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }



                    uint _S60 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_9->giScene_0->cards_0[_S60 + 1U].xyz, normal_2);
                    if(alignment_0 <= 0.25)
                    {

#line 511
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 513
                    float3 offset_0 = kernelContext_9->giScene_0->cards_0[_S60].xyz - position_3;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S48)
                    {

#line 516
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 519
                    float score_0 = squared_1 / alignment_0;

#line 519
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 520
                        better_0 = score_0 < bestScore_2;

)cy_msl"
    R"cy_msl(#line 520
                    }
                    else
                    {

#line 521
                        if(alignment_0 != bestAlignment_2)
                        {

#line 521
                            better_0 = alignment_0 > bestAlignment_2;

#line 521
                        }
                        else
                        {

#line 521
                            better_0 = handle_0 < best_2;

#line 521
                        }

#line 520
                    }

#line 520
                    float bestScore_3;

#line 520
                    float bestAlignment_3;

#line 520
                    uint best_3;


                    if(better_0)
                    {

#line 523
                        bestScore_3 = score_0;

#line 523
                        bestAlignment_3 = alignment_0;

#line 523
                        best_3 = handle_0;

#line 523
                    }
                    else
                    {

#line 523
                        bestScore_3 = bestScore_2;

#line 523
                        bestAlignment_3 = bestAlignment_2;

#line 523
                        best_3 = best_2;

#line 523
                    }

#line 523
                    bestScore_2 = bestScore_3;

#line 523
                    bestAlignment_2 = bestAlignment_3;

#line 523
                    best_2 = best_3;

#line 503
                    item_0 = item_0 + 1U;

#line 503
                }

#line 499
                int x_2 = x_1 + int(1);

#line 499
                bestScore_1 = bestScore_2;

#line 499
                bestAlignment_1 = bestAlignment_2;

#line 499
                best_1 = best_2;

#line 499
                x_1 = x_2;

#line 499
            }

#line 498
            int y_2 = y_1 + int(1);

#line 498
            bestScore_0 = bestScore_1;

#line 498
            bestAlignment_0 = bestAlignment_1;

#line 498
            best_0 = best_1;

#line 498
            y_1 = y_2;

#line 498
        }

#line 497
        z_1 = z_1 + int(1);

#line 497
    }

#line 532
    return best_0;
}


#line 776 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpWorldRadiance_0(const GiWorldHit_0 thread* hit_3, float3 direction_3, uint source_2, uint cacheFrame_0, KernelContext_0 thread* kernelContext_10)
{

#line 777
    thread GiTierSample_0 sample_1 = gpTierSample_0(source_2);
    if(!hit_3->hit_0)
    {

#line 779
        (&sample_1)->source_0 = 9U;

#line 779
        float3 _S61 = gpSky_0(direction_3, kernelContext_10);
        (&sample_1)->radiance_0 = _S61;
        (&sample_1)->confidence_0 = 1.0;
        (&sample_1)->hitDistance_0 = -1.0;
        return sample_1;
    }
    (&sample_1)->hitDistance_0 = hit_3->t_0;

#line 785
    uint _S62 = giFindCard_0(hit_3->position_0, hit_3->normal_0, kernelContext_10);

    if(_S62 != 4294967295U)
    {

#line 788
        uint _S63 = _S62 * 2U;

#line 788
        (&sample_1)->radiance_0 = (*(kernelContext_10->giScene_0->cardState_0+_S63)).xyz;
        uint last_0 = (as_type<uint>(((*(kernelContext_10->giScene_0->cardState_0+(_S63 + 1U))).w)));

#line 789
        float age_0;
        if(cacheFrame_0 > last_0)
        {

#line 790
            age_0 = float(cacheFrame_0 - last_0);

#line 790
        }
        else
        {

#line 790
            age_0 = 0.0;

#line 790
        }


        (&sample_1)->confidence_0 = hit_3->confidence_1 * (1.0 - gpClamp_0(age_0 / 240.0, 0.0, 1.0) * 0.60000002384185791);
        return sample_1;
    }
    (&sample_1)->confidence_0 = hit_3->confidence_1 * 0.20000000298023224;
    return sample_1;
}


GiTierSample_0 gpWorldTrace_0(float3 origin_3, float3 unit_0, float reach_0, KernelContext_0 thread* kernelContext_11)
{

#line 802
    uint cacheFrame_1 = kernelContext_11->giProbes_0->constants_0[24U].w;

#line 802
    GiWorldHit_0 _S64 = GiSoftwareTier_trace_0(origin_3, unit_0, reach_0, kernelContext_11);

    uint _S65 = GiSoftwareTier_source_0();

#line 804
    thread GiWorldHit_0 _S66 = _S64;

#line 804
    GiTierSample_0 _S67 = gpWorldRadiance_0(&_S66, unit_0, _S65, cacheFrame_1, kernelContext_11);

#line 804
    return _S67;
}


#line 883
void gpWriteTierResult_0(uint ray_0, const GiTierSample_0 thread* sample_2, uint reads_0, KernelContext_0 thread* kernelContext_12)
{

#line 884
    uint _S68 = ray_0 * 2U;

#line 884
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

/// cyGiTraceTiered.metal, 39253 bytes.
inline constexpr char kGiTraceTieredMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 689 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiTierSample_0
{
    float3 radiance_0;
    float confidence_0;
    float hitDistance_0;
    uint source_0;
};


#line 81 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 78 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 78
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


#line 215 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiTraceHit_0
{
    bool hit_1;
    bool exhausted_0;
    float t_1;
    float closest_0;
    float3 position_1;
    float3 normal_1;
};


#line 115
uint giLevelCount_0(KernelContext_0 thread* kernelContext_0)
{

#line 116
    return kernelContext_0->giScene_0->constants_1[0U].x;
}


#line 123
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_1)
{

#line 124
    return (as_type<float>((kernelContext_1->giScene_0->constants_1[9U].y)));
}


#line 119
uint giWindowBricks_0(KernelContext_0 thread* kernelContext_2)
{

#line 120
    return kernelContext_2->giScene_0->constants_1[0U].y;
}


#line 167
int giWrap_0(int value_0, int modulus_0)
{

#line 168
    uint _S1 = uint(modulus_0);
    uint _S2 = (uint(value_0) + (_S1 << 20U)) % _S1;

#line 169
    return int(_S2);
}


#line 95
float giLerp_0(float a_0, float b_0, float t_2)
{

#line 96
    return a_0 + (b_0 - a_0) * t_2;
}


#line 128
float giTrilinear_0(uint slot_0, float fx_0, float fy_0, float fz_0, KernelContext_0 thread* kernelContext_3)
{
    int x0_0 = clamp(int(floor(fx_0)), int(0), int(3));
    int y0_0 = clamp(int(floor(fy_0)), int(0), int(3));
    int z0_0 = clamp(int(floor(fz_0)), int(0), int(3));



    float tx_0 = clamp(fx_0 - float(x0_0), 0.0, 1.0);
    float ty_0 = clamp(fy_0 - float(y0_0), 0.0, 1.0);

    uint base_0 = slot_0 * 125U;

    uint _S3 = uint(z0_0) * 5U;

#line 141
    uint _S4 = uint(y0_0);

#line 141
    uint _S5 = base_0 + (_S3 + _S4) * 5U;

#line 141
    uint _S6 = uint(x0_0);
    uint _S7 = uint(x0_0 + int(1));
    uint _S8 = uint(y0_0 + int(1));

#line 143
    uint _S9 = base_0 + (_S3 + _S8) * 5U;

    uint _S10 = uint(z0_0 + int(1)) * 5U;

#line 145
    uint _S11 = base_0 + (_S10 + _S4) * 5U;

    uint _S12 = base_0 + (_S10 + _S8) * 5U;

#line 153
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S5 + _S6], kernelContext_3->giScene_0->bricks_0[_S5 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S9 + _S6], kernelContext_3->giScene_0->bricks_0[_S9 + _S7], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S11 + _S6], kernelContext_3->giScene_0->bricks_0[_S11 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S12 + _S6], kernelContext_3->giScene_0->bricks_0[_S12 + _S7], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 175
float giSampleLevel_0(uint level_0, float3 p_0, float thread* farDistance_0, KernelContext_0 thread* kernelContext_4)
{

#line 176
    uint _S13 = 16U + level_0 * 2U;

#line 176
    float4 shape_0 = (as_type<float4>((kernelContext_4->giScene_0->constants_1[_S13])));
    int4 origin_0 = (as_type<int4>((kernelContext_4->giScene_0->constants_1[_S13 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_0.x / brick_0));
    int by_0 = int(floor(p_0.y / brick_0));
    int bz_0 = int(floor(p_0.z / brick_0));

#line 183
    uint _S14 = giWindowBricks_0(kernelContext_4);
    int dim_0 = int(_S14);
    int _S15 = origin_0.x;

#line 185
    bool _S16;

#line 185
    if(bx_0 < _S15)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = by_0 < (origin_0.y);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = bz_0 < (origin_0.z);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = bx_0 >= (_S15 + dim_0);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = by_0 >= (origin_0.y + dim_0);

#line 185
    }
    if(_S16)
    {

#line 186
        _S16 = true;

#line 186
    }
    else
    {

#line 186
        _S16 = bz_0 >= (origin_0.z + dim_0);

#line 186
    }

)cy_msl"
    R"cy_msl(#line 185
    if(_S16)
    {
        return *farDistance_0;
    }
    int _S17 = giWrap_0(bz_0, dim_0);

#line 189
    uint _S18 = uint(dim_0);

#line 189
    uint _S19 = uint(_S17) * _S18;

#line 189
    int _S20 = giWrap_0(by_0, dim_0);

#line 189
    uint _S21 = (_S19 + uint(_S20)) * _S18;
    int _S22 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_4->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S21 + uint(_S22))];
    if(slot_1 == 4294967295U)
    {

#line 193
        return *farDistance_0;
    }

    float3 local_0 = (p_0 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 196
    float _S23 = giTrilinear_0(slot_1, local_0.x, local_0.y, local_0.z, kernelContext_4);
    return _S23;
}


float giDistance_0(float3 p_1, KernelContext_0 thread* kernelContext_5)
{

#line 201
    uint _S24 = giLevelCount_0(kernelContext_5);

#line 201
    float answer_0 = 1.0e+06;

#line 201
    uint level_1 = 0U;


    for(;;)
    {

#line 204
        if(level_1 < _S24)
        {
        }
        else
        {

#line 204
            break;
        }

#line 205
        thread float farDistance_1 = 0.0;

#line 205
        float _S25 = giSampleLevel_0(level_1, p_1, &farDistance_1, kernelContext_5);

        float _S26 = min(answer_0, _S25);
        if(_S25 < farDistance_1)
        {

#line 209
            return _S25;
        }

#line 204
        uint level_2 = level_1 + 1U;

#line 204
        answer_0 = _S26;

#line 204
        level_1 = level_2;

#line 204
    }

#line 212
    return answer_0;
}


#line 99
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 100
    float squared_0 = dot(v_0, v_0);

#line 100
    float3 _S27;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 101
        _S27 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 101
    }
    else
    {

#line 101
        _S27 = fallback_0;

#line 101
    }

#line 101
    return _S27;
}


#line 225
GiTraceHit_0 giSphereTrace_0(float3 origin_1, float3 direction_0, float maxDistance_0, float tMin_0, KernelContext_0 thread* kernelContext_6)
{

#line 226
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_1 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_1 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_1 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_1 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S28 = giLevelCount_0(kernelContext_6);
    if(_S28 == 0U)
    {

#line 234
        return result_0;
    }

#line 234
    float _S29 = giFinestVoxel_0(kernelContext_6);


    float surface_0 = _S29 * 0.5;
    float _S30 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 239
    recent_0[int(0)] = 1.0e+06;

#line 239
    recent_0[int(1)] = 1.0e+06;

#line 239
    recent_0[int(2)] = 1.0e+06;

#line 239
    float closest_1 = 1.0e+06;

#line 239
    uint recentSlot_0 = 0U;

#line 239
    uint step_0 = 0U;

#line 239
    float t_3 = _S30;


    for(;;)
    {

#line 242
        if(step_0 < 192U)
        {
        }
        else
        {

#line 242
            break;
        }

#line 243
        if(t_3 > maxDistance_0)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_2 = origin_1 + direction_0 * float3(t_3) ;

#line 248
        float _S31 = giDistance_0(position_2, kernelContext_6);

        if(_S31 < surface_0)
        {

#line 251
            (&result_0)->hit_1 = true;
            (&result_0)->t_1 = t_3;
            (&result_0)->position_1 = position_2;


            float3 _S32 = float3(_S29, 0.0, 0.0);

#line 256
            float _S33 = giDistance_0(position_2 + _S32, kernelContext_6);

#line 256
            float _S34 = giDistance_0(position_2 - _S32, kernelContext_6);

#line 256
            float _S35 = _S33 - _S34;

            float3 _S36 = float3(0.0, _S29, 0.0);

#line 258
            float _S37 = giDistance_0(position_2 + _S36, kernelContext_6);

#line 258
            float _S38 = giDistance_0(position_2 - _S36, kernelContext_6);

#line 258
            float _S39 = _S37 - _S38;

            float3 _S40 = float3(0.0, 0.0, _S29);

#line 260
            float _S41 = giDistance_0(position_2 + _S40, kernelContext_6);

#line 260
            float _S42 = giDistance_0(position_2 - _S40, kernelContext_6);

            (&result_0)->normal_1 = giNormalisedOr_0(float3(_S35, _S39, _S41 - _S42), - direction_0);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S43 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S31;
        uint _S44 = (recentSlot_0 + 1U) % 3U;
        float t_4 = t_3 + max(_S31, surface_0);

#line 242
        uint step_1 = step_0 + 1U;

#line 242
        closest_1 = _S43;

#line 242
        recentSlot_0 = _S44;

#line 242
        step_0 = step_1;

#line 242
        t_3 = t_4;

#line 242
    }

#line 271
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


#line 696
GiTierSample_0 gpTierSample_0(uint source_1)
{

#line 697
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


#line 471 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_2, float cell_0)
{

#line 472
    return int(floor(value_2 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_3, float3 normal_2, KernelContext_0 thread* kernelContext_9)
{

#line 482
    uint4 grid_0 = kernelContext_9->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 485
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S48 = radius_0 * radius_0;
    float _S49 = position_3.x;

#line 490
    int _S50 = giCell_0(_S49 - radius_0, cell_1);

#line 490
    float _S51 = position_3.y;

#line 490
    int _S52 = giCell_0(_S51 - radius_0, cell_1);
    float _S53 = position_3.z;

#line 491
    int _S54 = giCell_0(_S53 - radius_0, cell_1);
    int _S55 = giCell_0(_S49 + radius_0, cell_1);

#line 492
    int _S56 = giCell_0(_S51 + radius_0, cell_1);
    int _S57 = giCell_0(_S53 + radius_0, cell_1);

#line 493
    float bestScore_0 = 1.0e+09;

#line 493
    float bestAlignment_0 = 0.0;

#line 493
    uint best_0 = 4294967295U;

#line 493
    int z_1 = _S54;



    for(;;)
    {

#line 497
        if(z_1 <= _S57)
        {
        }
        else
        {

#line 497
            break;
        }

#line 497
        int y_1 = _S52;
        for(;;)
        {

#line 498
            if(y_1 <= _S56)
            {
            }
            else
            {

#line 498
                break;
            }

#line 498
            float bestScore_1 = bestScore_0;

#line 498
            float bestAlignment_1 = bestAlignment_0;

#line 498
            uint best_1 = best_0;

#line 498
            int x_1 = _S50;
            for(;;)
            {

#line 499
                if(x_1 <= _S55)
                {
                }
                else
                {

#line 499
                    break;
                }
                uint _S58 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 501
                uint first_0 = kernelContext_9->giScene_0->gridRanges_0[_S58];
                uint _S59 = kernelContext_9->giScene_0->gridRanges_0[_S58 + 1U];

#line 502
                float bestScore_2 = bestScore_1;

#line 502
                float bestAlignment_2 = bestAlignment_1;

#line 502
                uint best_2 = best_1;

#line 502
                uint item_0 = first_0;
                for(;;)
                {

#line 503
                    if(item_0 < (first_0 + _S59))
                    {
                    }
                    else
                    {

#line 503
                        break;
                    }

#line 504
                    uint handle_0 = kernelContext_9->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_9->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 506
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }



                    uint _S60 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_9->giScene_0->cards_0[_S60 + 1U].xyz, normal_2);
                    if(alignment_0 <= 0.25)
                    {

#line 511
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 513
                    float3 offset_0 = kernelContext_9->giScene_0->cards_0[_S60].xyz - position_3;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S48)
                    {

#line 516
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 519
                    float score_0 = squared_1 / alignment_0;

#line 519
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 520
                        better_0 = score_0 < bestScore_2;

)cy_msl"
    R"cy_msl(#line 520
                    }
                    else
                    {

#line 521
                        if(alignment_0 != bestAlignment_2)
                        {

#line 521
                            better_0 = alignment_0 > bestAlignment_2;

#line 521
                        }
                        else
                        {

#line 521
                            better_0 = handle_0 < best_2;

#line 521
                        }

#line 520
                    }

#line 520
                    float bestScore_3;

#line 520
                    float bestAlignment_3;

#line 520
                    uint best_3;


                    if(better_0)
                    {

#line 523
                        bestScore_3 = score_0;

#line 523
                        bestAlignment_3 = alignment_0;

#line 523
                        best_3 = handle_0;

#line 523
                    }
                    else
                    {

#line 523
                        bestScore_3 = bestScore_2;

#line 523
                        bestAlignment_3 = bestAlignment_2;

#line 523
                        best_3 = best_2;

#line 523
                    }

#line 523
                    bestScore_2 = bestScore_3;

#line 523
                    bestAlignment_2 = bestAlignment_3;

#line 523
                    best_2 = best_3;

#line 503
                    item_0 = item_0 + 1U;

#line 503
                }

#line 499
                int x_2 = x_1 + int(1);

#line 499
                bestScore_1 = bestScore_2;

#line 499
                bestAlignment_1 = bestAlignment_2;

#line 499
                best_1 = best_2;

#line 499
                x_1 = x_2;

#line 499
            }

#line 498
            int y_2 = y_1 + int(1);

#line 498
            bestScore_0 = bestScore_1;

#line 498
            bestAlignment_0 = bestAlignment_1;

#line 498
            best_0 = best_1;

#line 498
            y_1 = y_2;

#line 498
        }

#line 497
        z_1 = z_1 + int(1);

#line 497
    }

#line 532
    return best_0;
}


#line 776 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpWorldRadiance_0(const GiWorldHit_0 thread* hit_3, float3 direction_3, uint source_2, uint cacheFrame_0, KernelContext_0 thread* kernelContext_10)
{

#line 777
    thread GiTierSample_0 sample_1 = gpTierSample_0(source_2);
    if(!hit_3->hit_0)
    {

#line 779
        (&sample_1)->source_0 = 9U;

#line 779
        float3 _S61 = gpSky_0(direction_3, kernelContext_10);
        (&sample_1)->radiance_0 = _S61;
        (&sample_1)->confidence_0 = 1.0;
        (&sample_1)->hitDistance_0 = -1.0;
        return sample_1;
    }
    (&sample_1)->hitDistance_0 = hit_3->t_0;

#line 785
    uint _S62 = giFindCard_0(hit_3->position_0, hit_3->normal_0, kernelContext_10);

    if(_S62 != 4294967295U)
    {

#line 788
        uint _S63 = _S62 * 2U;

#line 788
        (&sample_1)->radiance_0 = (*(kernelContext_10->giScene_0->cardState_0+_S63)).xyz;
        uint last_0 = (as_type<uint>(((*(kernelContext_10->giScene_0->cardState_0+(_S63 + 1U))).w)));

#line 789
        float age_0;
        if(cacheFrame_0 > last_0)
        {

#line 790
            age_0 = float(cacheFrame_0 - last_0);

#line 790
        }
        else
        {

#line 790
            age_0 = 0.0;

#line 790
        }


        (&sample_1)->confidence_0 = hit_3->confidence_1 * (1.0 - gpClamp_0(age_0 / 240.0, 0.0, 1.0) * 0.60000002384185791);
        return sample_1;
    }
    (&sample_1)->confidence_0 = hit_3->confidence_1 * 0.20000000298023224;
    return sample_1;
}


GiTierSample_0 gpWorldTrace_0(float3 origin_3, float3 unit_0, float reach_0, KernelContext_0 thread* kernelContext_11)
{

#line 802
    uint cacheFrame_1 = kernelContext_11->giProbes_0->constants_0[24U].w;

#line 802
    GiWorldHit_0 _S64 = GiSoftwareTier_trace_0(origin_3, unit_0, reach_0, kernelContext_11);

    uint _S65 = GiSoftwareTier_source_0();

#line 804
    thread GiWorldHit_0 _S66 = _S64;

#line 804
    GiTierSample_0 _S67 = gpWorldRadiance_0(&_S66, unit_0, _S65, cacheFrame_1, kernelContext_11);

#line 804
    return _S67;
}


#line 814
void gpCount_0(uint counter_0, uint amount_0, KernelContext_0 thread* kernelContext_12)
{

#line 815
    uint total_0 = simd_sum(amount_0);
    bool _S68 = simd_is_first();

#line 816
    bool _S69;

#line 816
    if(_S68)
    {

#line 816
        _S69 = total_0 != 0U;

#line 816
    }
    else
    {

#line 816
        _S69 = false;

#line 816
    }

#line 816
    if(_S69)
    {

#line 817
        uint _S70 = atomic_fetch_add_explicit(((atomic_uint device*)(kernelContext_12->giProbes_0->counters_0+counter_0)), total_0, memory_order_relaxed);

#line 816
    }


    return;
}


#line 617
struct GiScreenStep_0
{
    bool outside_0;
    float x_3;
    float y_3;
    float depth_0;
};


#line 588
float3 gpTransformPoint_0(uint first_1, float3 p_2, KernelContext_0 thread* kernelContext_13)
{



    return ((as_type<float4>((kernelContext_13->giProbes_0->constants_0[first_1]))) * float4(p_2.x)  + (as_type<float4>((kernelContext_13->giProbes_0->constants_0[first_1 + 1U]))) * float4(p_2.y)  + (as_type<float4>((kernelContext_13->giProbes_0->constants_0[first_1 + 2U]))) * float4(p_2.z)  + (as_type<float4>((kernelContext_13->giProbes_0->constants_0[first_1 + 3U])))).xyz;
}

float3 gpProjectPoint_0(uint first_2, float3 p_3, KernelContext_0 thread* kernelContext_14)
{



    float4 clip_0 = (as_type<float4>((kernelContext_14->giProbes_0->constants_0[first_2]))) * float4(p_3.x)  + (as_type<float4>((kernelContext_14->giProbes_0->constants_0[first_2 + 1U]))) * float4(p_3.y)  + (as_type<float4>((kernelContext_14->giProbes_0->constants_0[first_2 + 2U]))) * float4(p_3.z)  + (as_type<float4>((kernelContext_14->giProbes_0->constants_0[first_2 + 3U])));
    float _S71 = clip_0.w;

#line 602
    if(_S71 == 0.0)
    {

#line 603
        return clip_0.xyz;
    }
    return clip_0.xyz * float3((1.0 / _S71)) ;
}


#line 625
GiScreenStep_0 gpScreenStep_0(float3 point_0, uint width_0, uint height_0, KernelContext_0 thread* kernelContext_15)
{

#line 625
    float3 _S72 = gpTransformPoint_0(16U, point_0, kernelContext_15);
    thread GiScreenStep_0 step_2;

)cy_msl"
    R"cy_msl(    float _S73 = - _S72.z;

#line 628
    (&step_2)->depth_0 = _S73;

#line 628
    float3 _S74 = gpProjectPoint_0(20U, point_0, kernelContext_15);

    float _S75 = float(width_0);

#line 630
    (&step_2)->x_3 = (_S74.x * 0.5 + 0.5) * _S75;
    float _S76 = float(height_0);

#line 631
    (&step_2)->y_3 = (0.5 - _S74.y * 0.5) * _S76;

#line 631
    bool _S77;
    if(_S73 <= 0.0)
    {

#line 632
        _S77 = true;

#line 632
    }
    else
    {

#line 632
        _S77 = ((&step_2)->x_3) < 0.0;

#line 632
    }

#line 632
    if(_S77)
    {

#line 632
        _S77 = true;

#line 632
    }
    else
    {

#line 632
        _S77 = ((&step_2)->y_3) < 0.0;

#line 632
    }

#line 632
    if(_S77)
    {

#line 632
        _S77 = true;

#line 632
    }
    else
    {

#line 632
        _S77 = ((&step_2)->x_3) >= _S75;

#line 632
    }
    if(_S77)
    {

#line 633
        _S77 = true;

#line 633
    }
    else
    {

#line 633
        _S77 = ((&step_2)->y_3) >= _S76;

#line 633
    }

#line 632
    (&step_2)->outside_0 = _S77;

    return step_2;
}


#line 609
float gpEdgeConfidence_0(float x_4, float y_4, uint width_1, uint height_1)
{
    float u_0 = x_4 / float(width_1);
    float v_1 = y_4 / float(height_1);

    return gpClamp_0(min(min(u_0, 1.0 - u_0), min(v_1, 1.0 - v_1)) / 0.10000000149011612, 0.0, 1.0);
}


#line 639
float gpNearestDepth_0(int x0_1, int y0_1, int x1_0, int y1_0, uint thread* reads_0, KernelContext_0 thread* kernelContext_16)
{

#line 640
    uint _S78 = kernelContext_16->giProbes_0->constants_0[15U].x;
    int _S79 = max(x1_0 - x0_1, y1_0 - y0_1);

#line 641
    uint level_3 = 0U;

    for(;;)
    {

#line 643
        uint _S80 = level_3 + 1U;

#line 643
        bool _S81;

#line 643
        if(_S80 < _S78)
        {

#line 643
            _S81 = (int(1) << uint(int(level_3))) <= _S79;

#line 643
        }
        else
        {

#line 643
            _S81 = false;

#line 643
        }

#line 643
        if(_S81)
        {
        }
        else
        {

#line 643
            break;
        }

#line 643
        level_3 = _S80;

#line 643
    }


    uint4 shape_1 = kernelContext_16->giProbes_0->constants_0[26U + level_3];

#line 646
    uint _S82 = uint(int(level_3));
    int _S83 = x0_1 >> _S82;
    int ly0_0 = y0_1 >> _S82;
    uint _S84 = shape_1.y;

#line 649
    int _S85 = min(x1_0 >> _S82, int(_S84) - int(1));
    int _S86 = min(y1_0 >> _S82, int(shape_1.z) - int(1));

#line 650
    float nearest_0 = 1.00000001504746622e+30;

#line 650
    int y_5 = ly0_0;

    for(;;)
    {

#line 652
        if(y_5 <= _S86)
        {
        }
        else
        {

#line 652
            break;
        }

#line 652
        int x_5 = _S83;
        for(;;)
        {

#line 653
            if(x_5 <= _S85)
            {
            }
            else
            {

#line 653
                break;
            }

#line 654
            float _S87 = min(nearest_0, *(kernelContext_16->giProbes_0->screenHzb_0+(shape_1.x + uint(y_5) * _S84 + uint(x_5))));
            *reads_0 = *reads_0 + 1U;

#line 653
            int x_6 = x_5 + int(1);

#line 653
            nearest_0 = _S87;

#line 653
            x_5 = x_6;

#line 653
        }

#line 652
        y_5 = y_5 + int(1);

#line 652
    }

#line 658
    return nearest_0;
}




bool gpScreenRunEmpty_0(float3 origin_4, float3 direction_4, float step_3, uint index_0, uint run_0, const GiScreenStep_0 thread* here_0, uint width_2, uint height_2, uint thread* reads_1, KernelContext_0 thread* kernelContext_17)
{

#line 665
    GiScreenStep_0 _S88 = gpScreenStep_0(origin_4 + direction_4 * float3((step_3 * float(index_0 + run_0))) , width_2, height_2, kernelContext_17);

#line 665
    bool inside_0;

#line 670
    if(!_S88.outside_0)
    {

#line 670
        inside_0 = (_S88.x_3) >= 1.0;

#line 670
    }
    else
    {

#line 670
        inside_0 = false;

#line 670
    }

#line 670
    if(inside_0)
    {

#line 670
        inside_0 = (_S88.y_3) >= 1.0;

#line 670
    }
    else
    {

#line 670
        inside_0 = false;

#line 670
    }

#line 670
    if(inside_0)
    {

#line 670
        inside_0 = (_S88.x_3) < (float(width_2) - 1.0);

#line 670
    }
    else
    {

#line 670
        inside_0 = false;

#line 670
    }
    if(inside_0)
    {

)cy_msl"
    R"cy_msl(#line 671
        inside_0 = (_S88.y_3) < (float(height_2) - 1.0);

#line 671
    }
    else
    {

#line 671
        inside_0 = false;

#line 671
    }

#line 671
    if(inside_0)
    {

#line 671
        inside_0 = (here_0->x_3) >= 1.0;

#line 671
    }
    else
    {

#line 671
        inside_0 = false;

#line 671
    }
    if(inside_0)
    {

#line 672
        inside_0 = (here_0->y_3) >= 1.0;

#line 672
    }
    else
    {

#line 672
        inside_0 = false;

#line 672
    }

#line 672
    if(inside_0)
    {

#line 672
        inside_0 = (here_0->x_3) < (float(width_2) - 1.0);

#line 672
    }
    else
    {

#line 672
        inside_0 = false;

#line 672
    }

#line 672
    if(inside_0)
    {

#line 672
        inside_0 = (here_0->y_3) < (float(height_2) - 1.0);

#line 672
    }
    else
    {

#line 672
        inside_0 = false;

#line 672
    }

    if(!inside_0)
    {

#line 675
        return false;
    }

#line 675
    float _S89 = gpNearestDepth_0(max(int(floor(min(here_0->x_3, _S88.x_3))) - int(1), int(0)), max(int(floor(min(here_0->y_3, _S88.y_3))) - int(1), int(0)), min(int(floor(max(here_0->x_3, _S88.x_3))) + int(1), int(width_2) - int(1)), min(int(floor(max(here_0->y_3, _S88.y_3))) + int(1), int(height_2) - int(1)), reads_1, kernelContext_17);

#line 686
    return (max(here_0->depth_0, _S88.depth_0)) < (_S89 - abs(_S89) * 0.00009999999747379 - 0.00009999999747379);
}


#line 706
GiTierSample_0 gpScreenTrace_0(float3 origin_5, float3 direction_5, float maxDistance_2, bool accelerated_0, uint thread* reads_2, KernelContext_0 thread* kernelContext_18)
{
    thread GiTierSample_0 sample_2 = gpTierSample_0(5U);
    uint4 screen_0 = kernelContext_18->giProbes_0->constants_0[14U];
    uint width_3 = screen_0.x;
    uint height_3 = screen_0.y;

#line 711
    bool _S90;
    if(width_3 == 0U)
    {

#line 712
        _S90 = true;

#line 712
    }
    else
    {

#line 712
        _S90 = height_3 == 0U;

#line 712
    }

#line 712
    if(_S90)
    {

#line 713
        return sample_2;
    }
    float _S91 = (as_type<float>((screen_0.w)));
    uint _S92 = max(1U, screen_0.z);
    float _S93 = maxDistance_2 / float(_S92);

#line 717
    uint run_1 = 8U;

#line 717
    uint index_1 = 1U;

#line 722
    for(;;)
    {

#line 722
        if(index_1 <= _S92)
        {
        }
        else
        {

#line 722
            break;
        }

#line 723
        float t_5 = _S93 * float(index_1);

#line 723
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

#line 737
        float gap_0;

#line 737
        if(_S95)
        {

#line 737
            gap_0 = bufferDepth_0 - _S94.depth_0;

#line 737
        }
        else
        {

#line 737
            gap_0 = 1.00000001504746622e+30;

#line 737
        }
        if(_S95)
        {

#line 739
            float behind_0 = _S94.depth_0 - bufferDepth_0;
            if(behind_0 > 0.0)
            {

#line 741
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

#line 738
        }

#line 755
        if(accelerated_0)
        {

#line 755
            _S90 = (index_1 + 1U) < _S92;

#line 755
        }
        else
        {

#line 755
            _S90 = false;

#line 755
        }

#line 755
        bool _S96;

#line 755
        if(_S90)
        {

#line 755
            _S96 = gap_0 > (_S91 * 2.0);

#line 755
        }
        else
        {

#line 755
            _S96 = false;

#line 755
        }

#line 755
        uint skip_0;

#line 755
        if(_S96)
        {

#line 756
            uint _S97 = min(run_1, _S92 - index_1);

#line 756
            thread GiScreenStep_0 _S98 = _S94;

#line 756
            bool _S99 = gpScreenRunEmpty_0(origin_5, direction_5, _S93, index_1, _S97, &_S98, width_3, height_3, reads_2, kernelContext_18);

#line 756
            uint run_2;
            if(_S99)
            {

                uint _S100 = min(run_1 * 2U, 32U);

#line 760
                skip_0 = _S97;

#line 760
                run_2 = _S100;

#line 757
            }
            else
            {


                uint _S101 = max(run_1 / 2U, 2U);

#line 762
                skip_0 = 0U;

)cy_msl"
    R"cy_msl(#line 762
                run_2 = _S101;

#line 757
            }

#line 757
            run_1 = run_2;

#line 755
        }
        else
        {

#line 755
            skip_0 = 0U;

#line 755
        }

#line 755
        index_1 = index_1 + (1U + skip_0);

#line 722
    }

#line 768
    (&sample_2)->confidence_0 = 0.20000000298023224;
    (&sample_2)->hitDistance_0 = -1.0;
    return sample_2;
}


#line 823
uint gpTicket_0(uint counter_1, KernelContext_0 thread* kernelContext_19)
{
    uint count_1 = simd_sum(1U);
    uint rank_0 = simd_prefix_exclusive_sum(1U);

    bool _S102 = simd_is_first();

#line 828
    uint base_1;

#line 828
    if(_S102)
    {

#line 829
        uint _S103 = atomic_fetch_add_explicit(((atomic_uint device*)(kernelContext_19->giProbes_0->counters_0+counter_1)), count_1, memory_order_relaxed);

#line 829
        base_1 = _S103;

#line 828
    }
    else
    {

#line 828
        base_1 = 0U;

#line 828
    }


    uint _S104 = simd_broadcast_first(base_1);

#line 831
    return _S104 + rank_0;
}


#line 838
GiTierSample_0 gpTieredTrace_0(float3 origin_6, float3 direction_6, float maxDistance_3, bool screenPresent_0, uint thread* reads_3, KernelContext_0 thread* kernelContext_20)
{
    float3 unit_1 = giNormalisedOr_0(direction_6, float3(0.0, 0.0, -1.0));
    float4 hzb_0 = (as_type<float4>((kernelContext_20->giProbes_0->constants_0[15U])));
    float _S105 = min(maxDistance_3, hzb_0.z);
    float threshold_0 = hzb_0.w;
    bool accelerated_1 = (kernelContext_20->giProbes_0->constants_0[15U].y) != 0U;

    GiTierSample_0 _S106 = gpTierSample_0(0U);

#line 846
    GiTierSample_0 screen_1;
    if(screenPresent_0)
    {

#line 847
        gpCount_0(4U, 1U, kernelContext_20);

#line 847
        GiTierSample_0 _S107 = gpScreenTrace_0(origin_6, unit_1, _S105, accelerated_1, reads_3, kernelContext_20);


        if((_S107.confidence_0) >= threshold_0)
        {

#line 851
            return _S107;
        }

#line 851
        screen_1 = _S107;

#line 847
    }
    else
    {

#line 847
        screen_1 = _S106;

#line 847
    }

#line 858
    uint4 budget_0 = kernelContext_20->giProbes_0->constants_0[24U];

#line 858
    uint limit_0;
    if(screenPresent_0)
    {

#line 859
        limit_0 = min(budget_0.x, budget_0.y);

#line 859
    }
    else
    {

#line 859
        limit_0 = budget_0.x;

#line 859
    }

#line 859
    uint _S108 = gpTicket_0(0U, kernelContext_20);

    if(_S108 >= limit_0)
    {

#line 861
        gpCount_0(2U, 1U, kernelContext_20);

        return screen_1;
    }
    if(screenPresent_0)
    {

#line 865
        gpCount_0(1U, 1U, kernelContext_20);

#line 865
    }


    if((GiSoftwareTier_source_0()) == 7U)
    {

#line 868
        limit_0 = 6U;

#line 868
    }
    else
    {

#line 868
        limit_0 = 5U;

#line 868
    }

#line 868
    gpCount_0(limit_0, 1U, kernelContext_20);

#line 868
    GiTierSample_0 _S109 = gpWorldTrace_0(origin_6, unit_1, _S105, kernelContext_20);

    if((_S109.source_0) == 9U)
    {

#line 870
        limit_0 = 1U;

#line 870
    }
    else
    {

#line 870
        limit_0 = 0U;

#line 870
    }

#line 870
    gpCount_0(7U, limit_0, kernelContext_20);

#line 870
    bool _S110;
    if(!screenPresent_0)
    {

#line 871
        _S110 = true;

#line 871
    }
    else
    {

#line 871
        _S110 = (screen_1.confidence_0) <= 0.0;

#line 871
    }

#line 871
    if(_S110)
    {

#line 872
        return _S109;
    }
    thread GiTierSample_0 blended_0;
    (&blended_0)->radiance_0 = _S109.radiance_0 + (screen_1.radiance_0 - _S109.radiance_0) * float3(screen_1.confidence_0) ;
    (&blended_0)->confidence_0 = max(screen_1.confidence_0, _S109.confidence_0);
    bool _S111 = (screen_1.confidence_0) >= (_S109.confidence_0);

#line 877
    if(_S111)
    {

#line 877
        limit_0 = screen_1.source_0;

#line 877
    }
    else
    {

#line 877
        limit_0 = _S109.source_0;

#line 877
    }

#line 877
    (&blended_0)->source_0 = limit_0;

#line 877
    float _S112;

    if(_S111)
    {

#line 879
        _S112 = screen_1.hitDistance_0;

#line 879
    }
    else
    {

#line 879
        _S112 = _S109.hitDistance_0;

#line 879
    }

#line 878
    (&blended_0)->hitDistance_0 = _S112;

    return blended_0;
}

void gpWriteTierResult_0(uint ray_0, const GiTierSample_0 thread* sample_3, uint reads_4, KernelContext_0 thread* kernelContext_21)
{

#line 884
    uint _S113 = ray_0 * 2U;

#line 884
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

/// cyGiGatherProbesHardware.metal, 33989 bytes.
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


#line 78 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 81
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


#line 99 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 100
    float squared_0 = dot(v_0, v_0);

#line 100
    float3 _S1;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 101
        _S1 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 101
    }
    else
    {

#line 101
        _S1 = fallback_0;

#line 101
    }

#line 101
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


#line 27 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
float3 ghVertex_0(uint vertex_0, KernelContext_0 thread* kernelContext_1)
{

#line 28
    uint _S4 = vertex_0 * 3U;

#line 28
    return float3(kernelContext_1->giHardware_0->positions_0[_S4], kernelContext_1->giHardware_0->positions_0[_S4 + 1U], kernelContext_1->giHardware_0->positions_0[_S4 + 2U]);
}



GiWorldHit_0 GiHardwareTier_trace_0(float3 origin_0, float3 direction_1, float maxDistance_0, KernelContext_0 thread* kernelContext_2)
{

#line 34
    thread RayDesc_0 ray_0;
    (&ray_0)->Origin_0 = origin_0;
    (&ray_0)->Direction_0 = direction_1;
    (&ray_0)->TMin_0 = 0.00100000004749745;
    (&ray_0)->TMax_0 = maxDistance_0;
    thread raytracing::intersection_query<raytracing::triangle_data, raytracing::instancing> query_0;
    (&query_0)->reset({ (ray_0.Origin_0), (ray_0.Direction_0), (ray_0.TMin_0), (ray_0.TMax_0) }, (kernelContext_2->giHardware_0->topLevel_0), (255U), _slang_ray_flags_to_intersection_params((1U)));
    for(;;)
    {

#line 41
        bool _S5 = ((&query_0)->next());

#line 41
        if(_S5)
        {
        }
        else
        {

#line 41
            break;
        }

#line 41
    }

    uint _S6 = (_slang_committed_intersection_type_to_uint((&query_0)->get_committed_intersection_type()));

#line 43
    if(_S6 != 1U)
    {

#line 44
        return gpMiss_0();
    }
    float t_1 = ((&query_0)->get_committed_distance());
    uint instance_0 = ((&query_0)->get_committed_user_instance_id());
    uint primitive_0 = ((&query_0)->get_committed_primitive_id());
    uint _S7 = instance_0 * 4U;

)cy_msl"
    R"cy_msl(#line 49
    uint4 record_0 = kernelContext_2->giHardware_0->instances_0[_S7];
    uint first_0 = record_0.y + primitive_0 * 3U;
    uint _S8 = record_0.x;

#line 51
    float3 _S9 = ghVertex_0(_S8 + kernelContext_2->giHardware_0->indices_0[first_0], kernelContext_2);

#line 51
    float3 _S10 = ghVertex_0(_S8 + kernelContext_2->giHardware_0->indices_0[first_0 + 1U], kernelContext_2);

#line 51
    float3 _S11 = ghVertex_0(_S8 + kernelContext_2->giHardware_0->indices_0[first_0 + 2U], kernelContext_2);


    float4 c0_0 = (as_type<float4>((kernelContext_2->giHardware_0->instances_0[_S7 + 1U])));


    float3 local_0 = cross(_S10 - _S9, _S11 - _S9);
    float3 normal_1 = giNormalisedOr_0(c0_0.xyz * float3(local_0.x)  + (as_type<float4>((kernelContext_2->giHardware_0->instances_0[_S7 + 2U]))).xyz * float3(local_0.y)  + (as_type<float4>((kernelContext_2->giHardware_0->instances_0[_S7 + 3U]))).xyz * float3(local_0.z) , float3(0.0, 1.0, 0.0));

#line 58
    float3 normal_2;

    if((dot(normal_1, direction_1)) > 0.0)
    {

#line 60
        normal_2 = - normal_1;

#line 60
    }
    else
    {

#line 60
        normal_2 = normal_1;

#line 60
    }


    thread GiWorldHit_0 hit_1;
    (&hit_1)->hit_0 = true;
    (&hit_1)->t_0 = t_1;
    (&hit_1)->position_0 = origin_0 + direction_1 * float3(t_1) ;
    (&hit_1)->normal_0 = normal_2;


    (&hit_1)->confidence_0 = (as_type<float4>((kernelContext_2->giProbes_0->constants_0[25U]))).z - gpClamp_0(c0_0.w * 2.0, 0.0, 0.40000000596046448);
    return hit_1;
}


#line 471 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_1, float cell_0)
{

#line 472
    return int(floor(value_1 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_1, float3 normal_3, KernelContext_0 thread* kernelContext_3)
{

#line 482
    uint4 grid_0 = kernelContext_3->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 485
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S12 = radius_0 * radius_0;
    float _S13 = position_1.x;

#line 490
    int _S14 = giCell_0(_S13 - radius_0, cell_1);

#line 490
    float _S15 = position_1.y;

#line 490
    int _S16 = giCell_0(_S15 - radius_0, cell_1);
    float _S17 = position_1.z;

#line 491
    int _S18 = giCell_0(_S17 - radius_0, cell_1);
    int _S19 = giCell_0(_S13 + radius_0, cell_1);

#line 492
    int _S20 = giCell_0(_S15 + radius_0, cell_1);
    int _S21 = giCell_0(_S17 + radius_0, cell_1);

#line 493
    float bestScore_0 = 1.0e+09;

#line 493
    float bestAlignment_0 = 0.0;

#line 493
    uint best_0 = 4294967295U;

#line 493
    int z_1 = _S18;



    for(;;)
    {

#line 497
        if(z_1 <= _S21)
        {
        }
        else
        {

#line 497
            break;
        }

#line 497
        int y_1 = _S16;
        for(;;)
        {

#line 498
            if(y_1 <= _S20)
            {
            }
            else
            {

#line 498
                break;
            }

#line 498
            float bestScore_1 = bestScore_0;

#line 498
            float bestAlignment_1 = bestAlignment_0;

#line 498
            uint best_1 = best_0;

#line 498
            int x_1 = _S14;
            for(;;)
            {

#line 499
                if(x_1 <= _S19)
                {
                }
                else
                {

#line 499
                    break;
                }
                uint _S22 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 501
                uint first_1 = kernelContext_3->giScene_0->gridRanges_0[_S22];
                uint _S23 = kernelContext_3->giScene_0->gridRanges_0[_S22 + 1U];

#line 502
                float bestScore_2 = bestScore_1;

#line 502
                float bestAlignment_2 = bestAlignment_1;

#line 502
                uint best_2 = best_1;

#line 502
                uint item_0 = first_1;
                for(;;)
                {

#line 503
                    if(item_0 < (first_1 + _S23))
                    {
                    }
                    else
                    {

#line 503
                        break;
                    }

#line 504
                    uint handle_0 = kernelContext_3->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_3->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 506
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }



                    uint _S24 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_3->giScene_0->cards_0[_S24 + 1U].xyz, normal_3);
                    if(alignment_0 <= 0.25)
                    {

#line 511
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 513
                    float3 offset_0 = kernelContext_3->giScene_0->cards_0[_S24].xyz - position_1;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S12)
                    {

#line 516
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 519
                    float score_0 = squared_1 / alignment_0;

#line 519
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 520
                        better_0 = score_0 < bestScore_2;

#line 520
                    }
                    else
                    {

#line 521
                        if(alignment_0 != bestAlignment_2)
                        {

#line 521
                            better_0 = alignment_0 > bestAlignment_2;

#line 521
                        }
                        else
                        {

#line 521
                            better_0 = handle_0 < best_2;

#line 521
                        }

#line 520
                    }

#line 520
                    float bestScore_3;

#line 520
                    float bestAlignment_3;

#line 520
                    uint best_3;


                    if(better_0)
                    {

#line 523
                        bestScore_3 = score_0;

#line 523
                        bestAlignment_3 = alignment_0;

#line 523
                        best_3 = handle_0;

#line 523
                    }
                    else
                    {

)cy_msl"
    R"cy_msl(#line 523
                        bestScore_3 = bestScore_2;

#line 523
                        bestAlignment_3 = bestAlignment_2;

#line 523
                        best_3 = best_2;

#line 523
                    }

#line 523
                    bestScore_2 = bestScore_3;

#line 523
                    bestAlignment_2 = bestAlignment_3;

#line 523
                    best_2 = best_3;

#line 503
                    item_0 = item_0 + 1U;

#line 503
                }

#line 499
                int x_2 = x_1 + int(1);

#line 499
                bestScore_1 = bestScore_2;

#line 499
                bestAlignment_1 = bestAlignment_2;

#line 499
                best_1 = best_2;

#line 499
                x_1 = x_2;

#line 499
            }

#line 498
            int y_2 = y_1 + int(1);

#line 498
            bestScore_0 = bestScore_1;

#line 498
            bestAlignment_0 = bestAlignment_1;

#line 498
            best_0 = best_1;

#line 498
            y_1 = y_2;

#line 498
        }

#line 497
        z_1 = z_1 + int(1);

#line 497
    }

#line 532
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


#line 25 "src/rendering/gi_gpu/shaders/gi_hardware.slang"
[[kernel]] void cyGiGatherProbesHardware(uint3 group_0 [[threadgroup_position_in_grid]], uint3 lane_1 [[thread_position_in_threadgroup]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiDispatch_0 constant* giDispatch_1 [[buffer(1)]], GiHardwareSet_default_0 constant* giHardware_1 [[buffer(3)]])
{

#line 25
    thread KernelContext_0 kernelContext_6;

#line 25
    (&kernelContext_6)->giProbes_0 = giProbes_1;

#line 25
    (&kernelContext_6)->giScene_0 = giScene_1;

#line 25
    (&kernelContext_6)->giDispatch_0 = giDispatch_1;

#line 25
    (&kernelContext_6)->giHardware_0 = giHardware_1;

#line 25
    threadgroup array<float3, int(64)> gpRayRadiance_1;

#line 25
    (&kernelContext_6)->gpRayRadiance_0 = &gpRayRadiance_1;

#line 25
    threadgroup array<float, int(64)> gpRayDistance_1;

#line 25
    (&kernelContext_6)->gpRayDistance_0 = &gpRayDistance_1;

#line 25
    gpGatherProbe_0(group_0.x, lane_1.x, &kernelContext_6);

    return;
}

)cy_msl";

/// cyGiTraceHardware.metal, 19977 bytes.
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
            

#line 689 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiTierSample_0
{
    float3 radiance_0;
    float confidence_0;
    float hitDistance_0;
    uint source_0;
};


#line 81 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 78 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 78
struct GiHardwareSet_default_0
{
    metal::raytracing::acceleration_structure<metal::raytracing::instancing> topLevel_0;
    uint4 device* instances_0;
    float device* positions_0;
    uint device* indices_0;
};


#line 78
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


#line 27 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
float3 ghVertex_0(uint vertex_0, KernelContext_0 thread* kernelContext_0)
{

#line 28
    uint _S1 = vertex_0 * 3U;

#line 28
    return float3(kernelContext_0->giHardware_0->positions_0[_S1], kernelContext_0->giHardware_0->positions_0[_S1 + 1U], kernelContext_0->giHardware_0->positions_0[_S1 + 2U]);
}


#line 99 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 100
    float squared_0 = dot(v_0, v_0);

#line 100
    float3 _S2;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 101
        _S2 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 101
    }
    else
    {

#line 101
        _S2 = fallback_0;

#line 101
    }

#line 101
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


#line 33 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
GiWorldHit_0 GiHardwareTier_trace_0(float3 origin_0, float3 direction_0, float maxDistance_0, KernelContext_0 thread* kernelContext_1)
{

#line 34
    thread RayDesc_0 ray_0;
    (&ray_0)->Origin_0 = origin_0;
    (&ray_0)->Direction_0 = direction_0;
    (&ray_0)->TMin_0 = 0.00100000004749745;
    (&ray_0)->TMax_0 = maxDistance_0;
    thread raytracing::intersection_query<raytracing::triangle_data, raytracing::instancing> query_0;
    (&query_0)->reset({ (ray_0.Origin_0), (ray_0.Direction_0), (ray_0.TMin_0), (ray_0.TMax_0) }, (kernelContext_1->giHardware_0->topLevel_0), (255U), _slang_ray_flags_to_intersection_params((1U)));
    for(;;)
    {

#line 41
        bool _S4 = ((&query_0)->next());

#line 41
        if(_S4)
        {
        }
        else
        {

#line 41
            break;
        }

#line 41
    }

    uint _S5 = (_slang_committed_intersection_type_to_uint((&query_0)->get_committed_intersection_type()));

#line 43
    if(_S5 != 1U)
    {

#line 44
        return gpMiss_0();
    }
    float t_1 = ((&query_0)->get_committed_distance());
    uint instance_0 = ((&query_0)->get_committed_user_instance_id());
    uint primitive_0 = ((&query_0)->get_committed_primitive_id());
    uint _S6 = instance_0 * 4U;

#line 49
    uint4 record_0 = kernelContext_1->giHardware_0->instances_0[_S6];
    uint first_0 = record_0.y + primitive_0 * 3U;
    uint _S7 = record_0.x;

#line 51
    float3 _S8 = ghVertex_0(_S7 + kernelContext_1->giHardware_0->indices_0[first_0], kernelContext_1);

#line 51
    float3 _S9 = ghVertex_0(_S7 + kernelContext_1->giHardware_0->indices_0[first_0 + 1U], kernelContext_1);

#line 51
    float3 _S10 = ghVertex_0(_S7 + kernelContext_1->giHardware_0->indices_0[first_0 + 2U], kernelContext_1);

)cy_msl"
    R"cy_msl(
    float4 c0_0 = (as_type<float4>((kernelContext_1->giHardware_0->instances_0[_S6 + 1U])));


    float3 local_0 = cross(_S9 - _S8, _S10 - _S8);
    float3 normal_1 = giNormalisedOr_0(c0_0.xyz * float3(local_0.x)  + (as_type<float4>((kernelContext_1->giHardware_0->instances_0[_S6 + 2U]))).xyz * float3(local_0.y)  + (as_type<float4>((kernelContext_1->giHardware_0->instances_0[_S6 + 3U]))).xyz * float3(local_0.z) , float3(0.0, 1.0, 0.0));

#line 58
    float3 normal_2;

    if((dot(normal_1, direction_0)) > 0.0)
    {

#line 60
        normal_2 = - normal_1;

#line 60
    }
    else
    {

#line 60
        normal_2 = normal_1;

#line 60
    }


    thread GiWorldHit_0 hit_1;
    (&hit_1)->hit_0 = true;
    (&hit_1)->t_0 = t_1;
    (&hit_1)->position_0 = origin_0 + direction_0 * float3(t_1) ;
    (&hit_1)->normal_0 = normal_2;


    (&hit_1)->confidence_1 = (as_type<float4>((kernelContext_1->giProbes_0->constants_0[25U]))).z - gpClamp_0(c0_0.w * 2.0, 0.0, 0.40000000596046448);
    return hit_1;
}


#line 73
uint GiHardwareTier_source_0()
{

#line 74
    return 7U;
}


#line 696 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpTierSample_0(uint source_1)
{

#line 697
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


#line 471 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_1, float cell_0)
{

#line 472
    return int(floor(value_1 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_1, float3 normal_3, KernelContext_0 thread* kernelContext_3)
{

#line 482
    uint4 grid_0 = kernelContext_3->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 485
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S12 = radius_0 * radius_0;
    float _S13 = position_1.x;

#line 490
    int _S14 = giCell_0(_S13 - radius_0, cell_1);

#line 490
    float _S15 = position_1.y;

#line 490
    int _S16 = giCell_0(_S15 - radius_0, cell_1);
    float _S17 = position_1.z;

#line 491
    int _S18 = giCell_0(_S17 - radius_0, cell_1);
    int _S19 = giCell_0(_S13 + radius_0, cell_1);

#line 492
    int _S20 = giCell_0(_S15 + radius_0, cell_1);
    int _S21 = giCell_0(_S17 + radius_0, cell_1);

#line 493
    float bestScore_0 = 1.0e+09;

#line 493
    float bestAlignment_0 = 0.0;

#line 493
    uint best_0 = 4294967295U;

#line 493
    int z_1 = _S18;



    for(;;)
    {

#line 497
        if(z_1 <= _S21)
        {
        }
        else
        {

#line 497
            break;
        }

#line 497
        int y_1 = _S16;
        for(;;)
        {

#line 498
            if(y_1 <= _S20)
            {
            }
            else
            {

#line 498
                break;
            }

#line 498
            float bestScore_1 = bestScore_0;

#line 498
            float bestAlignment_1 = bestAlignment_0;

#line 498
            uint best_1 = best_0;

#line 498
            int x_1 = _S14;
            for(;;)
            {

#line 499
                if(x_1 <= _S19)
                {
                }
                else
                {

#line 499
                    break;
                }
                uint _S22 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 501
                uint first_1 = kernelContext_3->giScene_0->gridRanges_0[_S22];
                uint _S23 = kernelContext_3->giScene_0->gridRanges_0[_S22 + 1U];

#line 502
                float bestScore_2 = bestScore_1;

#line 502
                float bestAlignment_2 = bestAlignment_1;

#line 502
                uint best_2 = best_1;

#line 502
                uint item_0 = first_1;
                for(;;)
                {

#line 503
                    if(item_0 < (first_1 + _S23))
                    {
                    }
                    else
                    {

#line 503
                        break;
                    }

#line 504
                    uint handle_0 = kernelContext_3->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_3->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 506
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }



                    uint _S24 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_3->giScene_0->cards_0[_S24 + 1U].xyz, normal_3);
                    if(alignment_0 <= 0.25)
                    {

#line 511
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 513
                    float3 offset_0 = kernelContext_3->giScene_0->cards_0[_S24].xyz - position_1;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S12)
                    {

#line 516
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 519
                    float score_0 = squared_1 / alignment_0;

#line 519
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 520
                        better_0 = score_0 < bestScore_2;

#line 520
                    }
                    else
                    {

#line 521
                        if(alignment_0 != bestAlignment_2)
                        {

#line 521
                            better_0 = alignment_0 > bestAlignment_2;

#line 521
                        }
                        else
                        {

)cy_msl"
    R"cy_msl(#line 521
                            better_0 = handle_0 < best_2;

#line 521
                        }

#line 520
                    }

#line 520
                    float bestScore_3;

#line 520
                    float bestAlignment_3;

#line 520
                    uint best_3;


                    if(better_0)
                    {

#line 523
                        bestScore_3 = score_0;

#line 523
                        bestAlignment_3 = alignment_0;

#line 523
                        best_3 = handle_0;

#line 523
                    }
                    else
                    {

#line 523
                        bestScore_3 = bestScore_2;

#line 523
                        bestAlignment_3 = bestAlignment_2;

#line 523
                        best_3 = best_2;

#line 523
                    }

#line 523
                    bestScore_2 = bestScore_3;

#line 523
                    bestAlignment_2 = bestAlignment_3;

#line 523
                    best_2 = best_3;

#line 503
                    item_0 = item_0 + 1U;

#line 503
                }

#line 499
                int x_2 = x_1 + int(1);

#line 499
                bestScore_1 = bestScore_2;

#line 499
                bestAlignment_1 = bestAlignment_2;

#line 499
                best_1 = best_2;

#line 499
                x_1 = x_2;

#line 499
            }

#line 498
            int y_2 = y_1 + int(1);

#line 498
            bestScore_0 = bestScore_1;

#line 498
            bestAlignment_0 = bestAlignment_1;

#line 498
            best_0 = best_1;

#line 498
            y_1 = y_2;

#line 498
        }

#line 497
        z_1 = z_1 + int(1);

#line 497
    }

#line 532
    return best_0;
}


#line 776 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpWorldRadiance_0(const GiWorldHit_0 thread* hit_2, float3 direction_2, uint source_2, uint cacheFrame_0, KernelContext_0 thread* kernelContext_4)
{

#line 777
    thread GiTierSample_0 sample_1 = gpTierSample_0(source_2);
    if(!hit_2->hit_0)
    {

#line 779
        (&sample_1)->source_0 = 9U;

#line 779
        float3 _S25 = gpSky_0(direction_2, kernelContext_4);
        (&sample_1)->radiance_0 = _S25;
        (&sample_1)->confidence_0 = 1.0;
        (&sample_1)->hitDistance_0 = -1.0;
        return sample_1;
    }
    (&sample_1)->hitDistance_0 = hit_2->t_0;

#line 785
    uint _S26 = giFindCard_0(hit_2->position_0, hit_2->normal_0, kernelContext_4);

    if(_S26 != 4294967295U)
    {

#line 788
        uint _S27 = _S26 * 2U;

#line 788
        (&sample_1)->radiance_0 = (*(kernelContext_4->giScene_0->cardState_0+_S27)).xyz;
        uint last_0 = (as_type<uint>(((*(kernelContext_4->giScene_0->cardState_0+(_S27 + 1U))).w)));

#line 789
        float age_0;
        if(cacheFrame_0 > last_0)
        {

#line 790
            age_0 = float(cacheFrame_0 - last_0);

#line 790
        }
        else
        {

#line 790
            age_0 = 0.0;

#line 790
        }


        (&sample_1)->confidence_0 = hit_2->confidence_1 * (1.0 - gpClamp_0(age_0 / 240.0, 0.0, 1.0) * 0.60000002384185791);
        return sample_1;
    }
    (&sample_1)->confidence_0 = hit_2->confidence_1 * 0.20000000298023224;
    return sample_1;
}


GiTierSample_0 gpWorldTrace_0(float3 origin_1, float3 unit_0, float reach_0, KernelContext_0 thread* kernelContext_5)
{

#line 802
    uint cacheFrame_1 = kernelContext_5->giProbes_0->constants_0[24U].w;

#line 802
    GiWorldHit_0 _S28 = GiHardwareTier_trace_0(origin_1, unit_0, reach_0, kernelContext_5);

    uint _S29 = GiHardwareTier_source_0();

#line 804
    thread GiWorldHit_0 _S30 = _S28;

#line 804
    GiTierSample_0 _S31 = gpWorldRadiance_0(&_S30, unit_0, _S29, cacheFrame_1, kernelContext_5);

#line 804
    return _S31;
}


#line 883
void gpWriteTierResult_0(uint ray_1, const GiTierSample_0 thread* sample_2, uint reads_0, KernelContext_0 thread* kernelContext_6)
{

#line 884
    uint _S32 = ray_1 * 2U;

#line 884
    *(kernelContext_6->giProbes_0->tierResults_0+_S32) = float4(sample_2->radiance_0, sample_2->confidence_0);
    *(kernelContext_6->giProbes_0->tierResults_0+(_S32 + 1U)) = float4(sample_2->hitDistance_0, (as_type<float>((sample_2->source_0))), (as_type<float>((reads_0))), 0.0);

    return;
}


#line 31 "src/rendering/gi_gpu/shaders/gi_hardware.slang"
[[kernel]] void cyGiTraceHardware(uint3 thread_0 [[thread_position_in_grid]], GiDispatch_0 constant* giDispatch_1 [[buffer(1)]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiHardwareSet_default_0 constant* giHardware_1 [[buffer(3)]])
{

#line 31
    thread KernelContext_0 kernelContext_7;

#line 31
    (&kernelContext_7)->giDispatch_0 = giDispatch_1;

#line 31
    (&kernelContext_7)->giProbes_0 = giProbes_1;

#line 31
    (&kernelContext_7)->giScene_0 = giScene_1;

#line 31
    (&kernelContext_7)->giHardware_0 = giHardware_1;
    uint ray_2 = thread_0.x;
    if(ray_2 >= (giDispatch_1->count_0))
    {

#line 34
        return;
    }
    uint _S33 = ray_2 * 2U;

#line 36
    float4 origin_2 = (&kernelContext_7)->giProbes_0->tierRays_0[_S33];

#line 36
    GiTierSample_0 _S34 = gpWorldTrace_0(origin_2.xyz, giNormalisedOr_0((&kernelContext_7)->giProbes_0->tierRays_0[_S33 + 1U].xyz, float3(0.0, 0.0, -1.0)), origin_2.w, &kernelContext_7);

#line 36
    thread GiTierSample_0 _S35 = _S34;

#line 36
    gpWriteTierResult_0(ray_2, &_S35, 0U, &kernelContext_7);



    return;
}

)cy_msl";

/// cyGiTraceTieredHardware.metal, 35828 bytes.
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
            

#line 689 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiTierSample_0
{
    float3 radiance_0;
    float confidence_0;
    float hitDistance_0;
    uint source_0;
};


#line 81 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 78 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 22 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
struct GiHardwareSet_default_0
{
    metal::raytracing::acceleration_structure<metal::raytracing::instancing> topLevel_0;
    uint4 device* instances_0;
    float device* positions_0;
    uint device* indices_0;
};


#line 22
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


#line 27 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
float3 ghVertex_0(uint vertex_0, KernelContext_0 thread* kernelContext_0)
{

#line 28
    uint _S1 = vertex_0 * 3U;

#line 28
    return float3(kernelContext_0->giHardware_0->positions_0[_S1], kernelContext_0->giHardware_0->positions_0[_S1 + 1U], kernelContext_0->giHardware_0->positions_0[_S1 + 2U]);
}


#line 99 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 100
    float squared_0 = dot(v_0, v_0);

#line 100
    float3 _S2;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 101
        _S2 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 101
    }
    else
    {

#line 101
        _S2 = fallback_0;

#line 101
    }

#line 101
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


#line 33 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
GiWorldHit_0 GiHardwareTier_trace_0(float3 origin_0, float3 direction_0, float maxDistance_0, KernelContext_0 thread* kernelContext_1)
{

#line 34
    thread RayDesc_0 ray_0;
    (&ray_0)->Origin_0 = origin_0;
    (&ray_0)->Direction_0 = direction_0;
    (&ray_0)->TMin_0 = 0.00100000004749745;
    (&ray_0)->TMax_0 = maxDistance_0;
    thread raytracing::intersection_query<raytracing::triangle_data, raytracing::instancing> query_0;
    (&query_0)->reset({ (ray_0.Origin_0), (ray_0.Direction_0), (ray_0.TMin_0), (ray_0.TMax_0) }, (kernelContext_1->giHardware_0->topLevel_0), (255U), _slang_ray_flags_to_intersection_params((1U)));
    for(;;)
    {

#line 41
        bool _S4 = ((&query_0)->next());

#line 41
        if(_S4)
        {
        }
        else
        {

#line 41
            break;
        }

#line 41
    }

    uint _S5 = (_slang_committed_intersection_type_to_uint((&query_0)->get_committed_intersection_type()));

#line 43
    if(_S5 != 1U)
    {

#line 44
        return gpMiss_0();
    }
    float t_1 = ((&query_0)->get_committed_distance());
    uint instance_0 = ((&query_0)->get_committed_user_instance_id());
    uint primitive_0 = ((&query_0)->get_committed_primitive_id());
    uint _S6 = instance_0 * 4U;

#line 49
    uint4 record_0 = kernelContext_1->giHardware_0->instances_0[_S6];
    uint first_0 = record_0.y + primitive_0 * 3U;
    uint _S7 = record_0.x;

#line 51
    float3 _S8 = ghVertex_0(_S7 + kernelContext_1->giHardware_0->indices_0[first_0], kernelContext_1);

#line 51
    float3 _S9 = ghVertex_0(_S7 + kernelContext_1->giHardware_0->indices_0[first_0 + 1U], kernelContext_1);

#line 51
    float3 _S10 = ghVertex_0(_S7 + kernelContext_1->giHardware_0->indices_0[first_0 + 2U], kernelContext_1);

)cy_msl"
    R"cy_msl(
    float4 c0_0 = (as_type<float4>((kernelContext_1->giHardware_0->instances_0[_S6 + 1U])));


    float3 local_0 = cross(_S9 - _S8, _S10 - _S8);
    float3 normal_1 = giNormalisedOr_0(c0_0.xyz * float3(local_0.x)  + (as_type<float4>((kernelContext_1->giHardware_0->instances_0[_S6 + 2U]))).xyz * float3(local_0.y)  + (as_type<float4>((kernelContext_1->giHardware_0->instances_0[_S6 + 3U]))).xyz * float3(local_0.z) , float3(0.0, 1.0, 0.0));

#line 58
    float3 normal_2;

    if((dot(normal_1, direction_0)) > 0.0)
    {

#line 60
        normal_2 = - normal_1;

#line 60
    }
    else
    {

#line 60
        normal_2 = normal_1;

#line 60
    }


    thread GiWorldHit_0 hit_1;
    (&hit_1)->hit_0 = true;
    (&hit_1)->t_0 = t_1;
    (&hit_1)->position_0 = origin_0 + direction_0 * float3(t_1) ;
    (&hit_1)->normal_0 = normal_2;


    (&hit_1)->confidence_1 = (as_type<float4>((kernelContext_1->giProbes_0->constants_0[25U]))).z - gpClamp_0(c0_0.w * 2.0, 0.0, 0.40000000596046448);
    return hit_1;
}


#line 73
uint GiHardwareTier_source_0()
{

#line 74
    return 7U;
}


#line 696 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpTierSample_0(uint source_1)
{

#line 697
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


#line 471 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_1, float cell_0)
{

#line 472
    return int(floor(value_1 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_1, float3 normal_3, KernelContext_0 thread* kernelContext_3)
{

#line 482
    uint4 grid_0 = kernelContext_3->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 485
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S12 = radius_0 * radius_0;
    float _S13 = position_1.x;

#line 490
    int _S14 = giCell_0(_S13 - radius_0, cell_1);

#line 490
    float _S15 = position_1.y;

#line 490
    int _S16 = giCell_0(_S15 - radius_0, cell_1);
    float _S17 = position_1.z;

#line 491
    int _S18 = giCell_0(_S17 - radius_0, cell_1);
    int _S19 = giCell_0(_S13 + radius_0, cell_1);

#line 492
    int _S20 = giCell_0(_S15 + radius_0, cell_1);
    int _S21 = giCell_0(_S17 + radius_0, cell_1);

#line 493
    float bestScore_0 = 1.0e+09;

#line 493
    float bestAlignment_0 = 0.0;

#line 493
    uint best_0 = 4294967295U;

#line 493
    int z_1 = _S18;



    for(;;)
    {

#line 497
        if(z_1 <= _S21)
        {
        }
        else
        {

#line 497
            break;
        }

#line 497
        int y_1 = _S16;
        for(;;)
        {

#line 498
            if(y_1 <= _S20)
            {
            }
            else
            {

#line 498
                break;
            }

#line 498
            float bestScore_1 = bestScore_0;

#line 498
            float bestAlignment_1 = bestAlignment_0;

#line 498
            uint best_1 = best_0;

#line 498
            int x_1 = _S14;
            for(;;)
            {

#line 499
                if(x_1 <= _S19)
                {
                }
                else
                {

#line 499
                    break;
                }
                uint _S22 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 501
                uint first_1 = kernelContext_3->giScene_0->gridRanges_0[_S22];
                uint _S23 = kernelContext_3->giScene_0->gridRanges_0[_S22 + 1U];

#line 502
                float bestScore_2 = bestScore_1;

#line 502
                float bestAlignment_2 = bestAlignment_1;

#line 502
                uint best_2 = best_1;

#line 502
                uint item_0 = first_1;
                for(;;)
                {

#line 503
                    if(item_0 < (first_1 + _S23))
                    {
                    }
                    else
                    {

#line 503
                        break;
                    }

#line 504
                    uint handle_0 = kernelContext_3->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_3->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 506
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }



                    uint _S24 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_3->giScene_0->cards_0[_S24 + 1U].xyz, normal_3);
                    if(alignment_0 <= 0.25)
                    {

#line 511
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 513
                    float3 offset_0 = kernelContext_3->giScene_0->cards_0[_S24].xyz - position_1;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S12)
                    {

#line 516
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 519
                    float score_0 = squared_1 / alignment_0;

#line 519
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 520
                        better_0 = score_0 < bestScore_2;

#line 520
                    }
                    else
                    {

#line 521
                        if(alignment_0 != bestAlignment_2)
                        {

#line 521
                            better_0 = alignment_0 > bestAlignment_2;

#line 521
                        }
                        else
                        {

)cy_msl"
    R"cy_msl(#line 521
                            better_0 = handle_0 < best_2;

#line 521
                        }

#line 520
                    }

#line 520
                    float bestScore_3;

#line 520
                    float bestAlignment_3;

#line 520
                    uint best_3;


                    if(better_0)
                    {

#line 523
                        bestScore_3 = score_0;

#line 523
                        bestAlignment_3 = alignment_0;

#line 523
                        best_3 = handle_0;

#line 523
                    }
                    else
                    {

#line 523
                        bestScore_3 = bestScore_2;

#line 523
                        bestAlignment_3 = bestAlignment_2;

#line 523
                        best_3 = best_2;

#line 523
                    }

#line 523
                    bestScore_2 = bestScore_3;

#line 523
                    bestAlignment_2 = bestAlignment_3;

#line 523
                    best_2 = best_3;

#line 503
                    item_0 = item_0 + 1U;

#line 503
                }

#line 499
                int x_2 = x_1 + int(1);

#line 499
                bestScore_1 = bestScore_2;

#line 499
                bestAlignment_1 = bestAlignment_2;

#line 499
                best_1 = best_2;

#line 499
                x_1 = x_2;

#line 499
            }

#line 498
            int y_2 = y_1 + int(1);

#line 498
            bestScore_0 = bestScore_1;

#line 498
            bestAlignment_0 = bestAlignment_1;

#line 498
            best_0 = best_1;

#line 498
            y_1 = y_2;

#line 498
        }

#line 497
        z_1 = z_1 + int(1);

#line 497
    }

#line 532
    return best_0;
}


#line 776 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpWorldRadiance_0(const GiWorldHit_0 thread* hit_2, float3 direction_2, uint source_2, uint cacheFrame_0, KernelContext_0 thread* kernelContext_4)
{

#line 777
    thread GiTierSample_0 sample_1 = gpTierSample_0(source_2);
    if(!hit_2->hit_0)
    {

#line 779
        (&sample_1)->source_0 = 9U;

#line 779
        float3 _S25 = gpSky_0(direction_2, kernelContext_4);
        (&sample_1)->radiance_0 = _S25;
        (&sample_1)->confidence_0 = 1.0;
        (&sample_1)->hitDistance_0 = -1.0;
        return sample_1;
    }
    (&sample_1)->hitDistance_0 = hit_2->t_0;

#line 785
    uint _S26 = giFindCard_0(hit_2->position_0, hit_2->normal_0, kernelContext_4);

    if(_S26 != 4294967295U)
    {

#line 788
        uint _S27 = _S26 * 2U;

#line 788
        (&sample_1)->radiance_0 = (*(kernelContext_4->giScene_0->cardState_0+_S27)).xyz;
        uint last_0 = (as_type<uint>(((*(kernelContext_4->giScene_0->cardState_0+(_S27 + 1U))).w)));

#line 789
        float age_0;
        if(cacheFrame_0 > last_0)
        {

#line 790
            age_0 = float(cacheFrame_0 - last_0);

#line 790
        }
        else
        {

#line 790
            age_0 = 0.0;

#line 790
        }


        (&sample_1)->confidence_0 = hit_2->confidence_1 * (1.0 - gpClamp_0(age_0 / 240.0, 0.0, 1.0) * 0.60000002384185791);
        return sample_1;
    }
    (&sample_1)->confidence_0 = hit_2->confidence_1 * 0.20000000298023224;
    return sample_1;
}


GiTierSample_0 gpWorldTrace_0(float3 origin_1, float3 unit_0, float reach_0, KernelContext_0 thread* kernelContext_5)
{

#line 802
    uint cacheFrame_1 = kernelContext_5->giProbes_0->constants_0[24U].w;

#line 802
    GiWorldHit_0 _S28 = GiHardwareTier_trace_0(origin_1, unit_0, reach_0, kernelContext_5);

    uint _S29 = GiHardwareTier_source_0();

#line 804
    thread GiWorldHit_0 _S30 = _S28;

#line 804
    GiTierSample_0 _S31 = gpWorldRadiance_0(&_S30, unit_0, _S29, cacheFrame_1, kernelContext_5);

#line 804
    return _S31;
}


#line 814
void gpCount_0(uint counter_0, uint amount_0, KernelContext_0 thread* kernelContext_6)
{

#line 815
    uint total_0 = simd_sum(amount_0);
    bool _S32 = simd_is_first();

#line 816
    bool _S33;

#line 816
    if(_S32)
    {

#line 816
        _S33 = total_0 != 0U;

#line 816
    }
    else
    {

#line 816
        _S33 = false;

#line 816
    }

#line 816
    if(_S33)
    {

#line 817
        uint _S34 = atomic_fetch_add_explicit(((atomic_uint device*)(kernelContext_6->giProbes_0->counters_0+counter_0)), total_0, memory_order_relaxed);

#line 816
    }


    return;
}


#line 617
struct GiScreenStep_0
{
    bool outside_0;
    float x_3;
    float y_3;
    float depth_0;
};


#line 588
float3 gpTransformPoint_0(uint first_2, float3 p_0, KernelContext_0 thread* kernelContext_7)
{



    return ((as_type<float4>((kernelContext_7->giProbes_0->constants_0[first_2]))) * float4(p_0.x)  + (as_type<float4>((kernelContext_7->giProbes_0->constants_0[first_2 + 1U]))) * float4(p_0.y)  + (as_type<float4>((kernelContext_7->giProbes_0->constants_0[first_2 + 2U]))) * float4(p_0.z)  + (as_type<float4>((kernelContext_7->giProbes_0->constants_0[first_2 + 3U])))).xyz;
}

float3 gpProjectPoint_0(uint first_3, float3 p_1, KernelContext_0 thread* kernelContext_8)
{



    float4 clip_0 = (as_type<float4>((kernelContext_8->giProbes_0->constants_0[first_3]))) * float4(p_1.x)  + (as_type<float4>((kernelContext_8->giProbes_0->constants_0[first_3 + 1U]))) * float4(p_1.y)  + (as_type<float4>((kernelContext_8->giProbes_0->constants_0[first_3 + 2U]))) * float4(p_1.z)  + (as_type<float4>((kernelContext_8->giProbes_0->constants_0[first_3 + 3U])));
    float _S35 = clip_0.w;

#line 602
    if(_S35 == 0.0)
    {

#line 603
        return clip_0.xyz;
    }
    return clip_0.xyz * float3((1.0 / _S35)) ;
}


#line 625
GiScreenStep_0 gpScreenStep_0(float3 point_0, uint width_0, uint height_0, KernelContext_0 thread* kernelContext_9)
{

#line 625
    float3 _S36 = gpTransformPoint_0(16U, point_0, kernelContext_9);
    thread GiScreenStep_0 step_0;

    float _S37 = - _S36.z;

#line 628
    (&step_0)->depth_0 = _S37;

#line 628
    float3 _S38 = gpProjectPoint_0(20U, point_0, kernelContext_9);

    float _S39 = float(width_0);

#line 630
    (&step_0)->x_3 = (_S38.x * 0.5 + 0.5) * _S39;
    float _S40 = float(height_0);

#line 631
    (&step_0)->y_3 = (0.5 - _S38.y * 0.5) * _S40;

)cy_msl"
    R"cy_msl(#line 631
    bool _S41;
    if(_S37 <= 0.0)
    {

#line 632
        _S41 = true;

#line 632
    }
    else
    {

#line 632
        _S41 = ((&step_0)->x_3) < 0.0;

#line 632
    }

#line 632
    if(_S41)
    {

#line 632
        _S41 = true;

#line 632
    }
    else
    {

#line 632
        _S41 = ((&step_0)->y_3) < 0.0;

#line 632
    }

#line 632
    if(_S41)
    {

#line 632
        _S41 = true;

#line 632
    }
    else
    {

#line 632
        _S41 = ((&step_0)->x_3) >= _S39;

#line 632
    }
    if(_S41)
    {

#line 633
        _S41 = true;

#line 633
    }
    else
    {

#line 633
        _S41 = ((&step_0)->y_3) >= _S40;

#line 633
    }

#line 632
    (&step_0)->outside_0 = _S41;

    return step_0;
}


#line 609
float gpEdgeConfidence_0(float x_4, float y_4, uint width_1, uint height_1)
{
    float u_0 = x_4 / float(width_1);
    float v_1 = y_4 / float(height_1);

    return gpClamp_0(min(min(u_0, 1.0 - u_0), min(v_1, 1.0 - v_1)) / 0.10000000149011612, 0.0, 1.0);
}


#line 639
float gpNearestDepth_0(int x0_0, int y0_0, int x1_0, int y1_0, uint thread* reads_0, KernelContext_0 thread* kernelContext_10)
{

#line 640
    uint _S42 = kernelContext_10->giProbes_0->constants_0[15U].x;
    int _S43 = max(x1_0 - x0_0, y1_0 - y0_0);

#line 641
    uint level_0 = 0U;

    for(;;)
    {

#line 643
        uint _S44 = level_0 + 1U;

#line 643
        bool _S45;

#line 643
        if(_S44 < _S42)
        {

#line 643
            _S45 = (int(1) << uint(int(level_0))) <= _S43;

#line 643
        }
        else
        {

#line 643
            _S45 = false;

#line 643
        }

#line 643
        if(_S45)
        {
        }
        else
        {

#line 643
            break;
        }

#line 643
        level_0 = _S44;

#line 643
    }


    uint4 shape_0 = kernelContext_10->giProbes_0->constants_0[26U + level_0];

#line 646
    uint _S46 = uint(int(level_0));
    int _S47 = x0_0 >> _S46;
    int ly0_0 = y0_0 >> _S46;
    uint _S48 = shape_0.y;

#line 649
    int _S49 = min(x1_0 >> _S46, int(_S48) - int(1));
    int _S50 = min(y1_0 >> _S46, int(shape_0.z) - int(1));

#line 650
    float nearest_0 = 1.00000001504746622e+30;

#line 650
    int y_5 = ly0_0;

    for(;;)
    {

#line 652
        if(y_5 <= _S50)
        {
        }
        else
        {

#line 652
            break;
        }

#line 652
        int x_5 = _S47;
        for(;;)
        {

#line 653
            if(x_5 <= _S49)
            {
            }
            else
            {

#line 653
                break;
            }

#line 654
            float _S51 = min(nearest_0, *(kernelContext_10->giProbes_0->screenHzb_0+(shape_0.x + uint(y_5) * _S48 + uint(x_5))));
            *reads_0 = *reads_0 + 1U;

#line 653
            int x_6 = x_5 + int(1);

#line 653
            nearest_0 = _S51;

#line 653
            x_5 = x_6;

#line 653
        }

#line 652
        y_5 = y_5 + int(1);

#line 652
    }

#line 658
    return nearest_0;
}




bool gpScreenRunEmpty_0(float3 origin_2, float3 direction_3, float step_1, uint index_0, uint run_0, const GiScreenStep_0 thread* here_0, uint width_2, uint height_2, uint thread* reads_1, KernelContext_0 thread* kernelContext_11)
{

#line 665
    GiScreenStep_0 _S52 = gpScreenStep_0(origin_2 + direction_3 * float3((step_1 * float(index_0 + run_0))) , width_2, height_2, kernelContext_11);

#line 665
    bool inside_0;

#line 670
    if(!_S52.outside_0)
    {

#line 670
        inside_0 = (_S52.x_3) >= 1.0;

#line 670
    }
    else
    {

#line 670
        inside_0 = false;

#line 670
    }

#line 670
    if(inside_0)
    {

#line 670
        inside_0 = (_S52.y_3) >= 1.0;

#line 670
    }
    else
    {

#line 670
        inside_0 = false;

#line 670
    }

#line 670
    if(inside_0)
    {

#line 670
        inside_0 = (_S52.x_3) < (float(width_2) - 1.0);

#line 670
    }
    else
    {

#line 670
        inside_0 = false;

#line 670
    }
    if(inside_0)
    {

#line 671
        inside_0 = (_S52.y_3) < (float(height_2) - 1.0);

#line 671
    }
    else
    {

#line 671
        inside_0 = false;

#line 671
    }

#line 671
    if(inside_0)
    {

)cy_msl"
    R"cy_msl(#line 671
        inside_0 = (here_0->x_3) >= 1.0;

#line 671
    }
    else
    {

#line 671
        inside_0 = false;

#line 671
    }
    if(inside_0)
    {

#line 672
        inside_0 = (here_0->y_3) >= 1.0;

#line 672
    }
    else
    {

#line 672
        inside_0 = false;

#line 672
    }

#line 672
    if(inside_0)
    {

#line 672
        inside_0 = (here_0->x_3) < (float(width_2) - 1.0);

#line 672
    }
    else
    {

#line 672
        inside_0 = false;

#line 672
    }

#line 672
    if(inside_0)
    {

#line 672
        inside_0 = (here_0->y_3) < (float(height_2) - 1.0);

#line 672
    }
    else
    {

#line 672
        inside_0 = false;

#line 672
    }

    if(!inside_0)
    {

#line 675
        return false;
    }

#line 675
    float _S53 = gpNearestDepth_0(max(int(floor(min(here_0->x_3, _S52.x_3))) - int(1), int(0)), max(int(floor(min(here_0->y_3, _S52.y_3))) - int(1), int(0)), min(int(floor(max(here_0->x_3, _S52.x_3))) + int(1), int(width_2) - int(1)), min(int(floor(max(here_0->y_3, _S52.y_3))) + int(1), int(height_2) - int(1)), reads_1, kernelContext_11);

#line 686
    return (max(here_0->depth_0, _S52.depth_0)) < (_S53 - abs(_S53) * 0.00009999999747379 - 0.00009999999747379);
}


#line 706
GiTierSample_0 gpScreenTrace_0(float3 origin_3, float3 direction_4, float maxDistance_1, bool accelerated_0, uint thread* reads_2, KernelContext_0 thread* kernelContext_12)
{
    thread GiTierSample_0 sample_2 = gpTierSample_0(5U);
    uint4 screen_0 = kernelContext_12->giProbes_0->constants_0[14U];
    uint width_3 = screen_0.x;
    uint height_3 = screen_0.y;

#line 711
    bool _S54;
    if(width_3 == 0U)
    {

#line 712
        _S54 = true;

#line 712
    }
    else
    {

#line 712
        _S54 = height_3 == 0U;

#line 712
    }

#line 712
    if(_S54)
    {

#line 713
        return sample_2;
    }
    float _S55 = (as_type<float>((screen_0.w)));
    uint _S56 = max(1U, screen_0.z);
    float _S57 = maxDistance_1 / float(_S56);

#line 717
    uint run_1 = 8U;

#line 717
    uint index_1 = 1U;

#line 722
    for(;;)
    {

#line 722
        if(index_1 <= _S56)
        {
        }
        else
        {

#line 722
            break;
        }

#line 723
        float t_2 = _S57 * float(index_1);

#line 723
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

#line 737
        float gap_0;

#line 737
        if(_S59)
        {

#line 737
            gap_0 = bufferDepth_0 - _S58.depth_0;

#line 737
        }
        else
        {

#line 737
            gap_0 = 1.00000001504746622e+30;

#line 737
        }
        if(_S59)
        {

#line 739
            float behind_0 = _S58.depth_0 - bufferDepth_0;
            if(behind_0 > 0.0)
            {

#line 741
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

#line 738
        }

#line 755
        if(accelerated_0)
        {

#line 755
            _S54 = (index_1 + 1U) < _S56;

#line 755
        }
        else
        {

#line 755
            _S54 = false;

#line 755
        }

#line 755
        bool _S60;

#line 755
        if(_S54)
        {

#line 755
            _S60 = gap_0 > (_S55 * 2.0);

#line 755
        }
        else
        {

#line 755
            _S60 = false;

#line 755
        }

#line 755
        uint skip_0;

#line 755
        if(_S60)
        {

#line 756
            uint _S61 = min(run_1, _S56 - index_1);

#line 756
            thread GiScreenStep_0 _S62 = _S58;

#line 756
            bool _S63 = gpScreenRunEmpty_0(origin_3, direction_4, _S57, index_1, _S61, &_S62, width_3, height_3, reads_2, kernelContext_12);

#line 756
            uint run_2;
            if(_S63)
            {

                uint _S64 = min(run_1 * 2U, 32U);

#line 760
                skip_0 = _S61;

#line 760
                run_2 = _S64;

#line 757
            }
            else
            {


                uint _S65 = max(run_1 / 2U, 2U);

#line 762
                skip_0 = 0U;

#line 762
                run_2 = _S65;

#line 757
            }

#line 757
            run_1 = run_2;

#line 755
        }
        else
        {

#line 755
            skip_0 = 0U;

)cy_msl"
    R"cy_msl(#line 755
        }

#line 755
        index_1 = index_1 + (1U + skip_0);

#line 722
    }

#line 768
    (&sample_2)->confidence_0 = 0.20000000298023224;
    (&sample_2)->hitDistance_0 = -1.0;
    return sample_2;
}


#line 823
uint gpTicket_0(uint counter_1, KernelContext_0 thread* kernelContext_13)
{
    uint count_1 = simd_sum(1U);
    uint rank_0 = simd_prefix_exclusive_sum(1U);

    bool _S66 = simd_is_first();

#line 828
    uint base_0;

#line 828
    if(_S66)
    {

#line 829
        uint _S67 = atomic_fetch_add_explicit(((atomic_uint device*)(kernelContext_13->giProbes_0->counters_0+counter_1)), count_1, memory_order_relaxed);

#line 829
        base_0 = _S67;

#line 828
    }
    else
    {

#line 828
        base_0 = 0U;

#line 828
    }


    uint _S68 = simd_broadcast_first(base_0);

#line 831
    return _S68 + rank_0;
}


#line 838
GiTierSample_0 gpTieredTrace_0(float3 origin_4, float3 direction_5, float maxDistance_2, bool screenPresent_0, uint thread* reads_3, KernelContext_0 thread* kernelContext_14)
{
    float3 unit_1 = giNormalisedOr_0(direction_5, float3(0.0, 0.0, -1.0));
    float4 hzb_0 = (as_type<float4>((kernelContext_14->giProbes_0->constants_0[15U])));
    float _S69 = min(maxDistance_2, hzb_0.z);
    float threshold_0 = hzb_0.w;
    bool accelerated_1 = (kernelContext_14->giProbes_0->constants_0[15U].y) != 0U;

    GiTierSample_0 _S70 = gpTierSample_0(0U);

#line 846
    GiTierSample_0 screen_1;
    if(screenPresent_0)
    {

#line 847
        gpCount_0(4U, 1U, kernelContext_14);

#line 847
        GiTierSample_0 _S71 = gpScreenTrace_0(origin_4, unit_1, _S69, accelerated_1, reads_3, kernelContext_14);


        if((_S71.confidence_0) >= threshold_0)
        {

#line 851
            return _S71;
        }

#line 851
        screen_1 = _S71;

#line 847
    }
    else
    {

#line 847
        screen_1 = _S70;

#line 847
    }

#line 858
    uint4 budget_0 = kernelContext_14->giProbes_0->constants_0[24U];

#line 858
    uint limit_0;
    if(screenPresent_0)
    {

#line 859
        limit_0 = min(budget_0.x, budget_0.y);

#line 859
    }
    else
    {

#line 859
        limit_0 = budget_0.x;

#line 859
    }

#line 859
    uint _S72 = gpTicket_0(0U, kernelContext_14);

    if(_S72 >= limit_0)
    {

#line 861
        gpCount_0(2U, 1U, kernelContext_14);

        return screen_1;
    }
    if(screenPresent_0)
    {

#line 865
        gpCount_0(1U, 1U, kernelContext_14);

#line 865
    }


    if((GiHardwareTier_source_0()) == 7U)
    {

#line 868
        limit_0 = 6U;

#line 868
    }
    else
    {

#line 868
        limit_0 = 5U;

#line 868
    }

#line 868
    gpCount_0(limit_0, 1U, kernelContext_14);

#line 868
    GiTierSample_0 _S73 = gpWorldTrace_0(origin_4, unit_1, _S69, kernelContext_14);

    if((_S73.source_0) == 9U)
    {

#line 870
        limit_0 = 1U;

#line 870
    }
    else
    {

#line 870
        limit_0 = 0U;

#line 870
    }

#line 870
    gpCount_0(7U, limit_0, kernelContext_14);

#line 870
    bool _S74;
    if(!screenPresent_0)
    {

#line 871
        _S74 = true;

#line 871
    }
    else
    {

#line 871
        _S74 = (screen_1.confidence_0) <= 0.0;

#line 871
    }

#line 871
    if(_S74)
    {

#line 872
        return _S73;
    }
    thread GiTierSample_0 blended_0;
    (&blended_0)->radiance_0 = _S73.radiance_0 + (screen_1.radiance_0 - _S73.radiance_0) * float3(screen_1.confidence_0) ;
    (&blended_0)->confidence_0 = max(screen_1.confidence_0, _S73.confidence_0);
    bool _S75 = (screen_1.confidence_0) >= (_S73.confidence_0);

#line 877
    if(_S75)
    {

#line 877
        limit_0 = screen_1.source_0;

#line 877
    }
    else
    {

#line 877
        limit_0 = _S73.source_0;

#line 877
    }

#line 877
    (&blended_0)->source_0 = limit_0;

#line 877
    float _S76;

    if(_S75)
    {

#line 879
        _S76 = screen_1.hitDistance_0;

#line 879
    }
    else
    {

#line 879
        _S76 = _S73.hitDistance_0;

#line 879
    }

#line 878
    (&blended_0)->hitDistance_0 = _S76;

    return blended_0;
}

void gpWriteTierResult_0(uint ray_1, const GiTierSample_0 thread* sample_3, uint reads_4, KernelContext_0 thread* kernelContext_15)
{

#line 884
    uint _S77 = ray_1 * 2U;

#line 884
    *(kernelContext_15->giProbes_0->tierResults_0+_S77) = float4(sample_3->radiance_0, sample_3->confidence_0);
    *(kernelContext_15->giProbes_0->tierResults_0+(_S77 + 1U)) = float4(sample_3->hitDistance_0, (as_type<float>((sample_3->source_0))), (as_type<float>((reads_4))), 0.0);

    return;
}


#line 44 "src/rendering/gi_gpu/shaders/gi_hardware.slang"
[[kernel]] void cyGiTraceTieredHardware(uint3 thread_0 [[thread_position_in_grid]], GiDispatch_0 constant* giDispatch_1 [[buffer(1)]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiHardwareSet_default_0 constant* giHardware_1 [[buffer(3)]])
{

#line 44
    thread KernelContext_0 kernelContext_16;

#line 44
    (&kernelContext_16)->giDispatch_0 = giDispatch_1;

#line 44
    (&kernelContext_16)->giProbes_0 = giProbes_1;

#line 44
    (&kernelContext_16)->giScene_0 = giScene_1;

#line 44
    (&kernelContext_16)->giHardware_0 = giHardware_1;
    uint ray_2 = thread_0.x;
    if(ray_2 >= (giDispatch_1->count_0))
    {

#line 47
        return;
    }
    uint _S78 = ray_2 * 2U;

)cy_msl"
    R"cy_msl(#line 49
    float4 origin_5 = (&kernelContext_16)->giProbes_0->tierRays_0[_S78];
    float4 direction_6 = (&kernelContext_16)->giProbes_0->tierRays_0[_S78 + 1U];
    bool screenPresent_1 = ((&kernelContext_16)->giProbes_0->constants_0[14U].x) != 0U;
    thread uint reads_5 = 0U;

#line 52
    GiTierSample_0 _S79 = gpTieredTrace_0(origin_5.xyz, direction_6.xyz, origin_5.w, screenPresent_1, &reads_5, &kernelContext_16);

#line 52
    gpCount_0(3U, reads_5, &kernelContext_16);

#line 52
    thread GiTierSample_0 _S80 = _S79;

#line 52
    gpWriteTierResult_0(ray_2, &_S80, reads_5, &kernelContext_16);

#line 57
    return;
}

)cy_msl";

/// cyGiUnpackSurfaces.metal, 4089 bytes.
inline constexpr char kGiUnpackSurfacesMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

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


#line 25 "src/rendering/gi_gpu/shaders/gi_surfaces.slang"
struct GiSurfaceSet_default_0
{
    uint4 device* constants_0;
    depth2d<float, access::sample> depth_0;
    texture2d<float, access::sample> normals_0;
    float4 device* surfaces_0;
    float4 device* guidance_0;
};


#line 25
struct KernelContext_0
{
    GiSurfaceSet_default_0 constant* giSurfaces_0;
};


#line 39
[[kernel]] void cyGiUnpackSurfaces(uint3 thread_0 [[thread_position_in_grid]], GiSurfaceSet_default_0 constant* giSurfaces_1 [[buffer(0)]])
{

#line 39
    thread KernelContext_0 kernelContext_0;

#line 39
    (&kernelContext_0)->giSurfaces_0 = giSurfaces_1;
    uint4 full_0 = giSurfaces_1->constants_0[36U];
    uint _S7 = thread_0.x;

#line 41
    uint _S8 = full_0.x;

#line 41
    bool onGrid_0;

#line 41
    if(_S7 >= _S8)
    {

#line 41
        onGrid_0 = true;

#line 41
    }
    else
    {

#line 41
        onGrid_0 = (thread_0.y) >= (full_0.y);

#line 41
    }

#line 41
    if(onGrid_0)
    {

#line 42
        return;
    }
    uint4 counts_0 = giSurfaces_1->constants_0[0U];
    uint _S9 = max(full_0.z, 1U);
    uint _S10 = thread_0.y;

#line 46
    uint pixel_0 = _S10 * _S8 + _S7;
    uint _S11 = _S7 % _S9;

#line 47
    if(_S11 == 0U)
    {

#line 47
        uint _S12 = _S10 % _S9;

#line 47
        onGrid_0 = _S12 == 0U;

#line 47
    }
    else
    {

#line 47
        onGrid_0 = false;

#line 47
    }
    uint _S13 = _S10 / _S9;

#line 48
    uint _S14 = _S13 * counts_0.x;

#line 48
    uint _S15 = _S7 / _S9;
    uint record_0 = (counts_0.w * counts_0.z + (_S14 + _S15)) * 2U;
    uint2 _S16 = thread_0.xy;

#line 50
    int3 _S17 = int3(int2(_S16), int(0));

#line 50
    float depth_1 = (((&kernelContext_0)->giSurfaces_0->depth_0).read(vec<uint,2>(((_S17)).xy), uint(((_S17)).z)));
    if(depth_1 <= 0.0)
    {

#line 52
        *((&kernelContext_0)->giSurfaces_0->guidance_0+pixel_0) = float4(0.0, 1.0, 0.0, 0.0);
        if(onGrid_0)
        {

#line 54
            *((&kernelContext_0)->giSurfaces_0->surfaces_0+record_0) = float4(0.0, 0.0, 0.0, 0.0);
            *((&kernelContext_0)->giSurfaces_0->surfaces_0+(record_0 + 1U)) = float4(0.0, 1.0, 0.0, 1.0);

#line 53
        }



        return;
    }
    float4 projection_0 = (as_type<float4>((giSurfaces_1->constants_0[14U])));
    float viewDepth_0 = projection_0.w / (depth_1 + projection_0.z);
    float4 packed_0 = (((&kernelContext_0)->giSurfaces_0->normals_0).read(vec<uint,2>(((_S17)).xy), uint(((_S17)).z)));
    float3 normal_1 = decodeOctahedral_0(packed_0.xy);
    *((&kernelContext_0)->giSurfaces_0->guidance_0+pixel_0) = float4(normal_1, viewDepth_0);
    if(!onGrid_0)
    {

#line 65
        return;
    }

    float2 uv_0 = (float2(_S16) + float2(0.5)  + (as_type<float2>((giSurfaces_1->constants_0[19U].xy)))) / float2(float(_S8), float(full_0.y));


    float3 view_0 = float3((uv_0.x * 2.0 - 1.0) * viewDepth_0 / projection_0.x, (1.0 - uv_0.y * 2.0) * viewDepth_0 / projection_0.y, - viewDepth_0);

#line 77
    *((&kernelContext_0)->giSurfaces_0->surfaces_0+record_0) = float4(float3(dot((as_type<float3>((giSurfaces_1->constants_0[15U].xyz))), view_0), dot((as_type<float3>((giSurfaces_1->constants_0[16U].xyz))), view_0), dot((as_type<float3>((giSurfaces_1->constants_0[17U].xyz))), view_0)) + (as_type<float3>((giSurfaces_1->constants_0[1U].xyz))), viewDepth_0);
    *((&kernelContext_0)->giSurfaces_0->surfaces_0+(record_0 + 1U)) = float4(normal_1, packed_0.z);
    return;
}

)cy_msl";

/// cyGiResolve.metal, 70084 bytes.
inline constexpr auto kGiResolveMslText = detail::join_msl(
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 118 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
struct GrSample_0
{
    float3 radiance_0;
    float confidence_0;
    uint source_0;
};


#line 53
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


#line 78 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 66 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
struct GiResolveSet_default_0
{
    uint4 device* constants_2;
    float4 device* surfaces_0;
    float4 device* noisy_0;
    float4 device* probes_1;
    uint device* slots_0;
};


#line 66
struct KernelContext_0
{
    GiProbeSet_default_0 constant* giProbes_0;
    GiSceneSet_default_0 constant* giScene_0;
    GiResolveSet_default_0 constant* giResolve_0;
};


#line 325 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
uint GiSoftwareTier_source_0()
{

#line 326
    return 6U;
}


#line 123 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_0)
{

#line 124
    return (as_type<float>((kernelContext_0->giScene_0->constants_1[9U].y)));
}


#line 281 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiWorldHit_0
{
    bool hit_0;
    float t_0;
    float3 position_0;
    float3 normal_0;
    float confidence_1;
};


#line 215 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiTraceHit_0
{
    bool hit_1;
    bool exhausted_0;
    float t_1;
    float closest_0;
    float3 position_1;
    float3 normal_1;
};


#line 115
uint giLevelCount_0(KernelContext_0 thread* kernelContext_1)
{

#line 116
    return kernelContext_1->giScene_0->constants_1[0U].x;
}


#line 115
uint giLevelCount_1(KernelContext_0 thread* kernelContext_2)
{

#line 116
    return kernelContext_2->giScene_0->constants_1[0U].x;
}

uint giWindowBricks_0(KernelContext_0 thread* kernelContext_3)
{

#line 120
    return kernelContext_3->giScene_0->constants_1[0U].y;
}


#line 167
int giWrap_0(int value_0, int modulus_0)
{

#line 168
    uint _S1 = uint(modulus_0);
    uint _S2 = (uint(value_0) + (_S1 << 20U)) % _S1;

#line 169
    return int(_S2);
}


#line 95
float giLerp_0(float a_0, float b_0, float t_2)
{

#line 96
    return a_0 + (b_0 - a_0) * t_2;
}


#line 128
float giTrilinear_0(uint slot_0, float fx_0, float fy_0, float fz_0, KernelContext_0 thread* kernelContext_4)
{
    int x0_0 = clamp(int(floor(fx_0)), int(0), int(3));
    int y0_0 = clamp(int(floor(fy_0)), int(0), int(3));
    int z0_0 = clamp(int(floor(fz_0)), int(0), int(3));



    float tx_0 = clamp(fx_0 - float(x0_0), 0.0, 1.0);
    float ty_0 = clamp(fy_0 - float(y0_0), 0.0, 1.0);

    uint base_0 = slot_0 * 125U;

    uint _S3 = uint(z0_0) * 5U;

#line 141
    uint _S4 = uint(y0_0);

#line 141
    uint _S5 = base_0 + (_S3 + _S4) * 5U;

#line 141
    uint _S6 = uint(x0_0);
    uint _S7 = uint(x0_0 + int(1));
    uint _S8 = uint(y0_0 + int(1));

#line 143
    uint _S9 = base_0 + (_S3 + _S8) * 5U;

    uint _S10 = uint(z0_0 + int(1)) * 5U;

#line 145
    uint _S11 = base_0 + (_S10 + _S4) * 5U;

    uint _S12 = base_0 + (_S10 + _S8) * 5U;

#line 153
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_4->giScene_0->bricks_0[_S5 + _S6], kernelContext_4->giScene_0->bricks_0[_S5 + _S7], tx_0), giLerp_0(kernelContext_4->giScene_0->bricks_0[_S9 + _S6], kernelContext_4->giScene_0->bricks_0[_S9 + _S7], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_4->giScene_0->bricks_0[_S11 + _S6], kernelContext_4->giScene_0->bricks_0[_S11 + _S7], tx_0), giLerp_0(kernelContext_4->giScene_0->bricks_0[_S12 + _S6], kernelContext_4->giScene_0->bricks_0[_S12 + _S7], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 175
float giSampleLevel_0(uint level_0, float3 p_0, float thread* farDistance_0, KernelContext_0 thread* kernelContext_5)
{

#line 176
    uint _S13 = 16U + level_0 * 2U;

#line 176
    float4 shape_0 = (as_type<float4>((kernelContext_5->giScene_0->constants_1[_S13])));
    int4 origin_0 = (as_type<int4>((kernelContext_5->giScene_0->constants_1[_S13 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_0.x / brick_0));
    int by_0 = int(floor(p_0.y / brick_0));
    int bz_0 = int(floor(p_0.z / brick_0));

#line 183
    uint _S14 = giWindowBricks_0(kernelContext_5);
    int dim_0 = int(_S14);
    int _S15 = origin_0.x;

#line 185
    bool _S16;

#line 185
    if(bx_0 < _S15)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = by_0 < (origin_0.y);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = bz_0 < (origin_0.z);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = bx_0 >= (_S15 + dim_0);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = by_0 >= (origin_0.y + dim_0);

#line 185
    }
    if(_S16)
    {

#line 186
        _S16 = true;

#line 186
    }
    else
    {

#line 186
        _S16 = bz_0 >= (origin_0.z + dim_0);

#line 186
    }

#line 185
    if(_S16)
    {
        return *farDistance_0;
    }
    int _S17 = giWrap_0(bz_0, dim_0);

#line 189
    uint _S18 = uint(dim_0);

#line 189
    uint _S19 = uint(_S17) * _S18;

#line 189
    int _S20 = giWrap_0(by_0, dim_0);

#line 189
    uint _S21 = (_S19 + uint(_S20)) * _S18;
    int _S22 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_5->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S21 + uint(_S22))];
    if(slot_1 == 4294967295U)
    {

#line 193
        return *farDistance_0;
    }

    float3 local_0 = (p_0 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 196
    float _S23 = giTrilinear_0(slot_1, local_0.x, local_0.y, local_0.z, kernelContext_5);
    return _S23;
}


float giDistance_0(float3 p_1, KernelContext_0 thread* kernelContext_6)
{

#line 201
    uint _S24 = giLevelCount_1(kernelContext_6);

#line 201
    float answer_0 = 1.0e+06;

#line 201
    uint level_1 = 0U;


    for(;;)
    {

#line 204
        if(level_1 < _S24)
        {
        }
        else
        {

#line 204
            break;
        }

#line 205
        thread float farDistance_1 = 0.0;

#line 205
        float _S25 = giSampleLevel_0(level_1, p_1, &farDistance_1, kernelContext_6);

        float _S26 = min(answer_0, _S25);
        if(_S25 < farDistance_1)
        {

#line 209
            return _S25;
        }

#line 204
        uint level_2 = level_1 + 1U;

#line 204
        answer_0 = _S26;

#line 204
        level_1 = level_2;

#line 204
    }

#line 212
    return answer_0;
}


#line 99
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 100
    float squared_0 = dot(v_0, v_0);

#line 100
    float3 _S27;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 101
        _S27 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 101
    }
    else
    {

#line 101
        _S27 = fallback_0;

#line 101
    }

#line 101
    return _S27;
}


#line 225
GiTraceHit_0 giSphereTrace_0(float3 origin_1, float3 direction_0, float maxDistance_0, float tMin_0, KernelContext_0 thread* kernelContext_7)
{

#line 226
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_1 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_1 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_1 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_1 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S28 = giLevelCount_0(kernelContext_7);
    if(_S28 == 0U)
    {

#line 234
        return result_0;
    }

#line 234
    float _S29 = giFinestVoxel_0(kernelContext_7);


    float surface_0 = _S29 * 0.5;
    float _S30 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 239
    recent_0[int(0)] = 1.0e+06;

#line 239
    recent_0[int(1)] = 1.0e+06;

#line 239
    recent_0[int(2)] = 1.0e+06;

#line 239
    float closest_1 = 1.0e+06;

#line 239
    uint recentSlot_0 = 0U;

#line 239
    uint step_0 = 0U;

#line 239
    float t_3 = _S30;


    for(;;)
    {

#line 242
        if(step_0 < 192U)
        {
        }
        else
        {

#line 242
            break;
        }

#line 243
        if(t_3 > maxDistance_0)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_2 = origin_1 + direction_0 * float3(t_3) ;

#line 248
        float _S31 = giDistance_0(position_2, kernelContext_7);

        if(_S31 < surface_0)
        {

#line 251
            (&result_0)->hit_1 = true;
            (&result_0)->t_1 = t_3;
            (&result_0)->position_1 = position_2;


            float3 _S32 = float3(_S29, 0.0, 0.0);

#line 256
            float _S33 = giDistance_0(position_2 + _S32, kernelContext_7);

#line 256
            float _S34 = giDistance_0(position_2 - _S32, kernelContext_7);

#line 256
            float _S35 = _S33 - _S34;

            float3 _S36 = float3(0.0, _S29, 0.0);

#line 258
            float _S37 = giDistance_0(position_2 + _S36, kernelContext_7);

#line 258
            float _S38 = giDistance_0(position_2 - _S36, kernelContext_7);

#line 258
            float _S39 = _S37 - _S38;

            float3 _S40 = float3(0.0, 0.0, _S29);

#line 260
            float _S41 = giDistance_0(position_2 + _S40, kernelContext_7);

#line 260
            float _S42 = giDistance_0(position_2 - _S40, kernelContext_7);

            (&result_0)->normal_1 = giNormalisedOr_0(float3(_S35, _S39, _S41 - _S42), - direction_0);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S43 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S31;
        uint _S44 = (recentSlot_0 + 1U) % 3U;
        float t_4 = t_3 + max(_S31, surface_0);

#line 242
        uint step_1 = step_0 + 1U;

#line 242
        closest_1 = _S43;

#line 242
        recentSlot_0 = _S44;

#line 242
        step_0 = step_1;

#line 242
        t_3 = t_4;

#line 242
    }

#line 271
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

#line 107
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
GiWorldHit_0 GiSoftwareTier_trace_0(float3 origin_2, float3 direction_1, float maxDistance_1, KernelContext_0 thread* kernelContext_8)
{

#line 309
    GiTraceHit_0 _S46 = giSphereTrace_0(origin_2, direction_1, maxDistance_1, 0.0, kernelContext_8);

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



    (&hit_2)->confidence_1 = (as_type<float4>((kernelContext_8->giProbes_0->constants_0[25U]))).y * gpClamp_0(_S46.closest_0 / max((as_type<float>((kernelContext_8->giScene_0->constants_1[9U].z))), 0.00009999999747379), 0.25, 1.0);
    return hit_2;
}


#line 124 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
GrSample_0 grSample_0(float3 radiance_1, float confidence_2, uint source_1)
{

#line 125
    thread GrSample_0 sample_0;
    (&sample_0)->radiance_0 = radiance_1;
    (&sample_0)->confidence_0 = confidence_2;
    (&sample_0)->source_0 = source_1;
    return sample_0;
}


#line 689 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiTierSample_0
{
    float3 radiance_2;
    float confidence_3;
    float hitDistance_0;
    uint source_2;
};


#line 696
GiTierSample_0 gpTierSample_0(uint source_3)
{

#line 697
    thread GiTierSample_0 sample_1;
    (&sample_1)->radiance_2 = float3(0.0, 0.0, 0.0);
    (&sample_1)->confidence_3 = 0.0;
    (&sample_1)->hitDistance_0 = -1.0;
    (&sample_1)->source_2 = source_3;
    return sample_1;
}


#line 121
float3 gpSky_0(float3 direction_2, KernelContext_0 thread* kernelContext_9)
{

#line 122
    float4 zenith_0 = (as_type<float4>((kernelContext_9->giProbes_0->constants_0[2U])));
    float3 horizon_0 = (as_type<float3>((kernelContext_9->giProbes_0->constants_0[3U].xyz)));
    float3 ground_0 = (as_type<float3>((kernelContext_9->giProbes_0->constants_0[4U].xyz)));

    float _S47 = giNormalisedOr_0(direction_2, float3(0.0, 1.0, 0.0)).y;

#line 126
    if(_S47 < 0.0)
    {

#line 127
        return ground_0 * float3(zenith_0.w) ;
    }

    return (horizon_0 + (zenith_0.xyz - horizon_0) * float3(gpClamp_0(pow(_S47, 0.44999998807907104), 0.0, 1.0)) ) * float3(zenith_0.w) ;
}


#line 471 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_2, float cell_0)
{

#line 472
    return int(floor(value_2 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_3, float3 normal_2, KernelContext_0 thread* kernelContext_10)
{

#line 482
    uint4 grid_0 = kernelContext_10->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 485
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S48 = radius_0 * radius_0;
    float _S49 = position_3.x;

#line 490
    int _S50 = giCell_0(_S49 - radius_0, cell_1);

#line 490
    float _S51 = position_3.y;

#line 490
    int _S52 = giCell_0(_S51 - radius_0, cell_1);
    float _S53 = position_3.z;

#line 491
    int _S54 = giCell_0(_S53 - radius_0, cell_1);
    int _S55 = giCell_0(_S49 + radius_0, cell_1);

#line 492
    int _S56 = giCell_0(_S51 + radius_0, cell_1);
    int _S57 = giCell_0(_S53 + radius_0, cell_1);

#line 493
    float bestScore_0 = 1.0e+09;

#line 493
    float bestAlignment_0 = 0.0;

#line 493
    uint best_0 = 4294967295U;

#line 493
    int z_1 = _S54;



    for(;;)
    {

#line 497
        if(z_1 <= _S57)
        {
        }
        else
        {

#line 497
            break;
        }

#line 497
        int y_1 = _S52;
        for(;;)
        {

#line 498
            if(y_1 <= _S56)
            {
            }
            else
            {

)cy_msl",
    R"cy_msl(#line 498
                break;
            }

#line 498
            float bestScore_1 = bestScore_0;

#line 498
            float bestAlignment_1 = bestAlignment_0;

#line 498
            uint best_1 = best_0;

#line 498
            int x_1 = _S50;
            for(;;)
            {

#line 499
                if(x_1 <= _S55)
                {
                }
                else
                {

#line 499
                    break;
                }
                uint _S58 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 501
                uint first_0 = kernelContext_10->giScene_0->gridRanges_0[_S58];
                uint _S59 = kernelContext_10->giScene_0->gridRanges_0[_S58 + 1U];

#line 502
                float bestScore_2 = bestScore_1;

#line 502
                float bestAlignment_2 = bestAlignment_1;

#line 502
                uint best_2 = best_1;

#line 502
                uint item_0 = first_0;
                for(;;)
                {

#line 503
                    if(item_0 < (first_0 + _S59))
                    {
                    }
                    else
                    {

#line 503
                        break;
                    }

#line 504
                    uint handle_0 = kernelContext_10->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_10->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 506
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }



                    uint _S60 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_10->giScene_0->cards_0[_S60 + 1U].xyz, normal_2);
                    if(alignment_0 <= 0.25)
                    {

#line 511
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 513
                    float3 offset_0 = kernelContext_10->giScene_0->cards_0[_S60].xyz - position_3;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S48)
                    {

#line 516
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 519
                    float score_0 = squared_1 / alignment_0;

#line 519
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 520
                        better_0 = score_0 < bestScore_2;

#line 520
                    }
                    else
                    {

#line 521
                        if(alignment_0 != bestAlignment_2)
                        {

#line 521
                            better_0 = alignment_0 > bestAlignment_2;

#line 521
                        }
                        else
                        {

#line 521
                            better_0 = handle_0 < best_2;

#line 521
                        }

#line 520
                    }

#line 520
                    float bestScore_3;

#line 520
                    float bestAlignment_3;

#line 520
                    uint best_3;


                    if(better_0)
                    {

#line 523
                        bestScore_3 = score_0;

#line 523
                        bestAlignment_3 = alignment_0;

#line 523
                        best_3 = handle_0;

#line 523
                    }
                    else
                    {

#line 523
                        bestScore_3 = bestScore_2;

#line 523
                        bestAlignment_3 = bestAlignment_2;

#line 523
                        best_3 = best_2;

#line 523
                    }

#line 523
                    bestScore_2 = bestScore_3;

#line 523
                    bestAlignment_2 = bestAlignment_3;

#line 523
                    best_2 = best_3;

#line 503
                    item_0 = item_0 + 1U;

#line 503
                }

#line 499
                int x_2 = x_1 + int(1);

#line 499
                bestScore_1 = bestScore_2;

#line 499
                bestAlignment_1 = bestAlignment_2;

#line 499
                best_1 = best_2;

#line 499
                x_1 = x_2;

#line 499
            }

#line 498
            int y_2 = y_1 + int(1);

#line 498
            bestScore_0 = bestScore_1;

#line 498
            bestAlignment_0 = bestAlignment_1;

#line 498
            best_0 = best_1;

#line 498
            y_1 = y_2;

#line 498
        }

#line 497
        z_1 = z_1 + int(1);

#line 497
    }

#line 532
    return best_0;
}


#line 776 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpWorldRadiance_0(const GiWorldHit_0 thread* hit_3, float3 direction_3, uint source_4, uint cacheFrame_0, KernelContext_0 thread* kernelContext_11)
{

#line 777
    thread GiTierSample_0 sample_2 = gpTierSample_0(source_4);
    if(!hit_3->hit_0)
    {

#line 779
        (&sample_2)->source_2 = 9U;

#line 779
        float3 _S61 = gpSky_0(direction_3, kernelContext_11);
        (&sample_2)->radiance_2 = _S61;
        (&sample_2)->confidence_3 = 1.0;
        (&sample_2)->hitDistance_0 = -1.0;
        return sample_2;
    }
    (&sample_2)->hitDistance_0 = hit_3->t_0;

#line 785
    uint _S62 = giFindCard_0(hit_3->position_0, hit_3->normal_0, kernelContext_11);

    if(_S62 != 4294967295U)
    {

#line 788
        uint _S63 = _S62 * 2U;

#line 788
        (&sample_2)->radiance_2 = (*(kernelContext_11->giScene_0->cardState_0+_S63)).xyz;
        uint last_0 = (as_type<uint>(((*(kernelContext_11->giScene_0->cardState_0+(_S63 + 1U))).w)));

#line 789
        float age_0;
        if(cacheFrame_0 > last_0)
        {

#line 790
            age_0 = float(cacheFrame_0 - last_0);

#line 790
        }
        else
        {

#line 790
            age_0 = 0.0;

#line 790
        }


        (&sample_2)->confidence_3 = hit_3->confidence_1 * (1.0 - gpClamp_0(age_0 / 240.0, 0.0, 1.0) * 0.60000002384185791);
        return sample_2;
    }
    (&sample_2)->confidence_3 = hit_3->confidence_1 * 0.20000000298023224;
    return sample_2;
}


#line 247 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
GrSample_0 grWorldRay_0(float3 position_4, float3 normal_3, float3 unit_0, float lift_0, float reach_0, KernelContext_0 thread* kernelContext_12)
{
    uint cacheFrame_1 = kernelContext_12->giProbes_0->constants_0[24U].w;
    uint _S64 = GiSoftwareTier_source_0();

#line 250
    float height_0;

#line 250
    if(_S64 == 7U)
    {

#line 250
        height_0 = lift_0;

#line 250
    }
    else
    {

#line 250
        float _S65 = giFinestVoxel_0(kernelContext_12);

#line 250
        height_0 = max(lift_0, _S65);

#line 250
    }

#line 250
    GiWorldHit_0 _S66 = GiSoftwareTier_trace_0(position_4 + normal_3 * float3(height_0) , unit_0, reach_0, kernelContext_12);

#line 250
    bool _S67;

    if(_S66.hit_0)
    {

#line 252
        _S67 = (dot(_S66.normal_0, unit_0)) > 0.0;

#line 252
    }
    else
    {

#line 252
        _S67 = false;

#line 252
    }

#line 252
    if(_S67)
    {

#line 253
        return grSample_0(float3(0.0, 0.0, 0.0), 0.0, 0U);
    }

#line 253
    thread GiWorldHit_0 _S68 = _S66;

#line 253
    GiTierSample_0 _S69 = gpWorldRadiance_0(&_S68, unit_0, _S64, cacheFrame_1, kernelContext_12);


    return grSample_0(_S69.radiance_2, _S69.confidence_3, _S69.source_2);
}


#line 133
struct GrCombine_0
{
    float3 total_0;
    float totalWeight_0;
    float confidence_4;
};


#line 139
GrCombine_0 grCombineBegin_0()
{

#line 140
    thread GrCombine_0 state_0;
    (&state_0)->total_0 = float3(0.0, 0.0, 0.0);
    (&state_0)->totalWeight_0 = 0.0;
    (&state_0)->confidence_4 = 0.0;
    return state_0;
}


#line 200
uint grReflectionRays_0(float roughness_0, KernelContext_0 thread* kernelContext_13)
{

#line 201
    uint4 words_0 = kernelContext_13->giResolve_0->constants_2[4U];
    float mirror_0 = (as_type<float>((words_0.x)));
    float cacheMin_0 = (as_type<float>((words_0.y)));
    uint _S70 = max(1U, words_0.z);
    if(roughness_0 <= mirror_0)
    {

#line 206
        return _S70;
    }
    if(roughness_0 < cacheMin_0)
    {

        float _S71 = float(_S70);


        return max(1U, uint(floor(_S71 + (1.0 - _S71) * gpClamp_0((roughness_0 - mirror_0) / max(cacheMin_0 - mirror_0, 0.00009999999747379), 0.0, 1.0) + 0.5)));
    }
    return 0U;
}


#line 73
uint grHash_0(uint pixel_0, uint frame_0, uint stream_0)
{

    uint hash_0 = ((pixel_0 * 2654435769U) ^ (frame_0 * 2246822507U)) ^ (stream_0 * 3266489909U);

    uint hash_1 = (hash_0 ^ (hash_0 >> 15U)) * 625341585U;

    return hash_1 ^ (hash_1 >> 13U);
}

float grUnit_0(uint hash_2)
{

#line 84
    return float(hash_2 & 16777215U) / 1.6777216e+07;
}


#line 109
bool grTraces_0(uint pixel_1, uint frame_1, float density_0)
{

#line 110
    if(density_0 >= 1.0)
    {

#line 111
        return true;
    }
    return (grUnit_0(grHash_0(pixel_1, frame_1, 5U))) < density_0;
}


#line 220
float3 grLobeDirection_0(float3 reflection_0, float3 normal_4, float roughness_1, uint index_0, uint count_0, uint rotation_0)
{
    float3 axis_0 = giNormalisedOr_0(reflection_0, normal_4);

#line 222
    bool _S72;
    if(count_0 <= 1U)
    {

#line 223
        _S72 = true;

#line 223
    }
    else
    {

#line 223
        _S72 = roughness_1 <= 0.0;

#line 223
    }

#line 223
    if(_S72)
    {

#line 224
        return axis_0;
    }

    float _S73 = float(index_0);

    float radius_1 = sqrt((_S73 + 0.5) / float(count_0)) * (gpClamp_0(roughness_1, 0.0, 1.0) * 0.69999998807907104);
    float angle_0 = 2.3999631404876709 * _S73 + float(rotation_0) * 0.6180340051651001;

#line 230
    float3 tangent_0;
    if((abs(axis_0.y)) < 0.99000000953674316)
    {

#line 231
        tangent_0 = cross(float3(0.0, 1.0, 0.0), axis_0);

#line 231
    }
    else
    {

#line 231
        tangent_0 = cross(float3(1.0, 0.0, 0.0), axis_0);

#line 231
    }

    float3 tangent_1 = giNormalisedOr_0(tangent_0, float3(1.0, 0.0, 0.0));



    float3 unit_1 = giNormalisedOr_0(axis_0 + tangent_1 * float3((radius_1 * cos(angle_0)))  + cross(axis_0, tangent_1) * float3((radius_1 * sin(angle_0))) , axis_0);
    float _S74 = dot(unit_1, normal_4);

#line 238
    if(_S74 < 0.0)
    {

#line 238
        tangent_0 = giNormalisedOr_0(unit_1 - normal_4 * float3((2.0 * _S74)) , axis_0);

#line 238
    }
    else
    {

#line 238
        tangent_0 = unit_1;

#line 238
    }

#line 238
    return tangent_0;
}


#line 263
GrSample_0 grGatherSample_0(const GrSample_0 thread* ray_0, const GrSample_0 thread* standIn_0, uint excluded_0)
{

#line 263
    uint _S75 = standIn_0->source_0;

#line 263
    bool hasStandIn_0;
    if((standIn_0->source_0) != 0U)
    {

#line 264
        hasStandIn_0 = (standIn_0->confidence_0) > 0.0;

#line 264
    }
    else
    {

#line 264
        hasStandIn_0 = false;

#line 264
    }

#line 264
    if(hasStandIn_0)
    {

#line 264
        hasStandIn_0 = (excluded_0 & (1U << _S75)) == 0U;

#line 264
    }
    else
    {

#line 264
        hasStandIn_0 = false;

#line 264
    }

    thread GrSample_0 sample_3 = *ray_0;
    if((ray_0->source_0) == 0U)
    {

#line 268
        if(!hasStandIn_0)
        {

#line 269
            return *ray_0;
        }
        (&sample_3)->source_0 = _S75;
        (&sample_3)->radiance_0 = standIn_0->radiance_0;

#line 267
    }
    else
    {



        if(hasStandIn_0)
        {
            (&sample_3)->radiance_0 = standIn_0->radiance_0 + (ray_0->radiance_0 - standIn_0->radiance_0) * float3(gpClamp_0(ray_0->confidence_0, 0.0, 1.0)) ;

#line 273
        }

#line 267
    }

#line 277
    (&sample_3)->confidence_0 = 1.0;
    return sample_3;
}


#line 147
void grCombineAdd_0(GrCombine_0 thread* state_1, const GrSample_0 thread* sample_4, uint excluded_1)
{

#line 147
    uint _S76 = sample_4->source_0;
    if((sample_4->source_0) == 0U)
    {

#line 149
        return;
    }
    if((excluded_1 & (1U << _S76)) != 0U)
    {

#line 152
        return;
    }
    float weight_0 = gpClamp_0(sample_4->confidence_0, 0.0, 1.0);
    if(weight_0 <= 0.0)
    {

#line 156
        return;
    }
    state_1->total_0 = state_1->total_0 + sample_4->radiance_0 * float3(weight_0) ;
    state_1->totalWeight_0 = state_1->totalWeight_0 + weight_0;
    state_1->confidence_4 = max(state_1->confidence_4, weight_0);
    return;
}


#line 163
float3 grCombineEnd_0(const GrCombine_0 thread* state_2)
{

#line 163
    float _S77 = state_2->totalWeight_0;

#line 163
    float3 _S78;
    if((state_2->totalWeight_0) > 0.0)
    {

#line 164
        _S78 = state_2->total_0 / float3(_S77) ;

#line 164
    }
    else
    {

#line 164
        _S78 = float3(0.0, 0.0, 0.0);

#line 164
    }

#line 164
    return _S78;
}


#line 431
float3 grResolveSpecular_0(uint pixel_2, float3 position_5, float3 normal_5, float roughness_2, float3 reflection_1, float4 cached_0, uint frame_2, KernelContext_0 thread* kernelContext_14)
{

    float4 rays_1 = (as_type<float4>((kernelContext_14->giResolve_0->constants_2[2U])));
    float density_1 = (as_type<float>((kernelContext_14->giResolve_0->constants_2[1U].w)));
    thread GrCombine_0 state_3 = grCombineBegin_0();


    GrSample_0 _S79 = grSample_0(float3(0.0, 0.0, 0.0), 0.0, 0U);
    float _S80 = cached_0.w;

#line 440
    GrSample_0 cache_0;

#line 440
    if(_S80 > 0.0)
    {

#line 440
        cache_0 = grSample_0(cached_0.xyz, _S80 * gpClamp_0(roughness_2, 0.15000000596046448, 1.0), 3U);

#line 440
    }
    else
    {

#line 440
        cache_0 = _S79;

#line 440
    }

#line 440
    uint _S81 = grReflectionRays_0(roughness_2, kernelContext_14);

#line 440
    bool _S82;



    if(_S81 > 0U)
    {

#line 444
        _S82 = grTraces_0(pixel_2, frame_2, density_1);

#line 444
    }
    else
    {

#line 444
        _S82 = false;

#line 444
    }

#line 444
    if(_S82)
    {

        uint _S83 = grHash_0(pixel_2, frame_2, 7U) % _S81;

#line 447
        GrSample_0 _S84 = grWorldRay_0(position_5, normal_5, giNormalisedOr_0(grLobeDirection_0(reflection_1, normal_5, roughness_2, _S83, _S81, frame_2), float3(0.0, 0.0, -1.0)), rays_1.y, rays_1.x, kernelContext_14);

#line 447
        thread GrSample_0 _S85 = _S84;

#line 447
        thread GrSample_0 _S86 = cache_0;

#line 447
        GrSample_0 _S87 = grGatherSample_0(&_S85, &_S86, 0U);

#line 447
        thread GrSample_0 _S88 = _S87;

#line 447
        grCombineAdd_0(&state_3, &_S88, 0U);

#line 444
    }

#line 454
    if((cache_0.confidence_0) > 0.0)
    {

#line 454
        thread GrSample_0 _S89 = cache_0;

#line 454
        grCombineAdd_0(&state_3, &_S89, 0U);

#line 454
    }

#line 454
    thread GrCombine_0 _S90 = state_3;

#line 454
    float3 _S91 = grCombineEnd_0(&_S90);


    return _S91 * float3((as_type<float>((kernelContext_14->giResolve_0->constants_2[5U].y)))) ;
}


#line 88
void grBasis_0(float3 normal_6, float3 thread* tangent_2, float3 thread* bitangent_0)
{

#line 89
    float _S92 = normal_6.z;

#line 89
    float sign_0;

#line 89
    if(_S92 >= 0.0)
    {

#line 89
        sign_0 = 1.0;

#line 89
    }
    else
    {

#line 89
        sign_0 = -1.0;

#line 89
    }
    float a_1 = -1.0 / (sign_0 + _S92);
    float _S93 = normal_6.x;

#line 91
    float _S94 = normal_6.y;

#line 91
    float b_1 = _S93 * _S94 * a_1;
    *tangent_2 = float3(1.0 + sign_0 * _S93 * _S93 * a_1, sign_0 * b_1, - sign_0 * _S93);
    *bitangent_0 = float3(b_1, sign_0 + _S94 * _S94 * a_1, - _S94);
    return;
}


#line 96
float3 grCosineDirection_0(float3 normal_7, float u1_0, float u2_0)
{

#line 97
    thread float3 tangent_3;
    thread float3 bitangent_1;
    grBasis_0(normal_7, &tangent_3, &bitangent_1);
    float radius_2 = sqrt(u1_0);
    float angle_1 = 6.28318548202514648 * u2_0;

    return giNormalisedOr_0(tangent_3 * float3((radius_2 * cos(angle_1)))  + bitangent_1 * float3((radius_2 * sin(angle_1)))  + normal_7 * float3(sqrt(max(0.0, 1.0 - u1_0))) , normal_7);
}


#line 168
float grFarFieldWeight_0(float distance_0, KernelContext_0 thread* kernelContext_15)
{

#line 169
)cy_msl",
    R"cy_msl(    float4 far_0 = (as_type<float4>((kernelContext_15->giResolve_0->constants_2[3U])));
    float begin_0 = far_0.x;
    if(begin_0 <= 0.0)
    {

#line 172
        return 1.0;
    }
    float _S95 = far_0.y;

#line 174
    float ratio_0;

#line 174
    if(_S95 > 0.0)
    {

#line 174
        ratio_0 = far_0.z / _S95;

#line 174
    }
    else
    {

#line 174
        ratio_0 = 8.0;

#line 174
    }
    float width_0 = begin_0 * gpClamp_0(ratio_0 / 100.0, 0.15000000596046448, 1.0);
    if(distance_0 <= begin_0)
    {

#line 177
        return 0.0;
    }
    float t_5 = gpClamp_0((distance_0 - begin_0) / width_0, 0.0, 1.0);
    return t_5 * t_5 * (3.0 - 2.0 * t_5);
}


#line 278 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giHemisphereDirection_0(float3 normal_8, uint index_1, uint count_1)
{
    float _S96 = float(index_1);
    float radius_3 = sqrt((_S96 + 0.5) / float(count_1));
    float angle_2 = 2.3999631404876709 * _S96;
    float x_3 = radius_3 * cos(angle_2);
    float y_3 = radius_3 * sin(angle_2);
    float z_2 = sqrt(max(0.0, 1.0 - x_3 * x_3 - y_3 * y_3));

#line 285
    float3 tangent_4;
    if((abs(normal_8.y)) < 0.99000000953674316)
    {

#line 286
        tangent_4 = cross(float3(0.0, 1.0, 0.0), normal_8);

#line 286
    }
    else
    {

#line 286
        tangent_4 = cross(float3(1.0, 0.0, 0.0), normal_8);

#line 286
    }

    float3 tangent_5 = giNormalisedOr_0(tangent_4, float3(1.0, 0.0, 0.0));

    return giNormalisedOr_0(tangent_5 * float3(x_3)  + cross(normal_8, tangent_5) * float3(y_3)  + normal_8 * float3(z_2) , normal_8);
}


#line 184 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float grSkyVisibilityRay_0(float3 position_6, float3 normal_9, uint index_2, uint count_2, KernelContext_0 thread* kernelContext_16)
{

#line 184
    bool _S97;
    if(count_2 == 0U)
    {

#line 185
        _S97 = true;

#line 185
    }
    else
    {

#line 185
        uint _S98 = giLevelCount_0(kernelContext_16);

#line 185
        _S97 = _S98 == 0U;

#line 185
    }

#line 185
    if(_S97)
    {

#line 186
        return 1.0;
    }
    float reach_1 = (as_type<float>((kernelContext_16->giResolve_0->constants_2[3U].w)));

#line 188
    float _S99 = giFinestVoxel_0(kernelContext_16);

#line 188
    GiTraceHit_0 _S100 = giSphereTrace_0(position_6 + normal_9 * float3((_S99 * 2.0)) , giHemisphereDirection_0(normal_9, index_2, count_2), reach_1, 0.0, kernelContext_16);

#line 193
    if(_S100.hit_1)
    {

#line 194
        return 0.0;
    }
    return gpClamp_0(_S100.closest_0 / (_S99 * 4.0), 0.0, 1.0);
}


#line 393
float3 grResolveDiffuse_0(uint pixel_3, float3 position_7, float3 normal_10, float distance_1, float4 cached_1, uint frame_3, KernelContext_0 thread* kernelContext_17)
{
    float4 rays_2 = (as_type<float4>((kernelContext_17->giResolve_0->constants_2[2U])));
    uint excluded_2 = kernelContext_17->giResolve_0->constants_2[2U].w;
    float density_2 = (as_type<float>((kernelContext_17->giResolve_0->constants_2[1U].w)));
    thread GrCombine_0 state_4 = grCombineBegin_0();

    GrSample_0 cache_1 = grSample_0(cached_1.xyz, cached_1.w, 3U);

#line 400
    thread GrSample_0 _S101 = cache_1;

#line 400
    grCombineAdd_0(&state_4, &_S101, excluded_2);


    if(grTraces_0(pixel_3, frame_3, density_2))
    {

#line 404
        uint hash_3 = grHash_0(pixel_3, frame_3, 6U);

#line 404
        GrSample_0 _S102 = grWorldRay_0(position_7, normal_10, giNormalisedOr_0(grCosineDirection_0(normal_10, grUnit_0(hash_3), grUnit_0(hash_3 * 656542357U)), float3(0.0, 0.0, -1.0)), rays_2.y, rays_2.x, kernelContext_17);

#line 404
        thread GrSample_0 _S103 = _S102;

#line 404
        thread GrSample_0 _S104 = cache_1;

#line 404
        GrSample_0 _S105 = grGatherSample_0(&_S103, &_S104, excluded_2);

#line 404
        thread GrSample_0 _S106 = _S105;

#line 404
        grCombineAdd_0(&state_4, &_S106, excluded_2);

#line 403
    }

#line 415
    thread GrSample_0 sky_0 = grSample_0(float3(0.0, 0.0, 0.0), 0.0, 9U);

#line 415
    float _S107 = grFarFieldWeight_0(distance_1, kernelContext_17);

    if(_S107 > 0.0)
    {
        uint count_3 = (as_type<uint>((rays_2.z)));

#line 419
        float openness_0;
        if(count_3 > 0U)
        {

#line 421
            uint _S108 = (frame_3 + pixel_3) % count_3;

#line 421
            float _S109 = grSkyVisibilityRay_0(position_7, normal_10, _S108, count_3, kernelContext_17);

#line 421
            openness_0 = _S109;

#line 420
        }
        else
        {

#line 420
            openness_0 = 1.0;

#line 420
        }

#line 420
        float3 _S110 = gpSky_0(normal_10, kernelContext_17);


        (&sky_0)->radiance_0 = _S110 * float3(openness_0) ;
        (&sky_0)->confidence_0 = gpClamp_0(_S107 * openness_0, 0.0, 1.0);

#line 417
    }

#line 417
    thread GrSample_0 _S111 = sky_0;

#line 417
    grCombineAdd_0(&state_4, &_S111, excluded_2);

#line 417
    thread GrCombine_0 _S112 = state_4;

#line 417
    float3 _S113 = grCombineEnd_0(&_S112);

#line 427
    return _S113 * float3((as_type<float>((kernelContext_17->giResolve_0->constants_2[5U].x)))) ;
}


#line 440 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
int gpFloorDiv_0(float value_3, float divisor_0)
{

#line 441
    return int(floor(value_3 / divisor_0));
}


#line 487
uint gpLayoutProbe_0(uint level_3, int3 cell_2, KernelContext_0 thread* kernelContext_18)
{

#line 488
    int3 centre_0 = (as_type<int3>((kernelContext_18->giProbes_0->constants_0[6U + level_3].xyz)));
    int half_0 = int(kernelContext_18->giProbes_0->constants_0[1U].w);
    int3 _S114 = int3(half_0, half_0, half_0);

#line 490
    bool _S115;

#line 490
    if(any(cell_2 < (centre_0 - _S114)))
    {

#line 490
        _S115 = true;

#line 490
    }
    else
    {

#line 490
        _S115 = any(cell_2 > (centre_0 + _S114));

#line 490
    }

#line 490
    if(_S115)
    {

#line 491
        return 4294967295U;
    }
    int edge_0 = int(kernelContext_18->giProbes_0->constants_0[5U].z);
    uint cells_0 = uint(edge_0 * edge_0 * edge_0);
    int _S116 = giWrap_0(cell_2.z, edge_0);

#line 495
    uint _S117 = uint(edge_0);

#line 495
    uint _S118 = uint(_S116) * _S117;

#line 495
    int _S119 = giWrap_0(cell_2.y, edge_0);

#line 495
    uint _S120 = (_S118 + uint(_S119)) * _S117;

    int _S121 = giWrap_0(cell_2.x, edge_0);
    return kernelContext_18->giProbes_0->layout_0[level_3 * cells_0 + (_S120 + uint(_S121))];
}


#line 114
constant array<float3, int(6)> kGpAxes_0 = { float3(1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, -1.0, 0.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, -1.0) };

#line 445
float gpVisibilityWeight_0(uint probe_0, float3 position_8, float3 query_0, KernelContext_0 thread* kernelContext_19)
{

#line 446
    float3 offset_1 = query_0 - position_8;
    float span_0 = length(offset_1);
    if(span_0 <= 0.00009999999747379)
    {

#line 449
        return 1.0;
    }
    float3 _S122 = offset_1 / float3(span_0) ;
    uint _S123 = probe_0 * 4U;

#line 452
    float4 device* _S124 = kernelContext_19->giProbes_0->probes_0+(_S123 + 2U);
    float4 device* _S125 = kernelContext_19->giProbes_0->probes_0+(_S123 + 3U);
    array<float, int(6)> _S126 = { (*_S124).x, (*_S124).y, (*_S124).z, (*_S124).w, (*_S125).x, (*_S125).y };

#line 454
    uint axis_1 = 0U;

#line 454
    float expected_0 = 0.0;

#line 454
    float weight_1 = 0.0;


    for(;;)
    {

#line 457
        if(axis_1 < 6U)
        {
        }
        else
        {

#line 457
            break;
        }

#line 458
        float _S127 = max(0.0, dot(kGpAxes_0[axis_1], _S122));
        float expected_1 = expected_0 + _S126[axis_1] * _S127;
        float weight_2 = weight_1 + _S127;

#line 457
        axis_1 = axis_1 + 1U;

#line 457
        expected_0 = expected_1;

#line 457
        weight_1 = weight_2;

#line 457
    }

#line 462
    if(weight_1 <= 0.0)
    {

#line 463
        return 1.0;
    }



    float slack_0 = (as_type<float>((kernelContext_19->giProbes_0->constants_0[1U].y)));
    float _S128 = expected_0 / weight_1 + slack_0;

#line 469
    if(span_0 > _S128)
    {

#line 470
        return 0.0;
    }
    float _S129 = max(slack_0, 0.00009999999747379);

#line 472
    float visible_0 = gpClamp_0((_S128 - span_0) / _S129, 0.0, 1.0);

#line 472
    axis_1 = 0U;

    for(;;)
    {

#line 474
        if(axis_1 < 6U)
        {
        }
        else
        {

#line 474
            break;
        }

#line 475
        float cosine_0 = dot(kGpAxes_0[axis_1], _S122);
        if(cosine_0 < 0.5)
        {

#line 477
            axis_1 = axis_1 + 1U;

#line 474
            continue;
        }

#line 474
        visible_0 = min(visible_0, gpClamp_0((_S126[axis_1] + slack_0 - span_0 * cosine_0) / _S129, 0.0, 1.0));

#line 474
        axis_1 = axis_1 + 1U;

#line 474
    }

#line 483
    return visible_0;
}


#line 300 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float grFacing_0(float3 toward_0, float3 normal_11)
{

#line 301
    return gpClamp_0((dot(normal_11, toward_0) + 1.0) * 0.5, 0.0, 1.0);
}


#line 123 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float giFinestVoxel_1(KernelContext_0 thread* kernelContext_20)
{

#line 124
    return (as_type<float>((kernelContext_20->giScene_0->constants_1[9U].y)));
}


#line 119
uint giWindowBricks_1(KernelContext_0 thread* kernelContext_21)
{

#line 120
    return kernelContext_21->giScene_0->constants_1[0U].y;
}


#line 128
float giTrilinear_1(uint slot_2, float fx_1, float fy_1, float fz_1, KernelContext_0 thread* kernelContext_22)
{
    int x0_1 = clamp(int(floor(fx_1)), int(0), int(3));
    int y0_1 = clamp(int(floor(fy_1)), int(0), int(3));
    int z0_1 = clamp(int(floor(fz_1)), int(0), int(3));



    float tx_1 = clamp(fx_1 - float(x0_1), 0.0, 1.0);
    float ty_1 = clamp(fy_1 - float(y0_1), 0.0, 1.0);

    uint base_1 = slot_2 * 125U;

    uint _S130 = uint(z0_1) * 5U;

#line 141
    uint _S131 = uint(y0_1);

#line 141
    uint _S132 = base_1 + (_S130 + _S131) * 5U;

#line 141
    uint _S133 = uint(x0_1);
    uint _S134 = uint(x0_1 + int(1));
    uint _S135 = uint(y0_1 + int(1));

#line 143
    uint _S136 = base_1 + (_S130 + _S135) * 5U;

    uint _S137 = uint(z0_1 + int(1)) * 5U;

#line 145
    uint _S138 = base_1 + (_S137 + _S131) * 5U;

    uint _S139 = base_1 + (_S137 + _S135) * 5U;

#line 153
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_22->giScene_0->bricks_0[_S132 + _S133], kernelContext_22->giScene_0->bricks_0[_S132 + _S134], tx_1), giLerp_0(kernelContext_22->giScene_0->bricks_0[_S136 + _S133], kernelContext_22->giScene_0->bricks_0[_S136 + _S134], tx_1), ty_1), giLerp_0(giLerp_0(kernelContext_22->giScene_0->bricks_0[_S138 + _S133], kernelContext_22->giScene_0->bricks_0[_S138 + _S134], tx_1), giLerp_0(kernelContext_22->giScene_0->bricks_0[_S139 + _S133], kernelContext_22->giScene_0->bricks_0[_S139 + _S134], tx_1), ty_1), clamp(fz_1 - float(z0_1), 0.0, 1.0));
}


#line 175
float giSampleLevel_1(uint level_4, float3 p_2, float thread* farDistance_2, KernelContext_0 thread* kernelContext_23)
{

#line 176
    uint _S140 = 16U + level_4 * 2U;

#line 176
    float4 shape_1 = (as_type<float4>((kernelContext_23->giScene_0->constants_1[_S140])));
    int4 origin_3 = (as_type<int4>((kernelContext_23->giScene_0->constants_1[_S140 + 1U])));
    float voxel_1 = shape_1.x;
    float brick_1 = shape_1.y;
    *farDistance_2 = shape_1.z;
    int bx_1 = int(floor(p_2.x / brick_1));
    int by_1 = int(floor(p_2.y / brick_1));
    int bz_1 = int(floor(p_2.z / brick_1));

#line 183
    uint _S141 = giWindowBricks_1(kernelContext_23);
    int dim_1 = int(_S141);
    int _S142 = origin_3.x;

#line 185
    bool _S143;

#line 185
    if(bx_1 < _S142)
    {

#line 185
        _S143 = true;

#line 185
    }
    else
    {

#line 185
        _S143 = by_1 < (origin_3.y);

#line 185
    }

#line 185
    if(_S143)
    {

#line 185
        _S143 = true;

#line 185
    }
    else
    {

#line 185
        _S143 = bz_1 < (origin_3.z);

#line 185
    }

#line 185
    if(_S143)
    {

#line 185
        _S143 = true;

#line 185
    }
    else
    {

#line 185
        _S143 = bx_1 >= (_S142 + dim_1);

#line 185
    }

#line 185
    if(_S143)
    {

#line 185
        _S143 = true;

#line 185
    }
    else
    {

#line 185
        _S143 = by_1 >= (origin_3.y + dim_1);

#line 185
    }
    if(_S143)
    {

#line 186
        _S143 = true;

#line 186
    }
    else
    {

#line 186
        _S143 = bz_1 >= (origin_3.z + dim_1);

#line 186
    }

#line 185
    if(_S143)
    {
        return *farDistance_2;
    }
    int _S144 = giWrap_0(bz_1, dim_1);

#line 189
    uint _S145 = uint(dim_1);

#line 189
    uint _S146 = uint(_S144) * _S145;

#line 189
    int _S147 = giWrap_0(by_1, dim_1);

#line 189
    uint _S148 = (_S146 + uint(_S147)) * _S145;
    int _S149 = giWrap_0(bx_1, dim_1);
    uint slot_3 = kernelContext_23->giScene_0->pageTable_0[level_4 * uint(dim_1 * dim_1 * dim_1) + (_S148 + uint(_S149))];
    if(slot_3 == 4294967295U)
    {

#line 193
        return *farDistance_2;
    }

    float3 local_1 = (p_2 - float3(float(bx_1) * brick_1, float(by_1) * brick_1, float(bz_1) * brick_1)) / float3(voxel_1) ;

#line 196
    float _S150 = giTrilinear_1(slot_3, local_1.x, local_1.y, local_1.z, kernelContext_23);
    return _S150;
}


float giDistance_1(float3 p_3, KernelContext_0 thread* kernelContext_24)
{

#line 201
    uint _S151 = giLevelCount_0(kernelContext_24);

#line 201
    float answer_1 = 1.0e+06;

#line 201
    uint level_5 = 0U;


    for(;;)
    {

#line 204
        if(level_5 < _S151)
        {
        }
        else
        {

#line 204
            break;
        }

#line 205
        thread float farDistance_3 = 0.0;

#line 205
        float _S152 = giSampleLevel_1(level_5, p_3, &farDistance_3, kernelContext_24);

        float _S153 = min(answer_1, _S152);
        if(_S152 < farDistance_3)
        {

#line 209
            return _S152;
        }

#line 204
        uint level_6 = level_5 + 1U;

#line 204
        answer_1 = _S153;

#line 204
        level_5 = level_6;

#line 204
    }

#line 212
    return answer_1;
}


#line 225
GiTraceHit_0 giSphereTrace_1(float3 origin_4, float3 direction_4, float maxDistance_2, float tMin_1, KernelContext_0 thread* kernelContext_25)
{

#line 226
    thread GiTraceHit_0 result_1;
    (&result_1)->hit_1 = false;
    (&result_1)->exhausted_0 = false;
    (&result_1)->t_1 = 0.0;
    (&result_1)->closest_0 = 0.0;
    (&result_1)->position_1 = float3(0.0, 0.0, 0.0);
    (&result_1)->normal_1 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S154 = giLevelCount_0(kernelContext_25);
    if(_S154 == 0U)
    {

#line 234
        return result_1;
    }

#line 234
    float _S155 = giFinestVoxel_0(kernelContext_25);


    float surface_1 = _S155 * 0.5;
    float _S156 = max(tMin_1, surface_1);
    thread array<float, int(3)> recent_1;

#line 239
    recent_1[int(0)] = 1.0e+06;

#line 239
    recent_1[int(1)] = 1.0e+06;

#line 239
    recent_1[int(2)] = 1.0e+06;

#line 239
    float closest_2 = 1.0e+06;

#line 239
    uint recentSlot_1 = 0U;

#line 239
    uint step_2 = 0U;

#line 239
    float t_6 = _S156;


    for(;;)
    {

#line 242
        if(step_2 < 192U)
        {
        }
        else
        {

#line 242
            break;
        }

#line 243
        if(t_6 > maxDistance_2)
        {
            (&result_1)->closest_0 = min(closest_2, min(recent_1[int(0)], min(recent_1[int(1)], recent_1[int(2)])));
            return result_1;
        }
        float3 position_9 = origin_4 + direction_4 * float3(t_6) ;

#line 248
        float _S157 = giDistance_0(position_9, kernelContext_25);

        if(_S157 < surface_1)
        {

#line 251
            (&result_1)->hit_1 = true;
            (&result_1)->t_1 = t_6;
            (&result_1)->position_1 = position_9;


            float3 _S158 = float3(_S155, 0.0, 0.0);

#line 256
            float _S159 = giDistance_0(position_9 + _S158, kernelContext_25);

#line 256
)cy_msl",
    R"cy_msl(            float _S160 = giDistance_0(position_9 - _S158, kernelContext_25);

#line 256
            float _S161 = _S159 - _S160;

            float3 _S162 = float3(0.0, _S155, 0.0);

#line 258
            float _S163 = giDistance_0(position_9 + _S162, kernelContext_25);

#line 258
            float _S164 = giDistance_0(position_9 - _S162, kernelContext_25);

#line 258
            float _S165 = _S163 - _S164;

            float3 _S166 = float3(0.0, 0.0, _S155);

#line 260
            float _S167 = giDistance_0(position_9 + _S166, kernelContext_25);

#line 260
            float _S168 = giDistance_0(position_9 - _S166, kernelContext_25);

            (&result_1)->normal_1 = giNormalisedOr_0(float3(_S161, _S165, _S167 - _S168), - direction_4);
            (&result_1)->closest_0 = closest_2;
            return result_1;
        }
        float _S169 = min(closest_2, recent_1[recentSlot_1]);
        recent_1[recentSlot_1] = _S157;
        uint _S170 = (recentSlot_1 + 1U) % 3U;
        float t_7 = t_6 + max(_S157, surface_1);

#line 242
        uint step_3 = step_2 + 1U;

#line 242
        closest_2 = _S169;

#line 242
        recentSlot_1 = _S170;

#line 242
        step_2 = step_3;

#line 242
        t_6 = t_7;

#line 242
    }

#line 271
    (&result_1)->exhausted_0 = true;

    (&result_1)->closest_0 = min(closest_2, min(recent_1[int(0)], min(recent_1[int(1)], recent_1[int(2)])));
    return result_1;
}


#line 286 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
bool grProbeOccluded_0(float3 from_0, float3 to_0, KernelContext_0 thread* kernelContext_26)
{

#line 287
    float3 offset_2 = to_0 - from_0;
    float span_1 = length(offset_2);

#line 288
    float _S171 = giFinestVoxel_1(kernelContext_26);

    if(span_1 <= (_S171 * 2.0))
    {

#line 291
        return false;
    }

#line 291
    float _S172 = giDistance_1(from_0, kernelContext_26);

#line 291
    float _S173 = giDistance_1(to_0, kernelContext_26);

    if((_S172 + _S173) >= span_1)
    {

#line 294
        return false;
    }

#line 294
    GiTraceHit_0 _S174 = giSphereTrace_1(from_0, offset_2 / float3(span_1) , span_1 - _S171, 0.0, kernelContext_26);

    return _S174.hit_1;
}


#line 273 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
void gpLoadPayload_0(uint probe_1, uint floats_0, array<float, int(48)> thread* payload_0, KernelContext_0 thread* kernelContext_27)
{

#line 273
    uint index_3 = 0U;
    for(;;)
    {

#line 274
        if(index_3 < 48U)
        {
        }
        else
        {

#line 274
            break;
        }

#line 274
        float _S175;
        if(index_3 < floats_0)
        {

#line 275
            _S175 = *(kernelContext_27->giProbes_0->payloads_0+(probe_1 * floats_0 + index_3));

#line 275
        }
        else
        {

#line 275
            _S175 = 0.0;

#line 275
        }

#line 275
        (*payload_0)[index_3] = _S175;

#line 274
        index_3 = index_3 + 1U;

#line 274
    }


    return;
}


#line 110
float3 gpMax0_0(float3 v_1)
{

#line 111
    return float3(max(0.0, v_1.x), max(0.0, v_1.y), max(0.0, v_1.z));
}


#line 154
void gpOctahedralUv_0(float3 direction_5, float thread* u_0, float thread* v_2)
{

#line 155
    float sum_0 = abs(direction_5.x) + abs(direction_5.y) + abs(direction_5.z);

#line 155
    float3 p_4;
    if(sum_0 > 0.0)
    {

#line 156
        p_4 = direction_5 / float3(sum_0) ;

#line 156
    }
    else
    {

#line 156
        p_4 = float3(0.0, 0.0, 1.0);

#line 156
    }
    float _S176 = p_4.x;
    float _S177 = p_4.y;

#line 158
    float px_0;

#line 158
    float py_0;
    if((p_4.z) < 0.0)
    {

#line 160
        float _S178 = 1.0 - abs(_S177);

#line 160
        if(_S176 >= 0.0)
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
        float fx_2 = _S178 * px_0;
        float _S179 = 1.0 - abs(_S176);

#line 161
        if(_S177 >= 0.0)
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
        float fy_2 = _S179 * px_0;

#line 161
        px_0 = fx_2;

#line 161
        py_0 = fy_2;

#line 159
    }
    else
    {

#line 159
        px_0 = _S176;

#line 159
        py_0 = _S177;

#line 159
    }

#line 165
    *u_0 = gpClamp_0(px_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    *v_2 = gpClamp_0(py_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    return;
}


#line 220
float3 gpDecode_0(uint encoding_0, const array<float, int(48)> thread* payload_1, float3 direction_6)
{

#line 220
    uint c_0;
    if(encoding_0 == 0U)
    {



        array<float, int(4)> _S180 = { 0.28209498524665833, 0.32573533058166504 * direction_6.y, 0.32573533058166504 * direction_6.z, 0.32573533058166504 * direction_6.x };

        thread float3 result_2 = float3(0.0, 0.0, 0.0);

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
            uint _S181 = c_0 * 3U;

#line 230
            result_2.x = result_2.x + (*payload_1)[_S181] * _S180[c_0];
            result_2.y = result_2.y + (*payload_1)[_S181 + 1U] * _S180[c_0];
            result_2.z = result_2.z + (*payload_1)[_S181 + 2U] * _S180[c_0];

#line 229
            c_0 = c_0 + 1U;

#line 229
        }

#line 234
        return gpMax0_0(result_2);
    }
    if(encoding_0 == 1U)
    {

#line 237
        thread float u_1 = 0.0;
        thread float v_3 = 0.0;
        gpOctahedralUv_0(direction_6, &u_1, &v_3);
        uint x0_2 = uint(u_1);
        uint y0_2 = uint(v_3);
        uint _S182 = min(x0_2 + 1U, 3U);



        uint _S183 = y0_2 * 4U;

        uint _S184 = min(y0_2 + 1U, 3U) * 4U;

        uint _S185 = (_S183 + x0_2) * 3U;

#line 250
        float3 f00_0 = float3((*payload_1)[_S185], (*payload_1)[_S185 + 1U], (*payload_1)[_S185 + 2U]);
        uint _S186 = (_S183 + _S182) * 3U;
        uint _S187 = (_S184 + x0_2) * 3U;

#line 252
        float3 f01_0 = float3((*payload_1)[_S187], (*payload_1)[_S187 + 1U], (*payload_1)[_S187 + 2U]);
        uint _S188 = (_S184 + _S182) * 3U;

#line 253
        float3 _S189 = float3((u_1 - float(x0_2))) ;
        float3 top_0 = f00_0 + (float3((*payload_1)[_S186], (*payload_1)[_S186 + 1U], (*payload_1)[_S186 + 2U]) - f00_0) * _S189;

        return top_0 + (f01_0 + (float3((*payload_1)[_S188], (*payload_1)[_S188 + 1U], (*payload_1)[_S188 + 2U]) - f01_0) * _S189 - top_0) * float3((v_3 - float(y0_2))) ;
    }
    float3 _S190 = float3(0.0, 0.0, 0.0);

#line 258
    float3 result_3 = _S190;

#line 258
    c_0 = 0U;

#line 258
    float total_1 = 0.0;

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
        float _S191 = max(0.0, dot(kGpAxes_0[c_0], direction_6));
        if(_S191 <= 0.0)
        {

#line 263
            c_0 = c_0 + 1U;

#line 260
            continue;
        }



        uint _S192 = c_0 * 3U;

        float total_2 = total_1 + _S191;

#line 267
        result_3 = result_3 + float3((*payload_1)[_S192], (*payload_1)[_S192 + 1U], (*payload_1)[_S192 + 2U]) * float3(_S191) ;

#line 267
        total_1 = total_2;

#line 260
        c_0 = c_0 + 1U;

#line 260
    }

#line 269
    if(total_1 > 0.0)
    {

#line 269
        result_3 = result_3 / float3(total_1) ;

#line 269
    }
    else
    {

#line 269
        result_3 = _S190;

#line 269
    }

#line 269
    return result_3;
}


#line 305 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float4 grFinish_0(float3 total_3, float weight_3, float oldest_0)
{

#line 306
    if(weight_3 <= 0.0)
    {

#line 307
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    return float4(total_3 / float3(weight_3) , gpClamp_0(1.0 - gpClamp_0(oldest_0 / 240.0, 0.0, 1.0) * 0.75, 0.0, 1.0));
}


#line 317
void grSampleTraced_0(float3 position_10, float3 normal_12, float3 reflection_2, uint frame_4, float4 thread* diffuse_0, float4 thread* specular_0, KernelContext_0 thread* kernelContext_28)
{
    uint4 counts_0 = kernelContext_28->giProbes_0->constants_0[0U];
    uint _S193 = counts_0.x;
    uint _S194 = counts_0.y;
    uint _S195 = kernelContext_28->giProbes_0->constants_0[1U].z;
    float _S196 = (as_type<float>((kernelContext_28->giProbes_0->constants_0[5U].x)));
    float3 _S197 = position_10 + normal_12 * float3((as_type<float>((kernelContext_28->giProbes_0->constants_0[1U].y)))) ;
    float3 _S198 = float3(0.0, 0.0, 0.0);

#line 325
    float3 diffuseTotal_0 = _S198;

#line 325
    float diffuseOldest_0 = 0.0;

#line 325
    float3 specularTotal_0 = _S198;

#line 325
    float specularOldest_0 = 0.0;

#line 325
    uint level_7 = 0U;

#line 325
    float diffuseWeight_0 = 0.0;

#line 325
    float specularWeight_0 = 0.0;

#line 331
    for(;;)
    {

#line 331
        if(level_7 < _S195)
        {
        }
        else
        {

#line 331
            break;
        }

#line 332
        float spacing_0 = _S196 * float(1U << level_7);
        float _S199 = position_10.x;

#line 333
        int _S200 = gpFloorDiv_0(_S199 - spacing_0, spacing_0);
        float _S201 = position_10.y;

#line 334
        int _S202 = gpFloorDiv_0(_S201 - spacing_0, spacing_0);
        float _S203 = position_10.z;
        int _S204 = gpFloorDiv_0(_S199 + spacing_0, spacing_0);
        int _S205 = gpFloorDiv_0(_S201 + spacing_0, spacing_0);
        int _S206 = gpFloorDiv_0(_S203 + spacing_0, spacing_0);

#line 338
        int z_3 = gpFloorDiv_0(_S203 - spacing_0, spacing_0);
        for(;;)
        {

#line 339
            if(z_3 <= _S206)
            {
            }
            else
            {

#line 339
                break;
            }

#line 339
            float3 diffuseTotal_1 = diffuseTotal_0;

#line 339
            float diffuseOldest_1 = diffuseOldest_0;

#line 339
            float3 specularTotal_1 = specularTotal_0;

#line 339
            float specularOldest_1 = specularOldest_0;

#line 339
            int y_4 = _S202;

#line 339
            float diffuseWeight_1 = diffuseWeight_0;

#line 339
            float specularWeight_1 = specularWeight_0;
            for(;;)
            {

#line 340
                if(y_4 <= _S205)
                {
                }
                else
                {

#line 340
                    break;
                }

#line 340
                float3 diffuseTotal_2 = diffuseTotal_1;

#line 340
                float diffuseOldest_2 = diffuseOldest_1;

#line 340
                float3 specularTotal_2 = specularTotal_1;

#line 340
                float specularOldest_2 = specularOldest_1;

#line 340
                int x_4 = _S200;

#line 340
                float diffuseWeight_2 = diffuseWeight_1;

#line 340
                float specularWeight_2 = specularWeight_1;
                for(;;)
                {

#line 341
                    if(x_4 <= _S204)
                    {
                    }
                    else
                    {

#line 341
                        break;
                    }

#line 341
                    uint _S207 = gpLayoutProbe_0(level_7, int3(x_4, y_4, z_3), kernelContext_28);

                    if(_S207 == 4294967295U)
                    {

#line 344
                        x_4 = x_4 + int(1);

#line 341
                        continue;
                    }



                    uint _S208 = _S207 * 4U;

#line 346
                    float4 device* _S209 = kernelContext_28->giProbes_0->probes_0+(_S208 + 1U);

#line 346
                    float4 record_0 = *_S209;

#line 346
                    bool _S210;
                    if(((*_S209).y) == 0.0)
                    {

#line 347
                        _S210 = true;

#line 347
                    }
                    else
                    {

#line 347
                        _S210 = (record_0.z) == 0.0;

#line 347
                    }

#line 347
                    if(_S210)
                    {

#line 348
                        x_4 = x_4 + int(1);

#line 341
                        continue;
                    }

#line 350
                    float3 at_0 = (*(kernelContext_28->giProbes_0->probes_0+_S208)).xyz;
                    float3 offset_3 = position_10 - at_0;
                    float span_2 = length(offset_3);
                    if(span_2 > spacing_0)
                    {

#line 354
                        x_4 = x_4 + int(1);

#line 341
                        continue;
                    }

#line 356
                    float _S211 = 1.0 - gpClamp_0(span_2 / spacing_0, 0.0, 1.0);

#line 356
                    float _S212 = gpVisibilityWeight_0(_S207, at_0, position_10, kernelContext_28);

#line 356
                    float falloff_0 = _S211 * _S212;

                    bool apart_0 = span_2 > 0.00009999999747379;

#line 358
                    float3 toward_1;
                    if(apart_0)
                    {

#line 359
                        toward_1 = - offset_3 / float3(span_2) ;

#line 359
                    }
                    else
                    {

#line 359
                        toward_1 = normal_12;

#line 359
                    }

#line 359
                    float diffuseShare_0;
                    if(apart_0)
                    {

#line 360
                        diffuseShare_0 = falloff_0 * grFacing_0(toward_1, normal_12);

#line 360
                    }
                    else
                    {

#line 360
                        diffuseShare_0 = falloff_0;

#line 360
                    }

#line 360
                    float specularShare_0;

                    if(apart_0)
                    {

#line 362
                        specularShare_0 = falloff_0 * grFacing_0(toward_1, reflection_2);

#line 362
                    }
                    else
                    {

#line 362
                        specularShare_0 = falloff_0;

#line 362
                    }

#line 362
                    bool _S213;
                    if(diffuseShare_0 <= 0.0)
                    {

#line 363
                        _S213 = specularShare_0 <= 0.0;

#line 363
                    }
                    else
                    {

#line 363
                        _S213 = false;

#line 363
                    }

#line 363
                    bool _S214;

#line 363
                    if(_S213)
                    {

#line 363
                        _S214 = true;

#line 363
                    }
                    else
                    {

#line 363
                        bool _S215 = grProbeOccluded_0(_S197, at_0, kernelContext_28);

#line 363
                        _S214 = _S215;

#line 363
                    }

#line 363
                    if(_S214)
                    {
                        x_4 = x_4 + int(1);

#line 341
                        continue;
                    }

#line 367
                    thread array<float, int(48)> data_0;

#line 367
                    gpLoadPayload_0(_S207, _S194, &data_0, kernelContext_28);

                    uint last_1 = (as_type<uint>((record_0.x)));

#line 369
                    float age_1;
                    if(frame_4 > last_1)
                    {

#line 370
                        age_1 = float(frame_4 - last_1);

#line 370
                    }
                    else
                    {

#line 370
                        age_1 = 0.0;

#line 370
                    }

#line 370
                    float3 diffuseTotal_3;

#line 370
                    float diffuseOldest_3;

#line 370
                    float diffuseWeight_3;
                    if(diffuseShare_0 > 0.0)
                    {

#line 371
                        thread array<float, int(48)> _S216 = data_0;

#line 371
                        float3 _S217 = gpDecode_0(_S193, &_S216, normal_12);

                        float diffuseWeight_4 = diffuseWeight_2 + diffuseShare_0;
)cy_msl",
    R"cy_msl(                        float _S218 = max(diffuseOldest_2, age_1);

#line 374
                        diffuseTotal_3 = diffuseTotal_2 + _S217 * float3(diffuseShare_0) ;

#line 374
                        diffuseOldest_3 = _S218;

#line 374
                        diffuseWeight_3 = diffuseWeight_4;

#line 371
                    }
                    else
                    {

#line 371
                        diffuseTotal_3 = diffuseTotal_2;

#line 371
                        diffuseOldest_3 = diffuseOldest_2;

#line 371
                        diffuseWeight_3 = diffuseWeight_2;

#line 371
                    }

#line 371
                    float3 specularTotal_3;

#line 371
                    float specularOldest_3;

#line 371
                    float specularWeight_3;

#line 376
                    if(specularShare_0 > 0.0)
                    {

#line 376
                        thread array<float, int(48)> _S219 = data_0;

#line 376
                        float3 _S220 = gpDecode_0(_S193, &_S219, reflection_2);


                        float specularWeight_4 = specularWeight_2 + specularShare_0;
                        float _S221 = max(specularOldest_2, age_1);

#line 380
                        specularTotal_3 = specularTotal_2 + _S220 * float3(specularShare_0) ;

#line 380
                        specularOldest_3 = _S221;

#line 380
                        specularWeight_3 = specularWeight_4;

#line 376
                    }
                    else
                    {

#line 376
                        specularTotal_3 = specularTotal_2;

#line 376
                        specularOldest_3 = specularOldest_2;

#line 376
                        specularWeight_3 = specularWeight_2;

#line 376
                    }

#line 376
                    diffuseTotal_2 = diffuseTotal_3;

#line 376
                    diffuseOldest_2 = diffuseOldest_3;

#line 376
                    specularTotal_2 = specularTotal_3;

#line 376
                    specularOldest_2 = specularOldest_3;

#line 376
                    diffuseWeight_2 = diffuseWeight_3;

#line 376
                    specularWeight_2 = specularWeight_3;

#line 341
                    x_4 = x_4 + int(1);

#line 341
                }

#line 340
                int y_5 = y_4 + int(1);

#line 340
                diffuseTotal_1 = diffuseTotal_2;

#line 340
                diffuseOldest_1 = diffuseOldest_2;

#line 340
                specularTotal_1 = specularTotal_2;

#line 340
                specularOldest_1 = specularOldest_2;

#line 340
                y_4 = y_5;

#line 340
                diffuseWeight_1 = diffuseWeight_2;

#line 340
                specularWeight_1 = specularWeight_2;

#line 340
            }

#line 339
            int z_4 = z_3 + int(1);

#line 339
            diffuseTotal_0 = diffuseTotal_1;

#line 339
            diffuseOldest_0 = diffuseOldest_1;

#line 339
            specularTotal_0 = specularTotal_1;

#line 339
            specularOldest_0 = specularOldest_1;

#line 339
            z_3 = z_4;

#line 339
            diffuseWeight_0 = diffuseWeight_1;

#line 339
            specularWeight_0 = specularWeight_1;

#line 339
        }

#line 331
        level_7 = level_7 + 1U;

#line 331
    }

#line 386
    *diffuse_0 = grFinish_0(diffuseTotal_0, diffuseWeight_0, diffuseOldest_0);
    *specular_0 = grFinish_0(specularTotal_0, specularWeight_0, specularOldest_0);
    return;
}


#line 461
void grResolvePixel_0(uint pixel_4, KernelContext_0 thread* kernelContext_29)
{

#line 462
    uint4 counts_1 = kernelContext_29->giResolve_0->constants_2[0U];
    uint _S222 = counts_1.z;

#line 463
    if(pixel_4 >= _S222)
    {

#line 464
        return;
    }
    uint record_1 = (counts_1.w * _S222 + pixel_4) * 2U;
    float4 at_1 = kernelContext_29->giResolve_0->surfaces_0[record_1];
    float4 facing_0 = kernelContext_29->giResolve_0->surfaces_0[record_1 + 1U];
    if((at_1.w) <= 0.0)
    {

#line 470
        uint _S223 = pixel_4 * 2U;

#line 470
        float4 _S224 = float4(0.0, 0.0, 0.0, 0.0);

#line 470
        *(kernelContext_29->giResolve_0->noisy_0+_S223) = _S224;
        *(kernelContext_29->giResolve_0->noisy_0+(_S223 + 1U)) = _S224;
        return;
    }
    uint frame_5 = kernelContext_29->giResolve_0->constants_2[18U].x;

    float3 position_11 = at_1.xyz;
    float3 normal_13 = giNormalisedOr_0(facing_0.xyz, float3(0.0, 1.0, 0.0));
    float3 offset_4 = position_11 - (as_type<float3>((kernelContext_29->giResolve_0->constants_2[1U].xyz)));

    float3 incident_0 = giNormalisedOr_0(offset_4, float3(0.0, 0.0, -1.0));

    float3 reflection_3 = giNormalisedOr_0(incident_0 - normal_13 * float3((2.0 * dot(incident_0, normal_13))) , normal_13);
    thread float4 cachedDiffuse_0;
    thread float4 cachedSpecular_0;

#line 484
    grSampleTraced_0(position_11, normal_13, reflection_3, kernelContext_29->giResolve_0->constants_2[4U].w, &cachedDiffuse_0, &cachedSpecular_0, kernelContext_29);

#line 484
    float3 _S225 = grResolveDiffuse_0(pixel_4, position_11, normal_13, length(offset_4), cachedDiffuse_0, frame_5, kernelContext_29);

#line 484
    float3 _S226 = grResolveSpecular_0(pixel_4, position_11, normal_13, facing_0.w, reflection_3, cachedSpecular_0, frame_5, kernelContext_29);

#line 490
    uint _S227 = pixel_4 * 2U;

#line 490
    *(kernelContext_29->giResolve_0->noisy_0+_S227) = float4(_S225, 1.0);
    *(kernelContext_29->giResolve_0->noisy_0+(_S227 + 1U)) = float4(_S226, 1.0);
    return;
}


#line 15 "src/rendering/gi_gpu/shaders/gi_resolve.slang"
[[kernel]] void cyGiResolve(uint3 thread_0 [[thread_position_in_grid]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiResolveSet_default_0 constant* giResolve_1 [[buffer(3)]])
{

#line 15
    thread KernelContext_0 kernelContext_30;

#line 15
    (&kernelContext_30)->giProbes_0 = giProbes_1;

#line 15
    (&kernelContext_30)->giScene_0 = giScene_1;

#line 15
    (&kernelContext_30)->giResolve_0 = giResolve_1;

#line 15
    grResolvePixel_0(thread_0.x, &kernelContext_30);

    return;
}

)cy_msl");
inline constexpr const auto& kGiResolveMsl = kGiResolveMslText.text;

/// cyGiResolveHardware.metal, 75406 bytes.
inline constexpr auto kGiResolveHardwareMslText = detail::join_msl(
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
            

#line 118 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
struct GrSample_0
{
    float3 radiance_0;
    float confidence_0;
    uint source_0;
};


#line 53
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


#line 78 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 66 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
struct GiResolveSet_default_0
{
    uint4 device* constants_2;
    float4 device* surfaces_0;
    float4 device* noisy_0;
    float4 device* probes_1;
    uint device* slots_0;
};


#line 22 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
struct GiHardwareSet_default_0
{
    metal::raytracing::acceleration_structure<metal::raytracing::instancing> topLevel_0;
    uint4 device* instances_0;
    float device* positions_0;
    uint device* indices_0;
};


#line 22
struct KernelContext_0
{
    GiProbeSet_default_0 constant* giProbes_0;
    GiSceneSet_default_0 constant* giScene_0;
    GiResolveSet_default_0 constant* giResolve_0;
    GiHardwareSet_default_0 constant* giHardware_0;
};


#line 73
uint GiHardwareTier_source_0()
{

#line 74
    return 7U;
}


#line 123 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_0)
{

#line 124
    return (as_type<float>((kernelContext_0->giScene_0->constants_1[9U].y)));
}


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


#line 27 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
float3 ghVertex_0(uint vertex_0, KernelContext_0 thread* kernelContext_1)
{

#line 28
    uint _S1 = vertex_0 * 3U;

#line 28
    return float3(kernelContext_1->giHardware_0->positions_0[_S1], kernelContext_1->giHardware_0->positions_0[_S1 + 1U], kernelContext_1->giHardware_0->positions_0[_S1 + 2U]);
}


#line 99 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 100
    float squared_0 = dot(v_0, v_0);

#line 100
    float3 _S2;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 101
        _S2 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 101
    }
    else
    {

#line 101
        _S2 = fallback_0;

#line 101
    }

#line 101
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


#line 33 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
GiWorldHit_0 GiHardwareTier_trace_0(float3 origin_0, float3 direction_0, float maxDistance_0, KernelContext_0 thread* kernelContext_2)
{

#line 34
    thread RayDesc_0 ray_0;
    (&ray_0)->Origin_0 = origin_0;
    (&ray_0)->Direction_0 = direction_0;
    (&ray_0)->TMin_0 = 0.00100000004749745;
    (&ray_0)->TMax_0 = maxDistance_0;
    thread raytracing::intersection_query<raytracing::triangle_data, raytracing::instancing> query_0;
    (&query_0)->reset({ (ray_0.Origin_0), (ray_0.Direction_0), (ray_0.TMin_0), (ray_0.TMax_0) }, (kernelContext_2->giHardware_0->topLevel_0), (255U), _slang_ray_flags_to_intersection_params((1U)));
    for(;;)
    {

#line 41
        bool _S4 = ((&query_0)->next());

#line 41
        if(_S4)
        {
        }
        else
        {

#line 41
            break;
        }

#line 41
    }

    uint _S5 = (_slang_committed_intersection_type_to_uint((&query_0)->get_committed_intersection_type()));

#line 43
    if(_S5 != 1U)
    {

#line 44
        return gpMiss_0();
    }
    float t_1 = ((&query_0)->get_committed_distance());
    uint instance_0 = ((&query_0)->get_committed_user_instance_id());
    uint primitive_0 = ((&query_0)->get_committed_primitive_id());
    uint _S6 = instance_0 * 4U;

#line 49
    uint4 record_0 = kernelContext_2->giHardware_0->instances_0[_S6];
    uint first_0 = record_0.y + primitive_0 * 3U;
    uint _S7 = record_0.x;

#line 51
    float3 _S8 = ghVertex_0(_S7 + kernelContext_2->giHardware_0->indices_0[first_0], kernelContext_2);

#line 51
    float3 _S9 = ghVertex_0(_S7 + kernelContext_2->giHardware_0->indices_0[first_0 + 1U], kernelContext_2);

#line 51
    float3 _S10 = ghVertex_0(_S7 + kernelContext_2->giHardware_0->indices_0[first_0 + 2U], kernelContext_2);


    float4 c0_0 = (as_type<float4>((kernelContext_2->giHardware_0->instances_0[_S6 + 1U])));


    float3 local_0 = cross(_S9 - _S8, _S10 - _S8);
    float3 normal_1 = giNormalisedOr_0(c0_0.xyz * float3(local_0.x)  + (as_type<float4>((kernelContext_2->giHardware_0->instances_0[_S6 + 2U]))).xyz * float3(local_0.y)  + (as_type<float4>((kernelContext_2->giHardware_0->instances_0[_S6 + 3U]))).xyz * float3(local_0.z) , float3(0.0, 1.0, 0.0));

#line 58
    float3 normal_2;

    if((dot(normal_1, direction_0)) > 0.0)
    {

#line 60
        normal_2 = - normal_1;

#line 60
    }
    else
    {

#line 60
        normal_2 = normal_1;

#line 60
    }


    thread GiWorldHit_0 hit_1;
    (&hit_1)->hit_0 = true;
    (&hit_1)->t_0 = t_1;
    (&hit_1)->position_0 = origin_0 + direction_0 * float3(t_1) ;
    (&hit_1)->normal_0 = normal_2;


    (&hit_1)->confidence_1 = (as_type<float4>((kernelContext_2->giProbes_0->constants_0[25U]))).z - gpClamp_0(c0_0.w * 2.0, 0.0, 0.40000000596046448);
    return hit_1;
}


#line 124 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
GrSample_0 grSample_0(float3 radiance_1, float confidence_2, uint source_1)
{

#line 125
    thread GrSample_0 sample_0;
    (&sample_0)->radiance_0 = radiance_1;
    (&sample_0)->confidence_0 = confidence_2;
    (&sample_0)->source_0 = source_1;
    return sample_0;
}


#line 689 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiTierSample_0
{
    float3 radiance_2;
    float confidence_3;
    float hitDistance_0;
    uint source_2;
};


#line 696
GiTierSample_0 gpTierSample_0(uint source_3)
{

#line 697
    thread GiTierSample_0 sample_1;
    (&sample_1)->radiance_2 = float3(0.0, 0.0, 0.0);
    (&sample_1)->confidence_3 = 0.0;
    (&sample_1)->hitDistance_0 = -1.0;
    (&sample_1)->source_2 = source_3;
    return sample_1;
}


#line 121
float3 gpSky_0(float3 direction_1, KernelContext_0 thread* kernelContext_3)
{

#line 122
    float4 zenith_0 = (as_type<float4>((kernelContext_3->giProbes_0->constants_0[2U])));
    float3 horizon_0 = (as_type<float3>((kernelContext_3->giProbes_0->constants_0[3U].xyz)));
    float3 ground_0 = (as_type<float3>((kernelContext_3->giProbes_0->constants_0[4U].xyz)));

    float _S11 = giNormalisedOr_0(direction_1, float3(0.0, 1.0, 0.0)).y;

#line 126
    if(_S11 < 0.0)
    {

#line 127
        return ground_0 * float3(zenith_0.w) ;
    }

    return (horizon_0 + (zenith_0.xyz - horizon_0) * float3(gpClamp_0(pow(_S11, 0.44999998807907104), 0.0, 1.0)) ) * float3(zenith_0.w) ;
}


#line 471 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_1, float cell_0)
{

#line 472
    return int(floor(value_1 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_1, float3 normal_3, KernelContext_0 thread* kernelContext_4)
{

#line 482
    uint4 grid_0 = kernelContext_4->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 485
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S12 = radius_0 * radius_0;
    float _S13 = position_1.x;

#line 490
    int _S14 = giCell_0(_S13 - radius_0, cell_1);

#line 490
    float _S15 = position_1.y;

#line 490
    int _S16 = giCell_0(_S15 - radius_0, cell_1);
    float _S17 = position_1.z;

#line 491
    int _S18 = giCell_0(_S17 - radius_0, cell_1);
    int _S19 = giCell_0(_S13 + radius_0, cell_1);

#line 492
    int _S20 = giCell_0(_S15 + radius_0, cell_1);
    int _S21 = giCell_0(_S17 + radius_0, cell_1);

#line 493
    float bestScore_0 = 1.0e+09;

#line 493
    float bestAlignment_0 = 0.0;

#line 493
    uint best_0 = 4294967295U;

#line 493
    int z_1 = _S18;



    for(;;)
    {

#line 497
        if(z_1 <= _S21)
        {
        }
        else
        {

#line 497
            break;
        }

#line 497
        int y_1 = _S16;
        for(;;)
        {

#line 498
            if(y_1 <= _S20)
            {
            }
            else
            {

#line 498
                break;
            }

#line 498
            float bestScore_1 = bestScore_0;

#line 498
            float bestAlignment_1 = bestAlignment_0;

#line 498
            uint best_1 = best_0;

#line 498
            int x_1 = _S14;
            for(;;)
            {

#line 499
                if(x_1 <= _S19)
                {
                }
                else
                {

#line 499
                    break;
                }
                uint _S22 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 501
                uint first_1 = kernelContext_4->giScene_0->gridRanges_0[_S22];
                uint _S23 = kernelContext_4->giScene_0->gridRanges_0[_S22 + 1U];

#line 502
                float bestScore_2 = bestScore_1;

#line 502
                float bestAlignment_2 = bestAlignment_1;

#line 502
                uint best_2 = best_1;

#line 502
                uint item_0 = first_1;
                for(;;)
                {

#line 503
                    if(item_0 < (first_1 + _S23))
                    {
                    }
                    else
                    {

#line 503
                        break;
                    }

#line 504
                    uint handle_0 = kernelContext_4->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_4->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 506
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }



                    uint _S24 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_4->giScene_0->cards_0[_S24 + 1U].xyz, normal_3);
                    if(alignment_0 <= 0.25)
                    {

#line 511
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 513
                    float3 offset_0 = kernelContext_4->giScene_0->cards_0[_S24].xyz - position_1;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S12)
                    {

#line 516
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 519
                    float score_0 = squared_1 / alignment_0;

#line 519
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 520
                        better_0 = score_0 < bestScore_2;

#line 520
                    }
                    else
                    {

#line 521
                        if(alignment_0 != bestAlignment_2)
                        {

#line 521
                            better_0 = alignment_0 > bestAlignment_2;

#line 521
                        }
                        else
                        {

#line 521
                            better_0 = handle_0 < best_2;

#line 521
                        }

#line 520
                    }

#line 520
                    float bestScore_3;

#line 520
                    float bestAlignment_3;

#line 520
                    uint best_3;


                    if(better_0)
                    {

#line 523
                        bestScore_3 = score_0;

#line 523
                        bestAlignment_3 = alignment_0;

#line 523
                        best_3 = handle_0;

#line 523
                    }
                    else
                    {

#line 523
                        bestScore_3 = bestScore_2;

#line 523
)cy_msl",
    R"cy_msl(                        bestAlignment_3 = bestAlignment_2;

#line 523
                        best_3 = best_2;

#line 523
                    }

#line 523
                    bestScore_2 = bestScore_3;

#line 523
                    bestAlignment_2 = bestAlignment_3;

#line 523
                    best_2 = best_3;

#line 503
                    item_0 = item_0 + 1U;

#line 503
                }

#line 499
                int x_2 = x_1 + int(1);

#line 499
                bestScore_1 = bestScore_2;

#line 499
                bestAlignment_1 = bestAlignment_2;

#line 499
                best_1 = best_2;

#line 499
                x_1 = x_2;

#line 499
            }

#line 498
            int y_2 = y_1 + int(1);

#line 498
            bestScore_0 = bestScore_1;

#line 498
            bestAlignment_0 = bestAlignment_1;

#line 498
            best_0 = best_1;

#line 498
            y_1 = y_2;

#line 498
        }

#line 497
        z_1 = z_1 + int(1);

#line 497
    }

#line 532
    return best_0;
}


#line 776 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpWorldRadiance_0(const GiWorldHit_0 thread* hit_2, float3 direction_2, uint source_4, uint cacheFrame_0, KernelContext_0 thread* kernelContext_5)
{

#line 777
    thread GiTierSample_0 sample_2 = gpTierSample_0(source_4);
    if(!hit_2->hit_0)
    {

#line 779
        (&sample_2)->source_2 = 9U;

#line 779
        float3 _S25 = gpSky_0(direction_2, kernelContext_5);
        (&sample_2)->radiance_2 = _S25;
        (&sample_2)->confidence_3 = 1.0;
        (&sample_2)->hitDistance_0 = -1.0;
        return sample_2;
    }
    (&sample_2)->hitDistance_0 = hit_2->t_0;

#line 785
    uint _S26 = giFindCard_0(hit_2->position_0, hit_2->normal_0, kernelContext_5);

    if(_S26 != 4294967295U)
    {

#line 788
        uint _S27 = _S26 * 2U;

#line 788
        (&sample_2)->radiance_2 = (*(kernelContext_5->giScene_0->cardState_0+_S27)).xyz;
        uint last_0 = (as_type<uint>(((*(kernelContext_5->giScene_0->cardState_0+(_S27 + 1U))).w)));

#line 789
        float age_0;
        if(cacheFrame_0 > last_0)
        {

#line 790
            age_0 = float(cacheFrame_0 - last_0);

#line 790
        }
        else
        {

#line 790
            age_0 = 0.0;

#line 790
        }


        (&sample_2)->confidence_3 = hit_2->confidence_1 * (1.0 - gpClamp_0(age_0 / 240.0, 0.0, 1.0) * 0.60000002384185791);
        return sample_2;
    }
    (&sample_2)->confidence_3 = hit_2->confidence_1 * 0.20000000298023224;
    return sample_2;
}


#line 247 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
GrSample_0 grWorldRay_0(float3 position_2, float3 normal_4, float3 unit_0, float lift_0, float reach_0, KernelContext_0 thread* kernelContext_6)
{
    uint cacheFrame_1 = kernelContext_6->giProbes_0->constants_0[24U].w;
    uint _S28 = GiHardwareTier_source_0();

#line 250
    float height_0;

#line 250
    if(_S28 == 7U)
    {

#line 250
        height_0 = lift_0;

#line 250
    }
    else
    {

#line 250
        float _S29 = giFinestVoxel_0(kernelContext_6);

#line 250
        height_0 = max(lift_0, _S29);

#line 250
    }

#line 250
    GiWorldHit_0 _S30 = GiHardwareTier_trace_0(position_2 + normal_4 * float3(height_0) , unit_0, reach_0, kernelContext_6);

#line 250
    bool _S31;

    if(_S30.hit_0)
    {

#line 252
        _S31 = (dot(_S30.normal_0, unit_0)) > 0.0;

#line 252
    }
    else
    {

#line 252
        _S31 = false;

#line 252
    }

#line 252
    if(_S31)
    {

#line 253
        return grSample_0(float3(0.0, 0.0, 0.0), 0.0, 0U);
    }

#line 253
    thread GiWorldHit_0 _S32 = _S30;

#line 253
    GiTierSample_0 _S33 = gpWorldRadiance_0(&_S32, unit_0, _S28, cacheFrame_1, kernelContext_6);


    return grSample_0(_S33.radiance_2, _S33.confidence_3, _S33.source_2);
}


#line 133
struct GrCombine_0
{
    float3 total_0;
    float totalWeight_0;
    float confidence_4;
};


#line 139
GrCombine_0 grCombineBegin_0()
{

#line 140
    thread GrCombine_0 state_0;
    (&state_0)->total_0 = float3(0.0, 0.0, 0.0);
    (&state_0)->totalWeight_0 = 0.0;
    (&state_0)->confidence_4 = 0.0;
    return state_0;
}


#line 200
uint grReflectionRays_0(float roughness_0, KernelContext_0 thread* kernelContext_7)
{

#line 201
    uint4 words_0 = kernelContext_7->giResolve_0->constants_2[4U];
    float mirror_0 = (as_type<float>((words_0.x)));
    float cacheMin_0 = (as_type<float>((words_0.y)));
    uint _S34 = max(1U, words_0.z);
    if(roughness_0 <= mirror_0)
    {

#line 206
        return _S34;
    }
    if(roughness_0 < cacheMin_0)
    {

        float _S35 = float(_S34);


        return max(1U, uint(floor(_S35 + (1.0 - _S35) * gpClamp_0((roughness_0 - mirror_0) / max(cacheMin_0 - mirror_0, 0.00009999999747379), 0.0, 1.0) + 0.5)));
    }
    return 0U;
}


#line 73
uint grHash_0(uint pixel_0, uint frame_0, uint stream_0)
{

    uint hash_0 = ((pixel_0 * 2654435769U) ^ (frame_0 * 2246822507U)) ^ (stream_0 * 3266489909U);

    uint hash_1 = (hash_0 ^ (hash_0 >> 15U)) * 625341585U;

    return hash_1 ^ (hash_1 >> 13U);
}

float grUnit_0(uint hash_2)
{

#line 84
    return float(hash_2 & 16777215U) / 1.6777216e+07;
}


#line 109
bool grTraces_0(uint pixel_1, uint frame_1, float density_0)
{

#line 110
    if(density_0 >= 1.0)
    {

#line 111
        return true;
    }
    return (grUnit_0(grHash_0(pixel_1, frame_1, 5U))) < density_0;
}


#line 220
float3 grLobeDirection_0(float3 reflection_0, float3 normal_5, float roughness_1, uint index_0, uint count_0, uint rotation_0)
{
    float3 axis_0 = giNormalisedOr_0(reflection_0, normal_5);

#line 222
    bool _S36;
    if(count_0 <= 1U)
    {

#line 223
        _S36 = true;

#line 223
    }
    else
    {

#line 223
        _S36 = roughness_1 <= 0.0;

#line 223
    }

#line 223
    if(_S36)
    {

#line 224
        return axis_0;
    }

    float _S37 = float(index_0);

    float radius_1 = sqrt((_S37 + 0.5) / float(count_0)) * (gpClamp_0(roughness_1, 0.0, 1.0) * 0.69999998807907104);
    float angle_0 = 2.3999631404876709 * _S37 + float(rotation_0) * 0.6180340051651001;

#line 230
    float3 tangent_0;
    if((abs(axis_0.y)) < 0.99000000953674316)
    {

#line 231
        tangent_0 = cross(float3(0.0, 1.0, 0.0), axis_0);

#line 231
    }
    else
    {

#line 231
        tangent_0 = cross(float3(1.0, 0.0, 0.0), axis_0);

#line 231
    }

    float3 tangent_1 = giNormalisedOr_0(tangent_0, float3(1.0, 0.0, 0.0));



    float3 unit_1 = giNormalisedOr_0(axis_0 + tangent_1 * float3((radius_1 * cos(angle_0)))  + cross(axis_0, tangent_1) * float3((radius_1 * sin(angle_0))) , axis_0);
    float _S38 = dot(unit_1, normal_5);

#line 238
    if(_S38 < 0.0)
    {

#line 238
        tangent_0 = giNormalisedOr_0(unit_1 - normal_5 * float3((2.0 * _S38)) , axis_0);

#line 238
    }
    else
    {

#line 238
        tangent_0 = unit_1;

#line 238
    }

#line 238
    return tangent_0;
}


#line 263
GrSample_0 grGatherSample_0(const GrSample_0 thread* ray_1, const GrSample_0 thread* standIn_0, uint excluded_0)
{

#line 263
    uint _S39 = standIn_0->source_0;

#line 263
    bool hasStandIn_0;
    if((standIn_0->source_0) != 0U)
    {

#line 264
        hasStandIn_0 = (standIn_0->confidence_0) > 0.0;

#line 264
    }
    else
    {

#line 264
        hasStandIn_0 = false;

#line 264
    }

#line 264
    if(hasStandIn_0)
    {

#line 264
        hasStandIn_0 = (excluded_0 & (1U << _S39)) == 0U;

#line 264
    }
    else
    {

#line 264
        hasStandIn_0 = false;

#line 264
    }

    thread GrSample_0 sample_3 = *ray_1;
    if((ray_1->source_0) == 0U)
    {

#line 268
        if(!hasStandIn_0)
        {

#line 269
            return *ray_1;
        }
        (&sample_3)->source_0 = _S39;
        (&sample_3)->radiance_0 = standIn_0->radiance_0;

#line 267
    }
    else
    {



        if(hasStandIn_0)
        {
            (&sample_3)->radiance_0 = standIn_0->radiance_0 + (ray_1->radiance_0 - standIn_0->radiance_0) * float3(gpClamp_0(ray_1->confidence_0, 0.0, 1.0)) ;

#line 273
        }

#line 267
    }

#line 277
    (&sample_3)->confidence_0 = 1.0;
    return sample_3;
}


#line 147
void grCombineAdd_0(GrCombine_0 thread* state_1, const GrSample_0 thread* sample_4, uint excluded_1)
{

#line 147
    uint _S40 = sample_4->source_0;
    if((sample_4->source_0) == 0U)
    {

#line 149
        return;
    }
    if((excluded_1 & (1U << _S40)) != 0U)
    {

#line 152
        return;
    }
    float weight_0 = gpClamp_0(sample_4->confidence_0, 0.0, 1.0);
    if(weight_0 <= 0.0)
    {

#line 156
        return;
    }
    state_1->total_0 = state_1->total_0 + sample_4->radiance_0 * float3(weight_0) ;
    state_1->totalWeight_0 = state_1->totalWeight_0 + weight_0;
    state_1->confidence_4 = max(state_1->confidence_4, weight_0);
    return;
}


#line 163
float3 grCombineEnd_0(const GrCombine_0 thread* state_2)
{

#line 163
    float _S41 = state_2->totalWeight_0;

#line 163
    float3 _S42;
    if((state_2->totalWeight_0) > 0.0)
    {

#line 164
        _S42 = state_2->total_0 / float3(_S41) ;

#line 164
    }
    else
    {

#line 164
        _S42 = float3(0.0, 0.0, 0.0);

#line 164
    }

#line 164
    return _S42;
}


#line 431
float3 grResolveSpecular_0(uint pixel_2, float3 position_3, float3 normal_6, float roughness_2, float3 reflection_1, float4 cached_0, uint frame_2, KernelContext_0 thread* kernelContext_8)
{

    float4 rays_1 = (as_type<float4>((kernelContext_8->giResolve_0->constants_2[2U])));
    float density_1 = (as_type<float>((kernelContext_8->giResolve_0->constants_2[1U].w)));
    thread GrCombine_0 state_3 = grCombineBegin_0();


    GrSample_0 _S43 = grSample_0(float3(0.0, 0.0, 0.0), 0.0, 0U);
    float _S44 = cached_0.w;

#line 440
    GrSample_0 cache_0;

#line 440
    if(_S44 > 0.0)
    {

#line 440
        cache_0 = grSample_0(cached_0.xyz, _S44 * gpClamp_0(roughness_2, 0.15000000596046448, 1.0), 3U);

#line 440
    }
    else
    {

#line 440
        cache_0 = _S43;

#line 440
    }

#line 440
    uint _S45 = grReflectionRays_0(roughness_2, kernelContext_8);

#line 440
    bool _S46;



    if(_S45 > 0U)
    {

#line 444
        _S46 = grTraces_0(pixel_2, frame_2, density_1);

#line 444
    }
    else
    {

#line 444
        _S46 = false;

#line 444
    }

#line 444
    if(_S46)
    {

        uint _S47 = grHash_0(pixel_2, frame_2, 7U) % _S45;

#line 447
        GrSample_0 _S48 = grWorldRay_0(position_3, normal_6, giNormalisedOr_0(grLobeDirection_0(reflection_1, normal_6, roughness_2, _S47, _S45, frame_2), float3(0.0, 0.0, -1.0)), rays_1.y, rays_1.x, kernelContext_8);

#line 447
        thread GrSample_0 _S49 = _S48;

#line 447
        thread GrSample_0 _S50 = cache_0;

#line 447
        GrSample_0 _S51 = grGatherSample_0(&_S49, &_S50, 0U);

#line 447
        thread GrSample_0 _S52 = _S51;

#line 447
        grCombineAdd_0(&state_3, &_S52, 0U);

#line 444
    }

#line 454
    if((cache_0.confidence_0) > 0.0)
    {

#line 454
        thread GrSample_0 _S53 = cache_0;

#line 454
        grCombineAdd_0(&state_3, &_S53, 0U);

#line 454
    }

#line 454
    thread GrCombine_0 _S54 = state_3;

#line 454
    float3 _S55 = grCombineEnd_0(&_S54);


    return _S55 * float3((as_type<float>((kernelContext_8->giResolve_0->constants_2[5U].y)))) ;
}


#line 88
void grBasis_0(float3 normal_7, float3 thread* tangent_2, float3 thread* bitangent_0)
{

#line 89
    float _S56 = normal_7.z;

#line 89
    float sign_0;

#line 89
    if(_S56 >= 0.0)
    {

#line 89
        sign_0 = 1.0;

#line 89
    }
    else
    {

#line 89
        sign_0 = -1.0;

#line 89
    }
    float a_0 = -1.0 / (sign_0 + _S56);
    float _S57 = normal_7.x;

#line 91
    float _S58 = normal_7.y;

#line 91
    float b_0 = _S57 * _S58 * a_0;
    *tangent_2 = float3(1.0 + sign_0 * _S57 * _S57 * a_0, sign_0 * b_0, - sign_0 * _S57);
    *bitangent_0 = float3(b_0, sign_0 + _S58 * _S58 * a_0, - _S58);
    return;
}


#line 96
float3 grCosineDirection_0(float3 normal_8, float u1_0, float u2_0)
{

#line 97
    thread float3 tangent_3;
    thread float3 bitangent_1;
    grBasis_0(normal_8, &tangent_3, &bitangent_1);
    float radius_2 = sqrt(u1_0);
    float angle_1 = 6.28318548202514648 * u2_0;

    return giNormalisedOr_0(tangent_3 * float3((radius_2 * cos(angle_1)))  + bitangent_1 * float3((radius_2 * sin(angle_1)))  + normal_8 * float3(sqrt(max(0.0, 1.0 - u1_0))) , normal_8);
}


#line 168
float grFarFieldWeight_0(float distance_0, KernelContext_0 thread* kernelContext_9)
{

#line 169
    float4 far_0 = (as_type<float4>((kernelContext_9->giResolve_0->constants_2[3U])));
    float begin_0 = far_0.x;
    if(begin_0 <= 0.0)
    {

#line 172
        return 1.0;
    }
    float _S59 = far_0.y;

#line 174
    float ratio_0;

#line 174
    if(_S59 > 0.0)
    {

#line 174
        ratio_0 = far_0.z / _S59;

#line 174
    }
    else
    {

#line 174
        ratio_0 = 8.0;

#line 174
    }
    float width_0 = begin_0 * gpClamp_0(ratio_0 / 100.0, 0.15000000596046448, 1.0);
    if(distance_0 <= begin_0)
    {

#line 177
        return 0.0;
    }
    float t_2 = gpClamp_0((distance_0 - begin_0) / width_0, 0.0, 1.0);
    return t_2 * t_2 * (3.0 - 2.0 * t_2);
}


#line 115 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
uint giLevelCount_0(KernelContext_0 thread* kernelContext_10)
{

#line 116
    return kernelContext_10->giScene_0->constants_1[0U].x;
}


#line 278
float3 giHemisphereDirection_0(float3 normal_9, uint index_1, uint count_1)
{
    float _S60 = float(index_1);
    float radius_3 = sqrt((_S60 + 0.5) / float(count_1));
    float angle_2 = 2.3999631404876709 * _S60;
    float x_3 = radius_3 * cos(angle_2);
    float y_3 = radius_3 * sin(angle_2);
    float z_2 = sqrt(max(0.0, 1.0 - x_3 * x_3 - y_3 * y_3));

#line 285
    float3 tangent_4;
    if((abs(normal_9.y)) < 0.99000000953674316)
    {

#line 286
        tangent_4 = cross(float3(0.0, 1.0, 0.0), normal_9);

#line 286
    }
    else
    {

#line 286
        tangent_4 = cross(float3(1.0, 0.0, 0.0), normal_9);

#line 286
    }

    float3 tangent_5 = giNormalisedOr_0(tangent_4, float3(1.0, 0.0, 0.0));

    return giNormalisedOr_0(tangent_5 * float3(x_3)  + cross(normal_9, tangent_5) * float3(y_3)  + normal_9 * float3(z_2) , normal_9);
}


#line 215
struct GiTraceHit_0
{
    bool hit_3;
    bool exhausted_0;
    float t_3;
    float closest_0;
    float3 position_4;
    float3 normal_10;
};


#line 115
uint giLevelCount_1(KernelContext_0 thread* kernelContext_11)
{

#line 116
    return kernelContext_11->giScene_0->constants_1[0U].x;
}

uint giWindowBricks_0(KernelContext_0 thread* kernelContext_12)
{

#line 120
    return kernelContext_12->giScene_0->constants_1[0U].y;
}


#line 167
int giWrap_0(int value_2, int modulus_0)
{

#line 168
    uint _S61 = uint(modulus_0);
    uint _S62 = (uint(value_2) + (_S61 << 20U)) % _S61;

#line 169
    return int(_S62);
}


#line 95
float giLerp_0(float a_1, float b_1, float t_4)
{

#line 96
    return a_1 + (b_1 - a_1) * t_4;
}


#line 128
float giTrilinear_0(uint slot_0, float fx_0, float fy_0, float fz_0, KernelContext_0 thread* kernelContext_13)
{
    int x0_0 = clamp(int(floor(fx_0)), int(0), int(3));
    int y0_0 = clamp(int(floor(fy_0)), int(0), int(3));
    int z0_0 = clamp(int(floor(fz_0)), int(0), int(3));



    float tx_0 = clamp(fx_0 - float(x0_0), 0.0, 1.0);
    float ty_0 = clamp(fy_0 - float(y0_0), 0.0, 1.0);

    uint base_0 = slot_0 * 125U;

    uint _S63 = uint(z0_0) * 5U;

#line 141
    uint _S64 = uint(y0_0);

#line 141
    uint _S65 = base_0 + (_S63 + _S64) * 5U;

#line 141
    uint _S66 = uint(x0_0);
    uint _S67 = uint(x0_0 + int(1));
    uint _S68 = uint(y0_0 + int(1));

#line 143
    uint _S69 = base_0 + (_S63 + _S68) * 5U;

    uint _S70 = uint(z0_0 + int(1)) * 5U;

#line 145
    uint _S71 = base_0 + (_S70 + _S64) * 5U;

    uint _S72 = base_0 + (_S70 + _S68) * 5U;

#line 153
)cy_msl",
    R"cy_msl(    return giLerp_0(giLerp_0(giLerp_0(kernelContext_13->giScene_0->bricks_0[_S65 + _S66], kernelContext_13->giScene_0->bricks_0[_S65 + _S67], tx_0), giLerp_0(kernelContext_13->giScene_0->bricks_0[_S69 + _S66], kernelContext_13->giScene_0->bricks_0[_S69 + _S67], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_13->giScene_0->bricks_0[_S71 + _S66], kernelContext_13->giScene_0->bricks_0[_S71 + _S67], tx_0), giLerp_0(kernelContext_13->giScene_0->bricks_0[_S72 + _S66], kernelContext_13->giScene_0->bricks_0[_S72 + _S67], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 175
float giSampleLevel_0(uint level_0, float3 p_0, float thread* farDistance_0, KernelContext_0 thread* kernelContext_14)
{

#line 176
    uint _S73 = 16U + level_0 * 2U;

#line 176
    float4 shape_0 = (as_type<float4>((kernelContext_14->giScene_0->constants_1[_S73])));
    int4 origin_1 = (as_type<int4>((kernelContext_14->giScene_0->constants_1[_S73 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_0.x / brick_0));
    int by_0 = int(floor(p_0.y / brick_0));
    int bz_0 = int(floor(p_0.z / brick_0));

#line 183
    uint _S74 = giWindowBricks_0(kernelContext_14);
    int dim_0 = int(_S74);
    int _S75 = origin_1.x;

#line 185
    bool _S76;

#line 185
    if(bx_0 < _S75)
    {

#line 185
        _S76 = true;

#line 185
    }
    else
    {

#line 185
        _S76 = by_0 < (origin_1.y);

#line 185
    }

#line 185
    if(_S76)
    {

#line 185
        _S76 = true;

#line 185
    }
    else
    {

#line 185
        _S76 = bz_0 < (origin_1.z);

#line 185
    }

#line 185
    if(_S76)
    {

#line 185
        _S76 = true;

#line 185
    }
    else
    {

#line 185
        _S76 = bx_0 >= (_S75 + dim_0);

#line 185
    }

#line 185
    if(_S76)
    {

#line 185
        _S76 = true;

#line 185
    }
    else
    {

#line 185
        _S76 = by_0 >= (origin_1.y + dim_0);

#line 185
    }
    if(_S76)
    {

#line 186
        _S76 = true;

#line 186
    }
    else
    {

#line 186
        _S76 = bz_0 >= (origin_1.z + dim_0);

#line 186
    }

#line 185
    if(_S76)
    {
        return *farDistance_0;
    }
    int _S77 = giWrap_0(bz_0, dim_0);

#line 189
    uint _S78 = uint(dim_0);

#line 189
    uint _S79 = uint(_S77) * _S78;

#line 189
    int _S80 = giWrap_0(by_0, dim_0);

#line 189
    uint _S81 = (_S79 + uint(_S80)) * _S78;
    int _S82 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_14->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S81 + uint(_S82))];
    if(slot_1 == 4294967295U)
    {

#line 193
        return *farDistance_0;
    }

    float3 local_1 = (p_0 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 196
    float _S83 = giTrilinear_0(slot_1, local_1.x, local_1.y, local_1.z, kernelContext_14);
    return _S83;
}


float giDistance_0(float3 p_1, KernelContext_0 thread* kernelContext_15)
{

#line 201
    uint _S84 = giLevelCount_1(kernelContext_15);

#line 201
    float answer_0 = 1.0e+06;

#line 201
    uint level_1 = 0U;


    for(;;)
    {

#line 204
        if(level_1 < _S84)
        {
        }
        else
        {

#line 204
            break;
        }

#line 205
        thread float farDistance_1 = 0.0;

#line 205
        float _S85 = giSampleLevel_0(level_1, p_1, &farDistance_1, kernelContext_15);

        float _S86 = min(answer_0, _S85);
        if(_S85 < farDistance_1)
        {

#line 209
            return _S85;
        }

#line 204
        uint level_2 = level_1 + 1U;

#line 204
        answer_0 = _S86;

#line 204
        level_1 = level_2;

#line 204
    }

#line 212
    return answer_0;
}


#line 225
GiTraceHit_0 giSphereTrace_0(float3 origin_2, float3 direction_3, float maxDistance_1, float tMin_0, KernelContext_0 thread* kernelContext_16)
{

#line 226
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_3 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_3 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_4 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_10 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S87 = giLevelCount_0(kernelContext_16);
    if(_S87 == 0U)
    {

#line 234
        return result_0;
    }

#line 234
    float _S88 = giFinestVoxel_0(kernelContext_16);


    float surface_0 = _S88 * 0.5;
    float _S89 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 239
    recent_0[int(0)] = 1.0e+06;

#line 239
    recent_0[int(1)] = 1.0e+06;

#line 239
    recent_0[int(2)] = 1.0e+06;

#line 239
    float closest_1 = 1.0e+06;

#line 239
    uint recentSlot_0 = 0U;

#line 239
    uint step_0 = 0U;

#line 239
    float t_5 = _S89;


    for(;;)
    {

#line 242
        if(step_0 < 192U)
        {
        }
        else
        {

#line 242
            break;
        }

#line 243
        if(t_5 > maxDistance_1)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_5 = origin_2 + direction_3 * float3(t_5) ;

#line 248
        float _S90 = giDistance_0(position_5, kernelContext_16);

        if(_S90 < surface_0)
        {

#line 251
            (&result_0)->hit_3 = true;
            (&result_0)->t_3 = t_5;
            (&result_0)->position_4 = position_5;


            float3 _S91 = float3(_S88, 0.0, 0.0);

#line 256
            float _S92 = giDistance_0(position_5 + _S91, kernelContext_16);

#line 256
            float _S93 = giDistance_0(position_5 - _S91, kernelContext_16);

#line 256
            float _S94 = _S92 - _S93;

            float3 _S95 = float3(0.0, _S88, 0.0);

#line 258
            float _S96 = giDistance_0(position_5 + _S95, kernelContext_16);

#line 258
            float _S97 = giDistance_0(position_5 - _S95, kernelContext_16);

#line 258
            float _S98 = _S96 - _S97;

            float3 _S99 = float3(0.0, 0.0, _S88);

#line 260
            float _S100 = giDistance_0(position_5 + _S99, kernelContext_16);

#line 260
            float _S101 = giDistance_0(position_5 - _S99, kernelContext_16);

            (&result_0)->normal_10 = giNormalisedOr_0(float3(_S94, _S98, _S100 - _S101), - direction_3);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S102 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S90;
        uint _S103 = (recentSlot_0 + 1U) % 3U;
        float t_6 = t_5 + max(_S90, surface_0);

#line 242
        uint step_1 = step_0 + 1U;

#line 242
        closest_1 = _S102;

#line 242
        recentSlot_0 = _S103;

#line 242
        step_0 = step_1;

#line 242
        t_5 = t_6;

#line 242
    }

#line 271
    (&result_0)->exhausted_0 = true;

    (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
    return result_0;
}


#line 184 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float grSkyVisibilityRay_0(float3 position_6, float3 normal_11, uint index_2, uint count_2, KernelContext_0 thread* kernelContext_17)
{

#line 184
    bool _S104;
    if(count_2 == 0U)
    {

#line 185
        _S104 = true;

#line 185
    }
    else
    {

#line 185
        uint _S105 = giLevelCount_0(kernelContext_17);

#line 185
        _S104 = _S105 == 0U;

#line 185
    }

#line 185
    if(_S104)
    {

#line 186
        return 1.0;
    }
    float reach_1 = (as_type<float>((kernelContext_17->giResolve_0->constants_2[3U].w)));

#line 188
    float _S106 = giFinestVoxel_0(kernelContext_17);

#line 188
    GiTraceHit_0 _S107 = giSphereTrace_0(position_6 + normal_11 * float3((_S106 * 2.0)) , giHemisphereDirection_0(normal_11, index_2, count_2), reach_1, 0.0, kernelContext_17);

#line 193
    if(_S107.hit_3)
    {

#line 194
        return 0.0;
    }
    return gpClamp_0(_S107.closest_0 / (_S106 * 4.0), 0.0, 1.0);
}


#line 393
float3 grResolveDiffuse_0(uint pixel_3, float3 position_7, float3 normal_12, float distance_1, float4 cached_1, uint frame_3, KernelContext_0 thread* kernelContext_18)
{
    float4 rays_2 = (as_type<float4>((kernelContext_18->giResolve_0->constants_2[2U])));
    uint excluded_2 = kernelContext_18->giResolve_0->constants_2[2U].w;
    float density_2 = (as_type<float>((kernelContext_18->giResolve_0->constants_2[1U].w)));
    thread GrCombine_0 state_4 = grCombineBegin_0();

    GrSample_0 cache_1 = grSample_0(cached_1.xyz, cached_1.w, 3U);

#line 400
    thread GrSample_0 _S108 = cache_1;

#line 400
    grCombineAdd_0(&state_4, &_S108, excluded_2);


    if(grTraces_0(pixel_3, frame_3, density_2))
    {

#line 404
        uint hash_3 = grHash_0(pixel_3, frame_3, 6U);

#line 404
        GrSample_0 _S109 = grWorldRay_0(position_7, normal_12, giNormalisedOr_0(grCosineDirection_0(normal_12, grUnit_0(hash_3), grUnit_0(hash_3 * 656542357U)), float3(0.0, 0.0, -1.0)), rays_2.y, rays_2.x, kernelContext_18);

#line 404
        thread GrSample_0 _S110 = _S109;

#line 404
        thread GrSample_0 _S111 = cache_1;

#line 404
        GrSample_0 _S112 = grGatherSample_0(&_S110, &_S111, excluded_2);

#line 404
        thread GrSample_0 _S113 = _S112;

#line 404
        grCombineAdd_0(&state_4, &_S113, excluded_2);

#line 403
    }

#line 415
    thread GrSample_0 sky_0 = grSample_0(float3(0.0, 0.0, 0.0), 0.0, 9U);

#line 415
    float _S114 = grFarFieldWeight_0(distance_1, kernelContext_18);

    if(_S114 > 0.0)
    {
        uint count_3 = (as_type<uint>((rays_2.z)));

#line 419
        float openness_0;
        if(count_3 > 0U)
        {

#line 421
            uint _S115 = (frame_3 + pixel_3) % count_3;

#line 421
            float _S116 = grSkyVisibilityRay_0(position_7, normal_12, _S115, count_3, kernelContext_18);

#line 421
            openness_0 = _S116;

#line 420
        }
        else
        {

#line 420
            openness_0 = 1.0;

#line 420
        }

#line 420
        float3 _S117 = gpSky_0(normal_12, kernelContext_18);


        (&sky_0)->radiance_0 = _S117 * float3(openness_0) ;
        (&sky_0)->confidence_0 = gpClamp_0(_S114 * openness_0, 0.0, 1.0);

#line 417
    }

#line 417
    thread GrSample_0 _S118 = sky_0;

#line 417
    grCombineAdd_0(&state_4, &_S118, excluded_2);

#line 417
    thread GrCombine_0 _S119 = state_4;

#line 417
    float3 _S120 = grCombineEnd_0(&_S119);

#line 427
    return _S120 * float3((as_type<float>((kernelContext_18->giResolve_0->constants_2[5U].x)))) ;
}


#line 440 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
int gpFloorDiv_0(float value_3, float divisor_0)
{

#line 441
    return int(floor(value_3 / divisor_0));
}


#line 487
uint gpLayoutProbe_0(uint level_3, int3 cell_2, KernelContext_0 thread* kernelContext_19)
{

#line 488
    int3 centre_0 = (as_type<int3>((kernelContext_19->giProbes_0->constants_0[6U + level_3].xyz)));
    int half_0 = int(kernelContext_19->giProbes_0->constants_0[1U].w);
    int3 _S121 = int3(half_0, half_0, half_0);

#line 490
    bool _S122;

#line 490
    if(any(cell_2 < (centre_0 - _S121)))
    {

#line 490
        _S122 = true;

#line 490
    }
    else
    {

#line 490
        _S122 = any(cell_2 > (centre_0 + _S121));

#line 490
    }

#line 490
    if(_S122)
    {

#line 491
        return 4294967295U;
    }
    int edge_0 = int(kernelContext_19->giProbes_0->constants_0[5U].z);
    uint cells_0 = uint(edge_0 * edge_0 * edge_0);
    int _S123 = giWrap_0(cell_2.z, edge_0);

#line 495
    uint _S124 = uint(edge_0);

#line 495
    uint _S125 = uint(_S123) * _S124;

#line 495
    int _S126 = giWrap_0(cell_2.y, edge_0);

#line 495
    uint _S127 = (_S125 + uint(_S126)) * _S124;

    int _S128 = giWrap_0(cell_2.x, edge_0);
    return kernelContext_19->giProbes_0->layout_0[level_3 * cells_0 + (_S127 + uint(_S128))];
}


#line 114
constant array<float3, int(6)> kGpAxes_0 = { float3(1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, -1.0, 0.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, -1.0) };

#line 445
float gpVisibilityWeight_0(uint probe_0, float3 position_8, float3 query_1, KernelContext_0 thread* kernelContext_20)
{

#line 446
    float3 offset_1 = query_1 - position_8;
    float span_0 = length(offset_1);
    if(span_0 <= 0.00009999999747379)
    {

#line 449
        return 1.0;
    }
    float3 _S129 = offset_1 / float3(span_0) ;
    uint _S130 = probe_0 * 4U;

#line 452
    float4 device* _S131 = kernelContext_20->giProbes_0->probes_0+(_S130 + 2U);
    float4 device* _S132 = kernelContext_20->giProbes_0->probes_0+(_S130 + 3U);
    array<float, int(6)> _S133 = { (*_S131).x, (*_S131).y, (*_S131).z, (*_S131).w, (*_S132).x, (*_S132).y };

#line 454
    uint axis_1 = 0U;

#line 454
    float expected_0 = 0.0;

#line 454
    float weight_1 = 0.0;


    for(;;)
    {

#line 457
        if(axis_1 < 6U)
        {
        }
        else
        {

#line 457
            break;
        }

#line 458
        float _S134 = max(0.0, dot(kGpAxes_0[axis_1], _S129));
        float expected_1 = expected_0 + _S133[axis_1] * _S134;
        float weight_2 = weight_1 + _S134;

#line 457
        axis_1 = axis_1 + 1U;

#line 457
        expected_0 = expected_1;

#line 457
        weight_1 = weight_2;

#line 457
    }

#line 462
    if(weight_1 <= 0.0)
    {

#line 463
        return 1.0;
    }



    float slack_0 = (as_type<float>((kernelContext_20->giProbes_0->constants_0[1U].y)));
    float _S135 = expected_0 / weight_1 + slack_0;

#line 469
    if(span_0 > _S135)
    {

#line 470
        return 0.0;
    }
    float _S136 = max(slack_0, 0.00009999999747379);

#line 472
    float visible_0 = gpClamp_0((_S135 - span_0) / _S136, 0.0, 1.0);

#line 472
    axis_1 = 0U;

    for(;;)
    {

#line 474
        if(axis_1 < 6U)
        {
        }
        else
        {

#line 474
            break;
        }

#line 475
        float cosine_0 = dot(kGpAxes_0[axis_1], _S129);
        if(cosine_0 < 0.5)
        {

#line 477
            axis_1 = axis_1 + 1U;

#line 474
            continue;
        }

#line 474
        visible_0 = min(visible_0, gpClamp_0((_S133[axis_1] + slack_0 - span_0 * cosine_0) / _S136, 0.0, 1.0));

#line 474
        axis_1 = axis_1 + 1U;

#line 474
    }

#line 483
    return visible_0;
}


#line 300 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float grFacing_0(float3 toward_0, float3 normal_13)
{

#line 301
    return gpClamp_0((dot(normal_13, toward_0) + 1.0) * 0.5, 0.0, 1.0);
}


#line 123 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float giFinestVoxel_1(KernelContext_0 thread* kernelContext_21)
{

#line 124
    return (as_type<float>((kernelContext_21->giScene_0->constants_1[9U].y)));
}


#line 119
uint giWindowBricks_1(KernelContext_0 thread* kernelContext_22)
{

#line 120
    return kernelContext_22->giScene_0->constants_1[0U].y;
}


#line 128
float giTrilinear_1(uint slot_2, float fx_1, float fy_1, float fz_1, KernelContext_0 thread* kernelContext_23)
{
    int x0_1 = clamp(int(floor(fx_1)), int(0), int(3));
    int y0_1 = clamp(int(floor(fy_1)), int(0), int(3));
    int z0_1 = clamp(int(floor(fz_1)), int(0), int(3));



    float tx_1 = clamp(fx_1 - float(x0_1), 0.0, 1.0);
    float ty_1 = clamp(fy_1 - float(y0_1), 0.0, 1.0);

    uint base_1 = slot_2 * 125U;

    uint _S137 = uint(z0_1) * 5U;

#line 141
    uint _S138 = uint(y0_1);

#line 141
    uint _S139 = base_1 + (_S137 + _S138) * 5U;

#line 141
    uint _S140 = uint(x0_1);
    uint _S141 = uint(x0_1 + int(1));
    uint _S142 = uint(y0_1 + int(1));

#line 143
    uint _S143 = base_1 + (_S137 + _S142) * 5U;

    uint _S144 = uint(z0_1 + int(1)) * 5U;

#line 145
    uint _S145 = base_1 + (_S144 + _S138) * 5U;

    uint _S146 = base_1 + (_S144 + _S142) * 5U;

#line 153
)cy_msl",
    R"cy_msl(    return giLerp_0(giLerp_0(giLerp_0(kernelContext_23->giScene_0->bricks_0[_S139 + _S140], kernelContext_23->giScene_0->bricks_0[_S139 + _S141], tx_1), giLerp_0(kernelContext_23->giScene_0->bricks_0[_S143 + _S140], kernelContext_23->giScene_0->bricks_0[_S143 + _S141], tx_1), ty_1), giLerp_0(giLerp_0(kernelContext_23->giScene_0->bricks_0[_S145 + _S140], kernelContext_23->giScene_0->bricks_0[_S145 + _S141], tx_1), giLerp_0(kernelContext_23->giScene_0->bricks_0[_S146 + _S140], kernelContext_23->giScene_0->bricks_0[_S146 + _S141], tx_1), ty_1), clamp(fz_1 - float(z0_1), 0.0, 1.0));
}


#line 175
float giSampleLevel_1(uint level_4, float3 p_2, float thread* farDistance_2, KernelContext_0 thread* kernelContext_24)
{

#line 176
    uint _S147 = 16U + level_4 * 2U;

#line 176
    float4 shape_1 = (as_type<float4>((kernelContext_24->giScene_0->constants_1[_S147])));
    int4 origin_3 = (as_type<int4>((kernelContext_24->giScene_0->constants_1[_S147 + 1U])));
    float voxel_1 = shape_1.x;
    float brick_1 = shape_1.y;
    *farDistance_2 = shape_1.z;
    int bx_1 = int(floor(p_2.x / brick_1));
    int by_1 = int(floor(p_2.y / brick_1));
    int bz_1 = int(floor(p_2.z / brick_1));

#line 183
    uint _S148 = giWindowBricks_1(kernelContext_24);
    int dim_1 = int(_S148);
    int _S149 = origin_3.x;

#line 185
    bool _S150;

#line 185
    if(bx_1 < _S149)
    {

#line 185
        _S150 = true;

#line 185
    }
    else
    {

#line 185
        _S150 = by_1 < (origin_3.y);

#line 185
    }

#line 185
    if(_S150)
    {

#line 185
        _S150 = true;

#line 185
    }
    else
    {

#line 185
        _S150 = bz_1 < (origin_3.z);

#line 185
    }

#line 185
    if(_S150)
    {

#line 185
        _S150 = true;

#line 185
    }
    else
    {

#line 185
        _S150 = bx_1 >= (_S149 + dim_1);

#line 185
    }

#line 185
    if(_S150)
    {

#line 185
        _S150 = true;

#line 185
    }
    else
    {

#line 185
        _S150 = by_1 >= (origin_3.y + dim_1);

#line 185
    }
    if(_S150)
    {

#line 186
        _S150 = true;

#line 186
    }
    else
    {

#line 186
        _S150 = bz_1 >= (origin_3.z + dim_1);

#line 186
    }

#line 185
    if(_S150)
    {
        return *farDistance_2;
    }
    int _S151 = giWrap_0(bz_1, dim_1);

#line 189
    uint _S152 = uint(dim_1);

#line 189
    uint _S153 = uint(_S151) * _S152;

#line 189
    int _S154 = giWrap_0(by_1, dim_1);

#line 189
    uint _S155 = (_S153 + uint(_S154)) * _S152;
    int _S156 = giWrap_0(bx_1, dim_1);
    uint slot_3 = kernelContext_24->giScene_0->pageTable_0[level_4 * uint(dim_1 * dim_1 * dim_1) + (_S155 + uint(_S156))];
    if(slot_3 == 4294967295U)
    {

#line 193
        return *farDistance_2;
    }

    float3 local_2 = (p_2 - float3(float(bx_1) * brick_1, float(by_1) * brick_1, float(bz_1) * brick_1)) / float3(voxel_1) ;

#line 196
    float _S157 = giTrilinear_1(slot_3, local_2.x, local_2.y, local_2.z, kernelContext_24);
    return _S157;
}


float giDistance_1(float3 p_3, KernelContext_0 thread* kernelContext_25)
{

#line 201
    uint _S158 = giLevelCount_0(kernelContext_25);

#line 201
    float answer_1 = 1.0e+06;

#line 201
    uint level_5 = 0U;


    for(;;)
    {

#line 204
        if(level_5 < _S158)
        {
        }
        else
        {

#line 204
            break;
        }

#line 205
        thread float farDistance_3 = 0.0;

#line 205
        float _S159 = giSampleLevel_1(level_5, p_3, &farDistance_3, kernelContext_25);

        float _S160 = min(answer_1, _S159);
        if(_S159 < farDistance_3)
        {

#line 209
            return _S159;
        }

#line 204
        uint level_6 = level_5 + 1U;

#line 204
        answer_1 = _S160;

#line 204
        level_5 = level_6;

#line 204
    }

#line 212
    return answer_1;
}


#line 225
GiTraceHit_0 giSphereTrace_1(float3 origin_4, float3 direction_4, float maxDistance_2, float tMin_1, KernelContext_0 thread* kernelContext_26)
{

#line 226
    thread GiTraceHit_0 result_1;
    (&result_1)->hit_3 = false;
    (&result_1)->exhausted_0 = false;
    (&result_1)->t_3 = 0.0;
    (&result_1)->closest_0 = 0.0;
    (&result_1)->position_4 = float3(0.0, 0.0, 0.0);
    (&result_1)->normal_10 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S161 = giLevelCount_0(kernelContext_26);
    if(_S161 == 0U)
    {

#line 234
        return result_1;
    }

#line 234
    float _S162 = giFinestVoxel_0(kernelContext_26);


    float surface_1 = _S162 * 0.5;
    float _S163 = max(tMin_1, surface_1);
    thread array<float, int(3)> recent_1;

#line 239
    recent_1[int(0)] = 1.0e+06;

#line 239
    recent_1[int(1)] = 1.0e+06;

#line 239
    recent_1[int(2)] = 1.0e+06;

#line 239
    float closest_2 = 1.0e+06;

#line 239
    uint recentSlot_1 = 0U;

#line 239
    uint step_2 = 0U;

#line 239
    float t_7 = _S163;


    for(;;)
    {

#line 242
        if(step_2 < 192U)
        {
        }
        else
        {

#line 242
            break;
        }

#line 243
        if(t_7 > maxDistance_2)
        {
            (&result_1)->closest_0 = min(closest_2, min(recent_1[int(0)], min(recent_1[int(1)], recent_1[int(2)])));
            return result_1;
        }
        float3 position_9 = origin_4 + direction_4 * float3(t_7) ;

#line 248
        float _S164 = giDistance_0(position_9, kernelContext_26);

        if(_S164 < surface_1)
        {

#line 251
            (&result_1)->hit_3 = true;
            (&result_1)->t_3 = t_7;
            (&result_1)->position_4 = position_9;


            float3 _S165 = float3(_S162, 0.0, 0.0);

#line 256
            float _S166 = giDistance_0(position_9 + _S165, kernelContext_26);

#line 256
            float _S167 = giDistance_0(position_9 - _S165, kernelContext_26);

#line 256
            float _S168 = _S166 - _S167;

            float3 _S169 = float3(0.0, _S162, 0.0);

#line 258
            float _S170 = giDistance_0(position_9 + _S169, kernelContext_26);

#line 258
            float _S171 = giDistance_0(position_9 - _S169, kernelContext_26);

#line 258
            float _S172 = _S170 - _S171;

            float3 _S173 = float3(0.0, 0.0, _S162);

#line 260
            float _S174 = giDistance_0(position_9 + _S173, kernelContext_26);

#line 260
            float _S175 = giDistance_0(position_9 - _S173, kernelContext_26);

            (&result_1)->normal_10 = giNormalisedOr_0(float3(_S168, _S172, _S174 - _S175), - direction_4);
            (&result_1)->closest_0 = closest_2;
            return result_1;
        }
        float _S176 = min(closest_2, recent_1[recentSlot_1]);
        recent_1[recentSlot_1] = _S164;
        uint _S177 = (recentSlot_1 + 1U) % 3U;
        float t_8 = t_7 + max(_S164, surface_1);

#line 242
        uint step_3 = step_2 + 1U;

#line 242
        closest_2 = _S176;

#line 242
        recentSlot_1 = _S177;

#line 242
        step_2 = step_3;

#line 242
        t_7 = t_8;

#line 242
    }

#line 271
    (&result_1)->exhausted_0 = true;

    (&result_1)->closest_0 = min(closest_2, min(recent_1[int(0)], min(recent_1[int(1)], recent_1[int(2)])));
    return result_1;
}


#line 286 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
bool grProbeOccluded_0(float3 from_0, float3 to_0, KernelContext_0 thread* kernelContext_27)
{

#line 287
    float3 offset_2 = to_0 - from_0;
    float span_1 = length(offset_2);

#line 288
    float _S178 = giFinestVoxel_1(kernelContext_27);

    if(span_1 <= (_S178 * 2.0))
    {

#line 291
        return false;
    }

#line 291
    float _S179 = giDistance_1(from_0, kernelContext_27);

#line 291
    float _S180 = giDistance_1(to_0, kernelContext_27);

    if((_S179 + _S180) >= span_1)
    {

#line 294
        return false;
    }

#line 294
    GiTraceHit_0 _S181 = giSphereTrace_1(from_0, offset_2 / float3(span_1) , span_1 - _S178, 0.0, kernelContext_27);

    return _S181.hit_3;
}


#line 273 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
void gpLoadPayload_0(uint probe_1, uint floats_0, array<float, int(48)> thread* payload_0, KernelContext_0 thread* kernelContext_28)
{

#line 273
    uint index_3 = 0U;
    for(;;)
    {

#line 274
        if(index_3 < 48U)
        {
        }
        else
        {

#line 274
            break;
        }

#line 274
        float _S182;
        if(index_3 < floats_0)
        {

#line 275
            _S182 = *(kernelContext_28->giProbes_0->payloads_0+(probe_1 * floats_0 + index_3));

#line 275
        }
        else
        {

#line 275
            _S182 = 0.0;

#line 275
        }

#line 275
        (*payload_0)[index_3] = _S182;

#line 274
        index_3 = index_3 + 1U;

#line 274
    }


    return;
}


#line 110
float3 gpMax0_0(float3 v_1)
{

#line 111
    return float3(max(0.0, v_1.x), max(0.0, v_1.y), max(0.0, v_1.z));
}


#line 154
void gpOctahedralUv_0(float3 direction_5, float thread* u_0, float thread* v_2)
{

#line 155
    float sum_0 = abs(direction_5.x) + abs(direction_5.y) + abs(direction_5.z);

#line 155
    float3 p_4;
    if(sum_0 > 0.0)
    {

#line 156
        p_4 = direction_5 / float3(sum_0) ;

#line 156
    }
    else
    {

#line 156
        p_4 = float3(0.0, 0.0, 1.0);

#line 156
    }
    float _S183 = p_4.x;
    float _S184 = p_4.y;

#line 158
    float px_0;

#line 158
    float py_0;
    if((p_4.z) < 0.0)
    {

#line 160
        float _S185 = 1.0 - abs(_S184);

#line 160
        if(_S183 >= 0.0)
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
        float fx_2 = _S185 * px_0;
        float _S186 = 1.0 - abs(_S183);

#line 161
        if(_S184 >= 0.0)
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
        float fy_2 = _S186 * px_0;

#line 161
        px_0 = fx_2;

#line 161
        py_0 = fy_2;

#line 159
    }
    else
    {

#line 159
        px_0 = _S183;

#line 159
        py_0 = _S184;

#line 159
    }

#line 165
    *u_0 = gpClamp_0(px_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    *v_2 = gpClamp_0(py_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    return;
}


#line 220
float3 gpDecode_0(uint encoding_0, const array<float, int(48)> thread* payload_1, float3 direction_6)
{

#line 220
    uint c_0;
    if(encoding_0 == 0U)
    {



        array<float, int(4)> _S187 = { 0.28209498524665833, 0.32573533058166504 * direction_6.y, 0.32573533058166504 * direction_6.z, 0.32573533058166504 * direction_6.x };

        thread float3 result_2 = float3(0.0, 0.0, 0.0);

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
            uint _S188 = c_0 * 3U;

#line 230
            result_2.x = result_2.x + (*payload_1)[_S188] * _S187[c_0];
            result_2.y = result_2.y + (*payload_1)[_S188 + 1U] * _S187[c_0];
            result_2.z = result_2.z + (*payload_1)[_S188 + 2U] * _S187[c_0];

#line 229
            c_0 = c_0 + 1U;

#line 229
        }

#line 234
        return gpMax0_0(result_2);
    }
    if(encoding_0 == 1U)
    {

#line 237
        thread float u_1 = 0.0;
        thread float v_3 = 0.0;
        gpOctahedralUv_0(direction_6, &u_1, &v_3);
        uint x0_2 = uint(u_1);
        uint y0_2 = uint(v_3);
        uint _S189 = min(x0_2 + 1U, 3U);



        uint _S190 = y0_2 * 4U;

        uint _S191 = min(y0_2 + 1U, 3U) * 4U;

        uint _S192 = (_S190 + x0_2) * 3U;

#line 250
        float3 f00_0 = float3((*payload_1)[_S192], (*payload_1)[_S192 + 1U], (*payload_1)[_S192 + 2U]);
        uint _S193 = (_S190 + _S189) * 3U;
        uint _S194 = (_S191 + x0_2) * 3U;

#line 252
        float3 f01_0 = float3((*payload_1)[_S194], (*payload_1)[_S194 + 1U], (*payload_1)[_S194 + 2U]);
        uint _S195 = (_S191 + _S189) * 3U;

#line 253
        float3 _S196 = float3((u_1 - float(x0_2))) ;
        float3 top_0 = f00_0 + (float3((*payload_1)[_S193], (*payload_1)[_S193 + 1U], (*payload_1)[_S193 + 2U]) - f00_0) * _S196;

        return top_0 + (f01_0 + (float3((*payload_1)[_S195], (*payload_1)[_S195 + 1U], (*payload_1)[_S195 + 2U]) - f01_0) * _S196 - top_0) * float3((v_3 - float(y0_2))) ;
    }
    float3 _S197 = float3(0.0, 0.0, 0.0);

#line 258
    float3 result_3 = _S197;

#line 258
    c_0 = 0U;

#line 258
    float total_1 = 0.0;

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
        float _S198 = max(0.0, dot(kGpAxes_0[c_0], direction_6));
        if(_S198 <= 0.0)
        {

#line 263
            c_0 = c_0 + 1U;

#line 260
            continue;
        }



        uint _S199 = c_0 * 3U;

        float total_2 = total_1 + _S198;

#line 267
        result_3 = result_3 + float3((*payload_1)[_S199], (*payload_1)[_S199 + 1U], (*payload_1)[_S199 + 2U]) * float3(_S198) ;

#line 267
        total_1 = total_2;

#line 260
        c_0 = c_0 + 1U;

#line 260
    }

#line 269
    if(total_1 > 0.0)
    {

#line 269
        result_3 = result_3 / float3(total_1) ;

#line 269
    }
    else
    {

#line 269
        result_3 = _S197;

#line 269
    }

#line 269
    return result_3;
}


#line 305 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float4 grFinish_0(float3 total_3, float weight_3, float oldest_0)
{

#line 306
    if(weight_3 <= 0.0)
    {

#line 307
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    return float4(total_3 / float3(weight_3) , gpClamp_0(1.0 - gpClamp_0(oldest_0 / 240.0, 0.0, 1.0) * 0.75, 0.0, 1.0));
}


#line 317
void grSampleTraced_0(float3 position_10, float3 normal_14, float3 reflection_2, uint frame_4, float4 thread* diffuse_0, float4 thread* specular_0, KernelContext_0 thread* kernelContext_29)
{
    uint4 counts_0 = kernelContext_29->giProbes_0->constants_0[0U];
    uint _S200 = counts_0.x;
    uint _S201 = counts_0.y;
    uint _S202 = kernelContext_29->giProbes_0->constants_0[1U].z;
    float _S203 = (as_type<float>((kernelContext_29->giProbes_0->constants_0[5U].x)));
    float3 _S204 = position_10 + normal_14 * float3((as_type<float>((kernelContext_29->giProbes_0->constants_0[1U].y)))) ;
    float3 _S205 = float3(0.0, 0.0, 0.0);

#line 325
    float3 diffuseTotal_0 = _S205;

#line 325
    float diffuseOldest_0 = 0.0;

#line 325
    float3 specularTotal_0 = _S205;

#line 325
    float specularOldest_0 = 0.0;

#line 325
    uint level_7 = 0U;

#line 325
    float diffuseWeight_0 = 0.0;

#line 325
    float specularWeight_0 = 0.0;

#line 331
    for(;;)
    {

#line 331
        if(level_7 < _S202)
        {
        }
        else
        {

#line 331
            break;
        }

#line 332
        float spacing_0 = _S203 * float(1U << level_7);
        float _S206 = position_10.x;

#line 333
        int _S207 = gpFloorDiv_0(_S206 - spacing_0, spacing_0);
        float _S208 = position_10.y;

#line 334
        int _S209 = gpFloorDiv_0(_S208 - spacing_0, spacing_0);
        float _S210 = position_10.z;
        int _S211 = gpFloorDiv_0(_S206 + spacing_0, spacing_0);
        int _S212 = gpFloorDiv_0(_S208 + spacing_0, spacing_0);
        int _S213 = gpFloorDiv_0(_S210 + spacing_0, spacing_0);

#line 338
        int z_3 = gpFloorDiv_0(_S210 - spacing_0, spacing_0);
        for(;;)
        {

#line 339
            if(z_3 <= _S213)
            {
            }
            else
            {

#line 339
                break;
            }

#line 339
            float3 diffuseTotal_1 = diffuseTotal_0;

#line 339
            float diffuseOldest_1 = diffuseOldest_0;

#line 339
            float3 specularTotal_1 = specularTotal_0;

#line 339
            float specularOldest_1 = specularOldest_0;

#line 339
            int y_4 = _S209;

#line 339
            float diffuseWeight_1 = diffuseWeight_0;

#line 339
            float specularWeight_1 = specularWeight_0;
            for(;;)
            {

#line 340
                if(y_4 <= _S212)
                {
                }
)cy_msl",
    R"cy_msl(                else
                {

#line 340
                    break;
                }

#line 340
                float3 diffuseTotal_2 = diffuseTotal_1;

#line 340
                float diffuseOldest_2 = diffuseOldest_1;

#line 340
                float3 specularTotal_2 = specularTotal_1;

#line 340
                float specularOldest_2 = specularOldest_1;

#line 340
                int x_4 = _S207;

#line 340
                float diffuseWeight_2 = diffuseWeight_1;

#line 340
                float specularWeight_2 = specularWeight_1;
                for(;;)
                {

#line 341
                    if(x_4 <= _S211)
                    {
                    }
                    else
                    {

#line 341
                        break;
                    }

#line 341
                    uint _S214 = gpLayoutProbe_0(level_7, int3(x_4, y_4, z_3), kernelContext_29);

                    if(_S214 == 4294967295U)
                    {

#line 344
                        x_4 = x_4 + int(1);

#line 341
                        continue;
                    }



                    uint _S215 = _S214 * 4U;

#line 346
                    float4 device* _S216 = kernelContext_29->giProbes_0->probes_0+(_S215 + 1U);

#line 346
                    float4 record_1 = *_S216;

#line 346
                    bool _S217;
                    if(((*_S216).y) == 0.0)
                    {

#line 347
                        _S217 = true;

#line 347
                    }
                    else
                    {

#line 347
                        _S217 = (record_1.z) == 0.0;

#line 347
                    }

#line 347
                    if(_S217)
                    {

#line 348
                        x_4 = x_4 + int(1);

#line 341
                        continue;
                    }

#line 350
                    float3 at_0 = (*(kernelContext_29->giProbes_0->probes_0+_S215)).xyz;
                    float3 offset_3 = position_10 - at_0;
                    float span_2 = length(offset_3);
                    if(span_2 > spacing_0)
                    {

#line 354
                        x_4 = x_4 + int(1);

#line 341
                        continue;
                    }

#line 356
                    float _S218 = 1.0 - gpClamp_0(span_2 / spacing_0, 0.0, 1.0);

#line 356
                    float _S219 = gpVisibilityWeight_0(_S214, at_0, position_10, kernelContext_29);

#line 356
                    float falloff_0 = _S218 * _S219;

                    bool apart_0 = span_2 > 0.00009999999747379;

#line 358
                    float3 toward_1;
                    if(apart_0)
                    {

#line 359
                        toward_1 = - offset_3 / float3(span_2) ;

#line 359
                    }
                    else
                    {

#line 359
                        toward_1 = normal_14;

#line 359
                    }

#line 359
                    float diffuseShare_0;
                    if(apart_0)
                    {

#line 360
                        diffuseShare_0 = falloff_0 * grFacing_0(toward_1, normal_14);

#line 360
                    }
                    else
                    {

#line 360
                        diffuseShare_0 = falloff_0;

#line 360
                    }

#line 360
                    float specularShare_0;

                    if(apart_0)
                    {

#line 362
                        specularShare_0 = falloff_0 * grFacing_0(toward_1, reflection_2);

#line 362
                    }
                    else
                    {

#line 362
                        specularShare_0 = falloff_0;

#line 362
                    }

#line 362
                    bool _S220;
                    if(diffuseShare_0 <= 0.0)
                    {

#line 363
                        _S220 = specularShare_0 <= 0.0;

#line 363
                    }
                    else
                    {

#line 363
                        _S220 = false;

#line 363
                    }

#line 363
                    bool _S221;

#line 363
                    if(_S220)
                    {

#line 363
                        _S221 = true;

#line 363
                    }
                    else
                    {

#line 363
                        bool _S222 = grProbeOccluded_0(_S204, at_0, kernelContext_29);

#line 363
                        _S221 = _S222;

#line 363
                    }

#line 363
                    if(_S221)
                    {
                        x_4 = x_4 + int(1);

#line 341
                        continue;
                    }

#line 367
                    thread array<float, int(48)> data_0;

#line 367
                    gpLoadPayload_0(_S214, _S201, &data_0, kernelContext_29);

                    uint last_1 = (as_type<uint>((record_1.x)));

#line 369
                    float age_1;
                    if(frame_4 > last_1)
                    {

#line 370
                        age_1 = float(frame_4 - last_1);

#line 370
                    }
                    else
                    {

#line 370
                        age_1 = 0.0;

#line 370
                    }

#line 370
                    float3 diffuseTotal_3;

#line 370
                    float diffuseOldest_3;

#line 370
                    float diffuseWeight_3;
                    if(diffuseShare_0 > 0.0)
                    {

#line 371
                        thread array<float, int(48)> _S223 = data_0;

#line 371
                        float3 _S224 = gpDecode_0(_S200, &_S223, normal_14);

                        float diffuseWeight_4 = diffuseWeight_2 + diffuseShare_0;
                        float _S225 = max(diffuseOldest_2, age_1);

#line 374
                        diffuseTotal_3 = diffuseTotal_2 + _S224 * float3(diffuseShare_0) ;

#line 374
                        diffuseOldest_3 = _S225;

#line 374
                        diffuseWeight_3 = diffuseWeight_4;

#line 371
                    }
                    else
                    {

#line 371
                        diffuseTotal_3 = diffuseTotal_2;

#line 371
                        diffuseOldest_3 = diffuseOldest_2;

#line 371
                        diffuseWeight_3 = diffuseWeight_2;

#line 371
                    }

#line 371
                    float3 specularTotal_3;

#line 371
                    float specularOldest_3;

#line 371
                    float specularWeight_3;

#line 376
                    if(specularShare_0 > 0.0)
                    {

#line 376
                        thread array<float, int(48)> _S226 = data_0;

#line 376
                        float3 _S227 = gpDecode_0(_S200, &_S226, reflection_2);


                        float specularWeight_4 = specularWeight_2 + specularShare_0;
                        float _S228 = max(specularOldest_2, age_1);

#line 380
                        specularTotal_3 = specularTotal_2 + _S227 * float3(specularShare_0) ;

#line 380
                        specularOldest_3 = _S228;

#line 380
                        specularWeight_3 = specularWeight_4;

#line 376
                    }
                    else
                    {

#line 376
                        specularTotal_3 = specularTotal_2;

#line 376
                        specularOldest_3 = specularOldest_2;

#line 376
                        specularWeight_3 = specularWeight_2;

#line 376
                    }

#line 376
                    diffuseTotal_2 = diffuseTotal_3;

#line 376
                    diffuseOldest_2 = diffuseOldest_3;

#line 376
                    specularTotal_2 = specularTotal_3;

#line 376
                    specularOldest_2 = specularOldest_3;

#line 376
                    diffuseWeight_2 = diffuseWeight_3;

#line 376
                    specularWeight_2 = specularWeight_3;

#line 341
                    x_4 = x_4 + int(1);

#line 341
                }

#line 340
                int y_5 = y_4 + int(1);

#line 340
                diffuseTotal_1 = diffuseTotal_2;

#line 340
                diffuseOldest_1 = diffuseOldest_2;

#line 340
                specularTotal_1 = specularTotal_2;

#line 340
                specularOldest_1 = specularOldest_2;

#line 340
                y_4 = y_5;

#line 340
                diffuseWeight_1 = diffuseWeight_2;

#line 340
                specularWeight_1 = specularWeight_2;

#line 340
            }

#line 339
            int z_4 = z_3 + int(1);

#line 339
            diffuseTotal_0 = diffuseTotal_1;

#line 339
            diffuseOldest_0 = diffuseOldest_1;

#line 339
            specularTotal_0 = specularTotal_1;

#line 339
            specularOldest_0 = specularOldest_1;

#line 339
            z_3 = z_4;

#line 339
            diffuseWeight_0 = diffuseWeight_1;

#line 339
            specularWeight_0 = specularWeight_1;

#line 339
        }

#line 331
        level_7 = level_7 + 1U;

#line 331
    }

#line 386
    *diffuse_0 = grFinish_0(diffuseTotal_0, diffuseWeight_0, diffuseOldest_0);
    *specular_0 = grFinish_0(specularTotal_0, specularWeight_0, specularOldest_0);
    return;
}


#line 461
void grResolvePixel_0(uint pixel_4, KernelContext_0 thread* kernelContext_30)
{

#line 462
    uint4 counts_1 = kernelContext_30->giResolve_0->constants_2[0U];
    uint _S229 = counts_1.z;

#line 463
    if(pixel_4 >= _S229)
    {

#line 464
        return;
    }
    uint record_2 = (counts_1.w * _S229 + pixel_4) * 2U;
    float4 at_1 = kernelContext_30->giResolve_0->surfaces_0[record_2];
    float4 facing_0 = kernelContext_30->giResolve_0->surfaces_0[record_2 + 1U];
    if((at_1.w) <= 0.0)
    {

#line 470
        uint _S230 = pixel_4 * 2U;

#line 470
        float4 _S231 = float4(0.0, 0.0, 0.0, 0.0);

#line 470
        *(kernelContext_30->giResolve_0->noisy_0+_S230) = _S231;
        *(kernelContext_30->giResolve_0->noisy_0+(_S230 + 1U)) = _S231;
        return;
    }
    uint frame_5 = kernelContext_30->giResolve_0->constants_2[18U].x;

    float3 position_11 = at_1.xyz;
    float3 normal_15 = giNormalisedOr_0(facing_0.xyz, float3(0.0, 1.0, 0.0));
    float3 offset_4 = position_11 - (as_type<float3>((kernelContext_30->giResolve_0->constants_2[1U].xyz)));

    float3 incident_0 = giNormalisedOr_0(offset_4, float3(0.0, 0.0, -1.0));

    float3 reflection_3 = giNormalisedOr_0(incident_0 - normal_15 * float3((2.0 * dot(incident_0, normal_15))) , normal_15);
    thread float4 cachedDiffuse_0;
    thread float4 cachedSpecular_0;

#line 484
    grSampleTraced_0(position_11, normal_15, reflection_3, kernelContext_30->giResolve_0->constants_2[4U].w, &cachedDiffuse_0, &cachedSpecular_0, kernelContext_30);

#line 484
    float3 _S232 = grResolveDiffuse_0(pixel_4, position_11, normal_15, length(offset_4), cachedDiffuse_0, frame_5, kernelContext_30);

#line 484
    float3 _S233 = grResolveSpecular_0(pixel_4, position_11, normal_15, facing_0.w, reflection_3, cachedSpecular_0, frame_5, kernelContext_30);

#line 490
    uint _S234 = pixel_4 * 2U;

#line 490
    *(kernelContext_30->giResolve_0->noisy_0+_S234) = float4(_S232, 1.0);
    *(kernelContext_30->giResolve_0->noisy_0+(_S234 + 1U)) = float4(_S233, 1.0);
    return;
}


#line 18 "src/rendering/gi_gpu/shaders/gi_resolve_hardware.slang"
[[kernel]] void cyGiResolveHardware(uint3 thread_0 [[thread_position_in_grid]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiResolveSet_default_0 constant* giResolve_1 [[buffer(3)]], GiHardwareSet_default_0 constant* giHardware_1 [[buffer(4)]])
{

#line 18
    thread KernelContext_0 kernelContext_31;

#line 18
    (&kernelContext_31)->giProbes_0 = giProbes_1;

#line 18
    (&kernelContext_31)->giScene_0 = giScene_1;

#line 18
    (&kernelContext_31)->giResolve_0 = giResolve_1;

#line 18
    (&kernelContext_31)->giHardware_0 = giHardware_1;

#line 18
    grResolvePixel_0(thread_0.x, &kernelContext_31);

    return;
}

)cy_msl");
inline constexpr const auto& kGiResolveHardwareMsl = kGiResolveHardwareMslText.text;

/// cyGiReproject.metal, 8639 bytes.
inline constexpr char kGiReprojectMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 45 "src/rendering/gi_gpu/shaders/gi_denoise.slang"
struct GiDenoiseSet_default_0
{
    uint4 device* constants_0;
    float4 device* surfaces_0;
    float4 device* noisy_0;
    uint device* sources_0;
    float4 device* history_0;
    float device* variance_0;
    float4 device* filterA_0;
    float4 device* filterB_0;
};


#line 45
struct KernelContext_0
{
    GiDenoiseSet_default_0 constant* giDenoise_0;
};


#line 110
uint gdSurfaceRecord_0(uint pixel_0, KernelContext_0 thread* kernelContext_0)
{

#line 111
    uint4 counts_0 = kernelContext_0->giDenoise_0->constants_0[0U];
    return (counts_0.w * counts_0.z + pixel_0) * 2U;
}


#line 132
bool gdToScreen_0(uint first_0, float3 world_0, float2 thread* uv_0, KernelContext_0 thread* kernelContext_1)
{



    float4 clip_0 = (as_type<float4>((kernelContext_1->giDenoise_0->constants_0[first_0]))) * float4(world_0.x)  + (as_type<float4>((kernelContext_1->giDenoise_0->constants_0[first_0 + 1U]))) * float4(world_0.y)  + (as_type<float4>((kernelContext_1->giDenoise_0->constants_0[first_0 + 2U]))) * float4(world_0.z)  + (as_type<float4>((kernelContext_1->giDenoise_0->constants_0[first_0 + 3U])));
    *uv_0 = float2(0.0, 0.0);
    float _S1 = clip_0.w;

#line 139
    if(_S1 <= 9.99999993922529029e-09)
    {

#line 140
        return false;
    }
    float inverse_0 = 1.0 / _S1;
    *uv_0 = float2(clip_0.x * inverse_0 * 0.5 + 0.5, clip_0.y * inverse_0 * 0.5 + 0.5);
    return true;
}


#line 118
bool gdInvalidated_0(float3 point_0, KernelContext_0 thread* kernelContext_2)
{

#line 119
    uint _S2 = min(kernelContext_2->giDenoise_0->constants_0[26U].x, 4U);

#line 119
    uint region_0 = 0U;
    for(;;)
    {

#line 120
        if(region_0 < _S2)
        {
        }
        else
        {

#line 120
            break;
        }

#line 121
        uint _S3 = region_0 * 2U;

#line 121
        float3 lo_0 = (as_type<float3>((kernelContext_2->giDenoise_0->constants_0[27U + _S3].xyz)));
        float3 hi_0 = (as_type<float3>((kernelContext_2->giDenoise_0->constants_0[28U + _S3].xyz)));
        float _S4 = point_0.x;

#line 123
        bool _S5;

#line 123
        if(_S4 >= (lo_0.x))
        {

#line 123
            _S5 = (point_0.y) >= (lo_0.y);

#line 123
        }
        else
        {

#line 123
            _S5 = false;

#line 123
        }

#line 123
        bool _S6;

#line 123
        if(_S5)
        {

#line 123
            _S6 = (point_0.z) >= (lo_0.z);

#line 123
        }
        else
        {

#line 123
            _S6 = false;

#line 123
        }

#line 123
        bool _S7;

#line 123
        if(_S6)
        {

#line 123
            _S7 = _S4 <= (hi_0.x);

#line 123
        }
        else
        {

#line 123
            _S7 = false;

#line 123
        }

#line 123
        bool _S8;

#line 123
        if(_S7)
        {

#line 123
            _S8 = (point_0.y) <= (hi_0.y);

#line 123
        }
        else
        {

#line 123
            _S8 = false;

#line 123
        }

#line 123
        bool _S9;
        if(_S8)
        {

#line 124
            _S9 = (point_0.z) <= (hi_0.z);

#line 124
        }
        else
        {

#line 124
            _S9 = false;

#line 124
        }

#line 123
        if(_S9)
        {
            return true;
        }

#line 120
        region_0 = region_0 + 1U;

#line 120
    }

#line 128
    return false;
}


#line 149
[[kernel]] void cyGiReproject(uint3 thread_0 [[thread_position_in_grid]], GiDenoiseSet_default_0 constant* giDenoise_1 [[buffer(0)]])
{

#line 149
    thread KernelContext_0 kernelContext_3;

#line 149
    (&kernelContext_3)->giDenoise_0 = giDenoise_1;
    uint pixel_1 = thread_0.x;
    uint4 counts_1 = giDenoise_1->constants_0[0U];
    uint _S10 = counts_1.z;

#line 152
    if(pixel_1 >= _S10)
    {

#line 153
        return;
    }
    uint _S11 = pixel_1 * 2U;

#line 155
    *((&kernelContext_3)->giDenoise_0->sources_0+_S11) = (as_type<uint>((int(-1))));
    uint _S12 = _S11 + 1U;

#line 156
    *((&kernelContext_3)->giDenoise_0->sources_0+_S12) = (as_type<uint>((0.0)));

#line 156
    uint _S13 = gdSurfaceRecord_0(pixel_1, &kernelContext_3);
    float4 at_0 = (&kernelContext_3)->giDenoise_0->surfaces_0[_S13];

#line 157
    bool representable_0;

    if(!((giDenoise_1->constants_0[5U].w) != 0U))
    {

#line 159
        representable_0 = true;

#line 159
    }
    else
    {

#line 159
        representable_0 = (at_0.w) <= 0.0;

#line 159
    }

#line 159
    if(representable_0)
    {

#line 160
        return;
    }


    thread float2 previous_0;

    float2 _S14 = float2(0.0, 0.0);

    float3 _S15 = at_0.xyz;

#line 163
    thread float2 current_0;

#line 163
    bool _S16 = gdToScreen_0(6U, _S15, &current_0, &kernelContext_3);

#line 163
    float2 currentUv_0;

#line 163
    float2 motion_0;

#line 168
    if(_S16)
    {

#line 169
        float2 _S17 = current_0;

#line 169
        bool _S18 = gdToScreen_0(10U, _S15, &previous_0, &kernelContext_3);
        if(_S18)
        {

)cy_msl"
    R"cy_msl(#line 170
            currentUv_0 = float2(previous_0.x - current_0.x, previous_0.y - current_0.y);

#line 170
        }
        else
        {

#line 170
            currentUv_0 = _S14;

#line 170
        }

#line 167
        float2 _S19 = currentUv_0;

#line 167
        currentUv_0 = _S17;

#line 167
        motion_0 = _S19;

#line 167
        representable_0 = _S18;
    }
    else
    {

#line 168
        currentUv_0 = _S14;

#line 168
        motion_0 = _S14;

#line 168
        representable_0 = false;

#line 168
    }

#line 175
    float _S20 = currentUv_0.x + motion_0.x;

#line 175
    float _S21 = currentUv_0.y + motion_0.y;
    uint _S22 = counts_1.x;

#line 176
    float width_0 = float(_S22);
    float height_0 = float(counts_1.y);
    float fx_0 = floor(_S20 * width_0);
    float fy_0 = floor((1.0 - _S21) * height_0);

#line 179
    bool onScreen_0;
    if(fx_0 >= 0.0)
    {

#line 180
        onScreen_0 = fy_0 >= 0.0;

#line 180
    }
    else
    {

#line 180
        onScreen_0 = false;

#line 180
    }

#line 180
    if(onScreen_0)
    {

#line 180
        onScreen_0 = fx_0 < width_0;

#line 180
    }
    else
    {

#line 180
        onScreen_0 = false;

#line 180
    }

#line 180
    if(onScreen_0)
    {

#line 180
        onScreen_0 = fy_0 < height_0;

#line 180
    }
    else
    {

#line 180
        onScreen_0 = false;

#line 180
    }

#line 180
    uint landing_0;
    if(onScreen_0)
    {

#line 181
        landing_0 = uint(fy_0) * _S22 + uint(fx_0);

#line 181
    }
    else
    {

#line 181
        landing_0 = 0U;

#line 181
    }
    uint previousSlot_0 = 1U - counts_1.w;

#line 182
    float historyDepth_0;

    if(onScreen_0)
    {

#line 184
        historyDepth_0 = (&kernelContext_3)->giDenoise_0->surfaces_0[(previousSlot_0 * _S10 + landing_0) * 2U].w;

#line 184
    }
    else
    {

#line 184
        historyDepth_0 = 0.0;

#line 184
    }

    if(!representable_0)
    {

#line 187
        return;
    }
    if(_S20 < 0.0)
    {

#line 189
        representable_0 = true;

#line 189
    }
    else
    {

#line 189
        representable_0 = _S20 > 1.0;

#line 189
    }

#line 189
    if(representable_0)
    {

#line 189
        representable_0 = true;

#line 189
    }
    else
    {

#line 189
        representable_0 = _S21 < 0.0;

#line 189
    }

#line 189
    if(representable_0)
    {

#line 189
        representable_0 = true;

#line 189
    }
    else
    {

#line 189
        representable_0 = _S21 > 1.0;

#line 189
    }

#line 189
    if(representable_0)
    {

#line 190
        return;
    }

    float _S23 = at_0.w;

    if((abs(_S23 - historyDepth_0)) > (max(min(_S23, historyDepth_0), 0.00009999999747379) * (as_type<float>((giDenoise_1->constants_0[5U].z)))))
    {

#line 195
        representable_0 = true;

#line 195
    }
    else
    {

#line 195
        representable_0 = !onScreen_0;

#line 195
    }

#line 195
    if(representable_0)
    {

#line 195
        representable_0 = true;

#line 195
    }
    else
    {

#line 195
        representable_0 = historyDepth_0 <= 0.0;

#line 195
    }

#line 195
    if(representable_0)
    {

#line 195
        representable_0 = true;

#line 195
    }
    else
    {

#line 195
        bool _S24 = gdInvalidated_0(_S15, &kernelContext_3);

#line 195
        representable_0 = _S24;

#line 195
    }

#line 195
    if(representable_0)
    {
        return;
    }
    *((&kernelContext_3)->giDenoise_0->sources_0+_S11) = landing_0;
    *((&kernelContext_3)->giDenoise_0->sources_0+_S12) = (as_type<uint>((1.0)));
    return;
}

)cy_msl";

/// cyGiDenoiseAccumulate.metal, 5956 bytes.
inline constexpr char kGiDenoiseAccumulateMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 45 "src/rendering/gi_gpu/shaders/gi_denoise.slang"
struct GiDenoiseSet_default_0
{
    uint4 device* constants_0;
    float4 device* surfaces_0;
    float4 device* noisy_0;
    uint device* sources_0;
    float4 device* history_0;
    float device* variance_0;
    float4 device* filterA_0;
    float4 device* filterB_0;
};


#line 62
struct GiDenoisePush_0
{
    uint count_0;
    uint pass_0;
    uint direction_0;
    uint reserved_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    GiDenoiseSet_default_0 constant* giDenoise_0;
    GiDenoisePush_0 constant* gdPush_0;
};


#line 102 "src/rendering/gi_gpu/shaders/gi_denoise.slang"
uint gdPixels_0(KernelContext_0 thread* kernelContext_0)
{

#line 103
    return kernelContext_0->giDenoise_0->constants_0[0U].z;
}


#line 102
uint gdPixels_1(KernelContext_0 thread* kernelContext_1)
{

#line 103
    return kernelContext_1->giDenoise_0->constants_0[0U].z;
}


#line 82
float gdScalar_0(float3 value_0, uint domain_0)
{

#line 83
    if(domain_0 == 1U)
    {

#line 84
        return value_0.x;
    }
    return 0.2125999927520752 * value_0.x + 0.71520000696182251 * value_0.y + 0.07220000028610229 * value_0.z;
}


#line 77
float gdClamp_0(float value_1, float lo_0, float hi_0)
{

#line 77
    float _S1;
    if(value_1 < lo_0)
    {

#line 78
        _S1 = lo_0;

#line 78
    }
    else
    {

#line 78
        if(value_1 > hi_0)
        {

#line 78
            _S1 = hi_0;

#line 78
        }
        else
        {

#line 78
            _S1 = value_1;

#line 78
        }

#line 78
    }

#line 78
    return _S1;
}


#line 106
uint gdHistoryRecord_0(uint slot_0, uint signal_0, uint pixel_0, KernelContext_0 thread* kernelContext_2)
{

#line 107
    uint _S2 = slot_0 * 2U + signal_0;

#line 107
    uint _S3 = gdPixels_1(kernelContext_2);

#line 107
    return (_S2 * _S3 + pixel_0) * 2U;
}


#line 207
[[kernel]] void cyGiDenoiseAccumulate(uint3 thread_0 [[thread_position_in_grid]], GiDenoiseSet_default_0 constant* giDenoise_1 [[buffer(0)]], GiDenoisePush_0 constant* gdPush_1 [[buffer(1)]])
{

#line 207
    thread KernelContext_0 kernelContext_3;

#line 207
    (&kernelContext_3)->giDenoise_0 = giDenoise_1;

#line 207
    (&kernelContext_3)->gdPush_0 = gdPush_1;

#line 207
    uint _S4 = gdPixels_0(&kernelContext_3);

    uint _S5 = thread_0.x;

#line 209
    if(_S5 >= ((&kernelContext_3)->gdPush_0->count_0))
    {

#line 210
        return;
    }
    uint signal_1 = _S5 / _S4;
    uint pixel_1 = _S5 - signal_1 * _S4;
    uint _S6 = 20U + signal_1 * 3U;
    uint domain_1 = (&kernelContext_3)->giDenoise_0->constants_0[_S6 + 2U].y;
    float historyLength_0 = (as_type<float>(((&kernelContext_3)->giDenoise_0->constants_0[_S6 + 1U].y)));

#line 216
    float alphaFloor_0;
    if(historyLength_0 > 0.0)
    {

#line 217
        alphaFloor_0 = 1.0 / historyLength_0;

#line 217
    }
    else
    {

#line 217
        alphaFloor_0 = 1.0;

#line 217
    }
    uint current_0 = (&kernelContext_3)->giDenoise_0->constants_0[0U].w;

    uint _S7 = pixel_1 * 2U;

#line 220
    float4 noisy_1 = (&kernelContext_3)->giDenoise_0->noisy_0[_S7 + signal_1];
    float3 value_2 = noisy_1.xyz;
    float scalar_0 = gdScalar_0(value_2, domain_1);
    float fresh_0 = noisy_1.w;

#line 223
    float confidence_0;

#line 223
    bool _S8;

#line 223
    int source_0;



    if(((&kernelContext_3)->giDenoise_0->constants_0[5U].w) != 0U)
    {

#line 228
        int source_1 = (as_type<int>((*((&kernelContext_3)->giDenoise_0->sources_0+_S7))));
        if(source_1 >= int(0))
        {

#line 229
            _S8 = uint(source_1) < _S4;

#line 229
        }
        else
        {

#line 229
            _S8 = false;

#line 229
        }

#line 229
        if(_S8)
        {

#line 230
            float _S9 = gdClamp_0((as_type<float>((*((&kernelContext_3)->giDenoise_0->sources_0+(_S7 + 1U))))), 0.0, 1.0);

#line 230
            source_0 = source_1;

#line 230
            confidence_0 = _S9;

#line 229
        }
        else
        {

#line 229
            source_0 = int(-1);

#line 229
            confidence_0 = 0.0;

#line 229
        }

#line 227
    }
    else
    {

#line 227
        source_0 = int(-1);

#line 227
        confidence_0 = 0.0;

#line 227
    }

#line 227
    uint _S10 = gdHistoryRecord_0(current_0, signal_1, pixel_1, &kernelContext_3);

#line 237
    if(source_0 < int(0))
    {

#line 237
        _S8 = true;

#line 237
    }
    else
    {

#line 237
        _S8 = confidence_0 <= 0.0;

#line 237
    }

#line 237
    if(_S8)
    {

#line 238
        *((&kernelContext_3)->giDenoise_0->history_0+_S10) = float4(value_2, fresh_0);
        *((&kernelContext_3)->giDenoise_0->history_0+(_S10 + 1U)) = float4(scalar_0, scalar_0 * scalar_0, 0.0, 0.0);
        return;
    }

#line 240
    uint _S11 = gdHistoryRecord_0(1U - current_0, signal_1, uint(source_0), &kernelContext_3);


    float4 device* _S12 = (&kernelContext_3)->giDenoise_0->history_0+_S11;

#line 243
    float4 colour_0 = *_S12;
    float4 moments_0 = *((&kernelContext_3)->giDenoise_0->history_0+(_S11 + 1U));

)cy_msl"
    R"cy_msl(    float count_1 = (*_S12).w * confidence_0 + fresh_0;
    if(count_1 > 0.0)
    {

#line 247
        confidence_0 = fresh_0 / count_1;

#line 247
    }
    else
    {

#line 247
        confidence_0 = 1.0;

#line 247
    }

#line 247
    float _S13 = max(confidence_0, alphaFloor_0);
    float3 _S14 = colour_0.xyz;
    float _S15 = moments_0.x;

#line 249
    float first_0 = _S15 + (scalar_0 - _S15) * _S13;
    float _S16 = moments_0.y;

#line 250
    float second_0 = _S16 + (scalar_0 * scalar_0 - _S16) * _S13;
    *((&kernelContext_3)->giDenoise_0->history_0+_S10) = float4(_S14 + (value_2 - _S14) * float3(_S13) , min(count_1, historyLength_0));
    *((&kernelContext_3)->giDenoise_0->history_0+(_S10 + 1U)) = float4(first_0, second_0, 0.0, 0.0);
    return;
}

)cy_msl";

/// cyGiDenoiseVariance.metal, 6108 bytes.
inline constexpr char kGiDenoiseVarianceMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 45 "src/rendering/gi_gpu/shaders/gi_denoise.slang"
struct GiDenoiseSet_default_0
{
    uint4 device* constants_0;
    float4 device* surfaces_0;
    float4 device* noisy_0;
    uint device* sources_0;
    float4 device* history_0;
    float device* variance_0;
    float4 device* filterA_0;
    float4 device* filterB_0;
};


#line 62
struct GiDenoisePush_0
{
    uint count_0;
    uint pass_0;
    uint direction_0;
    uint reserved_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    GiDenoiseSet_default_0 constant* giDenoise_0;
    GiDenoisePush_0 constant* gdPush_0;
};


#line 102 "src/rendering/gi_gpu/shaders/gi_denoise.slang"
uint gdPixels_0(KernelContext_0 thread* kernelContext_0)
{

#line 103
    return kernelContext_0->giDenoise_0->constants_0[0U].z;
}


#line 102
uint gdPixels_1(KernelContext_0 thread* kernelContext_1)
{

#line 103
    return kernelContext_1->giDenoise_0->constants_0[0U].z;
}

uint gdHistoryRecord_0(uint slot_0, uint signal_0, uint pixel_0, KernelContext_0 thread* kernelContext_2)
{

#line 107
    uint _S1 = slot_0 * 2U + signal_0;

#line 107
    uint _S2 = gdPixels_1(kernelContext_2);

#line 107
    return (_S1 * _S2 + pixel_0) * 2U;
}


#line 96
float gdStableVariance_0(float first_0, float second_0)
{

#line 97
    float _S3 = max(0.0, second_0 - first_0 * first_0);

#line 97
    float _S4;

    if(_S3 <= (9.99999997475242708e-07 * max(1.0, abs(second_0))))
    {

#line 99
        _S4 = 0.0;

#line 99
    }
    else
    {

#line 99
        _S4 = _S3;

#line 99
    }

#line 99
    return _S4;
}


#line 259
[[kernel]] void cyGiDenoiseVariance(uint3 thread_0 [[thread_position_in_grid]], GiDenoiseSet_default_0 constant* giDenoise_1 [[buffer(0)]], GiDenoisePush_0 constant* gdPush_1 [[buffer(1)]])
{

#line 259
    thread KernelContext_0 kernelContext_3;

#line 259
    (&kernelContext_3)->giDenoise_0 = giDenoise_1;

#line 259
    (&kernelContext_3)->gdPush_0 = gdPush_1;

#line 259
    uint _S5 = gdPixels_0(&kernelContext_3);

    uint _S6 = thread_0.x;

#line 261
    if(_S6 >= ((&kernelContext_3)->gdPush_0->count_0))
    {

#line 262
        return;
    }
    uint signal_1 = _S6 / _S5;
    uint _S7 = signal_1 * _S5;

#line 265
    uint pixel_1 = _S6 - _S7;
    uint4 counts_0 = (&kernelContext_3)->giDenoise_0->constants_0[0U];
    uint current_0 = counts_0.w;

#line 267
    uint _S8 = gdHistoryRecord_0(current_0, signal_1, pixel_1, &kernelContext_3);

    float4 device* _S9 = (&kernelContext_3)->giDenoise_0->history_0+_S8;

#line 269
    float4 colour_0 = *_S9;
    float4 device* _S10 = (&kernelContext_3)->giDenoise_0->history_0+(_S8 + 1U);
    float variance_1 = gdStableVariance_0((*_S10).x, (*_S10).y);

#line 271
    float variance_2;
    if(((*_S9).w) < 4.0)
    {

        uint _S11 = counts_0.x;

#line 275
        uint _S12 = pixel_1 % _S11;

#line 275
        int _S13 = int(_S12);
        uint _S14 = pixel_1 / _S11;

#line 276
        int _S15 = int(_S14);

#line 276
        int dy_0 = int(-1);

#line 276
        variance_2 = 0.0;

#line 276
        float squares_0 = 0.0;

#line 276
        float taken_0 = 0.0;



        for(;;)
        {

#line 280
            if(dy_0 <= int(1))
            {
            }
            else
            {

#line 280
                break;
            }

#line 280
            int dx_0 = int(-1);

#line 280
            float sum_0 = variance_2;

#line 280
            float squares_1 = squares_0;

#line 280
            float taken_1 = taken_0;
            for(;;)
            {

#line 281
                if(dx_0 <= int(1))
                {
                }
                else
                {

#line 281
                    break;
                }

#line 282
                int sx_0 = _S13 + dx_0;
                int sy_0 = _S15 + dy_0;

#line 283
                bool _S16;
                if(sx_0 < int(0))
                {

#line 284
                    _S16 = true;

#line 284
                }
                else
                {

#line 284
                    _S16 = sy_0 < int(0);

#line 284
                }

#line 284
                bool _S17;

#line 284
                if(_S16)
                {

#line 284
                    _S17 = true;

#line 284
                }
                else
                {

#line 284
                    _S17 = sx_0 >= int(_S11);

#line 284
                }

#line 284
                bool _S18;

#line 284
                if(_S17)
                {

#line 284
                    _S18 = true;

#line 284
                }
                else
                {

#line 284
                    _S18 = sy_0 >= int(counts_0.y);

#line 284
                }

#line 284
                if(_S18)
                {

#line 285
                    dx_0 = dx_0 + int(1);

#line 281
                    continue;
                }

#line 281
                uint _S19 = gdHistoryRecord_0(current_0, signal_1, uint(sy_0) * _S11 + uint(sx_0), &kernelContext_3);

#line 288
                float value_0 = (*((&kernelContext_3)->giDenoise_0->history_0+(_S19 + 1U))).x;

                float squares_2 = squares_1 + value_0 * value_0;
                float taken_2 = taken_1 + 1.0;

#line 291
                sum_0 = sum_0 + value_0;

)cy_msl"
    R"cy_msl(#line 291
                squares_1 = squares_2;

#line 291
                taken_1 = taken_2;

#line 281
                dx_0 = dx_0 + int(1);

#line 281
            }

#line 280
            dy_0 = dy_0 + int(1);

#line 280
            variance_2 = sum_0;

#line 280
            squares_0 = squares_1;

#line 280
            taken_0 = taken_1;

#line 280
        }

#line 280
        variance_2 = max(variance_1, gdStableVariance_0(variance_2 / taken_0, squares_0 / taken_0));

#line 272
    }
    else
    {

#line 272
        variance_2 = variance_1;

#line 272
    }

#line 297
    uint _S20 = _S7 + pixel_1;

#line 297
    *((&kernelContext_3)->giDenoise_0->variance_0+_S20) = variance_2;

    *((&kernelContext_3)->giDenoise_0->filterA_0+_S20) = float4(colour_0.xyz, 0.0);
    return;
}

)cy_msl";

/// cyGiDenoiseFilter.metal, 9635 bytes.
inline constexpr char kGiDenoiseFilterMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 45 "src/rendering/gi_gpu/shaders/gi_denoise.slang"
struct GiDenoiseSet_default_0
{
    uint4 device* constants_0;
    float4 device* surfaces_0;
    float4 device* noisy_0;
    uint device* sources_0;
    float4 device* history_0;
    float device* variance_0;
    float4 device* filterA_0;
    float4 device* filterB_0;
};


#line 62
struct GiDenoisePush_0
{
    uint count_0;
    uint pass_0;
    uint direction_0;
    uint reserved_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    GiDenoiseSet_default_0 constant* giDenoise_0;
    GiDenoisePush_0 constant* gdPush_0;
};


#line 102 "src/rendering/gi_gpu/shaders/gi_denoise.slang"
uint gdPixels_0(KernelContext_0 thread* kernelContext_0)
{

#line 103
    return kernelContext_0->giDenoise_0->constants_0[0U].z;
}


#line 102
uint gdPixels_1(KernelContext_0 thread* kernelContext_1)
{

#line 103
    return kernelContext_1->giDenoise_0->constants_0[0U].z;
}


#line 304
float4 gdFilterRead_0(uint index_0, KernelContext_0 thread* kernelContext_2)
{

#line 304
    float4 _S1;
    if((kernelContext_2->gdPush_0->direction_0) == 0U)
    {

#line 305
        _S1 = *(kernelContext_2->giDenoise_0->filterA_0+index_0);

#line 305
    }
    else
    {

#line 305
        _S1 = *(kernelContext_2->giDenoise_0->filterB_0+index_0);

#line 305
    }

#line 305
    return _S1;
}

void gdFilterWrite_0(uint index_1, float4 value_0, KernelContext_0 thread* kernelContext_3)
{

#line 309
    if((kernelContext_3->gdPush_0->direction_0) == 0U)
    {

#line 310
        *(kernelContext_3->giDenoise_0->filterB_0+index_1) = value_0;

#line 309
    }
    else
    {
        *(kernelContext_3->giDenoise_0->filterA_0+index_1) = value_0;

#line 309
    }

#line 314
    return;
}


#line 110
uint gdSurfaceRecord_0(uint pixel_0, KernelContext_0 thread* kernelContext_4)
{

#line 111
    uint4 counts_0 = kernelContext_4->giDenoise_0->constants_0[0U];
    return (counts_0.w * counts_0.z + pixel_0) * 2U;
}


#line 82
float gdScalar_0(float3 value_1, uint domain_0)
{

#line 83
    if(domain_0 == 1U)
    {

#line 84
        return value_1.x;
    }
    return 0.2125999927520752 * value_1.x + 0.71520000696182251 * value_1.y + 0.07220000028610229 * value_1.z;
}


#line 106
uint gdHistoryRecord_0(uint slot_0, uint signal_0, uint pixel_1, KernelContext_0 thread* kernelContext_5)
{

#line 107
    uint _S2 = slot_0 * 2U + signal_0;

#line 107
    uint _S3 = gdPixels_1(kernelContext_5);

#line 107
    return (_S2 * _S3 + pixel_1) * 2U;
}


#line 89
float gdSpline_0(int offset_0)
{

#line 89
    int magnitude_0;
    if(offset_0 < int(0))
    {

#line 90
        magnitude_0 = - offset_0;

#line 90
    }
    else
    {

#line 90
        magnitude_0 = offset_0;

#line 90
    }

#line 90
    float _S4;
    if(magnitude_0 == int(0))
    {

#line 91
        _S4 = 0.375;

#line 91
    }
    else
    {

#line 92
        if(magnitude_0 == int(1))
        {

#line 92
            _S4 = 0.25;

#line 92
        }
        else
        {

#line 92
            if(magnitude_0 == int(2))
            {

#line 92
                _S4 = 0.0625;

#line 92
            }
            else
            {

#line 92
                _S4 = 0.0;

#line 92
            }

#line 92
        }

#line 91
    }

#line 91
    return _S4;
}


#line 318
[[kernel]] void cyGiDenoiseFilter(uint3 thread_0 [[thread_position_in_grid]], GiDenoiseSet_default_0 constant* giDenoise_1 [[buffer(0)]], GiDenoisePush_0 constant* gdPush_1 [[buffer(1)]])
{

#line 318
    thread KernelContext_0 kernelContext_6;

#line 318
    (&kernelContext_6)->giDenoise_0 = giDenoise_1;

#line 318
    (&kernelContext_6)->gdPush_0 = gdPush_1;

#line 318
    uint _S5 = gdPixels_0(&kernelContext_6);

    uint _S6 = thread_0.x;

#line 320
    if(_S6 >= ((&kernelContext_6)->gdPush_0->count_0))
    {

#line 321
        return;
    }
    uint signal_1 = _S6 / _S5;
    uint _S7 = signal_1 * _S5;

#line 324
    uint pixel_2 = _S6 - _S7;
    uint4 counts_1 = (&kernelContext_6)->giDenoise_0->constants_0[0U];

    uint _S8 = _S7 + pixel_2;

#line 327
    float4 _S9 = gdFilterRead_0(_S8, &kernelContext_6);
    uint _S10 = 20U + signal_1 * 3U;

#line 328
    uint4 shape_0 = (&kernelContext_6)->giDenoise_0->constants_0[_S10 + 1U];


    if(((&kernelContext_6)->gdPush_0->pass_0) >= (shape_0.z))
    {

#line 331
        gdFilterWrite_0(_S8, _S9, &kernelContext_6);

        return;
    }

#line 333
    uint _S11 = gdSurfaceRecord_0(pixel_2, &kernelContext_6);


    float depth_0 = (&kernelContext_6)->giDenoise_0->surfaces_0[_S11].w;
    if(depth_0 <= 0.0)
    {

#line 337
        gdFilterWrite_0(_S8, _S9, &kernelContext_6);

        return;
    }
    float4 _S12 = (as_type<float4>(((&kernelContext_6)->giDenoise_0->constants_0[_S10])));
    uint domain_1 = (&kernelContext_6)->giDenoise_0->constants_0[_S10 + 2U].y;
    float centreValue_0 = gdScalar_0(_S9.xyz, domain_1);
    float deviation_0 = sqrt(*((&kernelContext_6)->giDenoise_0->variance_0+_S8));

#line 344
    uint _S13 = gdHistoryRecord_0(counts_1.w, signal_1, pixel_2, &kernelContext_6);

#line 344
    bool _S14;

    if(((*((&kernelContext_6)->giDenoise_0->history_0+_S13)).w) >= 12.0)
    {

)cy_msl"
    R"cy_msl(#line 346
        _S14 = deviation_0 <= (0.01999999955296516 * (abs(centreValue_0) + 9.99999997475242708e-07));

#line 346
    }
    else
    {

#line 346
        _S14 = false;

#line 346
    }

#line 346
    if(_S14)
    {

#line 346
        gdFilterWrite_0(_S8, _S9, &kernelContext_6);


        return;
    }

#line 349
    uint _S15 = gdSurfaceRecord_0(pixel_2, &kernelContext_6);

    float4 facing_0 = (&kernelContext_6)->giDenoise_0->surfaces_0[_S15 + 1U];
    float roughness_0 = facing_0.w;
    float3 _S16 = facing_0.xyz;


    int _S17 = int(max(1U, uint(floor(float(1U << ((&kernelContext_6)->gdPush_0->pass_0)) * (1.0 + (as_type<float>((shape_0.x))) * roughness_0) + 0.5))));
    int extent_0 = int(shape_0.w);
    uint _S18 = counts_1.x;

#line 358
    uint _S19 = pixel_2 % _S18;

#line 358
    int _S20 = int(_S19);
    uint _S21 = pixel_2 / _S18;

#line 359
    int _S22 = int(_S21);



    int _S23 = - extent_0;

#line 363
    float3 sum_0 = float3(0.0, 0.0, 0.0);

#line 363
    int dy_0 = _S23;

#line 363
    float weightSum_0 = 0.0;

#line 363
    for(;;)
    {

#line 363
        if(dy_0 <= extent_0)
        {
        }
        else
        {

#line 363
            break;
        }

#line 363
        float3 sum_1 = sum_0;

#line 363
        int dx_0 = _S23;

#line 363
        float weightSum_1 = weightSum_0;
        for(;;)
        {

#line 364
            if(dx_0 <= extent_0)
            {
            }
            else
            {

#line 364
                break;
            }

#line 365
            int sx_0 = _S20 + dx_0 * _S17;
            int sy_0 = _S22 + dy_0 * _S17;
            if(sx_0 < int(0))
            {

#line 367
                _S14 = true;

#line 367
            }
            else
            {

#line 367
                _S14 = sy_0 < int(0);

#line 367
            }

#line 367
            bool _S24;

#line 367
            if(_S14)
            {

#line 367
                _S24 = true;

#line 367
            }
            else
            {

#line 367
                _S24 = sx_0 >= int(_S18);

#line 367
            }

#line 367
            bool _S25;

#line 367
            if(_S24)
            {

#line 367
                _S25 = true;

#line 367
            }
            else
            {

#line 367
                _S25 = sy_0 >= int(counts_1.y);

#line 367
            }

#line 367
            if(_S25)
            {

#line 368
                dx_0 = dx_0 + int(1);

#line 364
                continue;
            }

#line 370
            uint tap_0 = uint(sy_0) * _S18 + uint(sx_0);

#line 370
            uint _S26 = gdSurfaceRecord_0(tap_0, &kernelContext_6);

            float tapDepth_0 = (&kernelContext_6)->giDenoise_0->surfaces_0[_S26].w;
            if(tapDepth_0 <= 0.0)
            {

#line 374
                dx_0 = dx_0 + int(1);

#line 364
                continue;
            }

#line 379
            float4 tapFacing_0 = (&kernelContext_6)->giDenoise_0->surfaces_0[_S26 + 1U];


            float weight_0 = gdSpline_0(dx_0) * gdSpline_0(dy_0) * exp(- abs(tapDepth_0 - depth_0) / (_S12.x * abs(depth_0) + 9.99999997475242708e-07)) * pow(max(0.0, dot(_S16, tapFacing_0.xyz)), _S12.y) * exp(- abs(tapFacing_0.w - roughness_0) / (_S12.w + 9.99999997475242708e-07));

#line 382
            float4 _S27 = gdFilterRead_0(_S7 + tap_0, &kernelContext_6);

            float3 _S28 = _S27.xyz;

            float _S29 = max(weight_0 * exp(- abs(gdScalar_0(_S28, domain_1) - centreValue_0) / (_S12.z * deviation_0 + 9.99999997475242708e-07)), 0.0);
            if(_S29 <= 0.0)
            {

#line 388
                dx_0 = dx_0 + int(1);

#line 364
                continue;
            }

#line 391
            float weightSum_2 = weightSum_1 + _S29;

#line 391
            sum_1 = sum_1 + _S28 * float3(_S29) ;

#line 391
            weightSum_1 = weightSum_2;

#line 364
            dx_0 = dx_0 + int(1);

#line 364
        }

#line 363
        int dy_1 = dy_0 + int(1);

#line 363
        sum_0 = sum_1;

#line 363
        dy_0 = dy_1;

#line 363
        weightSum_0 = weightSum_1;

#line 363
    }

#line 363
    float4 _S30;

#line 394
    if(weightSum_0 > 0.0)
    {

#line 394
        _S30 = float4(sum_0 / float3(weightSum_0) , 0.0);

#line 394
    }
    else
    {

#line 394
        _S30 = _S9;

#line 394
    }

#line 394
    gdFilterWrite_0(_S8, _S30, &kernelContext_6);
    return;
}

)cy_msl";

/// cyGiPublish.metal, 7564 bytes.
inline constexpr char kGiPublishMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 5163 "hlsl.meta.slang"
struct GiPublishSet_default_0
{
    uint4 device* constants_0;
    float4 device* filtered_0;
    float4 device* surfaces_0;
    float4 device* guidance_0;
    float4 device* published_0;
    texture2d<float, access::read_write> diffuse_0;
    texture2d<float, access::read_write> specular_0;
};


#line 5163
struct KernelContext_0
{
    GiPublishSet_default_0 constant* giPublish_0;
};


#line 35 "src/rendering/gi_gpu/shaders/gi_publish.slang"
float3 gpuUpsample_0(uint2 pixel_0, float4 here_0, uint base_0, KernelContext_0 thread* kernelContext_0)
{

#line 36
    uint4 counts_0 = kernelContext_0->giPublish_0->constants_0[0U];

    float _S1 = float(max(kernelContext_0->giPublish_0->constants_0[36U].z, 1U));

#line 38
    float gx_0 = float(pixel_0.x) / _S1;
    float gy_0 = float(pixel_0.y) / _S1;
    uint _S2 = counts_0.x;

#line 40
    uint _S3 = _S2 - 1U;

#line 40
    uint _S4 = min(uint(gx_0), _S3);
    uint _S5 = counts_0.y - 1U;

#line 41
    uint _S6 = min(uint(gy_0), _S5);
    float _S7 = clamp(gx_0 - float(_S4), 0.0, 1.0);
    float _S8 = clamp(gy_0 - float(_S6), 0.0, 1.0);
    float3 _S9 = float3(0.0, 0.0, 0.0);

#line 44
    float nearestGap_0 = 1.00000001504746622e+30;

#line 44
    float3 total_0 = _S9;

#line 44
    float3 nearest_0 = _S9;

#line 44
    uint j_0 = 0U;

#line 44
    float weightTotal_0 = 0.0;



    for(;;)
    {

#line 48
        if(j_0 < 2U)
        {
        }
        else
        {

#line 48
            break;
        }

#line 48
        float nearestGap_1 = nearestGap_0;

#line 48
        float3 total_1 = total_0;

#line 48
        float3 nearest_1 = nearest_0;

#line 48
        uint i_0 = 0U;

#line 48
        float weightTotal_1 = weightTotal_0;
        for(;;)
        {

#line 49
            if(i_0 < 2U)
            {
            }
            else
            {

#line 49
                break;
            }

            uint tap_0 = min(_S6 + j_0, _S5) * _S2 + min(_S4 + i_0, _S3);
            uint record_0 = (counts_0.w * counts_0.z + tap_0) * 2U;
            float tapDepth_0 = kernelContext_0->giPublish_0->surfaces_0[record_0].w;
            if(tapDepth_0 <= 0.0)
            {

#line 56
                i_0 = i_0 + 1U;

#line 49
                continue;
            }

#line 58
            float3 value_0 = kernelContext_0->giPublish_0->filtered_0[base_0 + tap_0].xyz;
            float _S10 = here_0.w;

#line 59
            float gap_0 = abs(tapDepth_0 - _S10);

#line 59
            float nearestGap_2;

#line 59
            float3 nearest_2;
            if(gap_0 < nearestGap_1)
            {

#line 60
                nearestGap_2 = gap_0;

#line 60
                nearest_2 = value_0;

#line 60
            }
            else
            {

#line 60
                nearestGap_2 = nearestGap_1;

#line 60
                nearest_2 = nearest_1;

#line 60
            }

#line 60
            float _S11;



            if(i_0 == 0U)
            {

#line 64
                _S11 = 1.0 - _S7;

#line 64
            }
            else
            {

#line 64
                _S11 = _S7;

#line 64
            }

#line 64
            float _S12;

#line 64
            if(j_0 == 0U)
            {

#line 64
                _S12 = 1.0 - _S8;

#line 64
            }
            else
            {

#line 64
                _S12 = _S8;

#line 64
            }

#line 64
            float bilinear_0 = _S11 * _S12;
            float _S13 = max(0.0, dot(kernelContext_0->giPublish_0->surfaces_0[record_0 + 1U].xyz, here_0.xyz));

#line 65
            uint squaring_0 = 0U;

#line 65
            float facing_0 = _S13;
            for(;;)
            {

#line 66
                if(squaring_0 < 3U)
                {
                }
                else
                {

#line 66
                    break;
                }

#line 67
                float facing_1 = facing_0 * facing_0;

#line 66
                squaring_0 = squaring_0 + 1U;

#line 66
                facing_0 = facing_1;

#line 66
            }



            float weight_0 = bilinear_0 * exp(- gap_0 / (0.10000000149011612 * _S10 + 9.99999997475242708e-07)) * facing_0;
            if(weight_0 <= 0.0)
            {

#line 71
                nearestGap_1 = nearestGap_2;

#line 71
                nearest_1 = nearest_2;
                i_0 = i_0 + 1U;

#line 49
                continue;
            }

#line 74
            float3 _S14 = total_1 + value_0 * float3(weight_0) ;
            float weightTotal_2 = weightTotal_1 + weight_0;

#line 75
            nearestGap_1 = nearestGap_2;

#line 75
            total_1 = _S14;

#line 75
            nearest_1 = nearest_2;

#line 75
            weightTotal_1 = weightTotal_2;

#line 49
            i_0 = i_0 + 1U;

#line 49
        }

#line 48
        uint j_1 = j_0 + 1U;

#line 48
        nearestGap_0 = nearestGap_1;

#line 48
        total_0 = total_1;

#line 48
        nearest_0 = nearest_1;

#line 48
        j_0 = j_1;

#line 48
        weightTotal_0 = weightTotal_1;

#line 48
    }

#line 78
    if(weightTotal_0 > 9.99999997475242708e-07)
    {

)cy_msl"
    R"cy_msl(#line 78
        total_0 = total_0 / float3(weightTotal_0) ;

#line 78
    }
    else
    {

#line 78
        total_0 = nearest_0;

#line 78
    }

#line 78
    return total_0;
}



[[kernel]] void cyGiPublish(uint3 thread_0 [[thread_position_in_grid]], GiPublishSet_default_0 constant* giPublish_1 [[buffer(0)]])
{

#line 83
    thread KernelContext_0 kernelContext_1;

#line 83
    (&kernelContext_1)->giPublish_0 = giPublish_1;
    uint4 full_0 = giPublish_1->constants_0[36U];
    uint _S15 = thread_0.x;

#line 85
    uint _S16 = full_0.x;

#line 85
    bool _S17;

#line 85
    if(_S15 >= _S16)
    {

#line 85
        _S17 = true;

#line 85
    }
    else
    {

#line 85
        _S17 = (thread_0.y) >= (full_0.y);

#line 85
    }

#line 85
    if(_S17)
    {

#line 86
        return;
    }
    uint4 counts_1 = giPublish_1->constants_0[0U];
    uint pixel_1 = thread_0.y * _S16 + _S15;
    float4 here_1 = (&kernelContext_1)->giPublish_0->guidance_0[pixel_1];
    float3 _S18 = float3(0.0, 0.0, 0.0);

#line 91
    float3 diffuse_1;

#line 91
    float3 specular_1;

    if((here_1.w) > 0.0)
    {

#line 94
        if((max(full_0.z, 1U)) == 1U)
        {

            float3 _S19 = (&kernelContext_1)->giPublish_0->filtered_0[counts_1.z + pixel_1].xyz;

#line 97
            diffuse_1 = (&kernelContext_1)->giPublish_0->filtered_0[pixel_1].xyz;

#line 97
            specular_1 = _S19;

#line 94
        }
        else
        {


            uint2 _S20 = thread_0.xy;

#line 99
            float3 _S21 = gpuUpsample_0(_S20, here_1, 0U, &kernelContext_1);

#line 99
            float3 _S22 = gpuUpsample_0(_S20, here_1, counts_1.z, &kernelContext_1);

#line 99
            diffuse_1 = _S21;

#line 99
            specular_1 = _S22;

#line 94
        }

#line 93
    }
    else
    {

#line 93
        diffuse_1 = _S18;

#line 93
        specular_1 = _S18;

#line 93
    }

#line 103
    uint2 _S23 = thread_0.xy;

#line 103
    (&kernelContext_1)->giPublish_0->diffuse_0.write(float4(diffuse_1, 1.0),_S23);
    (&kernelContext_1)->giPublish_0->specular_0.write(float4(specular_1, 1.0),_S23);
    if(((full_0.w) & 1U) != 0U)
    {

#line 106
        *((&kernelContext_1)->giPublish_0->published_0+pixel_1) = float4(diffuse_1, 0.0);
        *((&kernelContext_1)->giPublish_0->published_0+(_S16 * full_0.y + pixel_1)) = float4(specular_1, 0.0);

#line 105
    }



    return;
}

)cy_msl";

/// cyGiProbePlace.metal, 14784 bytes.
inline constexpr char kGiProbePlaceMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 53 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
struct GiResolveSet_default_0
{
    uint4 device* constants_0;
    float4 device* surfaces_0;
    float4 device* noisy_0;
    float4 device* probes_0;
    uint device* slots_0;
};


#line 53
struct KernelContext_0
{
    GiResolveSet_default_0 constant* giResolve_0;
};


#line 60 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
uint spSlot_0(KernelContext_0 thread* kernelContext_0)
{

#line 60
    return kernelContext_0->giResolve_0->constants_0[0U].w;
}


#line 57
uint spUniformCount_0(KernelContext_0 thread* kernelContext_1)
{

#line 57
    return kernelContext_1->giResolve_0->constants_0[43U].w;
}


#line 57
uint spUniformCount_1(KernelContext_0 thread* kernelContext_2)
{

#line 57
    return kernelContext_2->giResolve_0->constants_0[43U].w;
}


#line 53
uint spTilesX_0(KernelContext_0 thread* kernelContext_3)
{

#line 53
    return kernelContext_3->giResolve_0->constants_0[41U].x;
}


#line 53
uint spTilesX_1(KernelContext_0 thread* kernelContext_4)
{

#line 53
    return kernelContext_4->giResolve_0->constants_0[41U].x;
}


#line 51
uint spSpacing_0(KernelContext_0 thread* kernelContext_5)
{

#line 51
    return kernelContext_5->giResolve_0->constants_0[40U].z;
}


#line 51
uint spSpacing_1(KernelContext_0 thread* kernelContext_6)
{

#line 51
    return kernelContext_6->giResolve_0->constants_0[40U].z;
}


#line 49
uint spWidth_0(KernelContext_0 thread* kernelContext_7)
{

#line 49
    return kernelContext_7->giResolve_0->constants_0[40U].x;
}


#line 49
uint spWidth_1(KernelContext_0 thread* kernelContext_8)
{

#line 49
    return kernelContext_8->giResolve_0->constants_0[40U].x;
}


#line 50
uint spHeight_0(KernelContext_0 thread* kernelContext_9)
{

#line 50
    return kernelContext_9->giResolve_0->constants_0[40U].y;
}


#line 63
uint spUniformPixel_0(uint tile_0, KernelContext_0 thread* kernelContext_10)
{

#line 63
    uint _S1 = spTilesX_1(kernelContext_10);
    uint _S2 = tile_0 % _S1;

#line 64
    uint _S3 = spSpacing_1(kernelContext_10);

#line 64
    uint _S4 = _S2 * _S3;

#line 64
    uint _S5 = spSpacing_1(kernelContext_10);

#line 64
    uint _S6 = _S4 + _S5 / 2U;

#line 64
    uint _S7 = spWidth_1(kernelContext_10);

#line 64
    uint _S8 = min(_S6, _S7 - 1U);

#line 64
    uint _S9 = spTilesX_1(kernelContext_10);
    uint _S10 = tile_0 / _S9;

#line 65
    uint _S11 = spSpacing_1(kernelContext_10);

#line 65
    uint _S12 = _S10 * _S11;

#line 65
    uint _S13 = spSpacing_1(kernelContext_10);

#line 65
    uint _S14 = _S12 + _S13 / 2U;

#line 65
    uint _S15 = spHeight_0(kernelContext_10);

#line 65
    uint _S16 = min(_S14, _S15 - 1U);

#line 65
    uint _S17 = spWidth_1(kernelContext_10);
    return _S16 * _S17 + _S8;
}


#line 99 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 100
    float squared_0 = dot(v_0, v_0);

#line 100
    float3 _S18;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 101
        _S18 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 101
    }
    else
    {

#line 101
        _S18 = fallback_0;

#line 101
    }

#line 101
    return _S18;
}


#line 58 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
uint spCapacity_0(KernelContext_0 thread* kernelContext_11)
{

#line 58
    return kernelContext_11->giResolve_0->constants_0[42U].z;
}


#line 58
uint spCapacity_1(KernelContext_0 thread* kernelContext_12)
{

#line 58
    return kernelContext_12->giResolve_0->constants_0[42U].z;
}


#line 59
uint spRecords_0(KernelContext_0 thread* kernelContext_13)
{

#line 59
    uint _S19 = spUniformCount_1(kernelContext_13);

#line 59
    uint _S20 = spCapacity_1(kernelContext_13);

#line 59
    return _S19 + _S20;
}


#line 76
uint spRecordBase_0(uint slot_0, uint record_0, KernelContext_0 thread* kernelContext_14)
{

#line 76
    uint _S21 = spRecords_0(kernelContext_14);
    return (slot_0 * _S21 + record_0) * 11U;
}


#line 258
void spWriteProbe_0(uint slot_1, uint record_1, float3 position_0, float depth_0, float3 normal_0, uint id_0, KernelContext_0 thread* kernelContext_15)
{

#line 258
    uint _S22 = spRecordBase_0(slot_1, record_1, kernelContext_15);

    *(kernelContext_15->giResolve_0->probes_0+_S22) = float4(position_0, depth_0);
    *(kernelContext_15->giResolve_0->probes_0+(_S22 + 1U)) = float4(normal_0, (as_type<float>((id_0))));

#line 261
    uint v_1 = 0U;
    for(;;)
    {

#line 262
        if(v_1 < 7U)
        {
        }
        else
        {

#line 262
            break;
        }

#line 263
        *(kernelContext_15->giResolve_0->probes_0+(_S22 + 2U + v_1)) = float4(0.0, 0.0, 0.0, 0.0);

#line 262
        v_1 = v_1 + 1U;

#line 262
    }


    *(kernelContext_15->giResolve_0->probes_0+(_S22 + 9U)) = float4(0.0, 0.0, 0.0, 0.0);
    *(kernelContext_15->giResolve_0->probes_0+(_S22 + 10U)) = float4(1.0, 0.0, 0.0, 0.0);
    return;
}


#line 55
uint spCellsX_0(KernelContext_0 thread* kernelContext_16)
{

#line 55
    return kernelContext_16->giResolve_0->constants_0[41U].z;
}


#line 55
uint spCellsX_1(KernelContext_0 thread* kernelContext_17)
{

#line 55
    return kernelContext_17->giResolve_0->constants_0[41U].z;
}


)cy_msl"
    R"cy_msl(#line 56
uint spCellsY_0(KernelContext_0 thread* kernelContext_18)
{

#line 56
    return kernelContext_18->giResolve_0->constants_0[41U].w;
}


#line 56
uint spCellsY_1(KernelContext_0 thread* kernelContext_19)
{

#line 56
    return kernelContext_19->giResolve_0->constants_0[41U].w;
}


#line 80
uint spTableIndex_0(uint slot_2, uint cell_0, KernelContext_0 thread* kernelContext_20)
{

#line 80
    uint _S23 = spCellsX_1(kernelContext_20);
    uint _S24 = slot_2 * _S23;

#line 81
    uint _S25 = spCellsY_1(kernelContext_20);

#line 81
    return 4U + _S24 * _S25 + cell_0;
}


#line 52
uint spCell_0(KernelContext_0 thread* kernelContext_21)
{

#line 52
    return kernelContext_21->giResolve_0->constants_0[40U].w;
}


#line 70
uint spAdaptivePixel_0(uint cell_1, KernelContext_0 thread* kernelContext_22)
{

#line 70
    uint _S26 = spCellsX_1(kernelContext_22);
    uint _S27 = cell_1 % _S26;

#line 71
    uint _S28 = spCell_0(kernelContext_22);

#line 71
    uint _S29 = _S27 * _S28;

#line 71
    uint _S30 = spCell_0(kernelContext_22);

#line 71
    uint _S31 = _S29 + _S30 / 2U;

#line 71
    uint _S32 = spWidth_1(kernelContext_22);

#line 71
    uint _S33 = min(_S31, _S32 - 1U);

#line 71
    uint _S34 = spCellsX_1(kernelContext_22);
    uint _S35 = cell_1 / _S34;

#line 72
    uint _S36 = spCell_0(kernelContext_22);

#line 72
    uint _S37 = _S35 * _S36;

#line 72
    uint _S38 = spCell_0(kernelContext_22);

#line 72
    uint _S39 = _S37 + _S38 / 2U;

#line 72
    uint _S40 = spHeight_0(kernelContext_22);

#line 72
    uint _S41 = min(_S39, _S40 - 1U);

#line 72
    uint _S42 = spWidth_1(kernelContext_22);
    return _S41 * _S42 + _S33;
}


#line 54
uint spTilesY_0(KernelContext_0 thread* kernelContext_23)
{

#line 54
    return kernelContext_23->giResolve_0->constants_0[41U].y;
}


#line 145
void spAxisTaps_0(uint coordinate_0, uint step_0, uint count_0, uint thread* base_0, float thread* fraction_0)
{

#line 146
    float g_0 = (float(coordinate_0) - float(step_0 / 2U)) / float(step_0);
    if(g_0 <= 0.0)
    {

#line 148
        *base_0 = 0U;
        *fraction_0 = 0.0;
        return;
    }
    uint _S43 = uint(g_0);

#line 152
    *base_0 = _S43;
    *fraction_0 = g_0 - float(_S43);
    if((*base_0 + 1U) >= count_0)
    {

#line 155
        *base_0 = count_0 - 1U;
        *fraction_0 = 0.0;

#line 154
    }



    return;
}


#line 139
struct SpTaps_0
{
    array<uint, int(4)> index_0;
    array<float, int(4)> weight_0;
    uint count_1;
};


#line 160
SpTaps_0 spBilinearTaps_0(uint x_0, uint y_0, uint step_1, uint countX_0, uint countY_0)
{

#line 161
    thread SpTaps_0 taps_0;
    (&taps_0)->count_1 = 0U;

#line 162
    uint t_0 = 0U;
    for(;;)
    {

#line 163
        if(t_0 < 4U)
        {
        }
        else
        {

#line 163
            break;
        }

#line 164
        (&taps_0)->index_0[t_0] = 0U;
        (&taps_0)->weight_0[t_0] = 0.0;

#line 163
        t_0 = t_0 + 1U;

#line 163
    }



    thread uint bx_0;

    thread float fx_0;

    spAxisTaps_0(x_0, step_1, countX_0, &bx_0, &fx_0);

#line 168
    thread uint by_0;

    thread float fy_0;

    spAxisTaps_0(y_0, step_1, countY_0, &by_0, &fy_0);

#line 172
    uint j_0 = 0U;
    for(;;)
    {

#line 173
        if(j_0 < 2U)
        {
        }
        else
        {

#line 173
            break;
        }

#line 173
        uint i_0 = 0U;
        for(;;)
        {

#line 174
            if(i_0 < 2U)
            {
            }
            else
            {

#line 174
                break;
            }

#line 174
            float _S44;
            if(i_0 == 0U)
            {

#line 175
                _S44 = 1.0 - fx_0;

#line 175
            }
            else
            {

#line 175
                _S44 = fx_0;

#line 175
            }

#line 175
            float _S45;

#line 175
            if(j_0 == 0U)
            {

#line 175
                _S45 = 1.0 - fy_0;

#line 175
            }
            else
            {

#line 175
                _S45 = fy_0;

#line 175
            }

#line 175
            float weight_1 = _S44 * _S45;
            if(weight_1 <= 0.0)
            {

#line 177
                i_0 = i_0 + 1U;

#line 174
                continue;
            }



            (&taps_0)->index_0[(&taps_0)->count_1] = (by_0 + j_0) * countX_0 + (bx_0 + i_0);
            (&taps_0)->weight_0[(&taps_0)->count_1] = weight_1;
            (&taps_0)->count_1 = (&taps_0)->count_1 + 1U;

#line 174
            i_0 = i_0 + 1U;

#line 174
        }

#line 173
        j_0 = j_0 + 1U;

#line 173
    }

)cy_msl"
    R"cy_msl(#line 184
    return taps_0;
}


#line 119
float spGeometryWeight_0(float3 probePosition_0, float3 probeNormal_0, float3 position_1, float3 normal_1, float depth_1, float tolerance_0)
{
    float plane_0 = abs(dot(probeNormal_0, position_1 - probePosition_0));
    float reach_0 = tolerance_0 * depth_1;
    if(reach_0 <= 0.0)
    {

#line 124
        return 0.0;
    }

    float _S46 = max(0.0, dot(probeNormal_0, normal_1));
    float facing_0 = _S46 * _S46;
    float facing_1 = facing_0 * facing_0;

    return max(0.0, 1.0 - plane_0 / reach_0) * (facing_1 * facing_1);
}


#line 84
uint spCounter_0(uint slot_3)
{

#line 85
    return slot_3;
}


#line 274
void spPlace_0(uint thread_0, KernelContext_0 thread* kernelContext_24)
{

#line 274
    uint _S47 = spSlot_0(kernelContext_24);

#line 274
    uint _S48 = spUniformCount_0(kernelContext_24);


    uint pixels_0 = kernelContext_24->giResolve_0->constants_0[0U].z;
    if(thread_0 < _S48)
    {

#line 278
        uint _S49 = spUniformPixel_0(thread_0, kernelContext_24);

        uint record_2 = (_S47 * pixels_0 + _S49) * 2U;
        float4 at_0 = kernelContext_24->giResolve_0->surfaces_0[record_2];
        float _S50 = at_0.w;

#line 282
        if(_S50 > 0.0)
        {

#line 282
            spWriteProbe_0(_S47, thread_0, at_0.xyz, _S50, giNormalisedOr_0(kernelContext_24->giResolve_0->surfaces_0[record_2 + 1U].xyz, float3(0.0, 1.0, 0.0)), thread_0, kernelContext_24);

#line 282
        }
        else
        {

#line 282
            spWriteProbe_0(_S47, thread_0, float3(0.0, 0.0, 0.0), 0.0, float3(0.0, 1.0, 0.0), thread_0, kernelContext_24);

#line 282
        }

#line 278
    }

#line 278
    uint _S51 = spCellsX_0(kernelContext_24);

#line 278
    uint _S52 = spCellsY_0(kernelContext_24);

#line 292
    if(thread_0 >= (_S51 * _S52))
    {

#line 293
        return;
    }

#line 293
    uint _S53 = spTableIndex_0(_S47, thread_0, kernelContext_24);

    *(kernelContext_24->giResolve_0->slots_0+_S53) = 4294967295U;

#line 295
    uint _S54 = spAdaptivePixel_0(thread_0, kernelContext_24);

    uint _S55 = _S47 * pixels_0;

#line 297
    uint record_3 = (_S55 + _S54) * 2U;
    float4 at_1 = kernelContext_24->giResolve_0->surfaces_0[record_3];
    float _S56 = at_1.w;

#line 299
    if(_S56 <= 0.0)
    {

#line 300
        return;
    }

    float3 _S57 = float3(0.0, 1.0, 0.0);

#line 303
    float3 normal_2 = giNormalisedOr_0(kernelContext_24->giResolve_0->surfaces_0[record_3 + 1U].xyz, _S57);
    float4 weights_0 = (as_type<float4>((kernelContext_24->giResolve_0->constants_0[43U])));

#line 304
    uint _S58 = spWidth_0(kernelContext_24);
    uint _S59 = _S54 % _S58;

#line 305
    uint _S60 = spWidth_0(kernelContext_24);

#line 305
    uint _S61 = _S54 / _S60;

#line 305
    uint _S62 = spSpacing_0(kernelContext_24);

#line 305
    uint _S63 = spTilesX_0(kernelContext_24);

#line 305
    uint _S64 = spTilesY_0(kernelContext_24);

#line 305
    SpTaps_0 _S65 = spBilinearTaps_0(_S59, _S61, _S62, _S63, _S64);

#line 305
    uint tap_0 = 0U;

#line 305
    float covered_0 = 0.0;


    for(;;)
    {

#line 308
        if(tap_0 < (_S65.count_1))
        {
        }
        else
        {

#line 308
            break;
        }

#line 308
        uint _S66 = spUniformPixel_0(_S65.index_0[tap_0], kernelContext_24);

        uint probeRecord_0 = (_S55 + _S66) * 2U;
        float4 probeAt_0 = kernelContext_24->giResolve_0->surfaces_0[probeRecord_0];
        if((probeAt_0.w) <= 0.0)
        {

#line 313
            tap_0 = tap_0 + 1U;

#line 308
            continue;
        }

#line 308
        covered_0 = covered_0 + _S65.weight_0[tap_0] * spGeometryWeight_0(probeAt_0.xyz, giNormalisedOr_0(kernelContext_24->giResolve_0->surfaces_0[probeRecord_0 + 1U].xyz, _S57), at_1.xyz, normal_2, _S56, weights_0.y);

#line 308
        tap_0 = tap_0 + 1U;

#line 308
    }

#line 320
    if(covered_0 >= (weights_0.x))
    {

#line 321
        return;
    }

    uint taken_0 = atomic_fetch_add_explicit(((atomic_uint device*)(kernelContext_24->giResolve_0->slots_0+spCounter_0(_S47))), 1U, memory_order_relaxed);

#line 324
    uint _S67 = spCapacity_0(kernelContext_24);
    if(taken_0 >= _S67)
    {

#line 326
        return;
    }

#line 326
    uint _S68 = spTableIndex_0(_S47, thread_0, kernelContext_24);

    *(kernelContext_24->giResolve_0->slots_0+_S68) = taken_0;

#line 328
    spWriteProbe_0(_S47, _S48 + taken_0, at_1.xyz, _S56, normal_2, _S48 + thread_0, kernelContext_24);

    return;
}


#line 20 "src/rendering/gi_gpu/shaders/gi_screen_probes.slang"
[[kernel]] void cyGiProbePlace(uint3 thread_1 [[thread_position_in_grid]], GiResolveSet_default_0 constant* giResolve_1 [[buffer(3)]])
{

#line 20
    thread KernelContext_0 kernelContext_25;

#line 20
    (&kernelContext_25)->giResolve_0 = giResolve_1;

#line 20
    spPlace_0(thread_1.x, &kernelContext_25);

    return;
}

)cy_msl";

/// cyGiProbeTrace.metal, 101854 bytes.
inline constexpr auto kGiProbeTraceMslText = detail::join_msl(
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 118 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
struct GrSample_0
{
    float3 radiance_0;
    float confidence_0;
    uint source_0;
};


#line 53
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


#line 78 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 66 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
struct GiResolveSet_default_0
{
    uint4 device* constants_2;
    float4 device* surfaces_0;
    float4 device* noisy_0;
    float4 device* probes_1;
    uint device* slots_0;
};


#line 603 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
struct KernelContext_0
{
    GiProbeSet_default_0 constant* giProbes_0;
    GiSceneSet_default_0 constant* giScene_0;
    GiResolveSet_default_0 constant* giResolve_0;
    float4 threadgroup* spStandIn_0;
    float3 threadgroup* spOrigin_0;
    array<float3, int(9)> threadgroup* spHistory_0;
    float threadgroup* spHistoryCount_0;
    uint threadgroup* spReprojected_0;
    array<float, int(16)> threadgroup* spWeights_0;
    array<float3, int(64)> threadgroup* spRayValue_0;
    array<float3, int(64)> threadgroup* spRayDirection_0;
    array<float, int(64)> threadgroup* spRayWeight_0;
    array<uint, int(64)> threadgroup* spRayValid_0;
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


#line 215 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiTraceHit_0
{
    bool hit_1;
    bool exhausted_0;
    float t_1;
    float closest_0;
    float3 position_1;
    float3 normal_1;
};


#line 115
uint giLevelCount_0(KernelContext_0 thread* kernelContext_0)
{

#line 116
    return kernelContext_0->giScene_0->constants_1[0U].x;
}


#line 123
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_1)
{

#line 124
    return (as_type<float>((kernelContext_1->giScene_0->constants_1[9U].y)));
}


#line 119
uint giWindowBricks_0(KernelContext_0 thread* kernelContext_2)
{

#line 120
    return kernelContext_2->giScene_0->constants_1[0U].y;
}


#line 167
int giWrap_0(int value_0, int modulus_0)
{

#line 168
    uint _S1 = uint(modulus_0);
    uint _S2 = (uint(value_0) + (_S1 << 20U)) % _S1;

#line 169
    return int(_S2);
}


#line 95
float giLerp_0(float a_0, float b_0, float t_2)
{

#line 96
    return a_0 + (b_0 - a_0) * t_2;
}


#line 128
float giTrilinear_0(uint slot_0, float fx_0, float fy_0, float fz_0, KernelContext_0 thread* kernelContext_3)
{
    int x0_0 = clamp(int(floor(fx_0)), int(0), int(3));
    int y0_0 = clamp(int(floor(fy_0)), int(0), int(3));
    int z0_0 = clamp(int(floor(fz_0)), int(0), int(3));



    float tx_0 = clamp(fx_0 - float(x0_0), 0.0, 1.0);
    float ty_0 = clamp(fy_0 - float(y0_0), 0.0, 1.0);

    uint base_0 = slot_0 * 125U;

    uint _S3 = uint(z0_0) * 5U;

#line 141
    uint _S4 = uint(y0_0);

#line 141
    uint _S5 = base_0 + (_S3 + _S4) * 5U;

#line 141
    uint _S6 = uint(x0_0);
    uint _S7 = uint(x0_0 + int(1));
    uint _S8 = uint(y0_0 + int(1));

#line 143
    uint _S9 = base_0 + (_S3 + _S8) * 5U;

    uint _S10 = uint(z0_0 + int(1)) * 5U;

#line 145
    uint _S11 = base_0 + (_S10 + _S4) * 5U;

    uint _S12 = base_0 + (_S10 + _S8) * 5U;

#line 153
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S5 + _S6], kernelContext_3->giScene_0->bricks_0[_S5 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S9 + _S6], kernelContext_3->giScene_0->bricks_0[_S9 + _S7], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S11 + _S6], kernelContext_3->giScene_0->bricks_0[_S11 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S12 + _S6], kernelContext_3->giScene_0->bricks_0[_S12 + _S7], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 175
float giSampleLevel_0(uint level_0, float3 p_0, float thread* farDistance_0, KernelContext_0 thread* kernelContext_4)
{

#line 176
    uint _S13 = 16U + level_0 * 2U;

#line 176
    float4 shape_0 = (as_type<float4>((kernelContext_4->giScene_0->constants_1[_S13])));
    int4 origin_0 = (as_type<int4>((kernelContext_4->giScene_0->constants_1[_S13 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_0.x / brick_0));
    int by_0 = int(floor(p_0.y / brick_0));
    int bz_0 = int(floor(p_0.z / brick_0));

#line 183
    uint _S14 = giWindowBricks_0(kernelContext_4);
    int dim_0 = int(_S14);
    int _S15 = origin_0.x;

#line 185
    bool _S16;

#line 185
    if(bx_0 < _S15)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = by_0 < (origin_0.y);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = bz_0 < (origin_0.z);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = bx_0 >= (_S15 + dim_0);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = by_0 >= (origin_0.y + dim_0);

#line 185
    }
    if(_S16)
    {

#line 186
        _S16 = true;

#line 186
    }
    else
    {

#line 186
        _S16 = bz_0 >= (origin_0.z + dim_0);

#line 186
    }

#line 185
    if(_S16)
    {
        return *farDistance_0;
    }
    int _S17 = giWrap_0(bz_0, dim_0);

#line 189
    uint _S18 = uint(dim_0);

#line 189
    uint _S19 = uint(_S17) * _S18;

#line 189
    int _S20 = giWrap_0(by_0, dim_0);

#line 189
    uint _S21 = (_S19 + uint(_S20)) * _S18;
    int _S22 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_4->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S21 + uint(_S22))];
    if(slot_1 == 4294967295U)
    {

#line 193
        return *farDistance_0;
    }

    float3 local_0 = (p_0 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 196
    float _S23 = giTrilinear_0(slot_1, local_0.x, local_0.y, local_0.z, kernelContext_4);
    return _S23;
}


float giDistance_0(float3 p_1, KernelContext_0 thread* kernelContext_5)
{

#line 201
    uint _S24 = giLevelCount_0(kernelContext_5);

#line 201
    float answer_0 = 1.0e+06;

#line 201
    uint level_1 = 0U;


    for(;;)
    {

#line 204
        if(level_1 < _S24)
        {
        }
        else
        {

#line 204
            break;
        }

#line 205
        thread float farDistance_1 = 0.0;

#line 205
        float _S25 = giSampleLevel_0(level_1, p_1, &farDistance_1, kernelContext_5);

        float _S26 = min(answer_0, _S25);
        if(_S25 < farDistance_1)
        {

#line 209
            return _S25;
        }

#line 204
        uint level_2 = level_1 + 1U;

#line 204
        answer_0 = _S26;

#line 204
        level_1 = level_2;

#line 204
    }

#line 212
    return answer_0;
}


#line 99
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 100
    float squared_0 = dot(v_0, v_0);

#line 100
    float3 _S27;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 101
        _S27 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 101
    }
    else
    {

#line 101
        _S27 = fallback_0;

#line 101
    }

#line 101
    return _S27;
}


#line 225
GiTraceHit_0 giSphereTrace_0(float3 origin_1, float3 direction_0, float maxDistance_0, float tMin_0, KernelContext_0 thread* kernelContext_6)
{

#line 226
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_1 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_1 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_1 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_1 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S28 = giLevelCount_0(kernelContext_6);
    if(_S28 == 0U)
    {

#line 234
        return result_0;
    }

#line 234
    float _S29 = giFinestVoxel_0(kernelContext_6);


    float surface_0 = _S29 * 0.5;
    float _S30 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 239
    recent_0[int(0)] = 1.0e+06;

#line 239
    recent_0[int(1)] = 1.0e+06;

#line 239
    recent_0[int(2)] = 1.0e+06;

#line 239
    float closest_1 = 1.0e+06;

#line 239
    uint recentSlot_0 = 0U;

#line 239
    uint step_0 = 0U;

#line 239
    float t_3 = _S30;


    for(;;)
    {

#line 242
        if(step_0 < 192U)
        {
        }
        else
        {

#line 242
            break;
        }

#line 243
        if(t_3 > maxDistance_0)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_2 = origin_1 + direction_0 * float3(t_3) ;

#line 248
        float _S31 = giDistance_0(position_2, kernelContext_6);

        if(_S31 < surface_0)
        {

#line 251
            (&result_0)->hit_1 = true;
            (&result_0)->t_1 = t_3;
            (&result_0)->position_1 = position_2;


            float3 _S32 = float3(_S29, 0.0, 0.0);

#line 256
            float _S33 = giDistance_0(position_2 + _S32, kernelContext_6);

#line 256
            float _S34 = giDistance_0(position_2 - _S32, kernelContext_6);

#line 256
            float _S35 = _S33 - _S34;

            float3 _S36 = float3(0.0, _S29, 0.0);

#line 258
            float _S37 = giDistance_0(position_2 + _S36, kernelContext_6);

#line 258
            float _S38 = giDistance_0(position_2 - _S36, kernelContext_6);

#line 258
            float _S39 = _S37 - _S38;

            float3 _S40 = float3(0.0, 0.0, _S29);

#line 260
            float _S41 = giDistance_0(position_2 + _S40, kernelContext_6);

#line 260
            float _S42 = giDistance_0(position_2 - _S40, kernelContext_6);

            (&result_0)->normal_1 = giNormalisedOr_0(float3(_S35, _S39, _S41 - _S42), - direction_0);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S43 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S31;
        uint _S44 = (recentSlot_0 + 1U) % 3U;
        float t_4 = t_3 + max(_S31, surface_0);

#line 242
        uint step_1 = step_0 + 1U;

#line 242
        closest_1 = _S43;

#line 242
        recentSlot_0 = _S44;

#line 242
        step_0 = step_1;

#line 242
        t_3 = t_4;

#line 242
    }

#line 271
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

#line 107
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


#line 124 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
GrSample_0 grSample_0(float3 radiance_1, float confidence_2, uint source_1)
{

#line 125
    thread GrSample_0 sample_0;
    (&sample_0)->radiance_0 = radiance_1;
    (&sample_0)->confidence_0 = confidence_2;
    (&sample_0)->source_0 = source_1;
    return sample_0;
}


#line 325 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
uint GiSoftwareTier_source_0()
{

#line 326
    return 6U;
}


#line 689
struct GiTierSample_0
{
    float3 radiance_2;
    float confidence_3;
    float hitDistance_0;
    uint source_2;
};


#line 696
GiTierSample_0 gpTierSample_0(uint source_3)
{

#line 697
    thread GiTierSample_0 sample_1;
    (&sample_1)->radiance_2 = float3(0.0, 0.0, 0.0);
    (&sample_1)->confidence_3 = 0.0;
    (&sample_1)->hitDistance_0 = -1.0;
    (&sample_1)->source_2 = source_3;
    return sample_1;
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


#line 471 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_2, float cell_0)
{

#line 472
    return int(floor(value_2 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_3, float3 normal_2, KernelContext_0 thread* kernelContext_9)
{

#line 482
    uint4 grid_0 = kernelContext_9->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 485
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S48 = radius_0 * radius_0;
    float _S49 = position_3.x;

#line 490
    int _S50 = giCell_0(_S49 - radius_0, cell_1);

#line 490
    float _S51 = position_3.y;

#line 490
    int _S52 = giCell_0(_S51 - radius_0, cell_1);
    float _S53 = position_3.z;

#line 491
    int _S54 = giCell_0(_S53 - radius_0, cell_1);
    int _S55 = giCell_0(_S49 + radius_0, cell_1);

#line 492
    int _S56 = giCell_0(_S51 + radius_0, cell_1);
    int _S57 = giCell_0(_S53 + radius_0, cell_1);

#line 493
    float bestScore_0 = 1.0e+09;

#line 493
    float bestAlignment_0 = 0.0;

#line 493
    uint best_0 = 4294967295U;

#line 493
    int z_1 = _S54;



)cy_msl",
    R"cy_msl(    for(;;)
    {

#line 497
        if(z_1 <= _S57)
        {
        }
        else
        {

#line 497
            break;
        }

#line 497
        int y_1 = _S52;
        for(;;)
        {

#line 498
            if(y_1 <= _S56)
            {
            }
            else
            {

#line 498
                break;
            }

#line 498
            float bestScore_1 = bestScore_0;

#line 498
            float bestAlignment_1 = bestAlignment_0;

#line 498
            uint best_1 = best_0;

#line 498
            int x_1 = _S50;
            for(;;)
            {

#line 499
                if(x_1 <= _S55)
                {
                }
                else
                {

#line 499
                    break;
                }
                uint _S58 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 501
                uint first_0 = kernelContext_9->giScene_0->gridRanges_0[_S58];
                uint _S59 = kernelContext_9->giScene_0->gridRanges_0[_S58 + 1U];

#line 502
                float bestScore_2 = bestScore_1;

#line 502
                float bestAlignment_2 = bestAlignment_1;

#line 502
                uint best_2 = best_1;

#line 502
                uint item_0 = first_0;
                for(;;)
                {

#line 503
                    if(item_0 < (first_0 + _S59))
                    {
                    }
                    else
                    {

#line 503
                        break;
                    }

#line 504
                    uint handle_0 = kernelContext_9->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_9->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 506
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }



                    uint _S60 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_9->giScene_0->cards_0[_S60 + 1U].xyz, normal_2);
                    if(alignment_0 <= 0.25)
                    {

#line 511
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 513
                    float3 offset_0 = kernelContext_9->giScene_0->cards_0[_S60].xyz - position_3;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S48)
                    {

#line 516
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 519
                    float score_0 = squared_1 / alignment_0;

#line 519
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 520
                        better_0 = score_0 < bestScore_2;

#line 520
                    }
                    else
                    {

#line 521
                        if(alignment_0 != bestAlignment_2)
                        {

#line 521
                            better_0 = alignment_0 > bestAlignment_2;

#line 521
                        }
                        else
                        {

#line 521
                            better_0 = handle_0 < best_2;

#line 521
                        }

#line 520
                    }

#line 520
                    float bestScore_3;

#line 520
                    float bestAlignment_3;

#line 520
                    uint best_3;


                    if(better_0)
                    {

#line 523
                        bestScore_3 = score_0;

#line 523
                        bestAlignment_3 = alignment_0;

#line 523
                        best_3 = handle_0;

#line 523
                    }
                    else
                    {

#line 523
                        bestScore_3 = bestScore_2;

#line 523
                        bestAlignment_3 = bestAlignment_2;

#line 523
                        best_3 = best_2;

#line 523
                    }

#line 523
                    bestScore_2 = bestScore_3;

#line 523
                    bestAlignment_2 = bestAlignment_3;

#line 523
                    best_2 = best_3;

#line 503
                    item_0 = item_0 + 1U;

#line 503
                }

#line 499
                int x_2 = x_1 + int(1);

#line 499
                bestScore_1 = bestScore_2;

#line 499
                bestAlignment_1 = bestAlignment_2;

#line 499
                best_1 = best_2;

#line 499
                x_1 = x_2;

#line 499
            }

#line 498
            int y_2 = y_1 + int(1);

#line 498
            bestScore_0 = bestScore_1;

#line 498
            bestAlignment_0 = bestAlignment_1;

#line 498
            best_0 = best_1;

#line 498
            y_1 = y_2;

#line 498
        }

#line 497
        z_1 = z_1 + int(1);

#line 497
    }

#line 532
    return best_0;
}


#line 776 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpWorldRadiance_0(const GiWorldHit_0 thread* hit_3, float3 direction_3, uint source_4, uint cacheFrame_0, KernelContext_0 thread* kernelContext_10)
{

#line 777
    thread GiTierSample_0 sample_2 = gpTierSample_0(source_4);
    if(!hit_3->hit_0)
    {

#line 779
        (&sample_2)->source_2 = 9U;

#line 779
        float3 _S61 = gpSky_0(direction_3, kernelContext_10);
        (&sample_2)->radiance_2 = _S61;
        (&sample_2)->confidence_3 = 1.0;
        (&sample_2)->hitDistance_0 = -1.0;
        return sample_2;
    }
    (&sample_2)->hitDistance_0 = hit_3->t_0;

#line 785
    uint _S62 = giFindCard_0(hit_3->position_0, hit_3->normal_0, kernelContext_10);

    if(_S62 != 4294967295U)
    {

#line 788
        uint _S63 = _S62 * 2U;

#line 788
        (&sample_2)->radiance_2 = (*(kernelContext_10->giScene_0->cardState_0+_S63)).xyz;
        uint last_0 = (as_type<uint>(((*(kernelContext_10->giScene_0->cardState_0+(_S63 + 1U))).w)));

#line 789
        float age_0;
        if(cacheFrame_0 > last_0)
        {

#line 790
            age_0 = float(cacheFrame_0 - last_0);

#line 790
        }
        else
        {

#line 790
            age_0 = 0.0;

#line 790
        }


        (&sample_2)->confidence_3 = hit_3->confidence_1 * (1.0 - gpClamp_0(age_0 / 240.0, 0.0, 1.0) * 0.60000002384185791);
        return sample_2;
    }
    (&sample_2)->confidence_3 = hit_3->confidence_1 * 0.20000000298023224;
    return sample_2;
}


#line 468 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
GrSample_0 spProbeRay_0(float3 origin_3, float3 unit_0, float reach_0, KernelContext_0 thread* kernelContext_11)
{

#line 469
    uint cacheFrame_1 = kernelContext_11->giProbes_0->constants_0[24U].w;

#line 469
    GiWorldHit_0 _S64 = GiSoftwareTier_trace_0(origin_3, unit_0, reach_0, kernelContext_11);

#line 469
    bool _S65;

    if(_S64.hit_0)
    {

#line 471
        _S65 = (dot(_S64.normal_0, unit_0)) > 0.0;

#line 471
    }
    else
    {

#line 471
        _S65 = false;

#line 471
    }

#line 471
    if(_S65)
    {

#line 472
        return grSample_0(float3(0.0, 0.0, 0.0), 0.0, 0U);
    }
    uint _S66 = GiSoftwareTier_source_0();

#line 474
    thread GiWorldHit_0 _S67 = _S64;

#line 474
    GiTierSample_0 _S68 = gpWorldRadiance_0(&_S67, unit_0, _S66, cacheFrame_1, kernelContext_11);
    return grSample_0(_S68.radiance_2, _S68.confidence_3, _S68.source_2);
}


#line 207
struct SpProbe_0
{
    float3 position_4;
    float depth_0;
    float3 normal_3;
    uint id_0;
};


#line 443
float3 spProbeOrigin_0(const SpProbe_0 thread* probe_0, float lift_0, KernelContext_0 thread* kernelContext_12)
{

#line 443
    uint _S69 = giLevelCount_0(kernelContext_12);
    if(_S69 == 0U)
    {

#line 445
        return probe_0->position_4 + probe_0->normal_3 * float3(lift_0) ;
    }

#line 445
    float _S70 = giFinestVoxel_0(kernelContext_12);

#line 445
    float clearance_0;


    if((GiSoftwareTier_source_0()) == 7U)
    {

#line 448
        clearance_0 = lift_0;

#line 448
    }
    else
    {

#line 448
        clearance_0 = max(lift_0, _S70);

#line 448
    }

#line 448
    float3 _S71 = probe_0->normal_3;
    float3 origin_4 = probe_0->position_4 + probe_0->normal_3 * float3(clearance_0) ;
    if(_S70 <= 0.0)
    {

#line 451
        return origin_4;
    }

#line 451
    float3 origin_5 = origin_4;

#line 451
    uint step_2 = 0U;

    for(;;)
    {

#line 453
        if(step_2 < 4U)
        {
        }
        else
        {

#line 453
            break;
        }

#line 453
        float _S72 = giDistance_0(origin_5, kernelContext_12);

        if(_S72 >= clearance_0)
        {

#line 456
            break;
        }

        float3 _S73 = float3(_S70, 0.0, 0.0);

#line 459
        float _S74 = giDistance_0(origin_5 + _S73, kernelContext_12);

#line 459
        float _S75 = giDistance_0(origin_5 - _S73, kernelContext_12);

#line 459
        float _S76 = _S74 - _S75;
        float3 _S77 = float3(0.0, _S70, 0.0);

#line 460
        float _S78 = giDistance_0(origin_5 + _S77, kernelContext_12);

#line 460
        float _S79 = giDistance_0(origin_5 - _S77, kernelContext_12);

#line 460
        float _S80 = _S78 - _S79;
        float3 _S81 = float3(0.0, 0.0, _S70);

#line 461
        float _S82 = giDistance_0(origin_5 + _S81, kernelContext_12);

#line 461
        float _S83 = giDistance_0(origin_5 - _S81, kernelContext_12);
        float3 _S84 = origin_5 + giNormalisedOr_0(float3(_S76, _S80, _S82 - _S83), _S71) * float3((clearance_0 - _S72)) ;

#line 453
        uint step_3 = step_2 + 1U;

#line 453
        origin_5 = _S84;

#line 453
        step_2 = step_3;

#line 453
    }

#line 464
    return origin_5;
}


#line 60
uint spSlot_0(KernelContext_0 thread* kernelContext_13)
{

#line 60
    return kernelContext_13->giResolve_0->constants_2[0U].w;
}


#line 84
uint spCounter_0(uint slot_2)
{

#line 85
    return slot_2;
}


#line 57
uint spUniformCount_0(KernelContext_0 thread* kernelContext_14)
{

#line 57
    return kernelContext_14->giResolve_0->constants_2[43U].w;
}


#line 58
uint spCapacity_0(KernelContext_0 thread* kernelContext_15)
{

#line 58
    return kernelContext_15->giResolve_0->constants_2[42U].z;
}


#line 57
uint spUniformCount_1(KernelContext_0 thread* kernelContext_16)
{

#line 57
    return kernelContext_16->giResolve_0->constants_2[43U].w;
}


#line 58
uint spCapacity_1(KernelContext_0 thread* kernelContext_17)
{

#line 58
    return kernelContext_17->giResolve_0->constants_2[42U].z;
}


#line 59
uint spRecords_0(KernelContext_0 thread* kernelContext_18)
{

#line 59
    uint _S85 = spUniformCount_1(kernelContext_18);

#line 59
    uint _S86 = spCapacity_1(kernelContext_18);

#line 59
    return _S85 + _S86;
}


#line 76
uint spRecordBase_0(uint slot_3, uint record_0, KernelContext_0 thread* kernelContext_19)
{

#line 76
    uint _S87 = spRecords_0(kernelContext_19);
    return (slot_3 * _S87 + record_0) * 11U;
}


#line 214
SpProbe_0 spLoadProbe_0(uint slot_4, uint record_1, KernelContext_0 thread* kernelContext_20)
{

#line 214
    uint _S88 = spRecordBase_0(slot_4, record_1, kernelContext_20);

    float4 device* _S89 = kernelContext_20->giResolve_0->probes_1+_S88;
    float4 device* _S90 = kernelContext_20->giResolve_0->probes_1+(_S88 + 1U);
    thread SpProbe_0 probe_1;
    (&probe_1)->position_4 = (*_S89).xyz;
    (&probe_1)->depth_0 = (*_S89).w;
    (&probe_1)->normal_3 = (*_S90).xyz;
    (&probe_1)->id_0 = (as_type<uint>(((*_S90).w)));
    return probe_1;
}


#line 440 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
int gpFloorDiv_0(float value_3, float divisor_0)
{

#line 441
    return int(floor(value_3 / divisor_0));
}


#line 487
uint gpLayoutProbe_0(uint level_3, int3 cell_2, KernelContext_0 thread* kernelContext_21)
{

#line 488
    int3 centre_0 = (as_type<int3>((kernelContext_21->giProbes_0->constants_0[6U + level_3].xyz)));
    int half_0 = int(kernelContext_21->giProbes_0->constants_0[1U].w);
    int3 _S91 = int3(half_0, half_0, half_0);

#line 490
    bool _S92;

#line 490
    if(any(cell_2 < (centre_0 - _S91)))
    {

#line 490
        _S92 = true;

#line 490
    }
    else
    {

#line 490
        _S92 = any(cell_2 > (centre_0 + _S91));

#line 490
    }

#line 490
    if(_S92)
    {

#line 491
        return 4294967295U;
    }
    int edge_0 = int(kernelContext_21->giProbes_0->constants_0[5U].z);
    uint cells_0 = uint(edge_0 * edge_0 * edge_0);
    int _S93 = giWrap_0(cell_2.z, edge_0);

#line 495
    uint _S94 = uint(edge_0);

#line 495
    uint _S95 = uint(_S93) * _S94;

#line 495
    int _S96 = giWrap_0(cell_2.y, edge_0);

#line 495
    uint _S97 = (_S95 + uint(_S96)) * _S94;

    int _S98 = giWrap_0(cell_2.x, edge_0);
    return kernelContext_21->giProbes_0->layout_0[level_3 * cells_0 + (_S97 + uint(_S98))];
}


#line 114
constant array<float3, int(6)> kGpAxes_0 = { float3(1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, -1.0, 0.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, -1.0) };

#line 445
float gpVisibilityWeight_0(uint probe_2, float3 position_5, float3 query_0, KernelContext_0 thread* kernelContext_22)
{

#line 446
    float3 offset_1 = query_0 - position_5;
    float span_0 = length(offset_1);
    if(span_0 <= 0.00009999999747379)
    {

#line 449
        return 1.0;
    }
    float3 _S99 = offset_1 / float3(span_0) ;
    uint _S100 = probe_2 * 4U;

#line 452
    float4 device* _S101 = kernelContext_22->giProbes_0->probes_0+(_S100 + 2U);
    float4 device* _S102 = kernelContext_22->giProbes_0->probes_0+(_S100 + 3U);
    array<float, int(6)> _S103 = { (*_S101).x, (*_S101).y, (*_S101).z, (*_S101).w, (*_S102).x, (*_S102).y };

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
        float _S104 = max(0.0, dot(kGpAxes_0[axis_0], _S99));
        float expected_1 = expected_0 + _S103[axis_0] * _S104;
        float weight_1 = weight_0 + _S104;

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



    float slack_0 = (as_type<float>((kernelContext_22->giProbes_0->constants_0[1U].y)));
    float _S105 = expected_0 / weight_0 + slack_0;

#line 469
    if(span_0 > _S105)
    {

#line 470
        return 0.0;
    }
    float _S106 = max(slack_0, 0.00009999999747379);

#line 472
    float visible_0 = gpClamp_0((_S105 - span_0) / _S106, 0.0, 1.0);

#line 472
    axis_0 = 0U;

    for(;;)
    {

#line 474
        if(axis_0 < 6U)
        {
        }
        else
        {

#line 474
            break;
        }

#line 475
        float cosine_0 = dot(kGpAxes_0[axis_0], _S99);
        if(cosine_0 < 0.5)
        {

#line 477
            axis_0 = axis_0 + 1U;

#line 474
            continue;
        }

#line 474
        visible_0 = min(visible_0, gpClamp_0((_S103[axis_0] + slack_0 - span_0 * cosine_0) / _S106, 0.0, 1.0));

#line 474
        axis_0 = axis_0 + 1U;

#line 474
    }

#line 483
    return visible_0;
}


#line 300 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float grFacing_0(float3 toward_0, float3 normal_4)
{

#line 301
    return gpClamp_0((dot(normal_4, toward_0) + 1.0) * 0.5, 0.0, 1.0);
}


#line 123 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float giFinestVoxel_1(KernelContext_0 thread* kernelContext_23)
{

#line 124
    return (as_type<float>((kernelContext_23->giScene_0->constants_1[9U].y)));
}


#line 201
float giDistance_1(float3 p_2, KernelContext_0 thread* kernelContext_24)
{

#line 201
    uint _S107 = giLevelCount_0(kernelContext_24);

#line 201
    float answer_1 = 1.0e+06;

#line 201
    uint level_4 = 0U;


    for(;;)
    {

#line 204
        if(level_4 < _S107)
        {
        }
        else
        {

#line 204
            break;
        }

#line 205
        thread float farDistance_2 = 0.0;

#line 205
)cy_msl",
    R"cy_msl(        float _S108 = giSampleLevel_0(level_4, p_2, &farDistance_2, kernelContext_24);

        float _S109 = min(answer_1, _S108);
        if(_S108 < farDistance_2)
        {

#line 209
            return _S108;
        }

#line 204
        uint level_5 = level_4 + 1U;

#line 204
        answer_1 = _S109;

#line 204
        level_4 = level_5;

#line 204
    }

#line 212
    return answer_1;
}


#line 225
GiTraceHit_0 giSphereTrace_1(float3 origin_6, float3 direction_4, float maxDistance_2, float tMin_1, KernelContext_0 thread* kernelContext_25)
{

#line 226
    thread GiTraceHit_0 result_1;
    (&result_1)->hit_1 = false;
    (&result_1)->exhausted_0 = false;
    (&result_1)->t_1 = 0.0;
    (&result_1)->closest_0 = 0.0;
    (&result_1)->position_1 = float3(0.0, 0.0, 0.0);
    (&result_1)->normal_1 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S110 = giLevelCount_0(kernelContext_25);
    if(_S110 == 0U)
    {

#line 234
        return result_1;
    }

#line 234
    float _S111 = giFinestVoxel_0(kernelContext_25);


    float surface_1 = _S111 * 0.5;
    float _S112 = max(tMin_1, surface_1);
    thread array<float, int(3)> recent_1;

#line 239
    recent_1[int(0)] = 1.0e+06;

#line 239
    recent_1[int(1)] = 1.0e+06;

#line 239
    recent_1[int(2)] = 1.0e+06;

#line 239
    float closest_2 = 1.0e+06;

#line 239
    uint recentSlot_1 = 0U;

#line 239
    uint step_4 = 0U;

#line 239
    float t_5 = _S112;


    for(;;)
    {

#line 242
        if(step_4 < 192U)
        {
        }
        else
        {

#line 242
            break;
        }

#line 243
        if(t_5 > maxDistance_2)
        {
            (&result_1)->closest_0 = min(closest_2, min(recent_1[int(0)], min(recent_1[int(1)], recent_1[int(2)])));
            return result_1;
        }
        float3 position_6 = origin_6 + direction_4 * float3(t_5) ;

#line 248
        float _S113 = giDistance_0(position_6, kernelContext_25);

        if(_S113 < surface_1)
        {

#line 251
            (&result_1)->hit_1 = true;
            (&result_1)->t_1 = t_5;
            (&result_1)->position_1 = position_6;


            float3 _S114 = float3(_S111, 0.0, 0.0);

#line 256
            float _S115 = giDistance_0(position_6 + _S114, kernelContext_25);

#line 256
            float _S116 = giDistance_0(position_6 - _S114, kernelContext_25);

#line 256
            float _S117 = _S115 - _S116;

            float3 _S118 = float3(0.0, _S111, 0.0);

#line 258
            float _S119 = giDistance_0(position_6 + _S118, kernelContext_25);

#line 258
            float _S120 = giDistance_0(position_6 - _S118, kernelContext_25);

#line 258
            float _S121 = _S119 - _S120;

            float3 _S122 = float3(0.0, 0.0, _S111);

#line 260
            float _S123 = giDistance_0(position_6 + _S122, kernelContext_25);

#line 260
            float _S124 = giDistance_0(position_6 - _S122, kernelContext_25);

            (&result_1)->normal_1 = giNormalisedOr_0(float3(_S117, _S121, _S123 - _S124), - direction_4);
            (&result_1)->closest_0 = closest_2;
            return result_1;
        }
        float _S125 = min(closest_2, recent_1[recentSlot_1]);
        recent_1[recentSlot_1] = _S113;
        uint _S126 = (recentSlot_1 + 1U) % 3U;
        float t_6 = t_5 + max(_S113, surface_1);

#line 242
        uint step_5 = step_4 + 1U;

#line 242
        closest_2 = _S125;

#line 242
        recentSlot_1 = _S126;

#line 242
        step_4 = step_5;

#line 242
        t_5 = t_6;

#line 242
    }

#line 271
    (&result_1)->exhausted_0 = true;

    (&result_1)->closest_0 = min(closest_2, min(recent_1[int(0)], min(recent_1[int(1)], recent_1[int(2)])));
    return result_1;
}


#line 286 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
bool grProbeOccluded_0(float3 from_0, float3 to_0, KernelContext_0 thread* kernelContext_26)
{

#line 287
    float3 offset_2 = to_0 - from_0;
    float span_1 = length(offset_2);

#line 288
    float _S127 = giFinestVoxel_1(kernelContext_26);

    if(span_1 <= (_S127 * 2.0))
    {

#line 291
        return false;
    }

#line 291
    float _S128 = giDistance_1(from_0, kernelContext_26);

#line 291
    float _S129 = giDistance_1(to_0, kernelContext_26);

    if((_S128 + _S129) >= span_1)
    {

#line 294
        return false;
    }

#line 294
    GiTraceHit_0 _S130 = giSphereTrace_1(from_0, offset_2 / float3(span_1) , span_1 - _S127, 0.0, kernelContext_26);

    return _S130.hit_1;
}


#line 273 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
void gpLoadPayload_0(uint probe_3, uint floats_0, array<float, int(48)> thread* payload_0, KernelContext_0 thread* kernelContext_27)
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
        float _S131;
        if(index_0 < floats_0)
        {

#line 275
            _S131 = *(kernelContext_27->giProbes_0->payloads_0+(probe_3 * floats_0 + index_0));

#line 275
        }
        else
        {

#line 275
            _S131 = 0.0;

#line 275
        }

#line 275
        (*payload_0)[index_0] = _S131;

#line 274
        index_0 = index_0 + 1U;

#line 274
    }


    return;
}


#line 110
float3 gpMax0_0(float3 v_1)
{

#line 111
    return float3(max(0.0, v_1.x), max(0.0, v_1.y), max(0.0, v_1.z));
}


#line 154
void gpOctahedralUv_0(float3 direction_5, float thread* u_0, float thread* v_2)
{

#line 155
    float sum_0 = abs(direction_5.x) + abs(direction_5.y) + abs(direction_5.z);

#line 155
    float3 p_3;
    if(sum_0 > 0.0)
    {

#line 156
        p_3 = direction_5 / float3(sum_0) ;

#line 156
    }
    else
    {

#line 156
        p_3 = float3(0.0, 0.0, 1.0);

#line 156
    }
    float _S132 = p_3.x;
    float _S133 = p_3.y;

#line 158
    float px_0;

#line 158
    float py_0;
    if((p_3.z) < 0.0)
    {

#line 160
        float _S134 = 1.0 - abs(_S133);

#line 160
        if(_S132 >= 0.0)
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
        float fx_1 = _S134 * px_0;
        float _S135 = 1.0 - abs(_S132);

#line 161
        if(_S133 >= 0.0)
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
        float fy_1 = _S135 * px_0;

#line 161
        px_0 = fx_1;

#line 161
        py_0 = fy_1;

#line 159
    }
    else
    {

#line 159
        px_0 = _S132;

#line 159
        py_0 = _S133;

#line 159
    }

#line 165
    *u_0 = gpClamp_0(px_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    *v_2 = gpClamp_0(py_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    return;
}


#line 220
float3 gpDecode_0(uint encoding_0, const array<float, int(48)> thread* payload_1, float3 direction_6)
{

#line 220
    uint c_0;
    if(encoding_0 == 0U)
    {



        array<float, int(4)> _S136 = { 0.28209498524665833, 0.32573533058166504 * direction_6.y, 0.32573533058166504 * direction_6.z, 0.32573533058166504 * direction_6.x };

        thread float3 result_2 = float3(0.0, 0.0, 0.0);

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
            uint _S137 = c_0 * 3U;

#line 230
            result_2.x = result_2.x + (*payload_1)[_S137] * _S136[c_0];
            result_2.y = result_2.y + (*payload_1)[_S137 + 1U] * _S136[c_0];
            result_2.z = result_2.z + (*payload_1)[_S137 + 2U] * _S136[c_0];

#line 229
            c_0 = c_0 + 1U;

#line 229
        }

#line 234
        return gpMax0_0(result_2);
    }
    if(encoding_0 == 1U)
    {

#line 237
        thread float u_1 = 0.0;
        thread float v_3 = 0.0;
        gpOctahedralUv_0(direction_6, &u_1, &v_3);
        uint x0_1 = uint(u_1);
        uint y0_1 = uint(v_3);
        uint _S138 = min(x0_1 + 1U, 3U);



        uint _S139 = y0_1 * 4U;

        uint _S140 = min(y0_1 + 1U, 3U) * 4U;

        uint _S141 = (_S139 + x0_1) * 3U;

#line 250
        float3 f00_0 = float3((*payload_1)[_S141], (*payload_1)[_S141 + 1U], (*payload_1)[_S141 + 2U]);
        uint _S142 = (_S139 + _S138) * 3U;
        uint _S143 = (_S140 + x0_1) * 3U;

#line 252
        float3 f01_0 = float3((*payload_1)[_S143], (*payload_1)[_S143 + 1U], (*payload_1)[_S143 + 2U]);
        uint _S144 = (_S140 + _S138) * 3U;

#line 253
        float3 _S145 = float3((u_1 - float(x0_1))) ;
        float3 top_0 = f00_0 + (float3((*payload_1)[_S142], (*payload_1)[_S142 + 1U], (*payload_1)[_S142 + 2U]) - f00_0) * _S145;

        return top_0 + (f01_0 + (float3((*payload_1)[_S144], (*payload_1)[_S144 + 1U], (*payload_1)[_S144 + 2U]) - f01_0) * _S145 - top_0) * float3((v_3 - float(y0_1))) ;
    }
    float3 _S146 = float3(0.0, 0.0, 0.0);

#line 258
    float3 result_3 = _S146;

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
        float _S147 = max(0.0, dot(kGpAxes_0[c_0], direction_6));
        if(_S147 <= 0.0)
        {

#line 263
            c_0 = c_0 + 1U;

#line 260
            continue;
        }



        uint _S148 = c_0 * 3U;

        float total_1 = total_0 + _S147;

#line 267
        result_3 = result_3 + float3((*payload_1)[_S148], (*payload_1)[_S148 + 1U], (*payload_1)[_S148 + 2U]) * float3(_S147) ;

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
        result_3 = result_3 / float3(total_0) ;

#line 269
    }
    else
    {

#line 269
        result_3 = _S146;

#line 269
    }

#line 269
    return result_3;
}


#line 305 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float4 grFinish_0(float3 total_2, float weight_2, float oldest_0)
{

#line 306
    if(weight_2 <= 0.0)
    {

#line 307
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    return float4(total_2 / float3(weight_2) , gpClamp_0(1.0 - gpClamp_0(oldest_0 / 240.0, 0.0, 1.0) * 0.75, 0.0, 1.0));
}


#line 317
void grSampleTraced_0(float3 position_7, float3 normal_5, float3 reflection_0, uint frame_0, float4 thread* diffuse_0, float4 thread* specular_0, KernelContext_0 thread* kernelContext_28)
{
    uint4 counts_0 = kernelContext_28->giProbes_0->constants_0[0U];
    uint _S149 = counts_0.x;
    uint _S150 = counts_0.y;
    uint _S151 = kernelContext_28->giProbes_0->constants_0[1U].z;
    float _S152 = (as_type<float>((kernelContext_28->giProbes_0->constants_0[5U].x)));
    float3 _S153 = position_7 + normal_5 * float3((as_type<float>((kernelContext_28->giProbes_0->constants_0[1U].y)))) ;
    float3 _S154 = float3(0.0, 0.0, 0.0);

#line 325
    float3 diffuseTotal_0 = _S154;

#line 325
    float diffuseOldest_0 = 0.0;

#line 325
    float3 specularTotal_0 = _S154;

#line 325
    float specularOldest_0 = 0.0;

#line 325
    uint level_6 = 0U;

#line 325
    float diffuseWeight_0 = 0.0;

#line 325
    float specularWeight_0 = 0.0;

#line 331
    for(;;)
    {

#line 331
        if(level_6 < _S151)
        {
        }
        else
        {

#line 331
            break;
        }

#line 332
        float spacing_0 = _S152 * float(1U << level_6);
        float _S155 = position_7.x;

#line 333
        int _S156 = gpFloorDiv_0(_S155 - spacing_0, spacing_0);
        float _S157 = position_7.y;

#line 334
        int _S158 = gpFloorDiv_0(_S157 - spacing_0, spacing_0);
        float _S159 = position_7.z;
        int _S160 = gpFloorDiv_0(_S155 + spacing_0, spacing_0);
        int _S161 = gpFloorDiv_0(_S157 + spacing_0, spacing_0);
        int _S162 = gpFloorDiv_0(_S159 + spacing_0, spacing_0);

#line 338
        int z_2 = gpFloorDiv_0(_S159 - spacing_0, spacing_0);
        for(;;)
        {

#line 339
            if(z_2 <= _S162)
            {
            }
            else
            {

#line 339
                break;
            }

#line 339
            float3 diffuseTotal_1 = diffuseTotal_0;

#line 339
            float diffuseOldest_1 = diffuseOldest_0;

#line 339
            float3 specularTotal_1 = specularTotal_0;

#line 339
            float specularOldest_1 = specularOldest_0;

#line 339
            int y_3 = _S158;

#line 339
            float diffuseWeight_1 = diffuseWeight_0;

#line 339
            float specularWeight_1 = specularWeight_0;
            for(;;)
            {

#line 340
                if(y_3 <= _S161)
                {
                }
                else
                {

#line 340
                    break;
                }

#line 340
                float3 diffuseTotal_2 = diffuseTotal_1;

#line 340
                float diffuseOldest_2 = diffuseOldest_1;

#line 340
                float3 specularTotal_2 = specularTotal_1;

#line 340
                float specularOldest_2 = specularOldest_1;

#line 340
                int x_3 = _S156;

#line 340
                float diffuseWeight_2 = diffuseWeight_1;

#line 340
                float specularWeight_2 = specularWeight_1;
                for(;;)
                {

#line 341
                    if(x_3 <= _S160)
                    {
                    }
                    else
                    {

#line 341
                        break;
                    }

#line 341
                    uint _S163 = gpLayoutProbe_0(level_6, int3(x_3, y_3, z_2), kernelContext_28);

                    if(_S163 == 4294967295U)
                    {

#line 344
                        x_3 = x_3 + int(1);

#line 341
                        continue;
                    }



                    uint _S164 = _S163 * 4U;

#line 346
                    float4 device* _S165 = kernelContext_28->giProbes_0->probes_0+(_S164 + 1U);

#line 346
                    float4 record_2 = *_S165;

#line 346
                    bool _S166;
                    if(((*_S165).y) == 0.0)
                    {

#line 347
                        _S166 = true;

#line 347
                    }
                    else
                    {

#line 347
                        _S166 = (record_2.z) == 0.0;

#line 347
                    }

#line 347
                    if(_S166)
                    {

#line 348
                        x_3 = x_3 + int(1);

#line 341
                        continue;
                    }

#line 350
                    float3 at_0 = (*(kernelContext_28->giProbes_0->probes_0+_S164)).xyz;
                    float3 offset_3 = position_7 - at_0;
                    float span_2 = length(offset_3);
                    if(span_2 > spacing_0)
                    {

#line 354
                        x_3 = x_3 + int(1);

#line 341
                        continue;
                    }

#line 356
                    float _S167 = 1.0 - gpClamp_0(span_2 / spacing_0, 0.0, 1.0);

#line 356
                    float _S168 = gpVisibilityWeight_0(_S163, at_0, position_7, kernelContext_28);

#line 356
                    float falloff_0 = _S167 * _S168;

                    bool apart_0 = span_2 > 0.00009999999747379;

#line 358
                    float3 toward_1;
                    if(apart_0)
                    {

#line 359
                        toward_1 = - offset_3 / float3(span_2) ;

#line 359
                    }
                    else
                    {

#line 359
                        toward_1 = normal_5;

#line 359
                    }

#line 359
                    float diffuseShare_0;
                    if(apart_0)
                    {

#line 360
                        diffuseShare_0 = falloff_0 * grFacing_0(toward_1, normal_5);

#line 360
                    }
                    else
                    {

#line 360
                        diffuseShare_0 = falloff_0;

#line 360
                    }

#line 360
                    float specularShare_0;

                    if(apart_0)
                    {

#line 362
)cy_msl",
    R"cy_msl(                        specularShare_0 = falloff_0 * grFacing_0(toward_1, reflection_0);

#line 362
                    }
                    else
                    {

#line 362
                        specularShare_0 = falloff_0;

#line 362
                    }

#line 362
                    bool _S169;
                    if(diffuseShare_0 <= 0.0)
                    {

#line 363
                        _S169 = specularShare_0 <= 0.0;

#line 363
                    }
                    else
                    {

#line 363
                        _S169 = false;

#line 363
                    }

#line 363
                    bool _S170;

#line 363
                    if(_S169)
                    {

#line 363
                        _S170 = true;

#line 363
                    }
                    else
                    {

#line 363
                        bool _S171 = grProbeOccluded_0(_S153, at_0, kernelContext_28);

#line 363
                        _S170 = _S171;

#line 363
                    }

#line 363
                    if(_S170)
                    {
                        x_3 = x_3 + int(1);

#line 341
                        continue;
                    }

#line 367
                    thread array<float, int(48)> data_0;

#line 367
                    gpLoadPayload_0(_S163, _S150, &data_0, kernelContext_28);

                    uint last_1 = (as_type<uint>((record_2.x)));

#line 369
                    float age_1;
                    if(frame_0 > last_1)
                    {

#line 370
                        age_1 = float(frame_0 - last_1);

#line 370
                    }
                    else
                    {

#line 370
                        age_1 = 0.0;

#line 370
                    }

#line 370
                    float3 diffuseTotal_3;

#line 370
                    float diffuseOldest_3;

#line 370
                    float diffuseWeight_3;
                    if(diffuseShare_0 > 0.0)
                    {

#line 371
                        thread array<float, int(48)> _S172 = data_0;

#line 371
                        float3 _S173 = gpDecode_0(_S149, &_S172, normal_5);

                        float diffuseWeight_4 = diffuseWeight_2 + diffuseShare_0;
                        float _S174 = max(diffuseOldest_2, age_1);

#line 374
                        diffuseTotal_3 = diffuseTotal_2 + _S173 * float3(diffuseShare_0) ;

#line 374
                        diffuseOldest_3 = _S174;

#line 374
                        diffuseWeight_3 = diffuseWeight_4;

#line 371
                    }
                    else
                    {

#line 371
                        diffuseTotal_3 = diffuseTotal_2;

#line 371
                        diffuseOldest_3 = diffuseOldest_2;

#line 371
                        diffuseWeight_3 = diffuseWeight_2;

#line 371
                    }

#line 371
                    float3 specularTotal_3;

#line 371
                    float specularOldest_3;

#line 371
                    float specularWeight_3;

#line 376
                    if(specularShare_0 > 0.0)
                    {

#line 376
                        thread array<float, int(48)> _S175 = data_0;

#line 376
                        float3 _S176 = gpDecode_0(_S149, &_S175, reflection_0);


                        float specularWeight_4 = specularWeight_2 + specularShare_0;
                        float _S177 = max(specularOldest_2, age_1);

#line 380
                        specularTotal_3 = specularTotal_2 + _S176 * float3(specularShare_0) ;

#line 380
                        specularOldest_3 = _S177;

#line 380
                        specularWeight_3 = specularWeight_4;

#line 376
                    }
                    else
                    {

#line 376
                        specularTotal_3 = specularTotal_2;

#line 376
                        specularOldest_3 = specularOldest_2;

#line 376
                        specularWeight_3 = specularWeight_2;

#line 376
                    }

#line 376
                    diffuseTotal_2 = diffuseTotal_3;

#line 376
                    diffuseOldest_2 = diffuseOldest_3;

#line 376
                    specularTotal_2 = specularTotal_3;

#line 376
                    specularOldest_2 = specularOldest_3;

#line 376
                    diffuseWeight_2 = diffuseWeight_3;

#line 376
                    specularWeight_2 = specularWeight_3;

#line 341
                    x_3 = x_3 + int(1);

#line 341
                }

#line 340
                int y_4 = y_3 + int(1);

#line 340
                diffuseTotal_1 = diffuseTotal_2;

#line 340
                diffuseOldest_1 = diffuseOldest_2;

#line 340
                specularTotal_1 = specularTotal_2;

#line 340
                specularOldest_1 = specularOldest_2;

#line 340
                y_3 = y_4;

#line 340
                diffuseWeight_1 = diffuseWeight_2;

#line 340
                specularWeight_1 = specularWeight_2;

#line 340
            }

#line 339
            int z_3 = z_2 + int(1);

#line 339
            diffuseTotal_0 = diffuseTotal_1;

#line 339
            diffuseOldest_0 = diffuseOldest_1;

#line 339
            specularTotal_0 = specularTotal_1;

#line 339
            specularOldest_0 = specularOldest_1;

#line 339
            z_2 = z_3;

#line 339
            diffuseWeight_0 = diffuseWeight_1;

#line 339
            specularWeight_0 = specularWeight_1;

#line 339
        }

#line 331
        level_6 = level_6 + 1U;

#line 331
    }

#line 386
    *diffuse_0 = grFinish_0(diffuseTotal_0, diffuseWeight_0, diffuseOldest_0);
    *specular_0 = grFinish_0(specularTotal_0, specularWeight_0, specularOldest_0);
    return;
}


#line 375 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
bool spInvalidated_0(float3 point_0, KernelContext_0 thread* kernelContext_29)
{

#line 376
    uint _S178 = min(kernelContext_29->giResolve_0->constants_2[26U].x, 4U);

#line 376
    uint region_0 = 0U;
    for(;;)
    {

#line 377
        if(region_0 < _S178)
        {
        }
        else
        {

#line 377
            break;
        }

#line 378
        uint _S179 = region_0 * 2U;

#line 378
        float3 lo_1 = (as_type<float3>((kernelContext_29->giResolve_0->constants_2[27U + _S179].xyz)));
        float3 hi_1 = (as_type<float3>((kernelContext_29->giResolve_0->constants_2[28U + _S179].xyz)));
        float _S180 = point_0.x;

#line 380
        bool _S181;

#line 380
        if(_S180 >= (lo_1.x))
        {

#line 380
            _S181 = (point_0.y) >= (lo_1.y);

#line 380
        }
        else
        {

#line 380
            _S181 = false;

#line 380
        }

#line 380
        bool _S182;

#line 380
        if(_S181)
        {

#line 380
            _S182 = (point_0.z) >= (lo_1.z);

#line 380
        }
        else
        {

#line 380
            _S182 = false;

#line 380
        }

#line 380
        bool _S183;

#line 380
        if(_S182)
        {

#line 380
            _S183 = _S180 <= (hi_1.x);

#line 380
        }
        else
        {

#line 380
            _S183 = false;

#line 380
        }

#line 380
        bool _S184;

#line 380
        if(_S183)
        {

#line 380
            _S184 = (point_0.y) <= (hi_1.y);

#line 380
        }
        else
        {

#line 380
            _S184 = false;

#line 380
        }

#line 380
        bool _S185;
        if(_S184)
        {

#line 381
            _S185 = (point_0.z) <= (hi_1.z);

#line 381
        }
        else
        {

#line 381
            _S185 = false;

#line 381
        }

#line 380
        if(_S185)
        {
            return true;
        }

#line 377
        region_0 = region_0 + 1U;

#line 377
    }

#line 385
    return false;
}


#line 49
uint spWidth_0(KernelContext_0 thread* kernelContext_30)
{

#line 49
    return kernelContext_30->giResolve_0->constants_2[40U].x;
}


#line 50
uint spHeight_0(KernelContext_0 thread* kernelContext_31)
{

#line 50
    return kernelContext_31->giResolve_0->constants_2[40U].y;
}


#line 335
bool spLanding_0(float3 position_8, uint thread* x_4, uint thread* y_5, KernelContext_0 thread* kernelContext_32)
{

#line 336
    *x_4 = 0U;
    *y_5 = 0U;

#line 337
    float4 _S186 = float4(position_8.x) ;

#line 337
    float4 _S187 = float4(position_8.y) ;

#line 337
    float4 _S188 = float4(position_8.z) ;

#line 342
    float4 clip_0 = (as_type<float4>((kernelContext_32->giResolve_0->constants_2[6U]))) * _S186 + (as_type<float4>((kernelContext_32->giResolve_0->constants_2[7U]))) * _S187 + (as_type<float4>((kernelContext_32->giResolve_0->constants_2[8U]))) * _S188 + (as_type<float4>((kernelContext_32->giResolve_0->constants_2[9U])));
    float _S189 = clip_0.w;

#line 343
    if(_S189 <= 9.99999993922529029e-09)
    {

#line 344
        return false;
    }
    float inverse_0 = 1.0 / _S189;

    float _S190 = clip_0.x * inverse_0 * 0.5 + 0.5;

#line 348
    float _S191 = clip_0.y * inverse_0 * 0.5 + 0.5;

#line 353
    float4 clip_1 = (as_type<float4>((kernelContext_32->giResolve_0->constants_2[10U]))) * _S186 + (as_type<float4>((kernelContext_32->giResolve_0->constants_2[11U]))) * _S187 + (as_type<float4>((kernelContext_32->giResolve_0->constants_2[12U]))) * _S188 + (as_type<float4>((kernelContext_32->giResolve_0->constants_2[13U])));
    float _S192 = clip_1.w;

#line 354
    if(_S192 <= 9.99999993922529029e-09)
    {

#line 355
        return false;
    }
    float inverse_1 = 1.0 / _S192;



    float _S193 = _S190 + (clip_1.x * inverse_1 * 0.5 + 0.5 - _S190);

#line 361
    float _S194 = _S191 + (clip_1.y * inverse_1 * 0.5 + 0.5 - _S191);

#line 361
    uint _S195 = spWidth_0(kernelContext_32);
    float width_0 = float(_S195);

#line 362
    uint _S196 = spHeight_0(kernelContext_32);
    float height_0 = float(_S196);
    float fx_2 = floor(_S193 * width_0);
    float fy_2 = floor((1.0 - _S194) * height_0);

#line 365
    bool _S197;
    if(fx_2 < 0.0)
    {

#line 366
        _S197 = true;

#line 366
    }
    else
    {

#line 366
        _S197 = fy_2 < 0.0;

#line 366
    }

#line 366
    if(_S197)
    {

#line 366
        _S197 = true;

#line 366
    }
    else
    {

#line 366
        _S197 = fx_2 >= width_0;

#line 366
    }

#line 366
    if(_S197)
    {

#line 366
        _S197 = true;

#line 366
    }
    else
    {

#line 366
        _S197 = fy_2 >= height_0;

#line 366
    }

#line 366
    if(_S197)
    {

#line 367
        return false;
    }
    *x_4 = uint(fx_2);
    *y_5 = uint(fy_2);
    return true;
}


#line 139
struct SpTaps_0
{
    array<uint, int(4)> index_1;
    array<float, int(4)> weight_3;
    uint count_0;
};


#line 51
uint spSpacing_0(KernelContext_0 thread* kernelContext_33)
{

#line 51
    return kernelContext_33->giResolve_0->constants_2[40U].z;
}


#line 53
uint spTilesX_0(KernelContext_0 thread* kernelContext_34)
{

#line 53
    return kernelContext_34->giResolve_0->constants_2[41U].x;
}


#line 54
uint spTilesY_0(KernelContext_0 thread* kernelContext_35)
{

#line 54
    return kernelContext_35->giResolve_0->constants_2[41U].y;
}


#line 145
void spAxisTaps_0(uint coordinate_0, uint step_6, uint count_1, uint thread* base_1, float thread* fraction_0)
{

#line 146
    float g_0 = (float(coordinate_0) - float(step_6 / 2U)) / float(step_6);
    if(g_0 <= 0.0)
    {

#line 148
        *base_1 = 0U;
        *fraction_0 = 0.0;
        return;
    }
    uint _S198 = uint(g_0);

#line 152
    *base_1 = _S198;
    *fraction_0 = g_0 - float(_S198);
    if((*base_1 + 1U) >= count_1)
    {

#line 155
        *base_1 = count_1 - 1U;
        *fraction_0 = 0.0;

#line 154
    }



    return;
}


#line 160
SpTaps_0 spBilinearTaps_0(uint x_5, uint y_6, uint step_7, uint countX_0, uint countY_0)
{

#line 161
    thread SpTaps_0 taps_0;
    (&taps_0)->count_0 = 0U;

#line 162
    uint t_7 = 0U;
    for(;;)
    {

#line 163
        if(t_7 < 4U)
        {
        }
        else
        {

#line 163
            break;
        }

#line 164
        (&taps_0)->index_1[t_7] = 0U;
        (&taps_0)->weight_3[t_7] = 0.0;

#line 163
        t_7 = t_7 + 1U;

#line 163
    }



    thread uint bx_1;

    thread float fx_3;

    spAxisTaps_0(x_5, step_7, countX_0, &bx_1, &fx_3);

#line 168
    thread uint by_1;

    thread float fy_3;

    spAxisTaps_0(y_6, step_7, countY_0, &by_1, &fy_3);

#line 172
    uint j_0 = 0U;
    for(;;)
    {

#line 173
        if(j_0 < 2U)
        {
        }
        else
        {

#line 173
            break;
        }

#line 173
        uint i_0 = 0U;
        for(;;)
        {

#line 174
            if(i_0 < 2U)
            {
            }
            else
            {

#line 174
                break;
            }

#line 174
            float _S199;
            if(i_0 == 0U)
            {

#line 175
                _S199 = 1.0 - fx_3;

#line 175
            }
            else
            {

#line 175
                _S199 = fx_3;

#line 175
            }

#line 175
            float _S200;

#line 175
            if(j_0 == 0U)
            {

#line 175
                _S200 = 1.0 - fy_3;

#line 175
            }
            else
            {

#line 175
                _S200 = fy_3;

#line 175
            }

#line 175
            float weight_4 = _S199 * _S200;
            if(weight_4 <= 0.0)
            {

#line 177
                i_0 = i_0 + 1U;

#line 174
                continue;
            }



            (&taps_0)->index_1[(&taps_0)->count_0] = (by_1 + j_0) * countX_0 + (bx_1 + i_0);
            (&taps_0)->weight_3[(&taps_0)->count_0] = weight_4;
            (&taps_0)->count_0 = (&taps_0)->count_0 + 1U;

#line 174
            i_0 = i_0 + 1U;

#line 174
        }

#line 173
        j_0 = j_0 + 1U;

#line 173
    }

#line 184
    return taps_0;
}


#line 52
uint spCell_0(KernelContext_0 thread* kernelContext_36)
{

#line 52
    return kernelContext_36->giResolve_0->constants_2[40U].w;
}

uint spCellsX_0(KernelContext_0 thread* kernelContext_37)
{

#line 55
    return kernelContext_37->giResolve_0->constants_2[41U].z;
}


#line 56
uint spCellsY_0(KernelContext_0 thread* kernelContext_38)
{

#line 56
    return kernelContext_38->giResolve_0->constants_2[41U].w;
}


#line 55
uint spCellsX_1(KernelContext_0 thread* kernelContext_39)
{

#line 55
    return kernelContext_39->giResolve_0->constants_2[41U].z;
}


#line 56
uint spCellsY_1(KernelContext_0 thread* kernelContext_40)
{

#line 56
    return kernelContext_40->giResolve_0->constants_2[41U].w;
}


#line 80
uint spTableIndex_0(uint slot_5, uint cell_3, KernelContext_0 thread* kernelContext_41)
{

#line 80
    uint _S201 = spCellsX_1(kernelContext_41);
    uint _S202 = slot_5 * _S201;

#line 81
    uint _S203 = spCellsY_1(kernelContext_41);

#line 81
    return 4U + _S202 * _S203 + cell_3;
}


#line 119
float spGeometryWeight_0(float3 probePosition_0, float3 probeNormal_0, float3 position_9, float3 normal_6, float depth_1, float tolerance_0)
{
    float plane_0 = abs(dot(probeNormal_0, position_9 - probePosition_0));
    float reach_1 = tolerance_0 * depth_1;
    if(reach_1 <= 0.0)
    {

#line 124
        return 0.0;
    }

    float _S204 = max(0.0, dot(probeNormal_0, normal_6));
    float facing_0 = _S204 * _S204;
    float facing_1 = facing_0 * facing_0;

    return max(0.0, 1.0 - plane_0 / reach_1) * (facing_1 * facing_1);
}


#line 226
void spLoadCoefficients_0(uint slot_6, uint record_3, array<float3, int(9)> thread* coefficients_0, float thread* history_0, KernelContext_0 thread* kernelContext_42)
{

#line 226
    uint _S205 = spRecordBase_0(slot_6, record_3, kernelContext_42);
    uint _S206 = _S205 + 2U;
    thread array<float, int(28)> values_0;

#line 228
    uint v_4 = 0U;
    for(;;)
    {

#line 229
        if(v_4 < 7U)
        {
        }
        else
        {

#line 229
            break;
        }

#line 230
        float4 device* _S207 = kernelContext_42->giResolve_0->probes_1+(_S206 + v_4);
        uint _S208 = v_4 * 4U;

#line 231
        values_0[_S208] = (*_S207).x;
        values_0[_S208 + 1U] = (*_S207).y;
)cy_msl",
    R"cy_msl(        values_0[_S208 + 2U] = (*_S207).z;
        values_0[_S208 + 3U] = (*_S207).w;

#line 229
        v_4 = v_4 + 1U;

#line 229
    }

#line 229
    uint k_0 = 0U;

#line 236
    for(;;)
    {

#line 236
        if(k_0 < 9U)
        {
        }
        else
        {

#line 236
            break;
        }

#line 237
        uint _S209 = k_0 * 3U;

#line 237
        (*coefficients_0)[k_0] = float3(values_0[_S209], values_0[_S209 + 1U], values_0[_S209 + 2U]);

#line 236
        k_0 = k_0 + 1U;

#line 236
    }


    *history_0 = values_0[int(27)];
    return;
}


#line 389
bool spReproject_0(const SpProbe_0 thread* probe_4, float tolerance_1, array<float3, int(9)> thread* coefficients_1, float thread* history_1, KernelContext_0 thread* kernelContext_43)
{

#line 389
    uint k_1 = 0U;
    for(;;)
    {

#line 390
        if(k_1 < 9U)
        {
        }
        else
        {

#line 390
            break;
        }

#line 391
        (*coefficients_1)[k_1] = float3(0.0, 0.0, 0.0);

#line 390
        k_1 = k_1 + 1U;

#line 390
    }


    *history_1 = 0.0;

#line 393
    float3 _S210 = probe_4->position_4;

#line 393
    bool _S211 = spInvalidated_0(probe_4->position_4, kernelContext_43);
    if(_S211)
    {

#line 395
        return false;
    }
    thread uint x_6;
    thread uint y_7;

#line 398
    bool _S212 = spLanding_0(_S210, &x_6, &y_7, kernelContext_43);
    if(!_S212)
    {

#line 400
        return false;
    }

#line 400
    uint _S213 = spSlot_0(kernelContext_43);


    uint previous_0 = 1U - _S213;

#line 403
    float bestWeight_0 = 0.0;

#line 403
    uint best_4 = 4294967295U;

#line 403
    uint pass_0 = 0U;


    for(;;)
    {

#line 406
        if(pass_0 < 2U)
        {
        }
        else
        {

#line 406
            break;
        }

#line 406
        SpTaps_0 _S214;

        if(pass_0 == 0U)
        {

#line 408
            uint _S215 = spSpacing_0(kernelContext_43);

#line 408
            uint _S216 = spTilesX_0(kernelContext_43);

#line 408
            uint _S217 = spTilesY_0(kernelContext_43);

#line 408
            _S214 = spBilinearTaps_0(x_6, y_7, _S215, _S216, _S217);

#line 408
        }
        else
        {

#line 408
            uint _S218 = spCell_0(kernelContext_43);

#line 408
            uint _S219 = spCellsX_0(kernelContext_43);

#line 408
            uint _S220 = spCellsY_0(kernelContext_43);

#line 408
            _S214 = spBilinearTaps_0(x_6, y_7, _S218, _S219, _S220);

#line 408
        }

#line 408
        float bestWeight_1 = bestWeight_0;

#line 408
        uint best_5 = best_4;

#line 408
        uint tap_0 = 0U;

        for(;;)
        {

#line 410
            if(tap_0 < (_S214.count_0))
            {
            }
            else
            {

#line 410
                break;
            }

#line 408
            SpTaps_0 _S221 = _S214;

            uint _S222 = tap_0;

#line 410
            uint record_4;

            if(pass_0 == 1U)
            {

#line 412
                uint _S223 = spTableIndex_0(previous_0, _S221.index_1[_S222], kernelContext_43);
                uint device* _S224 = kernelContext_43->giResolve_0->slots_0+_S223;

#line 413
                uint slot_7 = *_S224;
                if((*_S224) == 4294967295U)
                {

#line 415
                    tap_0 = tap_0 + 1U;

#line 410
                    continue;
                }

#line 410
                uint _S225 = spUniformCount_0(kernelContext_43);

#line 410
                record_4 = _S225 + slot_7;

            }
            else
            {

#line 412
                record_4 = _S221.index_1[_S222];

#line 412
            }

#line 412
            SpProbe_0 _S226 = spLoadProbe_0(previous_0, record_4, kernelContext_43);

#line 420
            if((_S226.depth_0) <= 0.0)
            {

#line 421
                tap_0 = tap_0 + 1U;

#line 410
                continue;
            }

#line 424
            float weight_5 = _S214.weight_3[tap_0] * spGeometryWeight_0(_S226.position_4, _S226.normal_3, _S210, probe_4->normal_3, probe_4->depth_0, tolerance_1);

#line 424
            uint best_6;

#line 424
            float bestWeight_2;


            if(weight_5 > bestWeight_1)
            {

#line 427
                bestWeight_2 = weight_5;

#line 427
                best_6 = record_4;

#line 427
            }
            else
            {

#line 427
                bestWeight_2 = bestWeight_1;

#line 427
                best_6 = best_5;

#line 427
            }

#line 427
            bestWeight_1 = bestWeight_2;

#line 427
            best_5 = best_6;

#line 410
            tap_0 = tap_0 + 1U;

#line 410
        }

#line 406
        uint pass_1 = pass_0 + 1U;

#line 406
        bestWeight_0 = bestWeight_1;

#line 406
        best_4 = best_5;

#line 406
        pass_0 = pass_1;

#line 406
    }

#line 406
    bool _S227;

#line 433
    if(best_4 == 4294967295U)
    {

#line 433
        _S227 = true;

#line 433
    }
    else
    {

#line 433
        _S227 = bestWeight_0 < 0.25;

#line 433
    }

#line 433
    if(_S227)
    {

#line 434
        return false;
    }

#line 434
    spLoadCoefficients_0(previous_0, best_4, coefficients_1, history_1, kernelContext_43);


    return true;
}


#line 88 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
void grBasis_0(float3 normal_7, float3 thread* tangent_0, float3 thread* bitangent_0)
{

#line 89
    float _S228 = normal_7.z;

#line 89
    float sign_0;

#line 89
    if(_S228 >= 0.0)
    {

#line 89
        sign_0 = 1.0;

#line 89
    }
    else
    {

#line 89
        sign_0 = -1.0;

#line 89
    }
    float a_1 = -1.0 / (sign_0 + _S228);
    float _S229 = normal_7.x;

#line 91
    float _S230 = normal_7.y;

#line 91
    float b_1 = _S229 * _S230 * a_1;
    *tangent_0 = float3(1.0 + sign_0 * _S229 * _S229 * a_1, sign_0 * b_1, - sign_0 * _S229);
    *bitangent_0 = float3(b_1, sign_0 + _S230 * _S230 * a_1, - _S230);
    return;
}


#line 188 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
float3 spHemisphereDirection_0(float3 normal_8, float z_4, float turn_0)
{

#line 189
    thread float3 tangent_1;
    thread float3 bitangent_1;
    grBasis_0(normal_8, &tangent_1, &bitangent_1);
    float radius_1 = sqrt(max(0.0, 1.0 - z_4 * z_4));
    float angle_0 = 6.28318548202514648 * turn_0;
    return giNormalisedOr_0(tangent_1 * float3((radius_1 * cos(angle_0)))  + bitangent_1 * float3((radius_1 * sin(angle_0)))  + normal_8 * float3(z_4) , normal_8);
}


#line 90
void spBasis_0(float3 d_0, array<float, int(9)> thread* basis_0)
{

#line 91
    (*basis_0)[int(0)] = 0.28209498524665833;
    float _S231 = d_0.y;

#line 92
    (*basis_0)[int(1)] = 0.48860299587249756 * _S231;
    float _S232 = d_0.z;

#line 93
    (*basis_0)[int(2)] = 0.48860299587249756 * _S232;
    float _S233 = d_0.x;

#line 94
    (*basis_0)[int(3)] = 0.48860299587249756 * _S233;
    (*basis_0)[int(4)] = 1.09254801273345947 * (_S233 * _S231);
    (*basis_0)[int(5)] = 1.09254801273345947 * (_S231 * _S232);
    (*basis_0)[int(6)] = 0.31539198756217957 * (3.0 * (_S232 * _S232) - 1.0);
    (*basis_0)[int(7)] = 1.09254801273345947 * (_S233 * _S232);
    (*basis_0)[int(8)] = 0.54627400636672974 * (_S233 * _S233 - _S231 * _S231);
    return;
}


#line 102
uint spBandOf_0(uint k_2)
{

#line 102
    uint _S234;
    if(k_2 == 0U)
    {

#line 103
        _S234 = 0U;

#line 103
    }
    else
    {

#line 103
        if(k_2 < 4U)
        {

#line 103
            _S234 = 1U;

#line 103
        }
        else
        {

#line 103
            _S234 = 2U;

#line 103
        }

#line 103
    }

#line 103
    return _S234;
}

float3 spEvaluate_0(const array<float3, int(9)> thread* coefficients_2, float3 direction_7, float3 band_0)
{

#line 107
    thread array<float, int(9)> basis_1;
    spBasis_0(direction_7, &basis_1);

#line 108
    float3 sum_1 = float3(0.0, 0.0, 0.0);

#line 108
    uint k_3 = 0U;

    for(;;)
    {

#line 110
        if(k_3 < 9U)
        {
        }
        else
        {

#line 110
            break;
        }

#line 111
        uint b_2 = spBandOf_0(k_3);

#line 111
        float scale_0;
        if(b_2 == 0U)
        {

#line 112
            scale_0 = band_0.x;

#line 112
        }
        else
        {

#line 112
            if(b_2 == 1U)
            {

#line 112
                scale_0 = band_0.y;

#line 112
            }
            else
            {

#line 112
                scale_0 = band_0.z;

#line 112
            }

#line 112
        }
        float3 _S235 = sum_1 + (*coefficients_2)[k_3] * float3((basis_1[k_3] * scale_0)) ;

#line 110
        uint k_4 = k_3 + 1U;

#line 110
        sum_1 = _S235;

#line 110
        k_3 = k_4;

#line 110
    }

#line 115
    return float3(max(sum_1.x, 0.0), max(sum_1.y, 0.0), max(sum_1.z, 0.0));
}


#line 506 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
float4 gpSample_0(float3 position_10, float3 normal_9, uint frame_1, KernelContext_0 thread* kernelContext_44)
{

#line 507
    uint4 counts_1 = kernelContext_44->giProbes_0->constants_0[0U];
    uint _S236 = counts_1.x;
    uint _S237 = counts_1.y;
    uint _S238 = kernelContext_44->giProbes_0->constants_0[1U].z;
    float _S239 = (as_type<float>((kernelContext_44->giProbes_0->constants_0[5U].x)));

#line 511
    float3 total_3 = float3(0.0, 0.0, 0.0);

#line 511
    float oldest_1 = 0.0;

#line 511
    uint level_7 = 0U;

#line 511
    float totalWeight_0 = 0.0;



    for(;;)
    {

#line 515
        if(level_7 < _S238)
        {
        }
        else
        {

#line 515
            break;
        }

#line 516
        float spacing_1 = _S239 * float(1U << level_7);
        float radius_2 = spacing_1 * 1.5;
        float _S240 = position_10.x;

#line 518
        int _S241 = gpFloorDiv_0(_S240 - radius_2, spacing_1);
        float _S242 = position_10.y;

#line 519
        int _S243 = gpFloorDiv_0(_S242 - radius_2, spacing_1);
        float _S244 = position_10.z;
        int _S245 = gpFloorDiv_0(_S240 + radius_2, spacing_1);
        int _S246 = gpFloorDiv_0(_S242 + radius_2, spacing_1);
        int _S247 = gpFloorDiv_0(_S244 + radius_2, spacing_1);

#line 523
        int z_5 = gpFloorDiv_0(_S244 - radius_2, spacing_1);
        for(;;)
        {

#line 524
            if(z_5 <= _S247)
            {
            }
            else
            {

#line 524
                break;
            }

#line 524
            float3 total_4 = total_3;

#line 524
            float oldest_2 = oldest_1;

#line 524
            int y_8 = _S243;

#line 524
            float totalWeight_1 = totalWeight_0;
            for(;;)
            {

#line 525
                if(y_8 <= _S246)
                {
                }
                else
                {

#line 525
                    break;
                }

#line 525
                float3 total_5 = total_4;

#line 525
                float oldest_3 = oldest_2;

#line 525
                int x_7 = _S241;

#line 525
                float totalWeight_2 = totalWeight_1;
                for(;;)
                {

#line 526
                    if(x_7 <= _S245)
                    {
                    }
                    else
                    {

#line 526
                        break;
                    }

#line 526
                    uint _S248 = gpLayoutProbe_0(level_7, int3(x_7, y_8, z_5), kernelContext_44);

                    if(_S248 == 4294967295U)
                    {

#line 529
                        x_7 = x_7 + int(1);

#line 526
                        continue;
                    }



                    uint _S249 = _S248 * 4U;

#line 531
                    float4 device* _S250 = kernelContext_44->giProbes_0->probes_0+(_S249 + 1U);

#line 531
                    float4 record_5 = *_S250;

#line 531
                    bool _S251;
                    if(((*_S250).y) == 0.0)
                    {

#line 532
                        _S251 = true;

#line 532
                    }
                    else
                    {

#line 532
                        _S251 = (record_5.z) == 0.0;

#line 532
                    }

#line 532
                    if(_S251)
                    {

#line 533
                        x_7 = x_7 + int(1);

#line 526
                        continue;
                    }

#line 535
                    float3 at_1 = (*(kernelContext_44->giProbes_0->probes_0+_S249)).xyz;
                    float3 offset_4 = position_10 - at_1;
                    float span_3 = length(offset_4);
                    if(span_3 > radius_2)
                    {

#line 539
                        x_7 = x_7 + int(1);

#line 526
                        continue;
                    }

#line 541
                    float weight_6 = 1.0 - gpClamp_0(span_3 / radius_2, 0.0, 1.0);

#line 541
                    float weight_7;
                    if(span_3 > 0.00009999999747379)
                    {

#line 542
                        weight_7 = weight_6 * gpClamp_0((dot(normal_9, - offset_4 / float3(span_3) ) + 1.0) * 0.5, 0.0, 1.0);

#line 542
                    }
                    else
                    {

#line 542
                        weight_7 = weight_6;

#line 542
                    }

#line 542
                    float _S252 = gpVisibilityWeight_0(_S248, at_1, position_10, kernelContext_44);



                    float weight_8 = weight_7 * _S252;
                    if(weight_8 <= 0.0)
                    {

#line 548
                        x_7 = x_7 + int(1);

#line 526
                        continue;
                    }

#line 550
                    thread array<float, int(48)> data_1;

#line 550
                    gpLoadPayload_0(_S248, _S237, &data_1, kernelContext_44);

#line 550
                    thread array<float, int(48)> _S253 = data_1;

#line 550
                    float3 _S254 = gpDecode_0(_S236, &_S253, normal_9);

                    float3 _S255 = total_5 + _S254 * float3(weight_8) ;
                    float totalWeight_3 = totalWeight_2 + weight_8;
                    uint last_2 = (as_type<uint>((record_5.x)));

#line 554
                    float age_2;
                    if(frame_1 > last_2)
                    {

#line 555
                        age_2 = float(frame_1 - last_2);

#line 555
                    }
                    else
                    {

#line 555
                        age_2 = 0.0;

#line 555
                    }
                    float _S256 = max(oldest_3, age_2);

#line 556
                    total_5 = _S255;

#line 556
                    oldest_3 = _S256;

#line 556
                    totalWeight_2 = totalWeight_3;

#line 526
                    x_7 = x_7 + int(1);

#line 526
                }

#line 525
                int y_9 = y_8 + int(1);

#line 525
                total_4 = total_5;

#line 525
                oldest_2 = oldest_3;

#line 525
                y_8 = y_9;

#line 525
                totalWeight_1 = totalWeight_2;

#line 525
            }

#line 524
            int z_6 = z_5 + int(1);

#line 524
            total_3 = total_4;

#line 524
            oldest_1 = oldest_2;

#line 524
            z_5 = z_6;

#line 524
            totalWeight_0 = totalWeight_1;

#line 524
        }

#line 515
        level_7 = level_7 + 1U;

#line 515
    }

#line 561
    if(totalWeight_0 <= 0.0)
    {

#line 562
        return float4(0.0, 0.0, 0.0, 0.0);
    }



    return float4(total_3 / float3(totalWeight_0) , gpClamp_0(1.0 - gpClamp_0(oldest_1 / 240.0, 0.0, 1.0) * 0.75, 0.0, 1.0));
}


#line 134 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
float spLuminance_0(float3 c_1)
{

#line 135
    return 0.2125999927520752 * c_1.x + 0.71520000696182251 * c_1.y + 0.07220000028610229 * c_1.z;
}


#line 73 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
uint grHash_0(uint pixel_0, uint frame_2, uint stream_0)
{

    uint hash_0 = ((pixel_0 * 2654435769U) ^ (frame_2 * 2246822507U)) ^ (stream_0 * 3266489909U);

    uint hash_1 = (hash_0 ^ (hash_0 >> 15U)) * 625341585U;

)cy_msl",
    R"cy_msl(    return hash_1 ^ (hash_1 >> 13U);
}

float grUnit_0(uint hash_2)
{

#line 84
    return float(hash_2 & 16777215U) / 1.6777216e+07;
}


#line 199 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
uint spGuideCell_0(float z_7, float turn_1)
{

    return min(uint(z_7 * 4.0), 3U) * 4U + min(uint(turn_1 * 4.0), 3U);
}


#line 263 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
GrSample_0 grGatherSample_0(const GrSample_0 thread* ray_0, const GrSample_0 thread* standIn_0, uint excluded_0)
{

#line 263
    uint _S257 = standIn_0->source_0;

#line 263
    bool hasStandIn_0;
    if((standIn_0->source_0) != 0U)
    {

#line 264
        hasStandIn_0 = (standIn_0->confidence_0) > 0.0;

#line 264
    }
    else
    {

#line 264
        hasStandIn_0 = false;

#line 264
    }

#line 264
    if(hasStandIn_0)
    {

#line 264
        hasStandIn_0 = (excluded_0 & (1U << _S257)) == 0U;

#line 264
    }
    else
    {

#line 264
        hasStandIn_0 = false;

#line 264
    }

    thread GrSample_0 sample_3 = *ray_0;
    if((ray_0->source_0) == 0U)
    {

#line 268
        if(!hasStandIn_0)
        {

#line 269
            return *ray_0;
        }
        (&sample_3)->source_0 = _S257;
        (&sample_3)->radiance_0 = standIn_0->radiance_0;

#line 267
    }
    else
    {



        if(hasStandIn_0)
        {
            (&sample_3)->radiance_0 = standIn_0->radiance_0 + (ray_0->radiance_0 - standIn_0->radiance_0) * float3(gpClamp_0(ray_0->confidence_0, 0.0, 1.0)) ;

#line 273
        }

#line 267
    }

#line 277
    (&sample_3)->confidence_0 = 1.0;
    return sample_3;
}


#line 242 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
void spStoreCoefficients_0(uint slot_8, uint record_6, const array<float3, int(9)> thread* coefficients_3, float history_2, KernelContext_0 thread* kernelContext_45)
{

#line 242
    uint _S258 = spRecordBase_0(slot_8, record_6, kernelContext_45);
    uint _S259 = _S258 + 2U;
    thread array<float, int(28)> values_1;

#line 244
    uint k_5 = 0U;
    for(;;)
    {

#line 245
        if(k_5 < 9U)
        {
        }
        else
        {

#line 245
            break;
        }

#line 246
        uint _S260 = k_5 * 3U;

#line 246
        values_1[_S260] = (*coefficients_3)[k_5].x;
        values_1[_S260 + 1U] = (*coefficients_3)[k_5].y;
        values_1[_S260 + 2U] = (*coefficients_3)[k_5].z;

#line 245
        k_5 = k_5 + 1U;

#line 245
    }

#line 250
    values_1[int(27)] = history_2;

#line 250
    uint v_5 = 0U;
    for(;;)
    {

#line 251
        if(v_5 < 7U)
        {
        }
        else
        {

#line 251
            break;
        }

#line 252
        uint _S261 = v_5 * 4U;

#line 252
        *(kernelContext_45->giResolve_0->probes_1+(_S259 + v_5)) = float4(values_1[_S261], values_1[_S261 + 1U], values_1[_S261 + 2U], values_1[_S261 + 3U]);

#line 251
        v_5 = v_5 + 1U;

#line 251
    }



    return;
}


#line 168 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float grFarFieldWeight_0(float distance_0, KernelContext_0 thread* kernelContext_46)
{

#line 169
    float4 far_0 = (as_type<float4>((kernelContext_46->giResolve_0->constants_2[3U])));
    float begin_0 = far_0.x;
    if(begin_0 <= 0.0)
    {

#line 172
        return 1.0;
    }
    float _S262 = far_0.y;

#line 174
    float ratio_0;

#line 174
    if(_S262 > 0.0)
    {

#line 174
        ratio_0 = far_0.z / _S262;

#line 174
    }
    else
    {

#line 174
        ratio_0 = 8.0;

#line 174
    }
    float width_1 = begin_0 * gpClamp_0(ratio_0 / 100.0, 0.15000000596046448, 1.0);
    if(distance_0 <= begin_0)
    {

#line 177
        return 0.0;
    }
    float t_8 = gpClamp_0((distance_0 - begin_0) / width_1, 0.0, 1.0);
    return t_8 * t_8 * (3.0 - 2.0 * t_8);
}


#line 278 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giHemisphereDirection_0(float3 normal_10, uint index_2, uint count_2)
{
    float _S263 = float(index_2);
    float radius_3 = sqrt((_S263 + 0.5) / float(count_2));
    float angle_1 = 2.3999631404876709 * _S263;
    float x_8 = radius_3 * cos(angle_1);
    float y_10 = radius_3 * sin(angle_1);
    float z_8 = sqrt(max(0.0, 1.0 - x_8 * x_8 - y_10 * y_10));

#line 285
    float3 tangent_2;
    if((abs(normal_10.y)) < 0.99000000953674316)
    {

#line 286
        tangent_2 = cross(float3(0.0, 1.0, 0.0), normal_10);

#line 286
    }
    else
    {

#line 286
        tangent_2 = cross(float3(1.0, 0.0, 0.0), normal_10);

#line 286
    }

    float3 tangent_3 = giNormalisedOr_0(tangent_2, float3(1.0, 0.0, 0.0));

    return giNormalisedOr_0(tangent_3 * float3(x_8)  + cross(normal_10, tangent_3) * float3(y_10)  + normal_10 * float3(z_8) , normal_10);
}


#line 184 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float grSkyVisibilityRay_0(float3 position_11, float3 normal_11, uint index_3, uint count_3, KernelContext_0 thread* kernelContext_47)
{

#line 184
    bool _S264;
    if(count_3 == 0U)
    {

#line 185
        _S264 = true;

#line 185
    }
    else
    {

#line 185
        uint _S265 = giLevelCount_0(kernelContext_47);

#line 185
        _S264 = _S265 == 0U;

#line 185
    }

#line 185
    if(_S264)
    {

#line 186
        return 1.0;
    }
    float reach_2 = (as_type<float>((kernelContext_47->giResolve_0->constants_2[3U].w)));

#line 188
    float _S266 = giFinestVoxel_0(kernelContext_47);

#line 188
    GiTraceHit_0 _S267 = giSphereTrace_0(position_11 + normal_11 * float3((_S266 * 2.0)) , giHemisphereDirection_0(normal_11, index_3, count_3), reach_2, 0.0, kernelContext_47);

#line 193
    if(_S267.hit_1)
    {

#line 194
        return 0.0;
    }
    return gpClamp_0(_S267.closest_0 / (_S266 * 4.0), 0.0, 1.0);
}


#line 491 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
void spTrace_0(uint record_7, uint lane_0, KernelContext_0 thread* kernelContext_48)
{

#line 491
    uint _S268 = spSlot_0(kernelContext_48);

#line 491
    bool allowed_0;


    if(record_7 == 0U)
    {

#line 494
        allowed_0 = lane_0 == 0U;

#line 494
    }
    else
    {

#line 494
        allowed_0 = false;

#line 494
    }

#line 494
    if(allowed_0)
    {

#line 495
        *(kernelContext_48->giResolve_0->slots_0+spCounter_0(1U - _S268)) = 0U;

#line 494
    }

#line 494
    uint _S269 = spUniformCount_0(kernelContext_48);



    if(record_7 >= _S269)
    {

#line 499
        uint _S270 = record_7 - _S269;

#line 499
        uint _S271 = *(kernelContext_48->giResolve_0->slots_0+spCounter_0(_S268));

#line 499
        uint _S272 = spCapacity_0(kernelContext_48);

#line 499
        allowed_0 = _S270 >= (min(_S271, _S272));

#line 498
    }
    else
    {

#line 498
        allowed_0 = false;

#line 498
    }

#line 498
    if(allowed_0)
    {
        return;
    }

#line 500
    SpProbe_0 _S273 = spLoadProbe_0(_S268, record_7, kernelContext_48);


    if((_S273.depth_0) <= 0.0)
    {

#line 504
        return;
    }
    uint4 shape_1 = kernelContext_48->giResolve_0->constants_2[42U];
    float4 weights_0 = (as_type<float4>((kernelContext_48->giResolve_0->constants_2[43U])));
    uint frame_3 = kernelContext_48->giResolve_0->constants_2[18U].x;
    uint cacheFrame_2 = kernelContext_48->giResolve_0->constants_2[4U].w;

#line 509
    uint cell_4;

    if(lane_0 == 0U)
    {

#line 511
        float3 _S274 = _S273.normal_3;
        thread float4 diffuse_1;
        thread float4 specular_1;

#line 513
        grSampleTraced_0(_S273.position_4, _S274, _S274, cacheFrame_2, &diffuse_1, &specular_1, kernelContext_48);

        *kernelContext_48->spStandIn_0 = diffuse_1;
        float _S275 = (as_type<float>((kernelContext_48->giResolve_0->constants_2[2U].y)));

#line 516
        thread SpProbe_0 _S276 = _S273;

#line 516
        float3 _S277 = spProbeOrigin_0(&_S276, _S275, kernelContext_48);

#line 516
        *kernelContext_48->spOrigin_0 = _S277;
        thread array<float3, int(9)> previous_1;
        thread float history_3 = 0.0;
        if(((shape_1.w) & 2U) != 0U)
        {

#line 519
            allowed_0 = (kernelContext_48->giResolve_0->constants_2[5U].w) != 0U;

#line 519
        }
        else
        {

#line 519
            allowed_0 = false;

#line 519
        }

#line 519
        bool found_0;
        if(allowed_0)
        {

#line 520
            float _S278 = weights_0.y;

#line 520
            thread SpProbe_0 _S279 = _S273;

#line 520
            bool _S280 = spReproject_0(&_S279, _S278, &previous_1, &history_3, kernelContext_48);

#line 520
            found_0 = _S280;

#line 520
        }
        else
        {

#line 520
            found_0 = false;

#line 520
        }

#line 520
        cell_4 = 0U;
        for(;;)
        {

#line 521
            if(cell_4 < 9U)
            {
            }
            else
            {

#line 521
                break;
            }

#line 522
            (*kernelContext_48->spHistory_0)[cell_4] = previous_1[cell_4];

#line 521
            cell_4 = cell_4 + 1U;

#line 521
        }


        *kernelContext_48->spHistoryCount_0 = history_3;
        if(found_0)
        {

#line 525
            cell_4 = 1U;

#line 525
        }
        else
        {

#line 525
            cell_4 = 0U;

#line 525
        }

#line 525
        *kernelContext_48->spReprojected_0 = cell_4;

#line 511
    }

#line 527
    threadgroup_barrier(mem_flags::mem_threadgroup);

#line 527
    float3 value_4;

    if(lane_0 < 16U)
    {

#line 530
        float z_9 = (float(lane_0 / 4U) + 0.5) / 4.0;

        float3 direction_8 = spHemisphereDirection_0(_S273.normal_3, z_9, (float(lane_0 % 4U) + 0.5) / 4.0);

        if((*kernelContext_48->spReprojected_0) != 0U)
        {

#line 535
            thread array<float3, int(9)> previous_2;

#line 535
            cell_4 = 0U;
            for(;;)
            {

#line 536
                if(cell_4 < 9U)
                {
                }
                else
                {

#line 536
                    break;
                }

#line 537
                previous_2[cell_4] = (*kernelContext_48->spHistory_0)[cell_4];

#line 536
                cell_4 = cell_4 + 1U;

#line 536
            }


            float3 _S281 = float3(1.0, 1.0, 1.0);

#line 539
            thread array<float3, int(9)> _S282 = previous_2;

#line 539
            float3 _S283 = spEvaluate_0(&_S282, direction_8, _S281);

#line 539
            value_4 = _S283;

#line 534
        }
        else
        {

#line 534
            float4 _S284 = gpSample_0(_S273.position_4, direction_8, cacheFrame_2, kernelContext_48);

#line 534
            value_4 = _S284.xyz;

#line 534
        }

#line 543
        (*kernelContext_48->spWeights_0)[lane_0] = max(spLuminance_0(value_4), 0.0) * z_9;

#line 529
    }

#line 545
    threadgroup_barrier(mem_flags::mem_threadgroup);

#line 545
    cell_4 = 0U;

#line 545
    float guideTotal_0 = 0.0;


    for(;;)
    {

#line 548
        if(cell_4 < 16U)
        {
        }
        else
        {

#line 548
            break;
        }

#line 549
        float guideTotal_1 = guideTotal_0 + (*kernelContext_48->spWeights_0)[cell_4];

#line 548
        cell_4 = cell_4 + 1U;

#line 548
        guideTotal_0 = guideTotal_1;

#line 548
    }

#line 548
    float guided_0;


    if(guideTotal_0 > 0.0)
    {

#line 551
        guided_0 = gpClamp_0(weights_0.z, 0.0, 0.89999997615814209);

#line 551
    }
    else
    {

#line 551
        guided_0 = 0.0;

#line 551
    }
    float uniformShare_0 = (1.0 - guided_0) * 0.5;

    uint _S285 = min(max(shape_1.x, 1U), 64U);

#line 554
    uint k_6;

#line 554
    float history_4;

#line 554
    float openness_0;

    if(lane_0 < _S285)
    {

#line 557
        uint hash_3 = grHash_0(_S273.id_0 * 64U + lane_0, frame_3, 8U);
        float u0_0 = grUnit_0(hash_3);
        float u1_0 = grUnit_0(hash_3 * 656542357U);
        float u2_0 = grUnit_0(hash_3 * 2654435769U);


        if(u0_0 >= uniformShare_0)
        {

#line 563
            allowed_0 = u0_0 < (uniformShare_0 + uniformShare_0);

#line 563
        }
        else
        {

#line 563
            allowed_0 = false;

#line 563
        }

#line 563
        if(allowed_0)
        {

#line 563
            history_4 = sqrt(u1_0);

#line 563
            openness_0 = u2_0;

#line 563
        }
        else
        {

#line 565
            if(u0_0 >= (uniformShare_0 + uniformShare_0))
            {

#line 566
                float _S286 = (u0_0 - uniformShare_0 - uniformShare_0) / guided_0 * guideTotal_0;

#line 566
                cell_4 = 0U;

#line 566
                history_4 = 0.0;


                for(;;)
                {

#line 569
                    if(cell_4 < 16U)
                    {
                    }
                    else
                    {

#line 569
                        k_6 = 15U;

#line 569
                        break;
                    }

#line 570
                    float running_0 = history_4 + (*kernelContext_48->spWeights_0)[cell_4];
                    if(((*kernelContext_48->spWeights_0)[cell_4]) > 0.0)
                    {

#line 571
                        allowed_0 = _S286 < running_0;

#line 571
                    }
                    else
                    {

#line 571
                        allowed_0 = false;

#line 571
                    }

#line 571
                    if(allowed_0)
                    {

#line 571
                        k_6 = cell_4;

                        break;
                    }

#line 569
                    cell_4 = cell_4 + 1U;

#line 569
                    history_4 = running_0;

#line 569
                }

#line 577
                float _S287 = (float(k_6 % 4U) + u2_0) / 4.0;

#line 577
                history_4 = (float(k_6 / 4U) + u1_0) / 4.0;

#line 577
                openness_0 = _S287;

#line 565
            }
            else
            {

#line 565
                history_4 = u1_0;

#line 565
                openness_0 = u2_0;

#line 565
            }

#line 563
        }

#line 579
        float density_0 = uniformShare_0 / 6.28318548202514648 + uniformShare_0 * history_4 / 3.14159274101257324;

#line 579
        float density_1;

        if(guided_0 > 0.0)
        {

#line 581
            density_1 = density_0 + guided_0 * ((*kernelContext_48->spWeights_0)[spGuideCell_0(history_4, openness_0)] / guideTotal_0) * 2.5464789867401123;

#line 581
        }
        else
        {

#line 581
            density_1 = density_0;

#line 581
        }



        float3 _S288 = float3(0.0, 0.0, 0.0);
        float3 direction_9 = spHemisphereDirection_0(_S273.normal_3, history_4, openness_0);
        bool _S289 = density_1 > 0.0;

#line 587
        if(_S289)
        {
            uint excluded_1 = kernelContext_48->giResolve_0->constants_2[2U].w;

            float4 standIn_1 = *kernelContext_48->spStandIn_0;

#line 591
            GrSample_0 _S290 = spProbeRay_0(*kernelContext_48->spOrigin_0, giNormalisedOr_0(direction_9, float3(0.0, 0.0, -1.0)), (as_type<float4>((kernelContext_48->giResolve_0->constants_2[2U]))).x, kernelContext_48);


            GrSample_0 _S291 = grSample_0(standIn_1.xyz, standIn_1.w, 3U);

#line 594
            thread GrSample_0 _S292 = _S290;

#line 594
            thread GrSample_0 _S293 = _S291;

#line 594
            GrSample_0 _S294 = grGatherSample_0(&_S292, &_S293, excluded_1);
            if((_S294.source_0) != 0U)
            {

#line 595
                value_4 = _S294.radiance_0;

#line 595
                k_6 = 1U;

#line 595
            }
            else
            {

#line 595
                value_4 = _S288;

#line 595
                k_6 = 0U;

#line 595
            }

#line 587
        }
        else
        {

#line 587
            value_4 = _S288;

#line 587
            k_6 = 0U;

#line 587
        }

#line 600
        (*kernelContext_48->spRayValue_0)[lane_0] = value_4;
        (*kernelContext_48->spRayDirection_0)[lane_0] = direction_9;
        if(_S289)
        {

#line 602
            guideTotal_0 = 1.0 / (density_1 * float(_S285));

#line 602
        }
)cy_msl",
    R"cy_msl(        else
        {

#line 602
            guideTotal_0 = 0.0;

#line 602
        }

#line 602
        (*kernelContext_48->spRayWeight_0)[lane_0] = guideTotal_0;
        (*kernelContext_48->spRayValid_0)[lane_0] = k_6;

#line 556
    }

#line 605
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if(lane_0 != 0U)
    {

#line 607
        return;
    }

    thread array<float3, int(9)> sum_2;

#line 610
    k_6 = 0U;
    for(;;)
    {

#line 611
        if(k_6 < 9U)
        {
        }
        else
        {

#line 611
            break;
        }

#line 612
        sum_2[k_6] = float3(0.0, 0.0, 0.0);

#line 611
        k_6 = k_6 + 1U;

#line 611
    }

#line 611
    uint ray_1 = 0U;


    for(;;)
    {

#line 614
        if(ray_1 < _S285)
        {
        }
        else
        {

#line 614
            break;
        }

#line 615
        if(((*kernelContext_48->spRayValid_0)[ray_1]) == 0U)
        {

#line 616
            ray_1 = ray_1 + 1U;

#line 614
            continue;
        }


        thread array<float, int(9)> basis_2;
        spBasis_0((*kernelContext_48->spRayDirection_0)[ray_1], &basis_2);
        float _S295 = (*kernelContext_48->spRayWeight_0)[ray_1];

#line 620
        k_6 = 0U;
        for(;;)
        {

#line 621
            if(k_6 < 9U)
            {
            }
            else
            {

#line 621
                break;
            }

#line 622
            sum_2[k_6] = sum_2[k_6] + (*kernelContext_48->spRayValue_0)[ray_1] * float3((basis_2[k_6] * _S295)) ;

#line 621
            k_6 = k_6 + 1U;

#line 621
        }

#line 614
        ray_1 = ray_1 + 1U;

#line 614
    }

#line 626
    float frames_0 = float(max(shape_1.y, 1U));
    thread array<float3, int(9)> blended_0;

    if((*kernelContext_48->spReprojected_0) != 0U)
    {

#line 630
        float _S296 = max(1.0 / (*kernelContext_48->spHistoryCount_0 + 1.0), 1.0 / frames_0);

#line 630
        k_6 = 0U;
        for(;;)
        {

#line 631
            if(k_6 < 9U)
            {
            }
            else
            {

#line 631
                break;
            }

#line 632
            blended_0[k_6] = (*kernelContext_48->spHistory_0)[k_6] + (sum_2[k_6] - (*kernelContext_48->spHistory_0)[k_6]) * float3(_S296) ;

#line 631
            k_6 = k_6 + 1U;

#line 631
        }

#line 631
        history_4 = min(*kernelContext_48->spHistoryCount_0 + 1.0, frames_0);

#line 629
    }
    else
    {

#line 629
        k_6 = 0U;

#line 636
        for(;;)
        {

#line 636
            if(k_6 < 9U)
            {
            }
            else
            {

#line 636
                break;
            }

#line 637
            blended_0[k_6] = sum_2[k_6];

#line 636
            k_6 = k_6 + 1U;

#line 636
        }

#line 636
        history_4 = 1.0;

#line 629
    }

#line 629
    thread array<float3, int(9)> _S297 = blended_0;

#line 629
    spStoreCoefficients_0(_S268, record_7, &_S297, history_4, kernelContext_48);

#line 629
    float _S298 = grFarFieldWeight_0(length(_S273.position_4 - (as_type<float3>((kernelContext_48->giResolve_0->constants_2[1U].xyz)))), kernelContext_48);

#line 645
    uint skyRays_0 = kernelContext_48->giResolve_0->constants_2[2U].z;
    if(_S298 > 0.0)
    {

#line 646
        allowed_0 = skyRays_0 > 0U;

#line 646
    }
    else
    {

#line 646
        allowed_0 = false;

#line 646
    }

#line 646
    if(allowed_0)
    {

#line 647
        uint _S299 = (frame_3 + _S273.id_0) % skyRays_0;

#line 647
        float _S300 = grSkyVisibilityRay_0(_S273.position_4, _S273.normal_3, _S299, skyRays_0, kernelContext_48);

#line 647
        openness_0 = _S300;

#line 646
    }
    else
    {

#line 646
        openness_0 = 1.0;

#line 646
    }

#line 646
    uint _S301 = spRecordBase_0(_S268, record_7, kernelContext_48);

#line 651
    *(kernelContext_48->giResolve_0->probes_1+(_S301 + 9U)) = *kernelContext_48->spStandIn_0;
    *(kernelContext_48->giResolve_0->probes_1+(_S301 + 10U)) = float4(openness_0, 0.0, 0.0, 0.0);
    return;
}


#line 26 "src/rendering/gi_gpu/shaders/gi_screen_probes.slang"
[[kernel]] void cyGiProbeTrace(uint3 group_0 [[threadgroup_position_in_grid]], uint3 lane_1 [[thread_position_in_threadgroup]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiResolveSet_default_0 constant* giResolve_1 [[buffer(3)]])
{

#line 26
    thread KernelContext_0 kernelContext_49;

#line 26
    (&kernelContext_49)->giProbes_0 = giProbes_1;

#line 26
    (&kernelContext_49)->giScene_0 = giScene_1;

#line 26
    (&kernelContext_49)->giResolve_0 = giResolve_1;

#line 26
    threadgroup float4 spStandIn_1;

#line 26
    (&kernelContext_49)->spStandIn_0 = &spStandIn_1;

#line 26
    threadgroup float3 spOrigin_1;

#line 26
    (&kernelContext_49)->spOrigin_0 = &spOrigin_1;

#line 26
    threadgroup array<float3, int(9)> spHistory_1;

#line 26
    (&kernelContext_49)->spHistory_0 = &spHistory_1;

#line 26
    threadgroup float spHistoryCount_1;

#line 26
    (&kernelContext_49)->spHistoryCount_0 = &spHistoryCount_1;

#line 26
    threadgroup uint spReprojected_1;

#line 26
    (&kernelContext_49)->spReprojected_0 = &spReprojected_1;

#line 26
    threadgroup array<float, int(16)> spWeights_1;

#line 26
    (&kernelContext_49)->spWeights_0 = &spWeights_1;

#line 26
    threadgroup array<float3, int(64)> spRayValue_1;

#line 26
    (&kernelContext_49)->spRayValue_0 = &spRayValue_1;

#line 26
    threadgroup array<float3, int(64)> spRayDirection_1;

#line 26
    (&kernelContext_49)->spRayDirection_0 = &spRayDirection_1;

#line 26
    threadgroup array<float, int(64)> spRayWeight_1;

#line 26
    (&kernelContext_49)->spRayWeight_0 = &spRayWeight_1;

#line 26
    threadgroup array<uint, int(64)> spRayValid_1;

#line 26
    (&kernelContext_49)->spRayValid_0 = &spRayValid_1;

#line 26
    spTrace_0(group_0.x, lane_1.x, &kernelContext_49);

    return;
}

)cy_msl");
inline constexpr const auto& kGiProbeTraceMsl = kGiProbeTraceMslText.text;

/// cyGiProbeIntegrate.metal, 53413 bytes.
inline constexpr char kGiProbeIntegrateMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 118 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
struct GrSample_0
{
    float3 radiance_0;
    float confidence_0;
    uint source_0;
};


#line 53
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


#line 78 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 66 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
struct GiResolveSet_default_0
{
    uint4 device* constants_2;
    float4 device* surfaces_0;
    float4 device* noisy_0;
    float4 device* probes_1;
    uint device* slots_0;
};


#line 66
struct KernelContext_0
{
    GiProbeSet_default_0 constant* giProbes_0;
    GiSceneSet_default_0 constant* giScene_0;
    GiResolveSet_default_0 constant* giResolve_0;
};


#line 325 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
uint GiSoftwareTier_source_0()
{

#line 326
    return 6U;
}


#line 123 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_0)
{

#line 124
    return (as_type<float>((kernelContext_0->giScene_0->constants_1[9U].y)));
}


#line 281 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiWorldHit_0
{
    bool hit_0;
    float t_0;
    float3 position_0;
    float3 normal_0;
    float confidence_1;
};


#line 215 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
struct GiTraceHit_0
{
    bool hit_1;
    bool exhausted_0;
    float t_1;
    float closest_0;
    float3 position_1;
    float3 normal_1;
};


#line 115
uint giLevelCount_0(KernelContext_0 thread* kernelContext_1)
{

#line 116
    return kernelContext_1->giScene_0->constants_1[0U].x;
}

uint giWindowBricks_0(KernelContext_0 thread* kernelContext_2)
{

#line 120
    return kernelContext_2->giScene_0->constants_1[0U].y;
}


#line 167
int giWrap_0(int value_0, int modulus_0)
{

#line 168
    uint _S1 = uint(modulus_0);
    uint _S2 = (uint(value_0) + (_S1 << 20U)) % _S1;

#line 169
    return int(_S2);
}


#line 95
float giLerp_0(float a_0, float b_0, float t_2)
{

#line 96
    return a_0 + (b_0 - a_0) * t_2;
}


#line 128
float giTrilinear_0(uint slot_0, float fx_0, float fy_0, float fz_0, KernelContext_0 thread* kernelContext_3)
{
    int x0_0 = clamp(int(floor(fx_0)), int(0), int(3));
    int y0_0 = clamp(int(floor(fy_0)), int(0), int(3));
    int z0_0 = clamp(int(floor(fz_0)), int(0), int(3));



    float tx_0 = clamp(fx_0 - float(x0_0), 0.0, 1.0);
    float ty_0 = clamp(fy_0 - float(y0_0), 0.0, 1.0);

    uint base_0 = slot_0 * 125U;

    uint _S3 = uint(z0_0) * 5U;

#line 141
    uint _S4 = uint(y0_0);

#line 141
    uint _S5 = base_0 + (_S3 + _S4) * 5U;

#line 141
    uint _S6 = uint(x0_0);
    uint _S7 = uint(x0_0 + int(1));
    uint _S8 = uint(y0_0 + int(1));

#line 143
    uint _S9 = base_0 + (_S3 + _S8) * 5U;

    uint _S10 = uint(z0_0 + int(1)) * 5U;

#line 145
    uint _S11 = base_0 + (_S10 + _S4) * 5U;

    uint _S12 = base_0 + (_S10 + _S8) * 5U;

#line 153
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S5 + _S6], kernelContext_3->giScene_0->bricks_0[_S5 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S9 + _S6], kernelContext_3->giScene_0->bricks_0[_S9 + _S7], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_3->giScene_0->bricks_0[_S11 + _S6], kernelContext_3->giScene_0->bricks_0[_S11 + _S7], tx_0), giLerp_0(kernelContext_3->giScene_0->bricks_0[_S12 + _S6], kernelContext_3->giScene_0->bricks_0[_S12 + _S7], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 175
float giSampleLevel_0(uint level_0, float3 p_0, float thread* farDistance_0, KernelContext_0 thread* kernelContext_4)
{

#line 176
    uint _S13 = 16U + level_0 * 2U;

#line 176
    float4 shape_0 = (as_type<float4>((kernelContext_4->giScene_0->constants_1[_S13])));
    int4 origin_0 = (as_type<int4>((kernelContext_4->giScene_0->constants_1[_S13 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_0.x / brick_0));
    int by_0 = int(floor(p_0.y / brick_0));
    int bz_0 = int(floor(p_0.z / brick_0));

#line 183
    uint _S14 = giWindowBricks_0(kernelContext_4);
    int dim_0 = int(_S14);
    int _S15 = origin_0.x;

#line 185
    bool _S16;

#line 185
    if(bx_0 < _S15)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = by_0 < (origin_0.y);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = bz_0 < (origin_0.z);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = bx_0 >= (_S15 + dim_0);

#line 185
    }

#line 185
    if(_S16)
    {

#line 185
        _S16 = true;

#line 185
    }
    else
    {

#line 185
        _S16 = by_0 >= (origin_0.y + dim_0);

#line 185
    }
    if(_S16)
    {

#line 186
        _S16 = true;

#line 186
    }
    else
    {

)cy_msl"
    R"cy_msl(#line 186
        _S16 = bz_0 >= (origin_0.z + dim_0);

#line 186
    }

#line 185
    if(_S16)
    {
        return *farDistance_0;
    }
    int _S17 = giWrap_0(bz_0, dim_0);

#line 189
    uint _S18 = uint(dim_0);

#line 189
    uint _S19 = uint(_S17) * _S18;

#line 189
    int _S20 = giWrap_0(by_0, dim_0);

#line 189
    uint _S21 = (_S19 + uint(_S20)) * _S18;
    int _S22 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_4->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S21 + uint(_S22))];
    if(slot_1 == 4294967295U)
    {

#line 193
        return *farDistance_0;
    }

    float3 local_0 = (p_0 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 196
    float _S23 = giTrilinear_0(slot_1, local_0.x, local_0.y, local_0.z, kernelContext_4);
    return _S23;
}


float giDistance_0(float3 p_1, KernelContext_0 thread* kernelContext_5)
{

#line 201
    uint _S24 = giLevelCount_0(kernelContext_5);

#line 201
    float answer_0 = 1.0e+06;

#line 201
    uint level_1 = 0U;


    for(;;)
    {

#line 204
        if(level_1 < _S24)
        {
        }
        else
        {

#line 204
            break;
        }

#line 205
        thread float farDistance_1 = 0.0;

#line 205
        float _S25 = giSampleLevel_0(level_1, p_1, &farDistance_1, kernelContext_5);

        float _S26 = min(answer_0, _S25);
        if(_S25 < farDistance_1)
        {

#line 209
            return _S25;
        }

#line 204
        uint level_2 = level_1 + 1U;

#line 204
        answer_0 = _S26;

#line 204
        level_1 = level_2;

#line 204
    }

#line 212
    return answer_0;
}


#line 99
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 100
    float squared_0 = dot(v_0, v_0);

#line 100
    float3 _S27;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 101
        _S27 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 101
    }
    else
    {

#line 101
        _S27 = fallback_0;

#line 101
    }

#line 101
    return _S27;
}


#line 225
GiTraceHit_0 giSphereTrace_0(float3 origin_1, float3 direction_0, float maxDistance_0, float tMin_0, KernelContext_0 thread* kernelContext_6)
{

#line 226
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_1 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_1 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_1 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_1 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S28 = giLevelCount_0(kernelContext_6);
    if(_S28 == 0U)
    {

#line 234
        return result_0;
    }

#line 234
    float _S29 = giFinestVoxel_0(kernelContext_6);


    float surface_0 = _S29 * 0.5;
    float _S30 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 239
    recent_0[int(0)] = 1.0e+06;

#line 239
    recent_0[int(1)] = 1.0e+06;

#line 239
    recent_0[int(2)] = 1.0e+06;

#line 239
    float closest_1 = 1.0e+06;

#line 239
    uint recentSlot_0 = 0U;

#line 239
    uint step_0 = 0U;

#line 239
    float t_3 = _S30;


    for(;;)
    {

#line 242
        if(step_0 < 192U)
        {
        }
        else
        {

#line 242
            break;
        }

#line 243
        if(t_3 > maxDistance_0)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_2 = origin_1 + direction_0 * float3(t_3) ;

#line 248
        float _S31 = giDistance_0(position_2, kernelContext_6);

        if(_S31 < surface_0)
        {

#line 251
            (&result_0)->hit_1 = true;
            (&result_0)->t_1 = t_3;
            (&result_0)->position_1 = position_2;


            float3 _S32 = float3(_S29, 0.0, 0.0);

#line 256
            float _S33 = giDistance_0(position_2 + _S32, kernelContext_6);

#line 256
            float _S34 = giDistance_0(position_2 - _S32, kernelContext_6);

#line 256
            float _S35 = _S33 - _S34;

            float3 _S36 = float3(0.0, _S29, 0.0);

#line 258
            float _S37 = giDistance_0(position_2 + _S36, kernelContext_6);

#line 258
            float _S38 = giDistance_0(position_2 - _S36, kernelContext_6);

#line 258
            float _S39 = _S37 - _S38;

            float3 _S40 = float3(0.0, 0.0, _S29);

#line 260
            float _S41 = giDistance_0(position_2 + _S40, kernelContext_6);

#line 260
            float _S42 = giDistance_0(position_2 - _S40, kernelContext_6);

            (&result_0)->normal_1 = giNormalisedOr_0(float3(_S35, _S39, _S41 - _S42), - direction_0);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S43 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S31;
        uint _S44 = (recentSlot_0 + 1U) % 3U;
        float t_4 = t_3 + max(_S31, surface_0);

#line 242
        uint step_1 = step_0 + 1U;

#line 242
        closest_1 = _S43;

#line 242
        recentSlot_0 = _S44;

#line 242
        step_0 = step_1;

#line 242
        t_3 = t_4;

#line 242
    }

#line 271
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

)cy_msl"
    R"cy_msl(#line 107
        _S45 = lo_0;

#line 107
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


#line 124 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
GrSample_0 grSample_0(float3 radiance_1, float confidence_2, uint source_1)
{

#line 125
    thread GrSample_0 sample_0;
    (&sample_0)->radiance_0 = radiance_1;
    (&sample_0)->confidence_0 = confidence_2;
    (&sample_0)->source_0 = source_1;
    return sample_0;
}


#line 689 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiTierSample_0
{
    float3 radiance_2;
    float confidence_3;
    float hitDistance_0;
    uint source_2;
};


#line 696
GiTierSample_0 gpTierSample_0(uint source_3)
{

#line 697
    thread GiTierSample_0 sample_1;
    (&sample_1)->radiance_2 = float3(0.0, 0.0, 0.0);
    (&sample_1)->confidence_3 = 0.0;
    (&sample_1)->hitDistance_0 = -1.0;
    (&sample_1)->source_2 = source_3;
    return sample_1;
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


#line 471 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_2, float cell_0)
{

#line 472
    return int(floor(value_2 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_3, float3 normal_2, KernelContext_0 thread* kernelContext_9)
{

#line 482
    uint4 grid_0 = kernelContext_9->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 485
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S48 = radius_0 * radius_0;
    float _S49 = position_3.x;

#line 490
    int _S50 = giCell_0(_S49 - radius_0, cell_1);

#line 490
    float _S51 = position_3.y;

#line 490
    int _S52 = giCell_0(_S51 - radius_0, cell_1);
    float _S53 = position_3.z;

#line 491
    int _S54 = giCell_0(_S53 - radius_0, cell_1);
    int _S55 = giCell_0(_S49 + radius_0, cell_1);

#line 492
    int _S56 = giCell_0(_S51 + radius_0, cell_1);
    int _S57 = giCell_0(_S53 + radius_0, cell_1);

#line 493
    float bestScore_0 = 1.0e+09;

#line 493
    float bestAlignment_0 = 0.0;

#line 493
    uint best_0 = 4294967295U;

#line 493
    int z_1 = _S54;



    for(;;)
    {

#line 497
        if(z_1 <= _S57)
        {
        }
        else
        {

#line 497
            break;
        }

#line 497
        int y_1 = _S52;
        for(;;)
        {

#line 498
            if(y_1 <= _S56)
            {
            }
            else
            {

#line 498
                break;
            }

#line 498
            float bestScore_1 = bestScore_0;

#line 498
            float bestAlignment_1 = bestAlignment_0;

#line 498
            uint best_1 = best_0;

#line 498
            int x_1 = _S50;
            for(;;)
            {

#line 499
                if(x_1 <= _S55)
                {
                }
                else
                {

#line 499
                    break;
                }
                uint _S58 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 501
                uint first_0 = kernelContext_9->giScene_0->gridRanges_0[_S58];
                uint _S59 = kernelContext_9->giScene_0->gridRanges_0[_S58 + 1U];

#line 502
                float bestScore_2 = bestScore_1;

#line 502
                float bestAlignment_2 = bestAlignment_1;

#line 502
                uint best_2 = best_1;

#line 502
                uint item_0 = first_0;
                for(;;)
                {

#line 503
                    if(item_0 < (first_0 + _S59))
                    {
                    }
                    else
                    {

#line 503
                        break;
                    }

#line 504
                    uint handle_0 = kernelContext_9->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_9->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 506
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }



                    uint _S60 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_9->giScene_0->cards_0[_S60 + 1U].xyz, normal_2);
                    if(alignment_0 <= 0.25)
                    {

#line 511
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 513
                    float3 offset_0 = kernelContext_9->giScene_0->cards_0[_S60].xyz - position_3;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S48)
                    {

#line 516
                        item_0 = item_0 + 1U;

)cy_msl"
    R"cy_msl(#line 503
                        continue;
                    }

#line 519
                    float score_0 = squared_1 / alignment_0;

#line 519
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 520
                        better_0 = score_0 < bestScore_2;

#line 520
                    }
                    else
                    {

#line 521
                        if(alignment_0 != bestAlignment_2)
                        {

#line 521
                            better_0 = alignment_0 > bestAlignment_2;

#line 521
                        }
                        else
                        {

#line 521
                            better_0 = handle_0 < best_2;

#line 521
                        }

#line 520
                    }

#line 520
                    float bestScore_3;

#line 520
                    float bestAlignment_3;

#line 520
                    uint best_3;


                    if(better_0)
                    {

#line 523
                        bestScore_3 = score_0;

#line 523
                        bestAlignment_3 = alignment_0;

#line 523
                        best_3 = handle_0;

#line 523
                    }
                    else
                    {

#line 523
                        bestScore_3 = bestScore_2;

#line 523
                        bestAlignment_3 = bestAlignment_2;

#line 523
                        best_3 = best_2;

#line 523
                    }

#line 523
                    bestScore_2 = bestScore_3;

#line 523
                    bestAlignment_2 = bestAlignment_3;

#line 523
                    best_2 = best_3;

#line 503
                    item_0 = item_0 + 1U;

#line 503
                }

#line 499
                int x_2 = x_1 + int(1);

#line 499
                bestScore_1 = bestScore_2;

#line 499
                bestAlignment_1 = bestAlignment_2;

#line 499
                best_1 = best_2;

#line 499
                x_1 = x_2;

#line 499
            }

#line 498
            int y_2 = y_1 + int(1);

#line 498
            bestScore_0 = bestScore_1;

#line 498
            bestAlignment_0 = bestAlignment_1;

#line 498
            best_0 = best_1;

#line 498
            y_1 = y_2;

#line 498
        }

#line 497
        z_1 = z_1 + int(1);

#line 497
    }

#line 532
    return best_0;
}


#line 776 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpWorldRadiance_0(const GiWorldHit_0 thread* hit_3, float3 direction_3, uint source_4, uint cacheFrame_0, KernelContext_0 thread* kernelContext_10)
{

#line 777
    thread GiTierSample_0 sample_2 = gpTierSample_0(source_4);
    if(!hit_3->hit_0)
    {

#line 779
        (&sample_2)->source_2 = 9U;

#line 779
        float3 _S61 = gpSky_0(direction_3, kernelContext_10);
        (&sample_2)->radiance_2 = _S61;
        (&sample_2)->confidence_3 = 1.0;
        (&sample_2)->hitDistance_0 = -1.0;
        return sample_2;
    }
    (&sample_2)->hitDistance_0 = hit_3->t_0;

#line 785
    uint _S62 = giFindCard_0(hit_3->position_0, hit_3->normal_0, kernelContext_10);

    if(_S62 != 4294967295U)
    {

#line 788
        uint _S63 = _S62 * 2U;

#line 788
        (&sample_2)->radiance_2 = (*(kernelContext_10->giScene_0->cardState_0+_S63)).xyz;
        uint last_0 = (as_type<uint>(((*(kernelContext_10->giScene_0->cardState_0+(_S63 + 1U))).w)));

#line 789
        float age_0;
        if(cacheFrame_0 > last_0)
        {

#line 790
            age_0 = float(cacheFrame_0 - last_0);

#line 790
        }
        else
        {

#line 790
            age_0 = 0.0;

#line 790
        }


        (&sample_2)->confidence_3 = hit_3->confidence_1 * (1.0 - gpClamp_0(age_0 / 240.0, 0.0, 1.0) * 0.60000002384185791);
        return sample_2;
    }
    (&sample_2)->confidence_3 = hit_3->confidence_1 * 0.20000000298023224;
    return sample_2;
}


#line 247 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
GrSample_0 grWorldRay_0(float3 position_4, float3 normal_3, float3 unit_0, float lift_0, float reach_0, KernelContext_0 thread* kernelContext_11)
{
    uint cacheFrame_1 = kernelContext_11->giProbes_0->constants_0[24U].w;
    uint _S64 = GiSoftwareTier_source_0();

#line 250
    float height_0;

#line 250
    if(_S64 == 7U)
    {

#line 250
        height_0 = lift_0;

#line 250
    }
    else
    {

#line 250
        float _S65 = giFinestVoxel_0(kernelContext_11);

#line 250
        height_0 = max(lift_0, _S65);

#line 250
    }

#line 250
    GiWorldHit_0 _S66 = GiSoftwareTier_trace_0(position_4 + normal_3 * float3(height_0) , unit_0, reach_0, kernelContext_11);

#line 250
    bool _S67;

    if(_S66.hit_0)
    {

#line 252
        _S67 = (dot(_S66.normal_0, unit_0)) > 0.0;

#line 252
    }
    else
    {

#line 252
        _S67 = false;

#line 252
    }

#line 252
    if(_S67)
    {

#line 253
        return grSample_0(float3(0.0, 0.0, 0.0), 0.0, 0U);
    }

#line 253
    thread GiWorldHit_0 _S68 = _S66;

#line 253
    GiTierSample_0 _S69 = gpWorldRadiance_0(&_S68, unit_0, _S64, cacheFrame_1, kernelContext_11);


    return grSample_0(_S69.radiance_2, _S69.confidence_3, _S69.source_2);
}


#line 49 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
uint spWidth_0(KernelContext_0 thread* kernelContext_12)
{

#line 49
    return kernelContext_12->giResolve_0->constants_2[40U].x;
}


#line 139
struct SpTaps_0
{
    array<uint, int(4)> index_0;
    array<float, int(4)> weight_0;
    uint count_0;
};


#line 51
uint spSpacing_0(KernelContext_0 thread* kernelContext_13)
{

#line 51
    return kernelContext_13->giResolve_0->constants_2[40U].z;
}

)cy_msl"
    R"cy_msl(
#line 53
uint spTilesX_0(KernelContext_0 thread* kernelContext_14)
{

#line 53
    return kernelContext_14->giResolve_0->constants_2[41U].x;
}


#line 54
uint spTilesY_0(KernelContext_0 thread* kernelContext_15)
{

#line 54
    return kernelContext_15->giResolve_0->constants_2[41U].y;
}


#line 145
void spAxisTaps_0(uint coordinate_0, uint step_2, uint count_1, uint thread* base_1, float thread* fraction_0)
{

#line 146
    float g_0 = (float(coordinate_0) - float(step_2 / 2U)) / float(step_2);
    if(g_0 <= 0.0)
    {

#line 148
        *base_1 = 0U;
        *fraction_0 = 0.0;
        return;
    }
    uint _S70 = uint(g_0);

#line 152
    *base_1 = _S70;
    *fraction_0 = g_0 - float(_S70);
    if((*base_1 + 1U) >= count_1)
    {

#line 155
        *base_1 = count_1 - 1U;
        *fraction_0 = 0.0;

#line 154
    }



    return;
}


#line 160
SpTaps_0 spBilinearTaps_0(uint x_3, uint y_3, uint step_3, uint countX_0, uint countY_0)
{

#line 161
    thread SpTaps_0 taps_0;
    (&taps_0)->count_0 = 0U;

#line 162
    uint t_5 = 0U;
    for(;;)
    {

#line 163
        if(t_5 < 4U)
        {
        }
        else
        {

#line 163
            break;
        }

#line 164
        (&taps_0)->index_0[t_5] = 0U;
        (&taps_0)->weight_0[t_5] = 0.0;

#line 163
        t_5 = t_5 + 1U;

#line 163
    }



    thread uint bx_1;

    thread float fx_1;

    spAxisTaps_0(x_3, step_3, countX_0, &bx_1, &fx_1);

#line 168
    thread uint by_1;

    thread float fy_1;

    spAxisTaps_0(y_3, step_3, countY_0, &by_1, &fy_1);

#line 172
    uint j_0 = 0U;
    for(;;)
    {

#line 173
        if(j_0 < 2U)
        {
        }
        else
        {

#line 173
            break;
        }

#line 173
        uint i_0 = 0U;
        for(;;)
        {

#line 174
            if(i_0 < 2U)
            {
            }
            else
            {

#line 174
                break;
            }

#line 174
            float _S71;
            if(i_0 == 0U)
            {

#line 175
                _S71 = 1.0 - fx_1;

#line 175
            }
            else
            {

#line 175
                _S71 = fx_1;

#line 175
            }

#line 175
            float _S72;

#line 175
            if(j_0 == 0U)
            {

#line 175
                _S72 = 1.0 - fy_1;

#line 175
            }
            else
            {

#line 175
                _S72 = fy_1;

#line 175
            }

#line 175
            float weight_1 = _S71 * _S72;
            if(weight_1 <= 0.0)
            {

#line 177
                i_0 = i_0 + 1U;

#line 174
                continue;
            }



            (&taps_0)->index_0[(&taps_0)->count_0] = (by_1 + j_0) * countX_0 + (bx_1 + i_0);
            (&taps_0)->weight_0[(&taps_0)->count_0] = weight_1;
            (&taps_0)->count_0 = (&taps_0)->count_0 + 1U;

#line 174
            i_0 = i_0 + 1U;

#line 174
        }

#line 173
        j_0 = j_0 + 1U;

#line 173
    }

#line 184
    return taps_0;
}


#line 52
uint spCell_0(KernelContext_0 thread* kernelContext_16)
{

#line 52
    return kernelContext_16->giResolve_0->constants_2[40U].w;
}

uint spCellsX_0(KernelContext_0 thread* kernelContext_17)
{

#line 55
    return kernelContext_17->giResolve_0->constants_2[41U].z;
}


#line 56
uint spCellsY_0(KernelContext_0 thread* kernelContext_18)
{

#line 56
    return kernelContext_18->giResolve_0->constants_2[41U].w;
}


#line 55
uint spCellsX_1(KernelContext_0 thread* kernelContext_19)
{

#line 55
    return kernelContext_19->giResolve_0->constants_2[41U].z;
}


#line 56
uint spCellsY_1(KernelContext_0 thread* kernelContext_20)
{

#line 56
    return kernelContext_20->giResolve_0->constants_2[41U].w;
}


#line 80
uint spTableIndex_0(uint slot_2, uint cell_2, KernelContext_0 thread* kernelContext_21)
{

#line 80
    uint _S73 = spCellsX_1(kernelContext_21);
    uint _S74 = slot_2 * _S73;

#line 81
    uint _S75 = spCellsY_1(kernelContext_21);

#line 81
    return 4U + _S74 * _S75 + cell_2;
}


#line 57
uint spUniformCount_0(KernelContext_0 thread* kernelContext_22)
{

#line 57
    return kernelContext_22->giResolve_0->constants_2[43U].w;
}


#line 207
struct SpProbe_0
{
    float3 position_5;
    float depth_0;
    float3 normal_4;
    uint id_0;
};


#line 57
uint spUniformCount_1(KernelContext_0 thread* kernelContext_23)
{

#line 57
    return kernelContext_23->giResolve_0->constants_2[43U].w;
}


#line 58
uint spCapacity_0(KernelContext_0 thread* kernelContext_24)
{

)cy_msl"
    R"cy_msl(#line 58
    return kernelContext_24->giResolve_0->constants_2[42U].z;
}


#line 59
uint spRecords_0(KernelContext_0 thread* kernelContext_25)
{

#line 59
    uint _S76 = spUniformCount_1(kernelContext_25);

#line 59
    uint _S77 = spCapacity_0(kernelContext_25);

#line 59
    return _S76 + _S77;
}


#line 76
uint spRecordBase_0(uint slot_3, uint record_0, KernelContext_0 thread* kernelContext_26)
{

#line 76
    uint _S78 = spRecords_0(kernelContext_26);
    return (slot_3 * _S78 + record_0) * 11U;
}


#line 214
SpProbe_0 spLoadProbe_0(uint slot_4, uint record_1, KernelContext_0 thread* kernelContext_27)
{

#line 214
    uint _S79 = spRecordBase_0(slot_4, record_1, kernelContext_27);

    float4 device* _S80 = kernelContext_27->giResolve_0->probes_1+_S79;
    float4 device* _S81 = kernelContext_27->giResolve_0->probes_1+(_S79 + 1U);
    thread SpProbe_0 probe_0;
    (&probe_0)->position_5 = (*_S80).xyz;
    (&probe_0)->depth_0 = (*_S80).w;
    (&probe_0)->normal_4 = (*_S81).xyz;
    (&probe_0)->id_0 = (as_type<uint>(((*_S81).w)));
    return probe_0;
}


#line 119
float spGeometryWeight_0(float3 probePosition_0, float3 probeNormal_0, float3 position_6, float3 normal_5, float depth_1, float tolerance_0)
{
    float plane_0 = abs(dot(probeNormal_0, position_6 - probePosition_0));
    float reach_1 = tolerance_0 * depth_1;
    if(reach_1 <= 0.0)
    {

#line 124
        return 0.0;
    }

    float _S82 = max(0.0, dot(probeNormal_0, normal_5));
    float facing_0 = _S82 * _S82;
    float facing_1 = facing_0 * facing_0;

    return max(0.0, 1.0 - plane_0 / reach_1) * (facing_1 * facing_1);
}


#line 226
void spLoadCoefficients_0(uint slot_5, uint record_2, array<float3, int(9)> thread* coefficients_0, float thread* history_0, KernelContext_0 thread* kernelContext_28)
{

#line 226
    uint _S83 = spRecordBase_0(slot_5, record_2, kernelContext_28);
    uint _S84 = _S83 + 2U;
    thread array<float, int(28)> values_0;

#line 228
    uint v_1 = 0U;
    for(;;)
    {

#line 229
        if(v_1 < 7U)
        {
        }
        else
        {

#line 229
            break;
        }

#line 230
        float4 device* _S85 = kernelContext_28->giResolve_0->probes_1+(_S84 + v_1);
        uint _S86 = v_1 * 4U;

#line 231
        values_0[_S86] = (*_S85).x;
        values_0[_S86 + 1U] = (*_S85).y;
        values_0[_S86 + 2U] = (*_S85).z;
        values_0[_S86 + 3U] = (*_S85).w;

#line 229
        v_1 = v_1 + 1U;

#line 229
    }

#line 229
    uint k_0 = 0U;

#line 236
    for(;;)
    {

#line 236
        if(k_0 < 9U)
        {
        }
        else
        {

#line 236
            break;
        }

#line 237
        uint _S87 = k_0 * 3U;

#line 237
        (*coefficients_0)[k_0] = float3(values_0[_S87], values_0[_S87 + 1U], values_0[_S87 + 2U]);

#line 236
        k_0 = k_0 + 1U;

#line 236
    }


    *history_0 = values_0[int(27)];
    return;
}


#line 90
void spBasis_0(float3 d_0, array<float, int(9)> thread* basis_0)
{

#line 91
    (*basis_0)[int(0)] = 0.28209498524665833;
    float _S88 = d_0.y;

#line 92
    (*basis_0)[int(1)] = 0.48860299587249756 * _S88;
    float _S89 = d_0.z;

#line 93
    (*basis_0)[int(2)] = 0.48860299587249756 * _S89;
    float _S90 = d_0.x;

#line 94
    (*basis_0)[int(3)] = 0.48860299587249756 * _S90;
    (*basis_0)[int(4)] = 1.09254801273345947 * (_S90 * _S88);
    (*basis_0)[int(5)] = 1.09254801273345947 * (_S88 * _S89);
    (*basis_0)[int(6)] = 0.31539198756217957 * (3.0 * (_S89 * _S89) - 1.0);
    (*basis_0)[int(7)] = 1.09254801273345947 * (_S90 * _S89);
    (*basis_0)[int(8)] = 0.54627400636672974 * (_S90 * _S90 - _S88 * _S88);
    return;
}


#line 102
uint spBandOf_0(uint k_1)
{

#line 102
    uint _S91;
    if(k_1 == 0U)
    {

#line 103
        _S91 = 0U;

#line 103
    }
    else
    {

#line 103
        if(k_1 < 4U)
        {

#line 103
            _S91 = 1U;

#line 103
        }
        else
        {

#line 103
            _S91 = 2U;

#line 103
        }

#line 103
    }

#line 103
    return _S91;
}

float3 spEvaluate_0(const array<float3, int(9)> thread* coefficients_1, float3 direction_4, float3 band_0)
{

#line 107
    thread array<float, int(9)> basis_1;
    spBasis_0(direction_4, &basis_1);

#line 108
    float3 sum_0 = float3(0.0, 0.0, 0.0);

#line 108
    uint k_2 = 0U;

    for(;;)
    {

#line 110
        if(k_2 < 9U)
        {
        }
        else
        {

#line 110
            break;
        }

#line 111
        uint b_1 = spBandOf_0(k_2);

#line 111
        float scale_0;
        if(b_1 == 0U)
        {

#line 112
            scale_0 = band_0.x;

#line 112
        }
        else
        {

#line 112
            if(b_1 == 1U)
            {

#line 112
                scale_0 = band_0.y;

#line 112
            }
            else
            {

#line 112
                scale_0 = band_0.z;

#line 112
            }

#line 112
        }
        float3 _S92 = sum_0 + (*coefficients_1)[k_2] * float3((basis_1[k_2] * scale_0)) ;

#line 110
        uint k_3 = k_2 + 1U;

#line 110
        sum_0 = _S92;

#line 110
        k_2 = k_3;

#line 110
    }

#line 115
    return float3(max(sum_0.x, 0.0), max(sum_0.y, 0.0), max(sum_0.z, 0.0));
}


#line 133 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
struct GrCombine_0
{
    float3 total_0;
    float totalWeight_0;
    float confidence_4;
};

)cy_msl"
    R"cy_msl(
#line 139
GrCombine_0 grCombineBegin_0()
{

#line 140
    thread GrCombine_0 state_0;
    (&state_0)->total_0 = float3(0.0, 0.0, 0.0);
    (&state_0)->totalWeight_0 = 0.0;
    (&state_0)->confidence_4 = 0.0;
    return state_0;
}

void grCombineAdd_0(GrCombine_0 thread* state_1, const GrSample_0 thread* sample_3, uint excluded_0)
{

#line 147
    uint _S93 = sample_3->source_0;
    if((sample_3->source_0) == 0U)
    {

#line 149
        return;
    }
    if((excluded_0 & (1U << _S93)) != 0U)
    {

#line 152
        return;
    }
    float weight_2 = gpClamp_0(sample_3->confidence_0, 0.0, 1.0);
    if(weight_2 <= 0.0)
    {

#line 156
        return;
    }
    state_1->total_0 = state_1->total_0 + sample_3->radiance_0 * float3(weight_2) ;
    state_1->totalWeight_0 = state_1->totalWeight_0 + weight_2;
    state_1->confidence_4 = max(state_1->confidence_4, weight_2);
    return;
}


#line 168
float grFarFieldWeight_0(float distance_0, KernelContext_0 thread* kernelContext_29)
{

#line 169
    float4 far_0 = (as_type<float4>((kernelContext_29->giResolve_0->constants_2[3U])));
    float begin_0 = far_0.x;
    if(begin_0 <= 0.0)
    {

#line 172
        return 1.0;
    }
    float _S94 = far_0.y;

#line 174
    float ratio_0;

#line 174
    if(_S94 > 0.0)
    {

#line 174
        ratio_0 = far_0.z / _S94;

#line 174
    }
    else
    {

#line 174
        ratio_0 = 8.0;

#line 174
    }
    float width_0 = begin_0 * gpClamp_0(ratio_0 / 100.0, 0.15000000596046448, 1.0);
    if(distance_0 <= begin_0)
    {

#line 177
        return 0.0;
    }
    float t_6 = gpClamp_0((distance_0 - begin_0) / width_0, 0.0, 1.0);
    return t_6 * t_6 * (3.0 - 2.0 * t_6);
}


#line 163
float3 grCombineEnd_0(const GrCombine_0 thread* state_2)
{

#line 163
    float _S95 = state_2->totalWeight_0;

#line 163
    float3 _S96;
    if((state_2->totalWeight_0) > 0.0)
    {

#line 164
        _S96 = state_2->total_0 / float3(_S95) ;

#line 164
    }
    else
    {

#line 164
        _S96 = float3(0.0, 0.0, 0.0);

#line 164
    }

#line 164
    return _S96;
}


#line 200
uint grReflectionRays_0(float roughness_0, KernelContext_0 thread* kernelContext_30)
{

#line 201
    uint4 words_0 = kernelContext_30->giResolve_0->constants_2[4U];
    float mirror_0 = (as_type<float>((words_0.x)));
    float cacheMin_0 = (as_type<float>((words_0.y)));
    uint _S97 = max(1U, words_0.z);
    if(roughness_0 <= mirror_0)
    {

#line 206
        return _S97;
    }
    if(roughness_0 < cacheMin_0)
    {

        float _S98 = float(_S97);


        return max(1U, uint(floor(_S98 + (1.0 - _S98) * gpClamp_0((roughness_0 - mirror_0) / max(cacheMin_0 - mirror_0, 0.00009999999747379), 0.0, 1.0) + 0.5)));
    }
    return 0U;
}


#line 73
uint grHash_0(uint pixel_0, uint frame_0, uint stream_0)
{

    uint hash_0 = ((pixel_0 * 2654435769U) ^ (frame_0 * 2246822507U)) ^ (stream_0 * 3266489909U);

    uint hash_1 = (hash_0 ^ (hash_0 >> 15U)) * 625341585U;

    return hash_1 ^ (hash_1 >> 13U);
}

float grUnit_0(uint hash_2)
{

#line 84
    return float(hash_2 & 16777215U) / 1.6777216e+07;
}


#line 109
bool grTraces_0(uint pixel_1, uint frame_1, float density_0)
{

#line 110
    if(density_0 >= 1.0)
    {

#line 111
        return true;
    }
    return (grUnit_0(grHash_0(pixel_1, frame_1, 5U))) < density_0;
}


#line 220
float3 grLobeDirection_0(float3 reflection_0, float3 normal_6, float roughness_1, uint index_1, uint count_2, uint rotation_0)
{
    float3 axis_0 = giNormalisedOr_0(reflection_0, normal_6);

#line 222
    bool _S99;
    if(count_2 <= 1U)
    {

#line 223
        _S99 = true;

#line 223
    }
    else
    {

#line 223
        _S99 = roughness_1 <= 0.0;

#line 223
    }

#line 223
    if(_S99)
    {

#line 224
        return axis_0;
    }

    float _S100 = float(index_1);

    float radius_1 = sqrt((_S100 + 0.5) / float(count_2)) * (gpClamp_0(roughness_1, 0.0, 1.0) * 0.69999998807907104);
    float angle_0 = 2.3999631404876709 * _S100 + float(rotation_0) * 0.6180340051651001;

#line 230
    float3 tangent_0;
    if((abs(axis_0.y)) < 0.99000000953674316)
    {

#line 231
        tangent_0 = cross(float3(0.0, 1.0, 0.0), axis_0);

#line 231
    }
    else
    {

#line 231
        tangent_0 = cross(float3(1.0, 0.0, 0.0), axis_0);

#line 231
    }

    float3 tangent_1 = giNormalisedOr_0(tangent_0, float3(1.0, 0.0, 0.0));



    float3 unit_1 = giNormalisedOr_0(axis_0 + tangent_1 * float3((radius_1 * cos(angle_0)))  + cross(axis_0, tangent_1) * float3((radius_1 * sin(angle_0))) , axis_0);
    float _S101 = dot(unit_1, normal_6);

#line 238
    if(_S101 < 0.0)
    {

#line 238
        tangent_0 = giNormalisedOr_0(unit_1 - normal_6 * float3((2.0 * _S101)) , axis_0);

#line 238
    }
    else
    {

#line 238
        tangent_0 = unit_1;

#line 238
    }

#line 238
    return tangent_0;
}


#line 263
GrSample_0 grGatherSample_0(const GrSample_0 thread* ray_0, const GrSample_0 thread* standIn_0, uint excluded_1)
{

#line 263
    uint _S102 = standIn_0->source_0;

#line 263
    bool hasStandIn_0;
    if((standIn_0->source_0) != 0U)
    {

#line 264
        hasStandIn_0 = (standIn_0->confidence_0) > 0.0;

#line 264
    }
    else
    {

#line 264
        hasStandIn_0 = false;

#line 264
    }

#line 264
    if(hasStandIn_0)
    {

#line 264
        hasStandIn_0 = (excluded_1 & (1U << _S102)) == 0U;

)cy_msl"
    R"cy_msl(#line 264
    }
    else
    {

#line 264
        hasStandIn_0 = false;

#line 264
    }

    thread GrSample_0 sample_4 = *ray_0;
    if((ray_0->source_0) == 0U)
    {

#line 268
        if(!hasStandIn_0)
        {

#line 269
            return *ray_0;
        }
        (&sample_4)->source_0 = _S102;
        (&sample_4)->radiance_0 = standIn_0->radiance_0;

#line 267
    }
    else
    {



        if(hasStandIn_0)
        {
            (&sample_4)->radiance_0 = standIn_0->radiance_0 + (ray_0->radiance_0 - standIn_0->radiance_0) * float3(gpClamp_0(ray_0->confidence_0, 0.0, 1.0)) ;

#line 273
        }

#line 267
    }

#line 277
    (&sample_4)->confidence_0 = 1.0;
    return sample_4;
}


#line 658 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
void spIntegrate_0(uint pixel_2, KernelContext_0 thread* kernelContext_31)
{

#line 659
    uint4 counts_0 = kernelContext_31->giResolve_0->constants_2[0U];
    uint _S103 = counts_0.z;

#line 660
    if(pixel_2 >= _S103)
    {

#line 661
        return;
    }
    uint _S104 = counts_0.w;

#line 663
    uint record_3 = (_S104 * _S103 + pixel_2) * 2U;
    float4 at_0 = kernelContext_31->giResolve_0->surfaces_0[record_3];
    float4 facing_2 = kernelContext_31->giResolve_0->surfaces_0[record_3 + 1U];
    float _S105 = at_0.w;

#line 666
    if(_S105 <= 0.0)
    {

#line 667
        uint _S106 = pixel_2 * 2U;

#line 667
        float4 _S107 = float4(0.0, 0.0, 0.0, 0.0);

#line 667
        *(kernelContext_31->giResolve_0->noisy_0+_S106) = _S107;
        *(kernelContext_31->giResolve_0->noisy_0+(_S106 + 1U)) = _S107;
        return;
    }

    uint frame_2 = kernelContext_31->giResolve_0->constants_2[18U].x;
    float3 position_7 = at_0.xyz;
    float3 normal_7 = giNormalisedOr_0(facing_2.xyz, float3(0.0, 1.0, 0.0));
    float roughness_2 = facing_2.w;
    float3 offset_1 = position_7 - (as_type<float3>((kernelContext_31->giResolve_0->constants_2[1U].xyz)));
    float3 _S108 = float3(0.0, 0.0, -1.0);

#line 677
    float3 incident_0 = giNormalisedOr_0(offset_1, _S108);

    float3 reflection_1 = giNormalisedOr_0(incident_0 - normal_7 * float3((2.0 * dot(incident_0, normal_7))) , normal_7);
    float rough_0 = gpClamp_0(roughness_2, 0.0, 1.0);
    float3 specularBand_0 = float3(1.0, 1.0 + -0.33333331346511841 * rough_0, 1.0 + -0.75 * rough_0);

    float3 irradianceBand_0 = float3(1.0, 0.66666668653488159, 0.25);
    float4 _S109 = (as_type<float4>((kernelContext_31->giResolve_0->constants_2[43U])));

#line 684
    uint _S110 = spWidth_0(kernelContext_31);
    uint _S111 = pixel_2 % _S110;

#line 685
    uint _S112 = spWidth_0(kernelContext_31);
    uint _S113 = pixel_2 / _S112;

    float3 _S114 = float3(0.0, 0.0, 0.0);

#line 688
    float nearestPlane_0 = 1.00000001504746622e+30;

#line 688
    float3 irradiance_0 = _S114;

#line 688
    float3 glossy_0 = _S114;

#line 688
    float3 cache_0 = _S114;

#line 688
    uint nearest_0 = 4294967295U;

#line 688
    uint pass_0 = 0U;

#line 688
    float cacheConfidence_0 = 0.0;

#line 688
    float openness_0 = 0.0;

#line 688
    float total_1 = 0.0;

#line 696
    for(;;)
    {

#line 696
        if(pass_0 < 2U)
        {
        }
        else
        {

#line 696
            break;
        }

#line 696
        SpTaps_0 _S115;

        if(pass_0 == 0U)
        {

#line 698
            uint _S116 = spSpacing_0(kernelContext_31);

#line 698
            uint _S117 = spTilesX_0(kernelContext_31);

#line 698
            uint _S118 = spTilesY_0(kernelContext_31);

#line 698
            _S115 = spBilinearTaps_0(_S111, _S113, _S116, _S117, _S118);

#line 698
        }
        else
        {

#line 698
            uint _S119 = spCell_0(kernelContext_31);

#line 698
            uint _S120 = spCellsX_0(kernelContext_31);

#line 698
            uint _S121 = spCellsY_0(kernelContext_31);

#line 698
            _S115 = spBilinearTaps_0(_S111, _S113, _S119, _S120, _S121);

#line 698
        }

#line 698
        float nearestPlane_1 = nearestPlane_0;

#line 698
        float3 irradiance_1 = irradiance_0;

#line 698
        float3 glossy_1 = glossy_0;

#line 698
        float3 cache_1 = cache_0;

#line 698
        uint nearest_1 = nearest_0;

#line 698
        uint tap_0 = 0U;

#line 698
        float cacheConfidence_1 = cacheConfidence_0;

#line 698
        float openness_1 = openness_0;

#line 698
        float total_2 = total_1;

        for(;;)
        {

#line 700
            if(tap_0 < (_S115.count_0))
            {
            }
            else
            {

#line 700
                break;
            }

#line 698
            SpTaps_0 _S122 = _S115;

            uint _S123 = tap_0;

#line 700
            uint index_2;

            if(pass_0 == 1U)
            {

#line 702
                uint _S124 = spTableIndex_0(_S104, _S122.index_0[_S123], kernelContext_31);
                uint device* _S125 = kernelContext_31->giResolve_0->slots_0+_S124;

#line 703
                uint taken_0 = *_S125;
                if((*_S125) == 4294967295U)
                {

#line 705
                    tap_0 = tap_0 + 1U;

#line 700
                    continue;
                }

#line 700
                uint _S126 = spUniformCount_0(kernelContext_31);

#line 700
                index_2 = _S126 + taken_0;

            }
            else
            {

#line 702
                index_2 = _S122.index_0[_S123];

#line 702
            }

#line 702
            SpProbe_0 _S127 = spLoadProbe_0(_S104, index_2, kernelContext_31);

#line 710
            if((_S127.depth_0) <= 0.0)
            {

#line 711
                tap_0 = tap_0 + 1U;

#line 700
                continue;
            }

#line 713
            float plane_1 = abs(dot(_S127.normal_4, position_7 - _S127.position_5));

#line 713
            float nearestPlane_2;

#line 713
            uint nearest_2;
            if(plane_1 < nearestPlane_1)
            {

)cy_msl"
    R"cy_msl(#line 714
                nearestPlane_2 = plane_1;

#line 714
                nearest_2 = index_2;

#line 714
            }
            else
            {

#line 714
                nearestPlane_2 = nearestPlane_1;

#line 714
                nearest_2 = nearest_1;

#line 714
            }

#line 719
            float weight_3 = _S115.weight_0[tap_0] * spGeometryWeight_0(_S127.position_5, _S127.normal_4, position_7, normal_7, _S105, _S109.y);

            if(weight_3 <= 0.0)
            {

#line 721
                nearestPlane_1 = nearestPlane_2;

#line 721
                nearest_1 = nearest_2;
                tap_0 = tap_0 + 1U;

#line 700
                continue;
            }

#line 724
            thread array<float3, int(9)> coefficients_2;
            thread float history_1;

#line 725
            spLoadCoefficients_0(_S104, index_2, &coefficients_2, &history_1, kernelContext_31);

#line 725
            uint _S128 = spRecordBase_0(_S104, index_2, kernelContext_31);


            float4 device* _S129 = kernelContext_31->giResolve_0->probes_1+(_S128 + 9U);

#line 728
            thread array<float3, int(9)> _S130 = coefficients_2;

#line 728
            float3 _S131 = spEvaluate_0(&_S130, normal_7, irradianceBand_0);

#line 728
            float3 _S132 = float3(weight_3) ;
            float3 _S133 = irradiance_1 + _S131 * _S132;

#line 729
            thread array<float3, int(9)> _S134 = coefficients_2;

#line 729
            float3 _S135 = spEvaluate_0(&_S134, reflection_1, specularBand_0);
            float3 _S136 = glossy_1 + _S135 * _S132;
            float3 _S137 = cache_1 + (*_S129).xyz * _S132;
            float cacheConfidence_2 = cacheConfidence_1 + (*_S129).w * weight_3;
            float openness_2 = openness_1 + (*(kernelContext_31->giResolve_0->probes_1+(_S128 + 10U))).x * weight_3;
            float total_3 = total_2 + weight_3;

#line 734
            nearestPlane_1 = nearestPlane_2;

#line 734
            irradiance_1 = _S133;

#line 734
            glossy_1 = _S136;

#line 734
            cache_1 = _S137;

#line 734
            nearest_1 = nearest_2;

#line 734
            cacheConfidence_1 = cacheConfidence_2;

#line 734
            openness_1 = openness_2;

#line 734
            total_2 = total_3;

#line 700
            tap_0 = tap_0 + 1U;

#line 700
        }

#line 696
        uint pass_1 = pass_0 + 1U;

#line 696
        nearestPlane_0 = nearestPlane_1;

#line 696
        irradiance_0 = irradiance_1;

#line 696
        glossy_0 = glossy_1;

#line 696
        cache_0 = cache_1;

#line 696
        nearest_0 = nearest_1;

#line 696
        pass_0 = pass_1;

#line 696
        cacheConfidence_0 = cacheConfidence_1;

#line 696
        openness_0 = openness_1;

#line 696
        total_1 = total_2;

#line 696
    }

#line 737
    if(total_1 < 0.00009999999747379)
    {

#line 738
        if(nearest_0 == 4294967295U)
        {
            uint _S138 = pixel_2 * 2U;

#line 740
            float4 _S139 = float4(0.0, 0.0, 0.0, (as_type<float>((kernelContext_31->giResolve_0->constants_2[44U].x))));

#line 740
            *(kernelContext_31->giResolve_0->noisy_0+_S138) = _S139;
            *(kernelContext_31->giResolve_0->noisy_0+(_S138 + 1U)) = _S139;
            return;
        }
        thread array<float3, int(9)> coefficients_3;
        thread float history_2;

#line 745
        spLoadCoefficients_0(_S104, nearest_0, &coefficients_3, &history_2, kernelContext_31);

#line 745
        uint _S140 = spRecordBase_0(_S104, nearest_0, kernelContext_31);

#line 745
        thread array<float3, int(9)> _S141 = coefficients_3;

#line 745
        float3 _S142 = spEvaluate_0(&_S141, normal_7, irradianceBand_0);

#line 745
        thread array<float3, int(9)> _S143 = coefficients_3;

#line 745
        float3 _S144 = spEvaluate_0(&_S143, reflection_1, specularBand_0);

#line 750
        uint _S145 = _S140 + 9U;
        float _S146 = (*(kernelContext_31->giResolve_0->probes_1+_S145)).w;
        float _S147 = (*(kernelContext_31->giResolve_0->probes_1+(_S140 + 10U))).x;

#line 752
        cache_0 = (*(kernelContext_31->giResolve_0->probes_1+_S145)).xyz;

#line 752
        irradiance_0 = _S142;

#line 752
        glossy_0 = _S144;

#line 752
        cacheConfidence_0 = _S146;

#line 752
        openness_0 = _S147;

#line 737
    }
    else
    {

#line 754
        float3 _S148 = irradiance_0 / float3(total_1) ;
        float3 _S149 = glossy_0 / float3(total_1) ;

        float cacheConfidence_3 = cacheConfidence_0 / total_1;
        float openness_3 = openness_0 / total_1;

#line 758
        cache_0 = cache_0 / float3(total_1) ;

#line 758
        irradiance_0 = _S148;

#line 758
        glossy_0 = _S149;

#line 758
        cacheConfidence_0 = cacheConfidence_3;

#line 758
        openness_0 = openness_3;

#line 737
    }

#line 761
    uint4 shape_1 = kernelContext_31->giResolve_0->constants_2[42U];
    uint excluded_2 = kernelContext_31->giResolve_0->constants_2[2U].w;
    GrCombine_0 _S150 = grCombineBegin_0();

#line 763
    thread GrCombine_0 state_3 = _S150;
    if(((shape_1.w) & 1U) != 0U)
    {

#line 764
        thread GrSample_0 _S151 = grSample_0(cache_0, cacheConfidence_0, 3U);

#line 764
        grCombineAdd_0(&state_3, &_S151, excluded_2);

#line 764
    }


    uint _S152 = GiSoftwareTier_source_0();

#line 767
    thread GrSample_0 _S153 = grSample_0(irradiance_0, 1.0, _S152);

#line 767
    grCombineAdd_0(&state_3, &_S153, excluded_2);
    thread GrSample_0 sky_0 = grSample_0(_S114, 0.0, 9U);

#line 768
    float _S154 = grFarFieldWeight_0(length(offset_1), kernelContext_31);

    if(_S154 > 0.0)
    {

#line 770
        float3 _S155 = gpSky_0(normal_7, kernelContext_31);
        (&sky_0)->radiance_0 = _S155 * float3(openness_0) ;
        (&sky_0)->confidence_0 = gpClamp_0(_S154 * openness_0, 0.0, 1.0);

#line 770
    }

#line 770
    thread GrSample_0 _S156 = sky_0;

#line 770
    grCombineAdd_0(&state_3, &_S156, excluded_2);

#line 770
    thread GrCombine_0 _S157 = state_3;

#line 770
    float3 _S158 = grCombineEnd_0(&_S157);

#line 775
    float3 diffuse_0 = _S158 * float3((as_type<float>((kernelContext_31->giResolve_0->constants_2[5U].x)))) ;

    GrSample_0 probeSpecular_0 = grSample_0(glossy_0, gpClamp_0(roughness_2, 0.15000000596046448, 1.0), _S152);
    thread GrCombine_0 reflected_0 = _S150;

#line 778
    uint _S159 = grReflectionRays_0(roughness_2, kernelContext_31);

    float density_1 = (as_type<float>((kernelContext_31->giResolve_0->constants_2[1U].w)));

#line 780
    bool _S160;
    if(_S159 > 0U)
    {

#line 781
        _S160 = grTraces_0(pixel_2, frame_2, density_1);

#line 781
    }
    else
    {

)cy_msl"
    R"cy_msl(#line 781
        _S160 = false;

#line 781
    }

#line 781
    if(_S160)
    {

#line 782
        float4 rays_1 = (as_type<float4>((kernelContext_31->giResolve_0->constants_2[2U])));


        uint _S161 = grHash_0(pixel_2, frame_2, 7U) % _S159;

#line 785
        GrSample_0 _S162 = grWorldRay_0(position_7, normal_7, giNormalisedOr_0(grLobeDirection_0(reflection_1, normal_7, roughness_2, _S161, _S159, frame_2), _S108), rays_1.y, rays_1.x, kernelContext_31);

#line 785
        thread GrSample_0 _S163 = _S162;

#line 785
        thread GrSample_0 _S164 = probeSpecular_0;

#line 785
        GrSample_0 _S165 = grGatherSample_0(&_S163, &_S164, 0U);

#line 785
        thread GrSample_0 _S166 = _S165;

#line 785
        grCombineAdd_0(&reflected_0, &_S166, 0U);

#line 781
    }

#line 781
    thread GrSample_0 _S167 = probeSpecular_0;

#line 781
    grCombineAdd_0(&reflected_0, &_S167, 0U);

#line 781
    thread GrCombine_0 _S168 = reflected_0;

#line 781
    float3 _S169 = grCombineEnd_0(&_S168);

#line 793
    float3 specular_0 = _S169 * float3((as_type<float>((kernelContext_31->giResolve_0->constants_2[5U].y)))) ;
    float samples_1 = (as_type<float>((kernelContext_31->giResolve_0->constants_2[44U].x)));
    uint _S170 = pixel_2 * 2U;

#line 795
    *(kernelContext_31->giResolve_0->noisy_0+_S170) = float4(diffuse_0, samples_1);
    *(kernelContext_31->giResolve_0->noisy_0+(_S170 + 1U)) = float4(specular_0, samples_1);
    return;
}


#line 32 "src/rendering/gi_gpu/shaders/gi_screen_probes.slang"
[[kernel]] void cyGiProbeIntegrate(uint3 thread_0 [[thread_position_in_grid]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiResolveSet_default_0 constant* giResolve_1 [[buffer(3)]])
{

#line 32
    thread KernelContext_0 kernelContext_32;

#line 32
    (&kernelContext_32)->giProbes_0 = giProbes_1;

#line 32
    (&kernelContext_32)->giScene_0 = giScene_1;

#line 32
    (&kernelContext_32)->giResolve_0 = giResolve_1;

#line 32
    spIntegrate_0(thread_0.x, &kernelContext_32);

    return;
}

)cy_msl";

/// cyGiProbeTraceHardware.metal, 107373 bytes.
inline constexpr auto kGiProbeTraceHardwareMslText = detail::join_msl(
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
            

#line 118 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
struct GrSample_0
{
    float3 radiance_0;
    float confidence_0;
    uint source_0;
};


#line 53
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


#line 78 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 66 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
struct GiResolveSet_default_0
{
    uint4 device* constants_2;
    float4 device* surfaces_0;
    float4 device* noisy_0;
    float4 device* probes_1;
    uint device* slots_0;
};


#line 22 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
struct GiHardwareSet_default_0
{
    metal::raytracing::acceleration_structure<metal::raytracing::instancing> topLevel_0;
    uint4 device* instances_0;
    float device* positions_0;
    uint device* indices_0;
};


#line 603 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
struct KernelContext_0
{
    GiProbeSet_default_0 constant* giProbes_0;
    GiSceneSet_default_0 constant* giScene_0;
    GiResolveSet_default_0 constant* giResolve_0;
    GiHardwareSet_default_0 constant* giHardware_0;
    float4 threadgroup* spStandIn_0;
    float3 threadgroup* spOrigin_0;
    array<float3, int(9)> threadgroup* spHistory_0;
    float threadgroup* spHistoryCount_0;
    uint threadgroup* spReprojected_0;
    array<float, int(16)> threadgroup* spWeights_0;
    array<float3, int(64)> threadgroup* spRayValue_0;
    array<float3, int(64)> threadgroup* spRayDirection_0;
    array<float, int(64)> threadgroup* spRayWeight_0;
    array<uint, int(64)> threadgroup* spRayValid_0;
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


#line 27 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
float3 ghVertex_0(uint vertex_0, KernelContext_0 thread* kernelContext_0)
{

#line 28
    uint _S1 = vertex_0 * 3U;

#line 28
    return float3(kernelContext_0->giHardware_0->positions_0[_S1], kernelContext_0->giHardware_0->positions_0[_S1 + 1U], kernelContext_0->giHardware_0->positions_0[_S1 + 2U]);
}


#line 99 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 100
    float squared_0 = dot(v_0, v_0);

#line 100
    float3 _S2;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 101
        _S2 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 101
    }
    else
    {

#line 101
        _S2 = fallback_0;

#line 101
    }

#line 101
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


#line 33 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
GiWorldHit_0 GiHardwareTier_trace_0(float3 origin_0, float3 direction_0, float maxDistance_0, KernelContext_0 thread* kernelContext_1)
{

#line 34
    thread RayDesc_0 ray_0;
    (&ray_0)->Origin_0 = origin_0;
    (&ray_0)->Direction_0 = direction_0;
    (&ray_0)->TMin_0 = 0.00100000004749745;
    (&ray_0)->TMax_0 = maxDistance_0;
    thread raytracing::intersection_query<raytracing::triangle_data, raytracing::instancing> query_0;
    (&query_0)->reset({ (ray_0.Origin_0), (ray_0.Direction_0), (ray_0.TMin_0), (ray_0.TMax_0) }, (kernelContext_1->giHardware_0->topLevel_0), (255U), _slang_ray_flags_to_intersection_params((1U)));
    for(;;)
    {

#line 41
        bool _S4 = ((&query_0)->next());

#line 41
        if(_S4)
        {
        }
        else
        {

#line 41
            break;
        }

#line 41
    }

    uint _S5 = (_slang_committed_intersection_type_to_uint((&query_0)->get_committed_intersection_type()));

#line 43
    if(_S5 != 1U)
    {

#line 44
        return gpMiss_0();
    }
    float t_1 = ((&query_0)->get_committed_distance());
    uint instance_0 = ((&query_0)->get_committed_user_instance_id());
    uint primitive_0 = ((&query_0)->get_committed_primitive_id());
    uint _S6 = instance_0 * 4U;

#line 49
    uint4 record_0 = kernelContext_1->giHardware_0->instances_0[_S6];
    uint first_0 = record_0.y + primitive_0 * 3U;
    uint _S7 = record_0.x;

#line 51
    float3 _S8 = ghVertex_0(_S7 + kernelContext_1->giHardware_0->indices_0[first_0], kernelContext_1);

#line 51
    float3 _S9 = ghVertex_0(_S7 + kernelContext_1->giHardware_0->indices_0[first_0 + 1U], kernelContext_1);

#line 51
    float3 _S10 = ghVertex_0(_S7 + kernelContext_1->giHardware_0->indices_0[first_0 + 2U], kernelContext_1);


    float4 c0_0 = (as_type<float4>((kernelContext_1->giHardware_0->instances_0[_S6 + 1U])));


    float3 local_0 = cross(_S9 - _S8, _S10 - _S8);
    float3 normal_1 = giNormalisedOr_0(c0_0.xyz * float3(local_0.x)  + (as_type<float4>((kernelContext_1->giHardware_0->instances_0[_S6 + 2U]))).xyz * float3(local_0.y)  + (as_type<float4>((kernelContext_1->giHardware_0->instances_0[_S6 + 3U]))).xyz * float3(local_0.z) , float3(0.0, 1.0, 0.0));

#line 58
    float3 normal_2;

    if((dot(normal_1, direction_0)) > 0.0)
    {

#line 60
        normal_2 = - normal_1;

#line 60
    }
    else
    {

#line 60
        normal_2 = normal_1;

#line 60
    }


    thread GiWorldHit_0 hit_1;
    (&hit_1)->hit_0 = true;
    (&hit_1)->t_0 = t_1;
    (&hit_1)->position_0 = origin_0 + direction_0 * float3(t_1) ;
    (&hit_1)->normal_0 = normal_2;


    (&hit_1)->confidence_1 = (as_type<float4>((kernelContext_1->giProbes_0->constants_0[25U]))).z - gpClamp_0(c0_0.w * 2.0, 0.0, 0.40000000596046448);
    return hit_1;
}


#line 124 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
GrSample_0 grSample_0(float3 radiance_1, float confidence_2, uint source_1)
{

#line 125
    thread GrSample_0 sample_0;
    (&sample_0)->radiance_0 = radiance_1;
    (&sample_0)->confidence_0 = confidence_2;
    (&sample_0)->source_0 = source_1;
    return sample_0;
}


#line 73 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
uint GiHardwareTier_source_0()
{

#line 74
    return 7U;
}


#line 689 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiTierSample_0
{
    float3 radiance_2;
    float confidence_3;
    float hitDistance_0;
    uint source_2;
};


#line 696
GiTierSample_0 gpTierSample_0(uint source_3)
{

#line 697
    thread GiTierSample_0 sample_1;
    (&sample_1)->radiance_2 = float3(0.0, 0.0, 0.0);
    (&sample_1)->confidence_3 = 0.0;
    (&sample_1)->hitDistance_0 = -1.0;
    (&sample_1)->source_2 = source_3;
    return sample_1;
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


#line 471 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_1, float cell_0)
{

#line 472
    return int(floor(value_1 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_1, float3 normal_3, KernelContext_0 thread* kernelContext_3)
{

#line 482
    uint4 grid_0 = kernelContext_3->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 485
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S12 = radius_0 * radius_0;
    float _S13 = position_1.x;

#line 490
    int _S14 = giCell_0(_S13 - radius_0, cell_1);

#line 490
    float _S15 = position_1.y;

#line 490
    int _S16 = giCell_0(_S15 - radius_0, cell_1);
    float _S17 = position_1.z;

#line 491
    int _S18 = giCell_0(_S17 - radius_0, cell_1);
    int _S19 = giCell_0(_S13 + radius_0, cell_1);

#line 492
    int _S20 = giCell_0(_S15 + radius_0, cell_1);
    int _S21 = giCell_0(_S17 + radius_0, cell_1);

#line 493
    float bestScore_0 = 1.0e+09;

#line 493
    float bestAlignment_0 = 0.0;

#line 493
    uint best_0 = 4294967295U;

#line 493
    int z_1 = _S18;



    for(;;)
    {

#line 497
        if(z_1 <= _S21)
        {
        }
        else
        {

#line 497
            break;
        }

#line 497
        int y_1 = _S16;
        for(;;)
        {

#line 498
            if(y_1 <= _S20)
            {
            }
            else
            {

#line 498
                break;
            }

#line 498
            float bestScore_1 = bestScore_0;

#line 498
            float bestAlignment_1 = bestAlignment_0;

#line 498
            uint best_1 = best_0;

#line 498
            int x_1 = _S14;
            for(;;)
            {

#line 499
                if(x_1 <= _S19)
                {
                }
                else
                {

#line 499
                    break;
                }
                uint _S22 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 501
                uint first_1 = kernelContext_3->giScene_0->gridRanges_0[_S22];
                uint _S23 = kernelContext_3->giScene_0->gridRanges_0[_S22 + 1U];

#line 502
                float bestScore_2 = bestScore_1;

#line 502
                float bestAlignment_2 = bestAlignment_1;

#line 502
                uint best_2 = best_1;

#line 502
                uint item_0 = first_1;
                for(;;)
                {

#line 503
                    if(item_0 < (first_1 + _S23))
                    {
                    }
                    else
                    {

#line 503
                        break;
                    }

#line 504
                    uint handle_0 = kernelContext_3->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_3->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 506
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }



                    uint _S24 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_3->giScene_0->cards_0[_S24 + 1U].xyz, normal_3);
                    if(alignment_0 <= 0.25)
                    {

#line 511
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 513
                    float3 offset_0 = kernelContext_3->giScene_0->cards_0[_S24].xyz - position_1;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S12)
                    {

#line 516
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 519
                    float score_0 = squared_1 / alignment_0;

#line 519
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 520
                        better_0 = score_0 < bestScore_2;

#line 520
                    }
                    else
                    {

#line 521
                        if(alignment_0 != bestAlignment_2)
                        {

#line 521
                            better_0 = alignment_0 > bestAlignment_2;

#line 521
                        }
                        else
                        {

#line 521
                            better_0 = handle_0 < best_2;

#line 521
                        }

#line 520
                    }

#line 520
                    float bestScore_3;

#line 520
                    float bestAlignment_3;

#line 520
                    uint best_3;


                    if(better_0)
                    {

)cy_msl",
    R"cy_msl(#line 523
                        bestScore_3 = score_0;

#line 523
                        bestAlignment_3 = alignment_0;

#line 523
                        best_3 = handle_0;

#line 523
                    }
                    else
                    {

#line 523
                        bestScore_3 = bestScore_2;

#line 523
                        bestAlignment_3 = bestAlignment_2;

#line 523
                        best_3 = best_2;

#line 523
                    }

#line 523
                    bestScore_2 = bestScore_3;

#line 523
                    bestAlignment_2 = bestAlignment_3;

#line 523
                    best_2 = best_3;

#line 503
                    item_0 = item_0 + 1U;

#line 503
                }

#line 499
                int x_2 = x_1 + int(1);

#line 499
                bestScore_1 = bestScore_2;

#line 499
                bestAlignment_1 = bestAlignment_2;

#line 499
                best_1 = best_2;

#line 499
                x_1 = x_2;

#line 499
            }

#line 498
            int y_2 = y_1 + int(1);

#line 498
            bestScore_0 = bestScore_1;

#line 498
            bestAlignment_0 = bestAlignment_1;

#line 498
            best_0 = best_1;

#line 498
            y_1 = y_2;

#line 498
        }

#line 497
        z_1 = z_1 + int(1);

#line 497
    }

#line 532
    return best_0;
}


#line 776 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpWorldRadiance_0(const GiWorldHit_0 thread* hit_2, float3 direction_2, uint source_4, uint cacheFrame_0, KernelContext_0 thread* kernelContext_4)
{

#line 777
    thread GiTierSample_0 sample_2 = gpTierSample_0(source_4);
    if(!hit_2->hit_0)
    {

#line 779
        (&sample_2)->source_2 = 9U;

#line 779
        float3 _S25 = gpSky_0(direction_2, kernelContext_4);
        (&sample_2)->radiance_2 = _S25;
        (&sample_2)->confidence_3 = 1.0;
        (&sample_2)->hitDistance_0 = -1.0;
        return sample_2;
    }
    (&sample_2)->hitDistance_0 = hit_2->t_0;

#line 785
    uint _S26 = giFindCard_0(hit_2->position_0, hit_2->normal_0, kernelContext_4);

    if(_S26 != 4294967295U)
    {

#line 788
        uint _S27 = _S26 * 2U;

#line 788
        (&sample_2)->radiance_2 = (*(kernelContext_4->giScene_0->cardState_0+_S27)).xyz;
        uint last_0 = (as_type<uint>(((*(kernelContext_4->giScene_0->cardState_0+(_S27 + 1U))).w)));

#line 789
        float age_0;
        if(cacheFrame_0 > last_0)
        {

#line 790
            age_0 = float(cacheFrame_0 - last_0);

#line 790
        }
        else
        {

#line 790
            age_0 = 0.0;

#line 790
        }


        (&sample_2)->confidence_3 = hit_2->confidence_1 * (1.0 - gpClamp_0(age_0 / 240.0, 0.0, 1.0) * 0.60000002384185791);
        return sample_2;
    }
    (&sample_2)->confidence_3 = hit_2->confidence_1 * 0.20000000298023224;
    return sample_2;
}


#line 468 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
GrSample_0 spProbeRay_0(float3 origin_1, float3 unit_0, float reach_0, KernelContext_0 thread* kernelContext_5)
{

#line 469
    uint cacheFrame_1 = kernelContext_5->giProbes_0->constants_0[24U].w;

#line 469
    GiWorldHit_0 _S28 = GiHardwareTier_trace_0(origin_1, unit_0, reach_0, kernelContext_5);

#line 469
    bool _S29;

    if(_S28.hit_0)
    {

#line 471
        _S29 = (dot(_S28.normal_0, unit_0)) > 0.0;

#line 471
    }
    else
    {

#line 471
        _S29 = false;

#line 471
    }

#line 471
    if(_S29)
    {

#line 472
        return grSample_0(float3(0.0, 0.0, 0.0), 0.0, 0U);
    }
    uint _S30 = GiHardwareTier_source_0();

#line 474
    thread GiWorldHit_0 _S31 = _S28;

#line 474
    GiTierSample_0 _S32 = gpWorldRadiance_0(&_S31, unit_0, _S30, cacheFrame_1, kernelContext_5);
    return grSample_0(_S32.radiance_2, _S32.confidence_3, _S32.source_2);
}


#line 207
struct SpProbe_0
{
    float3 position_2;
    float depth_0;
    float3 normal_4;
    uint id_0;
};


#line 115 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
uint giLevelCount_0(KernelContext_0 thread* kernelContext_6)
{

#line 116
    return kernelContext_6->giScene_0->constants_1[0U].x;
}


#line 123
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_7)
{

#line 124
    return (as_type<float>((kernelContext_7->giScene_0->constants_1[9U].y)));
}


#line 119
uint giWindowBricks_0(KernelContext_0 thread* kernelContext_8)
{

#line 120
    return kernelContext_8->giScene_0->constants_1[0U].y;
}


#line 167
int giWrap_0(int value_2, int modulus_0)
{

#line 168
    uint _S33 = uint(modulus_0);
    uint _S34 = (uint(value_2) + (_S33 << 20U)) % _S33;

#line 169
    return int(_S34);
}


#line 95
float giLerp_0(float a_0, float b_0, float t_2)
{

#line 96
    return a_0 + (b_0 - a_0) * t_2;
}


#line 128
float giTrilinear_0(uint slot_0, float fx_0, float fy_0, float fz_0, KernelContext_0 thread* kernelContext_9)
{
    int x0_0 = clamp(int(floor(fx_0)), int(0), int(3));
    int y0_0 = clamp(int(floor(fy_0)), int(0), int(3));
    int z0_0 = clamp(int(floor(fz_0)), int(0), int(3));



    float tx_0 = clamp(fx_0 - float(x0_0), 0.0, 1.0);
    float ty_0 = clamp(fy_0 - float(y0_0), 0.0, 1.0);

    uint base_0 = slot_0 * 125U;

    uint _S35 = uint(z0_0) * 5U;

#line 141
    uint _S36 = uint(y0_0);

#line 141
    uint _S37 = base_0 + (_S35 + _S36) * 5U;

#line 141
    uint _S38 = uint(x0_0);
    uint _S39 = uint(x0_0 + int(1));
    uint _S40 = uint(y0_0 + int(1));

#line 143
    uint _S41 = base_0 + (_S35 + _S40) * 5U;

    uint _S42 = uint(z0_0 + int(1)) * 5U;

#line 145
    uint _S43 = base_0 + (_S42 + _S36) * 5U;

    uint _S44 = base_0 + (_S42 + _S40) * 5U;

#line 153
    return giLerp_0(giLerp_0(giLerp_0(kernelContext_9->giScene_0->bricks_0[_S37 + _S38], kernelContext_9->giScene_0->bricks_0[_S37 + _S39], tx_0), giLerp_0(kernelContext_9->giScene_0->bricks_0[_S41 + _S38], kernelContext_9->giScene_0->bricks_0[_S41 + _S39], tx_0), ty_0), giLerp_0(giLerp_0(kernelContext_9->giScene_0->bricks_0[_S43 + _S38], kernelContext_9->giScene_0->bricks_0[_S43 + _S39], tx_0), giLerp_0(kernelContext_9->giScene_0->bricks_0[_S44 + _S38], kernelContext_9->giScene_0->bricks_0[_S44 + _S39], tx_0), ty_0), clamp(fz_0 - float(z0_0), 0.0, 1.0));
}


#line 175
float giSampleLevel_0(uint level_0, float3 p_0, float thread* farDistance_0, KernelContext_0 thread* kernelContext_10)
{

#line 176
    uint _S45 = 16U + level_0 * 2U;

#line 176
    float4 shape_0 = (as_type<float4>((kernelContext_10->giScene_0->constants_1[_S45])));
    int4 origin_2 = (as_type<int4>((kernelContext_10->giScene_0->constants_1[_S45 + 1U])));
    float voxel_0 = shape_0.x;
    float brick_0 = shape_0.y;
    *farDistance_0 = shape_0.z;
    int bx_0 = int(floor(p_0.x / brick_0));
    int by_0 = int(floor(p_0.y / brick_0));
    int bz_0 = int(floor(p_0.z / brick_0));

#line 183
    uint _S46 = giWindowBricks_0(kernelContext_10);
    int dim_0 = int(_S46);
    int _S47 = origin_2.x;

#line 185
    bool _S48;

#line 185
    if(bx_0 < _S47)
    {

#line 185
        _S48 = true;

#line 185
    }
    else
    {

#line 185
        _S48 = by_0 < (origin_2.y);

#line 185
    }

#line 185
    if(_S48)
    {

#line 185
        _S48 = true;

#line 185
    }
    else
    {

#line 185
        _S48 = bz_0 < (origin_2.z);

#line 185
    }

#line 185
    if(_S48)
    {

#line 185
        _S48 = true;

#line 185
    }
    else
    {

#line 185
        _S48 = bx_0 >= (_S47 + dim_0);

#line 185
    }

#line 185
    if(_S48)
    {

#line 185
        _S48 = true;

#line 185
    }
    else
    {

#line 185
        _S48 = by_0 >= (origin_2.y + dim_0);

#line 185
    }
    if(_S48)
    {

#line 186
        _S48 = true;

#line 186
    }
    else
    {

#line 186
        _S48 = bz_0 >= (origin_2.z + dim_0);

#line 186
    }

#line 185
    if(_S48)
    {
        return *farDistance_0;
    }
    int _S49 = giWrap_0(bz_0, dim_0);

#line 189
    uint _S50 = uint(dim_0);

#line 189
    uint _S51 = uint(_S49) * _S50;

#line 189
    int _S52 = giWrap_0(by_0, dim_0);

#line 189
    uint _S53 = (_S51 + uint(_S52)) * _S50;
    int _S54 = giWrap_0(bx_0, dim_0);
    uint slot_1 = kernelContext_10->giScene_0->pageTable_0[level_0 * uint(dim_0 * dim_0 * dim_0) + (_S53 + uint(_S54))];
    if(slot_1 == 4294967295U)
    {

#line 193
        return *farDistance_0;
    }

    float3 local_1 = (p_0 - float3(float(bx_0) * brick_0, float(by_0) * brick_0, float(bz_0) * brick_0)) / float3(voxel_0) ;

#line 196
    float _S55 = giTrilinear_0(slot_1, local_1.x, local_1.y, local_1.z, kernelContext_10);
    return _S55;
}


float giDistance_0(float3 p_1, KernelContext_0 thread* kernelContext_11)
{

#line 201
    uint _S56 = giLevelCount_0(kernelContext_11);

#line 201
    float answer_0 = 1.0e+06;

#line 201
    uint level_1 = 0U;


    for(;;)
    {

#line 204
        if(level_1 < _S56)
        {
        }
        else
        {

#line 204
            break;
        }

#line 205
        thread float farDistance_1 = 0.0;

#line 205
        float _S57 = giSampleLevel_0(level_1, p_1, &farDistance_1, kernelContext_11);

        float _S58 = min(answer_0, _S57);
        if(_S57 < farDistance_1)
        {

#line 209
            return _S57;
        }

#line 204
        uint level_2 = level_1 + 1U;

#line 204
        answer_0 = _S58;

#line 204
        level_1 = level_2;

#line 204
    }

#line 212
    return answer_0;
}


#line 443 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
float3 spProbeOrigin_0(const SpProbe_0 thread* probe_0, float lift_0, KernelContext_0 thread* kernelContext_12)
{

#line 443
    uint _S59 = giLevelCount_0(kernelContext_12);
    if(_S59 == 0U)
    {

#line 445
        return probe_0->position_2 + probe_0->normal_4 * float3(lift_0) ;
    }

#line 445
    float _S60 = giFinestVoxel_0(kernelContext_12);

#line 445
    float clearance_0;


    if((GiHardwareTier_source_0()) == 7U)
    {

#line 448
        clearance_0 = lift_0;

#line 448
    }
    else
    {

#line 448
        clearance_0 = max(lift_0, _S60);

#line 448
    }

#line 448
    float3 _S61 = probe_0->normal_4;
    float3 origin_3 = probe_0->position_2 + probe_0->normal_4 * float3(clearance_0) ;
    if(_S60 <= 0.0)
    {

#line 451
        return origin_3;
    }

#line 451
    float3 origin_4 = origin_3;

#line 451
    uint step_0 = 0U;

    for(;;)
    {

#line 453
        if(step_0 < 4U)
        {
        }
        else
        {

#line 453
            break;
        }

#line 453
        float _S62 = giDistance_0(origin_4, kernelContext_12);

        if(_S62 >= clearance_0)
        {

#line 456
            break;
        }

        float3 _S63 = float3(_S60, 0.0, 0.0);

#line 459
        float _S64 = giDistance_0(origin_4 + _S63, kernelContext_12);

#line 459
        float _S65 = giDistance_0(origin_4 - _S63, kernelContext_12);

#line 459
        float _S66 = _S64 - _S65;
        float3 _S67 = float3(0.0, _S60, 0.0);

#line 460
        float _S68 = giDistance_0(origin_4 + _S67, kernelContext_12);

#line 460
        float _S69 = giDistance_0(origin_4 - _S67, kernelContext_12);

#line 460
        float _S70 = _S68 - _S69;
        float3 _S71 = float3(0.0, 0.0, _S60);

#line 461
        float _S72 = giDistance_0(origin_4 + _S71, kernelContext_12);

#line 461
        float _S73 = giDistance_0(origin_4 - _S71, kernelContext_12);
        float3 _S74 = origin_4 + giNormalisedOr_0(float3(_S66, _S70, _S72 - _S73), _S61) * float3((clearance_0 - _S62)) ;

#line 453
        uint step_1 = step_0 + 1U;

#line 453
        origin_4 = _S74;

#line 453
        step_0 = step_1;

#line 453
    }

#line 464
    return origin_4;
}


#line 60
uint spSlot_0(KernelContext_0 thread* kernelContext_13)
{

#line 60
    return kernelContext_13->giResolve_0->constants_2[0U].w;
}


#line 84
uint spCounter_0(uint slot_2)
{

#line 85
    return slot_2;
}


#line 57
uint spUniformCount_0(KernelContext_0 thread* kernelContext_14)
{

#line 57
    return kernelContext_14->giResolve_0->constants_2[43U].w;
}


#line 58
uint spCapacity_0(KernelContext_0 thread* kernelContext_15)
{

#line 58
    return kernelContext_15->giResolve_0->constants_2[42U].z;
}


#line 57
uint spUniformCount_1(KernelContext_0 thread* kernelContext_16)
{

#line 57
    return kernelContext_16->giResolve_0->constants_2[43U].w;
}


#line 58
uint spCapacity_1(KernelContext_0 thread* kernelContext_17)
{

#line 58
    return kernelContext_17->giResolve_0->constants_2[42U].z;
}


#line 59
uint spRecords_0(KernelContext_0 thread* kernelContext_18)
{

#line 59
    uint _S75 = spUniformCount_1(kernelContext_18);

#line 59
    uint _S76 = spCapacity_1(kernelContext_18);

#line 59
    return _S75 + _S76;
}


#line 76
uint spRecordBase_0(uint slot_3, uint record_1, KernelContext_0 thread* kernelContext_19)
{

#line 76
    uint _S77 = spRecords_0(kernelContext_19);
    return (slot_3 * _S77 + record_1) * 11U;
}


#line 214
SpProbe_0 spLoadProbe_0(uint slot_4, uint record_2, KernelContext_0 thread* kernelContext_20)
{

#line 214
    uint _S78 = spRecordBase_0(slot_4, record_2, kernelContext_20);

    float4 device* _S79 = kernelContext_20->giResolve_0->probes_1+_S78;
    float4 device* _S80 = kernelContext_20->giResolve_0->probes_1+(_S78 + 1U);
    thread SpProbe_0 probe_1;
    (&probe_1)->position_2 = (*_S79).xyz;
    (&probe_1)->depth_0 = (*_S79).w;
    (&probe_1)->normal_4 = (*_S80).xyz;
    (&probe_1)->id_0 = (as_type<uint>(((*_S80).w)));
    return probe_1;
}


#line 440 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
int gpFloorDiv_0(float value_3, float divisor_0)
{

#line 441
    return int(floor(value_3 / divisor_0));
}


#line 487
uint gpLayoutProbe_0(uint level_3, int3 cell_2, KernelContext_0 thread* kernelContext_21)
{

#line 488
    int3 centre_0 = (as_type<int3>((kernelContext_21->giProbes_0->constants_0[6U + level_3].xyz)));
    int half_0 = int(kernelContext_21->giProbes_0->constants_0[1U].w);
    int3 _S81 = int3(half_0, half_0, half_0);

#line 490
    bool _S82;

#line 490
    if(any(cell_2 < (centre_0 - _S81)))
    {

#line 490
        _S82 = true;

#line 490
    }
    else
    {

#line 490
        _S82 = any(cell_2 > (centre_0 + _S81));

#line 490
    }

#line 490
    if(_S82)
    {

#line 491
        return 4294967295U;
    }
    int edge_0 = int(kernelContext_21->giProbes_0->constants_0[5U].z);
    uint cells_0 = uint(edge_0 * edge_0 * edge_0);
    int _S83 = giWrap_0(cell_2.z, edge_0);

#line 495
    uint _S84 = uint(edge_0);

#line 495
    uint _S85 = uint(_S83) * _S84;

#line 495
    int _S86 = giWrap_0(cell_2.y, edge_0);

#line 495
    uint _S87 = (_S85 + uint(_S86)) * _S84;

    int _S88 = giWrap_0(cell_2.x, edge_0);
    return kernelContext_21->giProbes_0->layout_0[level_3 * cells_0 + (_S87 + uint(_S88))];
}


#line 114
constant array<float3, int(6)> kGpAxes_0 = { float3(1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, -1.0, 0.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, -1.0) };

#line 445
float gpVisibilityWeight_0(uint probe_2, float3 position_3, float3 query_1, KernelContext_0 thread* kernelContext_22)
{

#line 446
    float3 offset_1 = query_1 - position_3;
    float span_0 = length(offset_1);
    if(span_0 <= 0.00009999999747379)
    {

#line 449
        return 1.0;
    }
    float3 _S89 = offset_1 / float3(span_0) ;
    uint _S90 = probe_2 * 4U;

#line 452
    float4 device* _S91 = kernelContext_22->giProbes_0->probes_0+(_S90 + 2U);
    float4 device* _S92 = kernelContext_22->giProbes_0->probes_0+(_S90 + 3U);
    array<float, int(6)> _S93 = { (*_S91).x, (*_S91).y, (*_S91).z, (*_S91).w, (*_S92).x, (*_S92).y };

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
        float _S94 = max(0.0, dot(kGpAxes_0[axis_0], _S89));
        float expected_1 = expected_0 + _S93[axis_0] * _S94;
        float weight_1 = weight_0 + _S94;

#line 457
        axis_0 = axis_0 + 1U;

#line 457
)cy_msl",
    R"cy_msl(        expected_0 = expected_1;

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



    float slack_0 = (as_type<float>((kernelContext_22->giProbes_0->constants_0[1U].y)));
    float _S95 = expected_0 / weight_0 + slack_0;

#line 469
    if(span_0 > _S95)
    {

#line 470
        return 0.0;
    }
    float _S96 = max(slack_0, 0.00009999999747379);

#line 472
    float visible_0 = gpClamp_0((_S95 - span_0) / _S96, 0.0, 1.0);

#line 472
    axis_0 = 0U;

    for(;;)
    {

#line 474
        if(axis_0 < 6U)
        {
        }
        else
        {

#line 474
            break;
        }

#line 475
        float cosine_0 = dot(kGpAxes_0[axis_0], _S89);
        if(cosine_0 < 0.5)
        {

#line 477
            axis_0 = axis_0 + 1U;

#line 474
            continue;
        }

#line 474
        visible_0 = min(visible_0, gpClamp_0((_S93[axis_0] + slack_0 - span_0 * cosine_0) / _S96, 0.0, 1.0));

#line 474
        axis_0 = axis_0 + 1U;

#line 474
    }

#line 483
    return visible_0;
}


#line 300 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float grFacing_0(float3 toward_0, float3 normal_5)
{

#line 301
    return gpClamp_0((dot(normal_5, toward_0) + 1.0) * 0.5, 0.0, 1.0);
}


#line 123 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float giFinestVoxel_1(KernelContext_0 thread* kernelContext_23)
{

#line 124
    return (as_type<float>((kernelContext_23->giScene_0->constants_1[9U].y)));
}


#line 201
float giDistance_1(float3 p_2, KernelContext_0 thread* kernelContext_24)
{

#line 201
    uint _S97 = giLevelCount_0(kernelContext_24);

#line 201
    float answer_1 = 1.0e+06;

#line 201
    uint level_4 = 0U;


    for(;;)
    {

#line 204
        if(level_4 < _S97)
        {
        }
        else
        {

#line 204
            break;
        }

#line 205
        thread float farDistance_2 = 0.0;

#line 205
        float _S98 = giSampleLevel_0(level_4, p_2, &farDistance_2, kernelContext_24);

        float _S99 = min(answer_1, _S98);
        if(_S98 < farDistance_2)
        {

#line 209
            return _S98;
        }

#line 204
        uint level_5 = level_4 + 1U;

#line 204
        answer_1 = _S99;

#line 204
        level_4 = level_5;

#line 204
    }

#line 212
    return answer_1;
}

struct GiTraceHit_0
{
    bool hit_3;
    bool exhausted_0;
    float t_3;
    float closest_0;
    float3 position_4;
    float3 normal_6;
};

GiTraceHit_0 giSphereTrace_0(float3 origin_5, float3 direction_3, float maxDistance_1, float tMin_0, KernelContext_0 thread* kernelContext_25)
{

#line 226
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_3 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_3 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_4 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_6 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S100 = giLevelCount_0(kernelContext_25);
    if(_S100 == 0U)
    {

#line 234
        return result_0;
    }

#line 234
    float _S101 = giFinestVoxel_0(kernelContext_25);


    float surface_0 = _S101 * 0.5;
    float _S102 = max(tMin_0, surface_0);
    thread array<float, int(3)> recent_0;

#line 239
    recent_0[int(0)] = 1.0e+06;

#line 239
    recent_0[int(1)] = 1.0e+06;

#line 239
    recent_0[int(2)] = 1.0e+06;

#line 239
    float closest_1 = 1.0e+06;

#line 239
    uint recentSlot_0 = 0U;

#line 239
    uint step_2 = 0U;

#line 239
    float t_4 = _S102;


    for(;;)
    {

#line 242
        if(step_2 < 192U)
        {
        }
        else
        {

#line 242
            break;
        }

#line 243
        if(t_4 > maxDistance_1)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_5 = origin_5 + direction_3 * float3(t_4) ;

#line 248
        float _S103 = giDistance_0(position_5, kernelContext_25);

        if(_S103 < surface_0)
        {

#line 251
            (&result_0)->hit_3 = true;
            (&result_0)->t_3 = t_4;
            (&result_0)->position_4 = position_5;


            float3 _S104 = float3(_S101, 0.0, 0.0);

#line 256
            float _S105 = giDistance_0(position_5 + _S104, kernelContext_25);

#line 256
            float _S106 = giDistance_0(position_5 - _S104, kernelContext_25);

#line 256
            float _S107 = _S105 - _S106;

            float3 _S108 = float3(0.0, _S101, 0.0);

#line 258
            float _S109 = giDistance_0(position_5 + _S108, kernelContext_25);

#line 258
            float _S110 = giDistance_0(position_5 - _S108, kernelContext_25);

#line 258
            float _S111 = _S109 - _S110;

            float3 _S112 = float3(0.0, 0.0, _S101);

#line 260
            float _S113 = giDistance_0(position_5 + _S112, kernelContext_25);

#line 260
            float _S114 = giDistance_0(position_5 - _S112, kernelContext_25);

            (&result_0)->normal_6 = giNormalisedOr_0(float3(_S107, _S111, _S113 - _S114), - direction_3);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S115 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S103;
        uint _S116 = (recentSlot_0 + 1U) % 3U;
        float t_5 = t_4 + max(_S103, surface_0);

#line 242
        uint step_3 = step_2 + 1U;

#line 242
        closest_1 = _S115;

#line 242
        recentSlot_0 = _S116;

#line 242
        step_2 = step_3;

#line 242
        t_4 = t_5;

#line 242
    }

#line 271
    (&result_0)->exhausted_0 = true;

    (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
    return result_0;
}


#line 286 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
bool grProbeOccluded_0(float3 from_0, float3 to_0, KernelContext_0 thread* kernelContext_26)
{

#line 287
    float3 offset_2 = to_0 - from_0;
    float span_1 = length(offset_2);

#line 288
    float _S117 = giFinestVoxel_1(kernelContext_26);

    if(span_1 <= (_S117 * 2.0))
    {

#line 291
        return false;
    }

#line 291
    float _S118 = giDistance_1(from_0, kernelContext_26);

#line 291
    float _S119 = giDistance_1(to_0, kernelContext_26);

    if((_S118 + _S119) >= span_1)
    {

#line 294
        return false;
    }

#line 294
    GiTraceHit_0 _S120 = giSphereTrace_0(from_0, offset_2 / float3(span_1) , span_1 - _S117, 0.0, kernelContext_26);

    return _S120.hit_3;
}


#line 273 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
void gpLoadPayload_0(uint probe_3, uint floats_0, array<float, int(48)> thread* payload_0, KernelContext_0 thread* kernelContext_27)
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
        float _S121;
        if(index_0 < floats_0)
        {

#line 275
            _S121 = *(kernelContext_27->giProbes_0->payloads_0+(probe_3 * floats_0 + index_0));

#line 275
        }
        else
        {

#line 275
            _S121 = 0.0;

#line 275
        }

#line 275
        (*payload_0)[index_0] = _S121;

#line 274
        index_0 = index_0 + 1U;

#line 274
    }


    return;
}


#line 110
float3 gpMax0_0(float3 v_1)
{

#line 111
    return float3(max(0.0, v_1.x), max(0.0, v_1.y), max(0.0, v_1.z));
}


#line 154
void gpOctahedralUv_0(float3 direction_4, float thread* u_0, float thread* v_2)
{

#line 155
    float sum_0 = abs(direction_4.x) + abs(direction_4.y) + abs(direction_4.z);

#line 155
    float3 p_3;
    if(sum_0 > 0.0)
    {

#line 156
        p_3 = direction_4 / float3(sum_0) ;

#line 156
    }
    else
    {

#line 156
        p_3 = float3(0.0, 0.0, 1.0);

#line 156
    }
    float _S122 = p_3.x;
    float _S123 = p_3.y;

#line 158
    float px_0;

#line 158
    float py_0;
    if((p_3.z) < 0.0)
    {

#line 160
        float _S124 = 1.0 - abs(_S123);

#line 160
        if(_S122 >= 0.0)
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
        float fx_1 = _S124 * px_0;
        float _S125 = 1.0 - abs(_S122);

#line 161
        if(_S123 >= 0.0)
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
        float fy_1 = _S125 * px_0;

#line 161
        px_0 = fx_1;

#line 161
        py_0 = fy_1;

#line 159
    }
    else
    {

#line 159
        px_0 = _S122;

#line 159
        py_0 = _S123;

#line 159
    }

#line 165
    *u_0 = gpClamp_0(px_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    *v_2 = gpClamp_0(py_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    return;
}


#line 220
float3 gpDecode_0(uint encoding_0, const array<float, int(48)> thread* payload_1, float3 direction_5)
{

#line 220
    uint c_0;
    if(encoding_0 == 0U)
    {



        array<float, int(4)> _S126 = { 0.28209498524665833, 0.32573533058166504 * direction_5.y, 0.32573533058166504 * direction_5.z, 0.32573533058166504 * direction_5.x };

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
            uint _S127 = c_0 * 3U;

#line 230
            result_1.x = result_1.x + (*payload_1)[_S127] * _S126[c_0];
            result_1.y = result_1.y + (*payload_1)[_S127 + 1U] * _S126[c_0];
            result_1.z = result_1.z + (*payload_1)[_S127 + 2U] * _S126[c_0];

#line 229
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
        gpOctahedralUv_0(direction_5, &u_1, &v_3);
        uint x0_1 = uint(u_1);
        uint y0_1 = uint(v_3);
        uint _S128 = min(x0_1 + 1U, 3U);



        uint _S129 = y0_1 * 4U;

        uint _S130 = min(y0_1 + 1U, 3U) * 4U;

        uint _S131 = (_S129 + x0_1) * 3U;

#line 250
        float3 f00_0 = float3((*payload_1)[_S131], (*payload_1)[_S131 + 1U], (*payload_1)[_S131 + 2U]);
        uint _S132 = (_S129 + _S128) * 3U;
        uint _S133 = (_S130 + x0_1) * 3U;

#line 252
        float3 f01_0 = float3((*payload_1)[_S133], (*payload_1)[_S133 + 1U], (*payload_1)[_S133 + 2U]);
        uint _S134 = (_S130 + _S128) * 3U;

#line 253
        float3 _S135 = float3((u_1 - float(x0_1))) ;
        float3 top_0 = f00_0 + (float3((*payload_1)[_S132], (*payload_1)[_S132 + 1U], (*payload_1)[_S132 + 2U]) - f00_0) * _S135;

        return top_0 + (f01_0 + (float3((*payload_1)[_S134], (*payload_1)[_S134 + 1U], (*payload_1)[_S134 + 2U]) - f01_0) * _S135 - top_0) * float3((v_3 - float(y0_1))) ;
    }
    float3 _S136 = float3(0.0, 0.0, 0.0);

#line 258
    float3 result_2 = _S136;

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
        float _S137 = max(0.0, dot(kGpAxes_0[c_0], direction_5));
        if(_S137 <= 0.0)
        {

#line 263
            c_0 = c_0 + 1U;

#line 260
            continue;
        }



        uint _S138 = c_0 * 3U;

        float total_1 = total_0 + _S137;

#line 267
        result_2 = result_2 + float3((*payload_1)[_S138], (*payload_1)[_S138 + 1U], (*payload_1)[_S138 + 2U]) * float3(_S137) ;

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
        result_2 = _S136;

#line 269
    }

#line 269
    return result_2;
}


#line 305 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float4 grFinish_0(float3 total_2, float weight_2, float oldest_0)
{

#line 306
    if(weight_2 <= 0.0)
    {

#line 307
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    return float4(total_2 / float3(weight_2) , gpClamp_0(1.0 - gpClamp_0(oldest_0 / 240.0, 0.0, 1.0) * 0.75, 0.0, 1.0));
}


#line 317
void grSampleTraced_0(float3 position_6, float3 normal_7, float3 reflection_0, uint frame_0, float4 thread* diffuse_0, float4 thread* specular_0, KernelContext_0 thread* kernelContext_28)
{
    uint4 counts_0 = kernelContext_28->giProbes_0->constants_0[0U];
    uint _S139 = counts_0.x;
    uint _S140 = counts_0.y;
    uint _S141 = kernelContext_28->giProbes_0->constants_0[1U].z;
    float _S142 = (as_type<float>((kernelContext_28->giProbes_0->constants_0[5U].x)));
    float3 _S143 = position_6 + normal_7 * float3((as_type<float>((kernelContext_28->giProbes_0->constants_0[1U].y)))) ;
    float3 _S144 = float3(0.0, 0.0, 0.0);

#line 325
    float3 diffuseTotal_0 = _S144;

#line 325
    float diffuseOldest_0 = 0.0;

#line 325
    float3 specularTotal_0 = _S144;

#line 325
    float specularOldest_0 = 0.0;

#line 325
    uint level_6 = 0U;

#line 325
    float diffuseWeight_0 = 0.0;

#line 325
    float specularWeight_0 = 0.0;

#line 331
    for(;;)
    {

#line 331
        if(level_6 < _S141)
        {
        }
        else
        {

#line 331
            break;
        }

#line 332
        float spacing_0 = _S142 * float(1U << level_6);
        float _S145 = position_6.x;

#line 333
        int _S146 = gpFloorDiv_0(_S145 - spacing_0, spacing_0);
        float _S147 = position_6.y;

#line 334
        int _S148 = gpFloorDiv_0(_S147 - spacing_0, spacing_0);
        float _S149 = position_6.z;
        int _S150 = gpFloorDiv_0(_S145 + spacing_0, spacing_0);
        int _S151 = gpFloorDiv_0(_S147 + spacing_0, spacing_0);
        int _S152 = gpFloorDiv_0(_S149 + spacing_0, spacing_0);

#line 338
        int z_2 = gpFloorDiv_0(_S149 - spacing_0, spacing_0);
        for(;;)
        {

#line 339
            if(z_2 <= _S152)
            {
            }
            else
            {

#line 339
                break;
            }

#line 339
            float3 diffuseTotal_1 = diffuseTotal_0;

#line 339
            float diffuseOldest_1 = diffuseOldest_0;

#line 339
            float3 specularTotal_1 = specularTotal_0;

#line 339
            float specularOldest_1 = specularOldest_0;

#line 339
            int y_3 = _S148;

#line 339
            float diffuseWeight_1 = diffuseWeight_0;

#line 339
            float specularWeight_1 = specularWeight_0;
            for(;;)
            {

#line 340
                if(y_3 <= _S151)
                {
                }
                else
                {

#line 340
                    break;
                }

#line 340
                float3 diffuseTotal_2 = diffuseTotal_1;

#line 340
                float diffuseOldest_2 = diffuseOldest_1;

#line 340
                float3 specularTotal_2 = specularTotal_1;

#line 340
                float specularOldest_2 = specularOldest_1;

#line 340
                int x_3 = _S146;

#line 340
                float diffuseWeight_2 = diffuseWeight_1;

#line 340
                float specularWeight_2 = specularWeight_1;
                for(;;)
                {

#line 341
                    if(x_3 <= _S150)
                    {
                    }
                    else
                    {

#line 341
                        break;
                    }

#line 341
                    uint _S153 = gpLayoutProbe_0(level_6, int3(x_3, y_3, z_2), kernelContext_28);

                    if(_S153 == 4294967295U)
                    {

#line 344
                        x_3 = x_3 + int(1);

#line 341
                        continue;
                    }



                    uint _S154 = _S153 * 4U;

#line 346
                    float4 device* _S155 = kernelContext_28->giProbes_0->probes_0+(_S154 + 1U);

#line 346
                    float4 record_3 = *_S155;

#line 346
)cy_msl",
    R"cy_msl(                    bool _S156;
                    if(((*_S155).y) == 0.0)
                    {

#line 347
                        _S156 = true;

#line 347
                    }
                    else
                    {

#line 347
                        _S156 = (record_3.z) == 0.0;

#line 347
                    }

#line 347
                    if(_S156)
                    {

#line 348
                        x_3 = x_3 + int(1);

#line 341
                        continue;
                    }

#line 350
                    float3 at_0 = (*(kernelContext_28->giProbes_0->probes_0+_S154)).xyz;
                    float3 offset_3 = position_6 - at_0;
                    float span_2 = length(offset_3);
                    if(span_2 > spacing_0)
                    {

#line 354
                        x_3 = x_3 + int(1);

#line 341
                        continue;
                    }

#line 356
                    float _S157 = 1.0 - gpClamp_0(span_2 / spacing_0, 0.0, 1.0);

#line 356
                    float _S158 = gpVisibilityWeight_0(_S153, at_0, position_6, kernelContext_28);

#line 356
                    float falloff_0 = _S157 * _S158;

                    bool apart_0 = span_2 > 0.00009999999747379;

#line 358
                    float3 toward_1;
                    if(apart_0)
                    {

#line 359
                        toward_1 = - offset_3 / float3(span_2) ;

#line 359
                    }
                    else
                    {

#line 359
                        toward_1 = normal_7;

#line 359
                    }

#line 359
                    float diffuseShare_0;
                    if(apart_0)
                    {

#line 360
                        diffuseShare_0 = falloff_0 * grFacing_0(toward_1, normal_7);

#line 360
                    }
                    else
                    {

#line 360
                        diffuseShare_0 = falloff_0;

#line 360
                    }

#line 360
                    float specularShare_0;

                    if(apart_0)
                    {

#line 362
                        specularShare_0 = falloff_0 * grFacing_0(toward_1, reflection_0);

#line 362
                    }
                    else
                    {

#line 362
                        specularShare_0 = falloff_0;

#line 362
                    }

#line 362
                    bool _S159;
                    if(diffuseShare_0 <= 0.0)
                    {

#line 363
                        _S159 = specularShare_0 <= 0.0;

#line 363
                    }
                    else
                    {

#line 363
                        _S159 = false;

#line 363
                    }

#line 363
                    bool _S160;

#line 363
                    if(_S159)
                    {

#line 363
                        _S160 = true;

#line 363
                    }
                    else
                    {

#line 363
                        bool _S161 = grProbeOccluded_0(_S143, at_0, kernelContext_28);

#line 363
                        _S160 = _S161;

#line 363
                    }

#line 363
                    if(_S160)
                    {
                        x_3 = x_3 + int(1);

#line 341
                        continue;
                    }

#line 367
                    thread array<float, int(48)> data_0;

#line 367
                    gpLoadPayload_0(_S153, _S140, &data_0, kernelContext_28);

                    uint last_1 = (as_type<uint>((record_3.x)));

#line 369
                    float age_1;
                    if(frame_0 > last_1)
                    {

#line 370
                        age_1 = float(frame_0 - last_1);

#line 370
                    }
                    else
                    {

#line 370
                        age_1 = 0.0;

#line 370
                    }

#line 370
                    float3 diffuseTotal_3;

#line 370
                    float diffuseOldest_3;

#line 370
                    float diffuseWeight_3;
                    if(diffuseShare_0 > 0.0)
                    {

#line 371
                        thread array<float, int(48)> _S162 = data_0;

#line 371
                        float3 _S163 = gpDecode_0(_S139, &_S162, normal_7);

                        float diffuseWeight_4 = diffuseWeight_2 + diffuseShare_0;
                        float _S164 = max(diffuseOldest_2, age_1);

#line 374
                        diffuseTotal_3 = diffuseTotal_2 + _S163 * float3(diffuseShare_0) ;

#line 374
                        diffuseOldest_3 = _S164;

#line 374
                        diffuseWeight_3 = diffuseWeight_4;

#line 371
                    }
                    else
                    {

#line 371
                        diffuseTotal_3 = diffuseTotal_2;

#line 371
                        diffuseOldest_3 = diffuseOldest_2;

#line 371
                        diffuseWeight_3 = diffuseWeight_2;

#line 371
                    }

#line 371
                    float3 specularTotal_3;

#line 371
                    float specularOldest_3;

#line 371
                    float specularWeight_3;

#line 376
                    if(specularShare_0 > 0.0)
                    {

#line 376
                        thread array<float, int(48)> _S165 = data_0;

#line 376
                        float3 _S166 = gpDecode_0(_S139, &_S165, reflection_0);


                        float specularWeight_4 = specularWeight_2 + specularShare_0;
                        float _S167 = max(specularOldest_2, age_1);

#line 380
                        specularTotal_3 = specularTotal_2 + _S166 * float3(specularShare_0) ;

#line 380
                        specularOldest_3 = _S167;

#line 380
                        specularWeight_3 = specularWeight_4;

#line 376
                    }
                    else
                    {

#line 376
                        specularTotal_3 = specularTotal_2;

#line 376
                        specularOldest_3 = specularOldest_2;

#line 376
                        specularWeight_3 = specularWeight_2;

#line 376
                    }

#line 376
                    diffuseTotal_2 = diffuseTotal_3;

#line 376
                    diffuseOldest_2 = diffuseOldest_3;

#line 376
                    specularTotal_2 = specularTotal_3;

#line 376
                    specularOldest_2 = specularOldest_3;

#line 376
                    diffuseWeight_2 = diffuseWeight_3;

#line 376
                    specularWeight_2 = specularWeight_3;

#line 341
                    x_3 = x_3 + int(1);

#line 341
                }

#line 340
                int y_4 = y_3 + int(1);

#line 340
                diffuseTotal_1 = diffuseTotal_2;

#line 340
                diffuseOldest_1 = diffuseOldest_2;

#line 340
                specularTotal_1 = specularTotal_2;

#line 340
                specularOldest_1 = specularOldest_2;

#line 340
                y_3 = y_4;

#line 340
                diffuseWeight_1 = diffuseWeight_2;

#line 340
                specularWeight_1 = specularWeight_2;

#line 340
            }

#line 339
            int z_3 = z_2 + int(1);

#line 339
            diffuseTotal_0 = diffuseTotal_1;

#line 339
            diffuseOldest_0 = diffuseOldest_1;

#line 339
            specularTotal_0 = specularTotal_1;

#line 339
            specularOldest_0 = specularOldest_1;

#line 339
            z_2 = z_3;

#line 339
            diffuseWeight_0 = diffuseWeight_1;

#line 339
            specularWeight_0 = specularWeight_1;

#line 339
        }

#line 331
        level_6 = level_6 + 1U;

#line 331
    }

#line 386
    *diffuse_0 = grFinish_0(diffuseTotal_0, diffuseWeight_0, diffuseOldest_0);
    *specular_0 = grFinish_0(specularTotal_0, specularWeight_0, specularOldest_0);
    return;
}


#line 375 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
bool spInvalidated_0(float3 point_0, KernelContext_0 thread* kernelContext_29)
{

#line 376
    uint _S168 = min(kernelContext_29->giResolve_0->constants_2[26U].x, 4U);

#line 376
    uint region_0 = 0U;
    for(;;)
    {

#line 377
        if(region_0 < _S168)
        {
        }
        else
        {

#line 377
            break;
        }

#line 378
        uint _S169 = region_0 * 2U;

#line 378
        float3 lo_1 = (as_type<float3>((kernelContext_29->giResolve_0->constants_2[27U + _S169].xyz)));
        float3 hi_1 = (as_type<float3>((kernelContext_29->giResolve_0->constants_2[28U + _S169].xyz)));
        float _S170 = point_0.x;

#line 380
        bool _S171;

#line 380
        if(_S170 >= (lo_1.x))
        {

#line 380
            _S171 = (point_0.y) >= (lo_1.y);

#line 380
        }
        else
        {

#line 380
            _S171 = false;

#line 380
        }

#line 380
        bool _S172;

#line 380
        if(_S171)
        {

#line 380
            _S172 = (point_0.z) >= (lo_1.z);

#line 380
        }
        else
        {

#line 380
            _S172 = false;

#line 380
        }

#line 380
        bool _S173;

#line 380
        if(_S172)
        {

#line 380
            _S173 = _S170 <= (hi_1.x);

#line 380
        }
        else
        {

#line 380
            _S173 = false;

#line 380
        }

#line 380
        bool _S174;

#line 380
        if(_S173)
        {

#line 380
            _S174 = (point_0.y) <= (hi_1.y);

#line 380
        }
        else
        {

#line 380
            _S174 = false;

#line 380
        }

#line 380
        bool _S175;
        if(_S174)
        {

#line 381
            _S175 = (point_0.z) <= (hi_1.z);

#line 381
        }
        else
        {

#line 381
            _S175 = false;

#line 381
        }

#line 380
        if(_S175)
        {
            return true;
        }

#line 377
        region_0 = region_0 + 1U;

#line 377
    }

#line 385
    return false;
}


#line 49
uint spWidth_0(KernelContext_0 thread* kernelContext_30)
{

#line 49
    return kernelContext_30->giResolve_0->constants_2[40U].x;
}


#line 50
uint spHeight_0(KernelContext_0 thread* kernelContext_31)
{

#line 50
    return kernelContext_31->giResolve_0->constants_2[40U].y;
}


#line 335
bool spLanding_0(float3 position_7, uint thread* x_4, uint thread* y_5, KernelContext_0 thread* kernelContext_32)
{

#line 336
    *x_4 = 0U;
    *y_5 = 0U;

#line 337
    float4 _S176 = float4(position_7.x) ;

#line 337
    float4 _S177 = float4(position_7.y) ;

#line 337
    float4 _S178 = float4(position_7.z) ;

#line 342
    float4 clip_0 = (as_type<float4>((kernelContext_32->giResolve_0->constants_2[6U]))) * _S176 + (as_type<float4>((kernelContext_32->giResolve_0->constants_2[7U]))) * _S177 + (as_type<float4>((kernelContext_32->giResolve_0->constants_2[8U]))) * _S178 + (as_type<float4>((kernelContext_32->giResolve_0->constants_2[9U])));
    float _S179 = clip_0.w;

#line 343
    if(_S179 <= 9.99999993922529029e-09)
    {

#line 344
        return false;
    }
    float inverse_0 = 1.0 / _S179;

    float _S180 = clip_0.x * inverse_0 * 0.5 + 0.5;

#line 348
    float _S181 = clip_0.y * inverse_0 * 0.5 + 0.5;

#line 353
    float4 clip_1 = (as_type<float4>((kernelContext_32->giResolve_0->constants_2[10U]))) * _S176 + (as_type<float4>((kernelContext_32->giResolve_0->constants_2[11U]))) * _S177 + (as_type<float4>((kernelContext_32->giResolve_0->constants_2[12U]))) * _S178 + (as_type<float4>((kernelContext_32->giResolve_0->constants_2[13U])));
    float _S182 = clip_1.w;

#line 354
    if(_S182 <= 9.99999993922529029e-09)
    {

#line 355
        return false;
    }
    float inverse_1 = 1.0 / _S182;



    float _S183 = _S180 + (clip_1.x * inverse_1 * 0.5 + 0.5 - _S180);

#line 361
    float _S184 = _S181 + (clip_1.y * inverse_1 * 0.5 + 0.5 - _S181);

#line 361
    uint _S185 = spWidth_0(kernelContext_32);
    float width_0 = float(_S185);

#line 362
    uint _S186 = spHeight_0(kernelContext_32);
    float height_0 = float(_S186);
    float fx_2 = floor(_S183 * width_0);
    float fy_2 = floor((1.0 - _S184) * height_0);

#line 365
    bool _S187;
    if(fx_2 < 0.0)
    {

#line 366
        _S187 = true;

#line 366
    }
    else
    {

#line 366
        _S187 = fy_2 < 0.0;

#line 366
    }

#line 366
    if(_S187)
    {

#line 366
        _S187 = true;

#line 366
    }
    else
    {

#line 366
        _S187 = fx_2 >= width_0;

#line 366
    }

#line 366
    if(_S187)
    {

#line 366
        _S187 = true;

#line 366
    }
    else
    {

#line 366
        _S187 = fy_2 >= height_0;

#line 366
    }

#line 366
    if(_S187)
    {

#line 367
        return false;
    }
    *x_4 = uint(fx_2);
    *y_5 = uint(fy_2);
    return true;
}


#line 139
struct SpTaps_0
{
    array<uint, int(4)> index_1;
    array<float, int(4)> weight_3;
    uint count_0;
};


#line 51
uint spSpacing_0(KernelContext_0 thread* kernelContext_33)
{

#line 51
    return kernelContext_33->giResolve_0->constants_2[40U].z;
}


#line 53
uint spTilesX_0(KernelContext_0 thread* kernelContext_34)
{

#line 53
    return kernelContext_34->giResolve_0->constants_2[41U].x;
}


#line 54
uint spTilesY_0(KernelContext_0 thread* kernelContext_35)
{

#line 54
    return kernelContext_35->giResolve_0->constants_2[41U].y;
}


#line 145
void spAxisTaps_0(uint coordinate_0, uint step_4, uint count_1, uint thread* base_1, float thread* fraction_0)
{

#line 146
    float g_0 = (float(coordinate_0) - float(step_4 / 2U)) / float(step_4);
    if(g_0 <= 0.0)
    {

#line 148
        *base_1 = 0U;
        *fraction_0 = 0.0;
        return;
    }
    uint _S188 = uint(g_0);

#line 152
    *base_1 = _S188;
    *fraction_0 = g_0 - float(_S188);
    if((*base_1 + 1U) >= count_1)
    {

#line 155
        *base_1 = count_1 - 1U;
        *fraction_0 = 0.0;

#line 154
    }



    return;
}


#line 160
SpTaps_0 spBilinearTaps_0(uint x_5, uint y_6, uint step_5, uint countX_0, uint countY_0)
{

#line 161
    thread SpTaps_0 taps_0;
    (&taps_0)->count_0 = 0U;

#line 162
    uint t_6 = 0U;
    for(;;)
    {

#line 163
        if(t_6 < 4U)
        {
        }
        else
        {

#line 163
            break;
        }

#line 164
        (&taps_0)->index_1[t_6] = 0U;
        (&taps_0)->weight_3[t_6] = 0.0;

#line 163
        t_6 = t_6 + 1U;

#line 163
    }



    thread uint bx_1;

    thread float fx_3;

    spAxisTaps_0(x_5, step_5, countX_0, &bx_1, &fx_3);

#line 168
    thread uint by_1;

    thread float fy_3;

    spAxisTaps_0(y_6, step_5, countY_0, &by_1, &fy_3);

#line 172
    uint j_0 = 0U;
    for(;;)
    {

#line 173
        if(j_0 < 2U)
        {
        }
        else
        {

#line 173
            break;
        }

#line 173
        uint i_0 = 0U;
        for(;;)
        {

#line 174
            if(i_0 < 2U)
            {
            }
            else
            {

#line 174
                break;
            }

#line 174
            float _S189;
            if(i_0 == 0U)
            {

#line 175
                _S189 = 1.0 - fx_3;

#line 175
            }
            else
            {

#line 175
                _S189 = fx_3;

#line 175
            }

#line 175
            float _S190;

#line 175
            if(j_0 == 0U)
            {

#line 175
                _S190 = 1.0 - fy_3;

#line 175
            }
            else
            {

#line 175
                _S190 = fy_3;

#line 175
            }

#line 175
            float weight_4 = _S189 * _S190;
            if(weight_4 <= 0.0)
            {

#line 177
                i_0 = i_0 + 1U;

#line 174
                continue;
            }



            (&taps_0)->index_1[(&taps_0)->count_0] = (by_1 + j_0) * countX_0 + (bx_1 + i_0);
            (&taps_0)->weight_3[(&taps_0)->count_0] = weight_4;
            (&taps_0)->count_0 = (&taps_0)->count_0 + 1U;

#line 174
            i_0 = i_0 + 1U;

#line 174
        }

#line 173
        j_0 = j_0 + 1U;

#line 173
    }

#line 184
    return taps_0;
}


#line 52
uint spCell_0(KernelContext_0 thread* kernelContext_36)
{

#line 52
    return kernelContext_36->giResolve_0->constants_2[40U].w;
}

uint spCellsX_0(KernelContext_0 thread* kernelContext_37)
{

#line 55
)cy_msl",
    R"cy_msl(    return kernelContext_37->giResolve_0->constants_2[41U].z;
}


#line 56
uint spCellsY_0(KernelContext_0 thread* kernelContext_38)
{

#line 56
    return kernelContext_38->giResolve_0->constants_2[41U].w;
}


#line 55
uint spCellsX_1(KernelContext_0 thread* kernelContext_39)
{

#line 55
    return kernelContext_39->giResolve_0->constants_2[41U].z;
}


#line 56
uint spCellsY_1(KernelContext_0 thread* kernelContext_40)
{

#line 56
    return kernelContext_40->giResolve_0->constants_2[41U].w;
}


#line 80
uint spTableIndex_0(uint slot_5, uint cell_3, KernelContext_0 thread* kernelContext_41)
{

#line 80
    uint _S191 = spCellsX_1(kernelContext_41);
    uint _S192 = slot_5 * _S191;

#line 81
    uint _S193 = spCellsY_1(kernelContext_41);

#line 81
    return 4U + _S192 * _S193 + cell_3;
}


#line 119
float spGeometryWeight_0(float3 probePosition_0, float3 probeNormal_0, float3 position_8, float3 normal_8, float depth_1, float tolerance_0)
{
    float plane_0 = abs(dot(probeNormal_0, position_8 - probePosition_0));
    float reach_1 = tolerance_0 * depth_1;
    if(reach_1 <= 0.0)
    {

#line 124
        return 0.0;
    }

    float _S194 = max(0.0, dot(probeNormal_0, normal_8));
    float facing_0 = _S194 * _S194;
    float facing_1 = facing_0 * facing_0;

    return max(0.0, 1.0 - plane_0 / reach_1) * (facing_1 * facing_1);
}


#line 226
void spLoadCoefficients_0(uint slot_6, uint record_4, array<float3, int(9)> thread* coefficients_0, float thread* history_0, KernelContext_0 thread* kernelContext_42)
{

#line 226
    uint _S195 = spRecordBase_0(slot_6, record_4, kernelContext_42);
    uint _S196 = _S195 + 2U;
    thread array<float, int(28)> values_0;

#line 228
    uint v_4 = 0U;
    for(;;)
    {

#line 229
        if(v_4 < 7U)
        {
        }
        else
        {

#line 229
            break;
        }

#line 230
        float4 device* _S197 = kernelContext_42->giResolve_0->probes_1+(_S196 + v_4);
        uint _S198 = v_4 * 4U;

#line 231
        values_0[_S198] = (*_S197).x;
        values_0[_S198 + 1U] = (*_S197).y;
        values_0[_S198 + 2U] = (*_S197).z;
        values_0[_S198 + 3U] = (*_S197).w;

#line 229
        v_4 = v_4 + 1U;

#line 229
    }

#line 229
    uint k_0 = 0U;

#line 236
    for(;;)
    {

#line 236
        if(k_0 < 9U)
        {
        }
        else
        {

#line 236
            break;
        }

#line 237
        uint _S199 = k_0 * 3U;

#line 237
        (*coefficients_0)[k_0] = float3(values_0[_S199], values_0[_S199 + 1U], values_0[_S199 + 2U]);

#line 236
        k_0 = k_0 + 1U;

#line 236
    }


    *history_0 = values_0[int(27)];
    return;
}


#line 389
bool spReproject_0(const SpProbe_0 thread* probe_4, float tolerance_1, array<float3, int(9)> thread* coefficients_1, float thread* history_1, KernelContext_0 thread* kernelContext_43)
{

#line 389
    uint k_1 = 0U;
    for(;;)
    {

#line 390
        if(k_1 < 9U)
        {
        }
        else
        {

#line 390
            break;
        }

#line 391
        (*coefficients_1)[k_1] = float3(0.0, 0.0, 0.0);

#line 390
        k_1 = k_1 + 1U;

#line 390
    }


    *history_1 = 0.0;

#line 393
    float3 _S200 = probe_4->position_2;

#line 393
    bool _S201 = spInvalidated_0(probe_4->position_2, kernelContext_43);
    if(_S201)
    {

#line 395
        return false;
    }
    thread uint x_6;
    thread uint y_7;

#line 398
    bool _S202 = spLanding_0(_S200, &x_6, &y_7, kernelContext_43);
    if(!_S202)
    {

#line 400
        return false;
    }

#line 400
    uint _S203 = spSlot_0(kernelContext_43);


    uint previous_0 = 1U - _S203;

#line 403
    float bestWeight_0 = 0.0;

#line 403
    uint best_4 = 4294967295U;

#line 403
    uint pass_0 = 0U;


    for(;;)
    {

#line 406
        if(pass_0 < 2U)
        {
        }
        else
        {

#line 406
            break;
        }

#line 406
        SpTaps_0 _S204;

        if(pass_0 == 0U)
        {

#line 408
            uint _S205 = spSpacing_0(kernelContext_43);

#line 408
            uint _S206 = spTilesX_0(kernelContext_43);

#line 408
            uint _S207 = spTilesY_0(kernelContext_43);

#line 408
            _S204 = spBilinearTaps_0(x_6, y_7, _S205, _S206, _S207);

#line 408
        }
        else
        {

#line 408
            uint _S208 = spCell_0(kernelContext_43);

#line 408
            uint _S209 = spCellsX_0(kernelContext_43);

#line 408
            uint _S210 = spCellsY_0(kernelContext_43);

#line 408
            _S204 = spBilinearTaps_0(x_6, y_7, _S208, _S209, _S210);

#line 408
        }

#line 408
        float bestWeight_1 = bestWeight_0;

#line 408
        uint best_5 = best_4;

#line 408
        uint tap_0 = 0U;

        for(;;)
        {

#line 410
            if(tap_0 < (_S204.count_0))
            {
            }
            else
            {

#line 410
                break;
            }

#line 408
            SpTaps_0 _S211 = _S204;

            uint _S212 = tap_0;

#line 410
            uint record_5;

            if(pass_0 == 1U)
            {

#line 412
                uint _S213 = spTableIndex_0(previous_0, _S211.index_1[_S212], kernelContext_43);
                uint device* _S214 = kernelContext_43->giResolve_0->slots_0+_S213;

#line 413
                uint slot_7 = *_S214;
                if((*_S214) == 4294967295U)
                {

#line 415
                    tap_0 = tap_0 + 1U;

#line 410
                    continue;
                }

#line 410
                uint _S215 = spUniformCount_0(kernelContext_43);

#line 410
                record_5 = _S215 + slot_7;

            }
            else
            {

#line 412
                record_5 = _S211.index_1[_S212];

#line 412
            }

#line 412
            SpProbe_0 _S216 = spLoadProbe_0(previous_0, record_5, kernelContext_43);

#line 420
            if((_S216.depth_0) <= 0.0)
            {

#line 421
                tap_0 = tap_0 + 1U;

#line 410
                continue;
            }

#line 424
            float weight_5 = _S204.weight_3[tap_0] * spGeometryWeight_0(_S216.position_2, _S216.normal_4, _S200, probe_4->normal_4, probe_4->depth_0, tolerance_1);

#line 424
            uint best_6;

#line 424
            float bestWeight_2;


            if(weight_5 > bestWeight_1)
            {

#line 427
                bestWeight_2 = weight_5;

#line 427
                best_6 = record_5;

#line 427
            }
            else
            {

#line 427
                bestWeight_2 = bestWeight_1;

#line 427
                best_6 = best_5;

#line 427
            }

#line 427
            bestWeight_1 = bestWeight_2;

#line 427
            best_5 = best_6;

#line 410
            tap_0 = tap_0 + 1U;

#line 410
        }

#line 406
        uint pass_1 = pass_0 + 1U;

#line 406
        bestWeight_0 = bestWeight_1;

#line 406
        best_4 = best_5;

#line 406
        pass_0 = pass_1;

#line 406
    }

#line 406
    bool _S217;

#line 433
    if(best_4 == 4294967295U)
    {

#line 433
        _S217 = true;

#line 433
    }
    else
    {

#line 433
        _S217 = bestWeight_0 < 0.25;

#line 433
    }

#line 433
    if(_S217)
    {

#line 434
        return false;
    }

#line 434
    spLoadCoefficients_0(previous_0, best_4, coefficients_1, history_1, kernelContext_43);


    return true;
}


#line 88 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
void grBasis_0(float3 normal_9, float3 thread* tangent_0, float3 thread* bitangent_0)
{

#line 89
    float _S218 = normal_9.z;

#line 89
    float sign_0;

#line 89
    if(_S218 >= 0.0)
    {

#line 89
        sign_0 = 1.0;

#line 89
    }
    else
    {

#line 89
        sign_0 = -1.0;

#line 89
    }
    float a_1 = -1.0 / (sign_0 + _S218);
    float _S219 = normal_9.x;

#line 91
    float _S220 = normal_9.y;

#line 91
    float b_1 = _S219 * _S220 * a_1;
    *tangent_0 = float3(1.0 + sign_0 * _S219 * _S219 * a_1, sign_0 * b_1, - sign_0 * _S219);
    *bitangent_0 = float3(b_1, sign_0 + _S220 * _S220 * a_1, - _S220);
    return;
}


#line 188 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
float3 spHemisphereDirection_0(float3 normal_10, float z_4, float turn_0)
{

#line 189
    thread float3 tangent_1;
    thread float3 bitangent_1;
    grBasis_0(normal_10, &tangent_1, &bitangent_1);
    float radius_1 = sqrt(max(0.0, 1.0 - z_4 * z_4));
    float angle_0 = 6.28318548202514648 * turn_0;
    return giNormalisedOr_0(tangent_1 * float3((radius_1 * cos(angle_0)))  + bitangent_1 * float3((radius_1 * sin(angle_0)))  + normal_10 * float3(z_4) , normal_10);
}


#line 90
void spBasis_0(float3 d_0, array<float, int(9)> thread* basis_0)
{

#line 91
    (*basis_0)[int(0)] = 0.28209498524665833;
    float _S221 = d_0.y;

#line 92
    (*basis_0)[int(1)] = 0.48860299587249756 * _S221;
    float _S222 = d_0.z;

#line 93
    (*basis_0)[int(2)] = 0.48860299587249756 * _S222;
    float _S223 = d_0.x;

#line 94
    (*basis_0)[int(3)] = 0.48860299587249756 * _S223;
    (*basis_0)[int(4)] = 1.09254801273345947 * (_S223 * _S221);
    (*basis_0)[int(5)] = 1.09254801273345947 * (_S221 * _S222);
    (*basis_0)[int(6)] = 0.31539198756217957 * (3.0 * (_S222 * _S222) - 1.0);
    (*basis_0)[int(7)] = 1.09254801273345947 * (_S223 * _S222);
    (*basis_0)[int(8)] = 0.54627400636672974 * (_S223 * _S223 - _S221 * _S221);
    return;
}


#line 102
uint spBandOf_0(uint k_2)
{

#line 102
    uint _S224;
    if(k_2 == 0U)
    {

#line 103
        _S224 = 0U;

#line 103
    }
    else
    {

#line 103
        if(k_2 < 4U)
        {

#line 103
            _S224 = 1U;

#line 103
        }
        else
        {

#line 103
            _S224 = 2U;

#line 103
        }

#line 103
    }

#line 103
    return _S224;
}

float3 spEvaluate_0(const array<float3, int(9)> thread* coefficients_2, float3 direction_6, float3 band_0)
{

#line 107
    thread array<float, int(9)> basis_1;
    spBasis_0(direction_6, &basis_1);

#line 108
    float3 sum_1 = float3(0.0, 0.0, 0.0);

#line 108
    uint k_3 = 0U;

    for(;;)
    {

#line 110
        if(k_3 < 9U)
        {
        }
        else
        {

#line 110
            break;
        }

#line 111
        uint b_2 = spBandOf_0(k_3);

#line 111
        float scale_0;
        if(b_2 == 0U)
        {

#line 112
            scale_0 = band_0.x;

#line 112
        }
        else
        {

#line 112
            if(b_2 == 1U)
            {

#line 112
                scale_0 = band_0.y;

#line 112
            }
            else
            {

#line 112
                scale_0 = band_0.z;

#line 112
            }

#line 112
        }
        float3 _S225 = sum_1 + (*coefficients_2)[k_3] * float3((basis_1[k_3] * scale_0)) ;

#line 110
        uint k_4 = k_3 + 1U;

#line 110
        sum_1 = _S225;

#line 110
        k_3 = k_4;

#line 110
    }

#line 115
    return float3(max(sum_1.x, 0.0), max(sum_1.y, 0.0), max(sum_1.z, 0.0));
}


#line 506 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
float4 gpSample_0(float3 position_9, float3 normal_11, uint frame_1, KernelContext_0 thread* kernelContext_44)
{

#line 507
    uint4 counts_1 = kernelContext_44->giProbes_0->constants_0[0U];
    uint _S226 = counts_1.x;
    uint _S227 = counts_1.y;
    uint _S228 = kernelContext_44->giProbes_0->constants_0[1U].z;
    float _S229 = (as_type<float>((kernelContext_44->giProbes_0->constants_0[5U].x)));

#line 511
    float3 total_3 = float3(0.0, 0.0, 0.0);

#line 511
    float oldest_1 = 0.0;

#line 511
    uint level_7 = 0U;

#line 511
    float totalWeight_0 = 0.0;



    for(;;)
    {

#line 515
        if(level_7 < _S228)
        {
        }
        else
        {

#line 515
            break;
        }

#line 516
        float spacing_1 = _S229 * float(1U << level_7);
        float radius_2 = spacing_1 * 1.5;
        float _S230 = position_9.x;

#line 518
        int _S231 = gpFloorDiv_0(_S230 - radius_2, spacing_1);
        float _S232 = position_9.y;

#line 519
        int _S233 = gpFloorDiv_0(_S232 - radius_2, spacing_1);
        float _S234 = position_9.z;
        int _S235 = gpFloorDiv_0(_S230 + radius_2, spacing_1);
        int _S236 = gpFloorDiv_0(_S232 + radius_2, spacing_1);
        int _S237 = gpFloorDiv_0(_S234 + radius_2, spacing_1);

#line 523
        int z_5 = gpFloorDiv_0(_S234 - radius_2, spacing_1);
        for(;;)
        {

#line 524
            if(z_5 <= _S237)
            {
            }
            else
            {

#line 524
                break;
            }

#line 524
            float3 total_4 = total_3;

#line 524
            float oldest_2 = oldest_1;

#line 524
            int y_8 = _S233;

#line 524
            float totalWeight_1 = totalWeight_0;
            for(;;)
            {

#line 525
                if(y_8 <= _S236)
                {
                }
                else
                {

#line 525
                    break;
                }

#line 525
                float3 total_5 = total_4;

#line 525
                float oldest_3 = oldest_2;

#line 525
                int x_7 = _S231;

#line 525
                float totalWeight_2 = totalWeight_1;
                for(;;)
                {

#line 526
                    if(x_7 <= _S235)
                    {
                    }
                    else
                    {

#line 526
                        break;
                    }

#line 526
                    uint _S238 = gpLayoutProbe_0(level_7, int3(x_7, y_8, z_5), kernelContext_44);

                    if(_S238 == 4294967295U)
                    {

#line 529
                        x_7 = x_7 + int(1);

#line 526
                        continue;
                    }



                    uint _S239 = _S238 * 4U;

#line 531
                    float4 device* _S240 = kernelContext_44->giProbes_0->probes_0+(_S239 + 1U);

#line 531
                    float4 record_6 = *_S240;

#line 531
                    bool _S241;
                    if(((*_S240).y) == 0.0)
                    {

#line 532
                        _S241 = true;

#line 532
                    }
                    else
                    {

#line 532
                        _S241 = (record_6.z) == 0.0;

#line 532
                    }

#line 532
                    if(_S241)
                    {

#line 533
                        x_7 = x_7 + int(1);

#line 526
                        continue;
                    }

#line 535
                    float3 at_1 = (*(kernelContext_44->giProbes_0->probes_0+_S239)).xyz;
                    float3 offset_4 = position_9 - at_1;
                    float span_3 = length(offset_4);
                    if(span_3 > radius_2)
                    {

#line 539
                        x_7 = x_7 + int(1);

#line 526
                        continue;
                    }

#line 541
                    float weight_6 = 1.0 - gpClamp_0(span_3 / radius_2, 0.0, 1.0);

#line 541
                    float weight_7;
                    if(span_3 > 0.00009999999747379)
                    {

#line 542
                        weight_7 = weight_6 * gpClamp_0((dot(normal_11, - offset_4 / float3(span_3) ) + 1.0) * 0.5, 0.0, 1.0);

#line 542
                    }
                    else
                    {

#line 542
                        weight_7 = weight_6;

#line 542
                    }

#line 542
                    float _S242 = gpVisibilityWeight_0(_S238, at_1, position_9, kernelContext_44);



                    float weight_8 = weight_7 * _S242;
                    if(weight_8 <= 0.0)
                    {

#line 548
                        x_7 = x_7 + int(1);

#line 526
                        continue;
                    }

#line 550
                    thread array<float, int(48)> data_1;

#line 550
                    gpLoadPayload_0(_S238, _S227, &data_1, kernelContext_44);

#line 550
                    thread array<float, int(48)> _S243 = data_1;

#line 550
                    float3 _S244 = gpDecode_0(_S226, &_S243, normal_11);

                    float3 _S245 = total_5 + _S244 * float3(weight_8) ;
)cy_msl",
    R"cy_msl(                    float totalWeight_3 = totalWeight_2 + weight_8;
                    uint last_2 = (as_type<uint>((record_6.x)));

#line 554
                    float age_2;
                    if(frame_1 > last_2)
                    {

#line 555
                        age_2 = float(frame_1 - last_2);

#line 555
                    }
                    else
                    {

#line 555
                        age_2 = 0.0;

#line 555
                    }
                    float _S246 = max(oldest_3, age_2);

#line 556
                    total_5 = _S245;

#line 556
                    oldest_3 = _S246;

#line 556
                    totalWeight_2 = totalWeight_3;

#line 526
                    x_7 = x_7 + int(1);

#line 526
                }

#line 525
                int y_9 = y_8 + int(1);

#line 525
                total_4 = total_5;

#line 525
                oldest_2 = oldest_3;

#line 525
                y_8 = y_9;

#line 525
                totalWeight_1 = totalWeight_2;

#line 525
            }

#line 524
            int z_6 = z_5 + int(1);

#line 524
            total_3 = total_4;

#line 524
            oldest_1 = oldest_2;

#line 524
            z_5 = z_6;

#line 524
            totalWeight_0 = totalWeight_1;

#line 524
        }

#line 515
        level_7 = level_7 + 1U;

#line 515
    }

#line 561
    if(totalWeight_0 <= 0.0)
    {

#line 562
        return float4(0.0, 0.0, 0.0, 0.0);
    }



    return float4(total_3 / float3(totalWeight_0) , gpClamp_0(1.0 - gpClamp_0(oldest_1 / 240.0, 0.0, 1.0) * 0.75, 0.0, 1.0));
}


#line 134 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
float spLuminance_0(float3 c_1)
{

#line 135
    return 0.2125999927520752 * c_1.x + 0.71520000696182251 * c_1.y + 0.07220000028610229 * c_1.z;
}


#line 73 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
uint grHash_0(uint pixel_0, uint frame_2, uint stream_0)
{

    uint hash_0 = ((pixel_0 * 2654435769U) ^ (frame_2 * 2246822507U)) ^ (stream_0 * 3266489909U);

    uint hash_1 = (hash_0 ^ (hash_0 >> 15U)) * 625341585U;

    return hash_1 ^ (hash_1 >> 13U);
}

float grUnit_0(uint hash_2)
{

#line 84
    return float(hash_2 & 16777215U) / 1.6777216e+07;
}


#line 199 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
uint spGuideCell_0(float z_7, float turn_1)
{

    return min(uint(z_7 * 4.0), 3U) * 4U + min(uint(turn_1 * 4.0), 3U);
}


#line 263 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
GrSample_0 grGatherSample_0(const GrSample_0 thread* ray_1, const GrSample_0 thread* standIn_0, uint excluded_0)
{

#line 263
    uint _S247 = standIn_0->source_0;

#line 263
    bool hasStandIn_0;
    if((standIn_0->source_0) != 0U)
    {

#line 264
        hasStandIn_0 = (standIn_0->confidence_0) > 0.0;

#line 264
    }
    else
    {

#line 264
        hasStandIn_0 = false;

#line 264
    }

#line 264
    if(hasStandIn_0)
    {

#line 264
        hasStandIn_0 = (excluded_0 & (1U << _S247)) == 0U;

#line 264
    }
    else
    {

#line 264
        hasStandIn_0 = false;

#line 264
    }

    thread GrSample_0 sample_3 = *ray_1;
    if((ray_1->source_0) == 0U)
    {

#line 268
        if(!hasStandIn_0)
        {

#line 269
            return *ray_1;
        }
        (&sample_3)->source_0 = _S247;
        (&sample_3)->radiance_0 = standIn_0->radiance_0;

#line 267
    }
    else
    {



        if(hasStandIn_0)
        {
            (&sample_3)->radiance_0 = standIn_0->radiance_0 + (ray_1->radiance_0 - standIn_0->radiance_0) * float3(gpClamp_0(ray_1->confidence_0, 0.0, 1.0)) ;

#line 273
        }

#line 267
    }

#line 277
    (&sample_3)->confidence_0 = 1.0;
    return sample_3;
}


#line 242 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
void spStoreCoefficients_0(uint slot_8, uint record_7, const array<float3, int(9)> thread* coefficients_3, float history_2, KernelContext_0 thread* kernelContext_45)
{

#line 242
    uint _S248 = spRecordBase_0(slot_8, record_7, kernelContext_45);
    uint _S249 = _S248 + 2U;
    thread array<float, int(28)> values_1;

#line 244
    uint k_5 = 0U;
    for(;;)
    {

#line 245
        if(k_5 < 9U)
        {
        }
        else
        {

#line 245
            break;
        }

#line 246
        uint _S250 = k_5 * 3U;

#line 246
        values_1[_S250] = (*coefficients_3)[k_5].x;
        values_1[_S250 + 1U] = (*coefficients_3)[k_5].y;
        values_1[_S250 + 2U] = (*coefficients_3)[k_5].z;

#line 245
        k_5 = k_5 + 1U;

#line 245
    }

#line 250
    values_1[int(27)] = history_2;

#line 250
    uint v_5 = 0U;
    for(;;)
    {

#line 251
        if(v_5 < 7U)
        {
        }
        else
        {

#line 251
            break;
        }

#line 252
        uint _S251 = v_5 * 4U;

#line 252
        *(kernelContext_45->giResolve_0->probes_1+(_S249 + v_5)) = float4(values_1[_S251], values_1[_S251 + 1U], values_1[_S251 + 2U], values_1[_S251 + 3U]);

#line 251
        v_5 = v_5 + 1U;

#line 251
    }



    return;
}


#line 168 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float grFarFieldWeight_0(float distance_0, KernelContext_0 thread* kernelContext_46)
{

#line 169
    float4 far_0 = (as_type<float4>((kernelContext_46->giResolve_0->constants_2[3U])));
    float begin_0 = far_0.x;
    if(begin_0 <= 0.0)
    {

#line 172
        return 1.0;
    }
    float _S252 = far_0.y;

#line 174
    float ratio_0;

#line 174
    if(_S252 > 0.0)
    {

#line 174
        ratio_0 = far_0.z / _S252;

#line 174
    }
    else
    {

#line 174
        ratio_0 = 8.0;

#line 174
    }
    float width_1 = begin_0 * gpClamp_0(ratio_0 / 100.0, 0.15000000596046448, 1.0);
    if(distance_0 <= begin_0)
    {

#line 177
        return 0.0;
    }
    float t_7 = gpClamp_0((distance_0 - begin_0) / width_1, 0.0, 1.0);
    return t_7 * t_7 * (3.0 - 2.0 * t_7);
}


#line 278 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giHemisphereDirection_0(float3 normal_12, uint index_2, uint count_2)
{
    float _S253 = float(index_2);
    float radius_3 = sqrt((_S253 + 0.5) / float(count_2));
    float angle_1 = 2.3999631404876709 * _S253;
    float x_8 = radius_3 * cos(angle_1);
    float y_10 = radius_3 * sin(angle_1);
    float z_8 = sqrt(max(0.0, 1.0 - x_8 * x_8 - y_10 * y_10));

#line 285
    float3 tangent_2;
    if((abs(normal_12.y)) < 0.99000000953674316)
    {

#line 286
        tangent_2 = cross(float3(0.0, 1.0, 0.0), normal_12);

#line 286
    }
    else
    {

#line 286
        tangent_2 = cross(float3(1.0, 0.0, 0.0), normal_12);

#line 286
    }

    float3 tangent_3 = giNormalisedOr_0(tangent_2, float3(1.0, 0.0, 0.0));

    return giNormalisedOr_0(tangent_3 * float3(x_8)  + cross(normal_12, tangent_3) * float3(y_10)  + normal_12 * float3(z_8) , normal_12);
}


#line 225
GiTraceHit_0 giSphereTrace_1(float3 origin_6, float3 direction_7, float maxDistance_2, float tMin_1, KernelContext_0 thread* kernelContext_47)
{

#line 226
    thread GiTraceHit_0 result_3;
    (&result_3)->hit_3 = false;
    (&result_3)->exhausted_0 = false;
    (&result_3)->t_3 = 0.0;
    (&result_3)->closest_0 = 0.0;
    (&result_3)->position_4 = float3(0.0, 0.0, 0.0);
    (&result_3)->normal_6 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S254 = giLevelCount_0(kernelContext_47);
    if(_S254 == 0U)
    {

#line 234
        return result_3;
    }

#line 234
    float _S255 = giFinestVoxel_0(kernelContext_47);


    float surface_1 = _S255 * 0.5;
    float _S256 = max(tMin_1, surface_1);
    thread array<float, int(3)> recent_1;

#line 239
    recent_1[int(0)] = 1.0e+06;

#line 239
    recent_1[int(1)] = 1.0e+06;

#line 239
    recent_1[int(2)] = 1.0e+06;

#line 239
    float closest_2 = 1.0e+06;

#line 239
    uint recentSlot_1 = 0U;

#line 239
    uint step_6 = 0U;

#line 239
    float t_8 = _S256;


    for(;;)
    {

#line 242
        if(step_6 < 192U)
        {
        }
        else
        {

#line 242
            break;
        }

#line 243
        if(t_8 > maxDistance_2)
        {
            (&result_3)->closest_0 = min(closest_2, min(recent_1[int(0)], min(recent_1[int(1)], recent_1[int(2)])));
            return result_3;
        }
        float3 position_10 = origin_6 + direction_7 * float3(t_8) ;

#line 248
        float _S257 = giDistance_0(position_10, kernelContext_47);

        if(_S257 < surface_1)
        {

#line 251
            (&result_3)->hit_3 = true;
            (&result_3)->t_3 = t_8;
            (&result_3)->position_4 = position_10;


            float3 _S258 = float3(_S255, 0.0, 0.0);

#line 256
            float _S259 = giDistance_0(position_10 + _S258, kernelContext_47);

#line 256
            float _S260 = giDistance_0(position_10 - _S258, kernelContext_47);

#line 256
            float _S261 = _S259 - _S260;

            float3 _S262 = float3(0.0, _S255, 0.0);

#line 258
            float _S263 = giDistance_0(position_10 + _S262, kernelContext_47);

#line 258
            float _S264 = giDistance_0(position_10 - _S262, kernelContext_47);

#line 258
            float _S265 = _S263 - _S264;

            float3 _S266 = float3(0.0, 0.0, _S255);

#line 260
            float _S267 = giDistance_0(position_10 + _S266, kernelContext_47);

#line 260
            float _S268 = giDistance_0(position_10 - _S266, kernelContext_47);

            (&result_3)->normal_6 = giNormalisedOr_0(float3(_S261, _S265, _S267 - _S268), - direction_7);
            (&result_3)->closest_0 = closest_2;
            return result_3;
        }
        float _S269 = min(closest_2, recent_1[recentSlot_1]);
        recent_1[recentSlot_1] = _S257;
        uint _S270 = (recentSlot_1 + 1U) % 3U;
        float t_9 = t_8 + max(_S257, surface_1);

#line 242
        uint step_7 = step_6 + 1U;

#line 242
        closest_2 = _S269;

#line 242
        recentSlot_1 = _S270;

#line 242
        step_6 = step_7;

#line 242
        t_8 = t_9;

#line 242
    }

#line 271
    (&result_3)->exhausted_0 = true;

    (&result_3)->closest_0 = min(closest_2, min(recent_1[int(0)], min(recent_1[int(1)], recent_1[int(2)])));
    return result_3;
}


#line 184 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float grSkyVisibilityRay_0(float3 position_11, float3 normal_13, uint index_3, uint count_3, KernelContext_0 thread* kernelContext_48)
{

#line 184
    bool _S271;
    if(count_3 == 0U)
    {

#line 185
        _S271 = true;

#line 185
    }
    else
    {

#line 185
        uint _S272 = giLevelCount_0(kernelContext_48);

#line 185
        _S271 = _S272 == 0U;

#line 185
    }

#line 185
    if(_S271)
    {

#line 186
        return 1.0;
    }
    float reach_2 = (as_type<float>((kernelContext_48->giResolve_0->constants_2[3U].w)));

#line 188
    float _S273 = giFinestVoxel_0(kernelContext_48);

#line 188
    GiTraceHit_0 _S274 = giSphereTrace_1(position_11 + normal_13 * float3((_S273 * 2.0)) , giHemisphereDirection_0(normal_13, index_3, count_3), reach_2, 0.0, kernelContext_48);

#line 193
    if(_S274.hit_3)
    {

#line 194
        return 0.0;
    }
    return gpClamp_0(_S274.closest_0 / (_S273 * 4.0), 0.0, 1.0);
}


#line 491 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
void spTrace_0(uint record_8, uint lane_0, KernelContext_0 thread* kernelContext_49)
{

#line 491
    uint _S275 = spSlot_0(kernelContext_49);

#line 491
    bool allowed_0;


    if(record_8 == 0U)
    {

#line 494
        allowed_0 = lane_0 == 0U;

#line 494
    }
    else
    {

#line 494
        allowed_0 = false;

#line 494
    }

#line 494
    if(allowed_0)
    {

#line 495
        *(kernelContext_49->giResolve_0->slots_0+spCounter_0(1U - _S275)) = 0U;

#line 494
    }

#line 494
    uint _S276 = spUniformCount_0(kernelContext_49);



    if(record_8 >= _S276)
    {

#line 499
        uint _S277 = record_8 - _S276;

#line 499
        uint _S278 = *(kernelContext_49->giResolve_0->slots_0+spCounter_0(_S275));

#line 499
        uint _S279 = spCapacity_0(kernelContext_49);

#line 499
        allowed_0 = _S277 >= (min(_S278, _S279));

#line 498
    }
    else
    {

#line 498
        allowed_0 = false;

#line 498
    }

#line 498
    if(allowed_0)
    {
        return;
    }

#line 500
    SpProbe_0 _S280 = spLoadProbe_0(_S275, record_8, kernelContext_49);


    if((_S280.depth_0) <= 0.0)
    {

#line 504
        return;
    }
    uint4 shape_1 = kernelContext_49->giResolve_0->constants_2[42U];
    float4 weights_0 = (as_type<float4>((kernelContext_49->giResolve_0->constants_2[43U])));
    uint frame_3 = kernelContext_49->giResolve_0->constants_2[18U].x;
    uint cacheFrame_2 = kernelContext_49->giResolve_0->constants_2[4U].w;

#line 509
    uint cell_4;

    if(lane_0 == 0U)
    {

#line 511
        float3 _S281 = _S280.normal_4;
        thread float4 diffuse_1;
        thread float4 specular_1;

#line 513
        grSampleTraced_0(_S280.position_2, _S281, _S281, cacheFrame_2, &diffuse_1, &specular_1, kernelContext_49);

        *kernelContext_49->spStandIn_0 = diffuse_1;
        float _S282 = (as_type<float>((kernelContext_49->giResolve_0->constants_2[2U].y)));

#line 516
        thread SpProbe_0 _S283 = _S280;

#line 516
        float3 _S284 = spProbeOrigin_0(&_S283, _S282, kernelContext_49);

#line 516
        *kernelContext_49->spOrigin_0 = _S284;
        thread array<float3, int(9)> previous_1;
        thread float history_3 = 0.0;
        if(((shape_1.w) & 2U) != 0U)
        {

#line 519
            allowed_0 = (kernelContext_49->giResolve_0->constants_2[5U].w) != 0U;

#line 519
        }
        else
        {

#line 519
            allowed_0 = false;

#line 519
        }

#line 519
        bool found_0;
        if(allowed_0)
        {

#line 520
            float _S285 = weights_0.y;

#line 520
            thread SpProbe_0 _S286 = _S280;

#line 520
            bool _S287 = spReproject_0(&_S286, _S285, &previous_1, &history_3, kernelContext_49);

#line 520
            found_0 = _S287;

#line 520
        }
        else
        {

#line 520
            found_0 = false;

#line 520
        }

#line 520
        cell_4 = 0U;
        for(;;)
        {

#line 521
            if(cell_4 < 9U)
            {
            }
            else
            {

#line 521
                break;
            }

#line 522
            (*kernelContext_49->spHistory_0)[cell_4] = previous_1[cell_4];

#line 521
            cell_4 = cell_4 + 1U;

#line 521
        }


        *kernelContext_49->spHistoryCount_0 = history_3;
        if(found_0)
        {

#line 525
            cell_4 = 1U;

#line 525
        }
        else
        {

#line 525
            cell_4 = 0U;

#line 525
        }

#line 525
        *kernelContext_49->spReprojected_0 = cell_4;

#line 511
    }

#line 527
    threadgroup_barrier(mem_flags::mem_threadgroup);

#line 527
    float3 value_4;

    if(lane_0 < 16U)
    {

#line 530
        float z_9 = (float(lane_0 / 4U) + 0.5) / 4.0;

        float3 direction_8 = spHemisphereDirection_0(_S280.normal_4, z_9, (float(lane_0 % 4U) + 0.5) / 4.0);

        if((*kernelContext_49->spReprojected_0) != 0U)
        {

#line 535
            thread array<float3, int(9)> previous_2;

#line 535
            cell_4 = 0U;
            for(;;)
            {

#line 536
                if(cell_4 < 9U)
                {
                }
                else
                {

#line 536
                    break;
                }

#line 537
                previous_2[cell_4] = (*kernelContext_49->spHistory_0)[cell_4];

#line 536
                cell_4 = cell_4 + 1U;

#line 536
            }


            float3 _S288 = float3(1.0, 1.0, 1.0);

#line 539
            thread array<float3, int(9)> _S289 = previous_2;

#line 539
            float3 _S290 = spEvaluate_0(&_S289, direction_8, _S288);

#line 539
            value_4 = _S290;

#line 534
        }
        else
        {

#line 534
            float4 _S291 = gpSample_0(_S280.position_2, direction_8, cacheFrame_2, kernelContext_49);

#line 534
            value_4 = _S291.xyz;

#line 534
        }

#line 543
)cy_msl",
    R"cy_msl(        (*kernelContext_49->spWeights_0)[lane_0] = max(spLuminance_0(value_4), 0.0) * z_9;

#line 529
    }

#line 545
    threadgroup_barrier(mem_flags::mem_threadgroup);

#line 545
    cell_4 = 0U;

#line 545
    float guideTotal_0 = 0.0;


    for(;;)
    {

#line 548
        if(cell_4 < 16U)
        {
        }
        else
        {

#line 548
            break;
        }

#line 549
        float guideTotal_1 = guideTotal_0 + (*kernelContext_49->spWeights_0)[cell_4];

#line 548
        cell_4 = cell_4 + 1U;

#line 548
        guideTotal_0 = guideTotal_1;

#line 548
    }

#line 548
    float guided_0;


    if(guideTotal_0 > 0.0)
    {

#line 551
        guided_0 = gpClamp_0(weights_0.z, 0.0, 0.89999997615814209);

#line 551
    }
    else
    {

#line 551
        guided_0 = 0.0;

#line 551
    }
    float uniformShare_0 = (1.0 - guided_0) * 0.5;

    uint _S292 = min(max(shape_1.x, 1U), 64U);

#line 554
    uint k_6;

#line 554
    float history_4;

#line 554
    float openness_0;

    if(lane_0 < _S292)
    {

#line 557
        uint hash_3 = grHash_0(_S280.id_0 * 64U + lane_0, frame_3, 8U);
        float u0_0 = grUnit_0(hash_3);
        float u1_0 = grUnit_0(hash_3 * 656542357U);
        float u2_0 = grUnit_0(hash_3 * 2654435769U);


        if(u0_0 >= uniformShare_0)
        {

#line 563
            allowed_0 = u0_0 < (uniformShare_0 + uniformShare_0);

#line 563
        }
        else
        {

#line 563
            allowed_0 = false;

#line 563
        }

#line 563
        if(allowed_0)
        {

#line 563
            history_4 = sqrt(u1_0);

#line 563
            openness_0 = u2_0;

#line 563
        }
        else
        {

#line 565
            if(u0_0 >= (uniformShare_0 + uniformShare_0))
            {

#line 566
                float _S293 = (u0_0 - uniformShare_0 - uniformShare_0) / guided_0 * guideTotal_0;

#line 566
                cell_4 = 0U;

#line 566
                history_4 = 0.0;


                for(;;)
                {

#line 569
                    if(cell_4 < 16U)
                    {
                    }
                    else
                    {

#line 569
                        k_6 = 15U;

#line 569
                        break;
                    }

#line 570
                    float running_0 = history_4 + (*kernelContext_49->spWeights_0)[cell_4];
                    if(((*kernelContext_49->spWeights_0)[cell_4]) > 0.0)
                    {

#line 571
                        allowed_0 = _S293 < running_0;

#line 571
                    }
                    else
                    {

#line 571
                        allowed_0 = false;

#line 571
                    }

#line 571
                    if(allowed_0)
                    {

#line 571
                        k_6 = cell_4;

                        break;
                    }

#line 569
                    cell_4 = cell_4 + 1U;

#line 569
                    history_4 = running_0;

#line 569
                }

#line 577
                float _S294 = (float(k_6 % 4U) + u2_0) / 4.0;

#line 577
                history_4 = (float(k_6 / 4U) + u1_0) / 4.0;

#line 577
                openness_0 = _S294;

#line 565
            }
            else
            {

#line 565
                history_4 = u1_0;

#line 565
                openness_0 = u2_0;

#line 565
            }

#line 563
        }

#line 579
        float density_0 = uniformShare_0 / 6.28318548202514648 + uniformShare_0 * history_4 / 3.14159274101257324;

#line 579
        float density_1;

        if(guided_0 > 0.0)
        {

#line 581
            density_1 = density_0 + guided_0 * ((*kernelContext_49->spWeights_0)[spGuideCell_0(history_4, openness_0)] / guideTotal_0) * 2.5464789867401123;

#line 581
        }
        else
        {

#line 581
            density_1 = density_0;

#line 581
        }



        float3 _S295 = float3(0.0, 0.0, 0.0);
        float3 direction_9 = spHemisphereDirection_0(_S280.normal_4, history_4, openness_0);
        bool _S296 = density_1 > 0.0;

#line 587
        if(_S296)
        {
            uint excluded_1 = kernelContext_49->giResolve_0->constants_2[2U].w;

            float4 standIn_1 = *kernelContext_49->spStandIn_0;

#line 591
            GrSample_0 _S297 = spProbeRay_0(*kernelContext_49->spOrigin_0, giNormalisedOr_0(direction_9, float3(0.0, 0.0, -1.0)), (as_type<float4>((kernelContext_49->giResolve_0->constants_2[2U]))).x, kernelContext_49);


            GrSample_0 _S298 = grSample_0(standIn_1.xyz, standIn_1.w, 3U);

#line 594
            thread GrSample_0 _S299 = _S297;

#line 594
            thread GrSample_0 _S300 = _S298;

#line 594
            GrSample_0 _S301 = grGatherSample_0(&_S299, &_S300, excluded_1);
            if((_S301.source_0) != 0U)
            {

#line 595
                value_4 = _S301.radiance_0;

#line 595
                k_6 = 1U;

#line 595
            }
            else
            {

#line 595
                value_4 = _S295;

#line 595
                k_6 = 0U;

#line 595
            }

#line 587
        }
        else
        {

#line 587
            value_4 = _S295;

#line 587
            k_6 = 0U;

#line 587
        }

#line 600
        (*kernelContext_49->spRayValue_0)[lane_0] = value_4;
        (*kernelContext_49->spRayDirection_0)[lane_0] = direction_9;
        if(_S296)
        {

#line 602
            guideTotal_0 = 1.0 / (density_1 * float(_S292));

#line 602
        }
        else
        {

#line 602
            guideTotal_0 = 0.0;

#line 602
        }

#line 602
        (*kernelContext_49->spRayWeight_0)[lane_0] = guideTotal_0;
        (*kernelContext_49->spRayValid_0)[lane_0] = k_6;

#line 556
    }

#line 605
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if(lane_0 != 0U)
    {

#line 607
        return;
    }

    thread array<float3, int(9)> sum_2;

#line 610
    k_6 = 0U;
    for(;;)
    {

#line 611
        if(k_6 < 9U)
        {
        }
        else
        {

#line 611
            break;
        }

#line 612
        sum_2[k_6] = float3(0.0, 0.0, 0.0);

#line 611
        k_6 = k_6 + 1U;

#line 611
    }

#line 611
    uint ray_2 = 0U;


    for(;;)
    {

#line 614
        if(ray_2 < _S292)
        {
        }
        else
        {

#line 614
            break;
        }

#line 615
        if(((*kernelContext_49->spRayValid_0)[ray_2]) == 0U)
        {

#line 616
            ray_2 = ray_2 + 1U;

#line 614
            continue;
        }


        thread array<float, int(9)> basis_2;
        spBasis_0((*kernelContext_49->spRayDirection_0)[ray_2], &basis_2);
        float _S302 = (*kernelContext_49->spRayWeight_0)[ray_2];

#line 620
        k_6 = 0U;
        for(;;)
        {

#line 621
            if(k_6 < 9U)
            {
            }
            else
            {

#line 621
                break;
            }

#line 622
            sum_2[k_6] = sum_2[k_6] + (*kernelContext_49->spRayValue_0)[ray_2] * float3((basis_2[k_6] * _S302)) ;

#line 621
            k_6 = k_6 + 1U;

#line 621
        }

#line 614
        ray_2 = ray_2 + 1U;

#line 614
    }

#line 626
    float frames_0 = float(max(shape_1.y, 1U));
    thread array<float3, int(9)> blended_0;

    if((*kernelContext_49->spReprojected_0) != 0U)
    {

#line 630
        float _S303 = max(1.0 / (*kernelContext_49->spHistoryCount_0 + 1.0), 1.0 / frames_0);

#line 630
        k_6 = 0U;
        for(;;)
        {

#line 631
            if(k_6 < 9U)
            {
            }
            else
            {

#line 631
                break;
            }

#line 632
            blended_0[k_6] = (*kernelContext_49->spHistory_0)[k_6] + (sum_2[k_6] - (*kernelContext_49->spHistory_0)[k_6]) * float3(_S303) ;

#line 631
            k_6 = k_6 + 1U;

#line 631
        }

#line 631
        history_4 = min(*kernelContext_49->spHistoryCount_0 + 1.0, frames_0);

#line 629
    }
    else
    {

#line 629
        k_6 = 0U;

#line 636
        for(;;)
        {

#line 636
            if(k_6 < 9U)
            {
            }
            else
            {

#line 636
                break;
            }

#line 637
            blended_0[k_6] = sum_2[k_6];

#line 636
            k_6 = k_6 + 1U;

#line 636
        }

#line 636
        history_4 = 1.0;

#line 629
    }

#line 629
    thread array<float3, int(9)> _S304 = blended_0;

#line 629
    spStoreCoefficients_0(_S275, record_8, &_S304, history_4, kernelContext_49);

#line 629
    float _S305 = grFarFieldWeight_0(length(_S280.position_2 - (as_type<float3>((kernelContext_49->giResolve_0->constants_2[1U].xyz)))), kernelContext_49);

#line 645
    uint skyRays_0 = kernelContext_49->giResolve_0->constants_2[2U].z;
    if(_S305 > 0.0)
    {

#line 646
        allowed_0 = skyRays_0 > 0U;

#line 646
    }
    else
    {

#line 646
        allowed_0 = false;

#line 646
    }

#line 646
    if(allowed_0)
    {

#line 647
        uint _S306 = (frame_3 + _S280.id_0) % skyRays_0;

#line 647
        float _S307 = grSkyVisibilityRay_0(_S280.position_2, _S280.normal_4, _S306, skyRays_0, kernelContext_49);

#line 647
        openness_0 = _S307;

#line 646
    }
    else
    {

#line 646
        openness_0 = 1.0;

#line 646
    }

#line 646
    uint _S308 = spRecordBase_0(_S275, record_8, kernelContext_49);

#line 651
    *(kernelContext_49->giResolve_0->probes_1+(_S308 + 9U)) = *kernelContext_49->spStandIn_0;
    *(kernelContext_49->giResolve_0->probes_1+(_S308 + 10U)) = float4(openness_0, 0.0, 0.0, 0.0);
    return;
}


#line 17 "src/rendering/gi_gpu/shaders/gi_screen_probes_hardware.slang"
[[kernel]] void cyGiProbeTraceHardware(uint3 group_0 [[threadgroup_position_in_grid]], uint3 lane_1 [[thread_position_in_threadgroup]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiResolveSet_default_0 constant* giResolve_1 [[buffer(3)]], GiHardwareSet_default_0 constant* giHardware_1 [[buffer(4)]])
{

#line 17
    thread KernelContext_0 kernelContext_50;

#line 17
    (&kernelContext_50)->giProbes_0 = giProbes_1;

#line 17
    (&kernelContext_50)->giScene_0 = giScene_1;

#line 17
    (&kernelContext_50)->giResolve_0 = giResolve_1;

#line 17
    (&kernelContext_50)->giHardware_0 = giHardware_1;

#line 17
    threadgroup float4 spStandIn_1;

#line 17
    (&kernelContext_50)->spStandIn_0 = &spStandIn_1;

#line 17
    threadgroup float3 spOrigin_1;

#line 17
    (&kernelContext_50)->spOrigin_0 = &spOrigin_1;

#line 17
    threadgroup array<float3, int(9)> spHistory_1;

#line 17
    (&kernelContext_50)->spHistory_0 = &spHistory_1;

#line 17
    threadgroup float spHistoryCount_1;

#line 17
    (&kernelContext_50)->spHistoryCount_0 = &spHistoryCount_1;

#line 17
    threadgroup uint spReprojected_1;

#line 17
    (&kernelContext_50)->spReprojected_0 = &spReprojected_1;

#line 17
    threadgroup array<float, int(16)> spWeights_1;

#line 17
    (&kernelContext_50)->spWeights_0 = &spWeights_1;

#line 17
    threadgroup array<float3, int(64)> spRayValue_1;

#line 17
    (&kernelContext_50)->spRayValue_0 = &spRayValue_1;

#line 17
    threadgroup array<float3, int(64)> spRayDirection_1;

#line 17
    (&kernelContext_50)->spRayDirection_0 = &spRayDirection_1;

#line 17
    threadgroup array<float, int(64)> spRayWeight_1;

#line 17
    (&kernelContext_50)->spRayWeight_0 = &spRayWeight_1;

#line 17
    threadgroup array<uint, int(64)> spRayValid_1;

#line 17
    (&kernelContext_50)->spRayValid_0 = &spRayValid_1;

#line 17
    spTrace_0(group_0.x, lane_1.x, &kernelContext_50);

    return;
}

)cy_msl");
inline constexpr const auto& kGiProbeTraceHardwareMsl = kGiProbeTraceHardwareMslText.text;

/// cyGiProbeIntegrateHardware.metal, 50029 bytes.
inline constexpr char kGiProbeIntegrateHardwareMsl[] =
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
            

#line 118 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
struct GrSample_0
{
    float3 radiance_0;
    float confidence_0;
    uint source_0;
};


#line 53
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


#line 78 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
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


#line 66 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
struct GiResolveSet_default_0
{
    uint4 device* constants_2;
    float4 device* surfaces_0;
    float4 device* noisy_0;
    float4 device* probes_1;
    uint device* slots_0;
};


#line 22 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
struct GiHardwareSet_default_0
{
    metal::raytracing::acceleration_structure<metal::raytracing::instancing> topLevel_0;
    uint4 device* instances_0;
    float device* positions_0;
    uint device* indices_0;
};


#line 22
struct KernelContext_0
{
    GiProbeSet_default_0 constant* giProbes_0;
    GiSceneSet_default_0 constant* giScene_0;
    GiResolveSet_default_0 constant* giResolve_0;
    GiHardwareSet_default_0 constant* giHardware_0;
};


#line 73
uint GiHardwareTier_source_0()
{

#line 74
    return 7U;
}


#line 123 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float giFinestVoxel_0(KernelContext_0 thread* kernelContext_0)
{

#line 124
    return (as_type<float>((kernelContext_0->giScene_0->constants_1[9U].y)));
}


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


#line 27 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
float3 ghVertex_0(uint vertex_0, KernelContext_0 thread* kernelContext_1)
{

#line 28
    uint _S1 = vertex_0 * 3U;

#line 28
    return float3(kernelContext_1->giHardware_0->positions_0[_S1], kernelContext_1->giHardware_0->positions_0[_S1 + 1U], kernelContext_1->giHardware_0->positions_0[_S1 + 2U]);
}


#line 99 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float3 giNormalisedOr_0(float3 v_0, float3 fallback_0)
{

#line 100
    float squared_0 = dot(v_0, v_0);

#line 100
    float3 _S2;
    if(squared_0 > 9.99999993922529029e-09)
    {

#line 101
        _S2 = v_0 * float3((1.0 / sqrt(squared_0))) ;

#line 101
    }
    else
    {

#line 101
        _S2 = fallback_0;

#line 101
    }

#line 101
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


#line 33 "src/rendering/gi_gpu/shaders/gi_hardware_tier.slang"
GiWorldHit_0 GiHardwareTier_trace_0(float3 origin_0, float3 direction_0, float maxDistance_0, KernelContext_0 thread* kernelContext_2)
{

#line 34
    thread RayDesc_0 ray_0;
    (&ray_0)->Origin_0 = origin_0;
    (&ray_0)->Direction_0 = direction_0;
    (&ray_0)->TMin_0 = 0.00100000004749745;
    (&ray_0)->TMax_0 = maxDistance_0;
    thread raytracing::intersection_query<raytracing::triangle_data, raytracing::instancing> query_0;
    (&query_0)->reset({ (ray_0.Origin_0), (ray_0.Direction_0), (ray_0.TMin_0), (ray_0.TMax_0) }, (kernelContext_2->giHardware_0->topLevel_0), (255U), _slang_ray_flags_to_intersection_params((1U)));
    for(;;)
    {

#line 41
        bool _S4 = ((&query_0)->next());

#line 41
        if(_S4)
        {
        }
        else
        {

#line 41
            break;
        }

#line 41
    }

    uint _S5 = (_slang_committed_intersection_type_to_uint((&query_0)->get_committed_intersection_type()));

#line 43
    if(_S5 != 1U)
    {

#line 44
        return gpMiss_0();
    }
    float t_1 = ((&query_0)->get_committed_distance());
    uint instance_0 = ((&query_0)->get_committed_user_instance_id());
    uint primitive_0 = ((&query_0)->get_committed_primitive_id());
    uint _S6 = instance_0 * 4U;

)cy_msl"
    R"cy_msl(#line 49
    uint4 record_0 = kernelContext_2->giHardware_0->instances_0[_S6];
    uint first_0 = record_0.y + primitive_0 * 3U;
    uint _S7 = record_0.x;

#line 51
    float3 _S8 = ghVertex_0(_S7 + kernelContext_2->giHardware_0->indices_0[first_0], kernelContext_2);

#line 51
    float3 _S9 = ghVertex_0(_S7 + kernelContext_2->giHardware_0->indices_0[first_0 + 1U], kernelContext_2);

#line 51
    float3 _S10 = ghVertex_0(_S7 + kernelContext_2->giHardware_0->indices_0[first_0 + 2U], kernelContext_2);


    float4 c0_0 = (as_type<float4>((kernelContext_2->giHardware_0->instances_0[_S6 + 1U])));


    float3 local_0 = cross(_S9 - _S8, _S10 - _S8);
    float3 normal_1 = giNormalisedOr_0(c0_0.xyz * float3(local_0.x)  + (as_type<float4>((kernelContext_2->giHardware_0->instances_0[_S6 + 2U]))).xyz * float3(local_0.y)  + (as_type<float4>((kernelContext_2->giHardware_0->instances_0[_S6 + 3U]))).xyz * float3(local_0.z) , float3(0.0, 1.0, 0.0));

#line 58
    float3 normal_2;

    if((dot(normal_1, direction_0)) > 0.0)
    {

#line 60
        normal_2 = - normal_1;

#line 60
    }
    else
    {

#line 60
        normal_2 = normal_1;

#line 60
    }


    thread GiWorldHit_0 hit_1;
    (&hit_1)->hit_0 = true;
    (&hit_1)->t_0 = t_1;
    (&hit_1)->position_0 = origin_0 + direction_0 * float3(t_1) ;
    (&hit_1)->normal_0 = normal_2;


    (&hit_1)->confidence_1 = (as_type<float4>((kernelContext_2->giProbes_0->constants_0[25U]))).z - gpClamp_0(c0_0.w * 2.0, 0.0, 0.40000000596046448);
    return hit_1;
}


#line 124 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
GrSample_0 grSample_0(float3 radiance_1, float confidence_2, uint source_1)
{

#line 125
    thread GrSample_0 sample_0;
    (&sample_0)->radiance_0 = radiance_1;
    (&sample_0)->confidence_0 = confidence_2;
    (&sample_0)->source_0 = source_1;
    return sample_0;
}


#line 689 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
struct GiTierSample_0
{
    float3 radiance_2;
    float confidence_3;
    float hitDistance_0;
    uint source_2;
};


#line 696
GiTierSample_0 gpTierSample_0(uint source_3)
{

#line 697
    thread GiTierSample_0 sample_1;
    (&sample_1)->radiance_2 = float3(0.0, 0.0, 0.0);
    (&sample_1)->confidence_3 = 0.0;
    (&sample_1)->hitDistance_0 = -1.0;
    (&sample_1)->source_2 = source_3;
    return sample_1;
}


#line 121
float3 gpSky_0(float3 direction_1, KernelContext_0 thread* kernelContext_3)
{

#line 122
    float4 zenith_0 = (as_type<float4>((kernelContext_3->giProbes_0->constants_0[2U])));
    float3 horizon_0 = (as_type<float3>((kernelContext_3->giProbes_0->constants_0[3U].xyz)));
    float3 ground_0 = (as_type<float3>((kernelContext_3->giProbes_0->constants_0[4U].xyz)));

    float _S11 = giNormalisedOr_0(direction_1, float3(0.0, 1.0, 0.0)).y;

#line 126
    if(_S11 < 0.0)
    {

#line 127
        return ground_0 * float3(zenith_0.w) ;
    }

    return (horizon_0 + (zenith_0.xyz - horizon_0) * float3(gpClamp_0(pow(_S11, 0.44999998807907104), 0.0, 1.0)) ) * float3(zenith_0.w) ;
}


#line 471 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
int giCell_0(float value_1, float cell_0)
{

#line 472
    return int(floor(value_1 / cell_0));
}

uint giBucket_0(int x_0, int y_0, int z_0, uint buckets_0)
{
    return (((uint(x_0) * 73856093U) ^ (uint(y_0) * 19349663U)) ^ (uint(z_0) * 83492791U)) & (buckets_0 - 1U);
}


uint giFindCard_0(float3 position_1, float3 normal_3, KernelContext_0 thread* kernelContext_4)
{

#line 482
    uint4 grid_0 = kernelContext_4->giScene_0->constants_1[1U];
    uint buckets_1 = grid_0.x;
    if(buckets_1 == 0U)
    {

#line 485
        return 4294967295U;
    }
    float cell_1 = (as_type<float>((grid_0.y)));
    float radius_0 = (as_type<float>((grid_0.z)));
    float _S12 = radius_0 * radius_0;
    float _S13 = position_1.x;

#line 490
    int _S14 = giCell_0(_S13 - radius_0, cell_1);

#line 490
    float _S15 = position_1.y;

#line 490
    int _S16 = giCell_0(_S15 - radius_0, cell_1);
    float _S17 = position_1.z;

#line 491
    int _S18 = giCell_0(_S17 - radius_0, cell_1);
    int _S19 = giCell_0(_S13 + radius_0, cell_1);

#line 492
    int _S20 = giCell_0(_S15 + radius_0, cell_1);
    int _S21 = giCell_0(_S17 + radius_0, cell_1);

#line 493
    float bestScore_0 = 1.0e+09;

#line 493
    float bestAlignment_0 = 0.0;

#line 493
    uint best_0 = 4294967295U;

#line 493
    int z_1 = _S18;



    for(;;)
    {

#line 497
        if(z_1 <= _S21)
        {
        }
        else
        {

#line 497
            break;
        }

#line 497
        int y_1 = _S16;
        for(;;)
        {

#line 498
            if(y_1 <= _S20)
            {
            }
            else
            {

#line 498
                break;
            }

#line 498
            float bestScore_1 = bestScore_0;

#line 498
            float bestAlignment_1 = bestAlignment_0;

#line 498
            uint best_1 = best_0;

#line 498
            int x_1 = _S14;
            for(;;)
            {

#line 499
                if(x_1 <= _S19)
                {
                }
                else
                {

#line 499
                    break;
                }
                uint _S22 = giBucket_0(x_1, y_1, z_1, buckets_1) * 2U;

#line 501
                uint first_1 = kernelContext_4->giScene_0->gridRanges_0[_S22];
                uint _S23 = kernelContext_4->giScene_0->gridRanges_0[_S22 + 1U];

#line 502
                float bestScore_2 = bestScore_1;

#line 502
                float bestAlignment_2 = bestAlignment_1;

#line 502
                uint best_2 = best_1;

#line 502
                uint item_0 = first_1;
                for(;;)
                {

#line 503
                    if(item_0 < (first_1 + _S23))
                    {
                    }
                    else
                    {

#line 503
                        break;
                    }

#line 504
                    uint handle_0 = kernelContext_4->giScene_0->gridItems_0[item_0];
                    if(((*(kernelContext_4->giScene_0->cardState_0+handle_0 * 2U)).w) == 0.0)
                    {

#line 506
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }



                    uint _S24 = handle_0 * 4U;
                    float alignment_0 = dot(kernelContext_4->giScene_0->cards_0[_S24 + 1U].xyz, normal_3);
                    if(alignment_0 <= 0.25)
                    {

#line 511
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

#line 513
                    float3 offset_0 = kernelContext_4->giScene_0->cards_0[_S24].xyz - position_1;
                    float squared_1 = dot(offset_0, offset_0);
                    if(squared_1 > _S12)
                    {

#line 516
                        item_0 = item_0 + 1U;

#line 503
                        continue;
                    }

)cy_msl"
    R"cy_msl(#line 519
                    float score_0 = squared_1 / alignment_0;

#line 519
                    bool better_0;
                    if(score_0 != bestScore_2)
                    {

#line 520
                        better_0 = score_0 < bestScore_2;

#line 520
                    }
                    else
                    {

#line 521
                        if(alignment_0 != bestAlignment_2)
                        {

#line 521
                            better_0 = alignment_0 > bestAlignment_2;

#line 521
                        }
                        else
                        {

#line 521
                            better_0 = handle_0 < best_2;

#line 521
                        }

#line 520
                    }

#line 520
                    float bestScore_3;

#line 520
                    float bestAlignment_3;

#line 520
                    uint best_3;


                    if(better_0)
                    {

#line 523
                        bestScore_3 = score_0;

#line 523
                        bestAlignment_3 = alignment_0;

#line 523
                        best_3 = handle_0;

#line 523
                    }
                    else
                    {

#line 523
                        bestScore_3 = bestScore_2;

#line 523
                        bestAlignment_3 = bestAlignment_2;

#line 523
                        best_3 = best_2;

#line 523
                    }

#line 523
                    bestScore_2 = bestScore_3;

#line 523
                    bestAlignment_2 = bestAlignment_3;

#line 523
                    best_2 = best_3;

#line 503
                    item_0 = item_0 + 1U;

#line 503
                }

#line 499
                int x_2 = x_1 + int(1);

#line 499
                bestScore_1 = bestScore_2;

#line 499
                bestAlignment_1 = bestAlignment_2;

#line 499
                best_1 = best_2;

#line 499
                x_1 = x_2;

#line 499
            }

#line 498
            int y_2 = y_1 + int(1);

#line 498
            bestScore_0 = bestScore_1;

#line 498
            bestAlignment_0 = bestAlignment_1;

#line 498
            best_0 = best_1;

#line 498
            y_1 = y_2;

#line 498
        }

#line 497
        z_1 = z_1 + int(1);

#line 497
    }

#line 532
    return best_0;
}


#line 776 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
GiTierSample_0 gpWorldRadiance_0(const GiWorldHit_0 thread* hit_2, float3 direction_2, uint source_4, uint cacheFrame_0, KernelContext_0 thread* kernelContext_5)
{

#line 777
    thread GiTierSample_0 sample_2 = gpTierSample_0(source_4);
    if(!hit_2->hit_0)
    {

#line 779
        (&sample_2)->source_2 = 9U;

#line 779
        float3 _S25 = gpSky_0(direction_2, kernelContext_5);
        (&sample_2)->radiance_2 = _S25;
        (&sample_2)->confidence_3 = 1.0;
        (&sample_2)->hitDistance_0 = -1.0;
        return sample_2;
    }
    (&sample_2)->hitDistance_0 = hit_2->t_0;

#line 785
    uint _S26 = giFindCard_0(hit_2->position_0, hit_2->normal_0, kernelContext_5);

    if(_S26 != 4294967295U)
    {

#line 788
        uint _S27 = _S26 * 2U;

#line 788
        (&sample_2)->radiance_2 = (*(kernelContext_5->giScene_0->cardState_0+_S27)).xyz;
        uint last_0 = (as_type<uint>(((*(kernelContext_5->giScene_0->cardState_0+(_S27 + 1U))).w)));

#line 789
        float age_0;
        if(cacheFrame_0 > last_0)
        {

#line 790
            age_0 = float(cacheFrame_0 - last_0);

#line 790
        }
        else
        {

#line 790
            age_0 = 0.0;

#line 790
        }


        (&sample_2)->confidence_3 = hit_2->confidence_1 * (1.0 - gpClamp_0(age_0 / 240.0, 0.0, 1.0) * 0.60000002384185791);
        return sample_2;
    }
    (&sample_2)->confidence_3 = hit_2->confidence_1 * 0.20000000298023224;
    return sample_2;
}


#line 247 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
GrSample_0 grWorldRay_0(float3 position_2, float3 normal_4, float3 unit_0, float lift_0, float reach_0, KernelContext_0 thread* kernelContext_6)
{
    uint cacheFrame_1 = kernelContext_6->giProbes_0->constants_0[24U].w;
    uint _S28 = GiHardwareTier_source_0();

#line 250
    float height_0;

#line 250
    if(_S28 == 7U)
    {

#line 250
        height_0 = lift_0;

#line 250
    }
    else
    {

#line 250
        float _S29 = giFinestVoxel_0(kernelContext_6);

#line 250
        height_0 = max(lift_0, _S29);

#line 250
    }

#line 250
    GiWorldHit_0 _S30 = GiHardwareTier_trace_0(position_2 + normal_4 * float3(height_0) , unit_0, reach_0, kernelContext_6);

#line 250
    bool _S31;

    if(_S30.hit_0)
    {

#line 252
        _S31 = (dot(_S30.normal_0, unit_0)) > 0.0;

#line 252
    }
    else
    {

#line 252
        _S31 = false;

#line 252
    }

#line 252
    if(_S31)
    {

#line 253
        return grSample_0(float3(0.0, 0.0, 0.0), 0.0, 0U);
    }

#line 253
    thread GiWorldHit_0 _S32 = _S30;

#line 253
    GiTierSample_0 _S33 = gpWorldRadiance_0(&_S32, unit_0, _S28, cacheFrame_1, kernelContext_6);


    return grSample_0(_S33.radiance_2, _S33.confidence_3, _S33.source_2);
}


#line 49 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
uint spWidth_0(KernelContext_0 thread* kernelContext_7)
{

#line 49
    return kernelContext_7->giResolve_0->constants_2[40U].x;
}


#line 139
struct SpTaps_0
{
    array<uint, int(4)> index_0;
    array<float, int(4)> weight_0;
    uint count_0;
};


#line 51
uint spSpacing_0(KernelContext_0 thread* kernelContext_8)
{

#line 51
    return kernelContext_8->giResolve_0->constants_2[40U].z;
}


#line 53
uint spTilesX_0(KernelContext_0 thread* kernelContext_9)
{

)cy_msl"
    R"cy_msl(#line 53
    return kernelContext_9->giResolve_0->constants_2[41U].x;
}


#line 54
uint spTilesY_0(KernelContext_0 thread* kernelContext_10)
{

#line 54
    return kernelContext_10->giResolve_0->constants_2[41U].y;
}


#line 145
void spAxisTaps_0(uint coordinate_0, uint step_0, uint count_1, uint thread* base_0, float thread* fraction_0)
{

#line 146
    float g_0 = (float(coordinate_0) - float(step_0 / 2U)) / float(step_0);
    if(g_0 <= 0.0)
    {

#line 148
        *base_0 = 0U;
        *fraction_0 = 0.0;
        return;
    }
    uint _S34 = uint(g_0);

#line 152
    *base_0 = _S34;
    *fraction_0 = g_0 - float(_S34);
    if((*base_0 + 1U) >= count_1)
    {

#line 155
        *base_0 = count_1 - 1U;
        *fraction_0 = 0.0;

#line 154
    }



    return;
}


#line 160
SpTaps_0 spBilinearTaps_0(uint x_3, uint y_3, uint step_1, uint countX_0, uint countY_0)
{

#line 161
    thread SpTaps_0 taps_0;
    (&taps_0)->count_0 = 0U;

#line 162
    uint t_2 = 0U;
    for(;;)
    {

#line 163
        if(t_2 < 4U)
        {
        }
        else
        {

#line 163
            break;
        }

#line 164
        (&taps_0)->index_0[t_2] = 0U;
        (&taps_0)->weight_0[t_2] = 0.0;

#line 163
        t_2 = t_2 + 1U;

#line 163
    }



    thread uint bx_0;

    thread float fx_0;

    spAxisTaps_0(x_3, step_1, countX_0, &bx_0, &fx_0);

#line 168
    thread uint by_0;

    thread float fy_0;

    spAxisTaps_0(y_3, step_1, countY_0, &by_0, &fy_0);

#line 172
    uint j_0 = 0U;
    for(;;)
    {

#line 173
        if(j_0 < 2U)
        {
        }
        else
        {

#line 173
            break;
        }

#line 173
        uint i_0 = 0U;
        for(;;)
        {

#line 174
            if(i_0 < 2U)
            {
            }
            else
            {

#line 174
                break;
            }

#line 174
            float _S35;
            if(i_0 == 0U)
            {

#line 175
                _S35 = 1.0 - fx_0;

#line 175
            }
            else
            {

#line 175
                _S35 = fx_0;

#line 175
            }

#line 175
            float _S36;

#line 175
            if(j_0 == 0U)
            {

#line 175
                _S36 = 1.0 - fy_0;

#line 175
            }
            else
            {

#line 175
                _S36 = fy_0;

#line 175
            }

#line 175
            float weight_1 = _S35 * _S36;
            if(weight_1 <= 0.0)
            {

#line 177
                i_0 = i_0 + 1U;

#line 174
                continue;
            }



            (&taps_0)->index_0[(&taps_0)->count_0] = (by_0 + j_0) * countX_0 + (bx_0 + i_0);
            (&taps_0)->weight_0[(&taps_0)->count_0] = weight_1;
            (&taps_0)->count_0 = (&taps_0)->count_0 + 1U;

#line 174
            i_0 = i_0 + 1U;

#line 174
        }

#line 173
        j_0 = j_0 + 1U;

#line 173
    }

#line 184
    return taps_0;
}


#line 52
uint spCell_0(KernelContext_0 thread* kernelContext_11)
{

#line 52
    return kernelContext_11->giResolve_0->constants_2[40U].w;
}

uint spCellsX_0(KernelContext_0 thread* kernelContext_12)
{

#line 55
    return kernelContext_12->giResolve_0->constants_2[41U].z;
}


#line 56
uint spCellsY_0(KernelContext_0 thread* kernelContext_13)
{

#line 56
    return kernelContext_13->giResolve_0->constants_2[41U].w;
}


#line 55
uint spCellsX_1(KernelContext_0 thread* kernelContext_14)
{

#line 55
    return kernelContext_14->giResolve_0->constants_2[41U].z;
}


#line 56
uint spCellsY_1(KernelContext_0 thread* kernelContext_15)
{

#line 56
    return kernelContext_15->giResolve_0->constants_2[41U].w;
}


#line 80
uint spTableIndex_0(uint slot_0, uint cell_2, KernelContext_0 thread* kernelContext_16)
{

#line 80
    uint _S37 = spCellsX_1(kernelContext_16);
    uint _S38 = slot_0 * _S37;

#line 81
    uint _S39 = spCellsY_1(kernelContext_16);

#line 81
    return 4U + _S38 * _S39 + cell_2;
}


#line 57
uint spUniformCount_0(KernelContext_0 thread* kernelContext_17)
{

#line 57
    return kernelContext_17->giResolve_0->constants_2[43U].w;
}


#line 207
struct SpProbe_0
{
    float3 position_3;
    float depth_0;
    float3 normal_5;
    uint id_0;
};


#line 57
uint spUniformCount_1(KernelContext_0 thread* kernelContext_18)
{

#line 57
    return kernelContext_18->giResolve_0->constants_2[43U].w;
}


#line 58
uint spCapacity_0(KernelContext_0 thread* kernelContext_19)
{

#line 58
    return kernelContext_19->giResolve_0->constants_2[42U].z;
}


)cy_msl"
    R"cy_msl(#line 59
uint spRecords_0(KernelContext_0 thread* kernelContext_20)
{

#line 59
    uint _S40 = spUniformCount_1(kernelContext_20);

#line 59
    uint _S41 = spCapacity_0(kernelContext_20);

#line 59
    return _S40 + _S41;
}


#line 76
uint spRecordBase_0(uint slot_1, uint record_1, KernelContext_0 thread* kernelContext_21)
{

#line 76
    uint _S42 = spRecords_0(kernelContext_21);
    return (slot_1 * _S42 + record_1) * 11U;
}


#line 214
SpProbe_0 spLoadProbe_0(uint slot_2, uint record_2, KernelContext_0 thread* kernelContext_22)
{

#line 214
    uint _S43 = spRecordBase_0(slot_2, record_2, kernelContext_22);

    float4 device* _S44 = kernelContext_22->giResolve_0->probes_1+_S43;
    float4 device* _S45 = kernelContext_22->giResolve_0->probes_1+(_S43 + 1U);
    thread SpProbe_0 probe_0;
    (&probe_0)->position_3 = (*_S44).xyz;
    (&probe_0)->depth_0 = (*_S44).w;
    (&probe_0)->normal_5 = (*_S45).xyz;
    (&probe_0)->id_0 = (as_type<uint>(((*_S45).w)));
    return probe_0;
}


#line 119
float spGeometryWeight_0(float3 probePosition_0, float3 probeNormal_0, float3 position_4, float3 normal_6, float depth_1, float tolerance_0)
{
    float plane_0 = abs(dot(probeNormal_0, position_4 - probePosition_0));
    float reach_1 = tolerance_0 * depth_1;
    if(reach_1 <= 0.0)
    {

#line 124
        return 0.0;
    }

    float _S46 = max(0.0, dot(probeNormal_0, normal_6));
    float facing_0 = _S46 * _S46;
    float facing_1 = facing_0 * facing_0;

    return max(0.0, 1.0 - plane_0 / reach_1) * (facing_1 * facing_1);
}


#line 226
void spLoadCoefficients_0(uint slot_3, uint record_3, array<float3, int(9)> thread* coefficients_0, float thread* history_0, KernelContext_0 thread* kernelContext_23)
{

#line 226
    uint _S47 = spRecordBase_0(slot_3, record_3, kernelContext_23);
    uint _S48 = _S47 + 2U;
    thread array<float, int(28)> values_0;

#line 228
    uint v_1 = 0U;
    for(;;)
    {

#line 229
        if(v_1 < 7U)
        {
        }
        else
        {

#line 229
            break;
        }

#line 230
        float4 device* _S49 = kernelContext_23->giResolve_0->probes_1+(_S48 + v_1);
        uint _S50 = v_1 * 4U;

#line 231
        values_0[_S50] = (*_S49).x;
        values_0[_S50 + 1U] = (*_S49).y;
        values_0[_S50 + 2U] = (*_S49).z;
        values_0[_S50 + 3U] = (*_S49).w;

#line 229
        v_1 = v_1 + 1U;

#line 229
    }

#line 229
    uint k_0 = 0U;

#line 236
    for(;;)
    {

#line 236
        if(k_0 < 9U)
        {
        }
        else
        {

#line 236
            break;
        }

#line 237
        uint _S51 = k_0 * 3U;

#line 237
        (*coefficients_0)[k_0] = float3(values_0[_S51], values_0[_S51 + 1U], values_0[_S51 + 2U]);

#line 236
        k_0 = k_0 + 1U;

#line 236
    }


    *history_0 = values_0[int(27)];
    return;
}


#line 90
void spBasis_0(float3 d_0, array<float, int(9)> thread* basis_0)
{

#line 91
    (*basis_0)[int(0)] = 0.28209498524665833;
    float _S52 = d_0.y;

#line 92
    (*basis_0)[int(1)] = 0.48860299587249756 * _S52;
    float _S53 = d_0.z;

#line 93
    (*basis_0)[int(2)] = 0.48860299587249756 * _S53;
    float _S54 = d_0.x;

#line 94
    (*basis_0)[int(3)] = 0.48860299587249756 * _S54;
    (*basis_0)[int(4)] = 1.09254801273345947 * (_S54 * _S52);
    (*basis_0)[int(5)] = 1.09254801273345947 * (_S52 * _S53);
    (*basis_0)[int(6)] = 0.31539198756217957 * (3.0 * (_S53 * _S53) - 1.0);
    (*basis_0)[int(7)] = 1.09254801273345947 * (_S54 * _S53);
    (*basis_0)[int(8)] = 0.54627400636672974 * (_S54 * _S54 - _S52 * _S52);
    return;
}


#line 102
uint spBandOf_0(uint k_1)
{

#line 102
    uint _S55;
    if(k_1 == 0U)
    {

#line 103
        _S55 = 0U;

#line 103
    }
    else
    {

#line 103
        if(k_1 < 4U)
        {

#line 103
            _S55 = 1U;

#line 103
        }
        else
        {

#line 103
            _S55 = 2U;

#line 103
        }

#line 103
    }

#line 103
    return _S55;
}

float3 spEvaluate_0(const array<float3, int(9)> thread* coefficients_1, float3 direction_3, float3 band_0)
{

#line 107
    thread array<float, int(9)> basis_1;
    spBasis_0(direction_3, &basis_1);

#line 108
    float3 sum_0 = float3(0.0, 0.0, 0.0);

#line 108
    uint k_2 = 0U;

    for(;;)
    {

#line 110
        if(k_2 < 9U)
        {
        }
        else
        {

#line 110
            break;
        }

#line 111
        uint b_0 = spBandOf_0(k_2);

#line 111
        float scale_0;
        if(b_0 == 0U)
        {

#line 112
            scale_0 = band_0.x;

#line 112
        }
        else
        {

#line 112
            if(b_0 == 1U)
            {

#line 112
                scale_0 = band_0.y;

#line 112
            }
            else
            {

#line 112
                scale_0 = band_0.z;

#line 112
            }

#line 112
        }
        float3 _S56 = sum_0 + (*coefficients_1)[k_2] * float3((basis_1[k_2] * scale_0)) ;

#line 110
        uint k_3 = k_2 + 1U;

#line 110
        sum_0 = _S56;

#line 110
        k_2 = k_3;

#line 110
    }

#line 115
    return float3(max(sum_0.x, 0.0), max(sum_0.y, 0.0), max(sum_0.z, 0.0));
}


#line 133 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
struct GrCombine_0
{
    float3 total_0;
    float totalWeight_0;
    float confidence_4;
};


#line 139
GrCombine_0 grCombineBegin_0()
{

)cy_msl"
    R"cy_msl(#line 140
    thread GrCombine_0 state_0;
    (&state_0)->total_0 = float3(0.0, 0.0, 0.0);
    (&state_0)->totalWeight_0 = 0.0;
    (&state_0)->confidence_4 = 0.0;
    return state_0;
}

void grCombineAdd_0(GrCombine_0 thread* state_1, const GrSample_0 thread* sample_3, uint excluded_0)
{

#line 147
    uint _S57 = sample_3->source_0;
    if((sample_3->source_0) == 0U)
    {

#line 149
        return;
    }
    if((excluded_0 & (1U << _S57)) != 0U)
    {

#line 152
        return;
    }
    float weight_2 = gpClamp_0(sample_3->confidence_0, 0.0, 1.0);
    if(weight_2 <= 0.0)
    {

#line 156
        return;
    }
    state_1->total_0 = state_1->total_0 + sample_3->radiance_0 * float3(weight_2) ;
    state_1->totalWeight_0 = state_1->totalWeight_0 + weight_2;
    state_1->confidence_4 = max(state_1->confidence_4, weight_2);
    return;
}


#line 168
float grFarFieldWeight_0(float distance_0, KernelContext_0 thread* kernelContext_24)
{

#line 169
    float4 far_0 = (as_type<float4>((kernelContext_24->giResolve_0->constants_2[3U])));
    float begin_0 = far_0.x;
    if(begin_0 <= 0.0)
    {

#line 172
        return 1.0;
    }
    float _S58 = far_0.y;

#line 174
    float ratio_0;

#line 174
    if(_S58 > 0.0)
    {

#line 174
        ratio_0 = far_0.z / _S58;

#line 174
    }
    else
    {

#line 174
        ratio_0 = 8.0;

#line 174
    }
    float width_0 = begin_0 * gpClamp_0(ratio_0 / 100.0, 0.15000000596046448, 1.0);
    if(distance_0 <= begin_0)
    {

#line 177
        return 0.0;
    }
    float t_3 = gpClamp_0((distance_0 - begin_0) / width_0, 0.0, 1.0);
    return t_3 * t_3 * (3.0 - 2.0 * t_3);
}


#line 163
float3 grCombineEnd_0(const GrCombine_0 thread* state_2)
{

#line 163
    float _S59 = state_2->totalWeight_0;

#line 163
    float3 _S60;
    if((state_2->totalWeight_0) > 0.0)
    {

#line 164
        _S60 = state_2->total_0 / float3(_S59) ;

#line 164
    }
    else
    {

#line 164
        _S60 = float3(0.0, 0.0, 0.0);

#line 164
    }

#line 164
    return _S60;
}


#line 200
uint grReflectionRays_0(float roughness_0, KernelContext_0 thread* kernelContext_25)
{

#line 201
    uint4 words_0 = kernelContext_25->giResolve_0->constants_2[4U];
    float mirror_0 = (as_type<float>((words_0.x)));
    float cacheMin_0 = (as_type<float>((words_0.y)));
    uint _S61 = max(1U, words_0.z);
    if(roughness_0 <= mirror_0)
    {

#line 206
        return _S61;
    }
    if(roughness_0 < cacheMin_0)
    {

        float _S62 = float(_S61);


        return max(1U, uint(floor(_S62 + (1.0 - _S62) * gpClamp_0((roughness_0 - mirror_0) / max(cacheMin_0 - mirror_0, 0.00009999999747379), 0.0, 1.0) + 0.5)));
    }
    return 0U;
}


#line 73
uint grHash_0(uint pixel_0, uint frame_0, uint stream_0)
{

    uint hash_0 = ((pixel_0 * 2654435769U) ^ (frame_0 * 2246822507U)) ^ (stream_0 * 3266489909U);

    uint hash_1 = (hash_0 ^ (hash_0 >> 15U)) * 625341585U;

    return hash_1 ^ (hash_1 >> 13U);
}

float grUnit_0(uint hash_2)
{

#line 84
    return float(hash_2 & 16777215U) / 1.6777216e+07;
}


#line 109
bool grTraces_0(uint pixel_1, uint frame_1, float density_0)
{

#line 110
    if(density_0 >= 1.0)
    {

#line 111
        return true;
    }
    return (grUnit_0(grHash_0(pixel_1, frame_1, 5U))) < density_0;
}


#line 220
float3 grLobeDirection_0(float3 reflection_0, float3 normal_7, float roughness_1, uint index_1, uint count_2, uint rotation_0)
{
    float3 axis_0 = giNormalisedOr_0(reflection_0, normal_7);

#line 222
    bool _S63;
    if(count_2 <= 1U)
    {

#line 223
        _S63 = true;

#line 223
    }
    else
    {

#line 223
        _S63 = roughness_1 <= 0.0;

#line 223
    }

#line 223
    if(_S63)
    {

#line 224
        return axis_0;
    }

    float _S64 = float(index_1);

    float radius_1 = sqrt((_S64 + 0.5) / float(count_2)) * (gpClamp_0(roughness_1, 0.0, 1.0) * 0.69999998807907104);
    float angle_0 = 2.3999631404876709 * _S64 + float(rotation_0) * 0.6180340051651001;

#line 230
    float3 tangent_0;
    if((abs(axis_0.y)) < 0.99000000953674316)
    {

#line 231
        tangent_0 = cross(float3(0.0, 1.0, 0.0), axis_0);

#line 231
    }
    else
    {

#line 231
        tangent_0 = cross(float3(1.0, 0.0, 0.0), axis_0);

#line 231
    }

    float3 tangent_1 = giNormalisedOr_0(tangent_0, float3(1.0, 0.0, 0.0));



    float3 unit_1 = giNormalisedOr_0(axis_0 + tangent_1 * float3((radius_1 * cos(angle_0)))  + cross(axis_0, tangent_1) * float3((radius_1 * sin(angle_0))) , axis_0);
    float _S65 = dot(unit_1, normal_7);

#line 238
    if(_S65 < 0.0)
    {

#line 238
        tangent_0 = giNormalisedOr_0(unit_1 - normal_7 * float3((2.0 * _S65)) , axis_0);

#line 238
    }
    else
    {

#line 238
        tangent_0 = unit_1;

#line 238
    }

#line 238
    return tangent_0;
}


#line 263
GrSample_0 grGatherSample_0(const GrSample_0 thread* ray_1, const GrSample_0 thread* standIn_0, uint excluded_1)
{

#line 263
    uint _S66 = standIn_0->source_0;

#line 263
    bool hasStandIn_0;
    if((standIn_0->source_0) != 0U)
    {

#line 264
        hasStandIn_0 = (standIn_0->confidence_0) > 0.0;

#line 264
    }
    else
    {

#line 264
        hasStandIn_0 = false;

#line 264
    }

#line 264
    if(hasStandIn_0)
    {

#line 264
        hasStandIn_0 = (excluded_1 & (1U << _S66)) == 0U;

#line 264
    }
    else
    {

)cy_msl"
    R"cy_msl(#line 264
        hasStandIn_0 = false;

#line 264
    }

    thread GrSample_0 sample_4 = *ray_1;
    if((ray_1->source_0) == 0U)
    {

#line 268
        if(!hasStandIn_0)
        {

#line 269
            return *ray_1;
        }
        (&sample_4)->source_0 = _S66;
        (&sample_4)->radiance_0 = standIn_0->radiance_0;

#line 267
    }
    else
    {



        if(hasStandIn_0)
        {
            (&sample_4)->radiance_0 = standIn_0->radiance_0 + (ray_1->radiance_0 - standIn_0->radiance_0) * float3(gpClamp_0(ray_1->confidence_0, 0.0, 1.0)) ;

#line 273
        }

#line 267
    }

#line 277
    (&sample_4)->confidence_0 = 1.0;
    return sample_4;
}


#line 658 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
void spIntegrate_0(uint pixel_2, KernelContext_0 thread* kernelContext_26)
{

#line 659
    uint4 counts_0 = kernelContext_26->giResolve_0->constants_2[0U];
    uint _S67 = counts_0.z;

#line 660
    if(pixel_2 >= _S67)
    {

#line 661
        return;
    }
    uint _S68 = counts_0.w;

#line 663
    uint record_4 = (_S68 * _S67 + pixel_2) * 2U;
    float4 at_0 = kernelContext_26->giResolve_0->surfaces_0[record_4];
    float4 facing_2 = kernelContext_26->giResolve_0->surfaces_0[record_4 + 1U];
    float _S69 = at_0.w;

#line 666
    if(_S69 <= 0.0)
    {

#line 667
        uint _S70 = pixel_2 * 2U;

#line 667
        float4 _S71 = float4(0.0, 0.0, 0.0, 0.0);

#line 667
        *(kernelContext_26->giResolve_0->noisy_0+_S70) = _S71;
        *(kernelContext_26->giResolve_0->noisy_0+(_S70 + 1U)) = _S71;
        return;
    }

    uint frame_2 = kernelContext_26->giResolve_0->constants_2[18U].x;
    float3 position_5 = at_0.xyz;
    float3 normal_8 = giNormalisedOr_0(facing_2.xyz, float3(0.0, 1.0, 0.0));
    float roughness_2 = facing_2.w;
    float3 offset_1 = position_5 - (as_type<float3>((kernelContext_26->giResolve_0->constants_2[1U].xyz)));
    float3 _S72 = float3(0.0, 0.0, -1.0);

#line 677
    float3 incident_0 = giNormalisedOr_0(offset_1, _S72);

    float3 reflection_1 = giNormalisedOr_0(incident_0 - normal_8 * float3((2.0 * dot(incident_0, normal_8))) , normal_8);
    float rough_0 = gpClamp_0(roughness_2, 0.0, 1.0);
    float3 specularBand_0 = float3(1.0, 1.0 + -0.33333331346511841 * rough_0, 1.0 + -0.75 * rough_0);

    float3 irradianceBand_0 = float3(1.0, 0.66666668653488159, 0.25);
    float4 _S73 = (as_type<float4>((kernelContext_26->giResolve_0->constants_2[43U])));

#line 684
    uint _S74 = spWidth_0(kernelContext_26);
    uint _S75 = pixel_2 % _S74;

#line 685
    uint _S76 = spWidth_0(kernelContext_26);
    uint _S77 = pixel_2 / _S76;

    float3 _S78 = float3(0.0, 0.0, 0.0);

#line 688
    float nearestPlane_0 = 1.00000001504746622e+30;

#line 688
    float3 irradiance_0 = _S78;

#line 688
    float3 glossy_0 = _S78;

#line 688
    float3 cache_0 = _S78;

#line 688
    uint nearest_0 = 4294967295U;

#line 688
    uint pass_0 = 0U;

#line 688
    float cacheConfidence_0 = 0.0;

#line 688
    float openness_0 = 0.0;

#line 688
    float total_1 = 0.0;

#line 696
    for(;;)
    {

#line 696
        if(pass_0 < 2U)
        {
        }
        else
        {

#line 696
            break;
        }

#line 696
        SpTaps_0 _S79;

        if(pass_0 == 0U)
        {

#line 698
            uint _S80 = spSpacing_0(kernelContext_26);

#line 698
            uint _S81 = spTilesX_0(kernelContext_26);

#line 698
            uint _S82 = spTilesY_0(kernelContext_26);

#line 698
            _S79 = spBilinearTaps_0(_S75, _S77, _S80, _S81, _S82);

#line 698
        }
        else
        {

#line 698
            uint _S83 = spCell_0(kernelContext_26);

#line 698
            uint _S84 = spCellsX_0(kernelContext_26);

#line 698
            uint _S85 = spCellsY_0(kernelContext_26);

#line 698
            _S79 = spBilinearTaps_0(_S75, _S77, _S83, _S84, _S85);

#line 698
        }

#line 698
        float nearestPlane_1 = nearestPlane_0;

#line 698
        float3 irradiance_1 = irradiance_0;

#line 698
        float3 glossy_1 = glossy_0;

#line 698
        float3 cache_1 = cache_0;

#line 698
        uint nearest_1 = nearest_0;

#line 698
        uint tap_0 = 0U;

#line 698
        float cacheConfidence_1 = cacheConfidence_0;

#line 698
        float openness_1 = openness_0;

#line 698
        float total_2 = total_1;

        for(;;)
        {

#line 700
            if(tap_0 < (_S79.count_0))
            {
            }
            else
            {

#line 700
                break;
            }

#line 698
            SpTaps_0 _S86 = _S79;

            uint _S87 = tap_0;

#line 700
            uint index_2;

            if(pass_0 == 1U)
            {

#line 702
                uint _S88 = spTableIndex_0(_S68, _S86.index_0[_S87], kernelContext_26);
                uint device* _S89 = kernelContext_26->giResolve_0->slots_0+_S88;

#line 703
                uint taken_0 = *_S89;
                if((*_S89) == 4294967295U)
                {

#line 705
                    tap_0 = tap_0 + 1U;

#line 700
                    continue;
                }

#line 700
                uint _S90 = spUniformCount_0(kernelContext_26);

#line 700
                index_2 = _S90 + taken_0;

            }
            else
            {

#line 702
                index_2 = _S86.index_0[_S87];

#line 702
            }

#line 702
            SpProbe_0 _S91 = spLoadProbe_0(_S68, index_2, kernelContext_26);

#line 710
            if((_S91.depth_0) <= 0.0)
            {

#line 711
                tap_0 = tap_0 + 1U;

#line 700
                continue;
            }

#line 713
            float plane_1 = abs(dot(_S91.normal_5, position_5 - _S91.position_3));

#line 713
            float nearestPlane_2;

#line 713
            uint nearest_2;
            if(plane_1 < nearestPlane_1)
            {

#line 714
                nearestPlane_2 = plane_1;

)cy_msl"
    R"cy_msl(#line 714
                nearest_2 = index_2;

#line 714
            }
            else
            {

#line 714
                nearestPlane_2 = nearestPlane_1;

#line 714
                nearest_2 = nearest_1;

#line 714
            }

#line 719
            float weight_3 = _S79.weight_0[tap_0] * spGeometryWeight_0(_S91.position_3, _S91.normal_5, position_5, normal_8, _S69, _S73.y);

            if(weight_3 <= 0.0)
            {

#line 721
                nearestPlane_1 = nearestPlane_2;

#line 721
                nearest_1 = nearest_2;
                tap_0 = tap_0 + 1U;

#line 700
                continue;
            }

#line 724
            thread array<float3, int(9)> coefficients_2;
            thread float history_1;

#line 725
            spLoadCoefficients_0(_S68, index_2, &coefficients_2, &history_1, kernelContext_26);

#line 725
            uint _S92 = spRecordBase_0(_S68, index_2, kernelContext_26);


            float4 device* _S93 = kernelContext_26->giResolve_0->probes_1+(_S92 + 9U);

#line 728
            thread array<float3, int(9)> _S94 = coefficients_2;

#line 728
            float3 _S95 = spEvaluate_0(&_S94, normal_8, irradianceBand_0);

#line 728
            float3 _S96 = float3(weight_3) ;
            float3 _S97 = irradiance_1 + _S95 * _S96;

#line 729
            thread array<float3, int(9)> _S98 = coefficients_2;

#line 729
            float3 _S99 = spEvaluate_0(&_S98, reflection_1, specularBand_0);
            float3 _S100 = glossy_1 + _S99 * _S96;
            float3 _S101 = cache_1 + (*_S93).xyz * _S96;
            float cacheConfidence_2 = cacheConfidence_1 + (*_S93).w * weight_3;
            float openness_2 = openness_1 + (*(kernelContext_26->giResolve_0->probes_1+(_S92 + 10U))).x * weight_3;
            float total_3 = total_2 + weight_3;

#line 734
            nearestPlane_1 = nearestPlane_2;

#line 734
            irradiance_1 = _S97;

#line 734
            glossy_1 = _S100;

#line 734
            cache_1 = _S101;

#line 734
            nearest_1 = nearest_2;

#line 734
            cacheConfidence_1 = cacheConfidence_2;

#line 734
            openness_1 = openness_2;

#line 734
            total_2 = total_3;

#line 700
            tap_0 = tap_0 + 1U;

#line 700
        }

#line 696
        uint pass_1 = pass_0 + 1U;

#line 696
        nearestPlane_0 = nearestPlane_1;

#line 696
        irradiance_0 = irradiance_1;

#line 696
        glossy_0 = glossy_1;

#line 696
        cache_0 = cache_1;

#line 696
        nearest_0 = nearest_1;

#line 696
        pass_0 = pass_1;

#line 696
        cacheConfidence_0 = cacheConfidence_1;

#line 696
        openness_0 = openness_1;

#line 696
        total_1 = total_2;

#line 696
    }

#line 737
    if(total_1 < 0.00009999999747379)
    {

#line 738
        if(nearest_0 == 4294967295U)
        {
            uint _S102 = pixel_2 * 2U;

#line 740
            float4 _S103 = float4(0.0, 0.0, 0.0, (as_type<float>((kernelContext_26->giResolve_0->constants_2[44U].x))));

#line 740
            *(kernelContext_26->giResolve_0->noisy_0+_S102) = _S103;
            *(kernelContext_26->giResolve_0->noisy_0+(_S102 + 1U)) = _S103;
            return;
        }
        thread array<float3, int(9)> coefficients_3;
        thread float history_2;

#line 745
        spLoadCoefficients_0(_S68, nearest_0, &coefficients_3, &history_2, kernelContext_26);

#line 745
        uint _S104 = spRecordBase_0(_S68, nearest_0, kernelContext_26);

#line 745
        thread array<float3, int(9)> _S105 = coefficients_3;

#line 745
        float3 _S106 = spEvaluate_0(&_S105, normal_8, irradianceBand_0);

#line 745
        thread array<float3, int(9)> _S107 = coefficients_3;

#line 745
        float3 _S108 = spEvaluate_0(&_S107, reflection_1, specularBand_0);

#line 750
        uint _S109 = _S104 + 9U;
        float _S110 = (*(kernelContext_26->giResolve_0->probes_1+_S109)).w;
        float _S111 = (*(kernelContext_26->giResolve_0->probes_1+(_S104 + 10U))).x;

#line 752
        cache_0 = (*(kernelContext_26->giResolve_0->probes_1+_S109)).xyz;

#line 752
        irradiance_0 = _S106;

#line 752
        glossy_0 = _S108;

#line 752
        cacheConfidence_0 = _S110;

#line 752
        openness_0 = _S111;

#line 737
    }
    else
    {

#line 754
        float3 _S112 = irradiance_0 / float3(total_1) ;
        float3 _S113 = glossy_0 / float3(total_1) ;

        float cacheConfidence_3 = cacheConfidence_0 / total_1;
        float openness_3 = openness_0 / total_1;

#line 758
        cache_0 = cache_0 / float3(total_1) ;

#line 758
        irradiance_0 = _S112;

#line 758
        glossy_0 = _S113;

#line 758
        cacheConfidence_0 = cacheConfidence_3;

#line 758
        openness_0 = openness_3;

#line 737
    }

#line 761
    uint4 shape_0 = kernelContext_26->giResolve_0->constants_2[42U];
    uint excluded_2 = kernelContext_26->giResolve_0->constants_2[2U].w;
    GrCombine_0 _S114 = grCombineBegin_0();

#line 763
    thread GrCombine_0 state_3 = _S114;
    if(((shape_0.w) & 1U) != 0U)
    {

#line 764
        thread GrSample_0 _S115 = grSample_0(cache_0, cacheConfidence_0, 3U);

#line 764
        grCombineAdd_0(&state_3, &_S115, excluded_2);

#line 764
    }


    uint _S116 = GiHardwareTier_source_0();

#line 767
    thread GrSample_0 _S117 = grSample_0(irradiance_0, 1.0, _S116);

#line 767
    grCombineAdd_0(&state_3, &_S117, excluded_2);
    thread GrSample_0 sky_0 = grSample_0(_S78, 0.0, 9U);

#line 768
    float _S118 = grFarFieldWeight_0(length(offset_1), kernelContext_26);

    if(_S118 > 0.0)
    {

#line 770
        float3 _S119 = gpSky_0(normal_8, kernelContext_26);
        (&sky_0)->radiance_0 = _S119 * float3(openness_0) ;
        (&sky_0)->confidence_0 = gpClamp_0(_S118 * openness_0, 0.0, 1.0);

#line 770
    }

#line 770
    thread GrSample_0 _S120 = sky_0;

#line 770
    grCombineAdd_0(&state_3, &_S120, excluded_2);

#line 770
    thread GrCombine_0 _S121 = state_3;

#line 770
    float3 _S122 = grCombineEnd_0(&_S121);

#line 775
    float3 diffuse_0 = _S122 * float3((as_type<float>((kernelContext_26->giResolve_0->constants_2[5U].x)))) ;

    GrSample_0 probeSpecular_0 = grSample_0(glossy_0, gpClamp_0(roughness_2, 0.15000000596046448, 1.0), _S116);
    thread GrCombine_0 reflected_0 = _S114;

#line 778
    uint _S123 = grReflectionRays_0(roughness_2, kernelContext_26);

    float density_1 = (as_type<float>((kernelContext_26->giResolve_0->constants_2[1U].w)));

#line 780
    bool _S124;
    if(_S123 > 0U)
    {

#line 781
        _S124 = grTraces_0(pixel_2, frame_2, density_1);

#line 781
    }
    else
    {

#line 781
        _S124 = false;

)cy_msl"
    R"cy_msl(#line 781
    }

#line 781
    if(_S124)
    {

#line 782
        float4 rays_1 = (as_type<float4>((kernelContext_26->giResolve_0->constants_2[2U])));


        uint _S125 = grHash_0(pixel_2, frame_2, 7U) % _S123;

#line 785
        GrSample_0 _S126 = grWorldRay_0(position_5, normal_8, giNormalisedOr_0(grLobeDirection_0(reflection_1, normal_8, roughness_2, _S125, _S123, frame_2), _S72), rays_1.y, rays_1.x, kernelContext_26);

#line 785
        thread GrSample_0 _S127 = _S126;

#line 785
        thread GrSample_0 _S128 = probeSpecular_0;

#line 785
        GrSample_0 _S129 = grGatherSample_0(&_S127, &_S128, 0U);

#line 785
        thread GrSample_0 _S130 = _S129;

#line 785
        grCombineAdd_0(&reflected_0, &_S130, 0U);

#line 781
    }

#line 781
    thread GrSample_0 _S131 = probeSpecular_0;

#line 781
    grCombineAdd_0(&reflected_0, &_S131, 0U);

#line 781
    thread GrCombine_0 _S132 = reflected_0;

#line 781
    float3 _S133 = grCombineEnd_0(&_S132);

#line 793
    float3 specular_0 = _S133 * float3((as_type<float>((kernelContext_26->giResolve_0->constants_2[5U].y)))) ;
    float samples_1 = (as_type<float>((kernelContext_26->giResolve_0->constants_2[44U].x)));
    uint _S134 = pixel_2 * 2U;

#line 795
    *(kernelContext_26->giResolve_0->noisy_0+_S134) = float4(diffuse_0, samples_1);
    *(kernelContext_26->giResolve_0->noisy_0+(_S134 + 1U)) = float4(specular_0, samples_1);
    return;
}


#line 23 "src/rendering/gi_gpu/shaders/gi_screen_probes_hardware.slang"
[[kernel]] void cyGiProbeIntegrateHardware(uint3 thread_0 [[thread_position_in_grid]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiResolveSet_default_0 constant* giResolve_1 [[buffer(3)]], GiHardwareSet_default_0 constant* giHardware_1 [[buffer(4)]])
{

#line 23
    thread KernelContext_0 kernelContext_27;

#line 23
    (&kernelContext_27)->giProbes_0 = giProbes_1;

#line 23
    (&kernelContext_27)->giScene_0 = giScene_1;

#line 23
    (&kernelContext_27)->giResolve_0 = giResolve_1;

#line 23
    (&kernelContext_27)->giHardware_0 = giHardware_1;

#line 23
    spIntegrate_0(thread_0.x, &kernelContext_27);

    return;
}

)cy_msl";

}  // namespace cy::rendering::gi_gpu

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

/// cyGiDenoiseAccumulate.metal, 6786 bytes.
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


#line 206
float gdHistoryKept_0(float3 history_1, float3 value_2, uint signal_1, KernelContext_0 thread* kernelContext_3)
{

#line 207
    uint4 words_0 = kernelContext_3->giDenoise_0->constants_0[20U + signal_1 * 3U + 2U];
    float rejection_0 = (as_type<float>((words_0.z)));
    if(rejection_0 <= 0.0)
    {

#line 210
        return 1.0;
    }
    uint _S4 = words_0.y;

#line 212
    float before_0 = gdScalar_0(history_1, _S4);
    float now_0 = gdScalar_0(value_2, _S4);
    float deviation_0 = abs(now_0 - before_0);
    float allowed_0 = rejection_0 * max(abs(now_0), abs(before_0));
    if(deviation_0 <= allowed_0)
    {

#line 217
        return 1.0;
    }
    float ratio_0 = allowed_0 / deviation_0;
    return ratio_0 * ratio_0;
}



[[kernel]] void cyGiDenoiseAccumulate(uint3 thread_0 [[thread_position_in_grid]], GiDenoiseSet_default_0 constant* giDenoise_1 [[buffer(0)]], GiDenoisePush_0 constant* gdPush_1 [[buffer(1)]])
{

#line 225
    thread KernelContext_0 kernelContext_4;

#line 225
    (&kernelContext_4)->giDenoise_0 = giDenoise_1;

#line 225
    (&kernelContext_4)->gdPush_0 = gdPush_1;

#line 225
    uint _S5 = gdPixels_0(&kernelContext_4);

    uint _S6 = thread_0.x;

#line 227
    if(_S6 >= ((&kernelContext_4)->gdPush_0->count_0))
    {

#line 228
        return;
    }
    uint signal_2 = _S6 / _S5;
    uint pixel_1 = _S6 - signal_2 * _S5;
    uint _S7 = 20U + signal_2 * 3U;
    uint domain_1 = (&kernelContext_4)->giDenoise_0->constants_0[_S7 + 2U].y;
    float historyLength_0 = (as_type<float>(((&kernelContext_4)->giDenoise_0->constants_0[_S7 + 1U].y)));

#line 234
    float alphaFloor_0;
    if(historyLength_0 > 0.0)
    {

#line 235
        alphaFloor_0 = 1.0 / historyLength_0;

#line 235
    }
    else
    {

#line 235
        alphaFloor_0 = 1.0;

#line 235
    }
    uint current_0 = (&kernelContext_4)->giDenoise_0->constants_0[0U].w;

    uint _S8 = pixel_1 * 2U;

#line 238
    float4 noisy_1 = (&kernelContext_4)->giDenoise_0->noisy_0[_S8 + signal_2];
    float3 value_3 = noisy_1.xyz;
    float scalar_0 = gdScalar_0(value_3, domain_1);
    float fresh_0 = noisy_1.w;

#line 241
    float confidence_0;

#line 241
    bool _S9;

#line 241
    int source_0;



    if(((&kernelContext_4)->giDenoise_0->constants_0[5U].w) != 0U)
    {

#line 246
        int source_1 = (as_type<int>((*((&kernelContext_4)->giDenoise_0->sources_0+_S8))));
        if(source_1 >= int(0))
        {

#line 247
            _S9 = uint(source_1) < _S5;

#line 247
        }
        else
        {

#line 247
            _S9 = false;

#line 247
        }

#line 247
        if(_S9)
        {

#line 248
            float _S10 = gdClamp_0((as_type<float>((*((&kernelContext_4)->giDenoise_0->sources_0+(_S8 + 1U))))), 0.0, 1.0);

#line 248
            source_0 = source_1;

#line 248
            confidence_0 = _S10;

#line 247
        }
        else
        {

#line 247
            source_0 = int(-1);

#line 247
            confidence_0 = 0.0;

#line 247
        }

#line 245
    }
    else
    {

#line 245
        source_0 = int(-1);

#line 245
        confidence_0 = 0.0;

#line 245
    }

#line 245
    uint _S11 = gdHistoryRecord_0(current_0, signal_2, pixel_1, &kernelContext_4);

#line 255
    if(source_0 < int(0))
    {

#line 255
        _S9 = true;

)cy_msl"
    R"cy_msl(#line 255
    }
    else
    {

#line 255
        _S9 = confidence_0 <= 0.0;

#line 255
    }

#line 255
    if(_S9)
    {

#line 256
        *((&kernelContext_4)->giDenoise_0->history_0+_S11) = float4(value_3, fresh_0);
        *((&kernelContext_4)->giDenoise_0->history_0+(_S11 + 1U)) = float4(scalar_0, scalar_0 * scalar_0, 0.0, 0.0);
        return;
    }

#line 258
    uint _S12 = gdHistoryRecord_0(1U - current_0, signal_2, uint(source_0), &kernelContext_4);


    float4 device* _S13 = (&kernelContext_4)->giDenoise_0->history_0+_S12;
    float4 moments_0 = *((&kernelContext_4)->giDenoise_0->history_0+(_S12 + 1U));
    float _S14 = (*_S13).w * confidence_0;

#line 263
    float3 _S15 = (*_S13).xyz;

#line 263
    float _S16 = gdHistoryKept_0(_S15, value_3, signal_2, &kernelContext_4);
    float count_1 = _S14 * _S16 + fresh_0;
    if(count_1 > 0.0)
    {

#line 265
        confidence_0 = fresh_0 / count_1;

#line 265
    }
    else
    {

#line 265
        confidence_0 = 1.0;

#line 265
    }

#line 265
    float _S17 = max(confidence_0, alphaFloor_0);

    float _S18 = moments_0.x;

#line 267
    float first_0 = _S18 + (scalar_0 - _S18) * _S17;
    float _S19 = moments_0.y;

#line 268
    float second_0 = _S19 + (scalar_0 * scalar_0 - _S19) * _S17;
    *((&kernelContext_4)->giDenoise_0->history_0+_S11) = float4(_S15 + (value_3 - _S15) * float3(_S17) , min(count_1, historyLength_0));
    *((&kernelContext_4)->giDenoise_0->history_0+(_S11 + 1U)) = float4(first_0, second_0, 0.0, 0.0);
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


#line 277
[[kernel]] void cyGiDenoiseVariance(uint3 thread_0 [[thread_position_in_grid]], GiDenoiseSet_default_0 constant* giDenoise_1 [[buffer(0)]], GiDenoisePush_0 constant* gdPush_1 [[buffer(1)]])
{

#line 277
    thread KernelContext_0 kernelContext_3;

#line 277
    (&kernelContext_3)->giDenoise_0 = giDenoise_1;

#line 277
    (&kernelContext_3)->gdPush_0 = gdPush_1;

#line 277
    uint _S5 = gdPixels_0(&kernelContext_3);

    uint _S6 = thread_0.x;

#line 279
    if(_S6 >= ((&kernelContext_3)->gdPush_0->count_0))
    {

#line 280
        return;
    }
    uint signal_1 = _S6 / _S5;
    uint _S7 = signal_1 * _S5;

#line 283
    uint pixel_1 = _S6 - _S7;
    uint4 counts_0 = (&kernelContext_3)->giDenoise_0->constants_0[0U];
    uint current_0 = counts_0.w;

#line 285
    uint _S8 = gdHistoryRecord_0(current_0, signal_1, pixel_1, &kernelContext_3);

    float4 device* _S9 = (&kernelContext_3)->giDenoise_0->history_0+_S8;

#line 287
    float4 colour_0 = *_S9;
    float4 device* _S10 = (&kernelContext_3)->giDenoise_0->history_0+(_S8 + 1U);
    float variance_1 = gdStableVariance_0((*_S10).x, (*_S10).y);

#line 289
    float variance_2;
    if(((*_S9).w) < 4.0)
    {

        uint _S11 = counts_0.x;

#line 293
        uint _S12 = pixel_1 % _S11;

#line 293
        int _S13 = int(_S12);
        uint _S14 = pixel_1 / _S11;

#line 294
        int _S15 = int(_S14);

#line 294
        int dy_0 = int(-1);

#line 294
        variance_2 = 0.0;

#line 294
        float squares_0 = 0.0;

#line 294
        float taken_0 = 0.0;



        for(;;)
        {

#line 298
            if(dy_0 <= int(1))
            {
            }
            else
            {

#line 298
                break;
            }

#line 298
            int dx_0 = int(-1);

#line 298
            float sum_0 = variance_2;

#line 298
            float squares_1 = squares_0;

#line 298
            float taken_1 = taken_0;
            for(;;)
            {

#line 299
                if(dx_0 <= int(1))
                {
                }
                else
                {

#line 299
                    break;
                }

#line 300
                int sx_0 = _S13 + dx_0;
                int sy_0 = _S15 + dy_0;

#line 301
                bool _S16;
                if(sx_0 < int(0))
                {

#line 302
                    _S16 = true;

#line 302
                }
                else
                {

#line 302
                    _S16 = sy_0 < int(0);

#line 302
                }

#line 302
                bool _S17;

#line 302
                if(_S16)
                {

#line 302
                    _S17 = true;

#line 302
                }
                else
                {

#line 302
                    _S17 = sx_0 >= int(_S11);

#line 302
                }

#line 302
                bool _S18;

#line 302
                if(_S17)
                {

#line 302
                    _S18 = true;

#line 302
                }
                else
                {

#line 302
                    _S18 = sy_0 >= int(counts_0.y);

#line 302
                }

#line 302
                if(_S18)
                {

#line 303
                    dx_0 = dx_0 + int(1);

#line 299
                    continue;
                }

#line 299
                uint _S19 = gdHistoryRecord_0(current_0, signal_1, uint(sy_0) * _S11 + uint(sx_0), &kernelContext_3);

#line 306
                float value_0 = (*((&kernelContext_3)->giDenoise_0->history_0+(_S19 + 1U))).x;

                float squares_2 = squares_1 + value_0 * value_0;
                float taken_2 = taken_1 + 1.0;

#line 309
                sum_0 = sum_0 + value_0;

)cy_msl"
    R"cy_msl(#line 309
                squares_1 = squares_2;

#line 309
                taken_1 = taken_2;

#line 299
                dx_0 = dx_0 + int(1);

#line 299
            }

#line 298
            dy_0 = dy_0 + int(1);

#line 298
            variance_2 = sum_0;

#line 298
            squares_0 = squares_1;

#line 298
            taken_0 = taken_1;

#line 298
        }

#line 298
        variance_2 = max(variance_1, gdStableVariance_0(variance_2 / taken_0, squares_0 / taken_0));

#line 290
    }
    else
    {

#line 290
        variance_2 = variance_1;

#line 290
    }

#line 315
    uint _S20 = _S7 + pixel_1;

#line 315
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


#line 322
float4 gdFilterRead_0(uint index_0, KernelContext_0 thread* kernelContext_2)
{

#line 322
    float4 _S1;
    if((kernelContext_2->gdPush_0->direction_0) == 0U)
    {

#line 323
        _S1 = *(kernelContext_2->giDenoise_0->filterA_0+index_0);

#line 323
    }
    else
    {

#line 323
        _S1 = *(kernelContext_2->giDenoise_0->filterB_0+index_0);

#line 323
    }

#line 323
    return _S1;
}

void gdFilterWrite_0(uint index_1, float4 value_0, KernelContext_0 thread* kernelContext_3)
{

#line 327
    if((kernelContext_3->gdPush_0->direction_0) == 0U)
    {

#line 328
        *(kernelContext_3->giDenoise_0->filterB_0+index_1) = value_0;

#line 327
    }
    else
    {
        *(kernelContext_3->giDenoise_0->filterA_0+index_1) = value_0;

#line 327
    }

#line 332
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


#line 336
[[kernel]] void cyGiDenoiseFilter(uint3 thread_0 [[thread_position_in_grid]], GiDenoiseSet_default_0 constant* giDenoise_1 [[buffer(0)]], GiDenoisePush_0 constant* gdPush_1 [[buffer(1)]])
{

#line 336
    thread KernelContext_0 kernelContext_6;

#line 336
    (&kernelContext_6)->giDenoise_0 = giDenoise_1;

#line 336
    (&kernelContext_6)->gdPush_0 = gdPush_1;

#line 336
    uint _S5 = gdPixels_0(&kernelContext_6);

    uint _S6 = thread_0.x;

#line 338
    if(_S6 >= ((&kernelContext_6)->gdPush_0->count_0))
    {

#line 339
        return;
    }
    uint signal_1 = _S6 / _S5;
    uint _S7 = signal_1 * _S5;

#line 342
    uint pixel_2 = _S6 - _S7;
    uint4 counts_1 = (&kernelContext_6)->giDenoise_0->constants_0[0U];

    uint _S8 = _S7 + pixel_2;

#line 345
    float4 _S9 = gdFilterRead_0(_S8, &kernelContext_6);
    uint _S10 = 20U + signal_1 * 3U;

#line 346
    uint4 shape_0 = (&kernelContext_6)->giDenoise_0->constants_0[_S10 + 1U];


    if(((&kernelContext_6)->gdPush_0->pass_0) >= (shape_0.z))
    {

#line 349
        gdFilterWrite_0(_S8, _S9, &kernelContext_6);

        return;
    }

#line 351
    uint _S11 = gdSurfaceRecord_0(pixel_2, &kernelContext_6);


    float depth_0 = (&kernelContext_6)->giDenoise_0->surfaces_0[_S11].w;
    if(depth_0 <= 0.0)
    {

#line 355
        gdFilterWrite_0(_S8, _S9, &kernelContext_6);

        return;
    }
    float4 _S12 = (as_type<float4>(((&kernelContext_6)->giDenoise_0->constants_0[_S10])));
    uint domain_1 = (&kernelContext_6)->giDenoise_0->constants_0[_S10 + 2U].y;
    float centreValue_0 = gdScalar_0(_S9.xyz, domain_1);
    float deviation_0 = sqrt(*((&kernelContext_6)->giDenoise_0->variance_0+_S8));

#line 362
    uint _S13 = gdHistoryRecord_0(counts_1.w, signal_1, pixel_2, &kernelContext_6);

#line 362
    bool _S14;

    if(((*((&kernelContext_6)->giDenoise_0->history_0+_S13)).w) >= 12.0)
    {

)cy_msl"
    R"cy_msl(#line 364
        _S14 = deviation_0 <= (0.01999999955296516 * (abs(centreValue_0) + 9.99999997475242708e-07));

#line 364
    }
    else
    {

#line 364
        _S14 = false;

#line 364
    }

#line 364
    if(_S14)
    {

#line 364
        gdFilterWrite_0(_S8, _S9, &kernelContext_6);


        return;
    }

#line 367
    uint _S15 = gdSurfaceRecord_0(pixel_2, &kernelContext_6);

    float4 facing_0 = (&kernelContext_6)->giDenoise_0->surfaces_0[_S15 + 1U];
    float roughness_0 = facing_0.w;
    float3 _S16 = facing_0.xyz;


    int _S17 = int(max(1U, uint(floor(float(1U << ((&kernelContext_6)->gdPush_0->pass_0)) * (1.0 + (as_type<float>((shape_0.x))) * roughness_0) + 0.5))));
    int extent_0 = int(shape_0.w);
    uint _S18 = counts_1.x;

#line 376
    uint _S19 = pixel_2 % _S18;

#line 376
    int _S20 = int(_S19);
    uint _S21 = pixel_2 / _S18;

#line 377
    int _S22 = int(_S21);



    int _S23 = - extent_0;

#line 381
    float3 sum_0 = float3(0.0, 0.0, 0.0);

#line 381
    int dy_0 = _S23;

#line 381
    float weightSum_0 = 0.0;

#line 381
    for(;;)
    {

#line 381
        if(dy_0 <= extent_0)
        {
        }
        else
        {

#line 381
            break;
        }

#line 381
        float3 sum_1 = sum_0;

#line 381
        int dx_0 = _S23;

#line 381
        float weightSum_1 = weightSum_0;
        for(;;)
        {

#line 382
            if(dx_0 <= extent_0)
            {
            }
            else
            {

#line 382
                break;
            }

#line 383
            int sx_0 = _S20 + dx_0 * _S17;
            int sy_0 = _S22 + dy_0 * _S17;
            if(sx_0 < int(0))
            {

#line 385
                _S14 = true;

#line 385
            }
            else
            {

#line 385
                _S14 = sy_0 < int(0);

#line 385
            }

#line 385
            bool _S24;

#line 385
            if(_S14)
            {

#line 385
                _S24 = true;

#line 385
            }
            else
            {

#line 385
                _S24 = sx_0 >= int(_S18);

#line 385
            }

#line 385
            bool _S25;

#line 385
            if(_S24)
            {

#line 385
                _S25 = true;

#line 385
            }
            else
            {

#line 385
                _S25 = sy_0 >= int(counts_1.y);

#line 385
            }

#line 385
            if(_S25)
            {

#line 386
                dx_0 = dx_0 + int(1);

#line 382
                continue;
            }

#line 388
            uint tap_0 = uint(sy_0) * _S18 + uint(sx_0);

#line 388
            uint _S26 = gdSurfaceRecord_0(tap_0, &kernelContext_6);

            float tapDepth_0 = (&kernelContext_6)->giDenoise_0->surfaces_0[_S26].w;
            if(tapDepth_0 <= 0.0)
            {

#line 392
                dx_0 = dx_0 + int(1);

#line 382
                continue;
            }

#line 397
            float4 tapFacing_0 = (&kernelContext_6)->giDenoise_0->surfaces_0[_S26 + 1U];


            float weight_0 = gdSpline_0(dx_0) * gdSpline_0(dy_0) * exp(- abs(tapDepth_0 - depth_0) / (_S12.x * abs(depth_0) + 9.99999997475242708e-07)) * pow(max(0.0, dot(_S16, tapFacing_0.xyz)), _S12.y) * exp(- abs(tapFacing_0.w - roughness_0) / (_S12.w + 9.99999997475242708e-07));

#line 400
            float4 _S27 = gdFilterRead_0(_S7 + tap_0, &kernelContext_6);

            float3 _S28 = _S27.xyz;

            float _S29 = max(weight_0 * exp(- abs(gdScalar_0(_S28, domain_1) - centreValue_0) / (_S12.z * deviation_0 + 9.99999997475242708e-07)), 0.0);
            if(_S29 <= 0.0)
            {

#line 406
                dx_0 = dx_0 + int(1);

#line 382
                continue;
            }

#line 409
            float weightSum_2 = weightSum_1 + _S29;

#line 409
            sum_1 = sum_1 + _S28 * float3(_S29) ;

#line 409
            weightSum_1 = weightSum_2;

#line 382
            dx_0 = dx_0 + int(1);

#line 382
        }

#line 381
        int dy_1 = dy_0 + int(1);

#line 381
        sum_0 = sum_1;

#line 381
        dy_0 = dy_1;

#line 381
        weightSum_0 = weightSum_1;

#line 381
    }

#line 381
    float4 _S30;

#line 412
    if(weightSum_0 > 0.0)
    {

#line 412
        _S30 = float4(sum_0 / float3(weightSum_0) , 0.0);

#line 412
    }
    else
    {

#line 412
        _S30 = _S9;

#line 412
    }

#line 412
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

/// cyGiDenoiseTemporal.metal, 7116 bytes.
inline constexpr char kGiDenoiseTemporalMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 53 "src/rendering/gi_gpu/shaders/gi_temporal.slang"
float gtClamp_0(float value_0, float lo_0, float hi_0)
{

#line 53
    float _S1;
    if(value_0 < lo_0)
    {

#line 54
        _S1 = lo_0;

#line 54
    }
    else
    {

#line 54
        if(value_0 > hi_0)
        {

#line 54
            _S1 = hi_0;

#line 54
        }
        else
        {

#line 54
            _S1 = value_0;

#line 54
        }

#line 54
    }

#line 54
    return _S1;
}


#line 5163 "hlsl.meta.slang"
struct GiTemporalSet_default_0
{
    uint4 device* constants_0;
    float4 device* noisy_0;
    uint device* sources_0;
    float4 device* history_0;
    float4 device* denoised_0;
    float4 device* published_0;
    texture2d<float, access::read_write> diffuse_0;
    texture2d<float, access::read_write> specular_0;
};


#line 5163
struct KernelContext_0
{
    GiTemporalSet_default_0 constant* giTemporal_0;
};


#line 49 "src/rendering/gi_gpu/shaders/gi_temporal.slang"
uint gtHistoryRecord_0(uint slot_0, uint signal_0, uint pixel_0, KernelContext_0 thread* kernelContext_0)
{

#line 50
    return (slot_0 * 2U + signal_0) * kernelContext_0->giTemporal_0->constants_0[0U].z + pixel_0;
}


#line 58
float gtScalar_0(float3 value_1, uint domain_0)
{

#line 59
    if(domain_0 == 1U)
    {

#line 60
        return value_1.x;
    }
    return 0.2125999927520752 * value_1.x + 0.71520000696182251 * value_1.y + 0.07220000028610229 * value_1.z;
}


float gtHistoryKept_0(float3 history_1, float3 value_2, uint signal_1, KernelContext_0 thread* kernelContext_1)
{

#line 67
    uint4 words_0 = kernelContext_1->giTemporal_0->constants_0[20U + signal_1 * 3U + 2U];
    float rejection_0 = (as_type<float>((words_0.z)));
    if(rejection_0 <= 0.0)
    {

#line 70
        return 1.0;
    }
    uint _S2 = words_0.y;

#line 72
    float before_0 = gtScalar_0(history_1, _S2);
    float now_0 = gtScalar_0(value_2, _S2);
    float deviation_0 = abs(now_0 - before_0);
    float allowed_0 = rejection_0 * max(abs(now_0), abs(before_0));
    if(deviation_0 <= allowed_0)
    {

#line 77
        return 1.0;
    }
    float ratio_0 = allowed_0 / deviation_0;
    return ratio_0 * ratio_0;
}


float3 gtAccumulate_0(uint signal_2, uint pixel_1, int source_0, float confidence_0, KernelContext_0 thread* kernelContext_2)
{

#line 85
    uint4 counts_0 = kernelContext_2->giTemporal_0->constants_0[0U];
    float historyLength_0 = (as_type<float>((kernelContext_2->giTemporal_0->constants_0[20U + signal_2 * 3U + 1U].y)));

#line 86
    float alphaFloor_0;
    if(historyLength_0 > 0.0)
    {

#line 87
        alphaFloor_0 = 1.0 / historyLength_0;

#line 87
    }
    else
    {

#line 87
        alphaFloor_0 = 1.0;

#line 87
    }
    float4 noisy_1 = kernelContext_2->giTemporal_0->noisy_0[pixel_1 * 2U + signal_2];
    float3 value_3 = noisy_1.xyz;
    float fresh_0 = noisy_1.w;
    uint _S3 = counts_0.w;

#line 91
    uint _S4 = gtHistoryRecord_0(_S3, signal_2, pixel_1, kernelContext_2);

#line 91
    bool _S5;
    if(source_0 < int(0))
    {

#line 92
        _S5 = true;

#line 92
    }
    else
    {

#line 92
        _S5 = confidence_0 <= 0.0;

#line 92
    }

#line 92
    if(_S5)
    {

#line 93
        *(kernelContext_2->giTemporal_0->history_0+_S4) = float4(value_3, fresh_0);
        return value_3;
    }

#line 94
    uint _S6 = gtHistoryRecord_0(1U - _S3, signal_2, uint(source_0), kernelContext_2);

    float4 device* _S7 = kernelContext_2->giTemporal_0->history_0+_S6;
    float _S8 = (*_S7).w * confidence_0;

#line 97
    float3 _S9 = (*_S7).xyz;

#line 97
    float _S10 = gtHistoryKept_0(_S9, value_3, signal_2, kernelContext_2);
    float count_0 = _S8 * _S10 + fresh_0;

#line 98
    float _S11;
    if(count_0 > 0.0)
    {

#line 99
        _S11 = fresh_0 / count_0;

#line 99
    }
    else
    {

#line 99
        _S11 = 1.0;

#line 99
    }
    float3 blended_0 = _S9 + (value_3 - _S9) * float3(max(_S11, alphaFloor_0)) ;
    *(kernelContext_2->giTemporal_0->history_0+_S4) = float4(blended_0, min(count_0, historyLength_0));
    return blended_0;
}



[[kernel]] void cyGiDenoiseTemporal(uint3 thread_0 [[thread_position_in_grid]], GiTemporalSet_default_0 constant* giTemporal_1 [[buffer(0)]])
{

#line 107
    thread KernelContext_0 kernelContext_3;

#line 107
    (&kernelContext_3)->giTemporal_0 = giTemporal_1;
    uint4 counts_1 = giTemporal_1->constants_0[0U];
    uint _S12 = thread_0.x;

#line 109
    uint _S13 = counts_1.x;

#line 109
    bool _S14;

#line 109
    if(_S12 >= _S13)
    {

#line 109
        _S14 = true;

#line 109
    }
    else
    {

#line 109
        _S14 = (thread_0.y) >= (counts_1.y);

#line 109
    }

#line 109
    if(_S14)
    {

#line 110
        return;
    }
    uint pixel_2 = thread_0.y * _S13 + _S12;

#line 112
    int source_1;

#line 112
    float confidence_1;


    if((giTemporal_1->constants_0[5U].w) != 0U)
    {

#line 116
        uint _S15 = pixel_2 * 2U;

#line 116
        int source_2 = (as_type<int>(((&kernelContext_3)->giTemporal_0->sources_0[_S15])));
        if(source_2 >= int(0))
        {

#line 117
            _S14 = uint(source_2) < (counts_1.z);

#line 117
        }
        else
        {

#line 117
            _S14 = false;

#line 117
        }

#line 117
        if(_S14)
        {

)cy_msl"
    R"cy_msl(#line 118
            float _S16 = gtClamp_0((as_type<float>(((&kernelContext_3)->giTemporal_0->sources_0[_S15 + 1U]))), 0.0, 1.0);

#line 118
            source_1 = source_2;

#line 118
            confidence_1 = _S16;

#line 117
        }
        else
        {

#line 117
            source_1 = int(-1);

#line 117
            confidence_1 = 0.0;

#line 117
        }

#line 115
    }
    else
    {

#line 115
        source_1 = int(-1);

#line 115
        confidence_1 = 0.0;

#line 115
    }

#line 115
    float3 _S17 = gtAccumulate_0(0U, pixel_2, source_1, confidence_1, &kernelContext_3);

#line 115
    float3 _S18 = gtAccumulate_0(1U, pixel_2, source_1, confidence_1, &kernelContext_3);

#line 125
    uint4 full_0 = giTemporal_1->constants_0[36U];
    bool copy_0 = ((full_0.w) & 1U) != 0U;
    bool _S19 = (max(full_0.z, 1U)) != 1U;

#line 127
    if(_S19)
    {

#line 127
        _S14 = true;

#line 127
    }
    else
    {

#line 127
        _S14 = copy_0;

#line 127
    }

#line 127
    if(_S14)
    {

#line 128
        *((&kernelContext_3)->giTemporal_0->denoised_0+pixel_2) = float4(_S17, 0.0);
        *((&kernelContext_3)->giTemporal_0->denoised_0+(counts_1.z + pixel_2)) = float4(_S18, 0.0);

#line 127
    }



    if(_S19)
    {

#line 132
        return;
    }


    uint2 _S20 = thread_0.xy;

#line 136
    (&kernelContext_3)->giTemporal_0->diffuse_0.write(float4(_S17, 1.0),_S20);
    (&kernelContext_3)->giTemporal_0->specular_0.write(float4(_S18, 1.0),_S20);
    if(copy_0)
    {

#line 139
        *((&kernelContext_3)->giTemporal_0->published_0+pixel_2) = float4(_S17, 0.0);
        *((&kernelContext_3)->giTemporal_0->published_0+(full_0.x * full_0.y + pixel_2)) = float4(_S18, 0.0);

#line 138
    }



    return;
}

)cy_msl";

/// cyGiProbePlace.metal, 15120 bytes.
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


#line 64 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
uint spSlot_0(KernelContext_0 thread* kernelContext_0)
{

#line 64
    return kernelContext_0->giResolve_0->constants_0[0U].w;
}


#line 61
uint spUniformCount_0(KernelContext_0 thread* kernelContext_1)
{

#line 61
    return kernelContext_1->giResolve_0->constants_0[43U].w;
}


#line 61
uint spUniformCount_1(KernelContext_0 thread* kernelContext_2)
{

#line 61
    return kernelContext_2->giResolve_0->constants_0[43U].w;
}


#line 57
uint spTilesX_0(KernelContext_0 thread* kernelContext_3)
{

#line 57
    return kernelContext_3->giResolve_0->constants_0[41U].x;
}


#line 57
uint spTilesX_1(KernelContext_0 thread* kernelContext_4)
{

#line 57
    return kernelContext_4->giResolve_0->constants_0[41U].x;
}


#line 55
uint spSpacing_0(KernelContext_0 thread* kernelContext_5)
{

#line 55
    return kernelContext_5->giResolve_0->constants_0[40U].z;
}


#line 55
uint spSpacing_1(KernelContext_0 thread* kernelContext_6)
{

#line 55
    return kernelContext_6->giResolve_0->constants_0[40U].z;
}


#line 53
uint spWidth_0(KernelContext_0 thread* kernelContext_7)
{

#line 53
    return kernelContext_7->giResolve_0->constants_0[40U].x;
}


#line 53
uint spWidth_1(KernelContext_0 thread* kernelContext_8)
{

#line 53
    return kernelContext_8->giResolve_0->constants_0[40U].x;
}


#line 54
uint spHeight_0(KernelContext_0 thread* kernelContext_9)
{

#line 54
    return kernelContext_9->giResolve_0->constants_0[40U].y;
}


#line 67
uint spUniformPixel_0(uint tile_0, KernelContext_0 thread* kernelContext_10)
{

#line 67
    uint _S1 = spTilesX_1(kernelContext_10);
    uint _S2 = tile_0 % _S1;

#line 68
    uint _S3 = spSpacing_1(kernelContext_10);

#line 68
    uint _S4 = _S2 * _S3;

#line 68
    uint _S5 = spSpacing_1(kernelContext_10);

#line 68
    uint _S6 = _S4 + _S5 / 2U;

#line 68
    uint _S7 = spWidth_1(kernelContext_10);

#line 68
    uint _S8 = min(_S6, _S7 - 1U);

#line 68
    uint _S9 = spTilesX_1(kernelContext_10);
    uint _S10 = tile_0 / _S9;

#line 69
    uint _S11 = spSpacing_1(kernelContext_10);

#line 69
    uint _S12 = _S10 * _S11;

#line 69
    uint _S13 = spSpacing_1(kernelContext_10);

#line 69
    uint _S14 = _S12 + _S13 / 2U;

#line 69
    uint _S15 = spHeight_0(kernelContext_10);

#line 69
    uint _S16 = min(_S14, _S15 - 1U);

#line 69
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


#line 62 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
uint spCapacity_0(KernelContext_0 thread* kernelContext_11)
{

#line 62
    return kernelContext_11->giResolve_0->constants_0[42U].z;
}


#line 62
uint spCapacity_1(KernelContext_0 thread* kernelContext_12)
{

#line 62
    return kernelContext_12->giResolve_0->constants_0[42U].z;
}


#line 63
uint spRecords_0(KernelContext_0 thread* kernelContext_13)
{

#line 63
    uint _S19 = spUniformCount_1(kernelContext_13);

#line 63
    uint _S20 = spCapacity_1(kernelContext_13);

#line 63
    return _S19 + _S20;
}


#line 80
uint spRecordBase_0(uint slot_0, uint record_0, KernelContext_0 thread* kernelContext_14)
{

#line 80
    uint _S21 = spRecords_0(kernelContext_14);
    return (slot_0 * _S21 + record_0) * 18U;
}


#line 279
void spWriteProbe_0(uint slot_1, uint record_1, float3 position_0, float depth_0, float3 normal_0, uint id_0, KernelContext_0 thread* kernelContext_15)
{

#line 279
    uint _S22 = spRecordBase_0(slot_1, record_1, kernelContext_15);

    *(kernelContext_15->giResolve_0->probes_0+_S22) = float4(position_0, depth_0);
    *(kernelContext_15->giResolve_0->probes_0+(_S22 + 1U)) = float4(normal_0, (as_type<float>((id_0))));

#line 282
    uint v_1 = 0U;
    for(;;)
    {

#line 283
        if(v_1 < 7U)
        {
        }
        else
        {

#line 283
            break;
        }

#line 284
        *(kernelContext_15->giResolve_0->probes_0+(_S22 + 2U + v_1)) = float4(0.0, 0.0, 0.0, 0.0);

#line 283
        v_1 = v_1 + 1U;

#line 283
    }


    float4 _S23 = float4(0.0, 0.0, 0.0, 0.0);

#line 286
    *(kernelContext_15->giResolve_0->probes_0+(_S22 + 9U)) = _S23;
    *(kernelContext_15->giResolve_0->probes_0+(_S22 + 10U)) = float4(1.0, 0.0, 0.0, 0.0);

#line 287
    v_1 = 0U;
    for(;;)
    {

#line 288
        if(v_1 < 7U)
        {
        }
        else
        {

#line 288
            break;
        }

#line 289
        *(kernelContext_15->giResolve_0->probes_0+(_S22 + 11U + v_1)) = _S23;

)cy_msl"
    R"cy_msl(#line 288
        v_1 = v_1 + 1U;

#line 288
    }


    return;
}


#line 59
uint spCellsX_0(KernelContext_0 thread* kernelContext_16)
{

#line 59
    return kernelContext_16->giResolve_0->constants_0[41U].z;
}


#line 59
uint spCellsX_1(KernelContext_0 thread* kernelContext_17)
{

#line 59
    return kernelContext_17->giResolve_0->constants_0[41U].z;
}


#line 60
uint spCellsY_0(KernelContext_0 thread* kernelContext_18)
{

#line 60
    return kernelContext_18->giResolve_0->constants_0[41U].w;
}


#line 60
uint spCellsY_1(KernelContext_0 thread* kernelContext_19)
{

#line 60
    return kernelContext_19->giResolve_0->constants_0[41U].w;
}


#line 84
uint spTableIndex_0(uint slot_2, uint cell_0, KernelContext_0 thread* kernelContext_20)
{

#line 84
    uint _S24 = spCellsX_1(kernelContext_20);
    uint _S25 = slot_2 * _S24;

#line 85
    uint _S26 = spCellsY_1(kernelContext_20);

#line 85
    return 4U + _S25 * _S26 + cell_0;
}


#line 56
uint spCell_0(KernelContext_0 thread* kernelContext_21)
{

#line 56
    return kernelContext_21->giResolve_0->constants_0[40U].w;
}


#line 74
uint spAdaptivePixel_0(uint cell_1, KernelContext_0 thread* kernelContext_22)
{

#line 74
    uint _S27 = spCellsX_1(kernelContext_22);
    uint _S28 = cell_1 % _S27;

#line 75
    uint _S29 = spCell_0(kernelContext_22);

#line 75
    uint _S30 = _S28 * _S29;

#line 75
    uint _S31 = spCell_0(kernelContext_22);

#line 75
    uint _S32 = _S30 + _S31 / 2U;

#line 75
    uint _S33 = spWidth_1(kernelContext_22);

#line 75
    uint _S34 = min(_S32, _S33 - 1U);

#line 75
    uint _S35 = spCellsX_1(kernelContext_22);
    uint _S36 = cell_1 / _S35;

#line 76
    uint _S37 = spCell_0(kernelContext_22);

#line 76
    uint _S38 = _S36 * _S37;

#line 76
    uint _S39 = spCell_0(kernelContext_22);

#line 76
    uint _S40 = _S38 + _S39 / 2U;

#line 76
    uint _S41 = spHeight_0(kernelContext_22);

#line 76
    uint _S42 = min(_S40, _S41 - 1U);

#line 76
    uint _S43 = spWidth_1(kernelContext_22);
    return _S42 * _S43 + _S34;
}


#line 58
uint spTilesY_0(KernelContext_0 thread* kernelContext_23)
{

#line 58
    return kernelContext_23->giResolve_0->constants_0[41U].y;
}


#line 149
void spAxisTaps_0(uint coordinate_0, uint step_0, uint count_0, uint thread* base_0, float thread* fraction_0)
{

#line 150
    float g_0 = (float(coordinate_0) - float(step_0 / 2U)) / float(step_0);
    if(g_0 <= 0.0)
    {

#line 152
        *base_0 = 0U;
        *fraction_0 = 0.0;
        return;
    }
    uint _S44 = uint(g_0);

#line 156
    *base_0 = _S44;
    *fraction_0 = g_0 - float(_S44);
    if((*base_0 + 1U) >= count_0)
    {

#line 159
        *base_0 = count_0 - 1U;
        *fraction_0 = 0.0;

#line 158
    }



    return;
}


#line 143
struct SpTaps_0
{
    array<uint, int(4)> index_0;
    array<float, int(4)> weight_0;
    uint count_1;
};


#line 164
SpTaps_0 spBilinearTaps_0(uint x_0, uint y_0, uint step_1, uint countX_0, uint countY_0)
{

#line 165
    thread SpTaps_0 taps_0;
    (&taps_0)->count_1 = 0U;

#line 166
    uint t_0 = 0U;
    for(;;)
    {

#line 167
        if(t_0 < 4U)
        {
        }
        else
        {

#line 167
            break;
        }

#line 168
        (&taps_0)->index_0[t_0] = 0U;
        (&taps_0)->weight_0[t_0] = 0.0;

#line 167
        t_0 = t_0 + 1U;

#line 167
    }



    thread uint bx_0;

    thread float fx_0;

    spAxisTaps_0(x_0, step_1, countX_0, &bx_0, &fx_0);

#line 172
    thread uint by_0;

    thread float fy_0;

    spAxisTaps_0(y_0, step_1, countY_0, &by_0, &fy_0);

#line 176
    uint j_0 = 0U;
    for(;;)
    {

#line 177
        if(j_0 < 2U)
        {
        }
        else
        {

#line 177
            break;
        }

#line 177
        uint i_0 = 0U;
        for(;;)
        {

#line 178
            if(i_0 < 2U)
            {
            }
            else
            {

#line 178
                break;
            }

#line 178
            float _S45;
            if(i_0 == 0U)
            {

#line 179
                _S45 = 1.0 - fx_0;

#line 179
            }
            else
            {

#line 179
                _S45 = fx_0;

#line 179
            }

#line 179
            float _S46;

#line 179
            if(j_0 == 0U)
            {

#line 179
                _S46 = 1.0 - fy_0;

#line 179
            }
            else
            {

#line 179
                _S46 = fy_0;

#line 179
            }

#line 179
            float weight_1 = _S45 * _S46;
            if(weight_1 <= 0.0)
            {

)cy_msl"
    R"cy_msl(#line 181
                i_0 = i_0 + 1U;

#line 178
                continue;
            }



            (&taps_0)->index_0[(&taps_0)->count_1] = (by_0 + j_0) * countX_0 + (bx_0 + i_0);
            (&taps_0)->weight_0[(&taps_0)->count_1] = weight_1;
            (&taps_0)->count_1 = (&taps_0)->count_1 + 1U;

#line 178
            i_0 = i_0 + 1U;

#line 178
        }

#line 177
        j_0 = j_0 + 1U;

#line 177
    }

#line 188
    return taps_0;
}


#line 123
float spGeometryWeight_0(float3 probePosition_0, float3 probeNormal_0, float3 position_1, float3 normal_1, float depth_1, float tolerance_0)
{
    float plane_0 = abs(dot(probeNormal_0, position_1 - probePosition_0));
    float reach_0 = tolerance_0 * depth_1;
    if(reach_0 <= 0.0)
    {

#line 128
        return 0.0;
    }

    float _S47 = max(0.0, dot(probeNormal_0, normal_1));
    float facing_0 = _S47 * _S47;
    float facing_1 = facing_0 * facing_0;

    return max(0.0, 1.0 - plane_0 / reach_0) * (facing_1 * facing_1);
}


#line 88
uint spCounter_0(uint slot_3)
{

#line 89
    return slot_3;
}


#line 298
void spPlace_0(uint thread_0, KernelContext_0 thread* kernelContext_24)
{

#line 298
    uint _S48 = spSlot_0(kernelContext_24);

#line 298
    uint _S49 = spUniformCount_0(kernelContext_24);


    uint pixels_0 = kernelContext_24->giResolve_0->constants_0[0U].z;
    if(thread_0 < _S49)
    {

#line 302
        uint _S50 = spUniformPixel_0(thread_0, kernelContext_24);

        uint record_2 = (_S48 * pixels_0 + _S50) * 2U;
        float4 at_0 = kernelContext_24->giResolve_0->surfaces_0[record_2];
        float _S51 = at_0.w;

#line 306
        if(_S51 > 0.0)
        {

#line 306
            spWriteProbe_0(_S48, thread_0, at_0.xyz, _S51, giNormalisedOr_0(kernelContext_24->giResolve_0->surfaces_0[record_2 + 1U].xyz, float3(0.0, 1.0, 0.0)), thread_0, kernelContext_24);

#line 306
        }
        else
        {

#line 306
            spWriteProbe_0(_S48, thread_0, float3(0.0, 0.0, 0.0), 0.0, float3(0.0, 1.0, 0.0), thread_0, kernelContext_24);

#line 306
        }

#line 302
    }

#line 302
    uint _S52 = spCellsX_0(kernelContext_24);

#line 302
    uint _S53 = spCellsY_0(kernelContext_24);

#line 316
    if(thread_0 >= (_S52 * _S53))
    {

#line 317
        return;
    }

#line 317
    uint _S54 = spTableIndex_0(_S48, thread_0, kernelContext_24);

    *(kernelContext_24->giResolve_0->slots_0+_S54) = 4294967295U;

#line 319
    uint _S55 = spAdaptivePixel_0(thread_0, kernelContext_24);

    uint _S56 = _S48 * pixels_0;

#line 321
    uint record_3 = (_S56 + _S55) * 2U;
    float4 at_1 = kernelContext_24->giResolve_0->surfaces_0[record_3];
    float _S57 = at_1.w;

#line 323
    if(_S57 <= 0.0)
    {

#line 324
        return;
    }

    float3 _S58 = float3(0.0, 1.0, 0.0);

#line 327
    float3 normal_2 = giNormalisedOr_0(kernelContext_24->giResolve_0->surfaces_0[record_3 + 1U].xyz, _S58);
    float4 weights_0 = (as_type<float4>((kernelContext_24->giResolve_0->constants_0[43U])));

#line 328
    uint _S59 = spWidth_0(kernelContext_24);
    uint _S60 = _S55 % _S59;

#line 329
    uint _S61 = spWidth_0(kernelContext_24);

#line 329
    uint _S62 = _S55 / _S61;

#line 329
    uint _S63 = spSpacing_0(kernelContext_24);

#line 329
    uint _S64 = spTilesX_0(kernelContext_24);

#line 329
    uint _S65 = spTilesY_0(kernelContext_24);

#line 329
    SpTaps_0 _S66 = spBilinearTaps_0(_S60, _S62, _S63, _S64, _S65);

#line 329
    uint tap_0 = 0U;

#line 329
    float covered_0 = 0.0;


    for(;;)
    {

#line 332
        if(tap_0 < (_S66.count_1))
        {
        }
        else
        {

#line 332
            break;
        }

#line 332
        uint _S67 = spUniformPixel_0(_S66.index_0[tap_0], kernelContext_24);

        uint probeRecord_0 = (_S56 + _S67) * 2U;
        float4 probeAt_0 = kernelContext_24->giResolve_0->surfaces_0[probeRecord_0];
        if((probeAt_0.w) <= 0.0)
        {

#line 337
            tap_0 = tap_0 + 1U;

#line 332
            continue;
        }

#line 332
        covered_0 = covered_0 + _S66.weight_0[tap_0] * spGeometryWeight_0(probeAt_0.xyz, giNormalisedOr_0(kernelContext_24->giResolve_0->surfaces_0[probeRecord_0 + 1U].xyz, _S58), at_1.xyz, normal_2, _S57, weights_0.y);

#line 332
        tap_0 = tap_0 + 1U;

#line 332
    }

#line 344
    if(covered_0 >= (weights_0.x))
    {

#line 345
        return;
    }

    uint taken_0 = atomic_fetch_add_explicit(((atomic_uint device*)(kernelContext_24->giResolve_0->slots_0+spCounter_0(_S48))), 1U, memory_order_relaxed);

#line 348
    uint _S68 = spCapacity_0(kernelContext_24);
    if(taken_0 >= _S68)
    {

#line 350
        return;
    }

#line 350
    uint _S69 = spTableIndex_0(_S48, thread_0, kernelContext_24);

    *(kernelContext_24->giResolve_0->slots_0+_S69) = taken_0;

#line 352
    spWriteProbe_0(_S48, _S49 + taken_0, at_1.xyz, _S57, normal_2, _S49 + thread_0, kernelContext_24);

    return;
}


#line 22 "src/rendering/gi_gpu/shaders/gi_screen_probes.slang"
[[kernel]] void cyGiProbePlace(uint3 thread_1 [[thread_position_in_grid]], GiResolveSet_default_0 constant* giResolve_1 [[buffer(3)]])
{

#line 22
    thread KernelContext_0 kernelContext_25;

#line 22
    (&kernelContext_25)->giResolve_0 = giResolve_1;

#line 22
    spPlace_0(thread_1.x, &kernelContext_25);

    return;
}

)cy_msl";

/// cyGiProbeTrace.metal, 105548 bytes.
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


#line 663 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
struct KernelContext_0
{
    GiProbeSet_default_0 constant* giProbes_0;
    GiSceneSet_default_0 constant* giScene_0;
    GiResolveSet_default_0 constant* giResolve_0;
    array<float3, int(9)> threadgroup* spHistory_0;
    float threadgroup* spHistoryCount_0;
    uint threadgroup* spReprojected_0;
    uint threadgroup* spCarried_0;
    float4 threadgroup* spStandIn_0;
    float3 threadgroup* spOrigin_0;
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
)cy_msl",
    R"cy_msl(
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


#line 503 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
GrSample_0 spProbeRay_0(float3 origin_3, float3 unit_0, float reach_0, KernelContext_0 thread* kernelContext_11)
{

#line 504
    uint cacheFrame_1 = kernelContext_11->giProbes_0->constants_0[24U].w;

#line 504
    GiWorldHit_0 _S64 = GiSoftwareTier_trace_0(origin_3, unit_0, reach_0, kernelContext_11);

#line 504
    bool _S65;

    if(_S64.hit_0)
    {

#line 506
        _S65 = (dot(_S64.normal_0, unit_0)) > 0.0;

#line 506
    }
    else
    {

#line 506
        _S65 = false;

#line 506
    }

#line 506
    if(_S65)
    {

#line 507
        return grSample_0(float3(0.0, 0.0, 0.0), 0.0, 0U);
    }
    uint _S66 = GiSoftwareTier_source_0();

#line 509
    thread GiWorldHit_0 _S67 = _S64;

#line 509
    GiTierSample_0 _S68 = gpWorldRadiance_0(&_S67, unit_0, _S66, cacheFrame_1, kernelContext_11);
    return grSample_0(_S68.radiance_2, _S68.confidence_3, _S68.source_2);
}


#line 211
struct SpProbe_0
{
    float3 position_4;
    float depth_0;
    float3 normal_3;
    uint id_0;
};


#line 478
float3 spProbeOrigin_0(const SpProbe_0 thread* probe_0, float lift_0, KernelContext_0 thread* kernelContext_12)
{

#line 478
    uint _S69 = giLevelCount_0(kernelContext_12);
    if(_S69 == 0U)
    {

#line 480
        return probe_0->position_4 + probe_0->normal_3 * float3(lift_0) ;
    }

#line 480
    float _S70 = giFinestVoxel_0(kernelContext_12);

#line 480
    float clearance_0;


    if((GiSoftwareTier_source_0()) == 7U)
    {

#line 483
        clearance_0 = lift_0;

#line 483
    }
    else
    {

#line 483
        clearance_0 = max(lift_0, _S70);

#line 483
    }

#line 483
    float3 _S71 = probe_0->normal_3;
    float3 origin_4 = probe_0->position_4 + probe_0->normal_3 * float3(clearance_0) ;
    if(_S70 <= 0.0)
    {

#line 486
        return origin_4;
    }

#line 486
    float3 origin_5 = origin_4;

#line 486
    uint step_2 = 0U;

    for(;;)
    {

#line 488
        if(step_2 < 4U)
        {
        }
        else
        {

#line 488
            break;
        }

#line 488
        float _S72 = giDistance_0(origin_5, kernelContext_12);

        if(_S72 >= clearance_0)
        {

#line 491
            break;
        }

        float3 _S73 = float3(_S70, 0.0, 0.0);

#line 494
        float _S74 = giDistance_0(origin_5 + _S73, kernelContext_12);

#line 494
        float _S75 = giDistance_0(origin_5 - _S73, kernelContext_12);

#line 494
        float _S76 = _S74 - _S75;
        float3 _S77 = float3(0.0, _S70, 0.0);

#line 495
        float _S78 = giDistance_0(origin_5 + _S77, kernelContext_12);

#line 495
        float _S79 = giDistance_0(origin_5 - _S77, kernelContext_12);

#line 495
        float _S80 = _S78 - _S79;
        float3 _S81 = float3(0.0, 0.0, _S70);

#line 496
        float _S82 = giDistance_0(origin_5 + _S81, kernelContext_12);

#line 496
        float _S83 = giDistance_0(origin_5 - _S81, kernelContext_12);
        float3 _S84 = origin_5 + giNormalisedOr_0(float3(_S76, _S80, _S82 - _S83), _S71) * float3((clearance_0 - _S72)) ;

#line 488
        uint step_3 = step_2 + 1U;

#line 488
        origin_5 = _S84;

#line 488
        step_2 = step_3;

#line 488
    }

#line 499
    return origin_5;
}


#line 64
uint spSlot_0(KernelContext_0 thread* kernelContext_13)
{

#line 64
    return kernelContext_13->giResolve_0->constants_2[0U].w;
}


#line 88
uint spCounter_0(uint slot_2)
{

#line 89
    return slot_2;
}


#line 61
uint spUniformCount_0(KernelContext_0 thread* kernelContext_14)
{

#line 61
    return kernelContext_14->giResolve_0->constants_2[43U].w;
}


#line 62
uint spCapacity_0(KernelContext_0 thread* kernelContext_15)
{

#line 62
    return kernelContext_15->giResolve_0->constants_2[42U].z;
}


#line 61
uint spUniformCount_1(KernelContext_0 thread* kernelContext_16)
{

#line 61
    return kernelContext_16->giResolve_0->constants_2[43U].w;
}


#line 62
uint spCapacity_1(KernelContext_0 thread* kernelContext_17)
{

#line 62
    return kernelContext_17->giResolve_0->constants_2[42U].z;
}


#line 63
uint spRecords_0(KernelContext_0 thread* kernelContext_18)
{

#line 63
    uint _S85 = spUniformCount_1(kernelContext_18);

#line 63
    uint _S86 = spCapacity_1(kernelContext_18);

#line 63
    return _S85 + _S86;
}


#line 80
uint spRecordBase_0(uint slot_3, uint record_0, KernelContext_0 thread* kernelContext_19)
{

#line 80
    uint _S87 = spRecords_0(kernelContext_19);
    return (slot_3 * _S87 + record_0) * 18U;
}


#line 218
SpProbe_0 spLoadProbe_0(uint slot_4, uint record_1, KernelContext_0 thread* kernelContext_20)
{

#line 218
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


#line 399
bool spInvalidated_0(float3 point_0, KernelContext_0 thread* kernelContext_21)
{

#line 400
    uint _S91 = min(kernelContext_21->giResolve_0->constants_2[26U].x, 4U);

#line 400
    uint region_0 = 0U;
    for(;;)
    {

#line 401
        if(region_0 < _S91)
        {
        }
        else
        {

#line 401
            break;
        }

#line 402
        uint _S92 = region_0 * 2U;

#line 402
        float3 lo_1 = (as_type<float3>((kernelContext_21->giResolve_0->constants_2[27U + _S92].xyz)));
        float3 hi_1 = (as_type<float3>((kernelContext_21->giResolve_0->constants_2[28U + _S92].xyz)));
        float _S93 = point_0.x;

#line 404
        bool _S94;

#line 404
        if(_S93 >= (lo_1.x))
        {

#line 404
            _S94 = (point_0.y) >= (lo_1.y);

#line 404
        }
        else
        {

#line 404
            _S94 = false;

#line 404
        }

#line 404
        bool _S95;

#line 404
        if(_S94)
        {

#line 404
            _S95 = (point_0.z) >= (lo_1.z);

#line 404
        }
        else
        {

#line 404
            _S95 = false;

#line 404
        }

#line 404
        bool _S96;

#line 404
        if(_S95)
        {

#line 404
            _S96 = _S93 <= (hi_1.x);

#line 404
        }
        else
        {

#line 404
            _S96 = false;

#line 404
        }

#line 404
        bool _S97;

#line 404
        if(_S96)
        {

#line 404
            _S97 = (point_0.y) <= (hi_1.y);

#line 404
        }
        else
        {

#line 404
            _S97 = false;

#line 404
        }

#line 404
        bool _S98;
        if(_S97)
        {

#line 405
            _S98 = (point_0.z) <= (hi_1.z);

#line 405
        }
        else
        {

#line 405
            _S98 = false;

#line 405
        }

#line 404
        if(_S98)
        {
            return true;
        }

#line 401
        region_0 = region_0 + 1U;

#line 401
    }

#line 409
    return false;
}


#line 53
uint spWidth_0(KernelContext_0 thread* kernelContext_22)
{

#line 53
    return kernelContext_22->giResolve_0->constants_2[40U].x;
}


#line 54
uint spHeight_0(KernelContext_0 thread* kernelContext_23)
{

#line 54
    return kernelContext_23->giResolve_0->constants_2[40U].y;
}


#line 359
bool spLanding_0(float3 position_5, uint thread* x_3, uint thread* y_3, KernelContext_0 thread* kernelContext_24)
{

#line 360
    *x_3 = 0U;
    *y_3 = 0U;

#line 361
    float4 _S99 = float4(position_5.x) ;

#line 361
    float4 _S100 = float4(position_5.y) ;

#line 361
    float4 _S101 = float4(position_5.z) ;

#line 366
    float4 clip_0 = (as_type<float4>((kernelContext_24->giResolve_0->constants_2[6U]))) * _S99 + (as_type<float4>((kernelContext_24->giResolve_0->constants_2[7U]))) * _S100 + (as_type<float4>((kernelContext_24->giResolve_0->constants_2[8U]))) * _S101 + (as_type<float4>((kernelContext_24->giResolve_0->constants_2[9U])));
    float _S102 = clip_0.w;

#line 367
    if(_S102 <= 9.99999993922529029e-09)
    {

#line 368
        return false;
    }
    float inverse_0 = 1.0 / _S102;

    float _S103 = clip_0.x * inverse_0 * 0.5 + 0.5;

#line 372
    float _S104 = clip_0.y * inverse_0 * 0.5 + 0.5;

#line 377
    float4 clip_1 = (as_type<float4>((kernelContext_24->giResolve_0->constants_2[10U]))) * _S99 + (as_type<float4>((kernelContext_24->giResolve_0->constants_2[11U]))) * _S100 + (as_type<float4>((kernelContext_24->giResolve_0->constants_2[12U]))) * _S101 + (as_type<float4>((kernelContext_24->giResolve_0->constants_2[13U])));
    float _S105 = clip_1.w;

#line 378
    if(_S105 <= 9.99999993922529029e-09)
    {

#line 379
        return false;
    }
    float inverse_1 = 1.0 / _S105;



    float _S106 = _S103 + (clip_1.x * inverse_1 * 0.5 + 0.5 - _S103);

#line 385
    float _S107 = _S104 + (clip_1.y * inverse_1 * 0.5 + 0.5 - _S104);

#line 385
    uint _S108 = spWidth_0(kernelContext_24);
    float width_0 = float(_S108);

#line 386
    uint _S109 = spHeight_0(kernelContext_24);
    float height_0 = float(_S109);
    float fx_1 = floor(_S106 * width_0);
    float fy_1 = floor((1.0 - _S107) * height_0);

#line 389
    bool _S110;
    if(fx_1 < 0.0)
    {

#line 390
        _S110 = true;

#line 390
    }
    else
    {

#line 390
        _S110 = fy_1 < 0.0;

#line 390
    }

#line 390
    if(_S110)
    {

#line 390
        _S110 = true;

#line 390
    }
    else
    {

#line 390
        _S110 = fx_1 >= width_0;

#line 390
    }

#line 390
    if(_S110)
    {

#line 390
        _S110 = true;

#line 390
    }
    else
    {

#line 390
        _S110 = fy_1 >= height_0;

#line 390
    }

#line 390
)cy_msl",
    R"cy_msl(    if(_S110)
    {

#line 391
        return false;
    }
    *x_3 = uint(fx_1);
    *y_3 = uint(fy_1);
    return true;
}


#line 143
struct SpTaps_0
{
    array<uint, int(4)> index_0;
    array<float, int(4)> weight_0;
    uint count_0;
};


#line 55
uint spSpacing_0(KernelContext_0 thread* kernelContext_25)
{

#line 55
    return kernelContext_25->giResolve_0->constants_2[40U].z;
}


#line 57
uint spTilesX_0(KernelContext_0 thread* kernelContext_26)
{

#line 57
    return kernelContext_26->giResolve_0->constants_2[41U].x;
}


#line 58
uint spTilesY_0(KernelContext_0 thread* kernelContext_27)
{

#line 58
    return kernelContext_27->giResolve_0->constants_2[41U].y;
}


#line 149
void spAxisTaps_0(uint coordinate_0, uint step_4, uint count_1, uint thread* base_1, float thread* fraction_0)
{

#line 150
    float g_0 = (float(coordinate_0) - float(step_4 / 2U)) / float(step_4);
    if(g_0 <= 0.0)
    {

#line 152
        *base_1 = 0U;
        *fraction_0 = 0.0;
        return;
    }
    uint _S111 = uint(g_0);

#line 156
    *base_1 = _S111;
    *fraction_0 = g_0 - float(_S111);
    if((*base_1 + 1U) >= count_1)
    {

#line 159
        *base_1 = count_1 - 1U;
        *fraction_0 = 0.0;

#line 158
    }



    return;
}


#line 164
SpTaps_0 spBilinearTaps_0(uint x_4, uint y_4, uint step_5, uint countX_0, uint countY_0)
{

#line 165
    thread SpTaps_0 taps_0;
    (&taps_0)->count_0 = 0U;

#line 166
    uint t_5 = 0U;
    for(;;)
    {

#line 167
        if(t_5 < 4U)
        {
        }
        else
        {

#line 167
            break;
        }

#line 168
        (&taps_0)->index_0[t_5] = 0U;
        (&taps_0)->weight_0[t_5] = 0.0;

#line 167
        t_5 = t_5 + 1U;

#line 167
    }



    thread uint bx_1;

    thread float fx_2;

    spAxisTaps_0(x_4, step_5, countX_0, &bx_1, &fx_2);

#line 172
    thread uint by_1;

    thread float fy_2;

    spAxisTaps_0(y_4, step_5, countY_0, &by_1, &fy_2);

#line 176
    uint j_0 = 0U;
    for(;;)
    {

#line 177
        if(j_0 < 2U)
        {
        }
        else
        {

#line 177
            break;
        }

#line 177
        uint i_0 = 0U;
        for(;;)
        {

#line 178
            if(i_0 < 2U)
            {
            }
            else
            {

#line 178
                break;
            }

#line 178
            float _S112;
            if(i_0 == 0U)
            {

#line 179
                _S112 = 1.0 - fx_2;

#line 179
            }
            else
            {

#line 179
                _S112 = fx_2;

#line 179
            }

#line 179
            float _S113;

#line 179
            if(j_0 == 0U)
            {

#line 179
                _S113 = 1.0 - fy_2;

#line 179
            }
            else
            {

#line 179
                _S113 = fy_2;

#line 179
            }

#line 179
            float weight_1 = _S112 * _S113;
            if(weight_1 <= 0.0)
            {

#line 181
                i_0 = i_0 + 1U;

#line 178
                continue;
            }



            (&taps_0)->index_0[(&taps_0)->count_0] = (by_1 + j_0) * countX_0 + (bx_1 + i_0);
            (&taps_0)->weight_0[(&taps_0)->count_0] = weight_1;
            (&taps_0)->count_0 = (&taps_0)->count_0 + 1U;

#line 178
            i_0 = i_0 + 1U;

#line 178
        }

#line 177
        j_0 = j_0 + 1U;

#line 177
    }

#line 188
    return taps_0;
}


#line 56
uint spCell_0(KernelContext_0 thread* kernelContext_28)
{

#line 56
    return kernelContext_28->giResolve_0->constants_2[40U].w;
}

uint spCellsX_0(KernelContext_0 thread* kernelContext_29)
{

#line 59
    return kernelContext_29->giResolve_0->constants_2[41U].z;
}


#line 60
uint spCellsY_0(KernelContext_0 thread* kernelContext_30)
{

#line 60
    return kernelContext_30->giResolve_0->constants_2[41U].w;
}


#line 59
uint spCellsX_1(KernelContext_0 thread* kernelContext_31)
{

#line 59
    return kernelContext_31->giResolve_0->constants_2[41U].z;
}


#line 60
uint spCellsY_1(KernelContext_0 thread* kernelContext_32)
{

#line 60
    return kernelContext_32->giResolve_0->constants_2[41U].w;
}


#line 84
uint spTableIndex_0(uint slot_5, uint cell_2, KernelContext_0 thread* kernelContext_33)
{

#line 84
    uint _S114 = spCellsX_1(kernelContext_33);
    uint _S115 = slot_5 * _S114;

#line 85
    uint _S116 = spCellsY_1(kernelContext_33);

#line 85
    return 4U + _S115 * _S116 + cell_2;
}


#line 123
float spGeometryWeight_0(float3 probePosition_0, float3 probeNormal_0, float3 position_6, float3 normal_4, float depth_1, float tolerance_0)
{
    float plane_0 = abs(dot(probeNormal_0, position_6 - probePosition_0));
    float reach_1 = tolerance_0 * depth_1;
    if(reach_1 <= 0.0)
    {

#line 128
        return 0.0;
    }

    float _S117 = max(0.0, dot(probeNormal_0, normal_4));
    float facing_0 = _S117 * _S117;
    float facing_1 = facing_0 * facing_0;

    return max(0.0, 1.0 - plane_0 / reach_1) * (facing_1 * facing_1);
}


#line 80
uint spRecordBase_1(uint slot_6, uint record_2, KernelContext_0 thread* kernelContext_34)
{

#line 80
    uint _S118 = spRecords_0(kernelContext_34);
    return (slot_6 * _S118 + record_2) * 18U;
}


#line 231
void spLoadPacked_0(uint base_2, array<float3, int(9)> thread* coefficients_0, float thread* last_1, KernelContext_0 thread* kernelContext_35)
{

#line 232
    thread array<float, int(28)> values_0;

#line 232
    uint v_1 = 0U;
    for(;;)
    {

#line 233
        if(v_1 < 7U)
        {
        }
        else
        {

#line 233
            break;
        }

#line 234
        float4 device* _S119 = kernelContext_35->giResolve_0->probes_1+(base_2 + v_1);
        uint _S120 = v_1 * 4U;

#line 235
        values_0[_S120] = (*_S119).x;
        values_0[_S120 + 1U] = (*_S119).y;
        values_0[_S120 + 2U] = (*_S119).z;
        values_0[_S120 + 3U] = (*_S119).w;

#line 233
        v_1 = v_1 + 1U;

#line 233
    }

#line 233
    uint k_0 = 0U;

#line 240
    for(;;)
    {

#line 240
        if(k_0 < 9U)
        {
        }
        else
        {

#line 240
            break;
        }

#line 241
        uint _S121 = k_0 * 3U;

#line 241
        (*coefficients_0)[k_0] = float3(values_0[_S121], values_0[_S121 + 1U], values_0[_S121 + 2U]);

#line 240
        k_0 = k_0 + 1U;

#line 240
    }


    *last_1 = values_0[int(27)];
    return;
}


#line 260
void spLoadCoefficients_0(uint slot_7, uint record_3, array<float3, int(9)> thread* coefficients_1, float thread* history_0, KernelContext_0 thread* kernelContext_36)
{

#line 260
    uint _S122 = spRecordBase_1(slot_7, record_3, kernelContext_36);

#line 260
    spLoadPacked_0(_S122 + 2U, coefficients_1, history_0, kernelContext_36);

    return;
}


#line 413
bool spReproject_0(const SpProbe_0 thread* probe_2, float tolerance_1, array<float3, int(9)> thread* coefficients_2, float thread* history_1, uint thread* matched_0, KernelContext_0 thread* kernelContext_37)
{

#line 414
    uint k_1 = 0U;
    for(;;)
    {

#line 415
        if(k_1 < 9U)
        {
        }
        else
        {

#line 415
            break;
        }

#line 416
        (*coefficients_2)[k_1] = float3(0.0, 0.0, 0.0);

#line 415
        k_1 = k_1 + 1U;

#line 415
    }


    *history_1 = 0.0;
    *matched_0 = 4294967295U;

#line 419
    float3 _S123 = probe_2->position_4;

#line 419
    bool _S124 = spInvalidated_0(probe_2->position_4, kernelContext_37);
    if(_S124)
    {

#line 421
        return false;
    }
    thread uint x_5;
    thread uint y_5;

#line 424
    bool _S125 = spLanding_0(_S123, &x_5, &y_5, kernelContext_37);
    if(!_S125)
    {

#line 426
        return false;
    }

#line 426
    uint _S126 = spSlot_0(kernelContext_37);


    uint previous_0 = 1U - _S126;

#line 429
    float bestWeight_0 = 0.0;

#line 429
    uint best_4 = 4294967295U;

#line 429
    uint pass_0 = 0U;


    for(;;)
    {

#line 432
        if(pass_0 < 2U)
        {
        }
        else
        {

#line 432
            break;
        }

#line 432
        SpTaps_0 _S127;

        if(pass_0 == 0U)
        {

#line 434
            uint _S128 = spSpacing_0(kernelContext_37);

#line 434
            uint _S129 = spTilesX_0(kernelContext_37);

#line 434
            uint _S130 = spTilesY_0(kernelContext_37);

#line 434
            _S127 = spBilinearTaps_0(x_5, y_5, _S128, _S129, _S130);

#line 434
        }
        else
        {

#line 434
            uint _S131 = spCell_0(kernelContext_37);

#line 434
            uint _S132 = spCellsX_0(kernelContext_37);

#line 434
            uint _S133 = spCellsY_0(kernelContext_37);

#line 434
            _S127 = spBilinearTaps_0(x_5, y_5, _S131, _S132, _S133);

#line 434
        }

#line 434
        float bestWeight_1 = bestWeight_0;

#line 434
        uint best_5 = best_4;

#line 434
        uint tap_0 = 0U;

        for(;;)
        {

#line 436
            if(tap_0 < (_S127.count_0))
            {
            }
            else
            {

#line 436
                break;
            }

#line 434
            SpTaps_0 _S134 = _S127;

            uint _S135 = tap_0;

#line 436
            uint record_4;

            if(pass_0 == 1U)
            {

#line 438
                uint _S136 = spTableIndex_0(previous_0, _S134.index_0[_S135], kernelContext_37);
                uint device* _S137 = kernelContext_37->giResolve_0->slots_0+_S136;

#line 439
                uint slot_8 = *_S137;
                if((*_S137) == 4294967295U)
                {

#line 441
                    tap_0 = tap_0 + 1U;

#line 436
                    continue;
                }

#line 436
                uint _S138 = spUniformCount_0(kernelContext_37);

#line 436
                record_4 = _S138 + slot_8;

            }
            else
            {

#line 438
                record_4 = _S134.index_0[_S135];

#line 438
            }

#line 438
            SpProbe_0 _S139 = spLoadProbe_0(previous_0, record_4, kernelContext_37);

#line 446
            if((_S139.depth_0) <= 0.0)
            {

#line 447
                tap_0 = tap_0 + 1U;

#line 436
                continue;
            }

#line 450
            float weight_2 = _S127.weight_0[tap_0] * spGeometryWeight_0(_S139.position_4, _S139.normal_3, _S123, probe_2->normal_3, probe_2->depth_0, tolerance_1);

#line 450
            uint best_6;

#line 450
            float bestWeight_2;


            if(weight_2 > bestWeight_1)
            {

#line 453
                bestWeight_2 = weight_2;

#line 453
                best_6 = record_4;

#line 453
            }
            else
            {

#line 453
                bestWeight_2 = bestWeight_1;

#line 453
                best_6 = best_5;

#line 453
            }

#line 453
            bestWeight_1 = bestWeight_2;

#line 453
            best_5 = best_6;

#line 436
            tap_0 = tap_0 + 1U;

#line 436
        }

#line 432
        uint pass_1 = pass_0 + 1U;

#line 432
        bestWeight_0 = bestWeight_1;

#line 432
        best_4 = best_5;

#line 432
        pass_0 = pass_1;

#line 432
    }

#line 432
    bool _S140;

#line 459
    if(best_4 == 4294967295U)
    {

#line 459
        _S140 = true;

#line 459
    }
    else
    {

#line 459
        _S140 = bestWeight_0 < 0.25;

#line 459
    }

#line 459
    if(_S140)
    {

#line 460
        return false;
    }

#line 460
    spLoadCoefficients_0(previous_0, best_4, coefficients_2, history_1, kernelContext_37);


    *matched_0 = best_4;
    return true;
}


#line 73 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
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


#line 468 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
bool spProbeTraces_0(uint id_1, uint frame_1, float density_0)
{

#line 469
    if(density_0 >= 1.0)
    {

#line 470
        return true;
    }
    return (grUnit_0(grHash_0(id_1, frame_1, 9U))) < density_0;
}


#line 440 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
int gpFloorDiv_0(float value_3, float divisor_0)
{

#line 441
    return int(floor(value_3 / divisor_0));
}


#line 487
uint gpLayoutProbe_0(uint level_3, int3 cell_3, KernelContext_0 thread* kernelContext_38)
{

#line 488
    int3 centre_0 = (as_type<int3>((kernelContext_38->giProbes_0->constants_0[6U + level_3].xyz)));
    int half_0 = int(kernelContext_38->giProbes_0->constants_0[1U].w);
    int3 _S141 = int3(half_0, half_0, half_0);

#line 490
    bool _S142;

#line 490
    if(any(cell_3 < (centre_0 - _S141)))
    {

#line 490
        _S142 = true;

#line 490
    }
    else
    {

#line 490
        _S142 = any(cell_3 > (centre_0 + _S141));

#line 490
    }

#line 490
    if(_S142)
    {

#line 491
        return 4294967295U;
    }
    int edge_0 = int(kernelContext_38->giProbes_0->constants_0[5U].z);
    uint cells_0 = uint(edge_0 * edge_0 * edge_0);
    int _S143 = giWrap_0(cell_3.z, edge_0);

#line 495
    uint _S144 = uint(edge_0);

#line 495
    uint _S145 = uint(_S143) * _S144;

#line 495
    int _S146 = giWrap_0(cell_3.y, edge_0);

#line 495
    uint _S147 = (_S145 + uint(_S146)) * _S144;

    int _S148 = giWrap_0(cell_3.x, edge_0);
    return kernelContext_38->giProbes_0->layout_0[level_3 * cells_0 + (_S147 + uint(_S148))];
}


#line 114
constant array<float3, int(6)> kGpAxes_0 = { float3(1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, -1.0, 0.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, -1.0) };

#line 445
float gpVisibilityWeight_0(uint probe_3, float3 position_7, float3 query_0, KernelContext_0 thread* kernelContext_39)
{

#line 446
    float3 offset_1 = query_0 - position_7;
    float span_0 = length(offset_1);
    if(span_0 <= 0.00009999999747379)
    {

#line 449
        return 1.0;
    }
    float3 _S149 = offset_1 / float3(span_0) ;
    uint _S150 = probe_3 * 4U;

#line 452
    float4 device* _S151 = kernelContext_39->giProbes_0->probes_0+(_S150 + 2U);
    float4 device* _S152 = kernelContext_39->giProbes_0->probes_0+(_S150 + 3U);
    array<float, int(6)> _S153 = { (*_S151).x, (*_S151).y, (*_S151).z, (*_S151).w, (*_S152).x, (*_S152).y };

#line 454
    uint axis_0 = 0U;

#line 454
    float expected_0 = 0.0;

#line 454
    float weight_3 = 0.0;


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
        float _S154 = max(0.0, dot(kGpAxes_0[axis_0], _S149));
        float expected_1 = expected_0 + _S153[axis_0] * _S154;
        float weight_4 = weight_3 + _S154;

#line 457
        axis_0 = axis_0 + 1U;

#line 457
        expected_0 = expected_1;

#line 457
        weight_3 = weight_4;

#line 457
    }

#line 462
    if(weight_3 <= 0.0)
    {

#line 463
        return 1.0;
    }



    float slack_0 = (as_type<float>((kernelContext_39->giProbes_0->constants_0[1U].y)));
    float _S155 = expected_0 / weight_3 + slack_0;

#line 469
    if(span_0 > _S155)
    {

#line 470
        return 0.0;
    }
    float _S156 = max(slack_0, 0.00009999999747379);

#line 472
    float visible_0 = gpClamp_0((_S155 - span_0) / _S156, 0.0, 1.0);

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
        float cosine_0 = dot(kGpAxes_0[axis_0], _S149);
        if(cosine_0 < 0.5)
        {

#line 477
            axis_0 = axis_0 + 1U;

#line 474
            continue;
        }

#line 474
        visible_0 = min(visible_0, gpClamp_0((_S153[axis_0] + slack_0 - span_0 * cosine_0) / _S156, 0.0, 1.0));

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
)cy_msl",
    R"cy_msl(    return gpClamp_0((dot(normal_5, toward_0) + 1.0) * 0.5, 0.0, 1.0);
}


#line 123 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float giFinestVoxel_1(KernelContext_0 thread* kernelContext_40)
{

#line 124
    return (as_type<float>((kernelContext_40->giScene_0->constants_1[9U].y)));
}


#line 201
float giDistance_1(float3 p_2, KernelContext_0 thread* kernelContext_41)
{

#line 201
    uint _S157 = giLevelCount_0(kernelContext_41);

#line 201
    float answer_1 = 1.0e+06;

#line 201
    uint level_4 = 0U;


    for(;;)
    {

#line 204
        if(level_4 < _S157)
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
        float _S158 = giSampleLevel_0(level_4, p_2, &farDistance_2, kernelContext_41);

        float _S159 = min(answer_1, _S158);
        if(_S158 < farDistance_2)
        {

#line 209
            return _S158;
        }

#line 204
        uint level_5 = level_4 + 1U;

#line 204
        answer_1 = _S159;

#line 204
        level_4 = level_5;

#line 204
    }

#line 212
    return answer_1;
}


#line 225
GiTraceHit_0 giSphereTrace_1(float3 origin_6, float3 direction_4, float maxDistance_2, float tMin_1, KernelContext_0 thread* kernelContext_42)
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
    uint _S160 = giLevelCount_0(kernelContext_42);
    if(_S160 == 0U)
    {

#line 234
        return result_1;
    }

#line 234
    float _S161 = giFinestVoxel_0(kernelContext_42);


    float surface_1 = _S161 * 0.5;
    float _S162 = max(tMin_1, surface_1);
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
    float t_6 = _S162;


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
        if(t_6 > maxDistance_2)
        {
            (&result_1)->closest_0 = min(closest_2, min(recent_1[int(0)], min(recent_1[int(1)], recent_1[int(2)])));
            return result_1;
        }
        float3 position_8 = origin_6 + direction_4 * float3(t_6) ;

#line 248
        float _S163 = giDistance_0(position_8, kernelContext_42);

        if(_S163 < surface_1)
        {

#line 251
            (&result_1)->hit_1 = true;
            (&result_1)->t_1 = t_6;
            (&result_1)->position_1 = position_8;


            float3 _S164 = float3(_S161, 0.0, 0.0);

#line 256
            float _S165 = giDistance_0(position_8 + _S164, kernelContext_42);

#line 256
            float _S166 = giDistance_0(position_8 - _S164, kernelContext_42);

#line 256
            float _S167 = _S165 - _S166;

            float3 _S168 = float3(0.0, _S161, 0.0);

#line 258
            float _S169 = giDistance_0(position_8 + _S168, kernelContext_42);

#line 258
            float _S170 = giDistance_0(position_8 - _S168, kernelContext_42);

#line 258
            float _S171 = _S169 - _S170;

            float3 _S172 = float3(0.0, 0.0, _S161);

#line 260
            float _S173 = giDistance_0(position_8 + _S172, kernelContext_42);

#line 260
            float _S174 = giDistance_0(position_8 - _S172, kernelContext_42);

            (&result_1)->normal_1 = giNormalisedOr_0(float3(_S167, _S171, _S173 - _S174), - direction_4);
            (&result_1)->closest_0 = closest_2;
            return result_1;
        }
        float _S175 = min(closest_2, recent_1[recentSlot_1]);
        recent_1[recentSlot_1] = _S163;
        uint _S176 = (recentSlot_1 + 1U) % 3U;
        float t_7 = t_6 + max(_S163, surface_1);

#line 242
        uint step_7 = step_6 + 1U;

#line 242
        closest_2 = _S175;

#line 242
        recentSlot_1 = _S176;

#line 242
        step_6 = step_7;

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
bool grProbeOccluded_0(float3 from_0, float3 to_0, KernelContext_0 thread* kernelContext_43)
{

#line 287
    float3 offset_2 = to_0 - from_0;
    float span_1 = length(offset_2);

#line 288
    float _S177 = giFinestVoxel_1(kernelContext_43);

    if(span_1 <= (_S177 * 2.0))
    {

#line 291
        return false;
    }

#line 291
    float _S178 = giDistance_1(from_0, kernelContext_43);

#line 291
    float _S179 = giDistance_1(to_0, kernelContext_43);

    if((_S178 + _S179) >= span_1)
    {

#line 294
        return false;
    }

#line 294
    GiTraceHit_0 _S180 = giSphereTrace_1(from_0, offset_2 / float3(span_1) , span_1 - _S177, 0.0, kernelContext_43);

    return _S180.hit_1;
}


#line 273 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
void gpLoadPayload_0(uint probe_4, uint floats_0, array<float, int(48)> thread* payload_0, KernelContext_0 thread* kernelContext_44)
{

#line 273
    uint index_1 = 0U;
    for(;;)
    {

#line 274
        if(index_1 < 48U)
        {
        }
        else
        {

#line 274
            break;
        }

#line 274
        float _S181;
        if(index_1 < floats_0)
        {

#line 275
            _S181 = *(kernelContext_44->giProbes_0->payloads_0+(probe_4 * floats_0 + index_1));

#line 275
        }
        else
        {

#line 275
            _S181 = 0.0;

#line 275
        }

#line 275
        (*payload_0)[index_1] = _S181;

#line 274
        index_1 = index_1 + 1U;

#line 274
    }


    return;
}


#line 110
float3 gpMax0_0(float3 v_2)
{

#line 111
    return float3(max(0.0, v_2.x), max(0.0, v_2.y), max(0.0, v_2.z));
}


#line 154
void gpOctahedralUv_0(float3 direction_5, float thread* u_0, float thread* v_3)
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
    float _S182 = p_3.x;
    float _S183 = p_3.y;

#line 158
    float px_0;

#line 158
    float py_0;
    if((p_3.z) < 0.0)
    {

#line 160
        float _S184 = 1.0 - abs(_S183);

#line 160
        if(_S182 >= 0.0)
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
        float fx_3 = _S184 * px_0;
        float _S185 = 1.0 - abs(_S182);

#line 161
        if(_S183 >= 0.0)
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
        float fy_3 = _S185 * px_0;

#line 161
        px_0 = fx_3;

#line 161
        py_0 = fy_3;

#line 159
    }
    else
    {

#line 159
        px_0 = _S182;

#line 159
        py_0 = _S183;

#line 159
    }

#line 165
    *u_0 = gpClamp_0(px_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    *v_3 = gpClamp_0(py_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    return;
}


#line 220
float3 gpDecode_0(uint encoding_0, const array<float, int(48)> thread* payload_1, float3 direction_6)
{

#line 220
    uint c_0;
    if(encoding_0 == 0U)
    {



        array<float, int(4)> _S186 = { 0.28209498524665833, 0.32573533058166504 * direction_6.y, 0.32573533058166504 * direction_6.z, 0.32573533058166504 * direction_6.x };

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
            uint _S187 = c_0 * 3U;

#line 230
            result_2.x = result_2.x + (*payload_1)[_S187] * _S186[c_0];
            result_2.y = result_2.y + (*payload_1)[_S187 + 1U] * _S186[c_0];
            result_2.z = result_2.z + (*payload_1)[_S187 + 2U] * _S186[c_0];

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
        thread float v_4 = 0.0;
        gpOctahedralUv_0(direction_6, &u_1, &v_4);
        uint x0_1 = uint(u_1);
        uint y0_1 = uint(v_4);
        uint _S188 = min(x0_1 + 1U, 3U);



        uint _S189 = y0_1 * 4U;

        uint _S190 = min(y0_1 + 1U, 3U) * 4U;

        uint _S191 = (_S189 + x0_1) * 3U;

#line 250
        float3 f00_0 = float3((*payload_1)[_S191], (*payload_1)[_S191 + 1U], (*payload_1)[_S191 + 2U]);
        uint _S192 = (_S189 + _S188) * 3U;
        uint _S193 = (_S190 + x0_1) * 3U;

#line 252
        float3 f01_0 = float3((*payload_1)[_S193], (*payload_1)[_S193 + 1U], (*payload_1)[_S193 + 2U]);
        uint _S194 = (_S190 + _S188) * 3U;

#line 253
        float3 _S195 = float3((u_1 - float(x0_1))) ;
        float3 top_0 = f00_0 + (float3((*payload_1)[_S192], (*payload_1)[_S192 + 1U], (*payload_1)[_S192 + 2U]) - f00_0) * _S195;

        return top_0 + (f01_0 + (float3((*payload_1)[_S194], (*payload_1)[_S194 + 1U], (*payload_1)[_S194 + 2U]) - f01_0) * _S195 - top_0) * float3((v_4 - float(y0_1))) ;
    }
    float3 _S196 = float3(0.0, 0.0, 0.0);

#line 258
    float3 result_3 = _S196;

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
        float _S197 = max(0.0, dot(kGpAxes_0[c_0], direction_6));
        if(_S197 <= 0.0)
        {

#line 263
            c_0 = c_0 + 1U;

#line 260
            continue;
        }



        uint _S198 = c_0 * 3U;

        float total_1 = total_0 + _S197;

#line 267
        result_3 = result_3 + float3((*payload_1)[_S198], (*payload_1)[_S198 + 1U], (*payload_1)[_S198 + 2U]) * float3(_S197) ;

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
        result_3 = _S196;

#line 269
    }

#line 269
    return result_3;
}


#line 305 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float4 grFinish_0(float3 total_2, float weight_5, float oldest_0)
{

#line 306
    if(weight_5 <= 0.0)
    {

#line 307
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    return float4(total_2 / float3(weight_5) , gpClamp_0(1.0 - gpClamp_0(oldest_0 / 240.0, 0.0, 1.0) * 0.75, 0.0, 1.0));
}


#line 317
void grSampleTraced_0(float3 position_9, float3 normal_6, float3 reflection_0, uint frame_2, float4 thread* diffuse_0, float4 thread* specular_0, KernelContext_0 thread* kernelContext_45)
{
    uint4 counts_0 = kernelContext_45->giProbes_0->constants_0[0U];
    uint _S199 = counts_0.x;
    uint _S200 = counts_0.y;
    uint _S201 = kernelContext_45->giProbes_0->constants_0[1U].z;
    float _S202 = (as_type<float>((kernelContext_45->giProbes_0->constants_0[5U].x)));
    float3 _S203 = position_9 + normal_6 * float3((as_type<float>((kernelContext_45->giProbes_0->constants_0[1U].y)))) ;
    float3 _S204 = float3(0.0, 0.0, 0.0);

#line 325
    float3 diffuseTotal_0 = _S204;

#line 325
    float diffuseOldest_0 = 0.0;

#line 325
    float3 specularTotal_0 = _S204;

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
        if(level_6 < _S201)
        {
        }
        else
        {

#line 331
            break;
        }

#line 332
        float spacing_0 = _S202 * float(1U << level_6);
        float _S205 = position_9.x;

#line 333
        int _S206 = gpFloorDiv_0(_S205 - spacing_0, spacing_0);
        float _S207 = position_9.y;

#line 334
        int _S208 = gpFloorDiv_0(_S207 - spacing_0, spacing_0);
        float _S209 = position_9.z;
        int _S210 = gpFloorDiv_0(_S205 + spacing_0, spacing_0);
        int _S211 = gpFloorDiv_0(_S207 + spacing_0, spacing_0);
        int _S212 = gpFloorDiv_0(_S209 + spacing_0, spacing_0);

#line 338
        int z_2 = gpFloorDiv_0(_S209 - spacing_0, spacing_0);
        for(;;)
        {

#line 339
            if(z_2 <= _S212)
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
            int y_6 = _S208;

#line 339
            float diffuseWeight_1 = diffuseWeight_0;

#line 339
            float specularWeight_1 = specularWeight_0;
            for(;;)
            {

#line 340
                if(y_6 <= _S211)
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
                int x_6 = _S206;

#line 340
                float diffuseWeight_2 = diffuseWeight_1;

#line 340
                float specularWeight_2 = specularWeight_1;
                for(;;)
                {

#line 341
                    if(x_6 <= _S210)
                    {
                    }
                    else
                    {

#line 341
                        break;
                    }

#line 341
                    uint _S213 = gpLayoutProbe_0(level_6, int3(x_6, y_6, z_2), kernelContext_45);

                    if(_S213 == 4294967295U)
                    {

#line 344
                        x_6 = x_6 + int(1);

#line 341
                        continue;
                    }



                    uint _S214 = _S213 * 4U;

#line 346
                    float4 device* _S215 = kernelContext_45->giProbes_0->probes_0+(_S214 + 1U);

#line 346
                    float4 record_5 = *_S215;

#line 346
                    bool _S216;
                    if(((*_S215).y) == 0.0)
                    {

#line 347
                        _S216 = true;

#line 347
                    }
                    else
                    {

#line 347
                        _S216 = (record_5.z) == 0.0;

#line 347
                    }

#line 347
                    if(_S216)
                    {

#line 348
                        x_6 = x_6 + int(1);

#line 341
                        continue;
                    }

#line 350
                    float3 at_0 = (*(kernelContext_45->giProbes_0->probes_0+_S214)).xyz;
                    float3 offset_3 = position_9 - at_0;
                    float span_2 = length(offset_3);
                    if(span_2 > spacing_0)
                    {

#line 354
                        x_6 = x_6 + int(1);

#line 341
                        continue;
                    }

#line 356
                    float _S217 = 1.0 - gpClamp_0(span_2 / spacing_0, 0.0, 1.0);

#line 356
                    float _S218 = gpVisibilityWeight_0(_S213, at_0, position_9, kernelContext_45);

#line 356
                    float falloff_0 = _S217 * _S218;

                    bool apart_0 = span_2 > 0.00009999999747379;

#line 358
                    float3 toward_1;
                    if(apart_0)
                    {

#line 359
)cy_msl",
    R"cy_msl(                        toward_1 = - offset_3 / float3(span_2) ;

#line 359
                    }
                    else
                    {

#line 359
                        toward_1 = normal_6;

#line 359
                    }

#line 359
                    float diffuseShare_0;
                    if(apart_0)
                    {

#line 360
                        diffuseShare_0 = falloff_0 * grFacing_0(toward_1, normal_6);

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
                    bool _S219;
                    if(diffuseShare_0 <= 0.0)
                    {

#line 363
                        _S219 = specularShare_0 <= 0.0;

#line 363
                    }
                    else
                    {

#line 363
                        _S219 = false;

#line 363
                    }

#line 363
                    bool _S220;

#line 363
                    if(_S219)
                    {

#line 363
                        _S220 = true;

#line 363
                    }
                    else
                    {

#line 363
                        bool _S221 = grProbeOccluded_0(_S203, at_0, kernelContext_45);

#line 363
                        _S220 = _S221;

#line 363
                    }

#line 363
                    if(_S220)
                    {
                        x_6 = x_6 + int(1);

#line 341
                        continue;
                    }

#line 367
                    thread array<float, int(48)> data_0;

#line 367
                    gpLoadPayload_0(_S213, _S200, &data_0, kernelContext_45);

                    uint last_2 = (as_type<uint>((record_5.x)));

#line 369
                    float age_1;
                    if(frame_2 > last_2)
                    {

#line 370
                        age_1 = float(frame_2 - last_2);

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
                        thread array<float, int(48)> _S222 = data_0;

#line 371
                        float3 _S223 = gpDecode_0(_S199, &_S222, normal_6);

                        float diffuseWeight_4 = diffuseWeight_2 + diffuseShare_0;
                        float _S224 = max(diffuseOldest_2, age_1);

#line 374
                        diffuseTotal_3 = diffuseTotal_2 + _S223 * float3(diffuseShare_0) ;

#line 374
                        diffuseOldest_3 = _S224;

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
                        thread array<float, int(48)> _S225 = data_0;

#line 376
                        float3 _S226 = gpDecode_0(_S199, &_S225, reflection_0);


                        float specularWeight_4 = specularWeight_2 + specularShare_0;
                        float _S227 = max(specularOldest_2, age_1);

#line 380
                        specularTotal_3 = specularTotal_2 + _S226 * float3(specularShare_0) ;

#line 380
                        specularOldest_3 = _S227;

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
                    x_6 = x_6 + int(1);

#line 341
                }

#line 340
                int y_7 = y_6 + int(1);

#line 340
                diffuseTotal_1 = diffuseTotal_2;

#line 340
                diffuseOldest_1 = diffuseOldest_2;

#line 340
                specularTotal_1 = specularTotal_2;

#line 340
                specularOldest_1 = specularOldest_2;

#line 340
                y_6 = y_7;

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


#line 246 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
void spStorePacked_0(uint base_3, const array<float3, int(9)> thread* coefficients_3, float last_3, KernelContext_0 thread* kernelContext_46)
{

#line 247
    thread array<float, int(28)> values_1;

#line 247
    uint k_2 = 0U;
    for(;;)
    {

#line 248
        if(k_2 < 9U)
        {
        }
        else
        {

#line 248
            break;
        }

#line 249
        uint _S228 = k_2 * 3U;

#line 249
        values_1[_S228] = (*coefficients_3)[k_2].x;
        values_1[_S228 + 1U] = (*coefficients_3)[k_2].y;
        values_1[_S228 + 2U] = (*coefficients_3)[k_2].z;

#line 248
        k_2 = k_2 + 1U;

#line 248
    }

#line 253
    values_1[int(27)] = last_3;

#line 253
    uint v_5 = 0U;
    for(;;)
    {

#line 254
        if(v_5 < 7U)
        {
        }
        else
        {

#line 254
            break;
        }

#line 255
        uint _S229 = v_5 * 4U;

#line 255
        *(kernelContext_46->giResolve_0->probes_1+(base_3 + v_5)) = float4(values_1[_S229], values_1[_S229 + 1U], values_1[_S229 + 2U], values_1[_S229 + 3U]);

#line 254
        v_5 = v_5 + 1U;

#line 254
    }



    return;
}




void spStoreCoefficients_0(uint slot_9, uint record_6, const array<float3, int(9)> thread* coefficients_4, float history_2, KernelContext_0 thread* kernelContext_47)
{

#line 264
    uint _S230 = spRecordBase_1(slot_9, record_6, kernelContext_47);

#line 264
    spStorePacked_0(_S230 + 2U, coefficients_4, history_2, kernelContext_47);

    return;
}


#line 274
void spStoreFiltered_0(uint slot_10, uint record_7, const array<float3, int(9)> thread* coefficients_5, KernelContext_0 thread* kernelContext_48)
{

#line 274
    uint _S231 = spRecordBase_1(slot_10, record_7, kernelContext_48);

#line 274
    spStorePacked_0(_S231 + 11U, coefficients_5, 0.0, kernelContext_48);

    return;
}


#line 88 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
void grBasis_0(float3 normal_7, float3 thread* tangent_0, float3 thread* bitangent_0)
{

#line 89
    float _S232 = normal_7.z;

#line 89
    float sign_0;

#line 89
    if(_S232 >= 0.0)
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
    float a_1 = -1.0 / (sign_0 + _S232);
    float _S233 = normal_7.x;

#line 91
    float _S234 = normal_7.y;

#line 91
    float b_1 = _S233 * _S234 * a_1;
    *tangent_0 = float3(1.0 + sign_0 * _S233 * _S233 * a_1, sign_0 * b_1, - sign_0 * _S233);
    *bitangent_0 = float3(b_1, sign_0 + _S234 * _S234 * a_1, - _S234);
    return;
}


#line 192 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
float3 spHemisphereDirection_0(float3 normal_8, float z_4, float turn_0)
{

#line 193
    thread float3 tangent_1;
    thread float3 bitangent_1;
    grBasis_0(normal_8, &tangent_1, &bitangent_1);
    float radius_1 = sqrt(max(0.0, 1.0 - z_4 * z_4));
    float angle_0 = 6.28318548202514648 * turn_0;
    return giNormalisedOr_0(tangent_1 * float3((radius_1 * cos(angle_0)))  + bitangent_1 * float3((radius_1 * sin(angle_0)))  + normal_8 * float3(z_4) , normal_8);
}


#line 94
void spBasis_0(float3 d_0, array<float, int(9)> thread* basis_0)
{

#line 95
    (*basis_0)[int(0)] = 0.28209498524665833;
    float _S235 = d_0.y;

#line 96
    (*basis_0)[int(1)] = 0.48860299587249756 * _S235;
    float _S236 = d_0.z;

#line 97
    (*basis_0)[int(2)] = 0.48860299587249756 * _S236;
    float _S237 = d_0.x;

#line 98
    (*basis_0)[int(3)] = 0.48860299587249756 * _S237;
    (*basis_0)[int(4)] = 1.09254801273345947 * (_S237 * _S235);
    (*basis_0)[int(5)] = 1.09254801273345947 * (_S235 * _S236);
    (*basis_0)[int(6)] = 0.31539198756217957 * (3.0 * (_S236 * _S236) - 1.0);
    (*basis_0)[int(7)] = 1.09254801273345947 * (_S237 * _S236);
    (*basis_0)[int(8)] = 0.54627400636672974 * (_S237 * _S237 - _S235 * _S235);
    return;
}


#line 106
uint spBandOf_0(uint k_3)
{

#line 106
    uint _S238;
    if(k_3 == 0U)
    {

#line 107
        _S238 = 0U;

#line 107
    }
    else
    {

#line 107
        if(k_3 < 4U)
        {

#line 107
            _S238 = 1U;

#line 107
        }
        else
        {

#line 107
            _S238 = 2U;

#line 107
        }

#line 107
    }

#line 107
    return _S238;
}

float3 spEvaluate_0(const array<float3, int(9)> thread* coefficients_6, float3 direction_7, float3 band_0)
{

#line 111
    thread array<float, int(9)> basis_1;
    spBasis_0(direction_7, &basis_1);

#line 112
    float3 sum_1 = float3(0.0, 0.0, 0.0);

#line 112
    uint k_4 = 0U;

    for(;;)
    {

#line 114
        if(k_4 < 9U)
        {
        }
        else
        {

#line 114
            break;
        }

#line 115
        uint b_2 = spBandOf_0(k_4);

#line 115
        float scale_0;
        if(b_2 == 0U)
        {

#line 116
            scale_0 = band_0.x;

#line 116
        }
        else
        {

#line 116
            if(b_2 == 1U)
            {

#line 116
                scale_0 = band_0.y;

#line 116
            }
            else
            {

#line 116
                scale_0 = band_0.z;

#line 116
            }

#line 116
        }
        float3 _S239 = sum_1 + (*coefficients_6)[k_4] * float3((basis_1[k_4] * scale_0)) ;

#line 114
        uint k_5 = k_4 + 1U;

#line 114
        sum_1 = _S239;

#line 114
        k_4 = k_5;

#line 114
    }

#line 119
    return float3(max(sum_1.x, 0.0), max(sum_1.y, 0.0), max(sum_1.z, 0.0));
}


#line 506 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
float4 gpSample_0(float3 position_10, float3 normal_9, uint frame_3, KernelContext_0 thread* kernelContext_49)
{

#line 507
    uint4 counts_1 = kernelContext_49->giProbes_0->constants_0[0U];
    uint _S240 = counts_1.x;
    uint _S241 = counts_1.y;
    uint _S242 = kernelContext_49->giProbes_0->constants_0[1U].z;
    float _S243 = (as_type<float>((kernelContext_49->giProbes_0->constants_0[5U].x)));

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
        if(level_7 < _S242)
        {
        }
        else
        {

#line 515
            break;
        }

#line 516
        float spacing_1 = _S243 * float(1U << level_7);
        float radius_2 = spacing_1 * 1.5;
        float _S244 = position_10.x;

#line 518
        int _S245 = gpFloorDiv_0(_S244 - radius_2, spacing_1);
        float _S246 = position_10.y;

#line 519
        int _S247 = gpFloorDiv_0(_S246 - radius_2, spacing_1);
        float _S248 = position_10.z;
        int _S249 = gpFloorDiv_0(_S244 + radius_2, spacing_1);
        int _S250 = gpFloorDiv_0(_S246 + radius_2, spacing_1);
        int _S251 = gpFloorDiv_0(_S248 + radius_2, spacing_1);

#line 523
        int z_5 = gpFloorDiv_0(_S248 - radius_2, spacing_1);
        for(;;)
        {

#line 524
            if(z_5 <= _S251)
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
            int y_8 = _S247;

#line 524
            float totalWeight_1 = totalWeight_0;
            for(;;)
            {

#line 525
                if(y_8 <= _S250)
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
                int x_7 = _S245;

#line 525
                float totalWeight_2 = totalWeight_1;
                for(;;)
                {

#line 526
                    if(x_7 <= _S249)
                    {
                    }
                    else
                    {

#line 526
                        break;
                    }

#line 526
                    uint _S252 = gpLayoutProbe_0(level_7, int3(x_7, y_8, z_5), kernelContext_49);

                    if(_S252 == 4294967295U)
                    {

#line 529
                        x_7 = x_7 + int(1);

#line 526
                        continue;
                    }



                    uint _S253 = _S252 * 4U;

#line 531
                    float4 device* _S254 = kernelContext_49->giProbes_0->probes_0+(_S253 + 1U);

#line 531
                    float4 record_8 = *_S254;

#line 531
                    bool _S255;
                    if(((*_S254).y) == 0.0)
                    {

#line 532
                        _S255 = true;

#line 532
                    }
                    else
                    {

#line 532
                        _S255 = (record_8.z) == 0.0;

#line 532
                    }

#line 532
                    if(_S255)
                    {

#line 533
                        x_7 = x_7 + int(1);

#line 526
                        continue;
                    }

#line 535
                    float3 at_1 = (*(kernelContext_49->giProbes_0->probes_0+_S253)).xyz;
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
)cy_msl",
    R"cy_msl(                        weight_7 = weight_6 * gpClamp_0((dot(normal_9, - offset_4 / float3(span_3) ) + 1.0) * 0.5, 0.0, 1.0);

#line 542
                    }
                    else
                    {

#line 542
                        weight_7 = weight_6;

#line 542
                    }

#line 542
                    float _S256 = gpVisibilityWeight_0(_S252, at_1, position_10, kernelContext_49);



                    float weight_8 = weight_7 * _S256;
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
                    gpLoadPayload_0(_S252, _S241, &data_1, kernelContext_49);

#line 550
                    thread array<float, int(48)> _S257 = data_1;

#line 550
                    float3 _S258 = gpDecode_0(_S240, &_S257, normal_9);

                    float3 _S259 = total_5 + _S258 * float3(weight_8) ;
                    float totalWeight_3 = totalWeight_2 + weight_8;
                    uint last_4 = (as_type<uint>((record_8.x)));

#line 554
                    float age_2;
                    if(frame_3 > last_4)
                    {

#line 555
                        age_2 = float(frame_3 - last_4);

#line 555
                    }
                    else
                    {

#line 555
                        age_2 = 0.0;

#line 555
                    }
                    float _S260 = max(oldest_3, age_2);

#line 556
                    total_5 = _S259;

#line 556
                    oldest_3 = _S260;

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


#line 138 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
float spLuminance_0(float3 c_1)
{

#line 139
    return 0.2125999927520752 * c_1.x + 0.71520000696182251 * c_1.y + 0.07220000028610229 * c_1.z;
}


#line 203
uint spGuideCell_0(float z_7, float turn_1)
{

    return min(uint(z_7 * 4.0), 3U) * 4U + min(uint(turn_1 * 4.0), 3U);
}


#line 263 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
GrSample_0 grGatherSample_0(const GrSample_0 thread* ray_0, const GrSample_0 thread* standIn_0, uint excluded_0)
{

#line 263
    uint _S261 = standIn_0->source_0;

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
        hasStandIn_0 = (excluded_0 & (1U << _S261)) == 0U;

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
        (&sample_3)->source_0 = _S261;
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


#line 168
float grFarFieldWeight_0(float distance_0, KernelContext_0 thread* kernelContext_50)
{

#line 169
    float4 far_0 = (as_type<float4>((kernelContext_50->giResolve_0->constants_2[3U])));
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
float grSkyVisibilityRay_0(float3 position_11, float3 normal_11, uint index_3, uint count_3, KernelContext_0 thread* kernelContext_51)
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
        uint _S265 = giLevelCount_0(kernelContext_51);

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
    float reach_2 = (as_type<float>((kernelContext_51->giResolve_0->constants_2[3U].w)));

#line 188
    float _S266 = giFinestVoxel_0(kernelContext_51);

#line 188
    GiTraceHit_0 _S267 = giSphereTrace_0(position_11 + normal_11 * float3((_S266 * 2.0)) , giHemisphereDirection_0(normal_11, index_3, count_3), reach_2, 0.0, kernelContext_51);

#line 193
    if(_S267.hit_1)
    {

#line 194
        return 0.0;
    }
    return gpClamp_0(_S267.closest_0 / (_S266 * 4.0), 0.0, 1.0);
}


#line 528 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
void spTrace_0(uint record_9, uint lane_0, KernelContext_0 thread* kernelContext_52)
{

#line 528
    uint _S268 = spSlot_0(kernelContext_52);

#line 528
    bool allowed_0;


    if(record_9 == 0U)
    {

#line 531
        allowed_0 = lane_0 == 0U;

#line 531
    }
    else
    {

#line 531
        allowed_0 = false;

#line 531
    }

#line 531
    if(allowed_0)
    {

#line 532
        *(kernelContext_52->giResolve_0->slots_0+spCounter_0(1U - _S268)) = 0U;

#line 531
    }

#line 531
    uint _S269 = spUniformCount_0(kernelContext_52);



    if(record_9 >= _S269)
    {

#line 536
        uint _S270 = record_9 - _S269;

#line 536
        uint _S271 = *(kernelContext_52->giResolve_0->slots_0+spCounter_0(_S268));

#line 536
        uint _S272 = spCapacity_0(kernelContext_52);

#line 536
        allowed_0 = _S270 >= (min(_S271, _S272));

#line 535
    }
    else
    {

#line 535
        allowed_0 = false;

#line 535
    }

#line 535
    if(allowed_0)
    {
        return;
    }

#line 537
    SpProbe_0 _S273 = spLoadProbe_0(_S268, record_9, kernelContext_52);


    if((_S273.depth_0) <= 0.0)
    {

#line 541
        return;
    }
    uint4 shape_1 = kernelContext_52->giResolve_0->constants_2[42U];
    float4 weights_0 = (as_type<float4>((kernelContext_52->giResolve_0->constants_2[43U])));
    uint frame_4 = kernelContext_52->giResolve_0->constants_2[18U].x;
    uint cacheFrame_2 = kernelContext_52->giResolve_0->constants_2[4U].w;

    bool _S274 = lane_0 == 0U;

#line 548
    uint cell_4;

#line 548
    if(_S274)
    {

#line 549
        thread array<float3, int(9)> previous_1;
        thread float history_3 = 0.0;
        thread uint matched_1 = 4294967295U;
        if(((shape_1.w) & 2U) != 0U)
        {

#line 552
            allowed_0 = (kernelContext_52->giResolve_0->constants_2[5U].w) != 0U;

#line 552
        }
        else
        {

#line 552
            allowed_0 = false;

#line 552
        }

#line 552
        bool found_0;
        if(allowed_0)
        {

#line 553
            float _S275 = weights_0.y;

#line 553
            thread SpProbe_0 _S276 = _S273;

#line 553
            bool _S277 = spReproject_0(&_S276, _S275, &previous_1, &history_3, &matched_1, kernelContext_52);

#line 553
            found_0 = _S277;

#line 553
        }
        else
        {

#line 553
            found_0 = false;

#line 553
        }

#line 553
        cell_4 = 0U;
        for(;;)
        {

#line 554
            if(cell_4 < 9U)
            {
            }
            else
            {

#line 554
                break;
            }

#line 555
            (*kernelContext_52->spHistory_0)[cell_4] = previous_1[cell_4];

#line 554
            cell_4 = cell_4 + 1U;

#line 554
        }


        *kernelContext_52->spHistoryCount_0 = history_3;
        if(found_0)
        {

#line 558
            cell_4 = 1U;

#line 558
        }
        else
        {

#line 558
            cell_4 = 0U;

#line 558
        }

#line 558
        *kernelContext_52->spReprojected_0 = cell_4;
        float density_1 = (as_type<float>((kernelContext_52->giResolve_0->constants_2[1U].w)));
        if(found_0)
        {

#line 560
            allowed_0 = !spProbeTraces_0(_S273.id_0, frame_4, density_1);

#line 560
        }
        else
        {

#line 560
            allowed_0 = false;

#line 560
        }

#line 560
        if(allowed_0)
        {

#line 560
            cell_4 = matched_1;

#line 560
        }
        else
        {

#line 560
            cell_4 = 4294967295U;

#line 560
        }

#line 560
        *kernelContext_52->spCarried_0 = cell_4;
        if(cell_4 == 4294967295U)
        {

#line 561
            float3 _S278 = _S273.normal_3;
            thread float4 diffuse_1;
            thread float4 specular_1;

#line 563
            grSampleTraced_0(_S273.position_4, _S278, _S278, cacheFrame_2, &diffuse_1, &specular_1, kernelContext_52);


            *kernelContext_52->spStandIn_0 = diffuse_1;
            float _S279 = (as_type<float>((kernelContext_52->giResolve_0->constants_2[2U].y)));

#line 567
            thread SpProbe_0 _S280 = _S273;

#line 567
            float3 _S281 = spProbeOrigin_0(&_S280, _S279, kernelContext_52);

#line 567
            *kernelContext_52->spOrigin_0 = _S281;

#line 561
        }

#line 548
    }

#line 570
    threadgroup_barrier(mem_flags::mem_threadgroup);

#line 570
    uint k_6;
    if((*kernelContext_52->spCarried_0) != 4294967295U)
    {

        if(_S274)
        {

#line 575
            thread array<float3, int(9)> kept_0;

#line 575
            k_6 = 0U;
            for(;;)
            {

#line 576
                if(k_6 < 9U)
                {
                }
                else
                {

#line 576
                    break;
                }

#line 577
                kept_0[k_6] = (*kernelContext_52->spHistory_0)[k_6];

#line 576
                k_6 = k_6 + 1U;

#line 576
            }


            float _S282 = *kernelContext_52->spHistoryCount_0;

#line 579
            thread array<float3, int(9)> _S283 = kept_0;

#line 579
            spStoreCoefficients_0(_S268, record_9, &_S283, _S282, kernelContext_52);

#line 579
            thread array<float3, int(9)> _S284 = kept_0;

#line 579
            spStoreFiltered_0(_S268, record_9, &_S284, kernelContext_52);

#line 579
            uint _S285 = spRecordBase_0(1U - _S268, *kernelContext_52->spCarried_0, kernelContext_52);

#line 579
            uint _S286 = spRecordBase_0(_S268, record_9, kernelContext_52);



            *(kernelContext_52->giResolve_0->probes_1+(_S286 + 9U)) = *(kernelContext_52->giResolve_0->probes_1+(_S285 + 9U));
            *(kernelContext_52->giResolve_0->probes_1+(_S286 + 10U)) = *(kernelContext_52->giResolve_0->probes_1+(_S285 + 10U));

#line 574
        }

#line 586
        return;
    }

#line 586
    float3 value_4;


    if(lane_0 < 16U)
    {

#line 590
        float z_9 = (float(lane_0 / 4U) + 0.5) / 4.0;

        float3 direction_8 = spHemisphereDirection_0(_S273.normal_3, z_9, (float(lane_0 % 4U) + 0.5) / 4.0);

        if((*kernelContext_52->spReprojected_0) != 0U)
        {

#line 595
            thread array<float3, int(9)> previous_2;

#line 595
            cell_4 = 0U;
            for(;;)
            {

#line 596
                if(cell_4 < 9U)
                {
                }
                else
                {

#line 596
                    break;
                }

#line 597
                previous_2[cell_4] = (*kernelContext_52->spHistory_0)[cell_4];

#line 596
                cell_4 = cell_4 + 1U;

#line 596
            }


            float3 _S287 = float3(1.0, 1.0, 1.0);

#line 599
            thread array<float3, int(9)> _S288 = previous_2;

#line 599
            float3 _S289 = spEvaluate_0(&_S288, direction_8, _S287);

#line 599
            value_4 = _S289;

#line 594
        }
        else
        {

#line 594
            float4 _S290 = gpSample_0(_S273.position_4, direction_8, cacheFrame_2, kernelContext_52);

#line 594
            value_4 = _S290.xyz;

#line 594
        }

#line 603
        (*kernelContext_52->spWeights_0)[lane_0] = max(spLuminance_0(value_4), 0.0) * z_9;

#line 589
    }

#line 605
    threadgroup_barrier(mem_flags::mem_threadgroup);

#line 605
    cell_4 = 0U;

#line 605
    float guideTotal_0 = 0.0;


    for(;;)
    {

#line 608
        if(cell_4 < 16U)
        {
        }
        else
        {

#line 608
            break;
        }

#line 609
        float guideTotal_1 = guideTotal_0 + (*kernelContext_52->spWeights_0)[cell_4];

#line 608
        cell_4 = cell_4 + 1U;

#line 608
        guideTotal_0 = guideTotal_1;

#line 608
    }

#line 608
    float guided_0;


    if(guideTotal_0 > 0.0)
    {

#line 611
        guided_0 = gpClamp_0(weights_0.z, 0.0, 0.89999997615814209);

#line 611
    }
    else
    {

#line 611
        guided_0 = 0.0;

#line 611
    }
    float uniformShare_0 = (1.0 - guided_0) * 0.5;

    uint _S291 = min(max(shape_1.x, 1U), 64U);

#line 614
    float history_4;

#line 614
    float openness_0;

    if(lane_0 < _S291)
    {

#line 617
        uint hash_3 = grHash_0(_S273.id_0 * 64U + lane_0, frame_4, 8U);
        float u0_0 = grUnit_0(hash_3);
        float u1_0 = grUnit_0(hash_3 * 656542357U);
        float u2_0 = grUnit_0(hash_3 * 2654435769U);


        if(u0_0 >= uniformShare_0)
        {

#line 623
            allowed_0 = u0_0 < (uniformShare_0 + uniformShare_0);

#line 623
        }
        else
        {

#line 623
            allowed_0 = false;

#line 623
        }

#line 623
        if(allowed_0)
        {

#line 623
            history_4 = sqrt(u1_0);

#line 623
            openness_0 = u2_0;

#line 623
        }
        else
        {

#line 625
            if(u0_0 >= (uniformShare_0 + uniformShare_0))
            {

#line 626
                float _S292 = (u0_0 - uniformShare_0 - uniformShare_0) / guided_0 * guideTotal_0;

#line 626
                cell_4 = 0U;

#line 626
                history_4 = 0.0;


)cy_msl",
    R"cy_msl(                for(;;)
                {

#line 629
                    if(cell_4 < 16U)
                    {
                    }
                    else
                    {

#line 629
                        k_6 = 15U;

#line 629
                        break;
                    }

#line 630
                    float running_0 = history_4 + (*kernelContext_52->spWeights_0)[cell_4];
                    if(((*kernelContext_52->spWeights_0)[cell_4]) > 0.0)
                    {

#line 631
                        allowed_0 = _S292 < running_0;

#line 631
                    }
                    else
                    {

#line 631
                        allowed_0 = false;

#line 631
                    }

#line 631
                    if(allowed_0)
                    {

#line 631
                        k_6 = cell_4;

                        break;
                    }

#line 629
                    cell_4 = cell_4 + 1U;

#line 629
                    history_4 = running_0;

#line 629
                }

#line 637
                float _S293 = (float(k_6 % 4U) + u2_0) / 4.0;

#line 637
                history_4 = (float(k_6 / 4U) + u1_0) / 4.0;

#line 637
                openness_0 = _S293;

#line 625
            }
            else
            {

#line 625
                history_4 = u1_0;

#line 625
                openness_0 = u2_0;

#line 625
            }

#line 623
        }

#line 639
        float density_2 = uniformShare_0 / 6.28318548202514648 + uniformShare_0 * history_4 / 3.14159274101257324;

#line 639
        float density_3;

        if(guided_0 > 0.0)
        {

#line 641
            density_3 = density_2 + guided_0 * ((*kernelContext_52->spWeights_0)[spGuideCell_0(history_4, openness_0)] / guideTotal_0) * 2.5464789867401123;

#line 641
        }
        else
        {

#line 641
            density_3 = density_2;

#line 641
        }



        float3 _S294 = float3(0.0, 0.0, 0.0);
        float3 direction_9 = spHemisphereDirection_0(_S273.normal_3, history_4, openness_0);
        bool _S295 = density_3 > 0.0;

#line 647
        if(_S295)
        {
            uint excluded_1 = kernelContext_52->giResolve_0->constants_2[2U].w;

            float4 standIn_1 = *kernelContext_52->spStandIn_0;

#line 651
            GrSample_0 _S296 = spProbeRay_0(*kernelContext_52->spOrigin_0, giNormalisedOr_0(direction_9, float3(0.0, 0.0, -1.0)), (as_type<float4>((kernelContext_52->giResolve_0->constants_2[2U]))).x, kernelContext_52);


            GrSample_0 _S297 = grSample_0(standIn_1.xyz, standIn_1.w, 3U);

#line 654
            thread GrSample_0 _S298 = _S296;

#line 654
            thread GrSample_0 _S299 = _S297;

#line 654
            GrSample_0 _S300 = grGatherSample_0(&_S298, &_S299, excluded_1);
            if((_S300.source_0) != 0U)
            {

#line 655
                value_4 = _S300.radiance_0;

#line 655
                k_6 = 1U;

#line 655
            }
            else
            {

#line 655
                value_4 = _S294;

#line 655
                k_6 = 0U;

#line 655
            }

#line 647
        }
        else
        {

#line 647
            value_4 = _S294;

#line 647
            k_6 = 0U;

#line 647
        }

#line 660
        (*kernelContext_52->spRayValue_0)[lane_0] = value_4;
        (*kernelContext_52->spRayDirection_0)[lane_0] = direction_9;
        if(_S295)
        {

#line 662
            guideTotal_0 = 1.0 / (density_3 * float(_S291));

#line 662
        }
        else
        {

#line 662
            guideTotal_0 = 0.0;

#line 662
        }

#line 662
        (*kernelContext_52->spRayWeight_0)[lane_0] = guideTotal_0;
        (*kernelContext_52->spRayValid_0)[lane_0] = k_6;

#line 616
    }

#line 665
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if(lane_0 != 0U)
    {

#line 667
        return;
    }

    thread array<float3, int(9)> sum_2;

#line 670
    k_6 = 0U;
    for(;;)
    {

#line 671
        if(k_6 < 9U)
        {
        }
        else
        {

#line 671
            break;
        }

#line 672
        sum_2[k_6] = float3(0.0, 0.0, 0.0);

#line 671
        k_6 = k_6 + 1U;

#line 671
    }

#line 671
    uint ray_1 = 0U;


    for(;;)
    {

#line 674
        if(ray_1 < _S291)
        {
        }
        else
        {

#line 674
            break;
        }

#line 675
        if(((*kernelContext_52->spRayValid_0)[ray_1]) == 0U)
        {

#line 676
            ray_1 = ray_1 + 1U;

#line 674
            continue;
        }


        thread array<float, int(9)> basis_2;
        spBasis_0((*kernelContext_52->spRayDirection_0)[ray_1], &basis_2);
        float _S301 = (*kernelContext_52->spRayWeight_0)[ray_1];

#line 680
        k_6 = 0U;
        for(;;)
        {

#line 681
            if(k_6 < 9U)
            {
            }
            else
            {

#line 681
                break;
            }

#line 682
            sum_2[k_6] = sum_2[k_6] + (*kernelContext_52->spRayValue_0)[ray_1] * float3((basis_2[k_6] * _S301)) ;

#line 681
            k_6 = k_6 + 1U;

#line 681
        }

#line 674
        ray_1 = ray_1 + 1U;

#line 674
    }

#line 686
    float frames_0 = float(max(shape_1.y, 1U));
    thread array<float3, int(9)> blended_0;

    if((*kernelContext_52->spReprojected_0) != 0U)
    {

#line 690
        float _S302 = max(1.0 / (*kernelContext_52->spHistoryCount_0 + 1.0), 1.0 / frames_0);

#line 690
        k_6 = 0U;
        for(;;)
        {

#line 691
            if(k_6 < 9U)
            {
            }
            else
            {

#line 691
                break;
            }

#line 692
            blended_0[k_6] = (*kernelContext_52->spHistory_0)[k_6] + (sum_2[k_6] - (*kernelContext_52->spHistory_0)[k_6]) * float3(_S302) ;

#line 691
            k_6 = k_6 + 1U;

#line 691
        }

#line 691
        history_4 = min(*kernelContext_52->spHistoryCount_0 + 1.0, frames_0);

#line 689
    }
    else
    {

#line 689
        k_6 = 0U;

#line 696
        for(;;)
        {

#line 696
            if(k_6 < 9U)
            {
            }
            else
            {

#line 696
                break;
            }

#line 697
            blended_0[k_6] = sum_2[k_6];

#line 696
            k_6 = k_6 + 1U;

#line 696
        }

#line 696
        history_4 = 1.0;

#line 689
    }

#line 689
    thread array<float3, int(9)> _S303 = blended_0;

#line 689
    spStoreCoefficients_0(_S268, record_9, &_S303, history_4, kernelContext_52);

#line 689
    thread array<float3, int(9)> _S304 = blended_0;

#line 689
    spStoreFiltered_0(_S268, record_9, &_S304, kernelContext_52);

#line 689
    float _S305 = grFarFieldWeight_0(length(_S273.position_4 - (as_type<float3>((kernelContext_52->giResolve_0->constants_2[1U].xyz)))), kernelContext_52);

#line 706
    uint skyRays_0 = kernelContext_52->giResolve_0->constants_2[2U].z;
    if(_S305 > 0.0)
    {

#line 707
        allowed_0 = skyRays_0 > 0U;

#line 707
    }
    else
    {

#line 707
        allowed_0 = false;

#line 707
    }

#line 707
    if(allowed_0)
    {

#line 708
        uint _S306 = (frame_4 + _S273.id_0) % skyRays_0;

#line 708
        float _S307 = grSkyVisibilityRay_0(_S273.position_4, _S273.normal_3, _S306, skyRays_0, kernelContext_52);

#line 708
        openness_0 = _S307;

#line 707
    }
    else
    {

#line 707
        openness_0 = 1.0;

#line 707
    }

#line 707
    uint _S308 = spRecordBase_0(_S268, record_9, kernelContext_52);

#line 712
    *(kernelContext_52->giResolve_0->probes_1+(_S308 + 9U)) = *kernelContext_52->spStandIn_0;
    *(kernelContext_52->giResolve_0->probes_1+(_S308 + 10U)) = float4(openness_0, 0.0, 0.0, 0.0);
    return;
}


#line 28 "src/rendering/gi_gpu/shaders/gi_screen_probes.slang"
[[kernel]] void cyGiProbeTrace(uint3 group_0 [[threadgroup_position_in_grid]], uint3 lane_1 [[thread_position_in_threadgroup]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiResolveSet_default_0 constant* giResolve_1 [[buffer(3)]])
{

#line 28
    thread KernelContext_0 kernelContext_53;

#line 28
    (&kernelContext_53)->giProbes_0 = giProbes_1;

#line 28
    (&kernelContext_53)->giScene_0 = giScene_1;

#line 28
    (&kernelContext_53)->giResolve_0 = giResolve_1;

#line 28
    threadgroup array<float3, int(9)> spHistory_1;

#line 28
    (&kernelContext_53)->spHistory_0 = &spHistory_1;

#line 28
    threadgroup float spHistoryCount_1;

#line 28
    (&kernelContext_53)->spHistoryCount_0 = &spHistoryCount_1;

#line 28
    threadgroup uint spReprojected_1;

#line 28
    (&kernelContext_53)->spReprojected_0 = &spReprojected_1;

#line 28
    threadgroup uint spCarried_1;

#line 28
    (&kernelContext_53)->spCarried_0 = &spCarried_1;

#line 28
    threadgroup float4 spStandIn_1;

#line 28
    (&kernelContext_53)->spStandIn_0 = &spStandIn_1;

#line 28
    threadgroup float3 spOrigin_1;

#line 28
    (&kernelContext_53)->spOrigin_0 = &spOrigin_1;

#line 28
    threadgroup array<float, int(16)> spWeights_1;

#line 28
    (&kernelContext_53)->spWeights_0 = &spWeights_1;

#line 28
    threadgroup array<float3, int(64)> spRayValue_1;

#line 28
    (&kernelContext_53)->spRayValue_0 = &spRayValue_1;

#line 28
    threadgroup array<float3, int(64)> spRayDirection_1;

#line 28
    (&kernelContext_53)->spRayDirection_0 = &spRayDirection_1;

#line 28
    threadgroup array<float, int(64)> spRayWeight_1;

#line 28
    (&kernelContext_53)->spRayWeight_0 = &spRayWeight_1;

#line 28
    threadgroup array<uint, int(64)> spRayValid_1;

#line 28
    (&kernelContext_53)->spRayValid_0 = &spRayValid_1;

#line 28
    spTrace_0(group_0.x, lane_1.x, &kernelContext_53);

    return;
}

)cy_msl");
inline constexpr const auto& kGiProbeTraceMsl = kGiProbeTraceMslText.text;

/// cyGiProbeFilter.metal, 16795 bytes.
inline constexpr char kGiProbeFilterMsl[] =
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


#line 64 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
uint spSlot_0(KernelContext_0 thread* kernelContext_0)
{

#line 64
    return kernelContext_0->giResolve_0->constants_0[0U].w;
}


#line 61
uint spUniformCount_0(KernelContext_0 thread* kernelContext_1)
{

#line 61
    return kernelContext_1->giResolve_0->constants_0[43U].w;
}


#line 61
uint spUniformCount_1(KernelContext_0 thread* kernelContext_2)
{

#line 61
    return kernelContext_2->giResolve_0->constants_0[43U].w;
}


#line 62
uint spCapacity_0(KernelContext_0 thread* kernelContext_3)
{

#line 62
    return kernelContext_3->giResolve_0->constants_0[42U].z;
}


#line 62
uint spCapacity_1(KernelContext_0 thread* kernelContext_4)
{

#line 62
    return kernelContext_4->giResolve_0->constants_0[42U].z;
}


#line 63
uint spRecords_0(KernelContext_0 thread* kernelContext_5)
{

#line 63
    uint _S1 = spUniformCount_1(kernelContext_5);

#line 63
    uint _S2 = spCapacity_1(kernelContext_5);

#line 63
    return _S1 + _S2;
}


#line 63
uint spRecords_1(KernelContext_0 thread* kernelContext_6)
{

#line 63
    uint _S3 = spUniformCount_1(kernelContext_6);

#line 63
    uint _S4 = spCapacity_1(kernelContext_6);

#line 63
    return _S3 + _S4;
}


#line 88
uint spCounter_0(uint slot_0)
{

#line 89
    return slot_0;
}


#line 80
uint spRecordBase_0(uint slot_1, uint record_0, KernelContext_0 thread* kernelContext_7)
{

#line 80
    uint _S5 = spRecords_1(kernelContext_7);
    return (slot_1 * _S5 + record_0) * 18U;
}


#line 80
uint spRecordBase_1(uint slot_2, uint record_1, KernelContext_0 thread* kernelContext_8)
{

#line 80
    uint _S6 = spRecords_1(kernelContext_8);
    return (slot_2 * _S6 + record_1) * 18U;
}


#line 211
struct SpProbe_0
{
    float3 position_0;
    float depth_0;
    float3 normal_0;
    uint id_0;
};


#line 218
SpProbe_0 spLoadProbe_0(uint slot_3, uint record_2, KernelContext_0 thread* kernelContext_9)
{

#line 218
    uint _S7 = spRecordBase_0(slot_3, record_2, kernelContext_9);

    float4 device* _S8 = kernelContext_9->giResolve_0->probes_0+_S7;
    float4 device* _S9 = kernelContext_9->giResolve_0->probes_0+(_S7 + 1U);
    thread SpProbe_0 probe_0;
    (&probe_0)->position_0 = (*_S8).xyz;
    (&probe_0)->depth_0 = (*_S8).w;
    (&probe_0)->normal_0 = (*_S9).xyz;
    (&probe_0)->id_0 = (as_type<uint>(((*_S9).w)));
    return probe_0;
}


void spLoadPacked_0(uint base_0, array<float3, int(9)> thread* coefficients_0, float thread* last_0, KernelContext_0 thread* kernelContext_10)
{

#line 232
    thread array<float, int(28)> values_0;

#line 232
    uint v_0 = 0U;
    for(;;)
    {

#line 233
        if(v_0 < 7U)
        {
        }
        else
        {

#line 233
            break;
        }

#line 234
        float4 device* _S10 = kernelContext_10->giResolve_0->probes_0+(base_0 + v_0);
        uint _S11 = v_0 * 4U;

#line 235
        values_0[_S11] = (*_S10).x;
        values_0[_S11 + 1U] = (*_S10).y;
        values_0[_S11 + 2U] = (*_S10).z;
        values_0[_S11 + 3U] = (*_S10).w;

#line 233
        v_0 = v_0 + 1U;

#line 233
    }

#line 233
    uint k_0 = 0U;

#line 240
    for(;;)
    {

#line 240
        if(k_0 < 9U)
        {
        }
        else
        {

#line 240
            break;
        }

#line 241
        uint _S12 = k_0 * 3U;

#line 241
        (*coefficients_0)[k_0] = float3(values_0[_S12], values_0[_S12 + 1U], values_0[_S12 + 2U]);

#line 240
        k_0 = k_0 + 1U;

#line 240
    }


    *last_0 = values_0[int(27)];
    return;
}


#line 260
void spLoadCoefficients_0(uint slot_4, uint record_3, array<float3, int(9)> thread* coefficients_1, float thread* history_0, KernelContext_0 thread* kernelContext_11)
{

#line 260
    uint _S13 = spRecordBase_1(slot_4, record_3, kernelContext_11);

#line 260
    spLoadPacked_0(_S13 + 2U, coefficients_1, history_0, kernelContext_11);

    return;
}


#line 57
uint spTilesX_0(KernelContext_0 thread* kernelContext_12)
{

#line 57
    return kernelContext_12->giResolve_0->constants_0[41U].x;
}


#line 57
uint spTilesX_1(KernelContext_0 thread* kernelContext_13)
{

#line 57
    return kernelContext_13->giResolve_0->constants_0[41U].x;
}


#line 55
uint spSpacing_0(KernelContext_0 thread* kernelContext_14)
{

#line 55
    return kernelContext_14->giResolve_0->constants_0[40U].z;
}


#line 55
uint spSpacing_1(KernelContext_0 thread* kernelContext_15)
{

#line 55
    return kernelContext_15->giResolve_0->constants_0[40U].z;
}


#line 53
uint spWidth_0(KernelContext_0 thread* kernelContext_16)
{

#line 53
    return kernelContext_16->giResolve_0->constants_0[40U].x;
}


#line 53
uint spWidth_1(KernelContext_0 thread* kernelContext_17)
{

#line 53
    return kernelContext_17->giResolve_0->constants_0[40U].x;
}


#line 54
uint spHeight_0(KernelContext_0 thread* kernelContext_18)
{

)cy_msl"
    R"cy_msl(#line 54
    return kernelContext_18->giResolve_0->constants_0[40U].y;
}


#line 67
uint spUniformPixel_0(uint tile_0, KernelContext_0 thread* kernelContext_19)
{

#line 67
    uint _S14 = spTilesX_1(kernelContext_19);
    uint _S15 = tile_0 % _S14;

#line 68
    uint _S16 = spSpacing_1(kernelContext_19);

#line 68
    uint _S17 = _S15 * _S16;

#line 68
    uint _S18 = spSpacing_1(kernelContext_19);

#line 68
    uint _S19 = _S17 + _S18 / 2U;

#line 68
    uint _S20 = spWidth_1(kernelContext_19);

#line 68
    uint _S21 = min(_S19, _S20 - 1U);

#line 68
    uint _S22 = spTilesX_1(kernelContext_19);
    uint _S23 = tile_0 / _S22;

#line 69
    uint _S24 = spSpacing_1(kernelContext_19);

#line 69
    uint _S25 = _S23 * _S24;

#line 69
    uint _S26 = spSpacing_1(kernelContext_19);

#line 69
    uint _S27 = _S25 + _S26 / 2U;

#line 69
    uint _S28 = spHeight_0(kernelContext_19);

#line 69
    uint _S29 = min(_S27, _S28 - 1U);

#line 69
    uint _S30 = spWidth_1(kernelContext_19);
    return _S29 * _S30 + _S21;
}


#line 59
uint spCellsX_0(KernelContext_0 thread* kernelContext_20)
{

#line 59
    return kernelContext_20->giResolve_0->constants_0[41U].z;
}


#line 56
uint spCell_0(KernelContext_0 thread* kernelContext_21)
{

#line 56
    return kernelContext_21->giResolve_0->constants_0[40U].w;
}


#line 74
uint spAdaptivePixel_0(uint cell_0, KernelContext_0 thread* kernelContext_22)
{

#line 74
    uint _S31 = spCellsX_0(kernelContext_22);
    uint _S32 = cell_0 % _S31;

#line 75
    uint _S33 = spCell_0(kernelContext_22);

#line 75
    uint _S34 = _S32 * _S33;

#line 75
    uint _S35 = spCell_0(kernelContext_22);

#line 75
    uint _S36 = _S34 + _S35 / 2U;

#line 75
    uint _S37 = spWidth_1(kernelContext_22);

#line 75
    uint _S38 = min(_S36, _S37 - 1U);

#line 75
    uint _S39 = spCellsX_0(kernelContext_22);
    uint _S40 = cell_0 / _S39;

#line 76
    uint _S41 = spCell_0(kernelContext_22);

#line 76
    uint _S42 = _S40 * _S41;

#line 76
    uint _S43 = spCell_0(kernelContext_22);

#line 76
    uint _S44 = _S42 + _S43 / 2U;

#line 76
    uint _S45 = spHeight_0(kernelContext_22);

#line 76
    uint _S46 = min(_S44, _S45 - 1U);

#line 76
    uint _S47 = spWidth_1(kernelContext_22);
    return _S46 * _S47 + _S38;
}


#line 58
uint spTilesY_0(KernelContext_0 thread* kernelContext_23)
{

#line 58
    return kernelContext_23->giResolve_0->constants_0[41U].y;
}


#line 719
float spFilterTent_0(int offset_0, uint reach_0)
{

#line 719
    int _S48;
    if(offset_0 < int(0))
    {

#line 720
        _S48 = - offset_0;

#line 720
    }
    else
    {

#line 720
        _S48 = offset_0;

#line 720
    }
    return 1.0 - float(_S48) / float(reach_0 + 1U);
}


#line 123
float spGeometryWeight_0(float3 probePosition_0, float3 probeNormal_0, float3 position_1, float3 normal_1, float depth_1, float tolerance_0)
{
    float plane_0 = abs(dot(probeNormal_0, position_1 - probePosition_0));
    float reach_1 = tolerance_0 * depth_1;
    if(reach_1 <= 0.0)
    {

#line 128
        return 0.0;
    }

    float _S49 = max(0.0, dot(probeNormal_0, normal_1));
    float facing_0 = _S49 * _S49;
    float facing_1 = facing_0 * facing_0;

    return max(0.0, 1.0 - plane_0 / reach_1) * (facing_1 * facing_1);
}


#line 246
void spStorePacked_0(uint base_1, const array<float3, int(9)> thread* coefficients_2, float last_1, KernelContext_0 thread* kernelContext_24)
{

#line 247
    thread array<float, int(28)> values_1;

#line 247
    uint k_1 = 0U;
    for(;;)
    {

#line 248
        if(k_1 < 9U)
        {
        }
        else
        {

#line 248
            break;
        }

#line 249
        uint _S50 = k_1 * 3U;

#line 249
        values_1[_S50] = (*coefficients_2)[k_1].x;
        values_1[_S50 + 1U] = (*coefficients_2)[k_1].y;
        values_1[_S50 + 2U] = (*coefficients_2)[k_1].z;

#line 248
        k_1 = k_1 + 1U;

#line 248
    }

#line 253
    values_1[int(27)] = last_1;

#line 253
    uint v_1 = 0U;
    for(;;)
    {

#line 254
        if(v_1 < 7U)
        {
        }
        else
        {

#line 254
            break;
        }

#line 255
        uint _S51 = v_1 * 4U;

#line 255
        *(kernelContext_24->giResolve_0->probes_0+(base_1 + v_1)) = float4(values_1[_S51], values_1[_S51 + 1U], values_1[_S51 + 2U], values_1[_S51 + 3U]);

#line 254
        v_1 = v_1 + 1U;

#line 254
    }



    return;
}


#line 274
void spStoreFiltered_0(uint slot_5, uint record_4, const array<float3, int(9)> thread* coefficients_3, KernelContext_0 thread* kernelContext_25)
{

#line 274
    uint _S52 = spRecordBase_1(slot_5, record_4, kernelContext_25);

#line 274
    spStorePacked_0(_S52 + 11U, coefficients_3, 0.0, kernelContext_25);

    return;
}


#line 727
void spFilter_0(uint record_5, KernelContext_0 thread* kernelContext_26)
{

#line 727
    uint k_2;

#line 727
    uint _S53 = spSlot_0(kernelContext_26);

#line 727
    uint _S54 = spUniformCount_0(kernelContext_26);


    uint reach_2 = kernelContext_26->giResolve_0->constants_0[44U].y;

#line 730
    bool _S55;
    if(reach_2 == 0U)
    {

#line 731
        _S55 = true;

#line 731
    }
    else
    {

)cy_msl"
    R"cy_msl(#line 731
        uint _S56 = spRecords_0(kernelContext_26);

#line 731
        _S55 = record_5 >= _S56;

#line 731
    }

#line 731
    if(_S55)
    {

#line 732
        return;
    }
    if(record_5 >= _S54)
    {

#line 734
        uint _S57 = record_5 - _S54;

#line 734
        uint _S58 = *(kernelContext_26->giResolve_0->slots_0+spCounter_0(_S53));

#line 734
        uint _S59 = spCapacity_0(kernelContext_26);

#line 734
        _S55 = _S57 >= (min(_S58, _S59));

#line 734
    }
    else
    {

#line 734
        _S55 = false;

#line 734
    }

#line 734
    if(_S55)
    {

#line 735
        return;
    }

#line 735
    SpProbe_0 _S60 = spLoadProbe_0(_S53, record_5, kernelContext_26);


    if((_S60.depth_0) <= 0.0)
    {

#line 739
        return;
    }
    thread array<float3, int(9)> sum_0;
    thread float history_1;

#line 742
    spLoadCoefficients_0(_S53, record_5, &sum_0, &history_1, kernelContext_26);

#line 742
    uint pixel_0;

#line 747
    if((_S60.id_0) < _S54)
    {

#line 747
        uint _S61 = spUniformPixel_0(_S60.id_0, kernelContext_26);

#line 747
        pixel_0 = _S61;

#line 747
    }
    else
    {

#line 747
        uint _S62 = spAdaptivePixel_0(_S60.id_0 - _S54, kernelContext_26);

#line 747
        pixel_0 = _S62;

#line 747
    }

#line 747
    uint _S63 = spWidth_0(kernelContext_26);
    uint _S64 = pixel_0 % _S63;

#line 748
    uint _S65 = spSpacing_0(kernelContext_26);

#line 748
    uint _S66 = _S64 / _S65;

#line 748
    uint _S67 = spTilesX_0(kernelContext_26);

#line 748
    int _S68 = int(min(_S66, _S67 - 1U));

#line 748
    uint _S69 = spWidth_0(kernelContext_26);
    uint _S70 = pixel_0 / _S69;

#line 749
    uint _S71 = spSpacing_0(kernelContext_26);

#line 749
    uint _S72 = _S70 / _S71;

#line 749
    uint _S73 = spTilesY_0(kernelContext_26);

#line 749
    int _S74 = int(min(_S72, _S73 - 1U));
    float _S75 = (as_type<float>((kernelContext_26->giResolve_0->constants_0[43U].y)));
    float _S76 = (as_type<float>((kernelContext_26->giResolve_0->constants_0[44U].z)));
    int span_0 = int(reach_2);
    int _S77 = - span_0;

#line 753
    int dy_0 = _S77;

#line 753
    float total_0 = 1.0;

#line 753
    for(;;)
    {

#line 753
        if(dy_0 <= span_0)
        {
        }
        else
        {

#line 753
            break;
        }

#line 754
        int _S78 = _S74 + dy_0;

#line 754
        int dx_0 = _S77;

#line 754
        float total_1 = total_0;
        for(;;)
        {

#line 755
            if(dx_0 <= span_0)
            {
            }
            else
            {

#line 755
                break;
            }

#line 756
            int x_0 = _S68 + dx_0;
            if(x_0 < int(0))
            {

#line 757
                _S55 = true;

#line 757
            }
            else
            {

#line 757
                _S55 = _S78 < int(0);

#line 757
            }

#line 757
            bool _S79;

#line 757
            if(_S55)
            {

#line 757
                _S79 = true;

#line 757
            }
            else
            {

#line 757
                uint _S80 = spTilesX_0(kernelContext_26);

#line 757
                _S79 = x_0 >= int(_S80);

#line 757
            }

#line 757
            bool _S81;

#line 757
            if(_S79)
            {

#line 757
                _S81 = true;

#line 757
            }
            else
            {

#line 757
                uint _S82 = spTilesY_0(kernelContext_26);

#line 757
                _S81 = _S78 >= int(_S82);

#line 757
            }

#line 757
            if(_S81)
            {

#line 758
                dx_0 = dx_0 + int(1);

#line 755
                continue;
            }



            uint _S83 = uint(_S78);

#line 760
            uint _S84 = spTilesX_0(kernelContext_26);

#line 760
            uint tile_1 = _S83 * _S84 + uint(x_0);

#line 760
            SpProbe_0 _S85 = spLoadProbe_0(_S53, tile_1, kernelContext_26);

#line 760
            bool _S86;

            if(tile_1 == (_S60.id_0))
            {

#line 762
                _S86 = true;

#line 762
            }
            else
            {

#line 762
                _S86 = (_S85.depth_0) <= 0.0;

#line 762
            }

#line 762
            if(_S86)
            {

#line 763
                dx_0 = dx_0 + int(1);

#line 755
                continue;
            }

#line 768
            float weight_0 = spFilterTent_0(dx_0, reach_2) * spFilterTent_0(dy_0, reach_2) * max(1.0 - length(_S85.position_0 - _S60.position_0) / (_S76 * _S60.depth_0), 0.0) * spGeometryWeight_0(_S85.position_0, _S85.normal_0, _S60.position_0, _S60.normal_0, _S60.depth_0, _S75);



            if(weight_0 <= 0.0)
            {

)cy_msl"
    R"cy_msl(#line 773
                dx_0 = dx_0 + int(1);

#line 755
                continue;
            }

#line 775
            thread array<float3, int(9)> coefficients_4;
            thread float count_0;

#line 776
            spLoadCoefficients_0(_S53, tile_1, &coefficients_4, &count_0, kernelContext_26);

#line 776
            k_2 = 0U;

            for(;;)
            {

#line 778
                if(k_2 < 9U)
                {
                }
                else
                {

#line 778
                    break;
                }

#line 779
                sum_0[k_2] = sum_0[k_2] + coefficients_4[k_2] * float3(weight_0) ;

#line 778
                k_2 = k_2 + 1U;

#line 778
            }

#line 778
            total_1 = total_1 + weight_0;

#line 755
            dx_0 = dx_0 + int(1);

#line 755
        }

#line 753
        dy_0 = dy_0 + int(1);

#line 753
        total_0 = total_1;

#line 753
    }

#line 753
    k_2 = 0U;

#line 784
    for(;;)
    {

#line 784
        if(k_2 < 9U)
        {
        }
        else
        {

#line 784
            break;
        }

#line 785
        sum_0[k_2] = sum_0[k_2] / float3(total_0) ;

#line 784
        k_2 = k_2 + 1U;

#line 784
    }

#line 784
    thread array<float3, int(9)> _S87 = sum_0;

#line 784
    spStoreFiltered_0(_S53, record_5, &_S87, kernelContext_26);



    return;
}


#line 34 "src/rendering/gi_gpu/shaders/gi_screen_probes.slang"
[[kernel]] void cyGiProbeFilter(uint3 thread_0 [[thread_position_in_grid]], GiResolveSet_default_0 constant* giResolve_1 [[buffer(3)]])
{

#line 34
    thread KernelContext_0 kernelContext_27;

#line 34
    (&kernelContext_27)->giResolve_0 = giResolve_1;

#line 34
    spFilter_0(thread_0.x, &kernelContext_27);

    return;
}

)cy_msl";

/// cyGiProbeIntegrate.metal, 53551 bytes.
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


#line 53 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
uint spWidth_0(KernelContext_0 thread* kernelContext_12)
{

#line 53
    return kernelContext_12->giResolve_0->constants_2[40U].x;
}


#line 143
struct SpTaps_0
{
    array<uint, int(4)> index_0;
    array<float, int(4)> weight_0;
    uint count_0;
};


#line 55
uint spSpacing_0(KernelContext_0 thread* kernelContext_13)
{

#line 55
    return kernelContext_13->giResolve_0->constants_2[40U].z;
}

)cy_msl"
    R"cy_msl(
#line 57
uint spTilesX_0(KernelContext_0 thread* kernelContext_14)
{

#line 57
    return kernelContext_14->giResolve_0->constants_2[41U].x;
}


#line 58
uint spTilesY_0(KernelContext_0 thread* kernelContext_15)
{

#line 58
    return kernelContext_15->giResolve_0->constants_2[41U].y;
}


#line 149
void spAxisTaps_0(uint coordinate_0, uint step_2, uint count_1, uint thread* base_1, float thread* fraction_0)
{

#line 150
    float g_0 = (float(coordinate_0) - float(step_2 / 2U)) / float(step_2);
    if(g_0 <= 0.0)
    {

#line 152
        *base_1 = 0U;
        *fraction_0 = 0.0;
        return;
    }
    uint _S70 = uint(g_0);

#line 156
    *base_1 = _S70;
    *fraction_0 = g_0 - float(_S70);
    if((*base_1 + 1U) >= count_1)
    {

#line 159
        *base_1 = count_1 - 1U;
        *fraction_0 = 0.0;

#line 158
    }



    return;
}


#line 164
SpTaps_0 spBilinearTaps_0(uint x_3, uint y_3, uint step_3, uint countX_0, uint countY_0)
{

#line 165
    thread SpTaps_0 taps_0;
    (&taps_0)->count_0 = 0U;

#line 166
    uint t_5 = 0U;
    for(;;)
    {

#line 167
        if(t_5 < 4U)
        {
        }
        else
        {

#line 167
            break;
        }

#line 168
        (&taps_0)->index_0[t_5] = 0U;
        (&taps_0)->weight_0[t_5] = 0.0;

#line 167
        t_5 = t_5 + 1U;

#line 167
    }



    thread uint bx_1;

    thread float fx_1;

    spAxisTaps_0(x_3, step_3, countX_0, &bx_1, &fx_1);

#line 172
    thread uint by_1;

    thread float fy_1;

    spAxisTaps_0(y_3, step_3, countY_0, &by_1, &fy_1);

#line 176
    uint j_0 = 0U;
    for(;;)
    {

#line 177
        if(j_0 < 2U)
        {
        }
        else
        {

#line 177
            break;
        }

#line 177
        uint i_0 = 0U;
        for(;;)
        {

#line 178
            if(i_0 < 2U)
            {
            }
            else
            {

#line 178
                break;
            }

#line 178
            float _S71;
            if(i_0 == 0U)
            {

#line 179
                _S71 = 1.0 - fx_1;

#line 179
            }
            else
            {

#line 179
                _S71 = fx_1;

#line 179
            }

#line 179
            float _S72;

#line 179
            if(j_0 == 0U)
            {

#line 179
                _S72 = 1.0 - fy_1;

#line 179
            }
            else
            {

#line 179
                _S72 = fy_1;

#line 179
            }

#line 179
            float weight_1 = _S71 * _S72;
            if(weight_1 <= 0.0)
            {

#line 181
                i_0 = i_0 + 1U;

#line 178
                continue;
            }



            (&taps_0)->index_0[(&taps_0)->count_0] = (by_1 + j_0) * countX_0 + (bx_1 + i_0);
            (&taps_0)->weight_0[(&taps_0)->count_0] = weight_1;
            (&taps_0)->count_0 = (&taps_0)->count_0 + 1U;

#line 178
            i_0 = i_0 + 1U;

#line 178
        }

#line 177
        j_0 = j_0 + 1U;

#line 177
    }

#line 188
    return taps_0;
}


#line 56
uint spCell_0(KernelContext_0 thread* kernelContext_16)
{

#line 56
    return kernelContext_16->giResolve_0->constants_2[40U].w;
}

uint spCellsX_0(KernelContext_0 thread* kernelContext_17)
{

#line 59
    return kernelContext_17->giResolve_0->constants_2[41U].z;
}


#line 60
uint spCellsY_0(KernelContext_0 thread* kernelContext_18)
{

#line 60
    return kernelContext_18->giResolve_0->constants_2[41U].w;
}


#line 59
uint spCellsX_1(KernelContext_0 thread* kernelContext_19)
{

#line 59
    return kernelContext_19->giResolve_0->constants_2[41U].z;
}


#line 60
uint spCellsY_1(KernelContext_0 thread* kernelContext_20)
{

#line 60
    return kernelContext_20->giResolve_0->constants_2[41U].w;
}


#line 84
uint spTableIndex_0(uint slot_2, uint cell_2, KernelContext_0 thread* kernelContext_21)
{

#line 84
    uint _S73 = spCellsX_1(kernelContext_21);
    uint _S74 = slot_2 * _S73;

#line 85
    uint _S75 = spCellsY_1(kernelContext_21);

#line 85
    return 4U + _S74 * _S75 + cell_2;
}


#line 61
uint spUniformCount_0(KernelContext_0 thread* kernelContext_22)
{

#line 61
    return kernelContext_22->giResolve_0->constants_2[43U].w;
}


#line 211
struct SpProbe_0
{
    float3 position_5;
    float depth_0;
    float3 normal_4;
    uint id_0;
};


#line 61
uint spUniformCount_1(KernelContext_0 thread* kernelContext_23)
{

#line 61
    return kernelContext_23->giResolve_0->constants_2[43U].w;
}


#line 62
uint spCapacity_0(KernelContext_0 thread* kernelContext_24)
{

)cy_msl"
    R"cy_msl(#line 62
    return kernelContext_24->giResolve_0->constants_2[42U].z;
}


#line 63
uint spRecords_0(KernelContext_0 thread* kernelContext_25)
{

#line 63
    uint _S76 = spUniformCount_1(kernelContext_25);

#line 63
    uint _S77 = spCapacity_0(kernelContext_25);

#line 63
    return _S76 + _S77;
}


#line 80
uint spRecordBase_0(uint slot_3, uint record_0, KernelContext_0 thread* kernelContext_26)
{

#line 80
    uint _S78 = spRecords_0(kernelContext_26);
    return (slot_3 * _S78 + record_0) * 18U;
}


#line 218
SpProbe_0 spLoadProbe_0(uint slot_4, uint record_1, KernelContext_0 thread* kernelContext_27)
{

#line 218
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


#line 123
float spGeometryWeight_0(float3 probePosition_0, float3 probeNormal_0, float3 position_6, float3 normal_5, float depth_1, float tolerance_0)
{
    float plane_0 = abs(dot(probeNormal_0, position_6 - probePosition_0));
    float reach_1 = tolerance_0 * depth_1;
    if(reach_1 <= 0.0)
    {

#line 128
        return 0.0;
    }

    float _S82 = max(0.0, dot(probeNormal_0, normal_5));
    float facing_0 = _S82 * _S82;
    float facing_1 = facing_0 * facing_0;

    return max(0.0, 1.0 - plane_0 / reach_1) * (facing_1 * facing_1);
}


#line 231
void spLoadPacked_0(uint base_2, array<float3, int(9)> thread* coefficients_0, float thread* last_1, KernelContext_0 thread* kernelContext_28)
{

#line 232
    thread array<float, int(28)> values_0;

#line 232
    uint v_1 = 0U;
    for(;;)
    {

#line 233
        if(v_1 < 7U)
        {
        }
        else
        {

#line 233
            break;
        }

#line 234
        float4 device* _S83 = kernelContext_28->giResolve_0->probes_1+(base_2 + v_1);
        uint _S84 = v_1 * 4U;

#line 235
        values_0[_S84] = (*_S83).x;
        values_0[_S84 + 1U] = (*_S83).y;
        values_0[_S84 + 2U] = (*_S83).z;
        values_0[_S84 + 3U] = (*_S83).w;

#line 233
        v_1 = v_1 + 1U;

#line 233
    }

#line 233
    uint k_0 = 0U;

#line 240
    for(;;)
    {

#line 240
        if(k_0 < 9U)
        {
        }
        else
        {

#line 240
            break;
        }

#line 241
        uint _S85 = k_0 * 3U;

#line 241
        (*coefficients_0)[k_0] = float3(values_0[_S85], values_0[_S85 + 1U], values_0[_S85 + 2U]);

#line 240
        k_0 = k_0 + 1U;

#line 240
    }


    *last_1 = values_0[int(27)];
    return;
}


#line 269
void spLoadFiltered_0(uint slot_5, uint record_2, array<float3, int(9)> thread* coefficients_1, KernelContext_0 thread* kernelContext_29)
{

#line 269
    uint _S86 = spRecordBase_0(slot_5, record_2, kernelContext_29);
    thread float unused_0;

#line 270
    spLoadPacked_0(_S86 + 11U, coefficients_1, &unused_0, kernelContext_29);

    return;
}


#line 94
void spBasis_0(float3 d_0, array<float, int(9)> thread* basis_0)
{

#line 95
    (*basis_0)[int(0)] = 0.28209498524665833;
    float _S87 = d_0.y;

#line 96
    (*basis_0)[int(1)] = 0.48860299587249756 * _S87;
    float _S88 = d_0.z;

#line 97
    (*basis_0)[int(2)] = 0.48860299587249756 * _S88;
    float _S89 = d_0.x;

#line 98
    (*basis_0)[int(3)] = 0.48860299587249756 * _S89;
    (*basis_0)[int(4)] = 1.09254801273345947 * (_S89 * _S87);
    (*basis_0)[int(5)] = 1.09254801273345947 * (_S87 * _S88);
    (*basis_0)[int(6)] = 0.31539198756217957 * (3.0 * (_S88 * _S88) - 1.0);
    (*basis_0)[int(7)] = 1.09254801273345947 * (_S89 * _S88);
    (*basis_0)[int(8)] = 0.54627400636672974 * (_S89 * _S89 - _S87 * _S87);
    return;
}


#line 106
uint spBandOf_0(uint k_1)
{

#line 106
    uint _S90;
    if(k_1 == 0U)
    {

#line 107
        _S90 = 0U;

#line 107
    }
    else
    {

#line 107
        if(k_1 < 4U)
        {

#line 107
            _S90 = 1U;

#line 107
        }
        else
        {

#line 107
            _S90 = 2U;

#line 107
        }

#line 107
    }

#line 107
    return _S90;
}

float3 spEvaluate_0(const array<float3, int(9)> thread* coefficients_2, float3 direction_4, float3 band_0)
{

#line 111
    thread array<float, int(9)> basis_1;
    spBasis_0(direction_4, &basis_1);

#line 112
    float3 sum_0 = float3(0.0, 0.0, 0.0);

#line 112
    uint k_2 = 0U;

    for(;;)
    {

#line 114
        if(k_2 < 9U)
        {
        }
        else
        {

#line 114
            break;
        }

#line 115
        uint b_1 = spBandOf_0(k_2);

#line 115
        float scale_0;
        if(b_1 == 0U)
        {

#line 116
            scale_0 = band_0.x;

#line 116
        }
        else
        {

#line 116
            if(b_1 == 1U)
            {

#line 116
                scale_0 = band_0.y;

#line 116
            }
            else
            {

#line 116
                scale_0 = band_0.z;

#line 116
            }

#line 116
        }
        float3 _S91 = sum_0 + (*coefficients_2)[k_2] * float3((basis_1[k_2] * scale_0)) ;

#line 114
        uint k_3 = k_2 + 1U;

#line 114
        sum_0 = _S91;

#line 114
        k_2 = k_3;

)cy_msl"
    R"cy_msl(#line 114
    }

#line 119
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
    uint _S92 = sample_3->source_0;
    if((sample_3->source_0) == 0U)
    {

#line 149
        return;
    }
    if((excluded_0 & (1U << _S92)) != 0U)
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
float grFarFieldWeight_0(float distance_0, KernelContext_0 thread* kernelContext_30)
{

#line 169
    float4 far_0 = (as_type<float4>((kernelContext_30->giResolve_0->constants_2[3U])));
    float begin_0 = far_0.x;
    if(begin_0 <= 0.0)
    {

#line 172
        return 1.0;
    }
    float _S93 = far_0.y;

#line 174
    float ratio_0;

#line 174
    if(_S93 > 0.0)
    {

#line 174
        ratio_0 = far_0.z / _S93;

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
    float _S94 = state_2->totalWeight_0;

#line 163
    float3 _S95;
    if((state_2->totalWeight_0) > 0.0)
    {

#line 164
        _S95 = state_2->total_0 / float3(_S94) ;

#line 164
    }
    else
    {

#line 164
        _S95 = float3(0.0, 0.0, 0.0);

#line 164
    }

#line 164
    return _S95;
}


#line 200
uint grReflectionRays_0(float roughness_0, KernelContext_0 thread* kernelContext_31)
{

#line 201
    uint4 words_0 = kernelContext_31->giResolve_0->constants_2[4U];
    float mirror_0 = (as_type<float>((words_0.x)));
    float cacheMin_0 = (as_type<float>((words_0.y)));
    uint _S96 = max(1U, words_0.z);
    if(roughness_0 <= mirror_0)
    {

#line 206
        return _S96;
    }
    if(roughness_0 < cacheMin_0)
    {

        float _S97 = float(_S96);


        return max(1U, uint(floor(_S97 + (1.0 - _S97) * gpClamp_0((roughness_0 - mirror_0) / max(cacheMin_0 - mirror_0, 0.00009999999747379), 0.0, 1.0) + 0.5)));
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
    bool _S98;
    if(count_2 <= 1U)
    {

#line 223
        _S98 = true;

#line 223
    }
    else
    {

#line 223
        _S98 = roughness_1 <= 0.0;

#line 223
    }

#line 223
    if(_S98)
    {

#line 224
        return axis_0;
    }

    float _S99 = float(index_1);

    float radius_1 = sqrt((_S99 + 0.5) / float(count_2)) * (gpClamp_0(roughness_1, 0.0, 1.0) * 0.69999998807907104);
    float angle_0 = 2.3999631404876709 * _S99 + float(rotation_0) * 0.6180340051651001;

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
    float _S100 = dot(unit_1, normal_6);

#line 238
    if(_S100 < 0.0)
    {

#line 238
        tangent_0 = giNormalisedOr_0(unit_1 - normal_6 * float3((2.0 * _S100)) , axis_0);

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
    uint _S101 = standIn_0->source_0;

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

)cy_msl"
    R"cy_msl(#line 264
        hasStandIn_0 = false;

#line 264
    }

#line 264
    if(hasStandIn_0)
    {

#line 264
        hasStandIn_0 = (excluded_1 & (1U << _S101)) == 0U;

#line 264
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
        (&sample_4)->source_0 = _S101;
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


#line 793 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
void spIntegrate_0(uint pixel_2, KernelContext_0 thread* kernelContext_32)
{

#line 794
    uint4 counts_0 = kernelContext_32->giResolve_0->constants_2[0U];
    uint _S102 = counts_0.z;

#line 795
    if(pixel_2 >= _S102)
    {

#line 796
        return;
    }
    uint _S103 = counts_0.w;

#line 798
    uint record_3 = (_S103 * _S102 + pixel_2) * 2U;
    float4 at_0 = kernelContext_32->giResolve_0->surfaces_0[record_3];
    float4 facing_2 = kernelContext_32->giResolve_0->surfaces_0[record_3 + 1U];
    float _S104 = at_0.w;

#line 801
    if(_S104 <= 0.0)
    {

#line 802
        uint _S105 = pixel_2 * 2U;

#line 802
        float4 _S106 = float4(0.0, 0.0, 0.0, 0.0);

#line 802
        *(kernelContext_32->giResolve_0->noisy_0+_S105) = _S106;
        *(kernelContext_32->giResolve_0->noisy_0+(_S105 + 1U)) = _S106;
        return;
    }

    uint frame_2 = kernelContext_32->giResolve_0->constants_2[18U].x;
    float3 position_7 = at_0.xyz;
    float3 normal_7 = giNormalisedOr_0(facing_2.xyz, float3(0.0, 1.0, 0.0));
    float roughness_2 = facing_2.w;
    float3 offset_1 = position_7 - (as_type<float3>((kernelContext_32->giResolve_0->constants_2[1U].xyz)));
    float3 _S107 = float3(0.0, 0.0, -1.0);

#line 812
    float3 incident_0 = giNormalisedOr_0(offset_1, _S107);

    float3 reflection_1 = giNormalisedOr_0(incident_0 - normal_7 * float3((2.0 * dot(incident_0, normal_7))) , normal_7);
    float rough_0 = gpClamp_0(roughness_2, 0.0, 1.0);
    float3 specularBand_0 = float3(1.0, 1.0 + -0.33333331346511841 * rough_0, 1.0 + -0.75 * rough_0);

    float3 irradianceBand_0 = float3(1.0, 0.66666668653488159, 0.25);
    float4 _S108 = (as_type<float4>((kernelContext_32->giResolve_0->constants_2[43U])));

#line 819
    uint _S109 = spWidth_0(kernelContext_32);
    uint _S110 = pixel_2 % _S109;

#line 820
    uint _S111 = spWidth_0(kernelContext_32);
    uint _S112 = pixel_2 / _S111;

    float3 _S113 = float3(0.0, 0.0, 0.0);

#line 823
    float nearestPlane_0 = 1.00000001504746622e+30;

#line 823
    float3 irradiance_0 = _S113;

#line 823
    float3 glossy_0 = _S113;

#line 823
    float3 cache_0 = _S113;

#line 823
    uint nearest_0 = 4294967295U;

#line 823
    uint pass_0 = 0U;

#line 823
    float cacheConfidence_0 = 0.0;

#line 823
    float openness_0 = 0.0;

#line 823
    float total_1 = 0.0;

#line 831
    for(;;)
    {

#line 831
        if(pass_0 < 2U)
        {
        }
        else
        {

#line 831
            break;
        }

#line 831
        SpTaps_0 _S114;

        if(pass_0 == 0U)
        {

#line 833
            uint _S115 = spSpacing_0(kernelContext_32);

#line 833
            uint _S116 = spTilesX_0(kernelContext_32);

#line 833
            uint _S117 = spTilesY_0(kernelContext_32);

#line 833
            _S114 = spBilinearTaps_0(_S110, _S112, _S115, _S116, _S117);

#line 833
        }
        else
        {

#line 833
            uint _S118 = spCell_0(kernelContext_32);

#line 833
            uint _S119 = spCellsX_0(kernelContext_32);

#line 833
            uint _S120 = spCellsY_0(kernelContext_32);

#line 833
            _S114 = spBilinearTaps_0(_S110, _S112, _S118, _S119, _S120);

#line 833
        }

#line 833
        float nearestPlane_1 = nearestPlane_0;

#line 833
        float3 irradiance_1 = irradiance_0;

#line 833
        float3 glossy_1 = glossy_0;

#line 833
        float3 cache_1 = cache_0;

#line 833
        uint nearest_1 = nearest_0;

#line 833
        uint tap_0 = 0U;

#line 833
        float cacheConfidence_1 = cacheConfidence_0;

#line 833
        float openness_1 = openness_0;

#line 833
        float total_2 = total_1;

        for(;;)
        {

#line 835
            if(tap_0 < (_S114.count_0))
            {
            }
            else
            {

#line 835
                break;
            }

#line 833
            SpTaps_0 _S121 = _S114;

            uint _S122 = tap_0;

#line 835
            uint index_2;

            if(pass_0 == 1U)
            {

#line 837
                uint _S123 = spTableIndex_0(_S103, _S121.index_0[_S122], kernelContext_32);
                uint device* _S124 = kernelContext_32->giResolve_0->slots_0+_S123;

#line 838
                uint taken_0 = *_S124;
                if((*_S124) == 4294967295U)
                {

#line 840
                    tap_0 = tap_0 + 1U;

#line 835
                    continue;
                }

#line 835
                uint _S125 = spUniformCount_0(kernelContext_32);

#line 835
                index_2 = _S125 + taken_0;

            }
            else
            {

#line 837
                index_2 = _S121.index_0[_S122];

#line 837
            }

#line 837
            SpProbe_0 _S126 = spLoadProbe_0(_S103, index_2, kernelContext_32);

#line 845
            if((_S126.depth_0) <= 0.0)
            {

#line 846
                tap_0 = tap_0 + 1U;

)cy_msl"
    R"cy_msl(#line 835
                continue;
            }

#line 848
            float plane_1 = abs(dot(_S126.normal_4, position_7 - _S126.position_5));

#line 848
            float nearestPlane_2;

#line 848
            uint nearest_2;
            if(plane_1 < nearestPlane_1)
            {

#line 849
                nearestPlane_2 = plane_1;

#line 849
                nearest_2 = index_2;

#line 849
            }
            else
            {

#line 849
                nearestPlane_2 = nearestPlane_1;

#line 849
                nearest_2 = nearest_1;

#line 849
            }

#line 854
            float weight_3 = _S114.weight_0[tap_0] * spGeometryWeight_0(_S126.position_5, _S126.normal_4, position_7, normal_7, _S104, _S108.y);

            if(weight_3 <= 0.0)
            {

#line 856
                nearestPlane_1 = nearestPlane_2;

#line 856
                nearest_1 = nearest_2;
                tap_0 = tap_0 + 1U;

#line 835
                continue;
            }

#line 859
            thread array<float3, int(9)> coefficients_3;

#line 859
            spLoadFiltered_0(_S103, index_2, &coefficients_3, kernelContext_32);

#line 859
            uint _S127 = spRecordBase_0(_S103, index_2, kernelContext_32);


            float4 device* _S128 = kernelContext_32->giResolve_0->probes_1+(_S127 + 9U);

#line 862
            thread array<float3, int(9)> _S129 = coefficients_3;

#line 862
            float3 _S130 = spEvaluate_0(&_S129, normal_7, irradianceBand_0);

#line 862
            float3 _S131 = float3(weight_3) ;
            float3 _S132 = irradiance_1 + _S130 * _S131;

#line 863
            thread array<float3, int(9)> _S133 = coefficients_3;

#line 863
            float3 _S134 = spEvaluate_0(&_S133, reflection_1, specularBand_0);
            float3 _S135 = glossy_1 + _S134 * _S131;
            float3 _S136 = cache_1 + (*_S128).xyz * _S131;
            float cacheConfidence_2 = cacheConfidence_1 + (*_S128).w * weight_3;
            float openness_2 = openness_1 + (*(kernelContext_32->giResolve_0->probes_1+(_S127 + 10U))).x * weight_3;
            float total_3 = total_2 + weight_3;

#line 868
            nearestPlane_1 = nearestPlane_2;

#line 868
            irradiance_1 = _S132;

#line 868
            glossy_1 = _S135;

#line 868
            cache_1 = _S136;

#line 868
            nearest_1 = nearest_2;

#line 868
            cacheConfidence_1 = cacheConfidence_2;

#line 868
            openness_1 = openness_2;

#line 868
            total_2 = total_3;

#line 835
            tap_0 = tap_0 + 1U;

#line 835
        }

#line 831
        uint pass_1 = pass_0 + 1U;

#line 831
        nearestPlane_0 = nearestPlane_1;

#line 831
        irradiance_0 = irradiance_1;

#line 831
        glossy_0 = glossy_1;

#line 831
        cache_0 = cache_1;

#line 831
        nearest_0 = nearest_1;

#line 831
        pass_0 = pass_1;

#line 831
        cacheConfidence_0 = cacheConfidence_1;

#line 831
        openness_0 = openness_1;

#line 831
        total_1 = total_2;

#line 831
    }

#line 871
    if(total_1 < 0.00009999999747379)
    {

#line 872
        if(nearest_0 == 4294967295U)
        {
            uint _S137 = pixel_2 * 2U;

#line 874
            float4 _S138 = float4(0.0, 0.0, 0.0, (as_type<float>((kernelContext_32->giResolve_0->constants_2[44U].x))));

#line 874
            *(kernelContext_32->giResolve_0->noisy_0+_S137) = _S138;
            *(kernelContext_32->giResolve_0->noisy_0+(_S137 + 1U)) = _S138;
            return;
        }
        thread array<float3, int(9)> coefficients_4;

#line 878
        spLoadFiltered_0(_S103, nearest_0, &coefficients_4, kernelContext_32);

#line 878
        uint _S139 = spRecordBase_0(_S103, nearest_0, kernelContext_32);

#line 878
        thread array<float3, int(9)> _S140 = coefficients_4;

#line 878
        float3 _S141 = spEvaluate_0(&_S140, normal_7, irradianceBand_0);

#line 878
        thread array<float3, int(9)> _S142 = coefficients_4;

#line 878
        float3 _S143 = spEvaluate_0(&_S142, reflection_1, specularBand_0);

#line 883
        uint _S144 = _S139 + 9U;
        float _S145 = (*(kernelContext_32->giResolve_0->probes_1+_S144)).w;
        float _S146 = (*(kernelContext_32->giResolve_0->probes_1+(_S139 + 10U))).x;

#line 885
        cache_0 = (*(kernelContext_32->giResolve_0->probes_1+_S144)).xyz;

#line 885
        irradiance_0 = _S141;

#line 885
        glossy_0 = _S143;

#line 885
        cacheConfidence_0 = _S145;

#line 885
        openness_0 = _S146;

#line 871
    }
    else
    {

#line 887
        float3 _S147 = irradiance_0 / float3(total_1) ;
        float3 _S148 = glossy_0 / float3(total_1) ;

        float cacheConfidence_3 = cacheConfidence_0 / total_1;
        float openness_3 = openness_0 / total_1;

#line 891
        cache_0 = cache_0 / float3(total_1) ;

#line 891
        irradiance_0 = _S147;

#line 891
        glossy_0 = _S148;

#line 891
        cacheConfidence_0 = cacheConfidence_3;

#line 891
        openness_0 = openness_3;

#line 871
    }

#line 894
    uint4 shape_1 = kernelContext_32->giResolve_0->constants_2[42U];
    uint excluded_2 = kernelContext_32->giResolve_0->constants_2[2U].w;
    GrCombine_0 _S149 = grCombineBegin_0();

#line 896
    thread GrCombine_0 state_3 = _S149;
    if(((shape_1.w) & 1U) != 0U)
    {

#line 897
        thread GrSample_0 _S150 = grSample_0(cache_0, cacheConfidence_0, 3U);

#line 897
        grCombineAdd_0(&state_3, &_S150, excluded_2);

#line 897
    }


    uint _S151 = GiSoftwareTier_source_0();

#line 900
    thread GrSample_0 _S152 = grSample_0(irradiance_0, 1.0, _S151);

#line 900
    grCombineAdd_0(&state_3, &_S152, excluded_2);
    thread GrSample_0 sky_0 = grSample_0(_S113, 0.0, 9U);

#line 901
    float _S153 = grFarFieldWeight_0(length(offset_1), kernelContext_32);

    if(_S153 > 0.0)
    {

#line 903
        float3 _S154 = gpSky_0(normal_7, kernelContext_32);
        (&sky_0)->radiance_0 = _S154 * float3(openness_0) ;
        (&sky_0)->confidence_0 = gpClamp_0(_S153 * openness_0, 0.0, 1.0);

#line 903
    }

#line 903
    thread GrSample_0 _S155 = sky_0;

#line 903
    grCombineAdd_0(&state_3, &_S155, excluded_2);

#line 903
    thread GrCombine_0 _S156 = state_3;

#line 903
    float3 _S157 = grCombineEnd_0(&_S156);

#line 908
    float3 diffuse_0 = _S157 * float3((as_type<float>((kernelContext_32->giResolve_0->constants_2[5U].x)))) ;

    GrSample_0 probeSpecular_0 = grSample_0(glossy_0, gpClamp_0(roughness_2, 0.15000000596046448, 1.0), _S151);
    thread GrCombine_0 reflected_0 = _S149;

#line 911
    uint _S158 = grReflectionRays_0(roughness_2, kernelContext_32);

    float density_1 = (as_type<float>((kernelContext_32->giResolve_0->constants_2[1U].w)));

)cy_msl"
    R"cy_msl(#line 913
    bool _S159;
    if(_S158 > 0U)
    {

#line 914
        _S159 = grTraces_0(pixel_2, frame_2, density_1);

#line 914
    }
    else
    {

#line 914
        _S159 = false;

#line 914
    }

#line 914
    if(_S159)
    {

#line 915
        float4 rays_1 = (as_type<float4>((kernelContext_32->giResolve_0->constants_2[2U])));


        uint _S160 = grHash_0(pixel_2, frame_2, 7U) % _S158;

#line 918
        GrSample_0 _S161 = grWorldRay_0(position_7, normal_7, giNormalisedOr_0(grLobeDirection_0(reflection_1, normal_7, roughness_2, _S160, _S158, frame_2), _S107), rays_1.y, rays_1.x, kernelContext_32);

#line 918
        thread GrSample_0 _S162 = _S161;

#line 918
        thread GrSample_0 _S163 = probeSpecular_0;

#line 918
        GrSample_0 _S164 = grGatherSample_0(&_S162, &_S163, 0U);

#line 918
        thread GrSample_0 _S165 = _S164;

#line 918
        grCombineAdd_0(&reflected_0, &_S165, 0U);

#line 914
    }

#line 914
    thread GrSample_0 _S166 = probeSpecular_0;

#line 914
    grCombineAdd_0(&reflected_0, &_S166, 0U);

#line 914
    thread GrCombine_0 _S167 = reflected_0;

#line 914
    float3 _S168 = grCombineEnd_0(&_S167);

#line 926
    float3 specular_0 = _S168 * float3((as_type<float>((kernelContext_32->giResolve_0->constants_2[5U].y)))) ;
    float samples_1 = (as_type<float>((kernelContext_32->giResolve_0->constants_2[44U].x)));
    uint _S169 = pixel_2 * 2U;

#line 928
    *(kernelContext_32->giResolve_0->noisy_0+_S169) = float4(diffuse_0, samples_1);
    *(kernelContext_32->giResolve_0->noisy_0+(_S169 + 1U)) = float4(specular_0, samples_1);
    return;
}


#line 40 "src/rendering/gi_gpu/shaders/gi_screen_probes.slang"
[[kernel]] void cyGiProbeIntegrate(uint3 thread_0 [[thread_position_in_grid]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiResolveSet_default_0 constant* giResolve_1 [[buffer(3)]])
{

#line 40
    thread KernelContext_0 kernelContext_33;

#line 40
    (&kernelContext_33)->giProbes_0 = giProbes_1;

#line 40
    (&kernelContext_33)->giScene_0 = giScene_1;

#line 40
    (&kernelContext_33)->giResolve_0 = giResolve_1;

#line 40
    spIntegrate_0(thread_0.x, &kernelContext_33);

    return;
}

)cy_msl";

/// cyGiProbeTraceHardware.metal, 111083 bytes.
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


#line 663 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
struct KernelContext_0
{
    GiProbeSet_default_0 constant* giProbes_0;
    GiSceneSet_default_0 constant* giScene_0;
    GiResolveSet_default_0 constant* giResolve_0;
    GiHardwareSet_default_0 constant* giHardware_0;
    array<float3, int(9)> threadgroup* spHistory_0;
    float threadgroup* spHistoryCount_0;
    uint threadgroup* spReprojected_0;
    uint threadgroup* spCarried_0;
    float4 threadgroup* spStandIn_0;
    float3 threadgroup* spOrigin_0;
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


)cy_msl",
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


#line 503 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
GrSample_0 spProbeRay_0(float3 origin_1, float3 unit_0, float reach_0, KernelContext_0 thread* kernelContext_5)
{

#line 504
    uint cacheFrame_1 = kernelContext_5->giProbes_0->constants_0[24U].w;

#line 504
    GiWorldHit_0 _S28 = GiHardwareTier_trace_0(origin_1, unit_0, reach_0, kernelContext_5);

#line 504
    bool _S29;

    if(_S28.hit_0)
    {

#line 506
        _S29 = (dot(_S28.normal_0, unit_0)) > 0.0;

#line 506
    }
    else
    {

#line 506
        _S29 = false;

#line 506
    }

#line 506
    if(_S29)
    {

#line 507
        return grSample_0(float3(0.0, 0.0, 0.0), 0.0, 0U);
    }
    uint _S30 = GiHardwareTier_source_0();

#line 509
    thread GiWorldHit_0 _S31 = _S28;

#line 509
    GiTierSample_0 _S32 = gpWorldRadiance_0(&_S31, unit_0, _S30, cacheFrame_1, kernelContext_5);
    return grSample_0(_S32.radiance_2, _S32.confidence_3, _S32.source_2);
}


#line 211
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


#line 478 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
float3 spProbeOrigin_0(const SpProbe_0 thread* probe_0, float lift_0, KernelContext_0 thread* kernelContext_12)
{

#line 478
    uint _S59 = giLevelCount_0(kernelContext_12);
    if(_S59 == 0U)
    {

#line 480
        return probe_0->position_2 + probe_0->normal_4 * float3(lift_0) ;
    }

#line 480
    float _S60 = giFinestVoxel_0(kernelContext_12);

#line 480
    float clearance_0;


    if((GiHardwareTier_source_0()) == 7U)
    {

#line 483
        clearance_0 = lift_0;

#line 483
    }
    else
    {

#line 483
        clearance_0 = max(lift_0, _S60);

#line 483
    }

#line 483
    float3 _S61 = probe_0->normal_4;
    float3 origin_3 = probe_0->position_2 + probe_0->normal_4 * float3(clearance_0) ;
    if(_S60 <= 0.0)
    {

#line 486
        return origin_3;
    }

#line 486
    float3 origin_4 = origin_3;

#line 486
    uint step_0 = 0U;

    for(;;)
    {

#line 488
        if(step_0 < 4U)
        {
        }
        else
        {

#line 488
            break;
        }

#line 488
        float _S62 = giDistance_0(origin_4, kernelContext_12);

        if(_S62 >= clearance_0)
        {

#line 491
            break;
        }

        float3 _S63 = float3(_S60, 0.0, 0.0);

#line 494
        float _S64 = giDistance_0(origin_4 + _S63, kernelContext_12);

#line 494
        float _S65 = giDistance_0(origin_4 - _S63, kernelContext_12);

#line 494
        float _S66 = _S64 - _S65;
        float3 _S67 = float3(0.0, _S60, 0.0);

#line 495
        float _S68 = giDistance_0(origin_4 + _S67, kernelContext_12);

#line 495
        float _S69 = giDistance_0(origin_4 - _S67, kernelContext_12);

#line 495
        float _S70 = _S68 - _S69;
        float3 _S71 = float3(0.0, 0.0, _S60);

#line 496
        float _S72 = giDistance_0(origin_4 + _S71, kernelContext_12);

#line 496
        float _S73 = giDistance_0(origin_4 - _S71, kernelContext_12);
        float3 _S74 = origin_4 + giNormalisedOr_0(float3(_S66, _S70, _S72 - _S73), _S61) * float3((clearance_0 - _S62)) ;

#line 488
        uint step_1 = step_0 + 1U;

#line 488
        origin_4 = _S74;

#line 488
        step_0 = step_1;

#line 488
    }

#line 499
    return origin_4;
}


#line 64
uint spSlot_0(KernelContext_0 thread* kernelContext_13)
{

#line 64
    return kernelContext_13->giResolve_0->constants_2[0U].w;
}


#line 88
uint spCounter_0(uint slot_2)
{

#line 89
    return slot_2;
}


#line 61
uint spUniformCount_0(KernelContext_0 thread* kernelContext_14)
{

#line 61
    return kernelContext_14->giResolve_0->constants_2[43U].w;
}


#line 62
uint spCapacity_0(KernelContext_0 thread* kernelContext_15)
{

#line 62
    return kernelContext_15->giResolve_0->constants_2[42U].z;
}


#line 61
uint spUniformCount_1(KernelContext_0 thread* kernelContext_16)
{

#line 61
    return kernelContext_16->giResolve_0->constants_2[43U].w;
}


#line 62
uint spCapacity_1(KernelContext_0 thread* kernelContext_17)
{

#line 62
    return kernelContext_17->giResolve_0->constants_2[42U].z;
}


#line 63
uint spRecords_0(KernelContext_0 thread* kernelContext_18)
{

#line 63
    uint _S75 = spUniformCount_1(kernelContext_18);

#line 63
    uint _S76 = spCapacity_1(kernelContext_18);

#line 63
    return _S75 + _S76;
}


#line 80
uint spRecordBase_0(uint slot_3, uint record_1, KernelContext_0 thread* kernelContext_19)
{

#line 80
    uint _S77 = spRecords_0(kernelContext_19);
    return (slot_3 * _S77 + record_1) * 18U;
}


#line 218
SpProbe_0 spLoadProbe_0(uint slot_4, uint record_2, KernelContext_0 thread* kernelContext_20)
{

#line 218
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


#line 399
bool spInvalidated_0(float3 point_0, KernelContext_0 thread* kernelContext_21)
{

#line 400
    uint _S81 = min(kernelContext_21->giResolve_0->constants_2[26U].x, 4U);

#line 400
    uint region_0 = 0U;
    for(;;)
    {

#line 401
        if(region_0 < _S81)
        {
        }
        else
        {

#line 401
            break;
        }

#line 402
        uint _S82 = region_0 * 2U;

#line 402
        float3 lo_1 = (as_type<float3>((kernelContext_21->giResolve_0->constants_2[27U + _S82].xyz)));
        float3 hi_1 = (as_type<float3>((kernelContext_21->giResolve_0->constants_2[28U + _S82].xyz)));
        float _S83 = point_0.x;

#line 404
        bool _S84;

#line 404
        if(_S83 >= (lo_1.x))
        {

#line 404
            _S84 = (point_0.y) >= (lo_1.y);

#line 404
        }
        else
        {

#line 404
            _S84 = false;

#line 404
        }

#line 404
        bool _S85;

#line 404
        if(_S84)
        {

#line 404
            _S85 = (point_0.z) >= (lo_1.z);

#line 404
        }
        else
        {

#line 404
            _S85 = false;

#line 404
        }

#line 404
        bool _S86;

#line 404
        if(_S85)
        {

#line 404
            _S86 = _S83 <= (hi_1.x);

#line 404
        }
        else
        {

#line 404
            _S86 = false;

#line 404
        }

#line 404
        bool _S87;

#line 404
        if(_S86)
        {

#line 404
            _S87 = (point_0.y) <= (hi_1.y);

#line 404
        }
        else
        {

#line 404
            _S87 = false;

#line 404
        }

#line 404
        bool _S88;
        if(_S87)
        {

#line 405
            _S88 = (point_0.z) <= (hi_1.z);

#line 405
        }
        else
        {

#line 405
            _S88 = false;

#line 405
        }

#line 404
        if(_S88)
        {
            return true;
        }

#line 401
        region_0 = region_0 + 1U;

#line 401
    }

#line 409
    return false;
}


#line 53
uint spWidth_0(KernelContext_0 thread* kernelContext_22)
{

#line 53
    return kernelContext_22->giResolve_0->constants_2[40U].x;
}


#line 54
uint spHeight_0(KernelContext_0 thread* kernelContext_23)
{

#line 54
    return kernelContext_23->giResolve_0->constants_2[40U].y;
}


#line 359
bool spLanding_0(float3 position_3, uint thread* x_3, uint thread* y_3, KernelContext_0 thread* kernelContext_24)
{

#line 360
    *x_3 = 0U;
    *y_3 = 0U;

#line 361
    float4 _S89 = float4(position_3.x) ;

#line 361
    float4 _S90 = float4(position_3.y) ;

#line 361
    float4 _S91 = float4(position_3.z) ;

#line 366
)cy_msl",
    R"cy_msl(    float4 clip_0 = (as_type<float4>((kernelContext_24->giResolve_0->constants_2[6U]))) * _S89 + (as_type<float4>((kernelContext_24->giResolve_0->constants_2[7U]))) * _S90 + (as_type<float4>((kernelContext_24->giResolve_0->constants_2[8U]))) * _S91 + (as_type<float4>((kernelContext_24->giResolve_0->constants_2[9U])));
    float _S92 = clip_0.w;

#line 367
    if(_S92 <= 9.99999993922529029e-09)
    {

#line 368
        return false;
    }
    float inverse_0 = 1.0 / _S92;

    float _S93 = clip_0.x * inverse_0 * 0.5 + 0.5;

#line 372
    float _S94 = clip_0.y * inverse_0 * 0.5 + 0.5;

#line 377
    float4 clip_1 = (as_type<float4>((kernelContext_24->giResolve_0->constants_2[10U]))) * _S89 + (as_type<float4>((kernelContext_24->giResolve_0->constants_2[11U]))) * _S90 + (as_type<float4>((kernelContext_24->giResolve_0->constants_2[12U]))) * _S91 + (as_type<float4>((kernelContext_24->giResolve_0->constants_2[13U])));
    float _S95 = clip_1.w;

#line 378
    if(_S95 <= 9.99999993922529029e-09)
    {

#line 379
        return false;
    }
    float inverse_1 = 1.0 / _S95;



    float _S96 = _S93 + (clip_1.x * inverse_1 * 0.5 + 0.5 - _S93);

#line 385
    float _S97 = _S94 + (clip_1.y * inverse_1 * 0.5 + 0.5 - _S94);

#line 385
    uint _S98 = spWidth_0(kernelContext_24);
    float width_0 = float(_S98);

#line 386
    uint _S99 = spHeight_0(kernelContext_24);
    float height_0 = float(_S99);
    float fx_1 = floor(_S96 * width_0);
    float fy_1 = floor((1.0 - _S97) * height_0);

#line 389
    bool _S100;
    if(fx_1 < 0.0)
    {

#line 390
        _S100 = true;

#line 390
    }
    else
    {

#line 390
        _S100 = fy_1 < 0.0;

#line 390
    }

#line 390
    if(_S100)
    {

#line 390
        _S100 = true;

#line 390
    }
    else
    {

#line 390
        _S100 = fx_1 >= width_0;

#line 390
    }

#line 390
    if(_S100)
    {

#line 390
        _S100 = true;

#line 390
    }
    else
    {

#line 390
        _S100 = fy_1 >= height_0;

#line 390
    }

#line 390
    if(_S100)
    {

#line 391
        return false;
    }
    *x_3 = uint(fx_1);
    *y_3 = uint(fy_1);
    return true;
}


#line 143
struct SpTaps_0
{
    array<uint, int(4)> index_0;
    array<float, int(4)> weight_0;
    uint count_0;
};


#line 55
uint spSpacing_0(KernelContext_0 thread* kernelContext_25)
{

#line 55
    return kernelContext_25->giResolve_0->constants_2[40U].z;
}


#line 57
uint spTilesX_0(KernelContext_0 thread* kernelContext_26)
{

#line 57
    return kernelContext_26->giResolve_0->constants_2[41U].x;
}


#line 58
uint spTilesY_0(KernelContext_0 thread* kernelContext_27)
{

#line 58
    return kernelContext_27->giResolve_0->constants_2[41U].y;
}


#line 149
void spAxisTaps_0(uint coordinate_0, uint step_2, uint count_1, uint thread* base_1, float thread* fraction_0)
{

#line 150
    float g_0 = (float(coordinate_0) - float(step_2 / 2U)) / float(step_2);
    if(g_0 <= 0.0)
    {

#line 152
        *base_1 = 0U;
        *fraction_0 = 0.0;
        return;
    }
    uint _S101 = uint(g_0);

#line 156
    *base_1 = _S101;
    *fraction_0 = g_0 - float(_S101);
    if((*base_1 + 1U) >= count_1)
    {

#line 159
        *base_1 = count_1 - 1U;
        *fraction_0 = 0.0;

#line 158
    }



    return;
}


#line 164
SpTaps_0 spBilinearTaps_0(uint x_4, uint y_4, uint step_3, uint countX_0, uint countY_0)
{

#line 165
    thread SpTaps_0 taps_0;
    (&taps_0)->count_0 = 0U;

#line 166
    uint t_3 = 0U;
    for(;;)
    {

#line 167
        if(t_3 < 4U)
        {
        }
        else
        {

#line 167
            break;
        }

#line 168
        (&taps_0)->index_0[t_3] = 0U;
        (&taps_0)->weight_0[t_3] = 0.0;

#line 167
        t_3 = t_3 + 1U;

#line 167
    }



    thread uint bx_1;

    thread float fx_2;

    spAxisTaps_0(x_4, step_3, countX_0, &bx_1, &fx_2);

#line 172
    thread uint by_1;

    thread float fy_2;

    spAxisTaps_0(y_4, step_3, countY_0, &by_1, &fy_2);

#line 176
    uint j_0 = 0U;
    for(;;)
    {

#line 177
        if(j_0 < 2U)
        {
        }
        else
        {

#line 177
            break;
        }

#line 177
        uint i_0 = 0U;
        for(;;)
        {

#line 178
            if(i_0 < 2U)
            {
            }
            else
            {

#line 178
                break;
            }

#line 178
            float _S102;
            if(i_0 == 0U)
            {

#line 179
                _S102 = 1.0 - fx_2;

#line 179
            }
            else
            {

#line 179
                _S102 = fx_2;

#line 179
            }

#line 179
            float _S103;

#line 179
            if(j_0 == 0U)
            {

#line 179
                _S103 = 1.0 - fy_2;

#line 179
            }
            else
            {

#line 179
                _S103 = fy_2;

#line 179
            }

#line 179
            float weight_1 = _S102 * _S103;
            if(weight_1 <= 0.0)
            {

#line 181
                i_0 = i_0 + 1U;

#line 178
                continue;
            }



            (&taps_0)->index_0[(&taps_0)->count_0] = (by_1 + j_0) * countX_0 + (bx_1 + i_0);
            (&taps_0)->weight_0[(&taps_0)->count_0] = weight_1;
            (&taps_0)->count_0 = (&taps_0)->count_0 + 1U;

#line 178
            i_0 = i_0 + 1U;

#line 178
        }

#line 177
        j_0 = j_0 + 1U;

#line 177
    }

#line 188
    return taps_0;
}


#line 56
uint spCell_0(KernelContext_0 thread* kernelContext_28)
{

#line 56
    return kernelContext_28->giResolve_0->constants_2[40U].w;
}

uint spCellsX_0(KernelContext_0 thread* kernelContext_29)
{

#line 59
    return kernelContext_29->giResolve_0->constants_2[41U].z;
}


#line 60
uint spCellsY_0(KernelContext_0 thread* kernelContext_30)
{

#line 60
    return kernelContext_30->giResolve_0->constants_2[41U].w;
}


#line 59
uint spCellsX_1(KernelContext_0 thread* kernelContext_31)
{

#line 59
    return kernelContext_31->giResolve_0->constants_2[41U].z;
}


#line 60
uint spCellsY_1(KernelContext_0 thread* kernelContext_32)
{

#line 60
    return kernelContext_32->giResolve_0->constants_2[41U].w;
}


#line 84
uint spTableIndex_0(uint slot_5, uint cell_2, KernelContext_0 thread* kernelContext_33)
{

#line 84
    uint _S104 = spCellsX_1(kernelContext_33);
    uint _S105 = slot_5 * _S104;

#line 85
    uint _S106 = spCellsY_1(kernelContext_33);

#line 85
    return 4U + _S105 * _S106 + cell_2;
}


#line 123
float spGeometryWeight_0(float3 probePosition_0, float3 probeNormal_0, float3 position_4, float3 normal_5, float depth_1, float tolerance_0)
{
    float plane_0 = abs(dot(probeNormal_0, position_4 - probePosition_0));
    float reach_1 = tolerance_0 * depth_1;
    if(reach_1 <= 0.0)
    {

#line 128
        return 0.0;
    }

    float _S107 = max(0.0, dot(probeNormal_0, normal_5));
    float facing_0 = _S107 * _S107;
    float facing_1 = facing_0 * facing_0;

    return max(0.0, 1.0 - plane_0 / reach_1) * (facing_1 * facing_1);
}


#line 80
uint spRecordBase_1(uint slot_6, uint record_3, KernelContext_0 thread* kernelContext_34)
{

#line 80
    uint _S108 = spRecords_0(kernelContext_34);
    return (slot_6 * _S108 + record_3) * 18U;
}


#line 231
void spLoadPacked_0(uint base_2, array<float3, int(9)> thread* coefficients_0, float thread* last_1, KernelContext_0 thread* kernelContext_35)
{

#line 232
    thread array<float, int(28)> values_0;

#line 232
    uint v_1 = 0U;
    for(;;)
    {

#line 233
        if(v_1 < 7U)
        {
        }
        else
        {

#line 233
            break;
        }

#line 234
        float4 device* _S109 = kernelContext_35->giResolve_0->probes_1+(base_2 + v_1);
        uint _S110 = v_1 * 4U;

#line 235
        values_0[_S110] = (*_S109).x;
        values_0[_S110 + 1U] = (*_S109).y;
        values_0[_S110 + 2U] = (*_S109).z;
        values_0[_S110 + 3U] = (*_S109).w;

#line 233
        v_1 = v_1 + 1U;

#line 233
    }

#line 233
    uint k_0 = 0U;

#line 240
    for(;;)
    {

#line 240
        if(k_0 < 9U)
        {
        }
        else
        {

#line 240
            break;
        }

#line 241
        uint _S111 = k_0 * 3U;

#line 241
        (*coefficients_0)[k_0] = float3(values_0[_S111], values_0[_S111 + 1U], values_0[_S111 + 2U]);

#line 240
        k_0 = k_0 + 1U;

#line 240
    }


    *last_1 = values_0[int(27)];
    return;
}


#line 260
void spLoadCoefficients_0(uint slot_7, uint record_4, array<float3, int(9)> thread* coefficients_1, float thread* history_0, KernelContext_0 thread* kernelContext_36)
{

#line 260
    uint _S112 = spRecordBase_1(slot_7, record_4, kernelContext_36);

#line 260
    spLoadPacked_0(_S112 + 2U, coefficients_1, history_0, kernelContext_36);

    return;
}


#line 413
bool spReproject_0(const SpProbe_0 thread* probe_2, float tolerance_1, array<float3, int(9)> thread* coefficients_2, float thread* history_1, uint thread* matched_0, KernelContext_0 thread* kernelContext_37)
{

#line 414
    uint k_1 = 0U;
    for(;;)
    {

#line 415
        if(k_1 < 9U)
        {
        }
        else
        {

#line 415
            break;
        }

#line 416
        (*coefficients_2)[k_1] = float3(0.0, 0.0, 0.0);

#line 415
        k_1 = k_1 + 1U;

#line 415
    }


    *history_1 = 0.0;
    *matched_0 = 4294967295U;

#line 419
    float3 _S113 = probe_2->position_2;

#line 419
    bool _S114 = spInvalidated_0(probe_2->position_2, kernelContext_37);
    if(_S114)
    {

#line 421
        return false;
    }
    thread uint x_5;
    thread uint y_5;

#line 424
    bool _S115 = spLanding_0(_S113, &x_5, &y_5, kernelContext_37);
    if(!_S115)
    {

#line 426
        return false;
    }

#line 426
    uint _S116 = spSlot_0(kernelContext_37);


    uint previous_0 = 1U - _S116;

#line 429
    float bestWeight_0 = 0.0;

#line 429
    uint best_4 = 4294967295U;

#line 429
    uint pass_0 = 0U;


    for(;;)
    {

#line 432
        if(pass_0 < 2U)
        {
        }
        else
        {

#line 432
            break;
        }

#line 432
        SpTaps_0 _S117;

        if(pass_0 == 0U)
        {

#line 434
            uint _S118 = spSpacing_0(kernelContext_37);

#line 434
            uint _S119 = spTilesX_0(kernelContext_37);

#line 434
            uint _S120 = spTilesY_0(kernelContext_37);

#line 434
            _S117 = spBilinearTaps_0(x_5, y_5, _S118, _S119, _S120);

#line 434
        }
        else
        {

#line 434
            uint _S121 = spCell_0(kernelContext_37);

#line 434
            uint _S122 = spCellsX_0(kernelContext_37);

#line 434
            uint _S123 = spCellsY_0(kernelContext_37);

#line 434
            _S117 = spBilinearTaps_0(x_5, y_5, _S121, _S122, _S123);

#line 434
        }

#line 434
        float bestWeight_1 = bestWeight_0;

#line 434
        uint best_5 = best_4;

#line 434
        uint tap_0 = 0U;

        for(;;)
        {

#line 436
            if(tap_0 < (_S117.count_0))
            {
            }
            else
            {

#line 436
                break;
            }

#line 434
            SpTaps_0 _S124 = _S117;

            uint _S125 = tap_0;

#line 436
            uint record_5;

            if(pass_0 == 1U)
            {

#line 438
                uint _S126 = spTableIndex_0(previous_0, _S124.index_0[_S125], kernelContext_37);
                uint device* _S127 = kernelContext_37->giResolve_0->slots_0+_S126;

#line 439
                uint slot_8 = *_S127;
                if((*_S127) == 4294967295U)
                {

#line 441
                    tap_0 = tap_0 + 1U;

#line 436
                    continue;
                }

#line 436
                uint _S128 = spUniformCount_0(kernelContext_37);

#line 436
                record_5 = _S128 + slot_8;

            }
            else
            {

#line 438
                record_5 = _S124.index_0[_S125];

#line 438
            }

#line 438
            SpProbe_0 _S129 = spLoadProbe_0(previous_0, record_5, kernelContext_37);

#line 446
            if((_S129.depth_0) <= 0.0)
            {

#line 447
                tap_0 = tap_0 + 1U;

#line 436
                continue;
            }

#line 450
            float weight_2 = _S117.weight_0[tap_0] * spGeometryWeight_0(_S129.position_2, _S129.normal_4, _S113, probe_2->normal_4, probe_2->depth_0, tolerance_1);

#line 450
            uint best_6;

#line 450
            float bestWeight_2;


            if(weight_2 > bestWeight_1)
            {

#line 453
                bestWeight_2 = weight_2;

#line 453
                best_6 = record_5;

#line 453
            }
            else
            {

#line 453
                bestWeight_2 = bestWeight_1;

#line 453
                best_6 = best_5;

#line 453
            }

#line 453
            bestWeight_1 = bestWeight_2;

#line 453
            best_5 = best_6;

#line 436
            tap_0 = tap_0 + 1U;

#line 436
        }

#line 432
        uint pass_1 = pass_0 + 1U;

#line 432
        bestWeight_0 = bestWeight_1;

#line 432
        best_4 = best_5;

#line 432
        pass_0 = pass_1;

#line 432
    }

#line 432
    bool _S130;

#line 459
    if(best_4 == 4294967295U)
    {

#line 459
        _S130 = true;

#line 459
    }
    else
    {

#line 459
        _S130 = bestWeight_0 < 0.25;

#line 459
    }

#line 459
    if(_S130)
    {

#line 460
        return false;
    }

#line 460
    spLoadCoefficients_0(previous_0, best_4, coefficients_2, history_1, kernelContext_37);


    *matched_0 = best_4;
    return true;
}


#line 73 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
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


#line 468 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
bool spProbeTraces_0(uint id_1, uint frame_1, float density_0)
{

#line 469
    if(density_0 >= 1.0)
    {

#line 470
        return true;
    }
    return (grUnit_0(grHash_0(id_1, frame_1, 9U))) < density_0;
}


#line 440 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
int gpFloorDiv_0(float value_3, float divisor_0)
{

#line 441
    return int(floor(value_3 / divisor_0));
}


#line 487
uint gpLayoutProbe_0(uint level_3, int3 cell_3, KernelContext_0 thread* kernelContext_38)
{

#line 488
    int3 centre_0 = (as_type<int3>((kernelContext_38->giProbes_0->constants_0[6U + level_3].xyz)));
    int half_0 = int(kernelContext_38->giProbes_0->constants_0[1U].w);
    int3 _S131 = int3(half_0, half_0, half_0);

#line 490
    bool _S132;

#line 490
    if(any(cell_3 < (centre_0 - _S131)))
    {

#line 490
        _S132 = true;

#line 490
    }
    else
    {

#line 490
        _S132 = any(cell_3 > (centre_0 + _S131));

#line 490
    }

#line 490
    if(_S132)
    {

#line 491
        return 4294967295U;
    }
    int edge_0 = int(kernelContext_38->giProbes_0->constants_0[5U].z);
    uint cells_0 = uint(edge_0 * edge_0 * edge_0);
    int _S133 = giWrap_0(cell_3.z, edge_0);

#line 495
    uint _S134 = uint(edge_0);

#line 495
    uint _S135 = uint(_S133) * _S134;

#line 495
    int _S136 = giWrap_0(cell_3.y, edge_0);

#line 495
    uint _S137 = (_S135 + uint(_S136)) * _S134;

    int _S138 = giWrap_0(cell_3.x, edge_0);
    return kernelContext_38->giProbes_0->layout_0[level_3 * cells_0 + (_S137 + uint(_S138))];
}


#line 114
constant array<float3, int(6)> kGpAxes_0 = { float3(1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, -1.0, 0.0), float3(0.0, 0.0, 1.0), float3(0.0, 0.0, -1.0) };

#line 445
float gpVisibilityWeight_0(uint probe_3, float3 position_5, float3 query_1, KernelContext_0 thread* kernelContext_39)
{

#line 446
    float3 offset_1 = query_1 - position_5;
    float span_0 = length(offset_1);
    if(span_0 <= 0.00009999999747379)
    {

#line 449
        return 1.0;
    }
    float3 _S139 = offset_1 / float3(span_0) ;
    uint _S140 = probe_3 * 4U;

)cy_msl",
    R"cy_msl(#line 452
    float4 device* _S141 = kernelContext_39->giProbes_0->probes_0+(_S140 + 2U);
    float4 device* _S142 = kernelContext_39->giProbes_0->probes_0+(_S140 + 3U);
    array<float, int(6)> _S143 = { (*_S141).x, (*_S141).y, (*_S141).z, (*_S141).w, (*_S142).x, (*_S142).y };

#line 454
    uint axis_0 = 0U;

#line 454
    float expected_0 = 0.0;

#line 454
    float weight_3 = 0.0;


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
        float _S144 = max(0.0, dot(kGpAxes_0[axis_0], _S139));
        float expected_1 = expected_0 + _S143[axis_0] * _S144;
        float weight_4 = weight_3 + _S144;

#line 457
        axis_0 = axis_0 + 1U;

#line 457
        expected_0 = expected_1;

#line 457
        weight_3 = weight_4;

#line 457
    }

#line 462
    if(weight_3 <= 0.0)
    {

#line 463
        return 1.0;
    }



    float slack_0 = (as_type<float>((kernelContext_39->giProbes_0->constants_0[1U].y)));
    float _S145 = expected_0 / weight_3 + slack_0;

#line 469
    if(span_0 > _S145)
    {

#line 470
        return 0.0;
    }
    float _S146 = max(slack_0, 0.00009999999747379);

#line 472
    float visible_0 = gpClamp_0((_S145 - span_0) / _S146, 0.0, 1.0);

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
        float cosine_0 = dot(kGpAxes_0[axis_0], _S139);
        if(cosine_0 < 0.5)
        {

#line 477
            axis_0 = axis_0 + 1U;

#line 474
            continue;
        }

#line 474
        visible_0 = min(visible_0, gpClamp_0((_S143[axis_0] + slack_0 - span_0 * cosine_0) / _S146, 0.0, 1.0));

#line 474
        axis_0 = axis_0 + 1U;

#line 474
    }

#line 483
    return visible_0;
}


#line 300 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float grFacing_0(float3 toward_0, float3 normal_6)
{

#line 301
    return gpClamp_0((dot(normal_6, toward_0) + 1.0) * 0.5, 0.0, 1.0);
}


#line 123 "src/rendering/gi_gpu/shaders/gi_gpu_common.slang"
float giFinestVoxel_1(KernelContext_0 thread* kernelContext_40)
{

#line 124
    return (as_type<float>((kernelContext_40->giScene_0->constants_1[9U].y)));
}


#line 201
float giDistance_1(float3 p_2, KernelContext_0 thread* kernelContext_41)
{

#line 201
    uint _S147 = giLevelCount_0(kernelContext_41);

#line 201
    float answer_1 = 1.0e+06;

#line 201
    uint level_4 = 0U;


    for(;;)
    {

#line 204
        if(level_4 < _S147)
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
        float _S148 = giSampleLevel_0(level_4, p_2, &farDistance_2, kernelContext_41);

        float _S149 = min(answer_1, _S148);
        if(_S148 < farDistance_2)
        {

#line 209
            return _S148;
        }

#line 204
        uint level_5 = level_4 + 1U;

#line 204
        answer_1 = _S149;

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
    float t_4;
    float closest_0;
    float3 position_6;
    float3 normal_7;
};

GiTraceHit_0 giSphereTrace_0(float3 origin_5, float3 direction_3, float maxDistance_1, float tMin_0, KernelContext_0 thread* kernelContext_42)
{

#line 226
    thread GiTraceHit_0 result_0;
    (&result_0)->hit_3 = false;
    (&result_0)->exhausted_0 = false;
    (&result_0)->t_4 = 0.0;
    (&result_0)->closest_0 = 0.0;
    (&result_0)->position_6 = float3(0.0, 0.0, 0.0);
    (&result_0)->normal_7 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S150 = giLevelCount_0(kernelContext_42);
    if(_S150 == 0U)
    {

#line 234
        return result_0;
    }

#line 234
    float _S151 = giFinestVoxel_0(kernelContext_42);


    float surface_0 = _S151 * 0.5;
    float _S152 = max(tMin_0, surface_0);
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
    uint step_4 = 0U;

#line 239
    float t_5 = _S152;


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
        if(t_5 > maxDistance_1)
        {
            (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
            return result_0;
        }
        float3 position_7 = origin_5 + direction_3 * float3(t_5) ;

#line 248
        float _S153 = giDistance_0(position_7, kernelContext_42);

        if(_S153 < surface_0)
        {

#line 251
            (&result_0)->hit_3 = true;
            (&result_0)->t_4 = t_5;
            (&result_0)->position_6 = position_7;


            float3 _S154 = float3(_S151, 0.0, 0.0);

#line 256
            float _S155 = giDistance_0(position_7 + _S154, kernelContext_42);

#line 256
            float _S156 = giDistance_0(position_7 - _S154, kernelContext_42);

#line 256
            float _S157 = _S155 - _S156;

            float3 _S158 = float3(0.0, _S151, 0.0);

#line 258
            float _S159 = giDistance_0(position_7 + _S158, kernelContext_42);

#line 258
            float _S160 = giDistance_0(position_7 - _S158, kernelContext_42);

#line 258
            float _S161 = _S159 - _S160;

            float3 _S162 = float3(0.0, 0.0, _S151);

#line 260
            float _S163 = giDistance_0(position_7 + _S162, kernelContext_42);

#line 260
            float _S164 = giDistance_0(position_7 - _S162, kernelContext_42);

            (&result_0)->normal_7 = giNormalisedOr_0(float3(_S157, _S161, _S163 - _S164), - direction_3);
            (&result_0)->closest_0 = closest_1;
            return result_0;
        }
        float _S165 = min(closest_1, recent_0[recentSlot_0]);
        recent_0[recentSlot_0] = _S153;
        uint _S166 = (recentSlot_0 + 1U) % 3U;
        float t_6 = t_5 + max(_S153, surface_0);

#line 242
        uint step_5 = step_4 + 1U;

#line 242
        closest_1 = _S165;

#line 242
        recentSlot_0 = _S166;

#line 242
        step_4 = step_5;

#line 242
        t_5 = t_6;

#line 242
    }

#line 271
    (&result_0)->exhausted_0 = true;

    (&result_0)->closest_0 = min(closest_1, min(recent_0[int(0)], min(recent_0[int(1)], recent_0[int(2)])));
    return result_0;
}


#line 286 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
bool grProbeOccluded_0(float3 from_0, float3 to_0, KernelContext_0 thread* kernelContext_43)
{

#line 287
    float3 offset_2 = to_0 - from_0;
    float span_1 = length(offset_2);

#line 288
    float _S167 = giFinestVoxel_1(kernelContext_43);

    if(span_1 <= (_S167 * 2.0))
    {

#line 291
        return false;
    }

#line 291
    float _S168 = giDistance_1(from_0, kernelContext_43);

#line 291
    float _S169 = giDistance_1(to_0, kernelContext_43);

    if((_S168 + _S169) >= span_1)
    {

#line 294
        return false;
    }

#line 294
    GiTraceHit_0 _S170 = giSphereTrace_0(from_0, offset_2 / float3(span_1) , span_1 - _S167, 0.0, kernelContext_43);

    return _S170.hit_3;
}


#line 273 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
void gpLoadPayload_0(uint probe_4, uint floats_0, array<float, int(48)> thread* payload_0, KernelContext_0 thread* kernelContext_44)
{

#line 273
    uint index_1 = 0U;
    for(;;)
    {

#line 274
        if(index_1 < 48U)
        {
        }
        else
        {

#line 274
            break;
        }

#line 274
        float _S171;
        if(index_1 < floats_0)
        {

#line 275
            _S171 = *(kernelContext_44->giProbes_0->payloads_0+(probe_4 * floats_0 + index_1));

#line 275
        }
        else
        {

#line 275
            _S171 = 0.0;

#line 275
        }

#line 275
        (*payload_0)[index_1] = _S171;

#line 274
        index_1 = index_1 + 1U;

#line 274
    }


    return;
}


#line 110
float3 gpMax0_0(float3 v_2)
{

#line 111
    return float3(max(0.0, v_2.x), max(0.0, v_2.y), max(0.0, v_2.z));
}


#line 154
void gpOctahedralUv_0(float3 direction_4, float thread* u_0, float thread* v_3)
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
    float _S172 = p_3.x;
    float _S173 = p_3.y;

#line 158
    float px_0;

#line 158
    float py_0;
    if((p_3.z) < 0.0)
    {

#line 160
        float _S174 = 1.0 - abs(_S173);

#line 160
        if(_S172 >= 0.0)
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
        float fx_3 = _S174 * px_0;
        float _S175 = 1.0 - abs(_S172);

#line 161
        if(_S173 >= 0.0)
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
        float fy_3 = _S175 * px_0;

#line 161
        px_0 = fx_3;

#line 161
        py_0 = fy_3;

#line 159
    }
    else
    {

#line 159
        px_0 = _S172;

#line 159
        py_0 = _S173;

#line 159
    }

#line 165
    *u_0 = gpClamp_0(px_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    *v_3 = gpClamp_0(py_0 * 0.5 + 0.5, 0.0, 1.0) * 3.0;
    return;
}


#line 220
float3 gpDecode_0(uint encoding_0, const array<float, int(48)> thread* payload_1, float3 direction_5)
{

#line 220
    uint c_0;
    if(encoding_0 == 0U)
    {



        array<float, int(4)> _S176 = { 0.28209498524665833, 0.32573533058166504 * direction_5.y, 0.32573533058166504 * direction_5.z, 0.32573533058166504 * direction_5.x };

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
            uint _S177 = c_0 * 3U;

#line 230
            result_1.x = result_1.x + (*payload_1)[_S177] * _S176[c_0];
            result_1.y = result_1.y + (*payload_1)[_S177 + 1U] * _S176[c_0];
            result_1.z = result_1.z + (*payload_1)[_S177 + 2U] * _S176[c_0];

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
        thread float v_4 = 0.0;
        gpOctahedralUv_0(direction_5, &u_1, &v_4);
        uint x0_1 = uint(u_1);
        uint y0_1 = uint(v_4);
        uint _S178 = min(x0_1 + 1U, 3U);



        uint _S179 = y0_1 * 4U;

        uint _S180 = min(y0_1 + 1U, 3U) * 4U;

        uint _S181 = (_S179 + x0_1) * 3U;

#line 250
        float3 f00_0 = float3((*payload_1)[_S181], (*payload_1)[_S181 + 1U], (*payload_1)[_S181 + 2U]);
        uint _S182 = (_S179 + _S178) * 3U;
        uint _S183 = (_S180 + x0_1) * 3U;

#line 252
        float3 f01_0 = float3((*payload_1)[_S183], (*payload_1)[_S183 + 1U], (*payload_1)[_S183 + 2U]);
        uint _S184 = (_S180 + _S178) * 3U;

#line 253
        float3 _S185 = float3((u_1 - float(x0_1))) ;
        float3 top_0 = f00_0 + (float3((*payload_1)[_S182], (*payload_1)[_S182 + 1U], (*payload_1)[_S182 + 2U]) - f00_0) * _S185;

        return top_0 + (f01_0 + (float3((*payload_1)[_S184], (*payload_1)[_S184 + 1U], (*payload_1)[_S184 + 2U]) - f01_0) * _S185 - top_0) * float3((v_4 - float(y0_1))) ;
    }
    float3 _S186 = float3(0.0, 0.0, 0.0);

#line 258
    float3 result_2 = _S186;

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
        float _S187 = max(0.0, dot(kGpAxes_0[c_0], direction_5));
        if(_S187 <= 0.0)
        {

#line 263
            c_0 = c_0 + 1U;

#line 260
            continue;
        }



        uint _S188 = c_0 * 3U;

        float total_1 = total_0 + _S187;

#line 267
        result_2 = result_2 + float3((*payload_1)[_S188], (*payload_1)[_S188 + 1U], (*payload_1)[_S188 + 2U]) * float3(_S187) ;

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
        result_2 = _S186;

#line 269
    }

#line 269
    return result_2;
}


#line 305 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
float4 grFinish_0(float3 total_2, float weight_5, float oldest_0)
{

#line 306
    if(weight_5 <= 0.0)
    {

#line 307
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    return float4(total_2 / float3(weight_5) , gpClamp_0(1.0 - gpClamp_0(oldest_0 / 240.0, 0.0, 1.0) * 0.75, 0.0, 1.0));
}


#line 317
void grSampleTraced_0(float3 position_8, float3 normal_8, float3 reflection_0, uint frame_2, float4 thread* diffuse_0, float4 thread* specular_0, KernelContext_0 thread* kernelContext_45)
{
    uint4 counts_0 = kernelContext_45->giProbes_0->constants_0[0U];
    uint _S189 = counts_0.x;
    uint _S190 = counts_0.y;
    uint _S191 = kernelContext_45->giProbes_0->constants_0[1U].z;
    float _S192 = (as_type<float>((kernelContext_45->giProbes_0->constants_0[5U].x)));
    float3 _S193 = position_8 + normal_8 * float3((as_type<float>((kernelContext_45->giProbes_0->constants_0[1U].y)))) ;
    float3 _S194 = float3(0.0, 0.0, 0.0);

#line 325
    float3 diffuseTotal_0 = _S194;

#line 325
    float diffuseOldest_0 = 0.0;

#line 325
    float3 specularTotal_0 = _S194;

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
        if(level_6 < _S191)
        {
        }
        else
        {

#line 331
            break;
        }

#line 332
        float spacing_0 = _S192 * float(1U << level_6);
        float _S195 = position_8.x;

#line 333
        int _S196 = gpFloorDiv_0(_S195 - spacing_0, spacing_0);
        float _S197 = position_8.y;

#line 334
        int _S198 = gpFloorDiv_0(_S197 - spacing_0, spacing_0);
        float _S199 = position_8.z;
        int _S200 = gpFloorDiv_0(_S195 + spacing_0, spacing_0);
        int _S201 = gpFloorDiv_0(_S197 + spacing_0, spacing_0);
        int _S202 = gpFloorDiv_0(_S199 + spacing_0, spacing_0);

#line 338
        int z_2 = gpFloorDiv_0(_S199 - spacing_0, spacing_0);
        for(;;)
        {

#line 339
            if(z_2 <= _S202)
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
            int y_6 = _S198;

#line 339
            float diffuseWeight_1 = diffuseWeight_0;

#line 339
            float specularWeight_1 = specularWeight_0;
            for(;;)
            {

#line 340
                if(y_6 <= _S201)
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
                int x_6 = _S196;

#line 340
                float diffuseWeight_2 = diffuseWeight_1;

#line 340
                float specularWeight_2 = specularWeight_1;
)cy_msl",
    R"cy_msl(                for(;;)
                {

#line 341
                    if(x_6 <= _S200)
                    {
                    }
                    else
                    {

#line 341
                        break;
                    }

#line 341
                    uint _S203 = gpLayoutProbe_0(level_6, int3(x_6, y_6, z_2), kernelContext_45);

                    if(_S203 == 4294967295U)
                    {

#line 344
                        x_6 = x_6 + int(1);

#line 341
                        continue;
                    }



                    uint _S204 = _S203 * 4U;

#line 346
                    float4 device* _S205 = kernelContext_45->giProbes_0->probes_0+(_S204 + 1U);

#line 346
                    float4 record_6 = *_S205;

#line 346
                    bool _S206;
                    if(((*_S205).y) == 0.0)
                    {

#line 347
                        _S206 = true;

#line 347
                    }
                    else
                    {

#line 347
                        _S206 = (record_6.z) == 0.0;

#line 347
                    }

#line 347
                    if(_S206)
                    {

#line 348
                        x_6 = x_6 + int(1);

#line 341
                        continue;
                    }

#line 350
                    float3 at_0 = (*(kernelContext_45->giProbes_0->probes_0+_S204)).xyz;
                    float3 offset_3 = position_8 - at_0;
                    float span_2 = length(offset_3);
                    if(span_2 > spacing_0)
                    {

#line 354
                        x_6 = x_6 + int(1);

#line 341
                        continue;
                    }

#line 356
                    float _S207 = 1.0 - gpClamp_0(span_2 / spacing_0, 0.0, 1.0);

#line 356
                    float _S208 = gpVisibilityWeight_0(_S203, at_0, position_8, kernelContext_45);

#line 356
                    float falloff_0 = _S207 * _S208;

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
                        toward_1 = normal_8;

#line 359
                    }

#line 359
                    float diffuseShare_0;
                    if(apart_0)
                    {

#line 360
                        diffuseShare_0 = falloff_0 * grFacing_0(toward_1, normal_8);

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
                    bool _S209;
                    if(diffuseShare_0 <= 0.0)
                    {

#line 363
                        _S209 = specularShare_0 <= 0.0;

#line 363
                    }
                    else
                    {

#line 363
                        _S209 = false;

#line 363
                    }

#line 363
                    bool _S210;

#line 363
                    if(_S209)
                    {

#line 363
                        _S210 = true;

#line 363
                    }
                    else
                    {

#line 363
                        bool _S211 = grProbeOccluded_0(_S193, at_0, kernelContext_45);

#line 363
                        _S210 = _S211;

#line 363
                    }

#line 363
                    if(_S210)
                    {
                        x_6 = x_6 + int(1);

#line 341
                        continue;
                    }

#line 367
                    thread array<float, int(48)> data_0;

#line 367
                    gpLoadPayload_0(_S203, _S190, &data_0, kernelContext_45);

                    uint last_2 = (as_type<uint>((record_6.x)));

#line 369
                    float age_1;
                    if(frame_2 > last_2)
                    {

#line 370
                        age_1 = float(frame_2 - last_2);

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
                        thread array<float, int(48)> _S212 = data_0;

#line 371
                        float3 _S213 = gpDecode_0(_S189, &_S212, normal_8);

                        float diffuseWeight_4 = diffuseWeight_2 + diffuseShare_0;
                        float _S214 = max(diffuseOldest_2, age_1);

#line 374
                        diffuseTotal_3 = diffuseTotal_2 + _S213 * float3(diffuseShare_0) ;

#line 374
                        diffuseOldest_3 = _S214;

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
                        thread array<float, int(48)> _S215 = data_0;

#line 376
                        float3 _S216 = gpDecode_0(_S189, &_S215, reflection_0);


                        float specularWeight_4 = specularWeight_2 + specularShare_0;
                        float _S217 = max(specularOldest_2, age_1);

#line 380
                        specularTotal_3 = specularTotal_2 + _S216 * float3(specularShare_0) ;

#line 380
                        specularOldest_3 = _S217;

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
                    x_6 = x_6 + int(1);

#line 341
                }

#line 340
                int y_7 = y_6 + int(1);

#line 340
                diffuseTotal_1 = diffuseTotal_2;

#line 340
                diffuseOldest_1 = diffuseOldest_2;

#line 340
                specularTotal_1 = specularTotal_2;

#line 340
                specularOldest_1 = specularOldest_2;

#line 340
                y_6 = y_7;

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


#line 246 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
void spStorePacked_0(uint base_3, const array<float3, int(9)> thread* coefficients_3, float last_3, KernelContext_0 thread* kernelContext_46)
{

#line 247
    thread array<float, int(28)> values_1;

#line 247
    uint k_2 = 0U;
    for(;;)
    {

#line 248
        if(k_2 < 9U)
        {
        }
        else
        {

#line 248
            break;
        }

#line 249
        uint _S218 = k_2 * 3U;

#line 249
        values_1[_S218] = (*coefficients_3)[k_2].x;
        values_1[_S218 + 1U] = (*coefficients_3)[k_2].y;
        values_1[_S218 + 2U] = (*coefficients_3)[k_2].z;

#line 248
        k_2 = k_2 + 1U;

#line 248
    }

#line 253
    values_1[int(27)] = last_3;

#line 253
    uint v_5 = 0U;
    for(;;)
    {

#line 254
        if(v_5 < 7U)
        {
        }
        else
        {

#line 254
            break;
        }

#line 255
        uint _S219 = v_5 * 4U;

#line 255
        *(kernelContext_46->giResolve_0->probes_1+(base_3 + v_5)) = float4(values_1[_S219], values_1[_S219 + 1U], values_1[_S219 + 2U], values_1[_S219 + 3U]);

#line 254
        v_5 = v_5 + 1U;

#line 254
    }



    return;
}




void spStoreCoefficients_0(uint slot_9, uint record_7, const array<float3, int(9)> thread* coefficients_4, float history_2, KernelContext_0 thread* kernelContext_47)
{

#line 264
    uint _S220 = spRecordBase_1(slot_9, record_7, kernelContext_47);

#line 264
    spStorePacked_0(_S220 + 2U, coefficients_4, history_2, kernelContext_47);

    return;
}


#line 274
void spStoreFiltered_0(uint slot_10, uint record_8, const array<float3, int(9)> thread* coefficients_5, KernelContext_0 thread* kernelContext_48)
{

#line 274
    uint _S221 = spRecordBase_1(slot_10, record_8, kernelContext_48);

#line 274
    spStorePacked_0(_S221 + 11U, coefficients_5, 0.0, kernelContext_48);

    return;
}


#line 88 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
void grBasis_0(float3 normal_9, float3 thread* tangent_0, float3 thread* bitangent_0)
{

#line 89
    float _S222 = normal_9.z;

#line 89
    float sign_0;

#line 89
    if(_S222 >= 0.0)
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
    float a_1 = -1.0 / (sign_0 + _S222);
    float _S223 = normal_9.x;

#line 91
    float _S224 = normal_9.y;

#line 91
    float b_1 = _S223 * _S224 * a_1;
    *tangent_0 = float3(1.0 + sign_0 * _S223 * _S223 * a_1, sign_0 * b_1, - sign_0 * _S223);
    *bitangent_0 = float3(b_1, sign_0 + _S224 * _S224 * a_1, - _S224);
    return;
}


#line 192 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
float3 spHemisphereDirection_0(float3 normal_10, float z_4, float turn_0)
{

#line 193
    thread float3 tangent_1;
    thread float3 bitangent_1;
    grBasis_0(normal_10, &tangent_1, &bitangent_1);
    float radius_1 = sqrt(max(0.0, 1.0 - z_4 * z_4));
    float angle_0 = 6.28318548202514648 * turn_0;
    return giNormalisedOr_0(tangent_1 * float3((radius_1 * cos(angle_0)))  + bitangent_1 * float3((radius_1 * sin(angle_0)))  + normal_10 * float3(z_4) , normal_10);
}


#line 94
void spBasis_0(float3 d_0, array<float, int(9)> thread* basis_0)
{

#line 95
    (*basis_0)[int(0)] = 0.28209498524665833;
    float _S225 = d_0.y;

#line 96
    (*basis_0)[int(1)] = 0.48860299587249756 * _S225;
    float _S226 = d_0.z;

#line 97
    (*basis_0)[int(2)] = 0.48860299587249756 * _S226;
    float _S227 = d_0.x;

#line 98
    (*basis_0)[int(3)] = 0.48860299587249756 * _S227;
    (*basis_0)[int(4)] = 1.09254801273345947 * (_S227 * _S225);
    (*basis_0)[int(5)] = 1.09254801273345947 * (_S225 * _S226);
    (*basis_0)[int(6)] = 0.31539198756217957 * (3.0 * (_S226 * _S226) - 1.0);
    (*basis_0)[int(7)] = 1.09254801273345947 * (_S227 * _S226);
    (*basis_0)[int(8)] = 0.54627400636672974 * (_S227 * _S227 - _S225 * _S225);
    return;
}


#line 106
uint spBandOf_0(uint k_3)
{

#line 106
    uint _S228;
    if(k_3 == 0U)
    {

#line 107
        _S228 = 0U;

#line 107
    }
    else
    {

#line 107
        if(k_3 < 4U)
        {

#line 107
            _S228 = 1U;

#line 107
        }
        else
        {

#line 107
            _S228 = 2U;

#line 107
        }

#line 107
    }

#line 107
    return _S228;
}

float3 spEvaluate_0(const array<float3, int(9)> thread* coefficients_6, float3 direction_6, float3 band_0)
{

#line 111
    thread array<float, int(9)> basis_1;
    spBasis_0(direction_6, &basis_1);

#line 112
    float3 sum_1 = float3(0.0, 0.0, 0.0);

#line 112
    uint k_4 = 0U;

    for(;;)
    {

#line 114
        if(k_4 < 9U)
        {
        }
        else
        {

#line 114
            break;
        }

#line 115
        uint b_2 = spBandOf_0(k_4);

#line 115
        float scale_0;
        if(b_2 == 0U)
        {

#line 116
            scale_0 = band_0.x;

#line 116
        }
        else
        {

#line 116
            if(b_2 == 1U)
            {

#line 116
                scale_0 = band_0.y;

#line 116
            }
            else
            {

#line 116
                scale_0 = band_0.z;

#line 116
            }

#line 116
        }
        float3 _S229 = sum_1 + (*coefficients_6)[k_4] * float3((basis_1[k_4] * scale_0)) ;

#line 114
        uint k_5 = k_4 + 1U;

#line 114
        sum_1 = _S229;

#line 114
        k_4 = k_5;

#line 114
    }

#line 119
    return float3(max(sum_1.x, 0.0), max(sum_1.y, 0.0), max(sum_1.z, 0.0));
}


#line 506 "src/rendering/gi_gpu/shaders/gi_probe_common.slang"
float4 gpSample_0(float3 position_9, float3 normal_11, uint frame_3, KernelContext_0 thread* kernelContext_49)
{

#line 507
    uint4 counts_1 = kernelContext_49->giProbes_0->constants_0[0U];
    uint _S230 = counts_1.x;
    uint _S231 = counts_1.y;
    uint _S232 = kernelContext_49->giProbes_0->constants_0[1U].z;
    float _S233 = (as_type<float>((kernelContext_49->giProbes_0->constants_0[5U].x)));

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
        if(level_7 < _S232)
        {
        }
        else
        {

#line 515
            break;
        }

#line 516
        float spacing_1 = _S233 * float(1U << level_7);
        float radius_2 = spacing_1 * 1.5;
        float _S234 = position_9.x;

#line 518
        int _S235 = gpFloorDiv_0(_S234 - radius_2, spacing_1);
        float _S236 = position_9.y;

#line 519
        int _S237 = gpFloorDiv_0(_S236 - radius_2, spacing_1);
        float _S238 = position_9.z;
        int _S239 = gpFloorDiv_0(_S234 + radius_2, spacing_1);
        int _S240 = gpFloorDiv_0(_S236 + radius_2, spacing_1);
        int _S241 = gpFloorDiv_0(_S238 + radius_2, spacing_1);

#line 523
        int z_5 = gpFloorDiv_0(_S238 - radius_2, spacing_1);
        for(;;)
        {

#line 524
            if(z_5 <= _S241)
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
            int y_8 = _S237;

#line 524
            float totalWeight_1 = totalWeight_0;
            for(;;)
            {

#line 525
                if(y_8 <= _S240)
                {
                }
                else
                {

#line 525
                    break;
                }

#line 525
                float3 total_5 = total_4;

)cy_msl",
    R"cy_msl(#line 525
                float oldest_3 = oldest_2;

#line 525
                int x_7 = _S235;

#line 525
                float totalWeight_2 = totalWeight_1;
                for(;;)
                {

#line 526
                    if(x_7 <= _S239)
                    {
                    }
                    else
                    {

#line 526
                        break;
                    }

#line 526
                    uint _S242 = gpLayoutProbe_0(level_7, int3(x_7, y_8, z_5), kernelContext_49);

                    if(_S242 == 4294967295U)
                    {

#line 529
                        x_7 = x_7 + int(1);

#line 526
                        continue;
                    }



                    uint _S243 = _S242 * 4U;

#line 531
                    float4 device* _S244 = kernelContext_49->giProbes_0->probes_0+(_S243 + 1U);

#line 531
                    float4 record_9 = *_S244;

#line 531
                    bool _S245;
                    if(((*_S244).y) == 0.0)
                    {

#line 532
                        _S245 = true;

#line 532
                    }
                    else
                    {

#line 532
                        _S245 = (record_9.z) == 0.0;

#line 532
                    }

#line 532
                    if(_S245)
                    {

#line 533
                        x_7 = x_7 + int(1);

#line 526
                        continue;
                    }

#line 535
                    float3 at_1 = (*(kernelContext_49->giProbes_0->probes_0+_S243)).xyz;
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
                    float _S246 = gpVisibilityWeight_0(_S242, at_1, position_9, kernelContext_49);



                    float weight_8 = weight_7 * _S246;
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
                    gpLoadPayload_0(_S242, _S231, &data_1, kernelContext_49);

#line 550
                    thread array<float, int(48)> _S247 = data_1;

#line 550
                    float3 _S248 = gpDecode_0(_S230, &_S247, normal_11);

                    float3 _S249 = total_5 + _S248 * float3(weight_8) ;
                    float totalWeight_3 = totalWeight_2 + weight_8;
                    uint last_4 = (as_type<uint>((record_9.x)));

#line 554
                    float age_2;
                    if(frame_3 > last_4)
                    {

#line 555
                        age_2 = float(frame_3 - last_4);

#line 555
                    }
                    else
                    {

#line 555
                        age_2 = 0.0;

#line 555
                    }
                    float _S250 = max(oldest_3, age_2);

#line 556
                    total_5 = _S249;

#line 556
                    oldest_3 = _S250;

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


#line 138 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
float spLuminance_0(float3 c_1)
{

#line 139
    return 0.2125999927520752 * c_1.x + 0.71520000696182251 * c_1.y + 0.07220000028610229 * c_1.z;
}


#line 203
uint spGuideCell_0(float z_7, float turn_1)
{

    return min(uint(z_7 * 4.0), 3U) * 4U + min(uint(turn_1 * 4.0), 3U);
}


#line 263 "src/rendering/gi_gpu/shaders/gi_resolve_common.slang"
GrSample_0 grGatherSample_0(const GrSample_0 thread* ray_1, const GrSample_0 thread* standIn_0, uint excluded_0)
{

#line 263
    uint _S251 = standIn_0->source_0;

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
        hasStandIn_0 = (excluded_0 & (1U << _S251)) == 0U;

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
        (&sample_3)->source_0 = _S251;
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


#line 168
float grFarFieldWeight_0(float distance_0, KernelContext_0 thread* kernelContext_50)
{

#line 169
    float4 far_0 = (as_type<float4>((kernelContext_50->giResolve_0->constants_2[3U])));
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
GiTraceHit_0 giSphereTrace_1(float3 origin_6, float3 direction_7, float maxDistance_2, float tMin_1, KernelContext_0 thread* kernelContext_51)
{

#line 226
    thread GiTraceHit_0 result_3;
    (&result_3)->hit_3 = false;
    (&result_3)->exhausted_0 = false;
    (&result_3)->t_4 = 0.0;
    (&result_3)->closest_0 = 0.0;
    (&result_3)->position_6 = float3(0.0, 0.0, 0.0);
    (&result_3)->normal_7 = float3(0.0, 1.0, 0.0);

#line 232
    uint _S254 = giLevelCount_0(kernelContext_51);
    if(_S254 == 0U)
    {

#line 234
        return result_3;
    }

#line 234
    float _S255 = giFinestVoxel_0(kernelContext_51);


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
        float _S257 = giDistance_0(position_10, kernelContext_51);

        if(_S257 < surface_1)
        {

#line 251
            (&result_3)->hit_3 = true;
            (&result_3)->t_4 = t_8;
            (&result_3)->position_6 = position_10;


            float3 _S258 = float3(_S255, 0.0, 0.0);

#line 256
            float _S259 = giDistance_0(position_10 + _S258, kernelContext_51);

#line 256
            float _S260 = giDistance_0(position_10 - _S258, kernelContext_51);

#line 256
            float _S261 = _S259 - _S260;

            float3 _S262 = float3(0.0, _S255, 0.0);

#line 258
            float _S263 = giDistance_0(position_10 + _S262, kernelContext_51);

#line 258
            float _S264 = giDistance_0(position_10 - _S262, kernelContext_51);

#line 258
            float _S265 = _S263 - _S264;

            float3 _S266 = float3(0.0, 0.0, _S255);

#line 260
            float _S267 = giDistance_0(position_10 + _S266, kernelContext_51);

#line 260
            float _S268 = giDistance_0(position_10 - _S266, kernelContext_51);

            (&result_3)->normal_7 = giNormalisedOr_0(float3(_S261, _S265, _S267 - _S268), - direction_7);
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
float grSkyVisibilityRay_0(float3 position_11, float3 normal_13, uint index_3, uint count_3, KernelContext_0 thread* kernelContext_52)
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
        uint _S272 = giLevelCount_0(kernelContext_52);

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
    float reach_2 = (as_type<float>((kernelContext_52->giResolve_0->constants_2[3U].w)));

#line 188
    float _S273 = giFinestVoxel_0(kernelContext_52);

#line 188
    GiTraceHit_0 _S274 = giSphereTrace_1(position_11 + normal_13 * float3((_S273 * 2.0)) , giHemisphereDirection_0(normal_13, index_3, count_3), reach_2, 0.0, kernelContext_52);

#line 193
    if(_S274.hit_3)
    {

#line 194
        return 0.0;
    }
    return gpClamp_0(_S274.closest_0 / (_S273 * 4.0), 0.0, 1.0);
}


#line 528 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
void spTrace_0(uint record_10, uint lane_0, KernelContext_0 thread* kernelContext_53)
{

#line 528
    uint _S275 = spSlot_0(kernelContext_53);

#line 528
    bool allowed_0;


    if(record_10 == 0U)
    {

#line 531
        allowed_0 = lane_0 == 0U;

#line 531
    }
    else
    {

#line 531
        allowed_0 = false;

#line 531
    }

#line 531
    if(allowed_0)
    {

#line 532
        *(kernelContext_53->giResolve_0->slots_0+spCounter_0(1U - _S275)) = 0U;

#line 531
    }

#line 531
    uint _S276 = spUniformCount_0(kernelContext_53);



    if(record_10 >= _S276)
    {

#line 536
        uint _S277 = record_10 - _S276;

#line 536
        uint _S278 = *(kernelContext_53->giResolve_0->slots_0+spCounter_0(_S275));

#line 536
        uint _S279 = spCapacity_0(kernelContext_53);

#line 536
        allowed_0 = _S277 >= (min(_S278, _S279));

#line 535
    }
    else
    {

#line 535
        allowed_0 = false;

#line 535
    }

#line 535
    if(allowed_0)
    {
        return;
    }

#line 537
    SpProbe_0 _S280 = spLoadProbe_0(_S275, record_10, kernelContext_53);


    if((_S280.depth_0) <= 0.0)
    {

#line 541
        return;
    }
    uint4 shape_1 = kernelContext_53->giResolve_0->constants_2[42U];
    float4 weights_0 = (as_type<float4>((kernelContext_53->giResolve_0->constants_2[43U])));
    uint frame_4 = kernelContext_53->giResolve_0->constants_2[18U].x;
    uint cacheFrame_2 = kernelContext_53->giResolve_0->constants_2[4U].w;

    bool _S281 = lane_0 == 0U;

#line 548
    uint cell_4;

#line 548
    if(_S281)
    {

#line 549
        thread array<float3, int(9)> previous_1;
        thread float history_3 = 0.0;
        thread uint matched_1 = 4294967295U;
        if(((shape_1.w) & 2U) != 0U)
        {

#line 552
            allowed_0 = (kernelContext_53->giResolve_0->constants_2[5U].w) != 0U;

#line 552
        }
        else
        {

#line 552
            allowed_0 = false;

#line 552
        }

#line 552
        bool found_0;
        if(allowed_0)
        {

#line 553
            float _S282 = weights_0.y;

#line 553
            thread SpProbe_0 _S283 = _S280;

#line 553
            bool _S284 = spReproject_0(&_S283, _S282, &previous_1, &history_3, &matched_1, kernelContext_53);

#line 553
            found_0 = _S284;

#line 553
        }
        else
        {

#line 553
            found_0 = false;

#line 553
        }

#line 553
        cell_4 = 0U;
        for(;;)
        {

#line 554
            if(cell_4 < 9U)
            {
            }
            else
            {

#line 554
                break;
            }

#line 555
            (*kernelContext_53->spHistory_0)[cell_4] = previous_1[cell_4];

#line 554
            cell_4 = cell_4 + 1U;

#line 554
        }


        *kernelContext_53->spHistoryCount_0 = history_3;
        if(found_0)
        {

#line 558
            cell_4 = 1U;

#line 558
        }
        else
        {

#line 558
            cell_4 = 0U;

#line 558
        }

#line 558
        *kernelContext_53->spReprojected_0 = cell_4;
        float density_1 = (as_type<float>((kernelContext_53->giResolve_0->constants_2[1U].w)));
        if(found_0)
        {

#line 560
            allowed_0 = !spProbeTraces_0(_S280.id_0, frame_4, density_1);

#line 560
        }
        else
        {

#line 560
            allowed_0 = false;

#line 560
        }

#line 560
        if(allowed_0)
        {

#line 560
            cell_4 = matched_1;

#line 560
        }
        else
        {

#line 560
            cell_4 = 4294967295U;

#line 560
        }

#line 560
        *kernelContext_53->spCarried_0 = cell_4;
        if(cell_4 == 4294967295U)
)cy_msl",
    R"cy_msl(        {

#line 561
            float3 _S285 = _S280.normal_4;
            thread float4 diffuse_1;
            thread float4 specular_1;

#line 563
            grSampleTraced_0(_S280.position_2, _S285, _S285, cacheFrame_2, &diffuse_1, &specular_1, kernelContext_53);


            *kernelContext_53->spStandIn_0 = diffuse_1;
            float _S286 = (as_type<float>((kernelContext_53->giResolve_0->constants_2[2U].y)));

#line 567
            thread SpProbe_0 _S287 = _S280;

#line 567
            float3 _S288 = spProbeOrigin_0(&_S287, _S286, kernelContext_53);

#line 567
            *kernelContext_53->spOrigin_0 = _S288;

#line 561
        }

#line 548
    }

#line 570
    threadgroup_barrier(mem_flags::mem_threadgroup);

#line 570
    uint k_6;
    if((*kernelContext_53->spCarried_0) != 4294967295U)
    {

        if(_S281)
        {

#line 575
            thread array<float3, int(9)> kept_0;

#line 575
            k_6 = 0U;
            for(;;)
            {

#line 576
                if(k_6 < 9U)
                {
                }
                else
                {

#line 576
                    break;
                }

#line 577
                kept_0[k_6] = (*kernelContext_53->spHistory_0)[k_6];

#line 576
                k_6 = k_6 + 1U;

#line 576
            }


            float _S289 = *kernelContext_53->spHistoryCount_0;

#line 579
            thread array<float3, int(9)> _S290 = kept_0;

#line 579
            spStoreCoefficients_0(_S275, record_10, &_S290, _S289, kernelContext_53);

#line 579
            thread array<float3, int(9)> _S291 = kept_0;

#line 579
            spStoreFiltered_0(_S275, record_10, &_S291, kernelContext_53);

#line 579
            uint _S292 = spRecordBase_0(1U - _S275, *kernelContext_53->spCarried_0, kernelContext_53);

#line 579
            uint _S293 = spRecordBase_0(_S275, record_10, kernelContext_53);



            *(kernelContext_53->giResolve_0->probes_1+(_S293 + 9U)) = *(kernelContext_53->giResolve_0->probes_1+(_S292 + 9U));
            *(kernelContext_53->giResolve_0->probes_1+(_S293 + 10U)) = *(kernelContext_53->giResolve_0->probes_1+(_S292 + 10U));

#line 574
        }

#line 586
        return;
    }

#line 586
    float3 value_4;


    if(lane_0 < 16U)
    {

#line 590
        float z_9 = (float(lane_0 / 4U) + 0.5) / 4.0;

        float3 direction_8 = spHemisphereDirection_0(_S280.normal_4, z_9, (float(lane_0 % 4U) + 0.5) / 4.0);

        if((*kernelContext_53->spReprojected_0) != 0U)
        {

#line 595
            thread array<float3, int(9)> previous_2;

#line 595
            cell_4 = 0U;
            for(;;)
            {

#line 596
                if(cell_4 < 9U)
                {
                }
                else
                {

#line 596
                    break;
                }

#line 597
                previous_2[cell_4] = (*kernelContext_53->spHistory_0)[cell_4];

#line 596
                cell_4 = cell_4 + 1U;

#line 596
            }


            float3 _S294 = float3(1.0, 1.0, 1.0);

#line 599
            thread array<float3, int(9)> _S295 = previous_2;

#line 599
            float3 _S296 = spEvaluate_0(&_S295, direction_8, _S294);

#line 599
            value_4 = _S296;

#line 594
        }
        else
        {

#line 594
            float4 _S297 = gpSample_0(_S280.position_2, direction_8, cacheFrame_2, kernelContext_53);

#line 594
            value_4 = _S297.xyz;

#line 594
        }

#line 603
        (*kernelContext_53->spWeights_0)[lane_0] = max(spLuminance_0(value_4), 0.0) * z_9;

#line 589
    }

#line 605
    threadgroup_barrier(mem_flags::mem_threadgroup);

#line 605
    cell_4 = 0U;

#line 605
    float guideTotal_0 = 0.0;


    for(;;)
    {

#line 608
        if(cell_4 < 16U)
        {
        }
        else
        {

#line 608
            break;
        }

#line 609
        float guideTotal_1 = guideTotal_0 + (*kernelContext_53->spWeights_0)[cell_4];

#line 608
        cell_4 = cell_4 + 1U;

#line 608
        guideTotal_0 = guideTotal_1;

#line 608
    }

#line 608
    float guided_0;


    if(guideTotal_0 > 0.0)
    {

#line 611
        guided_0 = gpClamp_0(weights_0.z, 0.0, 0.89999997615814209);

#line 611
    }
    else
    {

#line 611
        guided_0 = 0.0;

#line 611
    }
    float uniformShare_0 = (1.0 - guided_0) * 0.5;

    uint _S298 = min(max(shape_1.x, 1U), 64U);

#line 614
    float history_4;

#line 614
    float openness_0;

    if(lane_0 < _S298)
    {

#line 617
        uint hash_3 = grHash_0(_S280.id_0 * 64U + lane_0, frame_4, 8U);
        float u0_0 = grUnit_0(hash_3);
        float u1_0 = grUnit_0(hash_3 * 656542357U);
        float u2_0 = grUnit_0(hash_3 * 2654435769U);


        if(u0_0 >= uniformShare_0)
        {

#line 623
            allowed_0 = u0_0 < (uniformShare_0 + uniformShare_0);

#line 623
        }
        else
        {

#line 623
            allowed_0 = false;

#line 623
        }

#line 623
        if(allowed_0)
        {

#line 623
            history_4 = sqrt(u1_0);

#line 623
            openness_0 = u2_0;

#line 623
        }
        else
        {

#line 625
            if(u0_0 >= (uniformShare_0 + uniformShare_0))
            {

#line 626
                float _S299 = (u0_0 - uniformShare_0 - uniformShare_0) / guided_0 * guideTotal_0;

#line 626
                cell_4 = 0U;

#line 626
                history_4 = 0.0;


                for(;;)
                {

#line 629
                    if(cell_4 < 16U)
                    {
                    }
                    else
                    {

#line 629
                        k_6 = 15U;

#line 629
                        break;
                    }

#line 630
                    float running_0 = history_4 + (*kernelContext_53->spWeights_0)[cell_4];
                    if(((*kernelContext_53->spWeights_0)[cell_4]) > 0.0)
                    {

#line 631
                        allowed_0 = _S299 < running_0;

#line 631
                    }
                    else
                    {

#line 631
                        allowed_0 = false;

#line 631
                    }

#line 631
                    if(allowed_0)
                    {

#line 631
                        k_6 = cell_4;

                        break;
                    }

#line 629
                    cell_4 = cell_4 + 1U;

#line 629
                    history_4 = running_0;

#line 629
                }

#line 637
                float _S300 = (float(k_6 % 4U) + u2_0) / 4.0;

#line 637
                history_4 = (float(k_6 / 4U) + u1_0) / 4.0;

#line 637
                openness_0 = _S300;

#line 625
            }
            else
            {

#line 625
                history_4 = u1_0;

#line 625
                openness_0 = u2_0;

#line 625
            }

#line 623
        }

#line 639
        float density_2 = uniformShare_0 / 6.28318548202514648 + uniformShare_0 * history_4 / 3.14159274101257324;

#line 639
        float density_3;

        if(guided_0 > 0.0)
        {

#line 641
            density_3 = density_2 + guided_0 * ((*kernelContext_53->spWeights_0)[spGuideCell_0(history_4, openness_0)] / guideTotal_0) * 2.5464789867401123;

#line 641
        }
        else
        {

#line 641
            density_3 = density_2;

#line 641
        }



        float3 _S301 = float3(0.0, 0.0, 0.0);
        float3 direction_9 = spHemisphereDirection_0(_S280.normal_4, history_4, openness_0);
        bool _S302 = density_3 > 0.0;

#line 647
        if(_S302)
        {
            uint excluded_1 = kernelContext_53->giResolve_0->constants_2[2U].w;

            float4 standIn_1 = *kernelContext_53->spStandIn_0;

#line 651
            GrSample_0 _S303 = spProbeRay_0(*kernelContext_53->spOrigin_0, giNormalisedOr_0(direction_9, float3(0.0, 0.0, -1.0)), (as_type<float4>((kernelContext_53->giResolve_0->constants_2[2U]))).x, kernelContext_53);


            GrSample_0 _S304 = grSample_0(standIn_1.xyz, standIn_1.w, 3U);

#line 654
            thread GrSample_0 _S305 = _S303;

#line 654
            thread GrSample_0 _S306 = _S304;

#line 654
            GrSample_0 _S307 = grGatherSample_0(&_S305, &_S306, excluded_1);
            if((_S307.source_0) != 0U)
            {

#line 655
                value_4 = _S307.radiance_0;

#line 655
                k_6 = 1U;

#line 655
            }
            else
            {

#line 655
                value_4 = _S301;

#line 655
                k_6 = 0U;

#line 655
            }

#line 647
        }
        else
        {

#line 647
            value_4 = _S301;

#line 647
            k_6 = 0U;

#line 647
        }

#line 660
        (*kernelContext_53->spRayValue_0)[lane_0] = value_4;
        (*kernelContext_53->spRayDirection_0)[lane_0] = direction_9;
        if(_S302)
        {

#line 662
            guideTotal_0 = 1.0 / (density_3 * float(_S298));

#line 662
        }
        else
        {

#line 662
            guideTotal_0 = 0.0;

#line 662
        }

#line 662
        (*kernelContext_53->spRayWeight_0)[lane_0] = guideTotal_0;
        (*kernelContext_53->spRayValid_0)[lane_0] = k_6;

#line 616
    }

#line 665
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if(lane_0 != 0U)
    {

#line 667
        return;
    }

    thread array<float3, int(9)> sum_2;

#line 670
    k_6 = 0U;
    for(;;)
    {

#line 671
        if(k_6 < 9U)
        {
        }
        else
        {

#line 671
            break;
        }

#line 672
        sum_2[k_6] = float3(0.0, 0.0, 0.0);

#line 671
        k_6 = k_6 + 1U;

#line 671
    }

#line 671
    uint ray_2 = 0U;


    for(;;)
    {

#line 674
        if(ray_2 < _S298)
        {
        }
        else
        {

#line 674
            break;
        }

#line 675
        if(((*kernelContext_53->spRayValid_0)[ray_2]) == 0U)
        {

#line 676
            ray_2 = ray_2 + 1U;

#line 674
            continue;
        }


        thread array<float, int(9)> basis_2;
        spBasis_0((*kernelContext_53->spRayDirection_0)[ray_2], &basis_2);
        float _S308 = (*kernelContext_53->spRayWeight_0)[ray_2];

#line 680
        k_6 = 0U;
        for(;;)
        {

#line 681
            if(k_6 < 9U)
            {
            }
            else
            {

#line 681
                break;
            }

#line 682
            sum_2[k_6] = sum_2[k_6] + (*kernelContext_53->spRayValue_0)[ray_2] * float3((basis_2[k_6] * _S308)) ;

#line 681
            k_6 = k_6 + 1U;

#line 681
        }

#line 674
        ray_2 = ray_2 + 1U;

#line 674
    }

#line 686
    float frames_0 = float(max(shape_1.y, 1U));
    thread array<float3, int(9)> blended_0;

    if((*kernelContext_53->spReprojected_0) != 0U)
    {

#line 690
        float _S309 = max(1.0 / (*kernelContext_53->spHistoryCount_0 + 1.0), 1.0 / frames_0);

#line 690
        k_6 = 0U;
        for(;;)
        {

#line 691
            if(k_6 < 9U)
            {
            }
            else
            {

#line 691
                break;
            }

#line 692
            blended_0[k_6] = (*kernelContext_53->spHistory_0)[k_6] + (sum_2[k_6] - (*kernelContext_53->spHistory_0)[k_6]) * float3(_S309) ;

#line 691
            k_6 = k_6 + 1U;

#line 691
        }

#line 691
        history_4 = min(*kernelContext_53->spHistoryCount_0 + 1.0, frames_0);

#line 689
    }
    else
    {

#line 689
        k_6 = 0U;

#line 696
        for(;;)
        {

#line 696
            if(k_6 < 9U)
            {
            }
            else
            {

#line 696
                break;
            }

#line 697
            blended_0[k_6] = sum_2[k_6];

#line 696
            k_6 = k_6 + 1U;

#line 696
        }

#line 696
        history_4 = 1.0;

#line 689
    }

#line 689
    thread array<float3, int(9)> _S310 = blended_0;

#line 689
    spStoreCoefficients_0(_S275, record_10, &_S310, history_4, kernelContext_53);

#line 689
    thread array<float3, int(9)> _S311 = blended_0;

#line 689
    spStoreFiltered_0(_S275, record_10, &_S311, kernelContext_53);

#line 689
    float _S312 = grFarFieldWeight_0(length(_S280.position_2 - (as_type<float3>((kernelContext_53->giResolve_0->constants_2[1U].xyz)))), kernelContext_53);

#line 706
    uint skyRays_0 = kernelContext_53->giResolve_0->constants_2[2U].z;
    if(_S312 > 0.0)
    {

#line 707
        allowed_0 = skyRays_0 > 0U;

#line 707
    }
    else
    {

#line 707
        allowed_0 = false;

#line 707
    }

#line 707
    if(allowed_0)
    {

#line 708
        uint _S313 = (frame_4 + _S280.id_0) % skyRays_0;

#line 708
        float _S314 = grSkyVisibilityRay_0(_S280.position_2, _S280.normal_4, _S313, skyRays_0, kernelContext_53);

#line 708
        openness_0 = _S314;

#line 707
    }
    else
    {

#line 707
        openness_0 = 1.0;

#line 707
    }

#line 707
    uint _S315 = spRecordBase_0(_S275, record_10, kernelContext_53);

#line 712
    *(kernelContext_53->giResolve_0->probes_1+(_S315 + 9U)) = *kernelContext_53->spStandIn_0;
    *(kernelContext_53->giResolve_0->probes_1+(_S315 + 10U)) = float4(openness_0, 0.0, 0.0, 0.0);
    return;
}


#line 17 "src/rendering/gi_gpu/shaders/gi_screen_probes_hardware.slang"
[[kernel]] void cyGiProbeTraceHardware(uint3 group_0 [[threadgroup_position_in_grid]], uint3 lane_1 [[thread_position_in_threadgroup]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiResolveSet_default_0 constant* giResolve_1 [[buffer(3)]], GiHardwareSet_default_0 constant* giHardware_1 [[buffer(4)]])
{

#line 17
    thread KernelContext_0 kernelContext_54;

#line 17
    (&kernelContext_54)->giProbes_0 = giProbes_1;

#line 17
    (&kernelContext_54)->giScene_0 = giScene_1;

#line 17
    (&kernelContext_54)->giResolve_0 = giResolve_1;

#line 17
    (&kernelContext_54)->giHardware_0 = giHardware_1;

#line 17
    threadgroup array<float3, int(9)> spHistory_1;

#line 17
    (&kernelContext_54)->spHistory_0 = &spHistory_1;

#line 17
    threadgroup float spHistoryCount_1;

#line 17
    (&kernelContext_54)->spHistoryCount_0 = &spHistoryCount_1;

#line 17
    threadgroup uint spReprojected_1;

#line 17
    (&kernelContext_54)->spReprojected_0 = &spReprojected_1;

#line 17
    threadgroup uint spCarried_1;

#line 17
    (&kernelContext_54)->spCarried_0 = &spCarried_1;

#line 17
    threadgroup float4 spStandIn_1;

#line 17
    (&kernelContext_54)->spStandIn_0 = &spStandIn_1;

#line 17
    threadgroup float3 spOrigin_1;

#line 17
    (&kernelContext_54)->spOrigin_0 = &spOrigin_1;

#line 17
    threadgroup array<float, int(16)> spWeights_1;

#line 17
    (&kernelContext_54)->spWeights_0 = &spWeights_1;

#line 17
    threadgroup array<float3, int(64)> spRayValue_1;

#line 17
    (&kernelContext_54)->spRayValue_0 = &spRayValue_1;

#line 17
    threadgroup array<float3, int(64)> spRayDirection_1;

#line 17
    (&kernelContext_54)->spRayDirection_0 = &spRayDirection_1;

#line 17
    threadgroup array<float, int(64)> spRayWeight_1;

#line 17
    (&kernelContext_54)->spRayWeight_0 = &spRayWeight_1;

#line 17
    threadgroup array<uint, int(64)> spRayValid_1;

#line 17
    (&kernelContext_54)->spRayValid_0 = &spRayValid_1;

#line 17
    spTrace_0(group_0.x, lane_1.x, &kernelContext_54);

    return;
}

)cy_msl");
inline constexpr const auto& kGiProbeTraceHardwareMsl = kGiProbeTraceHardwareMslText.text;

/// cyGiProbeIntegrateHardware.metal, 50168 bytes.
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


#line 53 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
uint spWidth_0(KernelContext_0 thread* kernelContext_7)
{

#line 53
    return kernelContext_7->giResolve_0->constants_2[40U].x;
}


#line 143
struct SpTaps_0
{
    array<uint, int(4)> index_0;
    array<float, int(4)> weight_0;
    uint count_0;
};


#line 55
uint spSpacing_0(KernelContext_0 thread* kernelContext_8)
{

#line 55
    return kernelContext_8->giResolve_0->constants_2[40U].z;
}


#line 57
uint spTilesX_0(KernelContext_0 thread* kernelContext_9)
{

)cy_msl"
    R"cy_msl(#line 57
    return kernelContext_9->giResolve_0->constants_2[41U].x;
}


#line 58
uint spTilesY_0(KernelContext_0 thread* kernelContext_10)
{

#line 58
    return kernelContext_10->giResolve_0->constants_2[41U].y;
}


#line 149
void spAxisTaps_0(uint coordinate_0, uint step_0, uint count_1, uint thread* base_0, float thread* fraction_0)
{

#line 150
    float g_0 = (float(coordinate_0) - float(step_0 / 2U)) / float(step_0);
    if(g_0 <= 0.0)
    {

#line 152
        *base_0 = 0U;
        *fraction_0 = 0.0;
        return;
    }
    uint _S34 = uint(g_0);

#line 156
    *base_0 = _S34;
    *fraction_0 = g_0 - float(_S34);
    if((*base_0 + 1U) >= count_1)
    {

#line 159
        *base_0 = count_1 - 1U;
        *fraction_0 = 0.0;

#line 158
    }



    return;
}


#line 164
SpTaps_0 spBilinearTaps_0(uint x_3, uint y_3, uint step_1, uint countX_0, uint countY_0)
{

#line 165
    thread SpTaps_0 taps_0;
    (&taps_0)->count_0 = 0U;

#line 166
    uint t_2 = 0U;
    for(;;)
    {

#line 167
        if(t_2 < 4U)
        {
        }
        else
        {

#line 167
            break;
        }

#line 168
        (&taps_0)->index_0[t_2] = 0U;
        (&taps_0)->weight_0[t_2] = 0.0;

#line 167
        t_2 = t_2 + 1U;

#line 167
    }



    thread uint bx_0;

    thread float fx_0;

    spAxisTaps_0(x_3, step_1, countX_0, &bx_0, &fx_0);

#line 172
    thread uint by_0;

    thread float fy_0;

    spAxisTaps_0(y_3, step_1, countY_0, &by_0, &fy_0);

#line 176
    uint j_0 = 0U;
    for(;;)
    {

#line 177
        if(j_0 < 2U)
        {
        }
        else
        {

#line 177
            break;
        }

#line 177
        uint i_0 = 0U;
        for(;;)
        {

#line 178
            if(i_0 < 2U)
            {
            }
            else
            {

#line 178
                break;
            }

#line 178
            float _S35;
            if(i_0 == 0U)
            {

#line 179
                _S35 = 1.0 - fx_0;

#line 179
            }
            else
            {

#line 179
                _S35 = fx_0;

#line 179
            }

#line 179
            float _S36;

#line 179
            if(j_0 == 0U)
            {

#line 179
                _S36 = 1.0 - fy_0;

#line 179
            }
            else
            {

#line 179
                _S36 = fy_0;

#line 179
            }

#line 179
            float weight_1 = _S35 * _S36;
            if(weight_1 <= 0.0)
            {

#line 181
                i_0 = i_0 + 1U;

#line 178
                continue;
            }



            (&taps_0)->index_0[(&taps_0)->count_0] = (by_0 + j_0) * countX_0 + (bx_0 + i_0);
            (&taps_0)->weight_0[(&taps_0)->count_0] = weight_1;
            (&taps_0)->count_0 = (&taps_0)->count_0 + 1U;

#line 178
            i_0 = i_0 + 1U;

#line 178
        }

#line 177
        j_0 = j_0 + 1U;

#line 177
    }

#line 188
    return taps_0;
}


#line 56
uint spCell_0(KernelContext_0 thread* kernelContext_11)
{

#line 56
    return kernelContext_11->giResolve_0->constants_2[40U].w;
}

uint spCellsX_0(KernelContext_0 thread* kernelContext_12)
{

#line 59
    return kernelContext_12->giResolve_0->constants_2[41U].z;
}


#line 60
uint spCellsY_0(KernelContext_0 thread* kernelContext_13)
{

#line 60
    return kernelContext_13->giResolve_0->constants_2[41U].w;
}


#line 59
uint spCellsX_1(KernelContext_0 thread* kernelContext_14)
{

#line 59
    return kernelContext_14->giResolve_0->constants_2[41U].z;
}


#line 60
uint spCellsY_1(KernelContext_0 thread* kernelContext_15)
{

#line 60
    return kernelContext_15->giResolve_0->constants_2[41U].w;
}


#line 84
uint spTableIndex_0(uint slot_0, uint cell_2, KernelContext_0 thread* kernelContext_16)
{

#line 84
    uint _S37 = spCellsX_1(kernelContext_16);
    uint _S38 = slot_0 * _S37;

#line 85
    uint _S39 = spCellsY_1(kernelContext_16);

#line 85
    return 4U + _S38 * _S39 + cell_2;
}


#line 61
uint spUniformCount_0(KernelContext_0 thread* kernelContext_17)
{

#line 61
    return kernelContext_17->giResolve_0->constants_2[43U].w;
}


#line 211
struct SpProbe_0
{
    float3 position_3;
    float depth_0;
    float3 normal_5;
    uint id_0;
};


#line 61
uint spUniformCount_1(KernelContext_0 thread* kernelContext_18)
{

#line 61
    return kernelContext_18->giResolve_0->constants_2[43U].w;
}


#line 62
uint spCapacity_0(KernelContext_0 thread* kernelContext_19)
{

#line 62
    return kernelContext_19->giResolve_0->constants_2[42U].z;
}


)cy_msl"
    R"cy_msl(#line 63
uint spRecords_0(KernelContext_0 thread* kernelContext_20)
{

#line 63
    uint _S40 = spUniformCount_1(kernelContext_20);

#line 63
    uint _S41 = spCapacity_0(kernelContext_20);

#line 63
    return _S40 + _S41;
}


#line 80
uint spRecordBase_0(uint slot_1, uint record_1, KernelContext_0 thread* kernelContext_21)
{

#line 80
    uint _S42 = spRecords_0(kernelContext_21);
    return (slot_1 * _S42 + record_1) * 18U;
}


#line 218
SpProbe_0 spLoadProbe_0(uint slot_2, uint record_2, KernelContext_0 thread* kernelContext_22)
{

#line 218
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


#line 123
float spGeometryWeight_0(float3 probePosition_0, float3 probeNormal_0, float3 position_4, float3 normal_6, float depth_1, float tolerance_0)
{
    float plane_0 = abs(dot(probeNormal_0, position_4 - probePosition_0));
    float reach_1 = tolerance_0 * depth_1;
    if(reach_1 <= 0.0)
    {

#line 128
        return 0.0;
    }

    float _S46 = max(0.0, dot(probeNormal_0, normal_6));
    float facing_0 = _S46 * _S46;
    float facing_1 = facing_0 * facing_0;

    return max(0.0, 1.0 - plane_0 / reach_1) * (facing_1 * facing_1);
}


#line 231
void spLoadPacked_0(uint base_1, array<float3, int(9)> thread* coefficients_0, float thread* last_1, KernelContext_0 thread* kernelContext_23)
{

#line 232
    thread array<float, int(28)> values_0;

#line 232
    uint v_1 = 0U;
    for(;;)
    {

#line 233
        if(v_1 < 7U)
        {
        }
        else
        {

#line 233
            break;
        }

#line 234
        float4 device* _S47 = kernelContext_23->giResolve_0->probes_1+(base_1 + v_1);
        uint _S48 = v_1 * 4U;

#line 235
        values_0[_S48] = (*_S47).x;
        values_0[_S48 + 1U] = (*_S47).y;
        values_0[_S48 + 2U] = (*_S47).z;
        values_0[_S48 + 3U] = (*_S47).w;

#line 233
        v_1 = v_1 + 1U;

#line 233
    }

#line 233
    uint k_0 = 0U;

#line 240
    for(;;)
    {

#line 240
        if(k_0 < 9U)
        {
        }
        else
        {

#line 240
            break;
        }

#line 241
        uint _S49 = k_0 * 3U;

#line 241
        (*coefficients_0)[k_0] = float3(values_0[_S49], values_0[_S49 + 1U], values_0[_S49 + 2U]);

#line 240
        k_0 = k_0 + 1U;

#line 240
    }


    *last_1 = values_0[int(27)];
    return;
}


#line 269
void spLoadFiltered_0(uint slot_3, uint record_3, array<float3, int(9)> thread* coefficients_1, KernelContext_0 thread* kernelContext_24)
{

#line 269
    uint _S50 = spRecordBase_0(slot_3, record_3, kernelContext_24);
    thread float unused_0;

#line 270
    spLoadPacked_0(_S50 + 11U, coefficients_1, &unused_0, kernelContext_24);

    return;
}


#line 94
void spBasis_0(float3 d_0, array<float, int(9)> thread* basis_0)
{

#line 95
    (*basis_0)[int(0)] = 0.28209498524665833;
    float _S51 = d_0.y;

#line 96
    (*basis_0)[int(1)] = 0.48860299587249756 * _S51;
    float _S52 = d_0.z;

#line 97
    (*basis_0)[int(2)] = 0.48860299587249756 * _S52;
    float _S53 = d_0.x;

#line 98
    (*basis_0)[int(3)] = 0.48860299587249756 * _S53;
    (*basis_0)[int(4)] = 1.09254801273345947 * (_S53 * _S51);
    (*basis_0)[int(5)] = 1.09254801273345947 * (_S51 * _S52);
    (*basis_0)[int(6)] = 0.31539198756217957 * (3.0 * (_S52 * _S52) - 1.0);
    (*basis_0)[int(7)] = 1.09254801273345947 * (_S53 * _S52);
    (*basis_0)[int(8)] = 0.54627400636672974 * (_S53 * _S53 - _S51 * _S51);
    return;
}


#line 106
uint spBandOf_0(uint k_1)
{

#line 106
    uint _S54;
    if(k_1 == 0U)
    {

#line 107
        _S54 = 0U;

#line 107
    }
    else
    {

#line 107
        if(k_1 < 4U)
        {

#line 107
            _S54 = 1U;

#line 107
        }
        else
        {

#line 107
            _S54 = 2U;

#line 107
        }

#line 107
    }

#line 107
    return _S54;
}

float3 spEvaluate_0(const array<float3, int(9)> thread* coefficients_2, float3 direction_3, float3 band_0)
{

#line 111
    thread array<float, int(9)> basis_1;
    spBasis_0(direction_3, &basis_1);

#line 112
    float3 sum_0 = float3(0.0, 0.0, 0.0);

#line 112
    uint k_2 = 0U;

    for(;;)
    {

#line 114
        if(k_2 < 9U)
        {
        }
        else
        {

#line 114
            break;
        }

#line 115
        uint b_0 = spBandOf_0(k_2);

#line 115
        float scale_0;
        if(b_0 == 0U)
        {

#line 116
            scale_0 = band_0.x;

#line 116
        }
        else
        {

#line 116
            if(b_0 == 1U)
            {

#line 116
                scale_0 = band_0.y;

#line 116
            }
            else
            {

#line 116
                scale_0 = band_0.z;

#line 116
            }

#line 116
        }
        float3 _S55 = sum_0 + (*coefficients_2)[k_2] * float3((basis_1[k_2] * scale_0)) ;

#line 114
        uint k_3 = k_2 + 1U;

#line 114
        sum_0 = _S55;

#line 114
        k_2 = k_3;

#line 114
    }

#line 119
    return float3(max(sum_0.x, 0.0), max(sum_0.y, 0.0), max(sum_0.z, 0.0));
}

)cy_msl"
    R"cy_msl(
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
    uint _S56 = sample_3->source_0;
    if((sample_3->source_0) == 0U)
    {

#line 149
        return;
    }
    if((excluded_0 & (1U << _S56)) != 0U)
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
float grFarFieldWeight_0(float distance_0, KernelContext_0 thread* kernelContext_25)
{

#line 169
    float4 far_0 = (as_type<float4>((kernelContext_25->giResolve_0->constants_2[3U])));
    float begin_0 = far_0.x;
    if(begin_0 <= 0.0)
    {

#line 172
        return 1.0;
    }
    float _S57 = far_0.y;

#line 174
    float ratio_0;

#line 174
    if(_S57 > 0.0)
    {

#line 174
        ratio_0 = far_0.z / _S57;

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
    float _S58 = state_2->totalWeight_0;

#line 163
    float3 _S59;
    if((state_2->totalWeight_0) > 0.0)
    {

#line 164
        _S59 = state_2->total_0 / float3(_S58) ;

#line 164
    }
    else
    {

#line 164
        _S59 = float3(0.0, 0.0, 0.0);

#line 164
    }

#line 164
    return _S59;
}


#line 200
uint grReflectionRays_0(float roughness_0, KernelContext_0 thread* kernelContext_26)
{

#line 201
    uint4 words_0 = kernelContext_26->giResolve_0->constants_2[4U];
    float mirror_0 = (as_type<float>((words_0.x)));
    float cacheMin_0 = (as_type<float>((words_0.y)));
    uint _S60 = max(1U, words_0.z);
    if(roughness_0 <= mirror_0)
    {

#line 206
        return _S60;
    }
    if(roughness_0 < cacheMin_0)
    {

        float _S61 = float(_S60);


        return max(1U, uint(floor(_S61 + (1.0 - _S61) * gpClamp_0((roughness_0 - mirror_0) / max(cacheMin_0 - mirror_0, 0.00009999999747379), 0.0, 1.0) + 0.5)));
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
    bool _S62;
    if(count_2 <= 1U)
    {

#line 223
        _S62 = true;

#line 223
    }
    else
    {

#line 223
        _S62 = roughness_1 <= 0.0;

#line 223
    }

#line 223
    if(_S62)
    {

#line 224
        return axis_0;
    }

    float _S63 = float(index_1);

    float radius_1 = sqrt((_S63 + 0.5) / float(count_2)) * (gpClamp_0(roughness_1, 0.0, 1.0) * 0.69999998807907104);
    float angle_0 = 2.3999631404876709 * _S63 + float(rotation_0) * 0.6180340051651001;

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
    float _S64 = dot(unit_1, normal_7);

#line 238
    if(_S64 < 0.0)
    {

#line 238
        tangent_0 = giNormalisedOr_0(unit_1 - normal_7 * float3((2.0 * _S64)) , axis_0);

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
    uint _S65 = standIn_0->source_0;

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

)cy_msl"
    R"cy_msl(#line 264
    }

#line 264
    if(hasStandIn_0)
    {

#line 264
        hasStandIn_0 = (excluded_1 & (1U << _S65)) == 0U;

#line 264
    }
    else
    {

#line 264
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
        (&sample_4)->source_0 = _S65;
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


#line 793 "src/rendering/gi_gpu/shaders/gi_screen_probes_common.slang"
void spIntegrate_0(uint pixel_2, KernelContext_0 thread* kernelContext_27)
{

#line 794
    uint4 counts_0 = kernelContext_27->giResolve_0->constants_2[0U];
    uint _S66 = counts_0.z;

#line 795
    if(pixel_2 >= _S66)
    {

#line 796
        return;
    }
    uint _S67 = counts_0.w;

#line 798
    uint record_4 = (_S67 * _S66 + pixel_2) * 2U;
    float4 at_0 = kernelContext_27->giResolve_0->surfaces_0[record_4];
    float4 facing_2 = kernelContext_27->giResolve_0->surfaces_0[record_4 + 1U];
    float _S68 = at_0.w;

#line 801
    if(_S68 <= 0.0)
    {

#line 802
        uint _S69 = pixel_2 * 2U;

#line 802
        float4 _S70 = float4(0.0, 0.0, 0.0, 0.0);

#line 802
        *(kernelContext_27->giResolve_0->noisy_0+_S69) = _S70;
        *(kernelContext_27->giResolve_0->noisy_0+(_S69 + 1U)) = _S70;
        return;
    }

    uint frame_2 = kernelContext_27->giResolve_0->constants_2[18U].x;
    float3 position_5 = at_0.xyz;
    float3 normal_8 = giNormalisedOr_0(facing_2.xyz, float3(0.0, 1.0, 0.0));
    float roughness_2 = facing_2.w;
    float3 offset_1 = position_5 - (as_type<float3>((kernelContext_27->giResolve_0->constants_2[1U].xyz)));
    float3 _S71 = float3(0.0, 0.0, -1.0);

#line 812
    float3 incident_0 = giNormalisedOr_0(offset_1, _S71);

    float3 reflection_1 = giNormalisedOr_0(incident_0 - normal_8 * float3((2.0 * dot(incident_0, normal_8))) , normal_8);
    float rough_0 = gpClamp_0(roughness_2, 0.0, 1.0);
    float3 specularBand_0 = float3(1.0, 1.0 + -0.33333331346511841 * rough_0, 1.0 + -0.75 * rough_0);

    float3 irradianceBand_0 = float3(1.0, 0.66666668653488159, 0.25);
    float4 _S72 = (as_type<float4>((kernelContext_27->giResolve_0->constants_2[43U])));

#line 819
    uint _S73 = spWidth_0(kernelContext_27);
    uint _S74 = pixel_2 % _S73;

#line 820
    uint _S75 = spWidth_0(kernelContext_27);
    uint _S76 = pixel_2 / _S75;

    float3 _S77 = float3(0.0, 0.0, 0.0);

#line 823
    float nearestPlane_0 = 1.00000001504746622e+30;

#line 823
    float3 irradiance_0 = _S77;

#line 823
    float3 glossy_0 = _S77;

#line 823
    float3 cache_0 = _S77;

#line 823
    uint nearest_0 = 4294967295U;

#line 823
    uint pass_0 = 0U;

#line 823
    float cacheConfidence_0 = 0.0;

#line 823
    float openness_0 = 0.0;

#line 823
    float total_1 = 0.0;

#line 831
    for(;;)
    {

#line 831
        if(pass_0 < 2U)
        {
        }
        else
        {

#line 831
            break;
        }

#line 831
        SpTaps_0 _S78;

        if(pass_0 == 0U)
        {

#line 833
            uint _S79 = spSpacing_0(kernelContext_27);

#line 833
            uint _S80 = spTilesX_0(kernelContext_27);

#line 833
            uint _S81 = spTilesY_0(kernelContext_27);

#line 833
            _S78 = spBilinearTaps_0(_S74, _S76, _S79, _S80, _S81);

#line 833
        }
        else
        {

#line 833
            uint _S82 = spCell_0(kernelContext_27);

#line 833
            uint _S83 = spCellsX_0(kernelContext_27);

#line 833
            uint _S84 = spCellsY_0(kernelContext_27);

#line 833
            _S78 = spBilinearTaps_0(_S74, _S76, _S82, _S83, _S84);

#line 833
        }

#line 833
        float nearestPlane_1 = nearestPlane_0;

#line 833
        float3 irradiance_1 = irradiance_0;

#line 833
        float3 glossy_1 = glossy_0;

#line 833
        float3 cache_1 = cache_0;

#line 833
        uint nearest_1 = nearest_0;

#line 833
        uint tap_0 = 0U;

#line 833
        float cacheConfidence_1 = cacheConfidence_0;

#line 833
        float openness_1 = openness_0;

#line 833
        float total_2 = total_1;

        for(;;)
        {

#line 835
            if(tap_0 < (_S78.count_0))
            {
            }
            else
            {

#line 835
                break;
            }

#line 833
            SpTaps_0 _S85 = _S78;

            uint _S86 = tap_0;

#line 835
            uint index_2;

            if(pass_0 == 1U)
            {

#line 837
                uint _S87 = spTableIndex_0(_S67, _S85.index_0[_S86], kernelContext_27);
                uint device* _S88 = kernelContext_27->giResolve_0->slots_0+_S87;

#line 838
                uint taken_0 = *_S88;
                if((*_S88) == 4294967295U)
                {

#line 840
                    tap_0 = tap_0 + 1U;

#line 835
                    continue;
                }

#line 835
                uint _S89 = spUniformCount_0(kernelContext_27);

#line 835
                index_2 = _S89 + taken_0;

            }
            else
            {

#line 837
                index_2 = _S85.index_0[_S86];

#line 837
            }

#line 837
            SpProbe_0 _S90 = spLoadProbe_0(_S67, index_2, kernelContext_27);

#line 845
            if((_S90.depth_0) <= 0.0)
            {

#line 846
                tap_0 = tap_0 + 1U;

#line 835
                continue;
            }

)cy_msl"
    R"cy_msl(#line 848
            float plane_1 = abs(dot(_S90.normal_5, position_5 - _S90.position_3));

#line 848
            float nearestPlane_2;

#line 848
            uint nearest_2;
            if(plane_1 < nearestPlane_1)
            {

#line 849
                nearestPlane_2 = plane_1;

#line 849
                nearest_2 = index_2;

#line 849
            }
            else
            {

#line 849
                nearestPlane_2 = nearestPlane_1;

#line 849
                nearest_2 = nearest_1;

#line 849
            }

#line 854
            float weight_3 = _S78.weight_0[tap_0] * spGeometryWeight_0(_S90.position_3, _S90.normal_5, position_5, normal_8, _S68, _S72.y);

            if(weight_3 <= 0.0)
            {

#line 856
                nearestPlane_1 = nearestPlane_2;

#line 856
                nearest_1 = nearest_2;
                tap_0 = tap_0 + 1U;

#line 835
                continue;
            }

#line 859
            thread array<float3, int(9)> coefficients_3;

#line 859
            spLoadFiltered_0(_S67, index_2, &coefficients_3, kernelContext_27);

#line 859
            uint _S91 = spRecordBase_0(_S67, index_2, kernelContext_27);


            float4 device* _S92 = kernelContext_27->giResolve_0->probes_1+(_S91 + 9U);

#line 862
            thread array<float3, int(9)> _S93 = coefficients_3;

#line 862
            float3 _S94 = spEvaluate_0(&_S93, normal_8, irradianceBand_0);

#line 862
            float3 _S95 = float3(weight_3) ;
            float3 _S96 = irradiance_1 + _S94 * _S95;

#line 863
            thread array<float3, int(9)> _S97 = coefficients_3;

#line 863
            float3 _S98 = spEvaluate_0(&_S97, reflection_1, specularBand_0);
            float3 _S99 = glossy_1 + _S98 * _S95;
            float3 _S100 = cache_1 + (*_S92).xyz * _S95;
            float cacheConfidence_2 = cacheConfidence_1 + (*_S92).w * weight_3;
            float openness_2 = openness_1 + (*(kernelContext_27->giResolve_0->probes_1+(_S91 + 10U))).x * weight_3;
            float total_3 = total_2 + weight_3;

#line 868
            nearestPlane_1 = nearestPlane_2;

#line 868
            irradiance_1 = _S96;

#line 868
            glossy_1 = _S99;

#line 868
            cache_1 = _S100;

#line 868
            nearest_1 = nearest_2;

#line 868
            cacheConfidence_1 = cacheConfidence_2;

#line 868
            openness_1 = openness_2;

#line 868
            total_2 = total_3;

#line 835
            tap_0 = tap_0 + 1U;

#line 835
        }

#line 831
        uint pass_1 = pass_0 + 1U;

#line 831
        nearestPlane_0 = nearestPlane_1;

#line 831
        irradiance_0 = irradiance_1;

#line 831
        glossy_0 = glossy_1;

#line 831
        cache_0 = cache_1;

#line 831
        nearest_0 = nearest_1;

#line 831
        pass_0 = pass_1;

#line 831
        cacheConfidence_0 = cacheConfidence_1;

#line 831
        openness_0 = openness_1;

#line 831
        total_1 = total_2;

#line 831
    }

#line 871
    if(total_1 < 0.00009999999747379)
    {

#line 872
        if(nearest_0 == 4294967295U)
        {
            uint _S101 = pixel_2 * 2U;

#line 874
            float4 _S102 = float4(0.0, 0.0, 0.0, (as_type<float>((kernelContext_27->giResolve_0->constants_2[44U].x))));

#line 874
            *(kernelContext_27->giResolve_0->noisy_0+_S101) = _S102;
            *(kernelContext_27->giResolve_0->noisy_0+(_S101 + 1U)) = _S102;
            return;
        }
        thread array<float3, int(9)> coefficients_4;

#line 878
        spLoadFiltered_0(_S67, nearest_0, &coefficients_4, kernelContext_27);

#line 878
        uint _S103 = spRecordBase_0(_S67, nearest_0, kernelContext_27);

#line 878
        thread array<float3, int(9)> _S104 = coefficients_4;

#line 878
        float3 _S105 = spEvaluate_0(&_S104, normal_8, irradianceBand_0);

#line 878
        thread array<float3, int(9)> _S106 = coefficients_4;

#line 878
        float3 _S107 = spEvaluate_0(&_S106, reflection_1, specularBand_0);

#line 883
        uint _S108 = _S103 + 9U;
        float _S109 = (*(kernelContext_27->giResolve_0->probes_1+_S108)).w;
        float _S110 = (*(kernelContext_27->giResolve_0->probes_1+(_S103 + 10U))).x;

#line 885
        cache_0 = (*(kernelContext_27->giResolve_0->probes_1+_S108)).xyz;

#line 885
        irradiance_0 = _S105;

#line 885
        glossy_0 = _S107;

#line 885
        cacheConfidence_0 = _S109;

#line 885
        openness_0 = _S110;

#line 871
    }
    else
    {

#line 887
        float3 _S111 = irradiance_0 / float3(total_1) ;
        float3 _S112 = glossy_0 / float3(total_1) ;

        float cacheConfidence_3 = cacheConfidence_0 / total_1;
        float openness_3 = openness_0 / total_1;

#line 891
        cache_0 = cache_0 / float3(total_1) ;

#line 891
        irradiance_0 = _S111;

#line 891
        glossy_0 = _S112;

#line 891
        cacheConfidence_0 = cacheConfidence_3;

#line 891
        openness_0 = openness_3;

#line 871
    }

#line 894
    uint4 shape_0 = kernelContext_27->giResolve_0->constants_2[42U];
    uint excluded_2 = kernelContext_27->giResolve_0->constants_2[2U].w;
    GrCombine_0 _S113 = grCombineBegin_0();

#line 896
    thread GrCombine_0 state_3 = _S113;
    if(((shape_0.w) & 1U) != 0U)
    {

#line 897
        thread GrSample_0 _S114 = grSample_0(cache_0, cacheConfidence_0, 3U);

#line 897
        grCombineAdd_0(&state_3, &_S114, excluded_2);

#line 897
    }


    uint _S115 = GiHardwareTier_source_0();

#line 900
    thread GrSample_0 _S116 = grSample_0(irradiance_0, 1.0, _S115);

#line 900
    grCombineAdd_0(&state_3, &_S116, excluded_2);
    thread GrSample_0 sky_0 = grSample_0(_S77, 0.0, 9U);

#line 901
    float _S117 = grFarFieldWeight_0(length(offset_1), kernelContext_27);

    if(_S117 > 0.0)
    {

#line 903
        float3 _S118 = gpSky_0(normal_8, kernelContext_27);
        (&sky_0)->radiance_0 = _S118 * float3(openness_0) ;
        (&sky_0)->confidence_0 = gpClamp_0(_S117 * openness_0, 0.0, 1.0);

#line 903
    }

#line 903
    thread GrSample_0 _S119 = sky_0;

#line 903
    grCombineAdd_0(&state_3, &_S119, excluded_2);

#line 903
    thread GrCombine_0 _S120 = state_3;

#line 903
    float3 _S121 = grCombineEnd_0(&_S120);

#line 908
    float3 diffuse_0 = _S121 * float3((as_type<float>((kernelContext_27->giResolve_0->constants_2[5U].x)))) ;

    GrSample_0 probeSpecular_0 = grSample_0(glossy_0, gpClamp_0(roughness_2, 0.15000000596046448, 1.0), _S115);
    thread GrCombine_0 reflected_0 = _S113;

#line 911
    uint _S122 = grReflectionRays_0(roughness_2, kernelContext_27);

    float density_1 = (as_type<float>((kernelContext_27->giResolve_0->constants_2[1U].w)));

#line 913
    bool _S123;
    if(_S122 > 0U)
    {

)cy_msl"
    R"cy_msl(#line 914
        _S123 = grTraces_0(pixel_2, frame_2, density_1);

#line 914
    }
    else
    {

#line 914
        _S123 = false;

#line 914
    }

#line 914
    if(_S123)
    {

#line 915
        float4 rays_1 = (as_type<float4>((kernelContext_27->giResolve_0->constants_2[2U])));


        uint _S124 = grHash_0(pixel_2, frame_2, 7U) % _S122;

#line 918
        GrSample_0 _S125 = grWorldRay_0(position_5, normal_8, giNormalisedOr_0(grLobeDirection_0(reflection_1, normal_8, roughness_2, _S124, _S122, frame_2), _S71), rays_1.y, rays_1.x, kernelContext_27);

#line 918
        thread GrSample_0 _S126 = _S125;

#line 918
        thread GrSample_0 _S127 = probeSpecular_0;

#line 918
        GrSample_0 _S128 = grGatherSample_0(&_S126, &_S127, 0U);

#line 918
        thread GrSample_0 _S129 = _S128;

#line 918
        grCombineAdd_0(&reflected_0, &_S129, 0U);

#line 914
    }

#line 914
    thread GrSample_0 _S130 = probeSpecular_0;

#line 914
    grCombineAdd_0(&reflected_0, &_S130, 0U);

#line 914
    thread GrCombine_0 _S131 = reflected_0;

#line 914
    float3 _S132 = grCombineEnd_0(&_S131);

#line 926
    float3 specular_0 = _S132 * float3((as_type<float>((kernelContext_27->giResolve_0->constants_2[5U].y)))) ;
    float samples_1 = (as_type<float>((kernelContext_27->giResolve_0->constants_2[44U].x)));
    uint _S133 = pixel_2 * 2U;

#line 928
    *(kernelContext_27->giResolve_0->noisy_0+_S133) = float4(diffuse_0, samples_1);
    *(kernelContext_27->giResolve_0->noisy_0+(_S133 + 1U)) = float4(specular_0, samples_1);
    return;
}


#line 23 "src/rendering/gi_gpu/shaders/gi_screen_probes_hardware.slang"
[[kernel]] void cyGiProbeIntegrateHardware(uint3 thread_0 [[thread_position_in_grid]], GiProbeSet_default_0 constant* giProbes_1 [[buffer(2)]], GiSceneSet_default_0 constant* giScene_1 [[buffer(0)]], GiResolveSet_default_0 constant* giResolve_1 [[buffer(3)]], GiHardwareSet_default_0 constant* giHardware_1 [[buffer(4)]])
{

#line 23
    thread KernelContext_0 kernelContext_28;

#line 23
    (&kernelContext_28)->giProbes_0 = giProbes_1;

#line 23
    (&kernelContext_28)->giScene_0 = giScene_1;

#line 23
    (&kernelContext_28)->giResolve_0 = giResolve_1;

#line 23
    (&kernelContext_28)->giHardware_0 = giHardware_1;

#line 23
    spIntegrate_0(thread_0.x, &kernelContext_28);

    return;
}

)cy_msl";

}  // namespace cy::rendering::gi_gpu

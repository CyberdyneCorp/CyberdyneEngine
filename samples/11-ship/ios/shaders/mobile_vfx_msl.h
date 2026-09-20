#pragma once
// Compiled MSL for the iOS spark-plume simulation kernel. GENERATED — do not edit by hand.

namespace cy::vfx::gpu {

inline constexpr char kMobileVfxMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 74 "vfx-dispatch.slang"
struct CyVfxInput_0
{
    float dt_0;
    float emitter_age_0;
    float particle_index_0;
    float spawn_index_0;
    float normalised_age_0;
    uint pass_0;
    uint capacity_0;
    uint spawn_scale_fixed_0;
};


#line 30
struct CyVfxEvent_0
{
    uint source_0;
    uint depth_0;
    float rank_0;
    float payload_0;
};


#line 36
struct CyVfxSet_default_0
{
    uint device* particles_0;
    uint device* alive_0;
    uint device* indices_0;
    uint device* counts_0;
    uint device* free_list_0;
    uint device* parameter_words_0;
    uint device* keys_0;
    CyVfxEvent_0 device* events_0;
    uint device* args_0;
};


#line 150
struct CyVfxParams_0
{
    float intensity_0;
    float3 gravity_0;
};


#line 150
struct KernelContext_0
{
    CyVfxInput_0 constant* cyVfxPush_0;
    CyVfxSet_default_0 constant* cyVfxSet_0;
    CyVfxInput_0 cyVfxInput_0;
    CyVfxParams_0 cyVfxParams_0;
};


#line 155
void cyVfxLoadParams_0(KernelContext_0 thread* kernelContext_0)
{

#line 156
    (&kernelContext_0->cyVfxParams_0)->intensity_0 = (as_type<float>((*(kernelContext_0->cyVfxSet_0->parameter_words_0+0U))));
    (&kernelContext_0->cyVfxParams_0)->gravity_0 = float3((as_type<float>((*(kernelContext_0->cyVfxSet_0->parameter_words_0+4U)))), (as_type<float>((*(kernelContext_0->cyVfxSet_0->parameter_words_0+5U)))), (as_type<float>((*(kernelContext_0->cyVfxSet_0->parameter_words_0+6U)))));
    return;
}


#line 285
void cyVfxKernel_sparks0_1_0(uint particle_0, KernelContext_0 thread* kernelContext_1)
{

    *(kernelContext_1->cyVfxSet_0->counts_0+2U) = uint(max((&kernelContext_1->cyVfxParams_0)->intensity_0 * 16.0, 0.0));
    return;
}


#line 100
float cyVfxRandom_0(uint particle_1, uint stream_0)
{

#line 101
    uint state_0 = particle_1 * 747796405U + stream_0 * 2891336453U + 1U;

    uint state_1 = (state_0 ^ (state_0 >> 16U)) * 2246822519U;

    uint state_2 = (state_1 ^ (state_1 >> 13U)) * 3266489917U;

    return float((state_2 ^ (state_2 >> 16U)) >> 8U) * 5.9604644775390625e-08;
}


#line 20
void cyVfxPackLane_0(uint thread* word_0, uint lane_0, uint bits_0, uint width_0)
{

#line 21
    if(width_0 >= 32U)
    {

#line 21
        *word_0 = bits_0;

#line 21
        return;
    }

#line 22
    uint shift_0 = lane_0 * width_0;
    uint mask_0 = ((1U << width_0) - 1U) << shift_0;
    *word_0 = ((*word_0) & (~mask_0)) | ((bits_0 << shift_0) & mask_0);
    return;
}


#line 169
void cyVfxStore_position_0(uint particle_2, float3 value_0, KernelContext_0 thread* kernelContext_2)
{

#line 170
    uint base_0 = particle_2 * 3U;
    thread uint w0_0 = 0U;
    thread uint w1_0 = 0U;
    thread uint w2_0 = 0U;
    cyVfxPackLane_0(&w0_0, 0U, (as_type<uint>((value_0[int(0)]))), 32U);
    cyVfxPackLane_0(&w1_0, 0U, (as_type<uint>((value_0[int(1)]))), 32U);
    cyVfxPackLane_0(&w2_0, 0U, (as_type<uint>((value_0[int(2)]))), 32U);
    *(kernelContext_2->cyVfxSet_0->particles_0+base_0) = w0_0;
    *(kernelContext_2->cyVfxSet_0->particles_0+(base_0 + 1U)) = w1_0;
    *(kernelContext_2->cyVfxSet_0->particles_0+(base_0 + 2U)) = w2_0;
    return;
}


#line 188
void cyVfxStore_velocity_0(uint particle_3, float3 value_1, KernelContext_0 thread* kernelContext_3)
{

#line 189
    uint base_1 = particle_3 * 3U;
    thread uint w0_1 = 0U;
    thread uint w1_1 = 0U;
    thread uint w2_1 = 0U;
    cyVfxPackLane_0(&w0_1, 0U, (as_type<uint>((value_1[int(0)]))), 32U);
    cyVfxPackLane_0(&w1_1, 0U, (as_type<uint>((value_1[int(1)]))), 32U);
    cyVfxPackLane_0(&w2_1, 0U, (as_type<uint>((value_1[int(2)]))), 32U);
    uint _S1 = 1536U + base_1;

#line 196
    *(kernelContext_3->cyVfxSet_0->particles_0+_S1) = w0_1;
    *(kernelContext_3->cyVfxSet_0->particles_0+(_S1 + 1U)) = w1_1;
    *(kernelContext_3->cyVfxSet_0->particles_0+(_S1 + 2U)) = w2_1;
    return;
}


#line 207
void cyVfxStore_age_0(uint particle_4, float value_2, KernelContext_0 thread* kernelContext_4)
{
    thread uint w0_2 = 0U;
    cyVfxPackLane_0(&w0_2, 0U, (as_type<uint>((value_2))), 32U);
    *(kernelContext_4->cyVfxSet_0->particles_0+(3072U + particle_4)) = w0_2;
    return;
}


#line 220
void cyVfxStore_lifetime_0(uint particle_5, float value_3, KernelContext_0 thread* kernelContext_5)
{
    thread uint w0_3 = 0U;
    cyVfxPackLane_0(&w0_3, 0U, (as_type<uint>((value_3))), 32U);
    *(kernelContext_5->cyVfxSet_0->particles_0+(3584U + particle_5)) = w0_3;
    return;
}


#line 233
void cyVfxStore_size_0(uint particle_6, float value_4, KernelContext_0 thread* kernelContext_6)
{
    thread uint w0_4 = 0U;
    cyVfxPackLane_0(&w0_4, 0U, (as_type<uint>((value_4))), 32U);
    *(kernelContext_6->cyVfxSet_0->particles_0+(4096U + particle_6)) = w0_4;
    return;
}


#line 18
uint cyVfxPackUnorm8_0(float value_5)
{

#line 18
    return uint(saturate(value_5) * 255.0 + 0.5);
}


#line 246
void cyVfxStore_color_0(uint particle_7, float4 value_6, KernelContext_0 thread* kernelContext_7)
{
    thread uint w0_5 = 0U;
    cyVfxPackLane_0(&w0_5, 0U, cyVfxPackUnorm8_0(value_6[int(0)]), 8U);
    cyVfxPackLane_0(&w0_5, 1U, cyVfxPackUnorm8_0(value_6[int(1)]), 8U);
    cyVfxPackLane_0(&w0_5, 2U, cyVfxPackUnorm8_0(value_6[int(2)]), 8U);
    cyVfxPackLane_0(&w0_5, 3U, cyVfxPackUnorm8_0(value_6[int(3)]), 8U);
    *(kernelContext_7->cyVfxSet_0->particles_0+(4608U + particle_7)) = w0_5;
    return;
}


#line 262
void cyVfxStore_emission_0(uint particle_8, float value_7, KernelContext_0 thread* kernelContext_8)
{
    thread uint w0_6 = 0U;
    cyVfxPackLane_0(&w0_6, 0U, (as_type<uint>((value_7))), 32U);
    *(kernelContext_8->cyVfxSet_0->particles_0+(5120U + particle_8)) = w0_6;
    return;
}



void cyVfxRaise_collision_0(uint particle_9, float rank_1, KernelContext_0 thread* kernelContext_9)
{
    uint slot_0 = atomic_fetch_add_explicit(((atomic_uint device*)(kernelContext_9->cyVfxSet_0->counts_0+8U)), 1U, memory_order_relaxed);

#line 274
    bool _S2;
    if(slot_0 >= 64U)
    {

#line 275
        _S2 = true;

#line 275
    }
    else
    {

#line 275
        _S2 = slot_0 >= 256U;

#line 275
    }

#line 275
    if(_S2)
    {

#line 275
        return;
    }

#line 276
    thread CyVfxEvent_0 raised_0;
    (&raised_0)->source_0 = particle_9;
    (&raised_0)->depth_0 = 0U;
    (&raised_0)->rank_0 = rank_1;
    (&raised_0)->payload_0 = rank_1;
    *(kernelContext_9->cyVfxSet_0->events_0+slot_0) = raised_0;
    return;
}


#line 140
void cyVfxKill_0(uint particle_10, KernelContext_0 thread* kernelContext_10)
{

#line 141
    if((*(kernelContext_10->cyVfxSet_0->alive_0+particle_10)) != 0U)
    {
        uint _S3 = atomic_fetch_add_explicit(((atomic_uint device*)(kernelContext_10->cyVfxSet_0->counts_0+5U)), 1U, memory_order_relaxed);
        uint _S4 = atomic_fetch_add_explicit(((atomic_uint device*)(kernelContext_10->cyVfxSet_0->counts_0+6U)), 4294967295U, memory_order_relaxed);

#line 141
    }

#line 146
    *(kernelContext_10->cyVfxSet_0->alive_0+particle_10) = 0U;
    return;
}


#line 292
void cyVfxKernel_sparks0_6_0(uint particle_11, KernelContext_0 thread* kernelContext_11)
{

#line 305
    float3 v10_0 = float3((2.0 * cyVfxRandom_0(particle_11, 22U) - 1.0) * 0.62000000476837158, 2.59999990463256836 + cyVfxRandom_0(particle_11, 17U) * 3.59999990463256836, 0.62000000476837158 * (2.0 * cyVfxRandom_0(particle_11, 26U) - 1.0)) + float3((&kernelContext_11->cyVfxInput_0)->dt_0)  * float3(0.0, -9.81000041961669922, 0.0);

#line 314
    float3 v19_0 = float3(2.0 * cyVfxRandom_0(particle_11, 4U) - 1.0, 2.0 * cyVfxRandom_0(particle_11, 7U) - 1.0, 2.0 * cyVfxRandom_0(particle_11, 10U) - 1.0) * float3(0.15999999642372131) ;


    float v22_0 = cyVfxRandom_0(particle_11, 35U) * 0.89999997615814209 + 0.55000001192092896;

    float v24_0 = saturate((&kernelContext_11->cyVfxInput_0)->dt_0 / v22_0);
    float v25_0 = mix(0.2199999988079071, 0.01999999955296516, v24_0);
    float4 v26_0 = float4(1.0, 0.41999998688697815, 0.11999999731779099, 0.89999997615814209);
    float v27_0 = mix(14000.0, 300.0, v24_0);
    float v28_0 = dot(v19_0, float3(0.0, 1.0, 0.0));
    bool v29_0 = ((&kernelContext_11->cyVfxInput_0)->dt_0) > v22_0;

#line 324
    cyVfxStore_position_0(particle_11, float3((&kernelContext_11->cyVfxInput_0)->dt_0)  * v10_0 + v19_0, kernelContext_11);

#line 324
    cyVfxStore_velocity_0(particle_11, v10_0, kernelContext_11);

#line 324
    cyVfxStore_age_0(particle_11, (&kernelContext_11->cyVfxInput_0)->dt_0, kernelContext_11);

#line 324
    cyVfxStore_lifetime_0(particle_11, v22_0, kernelContext_11);

#line 324
    cyVfxStore_size_0(particle_11, v25_0, kernelContext_11);

#line 324
    cyVfxStore_color_0(particle_11, v26_0, kernelContext_11);

#line 324
    cyVfxStore_emission_0(particle_11, v27_0, kernelContext_11);

#line 333
    if(v28_0 != 0.0)
    {

#line 333
        cyVfxRaise_collision_0(particle_11, v28_0, kernelContext_11);

#line 333
    }
    if(v29_0)
    {

#line 334
        cyVfxKill_0(particle_11, kernelContext_11);

#line 334
    }
    return;
}


#line 7
float cyVfxUnpackF32_0(uint word_1)
{

#line 7
    return (as_type<float>((word_1)));
}


#line 165
float3 cyVfxLoad_position_0(uint particle_12, KernelContext_0 thread* kernelContext_12)
{

#line 166
    uint base_2 = particle_12 * 3U;
    return float3(cyVfxUnpackF32_0(*(kernelContext_12->cyVfxSet_0->particles_0+base_2)), cyVfxUnpackF32_0(*(kernelContext_12->cyVfxSet_0->particles_0+(base_2 + 1U))), cyVfxUnpackF32_0(*(kernelContext_12->cyVfxSet_0->particles_0+(base_2 + 2U))));
}


#line 184
float3 cyVfxLoad_velocity_0(uint particle_13, KernelContext_0 thread* kernelContext_13)
{
    uint _S5 = 1536U + particle_13 * 3U;

#line 186
    return float3(cyVfxUnpackF32_0(*(kernelContext_13->cyVfxSet_0->particles_0+_S5)), cyVfxUnpackF32_0(*(kernelContext_13->cyVfxSet_0->particles_0+(_S5 + 1U))), cyVfxUnpackF32_0(*(kernelContext_13->cyVfxSet_0->particles_0+(_S5 + 2U))));
}


#line 203
float cyVfxLoad_age_0(uint particle_14, KernelContext_0 thread* kernelContext_14)
{
    return cyVfxUnpackF32_0(*(kernelContext_14->cyVfxSet_0->particles_0+(3072U + particle_14)));
}


#line 216
float cyVfxLoad_lifetime_0(uint particle_15, KernelContext_0 thread* kernelContext_15)
{
    return cyVfxUnpackF32_0(*(kernelContext_15->cyVfxSet_0->particles_0+(3584U + particle_15)));
}


#line 338
void cyVfxKernel_sparks0_4_0(uint particle_16, KernelContext_0 thread* kernelContext_16)
{
    float3 v0_0 = float3((&kernelContext_16->cyVfxInput_0)->dt_0)  * float3(0.0, -9.81000041961669922, 0.0);

#line 340
    float3 _S6 = cyVfxLoad_velocity_0(particle_16, kernelContext_16);

    float3 v1_0 = v0_0 + _S6;
    float3 v2_0 = float3((&kernelContext_16->cyVfxInput_0)->dt_0)  * v1_0;

#line 343
    float3 _S7 = cyVfxLoad_position_0(particle_16, kernelContext_16);
    float3 v3_0 = v2_0 + _S7;
    float _S8 = (&kernelContext_16->cyVfxInput_0)->dt_0;

#line 345
    float _S9 = cyVfxLoad_age_0(particle_16, kernelContext_16);

#line 345
    float v4_0 = _S8 + _S9;

#line 345
    float _S10 = cyVfxLoad_lifetime_0(particle_16, kernelContext_16);

    float v6_0 = saturate(v4_0 / _S10);
    float v7_0 = mix(0.2199999988079071, 0.01999999955296516, v6_0);
    float v8_0 = mix(14000.0, 300.0, v6_0);
    float3 _S11 = float3(0.0, 1.0, 0.0);

#line 350
    float3 _S12 = cyVfxLoad_position_0(particle_16, kernelContext_16);

#line 350
    float v9_0 = dot(_S11, _S12);

#line 350
    float _S13 = cyVfxLoad_lifetime_0(particle_16, kernelContext_16);
    bool v10_1 = v4_0 > _S13;

#line 351
    cyVfxStore_velocity_0(particle_16, v1_0, kernelContext_16);

#line 351
    cyVfxStore_position_0(particle_16, v3_0, kernelContext_16);

#line 351
    cyVfxStore_age_0(particle_16, v4_0, kernelContext_16);

#line 351
    cyVfxStore_size_0(particle_16, v7_0, kernelContext_16);

#line 351
    cyVfxStore_emission_0(particle_16, v8_0, kernelContext_16);

#line 357
    if(v9_0 != 0.0)
    {

#line 357
        cyVfxRaise_collision_0(particle_16, v9_0, kernelContext_16);

#line 357
    }
    if(v10_1)
    {

#line 358
        cyVfxKill_0(particle_16, kernelContext_16);

#line 358
    }
    return;
}


#line 367
[[kernel]] void cyVfxKernel(uint3 thread_0 [[thread_position_in_grid]], CyVfxInput_0 constant* cyVfxPush_1 [[buffer(1)]], CyVfxSet_default_0 constant* cyVfxSet_1 [[buffer(0)]])
{

#line 367
    thread KernelContext_0 kernelContext_17;

#line 367
    (&kernelContext_17)->cyVfxPush_0 = cyVfxPush_1;

#line 367
    (&kernelContext_17)->cyVfxSet_0 = cyVfxSet_1;
    (&kernelContext_17)->cyVfxInput_0 = *cyVfxPush_1;

#line 368
    cyVfxLoadParams_0(&kernelContext_17);

    uint tid_0 = thread_0.x;
    if(((&(&kernelContext_17)->cyVfxInput_0)->pass_0) == 0U)
    {

#line 372
        if(tid_0 != 0U)
        {

#line 372
            return;
        }

#line 373
        (&(&kernelContext_17)->cyVfxInput_0)->particle_index_0 = 0.0;

#line 373
        cyVfxKernel_sparks0_1_0(0U, &kernelContext_17);

        return;
    }
    if(((&(&kernelContext_17)->cyVfxInput_0)->pass_0) == 1U)
    {

#line 378
        if(tid_0 >= (*((&kernelContext_17)->cyVfxSet_0->counts_0+3U)))
        {

#line 378
            return;
        }

#line 379
        uint device* _S14 = (&kernelContext_17)->cyVfxSet_0->free_list_0+tid_0;
        (&(&kernelContext_17)->cyVfxInput_0)->particle_index_0 = float(*_S14);
        (&(&kernelContext_17)->cyVfxInput_0)->spawn_index_0 = float(tid_0);
        (&(&kernelContext_17)->cyVfxInput_0)->normalised_age_0 = 0.0;

#line 382
        cyVfxKernel_sparks0_6_0(*_S14, &kernelContext_17);

        *((&kernelContext_17)->cyVfxSet_0->alive_0+*_S14) = 1U;

        uint _S15 = atomic_fetch_add_explicit(((atomic_uint device*)((&kernelContext_17)->cyVfxSet_0->counts_0+4U)), 1U, memory_order_relaxed);
        return;
    }
    if(((&(&kernelContext_17)->cyVfxInput_0)->pass_0) == 3U)
    {

#line 390
        if(tid_0 >= (*((&kernelContext_17)->cyVfxSet_0->counts_0+0U)))
        {

#line 390
            return;
        }

#line 390
        float3 _S16 = cyVfxLoad_position_0(*((&kernelContext_17)->cyVfxSet_0->indices_0+tid_0), &kernelContext_17);



        *((&kernelContext_17)->cyVfxSet_0->keys_0+tid_0) = ~(as_type<uint>((dot(_S16, _S16))));
        return;
    }
    if(tid_0 >= (*((&kernelContext_17)->cyVfxSet_0->counts_0+0U)))
    {

#line 397
        return;
    }

#line 398
    uint device* _S17 = (&kernelContext_17)->cyVfxSet_0->indices_0+tid_0;
    (&(&kernelContext_17)->cyVfxInput_0)->particle_index_0 = float(*_S17);

#line 399
    cyVfxKernel_sparks0_4_0(*_S17, &kernelContext_17);

    return;
}

)cy_msl";

}  // namespace cy::vfx::gpu

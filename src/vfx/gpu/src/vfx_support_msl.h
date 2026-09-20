#pragma once
// Compiled MSL for the VFX GPU scheduler. GENERATED — do not edit by hand.

namespace cy::vfx::gpu {

inline constexpr char kVfxResetMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 115 "src/vfx/gpu/shaders/vfx_support.slang"
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
    uint block_capacity_0;
    uint sort_n_0;
    uint sort_passes_0;
    uint reserved_0;
};


#line 70
struct CyVfxEvent_0
{
    uint source_0;
    uint depth_0;
    float rank_0;
    float payload_0;
};


#line 77
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


#line 77
struct KernelContext_0
{
    CyVfxInput_0 constant* cyVfxPush_0;
    CyVfxSet_default_0 constant* cyVfxSet_0;
};


#line 144
[[kernel]] void vfx_reset(uint3 thread_0 [[thread_position_in_grid]], CyVfxInput_0 constant* cyVfxPush_1 [[buffer(1)]], CyVfxSet_default_0 constant* cyVfxSet_1 [[buffer(0)]])
{

#line 144
    thread KernelContext_0 kernelContext_0;

#line 144
    (&kernelContext_0)->cyVfxPush_0 = cyVfxPush_1;

#line 144
    (&kernelContext_0)->cyVfxSet_0 = cyVfxSet_1;
    uint slot_0 = thread_0.x;
    if(slot_0 < (cyVfxPush_1->block_capacity_0))
    {

#line 147
        *((&kernelContext_0)->cyVfxSet_0->alive_0+slot_0) = 0U;
        *((&kernelContext_0)->cyVfxSet_0->indices_0+slot_0) = 0U;
        *((&kernelContext_0)->cyVfxSet_0->free_list_0+slot_0) = 0U;
        *((&kernelContext_0)->cyVfxSet_0->keys_0+slot_0) = 4294967295U;

#line 146
    }

#line 152
    if(slot_0 == 0U)
    {

#line 152
        uint word_0 = 0U;
        for(;;)
        {

#line 153
            if(word_0 < 16U)
            {
            }
            else
            {

#line 153
                break;
            }

#line 154
            *((&kernelContext_0)->cyVfxSet_0->counts_0+word_0) = 0U;

#line 153
            word_0 = word_0 + 1U;

#line 153
        }

#line 153
        uint arg_0 = 0U;


        for(;;)
        {

#line 156
            if(arg_0 < 9U)
            {
            }
            else
            {

#line 156
                break;
            }

#line 157
            *((&kernelContext_0)->cyVfxSet_0->args_0+arg_0) = 0U;

#line 156
            arg_0 = arg_0 + 1U;

#line 156
        }

#line 152
    }

#line 160
    return;
}

)cy_msl";

inline constexpr char kVfxCompactMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 115 "src/vfx/gpu/shaders/vfx_support.slang"
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
    uint block_capacity_0;
    uint sort_n_0;
    uint sort_passes_0;
    uint reserved_0;
};


#line 70
struct CyVfxEvent_0
{
    uint source_0;
    uint depth_0;
    float rank_0;
    float payload_0;
};


#line 77
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


#line 175
struct KernelContext_0
{
    CyVfxInput_0 constant* cyVfxPush_0;
    CyVfxSet_default_0 constant* cyVfxSet_0;
    uint threadgroup* gs_live_base_0;
    uint threadgroup* gs_free_base_0;
    array<uint, int(64)> threadgroup* gs_scan_0;
};


#line 171
uint inclusive_scan_0(uint lane_0, uint value_0, KernelContext_0 thread* kernelContext_0)
{

    threadgroup_barrier(mem_flags::mem_threadgroup);
    (*kernelContext_0->gs_scan_0)[lane_0] = value_0;
    threadgroup_barrier(mem_flags::mem_threadgroup);

#line 176
    uint offset_0 = 1U;
    for(;;)
    {

#line 177
        if(offset_0 < 64U)
        {
        }
        else
        {

#line 177
            break;
        }

#line 177
        uint addend_0;

        if(lane_0 >= offset_0)
        {

#line 179
            addend_0 = (*kernelContext_0->gs_scan_0)[lane_0 - offset_0];

#line 179
        }
        else
        {

#line 179
            addend_0 = 0U;

#line 179
        }


        threadgroup_barrier(mem_flags::mem_threadgroup);
        (*kernelContext_0->gs_scan_0)[lane_0] = (*kernelContext_0->gs_scan_0)[lane_0] + addend_0;
        threadgroup_barrier(mem_flags::mem_threadgroup);

#line 177
        offset_0 = offset_0 << 1U;

#line 177
    }

#line 186
    return (*kernelContext_0->gs_scan_0)[lane_0];
}



[[kernel]] void vfx_compact(uint3 thread_0 [[thread_position_in_threadgroup]], CyVfxInput_0 constant* cyVfxPush_1 [[buffer(1)]], CyVfxSet_default_0 constant* cyVfxSet_1 [[buffer(0)]])
{

#line 191
    thread KernelContext_0 kernelContext_1;

#line 191
    (&kernelContext_1)->cyVfxPush_0 = cyVfxPush_1;

#line 191
    (&kernelContext_1)->cyVfxSet_0 = cyVfxSet_1;

#line 191
    threadgroup uint gs_live_base_1;

#line 191
    (&kernelContext_1)->gs_live_base_0 = &gs_live_base_1;

#line 191
    threadgroup uint gs_free_base_1;

#line 191
    (&kernelContext_1)->gs_free_base_0 = &gs_free_base_1;

#line 191
    threadgroup array<uint, int(64)> gs_scan_1;

#line 191
    (&kernelContext_1)->gs_scan_0 = &gs_scan_1;
    uint lane_1 = thread_0.x;
    uint _S1 = cyVfxPush_1->block_capacity_0;
    uint _S2 = cyVfxPush_1->capacity_0;

    bool _S3 = lane_1 == 0U;

#line 196
    uint chunk_0;

#line 196
    if(_S3)
    {

#line 197
        *(&kernelContext_1)->gs_live_base_0 = 0U;
        *(&kernelContext_1)->gs_free_base_0 = 0U;



        *((&kernelContext_1)->cyVfxSet_0->counts_0+4U) = 0U;
        *((&kernelContext_1)->cyVfxSet_0->counts_0+5U) = 0U;

#line 203
        chunk_0 = 0U;
        for(;;)
        {

#line 204
            if(chunk_0 < 8U)
            {
            }
            else
            {

#line 204
                break;
            }

#line 205
            *((&kernelContext_1)->cyVfxSet_0->counts_0+(8U + chunk_0)) = 0U;

#line 204
            chunk_0 = chunk_0 + 1U;

#line 204
        }

#line 196
    }

#line 208
    threadgroup_barrier(mem_flags::mem_threadgroup);

#line 208
    chunk_0 = 0U;

    for(;;)
    {

#line 210
        if(chunk_0 < _S1)
        {
        }
        else
        {

#line 210
            break;
        }

#line 211
        uint slot_0 = chunk_0 + lane_1;
        bool _S4 = slot_0 < _S1;

#line 212
        uint state_0;

#line 212
        if(_S4)
        {

#line 212
            state_0 = *((&kernelContext_1)->cyVfxSet_0->alive_0+slot_0);

#line 212
        }
        else
        {

#line 212
            state_0 = 0U;

#line 212
        }

#line 212
        bool _S5;

#line 217
        if(_S4)
        {

#line 217
            _S5 = state_0 != 0U;

#line 217
        }
        else
        {

#line 217
            _S5 = false;

#line 217
        }

#line 217
        bool _S6;

#line 217
        if(_S5)
        {

#line 217
            _S6 = slot_0 >= _S2;

#line 217
        }
        else
        {

#line 217
            _S6 = false;

#line 217
        }

#line 217
        uint state_1;

#line 217
        if(_S6)
        {

#line 218
            *((&kernelContext_1)->cyVfxSet_0->alive_0+slot_0) = 0U;

            uint _S7 = atomic_fetch_add_explicit(((atomic_uint device*)((&kernelContext_1)->cyVfxSet_0->counts_0+5U)), 1U, memory_order_relaxed);

#line 220
            state_1 = 0U;

#line 217
        }
        else
        {

#line 217
            state_1 = state_0;

#line 217
        }

#line 217
        uint is_live_0;

#line 233
        if(state_1 != 0U)
        {

#line 233
            is_live_0 = 1U;

#line 233
        }
        else
        {

#line 233
            is_live_0 = 0U;

#line 233
        }
        if(state_1 == 2U)
        {

#line 235
            *((&kernelContext_1)->cyVfxSet_0->alive_0+slot_0) = 1U;

#line 234
        }

#line 234
        uint _S8 = inclusive_scan_0(lane_1, is_live_0, &kernelContext_1);

#line 239
        uint live_total_0 = (*(&kernelContext_1)->gs_scan_0)[63U];
        uint live_index_0 = *(&kernelContext_1)->gs_live_base_0 + _S8 - is_live_0;
        if(is_live_0 != 0U)
        {

#line 242
            *((&kernelContext_1)->cyVfxSet_0->indices_0+live_index_0) = slot_0;

#line 241
        }

#line 241
        bool _S9;

#line 249
        if(_S4)
        {

#line 249
            _S9 = slot_0 < _S2;

#line 249
        }
        else
        {

#line 249
            _S9 = false;

#line 249
        }

#line 249
        bool _S10;

#line 249
        if(_S9)
        {

#line 249
            _S10 = state_1 == 0U;

#line 249
        }
        else
        {

#line 249
            _S10 = false;

#line 249
        }

#line 249
        uint is_free_0;

#line 249
        if(_S10)
        {

#line 249
            is_free_0 = 1U;

#line 249
        }
        else
        {

#line 249
            is_free_0 = 0U;

#line 249
        }

#line 249
        uint _S11 = inclusive_scan_0(lane_1, is_free_0, &kernelContext_1);

        uint free_total_0 = (*(&kernelContext_1)->gs_scan_0)[63U];
        uint free_index_0 = *(&kernelContext_1)->gs_free_base_0 + _S11 - is_free_0;
        if(is_free_0 != 0U)
        {

#line 254
            *((&kernelContext_1)->cyVfxSet_0->free_list_0+free_index_0) = slot_0;

#line 253
        }



        threadgroup_barrier(mem_flags::mem_threadgroup);
        if(_S3)
        {

#line 259
            *(&kernelContext_1)->gs_live_base_0 = *(&kernelContext_1)->gs_live_base_0 + live_total_0;
            *(&kernelContext_1)->gs_free_base_0 = *(&kernelContext_1)->gs_free_base_0 + free_total_0;

#line 258
        }



        threadgroup_barrier(mem_flags::mem_threadgroup);

#line 210
        chunk_0 = chunk_0 + 64U;

#line 210
    }

#line 265
    if(lane_1 != 0U)
    {

#line 266
        return;
    }

#line 266
    uint threadgroup* _S12 = (&kernelContext_1)->gs_live_base_0;


    uint live_0 = *_S12;
    uint free_0 = *(&kernelContext_1)->gs_free_base_0;
    *((&kernelContext_1)->cyVfxSet_0->counts_0+0U) = *_S12;
    *((&kernelContext_1)->cyVfxSet_0->counts_0+1U) = free_0;

#line 284
    uint _S13 = min((min(*((&kernelContext_1)->cyVfxSet_0->counts_0+2U), 65535U) * min((&kernelContext_1)->cyVfxPush_0->spawn_scale_fixed_0, 65536U)) >> 16U, free_0);
    *((&kernelContext_1)->cyVfxSet_0->counts_0+3U) = _S13;



    *((&kernelContext_1)->cyVfxSet_0->counts_0+6U) = live_0 + _S13;
    *((&kernelContext_1)->cyVfxSet_0->counts_0+7U) = (&kernelContext_1)->cyVfxPush_0->sort_passes_0;


    uint live_groups_0 = (live_0 + 64U - 1U) / 64U;
    uint spawn_groups_0 = (_S13 + 64U - 1U) / 64U;
    *((&kernelContext_1)->cyVfxSet_0->args_0+int(0)) = live_groups_0;
    *((&kernelContext_1)->cyVfxSet_0->args_0+int(1)) = 1U;
    *((&kernelContext_1)->cyVfxSet_0->args_0+int(2)) = 1U;
    *((&kernelContext_1)->cyVfxSet_0->args_0+int(3)) = spawn_groups_0;
    *((&kernelContext_1)->cyVfxSet_0->args_0+int(4)) = 1U;
    *((&kernelContext_1)->cyVfxSet_0->args_0+int(5)) = 1U;
    *((&kernelContext_1)->cyVfxSet_0->args_0+int(6)) = live_groups_0;
    *((&kernelContext_1)->cyVfxSet_0->args_0+int(7)) = 1U;
    *((&kernelContext_1)->cyVfxSet_0->args_0+int(8)) = 1U;
    return;
}

)cy_msl";

inline constexpr char kVfxSortMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 115 "src/vfx/gpu/shaders/vfx_support.slang"
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
    uint block_capacity_0;
    uint sort_n_0;
    uint sort_passes_0;
    uint reserved_0;
};


#line 70
struct CyVfxEvent_0
{
    uint source_0;
    uint depth_0;
    float rank_0;
    float payload_0;
};


#line 77
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


#line 336
struct KernelContext_0
{
    CyVfxInput_0 constant* cyVfxPush_0;
    CyVfxSet_default_0 constant* cyVfxSet_0;
    array<uint, int(2048)> threadgroup* gs_keys_0;
    array<uint, int(2048)> threadgroup* gs_index_0;
};


#line 329
[[kernel]] void vfx_sort(uint3 thread_0 [[thread_position_in_threadgroup]], CyVfxInput_0 constant* cyVfxPush_1 [[buffer(1)]], CyVfxSet_default_0 constant* cyVfxSet_1 [[buffer(0)]])
{

#line 329
    uint k_0;

#line 329
    uint j_0;

#line 329
    thread KernelContext_0 kernelContext_0;

#line 329
    (&kernelContext_0)->cyVfxPush_0 = cyVfxPush_1;

#line 329
    (&kernelContext_0)->cyVfxSet_0 = cyVfxSet_1;

#line 329
    threadgroup array<uint, int(2048)> gs_keys_1;

#line 329
    (&kernelContext_0)->gs_keys_0 = &gs_keys_1;

#line 329
    threadgroup array<uint, int(2048)> gs_index_1;

#line 329
    (&kernelContext_0)->gs_index_0 = &gs_index_1;
    uint lane_0 = thread_0.x;
    uint _S1 = min(cyVfxPush_1->sort_n_0, 2048U);
    uint _S2 = *(cyVfxSet_1->counts_0+0U);

#line 332
    uint slot_0 = lane_0;

    for(;;)
    {

#line 334
        if(slot_0 < _S1)
        {
        }
        else
        {

#line 334
            break;
        }

#line 335
        bool real_0 = slot_0 < _S2;
        if(real_0)
        {

#line 336
            k_0 = *((&kernelContext_0)->cyVfxSet_0->keys_0+slot_0);

#line 336
        }
        else
        {

#line 336
            k_0 = 4294967295U;

#line 336
        }

#line 336
        (*(&kernelContext_0)->gs_keys_0)[slot_0] = k_0;
        if(real_0)
        {

#line 337
            j_0 = *((&kernelContext_0)->cyVfxSet_0->indices_0+slot_0);

#line 337
        }
        else
        {

#line 337
            j_0 = 4294967295U;

#line 337
        }

#line 337
        (*(&kernelContext_0)->gs_index_0)[slot_0] = j_0;

#line 334
        slot_0 = slot_0 + 256U;

#line 334
    }

#line 339
    threadgroup_barrier(mem_flags::mem_threadgroup);

#line 339
    k_0 = 2U;

    for(;;)
    {

#line 341
        if(k_0 <= _S1)
        {
        }
        else
        {

#line 341
            break;
        }

#line 341
        j_0 = k_0 >> 1U;
        for(;;)
        {

#line 342
            if(j_0 > 0U)
            {
            }
            else
            {

#line 342
                break;
            }

#line 342
            uint i_0 = lane_0;
            for(;;)
            {

#line 343
                if(i_0 < _S1)
                {
                }
                else
                {

#line 343
                    break;
                }

#line 344
                uint partner_0 = i_0 ^ j_0;
                if(partner_0 <= i_0)
                {

#line 346
                    i_0 = i_0 + 256U;

#line 343
                    continue;
                }



                bool ascending_0 = (i_0 & k_0) == 0U;
                uint mine_0 = (*(&kernelContext_0)->gs_keys_0)[i_0];
                uint theirs_0 = (*(&kernelContext_0)->gs_keys_0)[partner_0];

#line 350
                bool _S3;
                if(ascending_0)
                {

#line 351
                    _S3 = mine_0 > theirs_0;

#line 351
                }
                else
                {

#line 351
                    _S3 = false;

#line 351
                }

#line 351
                bool _S4;

#line 351
                if(_S3)
                {

#line 351
                    _S4 = true;

#line 351
                }
                else
                {

#line 351
                    if(!ascending_0)
                    {

#line 351
                        _S4 = mine_0 < theirs_0;

#line 351
                    }
                    else
                    {

#line 351
                        _S4 = false;

#line 351
                    }

#line 351
                }

#line 351
                if(_S4)
                {

#line 352
                    (*(&kernelContext_0)->gs_keys_0)[i_0] = theirs_0;
                    (*(&kernelContext_0)->gs_keys_0)[partner_0] = mine_0;
                    uint index_mine_0 = (*(&kernelContext_0)->gs_index_0)[i_0];
                    (*(&kernelContext_0)->gs_index_0)[i_0] = (*(&kernelContext_0)->gs_index_0)[partner_0];
                    (*(&kernelContext_0)->gs_index_0)[partner_0] = index_mine_0;

#line 351
                }

#line 343
                i_0 = i_0 + 256U;

#line 343
            }

#line 359
            threadgroup_barrier(mem_flags::mem_threadgroup);

#line 342
            j_0 = j_0 >> 1U;

#line 342
        }

#line 341
        k_0 = k_0 << 1U;

#line 341
    }

#line 341
    j_0 = lane_0;

#line 365
    for(;;)
    {

#line 365
        if(j_0 < _S2)
        {
        }
        else
        {

#line 365
            break;
        }

#line 366
        *((&kernelContext_0)->cyVfxSet_0->keys_0+j_0) = (*(&kernelContext_0)->gs_keys_0)[j_0];
        *((&kernelContext_0)->cyVfxSet_0->indices_0+j_0) = (*(&kernelContext_0)->gs_index_0)[j_0];

#line 365
        j_0 = j_0 + 256U;

#line 365
    }



    return;
}

)cy_msl";

}  // namespace cy::vfx::gpu

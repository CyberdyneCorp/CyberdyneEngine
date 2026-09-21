// SPDX-License-Identifier: MIT
#pragma once
// Compiled MSL for the GPU skinning dispatch. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::skinning {

inline constexpr char kSkinVerticesMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 132 "src/rendering/skinning/shaders/skin.slang"
int low_half_0(uint word_0)
{

#line 133
    return int(word_0 << 16U) >> 16U;
}


#line 135
int high_half_0(uint word_1)
{

#line 136
    return int(word_1) >> 16U;
}


#line 125
float from_snorm16_0(int value_0)
{

#line 126
    float scaled_0 = float(value_0) / 32767.0;

#line 126
    float _S1;
    if(scaled_0 < -1.0)
    {

#line 127
        _S1 = -1.0;

#line 127
    }
    else
    {

#line 127
        _S1 = scaled_0;

#line 127
    }

#line 127
    return _S1;
}


#line 142
float sign_or_positive_0(float value_1)
{

#line 142
    float _S2;
    if(value_1 < 0.0)
    {

#line 143
        _S2 = -1.0;

#line 143
    }
    else
    {

#line 143
        _S2 = 1.0;

#line 143
    }

#line 143
    return _S2;
}


float3 octahedral_decode_0(int x_0, int y_0)
{

#line 148
    float fx_0 = from_snorm16_0(x_0);
    float fy_0 = from_snorm16_0(y_0);
    float _S3 = 1.0 - abs(fx_0) - abs(fy_0);

#line 150
    thread float3 vector_0 = float3(fx_0, fy_0, _S3);
    if(_S3 < 0.0)
    {
        float unfolded_y_0 = (1.0 - abs(vector_0.x)) * sign_or_positive_0(vector_0.y);
        vector_0.x = (1.0 - abs(vector_0.y)) * sign_or_positive_0(vector_0.x);
        vector_0.y = unfolded_y_0;

#line 151
    }

#line 157
    float length_squared_0 = dot(vector_0, vector_0);

#line 157
    float3 _S4;
    if(length_squared_0 > 0.0)
    {

#line 158
        _S4 = vector_0 * float3((1.0 / sqrt(length_squared_0))) ;

#line 158
    }
    else
    {

#line 158
        _S4 = float3(0.0, 0.0, 1.0);

#line 158
    }

#line 158
    return _S4;
}


#line 68
struct SkinConstants_0
{
    uint vertex_count_0;
    uint bone_count_0;
    uint pose_offset_0;
    uint influences_0;
    uint flags_0;
    uint first_input_vertex_0;
    uint first_output_vertex_0;
    uint active_blend_shapes_0;
};


#line 87
struct SkinSet_default_0
{
    float4 device* bones_0;
    uint device* in_positions_0;
    uint device* in_frames_0;
    uint device* influences_1;
    uint device* out_positions_0;
    uint device* out_frames_0;
    float4 device* bone_dual_quaternions_0;
    uint device* blend_shape_deltas_0;
    uint4 device* active_blend_shapes_1;
};


#line 87
struct KernelContext_0
{
    SkinConstants_0 constant* constants_0;
    SkinSet_default_0 constant* skinSet_0;
};


#line 190
uint find_delta_0(uint first_0, uint count_0, uint vertex_0, KernelContext_0 thread* kernelContext_0)
{

#line 190
    uint low_0 = 0U;

#line 190
    uint high_0 = count_0;


    for(;;)
    {

#line 193
        if(low_0 < high_0)
        {
        }
        else
        {

#line 193
            break;
        }

#line 194
        uint middle_0 = low_0 + (high_0 - low_0) / 2U;
        uint index_0 = first_0 + middle_0;
        uint at_0 = kernelContext_0->skinSet_0->blend_shape_deltas_0[index_0 * 10U];
        if(at_0 == vertex_0)
        {

#line 198
            return index_0;
        }
        if(at_0 < vertex_0)
        {

#line 200
            low_0 = middle_0 + 1U;

#line 200
        }
        else
        {

#line 200
            high_0 = middle_0;

#line 200
        }

#line 193
    }

#line 206
    return 4294967295U;
}

float3 delta_vector_0(uint index_1, uint component_offset_0, KernelContext_0 thread* kernelContext_1)
{

#line 210
    uint base_0 = index_1 * 10U + component_offset_0;
    return float3((as_type<float>((kernelContext_1->skinSet_0->blend_shape_deltas_0[base_0]))), (as_type<float>((kernelContext_1->skinSet_0->blend_shape_deltas_0[base_0 + 1U]))), (as_type<float>((kernelContext_1->skinSet_0->blend_shape_deltas_0[base_0 + 2U]))));
}




float3 dual_rotate_0(float4 real_0, float3 direction_0)
{

#line 218
    float3 _S5 = real_0.xyz;

#line 218
    return direction_0 + float3(2.0)  * cross(_S5, cross(_S5, direction_0) + float3(real_0.w)  * direction_0);
}

float3 dual_translation_0(float4 real_1, float4 dual_0)
{

#line 222
    float3 _S6 = dual_0.xyz;

#line 222
    float3 _S7 = real_1.xyz;

#line 222
    return float3(2.0)  * (float3(real_1.w)  * _S6 - float3(dual_0.w)  * _S7 + cross(_S7, _S6));
}


#line 181
float3 normalised_or_0(float3 vector_1, float3 fallback_0)
{

#line 182
    float length_squared_1 = dot(vector_1, vector_1);

#line 182
    float3 _S8;
    if(length_squared_1 > 0.0)
    {

#line 183
        _S8 = vector_1 * float3((1.0 / sqrt(length_squared_1))) ;

#line 183
    }
    else
    {

#line 183
        _S8 = fallback_0;

#line 183
    }

#line 183
    return _S8;
}


#line 117
float round_half_away_0(float value_2)
{

#line 117
    float _S9;
    if(value_2 < 0.0)
    {

#line 118
        _S9 = - floor(- value_2 + 0.5);

)cy_msl"
    R"cy_msl(#line 118
    }
    else
    {

#line 118
        _S9 = floor(value_2 + 0.5);

#line 118
    }

#line 118
    return _S9;
}

int to_snorm16_0(float value_3)
{

#line 122
    return int(round_half_away_0(clamp(value_3, -1.0, 1.0) * 32767.0));
}


#line 138
uint pack_halves_0(int low_1, int high_1)
{

#line 139
    return (uint(low_1) & 65535U) | (uint(high_1) << 16U);
}


#line 163
uint octahedral_encode_0(float3 unit_vector_0)
{

#line 164
    float _S10 = unit_vector_0.x;

#line 164
    float _S11 = unit_vector_0.y;

#line 164
    float _S12 = unit_vector_0.z;

#line 164
    float magnitude_0 = abs(_S10) + abs(_S11) + abs(_S12);
    if(magnitude_0 <= 0.0)
    {
        return 0U;
    }
    float inv_0 = 1.0 / magnitude_0;
    float x_1 = _S10 * inv_0;
    float y_1 = _S11 * inv_0;

#line 171
    float x_2;

#line 171
    float y_2;
    if(_S12 < 0.0)
    {
        float folded_y_0 = (1.0 - abs(x_1)) * sign_or_positive_0(y_1);

#line 174
        x_2 = (1.0 - abs(y_1)) * sign_or_positive_0(x_1);

#line 174
        y_2 = folded_y_0;

#line 172
    }
    else
    {

#line 172
        x_2 = x_1;

#line 172
        y_2 = y_1;

#line 172
    }

#line 178
    return pack_halves_0(to_snorm16_0(x_2), to_snorm16_0(y_2));
}


#line 228
[[kernel]] void skin_vertices(uint3 thread_0 [[thread_position_in_grid]], SkinConstants_0 constant* constants_1 [[buffer(1)]], SkinSet_default_0 constant* skinSet_1 [[buffer(0)]])
{

#line 228
    float3 normal_0;

#line 228
    float3 tangent_0;

#line 228
    thread KernelContext_0 kernelContext_2;

#line 228
    (&kernelContext_2)->constants_0 = constants_1;

#line 228
    (&kernelContext_2)->skinSet_0 = skinSet_1;
    uint vertex_1 = thread_0.x;
    if(vertex_1 >= (constants_1->vertex_count_0))
    {

#line 231
        return;
    }
    uint in_index_0 = (&kernelContext_2)->constants_0->first_input_vertex_0 + vertex_1;
    uint out_index_0 = (&kernelContext_2)->constants_0->first_output_vertex_0 + vertex_1;

#line 234
    uint encoded_tangent_0;
    if(((&kernelContext_2)->constants_0->influences_0) >= 8U)
    {

#line 235
        encoded_tangent_0 = 2U;

#line 235
    }
    else
    {

#line 235
        encoded_tangent_0 = 1U;

#line 235
    }
    bool frames_0 = (((&kernelContext_2)->constants_0->flags_0) & 1U) != 0U;
    bool dual_1 = (((&kernelContext_2)->constants_0->flags_0) & 2U) != 0U;
    bool shapes_0 = ((&kernelContext_2)->constants_0->active_blend_shapes_0) != 0U;

#line 238
    uint probe_0 = 0U;

#line 238
    uint weight_bytes_0 = 0U;

#line 244
    for(;;)
    {

#line 244
        if(probe_0 < encoded_tangent_0)
        {
        }
        else
        {

#line 244
            break;
        }

#line 245
        uint packed_0 = (&kernelContext_2)->skinSet_0->influences_1[(in_index_0 * encoded_tangent_0 + probe_0) * 2U + 1U];
        uint weight_bytes_1 = weight_bytes_0 + ((packed_0 & 255U) + ((packed_0 >> 8U) & 255U) + ((packed_0 >> 16U) & 255U) + ((packed_0 >> 24U) & 255U));

#line 244
        probe_0 = probe_0 + 1U;

#line 244
        weight_bytes_0 = weight_bytes_1;

#line 244
    }

#line 249
    uint position_base_0 = in_index_0 * 3U;
    uint out_position_base_0 = out_index_0 * 3U;
    uint frame_base_0 = in_index_0 * 2U;
    uint out_frame_base_0 = out_index_0 * 2U;

#line 252
    bool have_pivot_0;
    if(!shapes_0)
    {

#line 253
        have_pivot_0 = weight_bytes_0 == 0U;

#line 253
    }
    else
    {

#line 253
        have_pivot_0 = false;

#line 253
    }

#line 253
    if(have_pivot_0)
    {

#line 254
        *((&kernelContext_2)->skinSet_0->out_positions_0+out_position_base_0) = (&kernelContext_2)->skinSet_0->in_positions_0[position_base_0];
        *((&kernelContext_2)->skinSet_0->out_positions_0+(out_position_base_0 + 1U)) = (&kernelContext_2)->skinSet_0->in_positions_0[position_base_0 + 1U];
        *((&kernelContext_2)->skinSet_0->out_positions_0+(out_position_base_0 + 2U)) = (&kernelContext_2)->skinSet_0->in_positions_0[position_base_0 + 2U];
        if(frames_0)
        {

#line 258
            *((&kernelContext_2)->skinSet_0->out_frames_0+out_frame_base_0) = (&kernelContext_2)->skinSet_0->in_frames_0[frame_base_0];
            *((&kernelContext_2)->skinSet_0->out_frames_0+(out_frame_base_0 + 1U)) = (&kernelContext_2)->skinSet_0->in_frames_0[frame_base_0 + 1U];

#line 257
        }



        return;
    }


    float3 _S13 = float3((as_type<float>(((&kernelContext_2)->skinSet_0->in_positions_0[position_base_0]))), (as_type<float>(((&kernelContext_2)->skinSet_0->in_positions_0[position_base_0 + 1U]))), (as_type<float>(((&kernelContext_2)->skinSet_0->in_positions_0[position_base_0 + 2U]))));


    uint normal_word_0 = (&kernelContext_2)->skinSet_0->in_frames_0[frame_base_0];
    uint tangent_word_0 = (&kernelContext_2)->skinSet_0->in_frames_0[frame_base_0 + 1U];


    int tangent_x_0 = (low_half_0(tangent_word_0)) & int(-2);

#line 272
    float bitangent_sign_0;
    if((tangent_word_0 & 1U) != 0U)
    {

#line 273
        bitangent_sign_0 = -1.0;

#line 273
    }
    else
    {

#line 273
        bitangent_sign_0 = 1.0;

#line 273
    }
    float3 _S14 = float3(0.0, 0.0, 1.0);
    float3 _S15 = float3(1.0, 0.0, 0.0);

#line 275
    float3 normal_1;

#line 275
    float3 tangent_1;
    if(frames_0)
    {
        float3 _S16 = octahedral_decode_0(tangent_x_0, high_half_0(tangent_word_0));

#line 278
        normal_1 = octahedral_decode_0(low_half_0(normal_word_0), high_half_0(normal_word_0));

#line 278
        tangent_1 = _S16;

#line 276
    }
    else
    {

#line 276
        normal_1 = _S14;

#line 276
        tangent_1 = _S15;

#line 276
    }

#line 276
    uint slot_0 = 0U;

#line 276
    float3 rest_0 = _S13;

#line 281
    for(;;)
    {

#line 281
        if(slot_0 < ((&kernelContext_2)->constants_0->active_blend_shapes_0))
        {
        }
        else
        {

#line 281
            break;
        }

)cy_msl"
    R"cy_msl(#line 282
        uint4 shape_0 = (&kernelContext_2)->skinSet_0->active_blend_shapes_1[slot_0];

#line 282
        uint _S17 = find_delta_0(shape_0.x, shape_0.y, in_index_0, &kernelContext_2);

        if(_S17 == 4294967295U)
        {

#line 285
            slot_0 = slot_0 + 1U;

#line 281
            continue;
        }

#line 287
        float weight_0 = (as_type<float>((shape_0.z)));

#line 287
        float3 _S18 = delta_vector_0(_S17, 1U, &kernelContext_2);

#line 287
        float3 _S19 = float3(weight_0) ;
        float3 rest_1 = rest_0 + _S18 * _S19;
        if(frames_0)
        {

#line 289
            float3 _S20 = delta_vector_0(_S17, 4U, &kernelContext_2);
            float3 normal_2 = normal_1 + _S20 * _S19;

#line 290
            float3 _S21 = delta_vector_0(_S17, 7U, &kernelContext_2);
            float3 tangent_2 = tangent_1 + _S21 * _S19;

#line 291
            normal_0 = normal_2;

#line 291
            tangent_0 = tangent_2;

#line 289
        }
        else
        {

#line 289
            normal_0 = normal_1;

#line 289
            tangent_0 = tangent_1;

#line 289
        }

#line 289
        rest_0 = rest_1;

#line 289
        normal_1 = normal_0;

#line 289
        tangent_1 = tangent_0;

#line 281
        slot_0 = slot_0 + 1U;

#line 281
    }

#line 281
    uint block_0;

#line 281
    uint lane_0;

#line 281
    uint bone_0;

#line 281
    float blend_scale_0;

#line 281
    float weight_sum_0;

#line 281
    float3 moved_tangent_0;

#line 301
    if(dual_1)
    {

#line 302
        float4 _S22 = float4(0.0, 0.0, 0.0, 0.0);

#line 302
        have_pivot_0 = false;

#line 302
        float4 pivot_0 = _S22;

#line 302
        block_0 = 0U;

#line 302
        float4 blend_real_0 = _S22;

#line 302
        float4 blend_dual_0 = _S22;

#line 302
        blend_scale_0 = 0.0;

#line 302
        weight_sum_0 = 0.0;

#line 307
        for(;;)
        {

#line 307
            if(block_0 < encoded_tangent_0)
            {
            }
            else
            {

#line 307
                break;
            }

#line 308
            uint base_1 = (in_index_0 * encoded_tangent_0 + block_0) * 2U;
            uint _S23 = (&kernelContext_2)->skinSet_0->influences_1[base_1];
            uint _S24 = (&kernelContext_2)->skinSet_0->influences_1[base_1 + 1U];

#line 310
            bool have_pivot_1 = have_pivot_0;

#line 310
            float4 pivot_1 = pivot_0;

#line 310
            lane_0 = 0U;

#line 310
            float4 blend_real_1 = blend_real_0;

#line 310
            float4 blend_dual_1 = blend_dual_0;

#line 310
            float blend_scale_1 = blend_scale_0;

#line 310
            float weight_sum_1 = weight_sum_0;
            for(;;)
            {

#line 311
                if(lane_0 < 4U)
                {
                }
                else
                {

#line 311
                    break;
                }
                uint _S25 = lane_0 * 8U;

#line 313
                float weight_1 = float((_S24 >> _S25) & 255U) * 0.00392156885936856;
                if(weight_1 == 0.0)
                {

#line 315
                    lane_0 = lane_0 + 1U;

#line 311
                    continue;
                }

#line 317
                uint bone_1 = (_S23 >> _S25) & 255U;
                if(bone_1 >= ((&kernelContext_2)->constants_0->bone_count_0))
                {

#line 318
                    bone_0 = (&kernelContext_2)->constants_0->bone_count_0 - 1U;

#line 318
                }
                else
                {

#line 318
                    bone_0 = bone_1;

#line 318
                }


                uint row_0 = ((&kernelContext_2)->constants_0->pose_offset_0 + bone_0) * 3U;
                float4 real_2 = (&kernelContext_2)->skinSet_0->bone_dual_quaternions_0[row_0];
                float4 dual_part_0 = (&kernelContext_2)->skinSet_0->bone_dual_quaternions_0[row_0 + 1U];

#line 323
                bool have_pivot_2;

#line 323
                float4 pivot_2;
                if(!have_pivot_1)
                {

#line 324
                    pivot_2 = real_2;

#line 324
                    have_pivot_2 = true;

#line 324
                }
                else
                {

#line 324
                    pivot_2 = pivot_1;

#line 324
                    have_pivot_2 = have_pivot_1;

#line 324
                }

#line 324
                float signed_weight_0;

#line 331
                if((dot(pivot_2, real_2)) < 0.0)
                {

#line 331
                    signed_weight_0 = - weight_1;

#line 331
                }
                else
                {

#line 331
                    signed_weight_0 = weight_1;

#line 331
                }
                float4 blend_real_2 = blend_real_1 + real_2 * float4(signed_weight_0) ;
                float4 blend_dual_2 = blend_dual_1 + dual_part_0 * float4(signed_weight_0) ;
                float blend_scale_2 = blend_scale_1 + (&kernelContext_2)->skinSet_0->bone_dual_quaternions_0[row_0 + 2U].x * weight_1;
                float weight_sum_2 = weight_sum_1 + weight_1;

#line 335
                have_pivot_1 = have_pivot_2;

#line 335
                pivot_1 = pivot_2;

#line 335
                blend_real_1 = blend_real_2;

#line 335
                blend_dual_1 = blend_dual_2;

#line 335
                blend_scale_1 = blend_scale_2;

#line 335
                weight_sum_1 = weight_sum_2;

#line 311
                lane_0 = lane_0 + 1U;

#line 311
            }

#line 307
            uint block_1 = block_0 + 1U;

#line 307
            have_pivot_0 = have_pivot_1;

#line 307
            pivot_0 = pivot_1;

#line 307
            block_0 = block_1;

#line 307
            blend_real_0 = blend_real_1;

#line 307
            blend_dual_0 = blend_dual_1;

)cy_msl"
    R"cy_msl(#line 307
            blend_scale_0 = blend_scale_1;

#line 307
            weight_sum_0 = weight_sum_1;

#line 307
        }

#line 338
        float length_squared_2 = dot(blend_real_0, blend_real_0);
        if(weight_sum_0 != 0.0)
        {

#line 339
            have_pivot_0 = length_squared_2 > 0.0;

#line 339
        }
        else
        {

#line 339
            have_pivot_0 = false;

#line 339
        }

#line 339
        if(have_pivot_0)
        {

#line 339
            float4 _S26 = float4((1.0 / sqrt(length_squared_2))) ;

            float4 blend_real_3 = blend_real_0 * _S26;


            float3 _S27 = dual_rotate_0(blend_real_3, rest_0 * float3(blend_scale_0) ) + dual_translation_0(blend_real_3, blend_dual_0 * _S26);

            if(frames_0)
            {
                float3 _S28 = dual_rotate_0(blend_real_3, tangent_1);

#line 348
                normal_0 = dual_rotate_0(blend_real_3, normal_1);

#line 348
                tangent_0 = _S28;

#line 346
            }
            else
            {

#line 346
                normal_0 = normal_1;

#line 346
                tangent_0 = tangent_1;

#line 346
            }

#line 297
            float3 _S29 = normal_0;
            float3 _S30 = tangent_0;

#line 298
            normal_0 = _S27;

#line 298
            tangent_0 = _S29;

#line 298
            moved_tangent_0 = _S30;

#line 339
        }
        else
        {

#line 339
            normal_0 = rest_0;

#line 339
            tangent_0 = normal_1;

#line 339
            moved_tangent_0 = tangent_1;

#line 339
        }

#line 301
    }
    else
    {

#line 352
        float4 _S31 = float4(0.0, 0.0, 0.0, 0.0);

#line 352
        thread array<float4, int(3)> blended_0;

#line 352
        blended_0[int(0)] = _S31;

#line 352
        blended_0[int(1)] = _S31;

#line 352
        blended_0[int(2)] = _S31;

#line 352
        block_0 = 0U;

#line 352
        blend_scale_0 = 0.0;
        for(;;)
        {

#line 353
            if(block_0 < encoded_tangent_0)
            {
            }
            else
            {

#line 353
                break;
            }

#line 354
            uint base_2 = (in_index_0 * encoded_tangent_0 + block_0) * 2U;
            uint _S32 = (&kernelContext_2)->skinSet_0->influences_1[base_2];
            uint _S33 = (&kernelContext_2)->skinSet_0->influences_1[base_2 + 1U];

#line 356
            lane_0 = 0U;

#line 356
            weight_sum_0 = blend_scale_0;
            for(;;)
            {

#line 357
                if(lane_0 < 4U)
                {
                }
                else
                {

#line 357
                    break;
                }
                uint _S34 = lane_0 * 8U;

#line 359
                float weight_2 = float((_S33 >> _S34) & 255U) * 0.00392156885936856;
                if(weight_2 == 0.0)
                {

#line 361
                    lane_0 = lane_0 + 1U;

#line 357
                    continue;
                }

#line 365
                uint bone_2 = (_S32 >> _S34) & 255U;
                if(bone_2 >= ((&kernelContext_2)->constants_0->bone_count_0))
                {

#line 366
                    bone_0 = (&kernelContext_2)->constants_0->bone_count_0 - 1U;

#line 366
                }
                else
                {

#line 366
                    bone_0 = bone_2;

#line 366
                }


                uint row_1 = ((&kernelContext_2)->constants_0->pose_offset_0 + bone_0) * 3U;

#line 369
                float4 _S35 = float4(weight_2) ;
                blended_0[int(0)] = blended_0[int(0)] + (&kernelContext_2)->skinSet_0->bones_0[row_1] * _S35;
                blended_0[int(1)] = blended_0[int(1)] + (&kernelContext_2)->skinSet_0->bones_0[row_1 + 1U] * _S35;
                blended_0[int(2)] = blended_0[int(2)] + (&kernelContext_2)->skinSet_0->bones_0[row_1 + 2U] * _S35;

#line 372
                weight_sum_0 = weight_sum_0 + weight_2;

#line 357
                lane_0 = lane_0 + 1U;

#line 357
            }

#line 353
            block_0 = block_0 + 1U;

#line 353
            blend_scale_0 = weight_sum_0;

#line 353
        }

#line 376
        if(blend_scale_0 != 0.0)
        {
            float3 _S36 = float3(dot(blended_0[int(0)].xyz, rest_0) + blended_0[int(0)].w, dot(blended_0[int(1)].xyz, rest_0) + blended_0[int(1)].w, dot(blended_0[int(2)].xyz, rest_0) + blended_0[int(2)].w);


            if(frames_0)
            {



                float3 _S37 = float3(dot(blended_0[int(0)].xyz, tangent_1), dot(blended_0[int(1)].xyz, tangent_1), dot(blended_0[int(2)].xyz, tangent_1));

#line 386
                normal_0 = float3(dot(blended_0[int(0)].xyz, normal_1), dot(blended_0[int(1)].xyz, normal_1), dot(blended_0[int(2)].xyz, normal_1));

#line 386
                tangent_0 = _S37;

#line 381
            }
            else
            {

#line 381
                normal_0 = normal_1;

#line 381
                tangent_0 = tangent_1;

#line 381
            }

#line 297
            float3 _S38 = normal_0;
            float3 _S39 = tangent_0;

#line 298
            normal_0 = _S36;

#line 298
            tangent_0 = _S38;

#line 298
            moved_tangent_0 = _S39;

#line 376
        }
        else
        {

#line 376
            normal_0 = rest_0;

#line 376
            tangent_0 = normal_1;

#line 376
            moved_tangent_0 = tangent_1;

#line 376
        }

#line 301
    }

#line 392
    *((&kernelContext_2)->skinSet_0->out_positions_0+out_position_base_0) = (as_type<uint>((normal_0.x)));
    *((&kernelContext_2)->skinSet_0->out_positions_0+(out_position_base_0 + 1U)) = (as_type<uint>((normal_0.y)));
    *((&kernelContext_2)->skinSet_0->out_positions_0+(out_position_base_0 + 2U)) = (as_type<uint>((normal_0.z)));
    if(!frames_0)
    {

#line 396
        return;
    }

    uint encoded_normal_0 = octahedral_encode_0(normalised_or_0(tangent_0, _S14));

    uint encoded_tangent_1 = (octahedral_encode_0(normalised_or_0(moved_tangent_0, _S15))) & 4294967294U;
    if(bitangent_sign_0 < 0.0)
    {

#line 402
        encoded_tangent_0 = encoded_tangent_1 | 1U;

)cy_msl"
    R"cy_msl(#line 402
    }
    else
    {

#line 402
        encoded_tangent_0 = encoded_tangent_1;

#line 402
    }


    *((&kernelContext_2)->skinSet_0->out_frames_0+out_frame_base_0) = encoded_normal_0;
    *((&kernelContext_2)->skinSet_0->out_frames_0+(out_frame_base_0 + 1U)) = encoded_tangent_0;
    return;
}


)cy_msl";

}  // namespace cy::rendering::skinning

#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for samples/10-world. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::sample::world {

/// world_vertex.metal, 1585 bytes.
inline constexpr char kWorldVertexMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 3268 "core.meta.slang"
struct worldVertex_Result_0
{
    float4 clip_0 [[position]];
    float3 world_0 [[user(TEXCOORD)]];
    float3 normal_0 [[user(TEXCOORD_1)]];
    float3 color_0 [[user(TEXCOORD_2)]];
};


#line 3268
struct vertexInput_0
{
    float3 position_0 [[attribute(0)]];
    float3 normal_1 [[attribute(1)]];
    float3 color_1 [[attribute(2)]];
};


#line 75 "samples/10-world/shaders/world.slang"
struct WorldPush_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 row3_0;
    float4 light_0;
    float4 eye_0;
    float4 sun_0;
    float4 ambient_0;
};


#line 127
struct VertexOutput_0
{
    float4 clip_1;
    float3 world_1;
    float3 normal_2;
    float3 color_2;
};


#line 127
[[vertex]] worldVertex_Result_0 worldVertex(vertexInput_0 _S1 [[stage_in]], WorldPush_0 constant* push_0 [[buffer(1)]])
{

#line 138
    float4 _S2 = float4(_S1.position_0, 1.0);
    thread VertexOutput_0 output_0;
    (&output_0)->clip_1 = float4(dot(push_0->row0_0, _S2), dot(push_0->row1_0, _S2), dot(push_0->row2_0, _S2), dot(push_0->row3_0, _S2));

    (&output_0)->world_1 = _S1.position_0;
    (&output_0)->normal_2 = _S1.normal_1;
    (&output_0)->color_2 = _S1.color_1;

#line 144
    thread worldVertex_Result_0 _S3;

#line 144
    (&_S3)->clip_0 = output_0.clip_1;

#line 144
    (&_S3)->world_0 = output_0.world_1;

#line 144
    (&_S3)->normal_0 = output_0.normal_2;

#line 144
    (&_S3)->color_0 = output_0.color_2;

#line 144
    return _S3;
}

)cy_msl";

/// world_fragment.metal, 24388 bytes.
inline constexpr char kWorldFragmentMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 154 "samples/10-world/shaders/world.slang"
float3 tonemap_0(float3 radiance_0)
{

    return pow(saturate(radiance_0 / (float3(1.0)  + radiance_0)), float3(0.45454543828964233) );
}


#line 148 "src/rendering/shaders/cy/field.slang"
bool cyFieldIsImage_0(uint device* image_words_0)
{

#line 148
    bool _S1;

    if(image_words_0[int(0)] == 1129924164U)
    {

#line 150
        _S1 = image_words_0[int(1)] == 1U;

#line 150
    }
    else
    {

#line 150
        _S1 = false;

#line 150
    }

#line 150
    return _S1;
}


#line 118
struct CyFieldHeader_0
{
    uint components_0;
    uint encoding_0;
    uint interpolation_0;
    uint rule_0;
    uint verticalCells_0;
    float cellMetres_0;
    float verticalMetres_0;
    float verticalOrigin_0;
    float rangeMin_0;
    float rangeMax_0;
    array<float, int(4)> defaults_0;
    uint entries_0;
    int originTileX_0;
    int originTileZ_0;
};


#line 153
CyFieldHeader_0 cyFieldReadHeader_0(uint device* image_words_1)
{

    uint packed_0 = image_words_1[int(2)];

#line 155
    thread CyFieldHeader_0 header_0;

    (&header_0)->components_0 = packed_0 & 255U;
    (&header_0)->encoding_0 = (packed_0 >> 8U) & 255U;
    (&header_0)->interpolation_0 = (packed_0 >> 16U) & 255U;
    (&header_0)->rule_0 = (packed_0 >> 24U) & 255U;
    (&header_0)->verticalCells_0 = image_words_1[int(3)];
    (&header_0)->cellMetres_0 = (as_type<float>((image_words_1[int(4)])));
    (&header_0)->verticalMetres_0 = (as_type<float>((image_words_1[int(5)])));
    (&header_0)->verticalOrigin_0 = (as_type<float>((image_words_1[int(6)])));
    (&header_0)->rangeMin_0 = (as_type<float>((image_words_1[int(7)])));
    (&header_0)->rangeMax_0 = (as_type<float>((image_words_1[int(8)])));

#line 166
    uint index_0 = 0U;
    for(;;)
    {

#line 167
        if(index_0 < 4U)
        {
        }
        else
        {

#line 167
            break;
        }
        (&header_0)->defaults_0[index_0] = (as_type<float>((image_words_1[9U + index_0])));

#line 167
        index_0 = index_0 + 1U;

#line 167
    }



    (&header_0)->entries_0 = image_words_1[int(13)];
    (&header_0)->originTileX_0 = (as_type<int>((image_words_1[int(14)])));
    (&header_0)->originTileZ_0 = (as_type<int>((image_words_1[int(15)])));
    return header_0;
}


#line 318
int cyFieldIfloor_0(float value_0)
{
    return int(floor(value_0));
}


#line 312
int cyFieldFloorDiv_0(int value_1, int divisor_0)
{
    int quotient_0 = value_1 / divisor_0;
    int _S2 = value_1 % divisor_0;

#line 315
    bool _S3;

#line 315
    if(_S2 != int(0))
    {

#line 315
        _S3 = (value_1 < int(0)) != (divisor_0 < int(0));

#line 315
    }
    else
    {

#line 315
        _S3 = false;

#line 315
    }

#line 315
    int _S4;

#line 315
    if(_S3)
    {

#line 315
        _S4 = quotient_0 - int(1);

#line 315
    }
    else
    {

#line 315
        _S4 = quotient_0;

#line 315
    }

#line 315
    return _S4;
}


#line 280
bool cyFieldFindEntry_0(uint device* image_words_2, const CyFieldHeader_0 thread* header_1, uint layer_0, int tileX_0, int tileZ_0, uint thread* payloadWords_0)
{

    *payloadWords_0 = 0U;

#line 283
    uint low_0 = 0U;

#line 283
    uint high_0 = header_1->entries_0;


    for(;;)
    {

#line 286
        if(low_0 < high_0)
        {
        }
        else
        {

#line 286
            break;
        }
        uint middle_0 = low_0 + (high_0 - low_0) / 2U;
        uint base_0 = 16U + middle_0 * 4U;
        uint entryLayer_0 = image_words_2[base_0];
        int entryX_0 = (as_type<int>((image_words_2[base_0 + 1U])));
        int entryZ_0 = (as_type<int>((image_words_2[base_0 + 2U])));

#line 292
        bool _S5;
        if(entryLayer_0 < layer_0)
        {

#line 293
            _S5 = true;

#line 293
        }
        else
        {

#line 293
            if(entryLayer_0 == layer_0)
            {

#line 293
                _S5 = entryZ_0 < tileZ_0;

#line 293
            }
            else
            {

#line 293
                _S5 = false;

#line 293
            }

#line 293
        }

#line 293
        bool before_0;

#line 293
        bool _S6;

#line 293
        if(_S5)
        {

#line 293
            before_0 = true;

#line 293
        }
        else
        {

#line 294
            if(entryLayer_0 == layer_0)
            {

#line 294
                before_0 = entryZ_0 == tileZ_0;

#line 294
            }
            else
            {

#line 294
                before_0 = false;

#line 294
            }

#line 294
            if(before_0)
            {

#line 294
                _S6 = entryX_0 < tileX_0;

#line 294
            }
            else
            {

#line 294
                _S6 = false;

#line 294
            }

#line 294
            before_0 = _S6;

#line 293
        }

        if(before_0)
        {

)cy_msl"
    R"cy_msl(#line 295
            low_0 = middle_0 + 1U;


            continue;
        }
        if(entryLayer_0 == layer_0)
        {

#line 300
            _S6 = entryZ_0 == tileZ_0;

#line 300
        }
        else
        {

#line 300
            _S6 = false;

#line 300
        }

#line 300
        bool _S7;

#line 300
        if(_S6)
        {

#line 300
            _S7 = entryX_0 == tileX_0;

#line 300
        }
        else
        {

#line 300
            _S7 = false;

#line 300
        }

#line 300
        if(_S7)
        {
            *payloadWords_0 = image_words_2[base_0 + 3U];
            return true;
        }

#line 303
        high_0 = middle_0;

#line 286
    }

#line 307
    return false;
}


#line 348
struct CyFieldLattice_0
{
    int i0_0;
    int k0_0;
    int j0_0;
    float fx_0;
    float fz_0;
    float fy_0;
    bool linear_0;
};

CyFieldLattice_0 cyFieldLattice_0(const CyFieldHeader_0 thread* header_2, float x_0, float y_0, float z_0)
{
    thread CyFieldLattice_0 lattice_0;
    (&lattice_0)->i0_0 = int(0);
    (&lattice_0)->k0_0 = int(0);
    (&lattice_0)->j0_0 = int(0);
    (&lattice_0)->fx_0 = 0.0;
    (&lattice_0)->fz_0 = 0.0;
    (&lattice_0)->fy_0 = 0.0;
    bool _S8 = (header_2->interpolation_0) == 1U;

#line 368
    (&lattice_0)->linear_0 = _S8;
    if(_S8)
    {

#line 376
        float u_0 = x_0 / header_2->cellMetres_0 - 0.5;
        float w_0 = z_0 / header_2->cellMetres_0 - 0.5;
        int _S9 = cyFieldIfloor_0(u_0);

#line 378
        (&lattice_0)->i0_0 = _S9;
        int _S10 = cyFieldIfloor_0(w_0);

#line 379
        (&lattice_0)->k0_0 = _S10;
        (&lattice_0)->fx_0 = u_0 - float(_S9);
        (&lattice_0)->fz_0 = w_0 - float(_S10);

#line 369
    }
    else
    {

#line 385
        (&lattice_0)->i0_0 = cyFieldIfloor_0(x_0 / header_2->cellMetres_0);
        (&lattice_0)->k0_0 = cyFieldIfloor_0(z_0 / header_2->cellMetres_0);

#line 369
    }

#line 389
    float vertical_0 = (y_0 - header_2->verticalOrigin_0) / header_2->verticalMetres_0;

#line 389
    bool _S11;
    if((&lattice_0)->linear_0)
    {

#line 390
        _S11 = (header_2->verticalCells_0) > 1U;

#line 390
    }
    else
    {

#line 390
        _S11 = false;

#line 390
    }

#line 390
    if(_S11)
    {
        float v_0 = vertical_0 - 0.5;
        int _S12 = cyFieldIfloor_0(v_0);

#line 393
        (&lattice_0)->j0_0 = _S12;
        (&lattice_0)->fy_0 = v_0 - float(_S12);

#line 390
    }
    else
    {

#line 398
        (&lattice_0)->j0_0 = cyFieldIfloor_0(vertical_0);

#line 390
    }

#line 400
    return lattice_0;
}


#line 323
int cyFieldClampI32_0(int value_2, int low_1, int high_1)
{
    if(value_2 < low_1)
    {
        return low_1;
    }
    if(high_1 < value_2)
    {
        return high_1;
    }
    return value_2;
}


#line 177
uint cyFieldEncodingStride_0(uint encoding_1)
{
    if(encoding_1 == 0U)
    {
        return 4U;
    }

#line 181
    bool _S13;

    if(encoding_1 == 2U)
    {

#line 183
        _S13 = true;

#line 183
    }
    else
    {

#line 183
        _S13 = encoding_1 == 4U;

#line 183
    }

#line 183
    if(_S13)
    {
        return 2U;
    }
    if(encoding_1 == 1U)
    {

#line 187
        _S13 = true;

#line 187
    }
    else
    {

#line 187
        _S13 = encoding_1 == 3U;

#line 187
    }

#line 187
    if(_S13)
    {
        return 1U;
    }
    return 4U;
}


#line 218
uint cyFieldLoadU32_0(uint device* image_words_3, uint byteOffset_0)
{
    uint shift_0 = (byteOffset_0 & 3U) * 8U;
    if(shift_0 == 0U)
    {
        return image_words_3[byteOffset_0 >> 2U];
    }
    uint _S14 = byteOffset_0 >> 2U;
    return (image_words_3[_S14] >> shift_0) | (image_words_3[_S14 + 1U] << (32U - shift_0));
}


#line 203
uint cyFieldLoadByte_0(uint device* image_words_4, uint byteOffset_1)
{
    return (image_words_4[byteOffset_1 >> 2U] >> ((byteOffset_1 & 3U) * 8U)) & 255U;
}

uint cyFieldLoadU16_0(uint device* image_words_5, uint byteOffset_2)
{
    uint shift_1 = (byteOffset_2 & 3U) * 8U;
    if(shift_1 <= 16U)
    {
        return (image_words_5[byteOffset_2 >> 2U] >> shift_1) & 65535U;
    }
    return (cyFieldLoadByte_0(image_words_5, byteOffset_2)) | ((cyFieldLoadByte_0(image_words_5, byteOffset_2 + 1U)) << 8U);
}


#line 231
float4 cyFieldDecodePoint_0(uint device* image_words_6, const CyFieldHeader_0 thread* header_3, uint byteOffset_3)
{
    thread float4 out_0 = float4(0.0, 0.0, 0.0, 0.0);

#line 233
    uint _S15 = header_3->encoding_0;
    uint _S16 = cyFieldEncodingStride_0(header_3->encoding_0);

#line 234
    float _S17 = header_3->rangeMin_0;
    float _S18 = header_3->rangeMax_0 - header_3->rangeMin_0;

#line 235
    uint index_1 = 0U;
    for(;;)
    {

#line 236
        if(index_1 < (header_3->components_0))
        {
        }
        else
        {

#line 236
            break;
        }
        uint offset_0 = byteOffset_3 + index_1 * _S16;

#line 238
        float value_3;

)cy_msl"
    R"cy_msl(#line 249
        if(_S15 == 0U)
        {

#line 249
            value_3 = (as_type<float>((cyFieldLoadU32_0(image_words_6, offset_0))));

#line 249
        }
        else
        {

            if(_S15 == 1U)
            {

#line 253
                value_3 = _S17 + float(cyFieldLoadByte_0(image_words_6, offset_0)) / 255.0 * _S18;

#line 253
            }
            else
            {

                if(_S15 == 2U)
                {

#line 257
                    value_3 = _S17 + float(cyFieldLoadU16_0(image_words_6, offset_0)) / 65535.0 * _S18;

#line 257
                }
                else
                {

#line 265
                    if(_S15 == 3U)
                    {

#line 265
                        value_3 = float(cyFieldLoadByte_0(image_words_6, offset_0));

#line 265
                    }
                    else
                    {

#line 265
                        value_3 = float(cyFieldLoadU16_0(image_words_6, offset_0));

#line 265
                    }

#line 257
                }

#line 253
            }

#line 249
        }

#line 273
        out_0[index_1] = value_3;

#line 236
        index_1 = index_1 + 1U;

#line 236
    }

#line 275
    return out_0;
}


#line 404
struct CyFieldTile_0
{
    uint layer_1;
    int x_1;
    int z_1;
    uint payload_0;
};




float4 cyFieldReadPoint_0(uint device* image_words_7, const CyFieldHeader_0 thread* header_4, const CyFieldTile_0 thread* centre_0, int gi_0, int gk_0, int gj_0)
{


    int j_0 = cyFieldClampI32_0(gj_0, int(0), int(header_4->verticalCells_0) - int(1));
    int tileX_1 = cyFieldFloorDiv_0(gi_0, int(16));
    int tileZ_1 = cyFieldFloorDiv_0(gk_0, int(16));

#line 421
    uint _S19 = centre_0->payload_0;

#line 421
    int _S20 = centre_0->x_1;

#line 421
    bool _S21;

    if(tileX_1 != (centre_0->x_1))
    {

#line 423
        _S21 = true;

#line 423
    }
    else
    {

#line 423
        _S21 = tileZ_1 != (centre_0->z_1);

#line 423
    }

#line 423
    int tileX_2;

#line 423
    int tileZ_2;

#line 423
    uint payload_1;

#line 423
    if(_S21)
    {
        thread uint found_0 = 0U;

#line 425
        bool _S22 = cyFieldFindEntry_0(image_words_7, header_4, centre_0->layer_1, tileX_1, tileZ_1, &found_0);
        if(_S22)
        {

#line 426
            tileX_2 = tileX_1;

#line 426
            tileZ_2 = tileZ_1;

#line 426
            payload_1 = found_0;

#line 426
        }
        else
        {

#line 426
            tileX_2 = _S20;

#line 426
            tileZ_2 = centre_0->z_1;

#line 426
            payload_1 = _S19;

#line 426
        }

#line 423
    }
    else
    {

#line 423
        tileX_2 = tileX_1;

#line 423
        tileZ_2 = tileZ_1;

#line 423
        payload_1 = _S19;

#line 423
    }

#line 423
    float4 _S23 = cyFieldDecodePoint_0(image_words_7, header_4, payload_1 * 4U + (uint(j_0) * 16U * 16U + uint(cyFieldClampI32_0(gk_0 - tileZ_2 * int(16), int(0), int(15))) * 16U + uint(cyFieldClampI32_0(gi_0 - tileX_2 * int(16), int(0), int(15)))) * (cyFieldEncodingStride_0(header_4->encoding_0) * header_4->components_0));

#line 442
    return _S23;
}


#line 338
float cyFieldVerticalWeight_0(uint taps_0, uint dy_0, float fy_1)
{
    if(taps_0 == 1U)
    {
        return 1.0;
    }

#line 342
    float _S24;

    if(dy_0 == 0U)
    {

#line 344
        _S24 = 1.0 - fy_1;

#line 344
    }
    else
    {

#line 344
        _S24 = fy_1;

#line 344
    }

#line 344
    return _S24;
}


#line 446
bool cyFieldSampleLayer_0(uint device* image_words_8, const CyFieldHeader_0 thread* header_5, uint layer_2, float x_2, float y_1, float z_2, float4 thread* result_0)
{

    float4 _S25 = float4(0.0, 0.0, 0.0, 0.0);

#line 449
    *result_0 = _S25;

    thread CyFieldTile_0 centre_1;
    (&centre_1)->layer_1 = layer_2;
    int _S26 = cyFieldFloorDiv_0(cyFieldIfloor_0(x_2 / header_5->cellMetres_0), int(16));

#line 453
    (&centre_1)->x_1 = _S26;
    int _S27 = cyFieldFloorDiv_0(cyFieldIfloor_0(z_2 / header_5->cellMetres_0), int(16));

#line 454
    (&centre_1)->z_1 = _S27;
    (&centre_1)->payload_0 = 0U;
    thread uint payload_2 = 0U;

#line 456
    bool _S28 = cyFieldFindEntry_0(image_words_8, header_5, layer_2, _S26, _S27, &payload_2);
    if(!_S28)
    {
        return false;
    }
    (&centre_1)->payload_0 = payload_2;

#line 461
    CyFieldLattice_0 _S29 = cyFieldLattice_0(header_5, x_2, y_1, z_2);


    if(!_S29.linear_0)
    {

#line 464
        thread CyFieldTile_0 _S30 = centre_1;

#line 464
        float4 _S31 = cyFieldReadPoint_0(image_words_8, header_5, &_S30, _S29.i0_0, _S29.k0_0, _S29.j0_0);

        *result_0 = _S31;
        return true;
    }

    thread float4 accumulated_0 = _S25;

#line 470
    uint _S32;
    if((header_5->verticalCells_0) > 1U)
    {

#line 471
        _S32 = 2U;

#line 471
    }
    else
    {

#line 471
        _S32 = 1U;

#line 471
    }

#line 471
    uint dy_1 = 0U;


    for(;;)
    {

#line 474
        if(dy_1 < _S32)
        {
        }
        else
        {

)cy_msl"
    R"cy_msl(#line 474
            break;
        }
        float _S33 = cyFieldVerticalWeight_0(_S32, dy_1, _S29.fy_0);

#line 476
        uint dz_0 = 0U;
        for(;;)
        {

#line 477
            if(dz_0 < 2U)
            {
            }
            else
            {

#line 477
                break;
            }

#line 477
            float _S34;

            if(dz_0 == 0U)
            {

#line 479
                _S34 = 1.0 - _S29.fz_0;

#line 479
            }
            else
            {

#line 479
                _S34 = _S29.fz_0;

#line 479
            }

#line 479
            uint dx_0 = 0U;
            for(;;)
            {

#line 480
                if(dx_0 < 2U)
                {
                }
                else
                {

#line 480
                    break;
                }

#line 480
                float wx_0;

                if(dx_0 == 0U)
                {

#line 482
                    wx_0 = 1.0 - _S29.fx_0;

#line 482
                }
                else
                {

#line 482
                    wx_0 = _S29.fx_0;

#line 482
                }

                int _S35 = _S29.i0_0 + int(dx_0);

#line 484
                int _S36 = _S29.k0_0 + int(dz_0);
                int _S37 = _S29.j0_0 + int(dy_1);

#line 485
                thread CyFieldTile_0 _S38 = centre_1;

#line 485
                float4 _S39 = cyFieldReadPoint_0(image_words_8, header_5, &_S38, _S35, _S36, _S37);
                float _S40 = wx_0 * _S34 * _S33;

#line 486
                uint index_2 = 0U;
                for(;;)
                {

#line 487
                    if(index_2 < (header_5->components_0))
                    {
                    }
                    else
                    {

#line 487
                        break;
                    }
                    accumulated_0[index_2] = accumulated_0[index_2] + _S39[index_2] * _S40;

#line 487
                    index_2 = index_2 + 1U;

#line 487
                }

#line 480
                dx_0 = dx_0 + 1U;

#line 480
            }

#line 477
            dz_0 = dz_0 + 1U;

#line 477
        }

#line 474
        dy_1 = dy_1 + 1U;

#line 474
    }

#line 494
    *result_0 = accumulated_0;
    return true;
}


float4 cyFieldCombine_0(uint rule_1, float4 base_1, float4 delta_0, uint components_1)
{
    thread float4 out_1 = base_1;

#line 501
    uint index_3 = 0U;
    for(;;)
    {

#line 502
        if(index_3 < components_1)
        {
        }
        else
        {

#line 502
            break;
        }

#line 502
        uint _S41 = index_3;

#line 502
        uint _S42 = index_3;



        if(rule_1 == 0U)
        {
            out_1[index_3] = delta_0[_S42];

#line 506
        }
        else
        {

            if(rule_1 == 1U)
            {
                out_1[index_3] = base_1[_S41] + delta_0[_S42];

#line 510
            }
            else
            {

                if(rule_1 == 2U)
                {
                    out_1[index_3] = base_1[_S41] * delta_0[_S42];

#line 514
                }
                else
                {

#line 514
                    float _S43;



                    if(rule_1 == 3U)
                    {
                        if((base_1[_S41]) > (delta_0[_S42]))
                        {

#line 520
                            _S43 = base_1[_S41];

#line 520
                        }
                        else
                        {

#line 520
                            _S43 = delta_0[_S42];

#line 520
                        }

#line 520
                        out_1[index_3] = _S43;

#line 518
                    }
                    else
                    {

                        if(rule_1 == 4U)
                        {
                            if((base_1[_S41]) < (delta_0[_S42]))
                            {

#line 524
                                _S43 = base_1[_S41];

#line 524
                            }
                            else
                            {

#line 524
                                _S43 = delta_0[_S42];

#line 524
                            }

#line 524
                            out_1[index_3] = _S43;

#line 522
                        }

#line 518
                    }

#line 514
                }

#line 510
            }

#line 506
        }

#line 502
        index_3 = index_3 + 1U;

#line 502
    }

#line 527
    return out_1;
}


#line 142
struct CyFieldSample_0
{
    float4 value_4;
    bool resolved_0;
};


#line 534
CyFieldSample_0 cyFieldSample_0(uint device* image_words_9, float x_3, float y_2, float z_3)
{
    thread CyFieldSample_0 answer_0;
    float4 _S44 = float4(0.0, 0.0, 0.0, 0.0);

#line 537
    (&answer_0)->value_4 = _S44;
    (&answer_0)->resolved_0 = false;
    if(!cyFieldIsImage_0(image_words_9))
    {
        return answer_0;
    }

#line 541
    CyFieldHeader_0 _S45 = cyFieldReadHeader_0(image_words_9);

#line 541
    uint index_4 = 0U;


)cy_msl"
    R"cy_msl(    for(;;)
    {

#line 544
        if(index_4 < 4U)
        {
        }
        else
        {

#line 544
            break;
        }
        (&answer_0)->value_4[index_4] = _S45.defaults_0[index_4];

#line 544
        index_4 = index_4 + 1U;

#line 544
    }

#line 549
    thread float4 base_2 = _S44;
    thread float4 delta_1 = _S44;

#line 550
    thread CyFieldHeader_0 _S46 = _S45;

#line 550
    bool _S47 = cyFieldSampleLayer_0(image_words_9, &_S46, 0U, x_3, y_2, z_3, &base_2);

#line 550
    thread CyFieldHeader_0 _S48 = _S45;

#line 550
    bool _S49 = cyFieldSampleLayer_0(image_words_9, &_S48, 1U, x_3, y_2, z_3, &delta_1);


    bool _S50 = !_S47;

#line 553
    bool _S51;

#line 553
    if(_S50)
    {

#line 553
        _S51 = !_S49;

#line 553
    }
    else
    {

#line 553
        _S51 = false;

#line 553
    }

#line 553
    if(_S51)
    {
        return answer_0;
    }
    (&answer_0)->resolved_0 = true;
    if(_S50)
    {

#line 558
        index_4 = 0U;

        for(;;)
        {

#line 560
            if(index_4 < 4U)
            {
            }
            else
            {

#line 560
                break;
            }
            base_2[index_4] = _S45.defaults_0[index_4];

#line 560
            index_4 = index_4 + 1U;

#line 560
        }

#line 558
    }

#line 565
    if(!_S49)
    {
        (&answer_0)->value_4 = base_2;
        return answer_0;
    }
    (&answer_0)->value_4 = cyFieldCombine_0(_S45.rule_0, base_2, delta_1, _S45.components_0);
    return answer_0;
}


#line 35 "src/rendering/shaders/cy/cloud_shadow.slang"
float cyCloudShadowAtImage_0(uint device* image_words_10, float x_4, float y_3, float z_4)
{

#line 35
    CyFieldSample_0 _S52 = cyFieldSample_0(image_words_10, x_4, y_3, z_4);

#line 41
    return saturate(_S52.value_4.x);
}


#line 75 "samples/10-world/shaders/world.slang"
struct WorldPush_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 row3_0;
    float4 light_0;
    float4 eye_0;
    float4 sun_0;
    float4 ambient_0;
};


#line 163
struct WorldCloudShadow_default_0
{
    uint device* field_0;
    float4 device* placement_0;
};


#line 163
struct KernelContext_0
{
    WorldPush_0 constant* push_0;
    WorldCloudShadow_default_0 constant* cloudShadow_0;
};


#line 161
float sunThroughClouds_0(float3 world_0, KernelContext_0 thread* kernelContext_0)
{
    float4 placement_1 = kernelContext_0->cloudShadow_0->placement_0[int(0)];
    if((placement_1.z) < 0.5)
    {
        return 1.0;
    }

#line 166
    float _S53 = cyCloudShadowAtImage_0(kernelContext_0->cloudShadow_0->field_0, world_0.x + placement_1.x, world_0.y, world_0.z + placement_1.y);



    return _S53;
}


#line 170
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 170
struct pixelInput_0
{
    float3 world_1 [[user(TEXCOORD)]];
    float3 normal_0 [[user(TEXCOORD_1)]];
    float3 color_0 [[user(TEXCOORD_2)]];
};


#line 174
[[fragment]] pixelOutput_0 worldFragment(pixelInput_0 _S54 [[stage_in]], float4 clip_0 [[position]], WorldPush_0 constant* push_1 [[buffer(1)]], WorldCloudShadow_default_0 constant* cloudShadow_1 [[buffer(0)]])
{

#line 174
    thread KernelContext_0 kernelContext_1;

#line 174
    (&kernelContext_1)->push_0 = push_1;

#line 174
    (&kernelContext_1)->cloudShadow_0 = cloudShadow_1;

    if((push_1->eye_0.w) > 0.5)
    {

#line 176
        pixelOutput_0 _S55 = { float4(tonemap_0(_S54.color_0), 1.0) };



        return _S55;
    }

    float3 normal_1 = normalize(_S54.normal_0);
    float3 toEye_0 = normalize(push_1->eye_0.xyz - _S54.world_1);

#line 184
    float3 normal_2;
    if((dot(normal_1, toEye_0)) < 0.0)
    {

#line 185
        normal_2 = - normal_1;

#line 185
    }
    else
    {

#line 185
        normal_2 = normal_1;

#line 185
    }

#line 195
    float3 _S56 = (&kernelContext_1)->push_0->sun_0.xyz;

#line 195
    float _S57 = sunThroughClouds_0(_S54.world_1, &kernelContext_1);

#line 195
    float3 _S58 = _S56 * float3(_S57) ;

    float3 lit_0 = _S54.color_0 * ((&kernelContext_1)->push_0->ambient_0.xyz + _S58 * float3(saturate(dot(normal_2, - (&kernelContext_1)->push_0->light_0.xyz))) );

#line 197
    float3 lit_1;

    if(((&kernelContext_1)->push_0->sun_0.w) > 0.0)
    {

#line 199
        lit_1 = lit_0 + _S58 * float3((pow(saturate(dot(normal_2, normalize(- (&kernelContext_1)->push_0->light_0.xyz + toEye_0))), 120.0) * (&kernelContext_1)->push_0->sun_0.w)) ;

#line 199
    }
    else
    {

#line 199
        lit_1 = lit_0;

#line 199
    }

#line 199
    pixelOutput_0 _S59 = { float4(tonemap_0(lit_1), 1.0) };

#line 210
    return _S59;
}

)cy_msl";

}  // namespace cy::sample::world

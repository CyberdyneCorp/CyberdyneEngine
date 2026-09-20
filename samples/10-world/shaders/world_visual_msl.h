#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for samples/10-world. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::sample::world {

/// shadeTerrain.metal, 28789 bytes.
inline constexpr char kWorldShadeTerrainMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 46 "samples/10-world/shaders/world_visual.slang"
float3 loadFloat3_0(uint32_t device* input_0, uint address_0)
{

#line 46
    uint _S1 = as_type<uint>(input_0[(address_0)>>2]);

#line 46
    uint _S2 = as_type<uint>(input_0[(address_0 + 4U)>>2]);

#line 46
    uint _S3 = as_type<uint>(input_0[(address_0 + 8U)>>2]);

    return (as_type<float3>((uint3(_S1, _S2, _S3))));
}


#line 148 "build/release/shaders/cy/field.slang"
bool cyFieldIsImage_0(uint device* image_words_0)
{

#line 148
    bool _S4;

    if(image_words_0[int(0)] == 1129924164U)
    {

#line 150
        _S4 = image_words_0[int(1)] == 1U;

#line 150
    }
    else
    {

#line 150
        _S4 = false;

#line 150
    }

#line 150
    return _S4;
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
    int _S5 = value_1 % divisor_0;

#line 315
    bool _S6;

#line 315
    if(_S5 != int(0))
    {

#line 315
        _S6 = (value_1 < int(0)) != (divisor_0 < int(0));

#line 315
    }
    else
    {

#line 315
        _S6 = false;

#line 315
    }

#line 315
    int _S7;

#line 315
    if(_S6)
    {

#line 315
        _S7 = quotient_0 - int(1);

#line 315
    }
    else
    {

#line 315
        _S7 = quotient_0;

#line 315
    }

#line 315
    return _S7;
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
        bool _S8;
        if(entryLayer_0 < layer_0)
        {

#line 293
            _S8 = true;

#line 293
        }
        else
        {

#line 293
            if(entryLayer_0 == layer_0)
            {

#line 293
                _S8 = entryZ_0 < tileZ_0;

#line 293
            }
            else
            {

#line 293
                _S8 = false;

#line 293
            }

#line 293
        }

#line 293
        bool before_0;

#line 293
        bool _S9;

#line 293
        if(_S8)
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
                _S9 = entryX_0 < tileX_0;

#line 294
            }
            else
            {

#line 294
                _S9 = false;

#line 294
            }

#line 294
            before_0 = _S9;

#line 293
        }

        if(before_0)
        {

#line 295
            low_0 = middle_0 + 1U;


            continue;
        }
        if(entryLayer_0 == layer_0)
        {

#line 300
            _S9 = entryZ_0 == tileZ_0;

#line 300
        }
        else
        {

#line 300
            _S9 = false;

#line 300
        }

#line 300
        bool _S10;

#line 300
        if(_S9)
        {

#line 300
            _S10 = entryX_0 == tileX_0;

#line 300
        }
        else
        {

#line 300
            _S10 = false;

#line 300
        }

#line 300
        if(_S10)
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
    bool _S11 = (header_2->interpolation_0) == 1U;

#line 368
    (&lattice_0)->linear_0 = _S11;
    if(_S11)
    {

#line 376
        float u_0 = x_0 / header_2->cellMetres_0 - 0.5;
        float w_0 = z_0 / header_2->cellMetres_0 - 0.5;
        int _S12 = cyFieldIfloor_0(u_0);

#line 378
        (&lattice_0)->i0_0 = _S12;
        int _S13 = cyFieldIfloor_0(w_0);

#line 379
        (&lattice_0)->k0_0 = _S13;
        (&lattice_0)->fx_0 = u_0 - float(_S12);
        (&lattice_0)->fz_0 = w_0 - float(_S13);

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
    bool _S14;
    if((&lattice_0)->linear_0)
    {

#line 390
        _S14 = (header_2->verticalCells_0) > 1U;

#line 390
    }
    else
    {

#line 390
        _S14 = false;

#line 390
    }

#line 390
    if(_S14)
    {
        float v_0 = vertical_0 - 0.5;
        int _S15 = cyFieldIfloor_0(v_0);

#line 393
        (&lattice_0)->j0_0 = _S15;
        (&lattice_0)->fy_0 = v_0 - float(_S15);

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
    bool _S16;

    if(encoding_1 == 2U)
    {

#line 183
        _S16 = true;

#line 183
    }
    else
    {

#line 183
        _S16 = encoding_1 == 4U;

#line 183
    }

#line 183
    if(_S16)
    {
        return 2U;
    }
    if(encoding_1 == 1U)
    {

#line 187
        _S16 = true;

#line 187
    }
    else
    {

#line 187
        _S16 = encoding_1 == 3U;

#line 187
    }

#line 187
    if(_S16)
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
    uint _S17 = byteOffset_0 >> 2U;
    return (image_words_3[_S17] >> shift_0) | (image_words_3[_S17 + 1U] << (32U - shift_0));
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
    uint _S18 = header_3->encoding_0;
    uint _S19 = cyFieldEncodingStride_0(header_3->encoding_0);

#line 234
    float _S20 = header_3->rangeMin_0;
    float _S21 = header_3->rangeMax_0 - header_3->rangeMin_0;

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
        uint offset_0 = byteOffset_3 + index_1 * _S19;

#line 238
        float value_3;

#line 249
        if(_S18 == 0U)
        {

#line 249
            value_3 = (as_type<float>((cyFieldLoadU32_0(image_words_6, offset_0))));

#line 249
        }
        else
        {

            if(_S18 == 1U)
            {

#line 253
                value_3 = _S20 + float(cyFieldLoadByte_0(image_words_6, offset_0)) / 255.0 * _S21;

#line 253
            }
            else
            {

                if(_S18 == 2U)
                {

#line 257
                    value_3 = _S20 + float(cyFieldLoadU16_0(image_words_6, offset_0)) / 65535.0 * _S21;

#line 257
                }
                else
                {

#line 265
                    if(_S18 == 3U)
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
    uint _S22 = centre_0->payload_0;

#line 421
    int _S23 = centre_0->x_1;

#line 421
    bool _S24;

    if(tileX_1 != (centre_0->x_1))
    {

#line 423
        _S24 = true;

#line 423
    }
    else
    {

#line 423
        _S24 = tileZ_1 != (centre_0->z_1);

#line 423
    }

#line 423
    int tileX_2;

#line 423
    int tileZ_2;

#line 423
    uint payload_1;

#line 423
    if(_S24)
    {
        thread uint found_0 = 0U;

#line 425
        bool _S25 = cyFieldFindEntry_0(image_words_7, header_4, centre_0->layer_1, tileX_1, tileZ_1, &found_0);
        if(_S25)
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
            tileX_2 = _S23;

#line 426
            tileZ_2 = centre_0->z_1;

#line 426
            payload_1 = _S22;

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
        payload_1 = _S22;

#line 423
    }

#line 423
    float4 _S26 = cyFieldDecodePoint_0(image_words_7, header_4, payload_1 * 4U + (uint(j_0) * 16U * 16U + uint(cyFieldClampI32_0(gk_0 - tileZ_2 * int(16), int(0), int(15))) * 16U + uint(cyFieldClampI32_0(gi_0 - tileX_2 * int(16), int(0), int(15)))) * (cyFieldEncodingStride_0(header_4->encoding_0) * header_4->components_0));

#line 442
    return _S26;
}


#line 338
float cyFieldVerticalWeight_0(uint taps_0, uint dy_0, float fy_1)
{
    if(taps_0 == 1U)
    {
        return 1.0;
    }

#line 342
    float _S27;

    if(dy_0 == 0U)
    {

#line 344
        _S27 = 1.0 - fy_1;

#line 344
    }
    else
    {

#line 344
        _S27 = fy_1;

#line 344
    }

#line 344
    return _S27;
}


#line 446
bool cyFieldSampleLayer_0(uint device* image_words_8, const CyFieldHeader_0 thread* header_5, uint layer_2, float x_2, float y_1, float z_2, float4 thread* result_0)
{

    float4 _S28 = float4(0.0, 0.0, 0.0, 0.0);

#line 449
    *result_0 = _S28;

    thread CyFieldTile_0 centre_1;
    (&centre_1)->layer_1 = layer_2;
    int _S29 = cyFieldFloorDiv_0(cyFieldIfloor_0(x_2 / header_5->cellMetres_0), int(16));

#line 453
    (&centre_1)->x_1 = _S29;
    int _S30 = cyFieldFloorDiv_0(cyFieldIfloor_0(z_2 / header_5->cellMetres_0), int(16));

#line 454
    (&centre_1)->z_1 = _S30;
    (&centre_1)->payload_0 = 0U;
    thread uint payload_2 = 0U;

#line 456
    bool _S31 = cyFieldFindEntry_0(image_words_8, header_5, layer_2, _S29, _S30, &payload_2);
    if(!_S31)
    {
        return false;
    }
    (&centre_1)->payload_0 = payload_2;

#line 461
    CyFieldLattice_0 _S32 = cyFieldLattice_0(header_5, x_2, y_1, z_2);


    if(!_S32.linear_0)
    {

#line 464
        thread CyFieldTile_0 _S33 = centre_1;

#line 464
        float4 _S34 = cyFieldReadPoint_0(image_words_8, header_5, &_S33, _S32.i0_0, _S32.k0_0, _S32.j0_0);

        *result_0 = _S34;
        return true;
    }

    thread float4 accumulated_0 = _S28;

#line 470
    uint _S35;
    if((header_5->verticalCells_0) > 1U)
    {

#line 471
        _S35 = 2U;

#line 471
    }
    else
    {

#line 471
        _S35 = 1U;

#line 471
    }

#line 471
    uint dy_1 = 0U;


    for(;;)
    {

#line 474
        if(dy_1 < _S35)
        {
        }
        else
        {

#line 474
            break;
        }
        float _S36 = cyFieldVerticalWeight_0(_S35, dy_1, _S32.fy_0);

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
            float _S37;

            if(dz_0 == 0U)
            {

#line 479
                _S37 = 1.0 - _S32.fz_0;

#line 479
            }
            else
            {

#line 479
                _S37 = _S32.fz_0;

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
                    wx_0 = 1.0 - _S32.fx_0;

#line 482
                }
                else
                {

#line 482
                    wx_0 = _S32.fx_0;

#line 482
                }

                int _S38 = _S32.i0_0 + int(dx_0);

#line 484
                int _S39 = _S32.k0_0 + int(dz_0);
                int _S40 = _S32.j0_0 + int(dy_1);

#line 485
                thread CyFieldTile_0 _S41 = centre_1;

#line 485
                float4 _S42 = cyFieldReadPoint_0(image_words_8, header_5, &_S41, _S38, _S39, _S40);
                float _S43 = wx_0 * _S37 * _S36;

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
                    accumulated_0[index_2] = accumulated_0[index_2] + _S42[index_2] * _S43;

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
        uint _S44 = index_3;

#line 502
        uint _S45 = index_3;



        if(rule_1 == 0U)
        {
            out_1[index_3] = delta_0[_S45];

#line 506
        }
        else
        {

            if(rule_1 == 1U)
            {
                out_1[index_3] = base_1[_S44] + delta_0[_S45];

#line 510
            }
            else
            {

                if(rule_1 == 2U)
                {
                    out_1[index_3] = base_1[_S44] * delta_0[_S45];

#line 514
                }
                else
                {

#line 514
                    float _S46;



                    if(rule_1 == 3U)
                    {
                        if((base_1[_S44]) > (delta_0[_S45]))
                        {

#line 520
                            _S46 = base_1[_S44];

#line 520
                        }
                        else
                        {

#line 520
                            _S46 = delta_0[_S45];

#line 520
                        }

#line 520
                        out_1[index_3] = _S46;

#line 518
                    }
                    else
                    {

                        if(rule_1 == 4U)
                        {
                            if((base_1[_S44]) < (delta_0[_S45]))
                            {

#line 524
                                _S46 = base_1[_S44];

#line 524
                            }
                            else
                            {

#line 524
                                _S46 = delta_0[_S45];

#line 524
                            }

#line 524
                            out_1[index_3] = _S46;

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
    float4 _S47 = float4(0.0, 0.0, 0.0, 0.0);

#line 537
    (&answer_0)->value_4 = _S47;
    (&answer_0)->resolved_0 = false;
    if(!cyFieldIsImage_0(image_words_9))
    {
        return answer_0;
    }

#line 541
    CyFieldHeader_0 _S48 = cyFieldReadHeader_0(image_words_9);

#line 541
    uint index_4 = 0U;


    for(;;)
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
        (&answer_0)->value_4[index_4] = _S48.defaults_0[index_4];

#line 544
        index_4 = index_4 + 1U;

#line 544
    }

#line 549
    thread float4 base_2 = _S47;
    thread float4 delta_1 = _S47;

#line 550
    thread CyFieldHeader_0 _S49 = _S48;

#line 550
    bool _S50 = cyFieldSampleLayer_0(image_words_9, &_S49, 0U, x_3, y_2, z_3, &base_2);

#line 550
    thread CyFieldHeader_0 _S51 = _S48;

#line 550
    bool _S52 = cyFieldSampleLayer_0(image_words_9, &_S51, 1U, x_3, y_2, z_3, &delta_1);


    bool _S53 = !_S50;

#line 553
    bool _S54;

#line 553
    if(_S53)
    {

#line 553
        _S54 = !_S52;

#line 553
    }
    else
    {

#line 553
        _S54 = false;

#line 553
    }

#line 553
    if(_S54)
    {
        return answer_0;
    }
    (&answer_0)->resolved_0 = true;
    if(_S53)
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
            base_2[index_4] = _S48.defaults_0[index_4];

#line 560
            index_4 = index_4 + 1U;

#line 560
        }

#line 558
    }

#line 565
    if(!_S52)
    {
        (&answer_0)->value_4 = base_2;
        return answer_0;
    }
    (&answer_0)->value_4 = cyFieldCombine_0(_S48.rule_0, base_2, delta_1, _S48.components_0);
    return answer_0;
}


#line 79 "build/release/shaders/cy/terrain_shade.slang"
struct CyTerrainSubstrate_0
{
    float waterDistanceMetres_0;
    bool shoreResolved_0;
    float wetness_0;
    float snowDepthMetres_0;
    float vegetation_0;
};


#line 109
CyTerrainSubstrate_0 cyTerrainSampleSubstrateImages_0(uint device* waterDistance_words_0, uint device* wetness_words_0, uint device* snowDepth_words_0, uint device* vegetation_words_0, float x_4, float y_3, float z_4)
{
    CyFieldSample_0 _S55 = cyFieldSample_0(waterDistance_words_0, x_4, y_3, z_4);

    thread CyTerrainSubstrate_0 substrate_0;

    (&substrate_0)->waterDistanceMetres_0 = _S55.value_4.x;
    (&substrate_0)->shoreResolved_0 = _S55.resolved_0;

#line 116
    CyFieldSample_0 _S56 = cyFieldSample_0(wetness_words_0, x_4, y_3, z_4);
    (&substrate_0)->wetness_0 = _S56.value_4.x;

#line 117
    CyFieldSample_0 _S57 = cyFieldSample_0(snowDepth_words_0, x_4, y_3, z_4);
    (&substrate_0)->snowDepthMetres_0 = _S57.value_4.x;

#line 118
    CyFieldSample_0 _S58 = cyFieldSample_0(vegetation_words_0, x_4, y_3, z_4);
    (&substrate_0)->vegetation_0 = _S58.value_4.x;
    return substrate_0;
}


#line 54
struct CyTerrainPalette_0
{
    float3 rock_0;
    float3 sand_0;
    float3 grass_0;
    float3 alpine_0;
    float3 snow_0;
};


#line 127
float3 cyTerrainShade_0(const CyTerrainPalette_0 thread* palette_0, const CyTerrainSubstrate_0 thread* substrate_1, float slope_0, float altitudeMetres_0, float alpineMetres_0)
{

#line 128
    float3 _S59 = palette_0->grass_0;

#line 128
    bool _S60;

#line 135
    if(substrate_1->shoreResolved_0)
    {

#line 135
        _S60 = (substrate_1->waterDistanceMetres_0) < 14.0;

#line 135
    }
    else
    {

#line 135
        _S60 = false;

#line 135
    }

#line 135
    float3 base_3;

#line 135
    if(_S60)
    {

#line 135
        base_3 = mix(palette_0->sand_0, _S59, float3(saturate((substrate_1->waterDistanceMetres_0 - 4.0) / 10.0)) );

#line 135
    }
    else
    {


        if(slope_0 > 0.44999998807907104)
        {

#line 140
            base_3 = palette_0->rock_0;

#line 140
        }
        else
        {

            if(altitudeMetres_0 > alpineMetres_0)
            {

#line 144
                base_3 = palette_0->alpine_0;

#line 144
            }
            else
            {

#line 144
                base_3 = _S59;

#line 144
            }

#line 140
        }

#line 135
    }

#line 151
    float wet_0 = saturate(substrate_1->wetness_0);
    float3 shaded_0 = float3(base_3.x * (1.0 - wet_0 * 0.44999998807907104), base_3.y * (1.0 - wet_0 * 0.41999998688697815), base_3.z * (1.0 - wet_0 * 0.30000001192092896));

#line 159
    return mix(mix(shaded_0, mix(shaded_0, _S59, float3(0.5) ), float3((saturate(substrate_1->vegetation_0) * 0.40000000596046448)) ), palette_0->snow_0, float3((saturate(substrate_1->snowDepthMetres_0 * 1.20000004768371582) * saturate(1.29999995231628418 - slope_0 * 2.59999990463256836))) );
}


#line 170
float3 cyTerrainShadeAtImages_0(uint device* waterDistance_words_1, uint device* wetness_words_1, uint device* snowDepth_words_1, uint device* vegetation_words_1, const CyTerrainPalette_0 thread* palette_1, float x_5, float y_4, float z_5, float slope_1, float altitudeMetres_1, float alpineMetres_1)
{

    CyTerrainSubstrate_0 _S61 = cyTerrainSampleSubstrateImages_0(waterDistance_words_1, wetness_words_1, snowDepth_words_1, vegetation_words_1, x_5, y_4, z_5);

#line 173
    thread CyTerrainSubstrate_0 _S62 = _S61;

#line 173
    float3 _S63 = cyTerrainShade_0(palette_1, &_S62, slope_1, altitudeMetres_1, alpineMetres_1);



    return _S63;
}


#line 51 "samples/10-world/shaders/world_visual.slang"
void storeFloat3_0(uint32_t device* output_0, uint address_1, float3 value_5)
{
    uint3 _S64 = (as_type<uint3>((value_5)));

#line 53
    output_0[(address_1)>>2] = as_type<uint32_t>(_S64[int(0)]);

#line 53
    output_0[(address_1 + 4U)>>2] = as_type<uint32_t>(_S64[int(1)]);

#line 53
    output_0[(address_1 + 8U)>>2] = as_type<uint32_t>(_S64[int(2)]);
    return;
}


#line 7
struct VisualPush_0
{
    uint terrainCount_0;
    uint skyStart_0;
    uint skyCount_0;
    uint waterStart_0;
    uint waterCount_0;
    uint foamResolution_0;
    uint frameIndex_0;
    uint cloudSeed_0;
    float timeSeconds_0;
    float deltaSeconds_0;
    float wetness_1;
    float snowDepth_0;
    float cloudCoverage_0;
    float sunHeight_0;
    float exposure_0;
    float unusedFloat_0;
    float4 fieldOrigin_0;
};

struct VisualResources_default_0
{
    uint32_t device* terrainVertices_0;
    uint32_t device* terrainColours_0;
    uint32_t device* dynamicVertices_0;
    uint32_t device* dynamicColours_0;
    float device* foamPrevious_0;
    float device* foamNext_0;
    uint device* waterDistanceField_0;
    uint device* wetnessField_0;
    uint device* snowDepthField_0;
    uint device* vegetationField_0;
};


#line 28
struct KernelContext_0
{
    VisualPush_0 constant* visual_0;
    VisualResources_default_0 constant* resources_0;
};


#line 116
[[kernel]] void shadeTerrain(uint3 index_5 [[thread_position_in_grid]], VisualPush_0 constant* visual_1 [[buffer(1)]], VisualResources_default_0 constant* resources_1 [[buffer(0)]])
{

#line 116
    thread KernelContext_0 kernelContext_0;

#line 116
    (&kernelContext_0)->visual_0 = visual_1;

#line 116
    (&kernelContext_0)->resources_0 = resources_1;

#line 116
    uint index_6 = index_5.x;

    if(index_6 >= (visual_1->terrainCount_0))
    {

#line 119
        return;
    }

#line 120
    uint vertexAddress_0 = index_6 * 24U;
    float3 position_0 = loadFloat3_0((&kernelContext_0)->resources_0->terrainVertices_0, vertexAddress_0);

    float slope_2 = 1.0 - saturate(loadFloat3_0((&kernelContext_0)->resources_0->terrainVertices_0, vertexAddress_0 + 12U).y);

#line 123
    uint device* waterDistance_words_2 = (&kernelContext_0)->resources_0->waterDistanceField_0;

#line 123
    uint device* wetness_words_2 = (&kernelContext_0)->resources_0->wetnessField_0;

#line 123
    uint device* snowDepth_words_2 = (&kernelContext_0)->resources_0->snowDepthField_0;

#line 123
    uint device* vegetation_words_2 = (&kernelContext_0)->resources_0->vegetationField_0;

#line 132
    thread CyTerrainPalette_0 palette_2;
    (&palette_2)->rock_0 = float3(0.33000001311302185, 0.31000000238418579, 0.30000001192092896);
    (&palette_2)->sand_0 = float3(0.62000000476837158, 0.56000000238418579, 0.41999998688697815);
    (&palette_2)->grass_0 = float3(0.23999999463558197, 0.34000000357627869, 0.17000000178813934);
    (&palette_2)->alpine_0 = float3(0.43999999761581421, 0.43000000715255737, 0.40000000596046448);
    (&palette_2)->snow_0 = float3(0.86000001430511475, 0.87999999523162842, 0.92000001668930054);


    float _S65 = position_0.x + (&kernelContext_0)->visual_0->fieldOrigin_0.x;

#line 140
    float _S66 = position_0.y;
    float _S67 = position_0.z + (&kernelContext_0)->visual_0->fieldOrigin_0.y;

#line 141
    thread CyTerrainPalette_0 _S68 = palette_2;

#line 141
    float3 _S69 = cyTerrainShadeAtImages_0(waterDistance_words_2, wetness_words_2, snowDepth_words_2, vegetation_words_2, &_S68, _S65, _S66, _S67, slope_2, _S66, 132.0);
    storeFloat3_0((&kernelContext_0)->resources_0->terrainColours_0, index_6 * 12U, _S69);
    return;
}

)cy_msl";

/// shadeClouds.metal, 24175 bytes.
inline constexpr char kWorldShadeCloudsMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 46 "samples/10-world/shaders/world_visual.slang"
float3 loadFloat3_0(uint32_t device* input_0, uint address_0)
{

#line 46
    uint _S1 = as_type<uint>(input_0[(address_0)>>2]);

#line 46
    uint _S2 = as_type<uint>(input_0[(address_0 + 4U)>>2]);

#line 46
    uint _S3 = as_type<uint>(input_0[(address_0 + 8U)>>2]);

    return (as_type<float3>((uint3(_S1, _S2, _S3))));
}


#line 15 "build/release/shaders/cy/noise.slang"
uint3 hashPcg3d_0(uint3 value_0)
{
    uint3 _S4 = value_0 * uint3(1664525U)  + uint3(1013904223U) ;

#line 17
    thread uint3 v_0 = _S4;
    v_0.x = v_0.x + _S4.y * _S4.z;
    v_0.y = v_0.y + v_0.z * v_0.x;
    v_0.z = v_0.z + v_0.x * v_0.y;
    uint3 _S5 = v_0 ^ (v_0 >> (uint3(16U) ));

#line 21
    v_0 = _S5;
    v_0.x = v_0.x + _S5.y * _S5.z;
    v_0.y = v_0.y + v_0.z * v_0.x;
    v_0.z = v_0.z + v_0.x * v_0.y;
    return v_0;
}


#line 36
float valueNoise_0(float3 position_0)
{
    float3 _S6 = floor(position_0);
    float3 _S7 = position_0 - _S6;
    float3 _S8 = _S7 * _S7 * (float3(3.0)  - float3(2.0)  * _S7);
    uint3 _S9 = uint3(int3(_S6) + int3(int(1024)) );

#line 41
    uint corner_0 = 0U;

#line 41
    float result_0 = 0.0;


    for(;;)
    {

#line 44
        if(corner_0 < 8U)
        {
        }
        else
        {

#line 44
            break;
        }
        uint3 _S10 = uint3(corner_0 & 1U, (corner_0 >> 1U) & 1U, (corner_0 >> 2U) & 1U);

        float3 _S11 = mix(float3(1.0)  - _S8, _S8, float3(_S10));
        float result_1 = result_0 + float((hashPcg3d_0(_S9 + _S10).x) >> 8U) * 5.9604644775390625e-08 * _S11.x * _S11.y * _S11.z;

#line 44
        corner_0 = corner_0 + 1U;

#line 44
        result_0 = result_1;

#line 44
    }

#line 51
    return result_0;
}


#line 56 "samples/10-world/shaders/world_visual.slang"
float cloudFbm_0(float3 position_1, float3 seedOffset_0)
{


    return (valueNoise_0(position_1 + seedOffset_0) + valueNoise_0(position_1 * float3(2.02999997138977051)  + seedOffset_0 + float3(17.0, 31.0, 47.0)) * 0.5) / 1.5;
}


#line 7
struct VisualPush_0
{
    uint terrainCount_0;
    uint skyStart_0;
    uint skyCount_0;
    uint waterStart_0;
    uint waterCount_0;
    uint foamResolution_0;
    uint frameIndex_0;
    uint cloudSeed_0;
    float timeSeconds_0;
    float deltaSeconds_0;
    float wetness_0;
    float snowDepth_0;
    float cloudCoverage_0;
    float sunHeight_0;
    float exposure_0;
    float unusedFloat_0;
    float4 fieldOrigin_0;
};


#line 99 "build/release/shaders/cy/field.slang"
struct VisualResources_default_0
{
    uint32_t device* terrainVertices_0;
    uint32_t device* terrainColours_0;
    uint32_t device* dynamicVertices_0;
    uint32_t device* dynamicColours_0;
    float device* foamPrevious_0;
    float device* foamNext_0;
    uint device* waterDistanceField_0;
    uint device* wetnessField_0;
    uint device* snowDepthField_0;
    uint device* vegetationField_0;
};


#line 99
struct KernelContext_0
{
    VisualPush_0 constant* visual_0;
    VisualResources_default_0 constant* resources_0;
};


#line 63 "samples/10-world/shaders/world_visual.slang"
float cloudDensity_0(float3 position_2, float coverage_0, float3 seedOffset_1, KernelContext_0 thread* kernelContext_0)
{
    float heightFraction_0 = saturate((position_2.y - 900.0) / 3400.0);



    float3 advected_0 = position_2 - float3(kernelContext_0->visual_0->timeSeconds_0 * 9.0, 0.0, kernelContext_0->visual_0->timeSeconds_0 * 2.5);

#line 76
    return saturate((cloudFbm_0(advected_0 / float3(5200.0) , seedOffset_1) - (0.69999998807907104 - coverage_0 * 0.37999999523162842)) * 4.5) * (0.62000000476837158 + valueNoise_0(advected_0 / float3(780.0)  + seedOffset_1 + float3(71.0, 11.0, 29.0)) * 0.57999998331069946) * (smoothstep(0.0, 0.18000000715255737, heightFraction_0) * (1.0 - smoothstep(0.68000000715255737, 1.0, heightFraction_0)));
}

float3 composeClouds_0(float3 direction_0, KernelContext_0 thread* kernelContext_1)
{

#line 79
    float3 _S12;

#line 79
    float3 _S13;

#line 79
    float3 _S14;

#line 79
    float _S15;

#line 79
    int _S16;

#line 79
    float transmittance_0;

#line 79
    float3 scattering_0;

#line 79
    float3 _S17;

#line 79
    float _S18;

#line 79
    float3 _S19;

#line 79
    float _S20;

#line 79
    float3 _S21;

#line 79
    float _S22;

#line 79
    float3 _S23;

#line 79
    float _S24;

#line 79
    float3 _S25;

#line 79
    float _S26;

#line 79
    float3 _S27;

#line 79
    float _S28;

#line 79
    float3 _S29;

#line 79
    float _S30;

#line 79
    float3 _S31;

#line 79
    float _S32;

#line 79
    float3 _S33;

#line 79
    float _S34;

#line 79
    float3 _S35;

#line 79
    float _S36;

#line 79
    float3 _S37;

#line 79
    float _S38;

    float _S39 = direction_0.y;
    float daylight_0 = saturate(kernelContext_1->visual_0->sunHeight_0 * 3.0 + 0.25);

    float3 clear_0 = mix(float3(0.0020000000949949, 0.00499999988824129, 0.01799999922513962), float3(0.15999999642372131, 0.43000000715255737, 0.92000001668930054), float3(daylight_0) ) * float3((0.15999999642372131 + 0.8399999737739563 * saturate(_S39 * 0.5 + 0.5))) ;
    if(_S39 <= 0.02500000037252903)
    {

#line 86
        return clear_0;
    }
    float seed_0 = float((kernelContext_1->visual_0->cloudSeed_0) & 255U);
    float3 _S40 = float3(seed_0 * 0.37000000476837158, seed_0 * 0.18999999761581421, seed_0 * 0.52999997138977051);
    float3 _S41 = float3(kernelContext_1->visual_0->fieldOrigin_0.z, 0.0, kernelContext_1->visual_0->fieldOrigin_0.w);
    float _S42 = 900.0 / _S39;
    float _S43 = 3400.0 / (_S39 * 12.0);

#line 92
    for(;;)
    {

#line 92
        for(;;)
        {


            for(;;)
            {

                float3 position_3 = _S41 + direction_0 * float3((_S42 + _S43 * 0.5)) ;

#line 99
                float _S44 = cloudDensity_0(position_3, kernelContext_1->visual_0->cloudCoverage_0, _S40, kernelContext_1);

                float extinction_0 = 1.0 - exp(- _S44 * 0.47999998927116394);
                float heightFraction_1 = saturate((position_3.y - 900.0) / 3400.0);
                float3 dark_0 = float3(0.14000000059604645, 0.17000000178813934, 0.2199999988079071);

#line 103
                _S12 = dark_0;
                float3 bright_0 = float3(1.0, 0.93999999761581421, 0.8399999737739563) * float3((0.30000001192092896 + daylight_0 * 0.69999998807907104)) ;

#line 104
                _S13 = bright_0;

#line 104
                _S14 = mix(dark_0, bright_0, float3((0.25 + heightFraction_1 * 0.75)) ) * float3(extinction_0) ;


                float _S45 = 1.0 - extinction_0;

#line 107
                _S15 = _S45;
                if(_S45 < 0.01499999966472387)
                {

#line 108
                    _S16 = int(0);

#line 108
                    break;
                }

#line 108
                _S16 = int(2);

#line 108
                break;
            }

#line 108
            if(_S16 != int(2))
            {

#line 108
                break;
            }

#line 96
            _S16 = int(1);

#line 96
            break;
        }

#line 96
        if(_S16 != int(1))
        {

#line 96
            transmittance_0 = _S15;

#line 96
            scattering_0 = _S14;

#line 96
            break;
        }

#line 96
        for(;;)
        {

#line 96
            for(;;)
            {

                float3 position_4 = _S41 + direction_0 * float3((_S42 + _S43 * 1.5)) ;

#line 99
                float _S46 = cloudDensity_0(position_4, kernelContext_1->visual_0->cloudCoverage_0, _S40, kernelContext_1);

                float extinction_1 = 1.0 - exp(- _S46 * 0.47999998927116394);

#line 106
                float3 scattering_1 = _S14 + mix(_S12, _S13, float3((0.25 + saturate((position_4.y - 900.0) / 3400.0) * 0.75)) ) * float3((_S15 * extinction_1)) ;

#line 106
                _S17 = scattering_1;
                float transmittance_1 = _S15 * (1.0 - extinction_1);

#line 107
                _S18 = transmittance_1;
                if(transmittance_1 < 0.01499999966472387)
                {

#line 108
                    _S16 = int(0);

#line 108
                    transmittance_0 = transmittance_1;

#line 108
                    scattering_0 = scattering_1;

#line 108
                    break;
                }

#line 108
                _S16 = int(2);

#line 108
                transmittance_0 = _S15;

#line 108
                scattering_0 = _S14;

#line 108
                break;
            }

#line 108
            if(_S16 != int(2))
            {

#line 108
                break;
            }

#line 96
            _S16 = int(1);

#line 96
            break;
        }

#line 96
        if(_S16 != int(1))
        {

#line 96
            break;
        }

#line 96
        for(;;)
        {

#line 96
            for(;;)
            {

                float3 position_5 = _S41 + direction_0 * float3((_S42 + _S43 * 2.5)) ;

#line 99
                float _S47 = cloudDensity_0(position_5, kernelContext_1->visual_0->cloudCoverage_0, _S40, kernelContext_1);

                float extinction_2 = 1.0 - exp(- _S47 * 0.47999998927116394);

#line 106
                float3 scattering_2 = _S17 + mix(_S12, _S13, float3((0.25 + saturate((position_5.y - 900.0) / 3400.0) * 0.75)) ) * float3((_S18 * extinction_2)) ;

#line 106
                _S19 = scattering_2;
                float transmittance_2 = _S18 * (1.0 - extinction_2);

#line 107
                _S20 = transmittance_2;
                if(transmittance_2 < 0.01499999966472387)
                {

#line 108
                    _S16 = int(0);

#line 108
                    transmittance_0 = transmittance_2;

#line 108
                    scattering_0 = scattering_2;

#line 108
                    break;
                }

#line 108
                _S16 = int(2);

#line 108
                break;
            }

#line 108
            if(_S16 != int(2))
            {

#line 108
                break;
            }

#line 96
            _S16 = int(1);

#line 96
            break;
        }

#line 96
        if(_S16 != int(1))
        {

#line 96
            break;
        }

#line 96
        for(;;)
        {

#line 96
            for(;;)
            {

                float3 position_6 = _S41 + direction_0 * float3((_S42 + _S43 * 3.5)) ;

#line 99
                float _S48 = cloudDensity_0(position_6, kernelContext_1->visual_0->cloudCoverage_0, _S40, kernelContext_1);

                float extinction_3 = 1.0 - exp(- _S48 * 0.47999998927116394);

#line 106
                float3 scattering_3 = _S19 + mix(_S12, _S13, float3((0.25 + saturate((position_6.y - 900.0) / 3400.0) * 0.75)) ) * float3((_S20 * extinction_3)) ;

#line 106
                _S21 = scattering_3;
                float transmittance_3 = _S20 * (1.0 - extinction_3);

#line 107
                _S22 = transmittance_3;
                if(transmittance_3 < 0.01499999966472387)
                {

#line 108
                    _S16 = int(0);

#line 108
                    transmittance_0 = transmittance_3;

#line 108
                    scattering_0 = scattering_3;

#line 108
                    break;
                }

#line 108
                _S16 = int(2);

#line 108
                break;
            }

#line 108
            if(_S16 != int(2))
            {

#line 108
                break;
            }

#line 96
            _S16 = int(1);

#line 96
            break;
        }

#line 96
        if(_S16 != int(1))
        {

#line 96
            break;
        }

#line 96
        for(;;)
        {

#line 96
            for(;;)
            {

                float3 position_7 = _S41 + direction_0 * float3((_S42 + _S43 * 4.5)) ;

#line 99
                float _S49 = cloudDensity_0(position_7, kernelContext_1->visual_0->cloudCoverage_0, _S40, kernelContext_1);

                float extinction_4 = 1.0 - exp(- _S49 * 0.47999998927116394);

#line 106
                float3 scattering_4 = _S21 + mix(_S12, _S13, float3((0.25 + saturate((position_7.y - 900.0) / 3400.0) * 0.75)) ) * float3((_S22 * extinction_4)) ;

#line 106
                _S23 = scattering_4;
                float transmittance_4 = _S22 * (1.0 - extinction_4);

#line 107
                _S24 = transmittance_4;
                if(transmittance_4 < 0.01499999966472387)
                {

#line 108
                    _S16 = int(0);

#line 108
                    transmittance_0 = transmittance_4;

#line 108
                    scattering_0 = scattering_4;

#line 108
                    break;
                }

#line 108
                _S16 = int(2);

#line 108
                break;
            }

#line 108
            if(_S16 != int(2))
            {

#line 108
                break;
            }

#line 96
            _S16 = int(1);

#line 96
            break;
        }

#line 96
        if(_S16 != int(1))
        {

#line 96
            break;
        }

#line 96
        for(;;)
        {

#line 96
            for(;;)
            {

                float3 position_8 = _S41 + direction_0 * float3((_S42 + _S43 * 5.5)) ;

#line 99
                float _S50 = cloudDensity_0(position_8, kernelContext_1->visual_0->cloudCoverage_0, _S40, kernelContext_1);

                float extinction_5 = 1.0 - exp(- _S50 * 0.47999998927116394);

#line 106
                float3 scattering_5 = _S23 + mix(_S12, _S13, float3((0.25 + saturate((position_8.y - 900.0) / 3400.0) * 0.75)) ) * float3((_S24 * extinction_5)) ;

#line 106
                _S25 = scattering_5;
                float transmittance_5 = _S24 * (1.0 - extinction_5);

#line 107
                _S26 = transmittance_5;
                if(transmittance_5 < 0.01499999966472387)
                {

#line 108
                    _S16 = int(0);

#line 108
                    transmittance_0 = transmittance_5;

#line 108
                    scattering_0 = scattering_5;

#line 108
                    break;
                }

#line 108
                _S16 = int(2);

#line 108
                break;
            }

#line 108
            if(_S16 != int(2))
            {

#line 108
                break;
            }

#line 96
            _S16 = int(1);

#line 96
            break;
        }

#line 96
        if(_S16 != int(1))
        {

#line 96
            break;
        }

#line 96
        for(;;)
        {

#line 96
            for(;;)
            {

                float3 position_9 = _S41 + direction_0 * float3((_S42 + _S43 * 6.5)) ;

#line 99
                float _S51 = cloudDensity_0(position_9, kernelContext_1->visual_0->cloudCoverage_0, _S40, kernelContext_1);

                float extinction_6 = 1.0 - exp(- _S51 * 0.47999998927116394);

#line 106
                float3 scattering_6 = _S25 + mix(_S12, _S13, float3((0.25 + saturate((position_9.y - 900.0) / 3400.0) * 0.75)) ) * float3((_S26 * extinction_6)) ;

#line 106
                _S27 = scattering_6;
                float transmittance_6 = _S26 * (1.0 - extinction_6);

#line 107
                _S28 = transmittance_6;
                if(transmittance_6 < 0.01499999966472387)
                {

#line 108
                    _S16 = int(0);

#line 108
                    transmittance_0 = transmittance_6;

#line 108
                    scattering_0 = scattering_6;

#line 108
                    break;
                }

#line 108
                _S16 = int(2);

#line 108
                break;
            }

#line 108
            if(_S16 != int(2))
            {

#line 108
                break;
            }

#line 96
            _S16 = int(1);

#line 96
            break;
        }

#line 96
        if(_S16 != int(1))
        {

#line 96
            break;
        }

#line 96
        for(;;)
        {

#line 96
            for(;;)
            {

                float3 position_10 = _S41 + direction_0 * float3((_S42 + _S43 * 7.5)) ;

#line 99
                float _S52 = cloudDensity_0(position_10, kernelContext_1->visual_0->cloudCoverage_0, _S40, kernelContext_1);

                float extinction_7 = 1.0 - exp(- _S52 * 0.47999998927116394);

#line 106
                float3 scattering_7 = _S27 + mix(_S12, _S13, float3((0.25 + saturate((position_10.y - 900.0) / 3400.0) * 0.75)) ) * float3((_S28 * extinction_7)) ;

#line 106
                _S29 = scattering_7;
                float transmittance_7 = _S28 * (1.0 - extinction_7);

#line 107
                _S30 = transmittance_7;
                if(transmittance_7 < 0.01499999966472387)
                {

#line 108
                    _S16 = int(0);

#line 108
                    transmittance_0 = transmittance_7;

#line 108
                    scattering_0 = scattering_7;

#line 108
                    break;
                }

#line 108
                _S16 = int(2);

#line 108
                break;
            }

#line 108
            if(_S16 != int(2))
            {

#line 108
                break;
            }

#line 96
            _S16 = int(1);

#line 96
            break;
        }

#line 96
        if(_S16 != int(1))
        {

#line 96
            break;
        }

#line 96
        for(;;)
        {

#line 96
            for(;;)
            {

                float3 position_11 = _S41 + direction_0 * float3((_S42 + _S43 * 8.5)) ;

#line 99
                float _S53 = cloudDensity_0(position_11, kernelContext_1->visual_0->cloudCoverage_0, _S40, kernelContext_1);

                float extinction_8 = 1.0 - exp(- _S53 * 0.47999998927116394);

#line 106
                float3 scattering_8 = _S29 + mix(_S12, _S13, float3((0.25 + saturate((position_11.y - 900.0) / 3400.0) * 0.75)) ) * float3((_S30 * extinction_8)) ;

#line 106
                _S31 = scattering_8;
                float transmittance_8 = _S30 * (1.0 - extinction_8);

#line 107
                _S32 = transmittance_8;
                if(transmittance_8 < 0.01499999966472387)
                {

#line 108
                    _S16 = int(0);

#line 108
                    transmittance_0 = transmittance_8;

#line 108
                    scattering_0 = scattering_8;

#line 108
                    break;
                }

#line 108
                _S16 = int(2);

#line 108
                break;
            }

#line 108
            if(_S16 != int(2))
            {

#line 108
                break;
            }

#line 96
            _S16 = int(1);

#line 96
            break;
        }

#line 96
        if(_S16 != int(1))
        {

#line 96
            break;
        }

#line 96
        for(;;)
        {

#line 96
            for(;;)
            {

                float3 position_12 = _S41 + direction_0 * float3((_S42 + _S43 * 9.5)) ;

#line 99
                float _S54 = cloudDensity_0(position_12, kernelContext_1->visual_0->cloudCoverage_0, _S40, kernelContext_1);

                float extinction_9 = 1.0 - exp(- _S54 * 0.47999998927116394);

#line 106
                float3 scattering_9 = _S31 + mix(_S12, _S13, float3((0.25 + saturate((position_12.y - 900.0) / 3400.0) * 0.75)) ) * float3((_S32 * extinction_9)) ;

#line 106
                _S33 = scattering_9;
                float transmittance_9 = _S32 * (1.0 - extinction_9);

#line 107
                _S34 = transmittance_9;
                if(transmittance_9 < 0.01499999966472387)
                {

#line 108
                    _S16 = int(0);

#line 108
                    transmittance_0 = transmittance_9;

#line 108
                    scattering_0 = scattering_9;

#line 108
                    break;
                }

#line 108
                _S16 = int(2);

#line 108
                break;
            }

#line 108
            if(_S16 != int(2))
            {

#line 108
                break;
            }

#line 96
            _S16 = int(1);

#line 96
            break;
        }

#line 96
        if(_S16 != int(1))
        {

#line 96
            break;
        }

#line 96
        for(;;)
        {

#line 96
            for(;;)
            {

                float3 position_13 = _S41 + direction_0 * float3((_S42 + _S43 * 10.5)) ;

#line 99
                float _S55 = cloudDensity_0(position_13, kernelContext_1->visual_0->cloudCoverage_0, _S40, kernelContext_1);

                float extinction_10 = 1.0 - exp(- _S55 * 0.47999998927116394);

#line 106
                float3 scattering_10 = _S33 + mix(_S12, _S13, float3((0.25 + saturate((position_13.y - 900.0) / 3400.0) * 0.75)) ) * float3((_S34 * extinction_10)) ;

#line 106
                _S35 = scattering_10;
                float transmittance_10 = _S34 * (1.0 - extinction_10);

#line 107
                _S36 = transmittance_10;
                if(transmittance_10 < 0.01499999966472387)
                {

#line 108
                    _S16 = int(0);

#line 108
                    transmittance_0 = transmittance_10;

#line 108
                    scattering_0 = scattering_10;

#line 108
                    break;
                }

#line 108
                _S16 = int(2);

#line 108
                break;
            }

#line 108
            if(_S16 != int(2))
            {

#line 108
                break;
            }

#line 96
            _S16 = int(1);

#line 96
            break;
        }

#line 96
        if(_S16 != int(1))
        {

#line 96
            break;
        }

#line 96
        for(;;)
        {

#line 96
            for(;;)
            {

                float3 position_14 = _S41 + direction_0 * float3((_S42 + _S43 * 11.5)) ;

#line 99
                float _S56 = cloudDensity_0(position_14, kernelContext_1->visual_0->cloudCoverage_0, _S40, kernelContext_1);

                float extinction_11 = 1.0 - exp(- _S56 * 0.47999998927116394);

#line 106
                float3 scattering_11 = _S35 + mix(_S12, _S13, float3((0.25 + saturate((position_14.y - 900.0) / 3400.0) * 0.75)) ) * float3((_S36 * extinction_11)) ;

#line 106
                _S37 = scattering_11;
                float transmittance_11 = _S36 * (1.0 - extinction_11);

#line 107
                _S38 = transmittance_11;
                if(transmittance_11 < 0.01499999966472387)
                {

#line 108
                    _S16 = int(0);

#line 108
                    transmittance_0 = transmittance_11;

#line 108
                    scattering_0 = scattering_11;

#line 108
                    break;
                }

#line 108
                _S16 = int(2);

#line 108
                break;
            }

#line 108
            if(_S16 != int(2))
            {

#line 108
                break;
            }

#line 96
            _S16 = int(1);

#line 96
            break;
        }

#line 96
        if(_S16 != int(1))
        {

#line 96
            break;
        }

#line 96
        transmittance_0 = _S38;

#line 96
        scattering_0 = _S37;

#line 96
        break;
    }

#line 111
    return clear_0 * float3(transmittance_0)  + scattering_0;
}


#line 51
void storeFloat3_0(uint32_t device* output_0, uint address_1, float3 value_1)
{
    uint3 _S57 = (as_type<uint3>((value_1)));

#line 53
    output_0[(address_1)>>2] = as_type<uint32_t>(_S57[int(0)]);

#line 53
    output_0[(address_1 + 4U)>>2] = as_type<uint32_t>(_S57[int(1)]);

#line 53
    output_0[(address_1 + 8U)>>2] = as_type<uint32_t>(_S57[int(2)]);
    return;
}


#line 147
[[kernel]] void shadeClouds(uint3 index_0 [[thread_position_in_grid]], VisualPush_0 constant* visual_1 [[buffer(1)]], VisualResources_default_0 constant* resources_1 [[buffer(0)]])
{

#line 147
    thread KernelContext_0 kernelContext_2;

#line 147
    (&kernelContext_2)->visual_0 = visual_1;

#line 147
    (&kernelContext_2)->resources_0 = resources_1;

#line 147
    uint index_1 = index_0.x;

    if(index_1 >= (visual_1->skyCount_0))
    {

#line 150
        return;
    }

#line 151
    uint vertex_0 = (&kernelContext_2)->visual_0->skyStart_0 + index_1;

#line 151
    uint32_t device* _S58 = (&kernelContext_2)->resources_0->dynamicColours_0;

    uint _S59 = vertex_0 * 12U;

#line 153
    float3 _S60 = composeClouds_0(normalize(loadFloat3_0((&kernelContext_2)->resources_0->dynamicVertices_0, vertex_0 * 24U + 12U)), &kernelContext_2);

#line 153
    storeFloat3_0(_S58, _S59, _S60);
    return;
}

)cy_msl";

/// evolveFoam.metal, 4092 bytes.
inline constexpr char kWorldEvolveFoamMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 46 "samples/10-world/shaders/world_visual.slang"
float3 loadFloat3_0(uint32_t device* input_0, uint address_0)
{

#line 46
    uint _S1 = as_type<uint>(input_0[(address_0)>>2]);

#line 46
    uint _S2 = as_type<uint>(input_0[(address_0 + 4U)>>2]);

#line 46
    uint _S3 = as_type<uint>(input_0[(address_0 + 8U)>>2]);

    return (as_type<float3>((uint3(_S1, _S2, _S3))));
}

void storeFloat3_0(uint32_t device* output_0, uint address_1, float3 value_0)
{
    uint3 _S4 = (as_type<uint3>((value_0)));

#line 53
    output_0[(address_1)>>2] = as_type<uint32_t>(_S4[int(0)]);

#line 53
    output_0[(address_1 + 4U)>>2] = as_type<uint32_t>(_S4[int(1)]);

#line 53
    output_0[(address_1 + 8U)>>2] = as_type<uint32_t>(_S4[int(2)]);
    return;
}


#line 7
struct VisualPush_0
{
    uint terrainCount_0;
    uint skyStart_0;
    uint skyCount_0;
    uint waterStart_0;
    uint waterCount_0;
    uint foamResolution_0;
    uint frameIndex_0;
    uint cloudSeed_0;
    float timeSeconds_0;
    float deltaSeconds_0;
    float wetness_0;
    float snowDepth_0;
    float cloudCoverage_0;
    float sunHeight_0;
    float exposure_0;
    float unusedFloat_0;
    float4 fieldOrigin_0;
};


#line 99 "build/release/shaders/cy/field.slang"
struct VisualResources_default_0
{
    uint32_t device* terrainVertices_0;
    uint32_t device* terrainColours_0;
    uint32_t device* dynamicVertices_0;
    uint32_t device* dynamicColours_0;
    float device* foamPrevious_0;
    float device* foamNext_0;
    uint device* waterDistanceField_0;
    uint device* wetnessField_0;
    uint device* snowDepthField_0;
    uint device* vegetationField_0;
};


#line 99
struct KernelContext_0
{
    VisualPush_0 constant* visual_0;
    VisualResources_default_0 constant* resources_0;
};


#line 158 "samples/10-world/shaders/world_visual.slang"
[[kernel]] void evolveFoam(uint3 index_0 [[thread_position_in_grid]], VisualPush_0 constant* visual_1 [[buffer(1)]], VisualResources_default_0 constant* resources_1 [[buffer(0)]])
{

#line 158
    thread KernelContext_0 kernelContext_0;

#line 158
    (&kernelContext_0)->visual_0 = visual_1;

#line 158
    (&kernelContext_0)->resources_0 = resources_1;

#line 158
    uint index_1 = index_0.x;


    if(index_1 < (visual_1->foamResolution_0 * visual_1->foamResolution_0))
    {
        uint x_0 = index_1 % visual_1->foamResolution_0;
        uint y_0 = index_1 / visual_1->foamResolution_0;
        uint _S5 = y_0 * visual_1->foamResolution_0;
        uint _S6 = (x_0 + visual_1->foamResolution_0 - 1U) % visual_1->foamResolution_0;


        *((&kernelContext_0)->resources_0->foamNext_0+index_1) = max((&kernelContext_0)->resources_0->foamPrevious_0[_S5 + _S6] * exp(- (&kernelContext_0)->visual_0->deltaSeconds_0 / 9.0), saturate(sin(float(x_0) * 0.17000000178813934 + (&kernelContext_0)->visual_0->timeSeconds_0 * 0.69999998807907104) * cos(float(y_0) * 0.10999999940395355 - (&kernelContext_0)->visual_0->timeSeconds_0 * 0.40000000596046448) - 0.72000002861022949));

#line 161
    }

#line 172
    if(index_1 < ((&kernelContext_0)->visual_0->waterCount_0))
    {
        uint vertex_0 = (&kernelContext_0)->visual_0->waterStart_0 + index_1;
        float3 position_0 = loadFloat3_0((&kernelContext_0)->resources_0->dynamicVertices_0, vertex_0 * 24U);
        uint x_1 = uint(abs(position_0.x) * 0.25) % visual_1->foamResolution_0;
        uint y_1 = uint(abs(position_0.z) * 0.25) % visual_1->foamResolution_0;



        storeFloat3_0((&kernelContext_0)->resources_0->dynamicColours_0, vertex_0 * 12U, mix(float3(0.01999999955296516, 0.05000000074505806, 0.07000000029802322), float3(0.85000002384185791, 0.87999999523162842, 0.92000001668930054), float3(saturate(saturate(sin(float(x_1) * 0.17000000178813934 + (&kernelContext_0)->visual_0->timeSeconds_0 * 0.69999998807907104) * cos(float(y_1) * 0.10999999940395355 - (&kernelContext_0)->visual_0->timeSeconds_0 * 0.40000000596046448) - 0.72000002861022949))) ));

#line 172
    }

#line 184
    return;
}

)cy_msl";

}  // namespace cy::sample::world

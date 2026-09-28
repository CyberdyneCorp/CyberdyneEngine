#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for samples/10-world. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::sample::world {

/// water_vertex.metal, 1561 bytes.
inline constexpr char kWaterVertexMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 3268 "core.meta.slang"
struct waterVertex_Result_0
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


#line 104 "water.slang"
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


#line 184
struct VertexOutput_0
{
    float4 clip_1;
    float3 world_1;
    float3 normal_2;
    float3 color_2;
};


#line 184
[[vertex]] waterVertex_Result_0 waterVertex(vertexInput_0 _S1 [[stage_in]], WorldPush_0 constant* push_0 [[buffer(2)]])
{

#line 195
    float4 _S2 = float4(_S1.position_0, 1.0);
    thread VertexOutput_0 output_0;
    (&output_0)->clip_1 = float4(dot(push_0->row0_0, _S2), dot(push_0->row1_0, _S2), dot(push_0->row2_0, _S2), dot(push_0->row3_0, _S2));

    (&output_0)->world_1 = _S1.position_0;
    (&output_0)->normal_2 = _S1.normal_1;
    (&output_0)->color_2 = _S1.color_1;

#line 201
    thread waterVertex_Result_0 _S3;

#line 201
    (&_S3)->clip_0 = output_0.clip_1;

#line 201
    (&_S3)->world_0 = output_0.world_1;

#line 201
    (&_S3)->normal_0 = output_0.normal_2;

#line 201
    (&_S3)->color_0 = output_0.color_2;

#line 201
    return _S3;
}

)cy_msl";

/// water_fragment.metal, 46862 bytes.
inline constexpr char kWaterFragmentMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 184 "water.slang"
struct VertexOutput_0
{
    float4 clip_0;
    float3 world_0;
    float3 normal_0;
    float3 color_0;
};


#line 99 "../../../src/rendering/shaders/cy/field.slang"
struct WaterCloudShadow_default_0
{
    uint device* field_0;
    float4 device* placement_0;
    float4 device* aerial_0;
};


#line 121 "water.slang"
struct WaterParams_0
{
    float4 inverse0_0;
    float4 inverse1_0;
    float4 inverse2_0;
    float4 inverse3_0;
    float4 viewport_0;
    float4 extinction_0;
    float4 inScatter_0;
    float4 surface_0;
    float4 caustic_0;
    array<float4, int(16)> trains_0;
    array<float4, int(4)> phases_0;
};


#line 321
struct WaterSurface_default_0
{
    texture2d<float, access::sample> refraction_0;
    texture2d<float, access::sample> refractionDepth_0;
    texture2d<float, access::sample> reflection_0;
    WaterParams_0 device* params_0;
};


#line 104
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


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    WaterCloudShadow_default_0 constant* cloudShadow_0;
    WaterSurface_default_0 constant* water_0;
    WorldPush_0 constant* push_0;
};


#line 142 "../../../src/rendering/shaders/cy/field.slang"
struct CyFieldSample_0
{
    float4 value_0;
    bool resolved_0;
};

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


#line 404
struct CyFieldTile_0
{
    uint layer_0;
    int x_0;
    int z_0;
    uint payload_0;
};


#line 318
int cyFieldIfloor_0(float value_1)
{
    return int(floor(value_1));
}


#line 312
int cyFieldFloorDiv_0(int value_2, int divisor_0)
{
    int quotient_0 = value_2 / divisor_0;
    int _S2 = value_2 % divisor_0;

#line 315
    bool _S3;

#line 315
    if(_S2 != int(0))
    {

#line 315
        _S3 = (value_2 < int(0)) != (divisor_0 < int(0));

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
bool cyFieldFindEntry_0(uint device* image_words_2, const CyFieldHeader_0 thread* header_1, uint layer_1, int tileX_0, int tileZ_0, uint thread* payloadWords_0)
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
        if(entryLayer_0 < layer_1)
        {

#line 293
            _S5 = true;

#line 293
        }
        else
        {

#line 293
            if(entryLayer_0 == layer_1)
            {

#line 293
                _S5 = entryZ_0 < tileZ_0;

)cy_msl"
    R"cy_msl(#line 293
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
            if(entryLayer_0 == layer_1)
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

#line 295
            low_0 = middle_0 + 1U;


            continue;
        }
        if(entryLayer_0 == layer_1)
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

CyFieldLattice_0 cyFieldLattice_0(const CyFieldHeader_0 thread* header_2, float x_1, float y_0, float z_1)
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
        float u_0 = x_1 / header_2->cellMetres_0 - 0.5;
        float w_0 = z_1 / header_2->cellMetres_0 - 0.5;
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
        (&lattice_0)->i0_0 = cyFieldIfloor_0(x_1 / header_2->cellMetres_0);
        (&lattice_0)->k0_0 = cyFieldIfloor_0(z_1 / header_2->cellMetres_0);

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
int cyFieldClampI32_0(int value_3, int low_1, int high_1)
{
    if(value_3 < low_1)
    {
        return low_1;
    }
    if(high_1 < value_3)
    {
        return high_1;
    }
    return value_3;
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

)cy_msl"
    R"cy_msl(#line 187
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
        float value_4;

#line 249
        if(_S15 == 0U)
        {

#line 249
            value_4 = (as_type<float>((cyFieldLoadU32_0(image_words_6, offset_0))));

#line 249
        }
        else
        {

            if(_S15 == 1U)
            {

#line 253
                value_4 = _S17 + float(cyFieldLoadByte_0(image_words_6, offset_0)) / 255.0 * _S18;

#line 253
            }
            else
            {

                if(_S15 == 2U)
                {

#line 257
                    value_4 = _S17 + float(cyFieldLoadU16_0(image_words_6, offset_0)) / 65535.0 * _S18;

#line 257
                }
                else
                {

#line 265
                    if(_S15 == 3U)
                    {

#line 265
                        value_4 = float(cyFieldLoadByte_0(image_words_6, offset_0));

#line 265
                    }
                    else
                    {

#line 265
                        value_4 = float(cyFieldLoadU16_0(image_words_6, offset_0));

#line 265
                    }

#line 257
                }

#line 253
            }

#line 249
        }

#line 273
        out_0[index_1] = value_4;

#line 236
        index_1 = index_1 + 1U;

#line 236
    }

#line 275
    return out_0;
}


#line 415
float4 cyFieldReadPoint_0(uint device* image_words_7, const CyFieldHeader_0 thread* header_4, const CyFieldTile_0 thread* centre_0, int gi_0, int gk_0, int gj_0)
{


    int j_0 = cyFieldClampI32_0(gj_0, int(0), int(header_4->verticalCells_0) - int(1));
    int tileX_1 = cyFieldFloorDiv_0(gi_0, int(16));
    int tileZ_1 = cyFieldFloorDiv_0(gk_0, int(16));

#line 421
    uint _S19 = centre_0->payload_0;

#line 421
    int _S20 = centre_0->x_0;

#line 421
    bool _S21;

    if(tileX_1 != (centre_0->x_0))
    {

#line 423
        _S21 = true;

#line 423
    }
    else
    {

#line 423
        _S21 = tileZ_1 != (centre_0->z_0);

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
        bool _S22 = cyFieldFindEntry_0(image_words_7, header_4, centre_0->layer_0, tileX_1, tileZ_1, &found_0);
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
            tileZ_2 = centre_0->z_0;

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
    (&centre_1)->layer_0 = layer_2;
    int _S26 = cyFieldFloorDiv_0(cyFieldIfloor_0(x_2 / header_5->cellMetres_0), int(16));

)cy_msl"
    R"cy_msl(#line 453
    (&centre_1)->x_0 = _S26;
    int _S27 = cyFieldFloorDiv_0(cyFieldIfloor_0(z_2 / header_5->cellMetres_0), int(16));

#line 454
    (&centre_1)->z_0 = _S27;
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

#line 474
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

)cy_msl"
    R"cy_msl(#line 524
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


#line 534
CyFieldSample_0 cyFieldSample_0(uint device* image_words_9, float x_3, float y_2, float z_3)
{
    thread CyFieldSample_0 answer_0;
    float4 _S44 = float4(0.0, 0.0, 0.0, 0.0);

#line 537
    (&answer_0)->value_0 = _S44;
    (&answer_0)->resolved_0 = false;
    if(!cyFieldIsImage_0(image_words_9))
    {
        return answer_0;
    }

#line 541
    CyFieldHeader_0 _S45 = cyFieldReadHeader_0(image_words_9);

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
        (&answer_0)->value_0[index_4] = _S45.defaults_0[index_4];

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
        (&answer_0)->value_0 = base_2;
        return answer_0;
    }
    (&answer_0)->value_0 = cyFieldCombine_0(_S45.rule_0, base_2, delta_1, _S45.components_0);
    return answer_0;
}


#line 35 "../../../src/rendering/shaders/cy/cloud_shadow.slang"
float cyCloudShadowAtImage_0(uint device* image_words_10, float x_4, float y_3, float z_4)
{

#line 35
    CyFieldSample_0 _S52 = cyFieldSample_0(image_words_10, x_4, y_3, z_4);

#line 41
    return saturate(_S52.value_0.x);
}


#line 220 "water.slang"
float sunThroughClouds_0(float3 world_1, KernelContext_0 thread* kernelContext_0)
{
    float4 placement_1 = kernelContext_0->cloudShadow_0->placement_0[int(0)];
    if((placement_1.z) < 0.5)
    {
        return 1.0;
    }

#line 225
    float _S53 = cyCloudShadowAtImage_0(kernelContext_0->cloudShadow_0->field_0, world_1.x + placement_1.x, world_1.y, world_1.z + placement_1.y);



    return _S53;
}


#line 10027 "hlsl.meta.slang"
float3 worldAt_0(WaterParams_0 device* _S54, int _S55, float2 _S56, float _S57)
{

#line 10027
    WaterParams_0 device* _S58 = _S54+_S55;

#line 264 "water.slang"
    float4 _S59 = float4(_S56.x * 2.0 / _S58->viewport_0.x - 1.0, 1.0 - _S56.y * 2.0 / _S58->viewport_0.y, _S57, 1.0);



    return float3(dot(_S58->inverse0_0, _S59), dot(_S58->inverse1_0, _S59), dot(_S58->inverse2_0, _S59)) / float3(dot(_S58->inverse3_0, _S59)) ;
}


#line 9043 "hlsl.meta.slang"
int2 clampPixel_0(WaterParams_0 device* _S60, int _S61, int2 _S62)
{

#line 273 "water.slang"
    return clamp(_S62, int2(int(0), int(0)), int2((_S60+_S61)->viewport_0.xy) - int2(int(1), int(1)));
}


#line 214
float3 untonemap_0(float3 display_0)
{
    float3 _S63 = min(pow(saturate(display_0), float3(2.20000004768371582) ), float3(0.99900001287460327) );
    return _S63 / (float3(1.0)  - _S63);
}


#line 7394 "hlsl.meta.slang"
float causticFocus_0(WaterParams_0 device* _S64, int _S65, float2 _S66, float _S67, float _S68)
{

#line 7394
    WaterParams_0 device* _S69 = _S64+_S65;

#line 7394
    float4 _S70 = _S69->caustic_0;

#line 291 "water.slang"
    uint _S71 = min(uint(_S69->caustic_0.x), 16U);

#line 291
    uint index_5 = 0U;

#line 291
    float laplacian_0 = 0.0;
    for(;;)
    {

#line 292
        if(index_5 < _S71)
        {
        }
        else
        {

#line 292
            break;
        }


        float _S72 = _S69->trains_0[index_5].z;

        float laplacian_1 = laplacian_0 - saturate(2.0 - _S72 * _S68 * 0.63661974668502808) * _S69->trains_0[index_5].w * _S72 * _S72 * cos(dot(_S69->trains_0[index_5].xy, _S66) * _S72 + _S69->phases_0[index_5 / 4U][index_5 % 4U]);

#line 292
        index_5 = index_5 + 1U;

#line 292
        laplacian_0 = laplacian_1;

#line 292
    }

#line 301
    return min(1.0 / max(abs(1.0 + _S67 * _S69->surface_0.w * laplacian_0), 0.00009999999747379), _S70.y);
}


#line 15 "../../../src/rendering/shaders/cy/noise.slang"
uint3 hashPcg3d_0(uint3 value_5)
{
    uint3 _S73 = value_5 * uint3(1664525U)  + uint3(1013904223U) ;

#line 17
    thread uint3 v_1 = _S73;
    v_1.x = v_1.x + _S73.y * _S73.z;
    v_1.y = v_1.y + v_1.z * v_1.x;
    v_1.z = v_1.z + v_1.x * v_1.y;
    uint3 _S74 = v_1 ^ (v_1 >> (uint3(16U) ));

#line 21
    v_1 = _S74;
    v_1.x = v_1.x + _S74.y * _S74.z;
    v_1.y = v_1.y + v_1.z * v_1.x;
    v_1.z = v_1.z + v_1.x * v_1.y;
    return v_1;
}

)cy_msl"
    R"cy_msl(
#line 36
float valueNoise_0(float3 position_0)
{
    float3 _S75 = floor(position_0);
    float3 _S76 = position_0 - _S75;
    float3 _S77 = _S76 * _S76 * (float3(3.0)  - float3(2.0)  * _S76);
    uint3 _S78 = uint3(int3(_S75) + int3(int(1024)) );

#line 41
    uint corner_0 = 0U;

#line 41
    float result_1 = 0.0;


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
        uint3 _S79 = uint3(corner_0 & 1U, (corner_0 >> 1U) & 1U, (corner_0 >> 2U) & 1U);

        float3 _S80 = mix(float3(1.0)  - _S77, _S77, float3(_S79));
        float result_2 = result_1 + float((hashPcg3d_0(_S78 + _S79).x) >> 8U) * 5.9604644775390625e-08 * _S80.x * _S80.y * _S80.z;

#line 44
        corner_0 = corner_0 + 1U;

#line 44
        result_1 = result_2;

#line 44
    }

#line 51
    return result_1;
}


#line 306 "water.slang"
float foamBreakup_0(float2 xz_0, float seconds_0)
{


    return 0.5 + 0.5 * saturate(valueNoise_0(float3(xz_0 * float2(0.34999999403953552) , seconds_0 * 0.15000000596046448)) * 0.64999997615814209 + valueNoise_0(float3(xz_0 * float2(1.29999995231628418)  + float2(17.0) , seconds_0 * 0.40000000596046448)) * 0.34999999403953552);
}


#line 206
float3 tonemap_0(float3 radiance_0)
{

    return pow(saturate(radiance_0 / (float3(1.0)  + radiance_0)), float3(0.45454543828964233) );
}


#line 319
float4 shadeWater_0(const VertexOutput_0 thread* input_0, float4 device* table_words_0, KernelContext_0 thread* kernelContext_1)
{

#line 319
    WaterParams_0 device* _S81 = kernelContext_1->water_0->params_0;

#line 319
    WaterParams_0 device* _S82 = kernelContext_1->water_0->params_0+int(0);



    float3 normal_1 = normalize(input_0->normal_0);

#line 323
    float3 _S83 = input_0->world_0;
    float3 _S84 = normalize(kernelContext_1->push_0->eye_0.xyz - input_0->world_0);

#line 324
    float3 normal_2;
    if((dot(normal_1, _S84)) < 0.0)
    {

#line 325
        normal_2 = - normal_1;

#line 325
    }
    else
    {

#line 325
        normal_2 = normal_1;

#line 325
    }

#line 330
    float3 _S85 = kernelContext_1->push_0->sun_0.xyz;

#line 330
    float _S86 = sunThroughClouds_0(_S83, kernelContext_1);

#line 330
    float3 _S87 = _S85 * float3(_S86) ;
    float _S88 = saturate(dot(normal_2, - kernelContext_1->push_0->light_0.xyz));


    float3 _S89 = kernelContext_1->push_0->ambient_0.xyz + _S87 * float3(saturate(- kernelContext_1->push_0->light_0.y)) ;

#line 334
    float4 _S90 = input_0->clip_0;

#line 340
    int2 _S91 = int2(input_0->clip_0.xy);
    int3 _S92 = int3(_S91, int(0));

#line 341
    float _S93 = ((kernelContext_1->water_0->refractionDepth_0).read(vec<uint,2>(((_S92)).xy), uint(((_S92)).z)).x);
    bool _S94 = _S93 > 0.0;

#line 342
    float2 _S95 = float2(0.5) ;

#line 342
    float3 _S96 = worldAt_0(_S81, int(0), float2(_S91) + _S95, _S93);

#line 342
    float shore_0;

    if(_S94)
    {

#line 344
        shore_0 = max(_S83.y - _S96.y, 0.0);

#line 344
    }
    else
    {

#line 344
        shore_0 = 10000.0;

#line 344
    }

    float2 _S97 = _S96.xz;

#line 346
    float _S98 = max(length(dfdx(_S97)), length(dfdy(_S97)));

    float2 _S99 = normal_2.xz;

#line 348
    float4 _S100 = _S82->viewport_0;

#line 348
    int2 _S101 = clampPixel_0(_S81, int(0), _S91 + int2(round(_S99 * float2(_S82->viewport_0.z)  * float2(saturate(shore_0 * 0.5)) )));

    int3 _S102 = int3(_S101, int(0));

#line 350
    float bentDepth_0 = ((kernelContext_1->water_0->refractionDepth_0).read(vec<uint,2>(((_S102)).xy), uint(((_S102)).z)).x);

#line 350
    bool _S103;
    if(bentDepth_0 > (_S90.z))
    {

#line 351
        _S103 = true;

#line 351
    }
    else
    {

#line 351
        if(_S94)
        {

#line 351
            _S103 = bentDepth_0 <= 0.0;

#line 351
        }
        else
        {

#line 351
            _S103 = false;

#line 351
        }

#line 351
    }

#line 351
    float bentDepth_1;

#line 351
    int2 bentPixel_0;

#line 351
    if(_S103)
    {

#line 351
        bentPixel_0 = _S91;

#line 351
        bentDepth_1 = _S93;

#line 351
    }
    else
    {

#line 351
        bentPixel_0 = _S101;

#line 351
        bentDepth_1 = bentDepth_0;

#line 351
    }

#line 351
    float3 _S104 = worldAt_0(_S81, int(0), float2(bentPixel_0) + _S95, bentDepth_1);

#line 357
    bool _S105 = bentDepth_1 > 0.0;

#line 357
    if(_S105)
    {

#line 357
        bentDepth_1 = length(_S104 - _S83);

#line 357
    }
    else
    {

#line 357
        bentDepth_1 = 10000.0;

#line 357
    }

#line 357
    float4 _S106 = _S82->extinction_0;



    float3 _S107 = exp(- _S82->extinction_0.xyz * float3(bentDepth_1) );
    int3 _S108 = int3(bentPixel_0, int(0));

#line 362
    float3 bedRadiance_0 = untonemap_0(((kernelContext_1->water_0->refraction_0).read(vec<uint,2>(((_S108)).xy), uint(((_S108)).z))).xyz);

#line 362
    float4 _S109 = _S82->surface_0;

#line 367
    float _S110 = _S82->surface_0.z;

#line 367
    if(_S110 > 0.0)
    {

#line 367
        _S103 = _S105;

#line 367
    }
    else
    {

#line 367
        _S103 = false;

#line 367
    }

#line 367
    float3 bedRadiance_1;

#line 367
    if(_S103)
    {


)cy_msl"
    R"cy_msl(        float _S111 = max(_S109.x - _S104.y, 0.0);
        float3 _S112 = _S87 * float3(saturate(- kernelContext_1->push_0->light_0.y)) ;
        float3 _S113 = float3(1.0) ;

#line 373
        float _S114 = dot(_S112, _S113) / max(dot(_S112 + kernelContext_1->push_0->ambient_0.xyz, _S113), 9.99999997475242708e-07);
        float _S115 = exp(- _S106.y * _S111);

#line 374
        float _S116 = causticFocus_0(_S81, int(0), _S104.xz, _S111, _S98);

#line 374
        bedRadiance_1 = bedRadiance_0 * float3((1.0 + _S110 * _S114 * _S115 * (_S116 - 1.0))) ;

#line 367
    }
    else
    {

#line 367
        bedRadiance_1 = bedRadiance_0;

#line 367
    }

#line 367
    float4 _S117 = _S82->inScatter_0;

#line 379
    float3 _S118 = bedRadiance_1 * _S107 + (float3(1.0)  - _S107) * _S82->inScatter_0.xyz * _S89;

#line 379
    int2 _S119 = clampPixel_0(_S81, int(0), _S91 + int2(round(_S99 * float2(_S100.w) )));



    int3 _S120 = int3(_S119, int(0));
    float _S121 = _S117.w;

#line 391
    float3 lit_0 = mix(_S118, untonemap_0(((kernelContext_1->water_0->reflection_0).read(vec<uint,2>(((_S120)).xy), uint(((_S120)).z))).xyz), float3((_S121 + (1.0 - _S121) * pow(1.0 - saturate(dot(normal_2, _S84)), 5.0))) ) + _S87 * float3(pow(saturate(dot(normal_2, normalize(- kernelContext_1->push_0->light_0.xyz + _S84))), 120.0))  * float3(kernelContext_1->push_0->sun_0.w) ;

#line 396
    float _S122 = _S106.w;

#line 396
    if(_S122 > 0.0)
    {

#line 396
        _S103 = _S94;

#line 396
    }
    else
    {

#line 396
        _S103 = false;

#line 396
    }

#line 396
    if(_S103)
    {

#line 396
        shore_0 = saturate(1.0 - shore_0 / _S122) * foamBreakup_0(_S83.xz, _S109.y);

#line 396
    }
    else
    {

#line 396
        shore_0 = 0.0;

#line 396
    }

#line 402
    float _S123 = max(shore_0, saturate((input_0->color_0.x - 0.01999999955296516) / 0.82999998331069946));
    float3 _S124 = float3(0.89999997615814209)  * (kernelContext_1->push_0->ambient_0.xyz + _S87 * float3(_S88) );

#line 412
    return float4(tonemap_0(mix(lit_0, _S124, float3(_S123) )), 1.0);
}


#line 42 "../../../src/rendering/shaders/cy/aerial_perspective.slang"
struct CyAerialPerspective_0
{
    float3 transmittance_0;
    float3 inScattering_0;
};


#line 63
float cyAerialSliceDepth_0(float4 shape_0, float4 planes_0, float slice_0)
{
    float _S125 = shape_0.z;
    return mix(planes_0.x, planes_0.y, pow(min(slice_0 + 1.0, _S125) / _S125, max(shape_0.w, 1.0)));
}

float3 cyAerialFetch_0(float4 device* table_words_1, float4 shape_1, uint slice_1, uint x_5, uint y_4, uint which_0)
{

    uint width_0 = uint(shape_1.x);


    return table_words_1[5U + (slice_1 * uint(shape_1.y) * width_0 + y_4 * width_0 + x_5) * 2U + which_0].xyz;
}

float3 cyAerialPlane_0(float4 device* table_words_2, float4 shape_2, uint slice_2, float2 texel_0, uint which_1)
{

    float _S126 = shape_2.x;
    float _S127 = shape_2.y;
    float2 clamped_0 = clamp(texel_0, float2(0.0) , float2(_S126 - 1.0, _S127 - 1.0));
    float _S128 = clamped_0.x;

#line 84
    uint x0_0 = uint(_S128);
    float _S129 = clamped_0.y;

#line 85
    uint y0_0 = uint(_S129);
    uint _S130 = min(x0_0 + 1U, uint(_S126) - 1U);
    uint _S131 = min(y0_0 + 1U, uint(_S127) - 1U);



    float3 _S132 = float3((_S128 - float(x0_0))) ;


    return mix(mix(cyAerialFetch_0(table_words_2, shape_2, slice_2, x0_0, y0_0, which_1), cyAerialFetch_0(table_words_2, shape_2, slice_2, _S130, y0_0, which_1), _S132), mix(cyAerialFetch_0(table_words_2, shape_2, slice_2, x0_0, _S131, which_1), cyAerialFetch_0(table_words_2, shape_2, slice_2, _S130, _S131, which_1), _S132), float3((_S129 - float(y0_0))) );
}



CyAerialPerspective_0 cyAerialPerspectiveAt_0(float4 device* table_words_3, float3 offset_1)
{
    thread CyAerialPerspective_0 result_3;
    float3 _S133 = float3(1.0) ;

#line 102
    (&result_3)->transmittance_0 = _S133;
    float3 _S134 = float3(0.0) ;

#line 103
    (&result_3)->inScattering_0 = _S134;

    float4 forward_0 = table_words_3[int(0)];
    float4 right_0 = table_words_3[int(1)];
    float4 up_0 = table_words_3[int(2)];
    float4 shape_3 = table_words_3[int(3)];
    float4 planes_1 = table_words_3[int(4)];
    float depth_0 = dot(offset_1, forward_0.xyz);

#line 110
    bool _S135;
    if((forward_0.w) < 0.5)
    {

#line 111
        _S135 = true;

#line 111
    }
    else
    {

#line 111
        _S135 = depth_0 <= 0.0;

#line 111
    }

#line 111
    if(_S135)
    {
        return result_3;
    }



    float2 texel_1 = float2((dot(offset_1, right_0.xyz) / (depth_0 * right_0.w) * 0.5 + 0.5) * shape_3.x - 0.5, (dot(offset_1, up_0.xyz) / (depth_0 * up_0.w) * 0.5 + 0.5) * shape_3.y - 0.5);



    float _S136 = shape_3.z;

#line 122
    uint last_0 = uint(_S136) - 1U;
    float first_0 = cyAerialSliceDepth_0(shape_3, planes_1, 0.0);



    float _S137 = saturate(depth_0 / max(first_0, 9.99999997475242708e-07));

#line 127
    float fraction_0;

#line 127
    uint farSlice_0;

#line 127
    float3 nearT_0;

#line 127
    float3 nearS_0;
    if(depth_0 > first_0)
    {
        float _S138 = planes_1.x;


        uint _S139 = min(uint(max(pow(saturate((depth_0 - _S138) / max(planes_1.y - _S138, 9.99999997475242708e-07)), 1.0 / max(shape_3.w, 1.0)) * _S136 - 1.0, 0.0)), last_0);

#line 133
        float3 _S140 = cyAerialPlane_0(table_words_3, shape_3, _S139, texel_1, 0U);

#line 133
        float3 _S141 = cyAerialPlane_0(table_words_3, shape_3, _S139, texel_1, 1U);


        uint _S142 = _S139 + 1U;

#line 136
        uint _S143 = min(_S142, last_0);
        float nearEdge_0 = cyAerialSliceDepth_0(shape_3, planes_1, float(_S139));
        float farEdge_0 = cyAerialSliceDepth_0(shape_3, planes_1, float(_S142));
        if(_S139 == last_0)
        {

#line 139
            fraction_0 = 1.0;

#line 139
        }
        else
        {

#line 139
            fraction_0 = saturate((depth_0 - nearEdge_0) / max(farEdge_0 - nearEdge_0, 9.99999997475242708e-07));

#line 139
        }

#line 139
        farSlice_0 = _S143;

#line 139
        nearT_0 = _S140;

#line 139
        nearS_0 = _S141;

#line 128
    }
    else
    {

#line 128
        farSlice_0 = 0U;

#line 128
        nearT_0 = _S133;

#line 128
        fraction_0 = _S137;

#line 128
        nearS_0 = _S134;

#line 128
    }

#line 128
    float3 _S144 = cyAerialPlane_0(table_words_3, shape_3, farSlice_0, texel_1, 1U);

#line 143
    float3 _S145 = float3(fraction_0) ;

#line 143
    (&result_3)->transmittance_0 = mix(nearT_0, cyAerialPlane_0(table_words_3, shape_3, farSlice_0, texel_1, 0U), _S145);
    (&result_3)->inScattering_0 = mix(nearS_0, _S144, _S145);
    return result_3;
}


#line 241 "water.slang"
float3 bedBeforeAir_0(float4 device* table_words_4, float3 stored_0, float3 bed_0, KernelContext_0 thread* kernelContext_2)
{

#line 241
    CyAerialPerspective_0 _S146 = cyAerialPerspectiveAt_0(table_words_4, bed_0 - kernelContext_2->push_0->eye_0.xyz);


    return max(stored_0 - _S146.inScattering_0, float3(0.0) ) / max(_S146.transmittance_0, float3(0.00009999999747379) );
}

)cy_msl"
    R"cy_msl(

float3 seenThroughAir_0(float4 device* table_words_5, float3 world_2, float3 refracted_0, float3 reflected_0, float fresnel_0, float3 glitter_0, float3 foamRadiance_0, float foam_0, KernelContext_0 thread* kernelContext_3)
{
    CyAerialPerspective_0 _S147 = cyAerialPerspectiveAt_0(table_words_5, world_2 - kernelContext_3->push_0->eye_0.xyz);

#line 251
    float3 _S148 = float3((1.0 - fresnel_0)) ;

#line 258
    return mix(float3(fresnel_0)  * reflected_0 + (_S148 * refracted_0 + glitter_0) * _S147.transmittance_0 + _S148 * _S147.inScattering_0, foamRadiance_0 * _S147.transmittance_0 + _S147.inScattering_0, float3(foam_0) );
}


#line 319
float4 shadeWater_1(const VertexOutput_0 thread* input_1, float4 device* table_words_6, KernelContext_0 thread* kernelContext_4)
{

#line 319
    WaterParams_0 device* _S149 = kernelContext_4->water_0->params_0;

#line 319
    WaterParams_0 device* _S150 = kernelContext_4->water_0->params_0+int(0);



    float3 normal_3 = normalize(input_1->normal_0);

#line 323
    float3 _S151 = input_1->world_0;
    float3 _S152 = normalize(kernelContext_4->push_0->eye_0.xyz - input_1->world_0);

#line 324
    float3 normal_4;
    if((dot(normal_3, _S152)) < 0.0)
    {

#line 325
        normal_4 = - normal_3;

#line 325
    }
    else
    {

#line 325
        normal_4 = normal_3;

#line 325
    }

#line 330
    float3 _S153 = kernelContext_4->push_0->sun_0.xyz;

#line 330
    float _S154 = sunThroughClouds_0(_S151, kernelContext_4);

#line 330
    float3 _S155 = _S153 * float3(_S154) ;
    float _S156 = saturate(dot(normal_4, - kernelContext_4->push_0->light_0.xyz));


    float3 _S157 = kernelContext_4->push_0->ambient_0.xyz + _S155 * float3(saturate(- kernelContext_4->push_0->light_0.y)) ;

#line 334
    float4 _S158 = input_1->clip_0;

#line 340
    int2 _S159 = int2(input_1->clip_0.xy);
    int3 _S160 = int3(_S159, int(0));

#line 341
    float _S161 = ((kernelContext_4->water_0->refractionDepth_0).read(vec<uint,2>(((_S160)).xy), uint(((_S160)).z)).x);
    bool _S162 = _S161 > 0.0;

#line 342
    float2 _S163 = float2(0.5) ;

#line 342
    float3 _S164 = worldAt_0(_S149, int(0), float2(_S159) + _S163, _S161);

#line 342
    float shore_1;

    if(_S162)
    {

#line 344
        shore_1 = max(_S151.y - _S164.y, 0.0);

#line 344
    }
    else
    {

#line 344
        shore_1 = 10000.0;

#line 344
    }

    float2 _S165 = _S164.xz;

#line 346
    float _S166 = max(length(dfdx(_S165)), length(dfdy(_S165)));

    float2 _S167 = normal_4.xz;

#line 348
    float4 _S168 = _S150->viewport_0;

#line 348
    int2 _S169 = clampPixel_0(_S149, int(0), _S159 + int2(round(_S167 * float2(_S150->viewport_0.z)  * float2(saturate(shore_1 * 0.5)) )));

    int3 _S170 = int3(_S169, int(0));

#line 350
    float bentDepth_2 = ((kernelContext_4->water_0->refractionDepth_0).read(vec<uint,2>(((_S170)).xy), uint(((_S170)).z)).x);

#line 350
    bool _S171;
    if(bentDepth_2 > (_S158.z))
    {

#line 351
        _S171 = true;

#line 351
    }
    else
    {

#line 351
        if(_S162)
        {

#line 351
            _S171 = bentDepth_2 <= 0.0;

#line 351
        }
        else
        {

#line 351
            _S171 = false;

#line 351
        }

#line 351
    }

#line 351
    float bentDepth_3;

#line 351
    int2 bentPixel_1;

#line 351
    if(_S171)
    {

#line 351
        bentPixel_1 = _S159;

#line 351
        bentDepth_3 = _S161;

#line 351
    }
    else
    {

#line 351
        bentPixel_1 = _S169;

#line 351
        bentDepth_3 = bentDepth_2;

#line 351
    }

#line 351
    float3 _S172 = worldAt_0(_S149, int(0), float2(bentPixel_1) + _S163, bentDepth_3);

#line 357
    bool _S173 = bentDepth_3 > 0.0;

#line 357
    if(_S173)
    {

#line 357
        bentDepth_3 = length(_S172 - _S151);

#line 357
    }
    else
    {

#line 357
        bentDepth_3 = 10000.0;

#line 357
    }

#line 357
    float4 _S174 = _S150->extinction_0;



    float3 _S175 = exp(- _S150->extinction_0.xyz * float3(bentDepth_3) );
    int3 _S176 = int3(bentPixel_1, int(0));

#line 362
    float3 bedRadiance_2 = untonemap_0(((kernelContext_4->water_0->refraction_0).read(vec<uint,2>(((_S176)).xy), uint(((_S176)).z))).xyz);

#line 362
    float3 bedRadiance_3;
    if(_S173)
    {

#line 363
        float3 _S177 = bedBeforeAir_0(table_words_6, bedRadiance_2, _S172, kernelContext_4);

#line 363
        bedRadiance_3 = _S177;

#line 363
    }
    else
    {

#line 363
        bedRadiance_3 = bedRadiance_2;

#line 363
    }

#line 363
    float4 _S178 = _S150->surface_0;



    float _S179 = _S150->surface_0.z;

#line 367
    if(_S179 > 0.0)
    {

#line 367
        _S171 = _S173;

#line 367
    }
    else
    {

#line 367
        _S171 = false;

#line 367
    }

#line 367
    if(_S171)
    {


        float _S180 = max(_S178.x - _S172.y, 0.0);
        float3 _S181 = _S155 * float3(saturate(- kernelContext_4->push_0->light_0.y)) ;
        float3 _S182 = float3(1.0) ;

#line 373
        float _S183 = dot(_S181, _S182) / max(dot(_S181 + kernelContext_4->push_0->ambient_0.xyz, _S182), 9.99999997475242708e-07);
        float _S184 = exp(- _S174.y * _S180);

#line 374
        float _S185 = causticFocus_0(_S149, int(0), _S172.xz, _S180, _S166);

#line 374
        bedRadiance_3 = bedRadiance_3 * float3((1.0 + _S179 * _S183 * _S184 * (_S185 - 1.0))) ;

#line 367
    }

#line 367
    float4 _S186 = _S150->inScatter_0;

#line 379
    float3 _S187 = bedRadiance_3 * _S175 + (float3(1.0)  - _S175) * _S150->inScatter_0.xyz * _S157;

#line 379
    int2 _S188 = clampPixel_0(_S149, int(0), _S159 + int2(round(_S167 * float2(_S168.w) )));



    int3 _S189 = int3(_S188, int(0));

#line 383
    float3 _S190 = untonemap_0(((kernelContext_4->water_0->reflection_0).read(vec<uint,2>(((_S189)).xy), uint(((_S189)).z))).xyz);
    float _S191 = _S186.w;
    float _S192 = _S191 + (1.0 - _S191) * pow(1.0 - saturate(dot(normal_4, _S152)), 5.0);

)cy_msl"
    R"cy_msl(#line 390
    float3 _S193 = _S155 * float3(pow(saturate(dot(normal_4, normalize(- kernelContext_4->push_0->light_0.xyz + _S152))), 120.0))  * float3(kernelContext_4->push_0->sun_0.w) ;

#line 396
    float _S194 = _S174.w;

#line 396
    if(_S194 > 0.0)
    {

#line 396
        _S171 = _S162;

#line 396
    }
    else
    {

#line 396
        _S171 = false;

#line 396
    }

#line 396
    if(_S171)
    {

#line 396
        shore_1 = saturate(1.0 - shore_1 / _S194) * foamBreakup_0(_S151.xz, _S178.y);

#line 396
    }
    else
    {

#line 396
        shore_1 = 0.0;

#line 396
    }

#line 402
    float _S195 = max(shore_1, saturate((input_1->color_0.x - 0.01999999955296516) / 0.82999998331069946));
    float3 _S196 = float3(0.89999997615814209)  * (kernelContext_4->push_0->ambient_0.xyz + _S155 * float3(_S156) );

#line 403
    float3 _S197 = seenThroughAir_0(table_words_6, _S151, _S187, _S190, _S192, _S193, _S196, _S195, kernelContext_4);


    return float4(tonemap_0(_S197), 1.0);
}


#line 232
void aerialTable_0(float4 device* thread* _S198, KernelContext_0 thread* kernelContext_5)
{

#line 232
    *_S198 = kernelContext_5->cloudShadow_0->aerial_0;

#line 232
    return;
}


#line 58 "../../../src/rendering/shaders/cy/aerial_perspective.slang"
bool cyAerialPerspectiveEnabled_0(float4 device* table_words_7)
{
    return (table_words_7[int(0)].w) > 0.5;
}


#line 60
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 60
struct pixelInput_0
{
    float3 world_3 [[user(TEXCOORD)]];
    float3 normal_5 [[user(TEXCOORD_1)]];
    float3 color_1 [[user(TEXCOORD_2)]];
};


#line 416 "water.slang"
[[fragment]] pixelOutput_0 waterFragment(pixelInput_0 _S199 [[stage_in]], float4 clip_1 [[position]], WaterCloudShadow_default_0 constant* cloudShadow_1 [[buffer(0)]], WaterSurface_default_0 constant* water_1 [[buffer(1)]], WorldPush_0 constant* push_1 [[buffer(2)]])
{

#line 416
    thread KernelContext_0 kernelContext_6;

#line 416
    (&kernelContext_6)->cloudShadow_0 = cloudShadow_1;

#line 416
    (&kernelContext_6)->water_0 = water_1;

#line 416
    (&kernelContext_6)->push_0 = push_1;

#line 416
    thread float4 device* _S200;

#line 416
    aerialTable_0(&_S200, &kernelContext_6);

#line 416
    float4 device* _S201 = _S200;


    if(cyAerialPerspectiveEnabled_0(_S200))
    {

#line 419
        thread VertexOutput_0 _S202;

#line 419
        (&_S202)->clip_0 = clip_1;

#line 419
        (&_S202)->world_0 = _S199.world_3;

#line 419
        (&_S202)->normal_0 = _S199.normal_5;

#line 419
        (&_S202)->color_0 = _S199.color_1;

#line 419
        float4 _S203 = shadeWater_1(&_S202, _S201, &kernelContext_6);

#line 419
        pixelOutput_0 _S204 = { _S203 };

        return _S204;
    }

#line 421
    thread VertexOutput_0 _S205;

#line 421
    (&_S205)->clip_0 = clip_1;

#line 421
    (&_S205)->world_0 = _S199.world_3;

#line 421
    (&_S205)->normal_0 = _S199.normal_5;

#line 421
    (&_S205)->color_0 = _S199.color_1;

#line 421
    float4 _S206 = shadeWater_0(&_S205, _S201, &kernelContext_6);

#line 421
    pixelOutput_0 _S207 = { _S206 };

    return _S207;
}

)cy_msl";

}  // namespace cy::sample::world

#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the volumetric fog march. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::fog {

/// volumetric_fog.metal, 12414 bytes.
inline constexpr char kVolumetricFogMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 5163 "hlsl.meta.slang"
struct CyFogSet_default_0
{
    texture2d<float, access::sample> shadowMap_0;
    float4 device* words_0;
    texture2d<float, access::read_write> output_0;
};


#line 5163
struct KernelContext_0
{
    CyFogSet_default_0 constant* cyFogSet_0;
};


#line 88 "src/rendering/fog/shaders/volumetric_fog.slang"
float4 fogWord_0(uint index_0, KernelContext_0 thread* kernelContext_0)
{
    return kernelContext_0->cyFogSet_0->words_0[index_0];
}


#line 88
float4 fogWord_1(uint index_1, KernelContext_0 thread* kernelContext_1)
{
    return kernelContext_1->cyFogSet_0->words_0[index_1];
}


float fogSliceDepth_0(float4 shape_0, float4 planes_0, uint slice_0)
{
    float _S1 = shape_0.z;
    return mix(planes_0.x, planes_0.y, pow(min(float(slice_0) + 1.0, _S1) / _S1, max(shape_0.w, 1.0)));
}


float fogPhase_0(float g_0, float cosTheta_0)
{
    float clamped_0 = clamp(g_0, -0.99000000953674316, 0.99000000953674316);
    float squared_0 = clamped_0 * clamped_0;
    float denominator_0 = 1.0 + squared_0 - 2.0 * clamped_0 * cosTheta_0;
    return (1.0 - squared_0) / (12.56637096405029297 * denominator_0 * sqrt(max(denominator_0, 9.99999997475242708e-07)));
}


#line 130
struct FogMediumSample_0
{
    float extinction_0;
    float3 scattering_0;
    float3 sunScattering_0;
    float3 emission_0;
};

void fogAddMedium_0(FogMediumSample_0 thread* sample_0, float extinction_1, float3 albedo_0, float g_1, float3 emission_1, float cosToSun_0)
{

    sample_0->emission_0 = sample_0->emission_0 + emission_1;
    if(extinction_1 <= 0.0)
    {
        return;
    }
    float3 scattering_1 = albedo_0 * float3(extinction_1) ;
    sample_0->extinction_0 = sample_0->extinction_0 + extinction_1;
    sample_0->scattering_0 = sample_0->scattering_0 + scattering_1;
    sample_0->sunScattering_0 = sample_0->sunScattering_0 + scattering_1 * float3(fogPhase_0(g_1, cosToSun_0)) ;
    return;
}


#line 110
float fogInsideDistance_0(float shape_1, float3 size_0, float3 local_0)
{
    float _S2 = local_0.x;

#line 112
    float _S3 = local_0.z;

#line 112
    float radial_0 = sqrt(_S2 * _S2 + _S3 * _S3);
    if(shape_1 < 0.5)
    {
        return min(size_0.x - abs(_S2), min(size_0.y - abs(local_0.y), size_0.z - abs(_S3)));
    }
    if(shape_1 < 1.5)
    {
        return size_0.x - length(local_0);
    }
    if(shape_1 < 2.5)
    {
        return min(size_0.x - radial_0, size_0.y - abs(local_0.y));
    }
    float _S4 = max(size_0.y, 9.99999997475242708e-07);
    float _S5 = local_0.y;
    return min(min(_S5, _S4 - _S5), size_0.x * (1.0 - _S5 / _S4) - radial_0);
}


#line 153
FogMediumSample_0 fogSampleMedium_0(float3 world_0, float cosToSun_1, KernelContext_0 thread* kernelContext_2)
{
    thread FogMediumSample_0 sample_1;
    (&sample_1)->extinction_0 = 0.0;
    float3 _S6 = float3(0.0) ;

#line 157
    (&sample_1)->scattering_0 = _S6;
    (&sample_1)->sunScattering_0 = _S6;
    (&sample_1)->emission_0 = _S6;

#line 159
    float4 _S7 = fogWord_0(15U, kernelContext_2);


    float _S8 = _S7.x;

#line 162
    if(_S8 > 0.0)
    {

        float extinction_2 = _S8 * exp(- max(world_0.y - _S7.y, 0.0) / max(_S7.z, 0.00100000004749745));

#line 165
        float4 _S9 = fogWord_0(16U, kernelContext_2);
        fogAddMedium_0(&sample_1, extinction_2, _S9.xyz, _S7.w, _S6, cosToSun_1);

#line 162
    }

#line 162
    float4 _S10 = fogWord_0(6U, kernelContext_2);

#line 168
    uint _S11 = min(uint(_S10.w), 8U);

#line 168
    uint index_2 = 0U;
    for(;;)
    {

#line 169
        if(index_2 < _S11)
        {
        }
        else
        {

#line 169
            break;
        }
        uint base_0 = 17U + index_2 * 4U;

#line 171
        float4 _S12 = fogWord_0(base_0, kernelContext_2);

#line 171
        float4 _S13 = fogWord_0(base_0 + 1U, kernelContext_2);

#line 171
        float4 _S14 = fogWord_0(base_0 + 2U, kernelContext_2);

#line 171
        float4 _S15 = fogWord_0(base_0 + 3U, kernelContext_2);

#line 176
        float inside_0 = fogInsideDistance_0(_S12.w, _S13.xyz, world_0 - _S12.xyz);
        if(inside_0 <= 0.0)
        {
            index_2 = index_2 + 1U;

#line 169
            continue;
        }

#line 181
        float _S16 = _S13.w;

#line 181
        float coverage_0;

#line 181
        if(_S16 <= 0.0)
        {

#line 181
            coverage_0 = 1.0;

#line 181
        }
        else
        {

#line 181
            coverage_0 = saturate(inside_0 / _S16);

#line 181
        }
        fogAddMedium_0(&sample_1, _S14.w * coverage_0, _S14.xyz, _S15.w, _S15.xyz * float3(coverage_0) , cosToSun_1);

#line 169
        index_2 = index_2 + 1U;

#line 169
    }

#line 184
    return sample_1;
}

float fogLit_0(float reference_0, int2 texel_0, int extent_0, KernelContext_0 thread* kernelContext_3)
{
    int _S17 = extent_0 - int(1);
    int3 _S18 = int3(clamp(texel_0, int2(int(0), int(0)), int2(_S17, _S17)), int(0));

#line 190
    float _S19;

#line 190
    if(reference_0 >= ((kernelContext_3->cyFogSet_0->shadowMap_0).read(vec<uint,2>(((_S18)).xy), uint(((_S18)).z)).x))
    {

#line 190
        _S19 = 1.0;

#line 190
    }
    else
    {

#line 190
        _S19 = 0.0;

#line 190
    }

#line 190
    return _S19;
}


float fogShadowVisibility_0(float3 relative_0, KernelContext_0 thread* kernelContext_4)
{

#line 194
    float4 _S20 = fogWord_1(7U, kernelContext_4);

    if((_S20.w) < 0.5)
    {
        return 1.0;
    }
    float4 p_0 = float4(relative_0, 1.0);

#line 200
    float4 _S21 = fogWord_1(10U, kernelContext_4);
    float _S22 = dot(_S21, p_0);

#line 201
    float4 _S23 = fogWord_1(11U, kernelContext_4);

#line 201
    float _S24 = dot(_S23, p_0);

#line 201
    float4 _S25 = fogWord_1(12U, kernelContext_4);

#line 201
    float _S26 = dot(_S25, p_0);

#line 201
    float4 _S27 = fogWord_1(13U, kernelContext_4);
    float _S28 = dot(_S27, p_0);
    if(_S28 <= 0.0)
    {
        return 1.0;
    }
    float2 uv_0 = float2(_S22, _S24) / float2(_S28) ;
    float _S29 = uv_0.x;

#line 208
    bool _S30;

#line 208
    if(_S29 < 0.0)
    {

#line 208
        _S30 = true;

#line 208
    }
    else
    {

#line 208
        _S30 = _S29 > 1.0;

)cy_msl"
    R"cy_msl(#line 208
    }

#line 208
    if(_S30)
    {

#line 208
        _S30 = true;

#line 208
    }
    else
    {

#line 208
        _S30 = (uv_0.y) < 0.0;

#line 208
    }

#line 208
    if(_S30)
    {

#line 208
        _S30 = true;

#line 208
    }
    else
    {

#line 208
        _S30 = (uv_0.y) > 1.0;

#line 208
    }

#line 208
    if(_S30)
    {
        return 1.0;
    }

#line 210
    float4 _S31 = fogWord_1(14U, kernelContext_4);


    float reference_1 = _S26 / _S28 + _S31.x;
    float size_1 = _S31.y;
    int extent_1 = int(size_1);
    float2 t_0 = uv_0 * float2(size_1)  - float2(0.5) ;
    float2 f_0 = floor(t_0);
    int2 _S32 = int2(f_0);
    float2 a_0 = t_0 - f_0;

#line 219
    float _S33 = fogLit_0(reference_1, _S32, extent_1, kernelContext_4);

#line 219
    float _S34 = fogLit_0(reference_1, _S32 + int2(int(1), int(0)), extent_1, kernelContext_4);

    float _S35 = a_0.x;

#line 220
    float top_0 = mix(_S33, _S34, _S35);

#line 220
    float _S36 = fogLit_0(reference_1, _S32 + int2(int(0), int(1)), extent_1, kernelContext_4);

#line 220
    float _S37 = fogLit_0(reference_1, _S32 + int2(int(1), int(1)), extent_1, kernelContext_4);



    return mix(top_0, mix(_S36, _S37, _S35), a_0.y);
}


#line 258
[[kernel]] void cyVolumetricFog(uint3 id_0 [[thread_position_in_grid]], CyFogSet_default_0 constant* cyFogSet_1 [[buffer(0)]])
{

#line 258
    thread KernelContext_0 kernelContext_5;

#line 258
    (&kernelContext_5)->cyFogSet_0 = cyFogSet_1;

#line 258
    float4 _S38 = fogWord_0(0U, &kernelContext_5);

#line 258
    float4 _S39 = fogWord_0(1U, &kernelContext_5);

#line 258
    float4 _S40 = fogWord_0(2U, &kernelContext_5);

#line 258
    float4 _S41 = fogWord_0(3U, &kernelContext_5);

#line 258
    float4 _S42 = fogWord_0(4U, &kernelContext_5);

#line 265
    uint width_0 = uint(_S41.x);
    uint height_0 = uint(_S41.y);
    uint _S43 = uint(_S41.z);
    uint _S44 = id_0.x;

#line 268
    bool _S45;

#line 268
    if(_S44 == 0U)
    {

#line 268
        _S45 = (id_0.y) == 0U;

#line 268
    }
    else
    {

#line 268
        _S45 = false;

#line 268
    }

#line 268
    uint slice_1;

#line 268
    if(_S45)
    {

#line 268
        slice_1 = 0U;

#line 279
        for(;;)
        {

#line 279
            if(slice_1 < 6U)
            {
            }
            else
            {

#line 279
                break;
            }
            uint2 _S46 = uint2(slice_1, 0U);

#line 281
            float4 _S47 = fogWord_0(slice_1, &kernelContext_5);

#line 281
            (&kernelContext_5)->cyFogSet_0->output_0.write(_S47,_S46);

#line 279
            slice_1 = slice_1 + 1U;

#line 279
        }

#line 268
    }

#line 285
    if(_S44 >= width_0)
    {

#line 285
        _S45 = true;

#line 285
    }
    else
    {

#line 285
        _S45 = (id_0.y) >= height_0;

#line 285
    }

#line 285
    if(_S45)
    {
        return;
    }


    uint _S48 = id_0.y;

    float3 _S49 = _S38.xyz;

#line 293
    float3 direction_0 = normalize(_S49 + _S39.xyz * float3((((float(_S44) + 0.5) / float(width_0) * 2.0 - 1.0) * _S39.w))  + _S40.xyz * float3((((float(_S48) + 0.5) / float(height_0) * 2.0 - 1.0) * _S40.w)) );
    float _S50 = max(dot(direction_0, _S49), 0.00100000004749745);

#line 294
    float4 _S51 = fogWord_0(7U, &kernelContext_5);

    float _S52 = dot(direction_0, _S51.xyz);

#line 296
    float4 _S53 = fogWord_0(5U, &kernelContext_5);
    float3 _S54 = _S53.xyz;

#line 297
    float4 _S55 = fogWord_0(6U, &kernelContext_5);
    float3 _S56 = _S55.xyz;

#line 298
    float4 _S57 = fogWord_0(8U, &kernelContext_5);
    float3 _S58 = _S57.xyz;

#line 299
    float4 _S59 = fogWord_0(9U, &kernelContext_5);
    float3 _S60 = _S59.xyz;
    uint _S61 = max(uint(_S42.z), 1U);

#line 310
    float3 _S62 = float3(0.0) ;

#line 310
    float previous_0 = 0.0;

#line 310
    slice_1 = 0U;

#line 310
    float carried_0 = 1.0;

#line 310
    float3 scattered_0 = _S62;
    for(;;)
    {

#line 311
        if(slice_1 < _S43)
        {
        }
        else
        {

#line 311
            break;
        }
        float distance_0 = fogSliceDepth_0(_S41, _S42, slice_1) / _S50;
        float _S63 = max(distance_0 - previous_0, 0.0) / float(_S61);

#line 314
        uint sub_0 = 0U;

#line 323
        for(;;)
        {

#line 323
            if(sub_0 < _S61)
            {
            }
            else
            {

#line 323
                break;
            }

            float3 point_0 = _S54 + direction_0 * float3((previous_0 + (float(sub_0) + 0.5) * _S63)) ;

#line 326
            FogMediumSample_0 _S64 = fogSampleMedium_0(point_0 + _S56, _S52, &kernelContext_5);

#line 326
            float _S65 = fogShadowVisibility_0(point_0, &kernelContext_5);



            float3 source_0 = _S64.sunScattering_0 * _S58 * float3(_S65)  + _S64.scattering_0 * _S60 + _S64.emission_0;

#line 344
            float extinction_3 = max(_S64.extinction_0, 0.0) * _S63;
            float sliceTransmittance_0 = exp(- extinction_3);

#line 345
            float integrated_0;

            if(extinction_3 > 9.99999997475242708e-07)
            {

#line 347
                integrated_0 = (1.0 - sliceTransmittance_0) / max(_S64.extinction_0, 9.99999997475242708e-07);

)cy_msl"
    R"cy_msl(#line 347
            }
            else
            {

#line 347
                integrated_0 = _S63;

#line 347
            }

            float3 scattered_1 = scattered_0 + source_0 * float3(integrated_0)  * float3(carried_0) ;
            float carried_1 = carried_0 * sliceTransmittance_0;

#line 323
            sub_0 = sub_0 + 1U;

#line 323
            carried_0 = carried_1;

#line 323
            scattered_0 = scattered_1;

#line 323
        }

#line 359
        uint row_0 = 1U + slice_1 * height_0 + _S48;
        (&kernelContext_5)->cyFogSet_0->output_0.write(float4(carried_0, carried_0, carried_0, 1.0),uint2(_S44, row_0));
        (&kernelContext_5)->cyFogSet_0->output_0.write(float4(scattered_0, 1.0),uint2(width_0 + _S44, row_0));

#line 311
        uint slice_2 = slice_1 + 1U;

#line 311
        previous_0 = distance_0;

#line 311
        slice_1 = slice_2;

#line 311
    }

#line 364
    return;
}

)cy_msl";

/// volumetric_fog_table.metal, 19887 bytes.
inline constexpr char kVolumetricFogTableMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 69 "src/rendering/fog/shaders/volumetric_fog.slang"
struct CyFogSet_default_0
{
    texture2d<float, access::sample> shadowMap_0;
    float4 device* words_0;
    float4 device* table_0;
    float4 device* air_0;
};


#line 69
struct KernelContext_0
{
    CyFogSet_default_0 constant* cyFogSet_0;
};


#line 88
float4 fogWord_0(uint index_0, KernelContext_0 thread* kernelContext_0)
{
    return kernelContext_0->cyFogSet_0->words_0[index_0];
}


#line 88
float4 fogWord_1(uint index_1, KernelContext_0 thread* kernelContext_1)
{
    return kernelContext_1->cyFogSet_0->words_0[index_1];
}


float fogSliceDepth_0(float4 shape_0, float4 planes_0, uint slice_0)
{
    float _S1 = shape_0.z;
    return mix(planes_0.x, planes_0.y, pow(min(float(slice_0) + 1.0, _S1) / _S1, max(shape_0.w, 1.0)));
}


#line 63 "src/rendering/shaders/cy/aerial_perspective.slang"
float cyAerialSliceDepth_0(float4 shape_1, float4 planes_1, float slice_1)
{
    float _S2 = shape_1.z;
    return mix(planes_1.x, planes_1.y, pow(min(slice_1 + 1.0, _S2) / _S2, max(shape_1.w, 1.0)));
}

float3 cyAerialFetch_0(float4 device* table_words_0, float4 shape_2, uint slice_2, uint x_0, uint y_0, uint which_0)
{

    uint width_0 = uint(shape_2.x);


    return table_words_0[5U + (slice_2 * uint(shape_2.y) * width_0 + y_0 * width_0 + x_0) * 2U + which_0].xyz;
}

float3 cyAerialPlane_0(float4 device* table_words_1, float4 shape_3, uint slice_3, float2 texel_0, uint which_1)
{

    float _S3 = shape_3.x;
    float _S4 = shape_3.y;
    float2 clamped_0 = clamp(texel_0, float2(0.0) , float2(_S3 - 1.0, _S4 - 1.0));
    float _S5 = clamped_0.x;

#line 84
    uint x0_0 = uint(_S5);
    float _S6 = clamped_0.y;

#line 85
    uint y0_0 = uint(_S6);
    uint _S7 = min(x0_0 + 1U, uint(_S3) - 1U);
    uint _S8 = min(y0_0 + 1U, uint(_S4) - 1U);



    float3 _S9 = float3((_S5 - float(x0_0))) ;


    return mix(mix(cyAerialFetch_0(table_words_1, shape_3, slice_3, x0_0, y0_0, which_1), cyAerialFetch_0(table_words_1, shape_3, slice_3, _S7, y0_0, which_1), _S9), mix(cyAerialFetch_0(table_words_1, shape_3, slice_3, x0_0, _S8, which_1), cyAerialFetch_0(table_words_1, shape_3, slice_3, _S7, _S8, which_1), _S9), float3((_S6 - float(y0_0))) );
}


#line 42
struct CyAerialPerspective_0
{
    float3 transmittance_0;
    float3 inScattering_0;
};


#line 99
CyAerialPerspective_0 cyAerialPerspectiveAt_0(float4 device* table_words_2, float3 offset_0)
{
    thread CyAerialPerspective_0 result_0;
    float3 _S10 = float3(1.0) ;

#line 102
    (&result_0)->transmittance_0 = _S10;
    float3 _S11 = float3(0.0) ;

#line 103
    (&result_0)->inScattering_0 = _S11;

    float4 forward_0 = table_words_2[int(0)];
    float4 right_0 = table_words_2[int(1)];
    float4 up_0 = table_words_2[int(2)];
    float4 shape_4 = table_words_2[int(3)];
    float4 planes_2 = table_words_2[int(4)];
    float depth_0 = dot(offset_0, forward_0.xyz);

#line 110
    bool _S12;
    if((forward_0.w) < 0.5)
    {

#line 111
        _S12 = true;

#line 111
    }
    else
    {

#line 111
        _S12 = depth_0 <= 0.0;

#line 111
    }

#line 111
    if(_S12)
    {
        return result_0;
    }



    float2 texel_1 = float2((dot(offset_0, right_0.xyz) / (depth_0 * right_0.w) * 0.5 + 0.5) * shape_4.x - 0.5, (dot(offset_0, up_0.xyz) / (depth_0 * up_0.w) * 0.5 + 0.5) * shape_4.y - 0.5);



    float _S13 = shape_4.z;

#line 122
    uint last_0 = uint(_S13) - 1U;
    float first_0 = cyAerialSliceDepth_0(shape_4, planes_2, 0.0);



    float _S14 = saturate(depth_0 / max(first_0, 9.99999997475242708e-07));

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
        float _S15 = planes_2.x;


        uint _S16 = min(uint(max(pow(saturate((depth_0 - _S15) / max(planes_2.y - _S15, 9.99999997475242708e-07)), 1.0 / max(shape_4.w, 1.0)) * _S13 - 1.0, 0.0)), last_0);

#line 133
        float3 _S17 = cyAerialPlane_0(table_words_2, shape_4, _S16, texel_1, 0U);

#line 133
        float3 _S18 = cyAerialPlane_0(table_words_2, shape_4, _S16, texel_1, 1U);


        uint _S19 = _S16 + 1U;

#line 136
        uint _S20 = min(_S19, last_0);
        float nearEdge_0 = cyAerialSliceDepth_0(shape_4, planes_2, float(_S16));
        float farEdge_0 = cyAerialSliceDepth_0(shape_4, planes_2, float(_S19));
        if(_S16 == last_0)
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
        farSlice_0 = _S20;

#line 139
        nearT_0 = _S17;

#line 139
        nearS_0 = _S18;

#line 128
    }
    else
    {

#line 128
        farSlice_0 = 0U;

#line 128
        nearT_0 = _S10;

#line 128
        fraction_0 = _S14;

#line 128
        nearS_0 = _S11;

#line 128
    }

#line 128
    float3 _S21 = cyAerialPlane_0(table_words_2, shape_4, farSlice_0, texel_1, 1U);

#line 143
    float3 _S22 = float3(fraction_0) ;

#line 143
    (&result_0)->transmittance_0 = mix(nearT_0, cyAerialPlane_0(table_words_2, shape_4, farSlice_0, texel_1, 0U), _S22);
    (&result_0)->inScattering_0 = mix(nearS_0, _S21, _S22);
    return result_0;
}


#line 236 "src/rendering/fog/shaders/volumetric_fog.slang"
void fogAirStretch_0(float3 direction_0, float nearDistance_0, float farDistance_0, float3 thread* extinction_0, float3 thread* source_0, KernelContext_0 thread* kernelContext_2)
{

#line 237
    float4 device* table_words_3 = kernelContext_2->cyFogSet_0->air_0;

#line 237
    CyAerialPerspective_0 _S23 = cyAerialPerspectiveAt_0(table_words_3, direction_0 * float3(nearDistance_0) );

#line 237
    CyAerialPerspective_0 _S24 = cyAerialPerspectiveAt_0(table_words_3, direction_0 * float3(farDistance_0) );

#line 244
    float3 _S25 = float3(9.99999997475242708e-07) ;

#line 244
    float3 nearT_1 = max(_S23.transmittance_0, _S25);

    float3 added_0 = max(_S24.inScattering_0 - _S23.inScattering_0, float3(0.0) ) / nearT_1;

#line 246
    float3 _S26 = float3(max(farDistance_0 - nearDistance_0, 9.99999997475242708e-07)) ;
    float3 _S27 = - log(max(saturate(_S24.transmittance_0 / nearT_1), _S25)) / _S26;

#line 247
    *extinction_0 = _S27;
    float3 x_1 = _S27 * _S26;
    float _S28 = x_1.x;

#line 249
    float _S29;

#line 249
    if(_S28 < 0.00100000004749745)
    {

#line 249
        _S29 = 1.0 - 0.5 * _S28;

#line 249
    }
    else
    {

#line 249
        _S29 = (1.0 - exp(- _S28)) / _S28;

#line 249
    }
    float _S30 = x_1.y;

#line 250
    float _S31;

#line 250
    if(_S30 < 0.00100000004749745)
    {

)cy_msl"
    R"cy_msl(#line 250
        _S31 = 1.0 - 0.5 * _S30;

#line 250
    }
    else
    {

#line 250
        _S31 = (1.0 - exp(- _S30)) / _S30;

#line 250
    }
    float _S32 = x_1.z;

#line 251
    float _S33;

#line 251
    if(_S32 < 0.00100000004749745)
    {

#line 251
        _S33 = 1.0 - 0.5 * _S32;

#line 251
    }
    else
    {

#line 251
        _S33 = (1.0 - exp(- _S32)) / _S32;

#line 251
    }
    *source_0 = added_0 / (_S26 * float3(_S29, _S31, _S33));
    return;
}


#line 101
float fogPhase_0(float g_0, float cosTheta_0)
{
    float clamped_1 = clamp(g_0, -0.99000000953674316, 0.99000000953674316);
    float squared_0 = clamped_1 * clamped_1;
    float denominator_0 = 1.0 + squared_0 - 2.0 * clamped_1 * cosTheta_0;
    return (1.0 - squared_0) / (12.56637096405029297 * denominator_0 * sqrt(max(denominator_0, 9.99999997475242708e-07)));
}


#line 130
struct FogMediumSample_0
{
    float extinction_1;
    float3 scattering_0;
    float3 sunScattering_0;
    float3 emission_0;
};

void fogAddMedium_0(FogMediumSample_0 thread* sample_0, float extinction_2, float3 albedo_0, float g_1, float3 emission_1, float cosToSun_0)
{

    sample_0->emission_0 = sample_0->emission_0 + emission_1;
    if(extinction_2 <= 0.0)
    {
        return;
    }
    float3 scattering_1 = albedo_0 * float3(extinction_2) ;
    sample_0->extinction_1 = sample_0->extinction_1 + extinction_2;
    sample_0->scattering_0 = sample_0->scattering_0 + scattering_1;
    sample_0->sunScattering_0 = sample_0->sunScattering_0 + scattering_1 * float3(fogPhase_0(g_1, cosToSun_0)) ;
    return;
}


#line 110
float fogInsideDistance_0(float shape_5, float3 size_0, float3 local_0)
{
    float _S34 = local_0.x;

#line 112
    float _S35 = local_0.z;

#line 112
    float radial_0 = sqrt(_S34 * _S34 + _S35 * _S35);
    if(shape_5 < 0.5)
    {
        return min(size_0.x - abs(_S34), min(size_0.y - abs(local_0.y), size_0.z - abs(_S35)));
    }
    if(shape_5 < 1.5)
    {
        return size_0.x - length(local_0);
    }
    if(shape_5 < 2.5)
    {
        return min(size_0.x - radial_0, size_0.y - abs(local_0.y));
    }
    float _S36 = max(size_0.y, 9.99999997475242708e-07);
    float _S37 = local_0.y;
    return min(min(_S37, _S36 - _S37), size_0.x * (1.0 - _S37 / _S36) - radial_0);
}


#line 153
FogMediumSample_0 fogSampleMedium_0(float3 world_0, float cosToSun_1, KernelContext_0 thread* kernelContext_3)
{
    thread FogMediumSample_0 sample_1;
    (&sample_1)->extinction_1 = 0.0;
    float3 _S38 = float3(0.0) ;

#line 157
    (&sample_1)->scattering_0 = _S38;
    (&sample_1)->sunScattering_0 = _S38;
    (&sample_1)->emission_0 = _S38;

#line 159
    float4 _S39 = fogWord_0(15U, kernelContext_3);


    float _S40 = _S39.x;

#line 162
    if(_S40 > 0.0)
    {

        float extinction_3 = _S40 * exp(- max(world_0.y - _S39.y, 0.0) / max(_S39.z, 0.00100000004749745));

#line 165
        float4 _S41 = fogWord_0(16U, kernelContext_3);
        fogAddMedium_0(&sample_1, extinction_3, _S41.xyz, _S39.w, _S38, cosToSun_1);

#line 162
    }

#line 162
    float4 _S42 = fogWord_0(6U, kernelContext_3);

#line 168
    uint _S43 = min(uint(_S42.w), 8U);

#line 168
    uint index_2 = 0U;
    for(;;)
    {

#line 169
        if(index_2 < _S43)
        {
        }
        else
        {

#line 169
            break;
        }
        uint base_0 = 17U + index_2 * 4U;

#line 171
        float4 _S44 = fogWord_0(base_0, kernelContext_3);

#line 171
        float4 _S45 = fogWord_0(base_0 + 1U, kernelContext_3);

#line 171
        float4 _S46 = fogWord_0(base_0 + 2U, kernelContext_3);

#line 171
        float4 _S47 = fogWord_0(base_0 + 3U, kernelContext_3);

#line 176
        float inside_0 = fogInsideDistance_0(_S44.w, _S45.xyz, world_0 - _S44.xyz);
        if(inside_0 <= 0.0)
        {
            index_2 = index_2 + 1U;

#line 169
            continue;
        }

#line 181
        float _S48 = _S45.w;

#line 181
        float coverage_0;

#line 181
        if(_S48 <= 0.0)
        {

#line 181
            coverage_0 = 1.0;

#line 181
        }
        else
        {

#line 181
            coverage_0 = saturate(inside_0 / _S48);

#line 181
        }
        fogAddMedium_0(&sample_1, _S46.w * coverage_0, _S46.xyz, _S47.w, _S47.xyz * float3(coverage_0) , cosToSun_1);

#line 169
        index_2 = index_2 + 1U;

#line 169
    }

#line 184
    return sample_1;
}

float fogLit_0(float reference_0, int2 texel_2, int extent_0, KernelContext_0 thread* kernelContext_4)
{
    int _S49 = extent_0 - int(1);
    int3 _S50 = int3(clamp(texel_2, int2(int(0), int(0)), int2(_S49, _S49)), int(0));

#line 190
    float _S51;

#line 190
    if(reference_0 >= ((kernelContext_4->cyFogSet_0->shadowMap_0).read(vec<uint,2>(((_S50)).xy), uint(((_S50)).z)).x))
    {

#line 190
        _S51 = 1.0;

#line 190
    }
    else
    {

#line 190
        _S51 = 0.0;

#line 190
    }

#line 190
    return _S51;
}


float fogShadowVisibility_0(float3 relative_0, KernelContext_0 thread* kernelContext_5)
{

#line 194
    float4 _S52 = fogWord_1(7U, kernelContext_5);

    if((_S52.w) < 0.5)
    {
        return 1.0;
    }
    float4 p_0 = float4(relative_0, 1.0);

#line 200
    float4 _S53 = fogWord_1(10U, kernelContext_5);
    float _S54 = dot(_S53, p_0);

#line 201
    float4 _S55 = fogWord_1(11U, kernelContext_5);

#line 201
    float _S56 = dot(_S55, p_0);

#line 201
    float4 _S57 = fogWord_1(12U, kernelContext_5);

#line 201
    float _S58 = dot(_S57, p_0);

#line 201
    float4 _S59 = fogWord_1(13U, kernelContext_5);
    float _S60 = dot(_S59, p_0);
    if(_S60 <= 0.0)
    {
        return 1.0;
    }
    float2 uv_0 = float2(_S54, _S56) / float2(_S60) ;
    float _S61 = uv_0.x;

#line 208
    bool _S62;

#line 208
    if(_S61 < 0.0)
    {

#line 208
        _S62 = true;

#line 208
    }
    else
    {

#line 208
        _S62 = _S61 > 1.0;

)cy_msl"
    R"cy_msl(#line 208
    }

#line 208
    if(_S62)
    {

#line 208
        _S62 = true;

#line 208
    }
    else
    {

#line 208
        _S62 = (uv_0.y) < 0.0;

#line 208
    }

#line 208
    if(_S62)
    {

#line 208
        _S62 = true;

#line 208
    }
    else
    {

#line 208
        _S62 = (uv_0.y) > 1.0;

#line 208
    }

#line 208
    if(_S62)
    {
        return 1.0;
    }

#line 210
    float4 _S63 = fogWord_1(14U, kernelContext_5);


    float reference_1 = _S58 / _S60 + _S63.x;
    float size_1 = _S63.y;
    int extent_1 = int(size_1);
    float2 t_0 = uv_0 * float2(size_1)  - float2(0.5) ;
    float2 f_0 = floor(t_0);
    int2 _S64 = int2(f_0);
    float2 a_0 = t_0 - f_0;

#line 219
    float _S65 = fogLit_0(reference_1, _S64, extent_1, kernelContext_5);

#line 219
    float _S66 = fogLit_0(reference_1, _S64 + int2(int(1), int(0)), extent_1, kernelContext_5);

    float _S67 = a_0.x;

#line 220
    float top_0 = mix(_S65, _S66, _S67);

#line 220
    float _S68 = fogLit_0(reference_1, _S64 + int2(int(0), int(1)), extent_1, kernelContext_5);

#line 220
    float _S69 = fogLit_0(reference_1, _S64 + int2(int(1), int(1)), extent_1, kernelContext_5);



    return mix(top_0, mix(_S68, _S69, _S67), a_0.y);
}


#line 258
[[kernel]] void cyVolumetricFog(uint3 id_0 [[thread_position_in_grid]], CyFogSet_default_0 constant* cyFogSet_1 [[buffer(0)]])
{

#line 258
    thread KernelContext_0 kernelContext_6;

#line 258
    (&kernelContext_6)->cyFogSet_0 = cyFogSet_1;

#line 258
    float4 _S70 = fogWord_0(0U, &kernelContext_6);

#line 258
    float4 _S71 = fogWord_0(1U, &kernelContext_6);

#line 258
    float4 _S72 = fogWord_0(2U, &kernelContext_6);

#line 258
    float4 _S73 = fogWord_0(3U, &kernelContext_6);

#line 258
    float4 _S74 = fogWord_0(4U, &kernelContext_6);

#line 265
    uint width_1 = uint(_S73.x);
    uint height_0 = uint(_S73.y);
    uint _S75 = uint(_S73.z);
    uint _S76 = id_0.x;

#line 268
    bool _S77;

#line 268
    if(_S76 == 0U)
    {

#line 268
        _S77 = (id_0.y) == 0U;

#line 268
    }
    else
    {

#line 268
        _S77 = false;

#line 268
    }

#line 268
    uint slice_4;

#line 268
    if(_S77)
    {

#line 268
        slice_4 = 0U;

#line 273
        for(;;)
        {

#line 273
            if(slice_4 < 4U)
            {
            }
            else
            {

#line 273
                break;
            }
            float4 device* _S78 = (&kernelContext_6)->cyFogSet_0->table_0+slice_4;

#line 275
            float4 _S79 = fogWord_0(slice_4, &kernelContext_6);

#line 275
            *_S78 = _S79;

#line 273
            slice_4 = slice_4 + 1U;

#line 273
        }



        *((&kernelContext_6)->cyFogSet_0->table_0+4U) = float4(_S74.x, _S74.y, 1.0, 0.0);

#line 268
    }

#line 285
    if(_S76 >= width_1)
    {

#line 285
        _S77 = true;

#line 285
    }
    else
    {

#line 285
        _S77 = (id_0.y) >= height_0;

#line 285
    }

#line 285
    if(_S77)
    {
        return;
    }


    uint _S80 = id_0.y;

    float3 _S81 = _S70.xyz;

#line 293
    float3 direction_1 = normalize(_S81 + _S71.xyz * float3((((float(_S76) + 0.5) / float(width_1) * 2.0 - 1.0) * _S71.w))  + _S72.xyz * float3((((float(_S80) + 0.5) / float(height_0) * 2.0 - 1.0) * _S72.w)) );
    float _S82 = max(dot(direction_1, _S81), 0.00100000004749745);

#line 294
    float4 _S83 = fogWord_0(7U, &kernelContext_6);

    float _S84 = dot(direction_1, _S83.xyz);

#line 296
    float4 _S85 = fogWord_0(5U, &kernelContext_6);
    float3 _S86 = _S85.xyz;

#line 297
    float4 _S87 = fogWord_0(6U, &kernelContext_6);
    float3 _S88 = _S87.xyz;

#line 298
    float4 _S89 = fogWord_0(8U, &kernelContext_6);
    float3 _S90 = _S89.xyz;

#line 299
    float4 _S91 = fogWord_0(9U, &kernelContext_6);
    float3 _S92 = _S91.xyz;
    uint _S93 = max(uint(_S74.z), 1U);



    bool _S94 = (_S74.w) > 0.5;
    float3 _S95 = float3(1.0) ;



    float3 _S96 = float3(0.0) ;

#line 310
    float previous_0 = 0.0;

#line 310
    slice_4 = 0U;

#line 310
    float3 carried_0 = _S95;

#line 310
    float3 scattered_0 = _S96;
    for(;;)
    {

#line 311
        if(slice_4 < _S75)
        {
        }
        else
        {

#line 311
            break;
        }
        float distance_0 = fogSliceDepth_0(_S73, _S74, slice_4) / _S82;
        float _S97 = max(distance_0 - previous_0, 0.0) / float(_S93);

        thread float3 airExtinction_0 = _S96;
        thread float3 airSource_0 = _S96;
        if(_S94)
        {

#line 318
            fogAirStretch_0(direction_1, previous_0, distance_0, &airExtinction_0, &airSource_0, &kernelContext_6);

#line 318
        }

#line 318
        uint sub_0 = 0U;

#line 323
        for(;;)
        {

#line 323
            if(sub_0 < _S93)
            {
            }
            else
            {

#line 323
                break;
            }

            float3 point_0 = _S86 + direction_1 * float3((previous_0 + (float(sub_0) + 0.5) * _S97)) ;

#line 326
            FogMediumSample_0 _S98 = fogSampleMedium_0(point_0 + _S88, _S84, &kernelContext_6);

)cy_msl"
    R"cy_msl(#line 326
            float _S99 = fogShadowVisibility_0(point_0, &kernelContext_6);



            float3 source_1 = _S98.sunScattering_0 * _S90 * float3(_S99)  + _S98.scattering_0 * _S92 + _S98.emission_0;


            float3 extinction3_0 = (float3(max(_S98.extinction_1, 0.0))  + airExtinction_0) * float3(_S97) ;
            float3 transmitted_0 = exp(- extinction3_0);
            float3 sigma_0 = max(float3(_S98.extinction_1)  + airExtinction_0, float3(9.99999997475242708e-07) );

#line 335
            float _S100;

            if((extinction3_0.x) > 9.99999997475242708e-07)
            {

#line 337
                _S100 = (1.0 - transmitted_0.x) / sigma_0.x;

#line 337
            }
            else
            {

#line 337
                _S100 = _S97;

#line 337
            }

#line 337
            float _S101;
            if((extinction3_0.y) > 9.99999997475242708e-07)
            {

#line 338
                _S101 = (1.0 - transmitted_0.y) / sigma_0.y;

#line 338
            }
            else
            {

#line 338
                _S101 = _S97;

#line 338
            }

#line 338
            float _S102;
            if((extinction3_0.z) > 9.99999997475242708e-07)
            {

#line 339
                _S102 = (1.0 - transmitted_0.z) / sigma_0.z;

#line 339
            }
            else
            {

#line 339
                _S102 = _S97;

#line 339
            }
            float3 scattered_1 = scattered_0 + (source_1 + airSource_0) * float3(_S100, _S101, _S102) * carried_0;
            float3 carried_1 = carried_0 * transmitted_0;

#line 323
            sub_0 = sub_0 + 1U;

#line 323
            carried_0 = carried_1;

#line 323
            scattered_0 = scattered_1;

#line 323
        }

#line 356
        uint _S103 = 5U + (slice_4 * height_0 * width_1 + _S80 * width_1 + _S76) * 2U;

#line 356
        *((&kernelContext_6)->cyFogSet_0->table_0+_S103) = float4(carried_0, 0.0);
        *((&kernelContext_6)->cyFogSet_0->table_0+(_S103 + 1U)) = float4(scattered_0, 0.0);

#line 311
        uint slice_5 = slice_4 + 1U;

#line 311
        previous_0 = distance_0;

#line 311
        slice_4 = slice_5;

#line 311
    }

#line 364
    return;
}

)cy_msl";

}  // namespace cy::rendering::fog

#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for samples/10-world. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::sample::world {

/// world_vertex.metal, 1586 bytes.
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


#line 106 "samples/10-world/shaders/world.slang"
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


#line 166
struct VertexOutput_0
{
    float4 clip_1;
    float3 world_1;
    float3 normal_2;
    float3 color_2;
};


#line 166
[[vertex]] worldVertex_Result_0 worldVertex(vertexInput_0 _S1 [[stage_in]], WorldPush_0 constant* push_0 [[buffer(1)]])
{

#line 177
    float4 _S2 = float4(_S1.position_0, 1.0);
    thread VertexOutput_0 output_0;
    (&output_0)->clip_1 = float4(dot(push_0->row0_0, _S2), dot(push_0->row1_0, _S2), dot(push_0->row2_0, _S2), dot(push_0->row3_0, _S2));

    (&output_0)->world_1 = _S1.position_0;
    (&output_0)->normal_2 = _S1.normal_1;
    (&output_0)->color_2 = _S1.color_1;

#line 183
    thread worldVertex_Result_0 _S3;

#line 183
    (&_S3)->clip_0 = output_0.clip_1;

#line 183
    (&_S3)->world_0 = output_0.world_1;

#line 183
    (&_S3)->normal_0 = output_0.normal_2;

#line 183
    (&_S3)->color_0 = output_0.color_2;

#line 183
    return _S3;
}

)cy_msl";

/// world_fragment.metal, 46151 bytes.
inline constexpr char kWorldFragmentMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 106 "samples/10-world/shaders/world.slang"
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


#line 49 "src/rendering/shaders/cy/aerial_perspective.slang"
struct WorldCloudShadow_default_0
{
    uint device* field_0;
    float4 device* placement_0;
    float4 device* aerial_0;
    uint device* decals_0;
};


#line 49
struct KernelContext_0
{
    WorldPush_0 constant* push_0;
    WorldCloudShadow_default_0 constant* cloudShadow_0;
};


#line 216 "samples/10-world/shaders/world.slang"
uint WorldDecalSource_word_0(uint index_0, KernelContext_0 thread* kernelContext_0)
{
    return kernelContext_0->cloudShadow_0->decals_0[index_0];
}


#line 113 "src/rendering/shaders/cy/decal.slang"
float3 wordFloat3_0(uint index_1, KernelContext_0 thread* kernelContext_1)
{

#line 113
    uint _S1 = WorldDecalSource_word_0(index_1, kernelContext_1);

    float _S2 = (as_type<float>((_S1)));

#line 115
    uint _S3 = WorldDecalSource_word_0(index_1 + 1U, kernelContext_1);

#line 115
    float _S4 = (as_type<float>((_S3)));

#line 115
    uint _S5 = WorldDecalSource_word_0(index_1 + 2U, kernelContext_1);

#line 115
    return float3(_S2, _S4, (as_type<float>((_S5))));
}


#line 108
float wordFloat_0(uint index_2, KernelContext_0 thread* kernelContext_2)
{

#line 108
    uint _S6 = WorldDecalSource_word_0(index_2, kernelContext_2);

    return (as_type<float>((_S6)));
}


#line 71
struct CyDecalReceiver_0
{
    float3 relativePosition_0;
    float3 geometricNormal_0;
    float eyeDistance_0;
    uint channels_0;
};


#line 85
struct CyDecalSurface_0
{
    float3 albedo_0;
    float roughness_0;
    float metallic_0;
    float3 emission_0;
    float3 normal_0;
};


#line 216 "samples/10-world/shaders/world.slang"
uint WorldDecalSource_word_1(uint index_3, KernelContext_0 thread* kernelContext_3)
{
    return kernelContext_3->cloudShadow_0->decals_0[index_3];
}


#line 192 "src/rendering/shaders/cy/decal.slang"
float cyDecalAngleFade_0(float cosine_0, float limit_0)
{

#line 192
    bool _S7;

    if(cosine_0 <= 0.0)
    {

#line 194
        _S7 = true;

#line 194
    }
    else
    {

#line 194
        _S7 = cosine_0 <= limit_0;

#line 194
    }

#line 194
    if(_S7)
    {
        return 0.0;
    }
    float _S8 = (cosine_0 - limit_0) / max(1.0 - limit_0, 0.00009999999747379);
    return _S8 * _S8 * (3.0 - 2.0 * _S8);
}


float cyDecalDistanceFade_0(float metres_0, float start_0, float end_0)
{
    float _S9 = max(end_0, start_0 + 0.00009999999747379);
    if(metres_0 <= start_0)
    {
        return 1.0;
    }
    if(metres_0 >= _S9)
    {
        return 0.0;
    }
    float _S10 = 1.0 - (metres_0 - start_0) / (_S9 - start_0);
    return _S10 * _S10 * (3.0 - 2.0 * _S10);
}


#line 220 "samples/10-world/shaders/world.slang"
float WorldDecalSource_mask_0(uint slot_0, float2 uv_0)
{
    return 1.0;
}


#line 15 "src/rendering/shaders/cy/noise.slang"
uint3 hashPcg3d_0(uint3 value_0)
{
    uint3 _S11 = value_0 * uint3(1664525U)  + uint3(1013904223U) ;

#line 17
    thread uint3 v_0 = _S11;
    v_0.x = v_0.x + _S11.y * _S11.z;
    v_0.y = v_0.y + v_0.z * v_0.x;
    v_0.z = v_0.z + v_0.x * v_0.y;
    uint3 _S12 = v_0 ^ (v_0 >> (uint3(16U) ));

#line 21
    v_0 = _S12;
    v_0.x = v_0.x + _S12.y * _S12.z;
    v_0.y = v_0.y + v_0.z * v_0.x;
    v_0.z = v_0.z + v_0.x * v_0.y;
    return v_0;
}


#line 36
float valueNoise_0(float3 position_0)
{
    float3 _S13 = floor(position_0);
    float3 _S14 = position_0 - _S13;
    float3 _S15 = _S14 * _S14 * (float3(3.0)  - float3(2.0)  * _S14);
    uint3 _S16 = uint3(int3(_S13) + int3(int(1024)) );

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
        uint3 _S17 = uint3(corner_0 & 1U, (corner_0 >> 1U) & 1U, (corner_0 >> 2U) & 1U);

        float3 _S18 = mix(float3(1.0)  - _S15, _S15, float3(_S17));
        float result_1 = result_0 + float((hashPcg3d_0(_S16 + _S17).x) >> 8U) * 5.9604644775390625e-08 * _S18.x * _S18.y * _S18.z;

#line 44
        corner_0 = corner_0 + 1U;

#line 44
        result_0 = result_1;

#line 44
    }

#line 51
    return result_0;
}


#line 147 "src/rendering/shaders/cy/decal.slang"
float decalFbm_0(float2 uv_1, float frequency_0, uint seed_0)
{
    float _S19 = float(seed_0 % 4096U) * 1.61800003051757812;

#line 149
    uint octave_0 = 0U;

#line 149
    float scale_0 = frequency_0;

#line 149
    float amplitude_0 = 0.5;

#line 149
    float sum_0 = 0.0;



    for(;;)
    {

#line 153
        if(octave_0 < 3U)
        {
        }
        else
        {

#line 153
            break;
        }
        float sum_1 = sum_0 + valueNoise_0(float3(uv_1 * float2(scale_0) , _S19)) * amplitude_0;
        float amplitude_1 = amplitude_0 * 0.5;
        float scale_1 = scale_0 * 2.02999997138977051;

#line 153
        octave_0 = octave_0 + 1U;

#line 153
        scale_0 = scale_1;

#line 153
        amplitude_0 = amplitude_1;

#line 153
        sum_0 = sum_1;

#line 153
    }

#line 159
    return sum_0 / 0.875;
}


#line 167
float cyDecalShapeCoverage_0(uint shape_0, float2 uv_2, float a_0, float b_0, uint seed_1)
{
    float _S20 = length(uv_2 - float2(0.5) ) * 2.0;
    if(shape_0 == 1U)
    {

        return 1.0 - smoothstep(0.44999998807907104, 0.94999998807907104, _S20 + (decalFbm_0(uv_2, a_0, seed_1) - 0.5) * b_0);
    }
    if(shape_0 == 2U)
    {

        return 1.0 - smoothstep(b_0 * 0.69999998807907104, b_0, abs(_S20 - a_0));
    }
    if(shape_0 == 3U)
    {

)cy_msl"
    R"cy_msl(        return smoothstep(b_0, b_0 + 0.11999999731779099, decalFbm_0(uv_2, a_0, seed_1)) * (1.0 - smoothstep(0.75, 1.0, _S20));
    }
    return 1.0;
}


#line 220
void cyApplyDecal_0(uint rank_0, const CyDecalReceiver_0 thread* receiver_0, CyDecalSurface_0 thread* surface_0, KernelContext_0 thread* kernelContext_4)
{

#line 221
    uint _S21 = WorldDecalSource_word_1(4U, kernelContext_4);

    uint _S22 = _S21 + rank_0 * 36U;

#line 223
    uint _S23 = WorldDecalSource_word_1(_S22 + 28U, kernelContext_4);
    if((_S23 & (receiver_0->channels_0)) == 0U)
    {
        return;
    }

#line 226
    float3 _S24 = wordFloat3_0(_S22 + 4U, kernelContext_4);

#line 226
    float3 _S25 = wordFloat3_0(_S22 + 8U, kernelContext_4);

#line 226
    float3 _S26 = wordFloat3_0(_S22 + 12U, kernelContext_4);

#line 226
    float3 _S27 = receiver_0->relativePosition_0;

#line 226
    float3 _S28 = wordFloat3_0(_S22, kernelContext_4);

#line 231
    float3 _S29 = _S27 - _S28;
    float _S30 = dot(_S29, _S24);

#line 232
    float _S31 = dot(_S29, _S25);

#line 232
    float _S32 = dot(_S29, _S26);


    if(any((abs(float3(_S30, _S31, _S32))) > (float3(1.0) )))
    {
        return;
    }



    float _S33 = dot(receiver_0->geometricNormal_0, normalize(_S26));

#line 242
    float _S34 = wordFloat_0(_S22 + 3U, kernelContext_4);

#line 242
    float weight_0 = cyDecalAngleFade_0(_S33, _S34);

#line 242
    float _S35 = receiver_0->eyeDistance_0;

#line 242
    float _S36 = wordFloat_0(_S22 + 24U, kernelContext_4);

#line 242
    float _S37 = wordFloat_0(_S22 + 25U, kernelContext_4);
    float weight_1 = weight_0 * cyDecalDistanceFade_0(_S35, _S36, _S37);

#line 243
    float _S38 = wordFloat_0(_S22 + 34U, kernelContext_4);

#line 243
    float weight_2;


    if(_S38 > 0.0)
    {

#line 246
        weight_2 = weight_1 * (1.0 - smoothstep(1.0 - _S38, 1.0, abs(_S32)));

#line 246
    }
    else
    {

#line 246
        weight_2 = weight_1;

#line 246
    }



    if(weight_2 <= 0.0)
    {
        return;
    }

#line 252
    float2 _S39 = float2(0.5) ;


    float2 _S40 = float2(_S30, _S31) * _S39 + _S39;

#line 255
    uint _S41 = WorldDecalSource_word_1(_S22 + 29U, kernelContext_4);

#line 255
    float _S42 = wordFloat_0(_S22 + 30U, kernelContext_4);

#line 255
    float _S43 = wordFloat_0(_S22 + 31U, kernelContext_4);

#line 255
    uint _S44 = WorldDecalSource_word_1(_S22 + 35U, kernelContext_4);

#line 255
    uint _S45 = WorldDecalSource_word_1(_S22 + 32U, kernelContext_4);

#line 255
    float _S46;

#line 261
    if(_S45 == 4294967295U)
    {

#line 261
        _S46 = 1.0;

#line 261
    }
    else
    {

#line 261
        _S46 = WorldDecalSource_mask_0(_S45, _S40);

#line 261
    }

    float _S47 = weight_2 * (cyDecalShapeCoverage_0(_S41, _S40, _S42, _S43, _S44) * _S46);
    if(_S47 <= 0.0)
    {
        return;
    }

#line 266
    float3 _S48 = wordFloat3_0(_S22 + 16U, kernelContext_4);

#line 266
    float3 _S49 = wordFloat3_0(_S22 + 20U, kernelContext_4);

#line 271
    float3 _S50 = surface_0->albedo_0;

#line 271
    float _S51 = wordFloat_0(_S22 + 7U, kernelContext_4);

#line 271
    surface_0->albedo_0 = mix(_S50, _S48, float3((_S47 * _S51)) );
    float _S52 = surface_0->roughness_0;

#line 272
    float _S53 = wordFloat_0(_S22 + 19U, kernelContext_4);

#line 272
    float _S54 = wordFloat_0(_S22 + 11U, kernelContext_4);

#line 272
    surface_0->roughness_0 = mix(_S52, _S53, _S47 * _S54);

    float _S55 = surface_0->metallic_0;

#line 274
    float _S56 = wordFloat_0(_S22 + 23U, kernelContext_4);

#line 274
    float _S57 = wordFloat_0(_S22 + 26U, kernelContext_4);

#line 274
    surface_0->metallic_0 = mix(_S55, _S56, _S47 * _S57);

    float3 _S58 = surface_0->emission_0;

#line 276
    float _S59 = wordFloat_0(_S22 + 27U, kernelContext_4);

#line 276
    surface_0->emission_0 = mix(_S58, _S49, float3((_S47 * _S59)) );

#line 276
    float _S60 = wordFloat_0(_S22 + 33U, kernelContext_4);

#line 276
    float _S61 = wordFloat_0(_S22 + 15U, kernelContext_4);

#line 284
    float _S62 = _S60 * _S61 * weight_2;
    if(_S62 > 0.0)
    {

        float2 _S63 = float2(0.00390625, 0.0);

        float2 _S64 = float2(0.0, 0.00390625);



        float3 _S65 = float3(_S62)  * (float3(((cyDecalShapeCoverage_0(_S41, _S40 + _S63, _S42, _S43, _S44) - cyDecalShapeCoverage_0(_S41, _S40 - _S63, _S42, _S43, _S44)) / 0.0078125 * (length(_S24) * 0.5)))  * normalize(_S24) + float3(((cyDecalShapeCoverage_0(_S41, _S40 + _S64, _S42, _S43, _S44) - cyDecalShapeCoverage_0(_S41, _S40 - _S64, _S42, _S43, _S44)) / 0.0078125 * (length(_S25) * 0.5)))  * normalize(_S25));


        surface_0->normal_0 = normalize(surface_0->normal_0 - (_S65 - surface_0->normal_0 * float3(dot(surface_0->normal_0, _S65)) ));

#line 285
    }

#line 299
    return;
}


#line 334
uint cyDecalTableListEntry_0(uint index_4, KernelContext_0 thread* kernelContext_5)
{

#line 334
    uint _S66 = WorldDecalSource_word_0(index_4, kernelContext_5);

    return _S66;
}


#line 10 "src/rendering/shaders/cy/cluster.slang"
struct ClusterGrid_0
{
    uint3 dimensions_0;
    uint maxLightsPerCluster_0;
    float nearPlane_0;
    float farPlane_0;
    float sliceScale_0;
    float sliceBias_0;
};


uint clusterSliceOf_0(const ClusterGrid_0 thread* grid_0, float viewDepth_0)
{

    return uint(clamp(log2(max(viewDepth_0, grid_0->nearPlane_0)) * grid_0->sliceScale_0 + grid_0->sliceBias_0, 0.0, float(grid_0->dimensions_0.z - 1U)));
}


uint3 clusterCoordOf_0(const ClusterGrid_0 thread* grid_1, uint2 pixel_0, uint2 renderExtent_0, float viewDepth_1)
{
    uint2 _S67 = grid_1->dimensions_0.xy;
    uint2 _S68 = min(uint2(float2(pixel_0) / float2(renderExtent_0) * float2(_S67)), _S67 - uint2(1U) );

#line 31
    uint _S69 = clusterSliceOf_0(grid_1, viewDepth_1);

#line 31
    return uint3(_S68, _S69);
}



uint clusterIndexOf_0(const ClusterGrid_0 thread* grid_2, uint3 coord_0)
{
    return coord_0.x + grid_2->dimensions_0.x * (coord_0.y + grid_2->dimensions_0.y * coord_0.z);
}


#line 306 "src/rendering/shaders/cy/decal.slang"
uint2 cyDecalTableList_0(float3 relativePosition_1, float2 pixel_1, KernelContext_0 thread* kernelContext_6)
{

#line 306
    uint _S70 = WorldDecalSource_word_1(5U, kernelContext_6);

    if(_S70 == 0U)
    {
        return uint2(0U, 0U);
    }
    thread ClusterGrid_0 grid_3;

#line 312
    uint _S71 = WorldDecalSource_word_1(8U, kernelContext_6);

#line 312
    uint _S72 = WorldDecalSource_word_1(9U, kernelContext_6);

#line 312
    uint _S73 = WorldDecalSource_word_1(10U, kernelContext_6);
    (&grid_3)->dimensions_0 = uint3(_S71, _S72, _S73);

#line 313
    uint _S74 = WorldDecalSource_word_1(11U, kernelContext_6);
    (&grid_3)->maxLightsPerCluster_0 = _S74;

)cy_msl"
    R"cy_msl(#line 314
    float _S75 = wordFloat_0(12U, kernelContext_6);
    (&grid_3)->sliceScale_0 = _S75;

#line 315
    float _S76 = wordFloat_0(13U, kernelContext_6);
    (&grid_3)->sliceBias_0 = _S76;

#line 316
    float _S77 = wordFloat_0(14U, kernelContext_6);
    (&grid_3)->nearPlane_0 = _S77;

#line 317
    float _S78 = wordFloat_0(15U, kernelContext_6);
    (&grid_3)->farPlane_0 = _S78;
    if(_S73 == 0U)
    {
        return uint2(0U, 0U);
    }

#line 321
    uint _S79 = WorldDecalSource_word_1(16U, kernelContext_6);

#line 321
    uint _S80 = WorldDecalSource_word_1(17U, kernelContext_6);

    uint2 _S81 = uint2(_S79, _S80);

#line 323
    float3 _S82 = wordFloat3_0(18U, kernelContext_6);

#line 323
    float _S83 = wordFloat_0(21U, kernelContext_6);
    float4 _S84 = float4(_S82, _S83);
    float _S85 = dot(_S84.xyz, relativePosition_1) + _S84.w;
    uint2 _S86 = uint2(pixel_1);

#line 326
    thread ClusterGrid_0 _S87 = grid_3;

#line 326
    uint3 _S88 = clusterCoordOf_0(&_S87, _S86, _S81, _S85);

#line 326
    thread ClusterGrid_0 _S89 = grid_3;

#line 326
    uint _S90 = clusterIndexOf_0(&_S89, _S88);

#line 326
    uint _S91 = WorldDecalSource_word_1(6U, kernelContext_6);

    uint _S92 = _S91 + _S90 * 2U;

#line 328
    uint _S93 = WorldDecalSource_word_1(_S92, kernelContext_6);

#line 328
    uint _S94 = WorldDecalSource_word_1(_S92 + 1U, kernelContext_6);
    return uint2(_S93, _S94);
}


#line 135
uint cyDecalCount_0(KernelContext_0 thread* kernelContext_7)
{

#line 135
    uint _S95 = WorldDecalSource_word_0(0U, kernelContext_7);

#line 135
    uint _S96;

    if(_S95 == 1129923651U)
    {

#line 137
        uint _S97 = WorldDecalSource_word_0(2U, kernelContext_7);

#line 137
        _S96 = _S97;

#line 137
    }
    else
    {

#line 137
        _S96 = 0U;

#line 137
    }

#line 137
    return _S96;
}


#line 341
void cyApplyTableDecals_0(const CyDecalReceiver_0 thread* receiver_1, float2 pixel_2, CyDecalSurface_0 thread* surface_1, KernelContext_0 thread* kernelContext_8)
{

#line 342
    uint _S98 = cyDecalCount_0(kernelContext_8);


    if(_S98 == 0U)
    {
        return;
    }

#line 347
    uint2 _S99 = cyDecalTableList_0(receiver_1->relativePosition_0, pixel_2, kernelContext_8);

#line 347
    uint slot_1 = 0U;


    for(;;)
    {

#line 350
        if(slot_1 < (_S99.y))
        {
        }
        else
        {

#line 350
            break;
        }

#line 350
        uint _S100 = cyDecalTableListEntry_0(_S99.x + slot_1, kernelContext_8);


        if(_S100 < _S98)
        {

#line 353
            cyApplyDecal_0(_S100, receiver_1, surface_1, kernelContext_8);

#line 353
        }

#line 350
        slot_1 = slot_1 + 1U;

#line 350
    }

#line 358
    return;
}


#line 193 "samples/10-world/shaders/world.slang"
float3 tonemap_0(float3 radiance_0)
{

    return pow(saturate(radiance_0 / (float3(1.0)  + radiance_0)), float3(0.45454543828964233) );
}


#line 148 "src/rendering/shaders/cy/field.slang"
bool cyFieldIsImage_0(uint device* image_words_0)
{

#line 148
    bool _S101;

    if(image_words_0[int(0)] == 1129924164U)
    {

#line 150
        _S101 = image_words_0[int(1)] == 1U;

#line 150
    }
    else
    {

#line 150
        _S101 = false;

#line 150
    }

#line 150
    return _S101;
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
    uint index_5 = 0U;
    for(;;)
    {

#line 167
        if(index_5 < 4U)
        {
        }
        else
        {

#line 167
            break;
        }
        (&header_0)->defaults_0[index_5] = (as_type<float>((image_words_1[9U + index_5])));

#line 167
        index_5 = index_5 + 1U;

#line 167
    }



    (&header_0)->entries_0 = image_words_1[int(13)];
    (&header_0)->originTileX_0 = (as_type<int>((image_words_1[int(14)])));
    (&header_0)->originTileZ_0 = (as_type<int>((image_words_1[int(15)])));
    return header_0;
}


#line 318
int cyFieldIfloor_0(float value_1)
{
    return int(floor(value_1));
}


#line 312
int cyFieldFloorDiv_0(int value_2, int divisor_0)
{
    int quotient_0 = value_2 / divisor_0;
    int _S102 = value_2 % divisor_0;

#line 315
    bool _S103;

#line 315
    if(_S102 != int(0))
    {

#line 315
        _S103 = (value_2 < int(0)) != (divisor_0 < int(0));

#line 315
    }
    else
    {

#line 315
        _S103 = false;

#line 315
    }

#line 315
    int _S104;

)cy_msl"
    R"cy_msl(#line 315
    if(_S103)
    {

#line 315
        _S104 = quotient_0 - int(1);

#line 315
    }
    else
    {

#line 315
        _S104 = quotient_0;

#line 315
    }

#line 315
    return _S104;
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
        bool _S105;
        if(entryLayer_0 < layer_0)
        {

#line 293
            _S105 = true;

#line 293
        }
        else
        {

#line 293
            if(entryLayer_0 == layer_0)
            {

#line 293
                _S105 = entryZ_0 < tileZ_0;

#line 293
            }
            else
            {

#line 293
                _S105 = false;

#line 293
            }

#line 293
        }

#line 293
        bool before_0;

#line 293
        bool _S106;

#line 293
        if(_S105)
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
                _S106 = entryX_0 < tileX_0;

#line 294
            }
            else
            {

#line 294
                _S106 = false;

#line 294
            }

#line 294
            before_0 = _S106;

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
            _S106 = entryZ_0 == tileZ_0;

#line 300
        }
        else
        {

#line 300
            _S106 = false;

#line 300
        }

#line 300
        bool _S107;

#line 300
        if(_S106)
        {

#line 300
            _S107 = entryX_0 == tileX_0;

#line 300
        }
        else
        {

#line 300
            _S107 = false;

#line 300
        }

#line 300
        if(_S107)
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
    bool _S108 = (header_2->interpolation_0) == 1U;

#line 368
    (&lattice_0)->linear_0 = _S108;
    if(_S108)
    {

#line 376
        float u_0 = x_0 / header_2->cellMetres_0 - 0.5;
        float w_0 = z_0 / header_2->cellMetres_0 - 0.5;
        int _S109 = cyFieldIfloor_0(u_0);

#line 378
        (&lattice_0)->i0_0 = _S109;
        int _S110 = cyFieldIfloor_0(w_0);

#line 379
        (&lattice_0)->k0_0 = _S110;
        (&lattice_0)->fx_0 = u_0 - float(_S109);
        (&lattice_0)->fz_0 = w_0 - float(_S110);

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
    bool _S111;
    if((&lattice_0)->linear_0)
    {

#line 390
        _S111 = (header_2->verticalCells_0) > 1U;

#line 390
    }
    else
    {

#line 390
        _S111 = false;

#line 390
    }

#line 390
    if(_S111)
    {
        float v_1 = vertical_0 - 0.5;
        int _S112 = cyFieldIfloor_0(v_1);

#line 393
        (&lattice_0)->j0_0 = _S112;
        (&lattice_0)->fy_0 = v_1 - float(_S112);

#line 390
    }
    else
    {

)cy_msl"
    R"cy_msl(#line 398
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
    bool _S113;

    if(encoding_1 == 2U)
    {

#line 183
        _S113 = true;

#line 183
    }
    else
    {

#line 183
        _S113 = encoding_1 == 4U;

#line 183
    }

#line 183
    if(_S113)
    {
        return 2U;
    }
    if(encoding_1 == 1U)
    {

#line 187
        _S113 = true;

#line 187
    }
    else
    {

#line 187
        _S113 = encoding_1 == 3U;

#line 187
    }

#line 187
    if(_S113)
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
    uint _S114 = byteOffset_0 >> 2U;
    return (image_words_3[_S114] >> shift_0) | (image_words_3[_S114 + 1U] << (32U - shift_0));
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
    uint _S115 = header_3->encoding_0;
    uint _S116 = cyFieldEncodingStride_0(header_3->encoding_0);

#line 234
    float _S117 = header_3->rangeMin_0;
    float _S118 = header_3->rangeMax_0 - header_3->rangeMin_0;

#line 235
    uint index_6 = 0U;
    for(;;)
    {

#line 236
        if(index_6 < (header_3->components_0))
        {
        }
        else
        {

#line 236
            break;
        }
        uint offset_0 = byteOffset_3 + index_6 * _S116;

#line 238
        float value_4;

#line 249
        if(_S115 == 0U)
        {

#line 249
            value_4 = (as_type<float>((cyFieldLoadU32_0(image_words_6, offset_0))));

#line 249
        }
        else
        {

            if(_S115 == 1U)
            {

#line 253
                value_4 = _S117 + float(cyFieldLoadByte_0(image_words_6, offset_0)) / 255.0 * _S118;

#line 253
            }
            else
            {

                if(_S115 == 2U)
                {

#line 257
                    value_4 = _S117 + float(cyFieldLoadU16_0(image_words_6, offset_0)) / 65535.0 * _S118;

#line 257
                }
                else
                {

#line 265
                    if(_S115 == 3U)
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
        out_0[index_6] = value_4;

#line 236
        index_6 = index_6 + 1U;

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
    uint _S119 = centre_0->payload_0;

#line 421
    int _S120 = centre_0->x_1;

#line 421
    bool _S121;

    if(tileX_1 != (centre_0->x_1))
    {

#line 423
        _S121 = true;

#line 423
    }
    else
    {

#line 423
        _S121 = tileZ_1 != (centre_0->z_1);

#line 423
    }

#line 423
    int tileX_2;

#line 423
    int tileZ_2;

#line 423
    uint payload_1;

#line 423
    if(_S121)
    {
        thread uint found_0 = 0U;

#line 425
        bool _S122 = cyFieldFindEntry_0(image_words_7, header_4, centre_0->layer_1, tileX_1, tileZ_1, &found_0);
        if(_S122)
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

)cy_msl"
    R"cy_msl(#line 426
            tileX_2 = _S120;

#line 426
            tileZ_2 = centre_0->z_1;

#line 426
            payload_1 = _S119;

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
        payload_1 = _S119;

#line 423
    }

#line 423
    float4 _S123 = cyFieldDecodePoint_0(image_words_7, header_4, payload_1 * 4U + (uint(j_0) * 16U * 16U + uint(cyFieldClampI32_0(gk_0 - tileZ_2 * int(16), int(0), int(15))) * 16U + uint(cyFieldClampI32_0(gi_0 - tileX_2 * int(16), int(0), int(15)))) * (cyFieldEncodingStride_0(header_4->encoding_0) * header_4->components_0));

#line 442
    return _S123;
}


#line 338
float cyFieldVerticalWeight_0(uint taps_0, uint dy_0, float fy_1)
{
    if(taps_0 == 1U)
    {
        return 1.0;
    }

#line 342
    float _S124;

    if(dy_0 == 0U)
    {

#line 344
        _S124 = 1.0 - fy_1;

#line 344
    }
    else
    {

#line 344
        _S124 = fy_1;

#line 344
    }

#line 344
    return _S124;
}


#line 446
bool cyFieldSampleLayer_0(uint device* image_words_8, const CyFieldHeader_0 thread* header_5, uint layer_2, float x_2, float y_1, float z_2, float4 thread* result_2)
{

    float4 _S125 = float4(0.0, 0.0, 0.0, 0.0);

#line 449
    *result_2 = _S125;

    thread CyFieldTile_0 centre_1;
    (&centre_1)->layer_1 = layer_2;
    int _S126 = cyFieldFloorDiv_0(cyFieldIfloor_0(x_2 / header_5->cellMetres_0), int(16));

#line 453
    (&centre_1)->x_1 = _S126;
    int _S127 = cyFieldFloorDiv_0(cyFieldIfloor_0(z_2 / header_5->cellMetres_0), int(16));

#line 454
    (&centre_1)->z_1 = _S127;
    (&centre_1)->payload_0 = 0U;
    thread uint payload_2 = 0U;

#line 456
    bool _S128 = cyFieldFindEntry_0(image_words_8, header_5, layer_2, _S126, _S127, &payload_2);
    if(!_S128)
    {
        return false;
    }
    (&centre_1)->payload_0 = payload_2;

#line 461
    CyFieldLattice_0 _S129 = cyFieldLattice_0(header_5, x_2, y_1, z_2);


    if(!_S129.linear_0)
    {

#line 464
        thread CyFieldTile_0 _S130 = centre_1;

#line 464
        float4 _S131 = cyFieldReadPoint_0(image_words_8, header_5, &_S130, _S129.i0_0, _S129.k0_0, _S129.j0_0);

        *result_2 = _S131;
        return true;
    }

    thread float4 accumulated_0 = _S125;

#line 470
    uint _S132;
    if((header_5->verticalCells_0) > 1U)
    {

#line 471
        _S132 = 2U;

#line 471
    }
    else
    {

#line 471
        _S132 = 1U;

#line 471
    }

#line 471
    uint dy_1 = 0U;


    for(;;)
    {

#line 474
        if(dy_1 < _S132)
        {
        }
        else
        {

#line 474
            break;
        }
        float _S133 = cyFieldVerticalWeight_0(_S132, dy_1, _S129.fy_0);

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
            float _S134;

            if(dz_0 == 0U)
            {

#line 479
                _S134 = 1.0 - _S129.fz_0;

#line 479
            }
            else
            {

#line 479
                _S134 = _S129.fz_0;

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
                    wx_0 = 1.0 - _S129.fx_0;

#line 482
                }
                else
                {

#line 482
                    wx_0 = _S129.fx_0;

#line 482
                }

                int _S135 = _S129.i0_0 + int(dx_0);

#line 484
                int _S136 = _S129.k0_0 + int(dz_0);
                int _S137 = _S129.j0_0 + int(dy_1);

#line 485
                thread CyFieldTile_0 _S138 = centre_1;

#line 485
                float4 _S139 = cyFieldReadPoint_0(image_words_8, header_5, &_S138, _S135, _S136, _S137);
                float _S140 = wx_0 * _S134 * _S133;

#line 486
                uint index_7 = 0U;
                for(;;)
                {

#line 487
                    if(index_7 < (header_5->components_0))
                    {
                    }
                    else
                    {

#line 487
                        break;
                    }
                    accumulated_0[index_7] = accumulated_0[index_7] + _S139[index_7] * _S140;

#line 487
                    index_7 = index_7 + 1U;

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
    *result_2 = accumulated_0;
    return true;
}


float4 cyFieldCombine_0(uint rule_1, float4 base_1, float4 delta_0, uint components_1)
{
    thread float4 out_1 = base_1;

#line 501
    uint index_8 = 0U;
    for(;;)
    {

#line 502
        if(index_8 < components_1)
        {
        }
        else
        {

)cy_msl"
    R"cy_msl(#line 502
            break;
        }

#line 502
        uint _S141 = index_8;

#line 502
        uint _S142 = index_8;



        if(rule_1 == 0U)
        {
            out_1[index_8] = delta_0[_S142];

#line 506
        }
        else
        {

            if(rule_1 == 1U)
            {
                out_1[index_8] = base_1[_S141] + delta_0[_S142];

#line 510
            }
            else
            {

                if(rule_1 == 2U)
                {
                    out_1[index_8] = base_1[_S141] * delta_0[_S142];

#line 514
                }
                else
                {

#line 514
                    float _S143;



                    if(rule_1 == 3U)
                    {
                        if((base_1[_S141]) > (delta_0[_S142]))
                        {

#line 520
                            _S143 = base_1[_S141];

#line 520
                        }
                        else
                        {

#line 520
                            _S143 = delta_0[_S142];

#line 520
                        }

#line 520
                        out_1[index_8] = _S143;

#line 518
                    }
                    else
                    {

                        if(rule_1 == 4U)
                        {
                            if((base_1[_S141]) < (delta_0[_S142]))
                            {

#line 524
                                _S143 = base_1[_S141];

#line 524
                            }
                            else
                            {

#line 524
                                _S143 = delta_0[_S142];

#line 524
                            }

#line 524
                            out_1[index_8] = _S143;

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
        index_8 = index_8 + 1U;

#line 502
    }

#line 527
    return out_1;
}


#line 142
struct CyFieldSample_0
{
    float4 value_5;
    bool resolved_0;
};


#line 534
CyFieldSample_0 cyFieldSample_0(uint device* image_words_9, float x_3, float y_2, float z_3)
{
    thread CyFieldSample_0 answer_0;
    float4 _S144 = float4(0.0, 0.0, 0.0, 0.0);

#line 537
    (&answer_0)->value_5 = _S144;
    (&answer_0)->resolved_0 = false;
    if(!cyFieldIsImage_0(image_words_9))
    {
        return answer_0;
    }

#line 541
    CyFieldHeader_0 _S145 = cyFieldReadHeader_0(image_words_9);

#line 541
    uint index_9 = 0U;


    for(;;)
    {

#line 544
        if(index_9 < 4U)
        {
        }
        else
        {

#line 544
            break;
        }
        (&answer_0)->value_5[index_9] = _S145.defaults_0[index_9];

#line 544
        index_9 = index_9 + 1U;

#line 544
    }

#line 549
    thread float4 base_2 = _S144;
    thread float4 delta_1 = _S144;

#line 550
    thread CyFieldHeader_0 _S146 = _S145;

#line 550
    bool _S147 = cyFieldSampleLayer_0(image_words_9, &_S146, 0U, x_3, y_2, z_3, &base_2);

#line 550
    thread CyFieldHeader_0 _S148 = _S145;

#line 550
    bool _S149 = cyFieldSampleLayer_0(image_words_9, &_S148, 1U, x_3, y_2, z_3, &delta_1);


    bool _S150 = !_S147;

#line 553
    bool _S151;

#line 553
    if(_S150)
    {

#line 553
        _S151 = !_S149;

#line 553
    }
    else
    {

#line 553
        _S151 = false;

#line 553
    }

#line 553
    if(_S151)
    {
        return answer_0;
    }
    (&answer_0)->resolved_0 = true;
    if(_S150)
    {

#line 558
        index_9 = 0U;

        for(;;)
        {

#line 560
            if(index_9 < 4U)
            {
            }
            else
            {

#line 560
                break;
            }
            base_2[index_9] = _S145.defaults_0[index_9];

#line 560
            index_9 = index_9 + 1U;

#line 560
        }

#line 558
    }

#line 565
    if(!_S149)
    {
        (&answer_0)->value_5 = base_2;
        return answer_0;
    }
    (&answer_0)->value_5 = cyFieldCombine_0(_S145.rule_0, base_2, delta_1, _S145.components_0);
    return answer_0;
}


#line 35 "src/rendering/shaders/cy/cloud_shadow.slang"
float cyCloudShadowAtImage_0(uint device* image_words_10, float x_4, float y_3, float z_4)
{

#line 35
    CyFieldSample_0 _S152 = cyFieldSample_0(image_words_10, x_4, y_3, z_4);

#line 41
    return saturate(_S152.value_5.x);
}


#line 200 "samples/10-world/shaders/world.slang"
float sunThroughClouds_0(float3 world_0, KernelContext_0 thread* kernelContext_9)
{
    float4 placement_1 = kernelContext_9->cloudShadow_0->placement_0[int(0)];
    if((placement_1.z) < 0.5)
    {
        return 1.0;
    }

#line 205
    float _S153 = cyCloudShadowAtImage_0(kernelContext_9->cloudShadow_0->field_0, world_0.x + placement_1.x, world_0.y, world_0.z + placement_1.y);



    return _S153;
}


#line 58 "src/rendering/shaders/cy/aerial_perspective.slang"
bool cyAerialPerspectiveEnabled_0(float4 device* table_words_0)
{
    return (table_words_0[int(0)].w) > 0.5;
}

float cyAerialSliceDepth_0(float4 shape_1, float4 planes_0, float slice_0)
{
    float _S154 = shape_1.z;
    return mix(planes_0.x, planes_0.y, pow(min(slice_0 + 1.0, _S154) / _S154, max(shape_1.w, 1.0)));
}

float3 cyAerialFetch_0(float4 device* table_words_1, float4 shape_2, uint slice_1, uint x_5, uint y_4, uint which_0)
{

)cy_msl"
    R"cy_msl(    uint width_0 = uint(shape_2.x);


    return table_words_1[5U + (slice_1 * uint(shape_2.y) * width_0 + y_4 * width_0 + x_5) * 2U + which_0].xyz;
}

float3 cyAerialPlane_0(float4 device* table_words_2, float4 shape_3, uint slice_2, float2 texel_0, uint which_1)
{

    float _S155 = shape_3.x;
    float _S156 = shape_3.y;
    float2 clamped_0 = clamp(texel_0, float2(0.0) , float2(_S155 - 1.0, _S156 - 1.0));
    float _S157 = clamped_0.x;

#line 84
    uint x0_0 = uint(_S157);
    float _S158 = clamped_0.y;

#line 85
    uint y0_0 = uint(_S158);
    uint _S159 = min(x0_0 + 1U, uint(_S155) - 1U);
    uint _S160 = min(y0_0 + 1U, uint(_S156) - 1U);



    float3 _S161 = float3((_S157 - float(x0_0))) ;


    return mix(mix(cyAerialFetch_0(table_words_2, shape_3, slice_2, x0_0, y0_0, which_1), cyAerialFetch_0(table_words_2, shape_3, slice_2, _S159, y0_0, which_1), _S161), mix(cyAerialFetch_0(table_words_2, shape_3, slice_2, x0_0, _S160, which_1), cyAerialFetch_0(table_words_2, shape_3, slice_2, _S159, _S160, which_1), _S161), float3((_S158 - float(y0_0))) );
}


#line 42
struct CyAerialPerspective_0
{
    float3 transmittance_0;
    float3 inScattering_0;
};


#line 99
CyAerialPerspective_0 cyAerialPerspectiveAt_0(float4 device* table_words_3, float3 offset_1)
{
    thread CyAerialPerspective_0 result_3;
    float3 _S162 = float3(1.0) ;

#line 102
    (&result_3)->transmittance_0 = _S162;
    float3 _S163 = float3(0.0) ;

#line 103
    (&result_3)->inScattering_0 = _S163;

    float4 forward_0 = table_words_3[int(0)];
    float4 right_0 = table_words_3[int(1)];
    float4 up_0 = table_words_3[int(2)];
    float4 shape_4 = table_words_3[int(3)];
    float4 planes_1 = table_words_3[int(4)];
    float depth_0 = dot(offset_1, forward_0.xyz);

#line 110
    bool _S164;
    if((forward_0.w) < 0.5)
    {

#line 111
        _S164 = true;

#line 111
    }
    else
    {

#line 111
        _S164 = depth_0 <= 0.0;

#line 111
    }

#line 111
    if(_S164)
    {
        return result_3;
    }



    float2 texel_1 = float2((dot(offset_1, right_0.xyz) / (depth_0 * right_0.w) * 0.5 + 0.5) * shape_4.x - 0.5, (dot(offset_1, up_0.xyz) / (depth_0 * up_0.w) * 0.5 + 0.5) * shape_4.y - 0.5);



    float _S165 = shape_4.z;

#line 122
    uint last_0 = uint(_S165) - 1U;
    float first_0 = cyAerialSliceDepth_0(shape_4, planes_1, 0.0);



    float _S166 = saturate(depth_0 / max(first_0, 9.99999997475242708e-07));

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
        float _S167 = planes_1.x;


        uint _S168 = min(uint(max(pow(saturate((depth_0 - _S167) / max(planes_1.y - _S167, 9.99999997475242708e-07)), 1.0 / max(shape_4.w, 1.0)) * _S165 - 1.0, 0.0)), last_0);

#line 133
        float3 _S169 = cyAerialPlane_0(table_words_3, shape_4, _S168, texel_1, 0U);

#line 133
        float3 _S170 = cyAerialPlane_0(table_words_3, shape_4, _S168, texel_1, 1U);


        uint _S171 = _S168 + 1U;

#line 136
        uint _S172 = min(_S171, last_0);
        float nearEdge_0 = cyAerialSliceDepth_0(shape_4, planes_1, float(_S168));
        float farEdge_0 = cyAerialSliceDepth_0(shape_4, planes_1, float(_S171));
        if(_S168 == last_0)
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
        farSlice_0 = _S172;

#line 139
        nearT_0 = _S169;

#line 139
        nearS_0 = _S170;

#line 128
    }
    else
    {

#line 128
        farSlice_0 = 0U;

#line 128
        nearT_0 = _S162;

#line 128
        fraction_0 = _S166;

#line 128
        nearS_0 = _S163;

#line 128
    }

#line 128
    float3 _S173 = cyAerialPlane_0(table_words_3, shape_4, farSlice_0, texel_1, 1U);

#line 143
    float3 _S174 = float3(fraction_0) ;

#line 143
    (&result_3)->transmittance_0 = mix(nearT_0, cyAerialPlane_0(table_words_3, shape_4, farSlice_0, texel_1, 0U), _S174);
    (&result_3)->inScattering_0 = mix(nearS_0, _S173, _S174);
    return result_3;
}


float3 cyApplyAerialPerspective_0(float4 device* table_words_4, float3 radiance_1, float3 offset_2)
{

#line 150
    CyAerialPerspective_0 _S175 = cyAerialPerspectiveAt_0(table_words_4, offset_2);


    return radiance_1 * _S175.transmittance_0 + _S175.inScattering_0;
}


#line 227 "samples/10-world/shaders/world.slang"
float3 throughAir_0(float3 lit_0, float3 world_1, KernelContext_0 thread* kernelContext_10)
{

#line 227
    float4 device* table_words_5 = kernelContext_10->cloudShadow_0->aerial_0;



    if(!cyAerialPerspectiveEnabled_0(table_words_5))
    {
        return lit_0;
    }
    return cyApplyAerialPerspective_0(table_words_5, lit_0, world_1 - kernelContext_10->push_0->eye_0.xyz);
}


#line 235
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 235
struct pixelInput_0
{
    float3 world_2 [[user(TEXCOORD)]];
    float3 normal_1 [[user(TEXCOORD_1)]];
    float3 color_0 [[user(TEXCOORD_2)]];
};


#line 239
[[fragment]] pixelOutput_0 worldFragment(pixelInput_0 _S176 [[stage_in]], float4 clip_0 [[position]], WorldPush_0 constant* push_1 [[buffer(1)]], WorldCloudShadow_default_0 constant* cloudShadow_1 [[buffer(0)]])
{

#line 239
    thread KernelContext_0 kernelContext_11;

#line 239
    (&kernelContext_11)->push_0 = push_1;

#line 239
    (&kernelContext_11)->cloudShadow_0 = cloudShadow_1;

    if((push_1->eye_0.w) > 0.5)
    {

#line 241
        pixelOutput_0 _S177 = { float4(tonemap_0(_S176.color_0), 1.0) };



        return _S177;
    }

    float3 normal_2 = normalize(_S176.normal_1);
    float3 toEye_0 = normalize(push_1->eye_0.xyz - _S176.world_2);

#line 249
    float3 normal_3;
    if((dot(normal_2, toEye_0)) < 0.0)
    {

#line 250
        normal_3 = - normal_2;

#line 250
    }
    else
    {

#line 250
        normal_3 = normal_2;

#line 250
    }

#line 250
    bool _S178;

#line 261
    if(((&kernelContext_11)->push_0->sun_0.w) <= 0.0)
    {

#line 261
        uint _S179 = cyDecalCount_0(&kernelContext_11);

#line 261
        _S178 = _S179 > 0U;

#line 261
    }
    else
    {

#line 261
        _S178 = false;

)cy_msl"
    R"cy_msl(#line 261
    }

#line 261
    float3 albedo_1;

#line 261
    if(_S178)
    {
        thread CyDecalReceiver_0 receiver_2;
        (&receiver_2)->relativePosition_0 = _S176.world_2;
        (&receiver_2)->geometricNormal_0 = normal_3;
        (&receiver_2)->eyeDistance_0 = length(_S176.world_2 - push_1->eye_0.xyz);
        (&receiver_2)->channels_0 = 1U;
        thread CyDecalSurface_0 decalled_0;
        (&decalled_0)->albedo_0 = _S176.color_0;
        (&decalled_0)->roughness_0 = 1.0;
        (&decalled_0)->metallic_0 = 0.0;
        (&decalled_0)->emission_0 = float3(0.0) ;
        (&decalled_0)->normal_0 = normal_3;
        float2 _S180 = clip_0.xy;

#line 274
        thread CyDecalReceiver_0 _S181 = receiver_2;

#line 274
        cyApplyTableDecals_0(&_S181, _S180, &decalled_0, &kernelContext_11);

#line 274
        normal_3 = (&decalled_0)->normal_0;

#line 274
        albedo_1 = (&decalled_0)->albedo_0;

#line 261
    }
    else
    {

#line 261
        albedo_1 = _S176.color_0;

#line 261
    }

#line 281
    float3 _S182 = (&kernelContext_11)->push_0->sun_0.xyz;

#line 281
    float _S183 = sunThroughClouds_0(_S176.world_2, &kernelContext_11);

#line 281
    float3 _S184 = _S182 * float3(_S183) ;

    float3 lit_1 = albedo_1 * ((&kernelContext_11)->push_0->ambient_0.xyz + _S184 * float3(saturate(dot(normal_3, - (&kernelContext_11)->push_0->light_0.xyz))) );

#line 283
    float3 lit_2;

    if(((&kernelContext_11)->push_0->sun_0.w) > 0.0)
    {

#line 285
        lit_2 = lit_1 + _S184 * float3((pow(saturate(dot(normal_3, normalize(- (&kernelContext_11)->push_0->light_0.xyz + toEye_0))), 120.0) * (&kernelContext_11)->push_0->sun_0.w)) ;

#line 285
    }
    else
    {

#line 285
        lit_2 = lit_1;

#line 285
    }

#line 285
    float3 _S185 = throughAir_0(lit_2, _S176.world_2, &kernelContext_11);

#line 285
    pixelOutput_0 _S186 = { float4(tonemap_0(_S185), 1.0) };

#line 296
    return _S186;
}

)cy_msl";

}  // namespace cy::sample::world

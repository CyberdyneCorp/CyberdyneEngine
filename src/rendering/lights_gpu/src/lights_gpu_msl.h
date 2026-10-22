#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the device light dispatches (src/rendering/lights_gpu). GENERATED — do not edit
// by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::lights_gpu {

/// cyAssignLights.metal, 16030 bytes.
inline constexpr char kAssignLightsMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 39 "src/rendering/lights_gpu/shaders/light_assignment.slang"
struct CyAssignConstants_0
{
    uint4 dimensions_0;
    float4 planes_0;
    float4 view_0;
    uint4 counts_0;
    uint4 copies_0;
};


#line 72
void clusterBox_0(const CyAssignConstants_0 thread* c_0, uint x_0, uint y_0, uint slice_0, float3 thread* boxMin_0, float3 thread* boxMax_0)
{

#line 73
    float4 _S1 = c_0->planes_0;

    float _S2 = c_0->planes_0.z;

#line 75
    float _S3;

#line 75
    if(_S2 != 0.0)
    {

#line 75
        _S3 = _S2;

#line 75
    }
    else
    {

#line 75
        _S3 = 1.0;

#line 75
    }
    float _S4 = _S1.x;

#line 76
    float _S5 = _S4 * exp2(float(slice_0) / _S3);
    float _S6 = _S4 * exp2(float(slice_0 + 1U) / _S3);
    float _S7 = float(c_0->dimensions_0.x);

    float _S8 = float(c_0->dimensions_0.y);

    float _S9 = c_0->view_0.x;

#line 82
    float _S10 = _S9 * c_0->view_0.y;

    float _S11 = (float(x_0) / _S7 * 2.0 - 1.0) * _S10;

#line 84
    float _S12 = min(_S11 * _S5, _S11 * _S6);
    float _S13 = (float(x_0 + 1U) / _S7 * 2.0 - 1.0) * _S10;

#line 85
    float _S14 = max(_S13 * _S5, _S13 * _S6);
    float _S15 = (1.0 - float(y_0 + 1U) / _S8 * 2.0) * _S9;

#line 86
    float _S16 = min(_S15 * _S5, _S15 * _S6);
    float _S17 = (1.0 - float(y_0) / _S8 * 2.0) * _S9;

#line 87
    float _S18 = max(_S17 * _S5, _S17 * _S6);
    *boxMin_0 = float3(min(_S12, _S14), min(_S16, _S18), - _S6);
    *boxMax_0 = float3(max(_S12, _S14), max(_S16, _S18), - _S5);
    return;
}



bool sphereInTile_0(const CyAssignConstants_0 thread* c_1, uint x_1, uint y_1, float4 sphere_0)
{
    float _S19 = c_1->view_0.x;

#line 97
    float _S20 = _S19 * c_1->view_0.y;

    float _S21 = float(c_1->dimensions_0.x);

    float _S22 = float(c_1->dimensions_0.y);

    float3 _S23 = sphere_0.xyz;

    float3 _S24 = float3(1.0, 0.0, (float(x_1) / _S21 * 2.0 - 1.0) * _S20);
    float3 _S25 = float3(-1.0, 0.0, - ((float(x_1 + 1U) / _S21 * 2.0 - 1.0) * _S20));
    float3 _S26 = float3(0.0, 1.0, (1.0 - float(y_1 + 1U) / _S22 * 2.0) * _S19);
    float3 _S27 = float3(0.0, -1.0, - ((1.0 - float(y_1) / _S22 * 2.0) * _S19));
    float _S28 = - sphere_0.w;

#line 109
    bool _S29;

#line 109
    if((dot(_S23, _S24)) >= (_S28 * length(_S24)))
    {

#line 109
        _S29 = (dot(_S23, _S25)) >= (_S28 * length(_S25));

#line 109
    }
    else
    {

#line 109
        _S29 = false;

#line 109
    }

#line 109
    if(_S29)
    {

#line 109
        _S29 = (dot(_S23, _S26)) >= (_S28 * length(_S26));

#line 109
    }
    else
    {

#line 109
        _S29 = false;

#line 109
    }
    if(_S29)
    {

#line 110
        _S29 = (dot(_S23, _S27)) >= (_S28 * length(_S27));

#line 110
    }
    else
    {

#line 110
        _S29 = false;

#line 110
    }

#line 109
    return _S29;
}



bool coneReachesSphere_0(float3 apex_0, float3 axis_0, float coneCos_0, float range_0, float3 center_0, float radius_0)
{

    float3 _S30 = center_0 - apex_0;
    float _S31 = length(_S30);
    if(_S31 > (range_0 + radius_0))
    {
        return false;
    }
    if(_S31 <= radius_0)
    {
        return true;
    }

    float _S32 = dot(_S30, axis_0);

    return (coneCos_0 * sqrt(max(_S31 * _S31 - _S32 * _S32, 0.0)) - sqrt(max(1.0 - coneCos_0 * coneCos_0, 0.0)) * _S32) <= radius_0;
}


#line 27
struct CyClusterElement_0
{
    float3 viewPosition_0;
    float radius_1;
    float3 viewDirection_0;
    float coneCos_1;
    uint payload_0;
    uint layerMask_0;
    uint2 reserved_0;
};


#line 48
struct CyAssignSet_default_0
{
    CyAssignConstants_0 device* constants_0;
    CyClusterElement_0 device* elements_0;
    uint2 device* hostHeaders_0;
    uint device* hostIndices_0;
    uint2 device* headers_0;
    uint device* indices_0;
    uint device* statistics_0;
};


#line 173
struct KernelContext_0
{
    CyAssignSet_default_0 constant* cyAssignSet_0;
    array<float4, int(64)> threadgroup* gSphere_0;
    array<float4, int(64)> threadgroup* gCone_0;
    array<uint, int(64)> threadgroup* gPayload_0;
    array<uint, int(64)> threadgroup* gVisible_0;
    array<uint, int(64)> threadgroup* gCompacted_0;
    uint threadgroup* gCompactedCount_0;
};


#line 135
[[kernel]] void cyAssignLights(uint3 group_0 [[threadgroup_position_in_grid]], uint3 thread_0 [[thread_position_in_threadgroup]], uint3 dispatch_0 [[thread_position_in_grid]], CyAssignSet_default_0 constant* cyAssignSet_1 [[buffer(0)]])
{

#line 136
    uint4 _S33;

#line 136
    uint visible_0;

#line 136
    uint k_0;

#line 136
    thread KernelContext_0 kernelContext_0;

#line 136
    (&kernelContext_0)->cyAssignSet_0 = cyAssignSet_1;

#line 136
    threadgroup array<float4, int(64)> gSphere_1;

#line 136
    (&kernelContext_0)->gSphere_0 = &gSphere_1;

#line 136
    threadgroup array<float4, int(64)> gCone_1;

#line 136
    (&kernelContext_0)->gCone_0 = &gCone_1;

#line 136
    threadgroup array<uint, int(64)> gPayload_1;

#line 136
    (&kernelContext_0)->gPayload_0 = &gPayload_1;

#line 136
    threadgroup array<uint, int(64)> gVisible_1;

#line 136
    (&kernelContext_0)->gVisible_0 = &gVisible_1;

#line 136
    threadgroup array<uint, int(64)> gCompacted_1;

#line 136
    (&kernelContext_0)->gCompacted_0 = &gCompacted_1;

#line 136
    threadgroup uint gCompactedCount_1;

#line 136
    (&kernelContext_0)->gCompactedCount_0 = &gCompactedCount_1;

    CyAssignConstants_0 _S34 = cyAssignSet_1->constants_0[int(0)];
    uint _S35 = group_0.x;
    uint _S36 = group_0.y;
    uint _S37 = thread_0.x;

    uint _S38 = _S34.dimensions_0.w;

#line 143
    uint _S39 = min(_S38, 64U);

#line 143
    bool _S40;
    if(_S37 < (_S34.dimensions_0.z))
    {

#line 144
        _S40 = _S35 < (_S34.dimensions_0.x);

#line 144
    }
    else
    {

#line 144
        _S40 = false;

)cy_msl"
    R"cy_msl(#line 144
    }

#line 144
    if(_S40)
    {

#line 144
        _S40 = _S36 < (_S34.dimensions_0.y);

#line 144
    }
    else
    {

#line 144
        _S40 = false;

#line 144
    }
    uint _S41 = _S34.counts_0.z;



    uint _S42 = _S34.dimensions_0.x;

#line 149
    uint _S43 = _S34.dimensions_0.y;

#line 149
    uint _S44 = _S42 * _S43 * 64U;

#line 149
    uint index_0 = (_S36 * _S42 + _S35) * 64U + _S37;

    for(;;)
    {

#line 151
        _S33 = _S34.copies_0;

#line 151
        if(index_0 < (_S34.copies_0.y))
        {
        }
        else
        {

#line 151
            break;
        }
        *((&kernelContext_0)->cyAssignSet_0->indices_0+index_0) = (&kernelContext_0)->cyAssignSet_0->hostIndices_0[index_0];

#line 151
        index_0 = index_0 + _S44;

#line 151
    }

#line 156
    float3 _S45 = float3(0.0) ;

#line 156
    thread float3 boxMin_1 = _S45;
    thread float3 boxMax_1 = _S45;

#line 157
    uint cluster_0;

    if(_S40)
    {

#line 159
        thread CyAssignConstants_0 _S46 = _S34;

#line 159
        clusterBox_0(&_S46, _S35, _S36, _S37, &boxMin_1, &boxMax_1);


        uint _S47 = _S35 + _S42 * (_S36 + _S43 * _S37);

#line 162
        cluster_0 = 1U;
        for(;;)
        {

#line 163
            if(cluster_0 < _S41)
            {
            }
            else
            {

#line 163
                break;
            }
            uint _S48 = _S47 * _S41 + cluster_0;
            uint2 device* _S49 = (&kernelContext_0)->cyAssignSet_0->headers_0+_S48;

#line 166
            uint2 _S50;
            if((_S33.x) != 0U)
            {

#line 167
                _S50 = (&kernelContext_0)->cyAssignSet_0->hostHeaders_0[_S48];

#line 167
            }
            else
            {

#line 167
                _S50 = uint2(0U, 0U);

#line 167
            }

#line 166
            *_S49 = _S50;

#line 163
            cluster_0 = cluster_0 + 1U;

#line 163
        }

#line 163
        cluster_0 = _S47;

#line 159
    }
    else
    {

#line 159
        cluster_0 = 0U;

#line 159
    }

#line 159
    float3 _S51 = float3(0.5) ;

#line 170
    float3 _S52 = (boxMin_1 + boxMax_1) * _S51;
    float _S53 = length((boxMax_1 - boxMin_1) * _S51);

    thread array<uint, int(64)> kept_0;
    thread array<float, int(64)> keptDistance_0;

#line 174
    uint batch_0 = 0U;

#line 174
    uint size_0 = 0U;

#line 174
    uint dropped_0 = 0U;



    for(;;)
    {

#line 178
        uint _S54 = _S34.counts_0.x;

#line 178
        if(batch_0 < _S54)
        {
        }
        else
        {

#line 178
            break;
        }
        uint _S55 = batch_0 + _S37;

#line 180
        bool _S56;

        if(_S55 < _S54)
        {
            CyClusterElement_0 _S57 = (&kernelContext_0)->cyAssignSet_0->elements_0[_S55];
            float4 _S58 = float4(_S57.viewPosition_0, _S57.radius_1);

#line 185
            (*(&kernelContext_0)->gSphere_0)[_S37] = _S58;
            (*(&kernelContext_0)->gCone_0)[_S37] = float4(_S57.viewDirection_0, _S57.coneCos_1);
            (*(&kernelContext_0)->gPayload_0)[_S37] = _S57.payload_0;

            if(((_S57.layerMask_0) & (_S34.counts_0.y)) != 0U)
            {

#line 189
                thread CyAssignConstants_0 _S59 = _S34;

#line 189
                bool _S60 = sphereInTile_0(&_S59, _S35, _S36, _S58);

#line 189
                _S56 = _S60;

#line 189
            }
            else
            {

#line 189
                _S56 = false;

#line 189
            }


            if(_S56)
            {

#line 192
                index_0 = 1U;

#line 192
            }
            else
            {

#line 192
                index_0 = 0U;

#line 192
            }

#line 192
            visible_0 = index_0;

#line 182
        }
        else
        {

#line 182
            visible_0 = 0U;

#line 182
        }

#line 195
        (*(&kernelContext_0)->gVisible_0)[_S37] = visible_0;
        threadgroup_barrier(mem_flags::mem_threadgroup);

#line 196
        uint size_1;
        if(_S37 == 0U)
        {

#line 197
            k_0 = 0U;

#line 197
            size_1 = 0U;


            for(;;)
            {

#line 200
                if(k_0 < 64U)
                {
                }
                else
                {

#line 200
                    break;
                }
                if(((*(&kernelContext_0)->gVisible_0)[k_0]) != 0U)
                {
                    (*(&kernelContext_0)->gCompacted_0)[size_1] = k_0;

#line 204
                    size_1 = size_1 + 1U;

#line 202
                }

#line 200
                k_0 = k_0 + 1U;

#line 200
            }

#line 208
            *(&kernelContext_0)->gCompactedCount_0 = size_1;

#line 197
        }

#line 210
        threadgroup_barrier(mem_flags::mem_threadgroup);

)cy_msl"
    R"cy_msl(#line 210
        uint dropped_1;
        if(_S40)
        {

#line 211
            k_0 = 0U;

#line 211
            size_1 = size_0;

#line 211
            dropped_1 = dropped_0;

            for(;;)
            {

#line 213
                if(k_0 < (*(&kernelContext_0)->gCompactedCount_0))
                {
                }
                else
                {

#line 213
                    break;
                }
                uint _S61 = (*(&kernelContext_0)->gCompacted_0)[k_0];

                float3 _S62 = (*(&kernelContext_0)->gSphere_0)[(*(&kernelContext_0)->gCompacted_0)[k_0]].xyz;
                float3 _S63 = _S62 - clamp(_S62, boxMin_1, boxMax_1);
                float _S64 = (*(&kernelContext_0)->gSphere_0)[(*(&kernelContext_0)->gCompacted_0)[k_0]].w;

#line 219
                if((dot(_S63, _S63)) > (_S64 * _S64))
                {
                    k_0 = k_0 + 1U;

#line 213
                    continue;
                }

#line 223
                float4 _S65 = (*(&kernelContext_0)->gCone_0)[_S61];
                float _S66 = (*(&kernelContext_0)->gCone_0)[_S61].w;

#line 224
                if(_S66 < 1.0)
                {

#line 224
                    _S56 = !coneReachesSphere_0(_S62, _S65.xyz, _S66, _S64, _S52, _S53);

#line 224
                }
                else
                {

#line 224
                    _S56 = false;

#line 224
                }

#line 224
                if(_S56)
                {

                    k_0 = k_0 + 1U;

#line 213
                    continue;
                }

#line 229
                float3 _S67 = _S62 - _S52;
                float _S68 = dot(_S67, _S67);
                if(size_1 < _S39)
                {
                    kept_0[size_1] = (*(&kernelContext_0)->gPayload_0)[_S61];
                    keptDistance_0[size_1] = _S68;

#line 234
                    size_1 = size_1 + 1U;

                    k_0 = k_0 + 1U;

#line 213
                    continue;
                }

#line 239
                uint dropped_2 = dropped_1 + 1U;

#line 239
                uint furthest_0 = 0U;

#line 239
                uint i_0 = 1U;

                for(;;)
                {

#line 241
                    if(i_0 < size_1)
                    {
                    }
                    else
                    {

#line 241
                        break;
                    }
                    if((keptDistance_0[i_0]) > keptDistance_0[furthest_0])
                    {

#line 243
                        furthest_0 = i_0;

#line 243
                    }

#line 241
                    i_0 = i_0 + 1U;

#line 241
                }

#line 248
                if(_S68 < keptDistance_0[furthest_0])
                {
                    kept_0[furthest_0] = (*(&kernelContext_0)->gPayload_0)[_S61];
                    keptDistance_0[furthest_0] = _S68;

#line 248
                }

#line 248
                dropped_1 = dropped_2;

#line 213
                k_0 = k_0 + 1U;

#line 213
            }

#line 211
        }
        else
        {

#line 211
            size_1 = size_0;

#line 211
            dropped_1 = dropped_0;

#line 211
        }

#line 255
        threadgroup_barrier(mem_flags::mem_threadgroup);

#line 178
        batch_0 = batch_0 + 64U;

#line 178
        size_0 = size_1;

#line 178
        dropped_0 = dropped_1;

#line 178
    }

#line 258
    if(!_S40)
    {
        return;
    }

#line 260
    visible_0 = 1U;


    for(;;)
    {

#line 263
        if(visible_0 < size_0)
        {
        }
        else
        {

#line 263
            break;
        }
        uint _S69 = kept_0[visible_0];

#line 265
        k_0 = visible_0;

        for(;;)
        {

#line 267
            if(k_0 > 0U)
            {

#line 267
                _S40 = (kept_0[k_0 - 1U]) > _S69;

#line 267
            }
            else
            {

#line 267
                _S40 = false;

#line 267
            }

#line 267
            if(_S40)
            {
            }
            else
            {

#line 267
                break;
            }
            uint _S70 = k_0 - 1U;

#line 269
            kept_0[k_0] = kept_0[_S70];

#line 269
            k_0 = _S70;

#line 267
        }

#line 272
        kept_0[k_0] = _S69;

#line 263
        visible_0 = visible_0 + 1U;

#line 263
    }

#line 274
    uint _S71 = _S34.counts_0.w + cluster_0 * _S38;

#line 274
    visible_0 = 0U;
    for(;;)
    {

#line 275
        if(visible_0 < size_0)
        {
        }
        else
        {

#line 275
            break;
        }
        *((&kernelContext_0)->cyAssignSet_0->indices_0+(_S71 + visible_0)) = kept_0[visible_0];

#line 275
        visible_0 = visible_0 + 1U;

#line 275
    }



    *((&kernelContext_0)->cyAssignSet_0->headers_0+cluster_0 * _S41) = uint2(_S71, size_0);
    if(dropped_0 != 0U)
    {
        uint _S72 = atomic_fetch_add_explicit(((atomic_uint device*)((&kernelContext_0)->cyAssignSet_0->statistics_0+int(0))), dropped_0, memory_order_relaxed);

#line 280
    }



    if(size_0 != 0U)
    {
        uint _S73 = atomic_fetch_add_explicit(((atomic_uint device*)((&kernelContext_0)->cyAssignSet_0->statistics_0+int(1))), size_0, memory_order_relaxed);
        uint _S74 = atomic_fetch_max_explicit(((atomic_uint device*)((&kernelContext_0)->cyAssignSet_0->statistics_0+int(2))), size_0, memory_order_relaxed);

#line 284
    }

#line 289
    return;
}

)cy_msl";

/// cyManyLightsCandidates.metal, 30157 bytes.
inline constexpr char kManyLightsCandidatesMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    float3 positionRelativeToCamera_0;
    float range_0;
    float3 direction_0;
    float intensity_0;
    float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 31 "src/rendering/lights_gpu/shaders/many_lights.slang"
struct CyManyLightSet_default_0
{
    uint4 device* words_0;
    Light_0 device* lights_0;
    uint4 device* candidates_0;
    depth2d<float, access::sample> depth_0;
    texture2d<float, access::sample> normals_0;
    texture2d<float, access::read_write> temporalReservoir_0;
    texture2d<float, access::read_write> historyReservoir_0;
    texture2d<float, access::read_write> finalReservoir_0;
    texture2d<float, access::read_write> historySurface_0;
    texture2d<float, access::read_write> surface_0;
    texture2d<float, access::read_write> guidance_0;
    texture2d<float, access::read_write> noisy_0;
    texture2d<float, access::read_write> resolved_0;
    texture2d<float, access::read_write> historyColour_0;
    texture2d<float, access::read_write> historyMoments_0;
    texture2d<float, access::read_write> accumulated_0;
    texture2d<float, access::read_write> moments_0;
    texture2d<float, access::read_write> variance_0;
    texture2d<float, access::read_write> filterSource_0;
    texture2d<float, access::read_write> filterOut_0;
    texture2d<float, access::read_write> target_0;
};


#line 31
struct KernelContext_0
{
    CyManyLightSet_default_0 constant* cyManyLights_0;
};


#line 95
float4 word_0(uint index_0, KernelContext_0 thread* kernelContext_0)
{
    return (as_type<float4>((kernelContext_0->cyManyLights_0->words_0[index_0])));
}


#line 95
float4 word_1(uint index_1, KernelContext_0 thread* kernelContext_1)
{
    return (as_type<float4>((kernelContext_1->cyManyLights_0->words_0[index_1])));
}


#line 154
float3 relativeFromClip_0(float2 ndc_0, float depth_1, KernelContext_0 thread* kernelContext_2)
{
    float4 _S1 = float4(ndc_0, depth_1, 1.0);

#line 156
    float4 _S2 = word_1(0U, kernelContext_2);
    float4 _S3 = _S2 * float4(_S1.x) ;

#line 157
    float4 _S4 = word_1(1U, kernelContext_2);

#line 157
    float4 _S5 = _S3 + _S4 * float4(_S1.y) ;

#line 157
    float4 _S6 = word_1(2U, kernelContext_2);

#line 157
    float4 _S7 = _S5 + _S6 * float4(_S1.z) ;

#line 157
    float4 _S8 = word_1(3U, kernelContext_2);

#line 157
    float4 _S9 = _S7 + _S8 * float4(_S1.w) ;
    return _S9.xyz / float3(_S9.w) ;
}


#line 23 "src/rendering/shaders/cy/packing.slang"
float3 decodeOctahedral_0(float2 encoded_0)
{
    float2 _S10 = encoded_0 * float2(2.0)  - float2(1.0) ;
    float _S11 = _S10.x;

#line 26
    float _S12 = _S10.y;

#line 26
    float _S13 = 1.0 - abs(_S11) - abs(_S12);

#line 26
    thread float3 normal_0 = float3(_S11, _S12, _S13);
    float _S14 = saturate(- _S13);
    float2 _S15 = float2(_S11, _S12);

#line 28
    normal_0.xy = _S15 + select(float2(_S14) , float2(- _S14) , _S15 >= (float2(0.0) ));
    return normalize(normal_0);
}


#line 146 "src/rendering/lights_gpu/shaders/many_lights.slang"
struct CySurface_0
{
    bool valid_0;
    float3 position_0;
    float3 normal_1;
    float viewDepth_0;
};


#line 161
CySurface_0 surfaceAt_0(uint2 pixel_0, KernelContext_0 thread* kernelContext_3)
{

    int3 _S16 = int3(int2(pixel_0), int(0));

#line 164
    float _S17 = ((kernelContext_3->cyManyLights_0->depth_0).read(vec<uint,2>(((_S16)).xy), uint(((_S16)).z)));

#line 163
    thread CySurface_0 surface_1;

    bool _S18 = _S17 > 0.0;

#line 165
    (&surface_1)->valid_0 = _S18;

#line 165
    float4 _S19 = word_0(8U, kernelContext_3);

    float2 _S20 = (float2(pixel_0) + float2(0.5) ) * _S19.zw;
    float2 _S21 = float2(_S20.x * 2.0 - 1.0, 1.0 - _S20.y * 2.0);

#line 168
    float3 _S22;
    if(_S18)
    {

#line 169
        float3 _S23 = relativeFromClip_0(_S21, _S17, kernelContext_3);

#line 169
        _S22 = _S23;

#line 169
    }
    else
    {

#line 169
        _S22 = float3(0.0) ;

#line 169
    }

#line 169
    (&surface_1)->position_0 = _S22;

    (&surface_1)->normal_1 = decodeOctahedral_0(((kernelContext_3->cyManyLights_0->normals_0).read(vec<uint,2>(((_S16)).xy), uint(((_S16)).z))).xy);

#line 171
    float _S24;
    if((&surface_1)->valid_0)
    {

#line 172
        float4 _S25 = word_0(11U, kernelContext_3);

#line 172
        _S24 = - dot(_S25.xyz, (&surface_1)->position_0);

#line 172
    }
    else
    {

#line 172
        _S24 = 0.0;

#line 172
    }

#line 172
    (&surface_1)->viewDepth_0 = _S24;
    return surface_1;
}


#line 314
int historySource_0(const CySurface_0 thread* surface_2, uint2 extent_0, float thread* confidence_0, KernelContext_0 thread* kernelContext_4)
{
    *confidence_0 = 0.0;

#line 316
    bool _S26;
    if(!surface_2->valid_0)
    {

#line 317
        _S26 = true;

#line 317
    }
    else
    {

#line 317
        float4 _S27 = word_0(10U, kernelContext_4);

#line 317
        _S26 = (_S27.z) < 0.5;

#line 317
    }

#line 317
    if(_S26)
    {
        return int(-1);
    }
    float4 _S28 = float4(surface_2->position_0, 1.0);

#line 321
    float4 _S29 = word_0(4U, kernelContext_4);
    float _S30 = dot(_S29, _S28);

#line 322
    float4 _S31 = word_0(5U, kernelContext_4);

#line 322
    float _S32 = dot(_S31, _S28);

#line 322
    float4 _S33 = word_0(7U, kernelContext_4);

#line 322
    float _S34 = dot(_S33, _S28);
    if(_S34 <= 0.0)
    {
        return int(-1);
    }
    float2 _S35 = float2(_S30, _S32) / float2(_S34) ;
    float _S36 = _S35.x * 0.5 + 0.5;

#line 328
    float _S37 = 0.5 - _S35.y * 0.5;

#line 328
    float2 _S38 = float2(_S36, _S37);
    if(_S36 < 0.0)
    {

#line 329
        _S26 = true;

#line 329
    }
    else
    {

#line 329
        _S26 = _S37 < 0.0;

#line 329
    }

#line 329
    if(_S26)
    {

#line 329
        _S26 = true;

#line 329
    }
    else
    {

#line 329
        _S26 = _S36 >= 1.0;

)cy_msl"
    R"cy_msl(#line 329
    }

#line 329
    if(_S26)
    {

#line 329
        _S26 = true;

#line 329
    }
    else
    {

#line 329
        _S26 = _S37 >= 1.0;

#line 329
    }

#line 329
    if(_S26)
    {
        return int(-1);
    }
    uint2 _S39 = uint2(_S38 * float2(extent_0));
    float4 _S40 = ((kernelContext_4->cyManyLights_0->historySurface_0).read(vec<uint,2>(((int2(_S39))).xy)));
    float _S41 = _S40.x;

#line 335
    if(_S41 <= 0.0)
    {

#line 335
        _S26 = true;

#line 335
    }
    else
    {

#line 335
        _S26 = (abs(_S41 - surface_2->viewDepth_0)) > (0.10000000149011612 * surface_2->viewDepth_0);

#line 335
    }

#line 335
    if(_S26)
    {

#line 335
        _S26 = true;

#line 335
    }
    else
    {

#line 335
        _S26 = (dot(_S40.yzw, surface_2->normal_1)) < 0.89999997615814209;

#line 335
    }

#line 335
    if(_S26)
    {

        return int(-1);
    }
    *confidence_0 = 1.0;
    return int(_S39.y * extent_0.x + _S39.x);
}


#line 197
struct Reservoir_0
{
    float light_0;
    float weightSum_0;
    float count_0;
    float target_1;
};

Reservoir_0 emptyReservoir_0()
{
    thread Reservoir_0 reservoir_0;
    (&reservoir_0)->light_0 = -1.0;
    (&reservoir_0)->weightSum_0 = 0.0;
    (&reservoir_0)->count_0 = 0.0;
    (&reservoir_0)->target_1 = 0.0;
    return reservoir_0;
}


#line 100
uint wordBits_0(uint index_2, uint lane_0, KernelContext_0 thread* kernelContext_5)
{
    return kernelContext_5->cyManyLights_0->words_0[index_2][lane_0];
}


#line 225
float4 packReservoir_0(const Reservoir_0 thread* reservoir_1)
{
    return float4(reservoir_1->light_0, reservoir_1->weightSum_0, reservoir_1->count_0, reservoir_1->target_1);
}


#line 120
uint lowbias32_0(uint value_0)
{


    uint x_0 = (value_0 ^ (value_0 >> 16U)) * 2146121005U;

    uint x_1 = (x_0 ^ (x_0 >> 15U)) * 2221713035U;

    return x_1 ^ (x_1 >> 16U);
}


#line 115
struct Stream_0
{
    uint state_0;
};


#line 131
Stream_0 streamFor_0(uint2 pixel_1, uint frame_0, uint salt_0)
{
    thread Stream_0 stream_0;
    (&stream_0)->state_0 = lowbias32_0(pixel_1.x + lowbias32_0(pixel_1.y + lowbias32_0(frame_0 + lowbias32_0(salt_0))));
    return stream_0;
}

float nextUnit_0(Stream_0 thread* stream_1)
{
    uint _S42 = stream_1->state_0 + 2654435769U;

#line 140
    stream_1->state_0 = _S42;
    return float((lowbias32_0(_S42)) >> 8U) * 5.9604644775390625e-08;
}


#line 297
void drawCandidate_0(Stream_0 thread* stream_2, uint count_1, uint thread* light_1, float thread* pdf_0, KernelContext_0 thread* kernelContext_6)
{
    float _S43 = nextUnit_0(stream_2);

#line 299
    uint _S44 = min(uint(_S43 * float(count_1)), count_1 - 1U);
    uint4 _S45 = kernelContext_6->cyManyLights_0->candidates_0[_S44];
    float _S46 = nextUnit_0(stream_2);

#line 301
    uint _S47;

#line 301
    if(_S46 < (as_type<float>((_S45.z))))
    {

#line 301
        _S47 = _S44;

#line 301
    }
    else
    {

#line 301
        _S47 = _S45.y;

#line 301
    }
    uint4 _S48 = kernelContext_6->cyManyLights_0->candidates_0[_S47];
    *light_1 = _S48.x;
    *pdf_0 = (as_type<float>((_S48.w)));
    return;
}


#line 114 "src/rendering/shaders/cy/light.slang"
bool isAreaLight_0(uint kind_1)
{
    uint _S49 = kind_1 & 255U;

#line 116
    bool _S50;
    if(_S49 >= 4U)
    {

#line 117
        _S50 = _S49 <= 7U;

#line 117
    }
    else
    {

#line 117
        _S50 = false;

#line 117
    }

#line 117
    return _S50;
}


#line 362
float areaLightWindow_0(const Light_0 thread* light_2, float3 surfaceRelativeToCamera_0)
{
    float3 _S51 = light_2->positionRelativeToCamera_0 - surfaceRelativeToCamera_0;
    float _S52 = dot(_S51, _S51) / max(light_2->range_0 * light_2->range_0, 9.99999997475242708e-07);
    float _S53 = saturate(1.0 - _S52 * _S52);
    return _S53 * _S53;
}


#line 161
float3 unitOr_0(float3 value_1, float3 fallback_0)
{
    float _S54 = dot(value_1, value_1);

#line 163
    float3 _S55;
    if(_S54 > 9.99999968265522539e-21)
    {

#line 164
        _S55 = value_1 * float3(rsqrt(_S54)) ;

#line 164
    }
    else
    {

#line 164
        _S55 = fallback_0;

#line 164
    }

#line 164
    return _S55;
}


#line 120
float signNotZero_0(float value_2)
{

#line 120
    float _S56;

    if(value_2 >= 0.0)
    {

#line 122
        _S56 = 1.0;

#line 122
    }
    else
    {

#line 122
        _S56 = -1.0;

#line 122
    }

#line 122
    return _S56;
}


float3 decodeAreaTangent_0(uint word_2)
{
    float _S57 = max(float(int(word_2 << 16U) >> 16U) / 32767.0, -1.0);
    float _S58 = max(float(int(word_2) >> 16U) / 32767.0, -1.0);
    float _S59 = 1.0 - abs(_S57) - abs(_S58);

#line 130
    thread float3 unit_0 = float3(_S57, _S58, _S59);
    if(_S59 < 0.0)
    {

        float _S60 = (1.0 - abs(unit_0.x)) * signNotZero_0(unit_0.y);
        unit_0.x = (1.0 - abs(unit_0.y)) * signNotZero_0(unit_0.x);
        unit_0.y = _S60;

#line 131
    }

)cy_msl"
    R"cy_msl(#line 138
    float _S61 = dot(unit_0, unit_0);

#line 138
    float3 _S62;
    if(_S61 > 0.0)
    {

#line 139
        _S62 = unit_0 * float3(rsqrt(_S61)) ;

#line 139
    }
    else
    {

#line 139
        _S62 = float3(1.0, 0.0, 0.0);

#line 139
    }

#line 139
    return _S62;
}


#line 168
float3 facingPerpendicular_0(float3 axis_0, float3 towards_0)
{

#line 168
    float3 _S63;

    if((abs(axis_0.y)) < 0.99000000953674316)
    {

#line 170
        _S63 = float3(0.0, 1.0, 0.0);

#line 170
    }
    else
    {

#line 170
        _S63 = float3(1.0, 0.0, 0.0);

#line 170
    }
    return unitOr_0(cross(axis_0, towards_0), normalize(cross(axis_0, _S63)));
}


#line 152
struct AreaQuad_0
{
    float3 corner0_0;
    float3 corner1_0;
    float3 corner2_0;
    float3 corner3_0;
    bool lit_0;
};


#line 175
AreaQuad_0 areaLightQuad_0(const Light_0 thread* light_3, float3 surfaceRelativeToCamera_1)
{

    uint _S64 = (light_3->kind_0) & 255U;
    float3 _S65 = light_3->positionRelativeToCamera_0 - surfaceRelativeToCamera_1;
    float3 _S66 = unitOr_0(light_3->direction_0, float3(0.0, -1.0, 0.0));


    float extentX_0 = light_3->spotScaleBias_0.x;
    float extentY_0 = light_3->spotScaleBias_0.y;
    bool _S67 = ((light_3->kind_0) & 256U) == 0U;

#line 185
    bool singleSided_0;
    if(_S64 == 4U)
    {

#line 186
        singleSided_0 = true;

#line 186
    }
    else
    {

#line 186
        singleSided_0 = _S64 == 5U;

#line 186
    }

#line 186
    float3 axisX_0;

#line 186
    float3 axisY_0;

#line 186
    float extentX_1;

#line 186
    float extentY_1;

#line 186
    if(singleSided_0)
    {
        float3 _S68 = decodeAreaTangent_0(light_3->shapeWord_0);
        float3 axisX_1 = unitOr_0(_S68 - _S66 * float3(dot(_S68, _S66)) , facingPerpendicular_0(_S66, _S65));
        float3 _S69 = cross(_S66, axisX_1);
        if(_S64 == 5U)
        {

            float extentY_2 = extentY_0 * 0.88622695207595825;

#line 194
            extentX_1 = extentX_0 * 0.88622695207595825;

#line 194
            extentY_1 = extentY_2;

#line 191
        }
        else
        {

#line 191
            extentX_1 = extentX_0;

#line 191
            extentY_1 = extentY_0;

#line 191
        }

#line 191
        axisX_0 = axisX_1;

#line 191
        axisY_0 = _S69;

#line 191
        singleSided_0 = _S67;

#line 186
    }
    else
    {

#line 197
        if(_S64 == 7U)
        {

#line 197
            axisX_0 = facingPerpendicular_0(_S66, _S65);

#line 197
            axisY_0 = _S66;

#line 197
            extentX_1 = extentY_0;

#line 197
            extentY_1 = extentX_0;

#line 197
        }
        else
        {

#line 210
            float extentX_2 = extentX_0 * 0.88622695207595825;

            float3 _S70 = float3(0.0, 0.0, 1.0);

#line 212
            float3 _S71 = unitOr_0(_S65, _S70);
            float3 axisX_2 = unitOr_0(cross(_S70, _S71), float3(1.0, 0.0, 0.0));
            float3 _S72 = cross(_S71, axisX_2);

#line 214
            axisX_0 = axisX_2;

#line 214
            axisY_0 = _S72;

#line 214
            extentX_1 = extentX_2;

#line 214
            extentY_1 = extentX_2;

#line 197
        }

#line 197
        singleSided_0 = false;

#line 186
    }

#line 217
    thread AreaQuad_0 quad_0;
    float3 _S73 = axisX_0 * float3(extentX_1) ;

#line 218
    float3 _S74 = _S65 - _S73;

#line 218
    float3 _S75 = axisY_0 * float3(extentY_1) ;

#line 218
    (&quad_0)->corner0_0 = _S74 - _S75;
    float3 _S76 = _S65 + _S73;

#line 219
    (&quad_0)->corner1_0 = _S76 - _S75;
    (&quad_0)->corner2_0 = _S76 + _S75;
    (&quad_0)->corner3_0 = _S74 + _S75;


    if(!singleSided_0)
    {

#line 224
        singleSided_0 = true;

#line 224
    }
    else
    {

#line 224
        singleSided_0 = (dot(cross(axisX_0, axisY_0), _S65)) <= 0.0;

#line 224
    }

#line 224
    (&quad_0)->lit_0 = singleSided_0;
    return quad_0;
}


float3 edgeVector_0(float3 a_0, float3 b_0)
{

    float _S77 = acos(clamp(dot(a_0, b_0), -1.0, 1.0));
    float3 _S78 = cross(a_0, b_0);
    float _S79 = length(_S78);

#line 234
    float _S80;
    if(_S79 > 9.99999997475242708e-07)
    {

#line 235
        _S80 = _S77 / _S79;

#line 235
    }
    else
    {

#line 235
        _S80 = 1.0;

#line 235
    }
    return _S78 * float3(_S80) ;
}


#line 243
struct PolygonIntegral_0
{
    float integral_0;
    float3 direction_1;
};

PolygonIntegral_0 cosinePolygonVector_0(float3 c0_0, float3 c1_0, float3 c2_0, float3 c3_0)
{
    thread PolygonIntegral_0 result_0;
    (&result_0)->integral_0 = 0.0;
    float3 _S81 = float3(0.0, 0.0, 1.0);

#line 253
    (&result_0)->direction_1 = _S81;
    array<float3, int(4)> _S82 = { unitOr_0(c0_0, _S81), unitOr_0(c1_0, _S81), unitOr_0(c2_0, _S81), unitOr_0(c3_0, _S81) };

    thread array<float3, int(5)> clipped_0;

#line 256
    uint index_3 = 0U;

#line 256
    uint written_0 = 0U;

)cy_msl"
    R"cy_msl(    for(;;)
    {

#line 258
        if(index_3 < 4U)
        {
        }
        else
        {

#line 258
            break;
        }

#line 258
        uint _S83 = index_3;


        uint _S84 = index_3 + 1U;

#line 261
        uint _S85 = _S84 % 4U;
        float _S86 = _S82[index_3].z;

#line 262
        bool _S87 = _S86 > 0.0;
        float _S88 = _S82[_S85].z;

#line 263
        bool _S89 = _S88 > 0.0;

#line 263
        uint written_1;
        if(_S87)
        {
            clipped_0[written_0] = _S82[_S83];

#line 266
            written_1 = written_0 + 1U;

#line 264
        }
        else
        {

#line 264
            written_1 = written_0;

#line 264
        }

#line 269
        if(_S87 != _S89)
        {

            clipped_0[written_1] = _S82[_S83] + (_S82[_S85] - _S82[_S83]) * float3((_S86 / (_S86 - _S88))) ;

#line 272
            written_0 = written_1 + 1U;

#line 269
        }
        else
        {

#line 269
            written_0 = written_1;

#line 269
        }

#line 258
        index_3 = _S84;

#line 258
    }

#line 276
    if(written_0 < 3U)
    {
        return result_0;
    }
    float3 _S90 = float3(0.0) ;

#line 280
    index_3 = 0U;

#line 280
    float3 sum_0 = _S90;
    for(;;)
    {

#line 281
        if(index_3 < written_0)
        {
        }
        else
        {

#line 281
            break;
        }
        float3 _S91 = unitOr_0(clipped_0[index_3], _S81);
        uint _S92 = index_3 + 1U;

#line 284
        uint _S93 = _S92 % written_0;
        float3 sum_1 = sum_0 + edgeVector_0(_S91, unitOr_0(clipped_0[_S93], _S81));

#line 281
        index_3 = _S92;

#line 281
        sum_0 = sum_1;

#line 281
    }

#line 287
    if((sum_0.z) < 0.0)
    {

#line 287
        sum_0 = - sum_0;

#line 287
    }



    (&result_0)->integral_0 = sum_0.z / 6.28318548202514648;
    (&result_0)->direction_1 = unitOr_0(sum_0, _S81);
    return result_0;
}


float cosinePolygonIntegral_0(float3 c0_1, float3 c1_1, float3 c2_1, float3 c3_1)
{
    PolygonIntegral_0 _S94 = cosinePolygonVector_0(c0_1, c1_1, c2_1, c3_1);

#line 299
    return _S94.integral_0;
}


float areaFormFactor_0(const AreaQuad_0 thread* quad_1, float3 normal_2)
{
    if(!quad_1->lit_0)
    {
        return 0.0;
    }

#line 307
    float3 _S95;

    if((abs(normal_2.z)) < 0.99900001287460327)
    {

#line 309
        _S95 = float3(0.0, 0.0, 1.0);

#line 309
    }
    else
    {

#line 309
        _S95 = float3(1.0, 0.0, 0.0);

#line 309
    }
    float3 _S96 = unitOr_0(cross(_S95, normal_2), float3(1.0, 0.0, 0.0));
    float3 _S97 = cross(normal_2, _S96);
    float _S98 = cosinePolygonIntegral_0(float3(dot(quad_1->corner0_0, _S96), dot(quad_1->corner0_0, _S97), dot(quad_1->corner0_0, normal_2)), float3(dot(quad_1->corner1_0, _S96), dot(quad_1->corner1_0, _S97), dot(quad_1->corner1_0, normal_2)), float3(dot(quad_1->corner2_0, _S96), dot(quad_1->corner2_0, _S97), dot(quad_1->corner2_0, normal_2)), float3(dot(quad_1->corner3_0, _S96), dot(quad_1->corner3_0, _S97), dot(quad_1->corner3_0, normal_2)));

#line 312
    return _S98;
}


#line 61
float distanceAttenuation_0(float distanceSquared_0, float range_1)
{
    float _S99 = distanceSquared_0 / max(range_1 * range_1, 9.99999997475242708e-07);
    float _S100 = saturate(1.0 - _S99 * _S99);
    return _S100 * _S100 / max(distanceSquared_0, 0.00009999999747379);
}


#line 52
struct LightSample_0
{
    float3 direction_2;
    float3 illuminance_0;
    float attenuation_0;
};


#line 68
LightSample_0 evaluateLight_0(const Light_0 thread* light_4, float3 surfaceRelativeToCamera_2)
{
    thread LightSample_0 result_1;

#line 70
    uint _S101 = light_4->kind_0;
    if((light_4->kind_0) == 0U)
    {
        (&result_1)->direction_2 = - light_4->direction_0;
        (&result_1)->attenuation_0 = 1.0;
        (&result_1)->illuminance_0 = light_4->color_0 * float3(light_4->intensity_0) ;
        return result_1;
    }

    float3 _S102 = light_4->positionRelativeToCamera_0 - surfaceRelativeToCamera_2;
    float _S103 = dot(_S102, _S102);
    (&result_1)->direction_2 = _S102 * float3(rsqrt(max(_S103, 9.99999993922529029e-09))) ;
    (&result_1)->attenuation_0 = distanceAttenuation_0(_S103, light_4->range_0);

    if(_S101 == 2U)
    {

        float _S104 = saturate(dot(- (&result_1)->direction_2, light_4->direction_0) * light_4->spotScaleBias_0.x + light_4->spotScaleBias_0.y);
        (&result_1)->attenuation_0 = (&result_1)->attenuation_0 * (_S104 * _S104);

#line 84
    }
    else
    {



        if(_S101 == 3U)
        {

            (&result_1)->attenuation_0 = (&result_1)->attenuation_0 * saturate(dot(- (&result_1)->direction_2, light_4->direction_0) * light_4->spotScaleBias_0.x + light_4->spotScaleBias_0.y);

#line 90
        }

#line 84
    }

#line 95
    (&result_1)->illuminance_0 = light_4->color_0 * float3((light_4->intensity_0 * (&result_1)->attenuation_0)) ;
    return result_1;
}


#line 412
float3 lightIrradiance_0(const Light_0 thread* light_5, float3 surfaceRelativeToCamera_3, float3 normal_3)
{

#line 412
    uint _S105 = light_5->kind_0;

    uint _S106 = (light_5->kind_0) & 255U;

#line 414
    bool _S107;
    if(_S106 == 0U)
    {

#line 415
        _S107 = true;

#line 415
    }
    else
    {

#line 415
        _S107 = _S106 == 8U;

#line 415
    }

#line 415
    if(_S107)
    {
        return float3(0.0) ;
    }
    if(isAreaLight_0(_S105))
    {

#line 419
        float _S108 = areaLightWindow_0(light_5, surfaceRelativeToCamera_3);

#line 419
        AreaQuad_0 _S109 = areaLightQuad_0(light_5, surfaceRelativeToCamera_3);



        float _S110 = light_5->intensity_0 * 3.14159274101257324;

#line 423
        thread AreaQuad_0 _S111 = _S109;

#line 423
        float _S112 = areaFormFactor_0(&_S111, normal_3);

#line 423
        return light_5->color_0 * float3((_S110 * _S112 * _S108)) ;
    }

)cy_msl"
    R"cy_msl(#line 423
    LightSample_0 _S113 = evaluateLight_0(light_5, surfaceRelativeToCamera_3);



    return _S113.illuminance_0 * float3(saturate(dot(normal_3, _S113.direction_2))) ;
}


#line 176 "src/rendering/lights_gpu/shaders/many_lights.slang"
float luminance_0(float3 value_3)
{
    return 0.2125999927520752 * value_3.x + 0.71520000696182251 * value_3.y + 0.07220000028610229 * value_3.z;
}



float targetOf_0(uint light_6, const CySurface_0 thread* surface_3, KernelContext_0 thread* kernelContext_7)
{

#line 183
    uint _S114 = wordBits_0(15U, 3U, kernelContext_7);

    if(light_6 >= _S114)
    {
        return 0.0;
    }

#line 187
    thread Light_0 _S115 = kernelContext_7->cyManyLights_0->lights_0[light_6];

#line 187
    float3 _S116 = lightIrradiance_0(&_S115, surface_3->position_0, surface_3->normal_1);

    return luminance_0(_S116);
}


#line 246
void reservoirUpdate_0(Reservoir_0 thread* reservoir_2, float light_7, float target_2, float sourcePdf_0, Stream_0 thread* stream_3)
{

    reservoir_2->count_0 = reservoir_2->count_0 + 1.0;

#line 249
    bool _S117;
    if(!(target_2 > 0.0))
    {

#line 250
        _S117 = true;

#line 250
    }
    else
    {

#line 250
        _S117 = !(sourcePdf_0 > 0.0);

#line 250
    }

#line 250
    if(_S117)
    {
        return;
    }
    float _S118 = target_2 / sourcePdf_0;
    reservoir_2->weightSum_0 = reservoir_2->weightSum_0 + _S118;
    float _S119 = nextUnit_0(stream_3);

#line 256
    if((_S119 * reservoir_2->weightSum_0) <= _S118)
    {
        reservoir_2->light_0 = light_7;
        reservoir_2->target_1 = target_2;

#line 256
    }

#line 261
    return;
}


#line 215
Reservoir_0 toReservoir_0(float4 packed_0)
{
    thread Reservoir_0 reservoir_3;
    (&reservoir_3)->light_0 = packed_0.x;
    (&reservoir_3)->weightSum_0 = packed_0.y;
    (&reservoir_3)->count_0 = packed_0.z;
    (&reservoir_3)->target_1 = packed_0.w;
    return reservoir_3;
}


#line 230
bool reservoirValid_0(const Reservoir_0 thread* reservoir_4)
{

#line 230
    bool _S120;

    if((reservoir_4->light_0) >= 0.0)
    {

#line 232
        _S120 = (reservoir_4->target_1) > 0.0;

#line 232
    }
    else
    {

#line 232
        _S120 = false;

#line 232
    }

#line 232
    return _S120;
}


float contributionWeight_0(const Reservoir_0 thread* reservoir_5)
{

#line 236
    bool _S121 = reservoirValid_0(reservoir_5);

#line 236
    bool _S122;

    if(!_S121)
    {

#line 238
        _S122 = true;

#line 238
    }
    else
    {

#line 238
        _S122 = (reservoir_5->count_0) <= 0.0;

#line 238
    }

#line 238
    if(_S122)
    {
        return 0.0;
    }
    return reservoir_5->weightSum_0 / (reservoir_5->count_0 * reservoir_5->target_1);
}


#line 265
void capReservoir_0(Reservoir_0 thread* reservoir_6, float cap_0)
{

#line 265
    bool _S123;

    if(cap_0 > 0.0)
    {

#line 267
        _S123 = (reservoir_6->count_0) > cap_0;

#line 267
    }
    else
    {

#line 267
        _S123 = false;

#line 267
    }

#line 267
    if(_S123)
    {
        reservoir_6->weightSum_0 = reservoir_6->weightSum_0 * (cap_0 / reservoir_6->count_0);
        reservoir_6->count_0 = cap_0;

#line 267
    }

#line 272
    return;
}

void reservoirCombine_0(Reservoir_0 thread* reservoir_7, const Reservoir_0 thread* other_0, float otherTargetHere_0, float cap_1, Stream_0 thread* stream_4)
{

#line 276
    bool _S124 = reservoirValid_0(other_0);

#line 276
    bool _S125;

    if(!_S124)
    {

#line 278
        _S125 = true;

#line 278
    }
    else
    {

#line 278
        _S125 = (other_0->count_0) <= 0.0;

#line 278
    }

#line 278
    if(_S125)
    {
        return;
    }

#line 280
    float _S126 = contributionWeight_0(other_0);

#line 280
    float _S127 = other_0->count_0;

    float _S128 = _S126 * otherTargetHere_0 * other_0->count_0;
    if(_S128 > 0.0)
    {
        reservoir_7->weightSum_0 = reservoir_7->weightSum_0 + _S128;
        float _S129 = nextUnit_0(stream_4);

#line 286
        if((_S129 * reservoir_7->weightSum_0) <= _S128)
        {
            reservoir_7->light_0 = other_0->light_0;
            reservoir_7->target_1 = otherTargetHere_0;

#line 286
        }

#line 283
    }

#line 292
    reservoir_7->count_0 = reservoir_7->count_0 + _S127;
    capReservoir_0(reservoir_7, cap_1);
    return;
}


#line 348
[[kernel]] void cyManyLightsCandidates(uint3 id_0 [[thread_position_in_grid]], CyManyLightSet_default_0 constant* cyManyLights_1 [[buffer(0)]])
{

#line 348
    thread KernelContext_0 kernelContext_8;

#line 348
    (&kernelContext_8)->cyManyLights_0 = cyManyLights_1;

#line 348
    float4 _S130 = word_0(8U, &kernelContext_8);

    uint2 _S131 = uint2(_S130.xy);
    uint _S132 = _S131.x;

#line 351
    bool _S133;

#line 351
    if((id_0.x) >= _S132)
    {

#line 351
        _S133 = true;

#line 351
    }
    else
    {

#line 351
        _S133 = (id_0.y) >= (_S131.y);

#line 351
    }

#line 351
    if(_S133)
    {
        return;
    }
    uint2 _S134 = id_0.xy;

)cy_msl"
    R"cy_msl(#line 355
    CySurface_0 _S135 = surfaceAt_0(_S134, &kernelContext_8);

#line 355
    float4 _S136;

    if(_S135.valid_0)
    {

#line 357
        _S136 = float4(_S135.viewDepth_0, _S135.normal_1);

#line 357
    }
    else
    {

#line 357
        _S136 = float4(0.0) ;

#line 357
    }

#line 357
    (&kernelContext_8)->cyManyLights_0->surface_0.write(_S136,_S134);

    thread float confidence_1 = 0.0;

#line 359
    thread CySurface_0 _S137 = _S135;

#line 359
    int _S138 = historySource_0(&_S137, _S131, &confidence_1, &kernelContext_8);

    (&kernelContext_8)->cyManyLights_0->guidance_0.write(float4(float(_S138), confidence_1, int(0), int(0)),_S134);

    thread Reservoir_0 reservoir_8 = emptyReservoir_0();

#line 363
    uint _S139 = wordBits_0(9U, 2U, &kernelContext_8);

    if(!_S135.valid_0)
    {

#line 365
        _S133 = true;

#line 365
    }
    else
    {

#line 365
        _S133 = _S139 == 0U;

#line 365
    }

#line 365
    if(_S133)
    {

#line 365
        thread Reservoir_0 _S140 = reservoir_8;

#line 365
        float4 _S141 = packReservoir_0(&_S140);

        (&kernelContext_8)->cyManyLights_0->temporalReservoir_0.write(_S141,_S134);
        return;
    }

#line 368
    uint _S142 = wordBits_0(9U, 0U, &kernelContext_8);

    thread Stream_0 stream_5 = streamFor_0(_S134, _S142, 1U);

#line 370
    uint _S143 = wordBits_0(9U, 1U, &kernelContext_8);
    uint _S144 = max(_S143, 1U);

#line 371
    uint draw_0 = 0U;
    for(;;)
    {

#line 372
        if(draw_0 < _S144)
        {
        }
        else
        {

#line 372
            break;
        }
        thread uint light_8 = 0U;
        thread float pdf_1 = 0.0;

#line 375
        drawCandidate_0(&stream_5, _S139, &light_8, &pdf_1, &kernelContext_8);

        float _S145 = float(light_8);

#line 377
        thread CySurface_0 _S146 = _S135;

#line 377
        float _S147 = targetOf_0(light_8, &_S146, &kernelContext_8);

#line 377
        reservoirUpdate_0(&reservoir_8, _S145, _S147, pdf_1, &stream_5);

#line 372
        draw_0 = draw_0 + 1U;

#line 372
    }

#line 372
    uint _S148 = wordBits_0(15U, 0U, &kernelContext_8);

#line 379
    if(_S148 != 0U)
    {

#line 379
        _S133 = _S138 >= int(0);

#line 379
    }
    else
    {

#line 379
        _S133 = false;

#line 379
    }

#line 379
    if(_S133)
    {
        uint _S149 = uint(_S138);

#line 381
        uint _S150 = _S149 % _S132;

#line 381
        uint _S151 = _S149 / _S132;
        float4 _S152 = (((&kernelContext_8)->cyManyLights_0->historyReservoir_0).read(vec<uint,2>(((int2(uint2(_S150, _S151)))).xy)));

#line 382
        Reservoir_0 _S153 = toReservoir_0(_S152);

#line 382
        thread Reservoir_0 _S154 = _S153;

#line 382
        bool _S155 = reservoirValid_0(&_S154);
        if(_S155)
        {
            uint _S156 = uint(_S153.light_0);

#line 385
            thread CySurface_0 _S157 = _S135;

#line 385
            float _S158 = targetOf_0(_S156, &_S157, &kernelContext_8);

#line 385
            float4 _S159 = word_0(10U, &kernelContext_8);
            float _S160 = _S159.x;

#line 386
            thread Reservoir_0 _S161 = _S153;

#line 386
            reservoirCombine_0(&reservoir_8, &_S161, _S158, _S160, &stream_5);

#line 383
        }

#line 379
    }

#line 379
    thread Reservoir_0 _S162 = reservoir_8;

#line 379
    float4 _S163 = packReservoir_0(&_S162);

#line 389
    (&kernelContext_8)->cyManyLights_0->temporalReservoir_0.write(_S163,_S134);
    return;
}

)cy_msl";

/// cyManyLightsSpatial.metal, 36870 bytes.
inline constexpr char kManyLightsSpatialMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    float3 positionRelativeToCamera_0;
    float range_0;
    float3 direction_0;
    float intensity_0;
    float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 31 "src/rendering/lights_gpu/shaders/many_lights.slang"
struct CyManyLightSet_default_0
{
    uint4 device* words_0;
    Light_0 device* lights_0;
    uint4 device* candidates_0;
    depth2d<float, access::sample> depth_0;
    texture2d<float, access::sample> normals_0;
    texture2d<float, access::read_write> temporalReservoir_0;
    texture2d<float, access::read_write> historyReservoir_0;
    texture2d<float, access::read_write> finalReservoir_0;
    texture2d<float, access::read_write> historySurface_0;
    texture2d<float, access::read_write> surface_0;
    texture2d<float, access::read_write> guidance_0;
    texture2d<float, access::read_write> noisy_0;
    texture2d<float, access::read_write> resolved_0;
    texture2d<float, access::read_write> historyColour_0;
    texture2d<float, access::read_write> historyMoments_0;
    texture2d<float, access::read_write> accumulated_0;
    texture2d<float, access::read_write> moments_0;
    texture2d<float, access::read_write> variance_0;
    texture2d<float, access::read_write> filterSource_0;
    texture2d<float, access::read_write> filterOut_0;
    texture2d<float, access::read_write> target_0;
};


#line 31
struct KernelContext_0
{
    CyManyLightSet_default_0 constant* cyManyLights_0;
};


#line 95
float4 word_0(uint index_0, KernelContext_0 thread* kernelContext_0)
{
    return (as_type<float4>((kernelContext_0->cyManyLights_0->words_0[index_0])));
}


#line 95
float4 word_1(uint index_1, KernelContext_0 thread* kernelContext_1)
{
    return (as_type<float4>((kernelContext_1->cyManyLights_0->words_0[index_1])));
}


#line 154
float3 relativeFromClip_0(float2 ndc_0, float depth_1, KernelContext_0 thread* kernelContext_2)
{
    float4 _S1 = float4(ndc_0, depth_1, 1.0);

#line 156
    float4 _S2 = word_1(0U, kernelContext_2);
    float4 _S3 = _S2 * float4(_S1.x) ;

#line 157
    float4 _S4 = word_1(1U, kernelContext_2);

#line 157
    float4 _S5 = _S3 + _S4 * float4(_S1.y) ;

#line 157
    float4 _S6 = word_1(2U, kernelContext_2);

#line 157
    float4 _S7 = _S5 + _S6 * float4(_S1.z) ;

#line 157
    float4 _S8 = word_1(3U, kernelContext_2);

#line 157
    float4 _S9 = _S7 + _S8 * float4(_S1.w) ;
    return _S9.xyz / float3(_S9.w) ;
}


#line 23 "src/rendering/shaders/cy/packing.slang"
float3 decodeOctahedral_0(float2 encoded_0)
{
    float2 _S10 = encoded_0 * float2(2.0)  - float2(1.0) ;
    float _S11 = _S10.x;

#line 26
    float _S12 = _S10.y;

#line 26
    float _S13 = 1.0 - abs(_S11) - abs(_S12);

#line 26
    thread float3 normal_0 = float3(_S11, _S12, _S13);
    float _S14 = saturate(- _S13);
    float2 _S15 = float2(_S11, _S12);

#line 28
    normal_0.xy = _S15 + select(float2(_S14) , float2(- _S14) , _S15 >= (float2(0.0) ));
    return normalize(normal_0);
}


#line 146 "src/rendering/lights_gpu/shaders/many_lights.slang"
struct CySurface_0
{
    bool valid_0;
    float3 position_0;
    float3 normal_1;
    float viewDepth_0;
};


#line 161
CySurface_0 surfaceAt_0(uint2 pixel_0, KernelContext_0 thread* kernelContext_3)
{

    int3 _S16 = int3(int2(pixel_0), int(0));

#line 164
    float _S17 = ((kernelContext_3->cyManyLights_0->depth_0).read(vec<uint,2>(((_S16)).xy), uint(((_S16)).z)));

#line 163
    thread CySurface_0 surface_1;

    bool _S18 = _S17 > 0.0;

#line 165
    (&surface_1)->valid_0 = _S18;

#line 165
    float4 _S19 = word_0(8U, kernelContext_3);

    float2 _S20 = (float2(pixel_0) + float2(0.5) ) * _S19.zw;
    float2 _S21 = float2(_S20.x * 2.0 - 1.0, 1.0 - _S20.y * 2.0);

#line 168
    float3 _S22;
    if(_S18)
    {

#line 169
        float3 _S23 = relativeFromClip_0(_S21, _S17, kernelContext_3);

#line 169
        _S22 = _S23;

#line 169
    }
    else
    {

#line 169
        _S22 = float3(0.0) ;

#line 169
    }

#line 169
    (&surface_1)->position_0 = _S22;

    (&surface_1)->normal_1 = decodeOctahedral_0(((kernelContext_3->cyManyLights_0->normals_0).read(vec<uint,2>(((_S16)).xy), uint(((_S16)).z))).xy);

#line 171
    float _S24;
    if((&surface_1)->valid_0)
    {

#line 172
        float4 _S25 = word_0(11U, kernelContext_3);

#line 172
        _S24 = - dot(_S25.xyz, (&surface_1)->position_0);

#line 172
    }
    else
    {

#line 172
        _S24 = 0.0;

#line 172
    }

#line 172
    (&surface_1)->viewDepth_0 = _S24;
    return surface_1;
}


#line 197
struct Reservoir_0
{
    float light_0;
    float weightSum_0;
    float count_0;
    float target_1;
};


#line 215
Reservoir_0 toReservoir_0(float4 packed_0)
{
    thread Reservoir_0 reservoir_0;
    (&reservoir_0)->light_0 = packed_0.x;
    (&reservoir_0)->weightSum_0 = packed_0.y;
    (&reservoir_0)->count_0 = packed_0.z;
    (&reservoir_0)->target_1 = packed_0.w;
    return reservoir_0;
}


#line 205
Reservoir_0 emptyReservoir_0()
{
    thread Reservoir_0 reservoir_1;
    (&reservoir_1)->light_0 = -1.0;
    (&reservoir_1)->weightSum_0 = 0.0;
    (&reservoir_1)->count_0 = 0.0;
    (&reservoir_1)->target_1 = 0.0;
    return reservoir_1;
}


#line 225
float4 packReservoir_0(const Reservoir_0 thread* reservoir_2)
{
    return float4(reservoir_2->light_0, reservoir_2->weightSum_0, reservoir_2->count_0, reservoir_2->target_1);
}


#line 100
uint wordBits_0(uint index_2, uint lane_0, KernelContext_0 thread* kernelContext_4)
{
    return kernelContext_4->cyManyLights_0->words_0[index_2][lane_0];
}


#line 120
uint lowbias32_0(uint value_0)
{


    uint x_0 = (value_0 ^ (value_0 >> 16U)) * 2146121005U;

    uint x_1 = (x_0 ^ (x_0 >> 15U)) * 2221713035U;

    return x_1 ^ (x_1 >> 16U);
}


#line 115
struct Stream_0
{
    uint state_0;
};


#line 131
Stream_0 streamFor_0(uint2 pixel_1, uint frame_0, uint salt_0)
{
    thread Stream_0 stream_0;
    (&stream_0)->state_0 = lowbias32_0(pixel_1.x + lowbias32_0(pixel_1.y + lowbias32_0(frame_0 + lowbias32_0(salt_0))));
    return stream_0;
}

float nextUnit_0(Stream_0 thread* stream_1)
{
    uint _S26 = stream_1->state_0 + 2654435769U;

#line 140
    stream_1->state_0 = _S26;
    return float((lowbias32_0(_S26)) >> 8U) * 5.9604644775390625e-08;
}


#line 230
bool reservoirValid_0(const Reservoir_0 thread* reservoir_3)
{

#line 230
    bool _S27;

)cy_msl"
    R"cy_msl(    if((reservoir_3->light_0) >= 0.0)
    {

#line 232
        _S27 = (reservoir_3->target_1) > 0.0;

#line 232
    }
    else
    {

#line 232
        _S27 = false;

#line 232
    }

#line 232
    return _S27;
}


#line 114 "src/rendering/shaders/cy/light.slang"
bool isAreaLight_0(uint kind_1)
{
    uint _S28 = kind_1 & 255U;

#line 116
    bool _S29;
    if(_S28 >= 4U)
    {

#line 117
        _S29 = _S28 <= 7U;

#line 117
    }
    else
    {

#line 117
        _S29 = false;

#line 117
    }

#line 117
    return _S29;
}


#line 362
float areaLightWindow_0(const Light_0 thread* light_1, float3 surfaceRelativeToCamera_0)
{
    float3 _S30 = light_1->positionRelativeToCamera_0 - surfaceRelativeToCamera_0;
    float _S31 = dot(_S30, _S30) / max(light_1->range_0 * light_1->range_0, 9.99999997475242708e-07);
    float _S32 = saturate(1.0 - _S31 * _S31);
    return _S32 * _S32;
}


#line 161
float3 unitOr_0(float3 value_1, float3 fallback_0)
{
    float _S33 = dot(value_1, value_1);

#line 163
    float3 _S34;
    if(_S33 > 9.99999968265522539e-21)
    {

#line 164
        _S34 = value_1 * float3(rsqrt(_S33)) ;

#line 164
    }
    else
    {

#line 164
        _S34 = fallback_0;

#line 164
    }

#line 164
    return _S34;
}


#line 120
float signNotZero_0(float value_2)
{

#line 120
    float _S35;

    if(value_2 >= 0.0)
    {

#line 122
        _S35 = 1.0;

#line 122
    }
    else
    {

#line 122
        _S35 = -1.0;

#line 122
    }

#line 122
    return _S35;
}


float3 decodeAreaTangent_0(uint word_2)
{
    float _S36 = max(float(int(word_2 << 16U) >> 16U) / 32767.0, -1.0);
    float _S37 = max(float(int(word_2) >> 16U) / 32767.0, -1.0);
    float _S38 = 1.0 - abs(_S36) - abs(_S37);

#line 130
    thread float3 unit_0 = float3(_S36, _S37, _S38);
    if(_S38 < 0.0)
    {

        float _S39 = (1.0 - abs(unit_0.x)) * signNotZero_0(unit_0.y);
        unit_0.x = (1.0 - abs(unit_0.y)) * signNotZero_0(unit_0.x);
        unit_0.y = _S39;

#line 131
    }

#line 138
    float _S40 = dot(unit_0, unit_0);

#line 138
    float3 _S41;
    if(_S40 > 0.0)
    {

#line 139
        _S41 = unit_0 * float3(rsqrt(_S40)) ;

#line 139
    }
    else
    {

#line 139
        _S41 = float3(1.0, 0.0, 0.0);

#line 139
    }

#line 139
    return _S41;
}


#line 168
float3 facingPerpendicular_0(float3 axis_0, float3 towards_0)
{

#line 168
    float3 _S42;

    if((abs(axis_0.y)) < 0.99000000953674316)
    {

#line 170
        _S42 = float3(0.0, 1.0, 0.0);

#line 170
    }
    else
    {

#line 170
        _S42 = float3(1.0, 0.0, 0.0);

#line 170
    }
    return unitOr_0(cross(axis_0, towards_0), normalize(cross(axis_0, _S42)));
}


#line 152
struct AreaQuad_0
{
    float3 corner0_0;
    float3 corner1_0;
    float3 corner2_0;
    float3 corner3_0;
    bool lit_0;
};


#line 175
AreaQuad_0 areaLightQuad_0(const Light_0 thread* light_2, float3 surfaceRelativeToCamera_1)
{

    uint _S43 = (light_2->kind_0) & 255U;
    float3 _S44 = light_2->positionRelativeToCamera_0 - surfaceRelativeToCamera_1;
    float3 _S45 = unitOr_0(light_2->direction_0, float3(0.0, -1.0, 0.0));


    float extentX_0 = light_2->spotScaleBias_0.x;
    float extentY_0 = light_2->spotScaleBias_0.y;
    bool _S46 = ((light_2->kind_0) & 256U) == 0U;

#line 185
    bool singleSided_0;
    if(_S43 == 4U)
    {

#line 186
        singleSided_0 = true;

#line 186
    }
    else
    {

#line 186
        singleSided_0 = _S43 == 5U;

#line 186
    }

#line 186
    float3 axisX_0;

#line 186
    float3 axisY_0;

#line 186
    float extentX_1;

#line 186
    float extentY_1;

#line 186
    if(singleSided_0)
    {
        float3 _S47 = decodeAreaTangent_0(light_2->shapeWord_0);
        float3 axisX_1 = unitOr_0(_S47 - _S45 * float3(dot(_S47, _S45)) , facingPerpendicular_0(_S45, _S44));
        float3 _S48 = cross(_S45, axisX_1);
        if(_S43 == 5U)
        {

            float extentY_2 = extentY_0 * 0.88622695207595825;

#line 194
            extentX_1 = extentX_0 * 0.88622695207595825;

#line 194
            extentY_1 = extentY_2;

#line 191
        }
        else
        {

#line 191
            extentX_1 = extentX_0;

#line 191
            extentY_1 = extentY_0;

#line 191
        }

#line 191
        axisX_0 = axisX_1;

#line 191
        axisY_0 = _S48;

#line 191
        singleSided_0 = _S46;

#line 186
    }
    else
    {

#line 197
        if(_S43 == 7U)
        {

#line 197
            axisX_0 = facingPerpendicular_0(_S45, _S44);

#line 197
            axisY_0 = _S45;

)cy_msl"
    R"cy_msl(#line 197
            extentX_1 = extentY_0;

#line 197
            extentY_1 = extentX_0;

#line 197
        }
        else
        {

#line 210
            float extentX_2 = extentX_0 * 0.88622695207595825;

            float3 _S49 = float3(0.0, 0.0, 1.0);

#line 212
            float3 _S50 = unitOr_0(_S44, _S49);
            float3 axisX_2 = unitOr_0(cross(_S49, _S50), float3(1.0, 0.0, 0.0));
            float3 _S51 = cross(_S50, axisX_2);

#line 214
            axisX_0 = axisX_2;

#line 214
            axisY_0 = _S51;

#line 214
            extentX_1 = extentX_2;

#line 214
            extentY_1 = extentX_2;

#line 197
        }

#line 197
        singleSided_0 = false;

#line 186
    }

#line 217
    thread AreaQuad_0 quad_0;
    float3 _S52 = axisX_0 * float3(extentX_1) ;

#line 218
    float3 _S53 = _S44 - _S52;

#line 218
    float3 _S54 = axisY_0 * float3(extentY_1) ;

#line 218
    (&quad_0)->corner0_0 = _S53 - _S54;
    float3 _S55 = _S44 + _S52;

#line 219
    (&quad_0)->corner1_0 = _S55 - _S54;
    (&quad_0)->corner2_0 = _S55 + _S54;
    (&quad_0)->corner3_0 = _S53 + _S54;


    if(!singleSided_0)
    {

#line 224
        singleSided_0 = true;

#line 224
    }
    else
    {

#line 224
        singleSided_0 = (dot(cross(axisX_0, axisY_0), _S44)) <= 0.0;

#line 224
    }

#line 224
    (&quad_0)->lit_0 = singleSided_0;
    return quad_0;
}


float3 edgeVector_0(float3 a_0, float3 b_0)
{

    float _S56 = acos(clamp(dot(a_0, b_0), -1.0, 1.0));
    float3 _S57 = cross(a_0, b_0);
    float _S58 = length(_S57);

#line 234
    float _S59;
    if(_S58 > 9.99999997475242708e-07)
    {

#line 235
        _S59 = _S56 / _S58;

#line 235
    }
    else
    {

#line 235
        _S59 = 1.0;

#line 235
    }
    return _S57 * float3(_S59) ;
}


#line 243
struct PolygonIntegral_0
{
    float integral_0;
    float3 direction_1;
};

PolygonIntegral_0 cosinePolygonVector_0(float3 c0_0, float3 c1_0, float3 c2_0, float3 c3_0)
{
    thread PolygonIntegral_0 result_0;
    (&result_0)->integral_0 = 0.0;
    float3 _S60 = float3(0.0, 0.0, 1.0);

#line 253
    (&result_0)->direction_1 = _S60;
    array<float3, int(4)> _S61 = { unitOr_0(c0_0, _S60), unitOr_0(c1_0, _S60), unitOr_0(c2_0, _S60), unitOr_0(c3_0, _S60) };

    thread array<float3, int(5)> clipped_0;

#line 256
    uint index_3 = 0U;

#line 256
    uint written_0 = 0U;

    for(;;)
    {

#line 258
        if(index_3 < 4U)
        {
        }
        else
        {

#line 258
            break;
        }

#line 258
        uint _S62 = index_3;


        uint _S63 = index_3 + 1U;

#line 261
        uint _S64 = _S63 % 4U;
        float _S65 = _S61[index_3].z;

#line 262
        bool _S66 = _S65 > 0.0;
        float _S67 = _S61[_S64].z;

#line 263
        bool _S68 = _S67 > 0.0;

#line 263
        uint written_1;
        if(_S66)
        {
            clipped_0[written_0] = _S61[_S62];

#line 266
            written_1 = written_0 + 1U;

#line 264
        }
        else
        {

#line 264
            written_1 = written_0;

#line 264
        }

#line 269
        if(_S66 != _S68)
        {

            clipped_0[written_1] = _S61[_S62] + (_S61[_S64] - _S61[_S62]) * float3((_S65 / (_S65 - _S67))) ;

#line 272
            written_0 = written_1 + 1U;

#line 269
        }
        else
        {

#line 269
            written_0 = written_1;

#line 269
        }

#line 258
        index_3 = _S63;

#line 258
    }

#line 276
    if(written_0 < 3U)
    {
        return result_0;
    }
    float3 _S69 = float3(0.0) ;

#line 280
    index_3 = 0U;

#line 280
    float3 sum_0 = _S69;
    for(;;)
    {

#line 281
        if(index_3 < written_0)
        {
        }
        else
        {

#line 281
            break;
        }
        float3 _S70 = unitOr_0(clipped_0[index_3], _S60);
        uint _S71 = index_3 + 1U;

#line 284
        uint _S72 = _S71 % written_0;
        float3 sum_1 = sum_0 + edgeVector_0(_S70, unitOr_0(clipped_0[_S72], _S60));

#line 281
        index_3 = _S71;

#line 281
        sum_0 = sum_1;

#line 281
    }

#line 287
    if((sum_0.z) < 0.0)
    {

#line 287
        sum_0 = - sum_0;

#line 287
    }



    (&result_0)->integral_0 = sum_0.z / 6.28318548202514648;
    (&result_0)->direction_1 = unitOr_0(sum_0, _S60);
    return result_0;
}


float cosinePolygonIntegral_0(float3 c0_1, float3 c1_1, float3 c2_1, float3 c3_1)
{
    PolygonIntegral_0 _S73 = cosinePolygonVector_0(c0_1, c1_1, c2_1, c3_1);

#line 299
    return _S73.integral_0;
}


float areaFormFactor_0(const AreaQuad_0 thread* quad_1, float3 normal_2)
{
    if(!quad_1->lit_0)
    {
        return 0.0;
    }

#line 307
    float3 _S74;

    if((abs(normal_2.z)) < 0.99900001287460327)
    {

#line 309
        _S74 = float3(0.0, 0.0, 1.0);

#line 309
    }
    else
    {

)cy_msl"
    R"cy_msl(#line 309
        _S74 = float3(1.0, 0.0, 0.0);

#line 309
    }
    float3 _S75 = unitOr_0(cross(_S74, normal_2), float3(1.0, 0.0, 0.0));
    float3 _S76 = cross(normal_2, _S75);
    float _S77 = cosinePolygonIntegral_0(float3(dot(quad_1->corner0_0, _S75), dot(quad_1->corner0_0, _S76), dot(quad_1->corner0_0, normal_2)), float3(dot(quad_1->corner1_0, _S75), dot(quad_1->corner1_0, _S76), dot(quad_1->corner1_0, normal_2)), float3(dot(quad_1->corner2_0, _S75), dot(quad_1->corner2_0, _S76), dot(quad_1->corner2_0, normal_2)), float3(dot(quad_1->corner3_0, _S75), dot(quad_1->corner3_0, _S76), dot(quad_1->corner3_0, normal_2)));

#line 312
    return _S77;
}


#line 61
float distanceAttenuation_0(float distanceSquared_0, float range_1)
{
    float _S78 = distanceSquared_0 / max(range_1 * range_1, 9.99999997475242708e-07);
    float _S79 = saturate(1.0 - _S78 * _S78);
    return _S79 * _S79 / max(distanceSquared_0, 0.00009999999747379);
}


#line 52
struct LightSample_0
{
    float3 direction_2;
    float3 illuminance_0;
    float attenuation_0;
};


#line 68
LightSample_0 evaluateLight_0(const Light_0 thread* light_3, float3 surfaceRelativeToCamera_2)
{
    thread LightSample_0 result_1;

#line 70
    uint _S80 = light_3->kind_0;
    if((light_3->kind_0) == 0U)
    {
        (&result_1)->direction_2 = - light_3->direction_0;
        (&result_1)->attenuation_0 = 1.0;
        (&result_1)->illuminance_0 = light_3->color_0 * float3(light_3->intensity_0) ;
        return result_1;
    }

    float3 _S81 = light_3->positionRelativeToCamera_0 - surfaceRelativeToCamera_2;
    float _S82 = dot(_S81, _S81);
    (&result_1)->direction_2 = _S81 * float3(rsqrt(max(_S82, 9.99999993922529029e-09))) ;
    (&result_1)->attenuation_0 = distanceAttenuation_0(_S82, light_3->range_0);

    if(_S80 == 2U)
    {

        float _S83 = saturate(dot(- (&result_1)->direction_2, light_3->direction_0) * light_3->spotScaleBias_0.x + light_3->spotScaleBias_0.y);
        (&result_1)->attenuation_0 = (&result_1)->attenuation_0 * (_S83 * _S83);

#line 84
    }
    else
    {



        if(_S80 == 3U)
        {

            (&result_1)->attenuation_0 = (&result_1)->attenuation_0 * saturate(dot(- (&result_1)->direction_2, light_3->direction_0) * light_3->spotScaleBias_0.x + light_3->spotScaleBias_0.y);

#line 90
        }

#line 84
    }

#line 95
    (&result_1)->illuminance_0 = light_3->color_0 * float3((light_3->intensity_0 * (&result_1)->attenuation_0)) ;
    return result_1;
}


#line 412
float3 lightIrradiance_0(const Light_0 thread* light_4, float3 surfaceRelativeToCamera_3, float3 normal_3)
{

#line 412
    uint _S84 = light_4->kind_0;

    uint _S85 = (light_4->kind_0) & 255U;

#line 414
    bool _S86;
    if(_S85 == 0U)
    {

#line 415
        _S86 = true;

#line 415
    }
    else
    {

#line 415
        _S86 = _S85 == 8U;

#line 415
    }

#line 415
    if(_S86)
    {
        return float3(0.0) ;
    }
    if(isAreaLight_0(_S84))
    {

#line 419
        float _S87 = areaLightWindow_0(light_4, surfaceRelativeToCamera_3);

#line 419
        AreaQuad_0 _S88 = areaLightQuad_0(light_4, surfaceRelativeToCamera_3);



        float _S89 = light_4->intensity_0 * 3.14159274101257324;

#line 423
        thread AreaQuad_0 _S90 = _S88;

#line 423
        float _S91 = areaFormFactor_0(&_S90, normal_3);

#line 423
        return light_4->color_0 * float3((_S89 * _S91 * _S87)) ;
    }

#line 423
    LightSample_0 _S92 = evaluateLight_0(light_4, surfaceRelativeToCamera_3);



    return _S92.illuminance_0 * float3(saturate(dot(normal_3, _S92.direction_2))) ;
}


#line 176 "src/rendering/lights_gpu/shaders/many_lights.slang"
float luminance_0(float3 value_3)
{
    return 0.2125999927520752 * value_3.x + 0.71520000696182251 * value_3.y + 0.07220000028610229 * value_3.z;
}



float targetOf_0(uint light_5, const CySurface_0 thread* surface_2, KernelContext_0 thread* kernelContext_5)
{

#line 183
    uint _S93 = wordBits_0(15U, 3U, kernelContext_5);

    if(light_5 >= _S93)
    {
        return 0.0;
    }

#line 187
    thread Light_0 _S94 = kernelContext_5->cyManyLights_0->lights_0[light_5];

#line 187
    float3 _S95 = lightIrradiance_0(&_S94, surface_2->position_0, surface_2->normal_1);

    return luminance_0(_S95);
}


#line 236
float contributionWeight_0(const Reservoir_0 thread* reservoir_4)
{

#line 236
    bool _S96 = reservoirValid_0(reservoir_4);

#line 236
    bool _S97;

    if(!_S96)
    {

#line 238
        _S97 = true;

#line 238
    }
    else
    {

#line 238
        _S97 = (reservoir_4->count_0) <= 0.0;

#line 238
    }

#line 238
    if(_S97)
    {
        return 0.0;
    }
    return reservoir_4->weightSum_0 / (reservoir_4->count_0 * reservoir_4->target_1);
}


#line 449
Reservoir_0 spatialReuse_0(uint2 pixel_2, const CySurface_0 thread* surface_3, const Reservoir_0 thread* own_0, uint2 extent_0, Stream_0 thread* stream_2, KernelContext_0 thread* kernelContext_6)
{

#line 450
    uint seen_0;

#line 450
    bool _S98;

#line 450
    float _S99;

    thread array<Reservoir_0, int(9)> inputs_0;


    inputs_0[int(0)] = *own_0;

#line 453
    thread array<CySurface_0, int(9)> surfaces_0;


    surfaces_0[int(0)] = *surface_3;

#line 454
    thread array<int2, int(9)> places_0;


    int2 _S100 = int2(pixel_2);

#line 457
    places_0[int(0)] = _S100;

#line 457
    uint _S101 = wordBits_0(9U, 3U, kernelContext_6);

    uint _S102 = min(_S101, 8U);

#line 459
    float4 _S103 = word_0(10U, kernelContext_6);
    float _S104 = _S103.y;

#line 460
    uint index_4 = 0U;

#line 460
    uint count_1 = 1U;
    for(;;)
    {

#line 461
        if(index_4 < _S102)
        {
        }
        else
        {

#line 461
            break;
        }
        float _S105 = nextUnit_0(stream_2);

#line 463
        float _S106 = _S105 * 6.28318548202514648;
        float _S107 = nextUnit_0(stream_2);
        int2 _S108 = _S100 + int2(round(float2(cos(_S106), sin(_S106)) * float2(((0.5 + _S107 * 0.5) * _S104)) ));
        int _S109 = _S108.x;

#line 466
        if(_S109 < int(0))
        {

#line 466
            _S98 = true;

#line 466
        }
        else
        {

#line 466
            _S98 = (_S108.y) < int(0);

#line 466
        }

#line 466
        bool _S110;

)cy_msl"
    R"cy_msl(#line 466
        if(_S98)
        {

#line 466
            _S110 = true;

#line 466
        }
        else
        {

#line 466
            _S110 = _S109 >= int(extent_0.x);

#line 466
        }

#line 466
        bool _S111;

#line 466
        if(_S110)
        {

#line 466
            _S111 = true;

#line 466
        }
        else
        {

#line 466
            _S111 = (_S108.y) >= int(extent_0.y);

#line 466
        }

#line 466
        if(_S111)
        {
            index_4 = index_4 + 1U;

#line 461
            continue;
        }

#line 461
        bool repeated_0 = false;

#line 461
        seen_0 = 0U;

#line 471
        for(;;)
        {

#line 471
            if(seen_0 < count_1)
            {
            }
            else
            {

#line 471
                break;
            }
            if(repeated_0)
            {

#line 473
                repeated_0 = true;

#line 473
            }
            else
            {

#line 473
                repeated_0 = all((places_0[seen_0]) == _S108);

#line 473
            }

#line 471
            seen_0 = seen_0 + 1U;

#line 471
        }



        float4 _S112 = ((kernelContext_6->cyManyLights_0->surface_0).read(vec<uint,2>(((_S108)).xy)));

#line 475
        bool _S113;
        if(repeated_0)
        {

#line 476
            _S113 = true;

#line 476
        }
        else
        {

#line 476
            _S113 = (_S112.x) <= 0.0;

#line 476
        }

#line 476
        bool _S114;

#line 476
        if(_S113)
        {

#line 476
            _S114 = true;

#line 476
        }
        else
        {

#line 476
            _S114 = (abs(_S112.x - surface_3->viewDepth_0)) > (0.10000000149011612 * surface_3->viewDepth_0);

#line 476
        }

#line 476
        bool _S115;

#line 476
        if(_S114)
        {

#line 476
            _S115 = true;

#line 476
        }
        else
        {

#line 476
            _S115 = (dot(_S112.yzw, surface_3->normal_1)) < 0.89999997615814209;

#line 476
        }

#line 476
        if(_S115)
        {

            index_4 = index_4 + 1U;

#line 461
            continue;
        }

#line 483
        float4 _S116 = ((kernelContext_6->cyManyLights_0->temporalReservoir_0).read(vec<uint,2>(((_S108)).xy)));

#line 483
        inputs_0[count_1] = toReservoir_0(_S116);

#line 483
        CySurface_0 _S117 = surfaceAt_0(uint2(_S108), kernelContext_6);
        surfaces_0[count_1] = _S117;
        places_0[count_1] = _S108;

#line 485
        count_1 = count_1 + 1U;

#line 461
        index_4 = index_4 + 1U;

#line 461
    }

#line 488
    thread Reservoir_0 combined_0 = emptyReservoir_0();

#line 488
    index_4 = 0U;

#line 488
    float total_0 = 0.0;

    for(;;)
    {

#line 490
        if(index_4 < count_1)
        {
        }
        else
        {

#line 490
            break;
        }
        float total_1 = total_0 + (&inputs_0[index_4])->count_0;

#line 490
        index_4 = index_4 + 1U;

#line 490
        total_0 = total_1;

#line 490
    }

#line 490
    index_4 = 0U;



    for(;;)
    {

#line 494
        if(index_4 < count_1)
        {
        }
        else
        {

#line 494
            break;
        }
        Reservoir_0 _S118 = inputs_0[index_4];

#line 496
        thread Reservoir_0 _S119 = inputs_0[index_4];

#line 496
        bool _S120 = reservoirValid_0(&_S119);
        if(!_S120)
        {
            index_4 = index_4 + 1U;

#line 494
            continue;
        }

#line 501
        uint _S121 = uint(_S118.light_0);

#line 501
        float _S122 = targetOf_0(_S121, surface_3, kernelContext_6);

        if(!(_S122 > 0.0))
        {
            index_4 = index_4 + 1U;

#line 494
            continue;
        }

#line 494
        seen_0 = 0U;

#line 494
        float denominator_0 = 0.0;

#line 508
        for(;;)
        {

#line 508
            if(seen_0 < count_1)
            {
            }
            else
            {

#line 508
                break;
            }
            if(seen_0 == index_4)
            {

#line 510
                _S99 = _S118.target_1;

#line 510
            }
            else
            {

#line 510
                thread CySurface_0 _S123 = surfaces_0[seen_0];

#line 510
                float _S124 = targetOf_0(_S121, &_S123, kernelContext_6);

)cy_msl"
    R"cy_msl(#line 510
                _S99 = _S124;

#line 510
            }
            float denominator_1 = denominator_0 + (&inputs_0[seen_0])->count_0 * _S99;

#line 508
            seen_0 = seen_0 + 1U;

#line 508
            denominator_0 = denominator_1;

#line 508
        }

#line 513
        if(denominator_0 > 0.0)
        {

#line 513
            _S99 = _S118.count_0 * _S118.target_1 / denominator_0;

#line 513
        }
        else
        {

#line 513
            _S99 = 0.0;

#line 513
        }
        float _S125 = _S99 * _S122;

#line 514
        thread Reservoir_0 _S126 = _S118;

#line 514
        float _S127 = contributionWeight_0(&_S126);

#line 514
        float _S128 = _S125 * _S127;
        (&combined_0)->weightSum_0 = (&combined_0)->weightSum_0 + _S128;
        if(_S128 > 0.0)
        {

#line 516
            float _S129 = nextUnit_0(stream_2);

#line 516
            _S98 = (_S129 * (&combined_0)->weightSum_0) <= _S128;

#line 516
        }
        else
        {

#line 516
            _S98 = false;

#line 516
        }

#line 516
        if(_S98)
        {
            (&combined_0)->light_0 = _S118.light_0;
            (&combined_0)->target_1 = _S122;

#line 516
        }

#line 494
        index_4 = index_4 + 1U;

#line 494
    }

#line 524
    (&combined_0)->count_0 = total_0;
    (&combined_0)->weightSum_0 = (&combined_0)->weightSum_0 * total_0;
    return combined_0;
}


#line 432 "src/rendering/shaders/cy/light.slang"
float3 sampleLightPoint_0(const Light_0 thread* light_6, float3 towards_1, float u_0, float v_0)
{

#line 432
    float3 _S130 = light_6->positionRelativeToCamera_0;

#line 432
    uint _S131 = light_6->kind_0;


    if(!isAreaLight_0(light_6->kind_0))
    {
        return _S130;
    }
    uint _S132 = _S131 & 255U;
    float _S133 = 2.0 * u_0 - 1.0;
    float _S134 = 2.0 * v_0 - 1.0;
    float3 _S135 = _S130 - towards_1;
    float3 _S136 = unitOr_0(light_6->direction_0, float3(0.0, -1.0, 0.0));
    if(_S132 == 4U)
    {
        float3 _S137 = decodeAreaTangent_0(light_6->shapeWord_0);
        float3 _S138 = unitOr_0(_S137 - _S136 * float3(dot(_S137, _S136)) , facingPerpendicular_0(_S136, _S135));

        return _S130 + _S138 * float3((_S133 * light_6->spotScaleBias_0.x))  + cross(_S136, _S138) * float3((_S134 * light_6->spotScaleBias_0.y)) ;
    }
    if(_S132 == 7U)
    {

        return _S130 + facingPerpendicular_0(_S136, _S135) * float3((_S133 * light_6->spotScaleBias_0.y))  + _S136 * float3((_S134 * light_6->spotScaleBias_0.x)) ;
    }

#line 454
    float3 axisX_3;

#line 454
    float3 axisY_1;



    if(_S132 == 5U)
    {
        float3 _S139 = decodeAreaTangent_0(light_6->shapeWord_0);
        float3 axisX_4 = unitOr_0(_S139 - _S136 * float3(dot(_S139, _S136)) , facingPerpendicular_0(_S136, _S135));
        float3 _S140 = cross(_S136, axisX_4);

#line 462
        axisX_3 = axisX_4;

#line 462
        axisY_1 = _S140;

#line 458
    }
    else
    {

#line 466
        float3 _S141 = unitOr_0(towards_1 - _S130, float3(0.0, 0.0, 1.0));
        float3 axisX_5 = facingPerpendicular_0(_S141, float3(0.0, 1.0, 0.0));
        float3 _S142 = cross(_S141, axisX_5);

#line 468
        axisX_3 = axisX_5;

#line 468
        axisY_1 = _S142;

#line 458
    }

#line 470
    float _S143 = light_6->spotScaleBias_0.x * sqrt(u_0);
    float _S144 = 6.28318548202514648 * v_0;
    return _S130 + axisX_3 * float3((_S143 * cos(_S144)))  + axisY_1 * float3((_S143 * sin(_S144))) ;
}


#line 397 "src/rendering/lights_gpu/shaders/many_lights.slang"
float traceVisibility_0(const CySurface_0 thread* surface_4, float3 to_0, uint2 extent_1, KernelContext_0 thread* kernelContext_7)
{

#line 397
    uint _S145 = wordBits_0(10U, 3U, kernelContext_7);

    uint _S146 = max(_S145, 1U);

#line 399
    float4 _S147 = word_0(14U, kernelContext_7);
    float _S148 = _S147.x;

#line 400
    float3 _S149 = surface_4->position_0;

#line 400
    float3 _S150 = surface_4->normal_1;

#line 400
    float4 _S151 = word_0(14U, kernelContext_7);
    float3 _S152 = _S149 + _S150 * float3(_S151.y) ;
    float3 _S153 = to_0 - _S152;

#line 402
    uint step_0 = 1U;
    for(;;)
    {

#line 403
        if(step_0 <= _S146)
        {
        }
        else
        {

#line 403
            break;
        }

        float3 _S154 = _S152 + _S153 * float3(((float(step_0) - 0.5) / float(_S146) * 0.98000001907348633)) ;

#line 406
        float4 _S155 = word_0(11U, kernelContext_7);


        float _S156 = - dot(_S155.xyz, _S154);
        if(_S156 <= 0.0)
        {
            return 1.0;
        }

#line 412
        float4 _S157 = word_0(16U, kernelContext_7);

        float4 _S158 = float4(_S154, 1.0);

#line 414
        float _S159 = dot(_S157, _S158);

#line 414
        float4 _S160 = word_0(17U, kernelContext_7);

#line 414
        float _S161 = dot(_S160, _S158);

#line 414
        float4 _S162 = word_0(19U, kernelContext_7);
        float _S163 = dot(_S162, _S158);
        if(_S163 <= 0.0)
        {
            return 1.0;
        }
        float2 _S164 = float2(_S159, _S161) / float2(_S163) ;
        float _S165 = _S164.x * 0.5 + 0.5;

#line 421
        float _S166 = 0.5 - _S164.y * 0.5;

#line 421
        float2 _S167 = float2(_S165, _S166);

#line 421
        bool _S168;
        if(_S165 < 0.0)
        {

#line 422
            _S168 = true;

#line 422
        }
        else
        {

#line 422
            _S168 = _S166 < 0.0;

#line 422
        }

#line 422
        bool _S169;

#line 422
        if(_S168)
        {

#line 422
            _S169 = true;

#line 422
        }
        else
        {

#line 422
            _S169 = _S165 >= 1.0;

#line 422
        }

#line 422
        bool _S170;

#line 422
        if(_S169)
        {

#line 422
            _S170 = true;

#line 422
        }
        else
        {

)cy_msl"
    R"cy_msl(#line 422
            _S170 = _S166 >= 1.0;

#line 422
        }

#line 422
        if(_S170)
        {
            return 1.0;
        }

        float4 _S171 = ((kernelContext_7->cyManyLights_0->surface_0).read(vec<uint,2>(((int2(uint2(_S167 * float2(extent_1))))).xy)));

#line 427
        float _S172 = _S171.x;

#line 427
        bool _S173;
        if(_S172 > 0.0)
        {

#line 428
            _S173 = _S156 > (_S172 + 0.00100000004749745);

#line 428
        }
        else
        {

#line 428
            _S173 = false;

#line 428
        }

#line 428
        bool _S174;

#line 428
        if(_S173)
        {

#line 428
            _S174 = _S156 < (_S172 + _S148);

#line 428
        }
        else
        {

#line 428
            _S174 = false;

#line 428
        }

#line 428
        if(_S174)
        {
            return 0.0;
        }

#line 403
        step_0 = step_0 + 1U;

#line 403
    }

#line 433
    return 1.0;
}


#line 531
[[kernel]] void cyManyLightsSpatial(uint3 id_0 [[thread_position_in_grid]], CyManyLightSet_default_0 constant* cyManyLights_1 [[buffer(0)]])
{

#line 531
    thread KernelContext_0 kernelContext_8;

#line 531
    (&kernelContext_8)->cyManyLights_0 = cyManyLights_1;

#line 531
    float4 _S175 = word_0(8U, &kernelContext_8);

    uint2 _S176 = uint2(_S175.xy);

#line 533
    bool _S177;
    if((id_0.x) >= (_S176.x))
    {

#line 534
        _S177 = true;

#line 534
    }
    else
    {

#line 534
        _S177 = (id_0.y) >= (_S176.y);

#line 534
    }

#line 534
    if(_S177)
    {
        return;
    }
    uint2 _S178 = id_0.xy;

#line 538
    CySurface_0 _S179 = surfaceAt_0(_S178, &kernelContext_8);

    float4 _S180 = (((&kernelContext_8)->cyManyLights_0->temporalReservoir_0).read(vec<uint,2>(((int2(_S178))).xy)));

#line 540
    Reservoir_0 reservoir_5 = toReservoir_0(_S180);
    if(!_S179.valid_0)
    {

#line 541
        thread Reservoir_0 _S181 = emptyReservoir_0();

#line 541
        float4 _S182 = packReservoir_0(&_S181);

        (&kernelContext_8)->cyManyLights_0->finalReservoir_0.write(_S182,_S178);
        (&kernelContext_8)->cyManyLights_0->noisy_0.write(float4(0.0) ,_S178);
        (&kernelContext_8)->cyManyLights_0->resolved_0.write(float4(-1.0, 0.0, 0.0, 0.0),_S178);
        return;
    }

#line 546
    uint _S183 = wordBits_0(9U, 0U, &kernelContext_8);

    thread Stream_0 stream_3 = streamFor_0(_S178, _S183, 2U);

#line 548
    uint _S184 = wordBits_0(15U, 1U, &kernelContext_8);

#line 548
    Reservoir_0 reservoir_6;
    if(_S184 != 0U)
    {

#line 549
        thread CySurface_0 _S185 = _S179;

#line 549
        thread Reservoir_0 _S186 = reservoir_5;

#line 549
        Reservoir_0 _S187 = spatialReuse_0(_S178, &_S185, &_S186, _S176, &stream_3, &kernelContext_8);

#line 549
        reservoir_6 = _S187;

#line 549
    }
    else
    {

#line 549
        reservoir_6 = reservoir_5;

#line 549
    }

#line 549
    thread Reservoir_0 _S188 = reservoir_6;

#line 549
    float4 _S189 = packReservoir_0(&_S188);



    (&kernelContext_8)->cyManyLights_0->finalReservoir_0.write(_S189,_S178);

#line 553
    thread Reservoir_0 _S190 = reservoir_6;

#line 553
    bool _S191 = reservoirValid_0(&_S190);
    if(!_S191)
    {
        (&kernelContext_8)->cyManyLights_0->noisy_0.write(float4(0.0) ,_S178);
        (&kernelContext_8)->cyManyLights_0->resolved_0.write(float4(-1.0, 0.0, 0.0, 0.0),_S178);
        return;
    }

#line 540
    Reservoir_0 _S192 = reservoir_6;

#line 560
    Light_0 _S193 = (&kernelContext_8)->cyManyLights_0->lights_0[uint(reservoir_6.light_0)];

#line 560
    thread Reservoir_0 _S194 = reservoir_6;

#line 560
    float _S195 = contributionWeight_0(&_S194);

#line 560
    uint _S196 = wordBits_0(15U, 2U, &kernelContext_8);

#line 560
    float visibility_0;


    if(_S196 != 0U)
    {
        float _S197 = nextUnit_0(&stream_3);

#line 565
        float _S198 = nextUnit_0(&stream_3);

#line 565
        thread Light_0 _S199 = _S193;

#line 565
        float3 _S200 = sampleLightPoint_0(&_S199, _S179.position_0, _S197, _S198);

#line 565
        thread CySurface_0 _S201 = _S179;

#line 565
        float _S202 = traceVisibility_0(&_S201, _S200, _S176, &kernelContext_8);

#line 565
        visibility_0 = _S202;

#line 563
    }
    else
    {

#line 563
        visibility_0 = 1.0;

#line 563
    }

#line 563
    thread Light_0 _S203 = _S193;

#line 563
    float3 _S204 = lightIrradiance_0(&_S203, _S179.position_0, _S179.normal_1);

#line 569
    float _S205 = _S195 * visibility_0;

#line 569
    (&kernelContext_8)->cyManyLights_0->noisy_0.write(float4(_S204 * float3(_S205) , 1.0),_S178);
    (&kernelContext_8)->cyManyLights_0->resolved_0.write(float4(_S192.light_0, _S205, 0.0, 0.0),_S178);
    return;
}

)cy_msl";

/// cyManyLightsAccumulate.metal, 4490 bytes.
inline constexpr char kManyLightsAccumulateMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    float3 positionRelativeToCamera_0;
    float range_0;
    float3 direction_0;
    float intensity_0;
    float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 31 "src/rendering/lights_gpu/shaders/many_lights.slang"
struct CyManyLightSet_default_0
{
    uint4 device* words_0;
    Light_0 device* lights_0;
    uint4 device* candidates_0;
    depth2d<float, access::sample> depth_0;
    texture2d<float, access::sample> normals_0;
    texture2d<float, access::read_write> temporalReservoir_0;
    texture2d<float, access::read_write> historyReservoir_0;
    texture2d<float, access::read_write> finalReservoir_0;
    texture2d<float, access::read_write> historySurface_0;
    texture2d<float, access::read_write> surface_0;
    texture2d<float, access::read_write> guidance_0;
    texture2d<float, access::read_write> noisy_0;
    texture2d<float, access::read_write> resolved_0;
    texture2d<float, access::read_write> historyColour_0;
    texture2d<float, access::read_write> historyMoments_0;
    texture2d<float, access::read_write> accumulated_0;
    texture2d<float, access::read_write> moments_0;
    texture2d<float, access::read_write> variance_0;
    texture2d<float, access::read_write> filterSource_0;
    texture2d<float, access::read_write> filterOut_0;
    texture2d<float, access::read_write> target_0;
};


#line 31
struct KernelContext_0
{
    CyManyLightSet_default_0 constant* cyManyLights_0;
};


#line 95
float4 word_0(uint index_0, KernelContext_0 thread* kernelContext_0)
{
    return (as_type<float4>((kernelContext_0->cyManyLights_0->words_0[index_0])));
}


#line 176
float luminance_0(float3 value_0)
{
    return 0.2125999927520752 * value_0.x + 0.71520000696182251 * value_0.y + 0.07220000028610229 * value_0.z;
}


#line 577
[[kernel]] void cyManyLightsAccumulate(uint3 id_0 [[thread_position_in_grid]], CyManyLightSet_default_0 constant* cyManyLights_1 [[buffer(0)]])
{

#line 577
    thread KernelContext_0 kernelContext_1;

#line 577
    (&kernelContext_1)->cyManyLights_0 = cyManyLights_1;

#line 577
    float4 _S1 = word_0(8U, &kernelContext_1);

    uint2 _S2 = uint2(_S1.xy);
    uint _S3 = _S2.x;

#line 580
    bool _S4;

#line 580
    if((id_0.x) >= _S3)
    {

#line 580
        _S4 = true;

#line 580
    }
    else
    {

#line 580
        _S4 = (id_0.y) >= (_S2.y);

#line 580
    }

#line 580
    if(_S4)
    {
        return;
    }
    uint2 _S5 = id_0.xy;
    int2 _S6 = int2(_S5);

#line 585
    float4 _S7 = (((&kernelContext_1)->cyManyLights_0->noisy_0).read(vec<uint,2>(((_S6)).xy)));

#line 585
    float3 _S8 = _S7.xyz;
    float _S9 = luminance_0(_S8);

    float2 _S10 = (((&kernelContext_1)->cyManyLights_0->guidance_0).read(vec<uint,2>(((_S6)).xy)).xy);

#line 588
    float4 _S11 = word_0(13U, &kernelContext_1);
    float _S12 = _S11.x;

#line 589
    float4 _S13 = word_0(13U, &kernelContext_1);
    float _S14 = _S13.w;
    int _S15 = int(_S10.x);
    float _S16 = saturate(_S10.y);
    if(_S15 < int(0))
    {

#line 593
        _S4 = true;

#line 593
    }
    else
    {

#line 593
        _S4 = _S16 <= 0.0;

#line 593
    }

#line 593
    if(_S4)
    {
        (&kernelContext_1)->cyManyLights_0->accumulated_0.write(float4(_S8, 0.0),_S5);
        (&kernelContext_1)->cyManyLights_0->moments_0.write(float4(_S9, _S9 * _S9, 1.0, 0.0),_S5);
        return;
    }
    uint _S17 = uint(_S15);

#line 599
    uint _S18 = _S17 % _S3;

#line 599
    uint _S19 = _S17 / _S3;
    int2 _S20 = int2(uint2(_S18, _S19));

#line 600
    float4 _S21 = (((&kernelContext_1)->cyManyLights_0->historyColour_0).read(vec<uint,2>(((_S20)).xy)));

#line 600
    float3 _S22 = _S21.xyz;
    float4 _S23 = (((&kernelContext_1)->cyManyLights_0->historyMoments_0).read(vec<uint,2>(((_S20)).xy)));

    float _S24 = _S23.z * _S16 + 1.0;

#line 603
    float _S25;
    if(_S24 > 0.0)
    {

#line 604
        _S25 = 1.0 / _S24;

#line 604
    }
    else
    {

#line 604
        _S25 = 1.0;

#line 604
    }

#line 604
    float _S26 = max(_S25, _S14);
    (&kernelContext_1)->cyManyLights_0->accumulated_0.write(float4(mix(_S22, _S8, float3(_S26) ), 0.0),_S5);
    (&kernelContext_1)->cyManyLights_0->moments_0.write(float4(mix(_S23.x, _S9, _S26), mix(_S23.y, _S9 * _S9, _S26), min(_S24, _S12), 0.0),_S5);


    return;
}

)cy_msl";

/// cyManyLightsVariance.metal, 5986 bytes.
inline constexpr char kManyLightsVarianceMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    float3 positionRelativeToCamera_0;
    float range_0;
    float3 direction_0;
    float intensity_0;
    float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 5163 "hlsl.meta.slang"
struct CyManyLightSet_default_0
{
    uint4 device* words_0;
    Light_0 device* lights_0;
    uint4 device* candidates_0;
    depth2d<float, access::sample> depth_0;
    texture2d<float, access::sample> normals_0;
    texture2d<float, access::read_write> temporalReservoir_0;
    texture2d<float, access::read_write> historyReservoir_0;
    texture2d<float, access::read_write> finalReservoir_0;
    texture2d<float, access::read_write> historySurface_0;
    texture2d<float, access::read_write> surface_0;
    texture2d<float, access::read_write> guidance_0;
    texture2d<float, access::read_write> noisy_0;
    texture2d<float, access::read_write> resolved_0;
    texture2d<float, access::read_write> historyColour_0;
    texture2d<float, access::read_write> historyMoments_0;
    texture2d<float, access::read_write> accumulated_0;
    texture2d<float, access::read_write> moments_0;
    texture2d<float, access::read_write> variance_0;
    texture2d<float, access::read_write> filterSource_0;
    texture2d<float, access::read_write> filterOut_0;
    texture2d<float, access::read_write> target_0;
};


#line 5163
struct KernelContext_0
{
    CyManyLightSet_default_0 constant* cyManyLights_0;
};


#line 95 "src/rendering/lights_gpu/shaders/many_lights.slang"
float4 word_0(uint index_0, KernelContext_0 thread* kernelContext_0)
{
    return (as_type<float4>((kernelContext_0->cyManyLights_0->words_0[index_0])));
}


#line 613
float stableVariance_0(float first_0, float second_0)
{
    float _S1 = max(0.0, second_0 - first_0 * first_0);

#line 615
    float _S2;

    if(_S1 <= (9.99999997475242708e-07 * max(1.0, abs(second_0))))
    {

#line 617
        _S2 = 0.0;

#line 617
    }
    else
    {

#line 617
        _S2 = _S1;

#line 617
    }

#line 617
    return _S2;
}



[[kernel]] void cyManyLightsVariance(uint3 id_0 [[thread_position_in_grid]], CyManyLightSet_default_0 constant* cyManyLights_1 [[buffer(0)]])
{

#line 622
    thread KernelContext_0 kernelContext_1;

#line 622
    (&kernelContext_1)->cyManyLights_0 = cyManyLights_1;

#line 622
    float4 _S3 = word_0(8U, &kernelContext_1);

    uint2 _S4 = uint2(_S3.xy);
    uint _S5 = _S4.x;

#line 625
    bool _S6;

#line 625
    if((id_0.x) >= _S5)
    {

#line 625
        _S6 = true;

#line 625
    }
    else
    {

#line 625
        _S6 = (id_0.y) >= (_S4.y);

#line 625
    }

#line 625
    if(_S6)
    {
        return;
    }
    uint2 _S7 = id_0.xy;
    int2 _S8 = int2(_S7);

#line 630
    float4 _S9 = (((&kernelContext_1)->cyManyLights_0->moments_0).read(vec<uint,2>(((_S8)).xy)));
    float variance_1 = stableVariance_0(_S9.x, _S9.y);

#line 631
    float variance_2;
    if((_S9.z) < 4.0)
    {

#line 632
        int dy_0 = int(-1);

#line 632
        variance_2 = 0.0;

#line 632
        float sumSquared_0 = 0.0;

#line 632
        float count_0 = 0.0;

#line 637
        for(;;)
        {

#line 637
            if(dy_0 <= int(1))
            {
            }
            else
            {

#line 637
                break;
            }

#line 637
            int dx_0 = int(-1);

#line 637
            float sum_0 = variance_2;

#line 637
            float sumSquared_1 = sumSquared_0;

#line 637
            float count_1 = count_0;

            for(;;)
            {

#line 639
                if(dx_0 <= int(1))
                {
                }
                else
                {

#line 639
                    break;
                }
                int2 _S10 = _S8 + int2(dx_0, dy_0);
                int _S11 = _S10.x;

#line 642
                if(_S11 < int(0))
                {

#line 642
                    _S6 = true;

#line 642
                }
                else
                {

#line 642
                    _S6 = (_S10.y) < int(0);

#line 642
                }

#line 642
                bool _S12;

#line 642
                if(_S6)
                {

#line 642
                    _S12 = true;

#line 642
                }
                else
                {

#line 642
                    _S12 = _S11 >= int(_S5);

#line 642
                }

#line 642
                bool _S13;

#line 642
                if(_S12)
                {

#line 642
                    _S13 = true;

#line 642
                }
                else
                {

#line 642
                    _S13 = (_S10.y) >= int(_S4.y);

#line 642
                }

#line 642
                if(_S13)
                {

                    dx_0 = dx_0 + int(1);

#line 639
                    continue;
                }

#line 647
                float4 _S14 = (((&kernelContext_1)->cyManyLights_0->moments_0).read(vec<uint,2>(((_S10)).xy)));

#line 647
                float _S15 = _S14.x;

                float sumSquared_2 = sumSquared_1 + _S15 * _S15;
                float count_2 = count_1 + 1.0;

#line 650
                sum_0 = sum_0 + _S15;

#line 650
                sumSquared_1 = sumSquared_2;

#line 650
                count_1 = count_2;

#line 639
                dx_0 = dx_0 + int(1);

#line 639
            }

#line 637
            dy_0 = dy_0 + int(1);

#line 637
            variance_2 = sum_0;

#line 637
            sumSquared_0 = sumSquared_1;

)cy_msl"
    R"cy_msl(#line 637
            count_0 = count_1;

#line 637
        }

#line 637
        variance_2 = max(variance_1, stableVariance_0(variance_2 / count_0, sumSquared_0 / count_0));

#line 632
    }
    else
    {

#line 632
        variance_2 = variance_1;

#line 632
    }

#line 656
    (&kernelContext_1)->cyManyLights_0->variance_0.write(float4(variance_2, int(0), int(0), int(0)),_S7);
    return;
}

)cy_msl";

/// cyManyLightsFilter.metal, 9071 bytes.
inline constexpr char kManyLightsFilterMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    float3 positionRelativeToCamera_0;
    float range_0;
    float3 direction_0;
    float intensity_0;
    float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 31 "src/rendering/lights_gpu/shaders/many_lights.slang"
struct CyManyLightSet_default_0
{
    uint4 device* words_0;
    Light_0 device* lights_0;
    uint4 device* candidates_0;
    depth2d<float, access::sample> depth_0;
    texture2d<float, access::sample> normals_0;
    texture2d<float, access::read_write> temporalReservoir_0;
    texture2d<float, access::read_write> historyReservoir_0;
    texture2d<float, access::read_write> finalReservoir_0;
    texture2d<float, access::read_write> historySurface_0;
    texture2d<float, access::read_write> surface_0;
    texture2d<float, access::read_write> guidance_0;
    texture2d<float, access::read_write> noisy_0;
    texture2d<float, access::read_write> resolved_0;
    texture2d<float, access::read_write> historyColour_0;
    texture2d<float, access::read_write> historyMoments_0;
    texture2d<float, access::read_write> accumulated_0;
    texture2d<float, access::read_write> moments_0;
    texture2d<float, access::read_write> variance_0;
    texture2d<float, access::read_write> filterSource_0;
    texture2d<float, access::read_write> filterOut_0;
    texture2d<float, access::read_write> target_0;
};


#line 71
struct CyManyLightPush_0
{
    uint pass_0;
    uint reserved0_0;
    uint reserved1_0;
    uint reserved2_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    CyManyLightSet_default_0 constant* cyManyLights_0;
    CyManyLightPush_0 constant* cyManyLightPush_0;
};


#line 95 "src/rendering/lights_gpu/shaders/many_lights.slang"
float4 word_0(uint index_0, KernelContext_0 thread* kernelContext_0)
{
    return (as_type<float4>((kernelContext_0->cyManyLights_0->words_0[index_0])));
}


#line 176
float luminance_0(float3 value_0)
{
    return 0.2125999927520752 * value_0.x + 0.71520000696182251 * value_0.y + 0.07220000028610229 * value_0.z;
}


#line 661
float splineWeight_0(int offset_0)
{
    int _S1 = abs(offset_0);

#line 663
    float _S2;
    if(_S1 == int(0))
    {

#line 664
        _S2 = 0.375;

#line 664
    }
    else
    {

#line 664
        if(_S1 == int(1))
        {

#line 664
            _S2 = 0.25;

#line 664
        }
        else
        {

#line 664
            if(_S1 == int(2))
            {

#line 664
                _S2 = 0.0625;

#line 664
            }
            else
            {

#line 664
                _S2 = 0.0;

#line 664
            }

#line 664
        }

#line 664
    }

#line 664
    return _S2;
}


float3 filterPixel_0(uint2 pixel_0, uint2 extent_0, KernelContext_0 thread* kernelContext_1)
{
    int2 _S3 = int2(pixel_0);

#line 670
    float4 _S4 = ((kernelContext_1->cyManyLights_0->filterSource_0).read(vec<uint,2>(((_S3)).xy)));
    float3 _S5 = _S4.xyz;
    float4 _S6 = ((kernelContext_1->cyManyLights_0->surface_0).read(vec<uint,2>(((_S3)).xy)));
    float _S7 = _S6.x;

#line 673
    float4 _S8 = word_0(12U, kernelContext_1);

#line 673
    float4 _S9 = word_0(13U, kernelContext_1);

#line 673
    float3 result_0;


    if(_S7 > 0.0)
    {
        float _S10 = luminance_0(_S5);
        float _S11 = ((kernelContext_1->cyManyLights_0->variance_0).read(vec<uint,2>(((_S3)).xy)).x);

#line 679
        float _S12 = sqrt(_S11);
        float4 _S13 = ((kernelContext_1->cyManyLights_0->moments_0).read(vec<uint,2>(((_S3)).xy)));

#line 680
        bool _S14;
        if((_S13.z) >= 12.0)
        {

#line 681
            _S14 = _S12 <= (0.01999999955296516 * (abs(_S10) + 9.99999997475242708e-07));

#line 681
        }
        else
        {

#line 681
            _S14 = false;

#line 681
        }
        if(!_S14)
        {
            uint _S15 = max(1U, 1U << (kernelContext_1->cyManyLightPush_0->pass_0));
            int _S16 = int(_S9.z);
            float3 _S17 = float3(0.0) ;

            int _S18 = - _S16;

#line 688
            int dy_0 = _S18;

#line 688
            float3 sum_0 = _S17;

#line 688
            float weightSum_0 = 0.0;

#line 688
            for(;;)
            {

#line 688
                if(dy_0 <= _S16)
                {
                }
                else
                {

#line 688
                    break;
                }

#line 688
                int dx_0 = _S18;

#line 688
                result_0 = sum_0;

#line 688
                float weightSum_1 = weightSum_0;

                for(;;)
                {

#line 690
                    if(dx_0 <= _S16)
                    {
                    }
                    else
                    {

#line 690
                        break;
                    }
                    int2 _S19 = _S3 + int2(dx_0, dy_0) * int2(int(_S15)) ;
                    int _S20 = _S19.x;

#line 693
                    if(_S20 < int(0))
                    {

#line 693
                        _S14 = true;

#line 693
                    }
                    else
                    {

#line 693
                        _S14 = (_S19.y) < int(0);

#line 693
                    }

#line 693
                    bool _S21;

#line 693
                    if(_S14)
                    {

#line 693
                        _S21 = true;

#line 693
                    }
                    else
                    {

#line 693
                        _S21 = _S20 >= int(extent_0.x);

#line 693
                    }

#line 693
                    bool _S22;

#line 693
                    if(_S21)
                    {

#line 693
                        _S22 = true;

#line 693
                    }
                    else
                    {

#line 693
                        _S22 = (_S19.y) >= int(extent_0.y);

#line 693
                    }

)cy_msl"
    R"cy_msl(#line 693
                    if(_S22)
                    {
                        dx_0 = dx_0 + int(1);

#line 690
                        continue;
                    }

#line 697
                    float4 _S23 = ((kernelContext_1->cyManyLights_0->surface_0).read(vec<uint,2>(((_S19)).xy)));
                    float _S24 = _S23.x;

#line 698
                    if(_S24 <= 0.0)
                    {
                        dx_0 = dx_0 + int(1);

#line 690
                        continue;
                    }

#line 704
                    float weight_0 = splineWeight_0(dx_0) * splineWeight_0(dy_0) * exp(- abs(_S24 - _S7) / (_S8.x * abs(_S7) + 9.99999997475242708e-07)) * pow(max(0.0, dot(_S6.yzw, _S23.yzw)), _S8.y);
                    float4 _S25 = ((kernelContext_1->cyManyLights_0->filterSource_0).read(vec<uint,2>(((_S19)).xy)));

#line 705
                    float3 _S26 = _S25.xyz;

                    float _S27 = max(weight_0 * exp(- abs(luminance_0(_S26) - _S10) / (_S8.z * _S12 + 9.99999997475242708e-07)), 0.0);
                    if(_S27 <= 0.0)
                    {
                        dx_0 = dx_0 + int(1);

#line 690
                        continue;
                    }

#line 713
                    float weightSum_2 = weightSum_1 + _S27;

#line 713
                    result_0 = result_0 + _S26 * float3(_S27) ;

#line 713
                    weightSum_1 = weightSum_2;

#line 690
                    dx_0 = dx_0 + int(1);

#line 690
                }

#line 688
                dy_0 = dy_0 + int(1);

#line 688
                sum_0 = result_0;

#line 688
                weightSum_0 = weightSum_1;

#line 688
            }

#line 716
            if(weightSum_0 > 0.0)
            {

#line 716
                result_0 = sum_0 / float3(weightSum_0) ;

#line 716
            }
            else
            {

#line 716
                result_0 = _S5;

#line 716
            }

#line 682
        }
        else
        {

#line 682
            result_0 = _S5;

#line 682
        }

#line 676
    }
    else
    {

#line 676
        result_0 = _S5;

#line 676
    }

#line 722
    return result_0;
}




[[kernel]] void cyManyLightsFilter(uint3 id_0 [[thread_position_in_grid]], CyManyLightSet_default_0 constant* cyManyLights_1 [[buffer(0)]], CyManyLightPush_0 constant* cyManyLightPush_1 [[buffer(1)]])
{

#line 728
    thread KernelContext_0 kernelContext_2;

#line 728
    (&kernelContext_2)->cyManyLights_0 = cyManyLights_1;

#line 728
    (&kernelContext_2)->cyManyLightPush_0 = cyManyLightPush_1;

#line 728
    float4 _S28 = word_0(8U, &kernelContext_2);

    uint2 _S29 = uint2(_S28.xy);

#line 730
    bool _S30;
    if((id_0.x) >= (_S29.x))
    {

#line 731
        _S30 = true;

#line 731
    }
    else
    {

#line 731
        _S30 = (id_0.y) >= (_S29.y);

#line 731
    }

#line 731
    if(_S30)
    {
        return;
    }
    uint2 _S31 = id_0.xy;

#line 735
    float3 _S32 = filterPixel_0(_S31, _S29, &kernelContext_2);

#line 735
    (&kernelContext_2)->cyManyLights_0->filterOut_0.write(float4(_S32, 1.0),_S31);
    return;
}

)cy_msl";

/// cyManyLightsFilterLast.metal, 9080 bytes.
inline constexpr char kManyLightsFilterLastMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    float3 positionRelativeToCamera_0;
    float range_0;
    float3 direction_0;
    float intensity_0;
    float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 31 "src/rendering/lights_gpu/shaders/many_lights.slang"
struct CyManyLightSet_default_0
{
    uint4 device* words_0;
    Light_0 device* lights_0;
    uint4 device* candidates_0;
    depth2d<float, access::sample> depth_0;
    texture2d<float, access::sample> normals_0;
    texture2d<float, access::read_write> temporalReservoir_0;
    texture2d<float, access::read_write> historyReservoir_0;
    texture2d<float, access::read_write> finalReservoir_0;
    texture2d<float, access::read_write> historySurface_0;
    texture2d<float, access::read_write> surface_0;
    texture2d<float, access::read_write> guidance_0;
    texture2d<float, access::read_write> noisy_0;
    texture2d<float, access::read_write> resolved_0;
    texture2d<float, access::read_write> historyColour_0;
    texture2d<float, access::read_write> historyMoments_0;
    texture2d<float, access::read_write> accumulated_0;
    texture2d<float, access::read_write> moments_0;
    texture2d<float, access::read_write> variance_0;
    texture2d<float, access::read_write> filterSource_0;
    texture2d<float, access::read_write> filterOut_0;
    texture2d<float, access::read_write> target_0;
};


#line 71
struct CyManyLightPush_0
{
    uint pass_0;
    uint reserved0_0;
    uint reserved1_0;
    uint reserved2_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    CyManyLightSet_default_0 constant* cyManyLights_0;
    CyManyLightPush_0 constant* cyManyLightPush_0;
};


#line 95 "src/rendering/lights_gpu/shaders/many_lights.slang"
float4 word_0(uint index_0, KernelContext_0 thread* kernelContext_0)
{
    return (as_type<float4>((kernelContext_0->cyManyLights_0->words_0[index_0])));
}


#line 176
float luminance_0(float3 value_0)
{
    return 0.2125999927520752 * value_0.x + 0.71520000696182251 * value_0.y + 0.07220000028610229 * value_0.z;
}


#line 661
float splineWeight_0(int offset_0)
{
    int _S1 = abs(offset_0);

#line 663
    float _S2;
    if(_S1 == int(0))
    {

#line 664
        _S2 = 0.375;

#line 664
    }
    else
    {

#line 664
        if(_S1 == int(1))
        {

#line 664
            _S2 = 0.25;

#line 664
        }
        else
        {

#line 664
            if(_S1 == int(2))
            {

#line 664
                _S2 = 0.0625;

#line 664
            }
            else
            {

#line 664
                _S2 = 0.0;

#line 664
            }

#line 664
        }

#line 664
    }

#line 664
    return _S2;
}


float3 filterPixel_0(uint2 pixel_0, uint2 extent_0, KernelContext_0 thread* kernelContext_1)
{
    int2 _S3 = int2(pixel_0);

#line 670
    float4 _S4 = ((kernelContext_1->cyManyLights_0->filterSource_0).read(vec<uint,2>(((_S3)).xy)));
    float3 _S5 = _S4.xyz;
    float4 _S6 = ((kernelContext_1->cyManyLights_0->surface_0).read(vec<uint,2>(((_S3)).xy)));
    float _S7 = _S6.x;

#line 673
    float4 _S8 = word_0(12U, kernelContext_1);

#line 673
    float4 _S9 = word_0(13U, kernelContext_1);

#line 673
    float3 result_0;


    if(_S7 > 0.0)
    {
        float _S10 = luminance_0(_S5);
        float _S11 = ((kernelContext_1->cyManyLights_0->variance_0).read(vec<uint,2>(((_S3)).xy)).x);

#line 679
        float _S12 = sqrt(_S11);
        float4 _S13 = ((kernelContext_1->cyManyLights_0->moments_0).read(vec<uint,2>(((_S3)).xy)));

#line 680
        bool _S14;
        if((_S13.z) >= 12.0)
        {

#line 681
            _S14 = _S12 <= (0.01999999955296516 * (abs(_S10) + 9.99999997475242708e-07));

#line 681
        }
        else
        {

#line 681
            _S14 = false;

#line 681
        }
        if(!_S14)
        {
            uint _S15 = max(1U, 1U << (kernelContext_1->cyManyLightPush_0->pass_0));
            int _S16 = int(_S9.z);
            float3 _S17 = float3(0.0) ;

            int _S18 = - _S16;

#line 688
            int dy_0 = _S18;

#line 688
            float3 sum_0 = _S17;

#line 688
            float weightSum_0 = 0.0;

#line 688
            for(;;)
            {

#line 688
                if(dy_0 <= _S16)
                {
                }
                else
                {

#line 688
                    break;
                }

#line 688
                int dx_0 = _S18;

#line 688
                result_0 = sum_0;

#line 688
                float weightSum_1 = weightSum_0;

                for(;;)
                {

#line 690
                    if(dx_0 <= _S16)
                    {
                    }
                    else
                    {

#line 690
                        break;
                    }
                    int2 _S19 = _S3 + int2(dx_0, dy_0) * int2(int(_S15)) ;
                    int _S20 = _S19.x;

#line 693
                    if(_S20 < int(0))
                    {

#line 693
                        _S14 = true;

#line 693
                    }
                    else
                    {

#line 693
                        _S14 = (_S19.y) < int(0);

#line 693
                    }

#line 693
                    bool _S21;

#line 693
                    if(_S14)
                    {

#line 693
                        _S21 = true;

#line 693
                    }
                    else
                    {

#line 693
                        _S21 = _S20 >= int(extent_0.x);

#line 693
                    }

#line 693
                    bool _S22;

#line 693
                    if(_S21)
                    {

#line 693
                        _S22 = true;

#line 693
                    }
                    else
                    {

#line 693
                        _S22 = (_S19.y) >= int(extent_0.y);

#line 693
                    }

)cy_msl"
    R"cy_msl(#line 693
                    if(_S22)
                    {
                        dx_0 = dx_0 + int(1);

#line 690
                        continue;
                    }

#line 697
                    float4 _S23 = ((kernelContext_1->cyManyLights_0->surface_0).read(vec<uint,2>(((_S19)).xy)));
                    float _S24 = _S23.x;

#line 698
                    if(_S24 <= 0.0)
                    {
                        dx_0 = dx_0 + int(1);

#line 690
                        continue;
                    }

#line 704
                    float weight_0 = splineWeight_0(dx_0) * splineWeight_0(dy_0) * exp(- abs(_S24 - _S7) / (_S8.x * abs(_S7) + 9.99999997475242708e-07)) * pow(max(0.0, dot(_S6.yzw, _S23.yzw)), _S8.y);
                    float4 _S25 = ((kernelContext_1->cyManyLights_0->filterSource_0).read(vec<uint,2>(((_S19)).xy)));

#line 705
                    float3 _S26 = _S25.xyz;

                    float _S27 = max(weight_0 * exp(- abs(luminance_0(_S26) - _S10) / (_S8.z * _S12 + 9.99999997475242708e-07)), 0.0);
                    if(_S27 <= 0.0)
                    {
                        dx_0 = dx_0 + int(1);

#line 690
                        continue;
                    }

#line 713
                    float weightSum_2 = weightSum_1 + _S27;

#line 713
                    result_0 = result_0 + _S26 * float3(_S27) ;

#line 713
                    weightSum_1 = weightSum_2;

#line 690
                    dx_0 = dx_0 + int(1);

#line 690
                }

#line 688
                dy_0 = dy_0 + int(1);

#line 688
                sum_0 = result_0;

#line 688
                weightSum_0 = weightSum_1;

#line 688
            }

#line 716
            if(weightSum_0 > 0.0)
            {

#line 716
                result_0 = sum_0 / float3(weightSum_0) ;

#line 716
            }
            else
            {

#line 716
                result_0 = _S5;

#line 716
            }

#line 682
        }
        else
        {

#line 682
            result_0 = _S5;

#line 682
        }

#line 676
    }
    else
    {

#line 676
        result_0 = _S5;

#line 676
    }

#line 722
    return result_0;
}


#line 741
[[kernel]] void cyManyLightsFilterLast(uint3 id_0 [[thread_position_in_grid]], CyManyLightSet_default_0 constant* cyManyLights_1 [[buffer(0)]], CyManyLightPush_0 constant* cyManyLightPush_1 [[buffer(1)]])
{

#line 741
    thread KernelContext_0 kernelContext_2;

#line 741
    (&kernelContext_2)->cyManyLights_0 = cyManyLights_1;

#line 741
    (&kernelContext_2)->cyManyLightPush_0 = cyManyLightPush_1;

#line 741
    float4 _S28 = word_0(8U, &kernelContext_2);

    uint2 _S29 = uint2(_S28.xy);

#line 743
    bool _S30;
    if((id_0.x) >= (_S29.x))
    {

#line 744
        _S30 = true;

#line 744
    }
    else
    {

#line 744
        _S30 = (id_0.y) >= (_S29.y);

#line 744
    }

#line 744
    if(_S30)
    {
        return;
    }
    uint2 _S31 = id_0.xy;

#line 748
    float3 _S32 = filterPixel_0(_S31, _S29, &kernelContext_2);

#line 748
    (&kernelContext_2)->cyManyLights_0->target_0.write(float4(_S32, 1.0),_S31);
    return;
}

)cy_msl";

}  // namespace cy::rendering::lights_gpu

#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the ambient occlusion dispatches. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::occlusion {

/// gtao.metal, 11571 bytes.
inline constexpr char kGtaoMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 42 "src/rendering/occlusion/shaders/gtao_common.slang"
constant array<int2, int(8)> kCyGtaoDirections_0 = { int2(int(1), int(0)), int2(int(2), int(1)), int2(int(1), int(1)), int2(int(1), int(2)), int2(int(0), int(1)), int2(int(-1), int(2)), int2(int(-1), int(1)), int2(int(-2), int(1)) };

#line 51
constant array<uint, int(16)> kBayer_0 = { 0U, 8U, 2U, 10U, 12U, 4U, 14U, 6U, 3U, 11U, 1U, 9U, 15U, 7U, 13U, 5U };

#line 15
float cyGtaoViewDepth_0(float4 projection_0, float depth_0)
{
    return projection_0.w / (depth_0 + projection_0.z);
}



float3 cyGtaoViewPosition_0(float4 projection_1, float4 extent_0, float2 pixelCentre_0, float viewDepth_0)
{
    float2 _S1 = pixelCentre_0 * extent_0.zw;

    return float3((_S1.x * 2.0 - 1.0) * viewDepth_0 / projection_1.x, (1.0 - _S1.y * 2.0) * viewDepth_0 / projection_1.y, - viewDepth_0);
}


#line 54 "src/rendering/occlusion/shaders/gtao.slang"
struct CyGtaoConstants_0
{
    float4 viewRow0_0;
    float4 viewRow1_0;
    float4 viewRow2_0;
    float4 projection_2;
    float4 extent_1;
    float4 radius_0;
    float4 control_0;
    float4 jitter_0;
};


#line 5163 "hlsl.meta.slang"
struct CyGtaoSet_default_0
{
    depth2d<float, access::sample> depth_1;
    texture2d<float, access::sample> normals_0;
    texture2d<float, access::read_write> output_0;
};


#line 5163
struct KernelContext_0
{
    CyGtaoConstants_0 constant* cyGtaoConstants_0;
    CyGtaoSet_default_0 constant* cyGtaoSet_0;
};


#line 94 "src/rendering/occlusion/shaders/gtao.slang"
float3 viewPositionAt_0(float2 pixelCentre_1, float viewDepth_1, KernelContext_0 thread* kernelContext_0)
{
    return cyGtaoViewPosition_0(kernelContext_0->cyGtaoConstants_0->projection_2, kernelContext_0->cyGtaoConstants_0->extent_1, pixelCentre_1 + kernelContext_0->cyGtaoConstants_0->jitter_0.xy, viewDepth_1);
}


#line 23 "src/rendering/shaders/cy/packing.slang"
float3 decodeOctahedral_0(float2 encoded_0)
{
    float2 _S2 = encoded_0 * float2(2.0)  - float2(1.0) ;
    float _S3 = _S2.x;

#line 26
    float _S4 = _S2.y;

#line 26
    float _S5 = 1.0 - abs(_S3) - abs(_S4);

#line 26
    thread float3 normal_0 = float3(_S3, _S4, _S5);
    float _S6 = saturate(- _S5);
    float2 _S7 = float2(_S3, _S4);

#line 28
    normal_0.xy = _S7 + select(float2(_S6) , float2(- _S6) , _S7 >= (float2(0.0) ));
    return normalize(normal_0);
}


#line 30 "src/rendering/occlusion/shaders/gtao_common.slang"
float3 cyGtaoNormal_0(float4 packed_0)
{
    return decodeOctahedral_0(packed_0.xy);
}


#line 86 "src/rendering/occlusion/shaders/gtao.slang"
float3 toView_0(float3 relative_0, KernelContext_0 thread* kernelContext_1)
{
    return float3(dot(kernelContext_1->cyGtaoConstants_0->viewRow0_0.xyz, relative_0), dot(kernelContext_1->cyGtaoConstants_0->viewRow1_0.xyz, relative_0), dot(kernelContext_1->cyGtaoConstants_0->viewRow2_0.xyz, relative_0));
}


#line 49 "src/rendering/occlusion/shaders/gtao_common.slang"
uint cyGtaoDither_0(uint2 pixel_0)
{

    return kBayer_0[((pixel_0.y) & 3U) * 4U + ((pixel_0.x) & 3U)];
}


#line 116 "src/rendering/occlusion/shaders/gtao.slang"
float searchHorizon_0(int2 pixel_1, float3 position_0, float3 viewVector_0, int2 lattice_0, float radiusPixels_0, float stepNoise_0, float lowHorizon_0, KernelContext_0 thread* kernelContext_2)
{

    uint _S8 = uint(kernelContext_2->cyGtaoConstants_0->control_0.y);
    int2 _S9 = int2(kernelContext_2->cyGtaoConstants_0->extent_1.xy);


    float _S10 = length(float2(lattice_0));

#line 123
    float horizon_0 = lowHorizon_0;

#line 123
    uint index_0 = 0U;

    for(;;)
    {

#line 125
        if(index_0 < _S8)
        {
        }
        else
        {

#line 125
            break;
        }
        float _S11 = (float(index_0) + stepNoise_0) / float(_S8);

        int2 _S12 = pixel_1 + lattice_0 * int2(max(int(floor(_S11 * _S11 * radiusPixels_0 / _S10 + 0.5)), int(1))) ;
        int _S13 = _S12.x;

#line 130
        bool _S14;

#line 130
        if(_S13 < int(0))
        {

#line 130
            _S14 = true;

#line 130
        }
        else
        {

#line 130
            _S14 = (_S12.y) < int(0);

#line 130
        }

#line 130
        bool _S15;

#line 130
        if(_S14)
        {

#line 130
            _S15 = true;

#line 130
        }
        else
        {

#line 130
            _S15 = _S13 >= (_S9.x);

#line 130
        }

#line 130
        bool _S16;

#line 130
        if(_S15)
        {

#line 130
            _S16 = true;

#line 130
        }
        else
        {

#line 130
            _S16 = (_S12.y) >= (_S9.y);

#line 130
        }

#line 130
        if(_S16)
        {
            index_0 = index_0 + 1U;

#line 125
            continue;
        }

#line 134
        int3 _S17 = int3(_S12, int(0));

#line 134
        float _S18 = ((kernelContext_2->cyGtaoSet_0->depth_1).read(vec<uint,2>(((_S17)).xy), uint(((_S17)).z)));
        if(_S18 <= 0.0)
        {
            index_0 = index_0 + 1U;

#line 125
            continue;
        }

#line 125
        float3 _S19 = viewPositionAt_0(float2(_S12) + float2(0.5) , cyGtaoViewDepth_0(kernelContext_2->cyGtaoConstants_0->projection_2, _S18), kernelContext_2);

#line 141
        float3 _S20 = _S19 - position_0;
        float _S21 = sqrt(dot(_S20, _S20));
        if(_S21 <= 0.0)
        {
            index_0 = index_0 + 1U;

#line 125
            continue;
        }

#line 125
        horizon_0 = max(horizon_0, mix(lowHorizon_0, dot(_S20, viewVector_0) / _S21 - kernelContext_2->cyGtaoConstants_0->control_0.w, saturate(_S21 * kernelContext_2->cyGtaoConstants_0->radius_0.y + kernelContext_2->cyGtaoConstants_0->radius_0.z)));

#line 125
        index_0 = index_0 + 1U;

#line 125
    }

#line 151
    return horizon_0;
}


#line 109
float arcIntegral_0(float h_0, float n_0, float cosN_0, float sinN_0)
{
    float _S22 = 2.0 * h_0;

#line 111
    return (cosN_0 + _S22 * sinN_0 - cos(_S22 - n_0)) * 0.25;
}


#line 101
float3 toRelative_0(float3 view_0, KernelContext_0 thread* kernelContext_3)
{
    return kernelContext_3->cyGtaoConstants_0->viewRow0_0.xyz * float3(view_0.x)  + kernelContext_3->cyGtaoConstants_0->viewRow1_0.xyz * float3(view_0.y)  + kernelContext_3->cyGtaoConstants_0->viewRow2_0.xyz * float3(view_0.z) ;
}


#line 156
[[kernel]] void cyGtao(uint3 thread_0 [[thread_position_in_grid]], CyGtaoConstants_0 constant* cyGtaoConstants_1 [[buffer(1)]], CyGtaoSet_default_0 constant* cyGtaoSet_1 [[buffer(0)]])
{

#line 156
    float visibility_0;

#line 156
    thread KernelContext_0 kernelContext_4;

#line 156
    (&kernelContext_4)->cyGtaoConstants_0 = cyGtaoConstants_1;

#line 156
    (&kernelContext_4)->cyGtaoSet_0 = cyGtaoSet_1;

    uint2 _S23 = uint2(cyGtaoConstants_1->extent_1.xy);

#line 158
    bool _S24;
    if((thread_0.x) >= (_S23.x))
    {

#line 159
        _S24 = true;

#line 159
    }
    else
    {

)cy_msl"
    R"cy_msl(#line 159
        _S24 = (thread_0.y) >= (_S23.y);

#line 159
    }

#line 159
    if(_S24)
    {
        return;
    }
    uint2 _S25 = thread_0.xy;

#line 163
    int2 _S26 = int2(_S25);
    int3 _S27 = int3(_S26, int(0));

#line 164
    float _S28 = (((&kernelContext_4)->cyGtaoSet_0->depth_1).read(vec<uint,2>(((_S27)).xy), uint(((_S27)).z)));
    if(_S28 <= 0.0)
    {
        (&kernelContext_4)->cyGtaoSet_0->output_0.write(float4(0.0, 0.0, 0.0, 1.0),uint2(_S26));
        return;
    }


    float _S29 = cyGtaoViewDepth_0((&kernelContext_4)->cyGtaoConstants_0->projection_2, _S28);

#line 172
    float3 _S30 = viewPositionAt_0(float2(_S26) + float2(0.5) , _S29, &kernelContext_4);

    float3 _S31 = normalize(- _S30);
    float3 _S32 = cyGtaoNormal_0((((&kernelContext_4)->cyGtaoSet_0->normals_0).read(vec<uint,2>(((_S27)).xy), uint(((_S27)).z))));

#line 175
    float3 _S33 = toView_0(_S32, &kernelContext_4);
    float3 _S34 = normalize(_S33);

    float _S35 = min((&kernelContext_4)->cyGtaoConstants_0->radius_0.x * (&kernelContext_4)->cyGtaoConstants_0->projection_2.y * 0.5 * cyGtaoConstants_1->extent_1.y / _S29, (&kernelContext_4)->cyGtaoConstants_0->control_0.z);


    if(_S35 < 1.0)
    {

        (&kernelContext_4)->cyGtaoSet_0->output_0.write(float4(_S32, 1.0),uint2(_S26));
        return;
    }



    uint _S36 = uint((&kernelContext_4)->cyGtaoConstants_0->control_0.x);
    uint _S37 = cyGtaoDither_0(_S25);

#line 191
    uint _S38 = 8U / _S36;

#line 191
    uint _S39 = _S37 % _S38;
    float _S40 = (float(cyGtaoDither_0(thread_0.yx)) + 0.5) / 16.0;



    float3 _S41 = float3(0.0) ;

#line 196
    uint slice_0 = 0U;

#line 196
    float visible_0 = 0.0;

#line 196
    float reference_0 = 0.0;

#line 196
    float3 bent_0 = _S41;
    for(;;)
    {

#line 197
        if(slice_0 < _S36)
        {
        }
        else
        {

#line 197
            break;
        }
        uint _S42 = 8U / _S36;

#line 199
        uint _S43 = _S39 + slice_0 * _S42;

        float2 _S44 = normalize(float2(kCyGtaoDirections_0[_S43]));
        float3 _S45 = float3(_S44.x, - _S44.y, 0.0);
        float3 _S46 = _S45 - _S31 * float3(dot(_S45, _S31)) ;
        float3 _S47 = normalize(cross(_S45, _S31));
        float3 _S48 = _S34 - _S47 * float3(dot(_S34, _S47)) ;
        float _S49 = sqrt(dot(_S48, _S48));
        if(_S49 <= 0.00009999999747379)
        {
            slice_0 = slice_0 + 1U;

#line 197
            continue;
        }

#line 211
        float _S50 = saturate(dot(_S48, _S31) / _S49);
        if((dot(_S46, _S48)) < 0.0)
        {

#line 212
            visibility_0 = -1.0;

#line 212
        }
        else
        {

#line 212
            visibility_0 = 1.0;

#line 212
        }

#line 212
        float _S51 = visibility_0 * acos(_S50);
        float _S52 = sin(_S51);


        float _S53 = cos(_S51 - 1.57079637050628662);

#line 216
        float _S54 = searchHorizon_0(_S26, _S30, _S31, kCyGtaoDirections_0[_S43], _S35, _S40, cos(_S51 + 1.57079637050628662), &kernelContext_4);

#line 216
        float _S55 = searchHorizon_0(_S26, _S30, _S31, - kCyGtaoDirections_0[_S43], _S35, _S40, _S53, &kernelContext_4);

#line 221
        float _S56 = _S51 + clamp(acos(clamp(_S54, -1.0, 1.0)) - _S51, -1.57079637050628662, 1.57079637050628662);

        float _S57 = _S51 + clamp(- acos(clamp(_S55, -1.0, 1.0)) - _S51, -1.57079637050628662, 1.57079637050628662);



        float _S58 = _S49 * (arcIntegral_0(_S57, _S51, _S50, _S52) + arcIntegral_0(_S56, _S51, _S50, _S52));
        float reference_1 = reference_0 + _S49 * (_S50 + _S51 * _S52);

        float _S59 = (_S57 + _S56) * 0.5;

        float3 bent_1 = bent_0 + (_S31 * float3(cos(_S59))  + normalize(_S46) * float3(sin(_S59)) ) * float3(_S58) ;

#line 232
        visible_0 = visible_0 + _S58;

#line 232
        reference_0 = reference_1;

#line 232
        bent_0 = bent_1;

#line 197
        slice_0 = slice_0 + 1U;

#line 197
    }

#line 235
    if(reference_0 > 0.0)
    {

#line 235
        visibility_0 = saturate(visible_0 / reference_0);

#line 235
    }
    else
    {

#line 235
        visibility_0 = 1.0;

#line 235
    }
    float visibility_1 = pow(visibility_0, (&kernelContext_4)->cyGtaoConstants_0->radius_0.w);
    float _S60 = sqrt(dot(bent_0, bent_0));
    if(_S60 > 9.99999997475242708e-07)
    {

#line 238
        float3 _S61 = toRelative_0(bent_0 / float3(_S60) , &kernelContext_4);

#line 238
        bent_0 = _S61;

#line 238
    }
    else
    {

#line 238
        bent_0 = _S32;

#line 238
    }
    (&kernelContext_4)->cyGtaoSet_0->output_0.write(float4(bent_0, visibility_1),uint2(_S26));
    return;
}

)cy_msl";

/// gtao_filter.metal, 10131 bytes.
inline constexpr char kGtaoFilterMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 15 "src/rendering/occlusion/shaders/gtao_common.slang"
float cyGtaoViewDepth_0(float4 projection_0, float depth_0)
{
    return projection_0.w / (depth_0 + projection_0.z);
}


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


#line 30 "src/rendering/occlusion/shaders/gtao_common.slang"
float3 cyGtaoNormal_0(float4 packed_0)
{
    return decodeOctahedral_0(packed_0.xy);
}


#line 81 "src/rendering/occlusion/shaders/gtao_filter.slang"
float stableVariance_0(float first_0, float second_0)
{
    float _S7 = max(0.0, second_0 - first_0 * first_0);

#line 83
    float _S8;

    if(_S7 <= (9.99999997475242708e-07 * max(1.0, abs(second_0))))
    {

#line 85
        _S8 = 0.0;

#line 85
    }
    else
    {

#line 85
        _S8 = _S7;

#line 85
    }

#line 85
    return _S8;
}


#line 45
struct CyGtaoFilterConstants_0
{
    float4 projection_1;
    float4 sigma_0;
    uint4 control_0;
};


#line 5163 "hlsl.meta.slang"
struct CyGtaoFilterSet_default_0
{
    depth2d<float, access::sample> depth_1;
    texture2d<float, access::sample> normals_0;
    texture2d<float, access::sample> raw_0;
    texture2d<float, access::sample> source_0;
    texture2d<float, access::read_write> output_0;
};


#line 5163
struct KernelContext_0
{
    CyGtaoFilterConstants_0 constant* cyGtaoFilterConstants_0;
    CyGtaoFilterSet_default_0 constant* cyGtaoFilterSet_0;
};


#line 89 "src/rendering/occlusion/shaders/gtao_filter.slang"
float rawVariance_0(int2 pixel_0, int2 size_0, KernelContext_0 thread* kernelContext_0)
{

#line 89
    int dy_0 = int(-1);

#line 89
    float sum_0 = 0.0;

#line 89
    float squares_0 = 0.0;

#line 89
    float count_0 = 0.0;

#line 94
    for(;;)
    {

#line 94
        if(dy_0 <= int(1))
        {
        }
        else
        {

#line 94
            break;
        }

#line 94
        int dx_0 = int(-1);

#line 94
        float sum_1 = sum_0;

#line 94
        float squares_1 = squares_0;

#line 94
        float count_1 = count_0;

        for(;;)
        {

#line 96
            if(dx_0 <= int(1))
            {
            }
            else
            {

#line 96
                break;
            }
            int2 _S9 = pixel_0 + int2(dx_0, dy_0);
            int _S10 = _S9.x;

#line 99
            bool _S11;

#line 99
            if(_S10 < int(0))
            {

#line 99
                _S11 = true;

#line 99
            }
            else
            {

#line 99
                _S11 = (_S9.y) < int(0);

#line 99
            }

#line 99
            bool _S12;

#line 99
            if(_S11)
            {

#line 99
                _S12 = true;

#line 99
            }
            else
            {

#line 99
                _S12 = _S10 >= (size_0.x);

#line 99
            }

#line 99
            bool _S13;

#line 99
            if(_S12)
            {

#line 99
                _S13 = true;

#line 99
            }
            else
            {

#line 99
                _S13 = (_S9.y) >= (size_0.y);

#line 99
            }

#line 99
            if(_S13)
            {
                dx_0 = dx_0 + int(1);

#line 96
                continue;
            }

#line 103
            int3 _S14 = int3(_S9, int(0));

#line 103
            float _S15 = ((kernelContext_0->cyGtaoFilterSet_0->raw_0).read(vec<uint,2>(((_S14)).xy), uint(((_S14)).z))).w;

            float squares_2 = squares_1 + _S15 * _S15;
            float count_2 = count_1 + 1.0;

#line 106
            sum_1 = sum_1 + _S15;

#line 106
            squares_1 = squares_2;

#line 106
            count_1 = count_2;

#line 96
            dx_0 = dx_0 + int(1);

#line 96
        }

#line 94
        dy_0 = dy_0 + int(1);

#line 94
        sum_0 = sum_1;

#line 94
        squares_0 = squares_1;

#line 94
        count_0 = count_1;

#line 94
    }

#line 112
    return stableVariance_0(sum_0 / count_0, squares_0 / count_0);
}


#line 74
float splineWeight_0(int offset_0)
{
    int _S16 = abs(offset_0);

#line 76
    float _S17;
    if(_S16 == int(0))
    {

#line 77
        _S17 = 0.375;

#line 77
    }
    else
    {

#line 77
        if(_S16 == int(1))
        {

#line 77
            _S17 = 0.25;

#line 77
        }
        else
        {

)cy_msl"
    R"cy_msl(#line 77
            if(_S16 == int(2))
            {

#line 77
                _S17 = 0.0625;

#line 77
            }
            else
            {

#line 77
                _S17 = 0.0;

#line 77
            }

#line 77
        }

#line 77
    }

#line 77
    return _S17;
}


#line 117
[[kernel]] void cyGtaoFilter(uint3 thread_0 [[thread_position_in_grid]], CyGtaoFilterConstants_0 constant* cyGtaoFilterConstants_1 [[buffer(1)]], CyGtaoFilterSet_default_0 constant* cyGtaoFilterSet_1 [[buffer(0)]])
{

#line 117
    thread KernelContext_0 kernelContext_1;

#line 117
    (&kernelContext_1)->cyGtaoFilterConstants_0 = cyGtaoFilterConstants_1;

#line 117
    (&kernelContext_1)->cyGtaoFilterSet_0 = cyGtaoFilterSet_1;

    int2 _S18 = int2(cyGtaoFilterConstants_1->control_0.zw);
    int2 _S19 = int2(thread_0.xy);
    int _S20 = _S18.x;

#line 121
    bool _S21;

#line 121
    if((_S19.x) >= _S20)
    {

#line 121
        _S21 = true;

#line 121
    }
    else
    {

#line 121
        _S21 = (_S19.y) >= (_S18.y);

#line 121
    }

#line 121
    if(_S21)
    {
        return;
    }
    int3 _S22 = int3(_S19, int(0));

#line 125
    float4 _S23 = (((&kernelContext_1)->cyGtaoFilterSet_0->source_0).read(vec<uint,2>(((_S22)).xy), uint(((_S22)).z)));
    float _S24 = (((&kernelContext_1)->cyGtaoFilterSet_0->depth_1).read(vec<uint,2>(((_S22)).xy), uint(((_S22)).z)));
    if(_S24 <= 0.0)
    {

        (&kernelContext_1)->cyGtaoFilterSet_0->output_0.write(_S23,uint2(_S19));
        return;
    }

    float4 _S25 = (&kernelContext_1)->cyGtaoFilterConstants_0->projection_1;
    float4 _S26 = (&kernelContext_1)->cyGtaoFilterConstants_0->sigma_0;
    float _S27 = cyGtaoViewDepth_0((&kernelContext_1)->cyGtaoFilterConstants_0->projection_1, _S24);
    float3 _S28 = cyGtaoNormal_0((((&kernelContext_1)->cyGtaoFilterSet_0->normals_0).read(vec<uint,2>(((_S22)).xy), uint(((_S22)).z))));

#line 137
    float _S29 = rawVariance_0(_S19, _S18, &kernelContext_1);
    float _S30 = sqrt(_S29);
    int _S31 = int(cyGtaoFilterConstants_1->control_0.x);
    int _S32 = int(cyGtaoFilterConstants_1->control_0.y);

    float4 _S33 = float4(0.0) ;

    int _S34 = - _S32;

#line 144
    int dy_1 = _S34;

#line 144
    float4 sum_2 = _S33;

#line 144
    float weightSum_0 = 0.0;

#line 144
    for(;;)
    {

#line 144
        if(dy_1 <= _S32)
        {
        }
        else
        {

#line 144
            break;
        }

#line 144
        int dx_1 = _S34;

#line 144
        float4 sum_3 = sum_2;

#line 144
        float weightSum_1 = weightSum_0;

        for(;;)
        {

#line 146
            if(dx_1 <= _S32)
            {
            }
            else
            {

#line 146
                break;
            }
            int2 _S35 = _S19 + int2(dx_1, dy_1) * int2(_S31) ;
            int _S36 = _S35.x;

#line 149
            if(_S36 < int(0))
            {

#line 149
                _S21 = true;

#line 149
            }
            else
            {

#line 149
                _S21 = (_S35.y) < int(0);

#line 149
            }

#line 149
            bool _S37;

#line 149
            if(_S21)
            {

#line 149
                _S37 = true;

#line 149
            }
            else
            {

#line 149
                _S37 = _S36 >= _S20;

#line 149
            }

#line 149
            bool _S38;

#line 149
            if(_S37)
            {

#line 149
                _S38 = true;

#line 149
            }
            else
            {

#line 149
                _S38 = (_S35.y) >= (_S18.y);

#line 149
            }

#line 149
            if(_S38)
            {
                dx_1 = dx_1 + int(1);

#line 146
                continue;
            }

#line 153
            int3 _S39 = int3(_S35, int(0));

#line 153
            float _S40 = (((&kernelContext_1)->cyGtaoFilterSet_0->depth_1).read(vec<uint,2>(((_S39)).xy), uint(((_S39)).z)));
            if(_S40 <= 0.0)
            {
                dx_1 = dx_1 + int(1);

#line 146
                continue;
            }

#line 163
            float4 _S41 = (((&kernelContext_1)->cyGtaoFilterSet_0->source_0).read(vec<uint,2>(((_S39)).xy), uint(((_S39)).z)));

            float _S42 = max(splineWeight_0(dx_1) * splineWeight_0(dy_1) * exp(- abs(cyGtaoViewDepth_0(_S25, _S40) - _S27) / (_S26.x * abs(_S27) + 9.99999997475242708e-07)) * pow(max(0.0, dot(_S28, cyGtaoNormal_0((((&kernelContext_1)->cyGtaoFilterSet_0->normals_0).read(vec<uint,2>(((_S39)).xy), uint(((_S39)).z)))))), _S26.y) * exp(- abs(_S41.w - _S23.w) / (_S26.z * _S30 + 9.99999997475242708e-07)), 0.0);
            if(_S42 <= 0.0)
            {
                dx_1 = dx_1 + int(1);

#line 146
                continue;
            }

#line 171
            float weightSum_2 = weightSum_1 + _S42;

#line 171
            sum_3 = sum_3 + _S41 * float4(_S42) ;

#line 171
            weightSum_1 = weightSum_2;

#line 146
            dx_1 = dx_1 + int(1);

#line 146
        }

#line 144
        dy_1 = dy_1 + int(1);

#line 144
        sum_2 = sum_3;

#line 144
        weightSum_0 = weightSum_1;

#line 144
    }

#line 174
    uint2 _S43 = uint2(_S19);

#line 174
    if(weightSum_0 > 0.0)
    {

#line 174
        sum_2 = sum_2 / float4(weightSum_0) ;

#line 174
    }
    else
    {

#line 174
        sum_2 = _S23;

#line 174
    }

#line 174
    (&kernelContext_1)->cyGtaoFilterSet_0->output_0.write(sum_2,_S43);
    return;
}

)cy_msl";

}  // namespace cy::rendering::occlusion

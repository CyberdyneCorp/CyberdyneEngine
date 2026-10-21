#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the temporal upscaler. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::upscale {

/// cyUpscaleReconstruct.metal, 29235 bytes.
inline constexpr char kUpscaleReconstructMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 67 "src/rendering/upscale/shaders/upscale_reconstruct.slang"
constant array<int2, int(4)> kCyUpscaleRidgePairs_0 = { int2(int(3), int(5)), int2(int(1), int(7)), int2(int(0), int(8)), int2(int(2), int(6)) };

#line 33 "src/rendering/upscale/shaders/upscale_common.slang"
uint2 cyUpscaleUnpack_0(uint packed_0)
{
    return uint2(packed_0 & 65535U, packed_0 >> 16U);
}


#line 11
struct CyUpscaleConstants_0
{
    float4 currentToPrevious0_0;
    float4 currentToPrevious1_0;
    float4 currentToPrevious2_0;
    float4 currentToPrevious3_0;
    uint4 extents_0;
    float4 sample_0;
    float4 history_0;
    float4 control_0;
};


#line 5163 "hlsl.meta.slang"
struct CyUpscaleReconstructSet_default_0
{
    texture2d<float, access::sample> color_0;
    depth2d<float, access::sample> depth_0;
    texture2d<float, access::sample> velocity_0;
    texture2d<float, access::sample> opaque_0;
    texture2d<float, access::sample> history_1;
    texture2d<float, access::sample> previousState_0;
    texture2d<float, access::read_write> reconstructed_0;
    texture2d<float, access::read_write> state_0;
};


#line 5163
struct KernelContext_0
{
    CyUpscaleConstants_0 constant* cyUpscale_0;
    CyUpscaleReconstructSet_default_0 constant* cyUpscaleReconstructSet_0;
};


#line 43 "src/rendering/upscale/shaders/upscale_common.slang"
uint2 cyUpscaleOutputExtent_0(KernelContext_0 thread* kernelContext_0)
{
    return cyUpscaleUnpack_0(kernelContext_0->cyUpscale_0->extents_0.y);
}


#line 43
uint2 cyUpscaleOutputExtent_1(KernelContext_0 thread* kernelContext_1)
{
    return cyUpscaleUnpack_0(kernelContext_1->cyUpscale_0->extents_0.y);
}


#line 38
uint2 cyUpscaleRenderExtent_0(KernelContext_0 thread* kernelContext_2)
{
    return cyUpscaleUnpack_0(kernelContext_2->cyUpscale_0->extents_0.x);
}


#line 48
uint cyUpscaleFlags_0(KernelContext_0 thread* kernelContext_3)
{
    return kernelContext_3->cyUpscale_0->extents_0.w;
}


#line 137
float2 cyUpscaleOutputUv_0(uint2 pixel_0, KernelContext_0 thread* kernelContext_4)
{
    float2 _S1 = float2(pixel_0) + float2(0.5) ;

#line 139
    uint2 _S2 = cyUpscaleOutputExtent_1(kernelContext_4);

#line 139
    return _S1 / float2(_S2);
}


#line 126
int2 cyUpscaleNearestTexel_0(float2 p_0, KernelContext_0 thread* kernelContext_5)
{
    return int2(floor(p_0 - kernelContext_5->cyUpscale_0->sample_0.xy));
}

int2 cyUpscaleClampTexel_0(int2 texel_0, uint2 extent_0)
{
    return clamp(texel_0, int2(int(0), int(0)), int2(extent_0) - int2(int(1), int(1)));
}


#line 66
float cyUpscaleLargest_0(float3 c_0)
{
    return max(c_0.x, max(c_0.y, c_0.z));
}


float3 cyUpscaleTonemap_0(float3 c_1, float exposure_0)
{
    float3 _S3 = c_1 * float3(exposure_0) ;
    return _S3 * float3((1.0 / (1.0 + max(cyUpscaleLargest_0(_S3), 0.0)))) ;
}


#line 84
float3 cyUpscaleToYCoCg_0(float3 c_2)
{
    float _S4 = c_2.x;

#line 86
    float _S5 = 0.5 * c_2.y;

#line 86
    float _S6 = c_2.z;

#line 86
    float _S7 = 0.25 * _S6;

#line 86
    return float3(0.25 * _S4 + _S5 + _S7, 0.5 * _S4 - 0.5 * _S6, -0.25 * _S4 + _S5 - _S7);
}


#line 119 "src/rendering/upscale/shaders/upscale_reconstruct.slang"
float cyUpscaleReactive_0(float3 finalColour_0, float3 opaque_1, float exposure_1)
{

    return saturate(cyUpscaleLargest_0(abs(cyUpscaleTonemap_0(finalColour_0, exposure_1) - cyUpscaleTonemap_0(opaque_1, exposure_1))) * 4.0);
}


struct CyUpscaleNeighbourhood_0
{
    float3 spatial_0;
    float3 evidence_0;
    float evidenceWeight_0;
    float3 mean_0;
    float3 deviation_0;
    float closestDepth_0;
    int2 closest_0;
    float nearDepth_0;
    float reactive_0;
    array<float2, int(9)> sampleUv_0;
    array<float, int(9)> luma_0;
    array<float3, int(9)> ycocg_0;
    float centreWeight_0;
};

CyUpscaleNeighbourhood_0 cyUpscaleGather_0(float2 p_1, float2 ratio_0, float exposure_2, KernelContext_0 thread* kernelContext_6)
{

#line 143
    int dx_0;

#line 143
    uint2 _S8 = cyUpscaleRenderExtent_0(kernelContext_6);

#line 143
    int2 _S9 = cyUpscaleNearestTexel_0(p_1, kernelContext_6);

#line 143
    uint _S10 = cyUpscaleFlags_0(kernelContext_6);



    bool _S11 = (_S10 & 4U) != 0U;
    thread CyUpscaleNeighbourhood_0 n_0;
    float3 _S12 = float3(0.0, 0.0, 0.0);

#line 149
    (&n_0)->spatial_0 = _S12;
    (&n_0)->evidence_0 = _S12;
    (&n_0)->evidenceWeight_0 = 0.0;
    (&n_0)->closestDepth_0 = -1.0;
    (&n_0)->closest_0 = int2(int(0), int(0));
    (&n_0)->reactive_0 = 0.0;
    (&n_0)->centreWeight_0 = 0.0;

#line 155
    int dy_0 = int(-1);

#line 155
    int tap_0 = int(0);

#line 155
    float spatialWeight_0 = 0.0;

#line 155
    float3 firstMoment_0 = _S12;

#line 155
    float3 secondMoment_0 = _S12;

#line 160
    for(;;)
    {

#line 160
        if(dy_0 <= int(1))
        {
        }
        else
        {

#line 160
            break;
        }

#line 160
        dx_0 = int(-1);

        for(;;)
        {

#line 162
            if(dx_0 <= int(1))
            {
            }
            else
            {

#line 162
                break;
            }
            int2 _S13 = cyUpscaleClampTexel_0(_S9 + int2(dx_0, dy_0), _S8);
            int3 _S14 = int3(_S13, int(0));

#line 165
            float3 _S15 = ((kernelContext_6->cyUpscaleReconstructSet_0->color_0).read(vec<uint,2>(((_S14)).xy), uint(((_S14)).z))).xyz;
            float2 _S16 = float2(_S13) + float2(0.5)  + kernelContext_6->cyUpscale_0->sample_0.xy;
            (&n_0)->sampleUv_0[tap_0] = _S16 / float2(_S8);
            float2 _S17 = p_1 - _S16;
            float2 _S18 = _S17 * ratio_0;
            float _S19 = exp(-2.28999996185302734 * dot(_S17, _S17));
            float _S20 = exp(-4.0 * dot(_S18, _S18));
            (&n_0)->spatial_0 = (&n_0)->spatial_0 + _S15 * float3(_S19) ;
            float spatialWeight_1 = spatialWeight_0 + _S19;
            (&n_0)->evidence_0 = (&n_0)->evidence_0 + _S15 * float3(_S20) ;
            (&n_0)->evidenceWeight_0 = (&n_0)->evidenceWeight_0 + _S20;
            float3 _S21 = cyUpscaleToYCoCg_0(cyUpscaleTonemap_0(_S15, exposure_2));
            float3 firstMoment_1 = firstMoment_0 + _S21;
            float3 secondMoment_1 = secondMoment_0 + _S21 * _S21;
            (&n_0)->luma_0[tap_0] = _S21.x;
            (&n_0)->ycocg_0[tap_0] = _S21;
            if(tap_0 == int(4))
            {
                (&n_0)->centreWeight_0 = _S20;

#line 181
            }



            float _S22 = ((kernelContext_6->cyUpscaleReconstructSet_0->depth_0).read(vec<uint,2>(((_S14)).xy), uint(((_S14)).z)));
            if(_S22 > ((&n_0)->closestDepth_0))
            {
                (&n_0)->closestDepth_0 = _S22;
                (&n_0)->closest_0 = _S13;

#line 186
            }

#line 191
            if(_S11)
            {

                (&n_0)->reactive_0 = max((&n_0)->reactive_0, cyUpscaleReactive_0(_S15, ((kernelContext_6->cyUpscaleReconstructSet_0->opaque_0).read(vec<uint,2>(((_S14)).xy), uint(((_S14)).z))).xyz, exposure_2));

#line 191
            }

#line 196
            int tap_1 = tap_0 + int(1);

#line 162
            dx_0 = dx_0 + int(1);

#line 162
            tap_0 = tap_1;

#line 162
            spatialWeight_0 = spatialWeight_1;

#line 162
            firstMoment_0 = firstMoment_1;

#line 162
            secondMoment_0 = secondMoment_1;

)cy_msl"
    R"cy_msl(#line 162
        }

#line 160
        dy_0 = dy_0 + int(1);

#line 160
    }

#line 199
    (&n_0)->nearDepth_0 = (&n_0)->closestDepth_0;

#line 199
    dx_0 = int(-2);
    for(;;)
    {

#line 200
        if(dx_0 <= int(2))
        {
        }
        else
        {

#line 200
            break;
        }

#line 200
        int rx_0 = int(-2);

        for(;;)
        {

#line 202
            if(rx_0 <= int(2))
            {
            }
            else
            {

#line 202
                break;
            }

#line 202
            bool _S23;

            if(rx_0 >= int(-1))
            {

#line 204
                _S23 = rx_0 <= int(1);

#line 204
            }
            else
            {

#line 204
                _S23 = false;

#line 204
            }

#line 204
            bool _S24;

#line 204
            if(_S23)
            {

#line 204
                _S24 = dx_0 >= int(-1);

#line 204
            }
            else
            {

#line 204
                _S24 = false;

#line 204
            }

#line 204
            bool _S25;

#line 204
            if(_S24)
            {

#line 204
                _S25 = dx_0 <= int(1);

#line 204
            }
            else
            {

#line 204
                _S25 = false;

#line 204
            }

#line 204
            if(_S25)
            {
                rx_0 = rx_0 + int(1);

#line 202
                continue;
            }

#line 209
            int3 _S26 = int3(cyUpscaleClampTexel_0(_S9 + int2(rx_0, dx_0), _S8), int(0));

#line 209
            (&n_0)->nearDepth_0 = max((&n_0)->nearDepth_0, ((kernelContext_6->cyUpscaleReconstructSet_0->depth_0).read(vec<uint,2>(((_S26)).xy), uint(((_S26)).z))));

#line 202
            rx_0 = rx_0 + int(1);

#line 202
        }

#line 200
        dx_0 = dx_0 + int(1);

#line 200
    }

#line 212
    (&n_0)->spatial_0 = (&n_0)->spatial_0 * float3((1.0 / spatialWeight_0)) ;

#line 212
    float3 _S27 = float3(0.1111111119389534) ;
    float3 _S28 = firstMoment_0 * _S27;

#line 213
    (&n_0)->mean_0 = _S28;

    (&n_0)->deviation_0 = sqrt(max(secondMoment_0 * _S27 - _S28 * _S28, _S12));
    return n_0;
}


#line 105 "src/rendering/upscale/shaders/upscale_common.slang"
float3 cyUpscalePreviousClip_0(float2 uv_0, float depth_1, KernelContext_0 thread* kernelContext_7)
{
    float4 _S29 = float4(uv_0.x * 2.0 - 1.0, 1.0 - uv_0.y * 2.0, depth_1, 1.0);
    float _S30 = dot(kernelContext_7->cyUpscale_0->currentToPrevious0_0, _S29);
    float _S31 = dot(kernelContext_7->cyUpscale_0->currentToPrevious1_0, _S29);
    float _S32 = dot(kernelContext_7->cyUpscale_0->currentToPrevious2_0, _S29);
    float _S33 = dot(kernelContext_7->cyUpscale_0->currentToPrevious3_0, _S29);

#line 111
    float _S34;
    if((abs(_S33)) > 9.99999993922529029e-09)
    {

#line 112
        _S34 = _S33;

#line 112
    }
    else
    {

#line 112
        _S34 = 9.99999993922529029e-09;

#line 112
    }
    return float3(_S30, _S31, _S32) / float3(_S34) ;
}


#line 105
float3 cyUpscalePreviousClip_1(float2 uv_1, float depth_2, KernelContext_0 thread* kernelContext_8)
{
    float4 _S35 = float4(uv_1.x * 2.0 - 1.0, 1.0 - uv_1.y * 2.0, depth_2, 1.0);
    float _S36 = dot(kernelContext_8->cyUpscale_0->currentToPrevious0_0, _S35);
    float _S37 = dot(kernelContext_8->cyUpscale_0->currentToPrevious1_0, _S35);
    float _S38 = dot(kernelContext_8->cyUpscale_0->currentToPrevious2_0, _S35);
    float _S39 = dot(kernelContext_8->cyUpscale_0->currentToPrevious3_0, _S35);

#line 111
    float _S40;
    if((abs(_S39)) > 9.99999993922529029e-09)
    {

#line 112
        _S40 = _S39;

#line 112
    }
    else
    {

#line 112
        _S40 = 9.99999993922529029e-09;

#line 112
    }
    return float3(_S36, _S37, _S38) / float3(_S40) ;
}



float2 cyUpscaleCameraMotion_0(float2 uv_2, float depth_3, KernelContext_0 thread* kernelContext_9)
{

#line 118
    float3 _S41 = cyUpscalePreviousClip_1(uv_2, depth_3, kernelContext_9);



    return float2(_S41.x * 0.5 + 0.5, 0.5 - _S41.y * 0.5) - uv_2;
}


#line 69 "src/rendering/upscale/shaders/upscale_reconstruct.slang"
void cyUpscaleCatmullRomWeights_0(float f_0, float4 thread* w_0)
{
    float _S42 = 0.5 * f_0;

#line 71
    (*w_0).x = f_0 * (-0.5 + f_0 * (1.0 - _S42));
    float _S43 = f_0 * f_0;

#line 72
    float _S44 = 1.5 * f_0;

#line 72
    (*w_0).y = 1.0 + _S43 * (-2.5 + _S44);
    (*w_0).z = f_0 * (0.5 + f_0 * (2.0 - _S44));
    (*w_0).w = _S43 * (-0.5 + _S42);
    return;
}

float4 cyUpscaleSampleHistory_0(float2 uv_3, KernelContext_0 thread* kernelContext_10)
{

#line 78
    uint2 _S45 = cyUpscaleOutputExtent_0(kernelContext_10);


    float2 _S46 = uv_3 * float2(_S45) - float2(0.5) ;
    float2 _S47 = floor(_S46);
    thread float4 wx_0;

    cyUpscaleCatmullRomWeights_0(_S46.x - _S47.x, &wx_0);

#line 84
    thread float4 wy_0;

    cyUpscaleCatmullRomWeights_0(_S46.y - _S47.y, &wy_0);
    int2 _S48 = int2(_S47) - int2(int(1), int(1));
    float4 _S49 = float4(0.0, 0.0, 0.0, 0.0);

#line 88
    int b_0 = int(0);

#line 88
    float4 sum_0 = _S49;
    for(;;)
    {

#line 89
        if(b_0 < int(4))
        {
        }
        else
        {

#line 89
            break;
        }

#line 89
        int a_0 = int(0);

        for(;;)
        {

#line 91
            if(a_0 < int(4))
            {
            }
            else
            {

#line 91
                break;
            }

            int3 _S50 = int3(cyUpscaleClampTexel_0(_S48 + int2(a_0, b_0), _S45), int(0));

)cy_msl"
    R"cy_msl(#line 94
            float4 sum_1 = sum_0 + ((kernelContext_10->cyUpscaleReconstructSet_0->history_1).read(vec<uint,2>(((_S50)).xy), uint(((_S50)).z))) * float4((wx_0[a_0] * wy_0[b_0])) ;

#line 91
            a_0 = a_0 + int(1);

#line 91
            sum_0 = sum_1;

#line 91
        }

#line 89
        b_0 = b_0 + int(1);

#line 89
    }

#line 97
    return sum_0;
}



float3 cyUpscalePreviousStateAt_0(float2 uv_4, KernelContext_0 thread* kernelContext_11)
{

#line 102
    uint2 _S51 = cyUpscaleOutputExtent_0(kernelContext_11);


    int2 _S52 = int2(floor(uv_4 * float2(_S51)));
    int3 _S53 = int3(cyUpscaleClampTexel_0(_S52, _S51), int(0));

#line 106
    float2 _S54 = ((kernelContext_11->cyUpscaleReconstructSet_0->previousState_0).read(vec<uint,2>(((_S53)).xy), uint(((_S53)).z)).xy);
    float _S55 = _S54.y;

#line 107
    float near_0 = _S55;

#line 107
    int dy_1 = int(-1);
    for(;;)
    {

#line 108
        if(dy_1 <= int(1))
        {
        }
        else
        {

#line 108
            break;
        }

#line 108
        int dx_1 = int(-1);

        for(;;)
        {

#line 110
            if(dx_1 <= int(1))
            {
            }
            else
            {

#line 110
                break;
            }

            int3 _S56 = int3(cyUpscaleClampTexel_0(_S52 + int2(dx_1, dy_1), _S51), int(0));

#line 113
            float _S57 = max(near_0, ((kernelContext_11->cyUpscaleReconstructSet_0->previousState_0).read(vec<uint,2>(((_S56)).xy), uint(((_S56)).z)).xy).y);

#line 110
            int dx_2 = dx_1 + int(1);

#line 110
            near_0 = _S57;

#line 110
            dx_1 = dx_2;

#line 110
        }

#line 108
        dy_1 = dy_1 + int(1);

#line 108
    }

#line 116
    return float3(_S54.x, _S55, near_0);
}


#line 96 "src/rendering/upscale/shaders/upscale_common.slang"
float cyUpscaleViewDepth_0(float depth_4, KernelContext_0 thread* kernelContext_12)
{
    float _S58 = depth_4 + kernelContext_12->cyUpscale_0->sample_0.z;

#line 98
    float _S59;
    if(_S58 > 0.0)
    {

#line 99
        _S59 = min(kernelContext_12->cyUpscale_0->sample_0.w / _S58, 1.00000001504746622e+30);

#line 99
    }
    else
    {

#line 99
        _S59 = 1.00000001504746622e+30;

#line 99
    }

#line 99
    return _S59;
}


#line 238 "src/rendering/upscale/shaders/upscale_reconstruct.slang"
float cyUpscaleUnexplainedFraction_0(const CyUpscaleNeighbourhood_0 thread* n_1, float2 motion_0, float exposure_3, KernelContext_0 thread* kernelContext_13)
{

#line 238
    uint2 _S60 = cyUpscaleOutputExtent_0(kernelContext_13);

#line 238
    int tap_2 = int(0);

#line 238
    float unexplained_0 = 0.0;



    for(;;)
    {

#line 242
        if(tap_2 < int(9))
        {
        }
        else
        {

#line 242
            break;
        }

        int2 _S61 = int2(floor((n_1->sampleUv_0[tap_2] + motion_0) * float2(_S60) - float2(0.5) ));

        float3 _S62 = float3(-1.0e+09, -1.0e+09, -1.0e+09);

#line 247
        float3 low_0 = float3(1.0e+09, 1.0e+09, 1.0e+09);

#line 247
        float3 high_0 = _S62;

#line 247
        int b_1 = int(0);
        for(;;)
        {

#line 248
            if(b_1 < int(2))
            {
            }
            else
            {

#line 248
                break;
            }

#line 248
            int a_1 = int(0);

            for(;;)
            {

#line 250
                if(a_1 < int(2))
                {
                }
                else
                {

#line 250
                    break;
                }

                int3 _S63 = int3(cyUpscaleClampTexel_0(_S61 + int2(a_1, b_1), _S60), int(0));

                float3 _S64 = cyUpscaleToYCoCg_0(cyUpscaleTonemap_0(max(((kernelContext_13->cyUpscaleReconstructSet_0->history_1).read(vec<uint,2>(((_S63)).xy), uint(((_S63)).z))).xyz, float3(0.0, 0.0, 0.0)), exposure_3));
                float3 _S65 = min(low_0, _S64);
                float3 _S66 = max(high_0, _S64);

#line 250
                int a_2 = a_1 + int(1);

#line 250
                low_0 = _S65;

#line 250
                high_0 = _S66;

#line 250
                a_1 = a_2;

#line 250
            }

#line 248
            b_1 = b_1 + int(1);

#line 248
        }

#line 248
        float3 _S67 = n_1->ycocg_0[tap_2];

#line 248
        float3 _S68 = float3((0.01999999955296516 + 0.10000000149011612 * max(high_0.x, 0.0))) ;

#line 248
        bool _S69;

#line 262
        if(any((n_1->ycocg_0[tap_2]) < (low_0 - _S68)))
        {

#line 262
            _S69 = true;

#line 262
        }
        else
        {

#line 262
            _S69 = any(_S67 > (high_0 + _S68));

#line 262
        }

#line 262
        float _S70;
        if(_S69)
        {

#line 263
            _S70 = 1.0;

#line 263
        }
        else
        {

#line 263
            _S70 = 0.0;

#line 263
        }

#line 263
        float unexplained_1 = unexplained_0 + _S70;

#line 242
        tap_2 = tap_2 + int(1);

#line 242
        unexplained_0 = unexplained_1;

#line 242
    }

#line 265
    return unexplained_0 * 0.1111111119389534;
}


#line 90 "src/rendering/upscale/shaders/upscale_common.slang"
float3 cyUpscaleFromYCoCg_0(float3 c_3)
{
    float _S71 = c_3.x;

#line 92
    float _S72 = c_3.y;

)cy_msl"
    R"cy_msl(#line 92
    float _S73 = c_3.z;

#line 92
    return float3(_S71 + _S72 - _S73, _S71 + _S73, _S71 - _S72 - _S73);
}


#line 78
float3 cyUpscaleUntonemap_0(float3 t_0, float exposure_4)
{

    return t_0 * float3((1.0 / (exposure_4 * max(1.0 - cyUpscaleLargest_0(t_0), 0.00009999999747379)))) ;
}


#line 269 "src/rendering/upscale/shaders/upscale_reconstruct.slang"
float3 cyUpscaleRectify_0(const CyUpscaleNeighbourhood_0 thread* n_2, float3 history_2, float gamma_0, float observed_0, float exposure_5)
{

    float3 _S74 = n_2->deviation_0 * float3(gamma_0) ;

#line 272
    float3 _S75 = n_2->mean_0;
    float3 _S76 = cyUpscaleToYCoCg_0(cyUpscaleTonemap_0(history_2, exposure_5)) - n_2->mean_0;
    float _S77 = max(abs(_S76.x) / max(_S74.x, 0.00000999999974738), max(abs(_S76.y) / max(_S74.y, 0.00000999999974738), abs(_S76.z) / max(_S74.z, 0.00000999999974738)));

#line 274
    float _S78;


    if(_S77 > 1.0)
    {

#line 277
        _S78 = observed_0 * (1.0 - 1.0 / _S77);

#line 277
    }
    else
    {

#line 277
        _S78 = 0.0;

#line 277
    }
    if(_S78 <= 0.0)
    {
        return history_2;
    }
    return cyUpscaleUntonemap_0(cyUpscaleFromYCoCg_0(_S75 + _S76 * float3((1.0 - _S78)) ), exposure_5);
}


#line 220
bool cyUpscaleThinFeature_0(const CyUpscaleNeighbourhood_0 thread* n_3)
{

#line 220
    float _S79 = n_3->luma_0[int(4)];

#line 220
    bool ridge_0 = false;

#line 220
    int index_0 = int(0);



    for(;;)
    {

#line 224
        if(index_0 < int(4))
        {
        }
        else
        {

#line 224
            break;
        }

#line 224
        float _S80 = n_3->luma_0[kCyUpscaleRidgePairs_0[index_0].x];

#line 224
        float _S81 = n_3->luma_0[kCyUpscaleRidgePairs_0[index_0].y];

#line 224
        bool _S82;



        if((_S79 - max(n_3->luma_0[kCyUpscaleRidgePairs_0[index_0].x], n_3->luma_0[kCyUpscaleRidgePairs_0[index_0].y])) > 0.05000000074505806)
        {

#line 228
            _S82 = true;

#line 228
        }
        else
        {

#line 228
            _S82 = (min(_S80, _S81) - _S79) > 0.05000000074505806;

#line 228
        }

#line 228
        if(_S82)
        {

#line 228
            ridge_0 = true;

#line 228
        }

#line 224
        index_0 = index_0 + int(1);

#line 224
    }

#line 234
    return ridge_0;
}


#line 287
[[kernel]] void cyUpscaleReconstruct(uint3 thread_0 [[thread_position_in_grid]], CyUpscaleConstants_0 constant* cyUpscale_1 [[buffer(1)]], CyUpscaleReconstructSet_default_0 constant* cyUpscaleReconstructSet_1 [[buffer(0)]])
{

#line 287
    thread KernelContext_0 kernelContext_14;

#line 287
    (&kernelContext_14)->cyUpscale_0 = cyUpscale_1;

#line 287
    (&kernelContext_14)->cyUpscaleReconstructSet_0 = cyUpscaleReconstructSet_1;

#line 287
    uint2 _S83 = cyUpscaleOutputExtent_0(&kernelContext_14);

#line 287
    bool disoccluded_0;


    if((thread_0.x) >= (_S83.x))
    {

#line 290
        disoccluded_0 = true;

#line 290
    }
    else
    {

#line 290
        disoccluded_0 = (thread_0.y) >= (_S83.y);

#line 290
    }

#line 290
    if(disoccluded_0)
    {
        return;
    }

#line 292
    uint2 _S84 = cyUpscaleRenderExtent_0(&kernelContext_14);


    float _S85 = (&kernelContext_14)->cyUpscale_0->control_0.z;

#line 295
    uint _S86 = cyUpscaleFlags_0(&kernelContext_14);

    float2 _S87 = float2(_S83);

#line 297
    float2 _S88 = float2(_S84);

#line 297
    float2 _S89 = _S87 / _S88;
    uint2 _S90 = thread_0.xy;

#line 298
    float2 _S91 = cyUpscaleOutputUv_0(_S90, &kernelContext_14);

#line 298
    CyUpscaleNeighbourhood_0 _S92 = cyUpscaleGather_0(_S91 * _S88, _S89, _S85, &kernelContext_14);



    if((_S86 & 2U) != 0U)
    {

#line 302
        disoccluded_0 = (_S92.closestDepth_0) > 0.0;

#line 302
    }
    else
    {

#line 302
        disoccluded_0 = false;

#line 302
    }

#line 302
    float2 _S93;
    if(disoccluded_0)
    {

#line 303
        int3 _S94 = int3(_S92.closest_0, int(0));

#line 303
        _S93 = (((&kernelContext_14)->cyUpscaleReconstructSet_0->velocity_0).read(vec<uint,2>(((_S94)).xy), uint(((_S94)).z)).xy);

#line 303
    }
    else
    {

#line 303
        float2 _S95 = cyUpscaleCameraMotion_0(_S91, _S92.closestDepth_0, &kernelContext_14);

#line 303
        _S93 = _S95;

#line 303
    }

    float2 _S96 = _S91 + _S93;

    float _S97 = saturate(length(_S93 * _S87) / (&kernelContext_14)->cyUpscale_0->history_0.z);
    float _S98 = mix((&kernelContext_14)->cyUpscale_0->history_0.x, (&kernelContext_14)->cyUpscale_0->history_0.y, _S97);

#line 313
    float3 _S99 = float3(0.0, 0.0, 0.0);
    if(all(_S96 >= float2(0.0, 0.0)))
    {

#line 314
        disoccluded_0 = all(_S96 <= float2(1.0, 1.0));

#line 314
    }
    else
    {

#line 314
        disoccluded_0 = false;

#line 314
    }
    if((_S86 & 1U) != 0U)
    {
    }
    else
    {

#line 315
        disoccluded_0 = false;

#line 315
    }

#line 315
    float keptDepth_0;

#line 315
    float weight_0;

#line 315
    float lock_0;

#line 315
    float3 historyLinear_0;

#line 315
    if(disoccluded_0)
    {

#line 315
        float4 _S100 = cyUpscaleSampleHistory_0(_S96, &kernelContext_14);

#line 315
        float3 _S101 = cyUpscalePreviousStateAt_0(_S96, &kernelContext_14);

)cy_msl"
    R"cy_msl(

        float _S102 = min(max(_S100.w, 0.0), _S98);


        if((_S86 & 8U) != 0U)
        {
            float _S103 = 1.0 - (&kernelContext_14)->cyUpscale_0->history_0.w;

#line 324
            float3 _S104 = cyUpscalePreviousClip_0(_S91, _S92.nearDepth_0, &kernelContext_14);

#line 324
            float3 _S105 = cyUpscalePreviousClip_0(_S91, _S92.closestDepth_0, &kernelContext_14);

#line 324
            float _S106 = cyUpscaleViewDepth_0(_S101.z, &kernelContext_14);

#line 324
            float _S107 = cyUpscaleViewDepth_0(_S104.z, &kernelContext_14);


            if(_S106 < (_S107 * _S103))
            {

#line 327
                disoccluded_0 = true;

#line 327
                keptDepth_0 = _S92.closestDepth_0;

#line 327
                weight_0 = 0.0;

#line 327
            }
            else
            {


                float _S108 = _S101.y;

#line 332
                float _S109 = cyUpscaleViewDepth_0(_S108, &kernelContext_14);

#line 332
                float _S110 = cyUpscaleViewDepth_0(_S105.z, &kernelContext_14);

#line 332
                if(_S109 < (_S110 * _S103))
                {

#line 332
                    keptDepth_0 = _S108;

#line 332
                }
                else
                {

#line 332
                    keptDepth_0 = _S92.closestDepth_0;

#line 332
                }

#line 332
                disoccluded_0 = false;

#line 332
                weight_0 = 1.0;

#line 327
            }

#line 322
        }
        else
        {

#line 322
            disoccluded_0 = false;

#line 322
            keptDepth_0 = _S92.closestDepth_0;

#line 322
            weight_0 = 1.0;

#line 322
        }

#line 337
        if((_S86 & 16U) != 0U)
        {

#line 337
            thread CyUpscaleNeighbourhood_0 _S111 = _S92;

#line 337
            float _S112 = cyUpscaleUnexplainedFraction_0(&_S111, _S93, _S85, &kernelContext_14);

#line 337
            weight_0 = min(weight_0, saturate(1.0 - (_S112 - 0.25) / 0.34999999403953552));

#line 337
        }

#line 343
        float trust_0 = weight_0 * (1.0 - _S92.reactive_0 * (&kernelContext_14)->cyUpscale_0->control_0.w);
        float weight_1 = _S102 * trust_0;

        if(disoccluded_0)
        {

#line 346
            disoccluded_0 = true;

#line 346
        }
        else
        {

#line 346
            disoccluded_0 = _S97 > 0.5;

#line 346
        }

#line 346
        if(disoccluded_0)
        {

#line 346
            lock_0 = -1.0;

#line 346
        }
        else
        {

            float _S113 = _S101.x;

#line 350
            if(_S113 < 0.0)
            {

#line 350
                lock_0 = min(_S113 + 0.25, 0.0);

#line 350
            }
            else
            {

                if(trust_0 < 0.5)
                {

#line 354
                    lock_0 = 0.0;

#line 354
                }
                else
                {

                    if((_S86 & 32U) != 0U)
                    {

#line 358
                        lock_0 = max(_S113 - 0.015625, 0.0);

#line 358
                    }
                    else
                    {

#line 358
                        lock_0 = 0.0;

#line 358
                    }

#line 354
                }

#line 350
            }

#line 346
        }

#line 362
        if((_S101.x) < 0.0)
        {

#line 362
            disoccluded_0 = true;

#line 362
        }
        else
        {

#line 362
            disoccluded_0 = lock_0 < 0.0;

#line 362
        }

        float _S114 = mix((&kernelContext_14)->cyUpscale_0->control_0.x, (&kernelContext_14)->cyUpscale_0->control_0.y, _S97);

        if(disoccluded_0)
        {

#line 366
            weight_0 = 1.0;

#line 366
        }
        else
        {

#line 366
            weight_0 = max(_S97, saturate(_S92.evidenceWeight_0)) * (1.0 - lock_0);

#line 366
        }

        float3 _S115 = max(_S100.xyz, _S99);

#line 368
        thread CyUpscaleNeighbourhood_0 _S116 = _S92;

#line 368
        float3 _S117 = cyUpscaleRectify_0(&_S116, _S115, _S114, weight_0, _S85);

#line 368
        historyLinear_0 = _S117;

#line 368
        weight_0 = weight_1;

#line 315
    }
    else
    {

#line 315
        lock_0 = 0.0;

#line 315
        historyLinear_0 = _S99;

#line 315
        keptDepth_0 = _S92.closestDepth_0;

#line 315
        weight_0 = 0.0;

#line 315
    }

#line 370
    if((_S86 & 32U) != 0U)
    {

#line 370
        disoccluded_0 = lock_0 >= 0.0;

#line 370
    }
    else
    {

#line 370
        disoccluded_0 = false;

#line 370
    }

#line 370
    if(disoccluded_0)
    {

#line 370
        disoccluded_0 = (_S92.centreWeight_0) >= 0.05000000074505806;

#line 370
    }
    else
    {

#line 370
        disoccluded_0 = false;

#line 370
    }

#line 370
    if(disoccluded_0)
    {

)cy_msl"
    R"cy_msl(#line 370
        thread CyUpscaleNeighbourhood_0 _S118 = _S92;

#line 370
        bool _S119 = cyUpscaleThinFeature_0(&_S118);

#line 370
        disoccluded_0 = _S119;

#line 370
    }
    else
    {

#line 370
        disoccluded_0 = false;

#line 370
    }
    if(disoccluded_0)
    {

#line 371
        disoccluded_0 = _S97 <= 0.5;

#line 371
    }
    else
    {

#line 371
        disoccluded_0 = false;

#line 371
    }

#line 370
    if(disoccluded_0)
    {

#line 370
        lock_0 = 1.0;

#line 370
    }

#line 370
    float4 result_0;

#line 377
    if(weight_0 <= 0.0)
    {

#line 377
        result_0 = float4(_S92.spatial_0, min(_S92.evidenceWeight_0, _S98));

#line 377
    }
    else
    {



        float _S120 = weight_0 + _S92.evidenceWeight_0;

#line 383
        result_0 = float4((historyLinear_0 * float3(weight_0)  + _S92.evidence_0) * float3((1.0 / _S120)) , min(_S120, _S98));

#line 377
    }

#line 386
    (&kernelContext_14)->cyUpscaleReconstructSet_0->reconstructed_0.write(result_0,_S90);
    (&kernelContext_14)->cyUpscaleReconstructSet_0->state_0.write(float4(lock_0, keptDepth_0, int(0), int(0)),_S90);
    return;
}

)cy_msl";

/// cyUpscaleSharpen.metal, 3903 bytes.
inline constexpr char kUpscaleSharpenMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 16 "src/rendering/upscale/shaders/upscale_sharpen.slang"
struct CySharpenConstants_0
{
    uint4 extent_0;
    float4 control_0;
};


#line 5163 "hlsl.meta.slang"
struct CyUpscaleSharpenSet_default_0
{
    texture2d<float, access::sample> reconstructed_0;
    texture2d<float, access::read_write> sharpened_0;
};


#line 5163
struct KernelContext_0
{
    CySharpenConstants_0 constant* cySharpen_0;
    CyUpscaleSharpenSet_default_0 constant* cyUpscaleSharpenSet_0;
};


#line 48 "src/rendering/upscale/shaders/upscale_sharpen.slang"
float3 cySharpenAt_0(int2 texel_0, KernelContext_0 thread* kernelContext_0)
{


    int3 _S1 = int3(clamp(texel_0, int2(int(0), int(0)), int2(kernelContext_0->cySharpen_0->extent_0.xy) - int2(int(1), int(1))), int(0));

#line 52
    return ((kernelContext_0->cyUpscaleSharpenSet_0->reconstructed_0).read(vec<uint,2>(((_S1)).xy), uint(((_S1)).z))).xyz;
}


#line 36
float3 cySharpenTonemap_0(float3 c_0, float exposure_0)
{
    float3 _S2 = c_0 * float3(exposure_0) ;
    return _S2 * float3((1.0 / (1.0 + max(max(_S2.x, max(_S2.y, _S2.z)), 0.0)))) ;
}


#line 55
float cySharpenLobe_0(float low_0, float high_0)
{


    return max(- (low_0 / max(4.0 * high_0, 9.99999997475242708e-07)), (1.0 - high_0) / min(4.0 * low_0 - 4.0, -9.99999997475242708e-07));
}


#line 42
float3 cySharpenUntonemap_0(float3 t_0, float exposure_1)
{

    return t_0 * float3((1.0 / (exposure_1 * max(1.0 - max(t_0.x, max(t_0.y, t_0.z)), 0.00009999999747379)))) ;
}


#line 64
[[kernel]] void cyUpscaleSharpen(uint3 thread_0 [[thread_position_in_grid]], CySharpenConstants_0 constant* cySharpen_1 [[buffer(1)]], CyUpscaleSharpenSet_default_0 constant* cyUpscaleSharpenSet_1 [[buffer(0)]])
{

#line 64
    thread KernelContext_0 kernelContext_1;

#line 64
    (&kernelContext_1)->cySharpen_0 = cySharpen_1;

#line 64
    (&kernelContext_1)->cyUpscaleSharpenSet_0 = cyUpscaleSharpenSet_1;

#line 64
    bool _S3;

    if((thread_0.x) >= (cySharpen_1->extent_0.x))
    {

#line 66
        _S3 = true;

#line 66
    }
    else
    {

#line 66
        _S3 = (thread_0.y) >= (cySharpen_1->extent_0.y);

#line 66
    }

#line 66
    if(_S3)
    {
        return;
    }
    float _S4 = (&kernelContext_1)->cySharpen_0->control_0.x;
    float _S5 = (&kernelContext_1)->cySharpen_0->control_0.y;
    uint2 _S6 = thread_0.xy;

#line 72
    int2 _S7 = int2(_S6);

#line 72
    float3 _S8 = cySharpenAt_0(_S7, &kernelContext_1);

#line 72
    float3 result_0;


    if(_S4 > 0.0)
    {

#line 75
        float3 _S9 = cySharpenAt_0(_S7 + int2(int(0), int(-1)), &kernelContext_1);

        float3 _S10 = cySharpenTonemap_0(_S9, _S5);

#line 77
        float3 _S11 = cySharpenAt_0(_S7 + int2(int(-1), int(0)), &kernelContext_1);
        float3 _S12 = cySharpenTonemap_0(_S11, _S5);
        float3 _S13 = cySharpenTonemap_0(_S8, _S5);

#line 79
        float3 _S14 = cySharpenAt_0(_S7 + int2(int(1), int(0)), &kernelContext_1);
        float3 _S15 = cySharpenTonemap_0(_S14, _S5);

#line 80
        float3 _S16 = cySharpenAt_0(_S7 + int2(int(0), int(1)), &kernelContext_1);
        float3 _S17 = cySharpenTonemap_0(_S16, _S5);
        float3 _S18 = min(min(min(_S10, _S12), min(_S13, _S15)), _S17);
        float3 _S19 = max(max(max(_S10, _S12), max(_S13, _S15)), _S17);


        float _S20 = max(-0.1875, min(max(cySharpenLobe_0(_S18.x, _S19.x), max(cySharpenLobe_0(_S18.y, _S19.y), cySharpenLobe_0(_S18.z, _S19.z))), 0.0)) * _S4;

#line 86
        result_0 = cySharpenUntonemap_0(max(((_S10 + _S12 + _S15 + _S17) * float3(_S20)  + _S13) * float3((1.0 / (4.0 * _S20 + 1.0))) , float3(0.0, 0.0, 0.0)), _S5);

#line 75
    }
    else
    {

#line 75
        result_0 = _S8;

#line 75
    }

#line 90
    (&kernelContext_1)->cyUpscaleSharpenSet_0->sharpened_0.write(float4(result_0, 1.0),_S6);
    return;
}

)cy_msl";

/// cyUpscaleCopy.metal, 1595 bytes.
inline constexpr char kUpscaleCopyMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 11 "src/rendering/upscale/shaders/upscale_copy.slang"
struct CyUpscaleCopyConstants_0
{
    uint4 extent_0;
};


#line 5163 "hlsl.meta.slang"
struct CyUpscaleCopySet_default_0
{
    texture2d<float, access::sample> source_0;
    texture2d<float, access::read_write> copy_0;
};


#line 5163
struct KernelContext_0
{
    CyUpscaleCopyConstants_0 constant* cyUpscaleCopyExtent_0;
    CyUpscaleCopySet_default_0 constant* cyUpscaleCopySet_0;
};


#line 31 "src/rendering/upscale/shaders/upscale_copy.slang"
[[kernel]] void cyUpscaleCopy(uint3 thread_0 [[thread_position_in_grid]], CyUpscaleCopyConstants_0 constant* cyUpscaleCopyExtent_1 [[buffer(1)]], CyUpscaleCopySet_default_0 constant* cyUpscaleCopySet_1 [[buffer(0)]])
{

#line 31
    thread KernelContext_0 kernelContext_0;

#line 31
    (&kernelContext_0)->cyUpscaleCopyExtent_0 = cyUpscaleCopyExtent_1;

#line 31
    (&kernelContext_0)->cyUpscaleCopySet_0 = cyUpscaleCopySet_1;

#line 31
    bool _S1;

    if((thread_0.x) >= (cyUpscaleCopyExtent_1->extent_0.x))
    {

#line 33
        _S1 = true;

#line 33
    }
    else
    {

#line 33
        _S1 = (thread_0.y) >= (cyUpscaleCopyExtent_1->extent_0.y);

#line 33
    }

#line 33
    if(_S1)
    {
        return;
    }
    uint2 _S2 = thread_0.xy;

#line 37
    int3 _S3 = int3(int2(_S2), int(0));

#line 37
    (&kernelContext_0)->cyUpscaleCopySet_0->copy_0.write((((&kernelContext_0)->cyUpscaleCopySet_0->source_0).read(vec<uint,2>(((_S3)).xy), uint(((_S3)).z))),_S2);
    return;
}

)cy_msl";

/// cyUpscaleResampleVertex.metal, 778 bytes.
inline constexpr char kUpscaleResampleVertexMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 31 "src/rendering/upscale/shaders/upscale_resample.slang"
struct cyUpscaleResampleVertex_Result_0
{
    float4 position_0 [[position]];
};


#line 31
struct CyUpscaleResampleVertex_0
{
    float4 position_1;
};


#line 31
[[vertex]] cyUpscaleResampleVertex_Result_0 cyUpscaleResampleVertex(uint vertexId_0 [[vertex_id]])
{

#line 40
    thread CyUpscaleResampleVertex_0 output_0;

    (&output_0)->position_1 = float4(float2(float((vertexId_0 << 1U) & 2U), float(vertexId_0 & 2U)) * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);

#line 42
    thread cyUpscaleResampleVertex_Result_0 _S1;

#line 42
    (&_S1)->position_0 = output_0.position_1;

#line 42
    return _S1;
}

)cy_msl";

/// cyUpscaleResampleFragment.metal, 4768 bytes.
inline constexpr char kUpscaleResampleFragmentMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 33 "src/rendering/upscale/shaders/upscale_common.slang"
uint2 cyUpscaleUnpack_0(uint packed_0)
{
    return uint2(packed_0 & 65535U, packed_0 >> 16U);
}


#line 11
struct CyUpscaleConstants_0
{
    float4 currentToPrevious0_0;
    float4 currentToPrevious1_0;
    float4 currentToPrevious2_0;
    float4 currentToPrevious3_0;
    uint4 extents_0;
    float4 sample_0;
    float4 history_0;
    float4 control_0;
};


#line 5319 "core.meta.slang"
struct CyUpscaleResampleSet_default_0
{
    depth2d<float, access::sample> depth_0;
    texture2d<float, access::sample> velocity_0;
};


#line 5319
struct KernelContext_0
{
    CyUpscaleConstants_0 constant* cyUpscale_0;
    CyUpscaleResampleSet_default_0 constant* cyUpscaleResampleSet_0;
};


#line 43 "src/rendering/upscale/shaders/upscale_common.slang"
uint2 cyUpscaleOutputExtent_0(KernelContext_0 thread* kernelContext_0)
{
    return cyUpscaleUnpack_0(kernelContext_0->cyUpscale_0->extents_0.y);
}


#line 137
float2 cyUpscaleOutputUv_0(uint2 pixel_0, KernelContext_0 thread* kernelContext_1)
{
    float2 _S1 = float2(pixel_0) + float2(0.5) ;

#line 139
    uint2 _S2 = cyUpscaleOutputExtent_0(kernelContext_1);

#line 139
    return _S1 / float2(_S2);
}


#line 38
uint2 cyUpscaleRenderExtent_0(KernelContext_0 thread* kernelContext_2)
{
    return cyUpscaleUnpack_0(kernelContext_2->cyUpscale_0->extents_0.x);
}


#line 126
int2 cyUpscaleNearestTexel_0(float2 p_0, KernelContext_0 thread* kernelContext_3)
{
    return int2(floor(p_0 - kernelContext_3->cyUpscale_0->sample_0.xy));
}

int2 cyUpscaleClampTexel_0(int2 texel_0, uint2 extent_0)
{
    return clamp(texel_0, int2(int(0), int(0)), int2(extent_0) - int2(int(1), int(1)));
}


#line 48
uint cyUpscaleFlags_0(KernelContext_0 thread* kernelContext_4)
{
    return kernelContext_4->cyUpscale_0->extents_0.w;
}


#line 105
float3 cyUpscalePreviousClip_0(float2 uv_0, float depth_1, KernelContext_0 thread* kernelContext_5)
{
    float4 _S3 = float4(uv_0.x * 2.0 - 1.0, 1.0 - uv_0.y * 2.0, depth_1, 1.0);
    float _S4 = dot(kernelContext_5->cyUpscale_0->currentToPrevious0_0, _S3);
    float _S5 = dot(kernelContext_5->cyUpscale_0->currentToPrevious1_0, _S3);
    float _S6 = dot(kernelContext_5->cyUpscale_0->currentToPrevious2_0, _S3);
    float _S7 = dot(kernelContext_5->cyUpscale_0->currentToPrevious3_0, _S3);

#line 111
    float _S8;
    if((abs(_S7)) > 9.99999993922529029e-09)
    {

#line 112
        _S8 = _S7;

#line 112
    }
    else
    {

#line 112
        _S8 = 9.99999993922529029e-09;

#line 112
    }
    return float3(_S4, _S5, _S6) / float3(_S8) ;
}



float2 cyUpscaleCameraMotion_0(float2 uv_1, float depth_2, KernelContext_0 thread* kernelContext_6)
{

#line 118
    float3 _S9 = cyUpscalePreviousClip_0(uv_1, depth_2, kernelContext_6);



    return float2(_S9.x * 0.5 + 0.5, 0.5 - _S9.y * 0.5) - uv_1;
}


#line 46 "src/rendering/upscale/shaders/upscale_resample.slang"
struct CyUpscaleResampleOutput_0
{
    float2 velocity_1 [[color(0)]];
    float depth_3 [[depth(any)]];
};


[[fragment]] CyUpscaleResampleOutput_0 cyUpscaleResampleFragment(float4 position_0 [[position]], CyUpscaleConstants_0 constant* cyUpscale_1 [[buffer(1)]], CyUpscaleResampleSet_default_0 constant* cyUpscaleResampleSet_1 [[buffer(0)]])
{

#line 53
    thread KernelContext_0 kernelContext_7;

#line 53
    (&kernelContext_7)->cyUpscale_0 = cyUpscale_1;

#line 53
    (&kernelContext_7)->cyUpscaleResampleSet_0 = cyUpscaleResampleSet_1;

#line 53
    float2 _S10 = cyUpscaleOutputUv_0(uint2(position_0.xy), &kernelContext_7);

#line 53
    uint2 _S11 = cyUpscaleRenderExtent_0(&kernelContext_7);

#line 53
    int2 _S12 = cyUpscaleNearestTexel_0(_S10 * float2(_S11), &kernelContext_7);

#line 59
    int3 _S13 = int3(cyUpscaleClampTexel_0(_S12, _S11), int(0));

#line 59
    float _S14 = (((&kernelContext_7)->cyUpscaleResampleSet_0->depth_0).read(vec<uint,2>(((_S13)).xy), uint(((_S13)).z)));

#line 59
    uint _S15 = cyUpscaleFlags_0(&kernelContext_7);
    bool _S16 = (_S15 & 2U) != 0U;
    thread CyUpscaleResampleOutput_0 output_0;
    (&output_0)->depth_3 = _S14;

#line 62
    bool _S17;
    if(_S16)
    {

#line 63
        _S17 = _S14 > 0.0;

#line 63
    }
    else
    {

#line 63
        _S17 = false;

#line 63
    }

#line 63
    float2 _S18;
    if(_S17)
    {

#line 64
        _S18 = (((&kernelContext_7)->cyUpscaleResampleSet_0->velocity_0).read(vec<uint,2>(((_S13)).xy), uint(((_S13)).z)).xy);

#line 64
    }
    else
    {

#line 64
        float2 _S19 = cyUpscaleCameraMotion_0(_S10, _S14, &kernelContext_7);

#line 64
        _S18 = _S19;

#line 64
    }

#line 63
    (&output_0)->velocity_1 = _S18;


    return output_0;
}

)cy_msl";

}  // namespace cy::rendering::upscale

#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the graded resolve and the metering dispatches. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::grading {

/// fullscreenVertex.metal, 936 bytes.
inline constexpr char kGradingVertexMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 49 "src/rendering/shaders/cy/fullscreen.slang"
struct fullscreenVertex_Result_0
{
    float4 position_0 [[position]];
    float2 uv_0 [[user(TEXCOORD)]];
};


#line 49
struct FullscreenVertex_0
{
    float4 position_1;
    float2 uv_1;
};


#line 453 "core"
[[vertex]] fullscreenVertex_Result_0 fullscreenVertex(uint vertexId_0 [[vertex_id]])
{

#line 72 "src/rendering/shaders/cy/fullscreen.slang"
    thread FullscreenVertex_0 output_0;
    float2 _S1 = float2(float((vertexId_0 << 1U) & 2U), float(vertexId_0 & 2U));

#line 73
    (&output_0)->uv_1 = _S1;


    (&output_0)->position_1 = float4(_S1 * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);

#line 76
    thread fullscreenVertex_Result_0 _S2;

#line 76
    (&_S2)->position_0 = output_0.position_1;

#line 76
    (&_S2)->uv_0 = output_0.uv_1;

#line 76
    return _S2;
}

)cy_msl";

/// cyGradedResolve.metal, 3971 bytes.
inline constexpr char kGradedResolveMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 58 "src/rendering/grading/shaders/graded_resolve.slang"
constant int fc_kGradedTonemapOperator_0 [[function_constant(0)]];
constant int kGradedTonemapOperator_0 = is_function_constant_defined(fc_kGradedTonemapOperator_0) ? fc_kGradedTonemapOperator_0 : int(1);

#line 28 "src/rendering/shaders/cy/tonemap.slang"
float3 applyExposure_0(float3 linear_0, float exposureStops_0)
{
    return linear_0 * float3(exp2(exposureStops_0)) ;
}


#line 8
float3 tonemapReinhard_0(float3 linear_1, float whitePoint_0)
{

#line 8
    float3 _S1 = float3(1.0) ;


    return linear_1 * (_S1 + linear_1 / float3((whitePoint_0 * whitePoint_0)) ) / (_S1 + linear_1);
}



float3 tonemapAcesApproximate_0(float3 linear_2)
{

#line 23
    return saturate(linear_2 * (float3(2.50999999046325684)  * linear_2 + float3(0.02999999932944775) ) / (linear_2 * (float3(2.43000006675720215)  * linear_2 + float3(0.5899999737739563) ) + float3(0.14000000059604645) ));
}


#line 42 "src/rendering/grading/shaders/graded_resolve.slang"
struct CyGradedResolveSet_default_0
{
    texture2d<float, access::sample> sceneColor_0;
    sampler linearClamp_0;
    texture3d<float, access::sample> lut_0;
    float4 device* exposureState_0;
};


#line 33
struct CyGradedResolvePush_0
{
    float4 exposure_0;
    float4 lut_1;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    CyGradedResolveSet_default_0 constant* cyGradedSet_0;
    CyGradedResolvePush_0 constant* cyGradedPush_0;
};


#line 71 "src/rendering/grading/shaders/graded_resolve.slang"
float3 cyGradedLookup_0(float3 displayLinear_0, float edge_0, KernelContext_0 thread* kernelContext_0)
{

#line 71
    float3 _S2 = float3(4096.0) ;

#line 71
    float3 _S3 = float3(1.0) ;

#line 71
    float3 _S4 = float3(12.00035190582275391) ;

#line 76
    return (exp2(saturate(((kernelContext_0->cyGradedSet_0->lut_0).sample((kernelContext_0->cyGradedSet_0->linearClamp_0), (log2(_S3 + saturate(displayLinear_0) * _S2) / _S4 * float3(((edge_0 - 1.0) / edge_0))  + float3((0.5 / edge_0)) ), level((0.0)))).xyz) * _S4) - _S3) / _S2;
}


#line 76
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 76
struct pixelInput_0
{
    float2 uv_0 [[user(TEXCOORD)]];
};


#line 80
[[fragment]] pixelOutput_0 cyGradedResolve(pixelInput_0 _S5 [[stage_in]], float4 position_0 [[position]], CyGradedResolveSet_default_0 constant* cyGradedSet_1 [[buffer(0)]], CyGradedResolvePush_0 constant* cyGradedPush_1 [[buffer(1)]])
{

#line 80
    thread KernelContext_0 kernelContext_1;

#line 80
    (&kernelContext_1)->cyGradedSet_0 = cyGradedSet_1;

#line 80
    (&kernelContext_1)->cyGradedPush_0 = cyGradedPush_1;

    float3 _S6 = ((cyGradedSet_1->sceneColor_0).sample((cyGradedSet_1->linearClamp_0), (_S5.uv_0))).xyz;
    float _S7 = cyGradedPush_1->exposure_0.x;

#line 83
    float stops_0;
    if((cyGradedPush_1->exposure_0.y) > 0.5)
    {

#line 84
        stops_0 = - (&kernelContext_1)->cyGradedSet_0->exposureState_0[int(0)].x - cyGradedPush_1->exposure_0.z;

#line 84
    }
    else
    {

#line 84
        stops_0 = _S7;

#line 84
    }



    float3 _S8 = applyExposure_0(_S6, stops_0);

#line 88
    float3 mapped_0;


    if(kGradedTonemapOperator_0 == int(1))
    {

#line 91
        mapped_0 = tonemapReinhard_0(_S8, 4.0);

#line 91
    }
    else
    {

        if(kGradedTonemapOperator_0 == int(2))
        {

#line 95
            mapped_0 = tonemapAcesApproximate_0(_S8);

#line 95
        }
        else
        {

#line 95
            mapped_0 = _S8;

#line 95
        }

#line 91
    }

#line 99
    if(((&kernelContext_1)->cyGradedPush_0->lut_1.x) > 0.5)
    {

#line 99
        float3 _S9 = cyGradedLookup_0(mapped_0, (&kernelContext_1)->cyGradedPush_0->lut_1.y, &kernelContext_1);

#line 99
        mapped_0 = _S9;

#line 99
    }

#line 99
    pixelOutput_0 _S10 = { float4(mapped_0, 1.0) };



    return _S10;
}

)cy_msl";

/// cyExposureClear.metal, 567 bytes.
inline constexpr char kExposureClearMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 23 "src/rendering/grading/shaders/exposure_clear.slang"
struct CyExposureClearSet_default_0
{
    uint device* histogram_0;
};


#line 21
[[kernel]] void cyExposureClear(uint3 sv_groupthreadid_0 [[thread_position_in_threadgroup]], CyExposureClearSet_default_0 constant* cyExposureClearSet_0 [[buffer(0)]])
{
    *(cyExposureClearSet_0->histogram_0+((sv_groupthreadid_0[int(2)] + sv_groupthreadid_0[int(1)]) * 256U + sv_groupthreadid_0[int(0)])) = 0U;
    return;
}

)cy_msl";

/// cyExposureHistogram.metal, 3261 bytes.
inline constexpr char kExposureHistogramMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 31 "src/rendering/grading/shaders/exposure_common.slang"
float cyExposureEv100_0(float luminance_0)
{
    return log2(max(luminance_0, 9.99999997475242708e-07) * 100.0 / 12.5);
}


uint cyExposureBin_0(float luminance_1, float minEv_0, float maxEv_0)
{



    return uint(clamp((cyExposureEv100_0(luminance_1) - minEv_0) / max(maxEv_0 - minEv_0, 9.99999997475242708e-07) * 256.0, 0.0, 255.0));
}


#line 11
struct CyExposureConstants_0
{
    float4 histogram_0;
    float4 limits_0;
    float4 curveEv_0;
    float4 curveCompensation_0;
    float4 frame_0;
    uint4 extent_0;
};


#line 40 "src/rendering/grading/shaders/exposure_histogram.slang"
struct CyExposureHistogramSet_default_0
{
    texture2d<float, access::sample> source_0;
    uint device* histogram_1;
};


#line 55
struct KernelContext_0
{
    CyExposureConstants_0 constant* cyExposureConstants_0;
    CyExposureHistogramSet_default_0 constant* cyExposureHistogramSet_0;
    array<uint, int(256)> threadgroup* cyExposureLocalBins_0;
};


#line 53
[[kernel]] void cyExposureHistogram(uint3 sv_groupthreadid_0 [[thread_position_in_threadgroup]], uint3 pixel_0 [[thread_position_in_grid]], CyExposureConstants_0 constant* cyExposureConstants_1 [[buffer(1)]], CyExposureHistogramSet_default_0 constant* cyExposureHistogramSet_1 [[buffer(0)]])
{

#line 53
    thread KernelContext_0 kernelContext_0;

#line 53
    (&kernelContext_0)->cyExposureConstants_0 = cyExposureConstants_1;

#line 53
    (&kernelContext_0)->cyExposureHistogramSet_0 = cyExposureHistogramSet_1;

#line 53
    threadgroup array<uint, int(256)> cyExposureLocalBins_1;

#line 53
    (&kernelContext_0)->cyExposureLocalBins_0 = &cyExposureLocalBins_1;

#line 53
    uint sv_groupindex_0 = (sv_groupthreadid_0[int(2)] * 16U + sv_groupthreadid_0[int(1)]) * 16U + sv_groupthreadid_0[int(0)];

    cyExposureLocalBins_1[sv_groupindex_0] = 0U;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    uint2 _S1 = cyExposureConstants_1->extent_0.xy;

#line 58
    bool _S2;
    if((pixel_0.x) < (_S1.x))
    {

#line 59
        _S2 = (pixel_0.y) < (_S1.y);

#line 59
    }
    else
    {

#line 59
        _S2 = false;

#line 59
    }

#line 59
    if(_S2)
    {
        int3 _S3 = int3(int2(pixel_0.xy), int(0));



        uint _S4 = atomic_fetch_add_explicit(((atomic_uint threadgroup*)(&(*(&kernelContext_0)->cyExposureLocalBins_0)[cyExposureBin_0(dot((((&kernelContext_0)->cyExposureHistogramSet_0->source_0).read(vec<uint,2>(((_S3)).xy), uint(((_S3)).z))).xyz, float3(0.2125999927520752, 0.71520000696182251, 0.07220000028610229)), (&kernelContext_0)->cyExposureConstants_0->histogram_0.x, (&kernelContext_0)->cyExposureConstants_0->histogram_0.y)])), 1U, memory_order_relaxed);

#line 59
    }

#line 67
    threadgroup_barrier(mem_flags::mem_threadgroup);

    uint _S5 = (*(&kernelContext_0)->cyExposureLocalBins_0)[sv_groupindex_0];
    if(((*(&kernelContext_0)->cyExposureLocalBins_0)[sv_groupindex_0]) != 0U)
    {
        uint _S6 = atomic_fetch_add_explicit(((atomic_uint device*)((&kernelContext_0)->cyExposureHistogramSet_0->histogram_1+sv_groupindex_0)), _S5, memory_order_relaxed);

#line 70
    }



    return;
}

)cy_msl";

/// cyExposureAdapt.metal, 5674 bytes.
inline constexpr char kExposureAdaptMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 11 "src/rendering/grading/shaders/exposure_common.slang"
struct CyExposureConstants_0
{
    float4 histogram_0;
    float4 limits_0;
    float4 curveEv_0;
    float4 curveCompensation_0;
    float4 frame_0;
    uint4 extent_0;
};


#line 94 "src/rendering/grading/shaders/exposure_adapt.slang"
struct CyExposureAdaptSet_default_0
{
    uint device* histogram_1;
    float4 device* state_0;
};


#line 94
struct KernelContext_0
{
    CyExposureConstants_0 constant* cyExposureConstants_0;
    CyExposureAdaptSet_default_0 constant* cyExposureAdaptSet_0;
};


#line 48
bool meteredMean_0(float thread* metered_0, KernelContext_0 thread* kernelContext_0)
{
    *metered_0 = 0.0;

#line 50
    uint bin_0 = 0U;

#line 50
    float total_0 = 0.0;

    for(;;)
    {

#line 52
        if(bin_0 < 256U)
        {
        }
        else
        {

#line 52
            break;
        }
        float total_1 = total_0 + float(kernelContext_0->cyExposureAdaptSet_0->histogram_1[bin_0]);

#line 52
        bin_0 = bin_0 + 1U;

#line 52
        total_0 = total_1;

#line 52
    }



    if(total_0 <= 0.0)
    {
        return false;
    }
    float _S1 = clamp(kernelContext_0->cyExposureConstants_0->histogram_0.z, 0.0, 1.0);

    float _S2 = total_0 * _S1;
    float _S3 = total_0 * clamp(kernelContext_0->cyExposureConstants_0->histogram_0.w, _S1, 1.0);
    float _S4 = kernelContext_0->cyExposureConstants_0->histogram_0.x;
    float _S5 = (kernelContext_0->cyExposureConstants_0->histogram_0.y - _S4) / 256.0;

#line 65
    float seen_0 = 0.0;

#line 65
    bin_0 = 0U;

#line 65
    float weighted_0 = 0.0;

#line 65
    float weight_0 = 0.0;

#line 72
    for(;;)
    {

#line 72
        if(bin_0 < 256U)
        {
        }
        else
        {

#line 72
            break;
        }

        float _S6 = seen_0 + float(kernelContext_0->cyExposureAdaptSet_0->histogram_1[bin_0]);

        float _S7 = min(_S6, _S3) - max(seen_0, _S2);
        if(_S7 > 0.0)
        {

            float weight_1 = weight_0 + _S7;

#line 81
            weighted_0 = weighted_0 + _S7 * (_S4 + _S5 * (float(bin_0) + 0.5));

#line 81
            weight_0 = weight_1;

#line 78
        }

#line 72
        uint bin_1 = bin_0 + 1U;

#line 72
        seen_0 = _S6;

#line 72
        bin_0 = bin_1;

#line 72
    }

#line 84
    if(weight_0 > 0.0)
    {

#line 84
        total_0 = weighted_0 / weight_0;

#line 84
    }
    else
    {

#line 84
        total_0 = _S4;

#line 84
    }

#line 84
    *metered_0 = total_0;
    return true;
}


#line 27
float compensationAt_0(float metered_1, KernelContext_0 thread* kernelContext_1)
{
    float4 _S8 = kernelContext_1->cyExposureConstants_0->curveEv_0;
    float4 _S9 = kernelContext_1->cyExposureConstants_0->curveCompensation_0;
    if(metered_1 <= (kernelContext_1->cyExposureConstants_0->curveEv_0.x))
    {
        return _S9.x;
    }

#line 33
    uint index_0 = 1U;

    for(;;)
    {

#line 35
        if(index_0 < 4U)
        {
        }
        else
        {

#line 35
            break;
        }

#line 35
        uint _S10 = index_0;

        if(metered_1 <= (_S8[index_0]))
        {
            uint _S11 = index_0 - 1U;

#line 39
            float _S12 = _S8[_S10] - _S8[_S11];

#line 39
            float _S13;
            if(_S12 > 9.99999997475242708e-07)
            {

#line 40
                _S13 = (metered_1 - _S8[_S11]) / _S12;

#line 40
            }
            else
            {

#line 40
                _S13 = 0.0;

#line 40
            }
            return mix(_S9[_S11], _S9[index_0], _S13);
        }

#line 35
        index_0 = index_0 + 1U;

#line 35
    }

#line 44
    return _S9.w;
}


#line 90
[[kernel]] void cyExposureAdapt(CyExposureConstants_0 constant* cyExposureConstants_1 [[buffer(1)]], CyExposureAdaptSet_default_0 constant* cyExposureAdaptSet_1 [[buffer(0)]])
{

#line 90
    thread KernelContext_0 kernelContext_2;

#line 90
    (&kernelContext_2)->cyExposureConstants_0 = cyExposureConstants_1;

#line 90
    (&kernelContext_2)->cyExposureAdaptSet_0 = cyExposureAdaptSet_1;

    float4 _S14 = cyExposureConstants_1->limits_0;
    float4 _S15 = cyExposureConstants_1->frame_0;
    float4 _S16 = *(cyExposureAdaptSet_1->state_0+int(0));
    bool _S17 = (cyExposureConstants_1->frame_0.y) > 0.5;

#line 95
    float adapted_0;

#line 95
    if(_S17)
    {

#line 95
        adapted_0 = _S15.z;

#line 95
    }
    else
    {

#line 95
        adapted_0 = _S16.x;

#line 95
    }

#line 95
    float _S18;
    if(_S17)
    {

#line 96
        _S18 = 0.0;

#line 96
    }
    else
    {

#line 96
        _S18 = _S16.w;

#line 96
    }

    float _S19 = _S14.x;

#line 98
    thread float metered_2 = _S19;

#line 98
    bool _S20 = meteredMean_0(&metered_2, &kernelContext_2);

#line 98
    float target_0;

    if(_S20)
    {

)cy_msl"
    R"cy_msl(#line 100
        float _S21 = compensationAt_0(metered_2, &kernelContext_2);

#line 100
        target_0 = clamp(metered_2 + _S21, _S19, _S14.y);

#line 100
    }
    else
    {

#line 100
        target_0 = _S19;

#line 100
    }

#line 108
    float _S22 = _S15.x;

#line 108
    if(_S22 > 0.0)
    {

#line 108
        float _S23;

        if(target_0 > adapted_0)
        {

#line 110
            _S23 = _S14.z;

#line 110
        }
        else
        {

#line 110
            _S23 = _S14.w;

#line 110
        }

#line 110
        adapted_0 = mix(adapted_0, target_0, clamp(1.0 - exp(- max(_S23, 0.0) * _S22), 0.0, 1.0));

#line 108
    }

#line 114
    *(cyExposureAdaptSet_1->state_0+int(0)) = float4(adapted_0, target_0, metered_2, _S18 + 1.0);
    return;
}

)cy_msl";

}  // namespace cy::rendering::grading

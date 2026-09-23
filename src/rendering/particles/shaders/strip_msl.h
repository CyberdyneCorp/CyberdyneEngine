#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the particle renderer. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::particles {

/// cyStripVertex.metal, 4945 bytes.
inline constexpr char kStripVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 69 "src/rendering/shaders/cy/strip.slang"
constant array<float2, int(6)> kCorners_0 = { float2(0.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0), float2(0.0, -1.0), float2(1.0, 1.0), float2(0.0, 1.0) };

#line 43
struct CyStripVertex_0
{
    float4 positionAndWidth_0;
    float4 color_0;
    float4 alongAndStrip_0;
};


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


#line 124 "src/rendering/shaders/cy/frame.slang"
struct CyFrameData_0
{
    float4 relativeToClipRow0_0;
    float4 relativeToClipRow1_0;
    float4 relativeToClipRow2_0;
    float4 relativeToClipRow3_0;
    float4 previousRelativeToClipRow0_0;
    float4 previousRelativeToClipRow1_0;
    float4 previousRelativeToClipRow2_0;
    float4 previousRelativeToClipRow3_0;
    float4 relativeToViewRow0_0;
    float4 relativeToViewRow1_0;
    float4 relativeToViewRow2_0;
    float4 relativeToViewRow3_0;
    float4 ambientAndOcclusion_0;
    float4 extentAndInverse_0;
    ClusterGrid_0 clusterGrid_0;
    uint4 counts_0;
    uint4 materialOffsets_0;
    float4 temporalFeedback_0;
    float4 temporalJitter_0;
    uint4 materialTextures_0;
};


#line 124
struct KernelContext_0
{
    CyStripVertex_0 device* cyStrips_0;
    CyFrameData_0 constant* cyFrame_0;
};


#line 239
float4 transformToClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S1 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrame_0->relativeToClipRow0_0, _S1), dot(kernelContext_0->cyFrame_0->relativeToClipRow1_0, _S1), dot(kernelContext_0->cyFrame_0->relativeToClipRow2_0, _S1), dot(kernelContext_0->cyFrame_0->relativeToClipRow3_0, _S1));
}


#line 77 "src/rendering/shaders/cy/strip.slang"
float3 sideAt_0(float3 position_0, float3 tangent_0)
{
    float3 _S2 = cross(tangent_0, - position_0);
    float _S3 = dot(_S2, _S2);

#line 80
    float3 _S4;
    if(_S3 > 9.999999960041972e-13)
    {

#line 81
        _S4 = _S2 * float3(rsqrt(_S3)) ;

#line 81
    }
    else
    {

#line 81
        _S4 = float3(0.0, 0.0, 0.0);

#line 81
    }

#line 81
    return _S4;
}


#line 81
struct cyStripVertex_Result_0
{
    float4 position_1 [[position]];
    float2 coordinates_0 [[user(TEXCOORD)]];
    float4 color_1 [[user(TEXCOORD_1)]];
};


#line 59
struct CyStripOutput_0
{
    float4 position_2;
    float2 coordinates_1;
    float4 color_2;
};


#line 59
[[vertex]] cyStripVertex_Result_0 cyStripVertex(uint vertexId_0 [[vertex_id]], CyStripVertex_0 device* cyStrips_1 [[buffer(0)]], CyFrameData_0 constant* cyFrame_1 [[buffer(1)]])
{

#line 59
    CyStripOutput_0 _S5;

#line 59
    thread KernelContext_0 kernelContext_1;

#line 59
    (&kernelContext_1)->cyStrips_0 = cyStrips_1;

#line 59
    (&kernelContext_1)->cyFrame_0 = cyFrame_1;

#line 59
    for(;;)
    {

#line 89
        uint _S6 = vertexId_0 / 6U;
        uint _S7 = vertexId_0 % 6U;
        CyStripVertex_0 _S8 = (&kernelContext_1)->cyStrips_0[_S6];
        CyStripVertex_0 _S9 = (&kernelContext_1)->cyStrips_0[_S6 + 1U];

#line 92
        CyStripVertex_0 _S10;

        if((kCorners_0[_S7].x) > 0.5)
        {

#line 94
            _S10 = _S9;

#line 94
        }
        else
        {

#line 94
            _S10 = _S8;

#line 94
        }

#line 94
        CyStripVertex_0 _S11 = _S10;
        float3 _S12 = _S10.positionAndWidth_0.xyz;

        thread CyStripOutput_0 output_0;
        float _S13 = kCorners_0[_S7].y;

#line 98
        (&output_0)->coordinates_1 = float2(_S10.alongAndStrip_0.x, _S13);
        (&output_0)->color_2 = _S10.color_0;

#line 104
        float3 _S14 = _S8.positionAndWidth_0.xyz;

#line 104
        float3 _S15 = _S9.positionAndWidth_0.xyz - _S14;

#line 104
        bool _S16;
        if(!((as_type<uint>((_S8.alongAndStrip_0.y))) == (as_type<uint>((_S9.alongAndStrip_0.y)))))
        {

#line 105
            _S16 = true;

#line 105
        }
        else
        {

#line 105
            _S16 = (dot(_S15, _S15)) <= 9.999999960041972e-13;

#line 105
        }

#line 105
        if(_S16)
        {

#line 105
            float4 _S17 = transformToClip_0(_S14, &kernelContext_1);

            (&output_0)->position_2 = _S17;

#line 107
            _S5 = output_0;
            break;
        }

#line 108
        float4 _S18 = transformToClip_0(_S12 + sideAt_0(_S12, _S15) * float3((_S13 * _S11.positionAndWidth_0.w)) , &kernelContext_1);



        (&output_0)->position_2 = _S18;

#line 112
        _S5 = output_0;
        break;
    }

#line 113
    thread cyStripVertex_Result_0 _S19;

#line 113
    (&_S19)->position_1 = _S5.position_2;

#line 113
    (&_S19)->coordinates_0 = _S5.coordinates_1;

#line 113
    (&_S19)->color_1 = _S5.color_2;

#line 113
    return _S19;
}

)cy_msl";

/// cyStripFragment.metal, 757 bytes.
inline constexpr char kStripFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 30 "src/rendering/shaders/cy/material.slang"
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 20 "src/rendering/shaders/cy/light.slang"
struct pixelInput_0
{
    float2 coordinates_0 [[user(TEXCOORD)]];
    float4 color_0 [[user(TEXCOORD_1)]];
};


#line 117 "src/rendering/shaders/cy/strip.slang"
[[fragment]] pixelOutput_0 cyStripFragment(pixelInput_0 _S1 [[stage_in]], float4 position_0 [[position]])
{


    float _S2 = _S1.coordinates_0.y;

#line 121
    pixelOutput_0 _S3 = { float4(_S1.color_0.xyz * float3((_S1.color_0.w * saturate(1.0 - _S2 * _S2) * saturate(1.0 - _S1.coordinates_0.x))) , 0.0) };

#line 130
    return _S3;
}

)cy_msl";

}  // namespace cy::rendering::particles

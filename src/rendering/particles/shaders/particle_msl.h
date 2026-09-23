#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the particle renderer. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::particles {

/// cyParticleVertex.metal, 3338 bytes.
inline constexpr char kParticleVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 63 "src/rendering/shaders/cy/particle.slang"
constant array<float2, int(6)> kCorners_0 = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0), float2(-1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0) };

#line 42
struct CyParticle_0
{
    float4 positionAndSize_0;
    float4 color_0;
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
    CyParticle_0 device* cyParticles_0;
    CyFrameData_0 constant* cyFrame_0;
};


#line 239
float4 transformToClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S1 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrame_0->relativeToClipRow0_0, _S1), dot(kernelContext_0->cyFrame_0->relativeToClipRow1_0, _S1), dot(kernelContext_0->cyFrame_0->relativeToClipRow2_0, _S1), dot(kernelContext_0->cyFrame_0->relativeToClipRow3_0, _S1));
}


#line 242
struct cyParticleVertex_Result_0
{
    float4 position_0 [[position]];
    float2 corner_0 [[user(TEXCOORD)]];
    float4 color_1 [[user(TEXCOORD_1)]];
};


#line 53 "src/rendering/shaders/cy/particle.slang"
struct CyParticleVertex_0
{
    float4 position_1;
    float2 corner_1;
    float4 color_2;
};


#line 53
[[vertex]] cyParticleVertex_Result_0 cyParticleVertex(uint vertexId_0 [[vertex_id]], CyParticle_0 device* cyParticles_1 [[buffer(0)]], CyFrameData_0 constant* cyFrame_1 [[buffer(1)]])
{

#line 53
    thread KernelContext_0 kernelContext_1;

#line 53
    (&kernelContext_1)->cyParticles_0 = cyParticles_1;

#line 53
    (&kernelContext_1)->cyFrame_0 = cyFrame_1;

#line 79
    CyParticle_0 _S2 = cyParticles_1[vertexId_0 / 6U];
    uint _S3 = vertexId_0 % 6U;

#line 89
    thread CyParticleVertex_0 output_0;

#line 89
    float4 _S4 = transformToClip_0(_S2.positionAndSize_0.xyz + (cyFrame_1->relativeToViewRow0_0.xyz * float3(kCorners_0[_S3].x)  + cyFrame_1->relativeToViewRow1_0.xyz * float3(kCorners_0[_S3].y) ) * float3(_S2.positionAndSize_0.w) , &kernelContext_1);
    (&output_0)->position_1 = _S4;
    (&output_0)->corner_1 = kCorners_0[_S3];
    (&output_0)->color_2 = _S2.color_0;

#line 92
    thread cyParticleVertex_Result_0 _S5;

#line 92
    (&_S5)->position_0 = output_0.position_1;

#line 92
    (&_S5)->corner_0 = output_0.corner_1;

#line 92
    (&_S5)->color_1 = output_0.color_2;

#line 92
    return _S5;
}

)cy_msl";

/// cyParticleFragment.metal, 743 bytes.
inline constexpr char kParticleFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 30 "src/rendering/shaders/cy/material.slang"
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 30
struct pixelInput_0
{
    float2 corner_0 [[user(TEXCOORD)]];
    float4 color_0 [[user(TEXCOORD_1)]];
};


#line 97 "src/rendering/shaders/cy/particle.slang"
[[fragment]] pixelOutput_0 cyParticleFragment(pixelInput_0 _S1 [[stage_in]], float4 position_0 [[position]])
{

#line 97
    float2 _S2 = _S1.corner_0;

#line 103
    float _S3 = saturate(1.0 - dot(_S2, _S2));
    float _S4 = _S1.color_0.w * _S3 * _S3;

#line 104
    pixelOutput_0 _S5 = { float4(_S1.color_0.xyz * float3(_S4) , _S4) };

#line 109
    return _S5;
}

)cy_msl";

}  // namespace cy::rendering::particles

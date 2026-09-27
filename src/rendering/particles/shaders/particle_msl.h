#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the particle renderer. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::particles {

/// cyParticleVertex.metal, 4542 bytes.
inline constexpr char kParticleVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 68 "src/rendering/shaders/cy/particle.slang"
constant array<float2, int(6)> kCorners_0 = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0), float2(-1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0) };

#line 47
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


#line 112 "src/rendering/shaders/cy/frame.slang"
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
    float4 shadowToClipRow0_0;
    float4 shadowToClipRow1_0;
    float4 shadowToClipRow2_0;
    float4 shadowToClipRow3_0;
    uint4 shadowControl_0;
};


#line 20 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    float3 positionRelativeToCamera_0;
    float range_0;
    float3 direction_0;
    float intensity_0;
    float3 color_1;
    uint kind_0;
    float2 spotScaleBias_0;
    float2 padding_0;
};


#line 186 "src/rendering/shaders/cy/frame.slang"
struct CyDrawInstance_0
{
    uint instanceSlot_0;
    uint material_0;
    uint parameterOffset_0;
    uint giAddress_0;
    uint layerMask_0;
    uint lodAndFade_0;
    uint surface_0;
    uint flags_0;
};


struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


struct CyFrameViewSet_default_0
{
    CyFrameData_0 constant* frame_0;
    Light_0 device* lights_0;
    uint2 device* clusterHeaders_0;
    uint device* clusterIndices_0;
    CyDrawInstance_0 device* drawInstances_0;
    CyInstanceTransform_0 device* instances_0;
    uint device* materialWords_0;
};


#line 208
struct KernelContext_0
{
    CyParticle_0 device* cyParticles_0;
    CyFrameViewSet_default_0 constant* cyFrameView_0;
};


#line 263
float4 transformToClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S1 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow0_0, _S1), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow1_0, _S1), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow2_0, _S1), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow3_0, _S1));
}


#line 266
struct cyParticleVertex_Result_0
{
    float4 position_0 [[position]];
    float2 corner_0 [[user(TEXCOORD)]];
    float4 color_2 [[user(TEXCOORD_1)]];
};


#line 58 "src/rendering/shaders/cy/particle.slang"
struct CyParticleVertex_0
{
    float4 position_1;
    float2 corner_1;
    float4 color_3;
};


#line 58
[[vertex]] cyParticleVertex_Result_0 cyParticleVertex(uint vertexId_0 [[vertex_id]], CyParticle_0 device* cyParticles_1 [[buffer(0)]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]])
{

#line 58
    thread KernelContext_0 kernelContext_1;

#line 58
    (&kernelContext_1)->cyParticles_0 = cyParticles_1;

#line 58
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 80
    CyParticle_0 _S2 = cyParticles_1[vertexId_0 / 6U];
    uint _S3 = vertexId_0 % 6U;

#line 90
    thread CyParticleVertex_0 output_0;

#line 90
    float4 _S4 = transformToClip_0(_S2.positionAndSize_0.xyz + (cyFrameView_1->frame_0->relativeToViewRow0_0.xyz * float3(kCorners_0[_S3].x)  + cyFrameView_1->frame_0->relativeToViewRow1_0.xyz * float3(kCorners_0[_S3].y) ) * float3(_S2.positionAndSize_0.w) , &kernelContext_1);
    (&output_0)->position_1 = _S4;
    (&output_0)->corner_1 = kCorners_0[_S3];
    (&output_0)->color_3 = _S2.color_0;

#line 93
    thread cyParticleVertex_Result_0 _S5;

#line 93
    (&_S5)->position_0 = output_0.position_1;

#line 93
    (&_S5)->corner_0 = output_0.corner_1;

#line 93
    (&_S5)->color_2 = output_0.color_3;

#line 93
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


#line 98 "src/rendering/shaders/cy/particle.slang"
[[fragment]] pixelOutput_0 cyParticleFragment(pixelInput_0 _S1 [[stage_in]], float4 position_0 [[position]])
{

#line 98
    float2 _S2 = _S1.corner_0;

#line 104
    float _S3 = saturate(1.0 - dot(_S2, _S2));
    float _S4 = _S1.color_0.w * _S3 * _S3;

#line 105
    pixelOutput_0 _S5 = { float4(_S1.color_0.xyz * float3(_S4) , _S4) };

#line 110
    return _S5;
}

)cy_msl";

}  // namespace cy::rendering::particles

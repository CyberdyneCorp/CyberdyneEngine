#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the rendering frame. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::pipeline {

/// DepthVertex.metal, 6552 bytes.
inline constexpr char kFrameDepthVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 247 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 327
float3 transformToRelative_0(const CyInstanceTransform_0 thread* instance_0, float3 modelPosition_0)
{
    float4 _S1 = float4(modelPosition_0, 1.0);
    return float3(dot(instance_0->row0_0, _S1), dot(instance_0->row1_0, _S1), dot(instance_0->row2_0, _S1));
}


#line 10 "src/rendering/shaders/cy/cluster.slang"
struct ClusterGrid_0
{
    packed_uint3 dimensions_0;
    uint maxLightsPerCluster_0;
    float nearPlane_0;
    float farPlane_0;
    float sliceScale_0;
    float sliceBias_0;
};


#line 114 "src/rendering/shaders/cy/frame.slang"
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
    uint4 occlusionControl_0;
    uint4 softShadowControl_0;
    float4 softShadowShape_0;
    uint4 probeVolumeControl_0;
    float4 probeVolumeOrigin_0;
    float4 probeVolumeParams_0;
    uint4 decalControl_0;
};


#line 20 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    float2 padding_0;
};


#line 234 "src/rendering/shaders/cy/frame.slang"
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


#line 256
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


#line 301
struct CyDrawPush_0
{
    uint drawIndex_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    CyFrameViewSet_default_0 constant* cyFrameView_0;
    CyDrawPush_0 constant* cyDraw_0;
};


#line 311 "src/rendering/shaders/cy/frame.slang"
float4 transformToClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow3_0, _S2));
}


#line 23 "src/rendering/shaders/cy/packing.slang"
float3 decodeOctahedral_0(float2 encoded_0)
{
    float2 _S3 = encoded_0 * float2(2.0)  - float2(1.0) ;
    float _S4 = _S3.x;

#line 26
    float _S5 = _S3.y;

#line 26
    float _S6 = 1.0 - abs(_S4) - abs(_S5);

#line 26
    thread float3 normal_0 = float3(_S4, _S5, _S6);
    float _S7 = saturate(- _S6);
    float2 _S8 = float2(_S4, _S5);

#line 28
    normal_0.xy = _S8 + select(float2(_S7) , float2(- _S7) , _S8 >= (float2(0.0) ));
    return normalize(normal_0);
}


#line 336 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 338
struct cyDepthVertex_Result_0
{
    float4 position_0 [[position]];
    float3 normal_1 [[user(TEXCOORD)]];
    float4 currentClip_0 [[user(TEXCOORD_1)]];
    float4 previousClip_0 [[user(TEXCOORD_2)]];
};


#line 338
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
};


#line 454
struct CyDepthVertex_0
{
    float4 position_1;
    float3 normal_2;
    float4 currentClip_1;
    float4 previousClip_1;
};


#line 454
[[vertex]] cyDepthVertex_Result_0 cyDepthVertex(vertexInput_0 _S9 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 454
    thread KernelContext_0 kernelContext_1;

#line 454
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 454
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 466
    CyInstanceTransform_0 _S10 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

#line 466
    thread CyInstanceTransform_0 _S11 = _S10;

#line 466
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    thread CyDepthVertex_0 output_0;

#line 467
    float4 _S13 = transformToClip_0(_S12, &kernelContext_1);

    (&output_0)->position_1 = _S13;
    (&output_0)->currentClip_1 = _S13;
    float4 _S14 = float4(_S12, 1.0);
    (&output_0)->previousClip_1 = float4(dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow0_0, _S14), dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow1_0, _S14), dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow2_0, _S14), dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow3_0, _S14));



    float3 _S15 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 476
    thread CyInstanceTransform_0 _S16 = _S10;

#line 476
    float3 _S17 = rotateToRelative_0(&_S16, _S15);

#line 476
    (&output_0)->normal_2 = _S17;

#line 476
    thread cyDepthVertex_Result_0 _S18;

#line 476
    (&_S18)->position_0 = output_0.position_1;

#line 476
    (&_S18)->normal_1 = output_0.normal_2;

#line 476
    (&_S18)->currentClip_0 = output_0.currentClip_1;

#line 476
    (&_S18)->previousClip_0 = output_0.previousClip_1;

#line 476
    return _S18;
}

)cy_msl";

/// DepthFragment.metal, 3972 bytes.
inline constexpr char kFrameDepthFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 12 "src/rendering/shaders/cy/packing.slang"
float2 encodeOctahedral_0(float3 normal_0)
{
    float3 _S1 = normal_0 / float3((abs(normal_0.x) + abs(normal_0.y) + abs(normal_0.z))) ;
    float2 _S2 = _S1.xy;

#line 15
    float2 encoded_0;
    if((_S1.z) < 0.0)
    {

#line 16
        encoded_0 = (float2(1.0)  - abs(_S1.yx)) * select(float2(-1.0) , float2(1.0) , _S2 >= (float2(0.0) ));

#line 16
    }
    else
    {

#line 16
        encoded_0 = _S2;

#line 16
    }

#line 16
    float2 _S3 = float2(0.5) ;



    return encoded_0 * _S3 + _S3;
}


#line 480 "src/rendering/shaders/cy/frame.slang"
struct CyDepthOutput_0
{
    float4 normalRoughness_0 [[color(0)]];
    float2 velocity_0 [[color(1)]];
};


#line 480
struct pixelInput_0
{
    float3 normal_1 [[user(TEXCOORD)]];
    float4 currentClip_0 [[user(TEXCOORD_1)]];
    float4 previousClip_0 [[user(TEXCOORD_2)]];
};


#line 10 "src/rendering/shaders/cy/cluster.slang"
struct ClusterGrid_0
{
    packed_uint3 dimensions_0;
    uint maxLightsPerCluster_0;
    float nearPlane_0;
    float farPlane_0;
    float sliceScale_0;
    float sliceBias_0;
};


#line 114 "src/rendering/shaders/cy/frame.slang"
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
    uint4 occlusionControl_0;
    uint4 softShadowControl_0;
    float4 softShadowShape_0;
    uint4 probeVolumeControl_0;
    float4 probeVolumeOrigin_0;
    float4 probeVolumeParams_0;
    uint4 decalControl_0;
};


#line 20 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    float2 padding_0;
};


#line 234 "src/rendering/shaders/cy/frame.slang"
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


#line 487
[[fragment]] CyDepthOutput_0 cyDepthFragment(pixelInput_0 _S4 [[stage_in]], float4 position_0 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_0 [[buffer(1)]])
{
    thread CyDepthOutput_0 output_0;
    (&output_0)->normalRoughness_0 = float4(encodeOctahedral_0(normalize(_S4.normal_1)), 1.0, 1.0);

#line 498
    float2 _S5 = _S4.currentClip_0.xy / float2(_S4.currentClip_0.w)  - cyFrameView_0->frame_0->temporalJitter_0.xy * float2(2.0)  * cyFrameView_0->frame_0->extentAndInverse_0.zw;

    float2 _S6 = _S4.previousClip_0.xy / float2(_S4.previousClip_0.w) ;
    (&output_0)->velocity_0 = float2((_S6.x - _S5.x) * 0.5, (_S5.y - _S6.y) * 0.5);

    return output_0;
}

)cy_msl";

/// ShadowVertex.metal, 4561 bytes.
inline constexpr char kFrameShadowVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 247 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 327
float3 transformToRelative_0(const CyInstanceTransform_0 thread* instance_0, float3 modelPosition_0)
{
    float4 _S1 = float4(modelPosition_0, 1.0);
    return float3(dot(instance_0->row0_0, _S1), dot(instance_0->row1_0, _S1), dot(instance_0->row2_0, _S1));
}


#line 10 "src/rendering/shaders/cy/cluster.slang"
struct ClusterGrid_0
{
    packed_uint3 dimensions_0;
    uint maxLightsPerCluster_0;
    float nearPlane_0;
    float farPlane_0;
    float sliceScale_0;
    float sliceBias_0;
};


#line 114 "src/rendering/shaders/cy/frame.slang"
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
    uint4 occlusionControl_0;
    uint4 softShadowControl_0;
    float4 softShadowShape_0;
    uint4 probeVolumeControl_0;
    float4 probeVolumeOrigin_0;
    float4 probeVolumeParams_0;
    uint4 decalControl_0;
};


#line 20 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    float2 padding_0;
};


#line 234 "src/rendering/shaders/cy/frame.slang"
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


#line 256
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


#line 301
struct CyDrawPush_0
{
    uint drawIndex_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    CyFrameViewSet_default_0 constant* cyFrameView_0;
    CyDrawPush_0 constant* cyDraw_0;
};


#line 318 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow3_0, _S2));
}


#line 321
struct cyShadowVertex_Result_0
{
    float4 position_0 [[position]];
};


#line 321
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
};


#line 429
struct CyShadowVertex_0
{
    float4 position_1;
};


#line 429
[[vertex]] cyShadowVertex_Result_0 cyShadowVertex(vertexInput_0 _S3 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 429
    thread KernelContext_0 kernelContext_1;

#line 429
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 429
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 439
    thread CyShadowVertex_0 output_0;

#line 439
    thread CyInstanceTransform_0 _S4 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

#line 439
    float3 _S5 = transformToRelative_0(&_S4, _S3.modelPosition_1);

#line 439
    float4 _S6 = transformToShadowClip_0(_S5, &kernelContext_1);
    (&output_0)->position_1 = _S6;

#line 440
    thread cyShadowVertex_Result_0 _S7;

#line 440
    (&_S7)->position_0 = output_0.position_1;

#line 440
    return _S7;
}

)cy_msl";

/// ShadowFragment.metal, 372 bytes.
inline constexpr char kFrameShadowFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 90 "core"
struct pixelOutput_0
{
    float output_0 [[color(0)]];
};


#line 445 "src/rendering/shaders/cy/frame.slang"
[[fragment]] pixelOutput_0 cyShadowFragment(float4 position_0 [[position]])
{

#line 445
    pixelOutput_0 _S1 = { position_0.z };

    return _S1;
}

)cy_msl";

/// ForwardVertex.metal, 6386 bytes.
inline constexpr char kFrameForwardVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 247 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 327
float3 transformToRelative_0(const CyInstanceTransform_0 thread* instance_0, float3 modelPosition_0)
{
    float4 _S1 = float4(modelPosition_0, 1.0);
    return float3(dot(instance_0->row0_0, _S1), dot(instance_0->row1_0, _S1), dot(instance_0->row2_0, _S1));
}


#line 10 "src/rendering/shaders/cy/cluster.slang"
struct ClusterGrid_0
{
    packed_uint3 dimensions_0;
    uint maxLightsPerCluster_0;
    float nearPlane_0;
    float farPlane_0;
    float sliceScale_0;
    float sliceBias_0;
};


#line 114 "src/rendering/shaders/cy/frame.slang"
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
    uint4 occlusionControl_0;
    uint4 softShadowControl_0;
    float4 softShadowShape_0;
    uint4 probeVolumeControl_0;
    float4 probeVolumeOrigin_0;
    float4 probeVolumeParams_0;
    uint4 decalControl_0;
};


#line 20 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    float2 padding_0;
};


#line 234 "src/rendering/shaders/cy/frame.slang"
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


#line 256
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


#line 301
struct CyDrawPush_0
{
    uint drawIndex_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    CyFrameViewSet_default_0 constant* cyFrameView_0;
    CyDrawPush_0 constant* cyDraw_0;
};


#line 311 "src/rendering/shaders/cy/frame.slang"
float4 transformToClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow3_0, _S2));
}


#line 23 "src/rendering/shaders/cy/packing.slang"
float3 decodeOctahedral_0(float2 encoded_0)
{
    float2 _S3 = encoded_0 * float2(2.0)  - float2(1.0) ;
    float _S4 = _S3.x;

#line 26
    float _S5 = _S3.y;

#line 26
    float _S6 = 1.0 - abs(_S4) - abs(_S5);

#line 26
    thread float3 normal_0 = float3(_S4, _S5, _S6);
    float _S7 = saturate(- _S6);
    float2 _S8 = float2(_S4, _S5);

#line 28
    normal_0.xy = _S8 + select(float2(_S7) , float2(- _S7) , _S8 >= (float2(0.0) ));
    return normalize(normal_0);
}


#line 336 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 338
struct cyForwardVertex_Result_0
{
    float4 position_0 [[position]];
    float3 relativePosition_0 [[user(TEXCOORD)]];
    float3 normal_1 [[user(TEXCOORD_1)]];
    float2 uv_0 [[user(TEXCOORD_2)]];
    uint drawIndex_1 [[user(TEXCOORD_3)]];
};


#line 338
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
    float2 uv_1 [[attribute(2)]];
};


#line 508
struct CyForwardVertex_0
{
    float4 position_1;
    float3 relativePosition_1;
    float3 normal_2;
    float2 uv_2;
    [[flat]] uint drawIndex_2;
};


#line 508
[[vertex]] cyForwardVertex_Result_0 cyForwardVertex(vertexInput_0 _S9 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 508
    thread KernelContext_0 kernelContext_1;

#line 508
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 508
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 522
    CyInstanceTransform_0 _S10 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

    thread CyForwardVertex_0 output_0;

#line 524
    thread CyInstanceTransform_0 _S11 = _S10;

#line 524
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    (&output_0)->relativePosition_1 = _S12;

#line 525
    float4 _S13 = transformToClip_0(_S12, &kernelContext_1);
    (&output_0)->position_1 = _S13;



    float3 _S14 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 530
    thread CyInstanceTransform_0 _S15 = _S10;

#line 530
    float3 _S16 = rotateToRelative_0(&_S15, _S14);

#line 530
    (&output_0)->normal_2 = _S16;
    (&output_0)->uv_2 = _S9.uv_1;
    (&output_0)->drawIndex_2 = cyDraw_1->drawIndex_0;

#line 532
    thread cyForwardVertex_Result_0 _S17;

#line 532
    (&_S17)->position_0 = output_0.position_1;

#line 532
    (&_S17)->relativePosition_0 = output_0.relativePosition_1;

#line 532
    (&_S17)->normal_1 = output_0.normal_2;

#line 532
    (&_S17)->uv_0 = output_0.uv_2;

#line 532
    (&_S17)->drawIndex_1 = output_0.drawIndex_2;

#line 532
    return _S17;
}

)cy_msl";

/// ForwardFragment.metal, 56324 bytes.
inline constexpr char kFrameForwardFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 538 "src/rendering/shaders/cy/frame.slang"
struct CyFrameShadowMap_0
{
    uint slot_0;
};


#line 10 "src/rendering/shaders/cy/cluster.slang"
struct ClusterGrid_0
{
    packed_uint3 dimensions_0;
    uint maxLightsPerCluster_0;
    float nearPlane_0;
    float farPlane_0;
    float sliceScale_0;
    float sliceBias_0;
};


#line 114 "src/rendering/shaders/cy/frame.slang"
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
    uint4 occlusionControl_0;
    uint4 softShadowControl_0;
    float4 softShadowShape_0;
    uint4 probeVolumeControl_0;
    float4 probeVolumeOrigin_0;
    float4 probeVolumeParams_0;
    uint4 decalControl_0;
};


#line 20 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    float2 padding_0;
};


#line 234 "src/rendering/shaders/cy/frame.slang"
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


#line 14 "src/rendering/shaders/cy/globals.slang"
struct CyGlobalsData_0
{
    float timeSeconds_0;
    float deltaSeconds_0;
    float exposureStops_0;
    float windStrength_0;
    float4 windDirectionAndSpeed_0;
};


#line 3595 "hlsl.meta.slang"
struct _Array_default_Texture2D128_0
{
    array<texture2d<float, access::sample>, int(128)> data_0;
};


#line 1187
struct CyFrameGlobalSet_default_0
{
    CyGlobalsData_0 constant* globals_0 [[id(0)]];
    array<texture2d<float, access::sample>, 128> textures_0 [[id(1)]];
    sampler sampler_0 [[id(129)]];
};


#line 1187
struct KernelContext_0
{
    CyFrameViewSet_default_0 constant* cyFrameView_0;
    CyFrameGlobalSet_default_0 constant* cyFrameGlobals_0;
};


#line 6 "src/rendering/shaders/cy/sampling.slang"
float radicalInverseBase2_0(uint index_0)
{

    uint bits_0 = (index_0 << 16U) | (index_0 >> 16U);
    uint bits_1 = ((bits_0 & 1431655765U) << 1U) | ((bits_0 & 2863311530U) >> 1U);
    uint bits_2 = ((bits_1 & 858993459U) << 2U) | ((bits_1 & 3435973836U) >> 2U);
    uint bits_3 = ((bits_2 & 252645135U) << 4U) | ((bits_2 & 4042322160U) >> 4U);

    return float(((bits_3 & 16711935U) << 8U) | ((bits_3 & 4278255360U) >> 8U)) * 2.32830643653869629e-10;
}

float2 hammersley_0(uint index_1, uint count_0)
{
    return float2(float(index_1) / float(count_0), radicalInverseBase2_0(index_1));
}


#line 106 "src/rendering/shaders/cy/shadow.slang"
float2 shadowDiscTap_0(int index_2, int count_1, float cosine_0, float sine_0)
{
    float2 _S1 = hammersley_0(uint(index_2), uint(count_1));

    float _S2 = _S1.y * 6.28318548202514648;
    float2 _S3 = float2(cos(_S2), sin(_S2)) * float2(sqrt(_S1.x)) ;
    float _S4 = _S3.x;

#line 112
    float _S5 = _S3.y;

#line 112
    return float2(_S4 * cosine_0 - _S5 * sine_0, _S4 * sine_0 + _S5 * cosine_0);
}


#line 294 "src/rendering/shaders/cy/frame.slang"
float4 cyMaterialSampleTextureLevel_0(uint bindlessIndex_0, float2 uv_0, float level_0, KernelContext_0 thread* kernelContext_0)
{
    return ((kernelContext_0->cyFrameGlobals_0->textures_0[bindlessIndex_0]).sample((kernelContext_0->cyFrameGlobals_0->sampler_0), (uv_0), level((level_0))));
}


#line 541
float CyFrameShadowMap_storedDepth_0(const CyFrameShadowMap_0 thread* this_0, float2 uv_1, KernelContext_0 thread* kernelContext_1)
{

#line 541
    float4 _S6 = cyMaterialSampleTextureLevel_0(this_0->slot_0, uv_1, 0.0, kernelContext_1);

    return _S6.x;
}


#line 149 "src/rendering/shaders/cy/shadow.slang"
float pcssFilter_0(const CyFrameShadowMap_0 thread* source_0, float2 uv_2, float receiver_0, float radius_0, float rotation_0, int taps_0, KernelContext_0 thread* kernelContext_2)
{

    float _S7 = cos(rotation_0);
    float _S8 = sin(rotation_0);

#line 153
    int index_3 = int(0);

#line 153
    float lit_0 = 0.0;

    for(;;)
    {

#line 155
        if(index_3 < taps_0)
        {
        }
        else
        {

#line 155
            break;
        }

#line 155
        float _S9 = CyFrameShadowMap_storedDepth_0(source_0, uv_2 + shadowDiscTap_0(index_3, taps_0, _S7, _S8) * float2(radius_0) , kernelContext_2);

#line 155
        float _S10;


        if(receiver_0 >= _S9)
        {

#line 158
            _S10 = 1.0;

#line 158
        }
        else
        {

#line 158
            _S10 = 0.0;

#line 158
        }

#line 158
        float lit_1 = lit_0 + _S10;

#line 155
        index_3 = index_3 + int(1);

#line 155
        lit_0 = lit_1;

#line 155
    }

#line 160
    return lit_0 / float(taps_0);
}


#line 89
struct PcssBlockers_0
{
    float averageDepth_0;
    float count_2;
};


#line 541 "src/rendering/shaders/cy/frame.slang"
float CyFrameShadowMap_storedDepth_1(const CyFrameShadowMap_0 thread* this_1, float2 uv_3, KernelContext_0 thread* kernelContext_3)
{

#line 541
    float4 _S11 = cyMaterialSampleTextureLevel_0(this_1->slot_0, uv_3, 0.0, kernelContext_3);

    return _S11.x;
}


#line 116 "src/rendering/shaders/cy/shadow.slang"
PcssBlockers_0 pcssBlockerSearch_0(const CyFrameShadowMap_0 thread* source_1, float2 uv_4, float receiver_1, float searchRadius_0, float rotation_1, int taps_1, KernelContext_0 thread* kernelContext_4)
{


    float _S12 = cos(rotation_1);
    float _S13 = sin(rotation_1);
    thread PcssBlockers_0 result_0;
    (&result_0)->averageDepth_0 = 0.0;
    (&result_0)->count_2 = 0.0;

#line 124
    int index_4 = int(0);
    for(;;)
    {

#line 125
        if(index_4 < taps_1)
        {
        }
        else
        {

#line 125
            break;
        }

#line 125
        float _S14 = CyFrameShadowMap_storedDepth_1(source_1, uv_4 + shadowDiscTap_0(index_4, taps_1, _S12, _S13) * float2(searchRadius_0) , kernelContext_4);


        if(_S14 > receiver_1)
        {
            (&result_0)->averageDepth_0 = (&result_0)->averageDepth_0 + _S14;
            (&result_0)->count_2 = (&result_0)->count_2 + 1.0;

#line 128
        }

#line 125
        index_4 = index_4 + int(1);

#line 125
    }

#line 134
    if(((&result_0)->count_2) > 0.0)
    {
        (&result_0)->averageDepth_0 = (&result_0)->averageDepth_0 / (&result_0)->count_2;

#line 134
    }



    return result_0;
}


#line 128 "src/rendering/shaders/cy/decal.slang"
float2 cyDecalWordUv_0(uint index_5, uint rows_0)
{

    return (float2(float(index_5 % 1024U), float(index_5 / 1024U)) + float2(0.5) ) / float2(1024.0, float(max(rows_0, 1U)));
}


#line 121
uint cyDecalWordFromTexel_0(float4 texel_0)
{
    uint4 _S15 = uint4(round(saturate(texel_0) * float4(255.0) ));
    return (((_S15.x) | ((_S15.y) << 8U)) | ((_S15.z) << 16U)) | ((_S15.w) << 24U);
}


#line 793 "src/rendering/shaders/cy/frame.slang"
uint CyFrameDecalSource_word_0(uint index_6, KernelContext_0 thread* kernelContext_5)
{

#line 793
    float4 _S16 = cyMaterialSampleTextureLevel_0(kernelContext_5->cyFrameView_0->frame_0->decalControl_0.x, cyDecalWordUv_0(index_6, kernelContext_5->cyFrameView_0->frame_0->decalControl_0.y), 0.0, kernelContext_5);


    return cyDecalWordFromTexel_0(_S16);
}


#line 113 "src/rendering/shaders/cy/decal.slang"
float3 wordFloat3_0(uint index_7, KernelContext_0 thread* kernelContext_6)
{

#line 113
    uint _S17 = CyFrameDecalSource_word_0(index_7, kernelContext_6);

    float _S18 = (as_type<float>((_S17)));

#line 115
    uint _S19 = CyFrameDecalSource_word_0(index_7 + 1U, kernelContext_6);

#line 115
    float _S20 = (as_type<float>((_S19)));

#line 115
    uint _S21 = CyFrameDecalSource_word_0(index_7 + 2U, kernelContext_6);

#line 115
    return float3(_S18, _S20, (as_type<float>((_S21))));
}


#line 108
float wordFloat_0(uint index_8, KernelContext_0 thread* kernelContext_7)
{

#line 108
    uint _S22 = CyFrameDecalSource_word_0(index_8, kernelContext_7);

    return (as_type<float>((_S22)));
}


#line 164 "src/rendering/shaders/cy/shadow.slang"
struct PcssShape_0
{
    float penumbraPerDepth_0;
    float minRadius_0;
    float maxRadius_0;
    int blockerTaps_0;
    int filterTaps_0;
};


#line 143
float pcssPenumbraRadius_0(float receiver_2, float averageBlocker_0, float penumbraPerDepth_1)
{
    return max(averageBlocker_0 - receiver_2, 0.0) * penumbraPerDepth_1;
}


#line 181
float pcssVisibility_0(const CyFrameShadowMap_0 thread* source_2, float2 uv_5, float receiver_3, const PcssShape_0 thread* shape_0, float rotation_2, KernelContext_0 thread* kernelContext_8)
{

#line 182
    float _S23 = shape_0->penumbraPerDepth_0;

#line 182
    float _S24 = shape_0->minRadius_0;

#line 182
    float _S25 = shape_0->maxRadius_0;

#line 182
    PcssBlockers_0 _S26 = pcssBlockerSearch_0(source_2, uv_5, receiver_3, clamp((1.0 - receiver_3) * shape_0->penumbraPerDepth_0, shape_0->minRadius_0, shape_0->maxRadius_0), rotation_2, shape_0->blockerTaps_0, kernelContext_8);

#line 189
    if((_S26.count_2) == 0.0)
    {
        return 1.0;
    }

#line 191
    float _S27 = pcssFilter_0(source_2, uv_5, receiver_3, clamp(pcssPenumbraRadius_0(receiver_3, _S26.averageDepth_0, _S23), _S24, _S25), rotation_2, shape_0->filterTaps_0, kernelContext_8);



    return _S27;
}


#line 71 "src/rendering/shaders/cy/decal.slang"
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


#line 793 "src/rendering/shaders/cy/frame.slang"
uint CyFrameDecalSource_word_1(uint index_9, KernelContext_0 thread* kernelContext_9)
{

#line 793
    float4 _S28 = cyMaterialSampleTextureLevel_0(kernelContext_9->cyFrameView_0->frame_0->decalControl_0.x, cyDecalWordUv_0(index_9, kernelContext_9->cyFrameView_0->frame_0->decalControl_0.y), 0.0, kernelContext_9);


    return cyDecalWordFromTexel_0(_S28);
}


#line 192 "src/rendering/shaders/cy/decal.slang"
float cyDecalAngleFade_0(float cosine_1, float limit_0)
{

#line 192
    bool _S29;

    if(cosine_1 <= 0.0)
    {

#line 194
        _S29 = true;

#line 194
    }
    else
    {

#line 194
        _S29 = cosine_1 <= limit_0;

#line 194
    }

#line 194
    if(_S29)
    {
        return 0.0;
    }
    float _S30 = (cosine_1 - limit_0) / max(1.0 - limit_0, 0.00009999999747379);
    return _S30 * _S30 * (3.0 - 2.0 * _S30);
}


float cyDecalDistanceFade_0(float metres_0, float start_0, float end_0)
{
    float _S31 = max(end_0, start_0 + 0.00009999999747379);
    if(metres_0 <= start_0)
    {
        return 1.0;
    }
    if(metres_0 >= _S31)
    {
        return 0.0;
    }
    float _S32 = 1.0 - (metres_0 - start_0) / (_S31 - start_0);
    return _S32 * _S32 * (3.0 - 2.0 * _S32);
}


#line 294 "src/rendering/shaders/cy/frame.slang"
float4 cyMaterialSampleTextureLevel_1(uint bindlessIndex_1, float2 uv_6, float level_1, KernelContext_0 thread* kernelContext_10)
{
    return ((kernelContext_10->cyFrameGlobals_0->textures_0[bindlessIndex_1]).sample((kernelContext_10->cyFrameGlobals_0->sampler_0), (uv_6), level((level_1))));
}


#line 798
float CyFrameDecalSource_mask_0(uint slot_1, float2 uv_7, KernelContext_0 thread* kernelContext_11)
{

#line 798
    float4 _S33 = cyMaterialSampleTextureLevel_1(slot_1, uv_7, 0.0, kernelContext_11);

    return _S33.w;
}


#line 15 "src/rendering/shaders/cy/noise.slang"
uint3 hashPcg3d_0(uint3 value_0)
{
    uint3 _S34 = value_0 * uint3(1664525U)  + uint3(1013904223U) ;

#line 17
    thread uint3 v_0 = _S34;
    v_0.x = v_0.x + _S34.y * _S34.z;
    v_0.y = v_0.y + v_0.z * v_0.x;
    v_0.z = v_0.z + v_0.x * v_0.y;
    uint3 _S35 = v_0 ^ (v_0 >> (uint3(16U) ));

#line 21
    v_0 = _S35;
    v_0.x = v_0.x + _S35.y * _S35.z;
    v_0.y = v_0.y + v_0.z * v_0.x;
    v_0.z = v_0.z + v_0.x * v_0.y;
    return v_0;
}


#line 36
float valueNoise_0(float3 position_0)
{
    float3 _S36 = floor(position_0);
    float3 _S37 = position_0 - _S36;
    float3 _S38 = _S37 * _S37 * (float3(3.0)  - float3(2.0)  * _S37);
    uint3 _S39 = uint3(int3(_S36) + int3(int(1024)) );

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
        uint3 _S40 = uint3(corner_0 & 1U, (corner_0 >> 1U) & 1U, (corner_0 >> 2U) & 1U);

        float3 _S41 = mix(float3(1.0)  - _S38, _S38, float3(_S40));
        float result_2 = result_1 + float((hashPcg3d_0(_S39 + _S40).x) >> 8U) * 5.9604644775390625e-08 * _S41.x * _S41.y * _S41.z;

#line 44
        corner_0 = corner_0 + 1U;

#line 44
        result_1 = result_2;

#line 44
    }

#line 51
    return result_1;
}


#line 147 "src/rendering/shaders/cy/decal.slang"
float decalFbm_0(float2 uv_8, float frequency_0, uint seed_0)
{
    float _S42 = float(seed_0 % 4096U) * 1.61800003051757812;

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
        float sum_1 = sum_0 + valueNoise_0(float3(uv_8 * float2(scale_0) , _S42)) * amplitude_0;
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
float cyDecalShapeCoverage_0(uint shape_1, float2 uv_9, float a_0, float b_0, uint seed_1)
{
    float _S43 = length(uv_9 - float2(0.5) ) * 2.0;
    if(shape_1 == 1U)
    {

        return 1.0 - smoothstep(0.44999998807907104, 0.94999998807907104, _S43 + (decalFbm_0(uv_9, a_0, seed_1) - 0.5) * b_0);
    }
    if(shape_1 == 2U)
    {

        return 1.0 - smoothstep(b_0 * 0.69999998807907104, b_0, abs(_S43 - a_0));
    }
    if(shape_1 == 3U)
    {

        return smoothstep(b_0, b_0 + 0.11999999731779099, decalFbm_0(uv_9, a_0, seed_1)) * (1.0 - smoothstep(0.75, 1.0, _S43));
    }
    return 1.0;
}


#line 220
void cyApplyDecal_0(uint rank_0, const CyDecalReceiver_0 thread* receiver_4, CyDecalSurface_0 thread* surface_1, KernelContext_0 thread* kernelContext_12)
{

#line 221
    uint _S44 = CyFrameDecalSource_word_1(4U, kernelContext_12);

    uint _S45 = _S44 + rank_0 * 36U;

#line 223
    uint _S46 = CyFrameDecalSource_word_1(_S45 + 28U, kernelContext_12);
    if((_S46 & (receiver_4->channels_0)) == 0U)
    {
        return;
    }

#line 226
    float3 _S47 = wordFloat3_0(_S45 + 4U, kernelContext_12);

#line 226
    float3 _S48 = wordFloat3_0(_S45 + 8U, kernelContext_12);

#line 226
    float3 _S49 = wordFloat3_0(_S45 + 12U, kernelContext_12);

#line 226
    float3 _S50 = receiver_4->relativePosition_0;

#line 226
    float3 _S51 = wordFloat3_0(_S45, kernelContext_12);

#line 231
    float3 _S52 = _S50 - _S51;
    float _S53 = dot(_S52, _S47);

#line 232
    float _S54 = dot(_S52, _S48);

#line 232
    float _S55 = dot(_S52, _S49);


    if(any((abs(float3(_S53, _S54, _S55))) > (float3(1.0) )))
    {
        return;
    }



    float _S56 = dot(receiver_4->geometricNormal_0, normalize(_S49));

#line 242
    float _S57 = wordFloat_0(_S45 + 3U, kernelContext_12);

#line 242
    float weight_0 = cyDecalAngleFade_0(_S56, _S57);

#line 242
    float _S58 = receiver_4->eyeDistance_0;

#line 242
    float _S59 = wordFloat_0(_S45 + 24U, kernelContext_12);

#line 242
    float _S60 = wordFloat_0(_S45 + 25U, kernelContext_12);
    float weight_1 = weight_0 * cyDecalDistanceFade_0(_S58, _S59, _S60);

#line 243
    float _S61 = wordFloat_0(_S45 + 34U, kernelContext_12);

#line 243
    float weight_2;


    if(_S61 > 0.0)
    {

#line 246
        weight_2 = weight_1 * (1.0 - smoothstep(1.0 - _S61, 1.0, abs(_S55)));

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
    float2 _S62 = float2(0.5) ;


    float2 _S63 = float2(_S53, _S54) * _S62 + _S62;

#line 255
    uint _S64 = CyFrameDecalSource_word_1(_S45 + 29U, kernelContext_12);

#line 255
    float _S65 = wordFloat_0(_S45 + 30U, kernelContext_12);

#line 255
    float _S66 = wordFloat_0(_S45 + 31U, kernelContext_12);

#line 255
    uint _S67 = CyFrameDecalSource_word_1(_S45 + 35U, kernelContext_12);

#line 255
    uint _S68 = CyFrameDecalSource_word_1(_S45 + 32U, kernelContext_12);

#line 255
    float _S69;

#line 261
    if(_S68 == 4294967295U)
    {

#line 261
        _S69 = 1.0;

#line 261
    }
    else
    {

#line 261
        float _S70 = CyFrameDecalSource_mask_0(_S68, _S63, kernelContext_12);

#line 261
        _S69 = _S70;

#line 261
    }

    float _S71 = weight_2 * (cyDecalShapeCoverage_0(_S64, _S63, _S65, _S66, _S67) * _S69);
    if(_S71 <= 0.0)
    {
        return;
    }

#line 266
    float3 _S72 = wordFloat3_0(_S45 + 16U, kernelContext_12);

#line 266
    float3 _S73 = wordFloat3_0(_S45 + 20U, kernelContext_12);

#line 271
    float3 _S74 = surface_1->albedo_0;

#line 271
    float _S75 = wordFloat_0(_S45 + 7U, kernelContext_12);

#line 271
    surface_1->albedo_0 = mix(_S74, _S72, float3((_S71 * _S75)) );
    float _S76 = surface_1->roughness_0;

#line 272
    float _S77 = wordFloat_0(_S45 + 19U, kernelContext_12);

#line 272
    float _S78 = wordFloat_0(_S45 + 11U, kernelContext_12);

#line 272
    surface_1->roughness_0 = mix(_S76, _S77, _S71 * _S78);

    float _S79 = surface_1->metallic_0;

#line 274
    float _S80 = wordFloat_0(_S45 + 23U, kernelContext_12);

#line 274
    float _S81 = wordFloat_0(_S45 + 26U, kernelContext_12);

#line 274
    surface_1->metallic_0 = mix(_S79, _S80, _S71 * _S81);

    float3 _S82 = surface_1->emission_0;

#line 276
    float _S83 = wordFloat_0(_S45 + 27U, kernelContext_12);

#line 276
    surface_1->emission_0 = mix(_S82, _S73, float3((_S71 * _S83)) );

#line 276
    float _S84 = wordFloat_0(_S45 + 33U, kernelContext_12);

#line 276
    float _S85 = wordFloat_0(_S45 + 15U, kernelContext_12);

#line 284
    float _S86 = _S84 * _S85 * weight_2;
    if(_S86 > 0.0)
    {

        float2 _S87 = float2(0.00390625, 0.0);

        float2 _S88 = float2(0.0, 0.00390625);



        float3 _S89 = float3(_S86)  * (float3(((cyDecalShapeCoverage_0(_S64, _S63 + _S87, _S65, _S66, _S67) - cyDecalShapeCoverage_0(_S64, _S63 - _S87, _S65, _S66, _S67)) / 0.0078125 * (length(_S47) * 0.5)))  * normalize(_S47) + float3(((cyDecalShapeCoverage_0(_S64, _S63 + _S88, _S65, _S66, _S67) - cyDecalShapeCoverage_0(_S64, _S63 - _S88, _S65, _S66, _S67)) / 0.0078125 * (length(_S48) * 0.5)))  * normalize(_S48));


        surface_1->normal_0 = normalize(surface_1->normal_0 - (_S89 - surface_1->normal_0 * float3(dot(surface_1->normal_0, _S89)) ));

#line 285
    }

#line 299
    return;
}


#line 334
uint cyDecalTableListEntry_0(uint index_10, KernelContext_0 thread* kernelContext_13)
{

#line 334
    uint _S90 = CyFrameDecalSource_word_0(index_10, kernelContext_13);

    return _S90;
}


#line 21 "src/rendering/shaders/cy/cluster.slang"
uint clusterSliceOf_0(const ClusterGrid_0 thread* grid_0, float viewDepth_0)
{

    return uint(clamp(log2(max(viewDepth_0, grid_0->nearPlane_0)) * grid_0->sliceScale_0 + grid_0->sliceBias_0, 0.0, float(grid_0->dimensions_0.z - 1U)));
}


uint3 clusterCoordOf_0(const ClusterGrid_0 thread* grid_1, uint2 pixel_0, uint2 renderExtent_0, float viewDepth_1)
{
    uint2 _S91 = grid_1->dimensions_0.xy;
    uint2 _S92 = min(uint2(float2(pixel_0) / float2(renderExtent_0) * float2(_S91)), _S91 - uint2(1U) );

#line 31
    uint _S93 = clusterSliceOf_0(grid_1, viewDepth_1);

#line 31
    return uint3(_S92, _S93);
}



uint clusterIndexOf_0(const ClusterGrid_0 thread* grid_2, uint3 coord_0)
{
    return coord_0.x + grid_2->dimensions_0.x * (coord_0.y + grid_2->dimensions_0.y * coord_0.z);
}


#line 306 "src/rendering/shaders/cy/decal.slang"
uint2 cyDecalTableList_0(float3 relativePosition_1, float2 pixel_1, KernelContext_0 thread* kernelContext_14)
{

#line 306
    uint _S94 = CyFrameDecalSource_word_1(5U, kernelContext_14);

    if(_S94 == 0U)
    {
        return uint2(0U, 0U);
    }
    thread ClusterGrid_0 grid_3;

#line 312
    uint _S95 = CyFrameDecalSource_word_1(8U, kernelContext_14);

#line 312
    uint _S96 = CyFrameDecalSource_word_1(9U, kernelContext_14);

#line 312
    uint _S97 = CyFrameDecalSource_word_1(10U, kernelContext_14);
    (&grid_3)->dimensions_0 = uint3(_S95, _S96, _S97);

#line 313
    uint _S98 = CyFrameDecalSource_word_1(11U, kernelContext_14);
    (&grid_3)->maxLightsPerCluster_0 = _S98;

#line 314
    float _S99 = wordFloat_0(12U, kernelContext_14);
    (&grid_3)->sliceScale_0 = _S99;

#line 315
    float _S100 = wordFloat_0(13U, kernelContext_14);
    (&grid_3)->sliceBias_0 = _S100;

#line 316
    float _S101 = wordFloat_0(14U, kernelContext_14);
    (&grid_3)->nearPlane_0 = _S101;

#line 317
    float _S102 = wordFloat_0(15U, kernelContext_14);
    (&grid_3)->farPlane_0 = _S102;
    if(_S97 == 0U)
    {
        return uint2(0U, 0U);
    }

#line 321
    uint _S103 = CyFrameDecalSource_word_1(16U, kernelContext_14);

#line 321
    uint _S104 = CyFrameDecalSource_word_1(17U, kernelContext_14);

    uint2 _S105 = uint2(_S103, _S104);

#line 323
    float3 _S106 = wordFloat3_0(18U, kernelContext_14);

#line 323
    float _S107 = wordFloat_0(21U, kernelContext_14);
    float4 _S108 = float4(_S106, _S107);
    float _S109 = dot(_S108.xyz, relativePosition_1) + _S108.w;
    uint2 _S110 = uint2(pixel_1);

#line 326
    thread ClusterGrid_0 _S111 = grid_3;

#line 326
    uint3 _S112 = clusterCoordOf_0(&_S111, _S110, _S105, _S109);

#line 326
    thread ClusterGrid_0 _S113 = grid_3;

#line 326
    uint _S114 = clusterIndexOf_0(&_S113, _S112);

#line 326
    uint _S115 = CyFrameDecalSource_word_1(6U, kernelContext_14);

    uint _S116 = _S115 + _S114 * 2U;

#line 328
    uint _S117 = CyFrameDecalSource_word_1(_S116, kernelContext_14);

#line 328
    uint _S118 = CyFrameDecalSource_word_1(_S116 + 1U, kernelContext_14);
    return uint2(_S117, _S118);
}


#line 135
uint cyDecalCount_0(KernelContext_0 thread* kernelContext_15)
{

#line 135
    uint _S119 = CyFrameDecalSource_word_0(0U, kernelContext_15);

#line 135
    uint _S120;

    if(_S119 == 1129923651U)
    {

#line 137
        uint _S121 = CyFrameDecalSource_word_0(2U, kernelContext_15);

#line 137
        _S120 = _S121;

#line 137
    }
    else
    {

#line 137
        _S120 = 0U;

#line 137
    }

#line 137
    return _S120;
}


#line 41 "src/rendering/shaders/cy/material.slang"
struct Surface_0
{
    float3 albedo_1;
    float3 normal_1;
    float roughness_1;
    float metallic_1;
    float3 emission_1;
    float occlusion_0;
    float opacity_0;
};




Surface_0 defaultSurface_0()
{
    thread Surface_0 surface_2;
    (&surface_2)->albedo_1 = float3(0.5) ;
    (&surface_2)->normal_1 = float3(0.0, 0.0, 1.0);
    (&surface_2)->roughness_1 = 0.5;
    (&surface_2)->metallic_1 = 0.0;
    (&surface_2)->emission_1 = float3(0.0) ;
    (&surface_2)->occlusion_0 = 1.0;
    (&surface_2)->opacity_0 = 1.0;
    return surface_2;
}


#line 355 "src/rendering/shaders/cy/frame.slang"
float3 readMaterialFloat3_0(uint material_1, uint wordOffset_0, KernelContext_0 thread* kernelContext_16)
{
    uint _S122 = material_1 * kernelContext_16->cyFrameView_0->frame_0->counts_0.y + wordOffset_0;
    return float3((as_type<float>((kernelContext_16->cyFrameView_0->materialWords_0[_S122]))), (as_type<float>((kernelContext_16->cyFrameView_0->materialWords_0[_S122 + 1U]))), (as_type<float>((kernelContext_16->cyFrameView_0->materialWords_0[_S122 + 2U]))));
}


#line 350
float readMaterialFloat_0(uint material_2, uint wordOffset_1, KernelContext_0 thread* kernelContext_17)
{
    return (as_type<float>((kernelContext_17->cyFrameView_0->materialWords_0[material_2 * kernelContext_17->cyFrameView_0->frame_0->counts_0.y + wordOffset_1])));
}


#line 365
uint readMaterialUint_0(uint material_3, uint wordOffset_2, KernelContext_0 thread* kernelContext_18)
{
    return kernelContext_18->cyFrameView_0->materialWords_0[material_3 * kernelContext_18->cyFrameView_0->frame_0->counts_0.y + wordOffset_2];
}


#line 375
uint materialTextureSlot_0(uint material_4, uint wordOffset_3, KernelContext_0 thread* kernelContext_19)
{
    if(wordOffset_3 == 4294967295U)
    {
        return 4294967295U;
    }

#line 379
    uint _S123 = readMaterialUint_0(material_4, wordOffset_3, kernelContext_19);

    return _S123;
}


#line 289
float4 cyMaterialSampleTexture_0(uint bindlessIndex_2, float2 uv_10, KernelContext_0 thread* kernelContext_20)
{
    return ((kernelContext_20->cyFrameGlobals_0->textures_0[bindlessIndex_2]).sample((kernelContext_20->cyFrameGlobals_0->sampler_0), (uv_10)));
}


#line 411
Surface_0 surfaceOf_0(uint material_5, float3 tint_1, float2 uv_11, KernelContext_0 thread* kernelContext_21)
{
    thread Surface_0 surface_3 = defaultSurface_0();

#line 413
    float3 _S124 = readMaterialFloat3_0(material_5, kernelContext_21->cyFrameView_0->frame_0->materialOffsets_0.x, kernelContext_21);
    (&surface_3)->albedo_1 = _S124 * tint_1;

#line 414
    float _S125 = readMaterialFloat_0(material_5, kernelContext_21->cyFrameView_0->frame_0->materialOffsets_0.y, kernelContext_21);
    (&surface_3)->roughness_1 = clamp(_S125, 0.01999999955296516, 1.0);

#line 415
    float _S126 = readMaterialFloat_0(material_5, kernelContext_21->cyFrameView_0->frame_0->materialOffsets_0.z, kernelContext_21);
    (&surface_3)->metallic_1 = saturate(_S126);

#line 416
    float3 _S127 = readMaterialFloat3_0(material_5, kernelContext_21->cyFrameView_0->frame_0->materialOffsets_0.w, kernelContext_21);
    (&surface_3)->emission_1 = _S127;

#line 417
    uint _S128 = materialTextureSlot_0(material_5, kernelContext_21->cyFrameView_0->frame_0->materialTextures_0.x, kernelContext_21);


    if(_S128 != 4294967295U)
    {

#line 420
        float4 _S129 = cyMaterialSampleTexture_0(_S128, uv_11, kernelContext_21);

        (&surface_3)->albedo_1 = (&surface_3)->albedo_1 * _S129.xyz;

#line 420
    }



    return surface_3;
}


#line 21 "src/rendering/shaders/cy/cluster.slang"
uint clusterSliceOf_1(const ClusterGrid_0 constant* grid_4, float viewDepth_2)
{

    return uint(clamp(log2(max(viewDepth_2, grid_4->nearPlane_0)) * grid_4->sliceScale_0 + grid_4->sliceBias_0, 0.0, float(grid_4->dimensions_0.z - 1U)));
}


uint3 clusterCoordOf_1(const ClusterGrid_0 constant* grid_5, uint2 pixel_2, uint2 renderExtent_1, float viewDepth_3)
{
    uint2 _S130 = grid_5->dimensions_0.xy;
    uint2 _S131 = min(uint2(float2(pixel_2) / float2(renderExtent_1) * float2(_S130)), _S130 - uint2(1U) );

#line 31
    uint _S132 = clusterSliceOf_1(grid_5, viewDepth_3);

#line 31
    return uint3(_S131, _S132);
}



uint clusterIndexOf_1(const ClusterGrid_0 constant* grid_6, uint3 coord_1)
{
    return coord_1.x + grid_6->dimensions_0.x * (coord_1.y + grid_6->dimensions_0.y * coord_1.z);
}


#line 342 "src/rendering/shaders/cy/frame.slang"
float viewDepthOf_0(float3 relative_0, KernelContext_0 thread* kernelContext_22)
{



    return - dot(kernelContext_22->cyFrameView_0->frame_0->relativeToViewRow2_0, float4(relative_0, 1.0));
}


#line 806
void applyFrameDecals_0(Surface_0 thread* surface_4, float3 thread* normal_2, float3 relativePosition_2, float2 fragmentCentre_0, uint receiverChannels_0, KernelContext_0 thread* kernelContext_23)
{


    thread CyDecalReceiver_0 receiver_5;
    (&receiver_5)->relativePosition_0 = relativePosition_2;
    (&receiver_5)->geometricNormal_0 = *normal_2;
    (&receiver_5)->eyeDistance_0 = length(relativePosition_2);
    (&receiver_5)->channels_0 = receiverChannels_0;
    thread CyDecalSurface_0 decalled_0;
    (&decalled_0)->albedo_0 = surface_4->albedo_1;
    (&decalled_0)->roughness_0 = surface_4->roughness_1;
    (&decalled_0)->metallic_0 = surface_4->metallic_1;
    (&decalled_0)->emission_0 = surface_4->emission_1;
    (&decalled_0)->normal_0 = *normal_2;

#line 825
    uint _S133 = kernelContext_23->cyFrameView_0->frame_0->decalControl_0.z;

#line 825
    uint _S134 = cyDecalCount_0(kernelContext_23);


    uint2 _S135 = uint2(0U, _S134);

#line 828
    uint2 list_0;

#line 828
    uint mode_0;
    if((_S133 & 2U) != 0U)
    {

#line 829
        list_0 = _S135;

#line 829
        mode_0 = 0U;

#line 829
    }
    else
    {

        if((_S133 & 1U) != 0U)
        {

            if(_S134 != 0U)
            {

#line 836
                uint2 _S136 = cyDecalTableList_0(relativePosition_2, fragmentCentre_0, kernelContext_23);

#line 836
                list_0 = _S136;

#line 836
            }
            else
            {

#line 836
                list_0 = uint2(0U) ;

#line 836
            }

#line 836
            mode_0 = 1U;

#line 833
        }
        else
        {

#line 833
            bool _S137;

#line 838
            if((kernelContext_23->cyFrameView_0->frame_0->counts_0.w) != 0U)
            {

#line 838
                _S137 = ((&kernelContext_23->cyFrameView_0->frame_0->clusterGrid_0)->dimensions_0.z) != 0U;

#line 838
            }
            else
            {

#line 838
                _S137 = false;

#line 838
            }

#line 838
            if(_S137)
            {
                uint2 _S138 = uint2(kernelContext_23->cyFrameView_0->frame_0->extentAndInverse_0.xy);
                uint2 _S139 = uint2(fragmentCentre_0);

#line 841
                float _S140 = viewDepthOf_0(relativePosition_2, kernelContext_23);

#line 841
                uint3 _S141 = clusterCoordOf_1(&kernelContext_23->cyFrameView_0->frame_0->clusterGrid_0, _S139, _S138, _S140);

#line 841
                uint _S142 = clusterIndexOf_1(&kernelContext_23->cyFrameView_0->frame_0->clusterGrid_0, _S141);

#line 841
                list_0 = kernelContext_23->cyFrameView_0->clusterHeaders_0[_S142 * kernelContext_23->cyFrameView_0->frame_0->counts_0.z + 1U];

#line 841
                mode_0 = 2U;

#line 838
            }
            else
            {

#line 838
                list_0 = _S135;

#line 838
                mode_0 = 0U;

#line 838
            }

#line 833
        }

#line 829
    }

#line 829
    uint slot_2 = 0U;

#line 849
    for(;;)
    {

#line 849
        if(slot_2 < (list_0.y))
        {
        }
        else
        {

#line 849
            break;
        }

#line 849
        uint rank_1;


        if(mode_0 == 1U)
        {

#line 852
            uint _S143 = cyDecalTableListEntry_0(list_0.x + slot_2, kernelContext_23);

#line 852
            rank_1 = _S143;

#line 852
        }
        else
        {

            if(mode_0 == 2U)
            {

#line 856
                rank_1 = kernelContext_23->cyFrameView_0->clusterIndices_0[list_0.x + slot_2];

#line 856
            }
            else
            {

#line 856
                rank_1 = slot_2;

#line 856
            }

#line 852
        }

#line 860
        if(rank_1 < _S134)
        {

#line 860
            thread CyDecalReceiver_0 _S144 = receiver_5;

#line 860
            cyApplyDecal_0(rank_1, &_S144, &decalled_0, kernelContext_23);

#line 860
        }

#line 849
        slot_2 = slot_2 + 1U;

#line 849
    }

#line 866
    surface_4->albedo_1 = (&decalled_0)->albedo_0;
    surface_4->roughness_1 = clamp((&decalled_0)->roughness_0, 0.01999999955296516, 1.0);
    surface_4->metallic_1 = saturate((&decalled_0)->metallic_0);
    surface_4->emission_1 = (&decalled_0)->emission_0;
    *normal_2 = (&decalled_0)->normal_0;
    return;
}


#line 42 "src/rendering/shaders/cy/light.slang"
float distanceAttenuation_0(float distanceSquared_0, float range_1)
{
    float _S145 = distanceSquared_0 / max(range_1 * range_1, 9.99999997475242708e-07);
    float _S146 = saturate(1.0 - _S145 * _S145);
    return _S146 * _S146 / max(distanceSquared_0, 0.00009999999747379);
}


#line 33
struct LightSample_0
{
    float3 direction_1;
    float3 illuminance_0;
    float attenuation_0;
};


#line 49
LightSample_0 evaluateLight_0(const Light_0 thread* light_0, float3 surfaceRelativeToCamera_0)
{
    thread LightSample_0 result_3;

#line 51
    uint _S147 = light_0->kind_0;
    if((light_0->kind_0) == 0U)
    {
        (&result_3)->direction_1 = - light_0->direction_0;
        (&result_3)->attenuation_0 = 1.0;
        (&result_3)->illuminance_0 = light_0->color_0 * float3(light_0->intensity_0) ;
        return result_3;
    }

    float3 _S148 = light_0->positionRelativeToCamera_0 - surfaceRelativeToCamera_0;
    float _S149 = dot(_S148, _S148);
    (&result_3)->direction_1 = _S148 * float3(rsqrt(max(_S149, 9.99999993922529029e-09))) ;
    (&result_3)->attenuation_0 = distanceAttenuation_0(_S149, light_0->range_0);

    if(_S147 == 2U)
    {

        float _S150 = saturate(dot(- (&result_3)->direction_1, light_0->direction_0) * light_0->spotScaleBias_0.x + light_0->spotScaleBias_0.y);
        (&result_3)->attenuation_0 = (&result_3)->attenuation_0 * (_S150 * _S150);

#line 65
    }

#line 71
    (&result_3)->illuminance_0 = light_0->color_0 * float3((light_0->intensity_0 * (&result_3)->attenuation_0)) ;
    return result_3;
}


#line 318 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_1, KernelContext_0 thread* kernelContext_24)
{
    float4 _S151 = float4(relative_1, 1.0);
    return float4(dot(kernelContext_24->cyFrameView_0->frame_0->shadowToClipRow0_0, _S151), dot(kernelContext_24->cyFrameView_0->frame_0->shadowToClipRow1_0, _S151), dot(kernelContext_24->cyFrameView_0->frame_0->shadowToClipRow2_0, _S151), dot(kernelContext_24->cyFrameView_0->frame_0->shadowToClipRow3_0, _S151));
}


#line 98 "src/rendering/shaders/cy/shadow.slang"
float shadowDiscRotation_0(float2 pixel_3)
{

    return fract(52.98291778564453125 * fract(dot(pixel_3, float2(0.06711056083440781, 0.00583714991807938)))) * 6.28318548202514648;
}


#line 549 "src/rendering/shaders/cy/frame.slang"
float softShadowVisibility_0(float2 uv_12, float reference_0, float2 pixel_4, KernelContext_0 thread* kernelContext_25)
{
    thread CyFrameShadowMap_0 map_0;
    (&map_0)->slot_0 = kernelContext_25->cyFrameView_0->frame_0->shadowControl_0.x;
    thread PcssShape_0 shape_2;
    (&shape_2)->penumbraPerDepth_0 = kernelContext_25->cyFrameView_0->frame_0->softShadowShape_0.x;
    (&shape_2)->minRadius_0 = kernelContext_25->cyFrameView_0->frame_0->softShadowShape_0.y;
    (&shape_2)->maxRadius_0 = kernelContext_25->cyFrameView_0->frame_0->softShadowShape_0.z;
    (&shape_2)->blockerTaps_0 = int(max(kernelContext_25->cyFrameView_0->frame_0->softShadowControl_0.z, 1U));
    (&shape_2)->filterTaps_0 = int(max(kernelContext_25->cyFrameView_0->frame_0->softShadowControl_0.w, 1U));
    float _S152 = shadowDiscRotation_0(pixel_4);

#line 559
    thread CyFrameShadowMap_0 _S153 = map_0;

#line 559
    thread PcssShape_0 _S154 = shape_2;

#line 559
    float _S155 = pcssVisibility_0(&_S153, uv_12, reference_0, &_S154, _S152, kernelContext_25);

#line 559
    return _S155;
}


#line 582
float directionalShadowVisibility_0(float3 relativePosition_3, float3 normal_3, float2 fragmentCentre_1, KernelContext_0 thread* kernelContext_26)
{

#line 582
    bool _S156;

    if((kernelContext_26->cyFrameView_0->frame_0->shadowControl_0.w) == 0U)
    {

#line 584
        _S156 = true;

#line 584
    }
    else
    {

#line 584
        _S156 = (kernelContext_26->cyFrameView_0->frame_0->shadowControl_0.x) == 4294967295U;

#line 584
    }

#line 584
    if(_S156)
    {
        return 1.0;
    }

#line 586
    float4 _S157 = transformToShadowClip_0(relativePosition_3 + normal_3 * float3(0.00499999988824129) , kernelContext_26);


    float _S158 = _S157.w;

#line 589
    if(_S158 <= 0.0)
    {
        return 1.0;
    }
    float3 _S159 = _S157.xyz / float3(_S158) ;
    float _S160 = _S159.x * 0.5 + 0.5;

#line 594
    float _S161 = 0.5 - _S159.y * 0.5;

#line 594
    float2 _S162 = float2(_S160, _S161);
    if(_S160 < 0.0)
    {

#line 595
        _S156 = true;

#line 595
    }
    else
    {

#line 595
        _S156 = _S160 > 1.0;

#line 595
    }

#line 595
    if(_S156)
    {

#line 595
        _S156 = true;

#line 595
    }
    else
    {

#line 595
        _S156 = _S161 < 0.0;

#line 595
    }

#line 595
    if(_S156)
    {

#line 595
        _S156 = true;

#line 595
    }
    else
    {

#line 595
        _S156 = _S161 > 1.0;

#line 595
    }

#line 595
    if(_S156)
    {
        return 1.0;
    }
    float _S163 = 1.0 / float(max(kernelContext_26->cyFrameView_0->frame_0->shadowControl_0.z, 1U));
    float _S164 = _S159.z + 0.00050000002374873;
    if(((kernelContext_26->cyFrameView_0->frame_0->softShadowControl_0.x) & 1U) != 0U)
    {

#line 601
        float _S165 = softShadowVisibility_0(_S162, _S164, fragmentCentre_1, kernelContext_26);

        return _S165;
    }

#line 603
    int y_0 = int(-1);

#line 603
    float visible_0 = 0.0;


    for(;;)
    {

#line 606
        if(y_0 <= int(1))
        {
        }
        else
        {

#line 606
            break;
        }

#line 606
        int x_0 = int(-1);

        for(;;)
        {

#line 608
            if(x_0 <= int(1))
            {
            }
            else
            {

#line 608
                break;
            }

#line 608
            float4 _S166 = cyMaterialSampleTextureLevel_0(kernelContext_26->cyFrameView_0->frame_0->shadowControl_0.x, _S162 + float2(float(x_0), float(y_0)) * float2(_S163) , 0.0, kernelContext_26);

#line 608
            float _S167;



            if(_S164 >= (_S166.x))
            {

#line 612
                _S167 = 1.0;

#line 612
            }
            else
            {

#line 612
                _S167 = 0.0;

#line 612
            }

#line 612
            float visible_1 = visible_0 + _S167;

#line 608
            x_0 = x_0 + int(1);

#line 608
            visible_0 = visible_1;

#line 608
        }

#line 606
        y_0 = y_0 + int(1);

#line 606
    }

#line 615
    return visible_0 / 9.0;
}


#line 565
float contactShadowVisibility_0(float2 fragmentCentre_2, KernelContext_0 thread* kernelContext_27)
{

#line 565
    bool _S168;

    if(((kernelContext_27->cyFrameView_0->frame_0->softShadowControl_0.x) & 2U) == 0U)
    {

#line 567
        _S168 = true;

#line 567
    }
    else
    {

#line 567
        _S168 = (kernelContext_27->cyFrameView_0->frame_0->softShadowControl_0.y) == 4294967295U;

#line 567
    }

#line 567
    if(_S168)
    {

        return 1.0;
    }

#line 570
    float4 _S169 = cyMaterialSampleTextureLevel_0(kernelContext_27->cyFrameView_0->frame_0->softShadowControl_0.y, fragmentCentre_2 * kernelContext_27->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_27);


    return _S169.x;
}


#line 61 "src/rendering/shaders/cy/brdf.slang"
float3 computeF0_0(float3 albedo_2, float metallic_2)
{
    return mix(float3(0.03999999910593033) , albedo_2, float3(metallic_2) );
}


#line 39
float3 diffuseLambert_0(float3 albedo_3)
{
    return albedo_3 * float3(0.31830987334251404) ;
}


#line 11
float distributionGgx_0(float normalDotHalf_0, float roughness_2)
{
    float _S170 = roughness_2 * roughness_2;
    float _S171 = _S170 * _S170;
    float _S172 = normalDotHalf_0 * normalDotHalf_0 * (_S171 - 1.0) + 1.0;
    return _S171 / max(3.14159274101257324 * _S172 * _S172, 1.00000001168609742e-07);
}


float visibilitySmithGgxCorrelated_0(float normalDotView_0, float normalDotLight_0, float roughness_3)
{

    float _S173 = roughness_3 * roughness_3;
    float _S174 = _S173 * _S173;
    float _S175 = 1.0 - _S174;



    return 0.5 / max(normalDotLight_0 * sqrt(normalDotView_0 * normalDotView_0 * _S175 + _S174) + normalDotView_0 * sqrt(normalDotLight_0 * normalDotLight_0 * _S175 + _S174), 1.00000001168609742e-07);
}

float3 fresnelSchlick_0(float3 f0_0, float viewDotHalf_0)
{

    return f0_0 + (float3(1.0)  - f0_0) * float3(pow(saturate(1.0 - viewDotHalf_0), 5.0)) ;
}


#line 45
float3 specularGgx_0(float3 normal_4, float3 view_0, float3 light_1, float roughness_4, float3 f0_1)
{
    float3 _S176 = normalize(view_0 + light_1);

#line 56
    return float3((distributionGgx_0(saturate(dot(normal_4, _S176)), roughness_4) * visibilitySmithGgxCorrelated_0(saturate(dot(normal_4, view_0)) + 0.00000999999974738, saturate(dot(normal_4, light_1)), roughness_4)))  * fresnelSchlick_0(f0_1, saturate(dot(view_0, _S176)));
}


#line 75 "src/rendering/shaders/cy/material.slang"
float3 shadeSurfaceWithLight_0(const Surface_0 thread* surface_5, float3 worldNormal_0, float3 viewDirection_0, const LightSample_0 thread* sample_0)
{

#line 76
    float3 _S177 = sample_0->direction_1;

    float _S178 = saturate(dot(worldNormal_0, sample_0->direction_1));
    if(_S178 <= 0.0)
    {
        return float3(0.0) ;
    }

#line 87
    return (diffuseLambert_0(surface_5->albedo_1 * float3((1.0 - surface_5->metallic_1)) ) + specularGgx_0(worldNormal_0, viewDirection_0, _S177, surface_5->roughness_1, computeF0_0(surface_5->albedo_1, surface_5->metallic_1))) * sample_0->illuminance_0 * float3(_S178) ;
}


#line 618 "src/rendering/shaders/cy/frame.slang"
float3 accumulateLights_0(const Surface_0 thread* surface_6, float3 relativePosition_4, float3 normal_5, float3 viewDir_0, float2 fragmentCentre_3, uint instanceFlags_0, KernelContext_0 thread* kernelContext_28)
{

#line 619
    bool _S179;

    uint2 _S180 = uint2(fragmentCentre_3);
    float3 _S181 = float3(0.0) ;
    uint _S182 = kernelContext_28->cyFrameView_0->frame_0->counts_0.x;

#line 623
    uint global_0 = 0U;

#line 623
    float3 lit_2 = _S181;

#line 632
    for(;;)
    {

#line 632
        if(global_0 < _S182)
        {
        }
        else
        {

#line 632
            break;
        }
        if((kernelContext_28->cyFrameView_0->lights_0[global_0].kind_0) != 0U)
        {
            global_0 = global_0 + 1U;

#line 632
            continue;
        }

#line 632
        thread Light_0 _S183 = kernelContext_28->cyFrameView_0->lights_0[global_0];

#line 632
        LightSample_0 _S184 = evaluateLight_0(&_S183, relativePosition_4);

#line 642
        if(global_0 == (kernelContext_28->cyFrameView_0->frame_0->shadowControl_0.y))
        {

#line 642
            _S179 = (instanceFlags_0 & 8U) != 0U;

#line 642
        }
        else
        {

#line 642
            _S179 = false;

#line 642
        }

#line 642
        float _S185;
        if(_S179)
        {

#line 643
            float _S186 = directionalShadowVisibility_0(relativePosition_4, normal_5, fragmentCentre_3, kernelContext_28);

#line 643
            float _S187 = contactShadowVisibility_0(fragmentCentre_3, kernelContext_28);

#line 643
            _S185 = min(_S186, _S187);

#line 643
        }
        else
        {

#line 643
            _S185 = 1.0;

#line 643
        }

#line 643
        thread LightSample_0 _S188 = _S184;

#line 643
        float3 _S189 = shadeSurfaceWithLight_0(surface_6, normal_5, viewDir_0, &_S188);

#line 643
        lit_2 = lit_2 + _S189 * float3(_S185) ;

#line 632
        global_0 = global_0 + 1U;

#line 632
    }

#line 650
    if((kernelContext_28->cyFrameView_0->frame_0->counts_0.w) == 0U)
    {

#line 650
        _S179 = true;

#line 650
    }
    else
    {

#line 650
        _S179 = ((&kernelContext_28->cyFrameView_0->frame_0->clusterGrid_0)->dimensions_0.z) == 0U;

#line 650
    }

#line 650
    uint index_11;

#line 650
    if(_S179)
    {

#line 650
        index_11 = 0U;

        for(;;)
        {

#line 652
            if(index_11 < _S182)
            {
            }
            else
            {

#line 652
                break;
            }
            if((kernelContext_28->cyFrameView_0->lights_0[index_11].kind_0) == 0U)
            {
                index_11 = index_11 + 1U;

#line 652
                continue;
            }

#line 652
            thread Light_0 _S190 = kernelContext_28->cyFrameView_0->lights_0[index_11];

#line 652
            LightSample_0 _S191 = evaluateLight_0(&_S190, relativePosition_4);

#line 652
            thread LightSample_0 _S192 = _S191;

#line 652
            float3 _S193 = shadeSurfaceWithLight_0(surface_6, normal_5, viewDir_0, &_S192);

#line 652
            lit_2 = lit_2 + _S193;

#line 652
            index_11 = index_11 + 1U;

#line 652
        }

#line 661
        return lit_2;
    }

    uint2 _S194 = uint2(kernelContext_28->cyFrameView_0->frame_0->extentAndInverse_0.xy);

#line 664
    float _S195 = viewDepthOf_0(relativePosition_4, kernelContext_28);

#line 664
    uint3 _S196 = clusterCoordOf_1(&kernelContext_28->cyFrameView_0->frame_0->clusterGrid_0, _S180, _S194, _S195);

#line 664
    uint _S197 = clusterIndexOf_1(&kernelContext_28->cyFrameView_0->frame_0->clusterGrid_0, _S196);

#line 669
    uint2 _S198 = kernelContext_28->cyFrameView_0->clusterHeaders_0[_S197 * kernelContext_28->cyFrameView_0->frame_0->counts_0.z];

#line 669
    index_11 = 0U;
    for(;;)
    {

#line 670
        if(index_11 < (_S198.y))
        {
        }
        else
        {

#line 670
            break;
        }
        uint _S199 = kernelContext_28->cyFrameView_0->clusterIndices_0[_S198.x + index_11];
        if(_S199 >= _S182)
        {

#line 673
            _S179 = true;

#line 673
        }
        else
        {

#line 673
            _S179 = (kernelContext_28->cyFrameView_0->lights_0[_S199].kind_0) == 0U;

#line 673
        }

#line 673
        if(_S179)
        {
            index_11 = index_11 + 1U;

#line 670
            continue;
        }

#line 670
        thread Light_0 _S200 = kernelContext_28->cyFrameView_0->lights_0[_S199];

#line 670
        LightSample_0 _S201 = evaluateLight_0(&_S200, relativePosition_4);

#line 670
        thread LightSample_0 _S202 = _S201;

#line 670
        float3 _S203 = shadeSurfaceWithLight_0(surface_6, normal_5, viewDir_0, &_S202);

#line 670
        lit_2 = lit_2 + _S203;

#line 670
        index_11 = index_11 + 1U;

#line 670
    }

#line 680
    return lit_2;
}


#line 700
float4 probeVolumeTexel_0(uint3 probe_0, uint texel_1, KernelContext_0 thread* kernelContext_29)
{
    uint3 _S204 = kernelContext_29->cyFrameView_0->frame_0->probeVolumeControl_0.yzw;

    uint _S205 = _S204.y;

#line 704
    float4 _S206 = cyMaterialSampleTextureLevel_0(kernelContext_29->cyFrameView_0->frame_0->probeVolumeControl_0.x, (float2(float(probe_0.x * 5U + texel_1), float(probe_0.y + _S205 * probe_0.z)) + float2(0.5) ) / float2(float(_S204.x * 5U), float(_S205 * _S204.z)), 0.0, kernelContext_29);


    return _S206;
}



float probeVisibility_0(float4 distances0_0, float4 distances1_0, float3 probePosition_0, float3 position_1, float3 normal_6, float3 query_0, KernelContext_0 thread* kernelContext_30)
{

    float3 _S207 = probePosition_0 - position_1;
    float _S208 = dot(_S207, _S207);

#line 716
    float3 _S209;
    if(_S208 > 9.999999960041972e-13)
    {

#line 717
        _S209 = _S207 * float3(rsqrt(_S208)) ;

#line 717
    }
    else
    {

#line 717
        _S209 = normal_6;

#line 717
    }
    float _S210 = max(0.00009999999747379, (dot(_S209, normal_6) + 1.0) * 0.5);
    float _S211 = _S210 * _S210 + 0.20000000298023224;



    float3 _S212 = query_0 - probePosition_0;

#line 723
    float _S213;
    if((_S212.x) >= 0.0)
    {

#line 724
        _S213 = distances0_0.y;

#line 724
    }
    else
    {

#line 724
        _S213 = distances0_0.z;

#line 724
    }

#line 724
    float _S214;
    if((_S212.y) >= 0.0)
    {

#line 725
        _S214 = distances0_0.w;

#line 725
    }
    else
    {

#line 725
        _S214 = distances1_0.x;

#line 725
    }

#line 725
    float _S215;
    if((_S212.z) >= 0.0)
    {

#line 726
        _S215 = distances1_0.y;

#line 726
    }
    else
    {

#line 726
        _S215 = distances1_0.z;

#line 726
    }

#line 726
    float3 _S216 = float3(kernelContext_30->cyFrameView_0->frame_0->probeVolumeParams_0.z) ;

    float3 _S217 = saturate((float3(_S213, _S214, _S215) + _S216 - abs(_S212)) / _S216);
    return _S211 * min(_S217.x, min(_S217.y, _S217.z));
}



float3 probeVolumeAmbient_0(float3 position_2, float3 normal_7, float3 flat_0, KernelContext_0 thread* kernelContext_31)
{
    uint3 _S218 = kernelContext_31->cyFrameView_0->frame_0->probeVolumeControl_0.yzw;

    float3 _S219 = position_2 + normal_7 * float3(kernelContext_31->cyFrameView_0->frame_0->probeVolumeParams_0.y) ;

#line 738
    float3 _S220 = float3(kernelContext_31->cyFrameView_0->frame_0->probeVolumeOrigin_0.w) ;
    float3 _S221 = (_S219 - kernelContext_31->cyFrameView_0->frame_0->probeVolumeOrigin_0.xyz) / _S220;

#line 739
    float3 _S222 = float3(1.0) ;
    float3 _S223 = float3(_S218) - _S222;
    float3 _S224 = float3(0.0) ;

#line 741
    float3 _S225 = max(max(- _S221, _S221 - _S223), _S224);
    float _S226 = saturate(1.0 - max(_S225.x, max(_S225.y, _S225.z)));
    if(_S226 <= 0.0)
    {
        return flat_0;
    }
    float3 _S227 = clamp(_S221, _S224, _S223);
    uint3 _S228 = min(uint3(floor(_S227)), uint3(max(int3(_S218) - int3(int(2)) , int3(int(0)) )));
    float3 _S229 = _S227 - float3(_S228);


    float4 _S230 = float4(0.28209498524665833, 0.32573533058166504 * normal_7.y, 0.32573533058166504 * normal_7.z, 0.32573533058166504 * normal_7.x);

#line 752
    uint corner_1 = 0U;

#line 752
    float3 total_0 = _S224;

#line 752
    float weightSum_0 = 0.0;



    for(;;)
    {

#line 756
        if(corner_1 < 8U)
        {
        }
        else
        {

#line 756
            break;
        }
        uint3 _S231 = uint3(corner_1 & 1U, (corner_1 >> 1U) & 1U, (corner_1 >> 2U) & 1U);
        uint3 _S232 = min(_S228 + _S231, _S218 - uint3(1U) );
        float3 _S233 = float3(_S231);
        float3 _S234 = _S233 * _S229 + (_S222 - _S233) * (_S222 - _S229);

#line 761
        float4 _S235 = probeVolumeTexel_0(_S232, 3U, kernelContext_31);

        float _S236 = _S234.x * _S234.y * _S234.z * _S235.x;
        if(_S236 <= 0.0)
        {
            corner_1 = corner_1 + 1U;

#line 756
            continue;
        }

#line 756
        float4 _S237 = probeVolumeTexel_0(_S232, 4U, kernelContext_31);

#line 756
        float _S238 = probeVisibility_0(_S235, _S237, kernelContext_31->cyFrameView_0->frame_0->probeVolumeOrigin_0.xyz + float3(_S232) * _S220, position_2, normal_7, _S219, kernelContext_31);

#line 771
        float _S239 = _S236 * _S238;

#line 771
        float4 _S240 = probeVolumeTexel_0(_S232, 0U, kernelContext_31);
        float _S241 = dot(_S240, _S230);

#line 772
        float4 _S242 = probeVolumeTexel_0(_S232, 1U, kernelContext_31);
        float _S243 = dot(_S242, _S230);

#line 773
        float4 _S244 = probeVolumeTexel_0(_S232, 2U, kernelContext_31);



        float weightSum_1 = weightSum_0 + _S239;

#line 777
        total_0 = total_0 + max(float3(_S241, _S243, dot(_S244, _S230)) * float3(kernelContext_31->cyFrameView_0->frame_0->probeVolumeParams_0.x) , _S224) * float3(_S239) ;

#line 777
        weightSum_0 = weightSum_1;

#line 756
        corner_1 = corner_1 + 1U;

#line 756
    }

#line 779
    if(weightSum_0 > 9.99999997475242708e-07)
    {

#line 779
        total_0 = total_0 / float3(weightSum_0) ;

#line 779
    }
    else
    {

#line 779
        total_0 = flat_0;

#line 779
    }
    return mix(flat_0, total_0, float3(_S226) );
}


#line 685
float occlusionVisibility_0(float2 fragmentCentre_4, KernelContext_0 thread* kernelContext_32)
{

#line 685
    float4 _S245 = cyMaterialSampleTextureLevel_0(kernelContext_32->cyFrameView_0->frame_0->occlusionControl_0.x, fragmentCentre_4 * kernelContext_32->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_32);


    return _S245.w;
}


#line 688
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 688
struct pixelInput_0
{
    float3 relativePosition_5 [[user(TEXCOORD)]];
    float3 normal_8 [[user(TEXCOORD_1)]];
    float2 uv_13 [[user(TEXCOORD_2)]];
    [[flat]] uint drawIndex_0 [[user(TEXCOORD_3)]];
};


#line 874
[[fragment]] pixelOutput_0 cyForwardFragment(pixelInput_0 _S246 [[stage_in]], float4 position_3 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyFrameGlobalSet_default_0 constant& cyFrameGlobals_1 [[buffer(0)]])
{

#line 874
    thread KernelContext_0 kernelContext_33;

#line 874
    (&kernelContext_33)->cyFrameView_0 = cyFrameView_1;

#line 874
    (&kernelContext_33)->cyFrameGlobals_0 = &cyFrameGlobals_1;

    CyDrawInstance_0 _S247 = cyFrameView_1->drawInstances_0[_S246.drawIndex_0];

#line 876
    Surface_0 _S248 = surfaceOf_0(_S247.material_0, cyFrameView_1->instances_0[_S247.instanceSlot_0].tint_0.xyz, _S246.uv_13, &kernelContext_33);

    thread Surface_0 surface_7 = _S248;

    thread float3 normal_9 = normalize(_S246.normal_8);



    if(((&kernelContext_33)->cyFrameView_0->frame_0->decalControl_0.x) != 4294967295U)
    {

#line 884
        applyFrameDecals_0(&surface_7, &normal_9, _S246.relativePosition_5, position_3.xy, _S247.layerMask_0, &kernelContext_33);

#line 884
    }

#line 891
    float3 _S249 = normalize(- _S246.relativePosition_5);


    float2 _S250 = position_3.xy;

#line 894
    thread Surface_0 _S251 = surface_7;

#line 894
    float3 _S252 = accumulateLights_0(&_S251, _S246.relativePosition_5, normal_9, _S249, _S250, _S247.flags_0, &kernelContext_33);

#line 901
    float3 ambientRadiance_0 = (&kernelContext_33)->cyFrameView_0->frame_0->ambientAndOcclusion_0.xyz;

#line 901
    float3 ambientRadiance_1;
    if(((&kernelContext_33)->cyFrameView_0->frame_0->probeVolumeControl_0.x) != 4294967295U)
    {

#line 902
        float3 _S253 = probeVolumeAmbient_0(_S246.relativePosition_5, normal_9, ambientRadiance_0, &kernelContext_33);

#line 902
        ambientRadiance_1 = _S253;

#line 902
    }
    else
    {

#line 902
        ambientRadiance_1 = ambientRadiance_0;

#line 902
    }



    float3 ambient_0 = (&surface_7)->albedo_1 * ambientRadiance_1 * float3((&surface_7)->occlusion_0) ;

#line 906
    float3 color_1;

#line 906
    float3 ambient_1;
    if(((&kernelContext_33)->cyFrameView_0->frame_0->occlusionControl_0.x) != 4294967295U)
    {

#line 907
        float _S254 = occlusionVisibility_0(_S250, &kernelContext_33);


        float3 ambient_2 = ambient_0 * float3(_S254) ;
        if(((&kernelContext_33)->cyFrameView_0->frame_0->occlusionControl_0.y) != 0U)
        {

#line 911
            color_1 = _S252 * float3(mix(1.0, _S254, (as_type<float>(((&kernelContext_33)->cyFrameView_0->frame_0->occlusionControl_0.z))))) ;

#line 911
        }
        else
        {

#line 911
            color_1 = _S252;

#line 911
        }

#line 911
        ambient_1 = ambient_2;

#line 907
    }
    else
    {

#line 907
        color_1 = _S252;

#line 907
        ambient_1 = ambient_0;

#line 907
    }

#line 907
    pixelOutput_0 _S255 = { float4(color_1 + (&surface_7)->emission_1 + ambient_1, (&surface_7)->opacity_0) };

#line 918
    return _S255;
}

)cy_msl";

/// ResolveVertex.metal, 936 bytes.
inline constexpr char kFrameResolveVertexMsl[] = R"cy_msl(#include <metal_stdlib>
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

/// ResolveFragment.metal, 3670 bytes.
inline constexpr char kFrameResolveFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 28 "src/rendering/shaders/cy/tonemap.slang"
float3 applyExposure_0(float3 linear_0, float exposureStops_0)
{
    return linear_0 * float3(exp2(exposureStops_0)) ;
}


#line 96 "src/rendering/shaders/cy/fullscreen.slang"
constant int fc_kTonemapOperator_0 [[function_constant(0)]];
constant int kTonemapOperator_0 = is_function_constant_defined(fc_kTonemapOperator_0) ? fc_kTonemapOperator_0 : int(1);

#line 8 "src/rendering/shaders/cy/tonemap.slang"
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


#line 90 "core"
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 90
struct pixelInput_0
{
    float2 uv_0 [[user(TEXCOORD)]];
};


#line 81 "src/rendering/shaders/cy/fullscreen.slang"
struct FullscreenPassSet_default_0
{
    texture2d<float, access::sample> sceneColor_0;
    sampler linearClamp_0;
    texture2d<float, access::sample> historyColor_0;
    texture2d<float, access::sample> velocity_0;
    texture2d<float, access::sample> depth_0;
};


#line 15
struct FullscreenGlobalsData_0
{
    float timeSeconds_0;
    float deltaSeconds_0;
    float exposureStops_1;
    float windStrength_0;
    float4 windDirectionAndSpeed_0;
};


#line 1055 "core"
struct FullscreenGlobalSet_default_0
{
    FullscreenGlobalsData_0 constant* data_0;
};


#line 34 "src/rendering/shaders/cy/fullscreen.slang"
struct FullscreenViewData_0
{
    array<float4, int(14)> rows_0;
    array<uint4, int(4)> words_0;
    float4 temporalFeedback_0;
    float4 temporalJitter_0;
};


#line 34
struct FullscreenViewSet_default_0
{
    FullscreenViewData_0 constant* frame_0;
};


#line 34
struct KernelContext_0
{
    FullscreenPassSet_default_0 constant* passSet_0;
    FullscreenGlobalSet_default_0 constant* globalSet_0;
    FullscreenViewSet_default_0 constant* viewSet_0;
};


#line 103
[[fragment]] pixelOutput_0 fullscreenResolve(pixelInput_0 _S2 [[stage_in]], float4 position_0 [[position]], FullscreenPassSet_default_0 constant* passSet_1 [[buffer(2)]], FullscreenGlobalSet_default_0 constant* globalSet_1 [[buffer(0)]], FullscreenViewSet_default_0 constant* viewSet_1 [[buffer(1)]])
{

#line 103
    thread KernelContext_0 kernelContext_0;

#line 103
    (&kernelContext_0)->passSet_0 = passSet_1;

#line 103
    (&kernelContext_0)->globalSet_0 = globalSet_1;

#line 103
    (&kernelContext_0)->viewSet_0 = viewSet_1;


    float3 _S3 = applyExposure_0(((passSet_1->sceneColor_0).sample((passSet_1->linearClamp_0), (_S2.uv_0))).xyz, globalSet_1->data_0->exposureStops_1);

#line 106
    float3 mapped_0;


    if(kTonemapOperator_0 == int(1))
    {

#line 109
        mapped_0 = tonemapReinhard_0(_S3, 4.0);

#line 109
    }
    else
    {

        if(kTonemapOperator_0 == int(2))
        {

#line 113
            mapped_0 = tonemapAcesApproximate_0(_S3);

#line 113
        }
        else
        {

#line 113
            mapped_0 = _S3;

#line 113
        }

#line 109
    }

#line 120
    if(((&kernelContext_0)->viewSet_0->frame_0->rows_0[int(13)].x) < 0.0)
    {

#line 120
        mapped_0 = - mapped_0;

#line 120
    }

#line 120
    pixelOutput_0 _S4 = { float4(mapped_0, 1.0) };

    return _S4;
}

)cy_msl";

/// TemporalFragment.metal, 3816 bytes.
inline constexpr char kFrameTemporalFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 90 "core"
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 90
struct pixelInput_0
{
    float2 uv_0 [[user(TEXCOORD)]];
};


#line 34 "src/rendering/shaders/cy/fullscreen.slang"
struct FullscreenViewData_0
{
    array<float4, int(14)> rows_0;
    array<uint4, int(4)> words_0;
    float4 temporalFeedback_0;
    float4 temporalJitter_0;
};


#line 1055 "core"
struct FullscreenViewSet_default_0
{
    FullscreenViewData_0 constant* frame_0;
};


#line 1055
struct FullscreenPassSet_default_0
{
    texture2d<float, access::sample> sceneColor_0;
    sampler linearClamp_0;
    texture2d<float, access::sample> historyColor_0;
    texture2d<float, access::sample> velocity_0;
    texture2d<float, access::sample> depth_0;
};


#line 1055
struct KernelContext_0
{
    FullscreenViewSet_default_0 constant* viewSet_0;
    FullscreenPassSet_default_0 constant* passSet_0;
};


#line 130 "src/rendering/shaders/cy/fullscreen.slang"
[[fragment]] pixelOutput_0 temporalResolve(pixelInput_0 _S1 [[stage_in]], float4 position_0 [[position]], FullscreenViewSet_default_0 constant* viewSet_1 [[buffer(1)]], FullscreenPassSet_default_0 constant* passSet_1 [[buffer(2)]])
{

#line 130
    thread KernelContext_0 kernelContext_0;

#line 130
    (&kernelContext_0)->viewSet_0 = viewSet_1;

#line 130
    (&kernelContext_0)->passSet_0 = passSet_1;

    float2 _S2 = viewSet_1->frame_0->rows_0[int(13)].zw;
    float4 _S3 = viewSet_1->frame_0->temporalFeedback_0;
    float4 _S4 = ((passSet_1->sceneColor_0).sample((passSet_1->linearClamp_0), (_S1.uv_0), level((0.0))));

    float3 _S5 = _S4.xyz;

#line 136
    float3 minimum_0 = _S5;

#line 136
    float3 maximum_0 = _S5;

#line 136
    int y_0 = int(-1);

    for(;;)
    {

#line 138
        if(y_0 <= int(1))
        {
        }
        else
        {

#line 138
            break;
        }

#line 138
        int x_0 = int(-1);

        for(;;)
        {

#line 140
            if(x_0 <= int(1))
            {
            }
            else
            {

#line 140
                break;
            }

            float3 _S6 = ((passSet_1->sceneColor_0).sample((passSet_1->linearClamp_0), (_S1.uv_0 + float2(float(x_0), float(y_0)) * _S2), level((0.0)))).xyz;
            float3 _S7 = min(minimum_0, _S6);
            float3 _S8 = max(maximum_0, _S6);

#line 140
            int x_1 = x_0 + int(1);

#line 140
            minimum_0 = _S7;

#line 140
            maximum_0 = _S8;

#line 140
            x_0 = x_1;

#line 140
        }

#line 138
        y_0 = y_0 + int(1);

#line 138
    }

#line 159
    float _S9 = (((&kernelContext_0)->passSet_0->depth_0).sample((passSet_1->linearClamp_0), (_S1.uv_0), level((0.0))).x);
    float2 _S10 = _S1.uv_0 + (((&kernelContext_0)->passSet_0->velocity_0).sample((passSet_1->linearClamp_0), (_S1.uv_0), level((0.0))).xy);

#line 160
    bool _S11;
    if(all(_S10 >= (float2(0.0) )))
    {

#line 161
        _S11 = all(_S10 <= (float2(1.0) ));

#line 161
    }
    else
    {

#line 161
        _S11 = false;

#line 161
    }

    float3 _S12 = clamp((((&kernelContext_0)->passSet_0->historyColor_0).sample((passSet_1->linearClamp_0), (_S10), level((0.0)))).xyz, minimum_0, maximum_0);
    if((_S3.x) > 0.5)
    {
    }
    else
    {

#line 164
        _S11 = false;

#line 164
    }

#line 164
    if(_S11)
    {

#line 164
        _S11 = _S9 > 0.0;

#line 164
    }
    else
    {

#line 164
        _S11 = false;

#line 164
    }

#line 164
    float _S13;

#line 164
    if(_S11)
    {

#line 164
        _S13 = _S3.y;

#line 164
    }
    else
    {

#line 164
        _S13 = 0.0;

#line 164
    }

#line 164
    pixelOutput_0 _S14 = { float4(mix(_S5, _S12, float3(_S13) ), _S4.w) };
    return _S14;
}

)cy_msl";

}  // namespace cy::rendering::pipeline

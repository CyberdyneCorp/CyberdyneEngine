#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the rendering frame. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::pipeline {

/// DepthVertex.metal, 6560 bytes.
inline constexpr char kFrameDepthVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 236 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 316
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
    uint4 volumetricFogControl_0;
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


#line 223 "src/rendering/shaders/cy/frame.slang"
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


#line 245
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


#line 290
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


#line 300 "src/rendering/shaders/cy/frame.slang"
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


#line 325 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 327
struct cyDepthVertex_Result_0
{
    float4 position_0 [[position]];
    float3 normal_1 [[user(TEXCOORD)]];
    float4 currentClip_0 [[user(TEXCOORD_1)]];
    float4 previousClip_0 [[user(TEXCOORD_2)]];
};


#line 327
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
};


#line 443
struct CyDepthVertex_0
{
    float4 position_1;
    float3 normal_2;
    float4 currentClip_1;
    float4 previousClip_1;
};


#line 443
[[vertex]] cyDepthVertex_Result_0 cyDepthVertex(vertexInput_0 _S9 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 443
    thread KernelContext_0 kernelContext_1;

#line 443
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 443
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 455
    CyInstanceTransform_0 _S10 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

#line 455
    thread CyInstanceTransform_0 _S11 = _S10;

#line 455
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    thread CyDepthVertex_0 output_0;

#line 456
    float4 _S13 = transformToClip_0(_S12, &kernelContext_1);

    (&output_0)->position_1 = _S13;
    (&output_0)->currentClip_1 = _S13;
    float4 _S14 = float4(_S12, 1.0);
    (&output_0)->previousClip_1 = float4(dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow0_0, _S14), dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow1_0, _S14), dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow2_0, _S14), dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow3_0, _S14));



    float3 _S15 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 465
    thread CyInstanceTransform_0 _S16 = _S10;

#line 465
    float3 _S17 = rotateToRelative_0(&_S16, _S15);

#line 465
    (&output_0)->normal_2 = _S17;

#line 465
    thread cyDepthVertex_Result_0 _S18;

#line 465
    (&_S18)->position_0 = output_0.position_1;

#line 465
    (&_S18)->normal_1 = output_0.normal_2;

#line 465
    (&_S18)->currentClip_0 = output_0.currentClip_1;

#line 465
    (&_S18)->previousClip_0 = output_0.previousClip_1;

#line 465
    return _S18;
}

)cy_msl";

/// DepthFragment.metal, 3980 bytes.
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


#line 469 "src/rendering/shaders/cy/frame.slang"
struct CyDepthOutput_0
{
    float4 normalRoughness_0 [[color(0)]];
    float2 velocity_0 [[color(1)]];
};


#line 469
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
    uint4 volumetricFogControl_0;
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


#line 223 "src/rendering/shaders/cy/frame.slang"
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


#line 476
[[fragment]] CyDepthOutput_0 cyDepthFragment(pixelInput_0 _S4 [[stage_in]], float4 position_0 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_0 [[buffer(1)]])
{
    thread CyDepthOutput_0 output_0;
    (&output_0)->normalRoughness_0 = float4(encodeOctahedral_0(normalize(_S4.normal_1)), 1.0, 1.0);

#line 487
    float2 _S5 = _S4.currentClip_0.xy / float2(_S4.currentClip_0.w)  - cyFrameView_0->frame_0->temporalJitter_0.xy * float2(2.0)  * cyFrameView_0->frame_0->extentAndInverse_0.zw;

    float2 _S6 = _S4.previousClip_0.xy / float2(_S4.previousClip_0.w) ;
    (&output_0)->velocity_0 = float2((_S6.x - _S5.x) * 0.5, (_S5.y - _S6.y) * 0.5);

    return output_0;
}

)cy_msl";

/// ShadowVertex.metal, 4569 bytes.
inline constexpr char kFrameShadowVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 236 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 316
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
    uint4 volumetricFogControl_0;
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


#line 223 "src/rendering/shaders/cy/frame.slang"
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


#line 245
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


#line 290
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


#line 307 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow3_0, _S2));
}


#line 310
struct cyShadowVertex_Result_0
{
    float4 position_0 [[position]];
};


#line 310
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
};


#line 418
struct CyShadowVertex_0
{
    float4 position_1;
};


#line 418
[[vertex]] cyShadowVertex_Result_0 cyShadowVertex(vertexInput_0 _S3 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 418
    thread KernelContext_0 kernelContext_1;

#line 418
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 418
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 428
    thread CyShadowVertex_0 output_0;

#line 428
    thread CyInstanceTransform_0 _S4 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

#line 428
    float3 _S5 = transformToRelative_0(&_S4, _S3.modelPosition_1);

#line 428
    float4 _S6 = transformToShadowClip_0(_S5, &kernelContext_1);
    (&output_0)->position_1 = _S6;

#line 429
    thread cyShadowVertex_Result_0 _S7;

#line 429
    (&_S7)->position_0 = output_0.position_1;

#line 429
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


#line 434 "src/rendering/shaders/cy/frame.slang"
[[fragment]] pixelOutput_0 cyShadowFragment(float4 position_0 [[position]])
{

#line 434
    pixelOutput_0 _S1 = { position_0.z };

    return _S1;
}

)cy_msl";

/// ForwardVertex.metal, 6394 bytes.
inline constexpr char kFrameForwardVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 236 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 316
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
    uint4 volumetricFogControl_0;
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


#line 223 "src/rendering/shaders/cy/frame.slang"
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


#line 245
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


#line 290
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


#line 300 "src/rendering/shaders/cy/frame.slang"
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


#line 325 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 327
struct cyForwardVertex_Result_0
{
    float4 position_0 [[position]];
    float3 relativePosition_0 [[user(TEXCOORD)]];
    float3 normal_1 [[user(TEXCOORD_1)]];
    float2 uv_0 [[user(TEXCOORD_2)]];
    uint drawIndex_1 [[user(TEXCOORD_3)]];
};


#line 327
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
    float2 uv_1 [[attribute(2)]];
};


#line 497
struct CyForwardVertex_0
{
    float4 position_1;
    float3 relativePosition_1;
    float3 normal_2;
    float2 uv_2;
    [[flat]] uint drawIndex_2;
};


#line 497
[[vertex]] cyForwardVertex_Result_0 cyForwardVertex(vertexInput_0 _S9 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 497
    thread KernelContext_0 kernelContext_1;

#line 497
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 497
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 511
    CyInstanceTransform_0 _S10 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

    thread CyForwardVertex_0 output_0;

#line 513
    thread CyInstanceTransform_0 _S11 = _S10;

#line 513
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    (&output_0)->relativePosition_1 = _S12;

#line 514
    float4 _S13 = transformToClip_0(_S12, &kernelContext_1);
    (&output_0)->position_1 = _S13;



    float3 _S14 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 519
    thread CyInstanceTransform_0 _S15 = _S10;

#line 519
    float3 _S16 = rotateToRelative_0(&_S15, _S14);

#line 519
    (&output_0)->normal_2 = _S16;
    (&output_0)->uv_2 = _S9.uv_1;
    (&output_0)->drawIndex_2 = cyDraw_1->drawIndex_0;

#line 521
    thread cyForwardVertex_Result_0 _S17;

#line 521
    (&_S17)->position_0 = output_0.position_1;

#line 521
    (&_S17)->relativePosition_0 = output_0.relativePosition_1;

#line 521
    (&_S17)->normal_1 = output_0.normal_2;

#line 521
    (&_S17)->uv_0 = output_0.uv_2;

#line 521
    (&_S17)->drawIndex_1 = output_0.drawIndex_2;

#line 521
    return _S17;
}

)cy_msl";

/// ForwardFragment.metal, 43722 bytes.
inline constexpr char kFrameForwardFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 778 "src/rendering/shaders/cy/frame.slang"
struct CyFrameFogVolume_0
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
    uint4 volumetricFogControl_0;
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


#line 223 "src/rendering/shaders/cy/frame.slang"
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


#line 4463 "hlsl.meta.slang"
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


#line 781 "src/rendering/shaders/cy/frame.slang"
float4 CyFrameFogVolume_texel_0(const CyFrameFogVolume_0 thread* this_0, int2 coordinate_0, KernelContext_0 thread* kernelContext_0)
{

    int3 _S1 = int3(coordinate_0, int(0));

#line 784
    return ((kernelContext_0->cyFrameGlobals_0->textures_0[this_0->slot_0]).read(vec<uint,2>(((_S1)).xy), uint(((_S1)).z)));
}


#line 65 "src/rendering/shaders/cy/volumetric_fog.slang"
float3 cyFogLerp_0(float3 a_0, float3 b_0, float t_0)
{
    return a_0 + (b_0 - a_0) * float3(t_0) ;
}


#line 76
float3 cyFogPlane_0(const CyFrameFogVolume_0 thread* source_0, float4 shape_0, uint slice_0, float2 texel_0, uint which_0, KernelContext_0 thread* kernelContext_1)
{

    float _S2 = shape_0.x;

#line 79
    uint width_0 = uint(_S2);
    float _S3 = shape_0.y;

#line 80
    uint height_0 = uint(_S3);
    float2 clamped_0 = clamp(texel_0, float2(0.0) , float2(_S2 - 1.0, _S3 - 1.0));
    float _S4 = clamped_0.x;

#line 82
    uint x0_0 = uint(_S4);
    float _S5 = clamped_0.y;

#line 83
    uint y0_0 = uint(_S5);
    uint _S6 = min(x0_0 + 1U, width_0 - 1U);

    float tx_0 = _S4 - float(x0_0);
    float ty_0 = _S5 - float(y0_0);
    int column_0 = int(which_0 * width_0);
    uint _S7 = 1U + slice_0 * height_0;

#line 89
    int row0_1 = int(_S7 + y0_0);
    int row1_1 = int(_S7 + min(y0_0 + 1U, height_0 - 1U));
    int _S8 = column_0 + int(x0_0);

#line 91
    float4 _S9 = CyFrameFogVolume_texel_0(source_0, int2(_S8, row0_1), kernelContext_1);

#line 91
    float3 _S10 = _S9.xyz;
    int _S11 = column_0 + int(_S6);

#line 92
    float4 _S12 = CyFrameFogVolume_texel_0(source_0, int2(_S11, row0_1), kernelContext_1);

#line 91
    float3 bottom_0 = cyFogLerp_0(_S10, _S12.xyz, tx_0);

#line 91
    float4 _S13 = CyFrameFogVolume_texel_0(source_0, int2(_S8, row1_1), kernelContext_1);

    float3 _S14 = _S13.xyz;

#line 93
    float4 _S15 = CyFrameFogVolume_texel_0(source_0, int2(_S11, row1_1), kernelContext_1);

    return cyFogLerp_0(bottom_0, cyFogLerp_0(_S14, _S15.xyz, tx_0), ty_0);
}


#line 48
struct CyFogAtPoint_0
{
    float3 transmittance_0;
    float3 inScattering_0;
};


#line 781 "src/rendering/shaders/cy/frame.slang"
float4 CyFrameFogVolume_texel_1(const CyFrameFogVolume_0 thread* this_1, int2 coordinate_1, KernelContext_0 thread* kernelContext_2)
{

    int3 _S16 = int3(coordinate_1, int(0));

#line 784
    return ((kernelContext_2->cyFrameGlobals_0->textures_0[this_1->slot_0]).read(vec<uint,2>(((_S16)).xy), uint(((_S16)).z)));
}


#line 70 "src/rendering/shaders/cy/volumetric_fog.slang"
float cyFogSliceDepth_0(float4 shape_1, float4 planes_0, float slice_1)
{
    float _S17 = shape_1.z;
    return mix(planes_0.x, planes_0.y, pow(min(slice_1 + 1.0, _S17) / _S17, max(shape_1.w, 1.0)));
}


#line 100
CyFogAtPoint_0 cyFogAt_0(const CyFrameFogVolume_0 thread* source_1, float3 relativePosition_0, KernelContext_0 thread* kernelContext_3)
{
    thread CyFogAtPoint_0 result_0;
    float3 _S18 = float3(1.0) ;

#line 103
    (&result_0)->transmittance_0 = _S18;
    float3 _S19 = float3(0.0) ;

#line 104
    (&result_0)->inScattering_0 = _S19;

#line 104
    float4 _S20 = CyFrameFogVolume_texel_1(source_1, int2(int(0), int(0)), kernelContext_3);

#line 104
    float4 _S21 = CyFrameFogVolume_texel_1(source_1, int2(int(1), int(0)), kernelContext_3);

#line 104
    float4 _S22 = CyFrameFogVolume_texel_1(source_1, int2(int(2), int(0)), kernelContext_3);

#line 104
    float4 _S23 = CyFrameFogVolume_texel_1(source_1, int2(int(3), int(0)), kernelContext_3);

#line 104
    float4 _S24 = CyFrameFogVolume_texel_1(source_1, int2(int(4), int(0)), kernelContext_3);

#line 104
    float4 _S25 = CyFrameFogVolume_texel_1(source_1, int2(int(5), int(0)), kernelContext_3);

#line 111
    float3 offset_0 = relativePosition_0 - _S25.xyz;
    float depth_0 = dot(offset_0, _S20.xyz);

#line 112
    bool _S26;
    if((_S20.w) < 0.5)
    {

#line 113
        _S26 = true;

#line 113
    }
    else
    {

#line 113
        _S26 = depth_0 <= 0.0;

#line 113
    }

#line 113
    if(_S26)
    {
        return result_0;
    }



    float2 texel_1 = float2((dot(offset_0, _S21.xyz) / (depth_0 * _S21.w) * 0.5 + 0.5) * _S23.x - 0.5, (dot(offset_0, _S22.xyz) / (depth_0 * _S22.w) * 0.5 + 0.5) * _S23.y - 0.5);



    float _S27 = _S23.z;

#line 124
    uint last_0 = uint(_S27) - 1U;
    float first_0 = cyFogSliceDepth_0(_S23, _S24, 0.0);



    float _S28 = saturate(depth_0 / max(first_0, 9.99999997475242708e-07));

#line 129
    float fraction_0;

#line 129
    uint farSlice_0;

#line 129
    float3 nearT_0;

#line 129
    float3 nearS_0;
    if(depth_0 > first_0)
    {
        float _S29 = _S24.x;


        uint _S30 = min(uint(max(pow(saturate((depth_0 - _S29) / max(_S24.y - _S29, 9.99999997475242708e-07)), 1.0 / max(_S23.w, 1.0)) * _S27 - 1.0, 0.0)), last_0);

#line 135
        float3 _S31 = cyFogPlane_0(source_1, _S23, _S30, texel_1, 0U, kernelContext_3);

#line 135
        float3 _S32 = cyFogPlane_0(source_1, _S23, _S30, texel_1, 1U, kernelContext_3);


        uint _S33 = _S30 + 1U;

#line 138
        uint _S34 = min(_S33, last_0);
        float nearEdge_0 = cyFogSliceDepth_0(_S23, _S24, float(_S30));
        float farEdge_0 = cyFogSliceDepth_0(_S23, _S24, float(_S33));
        if(_S30 == last_0)
        {

#line 141
            fraction_0 = 1.0;

#line 141
        }
        else
        {

#line 141
            fraction_0 = saturate((depth_0 - nearEdge_0) / max(farEdge_0 - nearEdge_0, 9.99999997475242708e-07));

#line 141
        }

#line 141
        farSlice_0 = _S34;

#line 141
        nearT_0 = _S31;

#line 141
        nearS_0 = _S32;

#line 130
    }
    else
    {

#line 130
        farSlice_0 = 0U;

#line 130
        nearT_0 = _S18;

#line 130
        fraction_0 = _S28;

#line 130
        nearS_0 = _S19;

#line 130
    }

#line 130
    float3 _S35 = cyFogPlane_0(source_1, _S23, farSlice_0, texel_1, 0U, kernelContext_3);

#line 130
    float3 _S36 = cyFogPlane_0(source_1, _S23, farSlice_0, texel_1, 1U, kernelContext_3);

#line 145
    (&result_0)->transmittance_0 = cyFogLerp_0(nearT_0, _S35, fraction_0);
    (&result_0)->inScattering_0 = cyFogLerp_0(nearS_0, _S36, fraction_0);
    return result_0;
}


#line 527 "src/rendering/shaders/cy/frame.slang"
struct CyFrameShadowMap_0
{
    uint slot_1;
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
    float2 _S37 = hammersley_0(uint(index_2), uint(count_1));

    float _S38 = _S37.y * 6.28318548202514648;
    float2 _S39 = float2(cos(_S38), sin(_S38)) * float2(sqrt(_S37.x)) ;
    float _S40 = _S39.x;

#line 112
    float _S41 = _S39.y;

#line 112
    return float2(_S40 * cosine_0 - _S41 * sine_0, _S40 * sine_0 + _S41 * cosine_0);
}


#line 283 "src/rendering/shaders/cy/frame.slang"
float4 cyMaterialSampleTextureLevel_0(uint bindlessIndex_0, float2 uv_0, float level_0, KernelContext_0 thread* kernelContext_4)
{
    return ((kernelContext_4->cyFrameGlobals_0->textures_0[bindlessIndex_0]).sample((kernelContext_4->cyFrameGlobals_0->sampler_0), (uv_0), level((level_0))));
}


#line 530
float CyFrameShadowMap_storedDepth_0(const CyFrameShadowMap_0 thread* this_2, float2 uv_1, KernelContext_0 thread* kernelContext_5)
{

#line 530
    float4 _S42 = cyMaterialSampleTextureLevel_0(this_2->slot_1, uv_1, 0.0, kernelContext_5);

    return _S42.x;
}


#line 149 "src/rendering/shaders/cy/shadow.slang"
float pcssFilter_0(const CyFrameShadowMap_0 thread* source_2, float2 uv_2, float receiver_0, float radius_0, float rotation_0, int taps_0, KernelContext_0 thread* kernelContext_6)
{

    float _S43 = cos(rotation_0);
    float _S44 = sin(rotation_0);

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
        float _S45 = CyFrameShadowMap_storedDepth_0(source_2, uv_2 + shadowDiscTap_0(index_3, taps_0, _S43, _S44) * float2(radius_0) , kernelContext_6);

#line 155
        float _S46;


        if(receiver_0 >= _S45)
        {

#line 158
            _S46 = 1.0;

#line 158
        }
        else
        {

#line 158
            _S46 = 0.0;

#line 158
        }

#line 158
        float lit_1 = lit_0 + _S46;

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


#line 530 "src/rendering/shaders/cy/frame.slang"
float CyFrameShadowMap_storedDepth_1(const CyFrameShadowMap_0 thread* this_3, float2 uv_3, KernelContext_0 thread* kernelContext_7)
{

#line 530
    float4 _S47 = cyMaterialSampleTextureLevel_0(this_3->slot_1, uv_3, 0.0, kernelContext_7);

    return _S47.x;
}


#line 116 "src/rendering/shaders/cy/shadow.slang"
PcssBlockers_0 pcssBlockerSearch_0(const CyFrameShadowMap_0 thread* source_3, float2 uv_4, float receiver_1, float searchRadius_0, float rotation_1, int taps_1, KernelContext_0 thread* kernelContext_8)
{


    float _S48 = cos(rotation_1);
    float _S49 = sin(rotation_1);
    thread PcssBlockers_0 result_1;
    (&result_1)->averageDepth_0 = 0.0;
    (&result_1)->count_2 = 0.0;

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
        float _S50 = CyFrameShadowMap_storedDepth_1(source_3, uv_4 + shadowDiscTap_0(index_4, taps_1, _S48, _S49) * float2(searchRadius_0) , kernelContext_8);


        if(_S50 > receiver_1)
        {
            (&result_1)->averageDepth_0 = (&result_1)->averageDepth_0 + _S50;
            (&result_1)->count_2 = (&result_1)->count_2 + 1.0;

#line 128
        }

#line 125
        index_4 = index_4 + int(1);

#line 125
    }

#line 134
    if(((&result_1)->count_2) > 0.0)
    {
        (&result_1)->averageDepth_0 = (&result_1)->averageDepth_0 / (&result_1)->count_2;

#line 134
    }



    return result_1;
}


#line 151 "src/rendering/shaders/cy/volumetric_fog.slang"
float3 cyApplyFog_0(const CyFrameFogVolume_0 thread* source_4, float3 radiance_0, float3 relativePosition_1, KernelContext_0 thread* kernelContext_9)
{

#line 151
    CyFogAtPoint_0 _S51 = cyFogAt_0(source_4, relativePosition_1, kernelContext_9);


    return radiance_0 * _S51.transmittance_0 + _S51.inScattering_0;
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
float pcssVisibility_0(const CyFrameShadowMap_0 thread* source_5, float2 uv_5, float receiver_3, const PcssShape_0 thread* shape_2, float rotation_2, KernelContext_0 thread* kernelContext_10)
{

#line 182
    float _S52 = shape_2->penumbraPerDepth_0;

#line 182
    float _S53 = shape_2->minRadius_0;

#line 182
    float _S54 = shape_2->maxRadius_0;

#line 182
    PcssBlockers_0 _S55 = pcssBlockerSearch_0(source_5, uv_5, receiver_3, clamp((1.0 - receiver_3) * shape_2->penumbraPerDepth_0, shape_2->minRadius_0, shape_2->maxRadius_0), rotation_2, shape_2->blockerTaps_0, kernelContext_10);

#line 189
    if((_S55.count_2) == 0.0)
    {
        return 1.0;
    }

#line 191
    float _S56 = pcssFilter_0(source_5, uv_5, receiver_3, clamp(pcssPenumbraRadius_0(receiver_3, _S55.averageDepth_0, _S52), _S53, _S54), rotation_2, shape_2->filterTaps_0, kernelContext_10);



    return _S56;
}


#line 41 "src/rendering/shaders/cy/material.slang"
struct Surface_0
{
    float3 albedo_0;
    float3 normal_0;
    float roughness_0;
    float metallic_0;
    float3 emission_0;
    float occlusion_0;
    float opacity_0;
};




Surface_0 defaultSurface_0()
{
    thread Surface_0 surface_1;
    (&surface_1)->albedo_0 = float3(0.5) ;
    (&surface_1)->normal_0 = float3(0.0, 0.0, 1.0);
    (&surface_1)->roughness_0 = 0.5;
    (&surface_1)->metallic_0 = 0.0;
    (&surface_1)->emission_0 = float3(0.0) ;
    (&surface_1)->occlusion_0 = 1.0;
    (&surface_1)->opacity_0 = 1.0;
    return surface_1;
}


#line 344 "src/rendering/shaders/cy/frame.slang"
float3 readMaterialFloat3_0(uint material_1, uint wordOffset_0, KernelContext_0 thread* kernelContext_11)
{
    uint _S57 = material_1 * kernelContext_11->cyFrameView_0->frame_0->counts_0.y + wordOffset_0;
    return float3((as_type<float>((kernelContext_11->cyFrameView_0->materialWords_0[_S57]))), (as_type<float>((kernelContext_11->cyFrameView_0->materialWords_0[_S57 + 1U]))), (as_type<float>((kernelContext_11->cyFrameView_0->materialWords_0[_S57 + 2U]))));
}


#line 339
float readMaterialFloat_0(uint material_2, uint wordOffset_1, KernelContext_0 thread* kernelContext_12)
{
    return (as_type<float>((kernelContext_12->cyFrameView_0->materialWords_0[material_2 * kernelContext_12->cyFrameView_0->frame_0->counts_0.y + wordOffset_1])));
}


#line 354
uint readMaterialUint_0(uint material_3, uint wordOffset_2, KernelContext_0 thread* kernelContext_13)
{
    return kernelContext_13->cyFrameView_0->materialWords_0[material_3 * kernelContext_13->cyFrameView_0->frame_0->counts_0.y + wordOffset_2];
}


#line 364
uint materialTextureSlot_0(uint material_4, uint wordOffset_3, KernelContext_0 thread* kernelContext_14)
{
    if(wordOffset_3 == 4294967295U)
    {
        return 4294967295U;
    }

#line 368
    uint _S58 = readMaterialUint_0(material_4, wordOffset_3, kernelContext_14);

    return _S58;
}


#line 278
float4 cyMaterialSampleTexture_0(uint bindlessIndex_1, float2 uv_6, KernelContext_0 thread* kernelContext_15)
{
    return ((kernelContext_15->cyFrameGlobals_0->textures_0[bindlessIndex_1]).sample((kernelContext_15->cyFrameGlobals_0->sampler_0), (uv_6)));
}


#line 400
Surface_0 surfaceOf_0(uint material_5, float3 tint_1, float2 uv_7, KernelContext_0 thread* kernelContext_16)
{
    thread Surface_0 surface_2 = defaultSurface_0();

#line 402
    float3 _S59 = readMaterialFloat3_0(material_5, kernelContext_16->cyFrameView_0->frame_0->materialOffsets_0.x, kernelContext_16);
    (&surface_2)->albedo_0 = _S59 * tint_1;

#line 403
    float _S60 = readMaterialFloat_0(material_5, kernelContext_16->cyFrameView_0->frame_0->materialOffsets_0.y, kernelContext_16);
    (&surface_2)->roughness_0 = clamp(_S60, 0.01999999955296516, 1.0);

#line 404
    float _S61 = readMaterialFloat_0(material_5, kernelContext_16->cyFrameView_0->frame_0->materialOffsets_0.z, kernelContext_16);
    (&surface_2)->metallic_0 = saturate(_S61);

#line 405
    float3 _S62 = readMaterialFloat3_0(material_5, kernelContext_16->cyFrameView_0->frame_0->materialOffsets_0.w, kernelContext_16);
    (&surface_2)->emission_0 = _S62;

#line 406
    uint _S63 = materialTextureSlot_0(material_5, kernelContext_16->cyFrameView_0->frame_0->materialTextures_0.x, kernelContext_16);


    if(_S63 != 4294967295U)
    {

#line 409
        float4 _S64 = cyMaterialSampleTexture_0(_S63, uv_7, kernelContext_16);

        (&surface_2)->albedo_0 = (&surface_2)->albedo_0 * _S64.xyz;

#line 409
    }



    return surface_2;
}


#line 42 "src/rendering/shaders/cy/light.slang"
float distanceAttenuation_0(float distanceSquared_0, float range_1)
{
    float _S65 = distanceSquared_0 / max(range_1 * range_1, 9.99999997475242708e-07);
    float _S66 = saturate(1.0 - _S65 * _S65);
    return _S66 * _S66 / max(distanceSquared_0, 0.00009999999747379);
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
    thread LightSample_0 result_2;

#line 51
    uint _S67 = light_0->kind_0;
    if((light_0->kind_0) == 0U)
    {
        (&result_2)->direction_1 = - light_0->direction_0;
        (&result_2)->attenuation_0 = 1.0;
        (&result_2)->illuminance_0 = light_0->color_0 * float3(light_0->intensity_0) ;
        return result_2;
    }

    float3 _S68 = light_0->positionRelativeToCamera_0 - surfaceRelativeToCamera_0;
    float _S69 = dot(_S68, _S68);
    (&result_2)->direction_1 = _S68 * float3(rsqrt(max(_S69, 9.99999993922529029e-09))) ;
    (&result_2)->attenuation_0 = distanceAttenuation_0(_S69, light_0->range_0);

    if(_S67 == 2U)
    {

        float _S70 = saturate(dot(- (&result_2)->direction_1, light_0->direction_0) * light_0->spotScaleBias_0.x + light_0->spotScaleBias_0.y);
        (&result_2)->attenuation_0 = (&result_2)->attenuation_0 * (_S70 * _S70);

#line 65
    }

#line 71
    (&result_2)->illuminance_0 = light_0->color_0 * float3((light_0->intensity_0 * (&result_2)->attenuation_0)) ;
    return result_2;
}


#line 307 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_17)
{
    float4 _S71 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_17->cyFrameView_0->frame_0->shadowToClipRow0_0, _S71), dot(kernelContext_17->cyFrameView_0->frame_0->shadowToClipRow1_0, _S71), dot(kernelContext_17->cyFrameView_0->frame_0->shadowToClipRow2_0, _S71), dot(kernelContext_17->cyFrameView_0->frame_0->shadowToClipRow3_0, _S71));
}


#line 98 "src/rendering/shaders/cy/shadow.slang"
float shadowDiscRotation_0(float2 pixel_0)
{

    return fract(52.98291778564453125 * fract(dot(pixel_0, float2(0.06711056083440781, 0.00583714991807938)))) * 6.28318548202514648;
}


#line 538 "src/rendering/shaders/cy/frame.slang"
float softShadowVisibility_0(float2 uv_8, float reference_0, float2 pixel_1, KernelContext_0 thread* kernelContext_18)
{
    thread CyFrameShadowMap_0 map_0;
    (&map_0)->slot_1 = kernelContext_18->cyFrameView_0->frame_0->shadowControl_0.x;
    thread PcssShape_0 shape_3;
    (&shape_3)->penumbraPerDepth_0 = kernelContext_18->cyFrameView_0->frame_0->softShadowShape_0.x;
    (&shape_3)->minRadius_0 = kernelContext_18->cyFrameView_0->frame_0->softShadowShape_0.y;
    (&shape_3)->maxRadius_0 = kernelContext_18->cyFrameView_0->frame_0->softShadowShape_0.z;
    (&shape_3)->blockerTaps_0 = int(max(kernelContext_18->cyFrameView_0->frame_0->softShadowControl_0.z, 1U));
    (&shape_3)->filterTaps_0 = int(max(kernelContext_18->cyFrameView_0->frame_0->softShadowControl_0.w, 1U));
    float _S72 = shadowDiscRotation_0(pixel_1);

#line 548
    thread CyFrameShadowMap_0 _S73 = map_0;

#line 548
    thread PcssShape_0 _S74 = shape_3;

#line 548
    float _S75 = pcssVisibility_0(&_S73, uv_8, reference_0, &_S74, _S72, kernelContext_18);

#line 548
    return _S75;
}


#line 571
float directionalShadowVisibility_0(float3 relativePosition_2, float3 normal_1, float2 fragmentCentre_0, KernelContext_0 thread* kernelContext_19)
{

#line 571
    bool _S76;

    if((kernelContext_19->cyFrameView_0->frame_0->shadowControl_0.w) == 0U)
    {

#line 573
        _S76 = true;

#line 573
    }
    else
    {

#line 573
        _S76 = (kernelContext_19->cyFrameView_0->frame_0->shadowControl_0.x) == 4294967295U;

#line 573
    }

#line 573
    if(_S76)
    {
        return 1.0;
    }

#line 575
    float4 _S77 = transformToShadowClip_0(relativePosition_2 + normal_1 * float3(0.00499999988824129) , kernelContext_19);


    float _S78 = _S77.w;

#line 578
    if(_S78 <= 0.0)
    {
        return 1.0;
    }
    float3 _S79 = _S77.xyz / float3(_S78) ;
    float _S80 = _S79.x * 0.5 + 0.5;

#line 583
    float _S81 = 0.5 - _S79.y * 0.5;

#line 583
    float2 _S82 = float2(_S80, _S81);
    if(_S80 < 0.0)
    {

#line 584
        _S76 = true;

#line 584
    }
    else
    {

#line 584
        _S76 = _S80 > 1.0;

#line 584
    }

#line 584
    if(_S76)
    {

#line 584
        _S76 = true;

#line 584
    }
    else
    {

#line 584
        _S76 = _S81 < 0.0;

#line 584
    }

#line 584
    if(_S76)
    {

#line 584
        _S76 = true;

#line 584
    }
    else
    {

#line 584
        _S76 = _S81 > 1.0;

#line 584
    }

#line 584
    if(_S76)
    {
        return 1.0;
    }
    float _S83 = 1.0 / float(max(kernelContext_19->cyFrameView_0->frame_0->shadowControl_0.z, 1U));
    float _S84 = _S79.z + 0.00050000002374873;
    if(((kernelContext_19->cyFrameView_0->frame_0->softShadowControl_0.x) & 1U) != 0U)
    {

#line 590
        float _S85 = softShadowVisibility_0(_S82, _S84, fragmentCentre_0, kernelContext_19);

        return _S85;
    }

#line 592
    int y_0 = int(-1);

#line 592
    float visible_0 = 0.0;


    for(;;)
    {

#line 595
        if(y_0 <= int(1))
        {
        }
        else
        {

#line 595
            break;
        }

#line 595
        int x_0 = int(-1);

        for(;;)
        {

#line 597
            if(x_0 <= int(1))
            {
            }
            else
            {

#line 597
                break;
            }

#line 597
            float4 _S86 = cyMaterialSampleTextureLevel_0(kernelContext_19->cyFrameView_0->frame_0->shadowControl_0.x, _S82 + float2(float(x_0), float(y_0)) * float2(_S83) , 0.0, kernelContext_19);

#line 597
            float _S87;



            if(_S84 >= (_S86.x))
            {

#line 601
                _S87 = 1.0;

#line 601
            }
            else
            {

#line 601
                _S87 = 0.0;

#line 601
            }

#line 601
            float visible_1 = visible_0 + _S87;

#line 597
            x_0 = x_0 + int(1);

#line 597
            visible_0 = visible_1;

#line 597
        }

#line 595
        y_0 = y_0 + int(1);

#line 595
    }

#line 604
    return visible_0 / 9.0;
}


#line 554
float contactShadowVisibility_0(float2 fragmentCentre_1, KernelContext_0 thread* kernelContext_20)
{

#line 554
    bool _S88;

    if(((kernelContext_20->cyFrameView_0->frame_0->softShadowControl_0.x) & 2U) == 0U)
    {

#line 556
        _S88 = true;

#line 556
    }
    else
    {

#line 556
        _S88 = (kernelContext_20->cyFrameView_0->frame_0->softShadowControl_0.y) == 4294967295U;

#line 556
    }

#line 556
    if(_S88)
    {

        return 1.0;
    }

#line 559
    float4 _S89 = cyMaterialSampleTextureLevel_0(kernelContext_20->cyFrameView_0->frame_0->softShadowControl_0.y, fragmentCentre_1 * kernelContext_20->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_20);


    return _S89.x;
}


#line 61 "src/rendering/shaders/cy/brdf.slang"
float3 computeF0_0(float3 albedo_1, float metallic_1)
{
    return mix(float3(0.03999999910593033) , albedo_1, float3(metallic_1) );
}


#line 39
float3 diffuseLambert_0(float3 albedo_2)
{
    return albedo_2 * float3(0.31830987334251404) ;
}


#line 11
float distributionGgx_0(float normalDotHalf_0, float roughness_1)
{
    float _S90 = roughness_1 * roughness_1;
    float _S91 = _S90 * _S90;
    float _S92 = normalDotHalf_0 * normalDotHalf_0 * (_S91 - 1.0) + 1.0;
    return _S91 / max(3.14159274101257324 * _S92 * _S92, 1.00000001168609742e-07);
}


float visibilitySmithGgxCorrelated_0(float normalDotView_0, float normalDotLight_0, float roughness_2)
{

    float _S93 = roughness_2 * roughness_2;
    float _S94 = _S93 * _S93;
    float _S95 = 1.0 - _S94;



    return 0.5 / max(normalDotLight_0 * sqrt(normalDotView_0 * normalDotView_0 * _S95 + _S94) + normalDotView_0 * sqrt(normalDotLight_0 * normalDotLight_0 * _S95 + _S94), 1.00000001168609742e-07);
}

float3 fresnelSchlick_0(float3 f0_0, float viewDotHalf_0)
{

    return f0_0 + (float3(1.0)  - f0_0) * float3(pow(saturate(1.0 - viewDotHalf_0), 5.0)) ;
}


#line 45
float3 specularGgx_0(float3 normal_2, float3 view_0, float3 light_1, float roughness_3, float3 f0_1)
{
    float3 _S96 = normalize(view_0 + light_1);

#line 56
    return float3((distributionGgx_0(saturate(dot(normal_2, _S96)), roughness_3) * visibilitySmithGgxCorrelated_0(saturate(dot(normal_2, view_0)) + 0.00000999999974738, saturate(dot(normal_2, light_1)), roughness_3)))  * fresnelSchlick_0(f0_1, saturate(dot(view_0, _S96)));
}


#line 75 "src/rendering/shaders/cy/material.slang"
float3 shadeSurfaceWithLight_0(const Surface_0 thread* surface_3, float3 worldNormal_0, float3 viewDirection_0, const LightSample_0 thread* sample_0)
{

#line 76
    float3 _S97 = sample_0->direction_1;

    float _S98 = saturate(dot(worldNormal_0, sample_0->direction_1));
    if(_S98 <= 0.0)
    {
        return float3(0.0) ;
    }

#line 87
    return (diffuseLambert_0(surface_3->albedo_0 * float3((1.0 - surface_3->metallic_0)) ) + specularGgx_0(worldNormal_0, viewDirection_0, _S97, surface_3->roughness_0, computeF0_0(surface_3->albedo_0, surface_3->metallic_0))) * sample_0->illuminance_0 * float3(_S98) ;
}


#line 331 "src/rendering/shaders/cy/frame.slang"
float viewDepthOf_0(float3 relative_1, KernelContext_0 thread* kernelContext_21)
{



    return - dot(kernelContext_21->cyFrameView_0->frame_0->relativeToViewRow2_0, float4(relative_1, 1.0));
}


#line 21 "src/rendering/shaders/cy/cluster.slang"
uint clusterSliceOf_0(const ClusterGrid_0 constant* grid_0, float viewDepth_0)
{

    return uint(clamp(log2(max(viewDepth_0, grid_0->nearPlane_0)) * grid_0->sliceScale_0 + grid_0->sliceBias_0, 0.0, float(grid_0->dimensions_0.z - 1U)));
}


uint3 clusterCoordOf_0(const ClusterGrid_0 constant* grid_1, uint2 pixel_2, uint2 renderExtent_0, float viewDepth_1)
{
    uint2 _S99 = grid_1->dimensions_0.xy;
    uint2 _S100 = min(uint2(float2(pixel_2) / float2(renderExtent_0) * float2(_S99)), _S99 - uint2(1U) );

#line 31
    uint _S101 = clusterSliceOf_0(grid_1, viewDepth_1);

#line 31
    return uint3(_S100, _S101);
}



uint clusterIndexOf_0(const ClusterGrid_0 constant* grid_2, uint3 coord_0)
{
    return coord_0.x + grid_2->dimensions_0.x * (coord_0.y + grid_2->dimensions_0.y * coord_0.z);
}


#line 607 "src/rendering/shaders/cy/frame.slang"
float3 accumulateLights_0(const Surface_0 thread* surface_4, float3 relativePosition_3, float3 normal_3, float3 viewDir_0, float2 fragmentCentre_2, uint instanceFlags_0, KernelContext_0 thread* kernelContext_22)
{

#line 608
    bool _S102;

    uint2 _S103 = uint2(fragmentCentre_2);
    float3 _S104 = float3(0.0) ;
    uint _S105 = kernelContext_22->cyFrameView_0->frame_0->counts_0.x;

#line 612
    uint global_0 = 0U;

#line 612
    float3 lit_2 = _S104;

#line 621
    for(;;)
    {

#line 621
        if(global_0 < _S105)
        {
        }
        else
        {

#line 621
            break;
        }
        if((kernelContext_22->cyFrameView_0->lights_0[global_0].kind_0) != 0U)
        {
            global_0 = global_0 + 1U;

#line 621
            continue;
        }

#line 621
        thread Light_0 _S106 = kernelContext_22->cyFrameView_0->lights_0[global_0];

#line 621
        LightSample_0 _S107 = evaluateLight_0(&_S106, relativePosition_3);

#line 631
        if(global_0 == (kernelContext_22->cyFrameView_0->frame_0->shadowControl_0.y))
        {

#line 631
            _S102 = (instanceFlags_0 & 8U) != 0U;

#line 631
        }
        else
        {

#line 631
            _S102 = false;

#line 631
        }

#line 631
        float _S108;
        if(_S102)
        {

#line 632
            float _S109 = directionalShadowVisibility_0(relativePosition_3, normal_3, fragmentCentre_2, kernelContext_22);

#line 632
            float _S110 = contactShadowVisibility_0(fragmentCentre_2, kernelContext_22);

#line 632
            _S108 = min(_S109, _S110);

#line 632
        }
        else
        {

#line 632
            _S108 = 1.0;

#line 632
        }

#line 632
        thread LightSample_0 _S111 = _S107;

#line 632
        float3 _S112 = shadeSurfaceWithLight_0(surface_4, normal_3, viewDir_0, &_S111);

#line 632
        lit_2 = lit_2 + _S112 * float3(_S108) ;

#line 621
        global_0 = global_0 + 1U;

#line 621
    }

#line 639
    if((kernelContext_22->cyFrameView_0->frame_0->counts_0.w) == 0U)
    {

#line 639
        _S102 = true;

#line 639
    }
    else
    {

#line 639
        _S102 = ((&kernelContext_22->cyFrameView_0->frame_0->clusterGrid_0)->dimensions_0.z) == 0U;

#line 639
    }

#line 639
    uint index_5;

#line 639
    if(_S102)
    {

#line 639
        index_5 = 0U;

        for(;;)
        {

#line 641
            if(index_5 < _S105)
            {
            }
            else
            {

#line 641
                break;
            }
            if((kernelContext_22->cyFrameView_0->lights_0[index_5].kind_0) == 0U)
            {
                index_5 = index_5 + 1U;

#line 641
                continue;
            }

#line 641
            thread Light_0 _S113 = kernelContext_22->cyFrameView_0->lights_0[index_5];

#line 641
            LightSample_0 _S114 = evaluateLight_0(&_S113, relativePosition_3);

#line 641
            thread LightSample_0 _S115 = _S114;

#line 641
            float3 _S116 = shadeSurfaceWithLight_0(surface_4, normal_3, viewDir_0, &_S115);

#line 641
            lit_2 = lit_2 + _S116;

#line 641
            index_5 = index_5 + 1U;

#line 641
        }

#line 650
        return lit_2;
    }

    uint2 _S117 = uint2(kernelContext_22->cyFrameView_0->frame_0->extentAndInverse_0.xy);

#line 653
    float _S118 = viewDepthOf_0(relativePosition_3, kernelContext_22);

#line 653
    uint3 _S119 = clusterCoordOf_0(&kernelContext_22->cyFrameView_0->frame_0->clusterGrid_0, _S103, _S117, _S118);

#line 653
    uint _S120 = clusterIndexOf_0(&kernelContext_22->cyFrameView_0->frame_0->clusterGrid_0, _S119);

#line 658
    uint2 _S121 = kernelContext_22->cyFrameView_0->clusterHeaders_0[_S120 * kernelContext_22->cyFrameView_0->frame_0->counts_0.z];

#line 658
    index_5 = 0U;
    for(;;)
    {

#line 659
        if(index_5 < (_S121.y))
        {
        }
        else
        {

#line 659
            break;
        }
        uint _S122 = kernelContext_22->cyFrameView_0->clusterIndices_0[_S121.x + index_5];
        if(_S122 >= _S105)
        {

#line 662
            _S102 = true;

#line 662
        }
        else
        {

#line 662
            _S102 = (kernelContext_22->cyFrameView_0->lights_0[_S122].kind_0) == 0U;

#line 662
        }

#line 662
        if(_S102)
        {
            index_5 = index_5 + 1U;

#line 659
            continue;
        }

#line 659
        thread Light_0 _S123 = kernelContext_22->cyFrameView_0->lights_0[_S122];

#line 659
        LightSample_0 _S124 = evaluateLight_0(&_S123, relativePosition_3);

#line 659
        thread LightSample_0 _S125 = _S124;

#line 659
        float3 _S126 = shadeSurfaceWithLight_0(surface_4, normal_3, viewDir_0, &_S125);

#line 659
        lit_2 = lit_2 + _S126;

#line 659
        index_5 = index_5 + 1U;

#line 659
    }

#line 669
    return lit_2;
}


#line 689
float4 probeVolumeTexel_0(uint3 probe_0, uint texel_2, KernelContext_0 thread* kernelContext_23)
{
    uint3 _S127 = kernelContext_23->cyFrameView_0->frame_0->probeVolumeControl_0.yzw;

    uint _S128 = _S127.y;

#line 693
    float4 _S129 = cyMaterialSampleTextureLevel_0(kernelContext_23->cyFrameView_0->frame_0->probeVolumeControl_0.x, (float2(float(probe_0.x * 5U + texel_2), float(probe_0.y + _S128 * probe_0.z)) + float2(0.5) ) / float2(float(_S127.x * 5U), float(_S128 * _S127.z)), 0.0, kernelContext_23);


    return _S129;
}



float probeVisibility_0(float4 distances0_0, float4 distances1_0, float3 probePosition_0, float3 position_0, float3 normal_4, float3 query_0, KernelContext_0 thread* kernelContext_24)
{

    float3 _S130 = probePosition_0 - position_0;
    float _S131 = dot(_S130, _S130);

#line 705
    float3 _S132;
    if(_S131 > 9.999999960041972e-13)
    {

#line 706
        _S132 = _S130 * float3(rsqrt(_S131)) ;

#line 706
    }
    else
    {

#line 706
        _S132 = normal_4;

#line 706
    }
    float _S133 = max(0.00009999999747379, (dot(_S132, normal_4) + 1.0) * 0.5);
    float _S134 = _S133 * _S133 + 0.20000000298023224;



    float3 _S135 = query_0 - probePosition_0;

#line 712
    float _S136;
    if((_S135.x) >= 0.0)
    {

#line 713
        _S136 = distances0_0.y;

#line 713
    }
    else
    {

#line 713
        _S136 = distances0_0.z;

#line 713
    }

#line 713
    float _S137;
    if((_S135.y) >= 0.0)
    {

#line 714
        _S137 = distances0_0.w;

#line 714
    }
    else
    {

#line 714
        _S137 = distances1_0.x;

#line 714
    }

#line 714
    float _S138;
    if((_S135.z) >= 0.0)
    {

#line 715
        _S138 = distances1_0.y;

#line 715
    }
    else
    {

#line 715
        _S138 = distances1_0.z;

#line 715
    }

#line 715
    float3 _S139 = float3(kernelContext_24->cyFrameView_0->frame_0->probeVolumeParams_0.z) ;

    float3 _S140 = saturate((float3(_S136, _S137, _S138) + _S139 - abs(_S135)) / _S139);
    return _S134 * min(_S140.x, min(_S140.y, _S140.z));
}



float3 probeVolumeAmbient_0(float3 position_1, float3 normal_5, float3 flat_0, KernelContext_0 thread* kernelContext_25)
{
    uint3 _S141 = kernelContext_25->cyFrameView_0->frame_0->probeVolumeControl_0.yzw;

    float3 _S142 = position_1 + normal_5 * float3(kernelContext_25->cyFrameView_0->frame_0->probeVolumeParams_0.y) ;

#line 727
    float3 _S143 = float3(kernelContext_25->cyFrameView_0->frame_0->probeVolumeOrigin_0.w) ;
    float3 _S144 = (_S142 - kernelContext_25->cyFrameView_0->frame_0->probeVolumeOrigin_0.xyz) / _S143;

#line 728
    float3 _S145 = float3(1.0) ;
    float3 _S146 = float3(_S141) - _S145;
    float3 _S147 = float3(0.0) ;

#line 730
    float3 _S148 = max(max(- _S144, _S144 - _S146), _S147);
    float _S149 = saturate(1.0 - max(_S148.x, max(_S148.y, _S148.z)));
    if(_S149 <= 0.0)
    {
        return flat_0;
    }
    float3 _S150 = clamp(_S144, _S147, _S146);
    uint3 _S151 = min(uint3(floor(_S150)), uint3(max(int3(_S141) - int3(int(2)) , int3(int(0)) )));
    float3 _S152 = _S150 - float3(_S151);


    float4 _S153 = float4(0.28209498524665833, 0.32573533058166504 * normal_5.y, 0.32573533058166504 * normal_5.z, 0.32573533058166504 * normal_5.x);

#line 741
    uint corner_0 = 0U;

#line 741
    float3 total_0 = _S147;

#line 741
    float weightSum_0 = 0.0;



    for(;;)
    {

#line 745
        if(corner_0 < 8U)
        {
        }
        else
        {

#line 745
            break;
        }
        uint3 _S154 = uint3(corner_0 & 1U, (corner_0 >> 1U) & 1U, (corner_0 >> 2U) & 1U);
        uint3 _S155 = min(_S151 + _S154, _S141 - uint3(1U) );
        float3 _S156 = float3(_S154);
        float3 _S157 = _S156 * _S152 + (_S145 - _S156) * (_S145 - _S152);

#line 750
        float4 _S158 = probeVolumeTexel_0(_S155, 3U, kernelContext_25);

        float _S159 = _S157.x * _S157.y * _S157.z * _S158.x;
        if(_S159 <= 0.0)
        {
            corner_0 = corner_0 + 1U;

#line 745
            continue;
        }

#line 745
        float4 _S160 = probeVolumeTexel_0(_S155, 4U, kernelContext_25);

#line 745
        float _S161 = probeVisibility_0(_S158, _S160, kernelContext_25->cyFrameView_0->frame_0->probeVolumeOrigin_0.xyz + float3(_S155) * _S143, position_1, normal_5, _S142, kernelContext_25);

#line 760
        float _S162 = _S159 * _S161;

#line 760
        float4 _S163 = probeVolumeTexel_0(_S155, 0U, kernelContext_25);
        float _S164 = dot(_S163, _S153);

#line 761
        float4 _S165 = probeVolumeTexel_0(_S155, 1U, kernelContext_25);
        float _S166 = dot(_S165, _S153);

#line 762
        float4 _S167 = probeVolumeTexel_0(_S155, 2U, kernelContext_25);



        float weightSum_1 = weightSum_0 + _S162;

#line 766
        total_0 = total_0 + max(float3(_S164, _S166, dot(_S167, _S153)) * float3(kernelContext_25->cyFrameView_0->frame_0->probeVolumeParams_0.x) , _S147) * float3(_S162) ;

#line 766
        weightSum_0 = weightSum_1;

#line 745
        corner_0 = corner_0 + 1U;

#line 745
    }

#line 768
    if(weightSum_0 > 9.99999997475242708e-07)
    {

#line 768
        total_0 = total_0 / float3(weightSum_0) ;

#line 768
    }
    else
    {

#line 768
        total_0 = flat_0;

#line 768
    }
    return mix(flat_0, total_0, float3(_S149) );
}


#line 674
float occlusionVisibility_0(float2 fragmentCentre_3, KernelContext_0 thread* kernelContext_26)
{

#line 674
    float4 _S168 = cyMaterialSampleTextureLevel_0(kernelContext_26->cyFrameView_0->frame_0->occlusionControl_0.x, fragmentCentre_3 * kernelContext_26->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_26);


    return _S168.w;
}


#line 677
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 677
struct pixelInput_0
{
    float3 relativePosition_4 [[user(TEXCOORD)]];
    float3 normal_6 [[user(TEXCOORD_1)]];
    float2 uv_9 [[user(TEXCOORD_2)]];
    [[flat]] uint drawIndex_0 [[user(TEXCOORD_3)]];
};


#line 792
[[fragment]] pixelOutput_0 cyForwardFragment(pixelInput_0 _S169 [[stage_in]], float4 position_2 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyFrameGlobalSet_default_0 constant& cyFrameGlobals_1 [[buffer(0)]])
{

#line 792
    thread KernelContext_0 kernelContext_27;

#line 792
    (&kernelContext_27)->cyFrameView_0 = cyFrameView_1;

#line 792
    (&kernelContext_27)->cyFrameGlobals_0 = &cyFrameGlobals_1;

    CyDrawInstance_0 _S170 = cyFrameView_1->drawInstances_0[_S169.drawIndex_0];

#line 794
    Surface_0 _S171 = surfaceOf_0(_S170.material_0, cyFrameView_1->instances_0[_S170.instanceSlot_0].tint_0.xyz, _S169.uv_9, &kernelContext_27);



    float3 _S172 = normalize(_S169.normal_6);


    float3 _S173 = normalize(- _S169.relativePosition_4);


    float2 _S174 = position_2.xy;

#line 804
    thread Surface_0 _S175 = _S171;

#line 804
    float3 _S176 = accumulateLights_0(&_S175, _S169.relativePosition_4, _S172, _S173, _S174, _S170.flags_0, &kernelContext_27);

#line 811
    float3 ambientRadiance_0 = (&kernelContext_27)->cyFrameView_0->frame_0->ambientAndOcclusion_0.xyz;

#line 811
    float3 ambientRadiance_1;
    if(((&kernelContext_27)->cyFrameView_0->frame_0->probeVolumeControl_0.x) != 4294967295U)
    {

#line 812
        float3 _S177 = probeVolumeAmbient_0(_S169.relativePosition_4, _S172, ambientRadiance_0, &kernelContext_27);

#line 812
        ambientRadiance_1 = _S177;

#line 812
    }
    else
    {

#line 812
        ambientRadiance_1 = ambientRadiance_0;

#line 812
    }



    float3 ambient_0 = _S171.albedo_0 * ambientRadiance_1 * float3(_S171.occlusion_0) ;

#line 816
    float3 color_1;

#line 816
    float3 ambient_1;
    if(((&kernelContext_27)->cyFrameView_0->frame_0->occlusionControl_0.x) != 4294967295U)
    {

#line 817
        float _S178 = occlusionVisibility_0(_S174, &kernelContext_27);


        float3 ambient_2 = ambient_0 * float3(_S178) ;
        if(((&kernelContext_27)->cyFrameView_0->frame_0->occlusionControl_0.y) != 0U)
        {

#line 821
            color_1 = _S176 * float3(mix(1.0, _S178, (as_type<float>(((&kernelContext_27)->cyFrameView_0->frame_0->occlusionControl_0.z))))) ;

#line 821
        }
        else
        {

#line 821
            color_1 = _S176;

#line 821
        }

#line 821
        ambient_1 = ambient_2;

#line 817
    }
    else
    {

#line 817
        color_1 = _S176;

#line 817
        ambient_1 = ambient_0;

#line 817
    }

#line 827
    float3 color_2 = color_1 + _S171.emission_0 + ambient_1;



    if(((&kernelContext_27)->cyFrameView_0->frame_0->volumetricFogControl_0.x) != 4294967295U)
    {
        thread CyFrameFogVolume_0 fog_0;
        (&fog_0)->slot_0 = (&kernelContext_27)->cyFrameView_0->frame_0->volumetricFogControl_0.x;

#line 834
        thread CyFrameFogVolume_0 _S179 = fog_0;

#line 834
        float3 _S180 = cyApplyFog_0(&_S179, color_2, _S169.relativePosition_4, &kernelContext_27);

#line 834
        color_1 = _S180;

#line 831
    }
    else
    {

#line 831
        color_1 = color_2;

#line 831
    }

#line 831
    pixelOutput_0 _S181 = { float4(color_1, _S171.opacity_0) };

#line 837
    return _S181;
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

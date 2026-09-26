#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the rendering frame. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::pipeline {

/// DepthVertex.metal, 6465 bytes.
inline constexpr char kFrameDepthVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 215 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 295
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
    uint4 occlusionControl_0;
    uint4 probeVolumeControl_0;
    float4 probeVolumeOrigin_0;
    float4 probeVolumeParams_0;
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


#line 202 "src/rendering/shaders/cy/frame.slang"
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


#line 224
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


#line 269
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


#line 279 "src/rendering/shaders/cy/frame.slang"
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


#line 304 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 306
struct cyDepthVertex_Result_0
{
    float4 position_0 [[position]];
    float3 normal_1 [[user(TEXCOORD)]];
    float4 currentClip_0 [[user(TEXCOORD_1)]];
    float4 previousClip_0 [[user(TEXCOORD_2)]];
};


#line 306
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
};


#line 422
struct CyDepthVertex_0
{
    float4 position_1;
    float3 normal_2;
    float4 currentClip_1;
    float4 previousClip_1;
};


#line 422
[[vertex]] cyDepthVertex_Result_0 cyDepthVertex(vertexInput_0 _S9 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 422
    thread KernelContext_0 kernelContext_1;

#line 422
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 422
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 434
    CyInstanceTransform_0 _S10 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

#line 434
    thread CyInstanceTransform_0 _S11 = _S10;

#line 434
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    thread CyDepthVertex_0 output_0;

#line 435
    float4 _S13 = transformToClip_0(_S12, &kernelContext_1);

    (&output_0)->position_1 = _S13;
    (&output_0)->currentClip_1 = _S13;
    float4 _S14 = float4(_S12, 1.0);
    (&output_0)->previousClip_1 = float4(dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow0_0, _S14), dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow1_0, _S14), dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow2_0, _S14), dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow3_0, _S14));



    float3 _S15 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 444
    thread CyInstanceTransform_0 _S16 = _S10;

#line 444
    float3 _S17 = rotateToRelative_0(&_S16, _S15);

#line 444
    (&output_0)->normal_2 = _S17;

#line 444
    thread cyDepthVertex_Result_0 _S18;

#line 444
    (&_S18)->position_0 = output_0.position_1;

#line 444
    (&_S18)->normal_1 = output_0.normal_2;

#line 444
    (&_S18)->currentClip_0 = output_0.currentClip_1;

#line 444
    (&_S18)->previousClip_0 = output_0.previousClip_1;

#line 444
    return _S18;
}

)cy_msl";

/// DepthFragment.metal, 3885 bytes.
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


#line 448 "src/rendering/shaders/cy/frame.slang"
struct CyDepthOutput_0
{
    float4 normalRoughness_0 [[color(0)]];
    float2 velocity_0 [[color(1)]];
};


#line 448
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
    uint4 occlusionControl_0;
    uint4 probeVolumeControl_0;
    float4 probeVolumeOrigin_0;
    float4 probeVolumeParams_0;
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


#line 202 "src/rendering/shaders/cy/frame.slang"
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


#line 455
[[fragment]] CyDepthOutput_0 cyDepthFragment(pixelInput_0 _S4 [[stage_in]], float4 position_0 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_0 [[buffer(1)]])
{
    thread CyDepthOutput_0 output_0;
    (&output_0)->normalRoughness_0 = float4(encodeOctahedral_0(normalize(_S4.normal_1)), 1.0, 1.0);

#line 466
    float2 _S5 = _S4.currentClip_0.xy / float2(_S4.currentClip_0.w)  - cyFrameView_0->frame_0->temporalJitter_0.xy * float2(2.0)  * cyFrameView_0->frame_0->extentAndInverse_0.zw;

    float2 _S6 = _S4.previousClip_0.xy / float2(_S4.previousClip_0.w) ;
    (&output_0)->velocity_0 = float2((_S6.x - _S5.x) * 0.5, (_S5.y - _S6.y) * 0.5);

    return output_0;
}

)cy_msl";

/// ShadowVertex.metal, 4474 bytes.
inline constexpr char kFrameShadowVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 215 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 295
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
    uint4 occlusionControl_0;
    uint4 probeVolumeControl_0;
    float4 probeVolumeOrigin_0;
    float4 probeVolumeParams_0;
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


#line 202 "src/rendering/shaders/cy/frame.slang"
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


#line 224
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


#line 269
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


#line 286 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow3_0, _S2));
}


#line 289
struct cyShadowVertex_Result_0
{
    float4 position_0 [[position]];
};


#line 289
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
};


#line 397
struct CyShadowVertex_0
{
    float4 position_1;
};


#line 397
[[vertex]] cyShadowVertex_Result_0 cyShadowVertex(vertexInput_0 _S3 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 397
    thread KernelContext_0 kernelContext_1;

#line 397
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 397
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 407
    thread CyShadowVertex_0 output_0;

#line 407
    thread CyInstanceTransform_0 _S4 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

#line 407
    float3 _S5 = transformToRelative_0(&_S4, _S3.modelPosition_1);

#line 407
    float4 _S6 = transformToShadowClip_0(_S5, &kernelContext_1);
    (&output_0)->position_1 = _S6;

#line 408
    thread cyShadowVertex_Result_0 _S7;

#line 408
    (&_S7)->position_0 = output_0.position_1;

#line 408
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


#line 413 "src/rendering/shaders/cy/frame.slang"
[[fragment]] pixelOutput_0 cyShadowFragment(float4 position_0 [[position]])
{

#line 413
    pixelOutput_0 _S1 = { position_0.z };

    return _S1;
}

)cy_msl";

/// ForwardVertex.metal, 6299 bytes.
inline constexpr char kFrameForwardVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 215 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 295
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
    uint4 occlusionControl_0;
    uint4 probeVolumeControl_0;
    float4 probeVolumeOrigin_0;
    float4 probeVolumeParams_0;
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


#line 202 "src/rendering/shaders/cy/frame.slang"
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


#line 224
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


#line 269
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


#line 279 "src/rendering/shaders/cy/frame.slang"
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


#line 304 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 306
struct cyForwardVertex_Result_0
{
    float4 position_0 [[position]];
    float3 relativePosition_0 [[user(TEXCOORD)]];
    float3 normal_1 [[user(TEXCOORD_1)]];
    float2 uv_0 [[user(TEXCOORD_2)]];
    uint drawIndex_1 [[user(TEXCOORD_3)]];
};


#line 306
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
    float2 uv_1 [[attribute(2)]];
};


#line 476
struct CyForwardVertex_0
{
    float4 position_1;
    float3 relativePosition_1;
    float3 normal_2;
    float2 uv_2;
    [[flat]] uint drawIndex_2;
};


#line 476
[[vertex]] cyForwardVertex_Result_0 cyForwardVertex(vertexInput_0 _S9 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 476
    thread KernelContext_0 kernelContext_1;

#line 476
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 476
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 490
    CyInstanceTransform_0 _S10 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

    thread CyForwardVertex_0 output_0;

#line 492
    thread CyInstanceTransform_0 _S11 = _S10;

#line 492
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    (&output_0)->relativePosition_1 = _S12;

#line 493
    float4 _S13 = transformToClip_0(_S12, &kernelContext_1);
    (&output_0)->position_1 = _S13;



    float3 _S14 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 498
    thread CyInstanceTransform_0 _S15 = _S10;

#line 498
    float3 _S16 = rotateToRelative_0(&_S15, _S14);

#line 498
    (&output_0)->normal_2 = _S16;
    (&output_0)->uv_2 = _S9.uv_1;
    (&output_0)->drawIndex_2 = cyDraw_1->drawIndex_0;

#line 500
    thread cyForwardVertex_Result_0 _S17;

#line 500
    (&_S17)->position_0 = output_0.position_1;

#line 500
    (&_S17)->relativePosition_0 = output_0.relativePosition_1;

#line 500
    (&_S17)->normal_1 = output_0.normal_2;

#line 500
    (&_S17)->uv_0 = output_0.uv_2;

#line 500
    (&_S17)->drawIndex_1 = output_0.drawIndex_2;

#line 500
    return _S17;
}

)cy_msl";

/// ForwardFragment.metal, 28068 bytes.
inline constexpr char kFrameForwardFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

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
    thread Surface_0 surface_0;
    (&surface_0)->albedo_0 = float3(0.5) ;
    (&surface_0)->normal_0 = float3(0.0, 0.0, 1.0);
    (&surface_0)->roughness_0 = 0.5;
    (&surface_0)->metallic_0 = 0.0;
    (&surface_0)->emission_0 = float3(0.0) ;
    (&surface_0)->occlusion_0 = 1.0;
    (&surface_0)->opacity_0 = 1.0;
    return surface_0;
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
    uint4 occlusionControl_0;
    uint4 probeVolumeControl_0;
    float4 probeVolumeOrigin_0;
    float4 probeVolumeParams_0;
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


#line 202 "src/rendering/shaders/cy/frame.slang"
struct CyDrawInstance_0
{
    uint instanceSlot_0;
    uint material_0;
    uint parameterOffset_0;
    uint giAddress_0;
    uint layerMask_0;
    uint lodAndFade_0;
    uint surface_1;
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


#line 14
struct _Array_default_Texture2D128_0
{
    array<texture2d<float, access::sample>, int(128)> data_0;
};


#line 14
struct CyFrameGlobalSet_default_0
{
    CyGlobalsData_0 constant* globals_0 [[id(0)]];
    array<texture2d<float, access::sample>, 128> textures_0 [[id(1)]];
    sampler sampler_0 [[id(129)]];
};


#line 14
struct KernelContext_0
{
    CyFrameViewSet_default_0 constant* cyFrameView_0;
    CyFrameGlobalSet_default_0 constant* cyFrameGlobals_0;
};


#line 323 "src/rendering/shaders/cy/frame.slang"
float3 readMaterialFloat3_0(uint material_1, uint wordOffset_0, KernelContext_0 thread* kernelContext_0)
{
    uint _S1 = material_1 * kernelContext_0->cyFrameView_0->frame_0->counts_0.y + wordOffset_0;
    return float3((as_type<float>((kernelContext_0->cyFrameView_0->materialWords_0[_S1]))), (as_type<float>((kernelContext_0->cyFrameView_0->materialWords_0[_S1 + 1U]))), (as_type<float>((kernelContext_0->cyFrameView_0->materialWords_0[_S1 + 2U]))));
}


#line 318
float readMaterialFloat_0(uint material_2, uint wordOffset_1, KernelContext_0 thread* kernelContext_1)
{
    return (as_type<float>((kernelContext_1->cyFrameView_0->materialWords_0[material_2 * kernelContext_1->cyFrameView_0->frame_0->counts_0.y + wordOffset_1])));
}


#line 333
uint readMaterialUint_0(uint material_3, uint wordOffset_2, KernelContext_0 thread* kernelContext_2)
{
    return kernelContext_2->cyFrameView_0->materialWords_0[material_3 * kernelContext_2->cyFrameView_0->frame_0->counts_0.y + wordOffset_2];
}


#line 343
uint materialTextureSlot_0(uint material_4, uint wordOffset_3, KernelContext_0 thread* kernelContext_3)
{
    if(wordOffset_3 == 4294967295U)
    {
        return 4294967295U;
    }

#line 347
    uint _S2 = readMaterialUint_0(material_4, wordOffset_3, kernelContext_3);

    return _S2;
}


#line 257
float4 cyMaterialSampleTexture_0(uint bindlessIndex_0, float2 uv_0, KernelContext_0 thread* kernelContext_4)
{
    return ((kernelContext_4->cyFrameGlobals_0->textures_0[bindlessIndex_0]).sample((kernelContext_4->cyFrameGlobals_0->sampler_0), (uv_0)));
}


#line 379
Surface_0 surfaceOf_0(uint material_5, float3 tint_1, float2 uv_1, KernelContext_0 thread* kernelContext_5)
{
    thread Surface_0 surface_2 = defaultSurface_0();

#line 381
    float3 _S3 = readMaterialFloat3_0(material_5, kernelContext_5->cyFrameView_0->frame_0->materialOffsets_0.x, kernelContext_5);
    (&surface_2)->albedo_0 = _S3 * tint_1;

#line 382
    float _S4 = readMaterialFloat_0(material_5, kernelContext_5->cyFrameView_0->frame_0->materialOffsets_0.y, kernelContext_5);
    (&surface_2)->roughness_0 = clamp(_S4, 0.01999999955296516, 1.0);

#line 383
    float _S5 = readMaterialFloat_0(material_5, kernelContext_5->cyFrameView_0->frame_0->materialOffsets_0.z, kernelContext_5);
    (&surface_2)->metallic_0 = saturate(_S5);

#line 384
    float3 _S6 = readMaterialFloat3_0(material_5, kernelContext_5->cyFrameView_0->frame_0->materialOffsets_0.w, kernelContext_5);
    (&surface_2)->emission_0 = _S6;

#line 385
    uint _S7 = materialTextureSlot_0(material_5, kernelContext_5->cyFrameView_0->frame_0->materialTextures_0.x, kernelContext_5);


    if(_S7 != 4294967295U)
    {

#line 388
        float4 _S8 = cyMaterialSampleTexture_0(_S7, uv_1, kernelContext_5);

        (&surface_2)->albedo_0 = (&surface_2)->albedo_0 * _S8.xyz;

#line 388
    }



    return surface_2;
}


#line 42 "src/rendering/shaders/cy/light.slang"
float distanceAttenuation_0(float distanceSquared_0, float range_1)
{
    float _S9 = distanceSquared_0 / max(range_1 * range_1, 9.99999997475242708e-07);
    float _S10 = saturate(1.0 - _S9 * _S9);
    return _S10 * _S10 / max(distanceSquared_0, 0.00009999999747379);
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
    thread LightSample_0 result_0;

#line 51
    uint _S11 = light_0->kind_0;
    if((light_0->kind_0) == 0U)
    {
        (&result_0)->direction_1 = - light_0->direction_0;
        (&result_0)->attenuation_0 = 1.0;
        (&result_0)->illuminance_0 = light_0->color_0 * float3(light_0->intensity_0) ;
        return result_0;
    }

    float3 _S12 = light_0->positionRelativeToCamera_0 - surfaceRelativeToCamera_0;
    float _S13 = dot(_S12, _S12);
    (&result_0)->direction_1 = _S12 * float3(rsqrt(max(_S13, 9.99999993922529029e-09))) ;
    (&result_0)->attenuation_0 = distanceAttenuation_0(_S13, light_0->range_0);

    if(_S11 == 2U)
    {

        float _S14 = saturate(dot(- (&result_0)->direction_1, light_0->direction_0) * light_0->spotScaleBias_0.x + light_0->spotScaleBias_0.y);
        (&result_0)->attenuation_0 = (&result_0)->attenuation_0 * (_S14 * _S14);

#line 65
    }

#line 71
    (&result_0)->illuminance_0 = light_0->color_0 * float3((light_0->intensity_0 * (&result_0)->attenuation_0)) ;
    return result_0;
}


#line 286 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_6)
{
    float4 _S15 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_6->cyFrameView_0->frame_0->shadowToClipRow0_0, _S15), dot(kernelContext_6->cyFrameView_0->frame_0->shadowToClipRow1_0, _S15), dot(kernelContext_6->cyFrameView_0->frame_0->shadowToClipRow2_0, _S15), dot(kernelContext_6->cyFrameView_0->frame_0->shadowToClipRow3_0, _S15));
}


#line 262
float4 cyMaterialSampleTextureLevel_0(uint bindlessIndex_1, float2 uv_2, float level_0, KernelContext_0 thread* kernelContext_7)
{
    return ((kernelContext_7->cyFrameGlobals_0->textures_0[bindlessIndex_1]).sample((kernelContext_7->cyFrameGlobals_0->sampler_0), (uv_2), level((level_0))));
}


#line 510
float directionalShadowVisibility_0(float3 relativePosition_0, float3 normal_1, KernelContext_0 thread* kernelContext_8)
{

#line 510
    bool _S16;

    if((kernelContext_8->cyFrameView_0->frame_0->shadowControl_0.w) == 0U)
    {

#line 512
        _S16 = true;

#line 512
    }
    else
    {

#line 512
        _S16 = (kernelContext_8->cyFrameView_0->frame_0->shadowControl_0.x) == 4294967295U;

#line 512
    }

#line 512
    if(_S16)
    {
        return 1.0;
    }

#line 514
    float4 _S17 = transformToShadowClip_0(relativePosition_0 + normal_1 * float3(0.00499999988824129) , kernelContext_8);


    float _S18 = _S17.w;

#line 517
    if(_S18 <= 0.0)
    {
        return 1.0;
    }
    float3 _S19 = _S17.xyz / float3(_S18) ;
    float _S20 = _S19.x * 0.5 + 0.5;

#line 522
    float _S21 = 0.5 - _S19.y * 0.5;

#line 522
    float2 _S22 = float2(_S20, _S21);
    if(_S20 < 0.0)
    {

#line 523
        _S16 = true;

#line 523
    }
    else
    {

#line 523
        _S16 = _S20 > 1.0;

#line 523
    }

#line 523
    if(_S16)
    {

#line 523
        _S16 = true;

#line 523
    }
    else
    {

#line 523
        _S16 = _S21 < 0.0;

#line 523
    }

#line 523
    if(_S16)
    {

#line 523
        _S16 = true;

#line 523
    }
    else
    {

#line 523
        _S16 = _S21 > 1.0;

#line 523
    }

#line 523
    if(_S16)
    {
        return 1.0;
    }
    float _S23 = 1.0 / float(max(kernelContext_8->cyFrameView_0->frame_0->shadowControl_0.z, 1U));
    float _S24 = _S19.z + 0.00050000002374873;

#line 528
    int y_0 = int(-1);

#line 528
    float visible_0 = 0.0;

    for(;;)
    {

#line 530
        if(y_0 <= int(1))
        {
        }
        else
        {

#line 530
            break;
        }

#line 530
        int x_0 = int(-1);

        for(;;)
        {

#line 532
            if(x_0 <= int(1))
            {
            }
            else
            {

#line 532
                break;
            }

#line 532
            float4 _S25 = cyMaterialSampleTextureLevel_0(kernelContext_8->cyFrameView_0->frame_0->shadowControl_0.x, _S22 + float2(float(x_0), float(y_0)) * float2(_S23) , 0.0, kernelContext_8);

#line 532
            float _S26;



            if(_S24 >= (_S25.x))
            {

#line 536
                _S26 = 1.0;

#line 536
            }
            else
            {

#line 536
                _S26 = 0.0;

#line 536
            }

#line 536
            float visible_1 = visible_0 + _S26;

#line 532
            x_0 = x_0 + int(1);

#line 532
            visible_0 = visible_1;

#line 532
        }

#line 530
        y_0 = y_0 + int(1);

#line 530
    }

#line 539
    return visible_0 / 9.0;
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
    float _S27 = roughness_1 * roughness_1;
    float _S28 = _S27 * _S27;
    float _S29 = normalDotHalf_0 * normalDotHalf_0 * (_S28 - 1.0) + 1.0;
    return _S28 / max(3.14159274101257324 * _S29 * _S29, 1.00000001168609742e-07);
}


float visibilitySmithGgxCorrelated_0(float normalDotView_0, float normalDotLight_0, float roughness_2)
{

    float _S30 = roughness_2 * roughness_2;
    float _S31 = _S30 * _S30;
    float _S32 = 1.0 - _S31;



    return 0.5 / max(normalDotLight_0 * sqrt(normalDotView_0 * normalDotView_0 * _S32 + _S31) + normalDotView_0 * sqrt(normalDotLight_0 * normalDotLight_0 * _S32 + _S31), 1.00000001168609742e-07);
}

float3 fresnelSchlick_0(float3 f0_0, float viewDotHalf_0)
{

    return f0_0 + (float3(1.0)  - f0_0) * float3(pow(saturate(1.0 - viewDotHalf_0), 5.0)) ;
}


#line 45
float3 specularGgx_0(float3 normal_2, float3 view_0, float3 light_1, float roughness_3, float3 f0_1)
{
    float3 _S33 = normalize(view_0 + light_1);

#line 56
    return float3((distributionGgx_0(saturate(dot(normal_2, _S33)), roughness_3) * visibilitySmithGgxCorrelated_0(saturate(dot(normal_2, view_0)) + 0.00000999999974738, saturate(dot(normal_2, light_1)), roughness_3)))  * fresnelSchlick_0(f0_1, saturate(dot(view_0, _S33)));
}


#line 75 "src/rendering/shaders/cy/material.slang"
float3 shadeSurfaceWithLight_0(const Surface_0 thread* surface_3, float3 worldNormal_0, float3 viewDirection_0, const LightSample_0 thread* sample_0)
{

#line 76
    float3 _S34 = sample_0->direction_1;

    float _S35 = saturate(dot(worldNormal_0, sample_0->direction_1));
    if(_S35 <= 0.0)
    {
        return float3(0.0) ;
    }

#line 87
    return (diffuseLambert_0(surface_3->albedo_0 * float3((1.0 - surface_3->metallic_0)) ) + specularGgx_0(worldNormal_0, viewDirection_0, _S34, surface_3->roughness_0, computeF0_0(surface_3->albedo_0, surface_3->metallic_0))) * sample_0->illuminance_0 * float3(_S35) ;
}


#line 310 "src/rendering/shaders/cy/frame.slang"
float viewDepthOf_0(float3 relative_1, KernelContext_0 thread* kernelContext_9)
{



    return - dot(kernelContext_9->cyFrameView_0->frame_0->relativeToViewRow2_0, float4(relative_1, 1.0));
}


#line 21 "src/rendering/shaders/cy/cluster.slang"
uint clusterSliceOf_0(const ClusterGrid_0 constant* grid_0, float viewDepth_0)
{

    return uint(clamp(log2(max(viewDepth_0, grid_0->nearPlane_0)) * grid_0->sliceScale_0 + grid_0->sliceBias_0, 0.0, float(grid_0->dimensions_0.z - 1U)));
}


uint3 clusterCoordOf_0(const ClusterGrid_0 constant* grid_1, uint2 pixel_0, uint2 renderExtent_0, float viewDepth_1)
{
    uint2 _S36 = grid_1->dimensions_0.xy;
    uint2 _S37 = min(uint2(float2(pixel_0) / float2(renderExtent_0) * float2(_S36)), _S36 - uint2(1U) );

#line 31
    uint _S38 = clusterSliceOf_0(grid_1, viewDepth_1);

#line 31
    return uint3(_S37, _S38);
}



uint clusterIndexOf_0(const ClusterGrid_0 constant* grid_2, uint3 coord_0)
{
    return coord_0.x + grid_2->dimensions_0.x * (coord_0.y + grid_2->dimensions_0.y * coord_0.z);
}


#line 542 "src/rendering/shaders/cy/frame.slang"
float3 accumulateLights_0(const Surface_0 thread* surface_4, float3 relativePosition_1, float3 normal_3, float3 viewDir_0, uint2 pixel_1, uint instanceFlags_0, KernelContext_0 thread* kernelContext_10)
{

#line 543
    bool _S39;

    float3 _S40 = float3(0.0) ;
    uint _S41 = kernelContext_10->cyFrameView_0->frame_0->counts_0.x;

#line 546
    uint global_0 = 0U;

#line 546
    float3 lit_0 = _S40;

#line 555
    for(;;)
    {

#line 555
        if(global_0 < _S41)
        {
        }
        else
        {

#line 555
            break;
        }
        if((kernelContext_10->cyFrameView_0->lights_0[global_0].kind_0) != 0U)
        {
            global_0 = global_0 + 1U;

#line 555
            continue;
        }

#line 555
        thread Light_0 _S42 = kernelContext_10->cyFrameView_0->lights_0[global_0];

#line 555
        LightSample_0 _S43 = evaluateLight_0(&_S42, relativePosition_1);

#line 562
        if(global_0 == (kernelContext_10->cyFrameView_0->frame_0->shadowControl_0.y))
        {

#line 562
            _S39 = (instanceFlags_0 & 8U) != 0U;

#line 562
        }
        else
        {

#line 562
            _S39 = false;

#line 562
        }

#line 562
        float _S44;
        if(_S39)
        {

#line 563
            float _S45 = directionalShadowVisibility_0(relativePosition_1, normal_3, kernelContext_10);

#line 563
            _S44 = _S45;

#line 563
        }
        else
        {

#line 563
            _S44 = 1.0;

#line 563
        }

#line 563
        thread LightSample_0 _S46 = _S43;

#line 563
        float3 _S47 = shadeSurfaceWithLight_0(surface_4, normal_3, viewDir_0, &_S46);

#line 563
        lit_0 = lit_0 + _S47 * float3(_S44) ;

#line 555
        global_0 = global_0 + 1U;

#line 555
    }

#line 567
    if((kernelContext_10->cyFrameView_0->frame_0->counts_0.w) == 0U)
    {

#line 567
        _S39 = true;

#line 567
    }
    else
    {

#line 567
        _S39 = ((&kernelContext_10->cyFrameView_0->frame_0->clusterGrid_0)->dimensions_0.z) == 0U;

#line 567
    }

#line 567
    uint index_0;

#line 567
    if(_S39)
    {

#line 567
        index_0 = 0U;

        for(;;)
        {

#line 569
            if(index_0 < _S41)
            {
            }
            else
            {

#line 569
                break;
            }
            if((kernelContext_10->cyFrameView_0->lights_0[index_0].kind_0) == 0U)
            {
                index_0 = index_0 + 1U;

#line 569
                continue;
            }

#line 569
            thread Light_0 _S48 = kernelContext_10->cyFrameView_0->lights_0[index_0];

#line 569
            LightSample_0 _S49 = evaluateLight_0(&_S48, relativePosition_1);

#line 569
            thread LightSample_0 _S50 = _S49;

#line 569
            float3 _S51 = shadeSurfaceWithLight_0(surface_4, normal_3, viewDir_0, &_S50);

#line 569
            lit_0 = lit_0 + _S51;

#line 569
            index_0 = index_0 + 1U;

#line 569
        }

#line 578
        return lit_0;
    }

    uint2 _S52 = uint2(kernelContext_10->cyFrameView_0->frame_0->extentAndInverse_0.xy);

#line 581
    float _S53 = viewDepthOf_0(relativePosition_1, kernelContext_10);

#line 581
    uint3 _S54 = clusterCoordOf_0(&kernelContext_10->cyFrameView_0->frame_0->clusterGrid_0, pixel_1, _S52, _S53);

#line 581
    uint _S55 = clusterIndexOf_0(&kernelContext_10->cyFrameView_0->frame_0->clusterGrid_0, _S54);

#line 586
    uint2 _S56 = kernelContext_10->cyFrameView_0->clusterHeaders_0[_S55 * kernelContext_10->cyFrameView_0->frame_0->counts_0.z];

#line 586
    index_0 = 0U;
    for(;;)
    {

#line 587
        if(index_0 < (_S56.y))
        {
        }
        else
        {

#line 587
            break;
        }
        uint _S57 = kernelContext_10->cyFrameView_0->clusterIndices_0[_S56.x + index_0];
        if(_S57 >= _S41)
        {

#line 590
            _S39 = true;

#line 590
        }
        else
        {

#line 590
            _S39 = (kernelContext_10->cyFrameView_0->lights_0[_S57].kind_0) == 0U;

#line 590
        }

#line 590
        if(_S39)
        {
            index_0 = index_0 + 1U;

#line 587
            continue;
        }

#line 587
        thread Light_0 _S58 = kernelContext_10->cyFrameView_0->lights_0[_S57];

#line 587
        LightSample_0 _S59 = evaluateLight_0(&_S58, relativePosition_1);

#line 587
        thread LightSample_0 _S60 = _S59;

#line 587
        float3 _S61 = shadeSurfaceWithLight_0(surface_4, normal_3, viewDir_0, &_S60);

#line 587
        lit_0 = lit_0 + _S61;

#line 587
        index_0 = index_0 + 1U;

#line 587
    }

#line 597
    return lit_0;
}


#line 617
float4 probeVolumeTexel_0(uint3 probe_0, uint texel_0, KernelContext_0 thread* kernelContext_11)
{
    uint3 _S62 = kernelContext_11->cyFrameView_0->frame_0->probeVolumeControl_0.yzw;

    uint _S63 = _S62.y;

#line 621
    float4 _S64 = cyMaterialSampleTextureLevel_0(kernelContext_11->cyFrameView_0->frame_0->probeVolumeControl_0.x, (float2(float(probe_0.x * 5U + texel_0), float(probe_0.y + _S63 * probe_0.z)) + float2(0.5) ) / float2(float(_S62.x * 5U), float(_S63 * _S62.z)), 0.0, kernelContext_11);


    return _S64;
}



float probeVisibility_0(float4 distances0_0, float4 distances1_0, float3 probePosition_0, float3 position_0, float3 normal_4, float3 query_0, KernelContext_0 thread* kernelContext_12)
{

    float3 _S65 = probePosition_0 - position_0;
    float _S66 = dot(_S65, _S65);

#line 633
    float3 _S67;
    if(_S66 > 9.999999960041972e-13)
    {

#line 634
        _S67 = _S65 * float3(rsqrt(_S66)) ;

#line 634
    }
    else
    {

#line 634
        _S67 = normal_4;

#line 634
    }
    float _S68 = max(0.00009999999747379, (dot(_S67, normal_4) + 1.0) * 0.5);
    float _S69 = _S68 * _S68 + 0.20000000298023224;



    float3 _S70 = query_0 - probePosition_0;

#line 640
    float _S71;
    if((_S70.x) >= 0.0)
    {

#line 641
        _S71 = distances0_0.y;

#line 641
    }
    else
    {

#line 641
        _S71 = distances0_0.z;

#line 641
    }

#line 641
    float _S72;
    if((_S70.y) >= 0.0)
    {

#line 642
        _S72 = distances0_0.w;

#line 642
    }
    else
    {

#line 642
        _S72 = distances1_0.x;

#line 642
    }

#line 642
    float _S73;
    if((_S70.z) >= 0.0)
    {

#line 643
        _S73 = distances1_0.y;

#line 643
    }
    else
    {

#line 643
        _S73 = distances1_0.z;

#line 643
    }

#line 643
    float3 _S74 = float3(kernelContext_12->cyFrameView_0->frame_0->probeVolumeParams_0.z) ;

    float3 _S75 = saturate((float3(_S71, _S72, _S73) + _S74 - abs(_S70)) / _S74);
    return _S69 * min(_S75.x, min(_S75.y, _S75.z));
}



float3 probeVolumeAmbient_0(float3 position_1, float3 normal_5, float3 flat_0, KernelContext_0 thread* kernelContext_13)
{
    uint3 _S76 = kernelContext_13->cyFrameView_0->frame_0->probeVolumeControl_0.yzw;

    float3 _S77 = position_1 + normal_5 * float3(kernelContext_13->cyFrameView_0->frame_0->probeVolumeParams_0.y) ;

#line 655
    float3 _S78 = float3(kernelContext_13->cyFrameView_0->frame_0->probeVolumeOrigin_0.w) ;
    float3 _S79 = (_S77 - kernelContext_13->cyFrameView_0->frame_0->probeVolumeOrigin_0.xyz) / _S78;

#line 656
    float3 _S80 = float3(1.0) ;
    float3 _S81 = float3(_S76) - _S80;
    float3 _S82 = float3(0.0) ;

#line 658
    float3 _S83 = max(max(- _S79, _S79 - _S81), _S82);
    float _S84 = saturate(1.0 - max(_S83.x, max(_S83.y, _S83.z)));
    if(_S84 <= 0.0)
    {
        return flat_0;
    }
    float3 _S85 = clamp(_S79, _S82, _S81);
    uint3 _S86 = min(uint3(floor(_S85)), uint3(max(int3(_S76) - int3(int(2)) , int3(int(0)) )));
    float3 _S87 = _S85 - float3(_S86);


    float4 _S88 = float4(0.28209498524665833, 0.32573533058166504 * normal_5.y, 0.32573533058166504 * normal_5.z, 0.32573533058166504 * normal_5.x);

#line 669
    uint corner_0 = 0U;

#line 669
    float3 total_0 = _S82;

#line 669
    float weightSum_0 = 0.0;



    for(;;)
    {

#line 673
        if(corner_0 < 8U)
        {
        }
        else
        {

#line 673
            break;
        }
        uint3 _S89 = uint3(corner_0 & 1U, (corner_0 >> 1U) & 1U, (corner_0 >> 2U) & 1U);
        uint3 _S90 = min(_S86 + _S89, _S76 - uint3(1U) );
        float3 _S91 = float3(_S89);
        float3 _S92 = _S91 * _S87 + (_S80 - _S91) * (_S80 - _S87);

#line 678
        float4 _S93 = probeVolumeTexel_0(_S90, 3U, kernelContext_13);

        float _S94 = _S92.x * _S92.y * _S92.z * _S93.x;
        if(_S94 <= 0.0)
        {
            corner_0 = corner_0 + 1U;

#line 673
            continue;
        }

#line 673
        float4 _S95 = probeVolumeTexel_0(_S90, 4U, kernelContext_13);

#line 673
        float _S96 = probeVisibility_0(_S93, _S95, kernelContext_13->cyFrameView_0->frame_0->probeVolumeOrigin_0.xyz + float3(_S90) * _S78, position_1, normal_5, _S77, kernelContext_13);

#line 688
        float _S97 = _S94 * _S96;

#line 688
        float4 _S98 = probeVolumeTexel_0(_S90, 0U, kernelContext_13);
        float _S99 = dot(_S98, _S88);

#line 689
        float4 _S100 = probeVolumeTexel_0(_S90, 1U, kernelContext_13);
        float _S101 = dot(_S100, _S88);

#line 690
        float4 _S102 = probeVolumeTexel_0(_S90, 2U, kernelContext_13);



        float weightSum_1 = weightSum_0 + _S97;

#line 694
        total_0 = total_0 + max(float3(_S99, _S101, dot(_S102, _S88)) * float3(kernelContext_13->cyFrameView_0->frame_0->probeVolumeParams_0.x) , _S82) * float3(_S97) ;

#line 694
        weightSum_0 = weightSum_1;

#line 673
        corner_0 = corner_0 + 1U;

#line 673
    }

#line 696
    if(weightSum_0 > 9.99999997475242708e-07)
    {

#line 696
        total_0 = total_0 / float3(weightSum_0) ;

#line 696
    }
    else
    {

#line 696
        total_0 = flat_0;

#line 696
    }
    return mix(flat_0, total_0, float3(_S84) );
}


#line 602
float occlusionVisibility_0(float2 fragmentCentre_0, KernelContext_0 thread* kernelContext_14)
{

#line 602
    float4 _S103 = cyMaterialSampleTextureLevel_0(kernelContext_14->cyFrameView_0->frame_0->occlusionControl_0.x, fragmentCentre_0 * kernelContext_14->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_14);


    return _S103.w;
}


#line 605
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 605
struct pixelInput_0
{
    float3 relativePosition_2 [[user(TEXCOORD)]];
    float3 normal_6 [[user(TEXCOORD_1)]];
    float2 uv_3 [[user(TEXCOORD_2)]];
    [[flat]] uint drawIndex_0 [[user(TEXCOORD_3)]];
};


#line 701
[[fragment]] pixelOutput_0 cyForwardFragment(pixelInput_0 _S104 [[stage_in]], float4 position_2 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyFrameGlobalSet_default_0 constant& cyFrameGlobals_1 [[buffer(0)]])
{

#line 701
    thread KernelContext_0 kernelContext_15;

#line 701
    (&kernelContext_15)->cyFrameView_0 = cyFrameView_1;

#line 701
    (&kernelContext_15)->cyFrameGlobals_0 = &cyFrameGlobals_1;

    float2 _S105 = position_2.xy;

#line 703
    uint2 _S106 = uint2(_S105);
    CyDrawInstance_0 _S107 = cyFrameView_1->drawInstances_0[_S104.drawIndex_0];

#line 704
    Surface_0 _S108 = surfaceOf_0(_S107.material_0, cyFrameView_1->instances_0[_S107.instanceSlot_0].tint_0.xyz, _S104.uv_3, &kernelContext_15);



    float3 _S109 = normalize(_S104.normal_6);


    float3 _S110 = normalize(- _S104.relativePosition_2);

#line 711
    thread Surface_0 _S111 = _S108;

#line 711
    float3 _S112 = accumulateLights_0(&_S111, _S104.relativePosition_2, _S109, _S110, _S106, _S107.flags_0, &kernelContext_15);

#line 721
    float3 ambientRadiance_0 = (&kernelContext_15)->cyFrameView_0->frame_0->ambientAndOcclusion_0.xyz;

#line 721
    float3 ambientRadiance_1;
    if(((&kernelContext_15)->cyFrameView_0->frame_0->probeVolumeControl_0.x) != 4294967295U)
    {

#line 722
        float3 _S113 = probeVolumeAmbient_0(_S104.relativePosition_2, _S109, ambientRadiance_0, &kernelContext_15);

#line 722
        ambientRadiance_1 = _S113;

#line 722
    }
    else
    {

#line 722
        ambientRadiance_1 = ambientRadiance_0;

#line 722
    }



    float3 ambient_0 = _S108.albedo_0 * ambientRadiance_1 * float3(_S108.occlusion_0) ;

#line 726
    float3 color_1;

#line 726
    float3 ambient_1;
    if(((&kernelContext_15)->cyFrameView_0->frame_0->occlusionControl_0.x) != 4294967295U)
    {

#line 727
        float _S114 = occlusionVisibility_0(_S105, &kernelContext_15);


        float3 ambient_2 = ambient_0 * float3(_S114) ;
        if(((&kernelContext_15)->cyFrameView_0->frame_0->occlusionControl_0.y) != 0U)
        {

#line 731
            color_1 = _S112 * float3(mix(1.0, _S114, (as_type<float>(((&kernelContext_15)->cyFrameView_0->frame_0->occlusionControl_0.z))))) ;

#line 731
        }
        else
        {

#line 731
            color_1 = _S112;

#line 731
        }

#line 731
        ambient_1 = ambient_2;

#line 727
    }
    else
    {

#line 727
        color_1 = _S112;

#line 727
        ambient_1 = ambient_0;

#line 727
    }

#line 727
    pixelOutput_0 _S115 = { float4(color_1 + _S108.emission_0 + ambient_1, _S108.opacity_0) };

#line 738
    return _S115;
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

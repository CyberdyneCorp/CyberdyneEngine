#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the rendering frame. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::pipeline {

/// DepthVertex.metal, 6430 bytes.
inline constexpr char kFrameDepthVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 221 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 301
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


#line 113 "src/rendering/shaders/cy/frame.slang"
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


#line 208 "src/rendering/shaders/cy/frame.slang"
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


#line 230
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


#line 275
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


#line 285 "src/rendering/shaders/cy/frame.slang"
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


#line 310 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 312
struct cyDepthVertex_Result_0
{
    float4 position_0 [[position]];
    float3 normal_1 [[user(TEXCOORD)]];
    float4 currentClip_0 [[user(TEXCOORD_1)]];
    float4 previousClip_0 [[user(TEXCOORD_2)]];
};


#line 312
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
};


#line 428
struct CyDepthVertex_0
{
    float4 position_1;
    float3 normal_2;
    float4 currentClip_1;
    float4 previousClip_1;
};


#line 428
[[vertex]] cyDepthVertex_Result_0 cyDepthVertex(vertexInput_0 _S9 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 428
    thread KernelContext_0 kernelContext_1;

#line 428
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 428
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 440
    CyInstanceTransform_0 _S10 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

#line 440
    thread CyInstanceTransform_0 _S11 = _S10;

#line 440
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    thread CyDepthVertex_0 output_0;

#line 441
    float4 _S13 = transformToClip_0(_S12, &kernelContext_1);

    (&output_0)->position_1 = _S13;
    (&output_0)->currentClip_1 = _S13;
    float4 _S14 = float4(_S12, 1.0);
    (&output_0)->previousClip_1 = float4(dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow0_0, _S14), dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow1_0, _S14), dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow2_0, _S14), dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow3_0, _S14));



    float3 _S15 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 450
    thread CyInstanceTransform_0 _S16 = _S10;

#line 450
    float3 _S17 = rotateToRelative_0(&_S16, _S15);

#line 450
    (&output_0)->normal_2 = _S17;

#line 450
    thread cyDepthVertex_Result_0 _S18;

#line 450
    (&_S18)->position_0 = output_0.position_1;

#line 450
    (&_S18)->normal_1 = output_0.normal_2;

#line 450
    (&_S18)->currentClip_0 = output_0.currentClip_1;

#line 450
    (&_S18)->previousClip_0 = output_0.previousClip_1;

#line 450
    return _S18;
}

)cy_msl";

/// DepthFragment.metal, 3850 bytes.
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


#line 454 "src/rendering/shaders/cy/frame.slang"
struct CyDepthOutput_0
{
    float4 normalRoughness_0 [[color(0)]];
    float2 velocity_0 [[color(1)]];
};


#line 454
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


#line 113 "src/rendering/shaders/cy/frame.slang"
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


#line 208 "src/rendering/shaders/cy/frame.slang"
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


#line 461
[[fragment]] CyDepthOutput_0 cyDepthFragment(pixelInput_0 _S4 [[stage_in]], float4 position_0 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_0 [[buffer(1)]])
{
    thread CyDepthOutput_0 output_0;
    (&output_0)->normalRoughness_0 = float4(encodeOctahedral_0(normalize(_S4.normal_1)), 1.0, 1.0);

#line 472
    float2 _S5 = _S4.currentClip_0.xy / float2(_S4.currentClip_0.w)  - cyFrameView_0->frame_0->temporalJitter_0.xy * float2(2.0)  * cyFrameView_0->frame_0->extentAndInverse_0.zw;

    float2 _S6 = _S4.previousClip_0.xy / float2(_S4.previousClip_0.w) ;
    (&output_0)->velocity_0 = float2((_S6.x - _S5.x) * 0.5, (_S5.y - _S6.y) * 0.5);

    return output_0;
}

)cy_msl";

/// ShadowVertex.metal, 4439 bytes.
inline constexpr char kFrameShadowVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 221 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 301
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


#line 113 "src/rendering/shaders/cy/frame.slang"
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


#line 208 "src/rendering/shaders/cy/frame.slang"
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


#line 230
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


#line 275
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


#line 292 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow3_0, _S2));
}


#line 295
struct cyShadowVertex_Result_0
{
    float4 position_0 [[position]];
};


#line 295
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
};


#line 403
struct CyShadowVertex_0
{
    float4 position_1;
};


#line 403
[[vertex]] cyShadowVertex_Result_0 cyShadowVertex(vertexInput_0 _S3 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 403
    thread KernelContext_0 kernelContext_1;

#line 403
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 403
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 413
    thread CyShadowVertex_0 output_0;

#line 413
    thread CyInstanceTransform_0 _S4 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

#line 413
    float3 _S5 = transformToRelative_0(&_S4, _S3.modelPosition_1);

#line 413
    float4 _S6 = transformToShadowClip_0(_S5, &kernelContext_1);
    (&output_0)->position_1 = _S6;

#line 414
    thread cyShadowVertex_Result_0 _S7;

#line 414
    (&_S7)->position_0 = output_0.position_1;

#line 414
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


#line 419 "src/rendering/shaders/cy/frame.slang"
[[fragment]] pixelOutput_0 cyShadowFragment(float4 position_0 [[position]])
{

#line 419
    pixelOutput_0 _S1 = { position_0.z };

    return _S1;
}

)cy_msl";

/// ForwardVertex.metal, 6264 bytes.
inline constexpr char kFrameForwardVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 221 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 301
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


#line 113 "src/rendering/shaders/cy/frame.slang"
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


#line 208 "src/rendering/shaders/cy/frame.slang"
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


#line 230
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


#line 275
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


#line 285 "src/rendering/shaders/cy/frame.slang"
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


#line 310 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 312
struct cyForwardVertex_Result_0
{
    float4 position_0 [[position]];
    float3 relativePosition_0 [[user(TEXCOORD)]];
    float3 normal_1 [[user(TEXCOORD_1)]];
    float2 uv_0 [[user(TEXCOORD_2)]];
    uint drawIndex_1 [[user(TEXCOORD_3)]];
};


#line 312
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
    float2 uv_1 [[attribute(2)]];
};


#line 482
struct CyForwardVertex_0
{
    float4 position_1;
    float3 relativePosition_1;
    float3 normal_2;
    float2 uv_2;
    [[flat]] uint drawIndex_2;
};


#line 482
[[vertex]] cyForwardVertex_Result_0 cyForwardVertex(vertexInput_0 _S9 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 482
    thread KernelContext_0 kernelContext_1;

#line 482
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 482
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 496
    CyInstanceTransform_0 _S10 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

    thread CyForwardVertex_0 output_0;

#line 498
    thread CyInstanceTransform_0 _S11 = _S10;

#line 498
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    (&output_0)->relativePosition_1 = _S12;

#line 499
    float4 _S13 = transformToClip_0(_S12, &kernelContext_1);
    (&output_0)->position_1 = _S13;



    float3 _S14 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 504
    thread CyInstanceTransform_0 _S15 = _S10;

#line 504
    float3 _S16 = rotateToRelative_0(&_S15, _S14);

#line 504
    (&output_0)->normal_2 = _S16;
    (&output_0)->uv_2 = _S9.uv_1;
    (&output_0)->drawIndex_2 = cyDraw_1->drawIndex_0;

#line 506
    thread cyForwardVertex_Result_0 _S17;

#line 506
    (&_S17)->position_0 = output_0.position_1;

#line 506
    (&_S17)->relativePosition_0 = output_0.relativePosition_1;

#line 506
    (&_S17)->normal_1 = output_0.normal_2;

#line 506
    (&_S17)->uv_0 = output_0.uv_2;

#line 506
    (&_S17)->drawIndex_1 = output_0.drawIndex_2;

#line 506
    return _S17;
}

)cy_msl";

/// ForwardFragment.metal, 30349 bytes.
inline constexpr char kFrameForwardFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 512 "src/rendering/shaders/cy/frame.slang"
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


#line 113 "src/rendering/shaders/cy/frame.slang"
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


#line 208 "src/rendering/shaders/cy/frame.slang"
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


#line 268 "src/rendering/shaders/cy/frame.slang"
float4 cyMaterialSampleTextureLevel_0(uint bindlessIndex_0, float2 uv_0, float level_0, KernelContext_0 thread* kernelContext_0)
{
    return ((kernelContext_0->cyFrameGlobals_0->textures_0[bindlessIndex_0]).sample((kernelContext_0->cyFrameGlobals_0->sampler_0), (uv_0), level((level_0))));
}


#line 515
float CyFrameShadowMap_storedDepth_0(const CyFrameShadowMap_0 thread* this_0, float2 uv_1, KernelContext_0 thread* kernelContext_1)
{

#line 515
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


#line 515 "src/rendering/shaders/cy/frame.slang"
float CyFrameShadowMap_storedDepth_1(const CyFrameShadowMap_0 thread* this_1, float2 uv_3, KernelContext_0 thread* kernelContext_3)
{

#line 515
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


#line 164
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
float pcssVisibility_0(const CyFrameShadowMap_0 thread* source_2, float2 uv_5, float receiver_3, const PcssShape_0 thread* shape_0, float rotation_2, KernelContext_0 thread* kernelContext_5)
{

#line 182
    float _S15 = shape_0->penumbraPerDepth_0;

#line 182
    float _S16 = shape_0->minRadius_0;

#line 182
    float _S17 = shape_0->maxRadius_0;

#line 182
    PcssBlockers_0 _S18 = pcssBlockerSearch_0(source_2, uv_5, receiver_3, clamp((1.0 - receiver_3) * shape_0->penumbraPerDepth_0, shape_0->minRadius_0, shape_0->maxRadius_0), rotation_2, shape_0->blockerTaps_0, kernelContext_5);

#line 189
    if((_S18.count_2) == 0.0)
    {
        return 1.0;
    }

#line 191
    float _S19 = pcssFilter_0(source_2, uv_5, receiver_3, clamp(pcssPenumbraRadius_0(receiver_3, _S18.averageDepth_0, _S15), _S16, _S17), rotation_2, shape_0->filterTaps_0, kernelContext_5);



    return _S19;
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


#line 329 "src/rendering/shaders/cy/frame.slang"
float3 readMaterialFloat3_0(uint material_1, uint wordOffset_0, KernelContext_0 thread* kernelContext_6)
{
    uint _S20 = material_1 * kernelContext_6->cyFrameView_0->frame_0->counts_0.y + wordOffset_0;
    return float3((as_type<float>((kernelContext_6->cyFrameView_0->materialWords_0[_S20]))), (as_type<float>((kernelContext_6->cyFrameView_0->materialWords_0[_S20 + 1U]))), (as_type<float>((kernelContext_6->cyFrameView_0->materialWords_0[_S20 + 2U]))));
}


#line 324
float readMaterialFloat_0(uint material_2, uint wordOffset_1, KernelContext_0 thread* kernelContext_7)
{
    return (as_type<float>((kernelContext_7->cyFrameView_0->materialWords_0[material_2 * kernelContext_7->cyFrameView_0->frame_0->counts_0.y + wordOffset_1])));
}


#line 339
uint readMaterialUint_0(uint material_3, uint wordOffset_2, KernelContext_0 thread* kernelContext_8)
{
    return kernelContext_8->cyFrameView_0->materialWords_0[material_3 * kernelContext_8->cyFrameView_0->frame_0->counts_0.y + wordOffset_2];
}


#line 349
uint materialTextureSlot_0(uint material_4, uint wordOffset_3, KernelContext_0 thread* kernelContext_9)
{
    if(wordOffset_3 == 4294967295U)
    {
        return 4294967295U;
    }

#line 353
    uint _S21 = readMaterialUint_0(material_4, wordOffset_3, kernelContext_9);

    return _S21;
}


#line 263
float4 cyMaterialSampleTexture_0(uint bindlessIndex_1, float2 uv_6, KernelContext_0 thread* kernelContext_10)
{
    return ((kernelContext_10->cyFrameGlobals_0->textures_0[bindlessIndex_1]).sample((kernelContext_10->cyFrameGlobals_0->sampler_0), (uv_6)));
}


#line 385
Surface_0 surfaceOf_0(uint material_5, float3 tint_1, float2 uv_7, KernelContext_0 thread* kernelContext_11)
{
    thread Surface_0 surface_2 = defaultSurface_0();

#line 387
    float3 _S22 = readMaterialFloat3_0(material_5, kernelContext_11->cyFrameView_0->frame_0->materialOffsets_0.x, kernelContext_11);
    (&surface_2)->albedo_0 = _S22 * tint_1;

#line 388
    float _S23 = readMaterialFloat_0(material_5, kernelContext_11->cyFrameView_0->frame_0->materialOffsets_0.y, kernelContext_11);
    (&surface_2)->roughness_0 = clamp(_S23, 0.01999999955296516, 1.0);

#line 389
    float _S24 = readMaterialFloat_0(material_5, kernelContext_11->cyFrameView_0->frame_0->materialOffsets_0.z, kernelContext_11);
    (&surface_2)->metallic_0 = saturate(_S24);

#line 390
    float3 _S25 = readMaterialFloat3_0(material_5, kernelContext_11->cyFrameView_0->frame_0->materialOffsets_0.w, kernelContext_11);
    (&surface_2)->emission_0 = _S25;

#line 391
    uint _S26 = materialTextureSlot_0(material_5, kernelContext_11->cyFrameView_0->frame_0->materialTextures_0.x, kernelContext_11);


    if(_S26 != 4294967295U)
    {

#line 394
        float4 _S27 = cyMaterialSampleTexture_0(_S26, uv_7, kernelContext_11);

        (&surface_2)->albedo_0 = (&surface_2)->albedo_0 * _S27.xyz;

#line 394
    }



    return surface_2;
}


#line 42 "src/rendering/shaders/cy/light.slang"
float distanceAttenuation_0(float distanceSquared_0, float range_1)
{
    float _S28 = distanceSquared_0 / max(range_1 * range_1, 9.99999997475242708e-07);
    float _S29 = saturate(1.0 - _S28 * _S28);
    return _S29 * _S29 / max(distanceSquared_0, 0.00009999999747379);
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
    thread LightSample_0 result_1;

#line 51
    uint _S30 = light_0->kind_0;
    if((light_0->kind_0) == 0U)
    {
        (&result_1)->direction_1 = - light_0->direction_0;
        (&result_1)->attenuation_0 = 1.0;
        (&result_1)->illuminance_0 = light_0->color_0 * float3(light_0->intensity_0) ;
        return result_1;
    }

    float3 _S31 = light_0->positionRelativeToCamera_0 - surfaceRelativeToCamera_0;
    float _S32 = dot(_S31, _S31);
    (&result_1)->direction_1 = _S31 * float3(rsqrt(max(_S32, 9.99999993922529029e-09))) ;
    (&result_1)->attenuation_0 = distanceAttenuation_0(_S32, light_0->range_0);

    if(_S30 == 2U)
    {

        float _S33 = saturate(dot(- (&result_1)->direction_1, light_0->direction_0) * light_0->spotScaleBias_0.x + light_0->spotScaleBias_0.y);
        (&result_1)->attenuation_0 = (&result_1)->attenuation_0 * (_S33 * _S33);

#line 65
    }

#line 71
    (&result_1)->illuminance_0 = light_0->color_0 * float3((light_0->intensity_0 * (&result_1)->attenuation_0)) ;
    return result_1;
}


#line 292 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_12)
{
    float4 _S34 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_12->cyFrameView_0->frame_0->shadowToClipRow0_0, _S34), dot(kernelContext_12->cyFrameView_0->frame_0->shadowToClipRow1_0, _S34), dot(kernelContext_12->cyFrameView_0->frame_0->shadowToClipRow2_0, _S34), dot(kernelContext_12->cyFrameView_0->frame_0->shadowToClipRow3_0, _S34));
}


#line 98 "src/rendering/shaders/cy/shadow.slang"
float shadowDiscRotation_0(float2 pixel_0)
{

    return fract(52.98291778564453125 * fract(dot(pixel_0, float2(0.06711056083440781, 0.00583714991807938)))) * 6.28318548202514648;
}


#line 523 "src/rendering/shaders/cy/frame.slang"
float softShadowVisibility_0(float2 uv_8, float reference_0, float2 pixel_1, KernelContext_0 thread* kernelContext_13)
{
    thread CyFrameShadowMap_0 map_0;
    (&map_0)->slot_0 = kernelContext_13->cyFrameView_0->frame_0->shadowControl_0.x;
    thread PcssShape_0 shape_1;
    (&shape_1)->penumbraPerDepth_0 = kernelContext_13->cyFrameView_0->frame_0->softShadowShape_0.x;
    (&shape_1)->minRadius_0 = kernelContext_13->cyFrameView_0->frame_0->softShadowShape_0.y;
    (&shape_1)->maxRadius_0 = kernelContext_13->cyFrameView_0->frame_0->softShadowShape_0.z;
    (&shape_1)->blockerTaps_0 = int(max(kernelContext_13->cyFrameView_0->frame_0->softShadowControl_0.z, 1U));
    (&shape_1)->filterTaps_0 = int(max(kernelContext_13->cyFrameView_0->frame_0->softShadowControl_0.w, 1U));
    float _S35 = shadowDiscRotation_0(pixel_1);

#line 533
    thread CyFrameShadowMap_0 _S36 = map_0;

#line 533
    thread PcssShape_0 _S37 = shape_1;

#line 533
    float _S38 = pcssVisibility_0(&_S36, uv_8, reference_0, &_S37, _S35, kernelContext_13);

#line 533
    return _S38;
}


#line 556
float directionalShadowVisibility_0(float3 relativePosition_0, float3 normal_1, float2 fragmentCentre_0, KernelContext_0 thread* kernelContext_14)
{

#line 556
    bool _S39;

    if((kernelContext_14->cyFrameView_0->frame_0->shadowControl_0.w) == 0U)
    {

#line 558
        _S39 = true;

#line 558
    }
    else
    {

#line 558
        _S39 = (kernelContext_14->cyFrameView_0->frame_0->shadowControl_0.x) == 4294967295U;

#line 558
    }

#line 558
    if(_S39)
    {
        return 1.0;
    }

#line 560
    float4 _S40 = transformToShadowClip_0(relativePosition_0 + normal_1 * float3(0.00499999988824129) , kernelContext_14);


    float _S41 = _S40.w;

#line 563
    if(_S41 <= 0.0)
    {
        return 1.0;
    }
    float3 _S42 = _S40.xyz / float3(_S41) ;
    float _S43 = _S42.x * 0.5 + 0.5;

#line 568
    float _S44 = 0.5 - _S42.y * 0.5;

#line 568
    float2 _S45 = float2(_S43, _S44);
    if(_S43 < 0.0)
    {

#line 569
        _S39 = true;

#line 569
    }
    else
    {

#line 569
        _S39 = _S43 > 1.0;

#line 569
    }

#line 569
    if(_S39)
    {

#line 569
        _S39 = true;

#line 569
    }
    else
    {

#line 569
        _S39 = _S44 < 0.0;

#line 569
    }

#line 569
    if(_S39)
    {

#line 569
        _S39 = true;

#line 569
    }
    else
    {

#line 569
        _S39 = _S44 > 1.0;

#line 569
    }

#line 569
    if(_S39)
    {
        return 1.0;
    }
    float _S46 = 1.0 / float(max(kernelContext_14->cyFrameView_0->frame_0->shadowControl_0.z, 1U));
    float _S47 = _S42.z + 0.00050000002374873;
    if(((kernelContext_14->cyFrameView_0->frame_0->softShadowControl_0.x) & 1U) != 0U)
    {

#line 575
        float _S48 = softShadowVisibility_0(_S45, _S47, fragmentCentre_0, kernelContext_14);

        return _S48;
    }

#line 577
    int y_0 = int(-1);

#line 577
    float visible_0 = 0.0;


    for(;;)
    {

#line 580
        if(y_0 <= int(1))
        {
        }
        else
        {

#line 580
            break;
        }

#line 580
        int x_0 = int(-1);

        for(;;)
        {

#line 582
            if(x_0 <= int(1))
            {
            }
            else
            {

#line 582
                break;
            }

#line 582
            float4 _S49 = cyMaterialSampleTextureLevel_0(kernelContext_14->cyFrameView_0->frame_0->shadowControl_0.x, _S45 + float2(float(x_0), float(y_0)) * float2(_S46) , 0.0, kernelContext_14);

#line 582
            float _S50;



            if(_S47 >= (_S49.x))
            {

#line 586
                _S50 = 1.0;

#line 586
            }
            else
            {

#line 586
                _S50 = 0.0;

#line 586
            }

#line 586
            float visible_1 = visible_0 + _S50;

#line 582
            x_0 = x_0 + int(1);

#line 582
            visible_0 = visible_1;

#line 582
        }

#line 580
        y_0 = y_0 + int(1);

#line 580
    }

#line 589
    return visible_0 / 9.0;
}


#line 539
float contactShadowVisibility_0(float2 fragmentCentre_1, KernelContext_0 thread* kernelContext_15)
{

#line 539
    bool _S51;

    if(((kernelContext_15->cyFrameView_0->frame_0->softShadowControl_0.x) & 2U) == 0U)
    {

#line 541
        _S51 = true;

#line 541
    }
    else
    {

#line 541
        _S51 = (kernelContext_15->cyFrameView_0->frame_0->softShadowControl_0.y) == 4294967295U;

#line 541
    }

#line 541
    if(_S51)
    {

        return 1.0;
    }

#line 544
    float4 _S52 = cyMaterialSampleTextureLevel_0(kernelContext_15->cyFrameView_0->frame_0->softShadowControl_0.y, fragmentCentre_1 * kernelContext_15->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_15);


    return _S52.x;
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
    float _S53 = roughness_1 * roughness_1;
    float _S54 = _S53 * _S53;
    float _S55 = normalDotHalf_0 * normalDotHalf_0 * (_S54 - 1.0) + 1.0;
    return _S54 / max(3.14159274101257324 * _S55 * _S55, 1.00000001168609742e-07);
}


float visibilitySmithGgxCorrelated_0(float normalDotView_0, float normalDotLight_0, float roughness_2)
{

    float _S56 = roughness_2 * roughness_2;
    float _S57 = _S56 * _S56;
    float _S58 = 1.0 - _S57;



    return 0.5 / max(normalDotLight_0 * sqrt(normalDotView_0 * normalDotView_0 * _S58 + _S57) + normalDotView_0 * sqrt(normalDotLight_0 * normalDotLight_0 * _S58 + _S57), 1.00000001168609742e-07);
}

float3 fresnelSchlick_0(float3 f0_0, float viewDotHalf_0)
{

    return f0_0 + (float3(1.0)  - f0_0) * float3(pow(saturate(1.0 - viewDotHalf_0), 5.0)) ;
}


#line 45
float3 specularGgx_0(float3 normal_2, float3 view_0, float3 light_1, float roughness_3, float3 f0_1)
{
    float3 _S59 = normalize(view_0 + light_1);

#line 56
    return float3((distributionGgx_0(saturate(dot(normal_2, _S59)), roughness_3) * visibilitySmithGgxCorrelated_0(saturate(dot(normal_2, view_0)) + 0.00000999999974738, saturate(dot(normal_2, light_1)), roughness_3)))  * fresnelSchlick_0(f0_1, saturate(dot(view_0, _S59)));
}


#line 75 "src/rendering/shaders/cy/material.slang"
float3 shadeSurfaceWithLight_0(const Surface_0 thread* surface_3, float3 worldNormal_0, float3 viewDirection_0, const LightSample_0 thread* sample_0)
{

#line 76
    float3 _S60 = sample_0->direction_1;

    float _S61 = saturate(dot(worldNormal_0, sample_0->direction_1));
    if(_S61 <= 0.0)
    {
        return float3(0.0) ;
    }

#line 87
    return (diffuseLambert_0(surface_3->albedo_0 * float3((1.0 - surface_3->metallic_0)) ) + specularGgx_0(worldNormal_0, viewDirection_0, _S60, surface_3->roughness_0, computeF0_0(surface_3->albedo_0, surface_3->metallic_0))) * sample_0->illuminance_0 * float3(_S61) ;
}


#line 316 "src/rendering/shaders/cy/frame.slang"
float viewDepthOf_0(float3 relative_1, KernelContext_0 thread* kernelContext_16)
{



    return - dot(kernelContext_16->cyFrameView_0->frame_0->relativeToViewRow2_0, float4(relative_1, 1.0));
}


#line 21 "src/rendering/shaders/cy/cluster.slang"
uint clusterSliceOf_0(const ClusterGrid_0 constant* grid_0, float viewDepth_0)
{

    return uint(clamp(log2(max(viewDepth_0, grid_0->nearPlane_0)) * grid_0->sliceScale_0 + grid_0->sliceBias_0, 0.0, float(grid_0->dimensions_0.z - 1U)));
}


uint3 clusterCoordOf_0(const ClusterGrid_0 constant* grid_1, uint2 pixel_2, uint2 renderExtent_0, float viewDepth_1)
{
    uint2 _S62 = grid_1->dimensions_0.xy;
    uint2 _S63 = min(uint2(float2(pixel_2) / float2(renderExtent_0) * float2(_S62)), _S62 - uint2(1U) );

#line 31
    uint _S64 = clusterSliceOf_0(grid_1, viewDepth_1);

#line 31
    return uint3(_S63, _S64);
}



uint clusterIndexOf_0(const ClusterGrid_0 constant* grid_2, uint3 coord_0)
{
    return coord_0.x + grid_2->dimensions_0.x * (coord_0.y + grid_2->dimensions_0.y * coord_0.z);
}


#line 592 "src/rendering/shaders/cy/frame.slang"
float3 accumulateLights_0(const Surface_0 thread* surface_4, float3 relativePosition_1, float3 normal_3, float3 viewDir_0, float2 fragmentCentre_2, uint instanceFlags_0, KernelContext_0 thread* kernelContext_17)
{

#line 593
    bool _S65;

    uint2 _S66 = uint2(fragmentCentre_2);
    float3 _S67 = float3(0.0) ;
    uint _S68 = kernelContext_17->cyFrameView_0->frame_0->counts_0.x;

#line 597
    uint global_0 = 0U;

#line 597
    float3 lit_2 = _S67;

#line 606
    for(;;)
    {

#line 606
        if(global_0 < _S68)
        {
        }
        else
        {

#line 606
            break;
        }
        if((kernelContext_17->cyFrameView_0->lights_0[global_0].kind_0) != 0U)
        {
            global_0 = global_0 + 1U;

#line 606
            continue;
        }

#line 606
        thread Light_0 _S69 = kernelContext_17->cyFrameView_0->lights_0[global_0];

#line 606
        LightSample_0 _S70 = evaluateLight_0(&_S69, relativePosition_1);

#line 616
        if(global_0 == (kernelContext_17->cyFrameView_0->frame_0->shadowControl_0.y))
        {

#line 616
            _S65 = (instanceFlags_0 & 8U) != 0U;

#line 616
        }
        else
        {

#line 616
            _S65 = false;

#line 616
        }

#line 616
        float _S71;
        if(_S65)
        {

#line 617
            float _S72 = directionalShadowVisibility_0(relativePosition_1, normal_3, fragmentCentre_2, kernelContext_17);

#line 617
            float _S73 = contactShadowVisibility_0(fragmentCentre_2, kernelContext_17);

#line 617
            _S71 = min(_S72, _S73);

#line 617
        }
        else
        {

#line 617
            _S71 = 1.0;

#line 617
        }

#line 617
        thread LightSample_0 _S74 = _S70;

#line 617
        float3 _S75 = shadeSurfaceWithLight_0(surface_4, normal_3, viewDir_0, &_S74);

#line 617
        lit_2 = lit_2 + _S75 * float3(_S71) ;

#line 606
        global_0 = global_0 + 1U;

#line 606
    }

#line 624
    if((kernelContext_17->cyFrameView_0->frame_0->counts_0.w) == 0U)
    {

#line 624
        _S65 = true;

#line 624
    }
    else
    {

#line 624
        _S65 = ((&kernelContext_17->cyFrameView_0->frame_0->clusterGrid_0)->dimensions_0.z) == 0U;

#line 624
    }

#line 624
    uint index_5;

#line 624
    if(_S65)
    {

#line 624
        index_5 = 0U;

        for(;;)
        {

#line 626
            if(index_5 < _S68)
            {
            }
            else
            {

#line 626
                break;
            }
            if((kernelContext_17->cyFrameView_0->lights_0[index_5].kind_0) == 0U)
            {
                index_5 = index_5 + 1U;

#line 626
                continue;
            }

#line 626
            thread Light_0 _S76 = kernelContext_17->cyFrameView_0->lights_0[index_5];

#line 626
            LightSample_0 _S77 = evaluateLight_0(&_S76, relativePosition_1);

#line 626
            thread LightSample_0 _S78 = _S77;

#line 626
            float3 _S79 = shadeSurfaceWithLight_0(surface_4, normal_3, viewDir_0, &_S78);

#line 626
            lit_2 = lit_2 + _S79;

#line 626
            index_5 = index_5 + 1U;

#line 626
        }

#line 635
        return lit_2;
    }

    uint2 _S80 = uint2(kernelContext_17->cyFrameView_0->frame_0->extentAndInverse_0.xy);

#line 638
    float _S81 = viewDepthOf_0(relativePosition_1, kernelContext_17);

#line 638
    uint3 _S82 = clusterCoordOf_0(&kernelContext_17->cyFrameView_0->frame_0->clusterGrid_0, _S66, _S80, _S81);

#line 638
    uint _S83 = clusterIndexOf_0(&kernelContext_17->cyFrameView_0->frame_0->clusterGrid_0, _S82);

#line 643
    uint2 _S84 = kernelContext_17->cyFrameView_0->clusterHeaders_0[_S83 * kernelContext_17->cyFrameView_0->frame_0->counts_0.z];

#line 643
    index_5 = 0U;
    for(;;)
    {

#line 644
        if(index_5 < (_S84.y))
        {
        }
        else
        {

#line 644
            break;
        }
        uint _S85 = kernelContext_17->cyFrameView_0->clusterIndices_0[_S84.x + index_5];
        if(_S85 >= _S68)
        {

#line 647
            _S65 = true;

#line 647
        }
        else
        {

#line 647
            _S65 = (kernelContext_17->cyFrameView_0->lights_0[_S85].kind_0) == 0U;

#line 647
        }

#line 647
        if(_S65)
        {
            index_5 = index_5 + 1U;

#line 644
            continue;
        }

#line 644
        thread Light_0 _S86 = kernelContext_17->cyFrameView_0->lights_0[_S85];

#line 644
        LightSample_0 _S87 = evaluateLight_0(&_S86, relativePosition_1);

#line 644
        thread LightSample_0 _S88 = _S87;

#line 644
        float3 _S89 = shadeSurfaceWithLight_0(surface_4, normal_3, viewDir_0, &_S88);

#line 644
        lit_2 = lit_2 + _S89;

#line 644
        index_5 = index_5 + 1U;

#line 644
    }

#line 654
    return lit_2;
}



float occlusionVisibility_0(float2 fragmentCentre_3, KernelContext_0 thread* kernelContext_18)
{

#line 659
    float4 _S90 = cyMaterialSampleTextureLevel_0(kernelContext_18->cyFrameView_0->frame_0->occlusionControl_0.x, fragmentCentre_3 * kernelContext_18->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_18);


    return _S90.w;
}


#line 662
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 662
struct pixelInput_0
{
    float3 relativePosition_2 [[user(TEXCOORD)]];
    float3 normal_4 [[user(TEXCOORD_1)]];
    float2 uv_9 [[user(TEXCOORD_2)]];
    [[flat]] uint drawIndex_0 [[user(TEXCOORD_3)]];
};


#line 666
[[fragment]] pixelOutput_0 cyForwardFragment(pixelInput_0 _S91 [[stage_in]], float4 position_0 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyFrameGlobalSet_default_0 constant& cyFrameGlobals_1 [[buffer(0)]])
{

#line 666
    thread KernelContext_0 kernelContext_19;

#line 666
    (&kernelContext_19)->cyFrameView_0 = cyFrameView_1;

#line 666
    (&kernelContext_19)->cyFrameGlobals_0 = &cyFrameGlobals_1;

    CyDrawInstance_0 _S92 = cyFrameView_1->drawInstances_0[_S91.drawIndex_0];

#line 668
    Surface_0 _S93 = surfaceOf_0(_S92.material_0, cyFrameView_1->instances_0[_S92.instanceSlot_0].tint_0.xyz, _S91.uv_9, &kernelContext_19);



    float3 _S94 = normalize(_S91.normal_4);


    float3 _S95 = normalize(- _S91.relativePosition_2);


    float2 _S96 = position_0.xy;

#line 678
    thread Surface_0 _S97 = _S93;

#line 678
    float3 _S98 = accumulateLights_0(&_S97, _S91.relativePosition_2, _S94, _S95, _S96, _S92.flags_0, &kernelContext_19);

#line 683
    float3 ambient_0 = _S93.albedo_0 * (&kernelContext_19)->cyFrameView_0->frame_0->ambientAndOcclusion_0.xyz * float3(_S93.occlusion_0) ;

#line 683
    float3 color_1;

#line 683
    float3 ambient_1;
    if(((&kernelContext_19)->cyFrameView_0->frame_0->occlusionControl_0.x) != 4294967295U)
    {

#line 684
        float _S99 = occlusionVisibility_0(_S96, &kernelContext_19);


        float3 ambient_2 = ambient_0 * float3(_S99) ;
        if(((&kernelContext_19)->cyFrameView_0->frame_0->occlusionControl_0.y) != 0U)
        {

#line 688
            color_1 = _S98 * float3(mix(1.0, _S99, (as_type<float>(((&kernelContext_19)->cyFrameView_0->frame_0->occlusionControl_0.z))))) ;

#line 688
        }
        else
        {

#line 688
            color_1 = _S98;

#line 688
        }

#line 688
        ambient_1 = ambient_2;

#line 684
    }
    else
    {

#line 684
        color_1 = _S98;

#line 684
        ambient_1 = ambient_0;

#line 684
    }

#line 684
    pixelOutput_0 _S100 = { float4(color_1 + _S93.emission_0 + ambient_1, _S93.opacity_0) };

#line 695
    return _S100;
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

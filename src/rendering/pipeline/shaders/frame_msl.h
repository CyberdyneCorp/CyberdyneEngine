#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the rendering frame. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::pipeline {

/// DepthVertex.metal, 6369 bytes.
inline constexpr char kFrameDepthVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 205 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 285
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


#line 192 "src/rendering/shaders/cy/frame.slang"
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


#line 214
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


#line 259
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


#line 269 "src/rendering/shaders/cy/frame.slang"
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


#line 294 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 296
struct cyDepthVertex_Result_0
{
    float4 position_0 [[position]];
    float3 normal_1 [[user(TEXCOORD)]];
    float4 currentClip_0 [[user(TEXCOORD_1)]];
    float4 previousClip_0 [[user(TEXCOORD_2)]];
};


#line 296
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
};


#line 412
struct CyDepthVertex_0
{
    float4 position_1;
    float3 normal_2;
    float4 currentClip_1;
    float4 previousClip_1;
};


#line 412
[[vertex]] cyDepthVertex_Result_0 cyDepthVertex(vertexInput_0 _S9 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 412
    thread KernelContext_0 kernelContext_1;

#line 412
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 412
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 424
    CyInstanceTransform_0 _S10 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

#line 424
    thread CyInstanceTransform_0 _S11 = _S10;

#line 424
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    thread CyDepthVertex_0 output_0;

#line 425
    float4 _S13 = transformToClip_0(_S12, &kernelContext_1);

    (&output_0)->position_1 = _S13;
    (&output_0)->currentClip_1 = _S13;
    float4 _S14 = float4(_S12, 1.0);
    (&output_0)->previousClip_1 = float4(dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow0_0, _S14), dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow1_0, _S14), dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow2_0, _S14), dot((&kernelContext_1)->cyFrameView_0->frame_0->previousRelativeToClipRow3_0, _S14));



    float3 _S15 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 434
    thread CyInstanceTransform_0 _S16 = _S10;

#line 434
    float3 _S17 = rotateToRelative_0(&_S16, _S15);

#line 434
    (&output_0)->normal_2 = _S17;

#line 434
    thread cyDepthVertex_Result_0 _S18;

#line 434
    (&_S18)->position_0 = output_0.position_1;

#line 434
    (&_S18)->normal_1 = output_0.normal_2;

#line 434
    (&_S18)->currentClip_0 = output_0.currentClip_1;

#line 434
    (&_S18)->previousClip_0 = output_0.previousClip_1;

#line 434
    return _S18;
}

)cy_msl";

/// DepthFragment.metal, 3789 bytes.
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


#line 438 "src/rendering/shaders/cy/frame.slang"
struct CyDepthOutput_0
{
    float4 normalRoughness_0 [[color(0)]];
    float2 velocity_0 [[color(1)]];
};


#line 438
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


#line 192 "src/rendering/shaders/cy/frame.slang"
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


#line 445
[[fragment]] CyDepthOutput_0 cyDepthFragment(pixelInput_0 _S4 [[stage_in]], float4 position_0 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_0 [[buffer(1)]])
{
    thread CyDepthOutput_0 output_0;
    (&output_0)->normalRoughness_0 = float4(encodeOctahedral_0(normalize(_S4.normal_1)), 1.0, 1.0);

#line 456
    float2 _S5 = _S4.currentClip_0.xy / float2(_S4.currentClip_0.w)  - cyFrameView_0->frame_0->temporalJitter_0.xy * float2(2.0)  * cyFrameView_0->frame_0->extentAndInverse_0.zw;

    float2 _S6 = _S4.previousClip_0.xy / float2(_S4.previousClip_0.w) ;
    (&output_0)->velocity_0 = float2((_S6.x - _S5.x) * 0.5, (_S5.y - _S6.y) * 0.5);

    return output_0;
}

)cy_msl";

/// ShadowVertex.metal, 4378 bytes.
inline constexpr char kFrameShadowVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 205 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 285
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


#line 192 "src/rendering/shaders/cy/frame.slang"
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


#line 214
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


#line 259
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


#line 276 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow3_0, _S2));
}


#line 279
struct cyShadowVertex_Result_0
{
    float4 position_0 [[position]];
};


#line 279
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
};


#line 387
struct CyShadowVertex_0
{
    float4 position_1;
};


#line 387
[[vertex]] cyShadowVertex_Result_0 cyShadowVertex(vertexInput_0 _S3 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 387
    thread KernelContext_0 kernelContext_1;

#line 387
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 387
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 397
    thread CyShadowVertex_0 output_0;

#line 397
    thread CyInstanceTransform_0 _S4 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

#line 397
    float3 _S5 = transformToRelative_0(&_S4, _S3.modelPosition_1);

#line 397
    float4 _S6 = transformToShadowClip_0(_S5, &kernelContext_1);
    (&output_0)->position_1 = _S6;

#line 398
    thread cyShadowVertex_Result_0 _S7;

#line 398
    (&_S7)->position_0 = output_0.position_1;

#line 398
    return _S7;
}

)cy_msl";

/// ShadowFragment.metal, 413 bytes.
inline constexpr char kFrameShadowFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 90 "core"
struct pixelOutput_0
{
    float output_0 [[color(0)]];
};


#line 397 "/Users/leonardo/trabalho/CyberdyneEngine/src/rendering/shaders/cy/frame.slang"
[[fragment]] pixelOutput_0 cyShadowFragment(float4 position_0 [[position]])
{

#line 397
    pixelOutput_0 _S1 = { position_0.z };

    return _S1;
}

)cy_msl";

/// ForwardVertex.metal, 6203 bytes.
inline constexpr char kFrameForwardVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 205 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 285
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


#line 192 "src/rendering/shaders/cy/frame.slang"
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


#line 214
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


#line 259
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


#line 269 "src/rendering/shaders/cy/frame.slang"
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


#line 294 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 296
struct cyForwardVertex_Result_0
{
    float4 position_0 [[position]];
    float3 relativePosition_0 [[user(TEXCOORD)]];
    float3 normal_1 [[user(TEXCOORD_1)]];
    float2 uv_0 [[user(TEXCOORD_2)]];
    uint drawIndex_1 [[user(TEXCOORD_3)]];
};


#line 296
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
    float2 uv_1 [[attribute(2)]];
};


#line 466
struct CyForwardVertex_0
{
    float4 position_1;
    float3 relativePosition_1;
    float3 normal_2;
    float2 uv_2;
    [[flat]] uint drawIndex_2;
};


#line 466
[[vertex]] cyForwardVertex_Result_0 cyForwardVertex(vertexInput_0 _S9 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 466
    thread KernelContext_0 kernelContext_1;

#line 466
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 466
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 480
    CyInstanceTransform_0 _S10 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

    thread CyForwardVertex_0 output_0;

#line 482
    thread CyInstanceTransform_0 _S11 = _S10;

#line 482
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    (&output_0)->relativePosition_1 = _S12;

#line 483
    float4 _S13 = transformToClip_0(_S12, &kernelContext_1);
    (&output_0)->position_1 = _S13;



    float3 _S14 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 488
    thread CyInstanceTransform_0 _S15 = _S10;

#line 488
    float3 _S16 = rotateToRelative_0(&_S15, _S14);

#line 488
    (&output_0)->normal_2 = _S16;
    (&output_0)->uv_2 = _S9.uv_1;
    (&output_0)->drawIndex_2 = cyDraw_1->drawIndex_0;

#line 490
    thread cyForwardVertex_Result_0 _S17;

#line 490
    (&_S17)->position_0 = output_0.position_1;

#line 490
    (&_S17)->relativePosition_0 = output_0.relativePosition_1;

#line 490
    (&_S17)->normal_1 = output_0.normal_2;

#line 490
    (&_S17)->uv_0 = output_0.uv_2;

#line 490
    (&_S17)->drawIndex_1 = output_0.drawIndex_2;

#line 490
    return _S17;
}

)cy_msl";

/// ForwardFragment.metal, 22276 bytes.
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


#line 192 "src/rendering/shaders/cy/frame.slang"
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


#line 313 "src/rendering/shaders/cy/frame.slang"
float3 readMaterialFloat3_0(uint material_1, uint wordOffset_0, KernelContext_0 thread* kernelContext_0)
{
    uint _S1 = material_1 * kernelContext_0->cyFrameView_0->frame_0->counts_0.y + wordOffset_0;
    return float3((as_type<float>((kernelContext_0->cyFrameView_0->materialWords_0[_S1]))), (as_type<float>((kernelContext_0->cyFrameView_0->materialWords_0[_S1 + 1U]))), (as_type<float>((kernelContext_0->cyFrameView_0->materialWords_0[_S1 + 2U]))));
}


#line 308
float readMaterialFloat_0(uint material_2, uint wordOffset_1, KernelContext_0 thread* kernelContext_1)
{
    return (as_type<float>((kernelContext_1->cyFrameView_0->materialWords_0[material_2 * kernelContext_1->cyFrameView_0->frame_0->counts_0.y + wordOffset_1])));
}


#line 323
uint readMaterialUint_0(uint material_3, uint wordOffset_2, KernelContext_0 thread* kernelContext_2)
{
    return kernelContext_2->cyFrameView_0->materialWords_0[material_3 * kernelContext_2->cyFrameView_0->frame_0->counts_0.y + wordOffset_2];
}


#line 333
uint materialTextureSlot_0(uint material_4, uint wordOffset_3, KernelContext_0 thread* kernelContext_3)
{
    if(wordOffset_3 == 4294967295U)
    {
        return 4294967295U;
    }

#line 337
    uint _S2 = readMaterialUint_0(material_4, wordOffset_3, kernelContext_3);

    return _S2;
}


#line 247
float4 cyMaterialSampleTexture_0(uint bindlessIndex_0, float2 uv_0, KernelContext_0 thread* kernelContext_4)
{
    return ((kernelContext_4->cyFrameGlobals_0->textures_0[bindlessIndex_0]).sample((kernelContext_4->cyFrameGlobals_0->sampler_0), (uv_0)));
}


#line 369
Surface_0 surfaceOf_0(uint material_5, float3 tint_1, float2 uv_1, KernelContext_0 thread* kernelContext_5)
{
    thread Surface_0 surface_2 = defaultSurface_0();

#line 371
    float3 _S3 = readMaterialFloat3_0(material_5, kernelContext_5->cyFrameView_0->frame_0->materialOffsets_0.x, kernelContext_5);
    (&surface_2)->albedo_0 = _S3 * tint_1;

#line 372
    float _S4 = readMaterialFloat_0(material_5, kernelContext_5->cyFrameView_0->frame_0->materialOffsets_0.y, kernelContext_5);
    (&surface_2)->roughness_0 = clamp(_S4, 0.01999999955296516, 1.0);

#line 373
    float _S5 = readMaterialFloat_0(material_5, kernelContext_5->cyFrameView_0->frame_0->materialOffsets_0.z, kernelContext_5);
    (&surface_2)->metallic_0 = saturate(_S5);

#line 374
    float3 _S6 = readMaterialFloat3_0(material_5, kernelContext_5->cyFrameView_0->frame_0->materialOffsets_0.w, kernelContext_5);
    (&surface_2)->emission_0 = _S6;

#line 375
    uint _S7 = materialTextureSlot_0(material_5, kernelContext_5->cyFrameView_0->frame_0->materialTextures_0.x, kernelContext_5);


    if(_S7 != 4294967295U)
    {

#line 378
        float4 _S8 = cyMaterialSampleTexture_0(_S7, uv_1, kernelContext_5);

        (&surface_2)->albedo_0 = (&surface_2)->albedo_0 * _S8.xyz;

#line 378
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


#line 276 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_6)
{
    float4 _S15 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_6->cyFrameView_0->frame_0->shadowToClipRow0_0, _S15), dot(kernelContext_6->cyFrameView_0->frame_0->shadowToClipRow1_0, _S15), dot(kernelContext_6->cyFrameView_0->frame_0->shadowToClipRow2_0, _S15), dot(kernelContext_6->cyFrameView_0->frame_0->shadowToClipRow3_0, _S15));
}


#line 252
float4 cyMaterialSampleTextureLevel_0(uint bindlessIndex_1, float2 uv_2, float level_0, KernelContext_0 thread* kernelContext_7)
{
    return ((kernelContext_7->cyFrameGlobals_0->textures_0[bindlessIndex_1]).sample((kernelContext_7->cyFrameGlobals_0->sampler_0), (uv_2), level((level_0))));
}


#line 500
float directionalShadowVisibility_0(float3 relativePosition_0, float3 normal_1, KernelContext_0 thread* kernelContext_8)
{

#line 500
    bool _S16;

    if((kernelContext_8->cyFrameView_0->frame_0->shadowControl_0.w) == 0U)
    {

#line 502
        _S16 = true;

#line 502
    }
    else
    {

#line 502
        _S16 = (kernelContext_8->cyFrameView_0->frame_0->shadowControl_0.x) == 4294967295U;

#line 502
    }

#line 502
    if(_S16)
    {
        return 1.0;
    }

#line 504
    float4 _S17 = transformToShadowClip_0(relativePosition_0 + normal_1 * float3(0.00499999988824129) , kernelContext_8);


    float _S18 = _S17.w;

#line 507
    if(_S18 <= 0.0)
    {
        return 1.0;
    }
    float3 _S19 = _S17.xyz / float3(_S18) ;
    float _S20 = _S19.x * 0.5 + 0.5;

#line 512
    float _S21 = 0.5 - _S19.y * 0.5;

#line 512
    float2 _S22 = float2(_S20, _S21);
    if(_S20 < 0.0)
    {

#line 513
        _S16 = true;

#line 513
    }
    else
    {

#line 513
        _S16 = _S20 > 1.0;

#line 513
    }

#line 513
    if(_S16)
    {

#line 513
        _S16 = true;

#line 513
    }
    else
    {

#line 513
        _S16 = _S21 < 0.0;

#line 513
    }

#line 513
    if(_S16)
    {

#line 513
        _S16 = true;

#line 513
    }
    else
    {

#line 513
        _S16 = _S21 > 1.0;

#line 513
    }

#line 513
    if(_S16)
    {
        return 1.0;
    }
    float _S23 = 1.0 / float(max(kernelContext_8->cyFrameView_0->frame_0->shadowControl_0.z, 1U));
    float _S24 = _S19.z + 0.00050000002374873;

#line 518
    int y_0 = int(-1);

#line 518
    float visible_0 = 0.0;

    for(;;)
    {

#line 520
        if(y_0 <= int(1))
        {
        }
        else
        {

#line 520
            break;
        }

#line 520
        int x_0 = int(-1);

        for(;;)
        {

#line 522
            if(x_0 <= int(1))
            {
            }
            else
            {

#line 522
                break;
            }

#line 522
            float4 _S25 = cyMaterialSampleTextureLevel_0(kernelContext_8->cyFrameView_0->frame_0->shadowControl_0.x, _S22 + float2(float(x_0), float(y_0)) * float2(_S23) , 0.0, kernelContext_8);

#line 522
            float _S26;



            if(_S24 >= (_S25.x))
            {

#line 526
                _S26 = 1.0;

#line 526
            }
            else
            {

#line 526
                _S26 = 0.0;

#line 526
            }

#line 526
            float visible_1 = visible_0 + _S26;

#line 522
            x_0 = x_0 + int(1);

#line 522
            visible_0 = visible_1;

#line 522
        }

#line 520
        y_0 = y_0 + int(1);

#line 520
    }

#line 529
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


#line 300 "src/rendering/shaders/cy/frame.slang"
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


#line 532 "src/rendering/shaders/cy/frame.slang"
float3 accumulateLights_0(const Surface_0 thread* surface_4, float3 relativePosition_1, float3 normal_3, float3 viewDir_0, uint2 pixel_1, uint instanceFlags_0, KernelContext_0 thread* kernelContext_10)
{

#line 533
    bool _S39;

    float3 _S40 = float3(0.0) ;
    uint _S41 = kernelContext_10->cyFrameView_0->frame_0->counts_0.x;

#line 536
    uint global_0 = 0U;

#line 536
    float3 lit_0 = _S40;

#line 545
    for(;;)
    {

#line 545
        if(global_0 < _S41)
        {
        }
        else
        {

#line 545
            break;
        }
        if((kernelContext_10->cyFrameView_0->lights_0[global_0].kind_0) != 0U)
        {
            global_0 = global_0 + 1U;

#line 545
            continue;
        }

#line 545
        thread Light_0 _S42 = kernelContext_10->cyFrameView_0->lights_0[global_0];

#line 545
        LightSample_0 _S43 = evaluateLight_0(&_S42, relativePosition_1);

#line 552
        if(global_0 == (kernelContext_10->cyFrameView_0->frame_0->shadowControl_0.y))
        {

#line 552
            _S39 = (instanceFlags_0 & 8U) != 0U;

#line 552
        }
        else
        {

#line 552
            _S39 = false;

#line 552
        }

#line 552
        float _S44;
        if(_S39)
        {

#line 553
            float _S45 = directionalShadowVisibility_0(relativePosition_1, normal_3, kernelContext_10);

#line 553
            _S44 = _S45;

#line 553
        }
        else
        {

#line 553
            _S44 = 1.0;

#line 553
        }

#line 553
        thread LightSample_0 _S46 = _S43;

#line 553
        float3 _S47 = shadeSurfaceWithLight_0(surface_4, normal_3, viewDir_0, &_S46);

#line 553
        lit_0 = lit_0 + _S47 * float3(_S44) ;

#line 545
        global_0 = global_0 + 1U;

#line 545
    }

#line 557
    if((kernelContext_10->cyFrameView_0->frame_0->counts_0.w) == 0U)
    {

#line 557
        _S39 = true;

#line 557
    }
    else
    {

#line 557
        _S39 = ((&kernelContext_10->cyFrameView_0->frame_0->clusterGrid_0)->dimensions_0.z) == 0U;

#line 557
    }

#line 557
    uint index_0;

#line 557
    if(_S39)
    {

#line 557
        index_0 = 0U;

        for(;;)
        {

#line 559
            if(index_0 < _S41)
            {
            }
            else
            {

#line 559
                break;
            }
            if((kernelContext_10->cyFrameView_0->lights_0[index_0].kind_0) == 0U)
            {
                index_0 = index_0 + 1U;

#line 559
                continue;
            }

#line 559
            thread Light_0 _S48 = kernelContext_10->cyFrameView_0->lights_0[index_0];

#line 559
            LightSample_0 _S49 = evaluateLight_0(&_S48, relativePosition_1);

#line 559
            thread LightSample_0 _S50 = _S49;

#line 559
            float3 _S51 = shadeSurfaceWithLight_0(surface_4, normal_3, viewDir_0, &_S50);

#line 559
            lit_0 = lit_0 + _S51;

#line 559
            index_0 = index_0 + 1U;

#line 559
        }

#line 568
        return lit_0;
    }

    uint2 _S52 = uint2(kernelContext_10->cyFrameView_0->frame_0->extentAndInverse_0.xy);

#line 571
    float _S53 = viewDepthOf_0(relativePosition_1, kernelContext_10);

#line 571
    uint3 _S54 = clusterCoordOf_0(&kernelContext_10->cyFrameView_0->frame_0->clusterGrid_0, pixel_1, _S52, _S53);

#line 571
    uint _S55 = clusterIndexOf_0(&kernelContext_10->cyFrameView_0->frame_0->clusterGrid_0, _S54);

#line 576
    uint2 _S56 = kernelContext_10->cyFrameView_0->clusterHeaders_0[_S55 * kernelContext_10->cyFrameView_0->frame_0->counts_0.z];

#line 576
    index_0 = 0U;
    for(;;)
    {

#line 577
        if(index_0 < (_S56.y))
        {
        }
        else
        {

#line 577
            break;
        }
        uint _S57 = kernelContext_10->cyFrameView_0->clusterIndices_0[_S56.x + index_0];
        if(_S57 >= _S41)
        {

#line 580
            _S39 = true;

#line 580
        }
        else
        {

#line 580
            _S39 = (kernelContext_10->cyFrameView_0->lights_0[_S57].kind_0) == 0U;

#line 580
        }

#line 580
        if(_S39)
        {
            index_0 = index_0 + 1U;

#line 577
            continue;
        }

#line 577
        thread Light_0 _S58 = kernelContext_10->cyFrameView_0->lights_0[_S57];

#line 577
        LightSample_0 _S59 = evaluateLight_0(&_S58, relativePosition_1);

#line 577
        thread LightSample_0 _S60 = _S59;

#line 577
        float3 _S61 = shadeSurfaceWithLight_0(surface_4, normal_3, viewDir_0, &_S60);

#line 577
        lit_0 = lit_0 + _S61;

#line 577
        index_0 = index_0 + 1U;

#line 577
    }

#line 587
    return lit_0;
}



float occlusionVisibility_0(float2 fragmentCentre_0, KernelContext_0 thread* kernelContext_11)
{

#line 592
    float4 _S62 = cyMaterialSampleTextureLevel_0(kernelContext_11->cyFrameView_0->frame_0->occlusionControl_0.x, fragmentCentre_0 * kernelContext_11->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_11);


    return _S62.w;
}


#line 595
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 595
struct pixelInput_0
{
    float3 relativePosition_2 [[user(TEXCOORD)]];
    float3 normal_4 [[user(TEXCOORD_1)]];
    float2 uv_3 [[user(TEXCOORD_2)]];
    [[flat]] uint drawIndex_0 [[user(TEXCOORD_3)]];
};


#line 599
[[fragment]] pixelOutput_0 cyForwardFragment(pixelInput_0 _S63 [[stage_in]], float4 position_0 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyFrameGlobalSet_default_0 constant& cyFrameGlobals_1 [[buffer(0)]])
{

#line 599
    thread KernelContext_0 kernelContext_12;

#line 599
    (&kernelContext_12)->cyFrameView_0 = cyFrameView_1;

#line 599
    (&kernelContext_12)->cyFrameGlobals_0 = &cyFrameGlobals_1;

    float2 _S64 = position_0.xy;

#line 601
    uint2 _S65 = uint2(_S64);
    CyDrawInstance_0 _S66 = cyFrameView_1->drawInstances_0[_S63.drawIndex_0];

#line 602
    Surface_0 _S67 = surfaceOf_0(_S66.material_0, cyFrameView_1->instances_0[_S66.instanceSlot_0].tint_0.xyz, _S63.uv_3, &kernelContext_12);



    float3 _S68 = normalize(_S63.normal_4);


    float3 _S69 = normalize(- _S63.relativePosition_2);

#line 609
    thread Surface_0 _S70 = _S67;

#line 609
    float3 _S71 = accumulateLights_0(&_S70, _S63.relativePosition_2, _S68, _S69, _S65, _S66.flags_0, &kernelContext_12);

#line 617
    float3 ambient_0 = _S67.albedo_0 * (&kernelContext_12)->cyFrameView_0->frame_0->ambientAndOcclusion_0.xyz * float3(_S67.occlusion_0) ;

#line 617
    float3 color_1;

#line 617
    float3 ambient_1;
    if(((&kernelContext_12)->cyFrameView_0->frame_0->occlusionControl_0.x) != 4294967295U)
    {

#line 618
        float _S72 = occlusionVisibility_0(_S64, &kernelContext_12);


        float3 ambient_2 = ambient_0 * float3(_S72) ;
        if(((&kernelContext_12)->cyFrameView_0->frame_0->occlusionControl_0.y) != 0U)
        {

#line 622
            color_1 = _S71 * float3(mix(1.0, _S72, (as_type<float>(((&kernelContext_12)->cyFrameView_0->frame_0->occlusionControl_0.z))))) ;

#line 622
        }
        else
        {

#line 622
            color_1 = _S71;

#line 622
        }

#line 622
        ambient_1 = ambient_2;

#line 618
    }
    else
    {

#line 618
        color_1 = _S71;

#line 618
        ambient_1 = ambient_0;

#line 618
    }

#line 618
    pixelOutput_0 _S73 = { float4(color_1 + _S67.emission_0 + ambient_1, _S67.opacity_0) };

#line 629
    return _S73;
}

)cy_msl";

/// ResolveVertex.metal, 1018 bytes.
inline constexpr char kFrameResolveVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 49 "/Users/leonardo/trabalho/CyberdyneEngine/src/rendering/shaders/cy/fullscreen.slang"
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

#line 66 "/Users/leonardo/trabalho/CyberdyneEngine/src/rendering/shaders/cy/fullscreen.slang"
    thread FullscreenVertex_0 output_0;
    float2 _S1 = float2(float((vertexId_0 << 1U) & 2U), float(vertexId_0 & 2U));

#line 67
    (&output_0)->uv_1 = _S1;


    (&output_0)->position_1 = float4(_S1 * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);

#line 70
    thread fullscreenVertex_Result_0 _S2;

#line 70
    (&_S2)->position_0 = output_0.position_1;

#line 70
    (&_S2)->uv_0 = output_0.uv_1;

#line 70
    return _S2;
}

)cy_msl";

/// ResolveFragment.metal, 3870 bytes.
inline constexpr char kFrameResolveFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 28 "/Users/leonardo/trabalho/CyberdyneEngine/src/rendering/shaders/cy/tonemap.slang"
float3 applyExposure_0(float3 linear_0, float exposureStops_0)
{
    return linear_0 * float3(exp2(exposureStops_0)) ;
}


#line 90 "/Users/leonardo/trabalho/CyberdyneEngine/src/rendering/shaders/cy/fullscreen.slang"
constant int fc_kTonemapOperator_0 [[function_constant(0)]];
constant int kTonemapOperator_0 = is_function_constant_defined(fc_kTonemapOperator_0) ? fc_kTonemapOperator_0 : int(1);

#line 8 "/Users/leonardo/trabalho/CyberdyneEngine/src/rendering/shaders/cy/tonemap.slang"
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


#line 75 "/Users/leonardo/trabalho/CyberdyneEngine/src/rendering/shaders/cy/fullscreen.slang"
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


#line 34 "/Users/leonardo/trabalho/CyberdyneEngine/src/rendering/shaders/cy/fullscreen.slang"
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


#line 97
[[fragment]] pixelOutput_0 fullscreenResolve(pixelInput_0 _S2 [[stage_in]], float4 position_0 [[position]], FullscreenPassSet_default_0 constant* passSet_1 [[buffer(2)]], FullscreenGlobalSet_default_0 constant* globalSet_1 [[buffer(0)]], FullscreenViewSet_default_0 constant* viewSet_1 [[buffer(1)]])
{

#line 97
    thread KernelContext_0 kernelContext_0;

#line 97
    (&kernelContext_0)->passSet_0 = passSet_1;

#line 97
    (&kernelContext_0)->globalSet_0 = globalSet_1;

#line 97
    (&kernelContext_0)->viewSet_0 = viewSet_1;


    float3 _S3 = applyExposure_0(((passSet_1->sceneColor_0).sample((passSet_1->linearClamp_0), (_S2.uv_0))).xyz, globalSet_1->data_0->exposureStops_1);

#line 100
    float3 mapped_0;


    if(kTonemapOperator_0 == int(1))
    {

#line 103
        mapped_0 = tonemapReinhard_0(_S3, 4.0);

#line 103
    }
    else
    {

        if(kTonemapOperator_0 == int(2))
        {

#line 107
            mapped_0 = tonemapAcesApproximate_0(_S3);

#line 107
        }
        else
        {

#line 107
            mapped_0 = _S3;

#line 107
        }

#line 103
    }

#line 114
    if(((&kernelContext_0)->viewSet_0->frame_0->rows_0[int(13)].x) < 0.0)
    {

#line 114
        mapped_0 = - mapped_0;

#line 114
    }

#line 114
    pixelOutput_0 _S4 = { float4(mapped_0, 1.0) };

    return _S4;
}

)cy_msl";

/// TemporalFragment.metal, 3898 bytes.
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


#line 34 "/Users/leonardo/trabalho/CyberdyneEngine/src/rendering/shaders/cy/fullscreen.slang"
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


#line 124 "/Users/leonardo/trabalho/CyberdyneEngine/src/rendering/shaders/cy/fullscreen.slang"
[[fragment]] pixelOutput_0 temporalResolve(pixelInput_0 _S1 [[stage_in]], float4 position_0 [[position]], FullscreenViewSet_default_0 constant* viewSet_1 [[buffer(1)]], FullscreenPassSet_default_0 constant* passSet_1 [[buffer(2)]])
{

#line 124
    thread KernelContext_0 kernelContext_0;

#line 124
    (&kernelContext_0)->viewSet_0 = viewSet_1;

#line 124
    (&kernelContext_0)->passSet_0 = passSet_1;

    float2 _S2 = viewSet_1->frame_0->rows_0[int(13)].zw;
    float4 _S3 = viewSet_1->frame_0->temporalFeedback_0;
    float4 _S4 = ((passSet_1->sceneColor_0).sample((passSet_1->linearClamp_0), (_S1.uv_0), level((0.0))));

    float3 _S5 = _S4.xyz;

#line 130
    float3 minimum_0 = _S5;

#line 130
    float3 maximum_0 = _S5;

#line 130
    int y_0 = int(-1);

    for(;;)
    {

#line 132
        if(y_0 <= int(1))
        {
        }
        else
        {

#line 132
            break;
        }

#line 132
        int x_0 = int(-1);

        for(;;)
        {

#line 134
            if(x_0 <= int(1))
            {
            }
            else
            {

#line 134
                break;
            }

            float3 _S6 = ((passSet_1->sceneColor_0).sample((passSet_1->linearClamp_0), (_S1.uv_0 + float2(float(x_0), float(y_0)) * _S2), level((0.0)))).xyz;
            float3 _S7 = min(minimum_0, _S6);
            float3 _S8 = max(maximum_0, _S6);

#line 134
            int x_1 = x_0 + int(1);

#line 134
            minimum_0 = _S7;

#line 134
            maximum_0 = _S8;

#line 134
            x_0 = x_1;

#line 134
        }

#line 132
        y_0 = y_0 + int(1);

#line 132
    }

#line 153
    float _S9 = (((&kernelContext_0)->passSet_0->depth_0).sample((passSet_1->linearClamp_0), (_S1.uv_0), level((0.0))).x);
    float2 _S10 = _S1.uv_0 + (((&kernelContext_0)->passSet_0->velocity_0).sample((passSet_1->linearClamp_0), (_S1.uv_0), level((0.0))).xy);

#line 154
    bool _S11;
    if(all(_S10 >= (float2(0.0) )))
    {

#line 155
        _S11 = all(_S10 <= (float2(1.0) ));

#line 155
    }
    else
    {

#line 155
        _S11 = false;

#line 155
    }

    float3 _S12 = clamp((((&kernelContext_0)->passSet_0->historyColor_0).sample((passSet_1->linearClamp_0), (_S10), level((0.0)))).xyz, minimum_0, maximum_0);
    if((_S3.x) > 0.5)
    {
    }
    else
    {

#line 158
        _S11 = false;

#line 158
    }

#line 158
    if(_S11)
    {

#line 158
        _S11 = _S9 > 0.0;

#line 158
    }
    else
    {

#line 158
        _S11 = false;

#line 158
    }

#line 158
    float _S13;

#line 158
    if(_S11)
    {

#line 158
        _S13 = _S3.y;

#line 158
    }
    else
    {

#line 158
        _S13 = 0.0;

#line 158
    }

#line 158
    pixelOutput_0 _S14 = { float4(mix(_S5, _S12, float3(_S13) ), _S4.w) };
    return _S14;
}

)cy_msl";

}  // namespace cy::rendering::pipeline

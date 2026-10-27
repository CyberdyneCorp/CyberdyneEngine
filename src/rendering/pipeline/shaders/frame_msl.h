#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the rendering frame. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

#include <cstddef>
#include <initializer_list>

namespace cy::rendering::pipeline {

namespace detail {

/// One MSL module assembled from several raw-string chunks. See embed_msl.py for why.
template <std::size_t Size>
struct MslText {
    char text[Size];
};

template <std::size_t... Sizes>
consteval MslText<(Sizes + ...) - sizeof...(Sizes) + 1> join_msl(const char (&... chunks)[Sizes]) {
    MslText<(Sizes + ...) - sizeof...(Sizes) + 1> joined{};
    std::size_t at = 0;
    for (const char* chunk : {static_cast<const char*>(chunks)...}) {
        for (; *chunk != '\0'; ++chunk) {
            joined.text[at++] = *chunk;
        }
    }
    return joined;
}

}  // namespace detail

/// DepthVertex.metal, 8137 bytes.
inline constexpr char kFrameDepthVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 363 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 443
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


#line 115 "src/rendering/shaders/cy/frame.slang"
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
    uint4 volumetricFogControl_0;
    uint4 motionControl_0;
    uint4 lightmapControl_0;
    uint4 lightmapLayout_0;
    uint4 lightmapShadowLights_0;
    uint4 lightmapDirectLights_0;
    uint4 lightmapDebug_0;
    uint4 giControl_0;
    uint4 lightingControl_0;
    uint4 directionalLights_0;
    float4 viewmodelToClipRow0_0;
    float4 viewmodelToClipRow1_0;
    float4 viewmodelToClipRow2_0;
    float4 viewmodelToClipRow3_0;
    float4 previousViewmodelToClipRow0_0;
    float4 previousViewmodelToClipRow1_0;
    float4 previousViewmodelToClipRow2_0;
    float4 previousViewmodelToClipRow3_0;
    uint4 viewmodelControl_0;
};


#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 350 "src/rendering/shaders/cy/frame.slang"
struct CyDrawInstance_0
{
    uint instanceSlot_0;
    uint material_0;
    uint parameterOffset_0;
    uint giAddress_0;
    uint layerMask_1;
    uint lodAndFade_0;
    uint surface_0;
    uint flags_0;
};


#line 372
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


#line 417
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


#line 427 "src/rendering/shaders/cy/frame.slang"
float4 transformToClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow3_0, _S2));
}


#line 587
float3 previousRelativeOf_0(uint slot_0, const CyInstanceTransform_0 thread* current_0, float3 previousModelPosition_0, KernelContext_0 thread* kernelContext_1)
{
    uint _S3 = kernelContext_1->cyFrameView_0->frame_0->motionControl_0.x;

#line 589
    CyInstanceTransform_0 _S4;
    if(_S3 == 4294967295U)
    {

#line 590
        _S4 = *current_0;

#line 590
    }
    else
    {

#line 590
        _S4 = kernelContext_1->cyFrameView_0->instances_0[_S3 + slot_0];

#line 590
    }

#line 590
    thread CyInstanceTransform_0 _S5 = _S4;

#line 590
    float3 _S6 = transformToRelative_0(&_S5, previousModelPosition_0);
    return _S6;
}


#line 23 "src/rendering/shaders/cy/packing.slang"
float3 decodeOctahedral_0(float2 encoded_0)
{
    float2 _S7 = encoded_0 * float2(2.0)  - float2(1.0) ;
    float _S8 = _S7.x;

#line 26
    float _S9 = _S7.y;

#line 26
    float _S10 = 1.0 - abs(_S8) - abs(_S9);

#line 26
    thread float3 normal_0 = float3(_S8, _S9, _S10);
    float _S11 = saturate(- _S10);
    float2 _S12 = float2(_S8, _S9);

#line 28
    normal_0.xy = _S12 + select(float2(_S11) , float2(- _S11) , _S12 >= (float2(0.0) ));
    return normalize(normal_0);
}


#line 452 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 454
struct cyDepthVertex_Result_0
{
    float4 position_0 [[position]];
    float3 normal_1 [[user(TEXCOORD)]];
    float4 currentClip_0 [[user(TEXCOORD_1)]];
    float4 previousClip_0 [[user(TEXCOORD_2)]];
};


#line 454
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
    float3 previousModelPosition_1 [[attribute(2)]];
};


#line 570
struct CyDepthVertex_0
{
    float4 position_1;
    float3 normal_2;
    float4 currentClip_1;
    float4 previousClip_1;
};


#line 570
[[vertex]] cyDepthVertex_Result_0 cyDepthVertex(vertexInput_0 _S13 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 570
    thread KernelContext_0 kernelContext_2;

#line 570
    (&kernelContext_2)->cyFrameView_0 = cyFrameView_1;

#line 570
    (&kernelContext_2)->cyDraw_0 = cyDraw_1;

#line 598
    CyDrawInstance_0 _S14 = cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0];
    CyInstanceTransform_0 _S15 = cyFrameView_1->instances_0[_S14.instanceSlot_0];

#line 599
    thread CyInstanceTransform_0 _S16 = _S15;

#line 599
    float3 _S17 = transformToRelative_0(&_S16, _S13.modelPosition_1);
    thread CyDepthVertex_0 output_0;

#line 600
    float4 _S18 = transformToClip_0(_S17, &kernelContext_2);

    (&output_0)->position_1 = _S18;
    (&output_0)->currentClip_1 = _S18;

#line 603
    thread CyInstanceTransform_0 _S19 = _S15;

#line 603
    float3 _S20 = previousRelativeOf_0(_S14.instanceSlot_0, &_S19, _S13.previousModelPosition_1, &kernelContext_2);
    float4 _S21 = float4(_S20, 1.0);
    (&output_0)->previousClip_1 = float4(dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow0_0, _S21), dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow1_0, _S21), dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow2_0, _S21), dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow3_0, _S21));



    float3 _S22 = decodeOctahedral_0(_S13.packedNormal_0.xy);

#line 609
    thread CyInstanceTransform_0 _S23 = _S15;

#line 609
    float3 _S24 = rotateToRelative_0(&_S23, _S22);

#line 609
    (&output_0)->normal_2 = _S24;

#line 609
    thread cyDepthVertex_Result_0 _S25;

#line 609
    (&_S25)->position_0 = output_0.position_1;

#line 609
    (&_S25)->normal_1 = output_0.normal_2;

#line 609
    (&_S25)->currentClip_0 = output_0.currentClip_1;

#line 609
    (&_S25)->previousClip_0 = output_0.previousClip_1;

#line 609
    return _S25;
}

)cy_msl";

/// DepthFragment.metal, 4624 bytes.
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


#line 613 "src/rendering/shaders/cy/frame.slang"
struct CyDepthOutput_0
{
    float4 normalRoughness_0 [[color(0)]];
    float2 velocity_0 [[color(1)]];
};


#line 613
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


#line 115 "src/rendering/shaders/cy/frame.slang"
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
    uint4 volumetricFogControl_0;
    uint4 motionControl_0;
    uint4 lightmapControl_0;
    uint4 lightmapLayout_0;
    uint4 lightmapShadowLights_0;
    uint4 lightmapDirectLights_0;
    uint4 lightmapDebug_0;
    uint4 giControl_0;
    uint4 lightingControl_0;
    uint4 directionalLights_0;
    float4 viewmodelToClipRow0_0;
    float4 viewmodelToClipRow1_0;
    float4 viewmodelToClipRow2_0;
    float4 viewmodelToClipRow3_0;
    float4 previousViewmodelToClipRow0_0;
    float4 previousViewmodelToClipRow1_0;
    float4 previousViewmodelToClipRow2_0;
    float4 previousViewmodelToClipRow3_0;
    uint4 viewmodelControl_0;
};


#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 350 "src/rendering/shaders/cy/frame.slang"
struct CyDrawInstance_0
{
    uint instanceSlot_0;
    uint material_0;
    uint parameterOffset_0;
    uint giAddress_0;
    uint layerMask_1;
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


#line 620
[[fragment]] CyDepthOutput_0 cyDepthFragment(pixelInput_0 _S4 [[stage_in]], float4 position_0 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_0 [[buffer(1)]])
{
    thread CyDepthOutput_0 output_0;
    (&output_0)->normalRoughness_0 = float4(encodeOctahedral_0(normalize(_S4.normal_1)), 1.0, 1.0);

#line 631
    float2 _S5 = _S4.currentClip_0.xy / float2(_S4.currentClip_0.w)  - cyFrameView_0->frame_0->temporalJitter_0.xy * float2(2.0)  * cyFrameView_0->frame_0->extentAndInverse_0.zw;

    float2 _S6 = _S4.previousClip_0.xy / float2(_S4.previousClip_0.w) ;
    (&output_0)->velocity_0 = float2((_S6.x - _S5.x) * 0.5, (_S5.y - _S6.y) * 0.5);

    return output_0;
}

)cy_msl";

/// ShadowVertex.metal, 5213 bytes.
inline constexpr char kFrameShadowVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 363 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 443
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


#line 115 "src/rendering/shaders/cy/frame.slang"
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
    uint4 volumetricFogControl_0;
    uint4 motionControl_0;
    uint4 lightmapControl_0;
    uint4 lightmapLayout_0;
    uint4 lightmapShadowLights_0;
    uint4 lightmapDirectLights_0;
    uint4 lightmapDebug_0;
    uint4 giControl_0;
    uint4 lightingControl_0;
    uint4 directionalLights_0;
    float4 viewmodelToClipRow0_0;
    float4 viewmodelToClipRow1_0;
    float4 viewmodelToClipRow2_0;
    float4 viewmodelToClipRow3_0;
    float4 previousViewmodelToClipRow0_0;
    float4 previousViewmodelToClipRow1_0;
    float4 previousViewmodelToClipRow2_0;
    float4 previousViewmodelToClipRow3_0;
    uint4 viewmodelControl_0;
};


#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 350 "src/rendering/shaders/cy/frame.slang"
struct CyDrawInstance_0
{
    uint instanceSlot_0;
    uint material_0;
    uint parameterOffset_0;
    uint giAddress_0;
    uint layerMask_1;
    uint lodAndFade_0;
    uint surface_0;
    uint flags_0;
};


#line 372
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


#line 417
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


#line 434 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow3_0, _S2));
}


#line 437
struct cyShadowVertex_Result_0
{
    float4 position_0 [[position]];
};


#line 437
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
};


#line 545
struct CyShadowVertex_0
{
    float4 position_1;
};


#line 545
[[vertex]] cyShadowVertex_Result_0 cyShadowVertex(vertexInput_0 _S3 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 545
    thread KernelContext_0 kernelContext_1;

#line 545
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 545
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 555
    thread CyShadowVertex_0 output_0;

#line 555
    thread CyInstanceTransform_0 _S4 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

#line 555
    float3 _S5 = transformToRelative_0(&_S4, _S3.modelPosition_1);

#line 555
    float4 _S6 = transformToShadowClip_0(_S5, &kernelContext_1);
    (&output_0)->position_1 = _S6;

#line 556
    thread cyShadowVertex_Result_0 _S7;

#line 556
    (&_S7)->position_0 = output_0.position_1;

#line 556
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


#line 561 "src/rendering/shaders/cy/frame.slang"
[[fragment]] pixelOutput_0 cyShadowFragment(float4 position_0 [[position]])
{

#line 561
    pixelOutput_0 _S1 = { position_0.z };

    return _S1;
}

)cy_msl";

/// ForwardVertex.metal, 7263 bytes.
inline constexpr char kFrameForwardVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 363 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 443
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


#line 115 "src/rendering/shaders/cy/frame.slang"
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
    uint4 volumetricFogControl_0;
    uint4 motionControl_0;
    uint4 lightmapControl_0;
    uint4 lightmapLayout_0;
    uint4 lightmapShadowLights_0;
    uint4 lightmapDirectLights_0;
    uint4 lightmapDebug_0;
    uint4 giControl_0;
    uint4 lightingControl_0;
    uint4 directionalLights_0;
    float4 viewmodelToClipRow0_0;
    float4 viewmodelToClipRow1_0;
    float4 viewmodelToClipRow2_0;
    float4 viewmodelToClipRow3_0;
    float4 previousViewmodelToClipRow0_0;
    float4 previousViewmodelToClipRow1_0;
    float4 previousViewmodelToClipRow2_0;
    float4 previousViewmodelToClipRow3_0;
    uint4 viewmodelControl_0;
};


#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 350 "src/rendering/shaders/cy/frame.slang"
struct CyDrawInstance_0
{
    uint instanceSlot_0;
    uint material_0;
    uint parameterOffset_0;
    uint giAddress_0;
    uint layerMask_1;
    uint lodAndFade_0;
    uint surface_0;
    uint flags_0;
};


#line 372
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


#line 417
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


#line 427 "src/rendering/shaders/cy/frame.slang"
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


#line 452 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 454
struct cyForwardVertex_Result_0
{
    float4 position_0 [[position]];
    float3 relativePosition_0 [[user(TEXCOORD)]];
    float3 normal_1 [[user(TEXCOORD_1)]];
    float2 uv_0 [[user(TEXCOORD_2)]];
    uint drawIndex_1 [[user(TEXCOORD_3)]];
    float2 lightmapUv_0 [[user(TEXCOORD_4)]];
};


#line 454
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
    float2 uv_1 [[attribute(2)]];
    float2 lightmapUv_1 [[attribute(3)]];
};


#line 641
struct CyForwardVertex_0
{
    float4 position_1;
    float3 relativePosition_1;
    float3 normal_2;
    float2 uv_2;
    [[flat]] uint drawIndex_2;
    float2 lightmapUv_2;
};


#line 641
[[vertex]] cyForwardVertex_Result_0 cyForwardVertex(vertexInput_0 _S9 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 641
    thread KernelContext_0 kernelContext_1;

#line 641
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 641
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 662
    CyInstanceTransform_0 _S10 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

    thread CyForwardVertex_0 output_0;

#line 664
    thread CyInstanceTransform_0 _S11 = _S10;

#line 664
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    (&output_0)->relativePosition_1 = _S12;

#line 665
    float4 _S13 = transformToClip_0(_S12, &kernelContext_1);
    (&output_0)->position_1 = _S13;



    float3 _S14 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 670
    thread CyInstanceTransform_0 _S15 = _S10;

#line 670
    float3 _S16 = rotateToRelative_0(&_S15, _S14);

#line 670
    (&output_0)->normal_2 = _S16;
    (&output_0)->uv_2 = _S9.uv_1;
    (&output_0)->drawIndex_2 = cyDraw_1->drawIndex_0;
    (&output_0)->lightmapUv_2 = _S9.lightmapUv_1;

#line 673
    thread cyForwardVertex_Result_0 _S17;

#line 673
    (&_S17)->position_0 = output_0.position_1;

#line 673
    (&_S17)->relativePosition_0 = output_0.relativePosition_1;

#line 673
    (&_S17)->normal_1 = output_0.normal_2;

#line 673
    (&_S17)->uv_0 = output_0.uv_2;

#line 673
    (&_S17)->drawIndex_1 = output_0.drawIndex_2;

#line 673
    (&_S17)->lightmapUv_0 = output_0.lightmapUv_2;

#line 673
    return _S17;
}

)cy_msl";

/// ForwardFragment.metal, 107232 bytes.
inline constexpr auto kFrameForwardFragmentMslText = detail::join_msl(
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 1342 "src/rendering/shaders/cy/frame.slang"
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


#line 115 "src/rendering/shaders/cy/frame.slang"
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
    uint4 volumetricFogControl_0;
    uint4 motionControl_0;
    uint4 lightmapControl_0;
    uint4 lightmapLayout_0;
    uint4 lightmapShadowLights_0;
    uint4 lightmapDirectLights_0;
    uint4 lightmapDebug_0;
    uint4 giControl_0;
    uint4 lightingControl_0;
    uint4 directionalLights_0;
    float4 viewmodelToClipRow0_0;
    float4 viewmodelToClipRow1_0;
    float4 viewmodelToClipRow2_0;
    float4 viewmodelToClipRow3_0;
    float4 previousViewmodelToClipRow0_0;
    float4 previousViewmodelToClipRow1_0;
    float4 previousViewmodelToClipRow2_0;
    float4 previousViewmodelToClipRow3_0;
    uint4 viewmodelControl_0;
};


#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 350 "src/rendering/shaders/cy/frame.slang"
struct CyDrawInstance_0
{
    uint instanceSlot_0;
    uint material_0;
    uint parameterOffset_0;
    uint giAddress_0;
    uint layerMask_1;
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


#line 1345 "src/rendering/shaders/cy/frame.slang"
float4 CyFrameFogVolume_texel_0(const CyFrameFogVolume_0 thread* this_0, int2 coordinate_0, KernelContext_0 thread* kernelContext_0)
{

    int3 _S1 = int3(coordinate_0, int(0));

#line 1348
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


#line 1345 "src/rendering/shaders/cy/frame.slang"
float4 CyFrameFogVolume_texel_1(const CyFrameFogVolume_0 thread* this_1, int2 coordinate_1, KernelContext_0 thread* kernelContext_2)
{

    int3 _S16 = int3(coordinate_1, int(0));

#line 1348
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


#line 679 "src/rendering/shaders/cy/frame.slang"
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


#line 410 "src/rendering/shaders/cy/frame.slang"
float4 cyMaterialSampleTextureLevel_0(uint bindlessIndex_0, float2 uv_0, float level_0, KernelContext_0 thread* kernelContext_4)
{
    return ((kernelContext_4->cyFrameGlobals_0->textures_0[bindlessIndex_0]).sample((kernelContext_4->cyFrameGlobals_0->sampler_0), (uv_0), level((level_0))));
}


#line 682
float CyFrameShadowMap_storedDepth_0(const CyFrameShadowMap_0 thread* this_2, float2 uv_1, KernelContext_0 thread* kernelContext_5)
{

#line 682
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


#line 682 "src/rendering/shaders/cy/frame.slang"
float CyFrameShadowMap_storedDepth_1(const CyFrameShadowMap_0 thread* this_3, float2 uv_3, KernelContext_0 thread* kernelContext_7)
{

#line 682
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


#line 128 "src/rendering/shaders/cy/decal.slang"
float2 cyDecalWordUv_0(uint index_5, uint rows_0)
{

    return (float2(float(index_5 % 1024U), float(index_5 / 1024U)) + float2(0.5) ) / float2(1024.0, float(max(rows_0, 1U)));
}


#line 121
uint cyDecalWordFromTexel_0(float4 texel_2)
{
    uint4 _S51 = uint4(round(saturate(texel_2) * float4(255.0) ));
    return (((_S51.x) | ((_S51.y) << 8U)) | ((_S51.z) << 16U)) | ((_S51.w) << 24U);
}


#line 1256 "src/rendering/shaders/cy/frame.slang"
uint CyFrameDecalSource_word_0(uint index_6, KernelContext_0 thread* kernelContext_9)
{

#line 1256
    float4 _S52 = cyMaterialSampleTextureLevel_0(kernelContext_9->cyFrameView_0->frame_0->decalControl_0.x, cyDecalWordUv_0(index_6, kernelContext_9->cyFrameView_0->frame_0->decalControl_0.y), 0.0, kernelContext_9);


    return cyDecalWordFromTexel_0(_S52);
}


#line 113 "src/rendering/shaders/cy/decal.slang"
float3 wordFloat3_0(uint index_7, KernelContext_0 thread* kernelContext_10)
{

#line 113
    uint _S53 = CyFrameDecalSource_word_0(index_7, kernelContext_10);

    float _S54 = (as_type<float>((_S53)));

#line 115
    uint _S55 = CyFrameDecalSource_word_0(index_7 + 1U, kernelContext_10);

#line 115
    float _S56 = (as_type<float>((_S55)));

#line 115
    uint _S57 = CyFrameDecalSource_word_0(index_7 + 2U, kernelContext_10);

#line 115
)cy_msl",
    R"cy_msl(    return float3(_S54, _S56, (as_type<float>((_S57))));
}


#line 108
float wordFloat_0(uint index_8, KernelContext_0 thread* kernelContext_11)
{

#line 108
    uint _S58 = CyFrameDecalSource_word_0(index_8, kernelContext_11);

    return (as_type<float>((_S58)));
}


#line 151 "src/rendering/shaders/cy/volumetric_fog.slang"
float3 cyApplyFog_0(const CyFrameFogVolume_0 thread* source_4, float3 radiance_0, float3 relativePosition_1, KernelContext_0 thread* kernelContext_12)
{

#line 151
    CyFogAtPoint_0 _S59 = cyFogAt_0(source_4, relativePosition_1, kernelContext_12);


    return radiance_0 * _S59.transmittance_0 + _S59.inScattering_0;
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
float pcssVisibility_0(const CyFrameShadowMap_0 thread* source_5, float2 uv_5, float receiver_3, const PcssShape_0 thread* shape_2, float rotation_2, KernelContext_0 thread* kernelContext_13)
{

#line 182
    float _S60 = shape_2->penumbraPerDepth_0;

#line 182
    float _S61 = shape_2->minRadius_0;

#line 182
    float _S62 = shape_2->maxRadius_0;

#line 182
    PcssBlockers_0 _S63 = pcssBlockerSearch_0(source_5, uv_5, receiver_3, clamp((1.0 - receiver_3) * shape_2->penumbraPerDepth_0, shape_2->minRadius_0, shape_2->maxRadius_0), rotation_2, shape_2->blockerTaps_0, kernelContext_13);

#line 189
    if((_S63.count_2) == 0.0)
    {
        return 1.0;
    }

#line 191
    float _S64 = pcssFilter_0(source_5, uv_5, receiver_3, clamp(pcssPenumbraRadius_0(receiver_3, _S63.averageDepth_0, _S60), _S61, _S62), rotation_2, shape_2->filterTaps_0, kernelContext_13);



    return _S64;
}


#line 71 "src/rendering/shaders/cy/decal.slang"
struct CyDecalReceiver_0
{
    float3 relativePosition_2;
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


#line 1256 "src/rendering/shaders/cy/frame.slang"
uint CyFrameDecalSource_word_1(uint index_9, KernelContext_0 thread* kernelContext_14)
{

#line 1256
    float4 _S65 = cyMaterialSampleTextureLevel_0(kernelContext_14->cyFrameView_0->frame_0->decalControl_0.x, cyDecalWordUv_0(index_9, kernelContext_14->cyFrameView_0->frame_0->decalControl_0.y), 0.0, kernelContext_14);


    return cyDecalWordFromTexel_0(_S65);
}


#line 192 "src/rendering/shaders/cy/decal.slang"
float cyDecalAngleFade_0(float cosine_1, float limit_0)
{

#line 192
    bool _S66;

    if(cosine_1 <= 0.0)
    {

#line 194
        _S66 = true;

#line 194
    }
    else
    {

#line 194
        _S66 = cosine_1 <= limit_0;

#line 194
    }

#line 194
    if(_S66)
    {
        return 0.0;
    }
    float _S67 = (cosine_1 - limit_0) / max(1.0 - limit_0, 0.00009999999747379);
    return _S67 * _S67 * (3.0 - 2.0 * _S67);
}


float cyDecalDistanceFade_0(float metres_0, float start_0, float end_0)
{
    float _S68 = max(end_0, start_0 + 0.00009999999747379);
    if(metres_0 <= start_0)
    {
        return 1.0;
    }
    if(metres_0 >= _S68)
    {
        return 0.0;
    }
    float _S69 = 1.0 - (metres_0 - start_0) / (_S68 - start_0);
    return _S69 * _S69 * (3.0 - 2.0 * _S69);
}


#line 410 "src/rendering/shaders/cy/frame.slang"
float4 cyMaterialSampleTextureLevel_1(uint bindlessIndex_1, float2 uv_6, float level_1, KernelContext_0 thread* kernelContext_15)
{
    return ((kernelContext_15->cyFrameGlobals_0->textures_0[bindlessIndex_1]).sample((kernelContext_15->cyFrameGlobals_0->sampler_0), (uv_6), level((level_1))));
}


#line 1261
float CyFrameDecalSource_mask_0(uint slot_2, float2 uv_7, KernelContext_0 thread* kernelContext_16)
{

#line 1261
    float4 _S70 = cyMaterialSampleTextureLevel_1(slot_2, uv_7, 0.0, kernelContext_16);

    return _S70.w;
}


#line 15 "src/rendering/shaders/cy/noise.slang"
uint3 hashPcg3d_0(uint3 value_0)
{
    uint3 _S71 = value_0 * uint3(1664525U)  + uint3(1013904223U) ;

#line 17
    thread uint3 v_0 = _S71;
    v_0.x = v_0.x + _S71.y * _S71.z;
    v_0.y = v_0.y + v_0.z * v_0.x;
    v_0.z = v_0.z + v_0.x * v_0.y;
    uint3 _S72 = v_0 ^ (v_0 >> (uint3(16U) ));

#line 21
    v_0 = _S72;
    v_0.x = v_0.x + _S72.y * _S72.z;
    v_0.y = v_0.y + v_0.z * v_0.x;
    v_0.z = v_0.z + v_0.x * v_0.y;
    return v_0;
}


#line 36
float valueNoise_0(float3 position_0)
{
    float3 _S73 = floor(position_0);
    float3 _S74 = position_0 - _S73;
    float3 _S75 = _S74 * _S74 * (float3(3.0)  - float3(2.0)  * _S74);
    uint3 _S76 = uint3(int3(_S73) + int3(int(1024)) );

#line 41
    uint corner_0 = 0U;

#line 41
    float result_2 = 0.0;


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
        uint3 _S77 = uint3(corner_0 & 1U, (corner_0 >> 1U) & 1U, (corner_0 >> 2U) & 1U);

        float3 _S78 = mix(float3(1.0)  - _S75, _S75, float3(_S77));
        float result_3 = result_2 + float((hashPcg3d_0(_S76 + _S77).x) >> 8U) * 5.9604644775390625e-08 * _S78.x * _S78.y * _S78.z;

#line 44
        corner_0 = corner_0 + 1U;

#line 44
        result_2 = result_3;

#line 44
    }

#line 51
    return result_2;
}


#line 147 "src/rendering/shaders/cy/decal.slang"
float decalFbm_0(float2 uv_8, float frequency_0, uint seed_0)
{
    float _S79 = float(seed_0 % 4096U) * 1.61800003051757812;

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
        float sum_1 = sum_0 + valueNoise_0(float3(uv_8 * float2(scale_0) , _S79)) * amplitude_0;
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
float cyDecalShapeCoverage_0(uint shape_3, float2 uv_9, float a_1, float b_1, uint seed_1)
{
    float _S80 = length(uv_9 - float2(0.5) ) * 2.0;
    if(shape_3 == 1U)
    {

        return 1.0 - smoothstep(0.44999998807907104, 0.94999998807907104, _S80 + (decalFbm_0(uv_9, a_1, seed_1) - 0.5) * b_1);
    }
    if(shape_3 == 2U)
    {

        return 1.0 - smoothstep(b_1 * 0.69999998807907104, b_1, abs(_S80 - a_1));
    }
    if(shape_3 == 3U)
    {

        return smoothstep(b_1, b_1 + 0.11999999731779099, decalFbm_0(uv_9, a_1, seed_1)) * (1.0 - smoothstep(0.75, 1.0, _S80));
    }
    return 1.0;
}


#line 220
void cyApplyDecal_0(uint rank_0, const CyDecalReceiver_0 thread* receiver_4, CyDecalSurface_0 thread* surface_1, KernelContext_0 thread* kernelContext_17)
{

#line 221
    uint _S81 = CyFrameDecalSource_word_1(4U, kernelContext_17);

    uint _S82 = _S81 + rank_0 * 36U;

#line 223
    uint _S83 = CyFrameDecalSource_word_1(_S82 + 28U, kernelContext_17);
    if((_S83 & (receiver_4->channels_0)) == 0U)
    {
        return;
    }

#line 226
    float3 _S84 = wordFloat3_0(_S82 + 4U, kernelContext_17);

#line 226
    float3 _S85 = wordFloat3_0(_S82 + 8U, kernelContext_17);

#line 226
    float3 _S86 = wordFloat3_0(_S82 + 12U, kernelContext_17);

#line 226
    float3 _S87 = receiver_4->relativePosition_2;

#line 226
    float3 _S88 = wordFloat3_0(_S82, kernelContext_17);

#line 231
    float3 _S89 = _S87 - _S88;
    float _S90 = dot(_S89, _S84);

#line 232
    float _S91 = dot(_S89, _S85);

#line 232
    float _S92 = dot(_S89, _S86);


    if(any((abs(float3(_S90, _S91, _S92))) > (float3(1.0) )))
    {
        return;
    }



    float _S93 = dot(receiver_4->geometricNormal_0, normalize(_S86));

#line 242
    float _S94 = wordFloat_0(_S82 + 3U, kernelContext_17);

#line 242
    float weight_0 = cyDecalAngleFade_0(_S93, _S94);

#line 242
    float _S95 = receiver_4->eyeDistance_0;

#line 242
    float _S96 = wordFloat_0(_S82 + 24U, kernelContext_17);

#line 242
    float _S97 = wordFloat_0(_S82 + 25U, kernelContext_17);
    float weight_1 = weight_0 * cyDecalDistanceFade_0(_S95, _S96, _S97);

#line 243
    float _S98 = wordFloat_0(_S82 + 34U, kernelContext_17);

#line 243
    float weight_2;


    if(_S98 > 0.0)
    {

#line 246
        weight_2 = weight_1 * (1.0 - smoothstep(1.0 - _S98, 1.0, abs(_S92)));

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
    float2 _S99 = float2(0.5) ;


    float2 _S100 = float2(_S90, _S91) * _S99 + _S99;

#line 255
    uint _S101 = CyFrameDecalSource_word_1(_S82 + 29U, kernelContext_17);

#line 255
    float _S102 = wordFloat_0(_S82 + 30U, kernelContext_17);

#line 255
    float _S103 = wordFloat_0(_S82 + 31U, kernelContext_17);

#line 255
    uint _S104 = CyFrameDecalSource_word_1(_S82 + 35U, kernelContext_17);

#line 255
    uint _S105 = CyFrameDecalSource_word_1(_S82 + 32U, kernelContext_17);

#line 255
    float _S106;

#line 261
    if(_S105 == 4294967295U)
    {

#line 261
        _S106 = 1.0;

#line 261
    }
    else
    {

#line 261
        float _S107 = CyFrameDecalSource_mask_0(_S105, _S100, kernelContext_17);

#line 261
        _S106 = _S107;

#line 261
    }

    float _S108 = weight_2 * (cyDecalShapeCoverage_0(_S101, _S100, _S102, _S103, _S104) * _S106);
    if(_S108 <= 0.0)
    {
        return;
    }

#line 266
    float3 _S109 = wordFloat3_0(_S82 + 16U, kernelContext_17);

#line 266
    float3 _S110 = wordFloat3_0(_S82 + 20U, kernelContext_17);

#line 271
    float3 _S111 = surface_1->albedo_0;

#line 271
    float _S112 = wordFloat_0(_S82 + 7U, kernelContext_17);

#line 271
    surface_1->albedo_0 = mix(_S111, _S109, float3((_S108 * _S112)) );
    float _S113 = surface_1->roughness_0;

#line 272
    float _S114 = wordFloat_0(_S82 + 19U, kernelContext_17);

#line 272
    float _S115 = wordFloat_0(_S82 + 11U, kernelContext_17);

#line 272
    surface_1->roughness_0 = mix(_S113, _S114, _S108 * _S115);

    float _S116 = surface_1->metallic_0;

#line 274
    float _S117 = wordFloat_0(_S82 + 23U, kernelContext_17);

#line 274
    float _S118 = wordFloat_0(_S82 + 26U, kernelContext_17);

#line 274
    surface_1->metallic_0 = mix(_S116, _S117, _S108 * _S118);

    float3 _S119 = surface_1->emission_0;

#line 276
    float _S120 = wordFloat_0(_S82 + 27U, kernelContext_17);

#line 276
    surface_1->emission_0 = mix(_S119, _S110, float3((_S108 * _S120)) );

#line 276
    float _S121 = wordFloat_0(_S82 + 33U, kernelContext_17);

#line 276
    float _S122 = wordFloat_0(_S82 + 15U, kernelContext_17);

#line 284
    float _S123 = _S121 * _S122 * weight_2;
    if(_S123 > 0.0)
    {

        float2 _S124 = float2(0.00390625, 0.0);

        float2 _S125 = float2(0.0, 0.00390625);



        float3 _S126 = float3(_S123)  * (float3(((cyDecalShapeCoverage_0(_S101, _S100 + _S124, _S102, _S103, _S104) - cyDecalShapeCoverage_0(_S101, _S100 - _S124, _S102, _S103, _S104)) / 0.0078125 * (length(_S84) * 0.5)))  * normalize(_S84) + float3(((cyDecalShapeCoverage_0(_S101, _S100 + _S125, _S102, _S103, _S104) - cyDecalShapeCoverage_0(_S101, _S100 - _S125, _S102, _S103, _S104)) / 0.0078125 * (length(_S85) * 0.5)))  * normalize(_S85));


        surface_1->normal_0 = normalize(surface_1->normal_0 - (_S126 - surface_1->normal_0 * float3(dot(surface_1->normal_0, _S126)) ));

#line 285
    }

#line 299
    return;
}


#line 334
uint cyDecalTableListEntry_0(uint index_10, KernelContext_0 thread* kernelContext_18)
{

#line 334
    uint _S127 = CyFrameDecalSource_word_0(index_10, kernelContext_18);

    return _S127;
}


#line 21 "src/rendering/shaders/cy/cluster.slang"
uint clusterSliceOf_0(const ClusterGrid_0 thread* grid_0, float viewDepth_0)
{

    return uint(clamp(log2(max(viewDepth_0, grid_0->nearPlane_0)) * grid_0->sliceScale_0 + grid_0->sliceBias_0, 0.0, float(grid_0->dimensions_0.z - 1U)));
}


uint3 clusterCoordOf_0(const ClusterGrid_0 thread* grid_1, uint2 pixel_0, uint2 renderExtent_0, float viewDepth_1)
{
    uint2 _S128 = grid_1->dimensions_0.xy;
    uint2 _S129 = min(uint2(float2(pixel_0) / float2(renderExtent_0) * float2(_S128)), _S128 - uint2(1U) );

#line 31
    uint _S130 = clusterSliceOf_0(grid_1, viewDepth_1);

#line 31
    return uint3(_S129, _S130);
}



uint clusterIndexOf_0(const ClusterGrid_0 thread* grid_2, uint3 coord_0)
{
    return coord_0.x + grid_2->dimensions_0.x * (coord_0.y + grid_2->dimensions_0.y * coord_0.z);
}


#line 306 "src/rendering/shaders/cy/decal.slang"
uint2 cyDecalTableList_0(float3 relativePosition_3, float2 pixel_1, KernelContext_0 thread* kernelContext_19)
{

#line 306
    uint _S131 = CyFrameDecalSource_word_1(5U, kernelContext_19);

    if(_S131 == 0U)
    {
        return uint2(0U, 0U);
    }
    thread ClusterGrid_0 grid_3;

#line 312
    uint _S132 = CyFrameDecalSource_word_1(8U, kernelContext_19);

#line 312
    uint _S133 = CyFrameDecalSource_word_1(9U, kernelContext_19);

#line 312
    uint _S134 = CyFrameDecalSource_word_1(10U, kernelContext_19);
    (&grid_3)->dimensions_0 = uint3(_S132, _S133, _S134);

#line 313
    uint _S135 = CyFrameDecalSource_word_1(11U, kernelContext_19);
    (&grid_3)->maxLightsPerCluster_0 = _S135;

#line 314
    float _S136 = wordFloat_0(12U, kernelContext_19);
    (&grid_3)->sliceScale_0 = _S136;

#line 315
    float _S137 = wordFloat_0(13U, kernelContext_19);
    (&grid_3)->sliceBias_0 = _S137;

#line 316
    float _S138 = wordFloat_0(14U, kernelContext_19);
    (&grid_3)->nearPlane_0 = _S138;

#line 317
    float _S139 = wordFloat_0(15U, kernelContext_19);
    (&grid_3)->farPlane_0 = _S139;
    if(_S134 == 0U)
    {
        return uint2(0U, 0U);
    }

#line 321
    uint _S140 = CyFrameDecalSource_word_1(16U, kernelContext_19);

#line 321
    uint _S141 = CyFrameDecalSource_word_1(17U, kernelContext_19);

    uint2 _S142 = uint2(_S140, _S141);

#line 323
    float3 _S143 = wordFloat3_0(18U, kernelContext_19);

#line 323
    float _S144 = wordFloat_0(21U, kernelContext_19);
    float4 _S145 = float4(_S143, _S144);
    float _S146 = dot(_S145.xyz, relativePosition_3) + _S145.w;
    uint2 _S147 = uint2(pixel_1);

#line 326
    thread ClusterGrid_0 _S148 = grid_3;

#line 326
    uint3 _S149 = clusterCoordOf_0(&_S148, _S147, _S142, _S146);

#line 326
    thread ClusterGrid_0 _S150 = grid_3;

#line 326
    uint _S151 = clusterIndexOf_0(&_S150, _S149);

#line 326
    uint _S152 = CyFrameDecalSource_word_1(6U, kernelContext_19);

    uint _S153 = _S152 + _S151 * 2U;

#line 328
    uint _S154 = CyFrameDecalSource_word_1(_S153, kernelContext_19);

#line 328
    uint _S155 = CyFrameDecalSource_word_1(_S153 + 1U, kernelContext_19);
    return uint2(_S154, _S155);
}


#line 135
uint cyDecalCount_0(KernelContext_0 thread* kernelContext_20)
{

#line 135
    uint _S156 = CyFrameDecalSource_word_0(0U, kernelContext_20);

#line 135
    uint _S157;

    if(_S156 == 1129923651U)
    {

#line 137
        uint _S158 = CyFrameDecalSource_word_0(2U, kernelContext_20);

#line 137
        _S157 = _S158;

#line 137
    }
    else
    {

#line 137
        _S157 = 0U;

#line 137
    }

#line 137
    return _S157;
}


#line 74 "src/rendering/shaders/cy/material.slang"
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
)cy_msl",
    R"cy_msl(    (&surface_2)->metallic_1 = 0.0;
    (&surface_2)->emission_1 = float3(0.0) ;
    (&surface_2)->occlusion_0 = 1.0;
    (&surface_2)->opacity_0 = 1.0;
    return surface_2;
}


#line 471 "src/rendering/shaders/cy/frame.slang"
float3 readMaterialFloat3_0(uint material_1, uint wordOffset_0, KernelContext_0 thread* kernelContext_21)
{
    uint _S159 = material_1 * kernelContext_21->cyFrameView_0->frame_0->counts_0.y + wordOffset_0;
    return float3((as_type<float>((kernelContext_21->cyFrameView_0->materialWords_0[_S159]))), (as_type<float>((kernelContext_21->cyFrameView_0->materialWords_0[_S159 + 1U]))), (as_type<float>((kernelContext_21->cyFrameView_0->materialWords_0[_S159 + 2U]))));
}


#line 466
float readMaterialFloat_0(uint material_2, uint wordOffset_1, KernelContext_0 thread* kernelContext_22)
{
    return (as_type<float>((kernelContext_22->cyFrameView_0->materialWords_0[material_2 * kernelContext_22->cyFrameView_0->frame_0->counts_0.y + wordOffset_1])));
}


#line 481
uint readMaterialUint_0(uint material_3, uint wordOffset_2, KernelContext_0 thread* kernelContext_23)
{
    return kernelContext_23->cyFrameView_0->materialWords_0[material_3 * kernelContext_23->cyFrameView_0->frame_0->counts_0.y + wordOffset_2];
}


#line 491
uint materialTextureSlot_0(uint material_4, uint wordOffset_3, KernelContext_0 thread* kernelContext_24)
{
    if(wordOffset_3 == 4294967295U)
    {
        return 4294967295U;
    }

#line 495
    uint _S160 = readMaterialUint_0(material_4, wordOffset_3, kernelContext_24);

    return _S160;
}


#line 405
float4 cyMaterialSampleTexture_0(uint bindlessIndex_2, float2 uv_10, KernelContext_0 thread* kernelContext_25)
{
    return ((kernelContext_25->cyFrameGlobals_0->textures_0[bindlessIndex_2]).sample((kernelContext_25->cyFrameGlobals_0->sampler_0), (uv_10)));
}


#line 527
Surface_0 surfaceOf_0(uint material_5, float3 tint_1, float2 uv_11, KernelContext_0 thread* kernelContext_26)
{
    thread Surface_0 surface_3 = defaultSurface_0();

#line 529
    float3 _S161 = readMaterialFloat3_0(material_5, kernelContext_26->cyFrameView_0->frame_0->materialOffsets_0.x, kernelContext_26);
    (&surface_3)->albedo_1 = _S161 * tint_1;

#line 530
    float _S162 = readMaterialFloat_0(material_5, kernelContext_26->cyFrameView_0->frame_0->materialOffsets_0.y, kernelContext_26);
    (&surface_3)->roughness_1 = clamp(_S162, 0.01999999955296516, 1.0);

#line 531
    float _S163 = readMaterialFloat_0(material_5, kernelContext_26->cyFrameView_0->frame_0->materialOffsets_0.z, kernelContext_26);
    (&surface_3)->metallic_1 = saturate(_S163);

#line 532
    float3 _S164 = readMaterialFloat3_0(material_5, kernelContext_26->cyFrameView_0->frame_0->materialOffsets_0.w, kernelContext_26);
    (&surface_3)->emission_1 = _S164;

#line 533
    uint _S165 = materialTextureSlot_0(material_5, kernelContext_26->cyFrameView_0->frame_0->materialTextures_0.x, kernelContext_26);


    if(_S165 != 4294967295U)
    {

#line 536
        float4 _S166 = cyMaterialSampleTexture_0(_S165, uv_11, kernelContext_26);

        (&surface_3)->albedo_1 = (&surface_3)->albedo_1 * _S166.xyz;

#line 536
    }



    return surface_3;
}


#line 1365
float2 lightmapCoordinate_0(uint address_0, float2 uv2_0, KernelContext_0 thread* kernelContext_27)
{
    float _S167 = float(kernelContext_27->cyFrameView_0->frame_0->lightmapLayout_0.x);

    float _S168 = float(kernelContext_27->cyFrameView_0->frame_0->lightmapLayout_0.z);

#line 1369
    float2 _S169 = float2(float(kernelContext_27->cyFrameView_0->frame_0->lightmapLayout_0.x / 128U)) ;

#line 1375
    return (float2(float(address_0 & 127U), float((address_0 >> 7U) & 127U)) * _S169 + float2(_S168)  + uv2_0 * (float2(float(((address_0 >> 14U) & 127U) + 1U), float(((address_0 >> 21U) & 127U) + 1U)) * _S169 - float2((2.0 * _S168)) ) + float2(0.0, float((address_0 >> 28U) - 1U) * _S167)) / float2(_S167, _S167 * float(kernelContext_27->cyFrameView_0->frame_0->lightmapLayout_0.y));
}


#line 1452
float3 lightmapDensityHue_0(bool lightmapped_0, float2 uv_12, float3 relativePosition_4, KernelContext_0 thread* kernelContext_28)
{
    if(!lightmapped_0)
    {
        return float3(0.18000000715255737, 0.18000000715255737, 0.18000000715255737);
    }


    float2 _S170 = uv_12 * float2(float(kernelContext_28->cyFrameView_0->frame_0->lightmapLayout_0.x), float(kernelContext_28->cyFrameView_0->frame_0->lightmapLayout_0.x * kernelContext_28->cyFrameView_0->frame_0->lightmapLayout_0.y));
    float2 _S171 = dfdx(_S170);

#line 1461
    float2 _S172 = dfdy(_S170);
    float3 _S173 = dfdx(relativePosition_4);
    float3 _S174 = dfdy(relativePosition_4);

    float _S175 = clamp(log2(max(sqrt(dot(_S171, _S171) + dot(_S172, _S172)) / max(sqrt(dot(_S173, _S173) + dot(_S174, _S174)), 9.99999971718068537e-10), 9.99999971718068537e-10) / max((as_type<float>((kernelContext_28->cyFrameView_0->frame_0->lightmapDebug_0.y))), 9.99999997475242708e-07)), -1.0, 1.0);

#line 1465
    float3 _S176;
    if(_S175 < 0.0)
    {

#line 1466
        _S176 = mix(float3(0.10000000149011612, 0.89999997615814209, 0.20000000298023224), float3(0.10000000149011612, 0.25, 1.0), float3(- _S175) );

#line 1466
    }
    else
    {

#line 1466
        _S176 = mix(float3(0.10000000149011612, 0.89999997615814209, 0.20000000298023224), float3(1.0, 0.15000000596046448, 0.10000000149011612), float3(_S175) );

#line 1466
    }

    uint2 _S177 = uint2(floor(_S170));
    if(((_S177.x + _S177.y) & 1U) == 0U)
    {
    }
    else
    {

#line 1469
        _S176 = _S176 * float3(0.55000001192092896) ;

#line 1469
    }

#line 1469
    return _S176;
}


#line 1436
float frameExposureStops_0(KernelContext_0 thread* kernelContext_29)
{

    return kernelContext_29->cyFrameGlobals_0->globals_0->exposureStops_0;
}


#line 1436
float frameExposureStops_1(KernelContext_0 thread* kernelContext_30)
{

    return kernelContext_30->cyFrameGlobals_0->globals_0->exposureStops_0;
}


#line 1447
float3 lightmapDensityColour_0(bool lightmapped_1, float2 uv_13, float3 relativePosition_5, KernelContext_0 thread* kernelContext_31)
{

#line 1447
    float3 _S178 = lightmapDensityHue_0(lightmapped_1, uv_13, relativePosition_5, kernelContext_31);

#line 1447
    float _S179 = frameExposureStops_1(kernelContext_31);

    return _S178 * float3(exp2(- _S179)) ;
}


#line 1474
float3 lightmapChartColour_0(bool lightmapped_2, uint address_1, float2 uv_14, KernelContext_0 thread* kernelContext_32)
{
    if(!lightmapped_2)
    {

#line 1476
        float _S180 = frameExposureStops_0(kernelContext_32);

        return float3(0.18000000715255737, 0.18000000715255737, 0.18000000715255737) * float3(exp2(- _S180)) ;
    }



    float _S181 = float(((address_1 * 2654435761U) ^ (address_1 >> 15U)) >> 8U) * 3.5762786865234375e-07;
    float _S182 = fract(_S181);
    uint _S183 = uint(_S181) % 6U;
    float _S184 = 0.89999997615814209 * (1.0 - 0.64999997615814209 * (1.0 - _S182));

#line 1486
    float3 _S185 = float3(0.89999997615814209, _S184, 0.31499999761581421);

#line 1486
    float3 colour_0;
    if(_S183 == 1U)
    {

#line 1487
        colour_0 = float3(0.89999997615814209 * (1.0 - 0.64999997615814209 * _S182), 0.89999997615814209, 0.31499999761581421);

#line 1487
    }
    else
    {

#line 1487
        colour_0 = _S185;

#line 1487
    }
    if(_S183 == 2U)
    {

#line 1488
        colour_0 = float3(0.31499999761581421, 0.89999997615814209, _S184);

#line 1488
    }
    if(_S183 == 3U)
    {

#line 1489
        colour_0 = float3(0.31499999761581421, 0.89999997615814209 * (1.0 - 0.64999997615814209 * _S182), 0.89999997615814209);

#line 1489
    }
    if(_S183 == 4U)
    {

#line 1490
        colour_0 = float3(_S184, 0.31499999761581421, 0.89999997615814209);

#line 1490
    }
    if(_S183 == 5U)
    {

#line 1491
        colour_0 = float3(0.89999997615814209, 0.31499999761581421, 0.89999997615814209 * (1.0 - 0.64999997615814209 * _S182));

#line 1491
    }


    uint2 _S186 = uint2(floor(uv_14 * float2(float(kernelContext_32->cyFrameView_0->frame_0->lightmapLayout_0.x), float(kernelContext_32->cyFrameView_0->frame_0->lightmapLayout_0.x * kernelContext_32->cyFrameView_0->frame_0->lightmapLayout_0.y))));

#line 1494
    float _S187;
    if(((_S186.x + _S186.y) & 1U) == 0U)
    {

#line 1495
        _S187 = 1.0;

#line 1495
    }
    else
    {

#line 1495
        _S187 = 0.69999998807907104;

#line 1495
    }
    float3 _S188 = colour_0 * float3(_S187) ;

#line 1496
    float _S189 = frameExposureStops_0(kernelContext_32);

#line 1496
    return _S188 * float3(exp2(- _S189)) ;
}


#line 21 "src/rendering/shaders/cy/cluster.slang"
uint clusterSliceOf_1(const ClusterGrid_0 constant* grid_4, float viewDepth_2)
{

    return uint(clamp(log2(max(viewDepth_2, grid_4->nearPlane_0)) * grid_4->sliceScale_0 + grid_4->sliceBias_0, 0.0, float(grid_4->dimensions_0.z - 1U)));
}


uint3 clusterCoordOf_1(const ClusterGrid_0 constant* grid_5, uint2 pixel_2, uint2 renderExtent_1, float viewDepth_3)
{
    uint2 _S190 = grid_5->dimensions_0.xy;
    uint2 _S191 = min(uint2(float2(pixel_2) / float2(renderExtent_1) * float2(_S190)), _S190 - uint2(1U) );

#line 31
    uint _S192 = clusterSliceOf_1(grid_5, viewDepth_3);

#line 31
    return uint3(_S191, _S192);
}



uint clusterIndexOf_1(const ClusterGrid_0 constant* grid_6, uint3 coord_1)
{
    return coord_1.x + grid_6->dimensions_0.x * (coord_1.y + grid_6->dimensions_0.y * coord_1.z);
}


#line 458 "src/rendering/shaders/cy/frame.slang"
float viewDepthOf_0(float3 relative_0, KernelContext_0 thread* kernelContext_33)
{



    return - dot(kernelContext_33->cyFrameView_0->frame_0->relativeToViewRow2_0, float4(relative_0, 1.0));
}


#line 1269
void applyFrameDecals_0(Surface_0 thread* surface_4, float3 thread* normal_2, float3 relativePosition_6, float2 fragmentCentre_0, uint receiverChannels_0, KernelContext_0 thread* kernelContext_34)
{


    thread CyDecalReceiver_0 receiver_5;
    (&receiver_5)->relativePosition_2 = relativePosition_6;
    (&receiver_5)->geometricNormal_0 = *normal_2;
    (&receiver_5)->eyeDistance_0 = length(relativePosition_6);
    (&receiver_5)->channels_0 = receiverChannels_0;
    thread CyDecalSurface_0 decalled_0;
    (&decalled_0)->albedo_0 = surface_4->albedo_1;
    (&decalled_0)->roughness_0 = surface_4->roughness_1;
    (&decalled_0)->metallic_0 = surface_4->metallic_1;
    (&decalled_0)->emission_0 = surface_4->emission_1;
    (&decalled_0)->normal_0 = *normal_2;

#line 1288
    uint _S193 = kernelContext_34->cyFrameView_0->frame_0->decalControl_0.z;

#line 1288
    uint _S194 = cyDecalCount_0(kernelContext_34);


    uint2 _S195 = uint2(0U, _S194);

#line 1291
    uint2 list_0;

#line 1291
    uint mode_0;
    if((_S193 & 2U) != 0U)
    {

#line 1292
        list_0 = _S195;

#line 1292
        mode_0 = 0U;

#line 1292
    }
    else
    {

        if((_S193 & 1U) != 0U)
        {

            if(_S194 != 0U)
            {

#line 1299
                uint2 _S196 = cyDecalTableList_0(relativePosition_6, fragmentCentre_0, kernelContext_34);

#line 1299
                list_0 = _S196;

#line 1299
            }
            else
            {

#line 1299
                list_0 = uint2(0U) ;

#line 1299
            }

#line 1299
            mode_0 = 1U;

#line 1296
        }
        else
        {

#line 1296
            bool _S197;

#line 1301
            if((kernelContext_34->cyFrameView_0->frame_0->counts_0.w) != 0U)
            {

#line 1301
                _S197 = ((&kernelContext_34->cyFrameView_0->frame_0->clusterGrid_0)->dimensions_0.z) != 0U;

#line 1301
            }
            else
            {

#line 1301
                _S197 = false;

#line 1301
            }

#line 1301
            if(_S197)
            {
                uint2 _S198 = uint2(kernelContext_34->cyFrameView_0->frame_0->extentAndInverse_0.xy);
                uint2 _S199 = uint2(fragmentCentre_0);

#line 1304
                float _S200 = viewDepthOf_0(relativePosition_6, kernelContext_34);

#line 1304
                uint3 _S201 = clusterCoordOf_1(&kernelContext_34->cyFrameView_0->frame_0->clusterGrid_0, _S199, _S198, _S200);

#line 1304
                uint _S202 = clusterIndexOf_1(&kernelContext_34->cyFrameView_0->frame_0->clusterGrid_0, _S201);

#line 1304
                list_0 = kernelContext_34->cyFrameView_0->clusterHeaders_0[_S202 * kernelContext_34->cyFrameView_0->frame_0->counts_0.z + 1U];

#line 1304
                mode_0 = 2U;

#line 1301
            }
            else
            {

#line 1301
                list_0 = _S195;

#line 1301
                mode_0 = 0U;

#line 1301
            }

#line 1296
        }

#line 1292
    }

#line 1292
    uint slot_3 = 0U;

#line 1312
    for(;;)
    {

#line 1312
        if(slot_3 < (list_0.y))
        {
        }
        else
        {

#line 1312
            break;
        }

#line 1312
        uint rank_1;


        if(mode_0 == 1U)
        {

#line 1315
            uint _S203 = cyDecalTableListEntry_0(list_0.x + slot_3, kernelContext_34);

#line 1315
            rank_1 = _S203;

#line 1315
        }
        else
        {

            if(mode_0 == 2U)
            {

#line 1319
                rank_1 = kernelContext_34->cyFrameView_0->clusterIndices_0[list_0.x + slot_3];

#line 1319
            }
            else
            {

#line 1319
                rank_1 = slot_3;

#line 1319
            }

#line 1315
        }

#line 1323
        if(rank_1 < _S194)
        {

#line 1323
            thread CyDecalReceiver_0 _S204 = receiver_5;

#line 1323
            cyApplyDecal_0(rank_1, &_S204, &decalled_0, kernelContext_34);

#line 1323
        }

#line 1312
        slot_3 = slot_3 + 1U;

#line 1312
    }

#line 1329
    surface_4->albedo_1 = (&decalled_0)->albedo_0;
    surface_4->roughness_1 = clamp((&decalled_0)->roughness_0, 0.01999999955296516, 1.0);
    surface_4->metallic_1 = saturate((&decalled_0)->metallic_0);
    surface_4->emission_1 = (&decalled_0)->emission_0;
    *normal_2 = (&decalled_0)->normal_0;
    return;
}


#line 761
struct CyBakedLights_0
{
    bool active_0;
    float4 mask_0;
};


#line 1408
CyBakedLights_0 bakedLightsOf_0(bool lightmapped_3, float2 uv_15, KernelContext_0 thread* kernelContext_35)
{
    thread CyBakedLights_0 baked_0;
    (&baked_0)->active_0 = lightmapped_3;
    (&baked_0)->mask_0 = float4(1.0) ;

#line 1412
    bool _S205;
    if(lightmapped_3)
    {

#line 1413
        _S205 = (kernelContext_35->cyFrameView_0->frame_0->lightmapLayout_0.w) != 4294967295U;

#line 1413
    }
    else
    {

#line 1413
        _S205 = false;

#line 1413
    }

#line 1413
    if(_S205)
    {

#line 1413
        float4 _S206 = cyMaterialSampleTexture_0(kernelContext_35->cyFrameView_0->frame_0->lightmapLayout_0.w, uv_15, kernelContext_35);

        (&baked_0)->mask_0 = _S206;

#line 1413
    }



    return baked_0;
}


#line 427
float4 transformToClip_0(float3 relative_1, KernelContext_0 thread* kernelContext_36)
{
    float4 _S207 = float4(relative_1, 1.0);
    return float4(dot(kernelContext_36->cyFrameView_0->frame_0->relativeToClipRow0_0, _S207), dot(kernelContext_36->cyFrameView_0->frame_0->relativeToClipRow1_0, _S207), dot(kernelContext_36->cyFrameView_0->frame_0->relativeToClipRow2_0, _S207), dot(kernelContext_36->cyFrameView_0->frame_0->relativeToClipRow3_0, _S207));
}


#line 925
float2 frameClusterPixel_0(uint instanceFlags_0, float3 relativePosition_7, float2 fragmentCentre_1, KernelContext_0 thread* kernelContext_37)
{
    if((instanceFlags_0 & 256U) == 0U)
    {
        return fragmentCentre_1;
    }

#line 929
    float4 _S208 = transformToClip_0(relativePosition_7, kernelContext_37);


    float _S209 = _S208.w;

#line 932
    bool _S210;

#line 932
    if(_S209 <= 0.0)
    {

#line 932
        _S210 = true;

#line 932
    }
    else
    {

#line 932
        float _S211 = viewDepthOf_0(relativePosition_7, kernelContext_37);

#line 932
        _S210 = _S211 < ((&kernelContext_37->cyFrameView_0->frame_0->clusterGrid_0)->nearPlane_0);

)cy_msl",
    R"cy_msl(#line 932
    }

#line 932
    if(_S210)
    {
        return float2(-1.0, -1.0);
    }
    float2 _S212 = _S208.xy / float2(_S209) ;
    float _S213 = _S212.x;

#line 937
    if((abs(_S213)) > 1.0)
    {

#line 937
        _S210 = true;

#line 937
    }
    else
    {

#line 937
        _S210 = (abs(_S212.y)) > 1.0;

#line 937
    }

#line 937
    if(_S210)
    {
        return float2(-1.0, -1.0);
    }
    return float2(_S213 * 0.5 + 0.5, 0.5 - _S212.y * 0.5) * kernelContext_37->cyFrameView_0->frame_0->extentAndInverse_0.xy;
}


#line 61 "src/rendering/shaders/cy/light.slang"
float distanceAttenuation_0(float distanceSquared_0, float range_1)
{
    float _S214 = distanceSquared_0 / max(range_1 * range_1, 9.99999997475242708e-07);
    float _S215 = saturate(1.0 - _S214 * _S214);
    return _S215 * _S215 / max(distanceSquared_0, 0.00009999999747379);
}


#line 52
struct LightSample_0
{
    float3 direction_1;
    float3 illuminance_0;
    float attenuation_0;
};


#line 68
LightSample_0 evaluateLight_0(const Light_0 thread* light_0, float3 surfaceRelativeToCamera_0)
{
    thread LightSample_0 result_4;

#line 70
    uint _S216 = light_0->kind_0;
    if((light_0->kind_0) == 0U)
    {
        (&result_4)->direction_1 = - light_0->direction_0;
        (&result_4)->attenuation_0 = 1.0;
        (&result_4)->illuminance_0 = light_0->color_0 * float3(light_0->intensity_0) ;
        return result_4;
    }

    float3 _S217 = light_0->positionRelativeToCamera_0 - surfaceRelativeToCamera_0;
    float _S218 = dot(_S217, _S217);
    (&result_4)->direction_1 = _S217 * float3(rsqrt(max(_S218, 9.99999993922529029e-09))) ;
    (&result_4)->attenuation_0 = distanceAttenuation_0(_S218, light_0->range_0);

    if(_S216 == 2U)
    {

        float _S219 = saturate(dot(- (&result_4)->direction_1, light_0->direction_0) * light_0->spotScaleBias_0.x + light_0->spotScaleBias_0.y);
        (&result_4)->attenuation_0 = (&result_4)->attenuation_0 * (_S219 * _S219);

#line 84
    }
    else
    {



        if(_S216 == 3U)
        {

            (&result_4)->attenuation_0 = (&result_4)->attenuation_0 * saturate(dot(- (&result_4)->direction_1, light_0->direction_0) * light_0->spotScaleBias_0.x + light_0->spotScaleBias_0.y);

#line 90
        }

#line 84
    }

#line 95
    (&result_4)->illuminance_0 = light_0->color_0 * float3((light_0->intensity_0 * (&result_4)->attenuation_0)) ;
    return result_4;
}


#line 434 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_2, KernelContext_0 thread* kernelContext_38)
{
    float4 _S220 = float4(relative_2, 1.0);
    return float4(dot(kernelContext_38->cyFrameView_0->frame_0->shadowToClipRow0_0, _S220), dot(kernelContext_38->cyFrameView_0->frame_0->shadowToClipRow1_0, _S220), dot(kernelContext_38->cyFrameView_0->frame_0->shadowToClipRow2_0, _S220), dot(kernelContext_38->cyFrameView_0->frame_0->shadowToClipRow3_0, _S220));
}


#line 98 "src/rendering/shaders/cy/shadow.slang"
float shadowDiscRotation_0(float2 pixel_3)
{

    return fract(52.98291778564453125 * fract(dot(pixel_3, float2(0.06711056083440781, 0.00583714991807938)))) * 6.28318548202514648;
}


#line 690 "src/rendering/shaders/cy/frame.slang"
float softShadowVisibility_0(float2 uv_16, float reference_0, float2 pixel_4, KernelContext_0 thread* kernelContext_39)
{
    thread CyFrameShadowMap_0 map_0;
    (&map_0)->slot_1 = kernelContext_39->cyFrameView_0->frame_0->shadowControl_0.x;
    thread PcssShape_0 shape_4;
    (&shape_4)->penumbraPerDepth_0 = kernelContext_39->cyFrameView_0->frame_0->softShadowShape_0.x;
    (&shape_4)->minRadius_0 = kernelContext_39->cyFrameView_0->frame_0->softShadowShape_0.y;
    (&shape_4)->maxRadius_0 = kernelContext_39->cyFrameView_0->frame_0->softShadowShape_0.z;
    (&shape_4)->blockerTaps_0 = int(max(kernelContext_39->cyFrameView_0->frame_0->softShadowControl_0.z, 1U));
    (&shape_4)->filterTaps_0 = int(max(kernelContext_39->cyFrameView_0->frame_0->softShadowControl_0.w, 1U));
    float _S221 = shadowDiscRotation_0(pixel_4);

#line 700
    thread CyFrameShadowMap_0 _S222 = map_0;

#line 700
    thread PcssShape_0 _S223 = shape_4;

#line 700
    float _S224 = pcssVisibility_0(&_S222, uv_16, reference_0, &_S223, _S221, kernelContext_39);

#line 700
    return _S224;
}


#line 723
float directionalShadowVisibility_0(float3 relativePosition_8, float3 normal_3, float2 fragmentCentre_2, KernelContext_0 thread* kernelContext_40)
{

#line 723
    bool _S225;

    if((kernelContext_40->cyFrameView_0->frame_0->shadowControl_0.w) == 0U)
    {

#line 725
        _S225 = true;

#line 725
    }
    else
    {

#line 725
        _S225 = (kernelContext_40->cyFrameView_0->frame_0->shadowControl_0.x) == 4294967295U;

#line 725
    }

#line 725
    if(_S225)
    {
        return 1.0;
    }

#line 727
    float4 _S226 = transformToShadowClip_0(relativePosition_8 + normal_3 * float3(0.00499999988824129) , kernelContext_40);


    float _S227 = _S226.w;

#line 730
    if(_S227 <= 0.0)
    {
        return 1.0;
    }
    float3 _S228 = _S226.xyz / float3(_S227) ;
    float _S229 = _S228.x * 0.5 + 0.5;

#line 735
    float _S230 = 0.5 - _S228.y * 0.5;

#line 735
    float2 _S231 = float2(_S229, _S230);
    if(_S229 < 0.0)
    {

#line 736
        _S225 = true;

#line 736
    }
    else
    {

#line 736
        _S225 = _S229 > 1.0;

#line 736
    }

#line 736
    if(_S225)
    {

#line 736
        _S225 = true;

#line 736
    }
    else
    {

#line 736
        _S225 = _S230 < 0.0;

#line 736
    }

#line 736
    if(_S225)
    {

#line 736
        _S225 = true;

#line 736
    }
    else
    {

#line 736
        _S225 = _S230 > 1.0;

#line 736
    }

#line 736
    if(_S225)
    {
        return 1.0;
    }
    float _S232 = 1.0 / float(max(kernelContext_40->cyFrameView_0->frame_0->shadowControl_0.z, 1U));
    float _S233 = _S228.z + 0.00050000002374873;
    if(((kernelContext_40->cyFrameView_0->frame_0->softShadowControl_0.x) & 1U) != 0U)
    {

#line 742
        float _S234 = softShadowVisibility_0(_S231, _S233, fragmentCentre_2, kernelContext_40);

        return _S234;
    }

#line 744
    int y_0 = int(-1);

#line 744
    float visible_0 = 0.0;


    for(;;)
    {

#line 747
        if(y_0 <= int(1))
        {
        }
        else
        {

#line 747
            break;
        }

#line 747
        int x_0 = int(-1);

        for(;;)
        {

#line 749
            if(x_0 <= int(1))
            {
            }
            else
            {

#line 749
                break;
            }

#line 749
            float4 _S235 = cyMaterialSampleTextureLevel_0(kernelContext_40->cyFrameView_0->frame_0->shadowControl_0.x, _S231 + float2(float(x_0), float(y_0)) * float2(_S232) , 0.0, kernelContext_40);

#line 749
            float _S236;



            if(_S233 >= (_S235.x))
            {

#line 753
                _S236 = 1.0;

#line 753
            }
            else
            {

#line 753
                _S236 = 0.0;

#line 753
            }

#line 753
            float visible_1 = visible_0 + _S236;

#line 749
            x_0 = x_0 + int(1);

#line 749
            visible_0 = visible_1;

#line 749
        }

#line 747
        y_0 = y_0 + int(1);

#line 747
    }

#line 756
    return visible_0 / 9.0;
}


#line 706
float contactShadowVisibility_0(float2 fragmentCentre_3, KernelContext_0 thread* kernelContext_41)
{

#line 706
    bool _S237;

    if(((kernelContext_41->cyFrameView_0->frame_0->softShadowControl_0.x) & 2U) == 0U)
    {

#line 708
        _S237 = true;

#line 708
    }
    else
    {

#line 708
        _S237 = (kernelContext_41->cyFrameView_0->frame_0->softShadowControl_0.y) == 4294967295U;

#line 708
    }

#line 708
    if(_S237)
    {

        return 1.0;
    }

#line 711
    float4 _S238 = cyMaterialSampleTextureLevel_0(kernelContext_41->cyFrameView_0->frame_0->softShadowControl_0.y, fragmentCentre_3 * kernelContext_41->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_41);


    return _S238.x;
}


#line 870
float frameContactVisibility_0(uint instanceFlags_1, float2 fragmentCentre_4, KernelContext_0 thread* kernelContext_42)
{

#line 870
    bool _S239;

    if((instanceFlags_1 & 256U) != 0U)
    {

#line 872
        _S239 = ((kernelContext_42->cyFrameView_0->frame_0->viewmodelControl_0.x) & 1U) == 0U;

#line 872
    }
    else
    {

#line 872
        _S239 = false;

#line 872
    }

#line 872
    if(_S239)
    {

        return 1.0;
    }

#line 875
    float _S240 = contactShadowVisibility_0(fragmentCentre_4, kernelContext_42);

    return _S240;
}


#line 769
float bakedLightFactor_0(uint index_11, float4 mask_1, KernelContext_0 thread* kernelContext_43)
{

#line 769
    bool _S241;

    if(index_11 < 128U)
    {

#line 771
        _S241 = (((kernelContext_43->cyFrameView_0->frame_0->lightmapDirectLights_0[index_11 >> 5U]) >> (index_11 & 31U)) & 1U) != 0U;

#line 771
    }
    else
    {

#line 771
        _S241 = false;

#line 771
    }

#line 771
    if(_S241)
    {
        return 0.0;
    }

#line 773
    uint channel_0 = 0U;

    for(;;)
    {

#line 775
        if(channel_0 < 4U)
        {
        }
        else
        {

#line 775
            break;
        }
        if((kernelContext_43->cyFrameView_0->frame_0->lightmapShadowLights_0[channel_0]) == index_11)
        {
            return mask_1[channel_0];
        }

#line 775
        channel_0 = channel_0 + 1U;

#line 775
    }

#line 782
    return 1.0;
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
    float _S242 = roughness_2 * roughness_2;
    float _S243 = _S242 * _S242;
    float _S244 = normalDotHalf_0 * normalDotHalf_0 * (_S243 - 1.0) + 1.0;
    return _S243 / max(3.14159274101257324 * _S244 * _S244, 1.00000001168609742e-07);
}


float visibilitySmithGgxCorrelated_0(float normalDotView_0, float normalDotLight_0, float roughness_3)
{

    float _S245 = roughness_3 * roughness_3;
    float _S246 = _S245 * _S245;
    float _S247 = 1.0 - _S246;



    return 0.5 / max(normalDotLight_0 * sqrt(normalDotView_0 * normalDotView_0 * _S247 + _S246) + normalDotView_0 * sqrt(normalDotLight_0 * normalDotLight_0 * _S247 + _S246), 1.00000001168609742e-07);
}

float3 fresnelSchlick_0(float3 f0_0, float viewDotHalf_0)
{

    return f0_0 + (float3(1.0)  - f0_0) * float3(pow(saturate(1.0 - viewDotHalf_0), 5.0)) ;
}


#line 45
float3 specularGgx_0(float3 normal_4, float3 view_0, float3 light_1, float roughness_4, float3 f0_1)
{
    float3 _S248 = normalize(view_0 + light_1);

#line 56
    return float3((distributionGgx_0(saturate(dot(normal_4, _S248)), roughness_4) * visibilitySmithGgxCorrelated_0(saturate(dot(normal_4, view_0)) + 0.00000999999974738, saturate(dot(normal_4, light_1)), roughness_4)))  * fresnelSchlick_0(f0_1, saturate(dot(view_0, _S248)));
}


#line 108 "src/rendering/shaders/cy/material.slang"
float3 shadeSurfaceWithLight_0(const Surface_0 thread* surface_5, float3 worldNormal_0, float3 viewDirection_0, const LightSample_0 thread* sample_0)
{

#line 109
    float3 _S249 = sample_0->direction_1;

    float _S250 = saturate(dot(worldNormal_0, sample_0->direction_1));
    if(_S250 <= 0.0)
    {
        return float3(0.0) ;
    }

#line 120
    return (diffuseLambert_0(surface_5->albedo_1 * float3((1.0 - surface_5->metallic_1)) ) + specularGgx_0(worldNormal_0, viewDirection_0, _S249, surface_5->roughness_1, computeF0_0(surface_5->albedo_1, surface_5->metallic_1))) * sample_0->illuminance_0 * float3(_S250) ;
}


#line 881 "src/rendering/shaders/cy/frame.slang"
float3 directionalContribution_0(uint global_0, const Surface_0 thread* surface_6, float3 relativePosition_9, float3 normal_5, float3 viewDir_0, float2 fragmentCentre_5, uint instanceFlags_2, const CyBakedLights_0 thread* baked_1, KernelContext_0 thread* kernelContext_44)
{
    thread Light_0 _S251 = kernelContext_44->cyFrameView_0->lights_0[global_0];

#line 883
    LightSample_0 _S252 = evaluateLight_0(&_S251, relativePosition_9);

#line 883
    bool _S253;

#line 889
    if(global_0 == (kernelContext_44->cyFrameView_0->frame_0->shadowControl_0.y))
    {

#line 889
        _S253 = (instanceFlags_2 & 8U) != 0U;

#line 889
    }
    else
    {

#line 889
        _S253 = false;

#line 889
    }

#line 889
    float visibility_0;
    if(_S253)
    {

#line 890
        float _S254 = directionalShadowVisibility_0(relativePosition_9, normal_5, fragmentCentre_5, kernelContext_44);

#line 890
        float _S255 = frameContactVisibility_0(instanceFlags_2, fragmentCentre_5, kernelContext_44);

#line 890
        visibility_0 = min(_S254, _S255);

#line 890
    }
    else
    {

#line 890
        visibility_0 = 1.0;

#line 890
    }

#line 897
    if(baked_1->active_0)
    {

#line 897
        float _S256 = bakedLightFactor_0(global_0, baked_1->mask_0, kernelContext_44);

#line 897
        visibility_0 = min(visibility_0, _S256);

#line 897
    }

#line 897
    thread LightSample_0 _S257 = _S252;

#line 897
    float3 _S258 = shadeSurfaceWithLight_0(surface_6, normal_5, viewDir_0, &_S257);



    return _S258 * float3(visibility_0) ;
}


#line 909
struct LocalLightWalk_0
{
    uint count_3;
    uint offset_1;
    bool clustered_0;
    bool stochastic_0;
    uint selected_0;
    float weight_3;
};


#line 944
LocalLightWalk_0 localLightWalk_0(float3 relativePosition_10, float2 clusterPixel_0, float2 fragmentCentre_6, KernelContext_0 thread* kernelContext_45)
{
    thread LocalLightWalk_0 walk_0;
    (&walk_0)->count_3 = kernelContext_45->cyFrameView_0->frame_0->counts_0.x;
    (&walk_0)->offset_1 = 0U;
    (&walk_0)->clustered_0 = false;
    (&walk_0)->stochastic_0 = false;
    (&walk_0)->selected_0 = 0U;
    (&walk_0)->weight_3 = 0.0;

#line 952
    bool _S259;


    if(((kernelContext_45->cyFrameView_0->frame_0->lightingControl_0.w) & 1U) != 0U)
    {

#line 955
        float4 _S260 = cyMaterialSampleTextureLevel_0(kernelContext_45->cyFrameView_0->frame_0->lightingControl_0.z, fragmentCentre_6 * kernelContext_45->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_45);



        (&walk_0)->stochastic_0 = true;
        (&walk_0)->weight_3 = _S260.y;
        float _S261 = _S260.x;

#line 961
        if(_S261 >= 0.0)
        {

#line 961
            _S259 = ((&walk_0)->weight_3) > 0.0;

#line 961
        }
        else
        {

#line 961
            _S259 = false;

#line 961
        }

#line 961
        if(_S259)
        {

#line 961
            _S259 = _S261 < float((&walk_0)->count_3);

#line 961
        }
        else
        {

#line 961
            _S259 = false;

#line 961
        }

#line 961
        uint _S262;
        if(_S259)
        {

#line 962
            _S262 = uint(_S261);

#line 962
        }
        else
        {

#line 962
            _S262 = 0U;

#line 962
        }

#line 962
        (&walk_0)->selected_0 = _S262;
        if(_S259)
        {

#line 963
            _S262 = 1U;

#line 963
        }
        else
        {

#line 963
            _S262 = 0U;

#line 963
        }

#line 963
        (&walk_0)->count_3 = _S262;
        return walk_0;
    }
    if((kernelContext_45->cyFrameView_0->frame_0->counts_0.w) == 0U)
    {

#line 966
        _S259 = true;

#line 966
    }
    else
    {

#line 966
        _S259 = ((&kernelContext_45->cyFrameView_0->frame_0->clusterGrid_0)->dimensions_0.z) == 0U;

#line 966
    }

#line 966
    if(_S259)
    {

#line 966
        _S259 = true;

#line 966
    }
    else
    {

#line 966
        _S259 = (clusterPixel_0.x) < 0.0;

#line 966
    }

#line 966
    if(_S259)
    {
        return walk_0;
    }
    uint2 _S263 = uint2(kernelContext_45->cyFrameView_0->frame_0->extentAndInverse_0.xy);
    uint2 _S264 = uint2(clusterPixel_0);

#line 971
)cy_msl",
    R"cy_msl(    float _S265 = viewDepthOf_0(relativePosition_10, kernelContext_45);

#line 971
    uint3 _S266 = clusterCoordOf_1(&kernelContext_45->cyFrameView_0->frame_0->clusterGrid_0, _S264, _S263, _S265);

#line 971
    uint _S267 = clusterIndexOf_1(&kernelContext_45->cyFrameView_0->frame_0->clusterGrid_0, _S266);

#line 976
    uint2 _S268 = kernelContext_45->cyFrameView_0->clusterHeaders_0[_S267 * kernelContext_45->cyFrameView_0->frame_0->counts_0.z];
    (&walk_0)->clustered_0 = true;
    (&walk_0)->offset_1 = _S268.x;
    (&walk_0)->count_3 = _S268.y;
    return walk_0;
}

uint localLightIndex_0(const LocalLightWalk_0 thread* walk_1, uint slot_4, KernelContext_0 thread* kernelContext_46)
{
    if(walk_1->stochastic_0)
    {
        return walk_1->selected_0;
    }

#line 987
    uint _S269;

    if(walk_1->clustered_0)
    {

#line 989
        _S269 = kernelContext_46->cyFrameView_0->clusterIndices_0[walk_1->offset_1 + slot_4];

#line 989
    }
    else
    {

#line 989
        _S269 = slot_4;

#line 989
    }

#line 989
    return _S269;
}


#line 114 "src/rendering/shaders/cy/light.slang"
bool isAreaLight_0(uint kind_1)
{
    uint _S270 = kind_1 & 255U;

#line 116
    bool _S271;
    if(_S270 >= 4U)
    {

#line 117
        _S271 = _S270 <= 7U;

#line 117
    }
    else
    {

#line 117
        _S271 = false;

#line 117
    }

#line 117
    return _S271;
}


#line 796 "src/rendering/shaders/cy/frame.slang"
float4 frameLtcTexel_0(uint x_1, uint y_1, KernelContext_0 thread* kernelContext_47)
{

#line 796
    float4 _S272 = cyMaterialSampleTextureLevel_0(kernelContext_47->cyFrameView_0->frame_0->lightingControl_0.x, float2((float(x_1) + 0.5) / 24.0, (float(y_1) + 0.5) / 48.0), 0.0, kernelContext_47);



    return _S272;
}


#line 144 "src/rendering/shaders/cy/light.slang"
struct AreaLtc_0
{
    float4 transform_0;
    float magnitude_0;
};


#line 804 "src/rendering/shaders/cy/frame.slang"
AreaLtc_0 frameAreaLtc_0(float roughness_5, float cosTheta_0, KernelContext_0 thread* kernelContext_48)
{



    float _S273 = clamp(sqrt(saturate(roughness_5)) * 24.0 - 0.5, 0.0, 23.0);
    float _S274 = clamp(sqrt(saturate(1.0 - cosTheta_0)) * 24.0 - 0.5, 0.0, 23.0);
    uint _S275 = uint(_S273);
    uint _S276 = uint(_S274);
    uint _S277 = min(_S275 + 1U, 23U);
    uint _S278 = min(_S276 + 1U, 23U);
    float _S279 = _S273 - float(_S275);
    float _S280 = _S274 - float(_S276);

#line 816
    float4 _S281 = frameLtcTexel_0(_S275, _S276, kernelContext_48);

#line 816
    float4 _S282 = frameLtcTexel_0(_S277, _S276, kernelContext_48);
    float4 _S283 = float4(_S279) ;

#line 817
    float4 _S284 = mix(_S281, _S282, _S283);

#line 817
    float4 _S285 = frameLtcTexel_0(_S275, _S278, kernelContext_48);

#line 817
    float4 _S286 = frameLtcTexel_0(_S277, _S278, kernelContext_48);
    float4 _S287 = mix(_S285, _S286, _S283);
    uint _S288 = _S276 + 24U;

#line 819
    float4 _S289 = frameLtcTexel_0(_S275, _S288, kernelContext_48);

#line 819
    float4 _S290 = frameLtcTexel_0(_S277, _S288, kernelContext_48);

#line 819
    float4 _S291 = mix(_S289, _S290, _S283);

    uint _S292 = _S278 + 24U;

#line 821
    float4 _S293 = frameLtcTexel_0(_S275, _S292, kernelContext_48);

#line 821
    float4 _S294 = frameLtcTexel_0(_S277, _S292, kernelContext_48);
    float4 _S295 = float4(_S280) ;

#line 822
    float4 _S296 = mix(_S291, mix(_S293, _S294, _S283), _S295);
    thread AreaLtc_0 entry_0;
    (&entry_0)->transform_0 = mix(_S284, _S287, _S295);
    (&entry_0)->magnitude_0 = _S296.x;
    return entry_0;
}


#line 362 "src/rendering/shaders/cy/light.slang"
float areaLightWindow_0(const Light_0 thread* light_2, float3 surfaceRelativeToCamera_1)
{
    float3 _S297 = light_2->positionRelativeToCamera_0 - surfaceRelativeToCamera_1;
    float _S298 = dot(_S297, _S297) / max(light_2->range_0 * light_2->range_0, 9.99999997475242708e-07);
    float _S299 = saturate(1.0 - _S298 * _S298);
    return _S299 * _S299;
}


#line 161
float3 unitOr_0(float3 value_1, float3 fallback_0)
{
    float _S300 = dot(value_1, value_1);

#line 163
    float3 _S301;
    if(_S300 > 9.99999968265522539e-21)
    {

#line 164
        _S301 = value_1 * float3(rsqrt(_S300)) ;

#line 164
    }
    else
    {

#line 164
        _S301 = fallback_0;

#line 164
    }

#line 164
    return _S301;
}


#line 120
float signNotZero_0(float value_2)
{

#line 120
    float _S302;

    if(value_2 >= 0.0)
    {

#line 122
        _S302 = 1.0;

#line 122
    }
    else
    {

#line 122
        _S302 = -1.0;

#line 122
    }

#line 122
    return _S302;
}


float3 decodeAreaTangent_0(uint word_0)
{
    float _S303 = max(float(int(word_0 << 16U) >> 16U) / 32767.0, -1.0);
    float _S304 = max(float(int(word_0) >> 16U) / 32767.0, -1.0);
    float _S305 = 1.0 - abs(_S303) - abs(_S304);

#line 130
    thread float3 unit_0 = float3(_S303, _S304, _S305);
    if(_S305 < 0.0)
    {

        float _S306 = (1.0 - abs(unit_0.x)) * signNotZero_0(unit_0.y);
        unit_0.x = (1.0 - abs(unit_0.y)) * signNotZero_0(unit_0.x);
        unit_0.y = _S306;

#line 131
    }

#line 138
    float _S307 = dot(unit_0, unit_0);

#line 138
    float3 _S308;
    if(_S307 > 0.0)
    {

#line 139
        _S308 = unit_0 * float3(rsqrt(_S307)) ;

#line 139
    }
    else
    {

#line 139
        _S308 = float3(1.0, 0.0, 0.0);

#line 139
    }

#line 139
    return _S308;
}


#line 168
float3 facingPerpendicular_0(float3 axis_0, float3 towards_0)
{

#line 168
    float3 _S309;

    if((abs(axis_0.y)) < 0.99000000953674316)
    {

#line 170
        _S309 = float3(0.0, 1.0, 0.0);

#line 170
    }
    else
    {

#line 170
        _S309 = float3(1.0, 0.0, 0.0);

#line 170
    }
    return unitOr_0(cross(axis_0, towards_0), normalize(cross(axis_0, _S309)));
}


#line 152
struct AreaQuad_0
{
    float3 corner0_0;
    float3 corner1_0;
    float3 corner2_0;
    float3 corner3_0;
    bool lit_2;
};


#line 175
AreaQuad_0 areaLightQuad_0(const Light_0 thread* light_3, float3 surfaceRelativeToCamera_2)
{

    uint _S310 = (light_3->kind_0) & 255U;
    float3 _S311 = light_3->positionRelativeToCamera_0 - surfaceRelativeToCamera_2;
    float3 _S312 = unitOr_0(light_3->direction_0, float3(0.0, -1.0, 0.0));


    float extentX_0 = light_3->spotScaleBias_0.x;
    float extentY_0 = light_3->spotScaleBias_0.y;
    bool _S313 = ((light_3->kind_0) & 256U) == 0U;

#line 185
    bool singleSided_0;
    if(_S310 == 4U)
    {

#line 186
        singleSided_0 = true;

#line 186
    }
    else
    {

#line 186
        singleSided_0 = _S310 == 5U;

#line 186
    }

#line 186
    float3 axisX_0;

#line 186
    float3 axisY_0;

#line 186
    float extentX_1;

#line 186
    float extentY_1;

#line 186
    if(singleSided_0)
    {
        float3 _S314 = decodeAreaTangent_0(light_3->shapeWord_0);
        float3 axisX_1 = unitOr_0(_S314 - _S312 * float3(dot(_S314, _S312)) , facingPerpendicular_0(_S312, _S311));
        float3 _S315 = cross(_S312, axisX_1);
        if(_S310 == 5U)
        {

            float extentY_2 = extentY_0 * 0.88622695207595825;

#line 194
            extentX_1 = extentX_0 * 0.88622695207595825;

#line 194
            extentY_1 = extentY_2;

#line 191
        }
        else
        {

#line 191
            extentX_1 = extentX_0;

#line 191
            extentY_1 = extentY_0;

#line 191
        }

#line 191
        axisX_0 = axisX_1;

#line 191
        axisY_0 = _S315;

#line 191
        singleSided_0 = _S313;

#line 186
    }
    else
    {

#line 197
        if(_S310 == 7U)
        {

#line 197
            axisX_0 = facingPerpendicular_0(_S312, _S311);

#line 197
            axisY_0 = _S312;

#line 197
            extentX_1 = extentY_0;

#line 197
            extentY_1 = extentX_0;

#line 197
        }
        else
        {

#line 210
            float extentX_2 = extentX_0 * 0.88622695207595825;

            float3 _S316 = float3(0.0, 0.0, 1.0);

#line 212
            float3 _S317 = unitOr_0(_S311, _S316);
            float3 axisX_2 = unitOr_0(cross(_S316, _S317), float3(1.0, 0.0, 0.0));
            float3 _S318 = cross(_S317, axisX_2);

#line 214
            axisX_0 = axisX_2;

#line 214
            axisY_0 = _S318;

#line 214
            extentX_1 = extentX_2;

#line 214
            extentY_1 = extentX_2;

#line 197
        }

#line 197
        singleSided_0 = false;

#line 186
    }

#line 217
    thread AreaQuad_0 quad_0;
    float3 _S319 = axisX_0 * float3(extentX_1) ;

#line 218
    float3 _S320 = _S311 - _S319;

#line 218
    float3 _S321 = axisY_0 * float3(extentY_1) ;

#line 218
    (&quad_0)->corner0_0 = _S320 - _S321;
    float3 _S322 = _S311 + _S319;

#line 219
    (&quad_0)->corner1_0 = _S322 - _S321;
    (&quad_0)->corner2_0 = _S322 + _S321;
    (&quad_0)->corner3_0 = _S320 + _S321;


    if(!singleSided_0)
    {

#line 224
        singleSided_0 = true;

#line 224
    }
    else
    {

#line 224
        singleSided_0 = (dot(cross(axisX_0, axisY_0), _S311)) <= 0.0;

#line 224
    }

#line 224
    (&quad_0)->lit_2 = singleSided_0;
    return quad_0;
}


float3 edgeVector_0(float3 a_2, float3 b_2)
{

    float _S323 = acos(clamp(dot(a_2, b_2), -1.0, 1.0));
    float3 _S324 = cross(a_2, b_2);
    float _S325 = length(_S324);

#line 234
    float _S326;
    if(_S325 > 9.99999997475242708e-07)
    {

#line 235
        _S326 = _S323 / _S325;

#line 235
    }
    else
    {

#line 235
        _S326 = 1.0;

#line 235
    }
    return _S324 * float3(_S326) ;
}


#line 243
struct PolygonIntegral_0
{
    float integral_0;
    float3 direction_2;
};

PolygonIntegral_0 cosinePolygonVector_0(float3 c0_0, float3 c1_0, float3 c2_0, float3 c3_0)
{
    thread PolygonIntegral_0 result_5;
    (&result_5)->integral_0 = 0.0;
    float3 _S327 = float3(0.0, 0.0, 1.0);

#line 253
    (&result_5)->direction_2 = _S327;
    array<float3, int(4)> _S328 = { unitOr_0(c0_0, _S327), unitOr_0(c1_0, _S327), unitOr_0(c2_0, _S327), unitOr_0(c3_0, _S327) };

    thread array<float3, int(5)> clipped_0;

#line 256
    uint index_12 = 0U;

#line 256
    uint written_0 = 0U;

    for(;;)
    {

#line 258
        if(index_12 < 4U)
        {
        }
        else
        {

#line 258
            break;
        }

#line 258
        uint _S329 = index_12;


        uint _S330 = index_12 + 1U;

#line 261
        uint _S331 = _S330 % 4U;
        float _S332 = _S328[index_12].z;

#line 262
        bool _S333 = _S332 > 0.0;
        float _S334 = _S328[_S331].z;

#line 263
        bool _S335 = _S334 > 0.0;

#line 263
        uint written_1;
        if(_S333)
        {
            clipped_0[written_0] = _S328[_S329];

#line 266
            written_1 = written_0 + 1U;

#line 264
        }
        else
        {

#line 264
            written_1 = written_0;

#line 264
        }

#line 269
        if(_S333 != _S335)
        {

            clipped_0[written_1] = _S328[_S329] + (_S328[_S331] - _S328[_S329]) * float3((_S332 / (_S332 - _S334))) ;

#line 272
            written_0 = written_1 + 1U;

#line 269
        }
        else
        {

#line 269
            written_0 = written_1;

#line 269
        }

#line 258
        index_12 = _S330;

#line 258
    }

#line 276
    if(written_0 < 3U)
    {
        return result_5;
    }
    float3 _S336 = float3(0.0) ;

#line 280
    index_12 = 0U;

#line 280
    float3 sum_2 = _S336;
    for(;;)
    {

#line 281
        if(index_12 < written_0)
        {
        }
        else
        {

#line 281
            break;
        }
        float3 _S337 = unitOr_0(clipped_0[index_12], _S327);
        uint _S338 = index_12 + 1U;

#line 284
        uint _S339 = _S338 % written_0;
        float3 sum_3 = sum_2 + edgeVector_0(_S337, unitOr_0(clipped_0[_S339], _S327));

#line 281
        index_12 = _S338;

#line 281
        sum_2 = sum_3;

#line 281
    }

#line 287
    if((sum_2.z) < 0.0)
    {

#line 287
        sum_2 = - sum_2;

#line 287
    }



    (&result_5)->integral_0 = sum_2.z / 6.28318548202514648;
    (&result_5)->direction_2 = unitOr_0(sum_2, _S327);
    return result_5;
}


float cosinePolygonIntegral_0(float3 c0_1, float3 c1_1, float3 c2_1, float3 c3_1)
{
    PolygonIntegral_0 _S340 = cosinePolygonVector_0(c0_1, c1_1, c2_1, c3_1);

#line 299
    return _S340.integral_0;
}


float areaFormFactor_0(const AreaQuad_0 thread* quad_1, float3 normal_6)
{
    if(!quad_1->lit_2)
    {
        return 0.0;
    }

#line 307
    float3 _S341;

    if((abs(normal_6.z)) < 0.99900001287460327)
    {

#line 309
        _S341 = float3(0.0, 0.0, 1.0);

#line 309
    }
    else
    {

#line 309
        _S341 = float3(1.0, 0.0, 0.0);

#line 309
    }
    float3 _S342 = unitOr_0(cross(_S341, normal_6), float3(1.0, 0.0, 0.0));
    float3 _S343 = cross(normal_6, _S342);
    float _S344 = cosinePolygonIntegral_0(float3(dot(quad_1->corner0_0, _S342), dot(quad_1->corner0_0, _S343), dot(quad_1->corner0_0, normal_6)), float3(dot(quad_1->corner1_0, _S342), dot(quad_1->corner1_0, _S343), dot(quad_1->corner1_0, normal_6)), float3(dot(quad_1->corner2_0, _S342), dot(quad_1->corner2_0, _S343), dot(quad_1->corner2_0, normal_6)), float3(dot(quad_1->corner3_0, _S342), dot(quad_1->corner3_0, _S343), dot(quad_1->corner3_0, normal_6)));

#line 312
    return _S344;
}


#line 322
PolygonIntegral_0 areaSpecularIntegral_0(const AreaQuad_0 thread* quad_2, float3 normal_7, float3 view_1, const AreaLtc_0 thread* ltc_0)
{
    thread PolygonIntegral_0 result_6;
    (&result_6)->integral_0 = 0.0;
    (&result_6)->direction_2 = normal_7;
    if(!quad_2->lit_2)
    {
        return result_6;
    }

    float3 _S345 = unitOr_0(view_1 - normal_7 * float3(clamp(dot(normal_7, view_1), 0.00100000004749745, 1.0)) , float3(1.0, 0.0, 0.0));
    float3 _S346 = cross(normal_7, _S345);

#line 333
    float4 _S347 = ltc_0->transform_0;

    thread array<float3, int(4)> local_0;
    array<float3, int(4)> _S348 = { quad_2->corner0_0, quad_2->corner1_0, quad_2->corner2_0, quad_2->corner3_0 };

#line 336
    uint index_13 = 0U;
    for(;;)
    {

#line 337
        if(index_13 < 4U)
        {
        }
        else
        {

#line 337
            break;
        }

        float _S349 = dot(_S348[index_13], _S345);

#line 340
        float _S350 = dot(_S348[index_13], normal_7);

        local_0[index_13] = float3(_S347.x * _S349 + _S347.y * _S350, _S347.z * dot(_S348[index_13], _S346), _S347.w * _S349 + _S350);

#line 337
        index_13 = index_13 + 1U;

#line 337
    }

#line 345
    PolygonIntegral_0 _S351 = cosinePolygonVector_0(local_0[int(0)], local_0[int(1)], local_0[int(2)], local_0[int(3)]);


    float _S352 = _S347.x;

#line 348
    float _S353 = _S347.y;

#line 348
    float _S354 = _S347.w;

#line 348
    float _S355 = _S352 - _S353 * _S354;

#line 348
    bool _S356;
    if((abs(_S355)) >= 9.99999993922529029e-09)
    {

#line 349
        _S356 = (abs(_S347.z)) >= 9.99999993922529029e-09;

#line 349
    }
    else
    {

#line 349
        _S356 = false;

#line 349
    }

#line 349
    float3 gathered_0;

#line 349
    if(_S356)
    {
        float _S357 = _S351.direction_2.x;

#line 351
        float _S358 = _S351.direction_2.z;

#line 351
        gathered_0 = unitOr_0(float3((_S357 - _S353 * _S358) / _S355, _S351.direction_2.y / _S347.z, (_S352 * _S358 - _S354 * _S357) / _S355), _S351.direction_2);

#line 349
    }
    else
    {

#line 349
        gathered_0 = _S351.direction_2;

#line 349
    }

#line 355
    (&result_6)->integral_0 = _S351.integral_0;
    (&result_6)->direction_2 = unitOr_0(_S345 * float3(gathered_0.x)  + _S346 * float3(gathered_0.y)  + normal_7 * float3(gathered_0.z) , normal_7);

    return result_6;
}


#line 371
struct AreaShading_0
{
    float3 diffuse_0;
    float3 specular_0;
};


AreaShading_0 shadeAreaLight_0(const Light_0 thread* light_4, float3 surfaceRelativeToCamera_3, float3 normal_8, float3 view_2, float3 albedo_4, float roughness_6, float metallic_3, bool hasLtc_0, const AreaLtc_0 thread* ltc_1)
{


    thread AreaShading_0 result_7;
    float3 _S359 = float3(0.0) ;

#line 383
    (&result_7)->diffuse_0 = _S359;
)cy_msl",
    R"cy_msl(    (&result_7)->specular_0 = _S359;

#line 384
    float _S360 = areaLightWindow_0(light_4, surfaceRelativeToCamera_3);

    if(_S360 <= 0.0)
    {
        return result_7;
    }

#line 388
    AreaQuad_0 _S361 = areaLightQuad_0(light_4, surfaceRelativeToCamera_3);


    float3 _S362 = light_4->color_0 * float3((light_4->intensity_0 * _S360)) ;
    float3 _S363 = albedo_4 * float3((1.0 - metallic_3))  * _S362;

#line 392
    thread AreaQuad_0 _S364 = _S361;

#line 392
    float _S365 = areaFormFactor_0(&_S364, normal_8);

#line 392
    (&result_7)->diffuse_0 = _S363 * float3(_S365) ;
    if(!hasLtc_0)
    {
        return result_7;
    }

#line 395
    thread AreaQuad_0 _S366 = _S361;

#line 395
    PolygonIntegral_0 _S367 = areaSpecularIntegral_0(&_S366, normal_8, view_2, ltc_1);

#line 402
    float3 _S368 = mix(float3(0.03999999910593033) , albedo_4, float3(metallic_3) );

    (&result_7)->specular_0 = (_S368 + (float3(1.0)  - _S368) * float3(pow(1.0 - saturate(dot(view_2, unitOr_0(view_2 + _S367.direction_2, normal_8))), 5.0)) ) * _S362 * float3((_S367.integral_0 * ltc_1->magnitude_0)) ;
    return result_7;
}


#line 830 "src/rendering/shaders/cy/frame.slang"
AreaShading_0 frameAreaShading_0(uint index_14, const Surface_0 thread* surface_7, float3 relativePosition_11, float3 normal_9, float3 viewDir_1, KernelContext_0 thread* kernelContext_49)
{

    bool _S369 = (kernelContext_49->cyFrameView_0->frame_0->lightingControl_0.x) != 4294967295U;
    thread AreaLtc_0 ltc_2;
    (&ltc_2)->transform_0 = float4(1.0, 0.0, 1.0, 0.0);
    (&ltc_2)->magnitude_0 = 1.0;
    if(_S369)
    {

#line 837
        AreaLtc_0 _S370 = frameAreaLtc_0(surface_7->roughness_1, clamp(dot(normal_9, viewDir_1), 0.00100000004749745, 1.0), kernelContext_49);

        ltc_2 = _S370;

#line 837
    }

#line 837
    thread Light_0 _S371 = kernelContext_49->cyFrameView_0->lights_0[index_14];

#line 837
    thread AreaLtc_0 _S372 = ltc_2;

#line 837
    AreaShading_0 _S373 = shadeAreaLight_0(&_S371, relativePosition_11, normal_9, viewDir_1, surface_7->albedo_1, surface_7->roughness_1, surface_7->metallic_1, _S369, &_S372);



    return _S373;
}


#line 856
LightSample_0 bakedLightSample_0(uint index_15, float3 relativePosition_12, const CyBakedLights_0 thread* baked_2, KernelContext_0 thread* kernelContext_50)
{

#line 856
    thread Light_0 _S374 = kernelContext_50->cyFrameView_0->lights_0[index_15];

#line 856
    LightSample_0 _S375 = evaluateLight_0(&_S374, relativePosition_12);

    thread LightSample_0 sample_1 = _S375;
    if(baked_2->active_0)
    {

#line 859
        float _S376 = bakedLightFactor_0(index_15, baked_2->mask_0, kernelContext_50);

        (&sample_1)->illuminance_0 = (&sample_1)->illuminance_0 * float3(_S376) ;

#line 859
    }



    return sample_1;
}


#line 992
float3 accumulateLights_0(const Surface_0 thread* surface_8, float3 relativePosition_13, float3 normal_10, float3 viewDir_2, float2 fragmentCentre_7, uint instanceFlags_3, const CyBakedLights_0 thread* baked_3, KernelContext_0 thread* kernelContext_51)
{

#line 993
    bool _S377;

#line 993
    float2 _S378 = frameClusterPixel_0(instanceFlags_3, relativePosition_13, fragmentCentre_7, kernelContext_51);


    float3 _S379 = float3(0.0) ;
    uint _S380 = kernelContext_51->cyFrameView_0->frame_0->counts_0.x;

#line 1012
    bool _S381 = ((kernelContext_51->cyFrameView_0->frame_0->directionalLights_0.w) & 2147483648U) != 0U;

#line 1012
    uint _S382;
    if(_S381)
    {

#line 1013
        _S382 = min((kernelContext_51->cyFrameView_0->frame_0->directionalLights_0.w) & 255U, 3U);

#line 1013
    }
    else
    {

#line 1013
        _S382 = _S380;

#line 1013
    }

#line 1013
    uint slot_5 = 0U;

#line 1013
    float3 lit_3 = _S379;
    for(;;)
    {

#line 1014
        if(slot_5 < _S382)
        {
        }
        else
        {

#line 1014
            break;
        }

#line 1014
        uint _S383;

        if(_S381)
        {

#line 1016
            _S383 = kernelContext_51->cyFrameView_0->frame_0->directionalLights_0[slot_5];

#line 1016
        }
        else
        {

#line 1016
            _S383 = slot_5;

#line 1016
        }
        if(!_S381)
        {

#line 1017
            _S377 = (kernelContext_51->cyFrameView_0->lights_0[_S383].kind_0) != 0U;

#line 1017
        }
        else
        {

#line 1017
            _S377 = false;

#line 1017
        }

#line 1017
        if(_S377)
        {
            slot_5 = slot_5 + 1U;

#line 1014
            continue;
        }

#line 1014
        float3 _S384 = directionalContribution_0(_S383, surface_8, relativePosition_13, normal_10, viewDir_2, fragmentCentre_7, instanceFlags_3, baked_3, kernelContext_51);

#line 1014
        lit_3 = lit_3 + _S384;

#line 1014
        slot_5 = slot_5 + 1U;

#line 1014
    }

#line 1014
    LocalLightWalk_0 _S385 = localLightWalk_0(relativePosition_13, _S378, fragmentCentre_7, kernelContext_51);

#line 1014
    float3 sampled_0;

#line 1031
    if(_S385.stochastic_0)
    {

#line 1031
        float4 _S386 = cyMaterialSampleTextureLevel_0(kernelContext_51->cyFrameView_0->frame_0->lightingControl_0.y, fragmentCentre_7 * kernelContext_51->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_51);

#line 1031
        sampled_0 = surface_8->albedo_1 * float3((1.0 - surface_8->metallic_1))  * float3(0.31830987334251404)  * _S386.xyz;

#line 1031
    }
    else
    {

#line 1031
        sampled_0 = _S379;

#line 1031
    }

#line 1031
    slot_5 = 0U;

#line 1037
    for(;;)
    {

#line 1037
        if(slot_5 < (_S385.count_3))
        {
        }
        else
        {

#line 1037
            break;
        }

#line 1037
        thread LocalLightWalk_0 _S387 = _S385;

#line 1037
        uint _S388 = localLightIndex_0(&_S387, slot_5, kernelContext_51);


        if(_S388 >= _S380)
        {

#line 1040
            _S377 = true;

#line 1040
        }
        else
        {

#line 1040
            _S377 = (kernelContext_51->cyFrameView_0->lights_0[_S388].kind_0) == 0U;

#line 1040
        }

#line 1040
        bool _S389;

#line 1040
        if(_S377)
        {

#line 1040
            _S389 = true;

#line 1040
        }
        else
        {

#line 1040
            _S389 = (kernelContext_51->cyFrameView_0->lights_0[_S388].kind_0) == 8U;

#line 1040
        }

#line 1040
        if(_S389)
        {

            slot_5 = slot_5 + 1U;

#line 1037
            continue;
        }

#line 1049
        float3 _S390 = kernelContext_51->cyFrameView_0->lights_0[_S388].positionRelativeToCamera_0 - relativePosition_13;

#line 1049
        bool _S391;
        if(_S385.clustered_0)
        {

#line 1050
            _S391 = (dot(_S390, _S390)) >= (kernelContext_51->cyFrameView_0->lights_0[_S388].range_0 * kernelContext_51->cyFrameView_0->lights_0[_S388].range_0);

#line 1050
        }
        else
        {

#line 1050
            _S391 = false;

#line 1050
        }

#line 1050
        if(_S391)
        {
            slot_5 = slot_5 + 1U;

#line 1037
            continue;
        }

#line 1054
        if(isAreaLight_0(kernelContext_51->cyFrameView_0->lights_0[_S388].kind_0))
        {

#line 1054
            AreaShading_0 _S392 = frameAreaShading_0(_S388, surface_8, relativePosition_13, normal_10, viewDir_2, kernelContext_51);


            if(_S385.stochastic_0)
            {

#line 1057
                sampled_0 = sampled_0 + _S392.specular_0 * float3(_S385.weight_3) ;


                slot_5 = slot_5 + 1U;

#line 1037
                continue;
            }

#line 1037
            float _S393;

#line 1063
            if(baked_3->active_0)
            {

#line 1063
                float _S394 = bakedLightFactor_0(_S388, baked_3->mask_0, kernelContext_51);

#line 1063
                _S393 = _S394;

#line 1063
            }
            else
            {

#line 1063
                _S393 = 1.0;

#line 1063
            }

#line 1063
            lit_3 = lit_3 + (_S392.diffuse_0 + _S392.specular_0) * float3(_S393) ;

            slot_5 = slot_5 + 1U;

#line 1037
            continue;
        }

#line 1067
        if(_S385.stochastic_0)
        {

#line 1067
            thread Light_0 _S395 = kernelContext_51->cyFrameView_0->lights_0[_S388];

#line 1067
            LightSample_0 _S396 = evaluateLight_0(&_S395, relativePosition_13);

#line 1067
            sampled_0 = sampled_0 + specularGgx_0(normal_10, viewDir_2, _S396.direction_1, surface_8->roughness_1, computeF0_0(surface_8->albedo_1, surface_8->metallic_1)) * _S396.illuminance_0 * float3(saturate(dot(normal_10, _S396.direction_1)))  * float3(_S385.weight_3) ;

#line 1074
            slot_5 = slot_5 + 1U;

#line 1037
            continue;
        }

#line 1037
        LightSample_0 _S397 = bakedLightSample_0(_S388, relativePosition_13, baked_3, kernelContext_51);

#line 1037
        thread LightSample_0 _S398 = _S397;

#line 1037
        float3 _S399 = shadeSurfaceWithLight_0(surface_8, normal_10, viewDir_2, &_S398);

#line 1037
        lit_3 = lit_3 + _S399;

#line 1037
        slot_5 = slot_5 + 1U;

#line 1037
    }

#line 1079
    if(_S385.stochastic_0)
    {

#line 1079
        lit_3 = lit_3 + sampled_0;

#line 1079
    }

#line 1079
    return lit_3;
}


#line 1385
float3 lightmapAmbient_0(float2 uv_17, float3 normal_11, KernelContext_0 thread* kernelContext_52)
{

#line 1385
    float4 _S400 = cyMaterialSampleTexture_0(kernelContext_52->cyFrameView_0->frame_0->lightmapControl_0.x, uv_17, kernelContext_52);


    uint _S401 = kernelContext_52->cyFrameView_0->frame_0->lightmapControl_0.w;
    if(_S401 == 2U)
    {

#line 1389
        float4 _S402 = cyMaterialSampleTexture_0(kernelContext_52->cyFrameView_0->frame_0->lightmapControl_0.y, uv_17, kernelContext_52);



        return _S400.xyz * float3((max(0.0, 1.0 + dot(_S402.xyz, normal_11)) / max(_S402.w, 0.00100000004749745))) ;
    }
    if(_S401 == 3U)
    {

#line 1395
        float4 _S403 = cyMaterialSampleTexture_0(kernelContext_52->cyFrameView_0->frame_0->lightmapControl_0.y, uv_17, kernelContext_52);

#line 1395
        float4 _S404 = cyMaterialSampleTexture_0(kernelContext_52->cyFrameView_0->frame_0->lightmapControl_0.z, uv_17, kernelContext_52);



        return float3(max(0.0, _S400.w + dot(_S400.xyz, normal_11)), max(0.0, _S403.w + dot(_S403.xyz, normal_11)), max(0.0, _S404.w + dot(_S404.xyz, normal_11)));
    }


    return _S400.xyz;
}


#line 1163
float4 probeVolumeTexel_0(uint3 probe_0, uint texel_3, KernelContext_0 thread* kernelContext_53)
{
    uint3 _S405 = kernelContext_53->cyFrameView_0->frame_0->probeVolumeControl_0.yzw;

    uint _S406 = _S405.y;

#line 1167
    float4 _S407 = cyMaterialSampleTextureLevel_0(kernelContext_53->cyFrameView_0->frame_0->probeVolumeControl_0.x, (float2(float(probe_0.x * 5U + texel_3), float(probe_0.y + _S406 * probe_0.z)) + float2(0.5) ) / float2(float(_S405.x * 5U), float(_S406 * _S405.z)), 0.0, kernelContext_53);


    return _S407;
}



float probeVisibility_0(float4 distances0_0, float4 distances1_0, float3 probePosition_0, float3 position_1, float3 normal_12, float3 query_0, KernelContext_0 thread* kernelContext_54)
{

    float3 _S408 = probePosition_0 - position_1;
    float _S409 = dot(_S408, _S408);

#line 1179
    float3 _S410;
    if(_S409 > 9.999999960041972e-13)
    {

#line 1180
        _S410 = _S408 * float3(rsqrt(_S409)) ;

#line 1180
    }
    else
    {

#line 1180
        _S410 = normal_12;

#line 1180
    }
    float _S411 = max(0.00009999999747379, (dot(_S410, normal_12) + 1.0) * 0.5);
    float _S412 = _S411 * _S411 + 0.20000000298023224;



    float3 _S413 = query_0 - probePosition_0;

#line 1186
    float _S414;
    if((_S413.x) >= 0.0)
    {

#line 1187
        _S414 = distances0_0.y;

#line 1187
    }
    else
    {

#line 1187
        _S414 = distances0_0.z;

#line 1187
    }

#line 1187
    float _S415;
    if((_S413.y) >= 0.0)
    {

#line 1188
        _S415 = distances0_0.w;

#line 1188
    }
    else
    {

#line 1188
        _S415 = distances1_0.x;

#line 1188
    }

#line 1188
    float _S416;
    if((_S413.z) >= 0.0)
    {

#line 1189
        _S416 = distances1_0.y;

#line 1189
    }
    else
    {

#line 1189
        _S416 = distances1_0.z;

#line 1189
    }

#line 1189
    float3 _S417 = float3(kernelContext_54->cyFrameView_0->frame_0->probeVolumeParams_0.z) ;

    float3 _S418 = saturate((float3(_S414, _S415, _S416) + _S417 - abs(_S413)) / _S417);
    return _S412 * min(_S418.x, min(_S418.y, _S418.z));
}



float3 probeVolumeAmbient_0(float3 position_2, float3 normal_13, float3 flat_0, KernelContext_0 thread* kernelContext_55)
{
    uint3 _S419 = kernelContext_55->cyFrameView_0->frame_0->probeVolumeControl_0.yzw;

    float3 _S420 = position_2 + normal_13 * float3(kernelContext_55->cyFrameView_0->frame_0->probeVolumeParams_0.y) ;

#line 1201
    float3 _S421 = float3(kernelContext_55->cyFrameView_0->frame_0->probeVolumeOrigin_0.w) ;
    float3 _S422 = (_S420 - kernelContext_55->cyFrameView_0->frame_0->probeVolumeOrigin_0.xyz) / _S421;

#line 1202
    float3 _S423 = float3(1.0) ;
    float3 _S424 = float3(_S419) - _S423;
    float3 _S425 = float3(0.0) ;

#line 1204
    float3 _S426 = max(max(- _S422, _S422 - _S424), _S425);
    float _S427 = saturate(1.0 - max(_S426.x, max(_S426.y, _S426.z)));
    if(_S427 <= 0.0)
    {
        return flat_0;
    }
    float3 _S428 = clamp(_S422, _S425, _S424);
    uint3 _S429 = min(uint3(floor(_S428)), uint3(max(int3(_S419) - int3(int(2)) , int3(int(0)) )));
    float3 _S430 = _S428 - float3(_S429);


    float4 _S431 = float4(0.28209498524665833, 0.32573533058166504 * normal_13.y, 0.32573533058166504 * normal_13.z, 0.32573533058166504 * normal_13.x);

#line 1215
    uint corner_1 = 0U;

#line 1215
    float3 total_0 = _S425;

#line 1215
    float weightSum_0 = 0.0;



    for(;;)
    {

#line 1219
        if(corner_1 < 8U)
        {
        }
        else
        {

#line 1219
            break;
        }
        uint3 _S432 = uint3(corner_1 & 1U, (corner_1 >> 1U) & 1U, (corner_1 >> 2U) & 1U);
        uint3 _S433 = min(_S429 + _S432, _S419 - uint3(1U) );
        float3 _S434 = float3(_S432);
        float3 _S435 = _S434 * _S430 + (_S423 - _S434) * (_S423 - _S430);

#line 1224
        float4 _S436 = probeVolumeTexel_0(_S433, 3U, kernelContext_55);

        float _S437 = _S435.x * _S435.y * _S435.z * _S436.x;
        if(_S437 <= 0.0)
        {
            corner_1 = corner_1 + 1U;

#line 1219
            continue;
        }

#line 1219
        float4 _S438 = probeVolumeTexel_0(_S433, 4U, kernelContext_55);

#line 1219
        float _S439 = probeVisibility_0(_S436, _S438, kernelContext_55->cyFrameView_0->frame_0->probeVolumeOrigin_0.xyz + float3(_S433) * _S421, position_2, normal_13, _S420, kernelContext_55);

#line 1234
        float _S440 = _S437 * _S439;

#line 1234
        float4 _S441 = probeVolumeTexel_0(_S433, 0U, kernelContext_55);
        float _S442 = dot(_S441, _S431);

#line 1235
        float4 _S443 = probeVolumeTexel_0(_S433, 1U, kernelContext_55);
        float _S444 = dot(_S443, _S431);

#line 1236
        float4 _S445 = probeVolumeTexel_0(_S433, 2U, kernelContext_55);



        float weightSum_1 = weightSum_0 + _S440;

#line 1240
        total_0 = total_0 + max(float3(_S442, _S444, dot(_S445, _S431)) * float3(kernelContext_55->cyFrameView_0->frame_0->probeVolumeParams_0.x) , _S425) * float3(_S440) ;

#line 1240
        weightSum_0 = weightSum_1;

#line 1219
        corner_1 = corner_1 + 1U;

#line 1219
    }

#line 1242
    if(weightSum_0 > 9.99999997475242708e-07)
    {

#line 1242
        total_0 = total_0 / float3(weightSum_0) ;

#line 1242
    }
    else
    {

#line 1242
        total_0 = flat_0;

#line 1242
    }
    return mix(flat_0, total_0, float3(_S427) );
}


#line 1097
float3 frameGiSample_0(uint slot_6, float2 fragmentCentre_8, KernelContext_0 thread* kernelContext_56)
{

#line 1097
)cy_msl",
    R"cy_msl(    float4 _S446 = cyMaterialSampleTextureLevel_0(slot_6, fragmentCentre_8 * kernelContext_56->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_56);

    return _S446.xyz;
}


#line 1108
float3 frameGiAmbient_0(bool lightmapped_4, float2 fragmentCentre_9, float3 relativePosition_14, float3 normal_14, float3 ambient_0, KernelContext_0 thread* kernelContext_57)
{

    uint _S447 = kernelContext_57->cyFrameView_0->frame_0->giControl_0.z;

#line 1111
    float3 _S448 = frameGiSample_0(kernelContext_57->cyFrameView_0->frame_0->giControl_0.x, fragmentCentre_9, kernelContext_57);

#line 1111
    float3 _S449;

    if(lightmapped_4)
    {
        if((_S447 & 2U) != 0U)
        {

#line 1115
            _S449 = _S448;

#line 1115
        }
        else
        {

#line 1115
            _S449 = ambient_0;

#line 1115
        }

#line 1115
        return _S449;
    }
    if((kernelContext_57->cyFrameView_0->frame_0->probeVolumeControl_0.x) != 4294967295U)
    {
        if((_S447 & 4U) != 0U)
        {
            return _S448;
        }
        if((_S447 & 1U) != 0U)
        {

#line 1123
            float3 _S450 = probeVolumeAmbient_0(relativePosition_14, normal_14, _S448, kernelContext_57);

#line 1123
            _S449 = _S450;

#line 1123
        }
        else
        {

#line 1123
            _S449 = ambient_0;

#line 1123
        }

#line 1123
        return _S449;
    }

    if((_S447 & 1U) != 0U)
    {

#line 1126
        _S449 = _S448;

#line 1126
    }
    else
    {

#line 1126
        _S449 = ambient_0;

#line 1126
    }

#line 1126
    return _S449;
}



bool frameGiAdmits_0(bool lightmapped_5, KernelContext_0 thread* kernelContext_58)
{
    uint _S451 = kernelContext_58->cyFrameView_0->frame_0->giControl_0.z;
    if(lightmapped_5)
    {
        return (_S451 & 2U) != 0U;
    }
    return (_S451 & 5U) != 0U;
}




float3 frameEnvironmentBrdf_0(float3 f0_2, float roughness_7, float normalDotView_1)
{


    float4 _S452 = float4(roughness_7)  * float4(-1.0, -0.02749999985098839, -0.57200002670288086, 0.02199999988079071) + float4(1.0, 0.04250000044703484, 1.03999996185302734, -0.03999999910593033);
    float _S453 = _S452.x;
    float2 _S454 = float2(-1.03999996185302734, 1.03999996185302734) * float2((min(_S453 * _S453, exp2(-9.27999973297119141 * normalDotView_1)) * _S453 + _S452.y))  + _S452.zw;
    return f0_2 * float3(_S454.x)  + float3(_S454.y) ;
}


#line 1084
float occlusionVisibility_0(float2 fragmentCentre_10, KernelContext_0 thread* kernelContext_59)
{

#line 1084
    float4 _S455 = cyMaterialSampleTextureLevel_0(kernelContext_59->cyFrameView_0->frame_0->occlusionControl_0.x, fragmentCentre_10 * kernelContext_59->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_59);


    return _S455.w;
}


#line 641
struct CyForwardVertex_0
{
    float4 position_3;
    float3 relativePosition_15;
    float3 normal_15;
    float2 uv_18;
    [[flat]] uint drawIndex_0;
    float2 lightmapUv_0;
};


#line 1502
float4 cyShadeForward_0(const Surface_0 thread* resolved_0, const CyForwardVertex_0 thread* input_0, KernelContext_0 thread* kernelContext_60)
{
    CyDrawInstance_0 _S456 = kernelContext_60->cyFrameView_0->drawInstances_0[input_0->drawIndex_0];
    thread Surface_0 surface_9 = *resolved_0;

#line 1505
    bool _S457;


    if((_S456.giAddress_0) != 0U)
    {

#line 1508
        _S457 = (kernelContext_60->cyFrameView_0->frame_0->lightmapControl_0.w) != 0U;

#line 1508
    }
    else
    {

#line 1508
        _S457 = false;

#line 1508
    }

#line 1508
    float2 _S458;
    if(_S457)
    {

#line 1509
        float2 _S459 = lightmapCoordinate_0(_S456.giAddress_0, input_0->lightmapUv_0, kernelContext_60);

#line 1509
        _S458 = _S459;

#line 1509
    }
    else
    {

#line 1509
        _S458 = float2(0.0) ;

#line 1509
    }

    if((kernelContext_60->cyFrameView_0->frame_0->lightmapDebug_0.x) == 1U)
    {

#line 1511
        float3 _S460 = lightmapDensityColour_0(_S457, _S458, input_0->relativePosition_15, kernelContext_60);

        return float4(_S460, 1.0);
    }
    if((kernelContext_60->cyFrameView_0->frame_0->lightmapDebug_0.x) == 2U)
    {

#line 1515
        float3 _S461 = lightmapChartColour_0(_S457, _S456.giAddress_0, _S458, kernelContext_60);

        return float4(_S461, 1.0);
    }
    if((kernelContext_60->cyFrameView_0->frame_0->lightmapDebug_0.x) == 3U)
    {

#line 1519
        float4 _S462 = cyMaterialSampleTextureLevel_0(kernelContext_60->cyFrameView_0->frame_0->lightmapDebug_0.z, input_0->position_3.xy * kernelContext_60->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_60);

#line 1525
        if((_S462.w) > 0.0)
        {
            float3 _S463 = _S462.xyz;

#line 1527
            float _S464 = frameExposureStops_0(kernelContext_60);

#line 1527
            return float4(_S463 * float3(exp2(- _S464)) , 1.0);
        }

#line 1519
    }

#line 1531
    thread float3 normal_16 = normalize(input_0->normal_15);



    if((kernelContext_60->cyFrameView_0->frame_0->decalControl_0.x) != 4294967295U)
    {

#line 1535
        applyFrameDecals_0(&surface_9, &normal_16, input_0->relativePosition_15, input_0->position_3.xy, _S456.layerMask_1, kernelContext_60);

#line 1535
    }

#line 1535
    float3 _S465 = input_0->relativePosition_15;

#line 1542
    float3 _S466 = normalize(- input_0->relativePosition_15);

#line 1542
    CyBakedLights_0 _S467 = bakedLightsOf_0(_S457, _S458, kernelContext_60);

#line 1549
    float2 _S468 = input_0->position_3.xy;

#line 1549
    thread Surface_0 _S469 = surface_9;

#line 1549
    thread CyBakedLights_0 _S470 = _S467;

#line 1549
    float3 _S471 = accumulateLights_0(&_S469, _S465, normal_16, _S466, _S468, _S456.flags_0, &_S470, kernelContext_60);

#line 1563
    float3 ambientRadiance_0 = kernelContext_60->cyFrameView_0->frame_0->ambientAndOcclusion_0.xyz;

#line 1563
    float3 ambientRadiance_1;
    if(_S457)
    {

#line 1564
        float3 _S472 = lightmapAmbient_0(_S458, normal_16, kernelContext_60);

#line 1564
        ambientRadiance_1 = _S472;

#line 1564
    }
    else
    {

        if((kernelContext_60->cyFrameView_0->frame_0->probeVolumeControl_0.x) != 4294967295U)
        {

#line 1568
            float3 _S473 = probeVolumeAmbient_0(_S465, normal_16, ambientRadiance_0, kernelContext_60);

#line 1568
            ambientRadiance_1 = _S473;

#line 1568
        }
        else
        {

#line 1568
            ambientRadiance_1 = ambientRadiance_0;

#line 1568
        }

#line 1564
    }

#line 1575
    float3 _S474 = float3(0.0) ;
    bool _S475 = (kernelContext_60->cyFrameView_0->frame_0->giControl_0.x) != 4294967295U;

#line 1576
    float3 indirectSpecular_0;
    if(_S475)
    {

#line 1577
        float3 _S476 = frameGiAmbient_0(_S457, _S468, _S465, normal_16, ambientRadiance_1, kernelContext_60);



        if((kernelContext_60->cyFrameView_0->frame_0->giControl_0.y) != 4294967295U)
        {

#line 1581
            bool _S477 = frameGiAdmits_0(_S457, kernelContext_60);

#line 1581
            _S457 = _S477;

#line 1581
        }
        else
        {

#line 1581
            _S457 = false;

#line 1581
        }

#line 1581
        if(_S457)
        {
            float3 _S478 = frameEnvironmentBrdf_0(computeF0_0((&surface_9)->albedo_1, (&surface_9)->metallic_1), (&surface_9)->roughness_1, saturate(dot(normal_16, _S466)));

#line 1583
            float3 _S479 = frameGiSample_0(kernelContext_60->cyFrameView_0->frame_0->giControl_0.y, _S468, kernelContext_60);

#line 1583
            indirectSpecular_0 = _S479 * _S478 * float3((&surface_9)->occlusion_0) ;

#line 1581
        }
        else
        {

#line 1581
            indirectSpecular_0 = _S474;

#line 1581
        }

#line 1581
        ambientRadiance_1 = _S476;

#line 1577
    }
    else
    {

#line 1577
        indirectSpecular_0 = _S474;

#line 1577
    }

#line 1590
    float3 ambient_1 = (&surface_9)->albedo_1 * ambientRadiance_1 * float3((&surface_9)->occlusion_0) ;

#line 1590
    float3 color_1;

#line 1590
    float3 ambient_2;
    if((kernelContext_60->cyFrameView_0->frame_0->occlusionControl_0.x) != 4294967295U)
    {

#line 1591
        float _S480 = occlusionVisibility_0(_S468, kernelContext_60);

#line 1591
        float3 _S481 = float3(_S480) ;


        float3 ambient_3 = ambient_1 * _S481;

        float3 indirectSpecular_1 = indirectSpecular_0 * _S481;
        if((kernelContext_60->cyFrameView_0->frame_0->occlusionControl_0.y) != 0U)
        {

#line 1597
            color_1 = _S471 * float3(mix(1.0, _S480, (as_type<float>((kernelContext_60->cyFrameView_0->frame_0->occlusionControl_0.z))))) ;

#line 1597
        }
        else
        {

#line 1597
            color_1 = _S471;

#line 1597
        }

#line 1597
        ambient_2 = ambient_3;

#line 1597
        indirectSpecular_0 = indirectSpecular_1;

#line 1591
    }
    else
    {

#line 1591
        color_1 = _S471;

#line 1591
        ambient_2 = ambient_1;

#line 1591
    }

#line 1603
    float3 color_2 = color_1 + (&surface_9)->emission_1 + ambient_2;
    if(_S475)
    {

#line 1604
        color_1 = color_2 + indirectSpecular_0;

#line 1604
    }
    else
    {

#line 1604
        color_1 = color_2;

#line 1604
    }

#line 1611
    if((kernelContext_60->cyFrameView_0->frame_0->volumetricFogControl_0.x) != 4294967295U)
    {
        thread CyFrameFogVolume_0 fog_0;
        (&fog_0)->slot_0 = kernelContext_60->cyFrameView_0->frame_0->volumetricFogControl_0.x;

#line 1614
        thread CyFrameFogVolume_0 _S482 = fog_0;

#line 1614
        float3 _S483 = cyApplyFog_0(&_S482, color_1, _S465, kernelContext_60);

#line 1614
        color_1 = _S483;

#line 1611
    }

#line 1617
    return float4(color_1, (&surface_9)->opacity_0);
}


#line 1617
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 1617
struct pixelInput_0
{
    float3 relativePosition_16 [[user(TEXCOORD)]];
    float3 normal_17 [[user(TEXCOORD_1)]];
    float2 uv_19 [[user(TEXCOORD_2)]];
    [[flat]] uint drawIndex_1 [[user(TEXCOORD_3)]];
    float2 lightmapUv_1 [[user(TEXCOORD_4)]];
};


#line 1621
[[fragment]] pixelOutput_0 cyForwardFragment(pixelInput_0 _S484 [[stage_in]], float4 position_4 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyFrameGlobalSet_default_0 constant& cyFrameGlobals_1 [[buffer(0)]])
{

#line 1621
    thread KernelContext_0 kernelContext_61;

#line 1621
    (&kernelContext_61)->cyFrameView_0 = cyFrameView_1;

#line 1621
    (&kernelContext_61)->cyFrameGlobals_0 = &cyFrameGlobals_1;

    CyDrawInstance_0 _S485 = cyFrameView_1->drawInstances_0[_S484.drawIndex_1];

#line 1623
    Surface_0 _S486 = surfaceOf_0(_S485.material_0, cyFrameView_1->instances_0[_S485.instanceSlot_0].tint_0.xyz, _S484.uv_19, &kernelContext_61);

#line 1623
    thread Surface_0 _S487 = _S486;

#line 1623
    thread CyForwardVertex_0 _S488;

#line 1623
    (&_S488)->position_3 = position_4;

#line 1623
    (&_S488)->relativePosition_15 = _S484.relativePosition_16;

#line 1623
    (&_S488)->normal_15 = _S484.normal_17;

#line 1623
    (&_S488)->uv_18 = _S484.uv_19;

#line 1623
    (&_S488)->drawIndex_0 = _S484.drawIndex_1;

#line 1623
    (&_S488)->lightmapUv_0 = _S484.lightmapUv_1;

#line 1623
    float4 _S489 = cyShadeForward_0(&_S487, &_S488, &kernelContext_61);

#line 1623
    pixelOutput_0 _S490 = { _S489 };

    return _S490;
}

)cy_msl");
inline constexpr const auto& kFrameForwardFragmentMsl = kFrameForwardFragmentMslText.text;

/// SkinnedDepthVertex.metal, 8339 bytes.
inline constexpr char kFrameSkinnedDepthVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 363 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 443
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


#line 115 "src/rendering/shaders/cy/frame.slang"
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
    uint4 volumetricFogControl_0;
    uint4 motionControl_0;
    uint4 lightmapControl_0;
    uint4 lightmapLayout_0;
    uint4 lightmapShadowLights_0;
    uint4 lightmapDirectLights_0;
    uint4 lightmapDebug_0;
    uint4 giControl_0;
    uint4 lightingControl_0;
    uint4 directionalLights_0;
    float4 viewmodelToClipRow0_0;
    float4 viewmodelToClipRow1_0;
    float4 viewmodelToClipRow2_0;
    float4 viewmodelToClipRow3_0;
    float4 previousViewmodelToClipRow0_0;
    float4 previousViewmodelToClipRow1_0;
    float4 previousViewmodelToClipRow2_0;
    float4 previousViewmodelToClipRow3_0;
    uint4 viewmodelControl_0;
};


#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 350 "src/rendering/shaders/cy/frame.slang"
struct CyDrawInstance_0
{
    uint instanceSlot_0;
    uint material_0;
    uint parameterOffset_0;
    uint giAddress_0;
    uint layerMask_1;
    uint lodAndFade_0;
    uint surface_0;
    uint flags_0;
};


#line 372
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


#line 417
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


#line 427 "src/rendering/shaders/cy/frame.slang"
float4 transformToClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow3_0, _S2));
}


#line 587
float3 previousRelativeOf_0(uint slot_0, const CyInstanceTransform_0 thread* current_0, float3 previousModelPosition_0, KernelContext_0 thread* kernelContext_1)
{
    uint _S3 = kernelContext_1->cyFrameView_0->frame_0->motionControl_0.x;

#line 589
    CyInstanceTransform_0 _S4;
    if(_S3 == 4294967295U)
    {

#line 590
        _S4 = *current_0;

#line 590
    }
    else
    {

#line 590
        _S4 = kernelContext_1->cyFrameView_0->instances_0[_S3 + slot_0];

#line 590
    }

#line 590
    thread CyInstanceTransform_0 _S5 = _S4;

#line 590
    float3 _S6 = transformToRelative_0(&_S5, previousModelPosition_0);
    return _S6;
}


#line 1651
float2 cySkinnedNormalToStream_0(float4 frame_1)
{

#line 1651
    float2 _S7 = float2(0.5) ;

    return frame_1.xy * _S7 + _S7;
}


#line 23 "src/rendering/shaders/cy/packing.slang"
float3 decodeOctahedral_0(float2 encoded_0)
{
    float2 _S8 = encoded_0 * float2(2.0)  - float2(1.0) ;
    float _S9 = _S8.x;

#line 26
    float _S10 = _S8.y;

#line 26
    float _S11 = 1.0 - abs(_S9) - abs(_S10);

#line 26
    thread float3 normal_0 = float3(_S9, _S10, _S11);
    float _S12 = saturate(- _S11);
    float2 _S13 = float2(_S9, _S10);

#line 28
    normal_0.xy = _S13 + select(float2(_S12) , float2(- _S12) , _S13 >= (float2(0.0) ));
    return normalize(normal_0);
}


#line 452 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 454
struct cySkinnedDepthVertex_Result_0
{
    float4 position_0 [[position]];
    float3 normal_1 [[user(TEXCOORD)]];
    float4 currentClip_0 [[user(TEXCOORD_1)]];
    float4 previousClip_0 [[user(TEXCOORD_2)]];
};


#line 454
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 frame_2 [[attribute(1)]];
    float3 previousModelPosition_1 [[attribute(2)]];
};


#line 570
struct CyDepthVertex_0
{
    float4 position_1;
    float3 normal_2;
    float4 currentClip_1;
    float4 previousClip_1;
};


#line 570
[[vertex]] cySkinnedDepthVertex_Result_0 cySkinnedDepthVertex(vertexInput_0 _S14 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 570
    thread KernelContext_0 kernelContext_2;

#line 570
    (&kernelContext_2)->cyFrameView_0 = cyFrameView_1;

#line 570
    (&kernelContext_2)->cyDraw_0 = cyDraw_1;

#line 1660
    CyDrawInstance_0 _S15 = cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0];
    CyInstanceTransform_0 _S16 = cyFrameView_1->instances_0[_S15.instanceSlot_0];

#line 1661
    thread CyInstanceTransform_0 _S17 = _S16;

#line 1661
    float3 _S18 = transformToRelative_0(&_S17, _S14.modelPosition_1);
    thread CyDepthVertex_0 output_0;

#line 1662
    float4 _S19 = transformToClip_0(_S18, &kernelContext_2);

    (&output_0)->position_1 = _S19;
    (&output_0)->currentClip_1 = _S19;

#line 1665
    thread CyInstanceTransform_0 _S20 = _S16;

#line 1665
    float3 _S21 = previousRelativeOf_0(_S15.instanceSlot_0, &_S20, _S14.previousModelPosition_1, &kernelContext_2);
    float4 _S22 = float4(_S21, 1.0);
    (&output_0)->previousClip_1 = float4(dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow0_0, _S22), dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow1_0, _S22), dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow2_0, _S22), dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow3_0, _S22));



    float3 _S23 = decodeOctahedral_0(cySkinnedNormalToStream_0(_S14.frame_2));

#line 1671
    thread CyInstanceTransform_0 _S24 = _S16;

#line 1671
    float3 _S25 = rotateToRelative_0(&_S24, _S23);

#line 1671
    (&output_0)->normal_2 = _S25;

#line 1671
    thread cySkinnedDepthVertex_Result_0 _S26;

#line 1671
    (&_S26)->position_0 = output_0.position_1;

#line 1671
    (&_S26)->normal_1 = output_0.normal_2;

#line 1671
    (&_S26)->currentClip_0 = output_0.currentClip_1;

#line 1671
    (&_S26)->previousClip_0 = output_0.previousClip_1;

#line 1671
    return _S26;
}

)cy_msl";

/// SkinnedForwardVertex.metal, 7463 bytes.
inline constexpr char kFrameSkinnedForwardVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 363 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 443
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


#line 115 "src/rendering/shaders/cy/frame.slang"
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
    uint4 volumetricFogControl_0;
    uint4 motionControl_0;
    uint4 lightmapControl_0;
    uint4 lightmapLayout_0;
    uint4 lightmapShadowLights_0;
    uint4 lightmapDirectLights_0;
    uint4 lightmapDebug_0;
    uint4 giControl_0;
    uint4 lightingControl_0;
    uint4 directionalLights_0;
    float4 viewmodelToClipRow0_0;
    float4 viewmodelToClipRow1_0;
    float4 viewmodelToClipRow2_0;
    float4 viewmodelToClipRow3_0;
    float4 previousViewmodelToClipRow0_0;
    float4 previousViewmodelToClipRow1_0;
    float4 previousViewmodelToClipRow2_0;
    float4 previousViewmodelToClipRow3_0;
    uint4 viewmodelControl_0;
};


#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 350 "src/rendering/shaders/cy/frame.slang"
struct CyDrawInstance_0
{
    uint instanceSlot_0;
    uint material_0;
    uint parameterOffset_0;
    uint giAddress_0;
    uint layerMask_1;
    uint lodAndFade_0;
    uint surface_0;
    uint flags_0;
};


#line 372
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


#line 417
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


#line 427 "src/rendering/shaders/cy/frame.slang"
float4 transformToClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow3_0, _S2));
}


#line 1651
float2 cySkinnedNormalToStream_0(float4 frame_1)
{

#line 1651
    float2 _S3 = float2(0.5) ;

    return frame_1.xy * _S3 + _S3;
}


#line 23 "src/rendering/shaders/cy/packing.slang"
float3 decodeOctahedral_0(float2 encoded_0)
{
    float2 _S4 = encoded_0 * float2(2.0)  - float2(1.0) ;
    float _S5 = _S4.x;

#line 26
    float _S6 = _S4.y;

#line 26
    float _S7 = 1.0 - abs(_S5) - abs(_S6);

#line 26
    thread float3 normal_0 = float3(_S5, _S6, _S7);
    float _S8 = saturate(- _S7);
    float2 _S9 = float2(_S5, _S6);

#line 28
    normal_0.xy = _S9 + select(float2(_S8) , float2(- _S8) , _S9 >= (float2(0.0) ));
    return normalize(normal_0);
}


#line 452 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 454
struct cySkinnedForwardVertex_Result_0
{
    float4 position_0 [[position]];
    float3 relativePosition_0 [[user(TEXCOORD)]];
    float3 normal_1 [[user(TEXCOORD_1)]];
    float2 uv_0 [[user(TEXCOORD_2)]];
    uint drawIndex_1 [[user(TEXCOORD_3)]];
    float2 lightmapUv_0 [[user(TEXCOORD_4)]];
};


#line 454
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 frame_2 [[attribute(1)]];
    float2 uv_1 [[attribute(2)]];
    float2 lightmapUv_1 [[attribute(3)]];
};


#line 641
struct CyForwardVertex_0
{
    float4 position_1;
    float3 relativePosition_1;
    float3 normal_2;
    float2 uv_2;
    [[flat]] uint drawIndex_2;
    float2 lightmapUv_2;
};


#line 641
[[vertex]] cySkinnedForwardVertex_Result_0 cySkinnedForwardVertex(vertexInput_0 _S10 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 641
    thread KernelContext_0 kernelContext_1;

#line 641
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 641
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 1680
    CyInstanceTransform_0 _S11 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

    thread CyForwardVertex_0 output_0;

#line 1682
    thread CyInstanceTransform_0 _S12 = _S11;

#line 1682
    float3 _S13 = transformToRelative_0(&_S12, _S10.modelPosition_1);
    (&output_0)->relativePosition_1 = _S13;

#line 1683
    float4 _S14 = transformToClip_0(_S13, &kernelContext_1);
    (&output_0)->position_1 = _S14;
    float3 _S15 = decodeOctahedral_0(cySkinnedNormalToStream_0(_S10.frame_2));

#line 1685
    thread CyInstanceTransform_0 _S16 = _S11;

#line 1685
    float3 _S17 = rotateToRelative_0(&_S16, _S15);

#line 1685
    (&output_0)->normal_2 = _S17;
    (&output_0)->uv_2 = _S10.uv_1;
    (&output_0)->drawIndex_2 = cyDraw_1->drawIndex_0;
    (&output_0)->lightmapUv_2 = _S10.lightmapUv_1;

#line 1688
    thread cySkinnedForwardVertex_Result_0 _S18;

#line 1688
    (&_S18)->position_0 = output_0.position_1;

#line 1688
    (&_S18)->relativePosition_0 = output_0.relativePosition_1;

#line 1688
    (&_S18)->normal_1 = output_0.normal_2;

#line 1688
    (&_S18)->uv_0 = output_0.uv_2;

#line 1688
    (&_S18)->drawIndex_1 = output_0.drawIndex_2;

#line 1688
    (&_S18)->lightmapUv_0 = output_0.lightmapUv_2;

#line 1688
    return _S18;
}

)cy_msl";

/// ViewmodelDepthVertex.metal, 8432 bytes.
inline constexpr char kFrameViewmodelDepthVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 363 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 443
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


#line 115 "src/rendering/shaders/cy/frame.slang"
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
    uint4 volumetricFogControl_0;
    uint4 motionControl_0;
    uint4 lightmapControl_0;
    uint4 lightmapLayout_0;
    uint4 lightmapShadowLights_0;
    uint4 lightmapDirectLights_0;
    uint4 lightmapDebug_0;
    uint4 giControl_0;
    uint4 lightingControl_0;
    uint4 directionalLights_0;
    float4 viewmodelToClipRow0_0;
    float4 viewmodelToClipRow1_0;
    float4 viewmodelToClipRow2_0;
    float4 viewmodelToClipRow3_0;
    float4 previousViewmodelToClipRow0_0;
    float4 previousViewmodelToClipRow1_0;
    float4 previousViewmodelToClipRow2_0;
    float4 previousViewmodelToClipRow3_0;
    uint4 viewmodelControl_0;
};


#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 350 "src/rendering/shaders/cy/frame.slang"
struct CyDrawInstance_0
{
    uint instanceSlot_0;
    uint material_0;
    uint parameterOffset_0;
    uint giAddress_0;
    uint layerMask_1;
    uint lodAndFade_0;
    uint surface_0;
    uint flags_0;
};


#line 372
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


#line 417
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


#line 1720 "src/rendering/shaders/cy/frame.slang"
float4 transformToViewmodelClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow3_0, _S2));
}


#line 587
float3 previousRelativeOf_0(uint slot_0, const CyInstanceTransform_0 thread* current_0, float3 previousModelPosition_0, KernelContext_0 thread* kernelContext_1)
{
    uint _S3 = kernelContext_1->cyFrameView_0->frame_0->motionControl_0.x;

#line 589
    CyInstanceTransform_0 _S4;
    if(_S3 == 4294967295U)
    {

#line 590
        _S4 = *current_0;

#line 590
    }
    else
    {

#line 590
        _S4 = kernelContext_1->cyFrameView_0->instances_0[_S3 + slot_0];

#line 590
    }

#line 590
    thread CyInstanceTransform_0 _S5 = _S4;

#line 590
    float3 _S6 = transformToRelative_0(&_S5, previousModelPosition_0);
    return _S6;
}


#line 1727
float4 transformToPreviousViewmodelClip_0(float3 relative_1, KernelContext_0 thread* kernelContext_2)
{
    float4 _S7 = float4(relative_1, 1.0);
    return float4(dot(kernelContext_2->cyFrameView_0->frame_0->previousViewmodelToClipRow0_0, _S7), dot(kernelContext_2->cyFrameView_0->frame_0->previousViewmodelToClipRow1_0, _S7), dot(kernelContext_2->cyFrameView_0->frame_0->previousViewmodelToClipRow2_0, _S7), dot(kernelContext_2->cyFrameView_0->frame_0->previousViewmodelToClipRow3_0, _S7));
}


#line 23 "src/rendering/shaders/cy/packing.slang"
float3 decodeOctahedral_0(float2 encoded_0)
{
    float2 _S8 = encoded_0 * float2(2.0)  - float2(1.0) ;
    float _S9 = _S8.x;

#line 26
    float _S10 = _S8.y;

#line 26
    float _S11 = 1.0 - abs(_S9) - abs(_S10);

#line 26
    thread float3 normal_0 = float3(_S9, _S10, _S11);
    float _S12 = saturate(- _S11);
    float2 _S13 = float2(_S9, _S10);

#line 28
    normal_0.xy = _S13 + select(float2(_S12) , float2(- _S12) , _S13 >= (float2(0.0) ));
    return normalize(normal_0);
}


#line 452 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 454
struct cyViewmodelDepthVertex_Result_0
{
    float4 position_0 [[position]];
    float3 normal_1 [[user(TEXCOORD)]];
    float4 currentClip_0 [[user(TEXCOORD_1)]];
    float4 previousClip_0 [[user(TEXCOORD_2)]];
};


#line 454
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
    float3 previousModelPosition_1 [[attribute(2)]];
};


#line 570
struct CyDepthVertex_0
{
    float4 position_1;
    float3 normal_2;
    float4 currentClip_1;
    float4 previousClip_1;
};


#line 570
[[vertex]] cyViewmodelDepthVertex_Result_0 cyViewmodelDepthVertex(vertexInput_0 _S14 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 570
    thread KernelContext_0 kernelContext_3;

#line 570
    (&kernelContext_3)->cyFrameView_0 = cyFrameView_1;

#line 570
    (&kernelContext_3)->cyDraw_0 = cyDraw_1;

#line 1741
    CyDrawInstance_0 _S15 = cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0];
    CyInstanceTransform_0 _S16 = cyFrameView_1->instances_0[_S15.instanceSlot_0];

#line 1742
    thread CyInstanceTransform_0 _S17 = _S16;

#line 1742
    float3 _S18 = transformToRelative_0(&_S17, _S14.modelPosition_1);
    thread CyDepthVertex_0 output_0;

#line 1743
    float4 _S19 = transformToViewmodelClip_0(_S18, &kernelContext_3);

    (&output_0)->position_1 = _S19;
    (&output_0)->currentClip_1 = _S19;

#line 1746
    thread CyInstanceTransform_0 _S20 = _S16;

#line 1746
    float3 _S21 = previousRelativeOf_0(_S15.instanceSlot_0, &_S20, _S14.previousModelPosition_1, &kernelContext_3);

#line 1746
    float4 _S22 = transformToPreviousViewmodelClip_0(_S21, &kernelContext_3);
    (&output_0)->previousClip_1 = _S22;

    float3 _S23 = decodeOctahedral_0(_S14.packedNormal_0.xy);

#line 1749
    thread CyInstanceTransform_0 _S24 = _S16;

#line 1749
    float3 _S25 = rotateToRelative_0(&_S24, _S23);

#line 1749
    (&output_0)->normal_2 = _S25;

#line 1749
    thread cyViewmodelDepthVertex_Result_0 _S26;

#line 1749
    (&_S26)->position_0 = output_0.position_1;

#line 1749
    (&_S26)->normal_1 = output_0.normal_2;

#line 1749
    (&_S26)->currentClip_0 = output_0.currentClip_1;

#line 1749
    (&_S26)->previousClip_0 = output_0.previousClip_1;

#line 1749
    return _S26;
}

)cy_msl";

/// ViewmodelForwardVertex.metal, 7334 bytes.
inline constexpr char kFrameViewmodelForwardVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 363 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 443
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


#line 115 "src/rendering/shaders/cy/frame.slang"
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
    uint4 volumetricFogControl_0;
    uint4 motionControl_0;
    uint4 lightmapControl_0;
    uint4 lightmapLayout_0;
    uint4 lightmapShadowLights_0;
    uint4 lightmapDirectLights_0;
    uint4 lightmapDebug_0;
    uint4 giControl_0;
    uint4 lightingControl_0;
    uint4 directionalLights_0;
    float4 viewmodelToClipRow0_0;
    float4 viewmodelToClipRow1_0;
    float4 viewmodelToClipRow2_0;
    float4 viewmodelToClipRow3_0;
    float4 previousViewmodelToClipRow0_0;
    float4 previousViewmodelToClipRow1_0;
    float4 previousViewmodelToClipRow2_0;
    float4 previousViewmodelToClipRow3_0;
    uint4 viewmodelControl_0;
};


#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 350 "src/rendering/shaders/cy/frame.slang"
struct CyDrawInstance_0
{
    uint instanceSlot_0;
    uint material_0;
    uint parameterOffset_0;
    uint giAddress_0;
    uint layerMask_1;
    uint lodAndFade_0;
    uint surface_0;
    uint flags_0;
};


#line 372
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


#line 417
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


#line 1720 "src/rendering/shaders/cy/frame.slang"
float4 transformToViewmodelClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow3_0, _S2));
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


#line 452 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 454
struct cyViewmodelForwardVertex_Result_0
{
    float4 position_0 [[position]];
    float3 relativePosition_0 [[user(TEXCOORD)]];
    float3 normal_1 [[user(TEXCOORD_1)]];
    float2 uv_0 [[user(TEXCOORD_2)]];
    uint drawIndex_1 [[user(TEXCOORD_3)]];
    float2 lightmapUv_0 [[user(TEXCOORD_4)]];
};


#line 454
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
    float2 uv_1 [[attribute(2)]];
    float2 lightmapUv_1 [[attribute(3)]];
};


#line 641
struct CyForwardVertex_0
{
    float4 position_1;
    float3 relativePosition_1;
    float3 normal_2;
    float2 uv_2;
    [[flat]] uint drawIndex_2;
    float2 lightmapUv_2;
};


#line 641
[[vertex]] cyViewmodelForwardVertex_Result_0 cyViewmodelForwardVertex(vertexInput_0 _S9 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 641
    thread KernelContext_0 kernelContext_1;

#line 641
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 641
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 1760
    CyInstanceTransform_0 _S10 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

    thread CyForwardVertex_0 output_0;

#line 1762
    thread CyInstanceTransform_0 _S11 = _S10;

#line 1762
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    (&output_0)->relativePosition_1 = _S12;

#line 1763
    float4 _S13 = transformToViewmodelClip_0(_S12, &kernelContext_1);
    (&output_0)->position_1 = _S13;
    float3 _S14 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 1765
    thread CyInstanceTransform_0 _S15 = _S10;

#line 1765
    float3 _S16 = rotateToRelative_0(&_S15, _S14);

#line 1765
    (&output_0)->normal_2 = _S16;
    (&output_0)->uv_2 = _S9.uv_1;
    (&output_0)->drawIndex_2 = cyDraw_1->drawIndex_0;
    (&output_0)->lightmapUv_2 = _S9.lightmapUv_1;

#line 1768
    thread cyViewmodelForwardVertex_Result_0 _S17;

#line 1768
    (&_S17)->position_0 = output_0.position_1;

#line 1768
    (&_S17)->relativePosition_0 = output_0.relativePosition_1;

#line 1768
    (&_S17)->normal_1 = output_0.normal_2;

#line 1768
    (&_S17)->uv_0 = output_0.uv_2;

#line 1768
    (&_S17)->drawIndex_1 = output_0.drawIndex_2;

#line 1768
    (&_S17)->lightmapUv_0 = output_0.lightmapUv_2;

#line 1768
    return _S17;
}

)cy_msl";

/// SkinnedViewmodelDepthVertex.metal, 8619 bytes.
inline constexpr char kFrameSkinnedViewmodelDepthVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 363 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 443
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


#line 115 "src/rendering/shaders/cy/frame.slang"
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
    uint4 volumetricFogControl_0;
    uint4 motionControl_0;
    uint4 lightmapControl_0;
    uint4 lightmapLayout_0;
    uint4 lightmapShadowLights_0;
    uint4 lightmapDirectLights_0;
    uint4 lightmapDebug_0;
    uint4 giControl_0;
    uint4 lightingControl_0;
    uint4 directionalLights_0;
    float4 viewmodelToClipRow0_0;
    float4 viewmodelToClipRow1_0;
    float4 viewmodelToClipRow2_0;
    float4 viewmodelToClipRow3_0;
    float4 previousViewmodelToClipRow0_0;
    float4 previousViewmodelToClipRow1_0;
    float4 previousViewmodelToClipRow2_0;
    float4 previousViewmodelToClipRow3_0;
    uint4 viewmodelControl_0;
};


#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 350 "src/rendering/shaders/cy/frame.slang"
struct CyDrawInstance_0
{
    uint instanceSlot_0;
    uint material_0;
    uint parameterOffset_0;
    uint giAddress_0;
    uint layerMask_1;
    uint lodAndFade_0;
    uint surface_0;
    uint flags_0;
};


#line 372
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


#line 417
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


#line 1720 "src/rendering/shaders/cy/frame.slang"
float4 transformToViewmodelClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow3_0, _S2));
}


#line 587
float3 previousRelativeOf_0(uint slot_0, const CyInstanceTransform_0 thread* current_0, float3 previousModelPosition_0, KernelContext_0 thread* kernelContext_1)
{
    uint _S3 = kernelContext_1->cyFrameView_0->frame_0->motionControl_0.x;

#line 589
    CyInstanceTransform_0 _S4;
    if(_S3 == 4294967295U)
    {

#line 590
        _S4 = *current_0;

#line 590
    }
    else
    {

#line 590
        _S4 = kernelContext_1->cyFrameView_0->instances_0[_S3 + slot_0];

#line 590
    }

#line 590
    thread CyInstanceTransform_0 _S5 = _S4;

#line 590
    float3 _S6 = transformToRelative_0(&_S5, previousModelPosition_0);
    return _S6;
}


#line 1727
float4 transformToPreviousViewmodelClip_0(float3 relative_1, KernelContext_0 thread* kernelContext_2)
{
    float4 _S7 = float4(relative_1, 1.0);
    return float4(dot(kernelContext_2->cyFrameView_0->frame_0->previousViewmodelToClipRow0_0, _S7), dot(kernelContext_2->cyFrameView_0->frame_0->previousViewmodelToClipRow1_0, _S7), dot(kernelContext_2->cyFrameView_0->frame_0->previousViewmodelToClipRow2_0, _S7), dot(kernelContext_2->cyFrameView_0->frame_0->previousViewmodelToClipRow3_0, _S7));
}


#line 1651
float2 cySkinnedNormalToStream_0(float4 frame_1)
{

#line 1651
    float2 _S8 = float2(0.5) ;

    return frame_1.xy * _S8 + _S8;
}


#line 23 "src/rendering/shaders/cy/packing.slang"
float3 decodeOctahedral_0(float2 encoded_0)
{
    float2 _S9 = encoded_0 * float2(2.0)  - float2(1.0) ;
    float _S10 = _S9.x;

#line 26
    float _S11 = _S9.y;

#line 26
    float _S12 = 1.0 - abs(_S10) - abs(_S11);

#line 26
    thread float3 normal_0 = float3(_S10, _S11, _S12);
    float _S13 = saturate(- _S12);
    float2 _S14 = float2(_S10, _S11);

#line 28
    normal_0.xy = _S14 + select(float2(_S13) , float2(- _S13) , _S14 >= (float2(0.0) ));
    return normalize(normal_0);
}


#line 452 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 454
struct cySkinnedViewmodelDepthVertex_Result_0
{
    float4 position_0 [[position]];
    float3 normal_1 [[user(TEXCOORD)]];
    float4 currentClip_0 [[user(TEXCOORD_1)]];
    float4 previousClip_0 [[user(TEXCOORD_2)]];
};


#line 454
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 frame_2 [[attribute(1)]];
    float3 previousModelPosition_1 [[attribute(2)]];
};


#line 570
struct CyDepthVertex_0
{
    float4 position_1;
    float3 normal_2;
    float4 currentClip_1;
    float4 previousClip_1;
};


#line 570
[[vertex]] cySkinnedViewmodelDepthVertex_Result_0 cySkinnedViewmodelDepthVertex(vertexInput_0 _S15 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 570
    thread KernelContext_0 kernelContext_3;

#line 570
    (&kernelContext_3)->cyFrameView_0 = cyFrameView_1;

#line 570
    (&kernelContext_3)->cyDraw_0 = cyDraw_1;

#line 1777
    CyDrawInstance_0 _S16 = cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0];
    CyInstanceTransform_0 _S17 = cyFrameView_1->instances_0[_S16.instanceSlot_0];

#line 1778
    thread CyInstanceTransform_0 _S18 = _S17;

#line 1778
    float3 _S19 = transformToRelative_0(&_S18, _S15.modelPosition_1);
    thread CyDepthVertex_0 output_0;

#line 1779
    float4 _S20 = transformToViewmodelClip_0(_S19, &kernelContext_3);

    (&output_0)->position_1 = _S20;
    (&output_0)->currentClip_1 = _S20;

#line 1782
    thread CyInstanceTransform_0 _S21 = _S17;

#line 1782
    float3 _S22 = previousRelativeOf_0(_S16.instanceSlot_0, &_S21, _S15.previousModelPosition_1, &kernelContext_3);

#line 1782
    float4 _S23 = transformToPreviousViewmodelClip_0(_S22, &kernelContext_3);
    (&output_0)->previousClip_1 = _S23;

    float3 _S24 = decodeOctahedral_0(cySkinnedNormalToStream_0(_S15.frame_2));

#line 1785
    thread CyInstanceTransform_0 _S25 = _S17;

#line 1785
    float3 _S26 = rotateToRelative_0(&_S25, _S24);

#line 1785
    (&output_0)->normal_2 = _S26;

#line 1785
    thread cySkinnedViewmodelDepthVertex_Result_0 _S27;

#line 1785
    (&_S27)->position_0 = output_0.position_1;

#line 1785
    (&_S27)->normal_1 = output_0.normal_2;

#line 1785
    (&_S27)->currentClip_0 = output_0.currentClip_1;

#line 1785
    (&_S27)->previousClip_0 = output_0.previousClip_1;

#line 1785
    return _S27;
}

)cy_msl";

/// SkinnedViewmodelForwardVertex.metal, 7522 bytes.
inline constexpr char kFrameSkinnedViewmodelForwardVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 363 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 443
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


#line 115 "src/rendering/shaders/cy/frame.slang"
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
    uint4 volumetricFogControl_0;
    uint4 motionControl_0;
    uint4 lightmapControl_0;
    uint4 lightmapLayout_0;
    uint4 lightmapShadowLights_0;
    uint4 lightmapDirectLights_0;
    uint4 lightmapDebug_0;
    uint4 giControl_0;
    uint4 lightingControl_0;
    uint4 directionalLights_0;
    float4 viewmodelToClipRow0_0;
    float4 viewmodelToClipRow1_0;
    float4 viewmodelToClipRow2_0;
    float4 viewmodelToClipRow3_0;
    float4 previousViewmodelToClipRow0_0;
    float4 previousViewmodelToClipRow1_0;
    float4 previousViewmodelToClipRow2_0;
    float4 previousViewmodelToClipRow3_0;
    uint4 viewmodelControl_0;
};


#line 34 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    packed_float3 positionRelativeToCamera_0;
    float range_0;
    packed_float3 direction_0;
    float intensity_0;
    packed_float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    uint shapeWord_0;
    uint layerMask_0;
};


#line 350 "src/rendering/shaders/cy/frame.slang"
struct CyDrawInstance_0
{
    uint instanceSlot_0;
    uint material_0;
    uint parameterOffset_0;
    uint giAddress_0;
    uint layerMask_1;
    uint lodAndFade_0;
    uint surface_0;
    uint flags_0;
};


#line 372
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


#line 417
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


#line 1720 "src/rendering/shaders/cy/frame.slang"
float4 transformToViewmodelClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->viewmodelToClipRow3_0, _S2));
}


#line 1651
float2 cySkinnedNormalToStream_0(float4 frame_1)
{

#line 1651
    float2 _S3 = float2(0.5) ;

    return frame_1.xy * _S3 + _S3;
}


#line 23 "src/rendering/shaders/cy/packing.slang"
float3 decodeOctahedral_0(float2 encoded_0)
{
    float2 _S4 = encoded_0 * float2(2.0)  - float2(1.0) ;
    float _S5 = _S4.x;

#line 26
    float _S6 = _S4.y;

#line 26
    float _S7 = 1.0 - abs(_S5) - abs(_S6);

#line 26
    thread float3 normal_0 = float3(_S5, _S6, _S7);
    float _S8 = saturate(- _S7);
    float2 _S9 = float2(_S5, _S6);

#line 28
    normal_0.xy = _S9 + select(float2(_S8) , float2(- _S8) , _S9 >= (float2(0.0) ));
    return normalize(normal_0);
}


#line 452 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 454
struct cySkinnedViewmodelForwardVertex_Result_0
{
    float4 position_0 [[position]];
    float3 relativePosition_0 [[user(TEXCOORD)]];
    float3 normal_1 [[user(TEXCOORD_1)]];
    float2 uv_0 [[user(TEXCOORD_2)]];
    uint drawIndex_1 [[user(TEXCOORD_3)]];
    float2 lightmapUv_0 [[user(TEXCOORD_4)]];
};


#line 454
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 frame_2 [[attribute(1)]];
    float2 uv_1 [[attribute(2)]];
    float2 lightmapUv_1 [[attribute(3)]];
};


#line 641
struct CyForwardVertex_0
{
    float4 position_1;
    float3 relativePosition_1;
    float3 normal_2;
    float2 uv_2;
    [[flat]] uint drawIndex_2;
    float2 lightmapUv_2;
};


#line 641
[[vertex]] cySkinnedViewmodelForwardVertex_Result_0 cySkinnedViewmodelForwardVertex(vertexInput_0 _S10 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 641
    thread KernelContext_0 kernelContext_1;

#line 641
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 641
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 1796
    CyInstanceTransform_0 _S11 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

    thread CyForwardVertex_0 output_0;

#line 1798
    thread CyInstanceTransform_0 _S12 = _S11;

#line 1798
    float3 _S13 = transformToRelative_0(&_S12, _S10.modelPosition_1);
    (&output_0)->relativePosition_1 = _S13;

#line 1799
    float4 _S14 = transformToViewmodelClip_0(_S13, &kernelContext_1);
    (&output_0)->position_1 = _S14;
    float3 _S15 = decodeOctahedral_0(cySkinnedNormalToStream_0(_S10.frame_2));

#line 1801
    thread CyInstanceTransform_0 _S16 = _S11;

#line 1801
    float3 _S17 = rotateToRelative_0(&_S16, _S15);

#line 1801
    (&output_0)->normal_2 = _S17;
    (&output_0)->uv_2 = _S10.uv_1;
    (&output_0)->drawIndex_2 = cyDraw_1->drawIndex_0;
    (&output_0)->lightmapUv_2 = _S10.lightmapUv_1;

#line 1804
    thread cySkinnedViewmodelForwardVertex_Result_0 _S18;

#line 1804
    (&_S18)->position_0 = output_0.position_1;

#line 1804
    (&_S18)->relativePosition_0 = output_0.relativePosition_1;

#line 1804
    (&_S18)->normal_1 = output_0.normal_2;

#line 1804
    (&_S18)->uv_0 = output_0.uv_2;

#line 1804
    (&_S18)->drawIndex_1 = output_0.drawIndex_2;

#line 1804
    (&_S18)->lightmapUv_0 = output_0.lightmapUv_2;

#line 1804
    return _S18;
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

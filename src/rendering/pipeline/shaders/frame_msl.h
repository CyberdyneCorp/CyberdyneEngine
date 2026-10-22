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

/// DepthVertex.metal, 7803 bytes.
inline constexpr char kFrameDepthVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 334 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 414
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


#line 321 "src/rendering/shaders/cy/frame.slang"
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


#line 343
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


#line 388
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


#line 398 "src/rendering/shaders/cy/frame.slang"
float4 transformToClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow3_0, _S2));
}


#line 558
float3 previousRelativeOf_0(uint slot_0, const CyInstanceTransform_0 thread* current_0, float3 previousModelPosition_0, KernelContext_0 thread* kernelContext_1)
{
    uint _S3 = kernelContext_1->cyFrameView_0->frame_0->motionControl_0.x;

#line 560
    CyInstanceTransform_0 _S4;
    if(_S3 == 4294967295U)
    {

#line 561
        _S4 = *current_0;

#line 561
    }
    else
    {

#line 561
        _S4 = kernelContext_1->cyFrameView_0->instances_0[_S3 + slot_0];

#line 561
    }

#line 561
    thread CyInstanceTransform_0 _S5 = _S4;

#line 561
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


#line 423 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 425
struct cyDepthVertex_Result_0
{
    float4 position_0 [[position]];
    float3 normal_1 [[user(TEXCOORD)]];
    float4 currentClip_0 [[user(TEXCOORD_1)]];
    float4 previousClip_0 [[user(TEXCOORD_2)]];
};


#line 425
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
    float3 previousModelPosition_1 [[attribute(2)]];
};


#line 541
struct CyDepthVertex_0
{
    float4 position_1;
    float3 normal_2;
    float4 currentClip_1;
    float4 previousClip_1;
};


#line 541
[[vertex]] cyDepthVertex_Result_0 cyDepthVertex(vertexInput_0 _S13 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 541
    thread KernelContext_0 kernelContext_2;

#line 541
    (&kernelContext_2)->cyFrameView_0 = cyFrameView_1;

#line 541
    (&kernelContext_2)->cyDraw_0 = cyDraw_1;

#line 569
    CyDrawInstance_0 _S14 = cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0];
    CyInstanceTransform_0 _S15 = cyFrameView_1->instances_0[_S14.instanceSlot_0];

#line 570
    thread CyInstanceTransform_0 _S16 = _S15;

#line 570
    float3 _S17 = transformToRelative_0(&_S16, _S13.modelPosition_1);
    thread CyDepthVertex_0 output_0;

#line 571
    float4 _S18 = transformToClip_0(_S17, &kernelContext_2);

    (&output_0)->position_1 = _S18;
    (&output_0)->currentClip_1 = _S18;

#line 574
    thread CyInstanceTransform_0 _S19 = _S15;

#line 574
    float3 _S20 = previousRelativeOf_0(_S14.instanceSlot_0, &_S19, _S13.previousModelPosition_1, &kernelContext_2);
    float4 _S21 = float4(_S20, 1.0);
    (&output_0)->previousClip_1 = float4(dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow0_0, _S21), dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow1_0, _S21), dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow2_0, _S21), dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow3_0, _S21));



    float3 _S22 = decodeOctahedral_0(_S13.packedNormal_0.xy);

#line 580
    thread CyInstanceTransform_0 _S23 = _S15;

#line 580
    float3 _S24 = rotateToRelative_0(&_S23, _S22);

#line 580
    (&output_0)->normal_2 = _S24;

#line 580
    thread cyDepthVertex_Result_0 _S25;

#line 580
    (&_S25)->position_0 = output_0.position_1;

#line 580
    (&_S25)->normal_1 = output_0.normal_2;

#line 580
    (&_S25)->currentClip_0 = output_0.currentClip_1;

#line 580
    (&_S25)->previousClip_0 = output_0.previousClip_1;

#line 580
    return _S25;
}

)cy_msl";

/// DepthFragment.metal, 4290 bytes.
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


#line 584 "src/rendering/shaders/cy/frame.slang"
struct CyDepthOutput_0
{
    float4 normalRoughness_0 [[color(0)]];
    float2 velocity_0 [[color(1)]];
};


#line 584
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


#line 321 "src/rendering/shaders/cy/frame.slang"
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


#line 591
[[fragment]] CyDepthOutput_0 cyDepthFragment(pixelInput_0 _S4 [[stage_in]], float4 position_0 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_0 [[buffer(1)]])
{
    thread CyDepthOutput_0 output_0;
    (&output_0)->normalRoughness_0 = float4(encodeOctahedral_0(normalize(_S4.normal_1)), 1.0, 1.0);

#line 602
    float2 _S5 = _S4.currentClip_0.xy / float2(_S4.currentClip_0.w)  - cyFrameView_0->frame_0->temporalJitter_0.xy * float2(2.0)  * cyFrameView_0->frame_0->extentAndInverse_0.zw;

    float2 _S6 = _S4.previousClip_0.xy / float2(_S4.previousClip_0.w) ;
    (&output_0)->velocity_0 = float2((_S6.x - _S5.x) * 0.5, (_S5.y - _S6.y) * 0.5);

    return output_0;
}

)cy_msl";

/// ShadowVertex.metal, 4879 bytes.
inline constexpr char kFrameShadowVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 334 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 414
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


#line 321 "src/rendering/shaders/cy/frame.slang"
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


#line 343
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


#line 388
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


#line 405 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow3_0, _S2));
}


#line 408
struct cyShadowVertex_Result_0
{
    float4 position_0 [[position]];
};


#line 408
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
};


#line 516
struct CyShadowVertex_0
{
    float4 position_1;
};


#line 516
[[vertex]] cyShadowVertex_Result_0 cyShadowVertex(vertexInput_0 _S3 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 516
    thread KernelContext_0 kernelContext_1;

#line 516
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 516
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 526
    thread CyShadowVertex_0 output_0;

#line 526
    thread CyInstanceTransform_0 _S4 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

#line 526
    float3 _S5 = transformToRelative_0(&_S4, _S3.modelPosition_1);

#line 526
    float4 _S6 = transformToShadowClip_0(_S5, &kernelContext_1);
    (&output_0)->position_1 = _S6;

#line 527
    thread cyShadowVertex_Result_0 _S7;

#line 527
    (&_S7)->position_0 = output_0.position_1;

#line 527
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


#line 532 "src/rendering/shaders/cy/frame.slang"
[[fragment]] pixelOutput_0 cyShadowFragment(float4 position_0 [[position]])
{

#line 532
    pixelOutput_0 _S1 = { position_0.z };

    return _S1;
}

)cy_msl";

/// ForwardVertex.metal, 6929 bytes.
inline constexpr char kFrameForwardVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 334 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 414
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


#line 321 "src/rendering/shaders/cy/frame.slang"
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


#line 343
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


#line 388
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


#line 398 "src/rendering/shaders/cy/frame.slang"
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


#line 423 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 425
struct cyForwardVertex_Result_0
{
    float4 position_0 [[position]];
    float3 relativePosition_0 [[user(TEXCOORD)]];
    float3 normal_1 [[user(TEXCOORD_1)]];
    float2 uv_0 [[user(TEXCOORD_2)]];
    uint drawIndex_1 [[user(TEXCOORD_3)]];
    float2 lightmapUv_0 [[user(TEXCOORD_4)]];
};


#line 425
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
    float2 uv_1 [[attribute(2)]];
    float2 lightmapUv_1 [[attribute(3)]];
};


#line 612
struct CyForwardVertex_0
{
    float4 position_1;
    float3 relativePosition_1;
    float3 normal_2;
    float2 uv_2;
    [[flat]] uint drawIndex_2;
    float2 lightmapUv_2;
};


#line 612
[[vertex]] cyForwardVertex_Result_0 cyForwardVertex(vertexInput_0 _S9 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 612
    thread KernelContext_0 kernelContext_1;

#line 612
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 612
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 633
    CyInstanceTransform_0 _S10 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

    thread CyForwardVertex_0 output_0;

#line 635
    thread CyInstanceTransform_0 _S11 = _S10;

#line 635
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    (&output_0)->relativePosition_1 = _S12;

#line 636
    float4 _S13 = transformToClip_0(_S12, &kernelContext_1);
    (&output_0)->position_1 = _S13;



    float3 _S14 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 641
    thread CyInstanceTransform_0 _S15 = _S10;

#line 641
    float3 _S16 = rotateToRelative_0(&_S15, _S14);

#line 641
    (&output_0)->normal_2 = _S16;
    (&output_0)->uv_2 = _S9.uv_1;
    (&output_0)->drawIndex_2 = cyDraw_1->drawIndex_0;
    (&output_0)->lightmapUv_2 = _S9.lightmapUv_1;

#line 644
    thread cyForwardVertex_Result_0 _S17;

#line 644
    (&_S17)->position_0 = output_0.position_1;

#line 644
    (&_S17)->relativePosition_0 = output_0.relativePosition_1;

#line 644
    (&_S17)->normal_1 = output_0.normal_2;

#line 644
    (&_S17)->uv_0 = output_0.uv_2;

#line 644
    (&_S17)->drawIndex_1 = output_0.drawIndex_2;

#line 644
    (&_S17)->lightmapUv_0 = output_0.lightmapUv_2;

#line 644
    return _S17;
}

)cy_msl";

/// ForwardFragment.metal, 103972 bytes.
inline constexpr auto kFrameForwardFragmentMslText = detail::join_msl(
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 1258 "src/rendering/shaders/cy/frame.slang"
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


#line 321 "src/rendering/shaders/cy/frame.slang"
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


#line 1261 "src/rendering/shaders/cy/frame.slang"
float4 CyFrameFogVolume_texel_0(const CyFrameFogVolume_0 thread* this_0, int2 coordinate_0, KernelContext_0 thread* kernelContext_0)
{

    int3 _S1 = int3(coordinate_0, int(0));

#line 1264
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


#line 1261 "src/rendering/shaders/cy/frame.slang"
float4 CyFrameFogVolume_texel_1(const CyFrameFogVolume_0 thread* this_1, int2 coordinate_1, KernelContext_0 thread* kernelContext_2)
{

    int3 _S16 = int3(coordinate_1, int(0));

#line 1264
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


#line 650 "src/rendering/shaders/cy/frame.slang"
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


#line 381 "src/rendering/shaders/cy/frame.slang"
float4 cyMaterialSampleTextureLevel_0(uint bindlessIndex_0, float2 uv_0, float level_0, KernelContext_0 thread* kernelContext_4)
{
    return ((kernelContext_4->cyFrameGlobals_0->textures_0[bindlessIndex_0]).sample((kernelContext_4->cyFrameGlobals_0->sampler_0), (uv_0), level((level_0))));
}


#line 653
float CyFrameShadowMap_storedDepth_0(const CyFrameShadowMap_0 thread* this_2, float2 uv_1, KernelContext_0 thread* kernelContext_5)
{

#line 653
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


#line 653 "src/rendering/shaders/cy/frame.slang"
float CyFrameShadowMap_storedDepth_1(const CyFrameShadowMap_0 thread* this_3, float2 uv_3, KernelContext_0 thread* kernelContext_7)
{

#line 653
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


#line 1172 "src/rendering/shaders/cy/frame.slang"
uint CyFrameDecalSource_word_0(uint index_6, KernelContext_0 thread* kernelContext_9)
{

#line 1172
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
    return float3(_S54, _S56, (as_type<float>((_S57))));
}


#line 108
float wordFloat_0(uint index_8, KernelContext_0 thread* kernelContext_11)
{

#line 108
    uint _S58 = CyFrameDecalSource_word_0(index_8, kernelContext_11);

    return (as_type<float>((_S58)));
}


#line 151 "src/rendering/shaders/cy/volumetric_fog.slang"
)cy_msl",
    R"cy_msl(float3 cyApplyFog_0(const CyFrameFogVolume_0 thread* source_4, float3 radiance_0, float3 relativePosition_1, KernelContext_0 thread* kernelContext_12)
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


#line 1172 "src/rendering/shaders/cy/frame.slang"
uint CyFrameDecalSource_word_1(uint index_9, KernelContext_0 thread* kernelContext_14)
{

#line 1172
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


#line 381 "src/rendering/shaders/cy/frame.slang"
float4 cyMaterialSampleTextureLevel_1(uint bindlessIndex_1, float2 uv_6, float level_1, KernelContext_0 thread* kernelContext_15)
{
    return ((kernelContext_15->cyFrameGlobals_0->textures_0[bindlessIndex_1]).sample((kernelContext_15->cyFrameGlobals_0->sampler_0), (uv_6), level((level_1))));
}


#line 1177
float CyFrameDecalSource_mask_0(uint slot_2, float2 uv_7, KernelContext_0 thread* kernelContext_16)
{

#line 1177
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
    (&surface_2)->metallic_1 = 0.0;
    (&surface_2)->emission_1 = float3(0.0) ;
    (&surface_2)->occlusion_0 = 1.0;
    (&surface_2)->opacity_0 = 1.0;
    return surface_2;
}


#line 442 "src/rendering/shaders/cy/frame.slang"
float3 readMaterialFloat3_0(uint material_1, uint wordOffset_0, KernelContext_0 thread* kernelContext_21)
{
)cy_msl",
    R"cy_msl(    uint _S159 = material_1 * kernelContext_21->cyFrameView_0->frame_0->counts_0.y + wordOffset_0;
    return float3((as_type<float>((kernelContext_21->cyFrameView_0->materialWords_0[_S159]))), (as_type<float>((kernelContext_21->cyFrameView_0->materialWords_0[_S159 + 1U]))), (as_type<float>((kernelContext_21->cyFrameView_0->materialWords_0[_S159 + 2U]))));
}


#line 437
float readMaterialFloat_0(uint material_2, uint wordOffset_1, KernelContext_0 thread* kernelContext_22)
{
    return (as_type<float>((kernelContext_22->cyFrameView_0->materialWords_0[material_2 * kernelContext_22->cyFrameView_0->frame_0->counts_0.y + wordOffset_1])));
}


#line 452
uint readMaterialUint_0(uint material_3, uint wordOffset_2, KernelContext_0 thread* kernelContext_23)
{
    return kernelContext_23->cyFrameView_0->materialWords_0[material_3 * kernelContext_23->cyFrameView_0->frame_0->counts_0.y + wordOffset_2];
}


#line 462
uint materialTextureSlot_0(uint material_4, uint wordOffset_3, KernelContext_0 thread* kernelContext_24)
{
    if(wordOffset_3 == 4294967295U)
    {
        return 4294967295U;
    }

#line 466
    uint _S160 = readMaterialUint_0(material_4, wordOffset_3, kernelContext_24);

    return _S160;
}


#line 376
float4 cyMaterialSampleTexture_0(uint bindlessIndex_2, float2 uv_10, KernelContext_0 thread* kernelContext_25)
{
    return ((kernelContext_25->cyFrameGlobals_0->textures_0[bindlessIndex_2]).sample((kernelContext_25->cyFrameGlobals_0->sampler_0), (uv_10)));
}


#line 498
Surface_0 surfaceOf_0(uint material_5, float3 tint_1, float2 uv_11, KernelContext_0 thread* kernelContext_26)
{
    thread Surface_0 surface_3 = defaultSurface_0();

#line 500
    float3 _S161 = readMaterialFloat3_0(material_5, kernelContext_26->cyFrameView_0->frame_0->materialOffsets_0.x, kernelContext_26);
    (&surface_3)->albedo_1 = _S161 * tint_1;

#line 501
    float _S162 = readMaterialFloat_0(material_5, kernelContext_26->cyFrameView_0->frame_0->materialOffsets_0.y, kernelContext_26);
    (&surface_3)->roughness_1 = clamp(_S162, 0.01999999955296516, 1.0);

#line 502
    float _S163 = readMaterialFloat_0(material_5, kernelContext_26->cyFrameView_0->frame_0->materialOffsets_0.z, kernelContext_26);
    (&surface_3)->metallic_1 = saturate(_S163);

#line 503
    float3 _S164 = readMaterialFloat3_0(material_5, kernelContext_26->cyFrameView_0->frame_0->materialOffsets_0.w, kernelContext_26);
    (&surface_3)->emission_1 = _S164;

#line 504
    uint _S165 = materialTextureSlot_0(material_5, kernelContext_26->cyFrameView_0->frame_0->materialTextures_0.x, kernelContext_26);


    if(_S165 != 4294967295U)
    {

#line 507
        float4 _S166 = cyMaterialSampleTexture_0(_S165, uv_11, kernelContext_26);

        (&surface_3)->albedo_1 = (&surface_3)->albedo_1 * _S166.xyz;

#line 507
    }



    return surface_3;
}


#line 1281
float2 lightmapCoordinate_0(uint address_0, float2 uv2_0, KernelContext_0 thread* kernelContext_27)
{
    float _S167 = float(kernelContext_27->cyFrameView_0->frame_0->lightmapLayout_0.x);

    float _S168 = float(kernelContext_27->cyFrameView_0->frame_0->lightmapLayout_0.z);

#line 1285
    float2 _S169 = float2(float(kernelContext_27->cyFrameView_0->frame_0->lightmapLayout_0.x / 128U)) ;

#line 1291
    return (float2(float(address_0 & 127U), float((address_0 >> 7U) & 127U)) * _S169 + float2(_S168)  + uv2_0 * (float2(float(((address_0 >> 14U) & 127U) + 1U), float(((address_0 >> 21U) & 127U) + 1U)) * _S169 - float2((2.0 * _S168)) ) + float2(0.0, float((address_0 >> 28U) - 1U) * _S167)) / float2(_S167, _S167 * float(kernelContext_27->cyFrameView_0->frame_0->lightmapLayout_0.y));
}


#line 1368
float3 lightmapDensityHue_0(bool lightmapped_0, float2 uv_12, float3 relativePosition_4, KernelContext_0 thread* kernelContext_28)
{
    if(!lightmapped_0)
    {
        return float3(0.18000000715255737, 0.18000000715255737, 0.18000000715255737);
    }


    float2 _S170 = uv_12 * float2(float(kernelContext_28->cyFrameView_0->frame_0->lightmapLayout_0.x), float(kernelContext_28->cyFrameView_0->frame_0->lightmapLayout_0.x * kernelContext_28->cyFrameView_0->frame_0->lightmapLayout_0.y));
    float2 _S171 = dfdx(_S170);

#line 1377
    float2 _S172 = dfdy(_S170);
    float3 _S173 = dfdx(relativePosition_4);
    float3 _S174 = dfdy(relativePosition_4);

    float _S175 = clamp(log2(max(sqrt(dot(_S171, _S171) + dot(_S172, _S172)) / max(sqrt(dot(_S173, _S173) + dot(_S174, _S174)), 9.99999971718068537e-10), 9.99999971718068537e-10) / max((as_type<float>((kernelContext_28->cyFrameView_0->frame_0->lightmapDebug_0.y))), 9.99999997475242708e-07)), -1.0, 1.0);

#line 1381
    float3 _S176;
    if(_S175 < 0.0)
    {

#line 1382
        _S176 = mix(float3(0.10000000149011612, 0.89999997615814209, 0.20000000298023224), float3(0.10000000149011612, 0.25, 1.0), float3(- _S175) );

#line 1382
    }
    else
    {

#line 1382
        _S176 = mix(float3(0.10000000149011612, 0.89999997615814209, 0.20000000298023224), float3(1.0, 0.15000000596046448, 0.10000000149011612), float3(_S175) );

#line 1382
    }

    uint2 _S177 = uint2(floor(_S170));
    if(((_S177.x + _S177.y) & 1U) == 0U)
    {
    }
    else
    {

#line 1385
        _S176 = _S176 * float3(0.55000001192092896) ;

#line 1385
    }

#line 1385
    return _S176;
}


#line 1352
float frameExposureStops_0(KernelContext_0 thread* kernelContext_29)
{

    return kernelContext_29->cyFrameGlobals_0->globals_0->exposureStops_0;
}


#line 1352
float frameExposureStops_1(KernelContext_0 thread* kernelContext_30)
{

    return kernelContext_30->cyFrameGlobals_0->globals_0->exposureStops_0;
}


#line 1363
float3 lightmapDensityColour_0(bool lightmapped_1, float2 uv_13, float3 relativePosition_5, KernelContext_0 thread* kernelContext_31)
{

#line 1363
    float3 _S178 = lightmapDensityHue_0(lightmapped_1, uv_13, relativePosition_5, kernelContext_31);

#line 1363
    float _S179 = frameExposureStops_1(kernelContext_31);

    return _S178 * float3(exp2(- _S179)) ;
}


#line 1390
float3 lightmapChartColour_0(bool lightmapped_2, uint address_1, float2 uv_14, KernelContext_0 thread* kernelContext_32)
{
    if(!lightmapped_2)
    {

#line 1392
        float _S180 = frameExposureStops_0(kernelContext_32);

        return float3(0.18000000715255737, 0.18000000715255737, 0.18000000715255737) * float3(exp2(- _S180)) ;
    }



    float _S181 = float(((address_1 * 2654435761U) ^ (address_1 >> 15U)) >> 8U) * 3.5762786865234375e-07;
    float _S182 = fract(_S181);
    uint _S183 = uint(_S181) % 6U;
    float _S184 = 0.89999997615814209 * (1.0 - 0.64999997615814209 * (1.0 - _S182));

#line 1402
    float3 _S185 = float3(0.89999997615814209, _S184, 0.31499999761581421);

#line 1402
    float3 colour_0;
    if(_S183 == 1U)
    {

#line 1403
        colour_0 = float3(0.89999997615814209 * (1.0 - 0.64999997615814209 * _S182), 0.89999997615814209, 0.31499999761581421);

#line 1403
    }
    else
    {

#line 1403
        colour_0 = _S185;

#line 1403
    }
    if(_S183 == 2U)
    {

#line 1404
        colour_0 = float3(0.31499999761581421, 0.89999997615814209, _S184);

#line 1404
    }
    if(_S183 == 3U)
    {

#line 1405
        colour_0 = float3(0.31499999761581421, 0.89999997615814209 * (1.0 - 0.64999997615814209 * _S182), 0.89999997615814209);

#line 1405
    }
    if(_S183 == 4U)
    {

#line 1406
        colour_0 = float3(_S184, 0.31499999761581421, 0.89999997615814209);

#line 1406
    }
    if(_S183 == 5U)
    {

#line 1407
        colour_0 = float3(0.89999997615814209, 0.31499999761581421, 0.89999997615814209 * (1.0 - 0.64999997615814209 * _S182));

#line 1407
    }


    uint2 _S186 = uint2(floor(uv_14 * float2(float(kernelContext_32->cyFrameView_0->frame_0->lightmapLayout_0.x), float(kernelContext_32->cyFrameView_0->frame_0->lightmapLayout_0.x * kernelContext_32->cyFrameView_0->frame_0->lightmapLayout_0.y))));

#line 1410
    float _S187;
    if(((_S186.x + _S186.y) & 1U) == 0U)
    {

#line 1411
        _S187 = 1.0;

#line 1411
    }
    else
    {

#line 1411
        _S187 = 0.69999998807907104;

#line 1411
    }
    float3 _S188 = colour_0 * float3(_S187) ;

#line 1412
    float _S189 = frameExposureStops_0(kernelContext_32);

#line 1412
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


#line 429 "src/rendering/shaders/cy/frame.slang"
float viewDepthOf_0(float3 relative_0, KernelContext_0 thread* kernelContext_33)
{



    return - dot(kernelContext_33->cyFrameView_0->frame_0->relativeToViewRow2_0, float4(relative_0, 1.0));
}


#line 1185
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

#line 1204
    uint _S193 = kernelContext_34->cyFrameView_0->frame_0->decalControl_0.z;

#line 1204
    uint _S194 = cyDecalCount_0(kernelContext_34);


    uint2 _S195 = uint2(0U, _S194);

#line 1207
    uint2 list_0;

#line 1207
    uint mode_0;
    if((_S193 & 2U) != 0U)
    {

#line 1208
        list_0 = _S195;

#line 1208
        mode_0 = 0U;

#line 1208
    }
    else
    {

        if((_S193 & 1U) != 0U)
        {

            if(_S194 != 0U)
            {

#line 1215
                uint2 _S196 = cyDecalTableList_0(relativePosition_6, fragmentCentre_0, kernelContext_34);

#line 1215
                list_0 = _S196;

#line 1215
            }
            else
            {

#line 1215
                list_0 = uint2(0U) ;

#line 1215
            }

#line 1215
            mode_0 = 1U;

#line 1212
        }
        else
        {

#line 1212
            bool _S197;

#line 1217
            if((kernelContext_34->cyFrameView_0->frame_0->counts_0.w) != 0U)
            {

#line 1217
                _S197 = ((&kernelContext_34->cyFrameView_0->frame_0->clusterGrid_0)->dimensions_0.z) != 0U;

#line 1217
            }
            else
            {

#line 1217
                _S197 = false;

#line 1217
            }

#line 1217
            if(_S197)
            {
                uint2 _S198 = uint2(kernelContext_34->cyFrameView_0->frame_0->extentAndInverse_0.xy);
                uint2 _S199 = uint2(fragmentCentre_0);

#line 1220
                float _S200 = viewDepthOf_0(relativePosition_6, kernelContext_34);

#line 1220
                uint3 _S201 = clusterCoordOf_1(&kernelContext_34->cyFrameView_0->frame_0->clusterGrid_0, _S199, _S198, _S200);

#line 1220
                uint _S202 = clusterIndexOf_1(&kernelContext_34->cyFrameView_0->frame_0->clusterGrid_0, _S201);

#line 1220
                list_0 = kernelContext_34->cyFrameView_0->clusterHeaders_0[_S202 * kernelContext_34->cyFrameView_0->frame_0->counts_0.z + 1U];

#line 1220
                mode_0 = 2U;

#line 1217
            }
            else
            {

#line 1217
                list_0 = _S195;

#line 1217
                mode_0 = 0U;

#line 1217
            }

#line 1212
        }

#line 1208
    }

#line 1208
    uint slot_3 = 0U;

#line 1228
    for(;;)
    {

#line 1228
        if(slot_3 < (list_0.y))
        {
        }
        else
        {

#line 1228
            break;
        }

#line 1228
        uint rank_1;


        if(mode_0 == 1U)
        {

#line 1231
            uint _S203 = cyDecalTableListEntry_0(list_0.x + slot_3, kernelContext_34);

#line 1231
            rank_1 = _S203;

#line 1231
        }
        else
        {

            if(mode_0 == 2U)
            {

#line 1235
                rank_1 = kernelContext_34->cyFrameView_0->clusterIndices_0[list_0.x + slot_3];

#line 1235
            }
            else
            {

#line 1235
                rank_1 = slot_3;

#line 1235
            }

#line 1231
        }

#line 1239
        if(rank_1 < _S194)
        {

#line 1239
            thread CyDecalReceiver_0 _S204 = receiver_5;

#line 1239
            cyApplyDecal_0(rank_1, &_S204, &decalled_0, kernelContext_34);

#line 1239
        }

#line 1228
        slot_3 = slot_3 + 1U;

#line 1228
    }

#line 1245
    surface_4->albedo_1 = (&decalled_0)->albedo_0;
    surface_4->roughness_1 = clamp((&decalled_0)->roughness_0, 0.01999999955296516, 1.0);
    surface_4->metallic_1 = saturate((&decalled_0)->metallic_0);
    surface_4->emission_1 = (&decalled_0)->emission_0;
    *normal_2 = (&decalled_0)->normal_0;
    return;
}


#line 732
struct CyBakedLights_0
{
    bool active_0;
    float4 mask_0;
};


#line 1324
CyBakedLights_0 bakedLightsOf_0(bool lightmapped_3, float2 uv_15, KernelContext_0 thread* kernelContext_35)
{
    thread CyBakedLights_0 baked_0;
    (&baked_0)->active_0 = lightmapped_3;
    (&baked_0)->mask_0 = float4(1.0) ;

#line 1328
    bool _S205;
    if(lightmapped_3)
    {

#line 1329
        _S205 = (kernelContext_35->cyFrameView_0->frame_0->lightmapLayout_0.w) != 4294967295U;

#line 1329
    }
    else
    {

#line 1329
        _S205 = false;

#line 1329
    }

#line 1329
    if(_S205)
    {

#line 1329
        float4 _S206 = cyMaterialSampleTexture_0(kernelContext_35->cyFrameView_0->frame_0->lightmapLayout_0.w, uv_15, kernelContext_35);

        (&baked_0)->mask_0 = _S206;

#line 1329
    }



    return baked_0;
}


#line 61 "src/rendering/shaders/cy/light.slang"
float distanceAttenuation_0(float distanceSquared_0, float range_1)
{
    float _S207 = distanceSquared_0 / max(range_1 * range_1, 9.99999997475242708e-07);
    float _S208 = saturate(1.0 - _S207 * _S207);
    return _S208 * _S208 / max(distanceSquared_0, 0.00009999999747379);
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
    uint _S209 = light_0->kind_0;
    if((light_0->kind_0) == 0U)
    {
        (&result_4)->direction_1 = - light_0->direction_0;
        (&result_4)->attenuation_0 = 1.0;
        (&result_4)->illuminance_0 = light_0->color_0 * float3(light_0->intensity_0) ;
        return result_4;
    }

    float3 _S210 = light_0->positionRelativeToCamera_0 - surfaceRelativeToCamera_0;
    float _S211 = dot(_S210, _S210);
    (&result_4)->direction_1 = _S210 * float3(rsqrt(max(_S211, 9.99999993922529029e-09))) ;
    (&result_4)->attenuation_0 = distanceAttenuation_0(_S211, light_0->range_0);

    if(_S209 == 2U)
    {

        float _S212 = saturate(dot(- (&result_4)->direction_1, light_0->direction_0) * light_0->spotScaleBias_0.x + light_0->spotScaleBias_0.y);
        (&result_4)->attenuation_0 = (&result_4)->attenuation_0 * (_S212 * _S212);

#line 84
    }
    else
    {



)cy_msl",
    R"cy_msl(        if(_S209 == 3U)
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


#line 405 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_1, KernelContext_0 thread* kernelContext_36)
{
    float4 _S213 = float4(relative_1, 1.0);
    return float4(dot(kernelContext_36->cyFrameView_0->frame_0->shadowToClipRow0_0, _S213), dot(kernelContext_36->cyFrameView_0->frame_0->shadowToClipRow1_0, _S213), dot(kernelContext_36->cyFrameView_0->frame_0->shadowToClipRow2_0, _S213), dot(kernelContext_36->cyFrameView_0->frame_0->shadowToClipRow3_0, _S213));
}


#line 98 "src/rendering/shaders/cy/shadow.slang"
float shadowDiscRotation_0(float2 pixel_3)
{

    return fract(52.98291778564453125 * fract(dot(pixel_3, float2(0.06711056083440781, 0.00583714991807938)))) * 6.28318548202514648;
}


#line 661 "src/rendering/shaders/cy/frame.slang"
float softShadowVisibility_0(float2 uv_16, float reference_0, float2 pixel_4, KernelContext_0 thread* kernelContext_37)
{
    thread CyFrameShadowMap_0 map_0;
    (&map_0)->slot_1 = kernelContext_37->cyFrameView_0->frame_0->shadowControl_0.x;
    thread PcssShape_0 shape_4;
    (&shape_4)->penumbraPerDepth_0 = kernelContext_37->cyFrameView_0->frame_0->softShadowShape_0.x;
    (&shape_4)->minRadius_0 = kernelContext_37->cyFrameView_0->frame_0->softShadowShape_0.y;
    (&shape_4)->maxRadius_0 = kernelContext_37->cyFrameView_0->frame_0->softShadowShape_0.z;
    (&shape_4)->blockerTaps_0 = int(max(kernelContext_37->cyFrameView_0->frame_0->softShadowControl_0.z, 1U));
    (&shape_4)->filterTaps_0 = int(max(kernelContext_37->cyFrameView_0->frame_0->softShadowControl_0.w, 1U));
    float _S214 = shadowDiscRotation_0(pixel_4);

#line 671
    thread CyFrameShadowMap_0 _S215 = map_0;

#line 671
    thread PcssShape_0 _S216 = shape_4;

#line 671
    float _S217 = pcssVisibility_0(&_S215, uv_16, reference_0, &_S216, _S214, kernelContext_37);

#line 671
    return _S217;
}


#line 694
float directionalShadowVisibility_0(float3 relativePosition_7, float3 normal_3, float2 fragmentCentre_1, KernelContext_0 thread* kernelContext_38)
{

#line 694
    bool _S218;

    if((kernelContext_38->cyFrameView_0->frame_0->shadowControl_0.w) == 0U)
    {

#line 696
        _S218 = true;

#line 696
    }
    else
    {

#line 696
        _S218 = (kernelContext_38->cyFrameView_0->frame_0->shadowControl_0.x) == 4294967295U;

#line 696
    }

#line 696
    if(_S218)
    {
        return 1.0;
    }

#line 698
    float4 _S219 = transformToShadowClip_0(relativePosition_7 + normal_3 * float3(0.00499999988824129) , kernelContext_38);


    float _S220 = _S219.w;

#line 701
    if(_S220 <= 0.0)
    {
        return 1.0;
    }
    float3 _S221 = _S219.xyz / float3(_S220) ;
    float _S222 = _S221.x * 0.5 + 0.5;

#line 706
    float _S223 = 0.5 - _S221.y * 0.5;

#line 706
    float2 _S224 = float2(_S222, _S223);
    if(_S222 < 0.0)
    {

#line 707
        _S218 = true;

#line 707
    }
    else
    {

#line 707
        _S218 = _S222 > 1.0;

#line 707
    }

#line 707
    if(_S218)
    {

#line 707
        _S218 = true;

#line 707
    }
    else
    {

#line 707
        _S218 = _S223 < 0.0;

#line 707
    }

#line 707
    if(_S218)
    {

#line 707
        _S218 = true;

#line 707
    }
    else
    {

#line 707
        _S218 = _S223 > 1.0;

#line 707
    }

#line 707
    if(_S218)
    {
        return 1.0;
    }
    float _S225 = 1.0 / float(max(kernelContext_38->cyFrameView_0->frame_0->shadowControl_0.z, 1U));
    float _S226 = _S221.z + 0.00050000002374873;
    if(((kernelContext_38->cyFrameView_0->frame_0->softShadowControl_0.x) & 1U) != 0U)
    {

#line 713
        float _S227 = softShadowVisibility_0(_S224, _S226, fragmentCentre_1, kernelContext_38);

        return _S227;
    }

#line 715
    int y_0 = int(-1);

#line 715
    float visible_0 = 0.0;


    for(;;)
    {

#line 718
        if(y_0 <= int(1))
        {
        }
        else
        {

#line 718
            break;
        }

#line 718
        int x_0 = int(-1);

        for(;;)
        {

#line 720
            if(x_0 <= int(1))
            {
            }
            else
            {

#line 720
                break;
            }

#line 720
            float4 _S228 = cyMaterialSampleTextureLevel_0(kernelContext_38->cyFrameView_0->frame_0->shadowControl_0.x, _S224 + float2(float(x_0), float(y_0)) * float2(_S225) , 0.0, kernelContext_38);

#line 720
            float _S229;



            if(_S226 >= (_S228.x))
            {

#line 724
                _S229 = 1.0;

#line 724
            }
            else
            {

#line 724
                _S229 = 0.0;

#line 724
            }

#line 724
            float visible_1 = visible_0 + _S229;

#line 720
            x_0 = x_0 + int(1);

#line 720
            visible_0 = visible_1;

#line 720
        }

#line 718
        y_0 = y_0 + int(1);

#line 718
    }

#line 727
    return visible_0 / 9.0;
}


#line 677
float contactShadowVisibility_0(float2 fragmentCentre_2, KernelContext_0 thread* kernelContext_39)
{

#line 677
    bool _S230;

    if(((kernelContext_39->cyFrameView_0->frame_0->softShadowControl_0.x) & 2U) == 0U)
    {

#line 679
        _S230 = true;

#line 679
    }
    else
    {

#line 679
        _S230 = (kernelContext_39->cyFrameView_0->frame_0->softShadowControl_0.y) == 4294967295U;

#line 679
    }

#line 679
    if(_S230)
    {

        return 1.0;
    }

#line 682
    float4 _S231 = cyMaterialSampleTextureLevel_0(kernelContext_39->cyFrameView_0->frame_0->softShadowControl_0.y, fragmentCentre_2 * kernelContext_39->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_39);


    return _S231.x;
}


#line 740
float bakedLightFactor_0(uint index_11, float4 mask_1, KernelContext_0 thread* kernelContext_40)
{

#line 740
    bool _S232;

    if(index_11 < 128U)
    {

#line 742
        _S232 = (((kernelContext_40->cyFrameView_0->frame_0->lightmapDirectLights_0[index_11 >> 5U]) >> (index_11 & 31U)) & 1U) != 0U;

#line 742
    }
    else
    {

#line 742
        _S232 = false;

#line 742
    }

#line 742
    if(_S232)
    {
        return 0.0;
    }

#line 744
    uint channel_0 = 0U;

    for(;;)
    {

#line 746
        if(channel_0 < 4U)
        {
        }
        else
        {

#line 746
            break;
        }
        if((kernelContext_40->cyFrameView_0->frame_0->lightmapShadowLights_0[channel_0]) == index_11)
        {
            return mask_1[channel_0];
        }

#line 746
        channel_0 = channel_0 + 1U;

#line 746
    }

#line 753
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
    float _S233 = roughness_2 * roughness_2;
    float _S234 = _S233 * _S233;
    float _S235 = normalDotHalf_0 * normalDotHalf_0 * (_S234 - 1.0) + 1.0;
    return _S234 / max(3.14159274101257324 * _S235 * _S235, 1.00000001168609742e-07);
}


float visibilitySmithGgxCorrelated_0(float normalDotView_0, float normalDotLight_0, float roughness_3)
{

    float _S236 = roughness_3 * roughness_3;
    float _S237 = _S236 * _S236;
    float _S238 = 1.0 - _S237;



    return 0.5 / max(normalDotLight_0 * sqrt(normalDotView_0 * normalDotView_0 * _S238 + _S237) + normalDotView_0 * sqrt(normalDotLight_0 * normalDotLight_0 * _S238 + _S237), 1.00000001168609742e-07);
}

float3 fresnelSchlick_0(float3 f0_0, float viewDotHalf_0)
{

    return f0_0 + (float3(1.0)  - f0_0) * float3(pow(saturate(1.0 - viewDotHalf_0), 5.0)) ;
}


#line 45
float3 specularGgx_0(float3 normal_4, float3 view_0, float3 light_1, float roughness_4, float3 f0_1)
{
    float3 _S239 = normalize(view_0 + light_1);

#line 56
    return float3((distributionGgx_0(saturate(dot(normal_4, _S239)), roughness_4) * visibilitySmithGgxCorrelated_0(saturate(dot(normal_4, view_0)) + 0.00000999999974738, saturate(dot(normal_4, light_1)), roughness_4)))  * fresnelSchlick_0(f0_1, saturate(dot(view_0, _S239)));
}


#line 108 "src/rendering/shaders/cy/material.slang"
float3 shadeSurfaceWithLight_0(const Surface_0 thread* surface_5, float3 worldNormal_0, float3 viewDirection_0, const LightSample_0 thread* sample_0)
{

#line 109
    float3 _S240 = sample_0->direction_1;

    float _S241 = saturate(dot(worldNormal_0, sample_0->direction_1));
    if(_S241 <= 0.0)
    {
        return float3(0.0) ;
    }

#line 120
    return (diffuseLambert_0(surface_5->albedo_1 * float3((1.0 - surface_5->metallic_1)) ) + specularGgx_0(worldNormal_0, viewDirection_0, _S240, surface_5->roughness_1, computeF0_0(surface_5->albedo_1, surface_5->metallic_1))) * sample_0->illuminance_0 * float3(_S241) ;
}


#line 874 "src/rendering/shaders/cy/frame.slang"
float3 directionalContribution_0(uint global_0, const Surface_0 thread* surface_6, float3 relativePosition_8, float3 normal_5, float3 viewDir_0, float2 fragmentCentre_3, uint instanceFlags_0, const CyBakedLights_0 thread* baked_1, KernelContext_0 thread* kernelContext_41)
{
    thread Light_0 _S242 = kernelContext_41->cyFrameView_0->lights_0[global_0];

#line 876
    LightSample_0 _S243 = evaluateLight_0(&_S242, relativePosition_8);

#line 876
    bool _S244;

#line 882
    if(global_0 == (kernelContext_41->cyFrameView_0->frame_0->shadowControl_0.y))
    {

#line 882
        _S244 = (instanceFlags_0 & 8U) != 0U;

#line 882
    }
    else
    {

#line 882
        _S244 = false;

#line 882
    }

#line 882
    float visibility_0;
    if(_S244)
    {

#line 883
        float _S245 = directionalShadowVisibility_0(relativePosition_8, normal_5, fragmentCentre_3, kernelContext_41);

#line 883
        float _S246 = contactShadowVisibility_0(fragmentCentre_3, kernelContext_41);

#line 883
        visibility_0 = min(_S245, _S246);

#line 883
    }
    else
    {

#line 883
        visibility_0 = 1.0;

#line 883
    }

#line 890
    if(baked_1->active_0)
    {

#line 890
        float _S247 = bakedLightFactor_0(global_0, baked_1->mask_0, kernelContext_41);

#line 890
        visibility_0 = min(visibility_0, _S247);

#line 890
    }

#line 890
    thread LightSample_0 _S248 = _S243;

#line 890
    float3 _S249 = shadeSurfaceWithLight_0(surface_6, normal_5, viewDir_0, &_S248);



    return _S249 * float3(visibility_0) ;
}


#line 114 "src/rendering/shaders/cy/light.slang"
bool isAreaLight_0(uint kind_1)
{
    uint _S250 = kind_1 & 255U;

#line 116
    bool _S251;
    if(_S250 >= 4U)
    {

#line 117
        _S251 = _S250 <= 7U;

#line 117
    }
    else
    {

#line 117
        _S251 = false;

#line 117
    }

#line 117
    return _S251;
}


#line 767 "src/rendering/shaders/cy/frame.slang"
float4 frameLtcTexel_0(uint x_1, uint y_1, KernelContext_0 thread* kernelContext_42)
{

#line 767
    float4 _S252 = cyMaterialSampleTextureLevel_0(kernelContext_42->cyFrameView_0->frame_0->lightingControl_0.x, float2((float(x_1) + 0.5) / 24.0, (float(y_1) + 0.5) / 48.0), 0.0, kernelContext_42);



    return _S252;
}


#line 144 "src/rendering/shaders/cy/light.slang"
struct AreaLtc_0
{
    float4 transform_0;
    float magnitude_0;
};


#line 775 "src/rendering/shaders/cy/frame.slang"
AreaLtc_0 frameAreaLtc_0(float roughness_5, float cosTheta_0, KernelContext_0 thread* kernelContext_43)
{



    float _S253 = clamp(sqrt(saturate(roughness_5)) * 24.0 - 0.5, 0.0, 23.0);
    float _S254 = clamp(sqrt(saturate(1.0 - cosTheta_0)) * 24.0 - 0.5, 0.0, 23.0);
    uint _S255 = uint(_S253);
    uint _S256 = uint(_S254);
    uint _S257 = min(_S255 + 1U, 23U);
    uint _S258 = min(_S256 + 1U, 23U);
    float _S259 = _S253 - float(_S255);
    float _S260 = _S254 - float(_S256);

#line 787
    float4 _S261 = frameLtcTexel_0(_S255, _S256, kernelContext_43);

#line 787
    float4 _S262 = frameLtcTexel_0(_S257, _S256, kernelContext_43);
    float4 _S263 = float4(_S259) ;

#line 788
    float4 _S264 = mix(_S261, _S262, _S263);

#line 788
    float4 _S265 = frameLtcTexel_0(_S255, _S258, kernelContext_43);

#line 788
    float4 _S266 = frameLtcTexel_0(_S257, _S258, kernelContext_43);
    float4 _S267 = mix(_S265, _S266, _S263);
    uint _S268 = _S256 + 24U;

#line 790
    float4 _S269 = frameLtcTexel_0(_S255, _S268, kernelContext_43);

#line 790
    float4 _S270 = frameLtcTexel_0(_S257, _S268, kernelContext_43);

#line 790
    float4 _S271 = mix(_S269, _S270, _S263);

    uint _S272 = _S258 + 24U;

#line 792
    float4 _S273 = frameLtcTexel_0(_S255, _S272, kernelContext_43);

#line 792
    float4 _S274 = frameLtcTexel_0(_S257, _S272, kernelContext_43);
    float4 _S275 = float4(_S260) ;

#line 793
    float4 _S276 = mix(_S271, mix(_S273, _S274, _S263), _S275);
    thread AreaLtc_0 entry_0;
    (&entry_0)->transform_0 = mix(_S264, _S267, _S275);
    (&entry_0)->magnitude_0 = _S276.x;
    return entry_0;
}


#line 362 "src/rendering/shaders/cy/light.slang"
float areaLightWindow_0(const Light_0 thread* light_2, float3 surfaceRelativeToCamera_1)
{
    float3 _S277 = light_2->positionRelativeToCamera_0 - surfaceRelativeToCamera_1;
    float _S278 = dot(_S277, _S277) / max(light_2->range_0 * light_2->range_0, 9.99999997475242708e-07);
    float _S279 = saturate(1.0 - _S278 * _S278);
    return _S279 * _S279;
}


#line 161
float3 unitOr_0(float3 value_1, float3 fallback_0)
{
    float _S280 = dot(value_1, value_1);

#line 163
    float3 _S281;
    if(_S280 > 9.99999968265522539e-21)
    {

#line 164
        _S281 = value_1 * float3(rsqrt(_S280)) ;

#line 164
    }
    else
    {

#line 164
        _S281 = fallback_0;

#line 164
    }

#line 164
    return _S281;
}


#line 120
float signNotZero_0(float value_2)
{

#line 120
    float _S282;

    if(value_2 >= 0.0)
    {

#line 122
        _S282 = 1.0;

#line 122
    }
    else
    {

#line 122
        _S282 = -1.0;

#line 122
    }

#line 122
    return _S282;
}


float3 decodeAreaTangent_0(uint word_0)
{
    float _S283 = max(float(int(word_0 << 16U) >> 16U) / 32767.0, -1.0);
    float _S284 = max(float(int(word_0) >> 16U) / 32767.0, -1.0);
    float _S285 = 1.0 - abs(_S283) - abs(_S284);

#line 130
    thread float3 unit_0 = float3(_S283, _S284, _S285);
    if(_S285 < 0.0)
    {

        float _S286 = (1.0 - abs(unit_0.x)) * signNotZero_0(unit_0.y);
        unit_0.x = (1.0 - abs(unit_0.y)) * signNotZero_0(unit_0.x);
        unit_0.y = _S286;

#line 131
    }

#line 138
    float _S287 = dot(unit_0, unit_0);

#line 138
    float3 _S288;
    if(_S287 > 0.0)
    {

#line 139
        _S288 = unit_0 * float3(rsqrt(_S287)) ;

#line 139
    }
    else
    {

#line 139
        _S288 = float3(1.0, 0.0, 0.0);

#line 139
    }

#line 139
    return _S288;
}


#line 168
float3 facingPerpendicular_0(float3 axis_0, float3 towards_0)
{

#line 168
    float3 _S289;

    if((abs(axis_0.y)) < 0.99000000953674316)
    {

#line 170
        _S289 = float3(0.0, 1.0, 0.0);

#line 170
    }
    else
    {

#line 170
        _S289 = float3(1.0, 0.0, 0.0);

#line 170
    }
    return unitOr_0(cross(axis_0, towards_0), normalize(cross(axis_0, _S289)));
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

    uint _S290 = (light_3->kind_0) & 255U;
    float3 _S291 = light_3->positionRelativeToCamera_0 - surfaceRelativeToCamera_2;
    float3 _S292 = unitOr_0(light_3->direction_0, float3(0.0, -1.0, 0.0));


    float extentX_0 = light_3->spotScaleBias_0.x;
)cy_msl",
    R"cy_msl(    float extentY_0 = light_3->spotScaleBias_0.y;
    bool _S293 = ((light_3->kind_0) & 256U) == 0U;

#line 185
    bool singleSided_0;
    if(_S290 == 4U)
    {

#line 186
        singleSided_0 = true;

#line 186
    }
    else
    {

#line 186
        singleSided_0 = _S290 == 5U;

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
        float3 _S294 = decodeAreaTangent_0(light_3->shapeWord_0);
        float3 axisX_1 = unitOr_0(_S294 - _S292 * float3(dot(_S294, _S292)) , facingPerpendicular_0(_S292, _S291));
        float3 _S295 = cross(_S292, axisX_1);
        if(_S290 == 5U)
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
        axisY_0 = _S295;

#line 191
        singleSided_0 = _S293;

#line 186
    }
    else
    {

#line 197
        if(_S290 == 7U)
        {

#line 197
            axisX_0 = facingPerpendicular_0(_S292, _S291);

#line 197
            axisY_0 = _S292;

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

            float3 _S296 = float3(0.0, 0.0, 1.0);

#line 212
            float3 _S297 = unitOr_0(_S291, _S296);
            float3 axisX_2 = unitOr_0(cross(_S296, _S297), float3(1.0, 0.0, 0.0));
            float3 _S298 = cross(_S297, axisX_2);

#line 214
            axisX_0 = axisX_2;

#line 214
            axisY_0 = _S298;

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
    float3 _S299 = axisX_0 * float3(extentX_1) ;

#line 218
    float3 _S300 = _S291 - _S299;

#line 218
    float3 _S301 = axisY_0 * float3(extentY_1) ;

#line 218
    (&quad_0)->corner0_0 = _S300 - _S301;
    float3 _S302 = _S291 + _S299;

#line 219
    (&quad_0)->corner1_0 = _S302 - _S301;
    (&quad_0)->corner2_0 = _S302 + _S301;
    (&quad_0)->corner3_0 = _S300 + _S301;


    if(!singleSided_0)
    {

#line 224
        singleSided_0 = true;

#line 224
    }
    else
    {

#line 224
        singleSided_0 = (dot(cross(axisX_0, axisY_0), _S291)) <= 0.0;

#line 224
    }

#line 224
    (&quad_0)->lit_2 = singleSided_0;
    return quad_0;
}


float3 edgeVector_0(float3 a_2, float3 b_2)
{

    float _S303 = acos(clamp(dot(a_2, b_2), -1.0, 1.0));
    float3 _S304 = cross(a_2, b_2);
    float _S305 = length(_S304);

#line 234
    float _S306;
    if(_S305 > 9.99999997475242708e-07)
    {

#line 235
        _S306 = _S303 / _S305;

#line 235
    }
    else
    {

#line 235
        _S306 = 1.0;

#line 235
    }
    return _S304 * float3(_S306) ;
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
    float3 _S307 = float3(0.0, 0.0, 1.0);

#line 253
    (&result_5)->direction_2 = _S307;
    array<float3, int(4)> _S308 = { unitOr_0(c0_0, _S307), unitOr_0(c1_0, _S307), unitOr_0(c2_0, _S307), unitOr_0(c3_0, _S307) };

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
        uint _S309 = index_12;


        uint _S310 = index_12 + 1U;

#line 261
        uint _S311 = _S310 % 4U;
        float _S312 = _S308[index_12].z;

#line 262
        bool _S313 = _S312 > 0.0;
        float _S314 = _S308[_S311].z;

#line 263
        bool _S315 = _S314 > 0.0;

#line 263
        uint written_1;
        if(_S313)
        {
            clipped_0[written_0] = _S308[_S309];

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
        if(_S313 != _S315)
        {

            clipped_0[written_1] = _S308[_S309] + (_S308[_S311] - _S308[_S309]) * float3((_S312 / (_S312 - _S314))) ;

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
        index_12 = _S310;

#line 258
    }

#line 276
    if(written_0 < 3U)
    {
        return result_5;
    }
    float3 _S316 = float3(0.0) ;

#line 280
    index_12 = 0U;

#line 280
    float3 sum_2 = _S316;
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
        float3 _S317 = unitOr_0(clipped_0[index_12], _S307);
        uint _S318 = index_12 + 1U;

#line 284
        uint _S319 = _S318 % written_0;
        float3 sum_3 = sum_2 + edgeVector_0(_S317, unitOr_0(clipped_0[_S319], _S307));

#line 281
        index_12 = _S318;

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
    (&result_5)->direction_2 = unitOr_0(sum_2, _S307);
    return result_5;
}


float cosinePolygonIntegral_0(float3 c0_1, float3 c1_1, float3 c2_1, float3 c3_1)
{
    PolygonIntegral_0 _S320 = cosinePolygonVector_0(c0_1, c1_1, c2_1, c3_1);

#line 299
    return _S320.integral_0;
}


float areaFormFactor_0(const AreaQuad_0 thread* quad_1, float3 normal_6)
{
    if(!quad_1->lit_2)
    {
        return 0.0;
    }

#line 307
    float3 _S321;

    if((abs(normal_6.z)) < 0.99900001287460327)
    {

#line 309
        _S321 = float3(0.0, 0.0, 1.0);

#line 309
    }
    else
    {

#line 309
        _S321 = float3(1.0, 0.0, 0.0);

#line 309
    }
    float3 _S322 = unitOr_0(cross(_S321, normal_6), float3(1.0, 0.0, 0.0));
    float3 _S323 = cross(normal_6, _S322);
    float _S324 = cosinePolygonIntegral_0(float3(dot(quad_1->corner0_0, _S322), dot(quad_1->corner0_0, _S323), dot(quad_1->corner0_0, normal_6)), float3(dot(quad_1->corner1_0, _S322), dot(quad_1->corner1_0, _S323), dot(quad_1->corner1_0, normal_6)), float3(dot(quad_1->corner2_0, _S322), dot(quad_1->corner2_0, _S323), dot(quad_1->corner2_0, normal_6)), float3(dot(quad_1->corner3_0, _S322), dot(quad_1->corner3_0, _S323), dot(quad_1->corner3_0, normal_6)));

#line 312
    return _S324;
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

    float3 _S325 = unitOr_0(view_1 - normal_7 * float3(clamp(dot(normal_7, view_1), 0.00100000004749745, 1.0)) , float3(1.0, 0.0, 0.0));
    float3 _S326 = cross(normal_7, _S325);

#line 333
    float4 _S327 = ltc_0->transform_0;

    thread array<float3, int(4)> local_0;
    array<float3, int(4)> _S328 = { quad_2->corner0_0, quad_2->corner1_0, quad_2->corner2_0, quad_2->corner3_0 };

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

        float _S329 = dot(_S328[index_13], _S325);

#line 340
        float _S330 = dot(_S328[index_13], normal_7);

        local_0[index_13] = float3(_S327.x * _S329 + _S327.y * _S330, _S327.z * dot(_S328[index_13], _S326), _S327.w * _S329 + _S330);

#line 337
        index_13 = index_13 + 1U;

#line 337
    }

#line 345
    PolygonIntegral_0 _S331 = cosinePolygonVector_0(local_0[int(0)], local_0[int(1)], local_0[int(2)], local_0[int(3)]);


    float _S332 = _S327.x;

#line 348
    float _S333 = _S327.y;

#line 348
    float _S334 = _S327.w;

#line 348
    float _S335 = _S332 - _S333 * _S334;

#line 348
    bool _S336;
    if((abs(_S335)) >= 9.99999993922529029e-09)
    {

#line 349
        _S336 = (abs(_S327.z)) >= 9.99999993922529029e-09;

#line 349
    }
    else
    {

#line 349
        _S336 = false;

#line 349
    }

#line 349
    float3 gathered_0;

#line 349
    if(_S336)
    {
        float _S337 = _S331.direction_2.x;

#line 351
        float _S338 = _S331.direction_2.z;

#line 351
        gathered_0 = unitOr_0(float3((_S337 - _S333 * _S338) / _S335, _S331.direction_2.y / _S327.z, (_S332 * _S338 - _S334 * _S337) / _S335), _S331.direction_2);

#line 349
    }
    else
    {

#line 349
        gathered_0 = _S331.direction_2;

#line 349
    }

#line 355
    (&result_6)->integral_0 = _S331.integral_0;
    (&result_6)->direction_2 = unitOr_0(_S325 * float3(gathered_0.x)  + _S326 * float3(gathered_0.y)  + normal_7 * float3(gathered_0.z) , normal_7);

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
    float3 _S339 = float3(0.0) ;

#line 383
    (&result_7)->diffuse_0 = _S339;
    (&result_7)->specular_0 = _S339;

#line 384
    float _S340 = areaLightWindow_0(light_4, surfaceRelativeToCamera_3);

    if(_S340 <= 0.0)
    {
        return result_7;
    }

#line 388
    AreaQuad_0 _S341 = areaLightQuad_0(light_4, surfaceRelativeToCamera_3);


    float3 _S342 = light_4->color_0 * float3((light_4->intensity_0 * _S340)) ;
    float3 _S343 = albedo_4 * float3((1.0 - metallic_3))  * _S342;

#line 392
    thread AreaQuad_0 _S344 = _S341;

#line 392
    float _S345 = areaFormFactor_0(&_S344, normal_8);

#line 392
    (&result_7)->diffuse_0 = _S343 * float3(_S345) ;
    if(!hasLtc_0)
    {
        return result_7;
    }

#line 395
    thread AreaQuad_0 _S346 = _S341;

#line 395
    PolygonIntegral_0 _S347 = areaSpecularIntegral_0(&_S346, normal_8, view_2, ltc_1);

#line 402
    float3 _S348 = mix(float3(0.03999999910593033) , albedo_4, float3(metallic_3) );

    (&result_7)->specular_0 = (_S348 + (float3(1.0)  - _S348) * float3(pow(1.0 - saturate(dot(view_2, unitOr_0(view_2 + _S347.direction_2, normal_8))), 5.0)) ) * _S342 * float3((_S347.integral_0 * ltc_1->magnitude_0)) ;
    return result_7;
}


#line 801 "src/rendering/shaders/cy/frame.slang"
AreaShading_0 frameAreaShading_0(uint index_14, const Surface_0 thread* surface_7, float3 relativePosition_9, float3 normal_9, float3 viewDir_1, KernelContext_0 thread* kernelContext_44)
{

    bool _S349 = (kernelContext_44->cyFrameView_0->frame_0->lightingControl_0.x) != 4294967295U;
    thread AreaLtc_0 ltc_2;
    (&ltc_2)->transform_0 = float4(1.0, 0.0, 1.0, 0.0);
    (&ltc_2)->magnitude_0 = 1.0;
    if(_S349)
    {

#line 808
        AreaLtc_0 _S350 = frameAreaLtc_0(surface_7->roughness_1, clamp(dot(normal_9, viewDir_1), 0.00100000004749745, 1.0), kernelContext_44);

        ltc_2 = _S350;

#line 808
    }

#line 808
    thread Light_0 _S351 = kernelContext_44->cyFrameView_0->lights_0[index_14];

#line 808
    thread AreaLtc_0 _S352 = ltc_2;

#line 808
    AreaShading_0 _S353 = shadeAreaLight_0(&_S351, relativePosition_9, normal_9, viewDir_1, surface_7->albedo_1, surface_7->roughness_1, surface_7->metallic_1, _S349, &_S352);



    return _S353;
}


#line 834
float3 frameStochasticLighting_0(const Surface_0 thread* surface_8, float3 relativePosition_10, float3 normal_10, float3 viewDir_2, float2 fragmentCentre_4, KernelContext_0 thread* kernelContext_45)
{

    float2 _S354 = fragmentCentre_4 * kernelContext_45->cyFrameView_0->frame_0->extentAndInverse_0.zw;

#line 837
    float4 _S355 = cyMaterialSampleTextureLevel_0(kernelContext_45->cyFrameView_0->frame_0->lightingControl_0.y, _S354, 0.0, kernelContext_45);

#line 837
    float3 _S356 = surface_8->albedo_1;

#line 837
    float _S357 = surface_8->metallic_1;

    float3 lit_3 = surface_8->albedo_1 * float3((1.0 - surface_8->metallic_1))  * float3(0.31830987334251404)  * _S355.xyz;

#line 839
    float4 _S358 = cyMaterialSampleTextureLevel_0(kernelContext_45->cyFrameView_0->frame_0->lightingControl_0.z, _S354, 0.0, kernelContext_45);

    float _S359 = _S358.y;
    float _S360 = _S358.x;

#line 842
    bool _S361;

#line 842
    if(_S360 < 0.0)
    {

#line 842
        _S361 = true;

#line 842
    }
    else
    {

#line 842
        _S361 = _S359 <= 0.0;

#line 842
    }

#line 842
    if(_S361)
    {

#line 842
        _S361 = true;

#line 842
    }
    else
    {

#line 842
        _S361 = _S360 >= float(kernelContext_45->cyFrameView_0->frame_0->counts_0.x);

#line 842
    }

#line 842
    if(_S361)
    {
        return lit_3;
    }
    uint _S362 = uint(_S360);
    if(isAreaLight_0(kernelContext_45->cyFrameView_0->lights_0[_S362].kind_0))
    {

#line 847
        AreaShading_0 _S363 = frameAreaShading_0(_S362, surface_8, relativePosition_10, normal_10, viewDir_2, kernelContext_45);



        return lit_3 + _S363.specular_0 * float3(_S359) ;
    }

#line 851
    thread Light_0 _S364 = kernelContext_45->cyFrameView_0->lights_0[_S362];

#line 851
    LightSample_0 _S365 = evaluateLight_0(&_S364, relativePosition_10);

#line 858
    return lit_3 + specularGgx_0(normal_10, viewDir_2, _S365.direction_1, surface_8->roughness_1, computeF0_0(_S356, _S357)) * _S365.illuminance_0 * float3(saturate(dot(normal_10, _S365.direction_1)))  * float3(_S359) ;
}


#line 817
float3 frameAreaLight_0(uint index_15, const Surface_0 thread* surface_9, float3 relativePosition_11, float3 normal_11, float3 viewDir_3, const CyBakedLights_0 thread* baked_2, KernelContext_0 thread* kernelContext_46)
{

#line 818
    AreaShading_0 _S366 = frameAreaShading_0(index_15, surface_9, relativePosition_11, normal_11, viewDir_3, kernelContext_46);

#line 818
    float _S367;


    if(baked_2->active_0)
    {

#line 821
        float _S368 = bakedLightFactor_0(index_15, baked_2->mask_0, kernelContext_46);

#line 821
        _S367 = _S368;

#line 821
    }
    else
    {

#line 821
        _S367 = 1.0;

#line 821
    }
    return (_S366.diffuse_0 + _S366.specular_0) * float3(_S367) ;
}


#line 863
LightSample_0 bakedLightSample_0(uint index_16, float3 relativePosition_12, const CyBakedLights_0 thread* baked_3, KernelContext_0 thread* kernelContext_47)
{

#line 863
    thread Light_0 _S369 = kernelContext_47->cyFrameView_0->lights_0[index_16];

#line 863
    LightSample_0 _S370 = evaluateLight_0(&_S369, relativePosition_12);

    thread LightSample_0 sample_1 = _S370;
    if(baked_3->active_0)
    {

#line 866
        float _S371 = bakedLightFactor_0(index_16, baked_3->mask_0, kernelContext_47);

        (&sample_1)->illuminance_0 = (&sample_1)->illuminance_0 * float3(_S371) ;

#line 866
    }



    return sample_1;
}


#line 897
float3 accumulateLights_0(const Surface_0 thread* surface_10, float3 relativePosition_13, float3 normal_12, float3 viewDir_4, float2 fragmentCentre_5, uint instanceFlags_1, const CyBakedLights_0 thread* baked_4, KernelContext_0 thread* kernelContext_48)
{

    uint2 _S372 = uint2(fragmentCentre_5);
    float3 _S373 = float3(0.0) ;
    uint _S374 = kernelContext_48->cyFrameView_0->frame_0->counts_0.x;

#line 902
    uint index_17;

#line 902
    float3 lit_4;

#line 916
    if(((kernelContext_48->cyFrameView_0->frame_0->directionalLights_0.w) & 2147483648U) != 0U)
    {
)cy_msl",
    R"cy_msl(        uint _S375 = min((kernelContext_48->cyFrameView_0->frame_0->directionalLights_0.w) & 255U, 3U);

#line 918
        index_17 = 0U;

#line 918
        lit_4 = _S373;
        for(;;)
        {

#line 919
            if(index_17 < _S375)
            {
            }
            else
            {

#line 919
                break;
            }

#line 919
            float3 _S376 = directionalContribution_0(kernelContext_48->cyFrameView_0->frame_0->directionalLights_0[index_17], surface_10, relativePosition_13, normal_12, viewDir_4, fragmentCentre_5, instanceFlags_1, baked_4, kernelContext_48);

            float3 lit_5 = lit_4 + _S376;

#line 919
            index_17 = index_17 + 1U;

#line 919
            lit_4 = lit_5;

#line 919
        }

#line 916
    }
    else
    {

#line 916
        index_17 = 0U;

#line 916
        lit_4 = _S373;

#line 927
        for(;;)
        {

#line 927
            if(index_17 < _S374)
            {
            }
            else
            {

#line 927
                break;
            }
            if((kernelContext_48->cyFrameView_0->lights_0[index_17].kind_0) != 0U)
            {
                index_17 = index_17 + 1U;

#line 927
                continue;
            }

#line 927
            float3 _S377 = directionalContribution_0(index_17, surface_10, relativePosition_13, normal_12, viewDir_4, fragmentCentre_5, instanceFlags_1, baked_4, kernelContext_48);

#line 927
            lit_4 = lit_4 + _S377;

#line 927
            index_17 = index_17 + 1U;

#line 927
        }

#line 916
    }

#line 940
    if(((kernelContext_48->cyFrameView_0->frame_0->lightingControl_0.w) & 1U) != 0U)
    {

#line 940
        float3 _S378 = frameStochasticLighting_0(surface_10, relativePosition_13, normal_12, viewDir_4, fragmentCentre_5, kernelContext_48);

        return lit_4 + _S378;
    }

#line 942
    bool _S379;



    if((kernelContext_48->cyFrameView_0->frame_0->counts_0.w) == 0U)
    {

#line 946
        _S379 = true;

#line 946
    }
    else
    {

#line 946
        _S379 = ((&kernelContext_48->cyFrameView_0->frame_0->clusterGrid_0)->dimensions_0.z) == 0U;

#line 946
    }

#line 946
    if(_S379)
    {

#line 946
        index_17 = 0U;

        for(;;)
        {

#line 948
            if(index_17 < _S374)
            {
            }
            else
            {

#line 948
                break;
            }
            if((kernelContext_48->cyFrameView_0->lights_0[index_17].kind_0) == 0U)
            {

#line 950
                _S379 = true;

#line 950
            }
            else
            {

#line 950
                _S379 = (kernelContext_48->cyFrameView_0->lights_0[index_17].kind_0) == 8U;

#line 950
            }

#line 950
            if(_S379)
            {
                index_17 = index_17 + 1U;

#line 948
                continue;
            }

#line 954
            if(isAreaLight_0(kernelContext_48->cyFrameView_0->lights_0[index_17].kind_0))
            {

#line 954
                float3 _S380 = frameAreaLight_0(index_17, surface_10, relativePosition_13, normal_12, viewDir_4, baked_4, kernelContext_48);

#line 954
                lit_4 = lit_4 + _S380;


                index_17 = index_17 + 1U;

#line 948
                continue;
            }

#line 948
            LightSample_0 _S381 = bakedLightSample_0(index_17, relativePosition_13, baked_4, kernelContext_48);

#line 948
            thread LightSample_0 _S382 = _S381;

#line 948
            float3 _S383 = shadeSurfaceWithLight_0(surface_10, normal_12, viewDir_4, &_S382);

#line 948
            lit_4 = lit_4 + _S383;

#line 948
            index_17 = index_17 + 1U;

#line 948
        }

#line 962
        return lit_4;
    }

    uint2 _S384 = uint2(kernelContext_48->cyFrameView_0->frame_0->extentAndInverse_0.xy);

#line 965
    float _S385 = viewDepthOf_0(relativePosition_13, kernelContext_48);

#line 965
    uint3 _S386 = clusterCoordOf_1(&kernelContext_48->cyFrameView_0->frame_0->clusterGrid_0, _S372, _S384, _S385);

#line 965
    uint _S387 = clusterIndexOf_1(&kernelContext_48->cyFrameView_0->frame_0->clusterGrid_0, _S386);

#line 970
    uint2 _S388 = kernelContext_48->cyFrameView_0->clusterHeaders_0[_S387 * kernelContext_48->cyFrameView_0->frame_0->counts_0.z];

#line 970
    index_17 = 0U;
    for(;;)
    {

#line 971
        if(index_17 < (_S388.y))
        {
        }
        else
        {

#line 971
            break;
        }
        uint _S389 = kernelContext_48->cyFrameView_0->clusterIndices_0[_S388.x + index_17];
        if(_S389 >= _S374)
        {

#line 974
            _S379 = true;

#line 974
        }
        else
        {

#line 974
            _S379 = (kernelContext_48->cyFrameView_0->lights_0[_S389].kind_0) == 0U;

#line 974
        }

#line 974
        if(_S379)
        {
            index_17 = index_17 + 1U;

#line 971
            continue;
        }

#line 982
        float3 _S390 = kernelContext_48->cyFrameView_0->lights_0[_S389].positionRelativeToCamera_0 - relativePosition_13;
        if((dot(_S390, _S390)) >= (kernelContext_48->cyFrameView_0->lights_0[_S389].range_0 * kernelContext_48->cyFrameView_0->lights_0[_S389].range_0))
        {
            index_17 = index_17 + 1U;

#line 971
            continue;
        }

#line 987
        if(isAreaLight_0(kernelContext_48->cyFrameView_0->lights_0[_S389].kind_0))
        {

#line 987
            float3 _S391 = frameAreaLight_0(_S389, surface_10, relativePosition_13, normal_12, viewDir_4, baked_4, kernelContext_48);

#line 987
            lit_4 = lit_4 + _S391;


            index_17 = index_17 + 1U;

#line 971
            continue;
        }

#line 971
        LightSample_0 _S392 = bakedLightSample_0(_S389, relativePosition_13, baked_4, kernelContext_48);

#line 971
        thread LightSample_0 _S393 = _S392;

#line 971
        float3 _S394 = shadeSurfaceWithLight_0(surface_10, normal_12, viewDir_4, &_S393);

#line 971
        lit_4 = lit_4 + _S394;

#line 971
        index_17 = index_17 + 1U;

#line 971
    }

#line 995
    return lit_4;
}


#line 1301
float3 lightmapAmbient_0(float2 uv_17, float3 normal_13, KernelContext_0 thread* kernelContext_49)
{

#line 1301
    float4 _S395 = cyMaterialSampleTexture_0(kernelContext_49->cyFrameView_0->frame_0->lightmapControl_0.x, uv_17, kernelContext_49);


    uint _S396 = kernelContext_49->cyFrameView_0->frame_0->lightmapControl_0.w;
    if(_S396 == 2U)
    {

#line 1305
        float4 _S397 = cyMaterialSampleTexture_0(kernelContext_49->cyFrameView_0->frame_0->lightmapControl_0.y, uv_17, kernelContext_49);



        return _S395.xyz * float3((max(0.0, 1.0 + dot(_S397.xyz, normal_13)) / max(_S397.w, 0.00100000004749745))) ;
    }
    if(_S396 == 3U)
    {

#line 1311
        float4 _S398 = cyMaterialSampleTexture_0(kernelContext_49->cyFrameView_0->frame_0->lightmapControl_0.y, uv_17, kernelContext_49);

#line 1311
        float4 _S399 = cyMaterialSampleTexture_0(kernelContext_49->cyFrameView_0->frame_0->lightmapControl_0.z, uv_17, kernelContext_49);



        return float3(max(0.0, _S395.w + dot(_S395.xyz, normal_13)), max(0.0, _S398.w + dot(_S398.xyz, normal_13)), max(0.0, _S399.w + dot(_S399.xyz, normal_13)));
    }


    return _S395.xyz;
}


#line 1079
float4 probeVolumeTexel_0(uint3 probe_0, uint texel_3, KernelContext_0 thread* kernelContext_50)
{
    uint3 _S400 = kernelContext_50->cyFrameView_0->frame_0->probeVolumeControl_0.yzw;

    uint _S401 = _S400.y;

#line 1083
    float4 _S402 = cyMaterialSampleTextureLevel_0(kernelContext_50->cyFrameView_0->frame_0->probeVolumeControl_0.x, (float2(float(probe_0.x * 5U + texel_3), float(probe_0.y + _S401 * probe_0.z)) + float2(0.5) ) / float2(float(_S400.x * 5U), float(_S401 * _S400.z)), 0.0, kernelContext_50);


    return _S402;
}



float probeVisibility_0(float4 distances0_0, float4 distances1_0, float3 probePosition_0, float3 position_1, float3 normal_14, float3 query_0, KernelContext_0 thread* kernelContext_51)
{

    float3 _S403 = probePosition_0 - position_1;
    float _S404 = dot(_S403, _S403);

#line 1095
    float3 _S405;
    if(_S404 > 9.999999960041972e-13)
    {

#line 1096
        _S405 = _S403 * float3(rsqrt(_S404)) ;

#line 1096
    }
    else
    {

#line 1096
        _S405 = normal_14;

#line 1096
    }
    float _S406 = max(0.00009999999747379, (dot(_S405, normal_14) + 1.0) * 0.5);
    float _S407 = _S406 * _S406 + 0.20000000298023224;



    float3 _S408 = query_0 - probePosition_0;

#line 1102
    float _S409;
    if((_S408.x) >= 0.0)
    {

#line 1103
        _S409 = distances0_0.y;

#line 1103
    }
    else
    {

#line 1103
        _S409 = distances0_0.z;

#line 1103
    }

#line 1103
    float _S410;
    if((_S408.y) >= 0.0)
    {

#line 1104
        _S410 = distances0_0.w;

#line 1104
    }
    else
    {

#line 1104
        _S410 = distances1_0.x;

#line 1104
    }

#line 1104
    float _S411;
    if((_S408.z) >= 0.0)
    {

#line 1105
        _S411 = distances1_0.y;

#line 1105
    }
    else
    {

#line 1105
        _S411 = distances1_0.z;

#line 1105
    }

#line 1105
    float3 _S412 = float3(kernelContext_51->cyFrameView_0->frame_0->probeVolumeParams_0.z) ;

    float3 _S413 = saturate((float3(_S409, _S410, _S411) + _S412 - abs(_S408)) / _S412);
    return _S407 * min(_S413.x, min(_S413.y, _S413.z));
}



float3 probeVolumeAmbient_0(float3 position_2, float3 normal_15, float3 flat_0, KernelContext_0 thread* kernelContext_52)
{
    uint3 _S414 = kernelContext_52->cyFrameView_0->frame_0->probeVolumeControl_0.yzw;

    float3 _S415 = position_2 + normal_15 * float3(kernelContext_52->cyFrameView_0->frame_0->probeVolumeParams_0.y) ;

#line 1117
    float3 _S416 = float3(kernelContext_52->cyFrameView_0->frame_0->probeVolumeOrigin_0.w) ;
    float3 _S417 = (_S415 - kernelContext_52->cyFrameView_0->frame_0->probeVolumeOrigin_0.xyz) / _S416;

#line 1118
    float3 _S418 = float3(1.0) ;
    float3 _S419 = float3(_S414) - _S418;
    float3 _S420 = float3(0.0) ;

#line 1120
    float3 _S421 = max(max(- _S417, _S417 - _S419), _S420);
    float _S422 = saturate(1.0 - max(_S421.x, max(_S421.y, _S421.z)));
    if(_S422 <= 0.0)
    {
        return flat_0;
    }
    float3 _S423 = clamp(_S417, _S420, _S419);
    uint3 _S424 = min(uint3(floor(_S423)), uint3(max(int3(_S414) - int3(int(2)) , int3(int(0)) )));
    float3 _S425 = _S423 - float3(_S424);


    float4 _S426 = float4(0.28209498524665833, 0.32573533058166504 * normal_15.y, 0.32573533058166504 * normal_15.z, 0.32573533058166504 * normal_15.x);

#line 1131
    uint corner_1 = 0U;

#line 1131
    float3 total_0 = _S420;

#line 1131
    float weightSum_0 = 0.0;



    for(;;)
    {

#line 1135
        if(corner_1 < 8U)
        {
        }
        else
        {

#line 1135
            break;
        }
        uint3 _S427 = uint3(corner_1 & 1U, (corner_1 >> 1U) & 1U, (corner_1 >> 2U) & 1U);
        uint3 _S428 = min(_S424 + _S427, _S414 - uint3(1U) );
        float3 _S429 = float3(_S427);
        float3 _S430 = _S429 * _S425 + (_S418 - _S429) * (_S418 - _S425);

#line 1140
        float4 _S431 = probeVolumeTexel_0(_S428, 3U, kernelContext_52);

        float _S432 = _S430.x * _S430.y * _S430.z * _S431.x;
        if(_S432 <= 0.0)
        {
            corner_1 = corner_1 + 1U;

#line 1135
            continue;
        }

#line 1135
        float4 _S433 = probeVolumeTexel_0(_S428, 4U, kernelContext_52);

#line 1135
        float _S434 = probeVisibility_0(_S431, _S433, kernelContext_52->cyFrameView_0->frame_0->probeVolumeOrigin_0.xyz + float3(_S428) * _S416, position_2, normal_15, _S415, kernelContext_52);

#line 1150
        float _S435 = _S432 * _S434;

#line 1150
        float4 _S436 = probeVolumeTexel_0(_S428, 0U, kernelContext_52);
        float _S437 = dot(_S436, _S426);

#line 1151
        float4 _S438 = probeVolumeTexel_0(_S428, 1U, kernelContext_52);
        float _S439 = dot(_S438, _S426);

#line 1152
        float4 _S440 = probeVolumeTexel_0(_S428, 2U, kernelContext_52);



        float weightSum_1 = weightSum_0 + _S435;

#line 1156
        total_0 = total_0 + max(float3(_S437, _S439, dot(_S440, _S426)) * float3(kernelContext_52->cyFrameView_0->frame_0->probeVolumeParams_0.x) , _S420) * float3(_S435) ;

#line 1156
        weightSum_0 = weightSum_1;

#line 1135
        corner_1 = corner_1 + 1U;

#line 1135
    }

#line 1158
    if(weightSum_0 > 9.99999997475242708e-07)
    {

#line 1158
        total_0 = total_0 / float3(weightSum_0) ;

#line 1158
    }
    else
    {

#line 1158
        total_0 = flat_0;

#line 1158
    }
    return mix(flat_0, total_0, float3(_S422) );
}


#line 1013
float3 frameGiSample_0(uint slot_4, float2 fragmentCentre_6, KernelContext_0 thread* kernelContext_53)
{

#line 1013
    float4 _S441 = cyMaterialSampleTextureLevel_0(slot_4, fragmentCentre_6 * kernelContext_53->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_53);

    return _S441.xyz;
}


#line 1024
float3 frameGiAmbient_0(bool lightmapped_4, float2 fragmentCentre_7, float3 relativePosition_14, float3 normal_16, float3 ambient_0, KernelContext_0 thread* kernelContext_54)
{

    uint _S442 = kernelContext_54->cyFrameView_0->frame_0->giControl_0.z;

#line 1027
    float3 _S443 = frameGiSample_0(kernelContext_54->cyFrameView_0->frame_0->giControl_0.x, fragmentCentre_7, kernelContext_54);

#line 1027
    float3 _S444;

    if(lightmapped_4)
    {
        if((_S442 & 2U) != 0U)
        {

#line 1031
            _S444 = _S443;

#line 1031
        }
        else
        {

#line 1031
            _S444 = ambient_0;

#line 1031
        }

#line 1031
        return _S444;
    }
    if((kernelContext_54->cyFrameView_0->frame_0->probeVolumeControl_0.x) != 4294967295U)
    {
        if((_S442 & 4U) != 0U)
        {
            return _S443;
        }
        if((_S442 & 1U) != 0U)
        {

#line 1039
            float3 _S445 = probeVolumeAmbient_0(relativePosition_14, normal_16, _S443, kernelContext_54);

#line 1039
            _S444 = _S445;

#line 1039
        }
        else
        {

#line 1039
            _S444 = ambient_0;

#line 1039
        }

#line 1039
        return _S444;
    }

    if((_S442 & 1U) != 0U)
    {

#line 1042
        _S444 = _S443;

#line 1042
    }
    else
    {

#line 1042
        _S444 = ambient_0;

#line 1042
    }

#line 1042
    return _S444;
}



bool frameGiAdmits_0(bool lightmapped_5, KernelContext_0 thread* kernelContext_55)
{
    uint _S446 = kernelContext_55->cyFrameView_0->frame_0->giControl_0.z;
    if(lightmapped_5)
    {
        return (_S446 & 2U) != 0U;
    }
    return (_S446 & 5U) != 0U;
}




float3 frameEnvironmentBrdf_0(float3 f0_2, float roughness_7, float normalDotView_1)
{


    float4 _S447 = float4(roughness_7)  * float4(-1.0, -0.02749999985098839, -0.57200002670288086, 0.02199999988079071) + float4(1.0, 0.04250000044703484, 1.03999996185302734, -0.03999999910593033);
    float _S448 = _S447.x;
    float2 _S449 = float2(-1.03999996185302734, 1.03999996185302734) * float2((min(_S448 * _S448, exp2(-9.27999973297119141 * normalDotView_1)) * _S448 + _S447.y))  + _S447.zw;
    return f0_2 * float3(_S449.x)  + float3(_S449.y) ;
}


#line 1000
float occlusionVisibility_0(float2 fragmentCentre_8, KernelContext_0 thread* kernelContext_56)
{

#line 1000
    float4 _S450 = cyMaterialSampleTextureLevel_0(kernelContext_56->cyFrameView_0->frame_0->occlusionControl_0.x, fragmentCentre_8 * kernelContext_56->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_56);


    return _S450.w;
}


#line 612
struct CyForwardVertex_0
{
    float4 position_3;
    float3 relativePosition_15;
    float3 normal_17;
    float2 uv_18;
    [[flat]] uint drawIndex_0;
    float2 lightmapUv_0;
};


#line 1418
float4 cyShadeForward_0(const Surface_0 thread* resolved_0, const CyForwardVertex_0 thread* input_0, KernelContext_0 thread* kernelContext_57)
{
    CyDrawInstance_0 _S451 = kernelContext_57->cyFrameView_0->drawInstances_0[input_0->drawIndex_0];
    thread Surface_0 surface_11 = *resolved_0;

#line 1421
)cy_msl",
    R"cy_msl(    bool _S452;


    if((_S451.giAddress_0) != 0U)
    {

#line 1424
        _S452 = (kernelContext_57->cyFrameView_0->frame_0->lightmapControl_0.w) != 0U;

#line 1424
    }
    else
    {

#line 1424
        _S452 = false;

#line 1424
    }

#line 1424
    float2 _S453;
    if(_S452)
    {

#line 1425
        float2 _S454 = lightmapCoordinate_0(_S451.giAddress_0, input_0->lightmapUv_0, kernelContext_57);

#line 1425
        _S453 = _S454;

#line 1425
    }
    else
    {

#line 1425
        _S453 = float2(0.0) ;

#line 1425
    }

    if((kernelContext_57->cyFrameView_0->frame_0->lightmapDebug_0.x) == 1U)
    {

#line 1427
        float3 _S455 = lightmapDensityColour_0(_S452, _S453, input_0->relativePosition_15, kernelContext_57);

        return float4(_S455, 1.0);
    }
    if((kernelContext_57->cyFrameView_0->frame_0->lightmapDebug_0.x) == 2U)
    {

#line 1431
        float3 _S456 = lightmapChartColour_0(_S452, _S451.giAddress_0, _S453, kernelContext_57);

        return float4(_S456, 1.0);
    }
    if((kernelContext_57->cyFrameView_0->frame_0->lightmapDebug_0.x) == 3U)
    {

#line 1435
        float4 _S457 = cyMaterialSampleTextureLevel_0(kernelContext_57->cyFrameView_0->frame_0->lightmapDebug_0.z, input_0->position_3.xy * kernelContext_57->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_57);

#line 1441
        if((_S457.w) > 0.0)
        {
            float3 _S458 = _S457.xyz;

#line 1443
            float _S459 = frameExposureStops_0(kernelContext_57);

#line 1443
            return float4(_S458 * float3(exp2(- _S459)) , 1.0);
        }

#line 1435
    }

#line 1447
    thread float3 normal_18 = normalize(input_0->normal_17);



    if((kernelContext_57->cyFrameView_0->frame_0->decalControl_0.x) != 4294967295U)
    {

#line 1451
        applyFrameDecals_0(&surface_11, &normal_18, input_0->relativePosition_15, input_0->position_3.xy, _S451.layerMask_1, kernelContext_57);

#line 1451
    }

#line 1451
    float3 _S460 = input_0->relativePosition_15;

#line 1458
    float3 _S461 = normalize(- input_0->relativePosition_15);

#line 1458
    CyBakedLights_0 _S462 = bakedLightsOf_0(_S452, _S453, kernelContext_57);

#line 1465
    float2 _S463 = input_0->position_3.xy;

#line 1465
    thread Surface_0 _S464 = surface_11;

#line 1465
    thread CyBakedLights_0 _S465 = _S462;

#line 1465
    float3 _S466 = accumulateLights_0(&_S464, _S460, normal_18, _S461, _S463, _S451.flags_0, &_S465, kernelContext_57);

#line 1479
    float3 ambientRadiance_0 = kernelContext_57->cyFrameView_0->frame_0->ambientAndOcclusion_0.xyz;

#line 1479
    float3 ambientRadiance_1;
    if(_S452)
    {

#line 1480
        float3 _S467 = lightmapAmbient_0(_S453, normal_18, kernelContext_57);

#line 1480
        ambientRadiance_1 = _S467;

#line 1480
    }
    else
    {

        if((kernelContext_57->cyFrameView_0->frame_0->probeVolumeControl_0.x) != 4294967295U)
        {

#line 1484
            float3 _S468 = probeVolumeAmbient_0(_S460, normal_18, ambientRadiance_0, kernelContext_57);

#line 1484
            ambientRadiance_1 = _S468;

#line 1484
        }
        else
        {

#line 1484
            ambientRadiance_1 = ambientRadiance_0;

#line 1484
        }

#line 1480
    }

#line 1491
    float3 _S469 = float3(0.0) ;
    bool _S470 = (kernelContext_57->cyFrameView_0->frame_0->giControl_0.x) != 4294967295U;

#line 1492
    float3 indirectSpecular_0;
    if(_S470)
    {

#line 1493
        float3 _S471 = frameGiAmbient_0(_S452, _S463, _S460, normal_18, ambientRadiance_1, kernelContext_57);



        if((kernelContext_57->cyFrameView_0->frame_0->giControl_0.y) != 4294967295U)
        {

#line 1497
            bool _S472 = frameGiAdmits_0(_S452, kernelContext_57);

#line 1497
            _S452 = _S472;

#line 1497
        }
        else
        {

#line 1497
            _S452 = false;

#line 1497
        }

#line 1497
        if(_S452)
        {
            float3 _S473 = frameEnvironmentBrdf_0(computeF0_0((&surface_11)->albedo_1, (&surface_11)->metallic_1), (&surface_11)->roughness_1, saturate(dot(normal_18, _S461)));

#line 1499
            float3 _S474 = frameGiSample_0(kernelContext_57->cyFrameView_0->frame_0->giControl_0.y, _S463, kernelContext_57);

#line 1499
            indirectSpecular_0 = _S474 * _S473 * float3((&surface_11)->occlusion_0) ;

#line 1497
        }
        else
        {

#line 1497
            indirectSpecular_0 = _S469;

#line 1497
        }

#line 1497
        ambientRadiance_1 = _S471;

#line 1493
    }
    else
    {

#line 1493
        indirectSpecular_0 = _S469;

#line 1493
    }

#line 1506
    float3 ambient_1 = (&surface_11)->albedo_1 * ambientRadiance_1 * float3((&surface_11)->occlusion_0) ;

#line 1506
    float3 color_1;

#line 1506
    float3 ambient_2;
    if((kernelContext_57->cyFrameView_0->frame_0->occlusionControl_0.x) != 4294967295U)
    {

#line 1507
        float _S475 = occlusionVisibility_0(_S463, kernelContext_57);

#line 1507
        float3 _S476 = float3(_S475) ;


        float3 ambient_3 = ambient_1 * _S476;

        float3 indirectSpecular_1 = indirectSpecular_0 * _S476;
        if((kernelContext_57->cyFrameView_0->frame_0->occlusionControl_0.y) != 0U)
        {

#line 1513
            color_1 = _S466 * float3(mix(1.0, _S475, (as_type<float>((kernelContext_57->cyFrameView_0->frame_0->occlusionControl_0.z))))) ;

#line 1513
        }
        else
        {

#line 1513
            color_1 = _S466;

#line 1513
        }

#line 1513
        ambient_2 = ambient_3;

#line 1513
        indirectSpecular_0 = indirectSpecular_1;

#line 1507
    }
    else
    {

#line 1507
        color_1 = _S466;

#line 1507
        ambient_2 = ambient_1;

#line 1507
    }

#line 1519
    float3 color_2 = color_1 + (&surface_11)->emission_1 + ambient_2;
    if(_S470)
    {

#line 1520
        color_1 = color_2 + indirectSpecular_0;

#line 1520
    }
    else
    {

#line 1520
        color_1 = color_2;

#line 1520
    }

#line 1527
    if((kernelContext_57->cyFrameView_0->frame_0->volumetricFogControl_0.x) != 4294967295U)
    {
        thread CyFrameFogVolume_0 fog_0;
        (&fog_0)->slot_0 = kernelContext_57->cyFrameView_0->frame_0->volumetricFogControl_0.x;

#line 1530
        thread CyFrameFogVolume_0 _S477 = fog_0;

#line 1530
        float3 _S478 = cyApplyFog_0(&_S477, color_1, _S460, kernelContext_57);

#line 1530
        color_1 = _S478;

#line 1527
    }

#line 1533
    return float4(color_1, (&surface_11)->opacity_0);
}


#line 1533
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 1533
struct pixelInput_0
{
    float3 relativePosition_16 [[user(TEXCOORD)]];
    float3 normal_19 [[user(TEXCOORD_1)]];
    float2 uv_19 [[user(TEXCOORD_2)]];
    [[flat]] uint drawIndex_1 [[user(TEXCOORD_3)]];
    float2 lightmapUv_1 [[user(TEXCOORD_4)]];
};


#line 1537
[[fragment]] pixelOutput_0 cyForwardFragment(pixelInput_0 _S479 [[stage_in]], float4 position_4 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyFrameGlobalSet_default_0 constant& cyFrameGlobals_1 [[buffer(0)]])
{

#line 1537
    thread KernelContext_0 kernelContext_58;

#line 1537
    (&kernelContext_58)->cyFrameView_0 = cyFrameView_1;

#line 1537
    (&kernelContext_58)->cyFrameGlobals_0 = &cyFrameGlobals_1;

    CyDrawInstance_0 _S480 = cyFrameView_1->drawInstances_0[_S479.drawIndex_1];

#line 1539
    Surface_0 _S481 = surfaceOf_0(_S480.material_0, cyFrameView_1->instances_0[_S480.instanceSlot_0].tint_0.xyz, _S479.uv_19, &kernelContext_58);

#line 1539
    thread Surface_0 _S482 = _S481;

#line 1539
    thread CyForwardVertex_0 _S483;

#line 1539
    (&_S483)->position_3 = position_4;

#line 1539
    (&_S483)->relativePosition_15 = _S479.relativePosition_16;

#line 1539
    (&_S483)->normal_17 = _S479.normal_19;

#line 1539
    (&_S483)->uv_18 = _S479.uv_19;

#line 1539
    (&_S483)->drawIndex_0 = _S479.drawIndex_1;

#line 1539
    (&_S483)->lightmapUv_0 = _S479.lightmapUv_1;

#line 1539
    float4 _S484 = cyShadeForward_0(&_S482, &_S483, &kernelContext_58);

#line 1539
    pixelOutput_0 _S485 = { _S484 };

    return _S485;
}

)cy_msl");
inline constexpr const auto& kFrameForwardFragmentMsl = kFrameForwardFragmentMslText.text;

/// SkinnedDepthVertex.metal, 8005 bytes.
inline constexpr char kFrameSkinnedDepthVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 334 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 414
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


#line 321 "src/rendering/shaders/cy/frame.slang"
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


#line 343
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


#line 388
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


#line 398 "src/rendering/shaders/cy/frame.slang"
float4 transformToClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow3_0, _S2));
}


#line 558
float3 previousRelativeOf_0(uint slot_0, const CyInstanceTransform_0 thread* current_0, float3 previousModelPosition_0, KernelContext_0 thread* kernelContext_1)
{
    uint _S3 = kernelContext_1->cyFrameView_0->frame_0->motionControl_0.x;

#line 560
    CyInstanceTransform_0 _S4;
    if(_S3 == 4294967295U)
    {

#line 561
        _S4 = *current_0;

#line 561
    }
    else
    {

#line 561
        _S4 = kernelContext_1->cyFrameView_0->instances_0[_S3 + slot_0];

#line 561
    }

#line 561
    thread CyInstanceTransform_0 _S5 = _S4;

#line 561
    float3 _S6 = transformToRelative_0(&_S5, previousModelPosition_0);
    return _S6;
}


#line 1567
float2 cySkinnedNormalToStream_0(float4 frame_1)
{

#line 1567
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


#line 423 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 425
struct cySkinnedDepthVertex_Result_0
{
    float4 position_0 [[position]];
    float3 normal_1 [[user(TEXCOORD)]];
    float4 currentClip_0 [[user(TEXCOORD_1)]];
    float4 previousClip_0 [[user(TEXCOORD_2)]];
};


#line 425
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 frame_2 [[attribute(1)]];
    float3 previousModelPosition_1 [[attribute(2)]];
};


#line 541
struct CyDepthVertex_0
{
    float4 position_1;
    float3 normal_2;
    float4 currentClip_1;
    float4 previousClip_1;
};


#line 541
[[vertex]] cySkinnedDepthVertex_Result_0 cySkinnedDepthVertex(vertexInput_0 _S14 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 541
    thread KernelContext_0 kernelContext_2;

#line 541
    (&kernelContext_2)->cyFrameView_0 = cyFrameView_1;

#line 541
    (&kernelContext_2)->cyDraw_0 = cyDraw_1;

#line 1576
    CyDrawInstance_0 _S15 = cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0];
    CyInstanceTransform_0 _S16 = cyFrameView_1->instances_0[_S15.instanceSlot_0];

#line 1577
    thread CyInstanceTransform_0 _S17 = _S16;

#line 1577
    float3 _S18 = transformToRelative_0(&_S17, _S14.modelPosition_1);
    thread CyDepthVertex_0 output_0;

#line 1578
    float4 _S19 = transformToClip_0(_S18, &kernelContext_2);

    (&output_0)->position_1 = _S19;
    (&output_0)->currentClip_1 = _S19;

#line 1581
    thread CyInstanceTransform_0 _S20 = _S16;

#line 1581
    float3 _S21 = previousRelativeOf_0(_S15.instanceSlot_0, &_S20, _S14.previousModelPosition_1, &kernelContext_2);
    float4 _S22 = float4(_S21, 1.0);
    (&output_0)->previousClip_1 = float4(dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow0_0, _S22), dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow1_0, _S22), dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow2_0, _S22), dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow3_0, _S22));



    float3 _S23 = decodeOctahedral_0(cySkinnedNormalToStream_0(_S14.frame_2));

#line 1587
    thread CyInstanceTransform_0 _S24 = _S16;

#line 1587
    float3 _S25 = rotateToRelative_0(&_S24, _S23);

#line 1587
    (&output_0)->normal_2 = _S25;

#line 1587
    thread cySkinnedDepthVertex_Result_0 _S26;

#line 1587
    (&_S26)->position_0 = output_0.position_1;

#line 1587
    (&_S26)->normal_1 = output_0.normal_2;

#line 1587
    (&_S26)->currentClip_0 = output_0.currentClip_1;

#line 1587
    (&_S26)->previousClip_0 = output_0.previousClip_1;

#line 1587
    return _S26;
}

)cy_msl";

/// SkinnedForwardVertex.metal, 7129 bytes.
inline constexpr char kFrameSkinnedForwardVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 334 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 414
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


#line 321 "src/rendering/shaders/cy/frame.slang"
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


#line 343
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


#line 388
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


#line 398 "src/rendering/shaders/cy/frame.slang"
float4 transformToClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow3_0, _S2));
}


#line 1567
float2 cySkinnedNormalToStream_0(float4 frame_1)
{

#line 1567
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


#line 423 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 425
struct cySkinnedForwardVertex_Result_0
{
    float4 position_0 [[position]];
    float3 relativePosition_0 [[user(TEXCOORD)]];
    float3 normal_1 [[user(TEXCOORD_1)]];
    float2 uv_0 [[user(TEXCOORD_2)]];
    uint drawIndex_1 [[user(TEXCOORD_3)]];
    float2 lightmapUv_0 [[user(TEXCOORD_4)]];
};


#line 425
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 frame_2 [[attribute(1)]];
    float2 uv_1 [[attribute(2)]];
    float2 lightmapUv_1 [[attribute(3)]];
};


#line 612
struct CyForwardVertex_0
{
    float4 position_1;
    float3 relativePosition_1;
    float3 normal_2;
    float2 uv_2;
    [[flat]] uint drawIndex_2;
    float2 lightmapUv_2;
};


#line 612
[[vertex]] cySkinnedForwardVertex_Result_0 cySkinnedForwardVertex(vertexInput_0 _S10 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 612
    thread KernelContext_0 kernelContext_1;

#line 612
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 612
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 1596
    CyInstanceTransform_0 _S11 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

    thread CyForwardVertex_0 output_0;

#line 1598
    thread CyInstanceTransform_0 _S12 = _S11;

#line 1598
    float3 _S13 = transformToRelative_0(&_S12, _S10.modelPosition_1);
    (&output_0)->relativePosition_1 = _S13;

#line 1599
    float4 _S14 = transformToClip_0(_S13, &kernelContext_1);
    (&output_0)->position_1 = _S14;
    float3 _S15 = decodeOctahedral_0(cySkinnedNormalToStream_0(_S10.frame_2));

#line 1601
    thread CyInstanceTransform_0 _S16 = _S11;

#line 1601
    float3 _S17 = rotateToRelative_0(&_S16, _S15);

#line 1601
    (&output_0)->normal_2 = _S17;
    (&output_0)->uv_2 = _S10.uv_1;
    (&output_0)->drawIndex_2 = cyDraw_1->drawIndex_0;
    (&output_0)->lightmapUv_2 = _S10.lightmapUv_1;

#line 1604
    thread cySkinnedForwardVertex_Result_0 _S18;

#line 1604
    (&_S18)->position_0 = output_0.position_1;

#line 1604
    (&_S18)->relativePosition_0 = output_0.relativePosition_1;

#line 1604
    (&_S18)->normal_1 = output_0.normal_2;

#line 1604
    (&_S18)->uv_0 = output_0.uv_2;

#line 1604
    (&_S18)->drawIndex_1 = output_0.drawIndex_2;

#line 1604
    (&_S18)->lightmapUv_0 = output_0.lightmapUv_2;

#line 1604
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

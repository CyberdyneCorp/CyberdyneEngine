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

/// DepthVertex.metal, 7698 bytes.
inline constexpr char kFrameDepthVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 295 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 375
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


#line 282 "src/rendering/shaders/cy/frame.slang"
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


#line 304
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


#line 349
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


#line 359 "src/rendering/shaders/cy/frame.slang"
float4 transformToClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->relativeToClipRow3_0, _S2));
}


#line 519
float3 previousRelativeOf_0(uint slot_0, const CyInstanceTransform_0 thread* current_0, float3 previousModelPosition_0, KernelContext_0 thread* kernelContext_1)
{
    uint _S3 = kernelContext_1->cyFrameView_0->frame_0->motionControl_0.x;

#line 521
    CyInstanceTransform_0 _S4;
    if(_S3 == 4294967295U)
    {

#line 522
        _S4 = *current_0;

#line 522
    }
    else
    {

#line 522
        _S4 = kernelContext_1->cyFrameView_0->instances_0[_S3 + slot_0];

#line 522
    }

#line 522
    thread CyInstanceTransform_0 _S5 = _S4;

#line 522
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


#line 384 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 386
struct cyDepthVertex_Result_0
{
    float4 position_0 [[position]];
    float3 normal_1 [[user(TEXCOORD)]];
    float4 currentClip_0 [[user(TEXCOORD_1)]];
    float4 previousClip_0 [[user(TEXCOORD_2)]];
};


#line 386
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
    float3 previousModelPosition_1 [[attribute(2)]];
};


#line 502
struct CyDepthVertex_0
{
    float4 position_1;
    float3 normal_2;
    float4 currentClip_1;
    float4 previousClip_1;
};


#line 502
[[vertex]] cyDepthVertex_Result_0 cyDepthVertex(vertexInput_0 _S13 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 502
    thread KernelContext_0 kernelContext_2;

#line 502
    (&kernelContext_2)->cyFrameView_0 = cyFrameView_1;

#line 502
    (&kernelContext_2)->cyDraw_0 = cyDraw_1;

#line 530
    CyDrawInstance_0 _S14 = cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0];
    CyInstanceTransform_0 _S15 = cyFrameView_1->instances_0[_S14.instanceSlot_0];

#line 531
    thread CyInstanceTransform_0 _S16 = _S15;

#line 531
    float3 _S17 = transformToRelative_0(&_S16, _S13.modelPosition_1);
    thread CyDepthVertex_0 output_0;

#line 532
    float4 _S18 = transformToClip_0(_S17, &kernelContext_2);

    (&output_0)->position_1 = _S18;
    (&output_0)->currentClip_1 = _S18;

#line 535
    thread CyInstanceTransform_0 _S19 = _S15;

#line 535
    float3 _S20 = previousRelativeOf_0(_S14.instanceSlot_0, &_S19, _S13.previousModelPosition_1, &kernelContext_2);
    float4 _S21 = float4(_S20, 1.0);
    (&output_0)->previousClip_1 = float4(dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow0_0, _S21), dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow1_0, _S21), dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow2_0, _S21), dot((&kernelContext_2)->cyFrameView_0->frame_0->previousRelativeToClipRow3_0, _S21));



    float3 _S22 = decodeOctahedral_0(_S13.packedNormal_0.xy);

#line 541
    thread CyInstanceTransform_0 _S23 = _S15;

#line 541
    float3 _S24 = rotateToRelative_0(&_S23, _S22);

#line 541
    (&output_0)->normal_2 = _S24;

#line 541
    thread cyDepthVertex_Result_0 _S25;

#line 541
    (&_S25)->position_0 = output_0.position_1;

#line 541
    (&_S25)->normal_1 = output_0.normal_2;

#line 541
    (&_S25)->currentClip_0 = output_0.currentClip_1;

#line 541
    (&_S25)->previousClip_0 = output_0.previousClip_1;

#line 541
    return _S25;
}

)cy_msl";

/// DepthFragment.metal, 4185 bytes.
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


#line 545 "src/rendering/shaders/cy/frame.slang"
struct CyDepthOutput_0
{
    float4 normalRoughness_0 [[color(0)]];
    float2 velocity_0 [[color(1)]];
};


#line 545
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


#line 282 "src/rendering/shaders/cy/frame.slang"
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


#line 552
[[fragment]] CyDepthOutput_0 cyDepthFragment(pixelInput_0 _S4 [[stage_in]], float4 position_0 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_0 [[buffer(1)]])
{
    thread CyDepthOutput_0 output_0;
    (&output_0)->normalRoughness_0 = float4(encodeOctahedral_0(normalize(_S4.normal_1)), 1.0, 1.0);

#line 563
    float2 _S5 = _S4.currentClip_0.xy / float2(_S4.currentClip_0.w)  - cyFrameView_0->frame_0->temporalJitter_0.xy * float2(2.0)  * cyFrameView_0->frame_0->extentAndInverse_0.zw;

    float2 _S6 = _S4.previousClip_0.xy / float2(_S4.previousClip_0.w) ;
    (&output_0)->velocity_0 = float2((_S6.x - _S5.x) * 0.5, (_S5.y - _S6.y) * 0.5);

    return output_0;
}

)cy_msl";

/// ShadowVertex.metal, 4774 bytes.
inline constexpr char kFrameShadowVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 295 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 375
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


#line 282 "src/rendering/shaders/cy/frame.slang"
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


#line 304
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


#line 349
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


#line 366 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow0_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow1_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow2_0, _S2), dot(kernelContext_0->cyFrameView_0->frame_0->shadowToClipRow3_0, _S2));
}


#line 369
struct cyShadowVertex_Result_0
{
    float4 position_0 [[position]];
};


#line 369
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
};


#line 477
struct CyShadowVertex_0
{
    float4 position_1;
};


#line 477
[[vertex]] cyShadowVertex_Result_0 cyShadowVertex(vertexInput_0 _S3 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 477
    thread KernelContext_0 kernelContext_1;

#line 477
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 477
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 487
    thread CyShadowVertex_0 output_0;

#line 487
    thread CyInstanceTransform_0 _S4 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

#line 487
    float3 _S5 = transformToRelative_0(&_S4, _S3.modelPosition_1);

#line 487
    float4 _S6 = transformToShadowClip_0(_S5, &kernelContext_1);
    (&output_0)->position_1 = _S6;

#line 488
    thread cyShadowVertex_Result_0 _S7;

#line 488
    (&_S7)->position_0 = output_0.position_1;

#line 488
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


#line 493 "src/rendering/shaders/cy/frame.slang"
[[fragment]] pixelOutput_0 cyShadowFragment(float4 position_0 [[position]])
{

#line 493
    pixelOutput_0 _S1 = { position_0.z };

    return _S1;
}

)cy_msl";

/// ForwardVertex.metal, 6824 bytes.
inline constexpr char kFrameForwardVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 295 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 375
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


#line 282 "src/rendering/shaders/cy/frame.slang"
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


#line 304
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


#line 349
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


#line 359 "src/rendering/shaders/cy/frame.slang"
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


#line 384 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_1)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_1), dot(instance_1->row1_0.xyz, direction_1), dot(instance_1->row2_0.xyz, direction_1)));
}


#line 386
struct cyForwardVertex_Result_0
{
    float4 position_0 [[position]];
    float3 relativePosition_0 [[user(TEXCOORD)]];
    float3 normal_1 [[user(TEXCOORD_1)]];
    float2 uv_0 [[user(TEXCOORD_2)]];
    uint drawIndex_1 [[user(TEXCOORD_3)]];
    float2 lightmapUv_0 [[user(TEXCOORD_4)]];
};


#line 386
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
    float2 uv_1 [[attribute(2)]];
    float2 lightmapUv_1 [[attribute(3)]];
};


#line 573
struct CyForwardVertex_0
{
    float4 position_1;
    float3 relativePosition_1;
    float3 normal_2;
    float2 uv_2;
    [[flat]] uint drawIndex_2;
    float2 lightmapUv_2;
};


#line 573
[[vertex]] cyForwardVertex_Result_0 cyForwardVertex(vertexInput_0 _S9 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(3)]])
{

#line 573
    thread KernelContext_0 kernelContext_1;

#line 573
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 573
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 594
    CyInstanceTransform_0 _S10 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[cyDraw_1->drawIndex_0].instanceSlot_0];

    thread CyForwardVertex_0 output_0;

#line 596
    thread CyInstanceTransform_0 _S11 = _S10;

#line 596
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    (&output_0)->relativePosition_1 = _S12;

#line 597
    float4 _S13 = transformToClip_0(_S12, &kernelContext_1);
    (&output_0)->position_1 = _S13;



    float3 _S14 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 602
    thread CyInstanceTransform_0 _S15 = _S10;

#line 602
    float3 _S16 = rotateToRelative_0(&_S15, _S14);

#line 602
    (&output_0)->normal_2 = _S16;
    (&output_0)->uv_2 = _S9.uv_1;
    (&output_0)->drawIndex_2 = cyDraw_1->drawIndex_0;
    (&output_0)->lightmapUv_2 = _S9.lightmapUv_1;

#line 605
    thread cyForwardVertex_Result_0 _S17;

#line 605
    (&_S17)->position_0 = output_0.position_1;

#line 605
    (&_S17)->relativePosition_0 = output_0.relativePosition_1;

#line 605
    (&_S17)->normal_1 = output_0.normal_2;

#line 605
    (&_S17)->uv_0 = output_0.uv_2;

#line 605
    (&_S17)->drawIndex_1 = output_0.drawIndex_2;

#line 605
    (&_S17)->lightmapUv_0 = output_0.lightmapUv_2;

#line 605
    return _S17;
}

)cy_msl";

/// ForwardFragment.metal, 73084 bytes.
inline constexpr auto kFrameForwardFragmentMslText = detail::join_msl(
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 997 "src/rendering/shaders/cy/frame.slang"
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


#line 282 "src/rendering/shaders/cy/frame.slang"
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


#line 1000 "src/rendering/shaders/cy/frame.slang"
float4 CyFrameFogVolume_texel_0(const CyFrameFogVolume_0 thread* this_0, int2 coordinate_0, KernelContext_0 thread* kernelContext_0)
{

    int3 _S1 = int3(coordinate_0, int(0));

#line 1003
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


#line 1000 "src/rendering/shaders/cy/frame.slang"
float4 CyFrameFogVolume_texel_1(const CyFrameFogVolume_0 thread* this_1, int2 coordinate_1, KernelContext_0 thread* kernelContext_2)
{

    int3 _S16 = int3(coordinate_1, int(0));

#line 1003
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


#line 611 "src/rendering/shaders/cy/frame.slang"
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


#line 342 "src/rendering/shaders/cy/frame.slang"
float4 cyMaterialSampleTextureLevel_0(uint bindlessIndex_0, float2 uv_0, float level_0, KernelContext_0 thread* kernelContext_4)
{
    return ((kernelContext_4->cyFrameGlobals_0->textures_0[bindlessIndex_0]).sample((kernelContext_4->cyFrameGlobals_0->sampler_0), (uv_0), level((level_0))));
}


#line 614
float CyFrameShadowMap_storedDepth_0(const CyFrameShadowMap_0 thread* this_2, float2 uv_1, KernelContext_0 thread* kernelContext_5)
{

#line 614
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


#line 614 "src/rendering/shaders/cy/frame.slang"
float CyFrameShadowMap_storedDepth_1(const CyFrameShadowMap_0 thread* this_3, float2 uv_3, KernelContext_0 thread* kernelContext_7)
{

#line 614
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


#line 911 "src/rendering/shaders/cy/frame.slang"
uint CyFrameDecalSource_word_0(uint index_6, KernelContext_0 thread* kernelContext_9)
{

#line 911
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


#line 911 "src/rendering/shaders/cy/frame.slang"
uint CyFrameDecalSource_word_1(uint index_9, KernelContext_0 thread* kernelContext_14)
{

#line 911
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


#line 342 "src/rendering/shaders/cy/frame.slang"
float4 cyMaterialSampleTextureLevel_1(uint bindlessIndex_1, float2 uv_6, float level_1, KernelContext_0 thread* kernelContext_15)
{
    return ((kernelContext_15->cyFrameGlobals_0->textures_0[bindlessIndex_1]).sample((kernelContext_15->cyFrameGlobals_0->sampler_0), (uv_6), level((level_1))));
}


#line 916
float CyFrameDecalSource_mask_0(uint slot_2, float2 uv_7, KernelContext_0 thread* kernelContext_16)
{

#line 916
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


#line 403 "src/rendering/shaders/cy/frame.slang"
float3 readMaterialFloat3_0(uint material_1, uint wordOffset_0, KernelContext_0 thread* kernelContext_21)
{
)cy_msl",
    R"cy_msl(    uint _S159 = material_1 * kernelContext_21->cyFrameView_0->frame_0->counts_0.y + wordOffset_0;
    return float3((as_type<float>((kernelContext_21->cyFrameView_0->materialWords_0[_S159]))), (as_type<float>((kernelContext_21->cyFrameView_0->materialWords_0[_S159 + 1U]))), (as_type<float>((kernelContext_21->cyFrameView_0->materialWords_0[_S159 + 2U]))));
}


#line 398
float readMaterialFloat_0(uint material_2, uint wordOffset_1, KernelContext_0 thread* kernelContext_22)
{
    return (as_type<float>((kernelContext_22->cyFrameView_0->materialWords_0[material_2 * kernelContext_22->cyFrameView_0->frame_0->counts_0.y + wordOffset_1])));
}


#line 413
uint readMaterialUint_0(uint material_3, uint wordOffset_2, KernelContext_0 thread* kernelContext_23)
{
    return kernelContext_23->cyFrameView_0->materialWords_0[material_3 * kernelContext_23->cyFrameView_0->frame_0->counts_0.y + wordOffset_2];
}


#line 423
uint materialTextureSlot_0(uint material_4, uint wordOffset_3, KernelContext_0 thread* kernelContext_24)
{
    if(wordOffset_3 == 4294967295U)
    {
        return 4294967295U;
    }

#line 427
    uint _S160 = readMaterialUint_0(material_4, wordOffset_3, kernelContext_24);

    return _S160;
}


#line 337
float4 cyMaterialSampleTexture_0(uint bindlessIndex_2, float2 uv_10, KernelContext_0 thread* kernelContext_25)
{
    return ((kernelContext_25->cyFrameGlobals_0->textures_0[bindlessIndex_2]).sample((kernelContext_25->cyFrameGlobals_0->sampler_0), (uv_10)));
}


#line 459
Surface_0 surfaceOf_0(uint material_5, float3 tint_1, float2 uv_11, KernelContext_0 thread* kernelContext_26)
{
    thread Surface_0 surface_3 = defaultSurface_0();

#line 461
    float3 _S161 = readMaterialFloat3_0(material_5, kernelContext_26->cyFrameView_0->frame_0->materialOffsets_0.x, kernelContext_26);
    (&surface_3)->albedo_1 = _S161 * tint_1;

#line 462
    float _S162 = readMaterialFloat_0(material_5, kernelContext_26->cyFrameView_0->frame_0->materialOffsets_0.y, kernelContext_26);
    (&surface_3)->roughness_1 = clamp(_S162, 0.01999999955296516, 1.0);

#line 463
    float _S163 = readMaterialFloat_0(material_5, kernelContext_26->cyFrameView_0->frame_0->materialOffsets_0.z, kernelContext_26);
    (&surface_3)->metallic_1 = saturate(_S163);

#line 464
    float3 _S164 = readMaterialFloat3_0(material_5, kernelContext_26->cyFrameView_0->frame_0->materialOffsets_0.w, kernelContext_26);
    (&surface_3)->emission_1 = _S164;

#line 465
    uint _S165 = materialTextureSlot_0(material_5, kernelContext_26->cyFrameView_0->frame_0->materialTextures_0.x, kernelContext_26);


    if(_S165 != 4294967295U)
    {

#line 468
        float4 _S166 = cyMaterialSampleTexture_0(_S165, uv_11, kernelContext_26);

        (&surface_3)->albedo_1 = (&surface_3)->albedo_1 * _S166.xyz;

#line 468
    }



    return surface_3;
}


#line 1020
float2 lightmapCoordinate_0(uint address_0, float2 uv2_0, KernelContext_0 thread* kernelContext_27)
{
    float _S167 = float(kernelContext_27->cyFrameView_0->frame_0->lightmapLayout_0.x);

    float _S168 = float(kernelContext_27->cyFrameView_0->frame_0->lightmapLayout_0.z);

#line 1024
    float2 _S169 = float2(float(kernelContext_27->cyFrameView_0->frame_0->lightmapLayout_0.x / 128U)) ;

#line 1030
    return (float2(float(address_0 & 127U), float((address_0 >> 7U) & 127U)) * _S169 + float2(_S168)  + uv2_0 * (float2(float(((address_0 >> 14U) & 127U) + 1U), float(((address_0 >> 21U) & 127U) + 1U)) * _S169 - float2((2.0 * _S168)) ) + float2(0.0, float((address_0 >> 28U) - 1U) * _S167)) / float2(_S167, _S167 * float(kernelContext_27->cyFrameView_0->frame_0->lightmapLayout_0.y));
}


#line 1107
float3 lightmapDensityHue_0(bool lightmapped_0, float2 uv_12, float3 relativePosition_4, KernelContext_0 thread* kernelContext_28)
{
    if(!lightmapped_0)
    {
        return float3(0.18000000715255737, 0.18000000715255737, 0.18000000715255737);
    }


    float2 _S170 = uv_12 * float2(float(kernelContext_28->cyFrameView_0->frame_0->lightmapLayout_0.x), float(kernelContext_28->cyFrameView_0->frame_0->lightmapLayout_0.x * kernelContext_28->cyFrameView_0->frame_0->lightmapLayout_0.y));
    float2 _S171 = dfdx(_S170);

#line 1116
    float2 _S172 = dfdy(_S170);
    float3 _S173 = dfdx(relativePosition_4);
    float3 _S174 = dfdy(relativePosition_4);

    float _S175 = clamp(log2(max(sqrt(dot(_S171, _S171) + dot(_S172, _S172)) / max(sqrt(dot(_S173, _S173) + dot(_S174, _S174)), 9.99999971718068537e-10), 9.99999971718068537e-10) / max((as_type<float>((kernelContext_28->cyFrameView_0->frame_0->lightmapDebug_0.y))), 9.99999997475242708e-07)), -1.0, 1.0);

#line 1120
    float3 _S176;
    if(_S175 < 0.0)
    {

#line 1121
        _S176 = mix(float3(0.10000000149011612, 0.89999997615814209, 0.20000000298023224), float3(0.10000000149011612, 0.25, 1.0), float3(- _S175) );

#line 1121
    }
    else
    {

#line 1121
        _S176 = mix(float3(0.10000000149011612, 0.89999997615814209, 0.20000000298023224), float3(1.0, 0.15000000596046448, 0.10000000149011612), float3(_S175) );

#line 1121
    }

    uint2 _S177 = uint2(floor(_S170));
    if(((_S177.x + _S177.y) & 1U) == 0U)
    {
    }
    else
    {

#line 1124
        _S176 = _S176 * float3(0.55000001192092896) ;

#line 1124
    }

#line 1124
    return _S176;
}


#line 1091
float frameExposureStops_0(KernelContext_0 thread* kernelContext_29)
{

    return kernelContext_29->cyFrameGlobals_0->globals_0->exposureStops_0;
}


#line 1102
float3 lightmapDensityColour_0(bool lightmapped_1, float2 uv_13, float3 relativePosition_5, KernelContext_0 thread* kernelContext_30)
{

#line 1102
    float3 _S178 = lightmapDensityHue_0(lightmapped_1, uv_13, relativePosition_5, kernelContext_30);

#line 1102
    float _S179 = frameExposureStops_0(kernelContext_30);

    return _S178 * float3(exp2(- _S179)) ;
}


#line 21 "src/rendering/shaders/cy/cluster.slang"
uint clusterSliceOf_1(const ClusterGrid_0 constant* grid_4, float viewDepth_2)
{

    return uint(clamp(log2(max(viewDepth_2, grid_4->nearPlane_0)) * grid_4->sliceScale_0 + grid_4->sliceBias_0, 0.0, float(grid_4->dimensions_0.z - 1U)));
}


uint3 clusterCoordOf_1(const ClusterGrid_0 constant* grid_5, uint2 pixel_2, uint2 renderExtent_1, float viewDepth_3)
{
    uint2 _S180 = grid_5->dimensions_0.xy;
    uint2 _S181 = min(uint2(float2(pixel_2) / float2(renderExtent_1) * float2(_S180)), _S180 - uint2(1U) );

#line 31
    uint _S182 = clusterSliceOf_1(grid_5, viewDepth_3);

#line 31
    return uint3(_S181, _S182);
}



uint clusterIndexOf_1(const ClusterGrid_0 constant* grid_6, uint3 coord_1)
{
    return coord_1.x + grid_6->dimensions_0.x * (coord_1.y + grid_6->dimensions_0.y * coord_1.z);
}


#line 390 "src/rendering/shaders/cy/frame.slang"
float viewDepthOf_0(float3 relative_0, KernelContext_0 thread* kernelContext_31)
{



    return - dot(kernelContext_31->cyFrameView_0->frame_0->relativeToViewRow2_0, float4(relative_0, 1.0));
}


#line 924
void applyFrameDecals_0(Surface_0 thread* surface_4, float3 thread* normal_2, float3 relativePosition_6, float2 fragmentCentre_0, uint receiverChannels_0, KernelContext_0 thread* kernelContext_32)
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

#line 943
    uint _S183 = kernelContext_32->cyFrameView_0->frame_0->decalControl_0.z;

#line 943
    uint _S184 = cyDecalCount_0(kernelContext_32);


    uint2 _S185 = uint2(0U, _S184);

#line 946
    uint2 list_0;

#line 946
    uint mode_0;
    if((_S183 & 2U) != 0U)
    {

#line 947
        list_0 = _S185;

#line 947
        mode_0 = 0U;

#line 947
    }
    else
    {

        if((_S183 & 1U) != 0U)
        {

            if(_S184 != 0U)
            {

#line 954
                uint2 _S186 = cyDecalTableList_0(relativePosition_6, fragmentCentre_0, kernelContext_32);

#line 954
                list_0 = _S186;

#line 954
            }
            else
            {

#line 954
                list_0 = uint2(0U) ;

#line 954
            }

#line 954
            mode_0 = 1U;

#line 951
        }
        else
        {

#line 951
            bool _S187;

#line 956
            if((kernelContext_32->cyFrameView_0->frame_0->counts_0.w) != 0U)
            {

#line 956
                _S187 = ((&kernelContext_32->cyFrameView_0->frame_0->clusterGrid_0)->dimensions_0.z) != 0U;

#line 956
            }
            else
            {

#line 956
                _S187 = false;

#line 956
            }

#line 956
            if(_S187)
            {
                uint2 _S188 = uint2(kernelContext_32->cyFrameView_0->frame_0->extentAndInverse_0.xy);
                uint2 _S189 = uint2(fragmentCentre_0);

#line 959
                float _S190 = viewDepthOf_0(relativePosition_6, kernelContext_32);

#line 959
                uint3 _S191 = clusterCoordOf_1(&kernelContext_32->cyFrameView_0->frame_0->clusterGrid_0, _S189, _S188, _S190);

#line 959
                uint _S192 = clusterIndexOf_1(&kernelContext_32->cyFrameView_0->frame_0->clusterGrid_0, _S191);

#line 959
                list_0 = kernelContext_32->cyFrameView_0->clusterHeaders_0[_S192 * kernelContext_32->cyFrameView_0->frame_0->counts_0.z + 1U];

#line 959
                mode_0 = 2U;

#line 956
            }
            else
            {

#line 956
                list_0 = _S185;

#line 956
                mode_0 = 0U;

#line 956
            }

#line 951
        }

#line 947
    }

#line 947
    uint slot_3 = 0U;

#line 967
    for(;;)
    {

#line 967
        if(slot_3 < (list_0.y))
        {
        }
        else
        {

#line 967
            break;
        }

#line 967
        uint rank_1;


        if(mode_0 == 1U)
        {

#line 970
            uint _S193 = cyDecalTableListEntry_0(list_0.x + slot_3, kernelContext_32);

#line 970
            rank_1 = _S193;

#line 970
        }
        else
        {

            if(mode_0 == 2U)
            {

#line 974
                rank_1 = kernelContext_32->cyFrameView_0->clusterIndices_0[list_0.x + slot_3];

#line 974
            }
            else
            {

#line 974
                rank_1 = slot_3;

#line 974
            }

#line 970
        }

#line 978
        if(rank_1 < _S184)
        {

#line 978
            thread CyDecalReceiver_0 _S194 = receiver_5;

#line 978
            cyApplyDecal_0(rank_1, &_S194, &decalled_0, kernelContext_32);

#line 978
        }

#line 967
        slot_3 = slot_3 + 1U;

#line 967
    }

#line 984
    surface_4->albedo_1 = (&decalled_0)->albedo_0;
    surface_4->roughness_1 = clamp((&decalled_0)->roughness_0, 0.01999999955296516, 1.0);
    surface_4->metallic_1 = saturate((&decalled_0)->metallic_0);
    surface_4->emission_1 = (&decalled_0)->emission_0;
    *normal_2 = (&decalled_0)->normal_0;
    return;
}


#line 693
struct CyBakedLights_0
{
    bool active_0;
    float4 mask_0;
};


#line 1063
CyBakedLights_0 bakedLightsOf_0(bool lightmapped_2, float2 uv_14, KernelContext_0 thread* kernelContext_33)
{
    thread CyBakedLights_0 baked_0;
    (&baked_0)->active_0 = lightmapped_2;
    (&baked_0)->mask_0 = float4(1.0) ;

#line 1067
    bool _S195;
    if(lightmapped_2)
    {

#line 1068
        _S195 = (kernelContext_33->cyFrameView_0->frame_0->lightmapLayout_0.w) != 4294967295U;

#line 1068
    }
    else
    {

#line 1068
        _S195 = false;

#line 1068
    }

#line 1068
    if(_S195)
    {

#line 1068
        float4 _S196 = cyMaterialSampleTexture_0(kernelContext_33->cyFrameView_0->frame_0->lightmapLayout_0.w, uv_14, kernelContext_33);

        (&baked_0)->mask_0 = _S196;

#line 1068
    }



    return baked_0;
}


#line 42 "src/rendering/shaders/cy/light.slang"
float distanceAttenuation_0(float distanceSquared_0, float range_1)
{
    float _S197 = distanceSquared_0 / max(range_1 * range_1, 9.99999997475242708e-07);
    float _S198 = saturate(1.0 - _S197 * _S197);
    return _S198 * _S198 / max(distanceSquared_0, 0.00009999999747379);
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
    thread LightSample_0 result_4;

#line 51
    uint _S199 = light_0->kind_0;
    if((light_0->kind_0) == 0U)
    {
        (&result_4)->direction_1 = - light_0->direction_0;
        (&result_4)->attenuation_0 = 1.0;
        (&result_4)->illuminance_0 = light_0->color_0 * float3(light_0->intensity_0) ;
        return result_4;
    }

    float3 _S200 = light_0->positionRelativeToCamera_0 - surfaceRelativeToCamera_0;
    float _S201 = dot(_S200, _S200);
    (&result_4)->direction_1 = _S200 * float3(rsqrt(max(_S201, 9.99999993922529029e-09))) ;
    (&result_4)->attenuation_0 = distanceAttenuation_0(_S201, light_0->range_0);

    if(_S199 == 2U)
    {

        float _S202 = saturate(dot(- (&result_4)->direction_1, light_0->direction_0) * light_0->spotScaleBias_0.x + light_0->spotScaleBias_0.y);
        (&result_4)->attenuation_0 = (&result_4)->attenuation_0 * (_S202 * _S202);

#line 65
    }

#line 71
    (&result_4)->illuminance_0 = light_0->color_0 * float3((light_0->intensity_0 * (&result_4)->attenuation_0)) ;
    return result_4;
}


#line 366 "src/rendering/shaders/cy/frame.slang"
float4 transformToShadowClip_0(float3 relative_1, KernelContext_0 thread* kernelContext_34)
{
    float4 _S203 = float4(relative_1, 1.0);
    return float4(dot(kernelContext_34->cyFrameView_0->frame_0->shadowToClipRow0_0, _S203), dot(kernelContext_34->cyFrameView_0->frame_0->shadowToClipRow1_0, _S203), dot(kernelContext_34->cyFrameView_0->frame_0->shadowToClipRow2_0, _S203), dot(kernelContext_34->cyFrameView_0->frame_0->shadowToClipRow3_0, _S203));
}


#line 98 "src/rendering/shaders/cy/shadow.slang"
float shadowDiscRotation_0(float2 pixel_3)
{

    return fract(52.98291778564453125 * fract(dot(pixel_3, float2(0.06711056083440781, 0.00583714991807938)))) * 6.28318548202514648;
}


#line 622 "src/rendering/shaders/cy/frame.slang"
float softShadowVisibility_0(float2 uv_15, float reference_0, float2 pixel_4, KernelContext_0 thread* kernelContext_35)
{
    thread CyFrameShadowMap_0 map_0;
    (&map_0)->slot_1 = kernelContext_35->cyFrameView_0->frame_0->shadowControl_0.x;
    thread PcssShape_0 shape_4;
    (&shape_4)->penumbraPerDepth_0 = kernelContext_35->cyFrameView_0->frame_0->softShadowShape_0.x;
    (&shape_4)->minRadius_0 = kernelContext_35->cyFrameView_0->frame_0->softShadowShape_0.y;
    (&shape_4)->maxRadius_0 = kernelContext_35->cyFrameView_0->frame_0->softShadowShape_0.z;
    (&shape_4)->blockerTaps_0 = int(max(kernelContext_35->cyFrameView_0->frame_0->softShadowControl_0.z, 1U));
    (&shape_4)->filterTaps_0 = int(max(kernelContext_35->cyFrameView_0->frame_0->softShadowControl_0.w, 1U));
    float _S204 = shadowDiscRotation_0(pixel_4);

#line 632
    thread CyFrameShadowMap_0 _S205 = map_0;

#line 632
    thread PcssShape_0 _S206 = shape_4;

#line 632
    float _S207 = pcssVisibility_0(&_S205, uv_15, reference_0, &_S206, _S204, kernelContext_35);

#line 632
    return _S207;
}


#line 655
float directionalShadowVisibility_0(float3 relativePosition_7, float3 normal_3, float2 fragmentCentre_1, KernelContext_0 thread* kernelContext_36)
{

#line 655
    bool _S208;

    if((kernelContext_36->cyFrameView_0->frame_0->shadowControl_0.w) == 0U)
    {

#line 657
        _S208 = true;

#line 657
    }
    else
    {

#line 657
        _S208 = (kernelContext_36->cyFrameView_0->frame_0->shadowControl_0.x) == 4294967295U;

#line 657
    }

#line 657
    if(_S208)
    {
)cy_msl",
    R"cy_msl(        return 1.0;
    }

#line 659
    float4 _S209 = transformToShadowClip_0(relativePosition_7 + normal_3 * float3(0.00499999988824129) , kernelContext_36);


    float _S210 = _S209.w;

#line 662
    if(_S210 <= 0.0)
    {
        return 1.0;
    }
    float3 _S211 = _S209.xyz / float3(_S210) ;
    float _S212 = _S211.x * 0.5 + 0.5;

#line 667
    float _S213 = 0.5 - _S211.y * 0.5;

#line 667
    float2 _S214 = float2(_S212, _S213);
    if(_S212 < 0.0)
    {

#line 668
        _S208 = true;

#line 668
    }
    else
    {

#line 668
        _S208 = _S212 > 1.0;

#line 668
    }

#line 668
    if(_S208)
    {

#line 668
        _S208 = true;

#line 668
    }
    else
    {

#line 668
        _S208 = _S213 < 0.0;

#line 668
    }

#line 668
    if(_S208)
    {

#line 668
        _S208 = true;

#line 668
    }
    else
    {

#line 668
        _S208 = _S213 > 1.0;

#line 668
    }

#line 668
    if(_S208)
    {
        return 1.0;
    }
    float _S215 = 1.0 / float(max(kernelContext_36->cyFrameView_0->frame_0->shadowControl_0.z, 1U));
    float _S216 = _S211.z + 0.00050000002374873;
    if(((kernelContext_36->cyFrameView_0->frame_0->softShadowControl_0.x) & 1U) != 0U)
    {

#line 674
        float _S217 = softShadowVisibility_0(_S214, _S216, fragmentCentre_1, kernelContext_36);

        return _S217;
    }

#line 676
    int y_0 = int(-1);

#line 676
    float visible_0 = 0.0;


    for(;;)
    {

#line 679
        if(y_0 <= int(1))
        {
        }
        else
        {

#line 679
            break;
        }

#line 679
        int x_0 = int(-1);

        for(;;)
        {

#line 681
            if(x_0 <= int(1))
            {
            }
            else
            {

#line 681
                break;
            }

#line 681
            float4 _S218 = cyMaterialSampleTextureLevel_0(kernelContext_36->cyFrameView_0->frame_0->shadowControl_0.x, _S214 + float2(float(x_0), float(y_0)) * float2(_S215) , 0.0, kernelContext_36);

#line 681
            float _S219;



            if(_S216 >= (_S218.x))
            {

#line 685
                _S219 = 1.0;

#line 685
            }
            else
            {

#line 685
                _S219 = 0.0;

#line 685
            }

#line 685
            float visible_1 = visible_0 + _S219;

#line 681
            x_0 = x_0 + int(1);

#line 681
            visible_0 = visible_1;

#line 681
        }

#line 679
        y_0 = y_0 + int(1);

#line 679
    }

#line 688
    return visible_0 / 9.0;
}


#line 638
float contactShadowVisibility_0(float2 fragmentCentre_2, KernelContext_0 thread* kernelContext_37)
{

#line 638
    bool _S220;

    if(((kernelContext_37->cyFrameView_0->frame_0->softShadowControl_0.x) & 2U) == 0U)
    {

#line 640
        _S220 = true;

#line 640
    }
    else
    {

#line 640
        _S220 = (kernelContext_37->cyFrameView_0->frame_0->softShadowControl_0.y) == 4294967295U;

#line 640
    }

#line 640
    if(_S220)
    {

        return 1.0;
    }

#line 643
    float4 _S221 = cyMaterialSampleTextureLevel_0(kernelContext_37->cyFrameView_0->frame_0->softShadowControl_0.y, fragmentCentre_2 * kernelContext_37->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_37);


    return _S221.x;
}


#line 701
float bakedLightFactor_0(uint index_11, float4 mask_1, KernelContext_0 thread* kernelContext_38)
{

#line 701
    bool _S222;

    if(index_11 < 128U)
    {

#line 703
        _S222 = (((kernelContext_38->cyFrameView_0->frame_0->lightmapDirectLights_0[index_11 >> 5U]) >> (index_11 & 31U)) & 1U) != 0U;

#line 703
    }
    else
    {

#line 703
        _S222 = false;

#line 703
    }

#line 703
    if(_S222)
    {
        return 0.0;
    }

#line 705
    uint channel_0 = 0U;

    for(;;)
    {

#line 707
        if(channel_0 < 4U)
        {
        }
        else
        {

#line 707
            break;
        }
        if((kernelContext_38->cyFrameView_0->frame_0->lightmapShadowLights_0[channel_0]) == index_11)
        {
            return mask_1[channel_0];
        }

#line 707
        channel_0 = channel_0 + 1U;

#line 707
    }

#line 714
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
    float _S223 = roughness_2 * roughness_2;
    float _S224 = _S223 * _S223;
    float _S225 = normalDotHalf_0 * normalDotHalf_0 * (_S224 - 1.0) + 1.0;
    return _S224 / max(3.14159274101257324 * _S225 * _S225, 1.00000001168609742e-07);
}


float visibilitySmithGgxCorrelated_0(float normalDotView_0, float normalDotLight_0, float roughness_3)
{

    float _S226 = roughness_3 * roughness_3;
    float _S227 = _S226 * _S226;
    float _S228 = 1.0 - _S227;



    return 0.5 / max(normalDotLight_0 * sqrt(normalDotView_0 * normalDotView_0 * _S228 + _S227) + normalDotView_0 * sqrt(normalDotLight_0 * normalDotLight_0 * _S228 + _S227), 1.00000001168609742e-07);
}

float3 fresnelSchlick_0(float3 f0_0, float viewDotHalf_0)
{

    return f0_0 + (float3(1.0)  - f0_0) * float3(pow(saturate(1.0 - viewDotHalf_0), 5.0)) ;
}


#line 45
float3 specularGgx_0(float3 normal_4, float3 view_0, float3 light_1, float roughness_4, float3 f0_1)
{
    float3 _S229 = normalize(view_0 + light_1);

#line 56
    return float3((distributionGgx_0(saturate(dot(normal_4, _S229)), roughness_4) * visibilitySmithGgxCorrelated_0(saturate(dot(normal_4, view_0)) + 0.00000999999974738, saturate(dot(normal_4, light_1)), roughness_4)))  * fresnelSchlick_0(f0_1, saturate(dot(view_0, _S229)));
}


#line 108 "src/rendering/shaders/cy/material.slang"
float3 shadeSurfaceWithLight_0(const Surface_0 thread* surface_5, float3 worldNormal_0, float3 viewDirection_0, const LightSample_0 thread* sample_0)
{

#line 109
    float3 _S230 = sample_0->direction_1;

    float _S231 = saturate(dot(worldNormal_0, sample_0->direction_1));
    if(_S231 <= 0.0)
    {
        return float3(0.0) ;
    }

#line 120
    return (diffuseLambert_0(surface_5->albedo_1 * float3((1.0 - surface_5->metallic_1)) ) + specularGgx_0(worldNormal_0, viewDirection_0, _S230, surface_5->roughness_1, computeF0_0(surface_5->albedo_1, surface_5->metallic_1))) * sample_0->illuminance_0 * float3(_S231) ;
}


#line 719 "src/rendering/shaders/cy/frame.slang"
LightSample_0 bakedLightSample_0(uint index_12, float3 relativePosition_8, const CyBakedLights_0 thread* baked_1, KernelContext_0 thread* kernelContext_39)
{

#line 719
    thread Light_0 _S232 = kernelContext_39->cyFrameView_0->lights_0[index_12];

#line 719
    LightSample_0 _S233 = evaluateLight_0(&_S232, relativePosition_8);

    thread LightSample_0 sample_1 = _S233;
    if(baked_1->active_0)
    {

#line 722
        float _S234 = bakedLightFactor_0(index_12, baked_1->mask_0, kernelContext_39);

        (&sample_1)->illuminance_0 = (&sample_1)->illuminance_0 * float3(_S234) ;

#line 722
    }



    return sample_1;
}

float3 accumulateLights_0(const Surface_0 thread* surface_6, float3 relativePosition_9, float3 normal_5, float3 viewDir_0, float2 fragmentCentre_3, uint instanceFlags_0, const CyBakedLights_0 thread* baked_2, KernelContext_0 thread* kernelContext_40)
{

#line 730
    bool _S235;

    uint2 _S236 = uint2(fragmentCentre_3);
    float3 _S237 = float3(0.0) ;
    uint _S238 = kernelContext_40->cyFrameView_0->frame_0->counts_0.x;

#line 734
    uint global_0 = 0U;

#line 734
    float3 lit_2 = _S237;

#line 743
    for(;;)
    {

#line 743
        if(global_0 < _S238)
        {
        }
        else
        {

#line 743
            break;
        }
        if((kernelContext_40->cyFrameView_0->lights_0[global_0].kind_0) != 0U)
        {
            global_0 = global_0 + 1U;

#line 743
            continue;
        }

#line 743
        thread Light_0 _S239 = kernelContext_40->cyFrameView_0->lights_0[global_0];

#line 743
        LightSample_0 _S240 = evaluateLight_0(&_S239, relativePosition_9);

#line 753
        if(global_0 == (kernelContext_40->cyFrameView_0->frame_0->shadowControl_0.y))
        {

#line 753
            _S235 = (instanceFlags_0 & 8U) != 0U;

#line 753
        }
        else
        {

#line 753
            _S235 = false;

#line 753
        }

#line 753
        float visibility_0;
        if(_S235)
        {

#line 754
            float _S241 = directionalShadowVisibility_0(relativePosition_9, normal_5, fragmentCentre_3, kernelContext_40);

#line 754
            float _S242 = contactShadowVisibility_0(fragmentCentre_3, kernelContext_40);

#line 754
            visibility_0 = min(_S241, _S242);

#line 754
        }
        else
        {

#line 754
            visibility_0 = 1.0;

#line 754
        }

#line 754
        float visibility_1;

#line 761
        if(baked_2->active_0)
        {

#line 761
            float _S243 = bakedLightFactor_0(global_0, baked_2->mask_0, kernelContext_40);

#line 761
            visibility_1 = min(visibility_0, _S243);

#line 761
        }
        else
        {

#line 761
            visibility_1 = visibility_0;

#line 761
        }

#line 761
        thread LightSample_0 _S244 = _S240;

#line 761
        float3 _S245 = shadeSurfaceWithLight_0(surface_6, normal_5, viewDir_0, &_S244);

#line 761
        lit_2 = lit_2 + _S245 * float3(visibility_1) ;

#line 743
        global_0 = global_0 + 1U;

#line 743
    }

#line 768
    if((kernelContext_40->cyFrameView_0->frame_0->counts_0.w) == 0U)
    {

#line 768
        _S235 = true;

#line 768
    }
    else
    {

#line 768
        _S235 = ((&kernelContext_40->cyFrameView_0->frame_0->clusterGrid_0)->dimensions_0.z) == 0U;

#line 768
    }

#line 768
    uint index_13;

#line 768
    if(_S235)
    {

#line 768
        index_13 = 0U;

        for(;;)
        {

#line 770
            if(index_13 < _S238)
            {
            }
            else
            {

#line 770
                break;
            }
            if((kernelContext_40->cyFrameView_0->lights_0[index_13].kind_0) == 0U)
            {
                index_13 = index_13 + 1U;

#line 770
                continue;
            }

#line 770
            LightSample_0 _S246 = bakedLightSample_0(index_13, relativePosition_9, baked_2, kernelContext_40);

#line 770
            thread LightSample_0 _S247 = _S246;

#line 770
            float3 _S248 = shadeSurfaceWithLight_0(surface_6, normal_5, viewDir_0, &_S247);

#line 770
            lit_2 = lit_2 + _S248;

#line 770
            index_13 = index_13 + 1U;

#line 770
        }

#line 779
        return lit_2;
    }

    uint2 _S249 = uint2(kernelContext_40->cyFrameView_0->frame_0->extentAndInverse_0.xy);

#line 782
    float _S250 = viewDepthOf_0(relativePosition_9, kernelContext_40);

#line 782
    uint3 _S251 = clusterCoordOf_1(&kernelContext_40->cyFrameView_0->frame_0->clusterGrid_0, _S236, _S249, _S250);

#line 782
    uint _S252 = clusterIndexOf_1(&kernelContext_40->cyFrameView_0->frame_0->clusterGrid_0, _S251);

#line 787
    uint2 _S253 = kernelContext_40->cyFrameView_0->clusterHeaders_0[_S252 * kernelContext_40->cyFrameView_0->frame_0->counts_0.z];

#line 787
    index_13 = 0U;
    for(;;)
    {

#line 788
        if(index_13 < (_S253.y))
        {
        }
        else
        {

#line 788
            break;
        }
        uint _S254 = kernelContext_40->cyFrameView_0->clusterIndices_0[_S253.x + index_13];
        if(_S254 >= _S238)
        {

#line 791
            _S235 = true;

#line 791
        }
        else
        {

#line 791
            _S235 = (kernelContext_40->cyFrameView_0->lights_0[_S254].kind_0) == 0U;

#line 791
        }

#line 791
        if(_S235)
        {
            index_13 = index_13 + 1U;

#line 788
            continue;
        }

#line 788
        LightSample_0 _S255 = bakedLightSample_0(_S254, relativePosition_9, baked_2, kernelContext_40);

#line 788
        thread LightSample_0 _S256 = _S255;

#line 788
        float3 _S257 = shadeSurfaceWithLight_0(surface_6, normal_5, viewDir_0, &_S256);

#line 788
        lit_2 = lit_2 + _S257;

#line 788
        index_13 = index_13 + 1U;

#line 788
    }

#line 798
    return lit_2;
}


#line 1040
float3 lightmapAmbient_0(float2 uv_16, float3 normal_6, KernelContext_0 thread* kernelContext_41)
{

#line 1040
    float4 _S258 = cyMaterialSampleTexture_0(kernelContext_41->cyFrameView_0->frame_0->lightmapControl_0.x, uv_16, kernelContext_41);


    uint _S259 = kernelContext_41->cyFrameView_0->frame_0->lightmapControl_0.w;
    if(_S259 == 2U)
    {

#line 1044
        float4 _S260 = cyMaterialSampleTexture_0(kernelContext_41->cyFrameView_0->frame_0->lightmapControl_0.y, uv_16, kernelContext_41);



        return _S258.xyz * float3((max(0.0, 1.0 + dot(_S260.xyz, normal_6)) / max(_S260.w, 0.00100000004749745))) ;
    }
    if(_S259 == 3U)
    {

#line 1050
        float4 _S261 = cyMaterialSampleTexture_0(kernelContext_41->cyFrameView_0->frame_0->lightmapControl_0.y, uv_16, kernelContext_41);

#line 1050
        float4 _S262 = cyMaterialSampleTexture_0(kernelContext_41->cyFrameView_0->frame_0->lightmapControl_0.z, uv_16, kernelContext_41);



        return float3(max(0.0, _S258.w + dot(_S258.xyz, normal_6)), max(0.0, _S261.w + dot(_S261.xyz, normal_6)), max(0.0, _S262.w + dot(_S262.xyz, normal_6)));
    }


    return _S258.xyz;
}


#line 818
float4 probeVolumeTexel_0(uint3 probe_0, uint texel_3, KernelContext_0 thread* kernelContext_42)
{
    uint3 _S263 = kernelContext_42->cyFrameView_0->frame_0->probeVolumeControl_0.yzw;

    uint _S264 = _S263.y;

#line 822
    float4 _S265 = cyMaterialSampleTextureLevel_0(kernelContext_42->cyFrameView_0->frame_0->probeVolumeControl_0.x, (float2(float(probe_0.x * 5U + texel_3), float(probe_0.y + _S264 * probe_0.z)) + float2(0.5) ) / float2(float(_S263.x * 5U), float(_S264 * _S263.z)), 0.0, kernelContext_42);


    return _S265;
}



float probeVisibility_0(float4 distances0_0, float4 distances1_0, float3 probePosition_0, float3 position_1, float3 normal_7, float3 query_0, KernelContext_0 thread* kernelContext_43)
{

    float3 _S266 = probePosition_0 - position_1;
    float _S267 = dot(_S266, _S266);

#line 834
    float3 _S268;
    if(_S267 > 9.999999960041972e-13)
    {

#line 835
        _S268 = _S266 * float3(rsqrt(_S267)) ;

#line 835
    }
    else
    {

#line 835
        _S268 = normal_7;

#line 835
    }
    float _S269 = max(0.00009999999747379, (dot(_S268, normal_7) + 1.0) * 0.5);
    float _S270 = _S269 * _S269 + 0.20000000298023224;



    float3 _S271 = query_0 - probePosition_0;

#line 841
    float _S272;
    if((_S271.x) >= 0.0)
    {

#line 842
        _S272 = distances0_0.y;

#line 842
    }
    else
    {

#line 842
        _S272 = distances0_0.z;

#line 842
    }

#line 842
    float _S273;
    if((_S271.y) >= 0.0)
    {

#line 843
        _S273 = distances0_0.w;

#line 843
    }
    else
    {

#line 843
        _S273 = distances1_0.x;

#line 843
    }

#line 843
    float _S274;
    if((_S271.z) >= 0.0)
    {

#line 844
        _S274 = distances1_0.y;

#line 844
    }
    else
    {

#line 844
        _S274 = distances1_0.z;

#line 844
    }

#line 844
    float3 _S275 = float3(kernelContext_43->cyFrameView_0->frame_0->probeVolumeParams_0.z) ;

    float3 _S276 = saturate((float3(_S272, _S273, _S274) + _S275 - abs(_S271)) / _S275);
    return _S270 * min(_S276.x, min(_S276.y, _S276.z));
}



float3 probeVolumeAmbient_0(float3 position_2, float3 normal_8, float3 flat_0, KernelContext_0 thread* kernelContext_44)
{
    uint3 _S277 = kernelContext_44->cyFrameView_0->frame_0->probeVolumeControl_0.yzw;

    float3 _S278 = position_2 + normal_8 * float3(kernelContext_44->cyFrameView_0->frame_0->probeVolumeParams_0.y) ;

#line 856
    float3 _S279 = float3(kernelContext_44->cyFrameView_0->frame_0->probeVolumeOrigin_0.w) ;
)cy_msl",
    R"cy_msl(    float3 _S280 = (_S278 - kernelContext_44->cyFrameView_0->frame_0->probeVolumeOrigin_0.xyz) / _S279;

#line 857
    float3 _S281 = float3(1.0) ;
    float3 _S282 = float3(_S277) - _S281;
    float3 _S283 = float3(0.0) ;

#line 859
    float3 _S284 = max(max(- _S280, _S280 - _S282), _S283);
    float _S285 = saturate(1.0 - max(_S284.x, max(_S284.y, _S284.z)));
    if(_S285 <= 0.0)
    {
        return flat_0;
    }
    float3 _S286 = clamp(_S280, _S283, _S282);
    uint3 _S287 = min(uint3(floor(_S286)), uint3(max(int3(_S277) - int3(int(2)) , int3(int(0)) )));
    float3 _S288 = _S286 - float3(_S287);


    float4 _S289 = float4(0.28209498524665833, 0.32573533058166504 * normal_8.y, 0.32573533058166504 * normal_8.z, 0.32573533058166504 * normal_8.x);

#line 870
    uint corner_1 = 0U;

#line 870
    float3 total_0 = _S283;

#line 870
    float weightSum_0 = 0.0;



    for(;;)
    {

#line 874
        if(corner_1 < 8U)
        {
        }
        else
        {

#line 874
            break;
        }
        uint3 _S290 = uint3(corner_1 & 1U, (corner_1 >> 1U) & 1U, (corner_1 >> 2U) & 1U);
        uint3 _S291 = min(_S287 + _S290, _S277 - uint3(1U) );
        float3 _S292 = float3(_S290);
        float3 _S293 = _S292 * _S288 + (_S281 - _S292) * (_S281 - _S288);

#line 879
        float4 _S294 = probeVolumeTexel_0(_S291, 3U, kernelContext_44);

        float _S295 = _S293.x * _S293.y * _S293.z * _S294.x;
        if(_S295 <= 0.0)
        {
            corner_1 = corner_1 + 1U;

#line 874
            continue;
        }

#line 874
        float4 _S296 = probeVolumeTexel_0(_S291, 4U, kernelContext_44);

#line 874
        float _S297 = probeVisibility_0(_S294, _S296, kernelContext_44->cyFrameView_0->frame_0->probeVolumeOrigin_0.xyz + float3(_S291) * _S279, position_2, normal_8, _S278, kernelContext_44);

#line 889
        float _S298 = _S295 * _S297;

#line 889
        float4 _S299 = probeVolumeTexel_0(_S291, 0U, kernelContext_44);
        float _S300 = dot(_S299, _S289);

#line 890
        float4 _S301 = probeVolumeTexel_0(_S291, 1U, kernelContext_44);
        float _S302 = dot(_S301, _S289);

#line 891
        float4 _S303 = probeVolumeTexel_0(_S291, 2U, kernelContext_44);



        float weightSum_1 = weightSum_0 + _S298;

#line 895
        total_0 = total_0 + max(float3(_S300, _S302, dot(_S303, _S289)) * float3(kernelContext_44->cyFrameView_0->frame_0->probeVolumeParams_0.x) , _S283) * float3(_S298) ;

#line 895
        weightSum_0 = weightSum_1;

#line 874
        corner_1 = corner_1 + 1U;

#line 874
    }

#line 897
    if(weightSum_0 > 9.99999997475242708e-07)
    {

#line 897
        total_0 = total_0 / float3(weightSum_0) ;

#line 897
    }
    else
    {

#line 897
        total_0 = flat_0;

#line 897
    }
    return mix(flat_0, total_0, float3(_S285) );
}


#line 803
float occlusionVisibility_0(float2 fragmentCentre_4, KernelContext_0 thread* kernelContext_45)
{

#line 803
    float4 _S304 = cyMaterialSampleTextureLevel_0(kernelContext_45->cyFrameView_0->frame_0->occlusionControl_0.x, fragmentCentre_4 * kernelContext_45->cyFrameView_0->frame_0->extentAndInverse_0.zw, 0.0, kernelContext_45);


    return _S304.w;
}


#line 573
struct CyForwardVertex_0
{
    float4 position_3;
    float3 relativePosition_10;
    float3 normal_9;
    float2 uv_17;
    [[flat]] uint drawIndex_0;
    float2 lightmapUv_0;
};


#line 1130
float4 cyShadeForward_0(const Surface_0 thread* resolved_0, const CyForwardVertex_0 thread* input_0, KernelContext_0 thread* kernelContext_46)
{
    CyDrawInstance_0 _S305 = kernelContext_46->cyFrameView_0->drawInstances_0[input_0->drawIndex_0];
    thread Surface_0 surface_7 = *resolved_0;

#line 1133
    bool _S306;


    if((_S305.giAddress_0) != 0U)
    {

#line 1136
        _S306 = (kernelContext_46->cyFrameView_0->frame_0->lightmapControl_0.w) != 0U;

#line 1136
    }
    else
    {

#line 1136
        _S306 = false;

#line 1136
    }

#line 1136
    float2 _S307;
    if(_S306)
    {

#line 1137
        float2 _S308 = lightmapCoordinate_0(_S305.giAddress_0, input_0->lightmapUv_0, kernelContext_46);

#line 1137
        _S307 = _S308;

#line 1137
    }
    else
    {

#line 1137
        _S307 = float2(0.0) ;

#line 1137
    }

    if((kernelContext_46->cyFrameView_0->frame_0->lightmapDebug_0.x) == 1U)
    {

#line 1139
        float3 _S309 = lightmapDensityColour_0(_S306, _S307, input_0->relativePosition_10, kernelContext_46);

        return float4(_S309, 1.0);
    }

    thread float3 normal_10 = normalize(input_0->normal_9);



    if((kernelContext_46->cyFrameView_0->frame_0->decalControl_0.x) != 4294967295U)
    {

#line 1148
        applyFrameDecals_0(&surface_7, &normal_10, input_0->relativePosition_10, input_0->position_3.xy, _S305.layerMask_0, kernelContext_46);

#line 1148
    }

#line 1148
    float3 _S310 = input_0->relativePosition_10;

#line 1155
    float3 _S311 = normalize(- input_0->relativePosition_10);

#line 1155
    CyBakedLights_0 _S312 = bakedLightsOf_0(_S306, _S307, kernelContext_46);

#line 1162
    float2 _S313 = input_0->position_3.xy;

#line 1162
    thread Surface_0 _S314 = surface_7;

#line 1162
    thread CyBakedLights_0 _S315 = _S312;

#line 1162
    float3 _S316 = accumulateLights_0(&_S314, _S310, normal_10, _S311, _S313, _S305.flags_0, &_S315, kernelContext_46);

#line 1176
    float3 ambientRadiance_0 = kernelContext_46->cyFrameView_0->frame_0->ambientAndOcclusion_0.xyz;

#line 1176
    float3 ambientRadiance_1;
    if(_S306)
    {

#line 1177
        float3 _S317 = lightmapAmbient_0(_S307, normal_10, kernelContext_46);

#line 1177
        ambientRadiance_1 = _S317;

#line 1177
    }
    else
    {

        if((kernelContext_46->cyFrameView_0->frame_0->probeVolumeControl_0.x) != 4294967295U)
        {

#line 1181
            float3 _S318 = probeVolumeAmbient_0(_S310, normal_10, ambientRadiance_0, kernelContext_46);

#line 1181
            ambientRadiance_1 = _S318;

#line 1181
        }
        else
        {

#line 1181
            ambientRadiance_1 = ambientRadiance_0;

#line 1181
        }

#line 1177
    }

#line 1185
    float3 ambient_0 = (&surface_7)->albedo_1 * ambientRadiance_1 * float3((&surface_7)->occlusion_0) ;

#line 1185
    float3 color_1;

#line 1185
    float3 ambient_1;
    if((kernelContext_46->cyFrameView_0->frame_0->occlusionControl_0.x) != 4294967295U)
    {

#line 1186
        float _S319 = occlusionVisibility_0(_S313, kernelContext_46);


        float3 ambient_2 = ambient_0 * float3(_S319) ;
        if((kernelContext_46->cyFrameView_0->frame_0->occlusionControl_0.y) != 0U)
        {

#line 1190
            color_1 = _S316 * float3(mix(1.0, _S319, (as_type<float>((kernelContext_46->cyFrameView_0->frame_0->occlusionControl_0.z))))) ;

#line 1190
        }
        else
        {

#line 1190
            color_1 = _S316;

#line 1190
        }

#line 1190
        ambient_1 = ambient_2;

#line 1186
    }
    else
    {

#line 1186
        color_1 = _S316;

#line 1186
        ambient_1 = ambient_0;

#line 1186
    }

#line 1196
    float3 color_2 = color_1 + (&surface_7)->emission_1 + ambient_1;



    if((kernelContext_46->cyFrameView_0->frame_0->volumetricFogControl_0.x) != 4294967295U)
    {
        thread CyFrameFogVolume_0 fog_0;
        (&fog_0)->slot_0 = kernelContext_46->cyFrameView_0->frame_0->volumetricFogControl_0.x;

#line 1203
        thread CyFrameFogVolume_0 _S320 = fog_0;

#line 1203
        float3 _S321 = cyApplyFog_0(&_S320, color_2, _S310, kernelContext_46);

#line 1203
        color_1 = _S321;

#line 1200
    }
    else
    {

#line 1200
        color_1 = color_2;

#line 1200
    }

#line 1206
    return float4(color_1, (&surface_7)->opacity_0);
}


#line 1206
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 1206
struct pixelInput_0
{
    float3 relativePosition_11 [[user(TEXCOORD)]];
    float3 normal_11 [[user(TEXCOORD_1)]];
    float2 uv_18 [[user(TEXCOORD_2)]];
    [[flat]] uint drawIndex_1 [[user(TEXCOORD_3)]];
    float2 lightmapUv_1 [[user(TEXCOORD_4)]];
};


#line 1210
[[fragment]] pixelOutput_0 cyForwardFragment(pixelInput_0 _S322 [[stage_in]], float4 position_4 [[position]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(1)]], CyFrameGlobalSet_default_0 constant& cyFrameGlobals_1 [[buffer(0)]])
{

#line 1210
    thread KernelContext_0 kernelContext_47;

#line 1210
    (&kernelContext_47)->cyFrameView_0 = cyFrameView_1;

#line 1210
    (&kernelContext_47)->cyFrameGlobals_0 = &cyFrameGlobals_1;

    CyDrawInstance_0 _S323 = cyFrameView_1->drawInstances_0[_S322.drawIndex_1];

#line 1212
    Surface_0 _S324 = surfaceOf_0(_S323.material_0, cyFrameView_1->instances_0[_S323.instanceSlot_0].tint_0.xyz, _S322.uv_18, &kernelContext_47);

#line 1212
    thread Surface_0 _S325 = _S324;

#line 1212
    thread CyForwardVertex_0 _S326;

#line 1212
    (&_S326)->position_3 = position_4;

#line 1212
    (&_S326)->relativePosition_10 = _S322.relativePosition_11;

#line 1212
    (&_S326)->normal_9 = _S322.normal_11;

#line 1212
    (&_S326)->uv_17 = _S322.uv_18;

#line 1212
    (&_S326)->drawIndex_0 = _S322.drawIndex_1;

#line 1212
    (&_S326)->lightmapUv_0 = _S322.lightmapUv_1;

#line 1212
    float4 _S327 = cyShadeForward_0(&_S325, &_S326, &kernelContext_47);

#line 1212
    pixelOutput_0 _S328 = { _S327 };

    return _S328;
}

)cy_msl");
inline constexpr const auto& kFrameForwardFragmentMsl = kFrameForwardFragmentMslText.text;

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

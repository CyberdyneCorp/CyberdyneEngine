#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the rendering frame. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::pipeline {

/// depth_vertex.metal, 5641 bytes.
inline constexpr char kFrameDepthVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 145 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 187
float3 transformToRelative_0(const CyInstanceTransform_0 thread* instance_0, float3 modelPosition_0)
{
    float4 _S1 = float4(modelPosition_0, 1.0);
    return float3(dot(instance_0->row0_0, _S1), dot(instance_0->row1_0, _S1), dot(instance_0->row2_0, _S1));
}


#line 170
struct CyDrawPush_0
{
    uint drawIndex_0;
};


#line 132
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


#line 89 "src/rendering/shaders/cy/frame.slang"
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
};


#line 89
struct KernelContext_0
{
    CyDrawPush_0 constant* cyDraw_0;
    CyDrawInstance_0 device* cyDrawInstances_0;
    CyInstanceTransform_0 device* cyInstances_0;
    CyFrameData_0 constant* cyFrame_0;
};


#line 180
float4 transformToClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrame_0->relativeToClipRow0_0, _S2), dot(kernelContext_0->cyFrame_0->relativeToClipRow1_0, _S2), dot(kernelContext_0->cyFrame_0->relativeToClipRow2_0, _S2), dot(kernelContext_0->cyFrame_0->relativeToClipRow3_0, _S2));
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


#line 196 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_0)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_0), dot(instance_1->row1_0.xyz, direction_0), dot(instance_1->row2_0.xyz, direction_0)));
}


#line 198
struct cyDepthVertex_Result_0
{
    float4 position_0 [[position]];
    float3 normal_1 [[user(TEXCOORD)]];
    float4 currentClip_0 [[user(TEXCOORD_1)]];
    float4 previousClip_0 [[user(TEXCOORD_2)]];
};


#line 198
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
};


#line 239
struct CyDepthVertex_0
{
    float4 position_1;
    float3 normal_2;
    float4 currentClip_1;
    float4 previousClip_1;
};


#line 239
[[vertex]] cyDepthVertex_Result_0 cyDepthVertex(vertexInput_0 _S9 [[stage_in]], CyDrawPush_0 constant* cyDraw_1 [[buffer(7)]], CyDrawInstance_0 device* cyDrawInstances_1 [[buffer(4)]], CyInstanceTransform_0 device* cyInstances_1 [[buffer(5)]], CyFrameData_0 constant* cyFrame_1 [[buffer(0)]])
{

#line 239
    thread KernelContext_0 kernelContext_1;

#line 239
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 239
    (&kernelContext_1)->cyDrawInstances_0 = cyDrawInstances_1;

#line 239
    (&kernelContext_1)->cyInstances_0 = cyInstances_1;

#line 239
    (&kernelContext_1)->cyFrame_0 = cyFrame_1;

#line 251
    CyInstanceTransform_0 _S10 = cyInstances_1[cyDrawInstances_1[cyDraw_1->drawIndex_0].instanceSlot_0];

#line 251
    thread CyInstanceTransform_0 _S11 = _S10;

#line 251
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    thread CyDepthVertex_0 output_0;

#line 252
    float4 _S13 = transformToClip_0(_S12, &kernelContext_1);

    (&output_0)->position_1 = _S13;
    (&output_0)->currentClip_1 = _S13;
    float4 _S14 = float4(_S12, 1.0);
    (&output_0)->previousClip_1 = float4(dot((&kernelContext_1)->cyFrame_0->previousRelativeToClipRow0_0, _S14), dot((&kernelContext_1)->cyFrame_0->previousRelativeToClipRow1_0, _S14), dot((&kernelContext_1)->cyFrame_0->previousRelativeToClipRow2_0, _S14), dot((&kernelContext_1)->cyFrame_0->previousRelativeToClipRow3_0, _S14));



    float3 _S15 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 261
    thread CyInstanceTransform_0 _S16 = _S10;

#line 261
    float3 _S17 = rotateToRelative_0(&_S16, _S15);

#line 261
    (&output_0)->normal_2 = _S17;

#line 261
    thread cyDepthVertex_Result_0 _S18;

#line 261
    (&_S18)->position_0 = output_0.position_1;

#line 261
    (&_S18)->normal_1 = output_0.normal_2;

#line 261
    (&_S18)->currentClip_0 = output_0.currentClip_1;

#line 261
    (&_S18)->previousClip_0 = output_0.previousClip_1;

#line 261
    return _S18;
}

)cy_msl";

/// depth_fragment.metal, 1479 bytes.
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


#line 265 "src/rendering/shaders/cy/frame.slang"
struct CyDepthOutput_0
{
    float4 normalRoughness_0 [[color(0)]];
    float2 velocity_0 [[color(1)]];
};


#line 265
struct pixelInput_0
{
    float3 normal_1 [[user(TEXCOORD)]];
    float4 currentClip_0 [[user(TEXCOORD_1)]];
    float4 previousClip_0 [[user(TEXCOORD_2)]];
};

[[fragment]] CyDepthOutput_0 cyDepthFragment(pixelInput_0 _S4 [[stage_in]], float4 position_0 [[position]])
{
    thread CyDepthOutput_0 output_0;
    (&output_0)->normalRoughness_0 = float4(encodeOctahedral_0(normalize(_S4.normal_1)), 1.0, 1.0);
    float2 _S5 = _S4.currentClip_0.xy / float2(_S4.currentClip_0.w) ;
    float2 _S6 = _S4.previousClip_0.xy / float2(_S4.previousClip_0.w) ;
    (&output_0)->velocity_0 = float2((_S6.x - _S5.x) * 0.5, (_S5.y - _S6.y) * 0.5);

    return output_0;
}

)cy_msl";

/// forward_vertex.metal, 5527 bytes.
inline constexpr char kFrameForwardVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 145 "src/rendering/shaders/cy/frame.slang"
struct CyInstanceTransform_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 tint_0;
};


#line 187
float3 transformToRelative_0(const CyInstanceTransform_0 thread* instance_0, float3 modelPosition_0)
{
    float4 _S1 = float4(modelPosition_0, 1.0);
    return float3(dot(instance_0->row0_0, _S1), dot(instance_0->row1_0, _S1), dot(instance_0->row2_0, _S1));
}


#line 170
struct CyDrawPush_0
{
    uint drawIndex_0;
};


#line 132
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


#line 89 "src/rendering/shaders/cy/frame.slang"
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
};


#line 89
struct KernelContext_0
{
    CyDrawPush_0 constant* cyDraw_0;
    CyDrawInstance_0 device* cyDrawInstances_0;
    CyInstanceTransform_0 device* cyInstances_0;
    CyFrameData_0 constant* cyFrame_0;
};


#line 180
float4 transformToClip_0(float3 relative_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(kernelContext_0->cyFrame_0->relativeToClipRow0_0, _S2), dot(kernelContext_0->cyFrame_0->relativeToClipRow1_0, _S2), dot(kernelContext_0->cyFrame_0->relativeToClipRow2_0, _S2), dot(kernelContext_0->cyFrame_0->relativeToClipRow3_0, _S2));
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


#line 196 "src/rendering/shaders/cy/frame.slang"
float3 rotateToRelative_0(const CyInstanceTransform_0 thread* instance_1, float3 direction_0)
{
    return normalize(float3(dot(instance_1->row0_0.xyz, direction_0), dot(instance_1->row1_0.xyz, direction_0), dot(instance_1->row2_0.xyz, direction_0)));
}


#line 198
struct cyForwardVertex_Result_0
{
    float4 position_0 [[position]];
    float3 relativePosition_0 [[user(TEXCOORD)]];
    float3 normal_1 [[user(TEXCOORD_1)]];
    float2 uv_0 [[user(TEXCOORD_2)]];
    uint drawIndex_1 [[user(TEXCOORD_3)]];
};


#line 198
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
    float4 packedNormal_0 [[attribute(1)]];
    float2 uv_1 [[attribute(2)]];
};


#line 285
struct CyForwardVertex_0
{
    float4 position_1;
    float3 relativePosition_1;
    float3 normal_2;
    float2 uv_2;
    [[flat]] uint drawIndex_2;
};


#line 285
[[vertex]] cyForwardVertex_Result_0 cyForwardVertex(vertexInput_0 _S9 [[stage_in]], CyDrawPush_0 constant* cyDraw_1 [[buffer(7)]], CyDrawInstance_0 device* cyDrawInstances_1 [[buffer(4)]], CyInstanceTransform_0 device* cyInstances_1 [[buffer(5)]], CyFrameData_0 constant* cyFrame_1 [[buffer(0)]])
{

#line 285
    thread KernelContext_0 kernelContext_1;

#line 285
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 285
    (&kernelContext_1)->cyDrawInstances_0 = cyDrawInstances_1;

#line 285
    (&kernelContext_1)->cyInstances_0 = cyInstances_1;

#line 285
    (&kernelContext_1)->cyFrame_0 = cyFrame_1;

#line 299
    CyInstanceTransform_0 _S10 = cyInstances_1[cyDrawInstances_1[cyDraw_1->drawIndex_0].instanceSlot_0];

    thread CyForwardVertex_0 output_0;

#line 301
    thread CyInstanceTransform_0 _S11 = _S10;

#line 301
    float3 _S12 = transformToRelative_0(&_S11, _S9.modelPosition_1);
    (&output_0)->relativePosition_1 = _S12;

#line 302
    float4 _S13 = transformToClip_0(_S12, &kernelContext_1);
    (&output_0)->position_1 = _S13;



    float3 _S14 = decodeOctahedral_0(_S9.packedNormal_0.xy);

#line 307
    thread CyInstanceTransform_0 _S15 = _S10;

#line 307
    float3 _S16 = rotateToRelative_0(&_S15, _S14);

#line 307
    (&output_0)->normal_2 = _S16;
    (&output_0)->uv_2 = _S9.uv_1;
    (&output_0)->drawIndex_2 = cyDraw_1->drawIndex_0;

#line 309
    thread cyForwardVertex_Result_0 _S17;

#line 309
    (&_S17)->position_0 = output_0.position_1;

#line 309
    (&_S17)->relativePosition_0 = output_0.relativePosition_1;

#line 309
    (&_S17)->normal_1 = output_0.normal_2;

#line 309
    (&_S17)->uv_0 = output_0.uv_2;

#line 309
    (&_S17)->drawIndex_1 = output_0.drawIndex_2;

#line 309
    return _S17;
}

)cy_msl";

/// forward_fragment.metal, 14953 bytes.
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


#line 132 "src/rendering/shaders/cy/frame.slang"
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


#line 89 "src/rendering/shaders/cy/frame.slang"
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
};


#line 20 "src/rendering/shaders/cy/light.slang"
struct Light_0
{
    float3 positionRelativeToCamera_0;
    float range_0;
    float3 direction_0;
    float intensity_0;
    float3 color_0;
    uint kind_0;
    float2 spotScaleBias_0;
    float2 padding_0;
};


#line 361 "src/rendering/shaders/cy/frame.slang"
struct KernelContext_0
{
    CyDrawInstance_0 device* cyDrawInstances_0;
    CyInstanceTransform_0 device* cyInstances_0;
    CyFrameData_0 constant* cyFrame_0;
    uint device* cyMaterialWords_0;
    Light_0 device* cyLights_0;
    uint2 device* cyClusterHeaders_0;
    uint device* cyClusterIndices_0;
};


#line 215
float3 readMaterialFloat3_0(uint material_1, uint wordOffset_0, KernelContext_0 thread* kernelContext_0)
{
    uint _S1 = material_1 * kernelContext_0->cyFrame_0->counts_0.y + wordOffset_0;
    return float3((as_type<float>((kernelContext_0->cyMaterialWords_0[_S1]))), (as_type<float>((kernelContext_0->cyMaterialWords_0[_S1 + 1U]))), (as_type<float>((kernelContext_0->cyMaterialWords_0[_S1 + 2U]))));
}


#line 210
float readMaterialFloat_0(uint material_2, uint wordOffset_1, KernelContext_0 thread* kernelContext_1)
{
    return (as_type<float>((kernelContext_1->cyMaterialWords_0[material_2 * kernelContext_1->cyFrame_0->counts_0.y + wordOffset_1])));
}


#line 223
Surface_0 surfaceOf_0(uint material_3, float3 tint_1, KernelContext_0 thread* kernelContext_2)
{
    thread Surface_0 surface_2 = defaultSurface_0();

#line 225
    float3 _S2 = readMaterialFloat3_0(material_3, kernelContext_2->cyFrame_0->materialOffsets_0.x, kernelContext_2);
    (&surface_2)->albedo_0 = _S2 * tint_1;

#line 226
    float _S3 = readMaterialFloat_0(material_3, kernelContext_2->cyFrame_0->materialOffsets_0.y, kernelContext_2);
    (&surface_2)->roughness_0 = clamp(_S3, 0.01999999955296516, 1.0);

#line 227
    float _S4 = readMaterialFloat_0(material_3, kernelContext_2->cyFrame_0->materialOffsets_0.z, kernelContext_2);
    (&surface_2)->metallic_0 = saturate(_S4);

#line 228
    float3 _S5 = readMaterialFloat3_0(material_3, kernelContext_2->cyFrame_0->materialOffsets_0.w, kernelContext_2);
    (&surface_2)->emission_0 = _S5;
    return surface_2;
}


#line 42 "src/rendering/shaders/cy/light.slang"
float distanceAttenuation_0(float distanceSquared_0, float range_1)
{
    float _S6 = distanceSquared_0 / max(range_1 * range_1, 9.99999997475242708e-07);
    float _S7 = saturate(1.0 - _S6 * _S6);
    return _S7 * _S7 / max(distanceSquared_0, 0.00009999999747379);
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
    uint _S8 = light_0->kind_0;
    if((light_0->kind_0) == 0U)
    {
        (&result_0)->direction_1 = - light_0->direction_0;
        (&result_0)->attenuation_0 = 1.0;
        (&result_0)->illuminance_0 = light_0->color_0 * float3(light_0->intensity_0) ;
        return result_0;
    }

    float3 _S9 = light_0->positionRelativeToCamera_0 - surfaceRelativeToCamera_0;
    float _S10 = dot(_S9, _S9);
    (&result_0)->direction_1 = _S9 * float3(rsqrt(max(_S10, 9.99999993922529029e-09))) ;
    (&result_0)->attenuation_0 = distanceAttenuation_0(_S10, light_0->range_0);

    if(_S8 == 2U)
    {

        float _S11 = saturate(dot(- (&result_0)->direction_1, light_0->direction_0) * light_0->spotScaleBias_0.x + light_0->spotScaleBias_0.y);
        (&result_0)->attenuation_0 = (&result_0)->attenuation_0 * (_S11 * _S11);

#line 65
    }

#line 71
    (&result_0)->illuminance_0 = light_0->color_0 * float3((light_0->intensity_0 * (&result_0)->attenuation_0)) ;
    return result_0;
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
    float _S12 = roughness_1 * roughness_1;
    float _S13 = _S12 * _S12;
    float _S14 = normalDotHalf_0 * normalDotHalf_0 * (_S13 - 1.0) + 1.0;
    return _S13 / max(3.14159274101257324 * _S14 * _S14, 1.00000001168609742e-07);
}


float visibilitySmithGgxCorrelated_0(float normalDotView_0, float normalDotLight_0, float roughness_2)
{

    float _S15 = roughness_2 * roughness_2;
    float _S16 = _S15 * _S15;
    float _S17 = 1.0 - _S16;



    return 0.5 / max(normalDotLight_0 * sqrt(normalDotView_0 * normalDotView_0 * _S17 + _S16) + normalDotView_0 * sqrt(normalDotLight_0 * normalDotLight_0 * _S17 + _S16), 1.00000001168609742e-07);
}

float3 fresnelSchlick_0(float3 f0_0, float viewDotHalf_0)
{

    return f0_0 + (float3(1.0)  - f0_0) * float3(pow(saturate(1.0 - viewDotHalf_0), 5.0)) ;
}


#line 45
float3 specularGgx_0(float3 normal_1, float3 view_0, float3 light_1, float roughness_3, float3 f0_1)
{
    float3 _S18 = normalize(view_0 + light_1);

#line 56
    return float3((distributionGgx_0(saturate(dot(normal_1, _S18)), roughness_3) * visibilitySmithGgxCorrelated_0(saturate(dot(normal_1, view_0)) + 0.00000999999974738, saturate(dot(normal_1, light_1)), roughness_3)))  * fresnelSchlick_0(f0_1, saturate(dot(view_0, _S18)));
}


#line 75 "src/rendering/shaders/cy/material.slang"
float3 shadeSurfaceWithLight_0(const Surface_0 thread* surface_3, float3 worldNormal_0, float3 viewDirection_0, const LightSample_0 thread* sample_0)
{

#line 76
    float3 _S19 = sample_0->direction_1;

    float _S20 = saturate(dot(worldNormal_0, sample_0->direction_1));
    if(_S20 <= 0.0)
    {
        return float3(0.0) ;
    }

#line 87
    return (diffuseLambert_0(surface_3->albedo_0 * float3((1.0 - surface_3->metallic_0)) ) + specularGgx_0(worldNormal_0, viewDirection_0, _S19, surface_3->roughness_0, computeF0_0(surface_3->albedo_0, surface_3->metallic_0))) * sample_0->illuminance_0 * float3(_S20) ;
}


#line 202 "src/rendering/shaders/cy/frame.slang"
float viewDepthOf_0(float3 relative_0, KernelContext_0 thread* kernelContext_3)
{



    return - dot(kernelContext_3->cyFrame_0->relativeToViewRow2_0, float4(relative_0, 1.0));
}


#line 21 "src/rendering/shaders/cy/cluster.slang"
uint clusterSliceOf_0(const ClusterGrid_0 constant* grid_0, float viewDepth_0)
{

    return uint(clamp(log2(max(viewDepth_0, grid_0->nearPlane_0)) * grid_0->sliceScale_0 + grid_0->sliceBias_0, 0.0, float(grid_0->dimensions_0.z - 1U)));
}


uint3 clusterCoordOf_0(const ClusterGrid_0 constant* grid_1, uint2 pixel_0, uint2 renderExtent_0, float viewDepth_1)
{
    uint2 _S21 = grid_1->dimensions_0.xy;
    uint2 _S22 = min(uint2(float2(pixel_0) / float2(renderExtent_0) * float2(_S21)), _S21 - uint2(1U) );

#line 31
    uint _S23 = clusterSliceOf_0(grid_1, viewDepth_1);

#line 31
    return uint3(_S22, _S23);
}



uint clusterIndexOf_0(const ClusterGrid_0 constant* grid_2, uint3 coord_0)
{
    return coord_0.x + grid_2->dimensions_0.x * (coord_0.y + grid_2->dimensions_0.y * coord_0.z);
}


#line 319 "src/rendering/shaders/cy/frame.slang"
float3 accumulateLights_0(const Surface_0 thread* surface_4, float3 relativePosition_0, float3 normal_2, float3 viewDir_0, uint2 pixel_1, KernelContext_0 thread* kernelContext_4)
{

    float3 _S24 = float3(0.0) ;
    uint _S25 = kernelContext_4->cyFrame_0->counts_0.x;

#line 323
    uint global_0 = 0U;

#line 323
    float3 lit_0 = _S24;

#line 332
    for(;;)
    {

#line 332
        if(global_0 < _S25)
        {
        }
        else
        {

#line 332
            break;
        }
        if((kernelContext_4->cyLights_0[global_0].kind_0) != 0U)
        {
            global_0 = global_0 + 1U;

#line 332
            continue;
        }

#line 332
        thread Light_0 _S26 = kernelContext_4->cyLights_0[global_0];

#line 332
        LightSample_0 _S27 = evaluateLight_0(&_S26, relativePosition_0);

#line 332
        thread LightSample_0 _S28 = _S27;

#line 332
        float3 _S29 = shadeSurfaceWithLight_0(surface_4, normal_2, viewDir_0, &_S28);

#line 332
        lit_0 = lit_0 + _S29;

#line 332
        global_0 = global_0 + 1U;

#line 332
    }

#line 332
    bool _S30;

#line 342
    if((kernelContext_4->cyFrame_0->counts_0.w) == 0U)
    {

#line 342
        _S30 = true;

#line 342
    }
    else
    {

#line 342
        _S30 = ((&kernelContext_4->cyFrame_0->clusterGrid_0)->dimensions_0.z) == 0U;

#line 342
    }

#line 342
    uint index_0;

#line 342
    if(_S30)
    {

#line 342
        index_0 = 0U;

        for(;;)
        {

#line 344
            if(index_0 < _S25)
            {
            }
            else
            {

#line 344
                break;
            }
            if((kernelContext_4->cyLights_0[index_0].kind_0) == 0U)
            {
                index_0 = index_0 + 1U;

#line 344
                continue;
            }

#line 344
            thread Light_0 _S31 = kernelContext_4->cyLights_0[index_0];

#line 344
            LightSample_0 _S32 = evaluateLight_0(&_S31, relativePosition_0);

#line 344
            thread LightSample_0 _S33 = _S32;

#line 344
            float3 _S34 = shadeSurfaceWithLight_0(surface_4, normal_2, viewDir_0, &_S33);

#line 344
            lit_0 = lit_0 + _S34;

#line 344
            index_0 = index_0 + 1U;

#line 344
        }

#line 353
        return lit_0;
    }

    uint2 _S35 = uint2(kernelContext_4->cyFrame_0->extentAndInverse_0.xy);

#line 356
    float _S36 = viewDepthOf_0(relativePosition_0, kernelContext_4);

#line 356
    uint3 _S37 = clusterCoordOf_0(&kernelContext_4->cyFrame_0->clusterGrid_0, pixel_1, _S35, _S36);

#line 356
    uint _S38 = clusterIndexOf_0(&kernelContext_4->cyFrame_0->clusterGrid_0, _S37);

#line 361
    uint2 _S39 = kernelContext_4->cyClusterHeaders_0[_S38 * kernelContext_4->cyFrame_0->counts_0.z];

#line 361
    index_0 = 0U;
    for(;;)
    {

#line 362
        if(index_0 < (_S39.y))
        {
        }
        else
        {

#line 362
            break;
        }
        uint _S40 = kernelContext_4->cyClusterIndices_0[_S39.x + index_0];
        if(_S40 >= _S25)
        {

#line 365
            _S30 = true;

#line 365
        }
        else
        {

#line 365
            _S30 = (kernelContext_4->cyLights_0[_S40].kind_0) == 0U;

#line 365
        }

#line 365
        if(_S30)
        {
            index_0 = index_0 + 1U;

#line 362
            continue;
        }

#line 362
        thread Light_0 _S41 = kernelContext_4->cyLights_0[_S40];

#line 362
        LightSample_0 _S42 = evaluateLight_0(&_S41, relativePosition_0);

#line 362
        thread LightSample_0 _S43 = _S42;

#line 362
        float3 _S44 = shadeSurfaceWithLight_0(surface_4, normal_2, viewDir_0, &_S43);

#line 362
        lit_0 = lit_0 + _S44;

#line 362
        index_0 = index_0 + 1U;

#line 362
    }

#line 372
    return lit_0;
}


#line 372
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 372
struct pixelInput_0
{
    float3 relativePosition_1 [[user(TEXCOORD)]];
    float3 normal_3 [[user(TEXCOORD_1)]];
    float2 uv_0 [[user(TEXCOORD_2)]];
    [[flat]] uint drawIndex_0 [[user(TEXCOORD_3)]];
};


#line 376
[[fragment]] pixelOutput_0 cyForwardFragment(pixelInput_0 _S45 [[stage_in]], float4 position_0 [[position]], CyDrawInstance_0 device* cyDrawInstances_1 [[buffer(4)]], CyInstanceTransform_0 device* cyInstances_1 [[buffer(5)]], CyFrameData_0 constant* cyFrame_1 [[buffer(0)]], uint device* cyMaterialWords_1 [[buffer(6)]], Light_0 device* cyLights_1 [[buffer(1)]], uint2 device* cyClusterHeaders_1 [[buffer(2)]], uint device* cyClusterIndices_1 [[buffer(3)]])
{

#line 376
    thread KernelContext_0 kernelContext_5;

#line 376
    (&kernelContext_5)->cyDrawInstances_0 = cyDrawInstances_1;

#line 376
    (&kernelContext_5)->cyInstances_0 = cyInstances_1;

#line 376
    (&kernelContext_5)->cyFrame_0 = cyFrame_1;

#line 376
    (&kernelContext_5)->cyMaterialWords_0 = cyMaterialWords_1;

#line 376
    (&kernelContext_5)->cyLights_0 = cyLights_1;

#line 376
    (&kernelContext_5)->cyClusterHeaders_0 = cyClusterHeaders_1;

#line 376
    (&kernelContext_5)->cyClusterIndices_0 = cyClusterIndices_1;

    uint2 _S46 = uint2(position_0.xy);
    CyDrawInstance_0 _S47 = cyDrawInstances_1[_S45.drawIndex_0];

#line 379
    Surface_0 _S48 = surfaceOf_0(_S47.material_0, cyInstances_1[_S47.instanceSlot_0].tint_0.xyz, &kernelContext_5);



    float3 _S49 = normalize(_S45.normal_3);


    float3 _S50 = normalize(- _S45.relativePosition_1);

#line 386
    thread Surface_0 _S51 = _S48;

#line 386
    float3 _S52 = accumulateLights_0(&_S51, _S45.relativePosition_1, _S49, _S50, _S46, &kernelContext_5);

#line 386
    pixelOutput_0 _S53 = { float4(_S52 + _S48.emission_0 + _S48.albedo_0 * (&kernelContext_5)->cyFrame_0->ambientAndOcclusion_0.xyz * float3(_S48.occlusion_0) , _S48.opacity_0) };

#line 391
    return _S53;
}

)cy_msl";

/// resolve_vertex.metal, 936 bytes.
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

#line 66 "src/rendering/shaders/cy/fullscreen.slang"
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

/// resolve_fragment.metal, 3665 bytes.
inline constexpr char kFrameResolveFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 28 "src/rendering/shaders/cy/tonemap.slang"
float3 applyExposure_0(float3 linear_0, float exposureStops_0)
{
    return linear_0 * float3(exp2(exposureStops_0)) ;
}


#line 90 "src/rendering/shaders/cy/fullscreen.slang"
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


#line 75 "src/rendering/shaders/cy/fullscreen.slang"
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

/// temporal_fragment.metal, 3900 bytes.
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


#line 124 "src/rendering/shaders/cy/fullscreen.slang"
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
    float4 _S4 = viewSet_1->frame_0->temporalJitter_0;
    float4 _S5 = ((passSet_1->sceneColor_0).sample((passSet_1->linearClamp_0), (_S1.uv_0), level((0.0))));

    float3 _S6 = _S5.xyz;

#line 131
    float3 minimum_0 = _S6;

#line 131
    float3 maximum_0 = _S6;

#line 131
    int y_0 = int(-1);

    for(;;)
    {

#line 133
        if(y_0 <= int(1))
        {
        }
        else
        {

#line 133
            break;
        }

#line 133
        int x_0 = int(-1);

        for(;;)
        {

#line 135
            if(x_0 <= int(1))
            {
            }
            else
            {

#line 135
                break;
            }

            float3 _S7 = ((passSet_1->sceneColor_0).sample((passSet_1->linearClamp_0), (_S1.uv_0 + float2(float(x_0), float(y_0)) * _S2), level((0.0)))).xyz;
            float3 _S8 = min(minimum_0, _S7);
            float3 _S9 = max(maximum_0, _S7);

#line 135
            int x_1 = x_0 + int(1);

#line 135
            minimum_0 = _S8;

#line 135
            maximum_0 = _S9;

#line 135
            x_0 = x_1;

#line 135
        }

#line 133
        y_0 = y_0 + int(1);

#line 133
    }

#line 145
    float _S10 = (((&kernelContext_0)->passSet_0->depth_0).sample((passSet_1->linearClamp_0), (_S1.uv_0), level((0.0))).x);

    float2 _S11 = _S1.uv_0 + (((&kernelContext_0)->passSet_0->velocity_0).sample((passSet_1->linearClamp_0), (_S1.uv_0), level((0.0))).xy) + (_S4.zw - _S4.xy) * _S2;

#line 147
    bool _S12;
    if(all(_S11 >= (float2(0.0) )))
    {

#line 148
        _S12 = all(_S11 <= (float2(1.0) ));

#line 148
    }
    else
    {

#line 148
        _S12 = false;

#line 148
    }

    float3 _S13 = clamp((((&kernelContext_0)->passSet_0->historyColor_0).sample((passSet_1->linearClamp_0), (_S11), level((0.0)))).xyz, minimum_0, maximum_0);
    if((_S3.x) > 0.5)
    {
    }
    else
    {

#line 151
        _S12 = false;

#line 151
    }

#line 151
    if(_S12)
    {

#line 151
        _S12 = _S10 > 0.0;

#line 151
    }
    else
    {

#line 151
        _S12 = false;

#line 151
    }

#line 151
    float _S14;

#line 151
    if(_S12)
    {

#line 151
        _S14 = _S3.y;

#line 151
    }
    else
    {

#line 151
        _S14 = 0.0;

#line 151
    }

#line 151
    pixelOutput_0 _S15 = { float4(mix(_S6, _S13, float3(_S14) ), _S5.w) };
    return _S15;
}

)cy_msl";

}  // namespace cy::rendering::pipeline

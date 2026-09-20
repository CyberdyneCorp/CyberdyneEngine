#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for samples/03-first-light. GENERATED — do not edit by hand.
//
// Produced by samples/03-first-light/shaders/embed_msl.py from first_light.slang. The native
// artefacts are checked in so a shipping build does not need to link the Slang compiler.

#include <cy/core/base/types.h>

namespace cy::sample::first_light {

/// samples.03-first-light.shaders.first_light.shadowVertex.metal, 2861 bytes.
inline constexpr char kFirstLightShadowVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 60 "samples.03-first-light.shaders.first_light"
struct FrameConstants_0
{
    float4 viewProjectionRow0_0;
    float4 viewProjectionRow1_0;
    float4 viewProjectionRow2_0;
    float4 viewProjectionRow3_0;
    float4 lightViewProjectionRow0_0;
    float4 lightViewProjectionRow1_0;
    float4 lightViewProjectionRow2_0;
    float4 lightViewProjectionRow3_0;
    float4 sunDirectionAndBias_0;
    float4 sunColorAndAmbient_0;
    float4 shadowControl_0;
};


#line 87
struct FirstLightGlobals_default_0
{
    FrameConstants_0 constant* frame_0;
    texture2d<float, access::sample> albedoTexture_0;
    sampler albedoSampler_0;
    texture2d<float, access::sample> shadowMap_0;
    sampler shadowSampler_0;
};


#line 100
struct ObjectPush_0
{
    float4 modelRow0_0;
    float4 modelRow1_0;
    float4 modelRow2_0;
    float4 baseColor_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    FirstLightGlobals_default_0 constant* globals_0;
    ObjectPush_0 constant* object_0;
};


#line 134 "samples.03-first-light.shaders.first_light"
float3 toCameraRelative_0(float3 objectPosition_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S1 = float4(objectPosition_0, 1.0);
    return float3(dot(kernelContext_0->object_0->modelRow0_0, _S1), dot(kernelContext_0->object_0->modelRow1_0, _S1), dot(kernelContext_0->object_0->modelRow2_0, _S1));
}


#line 152
float4 transform_0(float4 row0_0, float4 row1_0, float4 row2_0, float4 row3_0, float3 relative_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(row0_0, _S2), dot(row1_0, _S2), dot(row2_0, _S2), dot(row3_0, _S2));
}


#line 155
struct shadowVertex_Result_0
{
    float4 clip_0 [[position]];
};


#line 113
struct vertexInput_0
{
    float3 position_0 [[attribute(0)]];
    float3 normal_0 [[attribute(1)]];
    float2 uv_0 [[attribute(2)]];
};


#line 128
struct DepthOnlyOutput_0
{
    float4 clip_1;
};


#line 128
[[vertex]] shadowVertex_Result_0 shadowVertex(vertexInput_0 _S3 [[stage_in]], FirstLightGlobals_default_0 constant* globals_1 [[buffer(0)]], ObjectPush_0 constant* object_1 [[buffer(1)]])
{

#line 128
    thread KernelContext_0 kernelContext_1;

#line 128
    (&kernelContext_1)->globals_0 = globals_1;

#line 128
    (&kernelContext_1)->object_0 = object_1;

#line 161
    thread DepthOnlyOutput_0 output_0;

#line 161
    float3 _S4 = toCameraRelative_0(_S3.position_0, &kernelContext_1);
    (&output_0)->clip_1 = transform_0(globals_1->frame_0->lightViewProjectionRow0_0, globals_1->frame_0->lightViewProjectionRow1_0, globals_1->frame_0->lightViewProjectionRow2_0, globals_1->frame_0->lightViewProjectionRow3_0, _S4);

#line 162
    thread shadowVertex_Result_0 _S5;

#line 162
    (&_S5)->clip_0 = output_0.clip_1;

#line 162
    return _S5;
}

)cy_msl";

/// samples.03-first-light.shaders.first_light.forwardVertex.metal, 3773 bytes.
inline constexpr char kFirstLightForwardVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 100 "samples.03-first-light.shaders.first_light"
struct ObjectPush_0
{
    float4 modelRow0_0;
    float4 modelRow1_0;
    float4 modelRow2_0;
    float4 baseColor_0;
};


#line 60
struct FrameConstants_0
{
    float4 viewProjectionRow0_0;
    float4 viewProjectionRow1_0;
    float4 viewProjectionRow2_0;
    float4 viewProjectionRow3_0;
    float4 lightViewProjectionRow0_0;
    float4 lightViewProjectionRow1_0;
    float4 lightViewProjectionRow2_0;
    float4 lightViewProjectionRow3_0;
    float4 sunDirectionAndBias_0;
    float4 sunColorAndAmbient_0;
    float4 shadowControl_0;
};


#line 87
struct FirstLightGlobals_default_0
{
    FrameConstants_0 constant* frame_0;
    texture2d<float, access::sample> albedoTexture_0;
    sampler albedoSampler_0;
    texture2d<float, access::sample> shadowMap_0;
    sampler shadowSampler_0;
};


#line 87
struct KernelContext_0
{
    ObjectPush_0 constant* object_0;
    FirstLightGlobals_default_0 constant* globals_0;
};


#line 134
float3 toCameraRelative_0(float3 objectPosition_0, KernelContext_0 thread* kernelContext_0)
{
    float4 _S1 = float4(objectPosition_0, 1.0);
    return float3(dot(kernelContext_0->object_0->modelRow0_0, _S1), dot(kernelContext_0->object_0->modelRow1_0, _S1), dot(kernelContext_0->object_0->modelRow2_0, _S1));
}


#line 145
float3 rotateNormal_0(float3 objectNormal_0, KernelContext_0 thread* kernelContext_1)
{
    return normalize(float3(dot(kernelContext_1->object_0->modelRow0_0.xyz, objectNormal_0), dot(kernelContext_1->object_0->modelRow1_0.xyz, objectNormal_0), dot(kernelContext_1->object_0->modelRow2_0.xyz, objectNormal_0)));
}



float4 transform_0(float4 row0_0, float4 row1_0, float4 row2_0, float4 row3_0, float3 relative_0)
{
    float4 _S2 = float4(relative_0, 1.0);
    return float4(dot(row0_0, _S2), dot(row1_0, _S2), dot(row2_0, _S2), dot(row3_0, _S2));
}


#line 113
struct forwardVertex_Result_0
{
    float4 clip_0 [[position]];
    float3 positionRelativeToCamera_0 [[user(TEXCOORD_1)]];
    float3 normal_0 [[user(TEXCOORD_2)]];
    float2 uv_0 [[user(TEXCOORD_3)]];
};


#line 113
struct vertexInput_0
{
    float3 position_0 [[attribute(0)]];
    float3 normal_1 [[attribute(1)]];
    float2 uv_1 [[attribute(2)]];
};

struct ForwardVertexOutput_0
{
    float4 clip_1;
    float3 positionRelativeToCamera_1;
    float3 normal_2;
    float2 uv_2;
};


#line 120
[[vertex]] forwardVertex_Result_0 forwardVertex(vertexInput_0 _S3 [[stage_in]], ObjectPush_0 constant* object_1 [[buffer(1)]], FirstLightGlobals_default_0 constant* globals_1 [[buffer(0)]])
{

#line 120
    thread KernelContext_0 kernelContext_2;

#line 120
    (&kernelContext_2)->object_0 = object_1;

#line 120
    (&kernelContext_2)->globals_0 = globals_1;

#line 173
    thread ForwardVertexOutput_0 output_0;

#line 173
    float3 _S4 = toCameraRelative_0(_S3.position_0, &kernelContext_2);
    (&output_0)->positionRelativeToCamera_1 = _S4;

#line 174
    float3 _S5 = rotateNormal_0(_S3.normal_1, &kernelContext_2);
    (&output_0)->normal_2 = _S5;
    (&output_0)->uv_2 = _S3.uv_1;
    (&output_0)->clip_1 = transform_0((&kernelContext_2)->globals_0->frame_0->viewProjectionRow0_0, (&kernelContext_2)->globals_0->frame_0->viewProjectionRow1_0, (&kernelContext_2)->globals_0->frame_0->viewProjectionRow2_0, (&kernelContext_2)->globals_0->frame_0->viewProjectionRow3_0, _S4);

#line 177
    thread forwardVertex_Result_0 _S6;

#line 177
    (&_S6)->clip_0 = output_0.clip_1;

#line 177
    (&_S6)->positionRelativeToCamera_0 = output_0.positionRelativeToCamera_1;

#line 177
    (&_S6)->normal_0 = output_0.normal_2;

#line 177
    (&_S6)->uv_0 = output_0.uv_2;

#line 177
    return _S6;
}

)cy_msl";

/// samples.03-first-light.shaders.first_light.forwardFragment.metal, 4570 bytes.
inline constexpr char kFirstLightForwardFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 152 "samples.03-first-light.shaders.first_light"
float4 transform_0(float4 row0_0, float4 row1_0, float4 row2_0, float4 row3_0, float3 relative_0)
{
    float4 _S1 = float4(relative_0, 1.0);
    return float4(dot(row0_0, _S1), dot(row1_0, _S1), dot(row2_0, _S1), dot(row3_0, _S1));
}


#line 60
struct FrameConstants_0
{
    float4 viewProjectionRow0_0;
    float4 viewProjectionRow1_0;
    float4 viewProjectionRow2_0;
    float4 viewProjectionRow3_0;
    float4 lightViewProjectionRow0_0;
    float4 lightViewProjectionRow1_0;
    float4 lightViewProjectionRow2_0;
    float4 lightViewProjectionRow3_0;
    float4 sunDirectionAndBias_0;
    float4 sunColorAndAmbient_0;
    float4 shadowControl_0;
};


#line 3099 "hlsl.meta.slang"
struct FirstLightGlobals_default_0
{
    FrameConstants_0 constant* frame_0;
    texture2d<float, access::sample> albedoTexture_0;
    sampler albedoSampler_0;
    texture2d<float, access::sample> shadowMap_0;
    sampler shadowSampler_0;
};


#line 100 "samples.03-first-light.shaders.first_light"
struct ObjectPush_0
{
    float4 modelRow0_0;
    float4 modelRow1_0;
    float4 modelRow2_0;
    float4 baseColor_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    FirstLightGlobals_default_0 constant* globals_0;
    ObjectPush_0 constant* object_0;
};


#line 184 "samples.03-first-light.shaders.first_light"
float sunVisibility_0(float3 positionRelativeToCamera_0, float3 normal_0, KernelContext_0 thread* kernelContext_0)
{
    if((kernelContext_0->globals_0->frame_0->shadowControl_0.x) < 0.5)
    {
        return 1.0;
    }

#line 194
    float4 _S2 = transform_0(kernelContext_0->globals_0->frame_0->lightViewProjectionRow0_0, kernelContext_0->globals_0->frame_0->lightViewProjectionRow1_0, kernelContext_0->globals_0->frame_0->lightViewProjectionRow2_0, kernelContext_0->globals_0->frame_0->lightViewProjectionRow3_0, positionRelativeToCamera_0 + normal_0 * float3(kernelContext_0->globals_0->frame_0->shadowControl_0.y) );



    float _S3 = _S2.w;

#line 198
    if(_S3 <= 0.0)
    {
        return 1.0;
    }
    float3 _S4 = _S2.xyz / float3(_S3) ;


    float _S5 = _S4.x * 0.5 + 0.5;

#line 205
    float _S6 = 0.5 - _S4.y * 0.5;

#line 205
    float2 _S7 = float2(_S5, _S6);

#line 205
    bool _S8;
    if(_S5 < 0.0)
    {

#line 206
        _S8 = true;

#line 206
    }
    else
    {

#line 206
        _S8 = _S5 > 1.0;

#line 206
    }

#line 206
    if(_S8)
    {

#line 206
        _S8 = true;

#line 206
    }
    else
    {

#line 206
        _S8 = _S6 < 0.0;

#line 206
    }

#line 206
    if(_S8)
    {

#line 206
        _S8 = true;

#line 206
    }
    else
    {

#line 206
        _S8 = _S6 > 1.0;

#line 206
    }

#line 206
    if(_S8)
    {
        return 1.0;
    }


    depth2d<float, access::sample> _S9;
    {
        auto _slang_ordinary_texture = kernelContext_0->globals_0->shadowMap_0;
        _S9 = *(depth2d<float, access::sample> thread*)(&_slang_ordinary_texture);
    }

#line 212
    float _S10 = ((_S9).sample_compare((kernelContext_0->globals_0->shadowSampler_0), (_S7), (_S4.z + kernelContext_0->globals_0->frame_0->sunDirectionAndBias_0.w), level((0.0))));

#line 212
    return _S10;
}


#line 212
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 212
struct pixelInput_0
{
    float3 positionRelativeToCamera_1 [[user(TEXCOORD_1)]];
    float3 normal_1 [[user(TEXCOORD_2)]];
    float2 uv_0 [[user(TEXCOORD_3)]];
};


#line 216
[[fragment]] pixelOutput_0 forwardFragment(pixelInput_0 _S11 [[stage_in]], float4 clip_0 [[position]], FirstLightGlobals_default_0 constant* globals_1 [[buffer(0)]], ObjectPush_0 constant* object_1 [[buffer(1)]])
{

#line 216
    thread KernelContext_0 kernelContext_1;

#line 216
    (&kernelContext_1)->globals_0 = globals_1;

#line 216
    (&kernelContext_1)->object_0 = object_1;

    float3 _S12 = normalize(_S11.normal_1);
    float3 _S13 = ((globals_1->albedoTexture_0).sample((globals_1->albedoSampler_0), (_S11.uv_0))).xyz * object_1->baseColor_0.xyz;

    float _S14 = max(dot(_S12, globals_1->frame_0->sunDirectionAndBias_0.xyz), 0.0);

#line 221
    float _S15 = sunVisibility_0(_S11.positionRelativeToCamera_1, _S12, &kernelContext_1);

#line 221
    pixelOutput_0 _S16 = { float4(pow(saturate(_S13 * (globals_1->frame_0->sunColorAndAmbient_0.xyz * float3(_S14)  * float3(_S15)  + float3(globals_1->frame_0->sunColorAndAmbient_0.w) )), float3(0.45454543828964233) ), 1.0) };

#line 229
    return _S16;
}

)cy_msl";

}  // namespace cy::sample::first_light

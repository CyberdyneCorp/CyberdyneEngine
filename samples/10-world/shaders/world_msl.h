#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for samples/10-world. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::sample::world {

/// world_vertex.metal, 1582 bytes.
inline constexpr char kWorldVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 3268 "core.meta.slang"
struct worldVertex_Result_0
{
    float4 clip_0 [[position]];
    float3 world_0 [[user(TEXCOORD)]];
    float3 normal_0 [[user(TEXCOORD_1)]];
    float3 color_0 [[user(TEXCOORD_2)]];
};


#line 3268
struct vertexInput_0
{
    float3 position_0 [[attribute(0)]];
    float3 normal_1 [[attribute(1)]];
    float3 color_1 [[attribute(2)]];
};


#line 55 "samples/10-world/shaders/world.slang"
struct WorldPush_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 row3_0;
    float4 light_0;
    float4 eye_0;
    float4 sun_0;
    float4 ambient_0;
};


#line 88
struct VertexOutput_0
{
    float4 clip_1;
    float3 world_1;
    float3 normal_2;
    float3 color_2;
};


#line 88
[[vertex]] worldVertex_Result_0 worldVertex(vertexInput_0 _S1 [[stage_in]], WorldPush_0 constant* push_0 [[buffer(0)]])
{

#line 99
    float4 _S2 = float4(_S1.position_0, 1.0);
    thread VertexOutput_0 output_0;
    (&output_0)->clip_1 = float4(dot(push_0->row0_0, _S2), dot(push_0->row1_0, _S2), dot(push_0->row2_0, _S2), dot(push_0->row3_0, _S2));

    (&output_0)->world_1 = _S1.position_0;
    (&output_0)->normal_2 = _S1.normal_1;
    (&output_0)->color_2 = _S1.color_1;

#line 105
    thread worldVertex_Result_0 _S3;

#line 105
    (&_S3)->clip_0 = output_0.clip_1;

#line 105
    (&_S3)->world_0 = output_0.world_1;

#line 105
    (&_S3)->normal_0 = output_0.normal_2;

#line 105
    (&_S3)->color_0 = output_0.color_2;

#line 105
    return _S3;
}

)cy_msl";

/// world_fragment.metal, 2270 bytes.
inline constexpr char kWorldFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 115 "samples/10-world/shaders/world.slang"
float3 tonemap_0(float3 radiance_0)
{

    return pow(saturate(radiance_0 / (float3(1.0)  + radiance_0)), float3(0.45454543828964233) );
}


#line 88
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 88
struct pixelInput_0
{
    float3 world_0 [[user(TEXCOORD)]];
    float3 normal_0 [[user(TEXCOORD_1)]];
    float3 color_0 [[user(TEXCOORD_2)]];
};


#line 55
struct WorldPush_0
{
    float4 row0_0;
    float4 row1_0;
    float4 row2_0;
    float4 row3_0;
    float4 light_0;
    float4 eye_0;
    float4 sun_0;
    float4 ambient_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    WorldPush_0 constant* push_0;
};


#line 122 "samples/10-world/shaders/world.slang"
[[fragment]] pixelOutput_0 worldFragment(pixelInput_0 _S1 [[stage_in]], float4 clip_0 [[position]], WorldPush_0 constant* push_1 [[buffer(0)]])
{

#line 122
    thread KernelContext_0 kernelContext_0;

#line 122
    (&kernelContext_0)->push_0 = push_1;

    if((push_1->eye_0.w) > 0.5)
    {

#line 124
        pixelOutput_0 _S2 = { float4(tonemap_0(_S1.color_0), 1.0) };



        return _S2;
    }

    float3 normal_1 = normalize(_S1.normal_0);
    float3 toEye_0 = normalize(push_1->eye_0.xyz - _S1.world_0);

#line 132
    float3 normal_2;
    if((dot(normal_1, toEye_0)) < 0.0)
    {

#line 133
        normal_2 = - normal_1;

#line 133
    }
    else
    {

#line 133
        normal_2 = normal_1;

#line 133
    }

#line 142
    float3 lit_0 = _S1.color_0 * ((&kernelContext_0)->push_0->ambient_0.xyz + (&kernelContext_0)->push_0->sun_0.xyz * float3(saturate(dot(normal_2, - (&kernelContext_0)->push_0->light_0.xyz))) );

#line 142
    float3 lit_1;

    if(((&kernelContext_0)->push_0->sun_0.w) > 0.0)
    {

#line 144
        lit_1 = lit_0 + (&kernelContext_0)->push_0->sun_0.xyz * float3((pow(saturate(dot(normal_2, normalize(- (&kernelContext_0)->push_0->light_0.xyz + toEye_0))), 120.0) * (&kernelContext_0)->push_0->sun_0.w)) ;

#line 144
    }
    else
    {

#line 144
        lit_1 = lit_0;

#line 144
    }

#line 144
    pixelOutput_0 _S3 = { float4(tonemap_0(lit_1), 1.0) };

#line 155
    return _S3;
}

)cy_msl";

}  // namespace cy::sample::world

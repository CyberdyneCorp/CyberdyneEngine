#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for bloom's four passes. GENERATED — do not edit by hand.
//
// Produced by src/rendering/pipeline/shaders/embed_bloom.py from
// src/rendering/shaders/cy/bloom.slang. The vertex stage is the frame's own `fullscreenVertex`
// (`frame_spirv.h`, `frame_msl.h`) and is not repeated here.

#include <cy/core/base/types.h>

namespace cy::rendering::pipeline {

/// bloomPrefilter.metal, 7482 bytes.
inline constexpr char kBloomPrefilterMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 75 "cy/bloom.slang"
struct BloomFragment_0
{
    float4 position_0;
    float2 uv_0;
};


#line 75
struct BloomPassSet_default_0
{
    texture2d<float, access::sample> source_0;
    texture2d<float, access::sample> detail_0;
    sampler linearClamp_0;
    texture2d<float, access::sample> lensDirt_0;
};


#line 48
struct BloomPush_0
{
    float4 extents_0;
    float4 detail_1;
    float4 threshold_0;
    float4 blend_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    BloomPassSet_default_0 constant* bloomSet_0;
    BloomPush_0 constant* bloomPush_0;
};


#line 179 "cy/bloom.slang"
float2 targetUv_0(const BloomFragment_0 thread* input_0, KernelContext_0 thread* kernelContext_0)
{
    return input_0->position_0.xy * kernelContext_0->bloomPush_0->extents_0.zw;
}


#line 24 "./cy/color.slang"
float luminance_0(float3 linear_0)
{
    return dot(linear_0, float3(0.2125999927520752, 0.71520000696182251, 0.07220000028610229));
}


#line 82 "cy/bloom.slang"
float3 prefilter_0(float3 colour_0, KernelContext_0 thread* kernelContext_1)
{
    float _S1 = kernelContext_1->bloomPush_0->threshold_0.x;
    float _S2 = kernelContext_1->bloomPush_0->threshold_0.y;
    float _S3 = luminance_0(colour_0);

#line 86
    float contribution_0;

    if(_S2 > 9.99999997475242708e-07)
    {
        float _S4 = clamp(_S3 - _S1 + _S2, 0.0, 2.0 * _S2);

#line 90
        contribution_0 = _S4 * _S4 / (4.0 * _S2);

#line 88
    }
    else
    {

#line 88
        contribution_0 = 0.0;

#line 88
    }

#line 93
    float _S5 = max(_S3 - _S1, contribution_0);
    if(_S3 > 9.99999997475242708e-07)
    {

#line 94
        contribution_0 = _S5 / _S3;

#line 94
    }
    else
    {

#line 94
        contribution_0 = 0.0;

#line 94
    }
    return colour_0 * float3(contribution_0) ;
}

float3 tap_0(texture2d<float, access::sample> texture_0, float2 uv_1, bool thresholded_0, KernelContext_0 thread* kernelContext_2)
{
    float3 _S6 = ((texture_0).sample((kernelContext_2->bloomSet_0->linearClamp_0), (uv_1), level((0.0)))).xyz;

#line 100
    float3 _S7;
    if(thresholded_0)
    {

#line 101
        float3 _S8 = prefilter_0(_S6, kernelContext_2);

#line 101
        _S7 = _S8;

#line 101
    }
    else
    {

#line 101
        _S7 = _S6;

#line 101
    }

#line 101
    return _S7;
}


#line 112
struct BoxWeights_0
{
    float centre_0;
    float topLeft_0;
    float topRight_0;
    float bottomLeft_0;
    float bottomRight_0;
};


#line 112
BoxWeights_0 BoxWeights_x24init_0(float centre_1, float topLeft_1, float topRight_1, float bottomLeft_1, float bottomRight_1)
{

#line 112
    thread BoxWeights_0 _S9;

    (&_S9)->centre_0 = centre_1;
    (&_S9)->topLeft_0 = topLeft_1;
    (&_S9)->topRight_0 = topRight_1;
    (&_S9)->bottomLeft_0 = bottomLeft_1;
    (&_S9)->bottomRight_0 = bottomRight_1;

#line 112
    return _S9;
}


#line 106
float karisWeight_0(float3 group_0, KernelContext_0 thread* kernelContext_3)
{
    return 1.0 / (1.0 + luminance_0(group_0) * kernelContext_3->bloomPush_0->threshold_0.z);
}


#line 124
float3 downsample13_0(texture2d<float, access::sample> texture_1, float2 uv_2, float2 texel_0, bool first_0, KernelContext_0 thread* kernelContext_4)
{

#line 124
    float3 _S10 = tap_0(texture_1, uv_2 + texel_0 * float2(-2.0, -2.0), first_0, kernelContext_4);

#line 124
    float3 _S11 = tap_0(texture_1, uv_2 + texel_0 * float2(0.0, -2.0), first_0, kernelContext_4);

#line 124
    float3 _S12 = tap_0(texture_1, uv_2 + texel_0 * float2(2.0, -2.0), first_0, kernelContext_4);

#line 124
    float3 _S13 = tap_0(texture_1, uv_2 + texel_0 * float2(-1.0, -1.0), first_0, kernelContext_4);

#line 124
    float3 _S14 = tap_0(texture_1, uv_2 + texel_0 * float2(1.0, -1.0), first_0, kernelContext_4);

#line 124
    float3 _S15 = tap_0(texture_1, uv_2 + texel_0 * float2(-2.0, 0.0), first_0, kernelContext_4);

#line 124
    float3 _S16 = tap_0(texture_1, uv_2, first_0, kernelContext_4);

#line 124
    float3 _S17 = tap_0(texture_1, uv_2 + texel_0 * float2(2.0, 0.0), first_0, kernelContext_4);

#line 124
    float3 _S18 = tap_0(texture_1, uv_2 + texel_0 * float2(-1.0, 1.0), first_0, kernelContext_4);

#line 124
    float3 _S19 = tap_0(texture_1, uv_2 + texel_0, first_0, kernelContext_4);

#line 124
    float3 _S20 = tap_0(texture_1, uv_2 + texel_0 * float2(-2.0, 2.0), first_0, kernelContext_4);

#line 124
    float3 _S21 = tap_0(texture_1, uv_2 + texel_0 * float2(0.0, 2.0), first_0, kernelContext_4);

#line 124
    float3 _S22 = tap_0(texture_1, uv_2 + texel_0 * float2(2.0, 2.0), first_0, kernelContext_4);

#line 124
    float3 _S23 = float3(0.25) ;

#line 140
    float3 _S24 = (_S13 + _S14 + _S18 + _S19) * _S23;
    float3 _S25 = (_S10 + _S11 + _S15 + _S16) * _S23;
    float3 _S26 = (_S11 + _S12 + _S16 + _S17) * _S23;
    float3 _S27 = (_S15 + _S16 + _S20 + _S21) * _S23;
    float3 _S28 = (_S16 + _S17 + _S21 + _S22) * _S23;

    thread BoxWeights_0 weights_0 = BoxWeights_x24init_0(0.5, 0.125, 0.125, 0.125, 0.125);

#line 146
    bool _S29;
    if(first_0)
    {

#line 147
        _S29 = (kernelContext_4->bloomPush_0->threshold_0.w) > 0.5;

#line 147
    }
    else
    {

#line 147
        _S29 = false;

#line 147
    }

#line 147
    if(_S29)
    {

#line 147
        float _S30 = karisWeight_0(_S24, kernelContext_4);

        (&weights_0)->centre_0 = (&weights_0)->centre_0 * _S30;

#line 149
        float _S31 = karisWeight_0(_S25, kernelContext_4);
        (&weights_0)->topLeft_0 = (&weights_0)->topLeft_0 * _S31;

#line 150
        float _S32 = karisWeight_0(_S26, kernelContext_4);
        (&weights_0)->topRight_0 = (&weights_0)->topRight_0 * _S32;

#line 151
        float _S33 = karisWeight_0(_S27, kernelContext_4);
        (&weights_0)->bottomLeft_0 = (&weights_0)->bottomLeft_0 * _S33;

#line 152
        float _S34 = karisWeight_0(_S28, kernelContext_4);
        (&weights_0)->bottomRight_0 = (&weights_0)->bottomRight_0 * _S34;

#line 147
    }

#line 159
    return (_S24 * float3((&weights_0)->centre_0)  + _S25 * float3((&weights_0)->topLeft_0)  + _S26 * float3((&weights_0)->topRight_0)  + _S27 * float3((&weights_0)->bottomLeft_0)  + _S28 * float3((&weights_0)->bottomRight_0) ) / float3(max((&weights_0)->centre_0 + (&weights_0)->topLeft_0 + (&weights_0)->topRight_0 + (&weights_0)->bottomLeft_0 + (&weights_0)->bottomRight_0, 9.99999997475242708e-07)) ;
}


#line 159
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 159
struct pixelInput_0
{
    float2 uv_3 [[user(TEXCOORD)]];
};


#line 185
[[fragment]] pixelOutput_0 bloomPrefilter(pixelInput_0 _S35 [[stage_in]], float4 position_1 [[position]], BloomPassSet_default_0 constant* bloomSet_1 [[buffer(2)]], BloomPush_0 constant* bloomPush_1 [[buffer(3)]])
{

#line 185
    thread KernelContext_0 kernelContext_5;

#line 185
    (&kernelContext_5)->bloomSet_0 = bloomSet_1;

#line 185
    (&kernelContext_5)->bloomPush_0 = bloomPush_1;

#line 185
    texture2d<float, access::sample> _S36 = bloomSet_1->source_0;

#line 185
    thread BloomFragment_0 _S37;

#line 185
    (&_S37)->position_0 = position_1;

#line 185
    (&_S37)->uv_0 = _S35.uv_3;

#line 185
    float2 _S38 = targetUv_0(&_S37, &kernelContext_5);

#line 185
    float3 _S39 = downsample13_0(_S36, _S38, (&kernelContext_5)->bloomPush_0->extents_0.xy, true, &kernelContext_5);

#line 185
    pixelOutput_0 _S40 = { float4(_S39, 1.0) };


    return _S40;
}

)cy_msl";

/// bloomDownsample.metal, 7484 bytes.
inline constexpr char kBloomDownsampleMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 75 "cy/bloom.slang"
struct BloomFragment_0
{
    float4 position_0;
    float2 uv_0;
};


#line 75
struct BloomPassSet_default_0
{
    texture2d<float, access::sample> source_0;
    texture2d<float, access::sample> detail_0;
    sampler linearClamp_0;
    texture2d<float, access::sample> lensDirt_0;
};


#line 48
struct BloomPush_0
{
    float4 extents_0;
    float4 detail_1;
    float4 threshold_0;
    float4 blend_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    BloomPassSet_default_0 constant* bloomSet_0;
    BloomPush_0 constant* bloomPush_0;
};


#line 179 "cy/bloom.slang"
float2 targetUv_0(const BloomFragment_0 thread* input_0, KernelContext_0 thread* kernelContext_0)
{
    return input_0->position_0.xy * kernelContext_0->bloomPush_0->extents_0.zw;
}


#line 24 "./cy/color.slang"
float luminance_0(float3 linear_0)
{
    return dot(linear_0, float3(0.2125999927520752, 0.71520000696182251, 0.07220000028610229));
}


#line 82 "cy/bloom.slang"
float3 prefilter_0(float3 colour_0, KernelContext_0 thread* kernelContext_1)
{
    float _S1 = kernelContext_1->bloomPush_0->threshold_0.x;
    float _S2 = kernelContext_1->bloomPush_0->threshold_0.y;
    float _S3 = luminance_0(colour_0);

#line 86
    float contribution_0;

    if(_S2 > 9.99999997475242708e-07)
    {
        float _S4 = clamp(_S3 - _S1 + _S2, 0.0, 2.0 * _S2);

#line 90
        contribution_0 = _S4 * _S4 / (4.0 * _S2);

#line 88
    }
    else
    {

#line 88
        contribution_0 = 0.0;

#line 88
    }

#line 93
    float _S5 = max(_S3 - _S1, contribution_0);
    if(_S3 > 9.99999997475242708e-07)
    {

#line 94
        contribution_0 = _S5 / _S3;

#line 94
    }
    else
    {

#line 94
        contribution_0 = 0.0;

#line 94
    }
    return colour_0 * float3(contribution_0) ;
}

float3 tap_0(texture2d<float, access::sample> texture_0, float2 uv_1, bool thresholded_0, KernelContext_0 thread* kernelContext_2)
{
    float3 _S6 = ((texture_0).sample((kernelContext_2->bloomSet_0->linearClamp_0), (uv_1), level((0.0)))).xyz;

#line 100
    float3 _S7;
    if(thresholded_0)
    {

#line 101
        float3 _S8 = prefilter_0(_S6, kernelContext_2);

#line 101
        _S7 = _S8;

#line 101
    }
    else
    {

#line 101
        _S7 = _S6;

#line 101
    }

#line 101
    return _S7;
}


#line 112
struct BoxWeights_0
{
    float centre_0;
    float topLeft_0;
    float topRight_0;
    float bottomLeft_0;
    float bottomRight_0;
};


#line 112
BoxWeights_0 BoxWeights_x24init_0(float centre_1, float topLeft_1, float topRight_1, float bottomLeft_1, float bottomRight_1)
{

#line 112
    thread BoxWeights_0 _S9;

    (&_S9)->centre_0 = centre_1;
    (&_S9)->topLeft_0 = topLeft_1;
    (&_S9)->topRight_0 = topRight_1;
    (&_S9)->bottomLeft_0 = bottomLeft_1;
    (&_S9)->bottomRight_0 = bottomRight_1;

#line 112
    return _S9;
}


#line 106
float karisWeight_0(float3 group_0, KernelContext_0 thread* kernelContext_3)
{
    return 1.0 / (1.0 + luminance_0(group_0) * kernelContext_3->bloomPush_0->threshold_0.z);
}


#line 124
float3 downsample13_0(texture2d<float, access::sample> texture_1, float2 uv_2, float2 texel_0, bool first_0, KernelContext_0 thread* kernelContext_4)
{

#line 124
    float3 _S10 = tap_0(texture_1, uv_2 + texel_0 * float2(-2.0, -2.0), first_0, kernelContext_4);

#line 124
    float3 _S11 = tap_0(texture_1, uv_2 + texel_0 * float2(0.0, -2.0), first_0, kernelContext_4);

#line 124
    float3 _S12 = tap_0(texture_1, uv_2 + texel_0 * float2(2.0, -2.0), first_0, kernelContext_4);

#line 124
    float3 _S13 = tap_0(texture_1, uv_2 + texel_0 * float2(-1.0, -1.0), first_0, kernelContext_4);

#line 124
    float3 _S14 = tap_0(texture_1, uv_2 + texel_0 * float2(1.0, -1.0), first_0, kernelContext_4);

#line 124
    float3 _S15 = tap_0(texture_1, uv_2 + texel_0 * float2(-2.0, 0.0), first_0, kernelContext_4);

#line 124
    float3 _S16 = tap_0(texture_1, uv_2, first_0, kernelContext_4);

#line 124
    float3 _S17 = tap_0(texture_1, uv_2 + texel_0 * float2(2.0, 0.0), first_0, kernelContext_4);

#line 124
    float3 _S18 = tap_0(texture_1, uv_2 + texel_0 * float2(-1.0, 1.0), first_0, kernelContext_4);

#line 124
    float3 _S19 = tap_0(texture_1, uv_2 + texel_0, first_0, kernelContext_4);

#line 124
    float3 _S20 = tap_0(texture_1, uv_2 + texel_0 * float2(-2.0, 2.0), first_0, kernelContext_4);

#line 124
    float3 _S21 = tap_0(texture_1, uv_2 + texel_0 * float2(0.0, 2.0), first_0, kernelContext_4);

#line 124
    float3 _S22 = tap_0(texture_1, uv_2 + texel_0 * float2(2.0, 2.0), first_0, kernelContext_4);

#line 124
    float3 _S23 = float3(0.25) ;

#line 140
    float3 _S24 = (_S13 + _S14 + _S18 + _S19) * _S23;
    float3 _S25 = (_S10 + _S11 + _S15 + _S16) * _S23;
    float3 _S26 = (_S11 + _S12 + _S16 + _S17) * _S23;
    float3 _S27 = (_S15 + _S16 + _S20 + _S21) * _S23;
    float3 _S28 = (_S16 + _S17 + _S21 + _S22) * _S23;

    thread BoxWeights_0 weights_0 = BoxWeights_x24init_0(0.5, 0.125, 0.125, 0.125, 0.125);

#line 146
    bool _S29;
    if(first_0)
    {

#line 147
        _S29 = (kernelContext_4->bloomPush_0->threshold_0.w) > 0.5;

#line 147
    }
    else
    {

#line 147
        _S29 = false;

#line 147
    }

#line 147
    if(_S29)
    {

#line 147
        float _S30 = karisWeight_0(_S24, kernelContext_4);

        (&weights_0)->centre_0 = (&weights_0)->centre_0 * _S30;

#line 149
        float _S31 = karisWeight_0(_S25, kernelContext_4);
        (&weights_0)->topLeft_0 = (&weights_0)->topLeft_0 * _S31;

#line 150
        float _S32 = karisWeight_0(_S26, kernelContext_4);
        (&weights_0)->topRight_0 = (&weights_0)->topRight_0 * _S32;

#line 151
        float _S33 = karisWeight_0(_S27, kernelContext_4);
        (&weights_0)->bottomLeft_0 = (&weights_0)->bottomLeft_0 * _S33;

#line 152
        float _S34 = karisWeight_0(_S28, kernelContext_4);
        (&weights_0)->bottomRight_0 = (&weights_0)->bottomRight_0 * _S34;

#line 147
    }

#line 159
    return (_S24 * float3((&weights_0)->centre_0)  + _S25 * float3((&weights_0)->topLeft_0)  + _S26 * float3((&weights_0)->topRight_0)  + _S27 * float3((&weights_0)->bottomLeft_0)  + _S28 * float3((&weights_0)->bottomRight_0) ) / float3(max((&weights_0)->centre_0 + (&weights_0)->topLeft_0 + (&weights_0)->topRight_0 + (&weights_0)->bottomLeft_0 + (&weights_0)->bottomRight_0, 9.99999997475242708e-07)) ;
}


#line 159
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 159
struct pixelInput_0
{
    float2 uv_3 [[user(TEXCOORD)]];
};


#line 192
[[fragment]] pixelOutput_0 bloomDownsample(pixelInput_0 _S35 [[stage_in]], float4 position_1 [[position]], BloomPassSet_default_0 constant* bloomSet_1 [[buffer(2)]], BloomPush_0 constant* bloomPush_1 [[buffer(3)]])
{

#line 192
    thread KernelContext_0 kernelContext_5;

#line 192
    (&kernelContext_5)->bloomSet_0 = bloomSet_1;

#line 192
    (&kernelContext_5)->bloomPush_0 = bloomPush_1;

#line 192
    texture2d<float, access::sample> _S36 = bloomSet_1->source_0;

#line 192
    thread BloomFragment_0 _S37;

#line 192
    (&_S37)->position_0 = position_1;

#line 192
    (&_S37)->uv_0 = _S35.uv_3;

#line 192
    float2 _S38 = targetUv_0(&_S37, &kernelContext_5);

#line 192
    float3 _S39 = downsample13_0(_S36, _S38, (&kernelContext_5)->bloomPush_0->extents_0.xy, false, &kernelContext_5);

#line 192
    pixelOutput_0 _S40 = { float4(_S39, 1.0) };


    return _S40;
}

)cy_msl";

/// bloomUpsample.metal, 5171 bytes.
inline constexpr char kBloomUpsampleMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 75 "cy/bloom.slang"
struct BloomFragment_0
{
    float4 position_0;
    float2 uv_0;
};


#line 48
struct BloomPush_0
{
    float4 extents_0;
    float4 detail_0;
    float4 threshold_0;
    float4 blend_0;
};


#line 5319 "core.meta.slang"
struct BloomPassSet_default_0
{
    texture2d<float, access::sample> source_0;
    texture2d<float, access::sample> detail_1;
    sampler linearClamp_0;
    texture2d<float, access::sample> lensDirt_0;
};


#line 5319
struct KernelContext_0
{
    BloomPush_0 constant* bloomPush_0;
    BloomPassSet_default_0 constant* bloomSet_0;
};


#line 179 "cy/bloom.slang"
float2 targetUv_0(const BloomFragment_0 thread* input_0, KernelContext_0 thread* kernelContext_0)
{
    return input_0->position_0.xy * kernelContext_0->bloomPush_0->extents_0.zw;
}


#line 24 "./cy/color.slang"
float luminance_0(float3 linear_0)
{
    return dot(linear_0, float3(0.2125999927520752, 0.71520000696182251, 0.07220000028610229));
}


#line 82 "cy/bloom.slang"
float3 prefilter_0(float3 colour_0, KernelContext_0 thread* kernelContext_1)
{
    float _S1 = kernelContext_1->bloomPush_0->threshold_0.x;
    float _S2 = kernelContext_1->bloomPush_0->threshold_0.y;
    float _S3 = luminance_0(colour_0);

#line 86
    float contribution_0;

    if(_S2 > 9.99999997475242708e-07)
    {
        float _S4 = clamp(_S3 - _S1 + _S2, 0.0, 2.0 * _S2);

#line 90
        contribution_0 = _S4 * _S4 / (4.0 * _S2);

#line 88
    }
    else
    {

#line 88
        contribution_0 = 0.0;

#line 88
    }

#line 93
    float _S5 = max(_S3 - _S1, contribution_0);
    if(_S3 > 9.99999997475242708e-07)
    {

#line 94
        contribution_0 = _S5 / _S3;

#line 94
    }
    else
    {

#line 94
        contribution_0 = 0.0;

#line 94
    }
    return colour_0 * float3(contribution_0) ;
}

float3 tap_0(texture2d<float, access::sample> texture_0, float2 uv_1, bool thresholded_0, KernelContext_0 thread* kernelContext_2)
{
    float3 _S6 = ((texture_0).sample((kernelContext_2->bloomSet_0->linearClamp_0), (uv_1), level((0.0)))).xyz;

#line 100
    float3 _S7;
    if(thresholded_0)
    {

#line 101
        float3 _S8 = prefilter_0(_S6, kernelContext_2);

#line 101
        _S7 = _S8;

#line 101
    }
    else
    {

#line 101
        _S7 = _S6;

#line 101
    }

#line 101
    return _S7;
}


#line 164
float3 tent_0(texture2d<float, access::sample> texture_1, float2 uv_2, float2 texel_0, KernelContext_0 thread* kernelContext_3)
{
    float2 _S9 = texel_0 * float2(kernelContext_3->bloomPush_0->blend_0.z, 1.0);

#line 166
    float3 _S10 = tap_0(texture_1, uv_2, false, kernelContext_3);
    float3 sum_0 = _S10 * float3(4.0) ;
    float _S11 = _S9.x;

#line 168
    float _S12 = - _S11;

#line 168
    float3 _S13 = tap_0(texture_1, uv_2 + float2(_S12, 0.0), false, kernelContext_3);

#line 168
    float3 _S14 = float3(2.0) ;

#line 168
    float3 sum_1 = sum_0 + _S13 * _S14;

#line 168
    float3 _S15 = tap_0(texture_1, uv_2 + float2(_S11, 0.0), false, kernelContext_3);
    float3 sum_2 = sum_1 + _S15 * _S14;
    float _S16 = _S9.y;

#line 170
    float _S17 = - _S16;

#line 170
    float3 _S18 = tap_0(texture_1, uv_2 + float2(0.0, _S17), false, kernelContext_3);

#line 170
    float3 sum_3 = sum_2 + _S18 * _S14;

#line 170
    float3 _S19 = tap_0(texture_1, uv_2 + float2(0.0, _S16), false, kernelContext_3);
    float3 sum_4 = sum_3 + _S19 * _S14;

#line 171
    float3 _S20 = tap_0(texture_1, uv_2 + float2(_S12, _S17), false, kernelContext_3);
    float3 sum_5 = sum_4 + _S20;

#line 172
    float3 _S21 = tap_0(texture_1, uv_2 + float2(_S11, _S17), false, kernelContext_3);
    float3 sum_6 = sum_5 + _S21;

#line 173
    float3 _S22 = tap_0(texture_1, uv_2 + float2(_S12, _S16), false, kernelContext_3);
    float3 sum_7 = sum_6 + _S22;

#line 174
    float3 _S23 = tap_0(texture_1, uv_2 + float2(_S11, _S16), false, kernelContext_3);

    return (sum_7 + _S23) * float3(0.0625) ;
}


#line 176
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 176
struct pixelInput_0
{
    float2 uv_3 [[user(TEXCOORD)]];
};


#line 200
[[fragment]] pixelOutput_0 bloomUpsample(pixelInput_0 _S24 [[stage_in]], float4 position_1 [[position]], BloomPush_0 constant* bloomPush_1 [[buffer(3)]], BloomPassSet_default_0 constant* bloomSet_1 [[buffer(2)]])
{

#line 200
    thread KernelContext_0 kernelContext_4;

#line 200
    (&kernelContext_4)->bloomPush_0 = bloomPush_1;

#line 200
    (&kernelContext_4)->bloomSet_0 = bloomSet_1;

#line 200
    thread BloomFragment_0 _S25;

#line 200
    (&_S25)->position_0 = position_1;

#line 200
    (&_S25)->uv_0 = _S24.uv_3;

#line 200
    float2 _S26 = targetUv_0(&_S25, &kernelContext_4);

#line 200
    float3 _S27 = tap_0((&kernelContext_4)->bloomSet_0->source_0, _S26, false, &kernelContext_4);

#line 200
    float3 _S28 = tent_0((&kernelContext_4)->bloomSet_0->detail_1, _S26, (&kernelContext_4)->bloomPush_0->detail_0.xy, &kernelContext_4);

#line 200
    pixelOutput_0 _S29 = { float4(mix(_S27, _S28, float3((&kernelContext_4)->bloomPush_0->blend_0.y) ), 1.0) };

#line 205
    return _S29;
}

)cy_msl";

/// bloomComposite.metal, 5776 bytes.
inline constexpr char kBloomCompositeMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 75 "cy/bloom.slang"
struct BloomFragment_0
{
    float4 position_0;
    float2 uv_0;
};


#line 75
struct BloomPassSet_default_0
{
    texture2d<float, access::sample> source_0;
    texture2d<float, access::sample> detail_0;
    sampler linearClamp_0;
    texture2d<float, access::sample> lensDirt_0;
};


#line 48
struct BloomPush_0
{
    float4 extents_0;
    float4 detail_1;
    float4 threshold_0;
    float4 blend_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    BloomPassSet_default_0 constant* bloomSet_0;
    BloomPush_0 constant* bloomPush_0;
};


#line 179 "cy/bloom.slang"
float2 targetUv_0(const BloomFragment_0 thread* input_0, KernelContext_0 thread* kernelContext_0)
{
    return input_0->position_0.xy * kernelContext_0->bloomPush_0->extents_0.zw;
}


#line 24 "./cy/color.slang"
float luminance_0(float3 linear_0)
{
    return dot(linear_0, float3(0.2125999927520752, 0.71520000696182251, 0.07220000028610229));
}


#line 82 "cy/bloom.slang"
float3 prefilter_0(float3 colour_0, KernelContext_0 thread* kernelContext_1)
{
    float _S1 = kernelContext_1->bloomPush_0->threshold_0.x;
    float _S2 = kernelContext_1->bloomPush_0->threshold_0.y;
    float _S3 = luminance_0(colour_0);

#line 86
    float contribution_0;

    if(_S2 > 9.99999997475242708e-07)
    {
        float _S4 = clamp(_S3 - _S1 + _S2, 0.0, 2.0 * _S2);

#line 90
        contribution_0 = _S4 * _S4 / (4.0 * _S2);

#line 88
    }
    else
    {

#line 88
        contribution_0 = 0.0;

#line 88
    }

#line 93
    float _S5 = max(_S3 - _S1, contribution_0);
    if(_S3 > 9.99999997475242708e-07)
    {

#line 94
        contribution_0 = _S5 / _S3;

#line 94
    }
    else
    {

#line 94
        contribution_0 = 0.0;

#line 94
    }
    return colour_0 * float3(contribution_0) ;
}

float3 tap_0(texture2d<float, access::sample> texture_0, float2 uv_1, bool thresholded_0, KernelContext_0 thread* kernelContext_2)
{
    float3 _S6 = ((texture_0).sample((kernelContext_2->bloomSet_0->linearClamp_0), (uv_1), level((0.0)))).xyz;

#line 100
    float3 _S7;
    if(thresholded_0)
    {

#line 101
        float3 _S8 = prefilter_0(_S6, kernelContext_2);

#line 101
        _S7 = _S8;

#line 101
    }
    else
    {

#line 101
        _S7 = _S6;

#line 101
    }

#line 101
    return _S7;
}


#line 164
float3 tent_0(texture2d<float, access::sample> texture_1, float2 uv_2, float2 texel_0, KernelContext_0 thread* kernelContext_3)
{
    float2 _S9 = texel_0 * float2(kernelContext_3->bloomPush_0->blend_0.z, 1.0);

#line 166
    float3 _S10 = tap_0(texture_1, uv_2, false, kernelContext_3);
    float3 sum_0 = _S10 * float3(4.0) ;
    float _S11 = _S9.x;

#line 168
    float _S12 = - _S11;

#line 168
    float3 _S13 = tap_0(texture_1, uv_2 + float2(_S12, 0.0), false, kernelContext_3);

#line 168
    float3 _S14 = float3(2.0) ;

#line 168
    float3 sum_1 = sum_0 + _S13 * _S14;

#line 168
    float3 _S15 = tap_0(texture_1, uv_2 + float2(_S11, 0.0), false, kernelContext_3);
    float3 sum_2 = sum_1 + _S15 * _S14;
    float _S16 = _S9.y;

#line 170
    float _S17 = - _S16;

#line 170
    float3 _S18 = tap_0(texture_1, uv_2 + float2(0.0, _S17), false, kernelContext_3);

#line 170
    float3 sum_3 = sum_2 + _S18 * _S14;

#line 170
    float3 _S19 = tap_0(texture_1, uv_2 + float2(0.0, _S16), false, kernelContext_3);
    float3 sum_4 = sum_3 + _S19 * _S14;

#line 171
    float3 _S20 = tap_0(texture_1, uv_2 + float2(_S12, _S17), false, kernelContext_3);
    float3 sum_5 = sum_4 + _S20;

#line 172
    float3 _S21 = tap_0(texture_1, uv_2 + float2(_S11, _S17), false, kernelContext_3);
    float3 sum_6 = sum_5 + _S21;

#line 173
    float3 _S22 = tap_0(texture_1, uv_2 + float2(_S12, _S16), false, kernelContext_3);
    float3 sum_7 = sum_6 + _S22;

#line 174
    float3 _S23 = tap_0(texture_1, uv_2 + float2(_S11, _S16), false, kernelContext_3);

    return (sum_7 + _S23) * float3(0.0625) ;
}


#line 176
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 176
struct pixelInput_0
{
    float2 uv_3 [[user(TEXCOORD)]];
};


#line 210
[[fragment]] pixelOutput_0 bloomComposite(pixelInput_0 _S24 [[stage_in]], float4 position_1 [[position]], BloomPassSet_default_0 constant* bloomSet_1 [[buffer(2)]], BloomPush_0 constant* bloomPush_1 [[buffer(3)]])
{

#line 210
    thread KernelContext_0 kernelContext_4;

#line 210
    (&kernelContext_4)->bloomSet_0 = bloomSet_1;

#line 210
    (&kernelContext_4)->bloomPush_0 = bloomPush_1;

    int3 _S25 = int3(int2(position_1.xy), int(0));

#line 212
    float4 _S26 = ((bloomSet_1->source_0).read(vec<uint,2>(((_S25)).xy), uint(((_S25)).z)));

#line 212
    thread BloomFragment_0 _S27;

#line 212
    (&_S27)->position_0 = position_1;

#line 212
    (&_S27)->uv_0 = _S24.uv_3;

#line 212
    float2 _S28 = targetUv_0(&_S27, &kernelContext_4);

#line 212
    float3 _S29 = tent_0((&kernelContext_4)->bloomSet_0->detail_0, _S28, (&kernelContext_4)->bloomPush_0->detail_1.xy, &kernelContext_4);

#line 212
    float3 bloom_0;


    if(((&kernelContext_4)->bloomPush_0->blend_0.w) > 0.0)
    {

#line 215
        bloom_0 = _S29 * (float3(1.0)  + (((&kernelContext_4)->bloomSet_0->lensDirt_0).sample(((&kernelContext_4)->bloomSet_0->linearClamp_0), (_S28), level((0.0)))).xyz * float3((&kernelContext_4)->bloomPush_0->blend_0.w) );

#line 215
    }
    else
    {

#line 215
        bloom_0 = _S29;

#line 215
    }

#line 220
    float _S30 = (&kernelContext_4)->bloomPush_0->blend_0.x;

#line 220
    float3 _S31 = _S26.xyz;

#line 220
    float3 _S32 = prefilter_0(_S31, &kernelContext_4);

#line 220
    pixelOutput_0 _S33 = { float4(_S31 + float3(_S30)  * (bloom_0 - _S32), _S26.w) };
    return _S33;
}

)cy_msl";

}  // namespace cy::rendering::pipeline

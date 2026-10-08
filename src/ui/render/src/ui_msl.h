// SPDX-License-Identifier: MIT
#pragma once
// Compiled MSL for CyberUI's primitive shader. GENERATED — do not edit by hand.
//
// Produced by src/ui/render/shaders/regenerate.py from ui.slang. Checked in rather than compiled by
// the build because the interface must draw in a build with no shader compiler at all.

#include <cy/core/base/types.h>

namespace cy::ui::render {

/// cyUiVertex.metal, 1904 bytes.
inline constexpr char kUiVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 90 "core"
struct cyUiVertex_Result_0
{
    float4 position_0 [[position]];
    uint primitive_0 [[user(CY_UI_PRIMITIVE)]];
};


#line 67 "src/ui/render/shaders/ui.slang"
struct CyUiPush_0
{
    float2 inverseExtent_0;
    uint first_0;
    uint flags_0;
};


#line 53
struct CyUiPrimitive_0
{
    float4 bounds_0;
    float4 uv_0;
    float4 shape_0;
    uint4 words_0;
};


#line 76
struct CyUiSet_default_0
{
    CyUiPrimitive_0 device* primitives_0;
    texture2d<float, access::sample> atlas_0;
    sampler pointClamp_0;
    sampler linearClamp_0;
};


#line 76
struct KernelContext_0
{
    CyUiPush_0 constant* cyUiPush_0;
    CyUiSet_default_0 constant* cyUi_0;
};


#line 93
struct CyUiVertex_0
{
    float4 position_1;
    [[flat]] uint primitive_1;
};


#line 93
[[vertex]] cyUiVertex_Result_0 cyUiVertex(uint vertexId_0 [[vertex_id]], uint instanceId_0 [[instance_id]], CyUiPush_0 constant* cyUiPush_1 [[buffer(1)]], CyUiSet_default_0 constant* cyUi_1 [[buffer(0)]])
{

#line 93
    thread KernelContext_0 kernelContext_0;

#line 93
    (&kernelContext_0)->cyUiPush_0 = cyUiPush_1;

#line 93
    (&kernelContext_0)->cyUi_0 = cyUi_1;

#line 103
    uint _S1 = cyUiPush_1->first_0 + instanceId_0;
    CyUiPrimitive_0 _S2 = cyUi_1->primitives_0[_S1];

    float2 _S3 = _S2.bounds_0.xy - float2(1.0)  + float2(float(vertexId_0 & 1U), float(vertexId_0 >> 1U)) * (_S2.bounds_0.zw + float2(2.0) );
    thread CyUiVertex_0 output_0;
    (&output_0)->position_1 = float4(_S3.x * cyUiPush_1->inverseExtent_0.x - 1.0, 1.0 - _S3.y * cyUiPush_1->inverseExtent_0.y, 0.0, 1.0);

    (&output_0)->primitive_1 = _S1;

#line 110
    thread cyUiVertex_Result_0 _S4;

#line 110
    (&_S4)->position_0 = output_0.position_1;

#line 110
    (&_S4)->primitive_0 = output_0.primitive_1;

#line 110
    return _S4;
}

)cy_msl";

/// cyUiFragment.metal, 5603 bytes.
inline constexpr char kUiFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 134 "src/ui/render/shaders/ui.slang"
float median_0(float a_0, float b_0, float c_0)
{
    return max(min(a_0, b_0), min(max(a_0, b_0), c_0));
}


#line 116
float roundedBoxDistance_0(float2 p_0, float2 halfSize_0, float radius_0)
{
    float _S1 = min(radius_0, min(halfSize_0.x, halfSize_0.y));
    float2 _S2 = abs(p_0) - halfSize_0 + float2(_S1) ;
    return length(max(_S2, float2(0.0, 0.0))) + min(max(_S2.x, _S2.y), 0.0) - _S1;
}


float coverage_0(float2 p_1, float2 halfSize_1, float radius_1)
{

#line 124
    bool _S3;

    if((halfSize_1.x) <= 0.0)
    {

#line 126
        _S3 = true;

#line 126
    }
    else
    {

#line 126
        _S3 = (halfSize_1.y) <= 0.0;

#line 126
    }

#line 126
    if(_S3)
    {
        return 0.0;
    }
    return saturate(0.5 - roundedBoxDistance_0(p_1, halfSize_1, radius_1));
}


#line 140
float4 unpackColour_0(uint colour_0)
{
    return float4(float((colour_0 >> 16U) & 255U), float((colour_0 >> 8U) & 255U), float(colour_0 & 255U), float(colour_0 >> 24U)) / float4(255.0) ;
}


float decodeSrgb_0(float value_0)
{

#line 146
    float _S4;

    if(value_0 <= 0.04044999927282333)
    {

#line 148
        _S4 = value_0 / 12.92000007629394531;

#line 148
    }
    else
    {

#line 148
        _S4 = pow((value_0 + 0.05499999970197678) / 1.0549999475479126, 2.40000009536743164);

#line 148
    }

#line 148
    return _S4;
}


float4 toLinear_0(float4 colour_1)
{
    float _S5 = colour_1.w;

#line 154
    if(_S5 <= 0.0)
    {
        return colour_1;
    }

#line 156
    float3 _S6 = float3(_S5) ;

    float3 _S7 = colour_1.xyz / _S6;
    return float4(float3(decodeSrgb_0(_S7.x), decodeSrgb_0(_S7.y), decodeSrgb_0(_S7.z)) * _S6, _S5);
}


#line 90 "core"
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 90
struct pixelInput_0
{
    [[flat]] uint primitive_0 [[user(CY_UI_PRIMITIVE)]];
};


#line 53 "src/ui/render/shaders/ui.slang"
struct CyUiPrimitive_0
{
    float4 bounds_0;
    float4 uv_0;
    float4 shape_0;
    uint4 words_0;
};


#line 167
struct CyUiSet_default_0
{
    CyUiPrimitive_0 device* primitives_0;
    texture2d<float, access::sample> atlas_0;
    sampler pointClamp_0;
    sampler linearClamp_0;
};


#line 67
struct CyUiPush_0
{
    float2 inverseExtent_0;
    uint first_0;
    uint flags_0;
};


#line 5319 "core.meta.slang"
struct KernelContext_0
{
    CyUiSet_default_0 constant* cyUi_0;
    CyUiPush_0 constant* cyUiPush_0;
};


#line 165 "src/ui/render/shaders/ui.slang"
[[fragment]] pixelOutput_0 cyUiFragment(pixelInput_0 _S8 [[stage_in]], float4 position_0 [[position]], CyUiSet_default_0 constant* cyUi_1 [[buffer(0)]], CyUiPush_0 constant* cyUiPush_1 [[buffer(1)]])
{

#line 165
    thread KernelContext_0 kernelContext_0;

#line 165
    (&kernelContext_0)->cyUi_0 = cyUi_1;

#line 165
    (&kernelContext_0)->cyUiPush_0 = cyUiPush_1;

    CyUiPrimitive_0 _S9 = cyUi_1->primitives_0[_S8.primitive_0];
    float2 _S10 = position_0.xy;
    float2 _S11 = _S9.bounds_0.zw;

#line 169
    float2 _S12 = _S11 * float2(0.5) ;
    float2 _S13 = _S9.bounds_0.xy;

#line 170
    float2 _S14 = _S10 - (_S13 + _S12);
    float _S15 = _S9.shape_0.x;
    float _S16 = _S9.shape_0.y;

    uint _S17 = _S9.words_0.z;

    float2 _S18 = _S9.uv_0.xy + saturate((_S10 - _S13) / _S11) * _S9.uv_0.zw;

#line 176
    float _S19;

#line 176
    float4 result_0;


    if(_S17 == 3U)
    {



        float4 _S20 = (((&kernelContext_0)->cyUi_0->atlas_0).sample(((&kernelContext_0)->cyUi_0->linearClamp_0), (_S18), level((0.0))));
        float _S21 = (median_0(_S20.x, _S20.y, _S20.z) - 0.5) * _S9.shape_0.z;
        float _S22 = coverage_0(_S14, _S12, 0.0);
        float _S23 = saturate(_S21 + 0.5);

#line 187
        float _S24 = _S23 * _S22;
        if(_S16 > 0.0)
        {

#line 188
            _S19 = saturate(_S21 + _S16 + 0.5);

#line 188
        }
        else
        {

#line 188
            _S19 = _S23;

#line 188
        }

#line 188
        result_0 = unpackColour_0(_S9.words_0.x) * float4(_S24)  + unpackColour_0(_S9.words_0.y) * float4((_S19 * _S22 - _S24)) ;

#line 179
    }
    else
    {

#line 195
        float _S25 = coverage_0(_S14, _S12, _S15);
        if(_S16 > 0.0)
        {

#line 196
            _S19 = coverage_0(_S14, _S12 - float2(_S16) , max(_S15 - _S16, 0.0));

#line 196
        }
        else
        {

#line 196
            _S19 = _S25;

#line 196
        }

        float4 fill_0 = unpackColour_0(_S9.words_0.x);
        if(_S17 == 2U)
        {

#line 199
            result_0 = fill_0 * float4((((&kernelContext_0)->cyUi_0->atlas_0).sample(((&kernelContext_0)->cyUi_0->pointClamp_0), (_S18), level((0.0)))).x) ;

#line 199
        }
        else
        {

            if(_S17 == 1U)
            {

#line 203
                result_0 = fill_0 * (((&kernelContext_0)->cyUi_0->atlas_0).sample(((&kernelContext_0)->cyUi_0->linearClamp_0), (_S18), level((0.0))));

#line 203
            }
            else
            {

#line 203
                result_0 = fill_0;

#line 203
            }

#line 199
        }

#line 199
        result_0 = result_0 * float4(_S19)  + unpackColour_0(_S9.words_0.y) * float4((_S25 - _S19)) ;

#line 179
    }

#line 209
    if(all(result_0 == float4(0.0, 0.0, 0.0, 0.0)))
    {
        discard_fragment();

#line 209
    }



    if((((&kernelContext_0)->cyUiPush_0->flags_0) & 1U) != 0U)
    {

#line 213
        result_0 = toLinear_0(result_0);

#line 213
    }

#line 213
    pixelOutput_0 _S26 = { result_0 };



    return _S26;
}

)cy_msl";

}  // namespace cy::ui::render

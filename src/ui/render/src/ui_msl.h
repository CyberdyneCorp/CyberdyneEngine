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


#line 68 "src/ui/render/shaders/ui.slang"
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


#line 77
struct CyUiSet_default_0
{
    CyUiPrimitive_0 device* primitives_0;
    texture2d<float, access::sample> atlas_0;
    sampler pointClamp_0;
    sampler linearClamp_0;
};


#line 77
struct KernelContext_0
{
    CyUiPush_0 constant* cyUiPush_0;
    CyUiSet_default_0 constant* cyUi_0;
};


#line 94
struct CyUiVertex_0
{
    float4 position_1;
    [[flat]] uint primitive_1;
};


#line 94
[[vertex]] cyUiVertex_Result_0 cyUiVertex(uint vertexId_0 [[vertex_id]], uint instanceId_0 [[instance_id]], CyUiPush_0 constant* cyUiPush_1 [[buffer(1)]], CyUiSet_default_0 constant* cyUi_1 [[buffer(0)]])
{

#line 94
    thread KernelContext_0 kernelContext_0;

#line 94
    (&kernelContext_0)->cyUiPush_0 = cyUiPush_1;

#line 94
    (&kernelContext_0)->cyUi_0 = cyUi_1;

#line 104
    uint _S1 = cyUiPush_1->first_0 + instanceId_0;
    CyUiPrimitive_0 _S2 = cyUi_1->primitives_0[_S1];

    float2 _S3 = _S2.bounds_0.xy - float2(1.0)  + float2(float(vertexId_0 & 1U), float(vertexId_0 >> 1U)) * (_S2.bounds_0.zw + float2(2.0) );
    thread CyUiVertex_0 output_0;
    (&output_0)->position_1 = float4(_S3.x * cyUiPush_1->inverseExtent_0.x - 1.0, 1.0 - _S3.y * cyUiPush_1->inverseExtent_0.y, 0.0, 1.0);

    (&output_0)->primitive_1 = _S1;

#line 111
    thread cyUiVertex_Result_0 _S4;

#line 111
    (&_S4)->position_0 = output_0.position_1;

#line 111
    (&_S4)->primitive_0 = output_0.primitive_1;

#line 111
    return _S4;
}

)cy_msl";

/// cyUiFragment.metal, 5796 bytes.
inline constexpr char kUiFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 135 "src/ui/render/shaders/ui.slang"
float median_0(float a_0, float b_0, float c_0)
{
    return max(min(a_0, b_0), min(max(a_0, b_0), c_0));
}


#line 117
float roundedBoxDistance_0(float2 p_0, float2 halfSize_0, float radius_0)
{
    float _S1 = min(radius_0, min(halfSize_0.x, halfSize_0.y));
    float2 _S2 = abs(p_0) - halfSize_0 + float2(_S1) ;
    return length(max(_S2, float2(0.0, 0.0))) + min(max(_S2.x, _S2.y), 0.0) - _S1;
}


float coverage_0(float2 p_1, float2 halfSize_1, float radius_1)
{

#line 125
    bool _S3;

    if((halfSize_1.x) <= 0.0)
    {

#line 127
        _S3 = true;

#line 127
    }
    else
    {

#line 127
        _S3 = (halfSize_1.y) <= 0.0;

#line 127
    }

#line 127
    if(_S3)
    {
        return 0.0;
    }
    return saturate(0.5 - roundedBoxDistance_0(p_1, halfSize_1, radius_1));
}


#line 141
float4 unpackColour_0(uint colour_0)
{
    return float4(float((colour_0 >> 16U) & 255U), float((colour_0 >> 8U) & 255U), float(colour_0 & 255U), float(colour_0 >> 24U)) / float4(255.0) ;
}


float decodeSrgb_0(float value_0)
{

#line 147
    float _S4;

    if(value_0 <= 0.04044999927282333)
    {

#line 149
        _S4 = value_0 / 12.92000007629394531;

#line 149
    }
    else
    {

#line 149
        _S4 = pow((value_0 + 0.05499999970197678) / 1.0549999475479126, 2.40000009536743164);

#line 149
    }

#line 149
    return _S4;
}


float4 toLinear_0(float4 colour_1)
{
    float _S5 = colour_1.w;

#line 155
    if(_S5 <= 0.0)
    {
        return colour_1;
    }

#line 157
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


#line 168
struct CyUiSet_default_0
{
    CyUiPrimitive_0 device* primitives_0;
    texture2d<float, access::sample> atlas_0;
    sampler pointClamp_0;
    sampler linearClamp_0;
};


#line 68
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


#line 166 "src/ui/render/shaders/ui.slang"
[[fragment]] pixelOutput_0 cyUiFragment(pixelInput_0 _S8 [[stage_in]], float4 position_0 [[position]], CyUiSet_default_0 constant* cyUi_1 [[buffer(0)]], CyUiPush_0 constant* cyUiPush_1 [[buffer(1)]])
{

#line 166
    thread KernelContext_0 kernelContext_0;

#line 166
    (&kernelContext_0)->cyUi_0 = cyUi_1;

#line 166
    (&kernelContext_0)->cyUiPush_0 = cyUiPush_1;

    CyUiPrimitive_0 _S9 = cyUi_1->primitives_0[_S8.primitive_0];
    float2 _S10 = position_0.xy;
    float2 _S11 = _S9.bounds_0.zw;

#line 170
    float2 _S12 = _S11 * float2(0.5) ;
    float2 _S13 = _S9.bounds_0.xy;

#line 171
    float2 _S14 = _S10 - (_S13 + _S12);
    float _S15 = _S9.shape_0.x;
    float _S16 = _S9.shape_0.y;

    uint _S17 = _S9.words_0.z;

    float2 _S18 = _S9.uv_0.xy + saturate((_S10 - _S13) / _S11) * _S9.uv_0.zw;

    float4 _S19 = float4(0.0, 0.0, 0.0, 0.0);

#line 179
    thread float4 result_0 = _S19;

#line 179
    float _S20;
    if(_S17 == 3U)
    {



        float4 _S21 = (((&kernelContext_0)->cyUi_0->atlas_0).sample(((&kernelContext_0)->cyUi_0->linearClamp_0), (_S18), level((0.0))));
        float _S22 = (median_0(_S21.x, _S21.y, _S21.z) - 0.5) * _S9.shape_0.z;
        float _S23 = coverage_0(_S14, _S12, 0.0);
        float _S24 = saturate(_S22 + 0.5);

#line 188
        float _S25 = _S24 * _S23;
        if(_S16 > 0.0)
        {

#line 189
            _S20 = saturate(_S22 + _S16 + 0.5);

#line 189
        }
        else
        {

#line 189
            _S20 = _S24;

#line 189
        }

        result_0 = unpackColour_0(_S9.words_0.x) * float4(_S25)  + unpackColour_0(_S9.words_0.y) * float4((_S20 * _S23 - _S25)) ;

#line 180
    }
    else
    {

#line 196
        float _S26 = coverage_0(_S14, _S12, _S15);
        if(_S16 > 0.0)
        {

#line 197
            _S20 = coverage_0(_S14, _S12 - float2(_S16) , max(_S15 - _S16, 0.0));

#line 197
        }
        else
        {

#line 197
            _S20 = _S26;

#line 197
        }

        float4 fill_0 = unpackColour_0(_S9.words_0.x);

#line 199
        float4 fill_1;
        if(_S17 == 2U)
        {

#line 200
            fill_1 = fill_0 * float4((((&kernelContext_0)->cyUi_0->atlas_0).sample(((&kernelContext_0)->cyUi_0->pointClamp_0), (_S18), level((0.0)))).x) ;

#line 200
        }
        else
        {

            if(_S17 == 1U)
            {

#line 204
                fill_1 = fill_0 * (((&kernelContext_0)->cyUi_0->atlas_0).sample(((&kernelContext_0)->cyUi_0->linearClamp_0), (_S18), level((0.0))));

#line 204
            }
            else
            {

#line 204
                fill_1 = fill_0;

#line 204
            }

#line 200
        }

#line 208
        result_0 = fill_1 * float4(_S20)  + unpackColour_0(_S9.words_0.y) * float4((_S26 - _S20)) ;

#line 180
    }

#line 210
    if(all(result_0 == _S19))
    {
        discard_fragment();

#line 210
    }



    if((((&kernelContext_0)->cyUiPush_0->flags_0) & 1U) != 0U)
    {
        result_0 = toLinear_0(result_0);

#line 214
    }



    float _S27 = _S9.shape_0.w;

#line 218
    if(_S27 > 0.0)
    {


        result_0.xyz = result_0.xyz * float3(_S27) ;

#line 218
    }

#line 218
    pixelOutput_0 _S28 = { result_0 };

#line 224
    return _S28;
}

)cy_msl";

}  // namespace cy::ui::render

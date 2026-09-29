// SPDX-License-Identifier: MIT
#pragma once
// Compiled MSL for the selection mask and outline composite. GENERATED — do not edit by hand.
//
// Produced by src/rendering/selection/shaders/regenerate.py from selection_mask.slang and
// selection_outline.slang. Checked in rather than compiled by the build because the pass must exist
// in a build with no shader compiler at all.

#include <cy/core/base/types.h>

namespace cy::rendering::selection {

/// cyOutlineMaskVertex.metal, 4829 bytes.
inline constexpr char kOutlineMaskVertexMsl[] = R"cy_msl(#include <metal_stdlib>
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
    uint3 dimensions_0;
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
    float3 positionRelativeToCamera_0;
    float range_0;
    float3 direction_0;
    float intensity_0;
    float3 color_0;
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


#line 362
struct cyOutlineMaskVertex_Result_0
{
    float4 position_0 [[position]];
};


#line 362
struct vertexInput_0
{
    float3 modelPosition_1 [[attribute(0)]];
};


#line 37 "src/rendering/selection/shaders/selection_mask.slang"
struct CyOutlineMaskVertex_0
{
    float4 position_1;
};


#line 37
[[vertex]] cyOutlineMaskVertex_Result_0 cyOutlineMaskVertex(vertexInput_0 _S3 [[stage_in]], CyFrameViewSet_default_0 constant* cyFrameView_1 [[buffer(0)]], CyDrawPush_0 constant* cyDraw_1 [[buffer(2)]])
{

#line 37
    thread KernelContext_0 kernelContext_1;

#line 37
    (&kernelContext_1)->cyFrameView_0 = cyFrameView_1;

#line 37
    (&kernelContext_1)->cyDraw_0 = cyDraw_1;

#line 47
    thread CyOutlineMaskVertex_0 output_0;

#line 47
    thread CyInstanceTransform_0 _S4 = cyFrameView_1->instances_0[cyFrameView_1->drawInstances_0[(cyDraw_1->drawIndex_0) & 16777215U].instanceSlot_0];

#line 47
    float3 _S5 = transformToRelative_0(&_S4, _S3.modelPosition_1);

#line 47
    float4 _S6 = transformToClip_0(_S5, &kernelContext_1);
    (&output_0)->position_1 = _S6;

#line 48
    thread cyOutlineMaskVertex_Result_0 _S7;

#line 48
    (&_S7)->position_0 = output_0.position_1;

#line 48
    return _S7;
}

)cy_msl";

/// cyOutlineMaskFragment.metal, 553 bytes.
inline constexpr char kOutlineMaskFragmentMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 90 "core"
struct pixelOutput_0
{
    uint output_0 [[color(0)]];
};


#line 349 "src/rendering/shaders/cy/frame.slang"
struct CyDrawPush_0
{
    uint drawIndex_0;
};


#line 56 "src/rendering/selection/shaders/selection_mask.slang"
[[fragment]] pixelOutput_0 cyOutlineMaskFragment(float4 position_0 [[position]], CyDrawPush_0 constant* cyDraw_0 [[buffer(2)]])
{

#line 56
    pixelOutput_0 _S1 = { (cyDraw_0->drawIndex_0) >> 24U };

    return _S1;
}

)cy_msl";

/// cyOutlineVertex.metal, 798 bytes.
inline constexpr char kOutlineVertexMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 80 "src/rendering/selection/shaders/selection_outline.slang"
struct cyOutlineVertex_Result_0
{
    float4 position_0 [[position]];
};


#line 80
struct CyOutlineVertex_0
{
    float4 position_1;
};


#line 453 "core"
[[vertex]] cyOutlineVertex_Result_0 cyOutlineVertex(uint vertexId_0 [[vertex_id]])
{

#line 91 "src/rendering/selection/shaders/selection_outline.slang"
    thread CyOutlineVertex_0 output_0;
    (&output_0)->position_1 = float4(float2(float((vertexId_0 << 1U) & 2U), float(vertexId_0 & 2U)) * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);

#line 92
    thread cyOutlineVertex_Result_0 _S1;

#line 92
    (&_S1)->position_0 = output_0.position_1;

#line 92
    return _S1;
}

)cy_msl";

/// cyOutlineComposite.metal, 8027 bytes.
inline constexpr char kOutlineCompositeMsl[] = R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 55 "src/rendering/selection/shaders/selection_outline.slang"
struct CyOutlineConstants_0
{
    uint2 extent_0;
    uint radius_0;
    uint dash_0;
    float occludedAlpha_0;
    float occludedFill_0;
    float depthTolerance_0;
    uint flags_0;
};


#line 42
struct CyOutlineStyle_0
{
    uint colour_0;
    uint kind_0;
    float width_0;
    float strength_0;
};


#line 66
struct CyOutlineSet_default_0
{
    texture2d<uint, access::sample> mask_0;
    texture2d<float, access::sample> maskDepth_0;
    texture2d<float, access::sample> sceneDepth_0;
    CyOutlineStyle_0 device* styles_0;
};


#line 66
struct KernelContext_0
{
    CyOutlineConstants_0 constant* cyOutlineConstants_0;
    CyOutlineSet_default_0 constant* cyOutline_0;
};


#line 102
bool hidden_0(int2 pixel_0, KernelContext_0 thread* kernelContext_0)
{
    int3 _S1 = int3(pixel_0, int(0));

#line 104
    float _S2 = ((kernelContext_0->cyOutline_0->maskDepth_0).read(vec<uint,2>(((_S1)).xy), uint(((_S1)).z)).x);

    return ((kernelContext_0->cyOutline_0->sceneDepth_0).read(vec<uint,2>(((_S1)).xy), uint(((_S1)).z)).x) > (_S2 + _S2 * kernelContext_0->cyOutlineConstants_0->depthTolerance_0);
}


#line 96
float4 unpackColour_0(uint colour_1)
{
    return float4(float(colour_1 & 255U), float((colour_1 >> 8U) & 255U), float((colour_1 >> 16U) & 255U), float(colour_1 >> 24U)) / float4(255.0) ;
}


#line 109
float4 premultiplied_0(uint colour_2, float alpha_0)
{
    return float4(unpackColour_0(colour_2).xyz * float3(alpha_0) , alpha_0);
}


#line 111
struct pixelOutput_0
{
    float4 output_0 [[color(0)]];
};


#line 115
[[fragment]] pixelOutput_0 cyOutlineComposite(float4 position_0 [[position]], CyOutlineConstants_0 constant* cyOutlineConstants_1 [[buffer(1)]], CyOutlineSet_default_0 constant* cyOutline_1 [[buffer(0)]])
{

#line 115
    thread KernelContext_0 kernelContext_1;

#line 115
    (&kernelContext_1)->cyOutlineConstants_0 = cyOutlineConstants_1;

#line 115
    (&kernelContext_1)->cyOutline_0 = cyOutline_1;

    int2 _S3 = int2(position_0.xy);
    bool _S4 = ((cyOutlineConstants_1->flags_0) & 1U) != 0U;
    int3 _S5 = int3(_S3, int(0));

#line 119
    uint _S6 = ((cyOutline_1->mask_0).read(vec<uint,2>(((_S5)).xy), uint(((_S5)).z)).x);

#line 119
    bool _S7;
    if(_S6 != 0U)
    {
        if(!_S4)
        {

#line 122
            _S7 = true;

#line 122
        }
        else
        {

#line 122
            _S7 = ((&kernelContext_1)->cyOutlineConstants_0->occludedFill_0) <= 0.0;

#line 122
        }

#line 122
        if(_S7)
        {

#line 122
            _S7 = true;

#line 122
        }
        else
        {

#line 122
            bool _S8 = hidden_0(_S3, &kernelContext_1);

#line 122
            _S7 = !_S8;

#line 122
        }

#line 122
        if(_S7)
        {
            discard_fragment();

#line 122
        }



        CyOutlineStyle_0 _S9 = (&kernelContext_1)->cyOutline_0->styles_0[_S6 - 1U];
        float _S10 = (&kernelContext_1)->cyOutlineConstants_0->occludedFill_0 * unpackColour_0(_S9.colour_0).w;
        if(_S10 <= 0.0)
        {
            discard_fragment();

#line 128
        }

#line 128
        pixelOutput_0 _S11 = { premultiplied_0(_S9.colour_0, _S10) };



        return _S11;
    }

    int _S12 = int((&kernelContext_1)->cyOutlineConstants_0->radius_0);
    int2 _S13 = int2((&kernelContext_1)->cyOutlineConstants_0->extent_0);



    int _S14 = - _S12;

#line 140
    int bestDistance_0 = _S12 * _S12 + int(1);

#line 140
    uint bestSlot_0 = 0U;

#line 140
    int2 bestPixel_0 = _S3;

#line 140
    int dy_0 = _S14;

#line 140
    for(;;)
    {

#line 140
        if(dy_0 <= _S12)
        {
        }
        else
        {

#line 140
            break;
        }

#line 140
        int bestDistance_1 = bestDistance_0;

#line 140
        uint bestSlot_1 = bestSlot_0;

#line 140
        int2 bestPixel_1 = bestPixel_0;

#line 140
        int dx_0 = _S14;

        for(;;)
        {

#line 142
            if(dx_0 <= _S12)
            {
            }
            else
            {

#line 142
                break;
            }
            int _S15 = dx_0 * dx_0 + dy_0 * dy_0;
            int2 _S16 = _S3 + int2(dx_0, dy_0);
            if(_S15 >= bestDistance_1)
            {

#line 146
                _S7 = true;

#line 146
            }
            else
            {

#line 146
                _S7 = (_S16.x) < int(0);

#line 146
            }

#line 146
            bool _S17;

#line 146
            if(_S7)
            {

#line 146
                _S17 = true;

#line 146
            }
            else
            {

#line 146
                _S17 = (_S16.y) < int(0);

#line 146
            }

#line 146
            bool _S18;

#line 146
            if(_S17)
            {

#line 146
                _S18 = true;

#line 146
            }
            else
            {

#line 146
                _S18 = (_S16.x) >= (_S13.x);

#line 146
            }

#line 146
            bool _S19;

#line 146
            if(_S18)
            {

#line 146
                _S19 = true;

#line 146
            }
            else
            {

#line 146
                _S19 = (_S16.y) >= (_S13.y);

#line 146
            }

#line 146
            if(_S19)
            {

                dx_0 = dx_0 + int(1);

#line 142
                continue;
            }

#line 151
            int3 _S20 = int3(_S16, int(0));

#line 151
            uint _S21 = ((cyOutline_1->mask_0).read(vec<uint,2>(((_S20)).xy), uint(((_S20)).z)).x);
            if(_S21 == 0U)
            {
                dx_0 = dx_0 + int(1);

#line 142
                continue;
            }

#line 156
            CyOutlineStyle_0 _S22 = (&kernelContext_1)->cyOutline_0->styles_0[_S21 - 1U];
            if(float(_S15) > (_S22.width_0 * _S22.width_0))
            {
                dx_0 = dx_0 + int(1);

#line 142
                continue;
            }

#line 142
            bestDistance_1 = _S15;

#line 142
            bestSlot_1 = _S21;

#line 142
            bestPixel_1 = _S16;

#line 142
            dx_0 = dx_0 + int(1);

#line 142
        }

#line 140
        int dy_1 = dy_0 + int(1);

#line 140
        bestDistance_0 = bestDistance_1;

#line 140
        bestSlot_0 = bestSlot_1;

#line 140
        bestPixel_0 = bestPixel_1;

#line 140
        dy_0 = dy_1;

#line 140
    }

#line 166
    if(bestSlot_0 == 0U)
    {
        discard_fragment();

#line 166
    }

#line 171
    CyOutlineStyle_0 _S23 = (&kernelContext_1)->cyOutline_0->styles_0[bestSlot_0 - 1U];

#line 171
    float alpha_1;

    if((_S23.kind_0) == 2U)
    {
        float _S24 = saturate((_S23.width_0 + 1.0 - sqrt(float(bestDistance_0))) / _S23.width_0);

#line 175
        alpha_1 = _S23.strength_0 * (_S24 * _S24);

#line 173
    }
    else
    {

#line 173
        alpha_1 = _S23.strength_0;

#line 173
    }

#line 178
    float alpha_2 = alpha_1 * unpackColour_0(_S23.colour_0).w;

#line 178
    bool _S25 = hidden_0(bestPixel_0, &kernelContext_1);
    if(_S25)
    {
        uint _S26 = (&kernelContext_1)->cyOutlineConstants_0->dash_0;
        if(!_S4)
        {

#line 182
            _S7 = true;

#line 182
        }
        else
        {

#line 182
            if(_S26 != 0U)
            {

#line 182
                uint _S27 = uint(_S3.x + _S3.y) / _S26;

#line 182
                _S7 = (_S27 & 1U) != 0U;

#line 182
            }
            else
            {

#line 182
                _S7 = false;

#line 182
            }

#line 182
        }

#line 182
        if(_S7)
        {
            discard_fragment();

#line 182
        }

#line 182
        alpha_1 = alpha_2 * (&kernelContext_1)->cyOutlineConstants_0->occludedAlpha_0;

#line 179
    }
    else
    {

#line 179
        alpha_1 = alpha_2;

#line 179
    }

#line 188
    if(alpha_1 <= 0.0)
    {
        discard_fragment();

#line 188
    }

#line 188
    pixelOutput_0 _S28 = { premultiplied_0(_S23.colour_0, alpha_1) };



    return _S28;
}

)cy_msl";

}  // namespace cy::rendering::selection

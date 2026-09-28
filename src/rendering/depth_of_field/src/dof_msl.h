#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for the depth of field dispatches. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::depth_of_field {

/// cyDofSetup.metal, 4174 bytes.
inline constexpr char kDofSetupMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 11 "src/rendering/depth_of_field/shaders/dof_common.slang"
struct CyDofConstants_0
{
    float4 depth_0;
    float4 lens_0;
    float4 aperture_0;
    uint4 extent_0;
    uint4 tiles_0;
};


#line 5163 "hlsl.meta.slang"
struct CyDofSetupSet_default_0
{
    texture2d<float, access::sample> source_0;
    depth2d<float, access::sample> depth_1;
    texture2d<float, access::read_write> layer_0;
};


#line 5163
struct KernelContext_0
{
    CyDofConstants_0 constant* cyDof_0;
    CyDofSetupSet_default_0 constant* cyDofSetupSet_0;
};


#line 35 "src/rendering/depth_of_field/shaders/dof_common.slang"
float cyDofViewDistance_0(float depth_2, KernelContext_0 thread* kernelContext_0)
{
    return (kernelContext_0->cyDof_0->depth_0.y - depth_2 * kernelContext_0->cyDof_0->depth_0.w) / (kernelContext_0->cyDof_0->depth_0.x - depth_2 * kernelContext_0->cyDof_0->depth_0.z);
}


float cyDofRadius_0(float distance_0, KernelContext_0 thread* kernelContext_1)
{
    float _S1 = max(distance_0, 0.00009999999747379);
    return kernelContext_1->cyDof_0->lens_0.x * (_S1 - kernelContext_1->cyDof_0->lens_0.y) / _S1;
}


float cyDofRadiusAtDepth_0(float depth_3, KernelContext_0 thread* kernelContext_2)
{

#line 48
    float _S2;

    if(depth_3 > 0.0)
    {

#line 50
        float _S3 = cyDofViewDistance_0(depth_3, kernelContext_2);

#line 50
        float _S4 = cyDofRadius_0(_S3, kernelContext_2);

#line 50
        _S2 = _S4;

#line 50
    }
    else
    {

#line 50
        _S2 = kernelContext_2->cyDof_0->lens_0.x;

#line 50
    }
    return clamp(_S2, - kernelContext_2->cyDof_0->lens_0.z, kernelContext_2->cyDof_0->lens_0.z);
}


#line 30 "src/rendering/depth_of_field/shaders/dof_setup.slang"
[[kernel]] void cyDofSetup(uint3 thread_0 [[thread_position_in_grid]], CyDofConstants_0 constant* cyDof_1 [[buffer(0)]], CyDofSetupSet_default_0 constant* cyDofSetupSet_1 [[buffer(1)]])
{

#line 30
    thread KernelContext_0 kernelContext_3;

#line 30
    (&kernelContext_3)->cyDof_0 = cyDof_1;

#line 30
    (&kernelContext_3)->cyDofSetupSet_0 = cyDofSetupSet_1;

#line 30
    bool _S5;

    if((thread_0.x) >= (cyDof_1->extent_0.z))
    {

#line 32
        _S5 = true;

#line 32
    }
    else
    {

#line 32
        _S5 = (thread_0.y) >= (cyDof_1->extent_0.w);

#line 32
    }

#line 32
    if(_S5)
    {
        return;
    }
    int2 _S6 = int2(cyDof_1->extent_0.xy) - int2(int(1)) ;
    thread array<float, int(4)> radii_0;
    thread array<float3, int(4)> colours_0;

#line 38
    float nearest_0 = (&kernelContext_3)->cyDof_0->lens_0.z;

#line 38
    uint corner_0 = 0U;

    for(;;)
    {

#line 40
        if(corner_0 < 4U)
        {
        }
        else
        {

#line 40
            break;
        }

        int3 _S7 = int3(min(int2(thread_0.xy) * int2(int(2))  + int2(int(corner_0 & 1U), int(corner_0 >> 1U)), _S6), int(0));

#line 43
        float _S8 = cyDofRadiusAtDepth_0((((&kernelContext_3)->cyDofSetupSet_0->depth_1).read(vec<uint,2>(((_S7)).xy), uint(((_S7)).z))), &kernelContext_3);

#line 43
        radii_0[corner_0] = _S8;
        colours_0[corner_0] = (((&kernelContext_3)->cyDofSetupSet_0->source_0).read(vec<uint,2>(((_S7)).xy), uint(((_S7)).z))).xyz;
        float _S9 = min(nearest_0, _S8);

#line 40
        uint corner_1 = corner_0 + 1U;

#line 40
        nearest_0 = _S9;

#line 40
        corner_0 = corner_1;

#line 40
    }

#line 47
    float3 _S10 = float3(0.0) ;

#line 47
    corner_0 = 0U;

#line 47
    float3 colour_0 = _S10;

#line 47
    float count_0 = 0.0;

    for(;;)
    {

#line 49
        if(corner_0 < 4U)
        {
        }
        else
        {

#line 49
            break;
        }
        if((radii_0[corner_0] - nearest_0) < 1.0)
        {

            float count_1 = count_0 + 1.0;

#line 54
            colour_0 = colour_0 + colours_0[corner_0];

#line 54
            count_0 = count_1;

#line 51
        }

#line 49
        corner_0 = corner_0 + 1U;

#line 49
    }

#line 57
    (&kernelContext_3)->cyDofSetupSet_0->layer_0.write(float4(colour_0 / float3(count_0) , nearest_0),thread_0.xy);
    return;
}

)cy_msl";

/// cyDofTiles.metal, 2609 bytes.
inline constexpr char kDofTilesMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 11 "src/rendering/depth_of_field/shaders/dof_common.slang"
struct CyDofConstants_0
{
    float4 depth_0;
    float4 lens_0;
    float4 aperture_0;
    uint4 extent_0;
    uint4 tiles_0;
};


#line 5163 "hlsl.meta.slang"
struct CyDofTilesSet_default_0
{
    texture2d<float, access::sample> source_0;
    texture2d<float, access::read_write> tiles_1;
};


#line 5163
struct KernelContext_0
{
    CyDofConstants_0 constant* cyDof_0;
    CyDofTilesSet_default_0 constant* cyDofTilesSet_0;
};


#line 28 "src/rendering/depth_of_field/shaders/dof_tiles.slang"
[[kernel]] void cyDofTiles(uint3 thread_0 [[thread_position_in_grid]], CyDofConstants_0 constant* cyDof_1 [[buffer(0)]], CyDofTilesSet_default_0 constant* cyDofTilesSet_1 [[buffer(1)]])
{

#line 28
    thread KernelContext_0 kernelContext_0;

#line 28
    (&kernelContext_0)->cyDof_0 = cyDof_1;

#line 28
    (&kernelContext_0)->cyDofTilesSet_0 = cyDofTilesSet_1;

#line 28
    bool _S1;

    if((thread_0.x) >= (cyDof_1->tiles_0.y))
    {

#line 30
        _S1 = true;

#line 30
    }
    else
    {

#line 30
        _S1 = (thread_0.y) >= (cyDof_1->tiles_0.z);

#line 30
    }

#line 30
    if(_S1)
    {
        return;
    }

    uint2 _S2 = thread_0.xy;

#line 35
    uint2 _S3 = uint2(cyDof_1->tiles_0.x) ;

#line 35
    uint2 _S4 = _S2 * _S3;
    uint2 _S5 = min(_S4 + _S3, (&kernelContext_0)->cyDof_0->extent_0.zw);


    uint _S6 = _S4.y;

#line 39
    float nearReach_0 = 0.0;

#line 39
    float farReach_0 = 0.0;

#line 39
    uint y_0 = _S6;

#line 39
    for(;;)
    {

#line 39
        if(y_0 < (_S5.y))
        {
        }
        else
        {

#line 39
            break;
        }

#line 39
        uint x_0 = _S4.x;

        for(;;)
        {

#line 41
            if(x_0 < (_S5.x))
            {
            }
            else
            {

#line 41
                break;
            }
            int3 _S7 = int3(int(x_0), int(y_0), int(0));

#line 43
            float _S8 = (((&kernelContext_0)->cyDofTilesSet_0->source_0).read(vec<uint,2>(((_S7)).xy), uint(((_S7)).z))).w;
            float _S9 = max(nearReach_0, - _S8);
            float _S10 = max(farReach_0, _S8);

#line 41
            uint x_1 = x_0 + 1U;

#line 41
            nearReach_0 = _S9;

#line 41
            farReach_0 = _S10;

#line 41
            x_0 = x_1;

#line 41
        }

#line 39
        y_0 = y_0 + 1U;

#line 39
    }

#line 48
    (&kernelContext_0)->cyDofTilesSet_0->tiles_1.write(float4(nearReach_0, farReach_0, 0.0, 0.0),_S2);
    return;
}

)cy_msl";

/// cyDofDilate.metal, 2342 bytes.
inline constexpr char kDofDilateMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 11 "src/rendering/depth_of_field/shaders/dof_common.slang"
struct CyDofConstants_0
{
    float4 depth_0;
    float4 lens_0;
    float4 aperture_0;
    uint4 extent_0;
    uint4 tiles_0;
};


#line 5163 "hlsl.meta.slang"
struct CyDofTilesSet_default_0
{
    texture2d<float, access::sample> source_0;
    texture2d<float, access::read_write> tiles_1;
};


#line 5163
struct KernelContext_0
{
    CyDofConstants_0 constant* cyDof_0;
    CyDofTilesSet_default_0 constant* cyDofTilesSet_0;
};


#line 53 "src/rendering/depth_of_field/shaders/dof_tiles.slang"
[[kernel]] void cyDofDilate(uint3 thread_0 [[thread_position_in_grid]], CyDofConstants_0 constant* cyDof_1 [[buffer(0)]], CyDofTilesSet_default_0 constant* cyDofTilesSet_1 [[buffer(1)]])
{

#line 53
    thread KernelContext_0 kernelContext_0;

#line 53
    (&kernelContext_0)->cyDof_0 = cyDof_1;

#line 53
    (&kernelContext_0)->cyDofTilesSet_0 = cyDofTilesSet_1;

#line 53
    bool _S1;

    if((thread_0.x) >= (cyDof_1->tiles_0.y))
    {

#line 55
        _S1 = true;

#line 55
    }
    else
    {

#line 55
        _S1 = (thread_0.y) >= (cyDof_1->tiles_0.z);

#line 55
    }

#line 55
    if(_S1)
    {
        return;
    }
    int2 _S2 = int2(cyDof_1->tiles_0.yz) - int2(int(1)) ;

#line 59
    float2 reach_0 = float2(0.0) ;

#line 59
    int dy_0 = int(-1);

    for(;;)
    {

#line 61
        if(dy_0 <= int(1))
        {
        }
        else
        {

#line 61
            break;
        }

#line 61
        int dx_0 = int(-1);

        for(;;)
        {

#line 63
            if(dx_0 <= int(1))
            {
            }
            else
            {

#line 63
                break;
            }

            int3 _S3 = int3(clamp(int2(thread_0.xy) + int2(dx_0, dy_0), int2(int(0)) , _S2), int(0));

#line 66
            float2 _S4 = max(reach_0, (((&kernelContext_0)->cyDofTilesSet_0->source_0).read(vec<uint,2>(((_S3)).xy), uint(((_S3)).z))).xy);

#line 63
            int dx_1 = dx_0 + int(1);

#line 63
            reach_0 = _S4;

#line 63
            dx_0 = dx_1;

#line 63
        }

#line 61
        dy_0 = dy_0 + int(1);

#line 61
    }

#line 69
    (&kernelContext_0)->cyDofTilesSet_0->tiles_1.write(float4(reach_0, 0.0, 0.0),thread_0.xy);
    return;
}

)cy_msl";

/// cyDofGather.metal, 8805 bytes.
inline constexpr char kDofGatherMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 11 "src/rendering/depth_of_field/shaders/dof_common.slang"
struct CyDofConstants_0
{
    float4 depth_0;
    float4 lens_0;
    float4 aperture_0;
    uint4 extent_0;
    uint4 tiles_0;
};


#line 5163 "hlsl.meta.slang"
struct CyDofGatherSet_default_0
{
    texture2d<float, access::sample> layer_0;
    texture2d<float, access::sample> tiles_1;
    texture2d<float, access::read_write> farField_0;
    texture2d<float, access::read_write> nearField_0;
};


#line 5163
struct KernelContext_0
{
    CyDofConstants_0 constant* cyDof_0;
    CyDofGatherSet_default_0 constant* cyDofGatherSet_0;
};


#line 61 "src/rendering/depth_of_field/shaders/dof_gather.slang"
float4 cyDofLayerAt_0(int2 texel_0, KernelContext_0 thread* kernelContext_0)
{
    int3 _S1 = int3(clamp(texel_0, int2(int(0)) , int2(kernelContext_0->cyDof_0->extent_0.zw) - int2(int(1)) ), int(0));

#line 63
    return ((kernelContext_0->cyDofGatherSet_0->layer_0).read(vec<uint,2>(((_S1)).xy), uint(((_S1)).z)));
}


#line 69 "src/rendering/depth_of_field/shaders/dof_common.slang"
uint cyDofRings_0(float radius_0, KernelContext_0 thread* kernelContext_1)
{
    return min(uint(ceil(max(radius_0, 1.0))), uint(kernelContext_1->cyDof_0->aperture_0.w));
}


#line 78
float cyDofSpan_0(float radius_1, uint rings_0)
{
    float _S2 = float(rings_0);

#line 80
    return (radius_1 + 0.5) * (_S2 + 0.5) / _S2;
}


#line 122
uint cyDofTapCount_0(uint rings_1)
{
    return 1U + 3U * rings_1 * (rings_1 + 1U);
}


#line 84
float3 cyDofTap_0(uint index_0, uint rings_2, float radius_2)
{

#line 84
    uint _S3;

    float _S4 = radius_2 / (float(rings_2) + 0.5);
    if(index_0 == 0U)
    {
        return float3(0.0, 0.0, 0.78539818525314331 * _S4 * _S4);
    }

#line 89
    uint ring_0 = 1U;


    for(;;)
    {

#line 92
        uint _S5 = 3U * ring_0;

#line 92
        _S3 = _S5;

#line 92
        uint _S6 = ring_0 + 1U;

#line 92
        if(index_0 >= (1U + _S5 * _S6))
        {
        }
        else
        {

#line 92
            break;
        }

#line 92
        ring_0 = _S6;

#line 92
    }

#line 97
    uint _S7 = 6U * ring_0;
    float _S8 = float(index_0 - (1U + _S3 * (ring_0 - 1U)));

#line 98
    float _S9;

#line 98
    if((ring_0 & 1U) != 0U)
    {

#line 98
        _S9 = 0.0;

#line 98
    }
    else
    {

#line 98
        _S9 = 0.5;

#line 98
    }
    float _S10 = 6.28318548202514648 * ((_S8 + _S9) / float(_S7));
    float _S11 = float(ring_0) * _S4;
    return float3(_S11 * cos(_S10), _S11 * sin(_S10), 1.04719758033752441 * _S4 * _S4);
}


#line 55
float cyDofApertureExtent_0(float angle_0, KernelContext_0 thread* kernelContext_2)
{
    float _S12 = kernelContext_2->cyDof_0->aperture_0.x;
    if(_S12 < 1.0)
    {
        return 1.0;
    }
    float _S13 = 6.28318548202514648 / _S12;
    float _S14 = angle_0 - kernelContext_2->cyDof_0->aperture_0.y;

    float _S15 = 0.5 * _S13;

#line 65
    return cos(_S15) / cos(_S14 - _S13 * floor(_S14 / _S13) - _S15);
}


#line 107
float cyDofReach_0(float2 offset_0, float radius_3, KernelContext_0 thread* kernelContext_3)
{
    float _S16 = length(offset_0);

#line 109
    float _S17;

    if(_S16 > 0.0)
    {

#line 111
        float _S18 = cyDofApertureExtent_0(atan2(- offset_0.y, - offset_0.x), kernelContext_3);

#line 111
        _S17 = _S18;

#line 111
    }
    else
    {

#line 111
        _S17 = 1.0;

#line 111
    }
    return saturate(radius_3 * _S17 - _S16 + 0.5);
}


#line 67 "src/rendering/depth_of_field/shaders/dof_gather.slang"
float3 cyDofGatherFar_0(int2 pixel_0, float4 centre_0, KernelContext_0 thread* kernelContext_4)
{
    float _S19 = centre_0.w;

#line 69
    if(_S19 <= (kernelContext_4->cyDof_0->lens_0.w))
    {
        return centre_0.xyz;
    }
    float _S20 = _S19 * 0.5;

#line 73
    uint _S21 = cyDofRings_0(_S20, kernelContext_4);

    float _S22 = cyDofSpan_0(_S20, _S21);
    float3 _S23 = float3(0.0) ;

#line 76
    uint index_1 = 0U;

#line 76
    float3 colour_0 = _S23;

#line 76
    float weight_0 = 0.0;

    for(;;)
    {

#line 78
        if(index_1 < (cyDofTapCount_0(_S21)))
        {
        }
        else
        {

#line 78
            break;
        }
        float3 _S24 = cyDofTap_0(index_1, _S21, _S22);
        int2 _S25 = int2(round(_S24.xy));

#line 81
        float4 _S26 = cyDofLayerAt_0(pixel_0 + _S25, kernelContext_4);

        float _S27 = _S26.w;

#line 83
        if(_S27 <= (kernelContext_4->cyDof_0->lens_0.w))
        {
            index_1 = index_1 + 1U;

#line 78
            continue;
        }

#line 87
        float _S28 = _S27 * 0.5;
        float _S29 = _S24.z;

#line 88
        float _S30 = cyDofReach_0(float2(_S25), _S28, kernelContext_4);

#line 88
        float _S31 = _S29 * _S30 / (kernelContext_4->cyDof_0->aperture_0.z * _S28 * _S28);

        float weight_1 = weight_0 + _S31;

#line 90
        colour_0 = colour_0 + _S26.xyz * float3(_S31) ;

#line 90
        weight_0 = weight_1;

#line 78
        index_1 = index_1 + 1U;

#line 78
    }

#line 92
    if(weight_0 > 0.0)
    {

#line 92
        colour_0 = colour_0 / float3(weight_0) ;

#line 92
    }
    else
    {

#line 92
        colour_0 = centre_0.xyz;

#line 92
    }

#line 92
    return colour_0;
}


#line 116 "src/rendering/depth_of_field/shaders/dof_common.slang"
float cyDofDefocus_0(float radiusPixels_0, KernelContext_0 thread* kernelContext_5)
{
    return saturate(abs(radiusPixels_0) - kernelContext_5->cyDof_0->lens_0.w);
}

)cy_msl"
    R"cy_msl(
#line 96 "src/rendering/depth_of_field/shaders/dof_gather.slang"
float4 cyDofGatherNear_0(int2 pixel_1, KernelContext_0 thread* kernelContext_6)
{

#line 96
    texture2d<float, access::sample> _S32 = kernelContext_6->cyDofGatherSet_0->tiles_1;

    uint2 _S33 = uint2(pixel_1) / uint2(kernelContext_6->cyDof_0->tiles_0.x) ;

#line 98
    int3 _S34 = int3(int2(_S33), int(0));

#line 98
    float _S35 = ((_S32).read(vec<uint,2>(((_S34)).xy), uint(((_S34)).z))).x;
    if(_S35 <= (kernelContext_6->cyDof_0->lens_0.w))
    {
        return float4(0.0) ;
    }
    float _S36 = _S35 * 0.5;

#line 103
    uint _S37 = cyDofRings_0(_S36, kernelContext_6);

    float _S38 = cyDofSpan_0(_S36, _S37);
    float3 _S39 = float3(0.0) ;

#line 106
    uint index_2 = 0U;

#line 106
    float3 colour_1 = _S39;

#line 106
    float coverage_0 = 0.0;

    for(;;)
    {

#line 108
        if(index_2 < (cyDofTapCount_0(_S37)))
        {
        }
        else
        {

#line 108
            break;
        }
        float3 _S40 = cyDofTap_0(index_2, _S37, _S38);
        int2 _S41 = int2(round(_S40.xy));

#line 111
        float4 _S42 = cyDofLayerAt_0(pixel_1 + _S41, kernelContext_6);

        float _S43 = _S42.w;

#line 113
        if(_S43 >= (- kernelContext_6->cyDof_0->lens_0.w))
        {
            index_2 = index_2 + 1U;

#line 108
            continue;
        }

#line 117
        float _S44 = - _S43 * 0.5;
        float _S45 = _S40.z;

#line 118
        float _S46 = cyDofReach_0(float2(_S41), _S44, kernelContext_6);

#line 118
        float _S47 = _S45 * _S46 / (kernelContext_6->cyDof_0->aperture_0.z * _S44 * _S44);

#line 118
        float _S48 = cyDofDefocus_0(_S43, kernelContext_6);

#line 118
        float _S49 = _S47 * _S48;


        float coverage_1 = coverage_0 + _S49;

#line 121
        colour_1 = colour_1 + _S42.xyz * float3(_S49) ;

#line 121
        coverage_0 = coverage_1;

#line 108
        index_2 = index_2 + 1U;

#line 108
    }

#line 123
    if(coverage_0 <= 0.0)
    {
        return float4(0.0) ;
    }
    float _S50 = saturate(coverage_0);
    return float4(colour_1 / float3(coverage_0)  * float3(_S50) , _S50);
}



[[kernel]] void cyDofGather(uint3 thread_0 [[thread_position_in_grid]], CyDofConstants_0 constant* cyDof_1 [[buffer(0)]], CyDofGatherSet_default_0 constant* cyDofGatherSet_1 [[buffer(1)]])
{

#line 133
    thread KernelContext_0 kernelContext_7;

#line 133
    (&kernelContext_7)->cyDof_0 = cyDof_1;

#line 133
    (&kernelContext_7)->cyDofGatherSet_0 = cyDofGatherSet_1;

#line 133
    bool _S51;

    if((thread_0.x) >= (cyDof_1->extent_0.z))
    {

#line 135
        _S51 = true;

#line 135
    }
    else
    {

#line 135
        _S51 = (thread_0.y) >= (cyDof_1->extent_0.w);

#line 135
    }

#line 135
    if(_S51)
    {
        return;
    }
    uint2 _S52 = thread_0.xy;

#line 139
    int2 _S53 = int2(_S52);

#line 139
    float4 _S54 = cyDofLayerAt_0(_S53, &kernelContext_7);

#line 139
    float3 _S55 = cyDofGatherFar_0(_S53, _S54, &kernelContext_7);
    (&kernelContext_7)->cyDofGatherSet_0->farField_0.write(float4(_S55, 1.0),_S52);

#line 140
    float4 _S56 = cyDofGatherNear_0(_S53, &kernelContext_7);
    (&kernelContext_7)->cyDofGatherSet_0->nearField_0.write(_S56,_S52);
    return;
}

)cy_msl";

/// cyDofComposite.metal, 7325 bytes.
inline constexpr char kDofCompositeMsl[] =
    R"cy_msl(#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;

#line 11 "src/rendering/depth_of_field/shaders/dof_common.slang"
struct CyDofConstants_0
{
    float4 depth_0;
    float4 lens_0;
    float4 aperture_0;
    uint4 extent_0;
    uint4 tiles_0;
};


#line 5163 "hlsl.meta.slang"
struct CyDofCompositeSet_default_0
{
    texture2d<float, access::sample> source_0;
    depth2d<float, access::sample> depth_1;
    texture2d<float, access::sample> layer_0;
    texture2d<float, access::sample> farField_0;
    texture2d<float, access::sample> nearField_0;
    texture2d<float, access::read_write> target_0;
};


#line 5163
struct KernelContext_0
{
    CyDofConstants_0 constant* cyDof_0;
    CyDofCompositeSet_default_0 constant* cyDofCompositeSet_0;
};


#line 35 "src/rendering/depth_of_field/shaders/dof_common.slang"
float cyDofViewDistance_0(float depth_2, KernelContext_0 thread* kernelContext_0)
{
    return (kernelContext_0->cyDof_0->depth_0.y - depth_2 * kernelContext_0->cyDof_0->depth_0.w) / (kernelContext_0->cyDof_0->depth_0.x - depth_2 * kernelContext_0->cyDof_0->depth_0.z);
}


float cyDofRadius_0(float distance_0, KernelContext_0 thread* kernelContext_1)
{
    float _S1 = max(distance_0, 0.00009999999747379);
    return kernelContext_1->cyDof_0->lens_0.x * (_S1 - kernelContext_1->cyDof_0->lens_0.y) / _S1;
}


float cyDofRadiusAtDepth_0(float depth_3, KernelContext_0 thread* kernelContext_2)
{

#line 48
    float _S2;

    if(depth_3 > 0.0)
    {

#line 50
        float _S3 = cyDofViewDistance_0(depth_3, kernelContext_2);

#line 50
        float _S4 = cyDofRadius_0(_S3, kernelContext_2);

#line 50
        _S2 = _S4;

#line 50
    }
    else
    {

#line 50
        _S2 = kernelContext_2->cyDof_0->lens_0.x;

#line 50
    }
    return clamp(_S2, - kernelContext_2->cyDof_0->lens_0.z, kernelContext_2->cyDof_0->lens_0.z);
}


#line 116
float cyDofDefocus_0(float radiusPixels_0, KernelContext_0 thread* kernelContext_3)
{
    return saturate(abs(radiusPixels_0) - kernelContext_3->cyDof_0->lens_0.w);
}


#line 36 "src/rendering/depth_of_field/shaders/dof_composite.slang"
[[kernel]] void cyDofComposite(uint3 thread_0 [[thread_position_in_grid]], CyDofConstants_0 constant* cyDof_1 [[buffer(0)]], CyDofCompositeSet_default_0 constant* cyDofCompositeSet_1 [[buffer(1)]])
{

#line 36
    thread KernelContext_0 kernelContext_4;

#line 36
    (&kernelContext_4)->cyDof_0 = cyDof_1;

#line 36
    (&kernelContext_4)->cyDofCompositeSet_0 = cyDofCompositeSet_1;

#line 36
    bool _S5;

    if((thread_0.x) >= (cyDof_1->extent_0.x))
    {

#line 38
        _S5 = true;

#line 38
    }
    else
    {

#line 38
        _S5 = (thread_0.y) >= (cyDof_1->extent_0.y);

#line 38
    }

#line 38
    if(_S5)
    {
        return;
    }
    uint2 _S6 = thread_0.xy;

#line 42
    int2 _S7 = int2(_S6);
    int3 _S8 = int3(_S7, int(0));

#line 43
    float4 _S9 = (((&kernelContext_4)->cyDofCompositeSet_0->source_0).read(vec<uint,2>(((_S8)).xy), uint(((_S8)).z)));

#line 43
    float _S10 = cyDofRadiusAtDepth_0((((&kernelContext_4)->cyDofCompositeSet_0->depth_1).read(vec<uint,2>(((_S8)).xy), uint(((_S8)).z))), &kernelContext_4);

#line 43
    float2 _S11 = float2(0.5) ;



    float2 _S12 = (float2(_S7) + _S11) * _S11 - _S11;
    int2 _S13 = int2(floor(_S12));
    float2 _S14 = _S12 - float2(_S13);
    int2 _S15 = int2(cyDof_1->extent_0.zw) - int2(int(1)) ;

    float3 _S16 = _S9.xyz;

#line 52
    float _S17;
    if(_S10 > 0.0)
    {

#line 53
        float _S18 = cyDofDefocus_0(_S10, &kernelContext_4);

#line 53
        _S17 = _S18;

#line 53
    }
    else
    {

#line 53
        _S17 = 0.0;

#line 53
    }

#line 53
    float weight_0;

#line 53
    uint corner_0;

#line 53
    float3 result_0;
    if(_S17 > 0.0)
    {
        float3 _S19 = float3(0.0) ;

#line 56
        corner_0 = 0U;

#line 56
        float3 colour_0 = _S19;

#line 56
        weight_0 = 0.0;

        for(;;)
        {

#line 58
            if(corner_0 < 4U)
            {
            }
            else
            {

#line 58
                break;
            }
            int _S20 = int(corner_0 & 1U);

#line 60
            int _S21 = int(corner_0 >> 1U);
            int2 _S22 = clamp(_S13 + int2(_S20, _S21), int2(int(0)) , _S15);

#line 61
            float _S23;
            if(_S20 == int(0))
            {

#line 62
                _S23 = 1.0 - _S14.x;

#line 62
            }
            else
            {

#line 62
                _S23 = _S14.x;

#line 62
            }

#line 62
            float _S24;
            if(_S21 == int(0))
            {

#line 63
                _S24 = 1.0 - _S14.y;

#line 63
            }
            else
            {

#line 63
                _S24 = _S14.y;

#line 63
            }

#line 62
            float _S25 = _S23 * _S24;

            int3 _S26 = int3(_S22, int(0));

#line 64
            if(((((&kernelContext_4)->cyDofCompositeSet_0->layer_0).read(vec<uint,2>(((_S26)).xy), uint(((_S26)).z))).w) > ((&kernelContext_4)->cyDof_0->lens_0.w))
            {

#line 64
                _S5 = _S25 > 0.0;

#line 64
            }
            else
            {

#line 64
                _S5 = false;

#line 64
            }

#line 64
            if(_S5)
            {

                float weight_1 = weight_0 + _S25;

#line 67
                colour_0 = colour_0 + (((&kernelContext_4)->cyDofCompositeSet_0->farField_0).read(vec<uint,2>(((_S26)).xy), uint(((_S26)).z))).xyz * float3(_S25) ;

#line 67
                weight_0 = weight_1;

#line 64
            }

#line 58
            corner_0 = corner_0 + 1U;

#line 58
        }

#line 70
        if(weight_0 > 0.0)
        {

#line 70
            result_0 = mix(_S16, colour_0 / float3(weight_0) , float3(_S17) );

)cy_msl"
    R"cy_msl(#line 70
        }
        else
        {

#line 70
            result_0 = _S16;

#line 70
        }

#line 54
    }
    else
    {

#line 54
        result_0 = _S16;

#line 54
    }

#line 76
    float4 _S27 = float4(0.0) ;

#line 76
    corner_0 = 0U;

#line 76
    float4 covered_0 = _S27;
    for(;;)
    {

#line 77
        if(corner_0 < 4U)
        {
        }
        else
        {

#line 77
            break;
        }
        int _S28 = int(corner_0 & 1U);

#line 79
        int _S29 = int(corner_0 >> 1U);
        int2 _S30 = clamp(_S13 + int2(_S28, _S29), int2(int(0)) , _S15);
        if(_S28 == int(0))
        {

#line 81
            _S17 = 1.0 - _S14.x;

#line 81
        }
        else
        {

#line 81
            _S17 = _S14.x;

#line 81
        }
        if(_S29 == int(0))
        {

#line 82
            weight_0 = 1.0 - _S14.y;

#line 82
        }
        else
        {

#line 82
            weight_0 = _S14.y;

#line 82
        }
        int3 _S31 = int3(_S30, int(0));

#line 83
        float4 covered_1 = covered_0 + (((&kernelContext_4)->cyDofCompositeSet_0->nearField_0).read(vec<uint,2>(((_S31)).xy), uint(((_S31)).z))) * float4((_S17 * weight_0)) ;

#line 77
        corner_0 = corner_0 + 1U;

#line 77
        covered_0 = covered_1;

#line 77
    }

#line 85
    float _S32 = covered_0.w;

#line 85
    if(_S32 > 0.0)
    {

#line 85
        result_0 = result_0 * float3((1.0 - _S32))  + covered_0.xyz;

#line 85
    }



    (&kernelContext_4)->cyDofCompositeSet_0->target_0.write(float4(result_0, _S9.w),_S6);
    return;
}

)cy_msl";

}  // namespace cy::rendering::depth_of_field

// SPDX-License-Identifier: MIT
// Motion blur's readback copy: the colour the gather read, into a texture the pass owns. Declared
// only by a pass created with `MotionBlurPassDescription::readback`, and never part of the blur.
//
// WHY A DISPATCH AND NOT A TRANSFER. The gather's colour input is the temporal history, an image
// the frame assembly owns and creates for sampling and drawing only, and whose next import it
// derives from how the frame's own passes left it. A transfer read of it would need a usage it
// does not have and leave it in a layout the next frame does not import it as; a sampled read is
// exactly what the gather already does to it.
//
// OUTPUT: the colour, texel for texel.
//
// REGENERATING: see motion_blur_tile_max.slang.

#include "motion_blur_common.slang"

struct CyMotionBlurCopySet
{
    Texture2D<float4> color;
    [format("rgba16f")] RWTexture2D<float4> copy;
};

[[vk::binding(0, 0)]] ParameterBlock<CyMotionBlurCopySet> cyMotionBlurCopySet;

[shader("compute")]
[numthreads(8, 8, 1)]
void cyMotionBlurCopy(uint3 thread : SV_DispatchThreadID)
{
    let size = uint2(cyMotionBlur.extent.xy);
    if (thread.x >= size.x || thread.y >= size.y)
    {
        return;
    }
    cyMotionBlurCopySet.copy[int2(thread.xy)] =
        cyMotionBlurCopySet.color.Load(int3(int2(thread.xy), 0));
}

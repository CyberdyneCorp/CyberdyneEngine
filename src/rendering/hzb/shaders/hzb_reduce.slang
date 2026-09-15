// One level of the hierarchical depth buffer's reduction. M11.c task 4.1.
//
// `rendering-culling-and-lod` — "Occlusion culling": the pyramid is built from the previous frame's
// depth. This is the chain that builds it: one dispatch per level, each thread producing one texel
// of the destination from the 2x2 (or 2x3, or 3x3 — see the fold below) footprint underneath it.
//
// ================================================================================================
// THIS IS `Hzb::reduce()` TRANSCRIBED, AND THAT IS WHAT MAKES THE DEVICE PASS CHECKABLE
// ================================================================================================
//
// `cy::render::culling::Hzb::reduce()` has been in the tree since M6 with its own suite. The device
// pass is not checked against a screenshot or against a counter that reads non-zero; it is checked
// against that model's answer TEXEL FOR TEXEL, in `integration.rendering_culling`. So the fold for
// an odd dimension — the extra row and column go into the SAME parent texel rather than being
// dropped, which is what keeps the pyramid conservative at the edges — is transcribed rather than
// re-derived, and the initial value is 1.0f (the near plane under reversed Z) so that the minimum
// over an empty footprint is not mistaken for an occluder.
//
// THE PYRAMID STORES THE FURTHEST DEPTH OF EACH FOOTPRINT, which under the engine's reversed-Z
// convention is the SMALLEST value. Getting this inequality backwards produces a renderer that
// culls what is visible and draws what is not, and it looks like a broken pyramid rather than a
// flipped comparison.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc hzb_reduce.slang -target spirv -profile spirv_1_5 \
//          -entry hzb_reduce -stage compute -o hzb_reduce.spv
//   python3 embed_spirv.py ../src/hzb_spirv.h kHzbReduceSpirv=hzb_reduce.spv
//   clang-format -i ../src/hzb_spirv.h

// The concatenated pyramid: every level's texels in order, coarsest last. Bound once; a level is a
// range within it, which is why the reduction's source and destination are OFFSETS rather than two
// bindings.
[[vk::binding(0, 0)]] RWStructuredBuffer<float> pyramid;

struct ReduceSizes
{
    uint sourceOffset;
    uint destinationOffset;
    uint sourceWidth;
    uint sourceHeight;
    uint destinationWidth;
    uint destinationHeight;
    uint reserved0;
    uint reserved1;
};

[[vk::push_constant]] ReduceSizes sizes;

static const uint kReduceGroupSize = 8u;

[shader("compute")]
[numthreads(8, 8, 1)]
void hzb_reduce(uint3 thread: SV_DispatchThreadID)
{
    const uint x = thread.x;
    const uint y = thread.y;
    if (x >= sizes.destinationWidth || y >= sizes.destinationHeight)
    {
        return;
    }

    // `Hzb::reduce()`'s fold, condition for condition. A parent texel on the last column of an odd
    // child takes the extra column as well; likewise the last row. Expressed as the C++ expresses
    // it rather than simplified, because the two differ exactly at the edge case the fold exists
    // for.
    const bool lastColumn =
        (x * 2u) + 1u == (sizes.destinationWidth * 2u) - 1u && sizes.sourceWidth % 2u == 1u;
    const bool lastRow =
        (y * 2u) + 1u == (sizes.destinationHeight * 2u) - 1u && sizes.sourceHeight % 2u == 1u;
    const uint lastX = lastColumn ? sizes.sourceWidth - 1u : (x * 2u) + 1u;
    const uint lastY = lastRow ? sizes.sourceHeight - 1u : (y * 2u) + 1u;

    float furthest = 1.0f;
    for (uint sampleY = y * 2u; sampleY <= lastY && sampleY < sizes.sourceHeight; ++sampleY)
    {
        for (uint sampleX = x * 2u; sampleX <= lastX && sampleX < sizes.sourceWidth; ++sampleX)
        {
            const float value =
                pyramid[sizes.sourceOffset + (sampleY * sizes.sourceWidth) + sampleX];
            furthest = value < furthest ? value : furthest;
        }
    }
    pyramid[sizes.destinationOffset + (y * sizes.destinationWidth) + x] = furthest;
}

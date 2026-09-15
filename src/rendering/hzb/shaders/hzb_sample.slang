// The hierarchical depth buffer's TEST, as the one copy every consumer includes. M11.c task 4.1.
//
// ================================================================================================
// WHY THIS IS AN INCLUDE AND NOT A SECOND COPY IN EACH CONSUMER
// ================================================================================================
//
// Two rows are waiting on one piece of work — `rendering-culling-and-lod`'s occlusion cull and
// `virtual-geometry`'s cluster-granular occlusion — and the specification delta M11.c adds says the
// two SHALL consume the same pyramid, because two implementations of one structure let one piece of
// work be recorded as two satisfied requirements. That claim is a link-graph fact on the C++ side
// (`cy::rendering-hzb`, linked by both) and it is THIS FILE on the shader side: `gpu_cull.slang`
// and `vg_traversal.slang` both `#include` it, and each one's slangc invocation carries
//
//     -I ../../hzb/shaders
//
// so the include path is part of the recorded command rather than something a regenerator guesses.
//
// ================================================================================================
// EVERY EXPRESSION IS `hzb.cpp`'S, IN ITS ORDER
// ================================================================================================
//
// `cy::render::culling::project_sphere`, `hzb_level_for` and `Hzb::occludes` are the CPU model this
// is checked against — not a reference "written for the test", but the model that has been in the
// tree since M6 and that `unit.rendering-culling` already covers. A comparison against it is only a
// comparison if the arithmetic is the same arithmetic, so the eight corners are visited in the same
// order, the near-plane rejection is the same `!(clip.w > 1.0e-6f)`, the level is the same
// `ceil(log2(span))`, and the occlusion test is the same STRICT `<` — an instance exactly at the
// occluder's depth is drawn, because coplanar geometry is where a wrong answer is visible.

#ifndef CY_HZB_SAMPLE_SLANG
#define CY_HZB_SAMPLE_SLANG

/// `cy::rendering::hzb::HzbParams`, 80 bytes. `viewProjection` is four COLUMNS of four, because
/// `cy::Mat4` is column-major with column vectors; the projection below is written out as
/// `c0*x + c1*y + c2*z + c3` rather than as a `mul`, whose operand order is a convention two
/// languages do not have to share.
struct HzbParams
{
    float4 viewProjection[4];
    uint width;
    uint height;
    uint levelCount;
    /// Zero answers "not occluded" for everything. `Hzb::invalidate()`, as data.
    uint enabled;
};

/// `cy::render::culling::halved` — never below one, so the chain terminates at a single texel.
uint hzbHalved(uint value)
{
    return value > 1u ? (value + 1u) / 2u : 1u;
}

uint hzbLevelWidth(uint width, uint level)
{
    for (uint step = 0u; step < level; ++step)
    {
        width = hzbHalved(width);
    }
    return width;
}

/// The first texel of a level in the concatenated pyramid. `cy::rendering::hzb::hzb_level_offset`.
uint hzbLevelOffset(uint width, uint height, uint level)
{
    uint offset = 0u;
    uint levelWidth = width;
    uint levelHeight = height;
    for (uint step = 0u; step < level; ++step)
    {
        offset += levelWidth * levelHeight;
        levelWidth = hzbHalved(levelWidth);
        levelHeight = hzbHalved(levelHeight);
    }
    return offset;
}

/// `cy::render::culling::ScreenRect`.
struct HzbRect
{
    float minX;
    float minY;
    float maxX;
    float maxY;
    float nearestDepth;
    bool valid;
};

float hzbClamp01(float value)
{
    if (value < 0.0f)
    {
        return 0.0f;
    }
    return value > 1.0f ? 1.0f : value;
}

/// `project_sphere`, corner for corner.
///
/// The projection of a sphere is an ellipse whose exact extent needs a quadratic; the bounding box
/// of the sphere is cheaper and CONTAINS that ellipse, which is the direction of error this test is
/// allowed to make — a rectangle larger than the silhouette can only fail to occlude.
HzbRect hzbProjectSphere(HzbParams params, float3 centre, float radius)
{
    HzbRect rect;
    rect.minX = 0.0f;
    rect.minY = 0.0f;
    rect.maxX = 0.0f;
    rect.maxY = 0.0f;
    rect.nearestDepth = 0.0f;
    rect.valid = false;
    if (params.width == 0u || params.height == 0u || !(radius > 0.0f))
    {
        return rect;
    }

    float minX = 1.0f;
    float minY = 1.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    float nearest = 0.0f;
    for (uint corner = 0u; corner < 8u; ++corner)
    {
        const float3 point = float3(centre.x + ((corner & 1u) != 0u ? radius : -radius),
                                    centre.y + ((corner & 2u) != 0u ? radius : -radius),
                                    centre.z + ((corner & 4u) != 0u ? radius : -radius));
        const float4 clip = params.viewProjection[0] * point.x + params.viewProjection[1] * point.y +
                            params.viewProjection[2] * point.z + params.viewProjection[3];
        if (!(clip.w > 1.0e-6f))
        {
            // A corner behind or on the eye. The whole box straddles the near plane, so nothing
            // useful can be said about it and it must be drawn.
            return rect;
        }
        const float inverseW = 1.0f / clip.w;
        const float ndcX = clip.x * inverseW;
        const float ndcY = clip.y * inverseW;
        const float depth = clip.z * inverseW;
        const float screenX = (ndcX * 0.5f) + 0.5f;
        const float screenY = (ndcY * 0.5f) + 0.5f;
        minX = screenX < minX ? screenX : minX;
        minY = screenY < minY ? screenY : minY;
        maxX = screenX > maxX ? screenX : maxX;
        maxY = screenY > maxY ? screenY : maxY;
        // Reversed Z: the NEAREST point has the LARGEST depth.
        nearest = depth > nearest ? depth : nearest;
    }
    if (maxX <= 0.0f || maxY <= 0.0f || minX >= 1.0f || minY >= 1.0f)
    {
        return rect;
    }

    rect.minX = hzbClamp01(minX) * float(params.width);
    rect.minY = hzbClamp01(minY) * float(params.height);
    rect.maxX = hzbClamp01(maxX) * float(params.width);
    rect.maxY = hzbClamp01(maxY) * float(params.height);
    rect.nearestDepth = hzbClamp01(nearest);
    rect.valid = true;
    return rect;
}

/// `hzb_level_for`: the level whose texels are at least half the rectangle across, so at most a 2x2
/// of them covers it and the test is four taps.
uint hzbLevelFor(HzbRect rect, uint levelCount)
{
    if (levelCount == 0u)
    {
        return 0u;
    }
    const float spanX = rect.maxX - rect.minX;
    const float spanY = rect.maxY - rect.minY;
    const float span = spanX > spanY ? spanX : spanY;
    if (!(span > 1.0f))
    {
        return 0u;
    }
    const uint level = uint(ceil(log2(span)));
    return level < levelCount ? level : levelCount - 1u;
}

uint hzbToTexel(float value, uint limit)
{
    if (!(value > 0.0f))
    {
        return 0u;
    }
    const uint texel = uint(value);
    return texel < limit ? texel : limit - 1u;
}

/// `Hzb::occludes`, over the concatenated pyramid.
///
/// False whenever the pyramid is disabled, the rectangle is invalid, or the rectangle leaves the
/// buffer — every one of those is a case where the answer is NOT KNOWN, and "not known" has to read
/// as "draw it". False occlusion is a correctness bug rather than a quality one.
bool hzbOccludes(StructuredBuffer<float> pyramid, HzbParams params, HzbRect rect)
{
    if (params.enabled == 0u || !rect.valid || params.levelCount == 0u)
    {
        return false;
    }
    const uint level = hzbLevelFor(rect, params.levelCount);
    const uint levelW = hzbLevelWidth(params.width, level);
    const uint levelH = hzbLevelWidth(params.height, level);
    const uint base = hzbLevelOffset(params.width, params.height, level);
    const float scale = 1.0f / float(1u << level);
    const uint minX = hzbToTexel(rect.minX * scale, levelW);
    const uint maxX = hzbToTexel(rect.maxX * scale, levelW);
    const uint minY = hzbToTexel(rect.minY * scale, levelH);
    const uint maxY = hzbToTexel(rect.maxY * scale, levelH);

    // The furthest depth anything already drawn reaches over the whole footprint.
    float furthest = 1.0f;
    for (uint y = minY; y <= maxY; ++y)
    {
        for (uint x = minX; x <= maxX; ++x)
        {
            const float value = pyramid[base + (y * levelW) + x];
            furthest = value < furthest ? value : furthest;
        }
    }
    // Occluded when the instance's NEAREST point is still further away than that — under reversed
    // Z, when its depth is smaller. STRICTLY less, so an instance exactly at the occluder's depth
    // is drawn.
    return rect.nearestDepth < furthest;
}

/// The whole test for a world-space bounding sphere, which is what `HzbOcclusionTester::occluded`
/// is on the CPU.
bool hzbOccludesSphere(StructuredBuffer<float> pyramid, HzbParams params, float3 centre,
                       float radius)
{
    return hzbOccludes(pyramid, params, hzbProjectSphere(params, centre, radius));
}

#endif  // CY_HZB_SAMPLE_SLANG

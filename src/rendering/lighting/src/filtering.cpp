#include <cy/rendering/lighting/filtering.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {

const char* shadow_filter_name(ShadowFilter filter) noexcept {
    switch (filter) {
        case ShadowFilter::Hard:
            return "hard";
        case ShadowFilter::PercentageCloser:
            return "pcf";
        case ShadowFilter::PercentageCloserSoft:
            return "pcss";
        case ShadowFilter::Count:
            break;
    }
    return "unknown";
}

u32 shadow_sample_count(ShadowQuality quality) noexcept {
    switch (quality) {
        case ShadowQuality::Low:
            return 1;
        case ShadowQuality::Medium:
            return 4;
        case ShadowQuality::High:
            return 8;
        case ShadowQuality::Ultra:
            return 16;
        case ShadowQuality::Count:
            break;
    }
    return 4;
}

u32 blocker_search_sample_count(ShadowQuality quality) noexcept {
    // Half the filter's, floored at four: the search estimates an average depth, and an average is
    // far less sensitive to the sample count than an edge is.
    return math::max(shadow_sample_count(quality) / 2U, 4U);
}

f32 shadow_depth_bias(const ShadowBiasSettings& settings, f32 normal_dot_light) noexcept {
    const f32 cosine = math::clamp(normal_dot_light, 1e-3F, 1.0F);
    const f32 tangent = std::sqrt(math::max(1.0F - (cosine * cosine), 0.0F)) / cosine;
    // Clamped at 4, which is about 76 degrees off the light. Past that the slope term would grow
    // without bound and the normal offset is what is actually holding the result together.
    return settings.constant + (settings.slope_scaled * math::min(tangent, 4.0F));
}

Vec3 shadow_normal_offset(const ShadowBiasSettings& settings, Vec3 surface_normal,
                          f32 normal_dot_light, f32 texel_world_size) noexcept {
    const f32 cosine = math::clamp(normal_dot_light, 0.0F, 1.0F);
    // sin(angle): the texel's footprint on the surface grows with exactly this, so the offset does
    // too. At normal incidence it is zero and the offset vanishes, which is what keeps a contact
    // shadow attached.
    const f32 sine = std::sqrt(math::max(1.0F - (cosine * cosine), 0.0F));
    const f32 magnitude = settings.normal_offset_texels * texel_world_size * sine;
    return surface_normal * magnitude;
}

f32 filter_radius_uv(f32 world_radius, f32 projection_extent, u32 atlas_size,
                     u32 tile_size) noexcept {
    if (projection_extent <= 0.0F || atlas_size == 0 || tile_size == 0) {
        return 0.0F;
    }
    // The world radius as a fraction of the projection's extent is the radius in the TILE's own UV;
    // scaling by the tile's share of the atlas puts it in atlas UV, which is what a sampler takes.
    const f32 tile_uv = world_radius / projection_extent;
    return tile_uv * (static_cast<f32>(tile_size) / static_cast<f32>(atlas_size));
}

f32 pcss_penumbra_radius(f32 receiver_depth, f32 average_blocker_depth,
                         f32 light_size_uv) noexcept {
    // REVERSED-Z: a blocker is nearer and therefore has the GREATER depth value. A search that
    // found nothing hands back a depth at or behind the receiver, and that must produce no kernel
    // rather than a negative one.
    if (average_blocker_depth <= receiver_depth) {
        return 0.0F;
    }
    const f32 separation = average_blocker_depth - receiver_depth;
    return (separation / math::max(average_blocker_depth, 1e-6F)) * light_size_uv;
}

}  // namespace cy::rendering

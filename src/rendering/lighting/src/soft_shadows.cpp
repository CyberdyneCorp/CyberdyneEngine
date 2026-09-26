#include <cy/rendering/lighting/soft_shadows.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {
namespace {

constexpr f32 kTwoPi = 6.28318530718F;

/// `cy/sampling.slang`'s `radicalInverseBase2`.
[[nodiscard]] f32 radical_inverse(u32 bits) noexcept {
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<f32>(bits) * 2.3283064365386963e-10F;
}

struct DiscTap {
    f32 u = 0.0F;
    f32 v = 0.0F;
};

/// `shadowDiscTap`: Hammersley spread over the unit disc, then rotated.
[[nodiscard]] DiscTap disc_tap(u32 index, u32 count, f32 cosine, f32 sine) noexcept {
    const f32 radius = std::sqrt(static_cast<f32>(index) / static_cast<f32>(count));
    const f32 angle = radical_inverse(index) * kTwoPi;
    const f32 x = std::cos(angle) * radius;
    const f32 y = std::sin(angle) * radius;
    return DiscTap{(x * cosine) - (y * sine), (x * sine) + (y * cosine)};
}

[[nodiscard]] f32 fraction(f32 value) noexcept {
    return value - std::floor(value);
}

[[nodiscard]] f32 filter(const ShadowDepthMap& map, f32 u, f32 v, f32 receiver, f32 radius,
                         f32 rotation, u32 taps) noexcept {
    const f32 cosine = std::cos(rotation);
    const f32 sine = std::sin(rotation);
    f32 lit = 0.0F;
    for (u32 index = 0; index < taps; ++index) {
        const DiscTap tap = disc_tap(index, taps, cosine, sine);
        const f32 stored = map.stored_depth(u + (tap.u * radius), v + (tap.v * radius));
        lit += receiver >= stored ? 1.0F : 0.0F;
    }
    return lit / static_cast<f32>(taps);
}

}  // namespace

PcssShape make_pcss_shape(const SoftShadowSettings& settings,
                          const DirectionalShadowFootprint& footprint) noexcept {
    PcssShape shape;
    shape.blocker_taps = blocker_search_sample_count(settings.quality);
    shape.filter_taps = shadow_sample_count(settings.quality);
    if (footprint.width_metres <= 0.0F || footprint.extent == 0U) {
        return shape;
    }
    shape.penumbra_per_depth =
        footprint.depth_range_metres * std::tan(settings.angular_radius) / footprint.width_metres;
    const f32 texel = 1.0F / static_cast<f32>(footprint.extent);
    shape.min_radius = settings.min_radius_texels * texel;
    shape.max_radius = math::max(settings.max_radius_texels, settings.min_radius_texels) * texel;
    return shape;
}

void write_soft_shadow_words(u32 flags, u32 contact_slot, const PcssShape& shape, u32 control[4],
                             f32 shape_words[4]) noexcept {
    control[0] = flags;
    control[1] = contact_slot;
    control[2] = shape.blocker_taps;
    control[3] = shape.filter_taps;
    shape_words[0] = shape.penumbra_per_depth;
    shape_words[1] = shape.min_radius;
    shape_words[2] = shape.max_radius;
    shape_words[3] = 0.0F;
}

f32 shadow_disc_rotation(f32 pixel_x, f32 pixel_y) noexcept {
    const f32 noise =
        fraction(52.9829189F * fraction((pixel_x * 0.06711056F) + (pixel_y * 0.00583715F)));
    return noise * kTwoPi;
}

f32 ShadowDepthMap::stored_depth(f32 u, f32 v) const noexcept {
    const auto last = static_cast<f32>(extent - 1U);
    const f32 x = math::clamp(std::floor(u * static_cast<f32>(extent)), 0.0F, last);
    const f32 y = math::clamp(std::floor(v * static_cast<f32>(extent)), 0.0F, last);
    return depths[(static_cast<usize>(y) * extent) + static_cast<usize>(x)];
}

PcssBlockers pcss_blocker_search(const ShadowDepthMap& map, f32 u, f32 v, f32 receiver,
                                 f32 search_radius, f32 rotation, u32 taps) noexcept {
    const f32 cosine = std::cos(rotation);
    const f32 sine = std::sin(rotation);
    PcssBlockers result;
    for (u32 index = 0; index < taps; ++index) {
        const DiscTap tap = disc_tap(index, taps, cosine, sine);
        const f32 stored =
            map.stored_depth(u + (tap.u * search_radius), v + (tap.v * search_radius));
        if (stored > receiver) {
            result.average_depth += stored;
            result.count += 1.0F;
        }
    }
    if (result.count > 0.0F) {
        result.average_depth /= result.count;
    }
    return result;
}

f32 pcss_directional_penumbra(f32 receiver, f32 average_blocker, f32 penumbra_per_depth) noexcept {
    return math::max(average_blocker - receiver, 0.0F) * penumbra_per_depth;
}

f32 pcss_visibility_reference(const ShadowDepthMap& map, f32 u, f32 v, f32 receiver,
                              const PcssShape& shape, f32 rotation) noexcept {
    const f32 search = math::clamp((1.0F - receiver) * shape.penumbra_per_depth, shape.min_radius,
                                   shape.max_radius);
    const u32 blocker_taps = math::max(shape.blocker_taps, 1U);
    const PcssBlockers blockers =
        pcss_blocker_search(map, u, v, receiver, search, rotation, blocker_taps);
    if (blockers.count == 0.0F) {
        return 1.0F;
    }
    const f32 radius = math::clamp(
        pcss_directional_penumbra(receiver, blockers.average_depth, shape.penumbra_per_depth),
        shape.min_radius, shape.max_radius);
    return filter(map, u, v, receiver, radius, rotation, math::max(shape.filter_taps, 1U));
}

}  // namespace cy::rendering

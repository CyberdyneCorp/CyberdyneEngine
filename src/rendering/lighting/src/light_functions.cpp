#include <cy/rendering/lighting/light_functions.h>

#include <cy/core/math/math.h>

#include <cmath>

namespace cy::rendering {
namespace {

[[nodiscard]] f32 wrap_unit(f32 value) noexcept {
    const f32 fraction = value - std::floor(value);
    return fraction < 0.0F ? fraction + 1.0F : fraction;
}

}  // namespace

const char* cookie_projection_kind_name(CookieProjectionKind kind) noexcept {
    switch (kind) {
        case CookieProjectionKind::OrthographicPlane:
            return "orthographic-plane";
        case CookieProjectionKind::ConePerspective:
            return "cone-perspective";
        case CookieProjectionKind::CubeDirection:
            return "cube-direction";
        case CookieProjectionKind::Count:
            break;
    }
    return "unknown";
}

CookieSample cookie_uv(const CookieProjection& projection, Vec3 world_position) noexcept {
    CookieSample sample;
    const Vec3 point = world_position + projection.world_offset;

    switch (projection.kind) {
        case CookieProjectionKind::OrthographicPlane: {
            // A directional light has no position: the cookie is a plane perpendicular to its
            // direction, and every point projects onto it by dropping the component along the
            // light. That is what makes a cloud cookie the same at any altitude, which is what a
            // cloud shadow is.
            const f32 scale = projection.world_scale > 0.0F ? projection.world_scale : 1.0F;
            sample.uv = Vec2{dot(point, projection.right) / scale,
                             dot(point, projection.up) / scale};
            sample.direction = normalized_or(projection.forward, Vec3{0.0F, 0.0F, -1.0F});
            sample.inside = true;
            break;
        }
        case CookieProjectionKind::ConePerspective: {
            const Vec3 offset = point - projection.position;
            const f32 depth = dot(offset, projection.forward);
            if (depth <= 1.0e-5F) {
                // Behind the apex. Lighting here is the classic gobo bug: the projection's
                // arithmetic is perfectly happy to mirror the pattern behind the projector.
                sample.inside = false;
                break;
            }
            const f32 half_extent = std::tan(math::clamp(projection.cone_half_angle, 1.0e-3F,
                                                         1.5F));
            const f32 x = dot(offset, projection.right) / (depth * half_extent);
            const f32 y = dot(offset, projection.up) / (depth * half_extent);
            // The cone's edge lands on the cookie's edge: [-1, 1] becomes [0, 1].
            sample.uv = Vec2{x * 0.5F + 0.5F, y * 0.5F + 0.5F};
            sample.direction = normalized_or(offset, projection.forward);
            sample.inside = true;
            break;
        }
        case CookieProjectionKind::CubeDirection: {
            const Vec3 offset = point - projection.position;
            sample.direction = normalized_or(offset, Vec3{0.0F, 0.0F, -1.0F});
            // The octahedral fold, so a cube cookie can also be stored as one 2D image. A caller
            // with a real cube map uses `direction` and ignores `uv`.
            const f32 sum = std::fabs(sample.direction.x) + std::fabs(sample.direction.y) +
                            std::fabs(sample.direction.z);
            const f32 inverse = sum > 0.0F ? 1.0F / sum : 0.0F;
            f32 u = sample.direction.x * inverse;
            f32 v = sample.direction.y * inverse;
            if (sample.direction.z < 0.0F) {
                const f32 folded_u = (1.0F - std::fabs(v)) * (u >= 0.0F ? 1.0F : -1.0F);
                const f32 folded_v = (1.0F - std::fabs(u)) * (v >= 0.0F ? 1.0F : -1.0F);
                u = folded_u;
                v = folded_v;
            }
            sample.uv = Vec2{u * 0.5F + 0.5F, v * 0.5F + 0.5F};
            sample.inside = true;
            break;
        }
        case CookieProjectionKind::Count:
            break;
    }

    if (!sample.inside) {
        return sample;
    }
    sample.uv = sample.uv + projection.scroll_uv;
    if (projection.tile) {
        sample.uv = Vec2{wrap_unit(sample.uv.x), wrap_unit(sample.uv.y)};
    } else if (sample.uv.x < 0.0F || sample.uv.x > 1.0F || sample.uv.y < 0.0F ||
               sample.uv.y > 1.0F) {
        sample.inside = false;
    }
    return sample;
}

void advance_cookie_scroll(CookieProjection& projection, Vec2 uv_per_second, Vec3 world_per_second,
                           f32 seconds) noexcept {
    projection.scroll_uv = projection.scroll_uv + uv_per_second * seconds;
    // Wrap here, once per light per frame, rather than per pixel. An unwrapped f32 UV drifts into
    // its own quantisation: at 0.1 UV per second, an hour of play reaches 360, where the spacing
    // between representable values is already coarser than a texel of a 4k cookie and the pattern
    // visibly steps.
    projection.scroll_uv = Vec2{wrap_unit(projection.scroll_uv.x), wrap_unit(projection.scroll_uv.y)};
    projection.world_offset = projection.world_offset + world_per_second * seconds;
}

f32 apply_light_function(const LightFunction& function, const CookieSample& sample,
                         f32 sampled_value) noexcept {
    const f32 value = sample.inside ? sampled_value : function.outside_value;
    return math::max(0.0F, value * function.intensity);
}

f32 cookie_filter_level(f32 footprint_uv, u32 mip_count) noexcept {
    if (mip_count <= 1U) {
        return 0.0F;
    }
    const f32 top = static_cast<f32>(mip_count - 1U);
    if (footprint_uv <= 0.0F) {
        return 0.0F;
    }
    // One texel of level `n` covers 2^n / size of UV space. Solving for the level whose texel
    // matches the footprint is a base-2 logarithm of the footprint in texels of level 0 — which is
    // the same derivation a hardware sampler does, written down here because the projection's
    // derivative is a CPU-side quantity for a directional cookie.
    const f32 level = std::log2(footprint_uv * static_cast<f32>(1U << (mip_count - 1U)));
    return math::clamp(level, 0.0F, top);
}

}  // namespace cy::rendering

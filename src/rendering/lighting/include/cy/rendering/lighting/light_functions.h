#pragma once
// Light functions and cookies: a texture masking a light's emission, projected in the light's own
// space. Task 10.3.
//
// `rendering-lighting-and-shadows` — "Light functions and cookies": "Point and spot lights SHALL
// support a **cookie** texture masking their emission, and directional lights SHALL support a
// cookie projected in light space, for effects such as window patterns and cloud shadows."
//
// ================================================================================================
// THE SCENARIO IS THE WHOLE DESIGN: "WITHOUT ADDITIONAL SHADOW MAP COST"
// ================================================================================================
//
// "WHEN a cloud-shadow cookie scrolls THEN the directional light's contribution SHALL be modulated
// by it without additional shadow map cost."
//
// A cookie is a coordinate transform and a texture read. It is not a shadow, it does not
// invalidate a shadow page, and it does not need one: `cookie_uv()` returns where to sample and
// nothing in this file names a shadow atlas, a page or a resolution. A scrolling cloud cookie
// therefore costs one `frac()` per frame on the CPU and one texture fetch per pixel, whatever the
// shadow system is doing — and an implementation that reached for a shadow map to do it would have
// to name one, which nothing here can.
//
// The scroll is applied to the PROJECTION, not to the texture: an offset added to the UV wraps
// with the texture's addressing mode, and a projection that translates does not. That distinction
// is why `CookieProjection::scroll_uv` is a UV offset and `CookieProjection::world_offset` is a
// metres one, and why both exist.

#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>

namespace cy::rendering {

/// How a cookie's texture coordinate is derived. The three cases are the three light types, and
/// they differ in what is projected rather than in what is sampled.
enum class CookieProjectionKind : u8 {
    /// A directional light: an orthographic projection along the light's direction, tiling in the
    /// plane perpendicular to it. Cloud shadows.
    OrthographicPlane = 0,
    /// A spot light: a perspective projection through the cone, so the cookie's centre lands on the
    /// cone's axis and its edge on the cone's edge. Window patterns, gobo lights.
    ConePerspective,
    /// A point light: a cube-map direction, so the cookie masks the whole sphere.
    CubeDirection,
    Count,
};

[[nodiscard]] const char* cookie_projection_kind_name(CookieProjectionKind kind) noexcept;

/// A cookie's projection. Everything is in world space except `scroll_uv`, which is in the
/// cookie's own.
struct CookieProjection {
    CookieProjectionKind kind = CookieProjectionKind::ConePerspective;

    /// The light's own frame. `forward` is the direction the light points; `right` and `up` span
    /// the cookie's plane.
    Vec3 forward{0.0F, 0.0F, -1.0F};
    Vec3 right{1.0F, 0.0F, 0.0F};
    Vec3 up{0.0F, 1.0F, 0.0F};
    /// The light's position. Ignored for `OrthographicPlane`, which has no position.
    Vec3 position{0.0F, 0.0F, 0.0F};

    /// Metres per cookie tile, for `OrthographicPlane`. A 500 m cloud cookie over a 2 km view is
    /// four tiles across.
    f32 world_scale = 100.0F;
    /// The half angle of the cone, in radians, for `ConePerspective`. The cookie's edge lands here.
    f32 cone_half_angle = 0.6F;

    /// A world-space translation applied before projection. Metres. This is how a cloud layer
    /// MOVES; it does not wrap and it is exact at any distance.
    Vec3 world_offset{0.0F, 0.0F, 0.0F};
    /// A UV-space offset applied after projection. Wraps with the texture's addressing mode, which
    /// is how a tiling pattern scrolls without the projection drifting.
    Vec2 scroll_uv{0.0F, 0.0F};

    /// Whether coordinates outside [0, 1] mask the light entirely (a gobo) or repeat (clouds).
    bool tile = false;
};

/// Where to sample the cookie for a world-space point, and whether the sample is inside it.
struct CookieSample {
    Vec2 uv{0.0F, 0.0F};
    /// For `CubeDirection`: the direction to sample the cube map with. Unit length.
    Vec3 direction{0.0F, 0.0F, -1.0F};
    /// False when the point falls outside a non-tiling cookie, or behind a spot's apex. A caller
    /// that ignores this lights the space behind a projector, which is the classic gobo bug.
    bool inside = false;
};

/// The cookie coordinate for one world-space point. A pure function of the projection and the
/// point, which is what lets the three projections be checked against hand-computed answers.
[[nodiscard]] CookieSample cookie_uv(const CookieProjection& projection,
                                     Vec3 world_position) noexcept;

/// Advance a scrolling cookie by `seconds`, in the units its projection uses. Separate from
/// `cookie_uv` because scrolling is a per-frame operation on the light and sampling is a per-pixel
/// one on the point — and because the wrap has to happen once rather than per pixel, or an
/// f32 UV drifts into its own quantisation after an hour of play.
void advance_cookie_scroll(CookieProjection& projection, Vec2 uv_per_second, Vec3 world_per_second,
                           f32 seconds) noexcept;

/// A light function is a cookie plus what it does to the light. `rendering-lighting-and-shadows`
/// names the texture; this is the small amount of policy around it.
struct LightFunction {
    CookieProjection projection;
    /// The texture's chain length, so a distant or grazing sample can pick a filtered level rather
    /// than aliasing. A cookie that aliases is a light that flickers.
    u32 mip_count = 1;
    /// Multiplied into the sampled value. 0 disables the cookie without unbinding it, which is what
    /// a fade does.
    f32 intensity = 1.0F;
    /// What a point outside a non-tiling cookie reads. Zero is a gobo; one is a mask that only
    /// darkens where it is drawn.
    f32 outside_value = 0.0F;
};

/// The modulation this light function applies at a point, given the value already fetched from the
/// texture. Splitting the fetch out is what keeps this file free of a sampler.
[[nodiscard]] f32 apply_light_function(const LightFunction& function, const CookieSample& sample,
                                       f32 sampled_value) noexcept;

/// The mip level a cookie sample should read, from the projected footprint. `footprint_uv` is the
/// UV-space extent one pixel covers, which the caller has from the derivative of `cookie_uv`.
[[nodiscard]] f32 cookie_filter_level(f32 footprint_uv, u32 mip_count) noexcept;

}  // namespace cy::rendering

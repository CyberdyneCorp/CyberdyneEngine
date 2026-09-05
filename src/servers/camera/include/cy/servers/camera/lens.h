#pragma once
// The lens model: one abstraction, two ways of authoring it. Task 4.3.2.
//
// `camera-system` — "Lens model": cameras "SHALL support both a **gameplay lens** — vertical field
// of view, aspect, near and far — and a **physical lens** — focal length, sensor dimensions,
// aperture, focus distance, shutter, and sensitivity — mapping to one lens abstraction", projection
// "SHALL support perspective, orthographic, and a custom projection supplied by a project", and
// "lens blending SHALL respect the lens model in use, so that a physical lens does not interpolate
// as though it were a field-of-view value".
//
// --- WHY THE TWO ARE ONE STRUCT AND NOT TWO TYPES -----------------------------------------------
//
// Because "both SHALL map to the same lens abstraction and blend correctly" is a requirement about
// a *cinematic and a gameplay camera blending into each other*. Two types would need a conversion
// at the blend, and a conversion at a blend is a place where one of the two directions is written
// once and never exercised. One struct carrying a `kind` has one code path, and `vertical_fov()` is
// the single derivation.
//
// --- THE INTERPOLATION RULE, WHICH IS THE POINT OF THE REQUIREMENT ------------------------------
//
// Focal length and field of view are related by an arctangent, so the midpoint of two focal lengths
// is NOT the focal length of the midpoint of two fields of view. An 18 mm and a 50 mm lens on a
// 24 mm sensor subtend 67.38 and 26.99 degrees; half way in focal length is 34 mm — 38.88 degrees
// — while half way in field of view is 47.19 degrees, which is 27.48 mm. An operator racking a
// zoom moves the barrel, so the first is what a physical blend must produce. `blend()` below
// therefore interpolates in the authored quantity of each lens and derives the other, and
// `tests/test_lens.cpp` asserts exactly those numbers so the rule cannot quietly become "lerp the
// field of view, it is close enough".
//
// --- WHAT IS DELIBERATELY ABSENT ----------------------------------------------------------------
//
// No matrix. `camera-system`: "The camera SHALL express projection **semantically**; the renderer
// SHALL construct backend-specific matrices." `to_projection()` produces `cy::render::Projection`,
// whose own `matrix()` is the single place reversed-Z is applied. Nothing here knows what a clip
// space looks like, and nothing here can be handed a jitter offset.

#include <cy/core/base/types.h>
#include <cy/servers/render/model.h>

namespace cy::camera {

/// Which of the two parameter sets is authoritative for this lens.
///
/// Not "which fields are populated" — both are always populated, so a debug view can show the
/// physical equivalent of a gameplay lens. It is which one the author edits and which one a blend
/// interpolates in.
enum class LensKind : u8 {
    Gameplay = 0,
    Physical,
};

[[nodiscard]] const char* lens_kind_name(LensKind kind) noexcept;

/// The gameplay half: what a designer types into a field-of-view box.
///
/// `far_plane` of zero means infinite, exactly as `render::Projection` spells it, so the two do not
/// disagree about what a zeroed struct means.
struct GameplayLens {
    f32 vertical_fov_radians = 1.0471975512F;  // 60 degrees
    f32 near_plane = 0.1F;
    f32 far_plane = 0.0F;
    /// Zero means "take the viewport's aspect", which is the common case and the one that keeps
    /// split-screen correct without anybody setting anything. A non-zero value forces an aspect,
    /// which is what a letterboxed cinematic and a fixed-aspect capture need.
    f32 aspect_override = 0.0F;
};

/// The physical half: what a director of photography would name.
///
/// `sensor_height_mm` is the one the field of view is derived from — the engine's field of view is
/// vertical — and `sensor_width_mm` is carried so that a horizontal framing decision and a depth of
/// field computation have the full sensor rather than half of it.
struct PhysicalLens {
    f32 focal_length_mm = 20.78F;  // ~60 degrees on a 24 mm sensor height
    f32 sensor_width_mm = 36.0F;   // full frame
    f32 sensor_height_mm = 24.0F;
    f32 aperture_f_stop = 2.8F;
    f32 focus_distance_m = 10.0F;
    f32 shutter_speed_s = 1.0F / 60.0F;
    f32 sensitivity_iso = 100.0F;
};

/// The lens, as everything above this module sees it.
struct Lens {
    LensKind kind = LensKind::Gameplay;
    render::ProjectionKind projection = render::ProjectionKind::Perspective;
    GameplayLens gameplay;
    PhysicalLens physical;
    /// Orthographic vertical extent in world units. First class, not a special case: strategy,
    /// two-dimensional and editor views need it and take the same rig and view path.
    f32 ortho_height = 10.0F;

    /// The vertical field of view this lens actually has, whichever way it was authored.
    [[nodiscard]] f32 vertical_fov_radians() const noexcept;

    /// The semantic projection the renderer builds its matrix from. `aspect` is the viewport's; a
    /// non-zero `gameplay.aspect_override` wins.
    [[nodiscard]] render::Projection to_projection() const noexcept;

    /// The aspect this lens imposes, or zero when it imposes none.
    [[nodiscard]] f32 aspect_override() const noexcept { return gameplay.aspect_override; }
};

/// Focal length to vertical field of view, and back. Free functions because a rig node, a
/// diagnostic and a test all want the conversion without a `Lens` to hang it on.
[[nodiscard]] f32 fov_from_focal_length(f32 focal_length_mm, f32 sensor_height_mm) noexcept;
[[nodiscard]] f32 focal_length_from_fov(f32 vertical_fov_radians, f32 sensor_height_mm) noexcept;

/// Blend two lenses, respecting the model each was authored in. See the header comment.
///
/// Two physical lenses interpolate in focal length; anything else interpolates in field of view.
/// The result takes `b`'s kind and projection at `t >= 0.5F`, because a projection kind has no
/// meaningful midpoint — a half-orthographic camera is not a thing — and a blend that silently
/// produced one would be worse than a switch at the halfway point.
[[nodiscard]] Lens blend(const Lens& a, const Lens& b, f32 t) noexcept;

}  // namespace cy::camera

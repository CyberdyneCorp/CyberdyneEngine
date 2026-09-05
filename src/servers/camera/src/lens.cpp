// The lens model: the two authoring forms, the derivation between them, and the blend rule.
// See cy/servers/camera/lens.h.

#include <cy/servers/camera/lens.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::camera {
namespace {

/// A focal length or a sensor height of zero would divide by zero or produce an infinite field of
/// view. Clamped rather than asserted: a lens arrives from an authored asset, and an asset with a
/// zero in it should produce a usable camera and a diagnostic elsewhere, not an abort.
constexpr f32 kMinMillimetres = 0.001F;

using math::lerp;

}  // namespace

const char* lens_kind_name(LensKind kind) noexcept {
    switch (kind) {
        case LensKind::Gameplay:
            return "gameplay";
        case LensKind::Physical:
            return "physical";
    }
    return "unknown";
}

f32 fov_from_focal_length(f32 focal_length_mm, f32 sensor_height_mm) noexcept {
    const f32 focal = math::max(focal_length_mm, kMinMillimetres);
    const f32 sensor = math::max(sensor_height_mm, kMinMillimetres);
    return 2.0F * std::atan(sensor / (2.0F * focal));
}

f32 focal_length_from_fov(f32 vertical_fov_radians, f32 sensor_height_mm) noexcept {
    const f32 sensor = math::max(sensor_height_mm, kMinMillimetres);
    // Clamped away from 0 and pi: both are degenerate lenses and both would divide by a tangent of
    // zero or of infinity.
    const f32 fov = math::clamp(vertical_fov_radians, 0.001F, math::kPi - 0.001F);
    return sensor / (2.0F * std::tan(fov * 0.5F));
}

f32 Lens::vertical_fov_radians() const noexcept {
    if (kind == LensKind::Physical) {
        return fov_from_focal_length(physical.focal_length_mm, physical.sensor_height_mm);
    }
    return gameplay.vertical_fov_radians;
}

render::Projection Lens::to_projection() const noexcept {
    render::Projection semantic;
    semantic.kind = projection;
    semantic.fov_y_radians = vertical_fov_radians();
    semantic.ortho_height = ortho_height;
    semantic.near_plane = gameplay.near_plane;
    semantic.far_plane = gameplay.far_plane;
    return semantic;
}

Lens blend(const Lens& a, const Lens& b, f32 t) noexcept {
    const f32 alpha = math::clamp(t, 0.0F, 1.0F);
    // The kind and the projection switch at the halfway point rather than interpolating: neither
    // has a midpoint, and a "half orthographic" projection is not a thing a renderer can build. See
    // the header comment.
    Lens result = (alpha >= 0.5F) ? b : a;

    // TWO PHYSICAL LENSES INTERPOLATE IN FOCAL LENGTH. This is the whole requirement: "a physical
    // lens does not interpolate as though it were a field-of-view value".
    if (a.kind == LensKind::Physical && b.kind == LensKind::Physical) {
        result.physical.focal_length_mm =
            lerp(a.physical.focal_length_mm, b.physical.focal_length_mm, alpha);
        result.physical.sensor_width_mm =
            lerp(a.physical.sensor_width_mm, b.physical.sensor_width_mm, alpha);
        result.physical.sensor_height_mm =
            lerp(a.physical.sensor_height_mm, b.physical.sensor_height_mm, alpha);
        result.physical.aperture_f_stop =
            lerp(a.physical.aperture_f_stop, b.physical.aperture_f_stop, alpha);
        result.physical.focus_distance_m =
            lerp(a.physical.focus_distance_m, b.physical.focus_distance_m, alpha);
        result.physical.shutter_speed_s =
            lerp(a.physical.shutter_speed_s, b.physical.shutter_speed_s, alpha);
        result.physical.sensitivity_iso =
            lerp(a.physical.sensitivity_iso, b.physical.sensitivity_iso, alpha);
        // The gameplay half is kept in step so a consumer reading it never sees a stale field of
        // view for a lens that is being racked.
        result.gameplay.vertical_fov_radians = fov_from_focal_length(
            result.physical.focal_length_mm, result.physical.sensor_height_mm);
    } else {
        // Anything else — two gameplay lenses, or one of each — interpolates in field of view,
        // which is the quantity both of them can express. Blending a gameplay lens into a physical
        // one in focal length would mean inventing a sensor for the gameplay one.
        result.gameplay.vertical_fov_radians =
            lerp(a.vertical_fov_radians(), b.vertical_fov_radians(), alpha);
        result.physical.focal_length_mm = focal_length_from_fov(
            result.gameplay.vertical_fov_radians, result.physical.sensor_height_mm);
    }

    result.gameplay.near_plane = lerp(a.gameplay.near_plane, b.gameplay.near_plane, alpha);
    // An infinite far plane is spelled as zero, so lerping it toward a finite one would walk the
    // far plane out from the near plane instead of in from infinity. Whichever side is infinite
    // wins: a blend between an infinite and a finite far plane stays infinite until the switch.
    result.gameplay.far_plane = (a.gameplay.far_plane == 0.0F || b.gameplay.far_plane == 0.0F)
                                    ? 0.0F
                                    : lerp(a.gameplay.far_plane, b.gameplay.far_plane, alpha);
    result.gameplay.aspect_override =
        lerp(a.gameplay.aspect_override, b.gameplay.aspect_override, alpha);
    result.ortho_height = lerp(a.ortho_height, b.ortho_height, alpha);
    return result;
}

}  // namespace cy::camera

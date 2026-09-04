#pragma once
// Physical light units, colour temperature and camera exposure. Task 4.4.1.
//
// `rendering-lighting-and-shadows` — "Physical light units": "Light intensity SHALL be expressible
// in **physical units**: lux for directional lights, lumens for point and spot lights, and nits
// (cd/m²) for area lights. Physical intensity SHALL be combined with the camera's exposure
// (aperture, shutter speed, ISO) so a physically-configured scene is correctly exposed without
// arbitrary multipliers."
//
// ================================================================================================
// WHAT "PHYSICAL UNITS" ACTUALLY MEANS HERE, BECAUSE IT IS NOT A SCALE FACTOR
// ================================================================================================
//
// It is not "multiply every light by 683 and call it physical". It is that THREE separate
// quantities stop being the same number:
//
//   * what an author types           — a photometric quantity in the unit its light type is sold
//   in:
//                                      lux for the sun, lumens for a bulb, nits for a panel;
//   * what the shader integrates     — luminous intensity in candela (lm/sr) for a punctual light,
//                                      or luminance in nits for an area light, because those are
//                                      what the reflectance equation is written in;
//   * what reaches the display       — the integrated luminance divided by the camera's exposure,
//                                      which is a photographic quantity derived from aperture,
//                                      shutter speed and sensitivity and from nothing else.
//
// A 1000-lumen bulb and a 100 000-lux sun then coexist without either being tuned, because the
// conversions below are the real ones and the exposure is a real camera's. The scenario the
// specification writes — "100 000 lux sunlight and an EV-based camera SHALL match real-world
// expectations without per-light tuning" — is exactly the claim that these three are separate.
//
// ================================================================================================
// THE CONVERSION THAT IS A CHOICE, AND IT IS WRITTEN DOWN
// ================================================================================================
//
// A point light's lumens spread over the whole sphere: candela = lumens / 4π. Nobody disagrees.
//
// A SPOT LIGHT'S DOES NOT. Two conventions are in use, and they differ by a factor that grows as
// the cone narrows:
//
//   * the physical one — the flux is confined to the cone, so candela = lumens / (2π(1 − cos θ));
//   * the photometric one, used by Frostbite and by Filament — candela = lumens / π, INDEPENDENT of
//     the cone angle, so that narrowing a spot's cone does not brighten what is left inside it.
//
// This engine uses the SECOND, and the reason is authoring rather than physics: an artist narrowing
// a beam expects the lit region to shrink, not to get brighter, and a physically-confined spot does
// both. `spot_candela_physical()` is provided beside it so a project that wants the other one has
// it by name rather than by patching this file, and so the difference is visible rather than
// implicit.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/servers/render/model.h>

#include <numbers>

namespace cy::rendering {

/// The unit an authored intensity is expressed in. Carried with the value, because "intensity 5"
/// means five different things across these five.
enum class PhotometricUnit : u8 {
    /// Illuminance, lm/m². Directional lights.
    Lux = 0,
    /// Luminous flux. Point and spot lights, as they are sold.
    Lumen,
    /// Luminous intensity, lm/sr. What a punctual light's shading actually integrates.
    Candela,
    /// Luminance, cd/m². Area lights and emissive surfaces.
    Nit,
    /// `rendering-lighting-and-shadows`: "A non-physical **arbitrary units** mode SHALL remain
    /// available for stylised projects." Passed through untouched.
    Arbitrary,
    Count,
};

[[nodiscard]] const char* photometric_unit_name(PhotometricUnit unit) noexcept;

/// Which unit a light kind's authored intensity is in by default.
[[nodiscard]] PhotometricUnit default_unit_for(render::LightKind kind) noexcept;

// --- Conversions -------------------------------------------------------------------------------

/// The engine's own spelling of pi at `f32`, so a conversion here and a conversion in a test do
/// not differ in their last bit.
inline constexpr f32 kPi = std::numbers::pi_v<f32>;

/// A point light's luminous intensity, in candela, from its flux in lumens.
[[nodiscard]] f32 point_candela_from_lumens(f32 lumens) noexcept;

/// A spot light's luminous intensity, in candela, from its flux in lumens — the PHOTOMETRIC
/// convention, independent of the cone angle. See the header comment for why this one.
[[nodiscard]] f32 spot_candela_from_lumens(f32 lumens) noexcept;

/// The physically-confined alternative, for a project that wants it. Narrowing the cone brightens
/// what remains, which is what the physics says and what most artists do not expect.
[[nodiscard]] f32 spot_candela_physical(f32 lumens, f32 outer_cone_radians) noexcept;

/// An area light's luminance, in nits, from the flux it emits over its area. Lambertian: the flux
/// spreads over π steradians of projected solid angle, not 2π.
[[nodiscard]] f32 area_nits_from_lumens(f32 lumens, f32 area_square_metres) noexcept;

/// Convert an authored value into the quantity the shader integrates for a light of this kind:
/// candela for point and spot, lux for directional, nits for area. `Arbitrary` passes through.
///
/// This is the ONE function the renderer calls. Everything above it exists so that this one has no
/// decisions left in it.
[[nodiscard]] f32 to_shading_intensity(render::LightKind kind, PhotometricUnit unit, f32 value,
                                       f32 outer_cone_radians, f32 area_square_metres) noexcept;

// --- Plausibility ------------------------------------------------------------------------------

/// The range a physically-configured light of this kind is expected to fall in, in its shading
/// unit. Not a clamp: a value outside it is reported, never corrected.
struct IntensityRange {
    f32 low = 0.0F;
    f32 high = 0.0F;
};

[[nodiscard]] IntensityRange plausible_range(render::LightKind kind) noexcept;

/// `rendering-lighting-and-shadows`: "WHEN a project switches from arbitrary to physical units THEN
/// the engine SHALL provide a documented conversion and flag lights whose values are implausible."
/// This is the flag half; `arbitrary_to_physical()` is the documented conversion.
[[nodiscard]] bool intensity_is_plausible(render::LightKind kind, f32 shading_intensity) noexcept;

/// THE DOCUMENTED CONVERSION. An arbitrary-units project's intensities are a linear scale whose 1.0
/// was chosen to look right at that project's exposure; the conversion maps 1.0 to the middle of
/// the kind's plausible range, geometrically rather than arithmetically because the range spans
/// orders of magnitude. It is a starting point that leaves a scene lit rather than black or blown
/// out, and it is deliberately reversible: `physical_to_arbitrary(arbitrary_to_physical(x)) == x`.
[[nodiscard]] f32 arbitrary_to_physical(render::LightKind kind, f32 arbitrary) noexcept;
[[nodiscard]] f32 physical_to_arbitrary(render::LightKind kind, f32 shading_intensity) noexcept;

// --- Colour temperature ------------------------------------------------------------------------

/// The linear Rec. 709 tint of a black body at `kelvin`, normalised to unit luminance.
///
/// "WHEN a light specifies 3200 K THEN the engine SHALL convert it to a linear RGB tint using a
/// black-body approximation, multiplied by the light's colour." Normalising to unit luminance is
/// what makes it a TINT rather than a second intensity: changing a light's temperature changes its
/// colour and leaves its brightness alone, which is what an artist means by the control.
///
/// The Planckian locus is approximated by Kim et al.'s cubics in 1/T, valid over 1667–25000 K and
/// clamped outside it. Below about 1667 K a black body is barely visible anyway, and above 25000 K
/// the locus has flattened.
[[nodiscard]] Vec3 blackbody_color(f32 kelvin) noexcept;

// --- Exposure ----------------------------------------------------------------------------------

/// A physical camera. `rendering-lighting-and-shadows` requires exposure to be derived from these
/// three and not from a slider, which is what makes a physically-lit scene expose correctly.
struct CameraExposure {
    /// f-number. f/1.4 is a fast prime; f/16 is bright daylight.
    f32 aperture = 4.0F;
    /// Seconds. 1/125 s is a hand-held default.
    f32 shutter_seconds = 1.0F / 125.0F;
    /// ISO sensitivity.
    f32 sensitivity = 100.0F;
    /// Stops added on top, for artistic control. The one non-physical knob, and it is additive in
    /// EV so it composes with the three above rather than fighting them.
    f32 compensation = 0.0F;
};

/// EV100 for a camera: `log2(N² / t) − log2(S / 100) − compensation`.
///
/// Compensation is SUBTRACTED, so positive compensation lowers the exposure value and therefore
/// brightens the image — the sign photographers use.
[[nodiscard]] f32 exposure_value(const CameraExposure& camera) noexcept;

/// The multiplier applied to luminance before tonemapping, from an exposure value.
///
/// `1 / (1.2 · 2^EV100)`. The 1.2 is the ISO 12232 saturation-based constant with the standard
/// 0.65 lens factor folded in — the same number Lagarde and de Rousiers publish — and it is what
/// makes an 18% grey card under 100 000 lux land where a photographer expects it.
[[nodiscard]] f32 exposure_multiplier(f32 exposure_value) noexcept;

/// The composition of the two, which is what a view actually needs.
[[nodiscard]] f32 exposure_multiplier(const CameraExposure& camera) noexcept;

/// The exposure value that renders a surface of `luminance` nits as mid-grey. What automatic
/// exposure converges to, and the inverse of `exposure_multiplier` in the sense that matters.
[[nodiscard]] f32 exposure_value_for_luminance(f32 average_luminance) noexcept;

}  // namespace cy::rendering

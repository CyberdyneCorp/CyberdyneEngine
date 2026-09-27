// SPDX-License-Identifier: MIT
#pragma once
// The participating medium volumetric fog is filled with: a height fog and a handful of authored
// fog volumes, and what one point of it does to light.
//
// `rendering-post-processing` — "Volumetric fog": "density injection (global fog plus fog volumes)"
// and "Fog volumes SHALL be authorable as boxes, spheres, cones, cylinders, and via a density
// function, each with an emissive and albedo colour and an anisotropy (Henyey-Greenstein `g`)
// parameter."
//
// ================================================================================================
// THE QUANTITIES ARE THE RADIATIVE-TRANSFER ONES, AND ONLY THOSE
// ================================================================================================
//
// A medium is an EXTINCTION coefficient sigma_t in 1/m, a single-scattering ALBEDO rho (so the
// scattering coefficient is sigma_s = rho sigma_t per channel), a PHASE FUNCTION — Henyey-Greenstein
// with its `g` — and an EMISSION in radiance per metre. There is no fog colour and no fog start and
// end distance anywhere here: the colour a fog takes is the light that reaches it times its albedo,
// and how far one sees through it is `extinction_for_visibility`'s Koschmieder relation.
//
// Two media at one point add their coefficients: sigma_t and sigma_s sum, and the phase function of
// the mixture is the sigma_s-weighted mean of the two. `MediumSample::sun_scattering` is that mean
// already multiplied through, sum over media of sigma_s,m * p_m(theta), which is why the sample
// takes the scattering angle: a mixture has no single `g`.
//
// ================================================================================================
// THE HEIGHT FOG
// ================================================================================================
//
//   sigma_t(h) = sigma_t0                             h <= base
//   sigma_t(h) = sigma_t0 * exp(-(h - base) / H)      h >  base
//
// The exponential is the isothermal atmosphere's own profile — the same law the sky's Rayleigh and
// Mie layers follow, with an aerosol scale height of kilometres there and of tens of metres for a
// ground fog. `base` is the altitude the fog is densest at, which for valley fog is the valley
// floor; below it the density stops growing rather than running off to infinity under the terrain.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>

namespace cy::rendering::fog {

/// The largest number of authored fog volumes one frame fills. A constant rather than a growable
/// list because the device reads them out of a fixed block: an unbounded list is an unbounded loop
/// per froxel sub-step.
inline constexpr u32 kMaxFogVolumes = 8;

/// The contrast threshold of Koschmieder's law, 2 %: the meteorological visibility V of a
/// homogeneous medium is where a black object's contrast against the horizon falls to it, so
/// `sigma_t = ln(1 / 0.02) / V = 3.912 / V`.
inline constexpr f32 kKoschmiederContrast = 0.02F;

/// The extinction coefficient, in 1/m, of a homogeneous medium in which one sees `metres` far —
/// the World Meteorological Organization's meteorological optical range. How a scene states a fog
/// density in a unit a person can check against a weather report.
[[nodiscard]] f32 extinction_for_visibility(f32 metres) noexcept;

/// The global fog. Zero extinction, the default, is no fog at all.
struct HeightFog {
    /// sigma_t at and below `base_height`, 1/m.
    f32 extinction = 0.0F;
    /// World altitude, metres, where the fog is densest.
    f32 base_height = 0.0F;
    /// The exponential's scale height, metres. Must be positive.
    f32 scale_height = 50.0F;
    /// Single-scattering albedo per channel. Fog and haze droplets absorb almost nothing in the
    /// visible, so the default is close to one.
    Vec3 albedo{0.95F, 0.95F, 0.95F};
    /// Henyey-Greenstein `g`. Water and haze droplets scatter strongly forward; 0.6 is within the
    /// range the Mie theory gives for fog droplet sizes.
    f32 anisotropy = 0.6F;
};

/// The authorable shapes. The density function is `HeightFog`; an arbitrary user function is not
/// something a fixed device block can carry.
enum class FogVolumeShape : u8 {
    /// `size` is the half extents along the world axes.
    Box = 0,
    /// `size.x` is the radius.
    Sphere,
    /// Vertical: `size.x` the radius, `size.y` the half height.
    Cylinder,
    /// Vertical, apex up: `centre` is the base's centre, `size.x` the base radius, `size.y` the
    /// height.
    Cone,
};

/// One authored volume: a shape with a constant density inside, falling to zero over `edge` metres
/// at its boundary so that its outline does not alias across the froxel grid.
struct FogVolume {
    FogVolumeShape shape = FogVolumeShape::Box;
    /// World position, metres.
    Vec3 centre{0.0F, 0.0F, 0.0F};
    Vec3 size{1.0F, 1.0F, 1.0F};
    /// Metres over which the density ramps from the boundary inward. Zero is a hard edge.
    f32 edge = 0.5F;
    /// sigma_t inside, 1/m.
    f32 extinction = 0.0F;
    Vec3 albedo{0.9F, 0.9F, 0.9F};
    /// Emitted radiance per metre of path — sigma_a times the medium's emitted radiance. A glowing
    /// smoke or a lit dust cloud; zero for fog.
    Vec3 emission{0.0F, 0.0F, 0.0F};
    f32 anisotropy = 0.0F;
};

/// Everything the fog is filled with this frame.
struct FogMedium {
    HeightFog height;
    FogVolume volumes[kMaxFogVolumes] = {};
    u32 volume_count = 0;
};

/// What one point of the medium does, for one scattering angle.
struct MediumSample {
    /// sigma_t, 1/m. Grey: every medium here extinguishes all wavelengths alike.
    f32 extinction = 0.0F;
    /// sigma_s per channel: what an ISOTROPIC incident radiance is scattered by. The ambient term
    /// multiplies this, because a phase function integrates to one over the sphere.
    Vec3 scattering{0.0F, 0.0F, 0.0F};
    /// Sum over media of sigma_s,m * p_m(theta), 1/(m sr): what the sun's illuminance is scattered
    /// toward the eye by.
    Vec3 sun_scattering{0.0F, 0.0F, 0.0F};
    /// Emitted radiance per metre.
    Vec3 emission{0.0F, 0.0F, 0.0F};
};

/// The height fog's extinction at a world altitude.
[[nodiscard]] f32 height_fog_extinction(const HeightFog& fog, f32 altitude) noexcept;

/// How much of a volume's density is present at a world point: 1 inside by more than `edge`, 0
/// outside, linear in the distance to the boundary between.
[[nodiscard]] f32 fog_volume_coverage(const FogVolume& volume, Vec3 world) noexcept;

/// The medium at a world point. `cos_to_sun` is the cosine between the direction from the eye to
/// the point and the direction to the sun — the Henyey-Greenstein argument, positive looking toward
/// the light.
[[nodiscard]] MediumSample sample_medium(const FogMedium& medium, Vec3 world,
                                         f32 cos_to_sun) noexcept;

/// Whether the medium holds anything at all. A frame whose medium is empty fills a volume that
/// leaves every surface exactly as it was lit.
[[nodiscard]] bool medium_is_empty(const FogMedium& medium) noexcept;

}  // namespace cy::rendering::fog

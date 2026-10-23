// SPDX-License-Identifier: MIT
#pragma once
// An area light of the frame, shaded: the C++ twin of `cy/light.slang`'s `shadeAreaLight`.
//
// `rendering-lighting-and-shadows` — "Area light evaluation" and "Light types".
//
// ================================================================================================
// WHAT THIS FILE IS FOR, BESIDE area.h
// ================================================================================================
//
// area.h evaluates an `AreaLight` stated in a shading frame. A frame carries `GpuLight` records,
// camera-relative, 64 bytes each (lights.h), and a shader reads those. This file is the join: the
// record unpacked into an `AreaLight` about one shading point (`area_light_from_record`), and the
// whole of what the forward pass adds for it (`shade_area_light`) — diffuse, specular with its
// Fresnel, the two-sided rule and the window that ends the light at its reach — in the order the
// shader does it. `render.area_lights` holds every lit pixel of a device frame to this function,
// and `integration.render_area_lights` holds this function to an integration of the same BRDF over
// the emitter's own surface.
//
// ================================================================================================
// THE FRESNEL IS THE LIGHT'S, NOT THE HEMISPHERE'S
// ================================================================================================
//
// The published LTC shading takes Fresnel as a split sum: `F0 (magnitude - fresnel) + fresnel`,
// both terms integrals over the WHOLE hemisphere. That averages Schlick's `(1 - v.h)^5` over
// grazing directions the light does not cover, and for a dielectric — F0 of 0.04, where that term
// is most of the answer — it put a rect light's highlight 20 to 37% above the lobe integrated over
// the emitter with its own Fresnel (`integration.render_area_lights`, measured). So the frame takes
// Fresnel where the lobe GATHERS this light from: the polygon's vector form factor in the cosine's
// space, carried back through the LTC's forward transform, is the mean direction the BRDF sees the
// emitter in, and Schlick is evaluated at its half vector. `magnitude`, the lobe's energy with a
// Fresnel of one, multiplies the integral as before.
//
// ================================================================================================
// THE WINDOW, AND WHY AN AREA LIGHT HAS ONE
// ================================================================================================
//
// A light's cluster lists end at its reach (`light_bounding_radius`), and a light that is still
// contributing at the boundary of a cluster it was not assigned to pops. A punctual light is
// windowed to zero at its range by `distanceAttenuation`'s smooth step and an area light takes the
// same window on the distance to its centre: `(1 - (d^2 / R^2)^2)^2`. With no range authored the
// reach is where the far-field illuminance is `kAreaLightReachIlluminance`, so the window cuts
// nothing a viewer could see.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/rendering/lighting/area.h>
#include <cy/rendering/lighting/lights.h>

namespace cy::rendering {

/// The record as an `AreaLight` about the shading point at `position`, camera-relative like the
/// record. A sphere faces the point (area.h does that); a tube's quad is turned about its axis to
/// face it, which is what makes its silhouette the rectangle a cylinder shows from the side.
[[nodiscard]] AreaLight area_light_from_record(const GpuLight& light, Vec3 position) noexcept;

/// The window on the distance from `position` to the light's centre that ends it at its reach. 1
/// well inside the reach, 0 beyond it.
[[nodiscard]] f32 area_light_window(const GpuLight& light, Vec3 position) noexcept;

/// What the forward pass adds for one area light: outgoing radiance, already multiplied by the
/// light's colour, its luminance and the window.
struct AreaShading {
    Vec3 diffuse{0.0F, 0.0F, 0.0F};
    Vec3 specular{0.0F, 0.0F, 0.0F};
    /// The form factor the diffuse term used: the cosine-weighted fraction of the hemisphere the
    /// emitter covers, over pi. Exposed for the stochastic path, whose target function it is.
    f32 form_factor = 0.0F;
};

/// The surface the light lands on, in the material model `cy/material.slang` shades.
struct AreaSurface {
    Vec3 albedo{0.5F, 0.5F, 0.5F};
    f32 roughness = 0.5F;
    f32 metallic = 0.0F;
};

/// Shade `light` at `position` with unit `normal` and unit `view` (towards the eye). `table` is
/// the frame's LTC table; an unbuilt one shades the diffuse term only, which is what a frame with
/// no table bound draws.
[[nodiscard]] AreaShading shade_area_light(const LtcTable& table, const GpuLight& light,
                                           Vec3 position, Vec3 normal, Vec3 view,
                                           const AreaSurface& surface) noexcept;

/// The unshadowed irradiance, in lux, a surface at `position` facing `normal` receives from any
/// local light of the frame — `E = L * pi * form factor` for an area light, the windowed
/// inverse-square law and the cone for a punctual one. Zero for a directional light. The target
/// function the stochastic path resamples towards, and the brute-force sum its estimate is held to.
[[nodiscard]] Vec3 light_irradiance(const GpuLight& light, Vec3 position, Vec3 normal) noexcept;

/// A point on an area light's surface for two uniform numbers in [0, 1): what a visibility ray is
/// traced to, so a shadow's penumbra is the emitter's own. A punctual light answers its position.
[[nodiscard]] Vec3 sample_light_point(const GpuLight& light, Vec3 towards, f32 u, f32 v) noexcept;

}  // namespace cy::rendering

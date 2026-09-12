#pragma once
// The surface closure's parameters, the water column's colour, underwater, and the caustic tiers.
// M10 task 2.3.
//
// `water` — "Water surface shading": water is shaded "through a WATER SURFACE CLOSURE in the
// material system, not forced through the opaque metallic-roughness model", accounting for
// "reflection, refraction, wavelength-dependent absorption and scattering through the water column,
// surface roughness, normals, and foam coverage", with "colour with depth [following]
// physically-inspired attenuation over the water column thickness, so deep water darkens and shifts
// hue without hand-authored gradients".
//
// ================================================================================================
// WHAT THIS FILE IS, AND WHAT IT DELIBERATELY IS NOT
// ================================================================================================
//
// It is the PARAMETERS of the closure and the arithmetic that is physical rather than artistic:
// Beer-Lambert transmittance over a column, the in-scattered term, the reflection escalation the
// illumination hierarchy takes, the caustic tier a profile selects, and the explicit crossing of
// the surface. Every one of those is a number a test can check.
//
// It is NOT a shader and not a material. `material-compiler` owns the closure's IR and
// `rendering-materials-and-shading` owns how it is evaluated on a device; a water module that
// emitted shader code would be a second material system, which `water`'s own requirement forbids in
// as many words. `WaterSurfaceClosure` is what a material's water node is FED — and a renderer-side
// row binds it. That boundary is why this module links no renderer, and the gap it leaves is
// recorded rather than hidden.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/water/body.h>

namespace cy::water {

/// Transmittance through `thickness` metres of a body's water, per channel.
///
/// Beer-Lambert over absorption plus scattering: T = exp(-(a + b) d). This is the whole of "deep
/// water darkens and shifts hue without hand-authored gradients" — the hue shift is the three
/// channels having three exponents, and nothing in it was painted.
[[nodiscard]] Vec3 water_transmittance(const WaterOptics& optics, f32 thickness_metres) noexcept;

/// The light scattered back out of a column of `thickness` metres, per channel. The complement of
/// the transmittance, tinted by the body's own scatter colour — which is why a silty lake is pale
/// and a clear sea is dark at the same depth.
[[nodiscard]] Vec3 water_in_scatter(const WaterOptics& optics, f32 thickness_metres) noexcept;

/// The colour a background of `background` takes on when seen through `thickness` metres of this
/// body's water. Transmittance times background, plus the in-scattered term.
[[nodiscard]] Vec3 water_column_colour(const WaterOptics& optics, f32 thickness_metres,
                                       const Vec3& background) noexcept;

/// Everything the material system's water closure needs for one shading point.
///
/// A structure of values with no methods, because it crosses into a system this module does not
/// link: what is on the other side is a material node's inputs, and a type with behaviour here
/// would be behaviour that has to exist on both sides of that boundary.
struct WaterSurfaceClosure {
    Vec3 normal{0.0F, 1.0F, 0.0F};
    /// Roughness after foam: foam is a rough, bright, opaque surface and the closure's own
    /// roughness has to reflect that or a wake reads as polished plastic.
    f32 roughness = 0.02F;
    /// Foam coverage in [0, 1] at this point.
    f32 foam = 0.0F;
    /// The water column between this point and the bed, metres. What absorption integrates over.
    f32 column_thickness = 0.0F;
    Vec3 absorption{0.0F, 0.0F, 0.0F};
    Vec3 scattering{0.0F, 0.0F, 0.0F};
    Vec3 scatter_colour{0.0F, 0.0F, 0.0F};
    f32 refractive_index = 1.333F;
    /// The Fresnel reflectance at normal incidence, derived from the index. Carried rather than
    /// recomputed so that the CPU and the shader cannot disagree about it.
    f32 f0 = 0.02F;
};

/// Build the closure for a shading point of a body.
[[nodiscard]] WaterSurfaceClosure build_closure(const WaterOptics& optics, const Vec3& normal,
                                                f32 foam_coverage, f32 column_thickness) noexcept;

// --- Reflections ---------------------------------------------------------------------------

/// Where a reflection comes from. `water` — "Reflections SHALL use the illumination hierarchy:
/// screen tracing first, escalating to world or hardware tracing by confidence and roughness, with
/// DISTANT ROUGH WATER RESOLVED FROM CACHED RADIANCE."
///
/// Named here rather than in the renderer because the DECISION is water's — it depends on the
/// surface's roughness and the viewer's distance, both of which this module knows — while the
/// execution is the renderer's.
enum class ReflectionSource : u8 { ScreenTrace = 0, WorldTrace, HardwareTrace, CachedRadiance };

[[nodiscard]] const char* reflection_source_name(ReflectionSource source) noexcept;

/// What the renderer offers.
struct ReflectionBudget {
    bool screen_tracing = true;
    bool world_tracing = true;
    bool hardware_tracing = false;
    /// Beyond this distance, in metres, water resolves from cached radiance whatever its roughness.
    f32 trace_distance_metres = 250.0F;
    /// Above this roughness a trace is not worth its rays: a rough surface's reflection is an
    /// average of a wide lobe, which is what a cached probe already stores.
    f32 rough_threshold = 0.25F;
};

[[nodiscard]] ReflectionSource select_reflection(const ReflectionBudget& budget, f32 roughness,
                                                 f32 distance_metres) noexcept;

// --- Underwater ---------------------------------------------------------------------------

/// The renderer profile's tier, as far as water's own decisions need it.
enum class RendererTier : u8 { Low = 0, Standard, High };

/// `water` — "Caustics": "a projected animated approximation, a surface-derived approximation
/// computed from the water surface, and a traced solution where ray tracing is available. The tier
/// SHALL be selected by renderer profile and by the GI budget allocation, and the SURFACE-DERIVED
/// tier SHALL BE THE DEFAULT for real-time use."
enum class CausticTier : u8 { Projected = 0, SurfaceDerived, Traced };

[[nodiscard]] const char* caustic_tier_name(CausticTier tier) noexcept;

/// Select the caustic tier. `gi_budget` is the fraction of the illumination budget allocated to
/// water, in [0, 1].
[[nodiscard]] CausticTier select_caustic_tier(RendererTier tier, f32 gi_budget,
                                              bool ray_tracing_available) noexcept;

/// What the camera is doing with respect to the surface, and what underwater rendering needs.
///
/// `water` — "Underwater rendering": the engine applies "THAT BODY'S PARAMETERS ... rather than a
/// fixed fullscreen tint", and "The transition across the surface SHALL be handled EXPLICITLY,
/// including a camera intersecting the surface, so that the boundary is not a hard switch."
struct UnderwaterState {
    bool camera_submerged = false;
    /// How much of the view is underwater, in [0, 1]. Zero is fully above, one is fully below, and
    /// anything between is the camera intersecting the surface — the case the specification names
    /// and the reason this is a fraction rather than a boolean.
    f32 submerged_fraction = 0.0F;
    /// How deep the camera is, metres. Negative above the surface.
    f32 depth_metres = 0.0F;
    /// The fog colour and extinction the volumetric system is given for this body at this depth.
    Vec3 fog_colour{0.0F, 0.0F, 0.0F};
    Vec3 extinction{0.0F, 0.0F, 0.0F};
    /// Caustic strength at this depth, in [0, 1]: caustics weaken with depth because the light
    /// focusing them has been attenuated on the way down.
    f32 caustic_strength = 0.0F;
};

/// Derive the underwater state. `camera_radius` is the near-plane's half height in metres — the
/// distance over which a camera straddles the surface — and it is what turns a hard switch into the
/// declared crossing.
[[nodiscard]] UnderwaterState underwater_state(const WaterOptics& optics, f64 surface_height,
                                               f64 camera_height, f32 camera_radius) noexcept;

}  // namespace cy::water

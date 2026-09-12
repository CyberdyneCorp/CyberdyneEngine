// The water column's colour, the reflection escalation, the caustic tiers and the surface crossing.
// M10 task 2.3.

#include <cy/water/shading.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::water {

namespace {

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    return (value < 0.0F) ? 0.0F : ((value > 1.0F) ? 1.0F : value);
}

[[nodiscard]] Vec3 exponential(const Vec3& coefficient, f32 thickness) noexcept {
    return Vec3{std::exp(-coefficient.x * thickness), std::exp(-coefficient.y * thickness),
                std::exp(-coefficient.z * thickness)};
}

}  // namespace

Vec3 water_transmittance(const WaterOptics& optics, f32 thickness_metres) noexcept {
    const f32 thickness = (thickness_metres < 0.0F) ? 0.0F : thickness_metres;
    // Beer-Lambert over absorption PLUS scattering: light removed from the straight path is gone
    // from it whether it was absorbed or turned aside, and a transmittance that counted only
    // absorption would make a silty lake as clear as a clean one.
    return exponential(optics.absorption + optics.scattering, thickness);
}

Vec3 water_in_scatter(const WaterOptics& optics, f32 thickness_metres) noexcept {
    const Vec3 transmittance = water_transmittance(optics, thickness_metres);
    const Vec3 lost{1.0F - transmittance.x, 1.0F - transmittance.y, 1.0F - transmittance.z};
    // The fraction of the extinction that is scattering rather than absorption decides how much of
    // what was removed comes back out toward the viewer. A single-scattering approximation, which
    // is what the specification's "physically-inspired" asks for and all a closure's input needs.
    const Vec3 total = optics.absorption + optics.scattering;
    const auto ratio = [](f32 scatter, f32 extinction) noexcept {
        return (extinction > 1e-6F) ? (scatter / extinction) : 0.0F;
    };
    const Vec3 albedo{ratio(optics.scattering.x, total.x), ratio(optics.scattering.y, total.y),
                      ratio(optics.scattering.z, total.z)};
    return cwise_mul(cwise_mul(lost, albedo), optics.scatter_colour);
}

Vec3 water_column_colour(const WaterOptics& optics, f32 thickness_metres,
                         const Vec3& background) noexcept {
    return cwise_mul(water_transmittance(optics, thickness_metres), background) +
           water_in_scatter(optics, thickness_metres);
}

WaterSurfaceClosure build_closure(const WaterOptics& optics, const Vec3& normal, f32 foam_coverage,
                                  f32 column_thickness) noexcept {
    WaterSurfaceClosure closure;
    closure.normal = normal;
    closure.foam = clamp01(foam_coverage);
    // Foam is rough, and a wake that kept the water's own roughness reads as polished plastic. The
    // 0.6 is the roughness of a broken, air-filled surface; the blend is linear in coverage.
    closure.roughness = math::lerp(optics.roughness, 0.6F, closure.foam);
    closure.column_thickness = (column_thickness < 0.0F) ? 0.0F : column_thickness;
    closure.absorption = optics.absorption;
    closure.scattering = optics.scattering;
    closure.scatter_colour = optics.scatter_colour;
    closure.refractive_index = optics.refractive_index;
    // Schlick's F0 for a dielectric against air. Carried on the closure rather than recomputed on
    // the device, so the CPU and the shader cannot disagree about the reflectance of water.
    const f32 step = (optics.refractive_index - 1.0F) / (optics.refractive_index + 1.0F);
    closure.f0 = step * step;
    return closure;
}

const char* reflection_source_name(ReflectionSource source) noexcept {
    switch (source) {
        case ReflectionSource::ScreenTrace:
            return "screen-trace";
        case ReflectionSource::WorldTrace:
            return "world-trace";
        case ReflectionSource::HardwareTrace:
            return "hardware-trace";
        case ReflectionSource::CachedRadiance:
            return "cached-radiance";
    }
    return "unknown";
}

ReflectionSource select_reflection(const ReflectionBudget& budget, f32 roughness,
                                   f32 distance_metres) noexcept {
    // "Distant rough water resolved from cached radiance" is checked FIRST, because it is the case
    // the specification names and because a rough surface's reflection is an average of a wide lobe
    // — which is what a cached probe already holds. Tracing it spends rays to reproduce a blur.
    if (distance_metres > budget.trace_distance_metres || roughness > budget.rough_threshold) {
        return ReflectionSource::CachedRadiance;
    }
    // Otherwise the illumination hierarchy, cheapest first, escalating only where the cheaper tier
    // is not available — screen tracing, then world, then hardware.
    if (budget.screen_tracing) {
        return ReflectionSource::ScreenTrace;
    }
    if (budget.world_tracing) {
        return ReflectionSource::WorldTrace;
    }
    if (budget.hardware_tracing) {
        return ReflectionSource::HardwareTrace;
    }
    return ReflectionSource::CachedRadiance;
}

const char* caustic_tier_name(CausticTier tier) noexcept {
    switch (tier) {
        case CausticTier::Projected:
            return "projected";
        case CausticTier::SurfaceDerived:
            return "surface-derived";
        case CausticTier::Traced:
            return "traced";
    }
    return "unknown";
}

CausticTier select_caustic_tier(RendererTier tier, f32 gi_budget,
                                bool ray_tracing_available) noexcept {
    if (tier == RendererTier::Low || gi_budget < 0.1F) {
        // The projected approximation is a looping texture and does not follow the surface. It is
        // the tier for a profile that cannot afford to derive one, and nothing else.
        return CausticTier::Projected;
    }
    if (ray_tracing_available && tier == RendererTier::High && gi_budget >= 0.5F) {
        return CausticTier::Traced;
    }
    // "The surface-derived tier SHALL BE THE DEFAULT for real-time use", in as many words.
    return CausticTier::SurfaceDerived;
}

UnderwaterState underwater_state(const WaterOptics& optics, f64 surface_height, f64 camera_height,
                                 f32 camera_radius) noexcept {
    UnderwaterState state;
    const auto depth = static_cast<f32>(surface_height - camera_height);
    state.depth_metres = depth;
    state.camera_submerged = depth > 0.0F;

    // THE CROSSING, EXPLICITLY. Over a camera of finite extent the surface sweeps from the top of
    // the view to the bottom, and the fraction submerged is where it has got to. A boolean here is
    // the hard switch the specification forbids; this is one number, and it is zero and one at the
    // two ends so a consumer that only wants the boolean still has it.
    if (camera_radius > 0.0F) {
        state.submerged_fraction = (depth + camera_radius) / (2.0F * camera_radius);
    } else {
        state.submerged_fraction = state.camera_submerged ? 1.0F : 0.0F;
    }
    state.submerged_fraction =
        (state.submerged_fraction < 0.0F)
            ? 0.0F
            : ((state.submerged_fraction > 1.0F) ? 1.0F : state.submerged_fraction);

    const f32 clamped_depth = (depth < 0.0F) ? 0.0F : depth;
    state.extinction = optics.absorption + optics.scattering;
    // THE BODY'S OWN PARAMETERS, not a fixed tint: the fog colour is what this body's water does to
    // the light reaching this depth, so a clear sea and a silty lake differ here by construction.
    state.fog_colour = water_in_scatter(optics, clamped_depth);
    // Caustics weaken with depth because the light that focuses them has been attenuated on the way
    // down. The green channel's transmittance stands for the luminance, which is where the eye's
    // sensitivity is.
    state.caustic_strength = water_transmittance(optics, clamped_depth).y;
    return state;
}

}  // namespace cy::water

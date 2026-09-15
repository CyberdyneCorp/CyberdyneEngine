#pragma once
// ILLUMINATION READING THE CLOUD SHADOW FIELD. M11.c task 5.2.
//
// `atmosphere-sky-and-clouds` — "Cloud shadows": "Clouds SHALL cast shadows onto the world through a
// COARSE WORLD-SCALE SHADOW REPRESENTATION — a low-frequency field or map covering a large area at
// low resolution — CONSUMED BY TERRAIN, FOLIAGE, WATER, AND ILLUMINATION."
//
// ================================================================================================
// THE FOURTH CONSUMER, AND WHY IT HAD TO BE WRITTEN RATHER THAN DECLARED
// ================================================================================================
//
// `CloudShadowField::declare_consumers()` has declared all four to the registry since M10 —
// terrain-materials, foliage, water-shading and illumination — and `FieldRegistry::validate()` is
// happy with all four, because declaring a consumer is a CONFIGURATION entry and validation checks
// configuration. Until this file NOTHING OUTSIDE `src/rendering/sky/` CALLED
// `CloudShadowField::sample` or named `cloud_shadow_field_id()` at all. A declared consumer that
// never samples is a row in a table, and `m11a:sky-field-consumed-outside-the-sky` is the criterion
// that was written to say so — and could not, because what it did was search for the words.
//
// This is illumination's half, and it is the half `atmosphere-sky-and-clouds`' own scenario is
// about: a cloud crosses a valley and the valley goes dark.
//
// ================================================================================================
// A CLOUD SHADOWS THE SUN AND NOT A TORCH
// ================================================================================================
//
// Only `kGpuLightDirectional` is attenuated, and the reason is physical rather than a simplifying
// choice: the field is "the fraction of direct SUNLIGHT reaching the surface through the cloud
// layers", computed by marching from the ground to the top of the deck along the sun's direction. A
// point light inside the world is under the deck with the surface it lights, so there is nothing
// between them for the field to describe. An implementation that multiplied every light by it would
// dim a character's own lantern when a cloud passed overhead, which reads as a bug and is one.
//
// AND THE SKY LIGHT IS NOT ATTENUATED HERE EITHER. A cloud deck REDISTRIBUTES ambient light rather
// than removing it — an overcast sky is brighter overhead than a clear one and the ground under it
// is not black — and that redistribution is `sky_light.h`'s, computed from the same atmosphere. A
// second attenuation applied here would darken the sky term twice.
//
// ================================================================================================
// IT SAMPLES THROUGH THE SUBSTRATE, WHICH IS WHAT MAKES THE FOUR CONSUMERS AGREE
// ================================================================================================
//
// `CloudShadowField::sample` and nothing else. Terrain, foliage and water read the same function,
// so the shadow a light is attenuated by and the shadow a leaf is darkened by are ONE NUMBER rather
// than four derivations of one idea — which is the requirement's "consumed by" list made true by
// construction instead of by four modules agreeing to be careful.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/environment/store.h>
#include <cy/rendering/lighting/lights.h>
#include <cy/world/coordinates.h>

namespace cy::rendering {

/// What illumination read, so that a frame can report it rather than assert it.
struct CloudShadowIllumination {
    /// Directional lights this call attenuated.
    u32 directional_lights = 0;
    /// Lights it left alone, because a cloud does not shadow a torch.
    u32 punctual_lights = 0;
    /// The fraction of sunlight the field returned at the position asked about. 1 in full sun, and
    /// 1 as well where the field has no data — the declared default, which is the one value that is
    /// invisible in an unstreamed world.
    f32 transmittance = 1.0F;
};

/// The fraction of direct sunlight reaching `at`, read from the substrate.
///
/// A thin wrapper on `sky::CloudShadowField::sample` and deliberately thin: it exists so that
/// illumination has ONE call site for the field, so that the call can be found, and so that this
/// module's dependency on the sky is one header rather than a habit.
[[nodiscard]] f32 cloud_shadow_at(const environment::FieldStore& store,
                                  const world::WorldVec3d& at) noexcept;

/// One directional light's illuminance at `at`, in the same lux `GpuLight::intensity` carries.
[[nodiscard]] f32 sunlight_under_clouds(const GpuLight& sun, const environment::FieldStore& store,
                                        const world::WorldVec3d& at) noexcept;

/// Attenuate every directional light in `lights` by the cloud deck above `at`, in place.
///
/// The position is the SHADED point rather than each light's, because a directional light has no
/// position: what the field describes is how much of the sun reaches the ground here.
CloudShadowIllumination apply_cloud_shadows(Span<GpuLight> lights,
                                            const environment::FieldStore& store,
                                            const world::WorldVec3d& at) noexcept;

}  // namespace cy::rendering

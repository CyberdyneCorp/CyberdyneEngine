#pragma once
// THE COMPOSITION POINT WHERE THE ATMOSPHERE BECOMES ILLUMINATION. M11.c tasks 2.1–2.4.
//
// ================================================================================================
// WHAT THIS MODULE IS, AND WHY IT IS A MODULE RATHER THAN THREE LINES SOMEWHERE
// ================================================================================================
//
// [Dependency cycle 2](docs/roadmap/dependencies.md) is entirely about this seam: an analytic sky
// seeds at M7, *"the physical atmosphere, its precomputed tables and volumetric clouds land at M10,
// and GI reaches Complete there"*. The atmosphere landed at M10 and the seam was never joined.
// `src/rendering/gi/` links `cy::rendering-denoise` and `cy::rendering-raytracing` and not
// `cy::rendering-sky`; `sky_light.h` carries a three-line adapter in a comment and says a
// composition point writes it; and until this module existed, **nothing in the tree constructed a
// `gi::SkyTerm` from an atmosphere at all**, so every scenario in `rendering-global-illumination`'s
// "Sky and atmosphere" was satisfied by a two-colour gradient with nobody able to tell.
//
// `dependencies.md` calls joining it *"one adapter at one composition point"*. **It is not one
// adapter, and this file is the measured answer to that.** The three lines the adapter would be are
// `SkyIllumination::fit()`, four of the two hundred here. The rest is the half of the requirement an
// adapter cannot do:
//
//   * *"updated incrementally where the sky changes continuously"* — a sun that moved a quarter of a
//     degree must not cost what `IlluminationSystem::configure()` costs, which is the clipmap
//     rebuilt, the probe cache discarded and the acceleration service reconstructed. So the term is
//     refitted on a THRESHOLD and installed through `set_sky_term`, which touches the two places
//     the sky is read from and nothing else.
//   * *"invalidating only the illumination that depends on it"* — a sun that rotated moved no
//     geometry, so the sparse distance field cannot have been invalidated by it.
//     `InvalidationCause::SkyChanged` exists for that and `IlluminationSystem` skips the field for
//     it, which makes `IlluminationFrameReport::invalidated_field_bricks == 0` the check.
//   * *"under the illumination budget"* — the fit is capped at one per update and its direction
//     count is a lever, so the cost of a whole day is the number of threshold crossings rather than
//     the number of frames. `SkyIlluminationReport` carries both numbers so a comparison against a
//     full recomputation is arithmetic rather than an argument.
//
// ================================================================================================
// WHY IT IS ITS OWN MODULE AND NOT A DEPENDENCY ADDED TO ONE OF THE TWO
// ================================================================================================
//
// `src/rendering/sky/CMakeLists.txt` says, in capitals, that it does not depend on
// `cy::rendering-gi`, *"because a sky that depended on a global illumination system could not be
// tested without one"* — and the gradient fit is measured in that module's own suite with no GI
// present, which is what makes "sufficient for GI's sky term" a measurement. The reverse direction
// costs the same thing in the other currency: `cy::rendering-gi` links two modules today and
// `tests/` runs every illumination case headless, and making it link the sky would drag
// `cy::environment`, `cy::world`, `cy::rendering-post`, `cy::rendering-temporal` and
// `cy::rendering-arbiter` behind it.
//
// So the composition point is a third module that names both and that neither names. That is also
// what makes the seam a LINK-GRAPH FACT rather than a paragraph: exactly one library in this tree
// depends on `cy::rendering-gi` and `cy::rendering-sky` at once, it is this one, and deleting it
// takes `m11c:gi-sky-term-constructed` red.
//
// ================================================================================================
// THE CLOUD SHADOW FIELD, AND THE ONE FUNCTION IT IS READ THROUGH
// ================================================================================================
//
// `atmosphere-sky-and-clouds` requires the coarse cloud shadow field to be *"consumed by terrain,
// foliage, water, and illumination"*. This module is the fourth consumer, and it reads the field
// through `sky::CloudShadowField::sample()` — the static function terrain, foliage and water call —
// rather than through `FieldStore::sample_at()` with a residency of its own. That is deliberate and
// it is this rung correcting a specific mistake: `m10:sky-field-round-trip` records a check that
// was moved OFF the defect it named by being rewritten to read from `store.sample_at()`, leaving
// the function every consumer actually calls unjudged. A consumer that picks its own sampling path
// is a consumer that cannot be broken by the defect its own row declares.
//
// WHAT THE FIELD ATTENUATES, AND WHAT IT DOES NOT. The sun, and not the sky term. The field is the
// fraction of DIRECT sunlight reaching a position; cloud cover scatters the rest into the sky
// rather than deleting it, so dimming the gradient by the same number would take the light out of
// the frame twice. An overcast sky in this model is a dark sun and an atmosphere whose own
// scattering the physical model already carries.

#include <cy/core/base/error.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/environment/store.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/sky/atmosphere.h>
#include <cy/rendering/sky/cloud_shadows.h>
#include <cy/rendering/sky/sky_light.h>
#include <cy/world/coordinates.h>

namespace cy::rendering::skylight {

/// Where the illumination sky term came from.
///
/// `rendering-global-illumination` — "The placeholder is reported as a placeholder": *"WHEN
/// illumination runs with no atmosphere present and falls back to the analytic gradient THEN the
/// frame's diagnostics SHALL report the fallback, so a published picture's lighting provenance is
/// readable rather than inferred."* A two-colour gradient and a fitted atmosphere produce pictures
/// that are both plausible; this enumerator is the only thing that tells them apart afterwards.
enum class SkyTermProvenance : u8 {
    /// The declared fallback. A configuration with no atmosphere, reported as such.
    AnalyticFallback = 0,
    /// Fitted to a physical atmosphere by `sky::fit_sky_gradient`.
    PhysicalAtmosphere,
};

/// The enumerator's own spelling, for a diagnostic and for a published caption. Never null.
[[nodiscard]] const char* sky_term_provenance_name(SkyTermProvenance provenance) noexcept;

/// The levers. Each is a budget lever in the sense `rendering-global-illumination` uses: a number
/// an arbiter may move without the seam changing shape.
struct SkyIlluminationSettings {
    /// How far the sun may move, in radians, before the term is refitted.
    ///
    /// `sky::kSunMovementThreshold` — about a quarter of a degree, half the sun's own diameter — is
    /// the default because it is what `SkyViewTable` rebuilds on, and a term and a table that
    /// disagreed about when the sun had moved would step against each other in the same frame.
    f32 refit_threshold_rad = sky::kSunMovementThreshold;
    /// Directions per axis the gradient fit integrates. The fit's whole cost, and the first lever a
    /// budget takes.
    u32 fit_samples = 24;
    /// Refits permitted in one update. One, and this is what bounds a day: the cost of the day is
    /// the number of threshold crossings rather than the number of frames.
    u32 max_fits_per_update = 1;
    /// Read the cloud shadow field where one is attached.
    bool consume_cloud_shadow = true;
    /// The observer's altitude above the planet's surface, in metres, for the atmosphere's own
    /// planet-centred frame.
    f32 observer_altitude_metres = 0.0F;
    /// A multiplier on the fitted term, for a project whose lights are not in physical units.
    ///
    /// ONE IS THE PHYSICAL ANSWER AND IT IS THE DEFAULT. This exists because the atmosphere answers
    /// in LUX — a clear midday sky delivers some tens of thousands of them — and a scene whose lamps
    /// were authored as "intensity 30" is not in that unit. Mixing the two does not look like a
    /// bright day; it looks like the sky is the only light in the world.
    ///
    /// MEASURED, on `samples/07-fidelity`'s district during M11.c: lighting it through this seam
    /// with an unscaled physical sky moved its mean indirect luminance from 0.54 to 1905 and its
    /// convergence metric from 0.846 in ten frames to 0.609 after two thousand, because a probe
    /// gathering 24 rays a frame across a four-thousand-to-one contrast never settles. Scaling the
    /// term to the irradiance that shot's own placeholder delivered — an exposure of 0.000345 —
    /// brought the luminance back to 0.66 and the convergence to 0.842 at the frame cap. **That last
    /// number is the finding and it is not this setting's**: the shot's target is 0.85 and its
    /// measured metric sits within half a per cent of it either way, so ANY change to its sky term
    /// tips it, and re-lighting an M7 artefact is not a thing an exposure lever can make safe.
    ///
    /// So the exposure is an explicit, reported decision rather than a number folded into the fit.
    /// `SkyIlluminationReport::exposure` carries it, and a picture published with an exposure that
    /// is not one is a picture whose sky is the atmosphere's COLOUR and the project's BRIGHTNESS —
    /// which is a true sentence and a different one from "lit by a physical sky".
    f32 exposure = 1.0F;
};

/// What one update did, and what it cost. The frame diagnostics the requirement asks for.
struct SkyIlluminationReport {
    SkyTermProvenance provenance = SkyTermProvenance::AnalyticFallback;
    /// The term was refitted this update. False is the ordinary frame.
    bool refitted = false;
    /// How far the sun moved since the term was last fitted, in radians.
    f32 sun_delta_rad = 0.0F;
    /// The relative change in the term's own cosine-weighted irradiance across this update, for an
    /// upward-facing surface. Zero on a reuse. **The number "without a visible step" is measured
    /// with**, and the reason a threshold is a quarter of a degree rather than a taste.
    f32 irradiance_step = 0.0F;
    /// The exposure the term was scaled by. One is the physical answer; anything else says the
    /// picture's sky is the atmosphere's colour at the project's brightness. See the settings.
    f32 exposure = 1.0F;
    /// The fraction of direct sunlight the cloud shadow field reported at the observer. One where
    /// no field is attached, which is also what an unclouded sky reports — `cloud_field_read` is
    /// what tells the two apart.
    f32 cloud_transmittance = 1.0F;
    bool cloud_field_read = false;
    /// The sun as illumination should see it: derived from the atmosphere's own
    /// `sun_illuminance()`, attenuated by the cloud shadow field. Only meaningful when
    /// `sun_from_atmosphere` — a fallback configuration has no atmosphere to derive one from.
    gi::GiLight sun{};
    bool sun_from_atmosphere = false;
    /// An invalidation record was filed for `InvalidationCause::SkyChanged`. The illumination
    /// system services it on its next `update()`, and the counts are in that frame's own report.
    bool invalidation_filed = false;
    // --- cumulative, since `configure()` ---------------------------------------------------------
    u32 fits = 0;
    u32 reuses = 0;
    /// Directions integrated across every fit. The unit of work, and a number that does not depend
    /// on how fast the machine is — which a millisecond count would. `SkyTableStats` reports the
    /// same quantity for the same reason.
    u64 directions_integrated = 0;
};

/// What the caller knows about this frame that this module cannot.
struct SkyIlluminationFrame {
    /// Pointing FROM the surface TOWARDS the sun — `sky::CelestialBody::direction`'s convention and
    /// every function in `atmosphere.h`'s, and the opposite of a directional light's travel vector.
    Vec3 sun_direction{0.0F, 1.0F, 0.0F};
    /// Where the sky is visible, in world space. What a refit invalidates, and nothing outside it.
    Aabb lit_region{};
    /// The observer, for the cloud shadow sample. The field is coarse — its cells are over a
    /// hundred metres wide — so one sample per view is the resolution the phenomenon has.
    world::WorldVec3d observer{};
    u64 frame = 0;
    /// The identity the invalidation is attributed to. `rendering-global-illumination` requires
    /// invalidations to be attributable, and "the sky" is a source like a light or an instance.
    u64 sky_id = 1;
};

/// The cosine-weighted irradiance a surface receives from a `gi::SkyTerm`, in the term's own units.
///
/// Deterministic by construction: a fixed stratified hemisphere rather than a random one, so two
/// runs and two machines agree about whether the sky stepped. `rings` is directions per axis.
[[nodiscard]] Vec3 term_irradiance(const gi::SkyTerm& term, Vec3 normal, u32 rings = 8) noexcept;

/// The seam.
///
/// Not thread-safe: it belongs to the view whose illumination system it drives, for the reason
/// `denoise::Denoiser` gives about a history keyed by view inside a shared object.
class SkyIllumination {
public:
    SkyIllumination() noexcept = default;

    [[nodiscard]] Status configure(const SkyIlluminationSettings& settings) noexcept;
    [[nodiscard]] const SkyIlluminationSettings& settings() const noexcept { return settings_; }

    /// Build the term from this atmosphere. The provenance becomes the atmosphere's and the next
    /// `update()` fits unconditionally, because a new atmosphere is not a sun that moved.
    [[nodiscard]] Status set_atmosphere(const sky::Atmosphere& atmosphere) noexcept;

    /// The declared fallback: a configuration with no atmosphere keeps the analytic gradient, and
    /// every report says so. **This is a first-class configuration and not a failure**, which is
    /// why it is a method rather than what happens when `set_atmosphere` is not called: a project
    /// with a stylised sky is entitled to one, and entitled to have its pictures say which it used.
    void use_analytic_fallback(const gi::SkyTerm& term) noexcept;

    /// Attach the field store the cloud shadow is read from, or null to detach. The store is
    /// borrowed and must outlive this object.
    void set_cloud_shadow_source(const environment::FieldStore* store) noexcept;

    /// The fraction of direct sunlight reaching a world position, through
    /// `sky::CloudShadowField::sample` — the function terrain, foliage and water call. One with no
    /// field attached or with the lever off.
    [[nodiscard]] f32 sunlight_fraction(const world::WorldVec3d& at) const noexcept;

    /// One update: refit if the sun has moved past the threshold, install the term, file the
    /// invalidation, and derive the sun the atmosphere implies.
    ///
    /// It does NOT call `IlluminationSystem::update()`. The frame belongs to the caller; what this
    /// owns is the decision about whether the sky changed and what that change invalidates.
    SkyIlluminationReport update(const SkyIlluminationFrame& frame,
                                 gi::IlluminationSystem& system) noexcept;

    [[nodiscard]] const gi::SkyTerm& term() const noexcept { return term_; }
    [[nodiscard]] SkyTermProvenance provenance() const noexcept { return provenance_; }
    [[nodiscard]] const SkyIlluminationReport& last_report() const noexcept { return report_; }

private:
    /// Fit the gradient to the atmosphere at this sun direction, and account for the work.
    void fit(Vec3 sun_direction) noexcept;

    SkyIlluminationSettings settings_{};
    sky::Atmosphere atmosphere_{};
    gi::SkyTerm term_{};
    const environment::FieldStore* clouds_ = nullptr;
    Vec3 fitted_sun_{0.0F, 1.0F, 0.0F};
    SkyTermProvenance provenance_ = SkyTermProvenance::AnalyticFallback;
    bool has_atmosphere_ = false;
    bool fitted_ = false;
    SkyIlluminationReport report_{};
};

}  // namespace cy::rendering::skylight

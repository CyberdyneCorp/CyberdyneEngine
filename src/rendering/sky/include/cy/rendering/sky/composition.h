#pragma once
// SKY COMPOSITION: the background, the stars, the light the sky IS, and the state weather publishes
// into the local volumetrics. M10 task 3.3.
//
// `atmosphere-sky-and-clouds` — "Sky composition": "The sky SHALL be composed from: the atmosphere,
// celestial bodies, STARS AND BACKGROUND SKY CONTENT, clouds, and optional phenomena such as
// aurorae. The sky SHALL be rendered as a dedicated path producing RADIANCE FOR THE BACKGROUND, a
// FILTERED RADIANCE MAP for specular image-based lighting, and IRRADIANCE for ambient diffuse —
// consumed by `rendering-global-illumination`."
//
// M7's Seed built the irradiance and the three-colour gradient and measured both. What is here is
// the other three: the stars, the composition that puts every element in one radiance, and the
// filtered radiance map a specular lobe reads.
//
// ================================================================================================
// "NIGHT IS NOT A TEXTURE BY NECESSITY" IS A STRUCTURAL CLAIM, SO IT IS A STRUCTURAL ANSWER
// ================================================================================================
//
// "Background sky content SHALL be supportable as procedural stars, a star catalogue, or authored
// imagery, and THE CHOICE SHALL BE A QUALITY AND CONTENT DECISION RATHER THAN A STRUCTURAL ONE."
//
// So there is ONE path. `StarField` holds a source and a list of `Star`s, and `star_radiance()`
// reads the list without knowing where it came from: `generate_stars()` fills it from a seed,
// `StarField::catalogue` fills it from authored entries, and `StarSource::Imagery` names a texture
// the composition samples instead. A project moving from procedural stars to a real catalogue
// changes one enumerator and the number of entries — not a code path, and not a second night sky.
//
// ================================================================================================
// PLANETARY SCALE: THE SPLIT, NOT A SECOND IMPLEMENTATION
// ================================================================================================
//
// "Evaluation SHALL use the world's large-coordinate representation for camera position and SHALL
// remain CAMERA-RELATIVE for rendering, so precision does not degrade at altitude or distance...
// The horizon, atmospheric thickness with altitude, and the transition to space SHALL follow from
// the model rather than from separate implementations per altitude band."
//
// `PlanetaryView` is that sentence as a type. It takes a `world::WorldVec3d` — the engine's
// large-coordinate form — and produces exactly two things: the ALTITUDE, which is all the
// atmosphere needs from an absolute position, and a camera-relative origin at which every ray
// starts. There is no altitude band anywhere in this module and no `if (altitude > ...)`: a camera
// at 2 m and a camera at 400 km take the same code, and `test_sky_composition.cpp` walks one
// continuously from the ground to orbit and asserts that the radiance never steps.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/sky/atmosphere.h>
#include <cy/rendering/sky/celestial.h>
#include <cy/rendering/sky/clouds.h>
#include <cy/rendering/sky/sky_light.h>
#include <cy/rendering/sky/tables.h>
#include <cy/world/coordinates.h>

namespace cy::rendering::sky {

// ================================================================================================
// PLANETARY SCALE
// ================================================================================================

/// The camera, split into the one absolute quantity the atmosphere needs and the relative frame
/// everything else is computed in.
struct PlanetaryView {
    /// Metres above the planet's surface. The only thing derived from the absolute position, and
    /// the reason nothing downstream has to carry an `f64`.
    f32 altitude_metres = 0.0F;
    /// The planet-centred position the atmosphere integrates from: the altitude lifted onto the
    /// planet's radius. Its magnitude is the planet's radius plus the altitude, which is a number
    /// an `f32` carries to about half a metre at Earth's scale — the precision argument in one
    /// line, and why the eye's HORIZONTAL position is deliberately absent from it.
    Vec3 planet_relative{0.0F, 0.0F, 0.0F};
    /// The world position the clouds are addressed by, which does need the horizontal coordinates.
    /// Kept separate so that the atmosphere cannot accidentally be given a position a thousand
    /// kilometres from the origin and lose its precision to it.
    Vec3 world_position{0.0F, 0.0F, 0.0F};
};

/// Build the split from the engine's large-coordinate form.
[[nodiscard]] PlanetaryView planetary_view(const Atmosphere& atmosphere,
                                           const world::WorldVec3d& camera) noexcept;

/// The angle of the horizon below the local horizontal, in radians, from the model alone.
///
/// Here because "the horizon... SHALL follow from the model rather than from separate
/// implementations per altitude band" is a claim that can be checked: at the ground it is zero, at
/// 400 km it is about twenty degrees, and it is one `acos` either way.
[[nodiscard]] f32 horizon_dip(const Atmosphere& atmosphere, f32 altitude_metres) noexcept;

// ================================================================================================
// STARS AND BACKGROUND SKY CONTENT
// ================================================================================================

/// Where the background comes from. A quality and content decision: every value below composes
/// through the same function.
enum class StarSource : u8 {
    /// None. A daylight-only project, or a planet whose sky is opaque.
    None = 0,
    /// Generated from the world's seed. What a project gets without shipping a catalogue.
    Procedural,
    /// Authored entries — a real catalogue, or a constellation an art director placed.
    Catalogue,
    /// An authored cubemap or equirectangular image, named by an asset identity this module does
    /// not resolve. The composition multiplies it by the same visibility the other two use.
    Imagery,
    Count,
};

[[nodiscard]] const char* star_source_name(StarSource source) noexcept;

/// One star. Direction and illuminance, not a pixel: a star is a point light at infinity, and
/// storing its illuminance is what lets the composition put it through the same atmosphere the sun
/// goes through.
struct Star {
    /// Unit vector, in the same frame as `CelestialBody::direction`.
    Vec3 direction{0.0F, 1.0F, 0.0F};
    /// Illuminance at the top of the atmosphere, in lux. Sirius is about 1e-5; the faintest star a
    /// dark-adapted eye sees is about 1e-8.
    f32 illuminance = 1.0e-7F;
    /// The star's colour as a normalised multiplier — a blue-white O star against a red M dwarf.
    Vec3 tint{1.0F, 1.0F, 1.0F};
};

/// The background, whichever of the three it came from.
struct StarField {
    explicit StarField(Allocator& allocator = current_allocator()) noexcept : stars(allocator) {}

    StarSource source = StarSource::Procedural;
    Array<Star> stars;
    /// The asset the `Imagery` source names. Opaque here: this module does not resolve assets, and
    /// a sky that depended on the asset system could not be measured without one.
    u64 imagery_asset = 0;
    /// Multiplies every star's illuminance. The one dial an art director reaches for, and the
    /// reason a project does not have to edit a catalogue to make the night brighter.
    f32 intensity = 1.0F;
};

/// Fill `field` with `count` stars from a seed, deterministically.
///
/// Deterministic through `cy::determinism::RandomStream`, which is the engine's own counter-based
/// stream and not a generator of this module's own — `simulation-and-determinism` says derivation
/// is that and nothing else. Two machines with one seed therefore see one sky, which matters the
/// moment a screenshot is compared or a replay is verified.
///
/// The magnitude distribution is the real one: faint stars vastly outnumber bright ones, so a
/// uniform draw over illuminance produces a sky that looks like a bug.
[[nodiscard]] Status generate_stars(StarField& field, u64 seed, u32 count) noexcept;

/// The radiance the background adds in a direction, before the atmosphere is applied.
///
/// `visibility` is `CelestialState::star_visibility` — zero in daylight, one well after dusk —
/// which is the value M7's celestial model already computes and drives nothing, because the sky
/// composition consumes it and the sky composition was not there.
[[nodiscard]] Vec3 star_radiance(const StarField& field, Vec3 direction, f32 visibility,
                                 f32 angular_radius) noexcept;

// ================================================================================================
// AURORAE
// ================================================================================================

/// "optional phenomena such as aurorae". Optional means a declared, defaulted-off element of the
/// composition rather than a project's own pass bolted beside the sky — an aurora drawn outside the
/// composition would not be occluded by cloud and would not be attenuated by the atmosphere.
struct Aurora {
    bool enabled = false;
    /// Metres. Real aurorae sit between 90 and 300 km, which is above the atmosphere's top in most
    /// parameter sets — so they are composed as a background element, behind the air and in front
    /// of the stars.
    f32 altitude = 120000.0F;
    /// Degrees from the pole at which the oval sits, and its width.
    f32 oval_latitude = 67.0F;
    f32 oval_width = 6.0F;
    Vec3 colour{0.15F, 1.0F, 0.35F};
    f32 intensity = 0.02F;
};

// ================================================================================================
// THE FILTERED RADIANCE MAP
// ================================================================================================

/// The specular half of what `rendering-global-illumination` consumes: "a filtered radiance map for
/// specular image-based lighting".
///
/// Stored as an OCTAHEDRAL map per roughness level, because that is the projection with no seam a
/// mip chain has to special-case and no pole a latitude-longitude map over-samples. Level 0 is the
/// sharp sky; each level after it is filtered for a rougher lobe and is half the resolution, which
/// is the standard arrangement and is why a rough reflection costs a quarter as much to fetch.
class SkyRadianceMap {
public:
    explicit SkyRadianceMap(Allocator& allocator = current_allocator()) noexcept
        : texels_(allocator) {}

    /// Roughness levels. Five: mirror, and four steps to fully rough. A sixth would be finer than
    /// the difference a lobe can show at the base resolution this is built at.
    static constexpr u32 kLevels = 5;

    /// `base_resolution` is the edge of level 0's square. 32 is enough for the sky, which has no
    /// high-frequency content except the sun's disc — and the sun's disc is drawn separately for
    /// exactly that reason (`stellar_radiance()`).
    [[nodiscard]] Status configure(u32 base_resolution) noexcept;

    /// The callable `build()` takes. A function pointer and a context rather than a template
    /// parameter, so that the filtering lives in a `.cpp` and this header stays a header — and so
    /// that a caller can hand it a lambda without the whole prefilter being instantiated per
    /// caller.
    struct Sampler {
        Vec3 (*function)(void* context, Vec3 direction) = nullptr;
        void* context = nullptr;

        [[nodiscard]] Vec3 operator()(Vec3 direction) const noexcept {
            return function != nullptr ? function(context, direction) : Vec3{0.0F, 0.0F, 0.0F};
        }
    };

    /// Build every level from the composed sky. `radiance` is called with a direction and returns
    /// the sky's radiance there — the composition, so that clouds and stars are in the reflection
    /// and not only in the background.
    [[nodiscard]] Status build(const Sampler& radiance) noexcept;

    /// The filtered radiance in a direction at a roughness in [0, 1]. Linear between the two levels
    /// that straddle it, so a material's roughness does not step.
    [[nodiscard]] Vec3 sample(Vec3 direction, f32 roughness) const noexcept;

    [[nodiscard]] u32 base_resolution() const noexcept { return base_; }
    [[nodiscard]] u64 texels() const noexcept;
    [[nodiscard]] bool built() const noexcept { return built_; }

private:
    [[nodiscard]] u32 level_resolution(u32 level) const noexcept;
    [[nodiscard]] usize level_offset(u32 level) const noexcept;

    Array<Vec3> texels_;
    u32 base_ = 32;
    bool built_ = false;
};

// ================================================================================================
// THE COMPOSITION
// ================================================================================================

/// Everything the sky is composed from. A view rather than an owner: each element belongs to the
/// system that produces it, and holding them by pointer is what lets a project compose a sky with
/// no clouds, no stars, or no tables without a second code path for each absence.
struct SkyCompositionInputs {
    const Atmosphere* atmosphere = nullptr;
    const AtmosphereTables* tables = nullptr;
    /// Optional. Without it the sky is ray marched, which is what a cook or a screenshot does.
    const IncrementalSkyView* sky_view = nullptr;
    const CelestialState* celestial = nullptr;
    const StarField* stars = nullptr;
    const CloudField* clouds = nullptr;
    const Aurora* aurora = nullptr;

    PlanetaryView view;
    CloudQuality cloud_quality;
    f64 time_seconds = 0.0;
};

/// What one direction of sky came out as, and — for the diagnostic — what it was made of.
struct SkyCompositionSample {
    Vec3 radiance{0.0F, 0.0F, 0.0F};
    /// The elements, separately. "For any pixel of sky or cloud, the tooling SHALL be able to
    /// report what determined it" — this is that report's numeric half, and `diagnostics.h` is its
    /// readable one.
    Vec3 atmosphere_radiance{0.0F, 0.0F, 0.0F};
    Vec3 stellar{0.0F, 0.0F, 0.0F};
    Vec3 stars{0.0F, 0.0F, 0.0F};
    Vec3 aurora{0.0F, 0.0F, 0.0F};
    Vec3 clouds{0.0F, 0.0F, 0.0F};
    f32 cloud_transmittance = 1.0F;
    /// Where along the ray the clouds became opaque enough to matter, in metres, or -1 where they
    /// never did. `CloudMarchResult::half_transmittance_depth`, carried out so that the diagnostic
    /// can ask the reconstruction what was AT that point rather than guessing a depth.
    f32 cloud_depth_metres = -1.0F;
    u32 dominant_cloud_layer = kMaxCloudLayers;
    CloudMarchStats cloud_stats;
    /// True when the atmosphere came from the sky view table rather than from a march. The "sky is
    /// a lookup" scenario, reported per sample so a test can count how many pixels marched.
    bool from_table = false;
};

/// Compose one direction of sky.
[[nodiscard]] SkyCompositionSample compose_sky(const SkyCompositionInputs& inputs,
                                               Vec3 direction) noexcept;

/// The three things `rendering-global-illumination` consumes, produced together because they are
/// three views of one sky and producing them apart is how they drift.
struct SkyLighting {
    SkyIrradianceSh irradiance;
    SkyGradient gradient;
    /// The sunlight reaching the ground, after the atmosphere AND after the clouds overhead. The
    /// directional light a renderer places.
    Vec3 sun_illuminance{0.0F, 0.0F, 0.0F};
    /// The fraction of the sun that survived the clouds. Reported separately so that "WHEN cloud
    /// cover thickens THEN the radiance and irradiance the illumination system consumes SHALL
    /// change accordingly" can be attributed to the clouds rather than to the time of day.
    f32 cloud_transmittance = 1.0F;
    /// Mean sky radiance over the hemisphere, which is what the irradiance is an integral of and
    /// what a test compares between two cloud covers.
    Vec3 mean_sky_radiance{0.0F, 0.0F, 0.0F};
};

/// Produce the lighting from the same composition the background uses. `samples` is the stratified
/// sphere resolution; the default matches `project_sky_irradiance()`'s.
[[nodiscard]] SkyLighting compose_sky_lighting(const SkyCompositionInputs& inputs,
                                               u32 samples = 24) noexcept;

// ================================================================================================
// VOLUMETRIC INTEGRATION — weather reaches fog through STATE
// ================================================================================================
//
// "Weather SHALL influence local volumetrics through VISIBILITY AND AEROSOL DENSITY PUBLISHED AS
// STATE, not by writing fog parameters directly", and the scenario: "WHEN humidity rises THEN
// visibility SHALL change and the fog parameters SHALL FOLLOW FROM IT."
//
// The enforcement is the direction of the arrow. `AtmosphericMedium` is published by whoever owns
// the weather; `derive_fog_parameters()` is a pure function from it; and there is no function in
// this module that takes a `FogParameters` and writes it anywhere. A weather system that wanted to
// set a fog density directly would have to invent its own type, which is visible in review.

/// The state weather publishes. Two numbers and their provenance, not a fog setting.
struct AtmosphericMedium {
    /// Metres at which a black object is at 2% contrast — the meteorological definition, so that a
    /// weather system and a renderer mean the same thing by it.
    f32 visibility_metres = 40000.0F;
    /// Aerosol density relative to the atmosphere's own Mie coefficient. 1 is the clear-air value;
    /// fog is tens.
    f32 aerosol_density = 1.0F;
    /// Relative humidity in [0, 1], carried because it is what a haze's particle growth follows and
    /// because it is the scenario's own input.
    f32 humidity = 0.5F;
    /// Injected by VFX and by fires. Named here so that "VFX SHALL be able to inject density and
    /// emission into the local volumetric medium rather than producing an unrelated volumetric
    /// effect" has a field to inject into.
    f32 injected_density = 0.0F;
    Vec3 injected_emission{0.0F, 0.0F, 0.0F};
};

/// Derive the medium from the weather state. The one place humidity becomes visibility, so that a
/// project changing the relationship changes it once.
[[nodiscard]] AtmosphericMedium medium_from_weather(const Atmosphere& atmosphere,
                                                    const CloudWeatherState& weather) noexcept;

/// The froxel volume's parameters, derived. Everything a local volumetric pass needs and nothing a
/// weather system may set.
struct FogParameters {
    /// Per metre, per channel.
    Vec3 scattering{0.0F, 0.0F, 0.0F};
    Vec3 extinction{0.0F, 0.0F, 0.0F};
    Vec3 emission{0.0F, 0.0F, 0.0F};
    /// Henyey-Greenstein anisotropy for the medium, which grows as droplets do.
    f32 anisotropy = 0.7F;
};

[[nodiscard]] FogParameters derive_fog_parameters(const Atmosphere& atmosphere,
                                                  const AtmosphericMedium& medium) noexcept;

}  // namespace cy::rendering::sky

#pragma once
// ENVIRONMENT PROFILES AND THE QUALITY TIERS. M10 task 3.3.
//
// `atmosphere-sky-and-clouds` — "Environment profiles": "Atmosphere, sky, celestial, and cloud
// configuration SHALL be authorable as an environment profile asset, and a project SHALL be able to
// define several — an Earth-like world, a dusty desert planet, a toxic volcanic world, or
// successive stages of a TERRAFORMING PROCESS. Profiles SHALL be TRANSITIONABLE: a project SHALL be
// able to move between them over time with PER-PARAMETER TRANSITION BEHAVIOUR, rather than
// switching instantly. Profiles SHALL COMPOSE WITH WEATHER PRESETS RATHER THAN DUPLICATING THEM:
// the profile describes the world's atmosphere, the preset describes today's weather."
//
// ================================================================================================
// THE LAST SENTENCE IS ENFORCED BY WHAT IS ABSENT FROM `EnvironmentProfile`
// ================================================================================================
//
// There is NO `CloudWeatherState` in `EnvironmentProfile`, and that is the requirement rather than
// an oversight. A profile that carried today's wind and humidity would be a profile a project had
// to duplicate for every weather it wanted, and the duplicate would drift: someone would fix the
// scattering coefficients in the rainy variant and not in the clear one.
//
// So the composition is `configure_sky(profile, weather)` — two inputs, one runtime configuration —
// and `tests/test_sky_composition.cpp` holds the consequence as a pair of cases: one weather across
// two profiles gives two atmospheres, and two weathers across one profile give two skies with the
// same atmosphere.
//
// ================================================================================================
// PER-PARAMETER TRANSITION BEHAVIOUR, AND WHY IT IS PER GROUP RATHER THAN PER FIELD
// ================================================================================================
//
// "with per-parameter transition behaviour, rather than switching instantly."
//
// A curve per FIELD would be forty-odd curves an author has to fill in, and the forty-first field
// added to `Atmosphere` would silently get whatever the struct's default is. A curve per GROUP —
// atmosphere, celestial, clouds, stars, aurora, stylisation — is the granularity the requirement's
// own scenario needs: a terraforming stage wants its ozone to arrive over an hour while its cloud
// decks thicken over a week, and those are two groups. A project needing a single parameter on its
// own curve runs a second transition with only that group enabled, which is the same mechanism
// rather than a new one.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/arbiter/subsystem.h>
#include <cy/rendering/sky/atmosphere.h>
#include <cy/rendering/sky/celestial.h>
#include <cy/rendering/sky/cloud_shadows.h>
#include <cy/rendering/sky/clouds.h>
#include <cy/rendering/sky/composition.h>
#include <cy/rendering/sky/sky_light.h>

namespace cy::rendering::sky {

// ================================================================================================
// QUALITY TIERS
// ================================================================================================

/// "Atmosphere, sky, and cloud rendering SHALL provide quality tiers spanning MOBILE TO CINEMATIC:
/// table-based atmosphere with simple layered clouds at the low end, volumetric clouds with shadows
/// and temporal reconstruction in the middle, and higher ray march counts with advanced scattering
/// at the top."
enum class SkyQualityTier : u8 { Mobile = 0, Low, Medium, High, Cinematic, Count };

[[nodiscard]] const char* sky_quality_tier_name(SkyQualityTier tier) noexcept;

/// Every drawing lever the sky has, at one tier. NOTHING in this struct is state: there is no
/// coverage here, no wind, no precipitation, and that absence is what makes "a tier SHALL change
/// how the sky is drawn, never what the weather is" checkable rather than asserted.
struct SkyQualitySettings {
    SkyTableQuality tables = SkyTableQuality::Medium;
    /// Rows of the sky view table re-integrated per update. See `IncrementalSkyView`.
    u32 sky_view_row_budget = 4;
    /// Edge of the filtered radiance map's level 0.
    u32 radiance_map_resolution = 32;
    CloudQuality clouds;
    CloudShadowQuality cloud_shadows;
    /// False at `Mobile`: the low tier draws the cloud LAYERS from the same weather map without
    /// marching a volume. "table-based atmosphere with SIMPLE LAYERED CLOUDS at the low end" is
    /// that sentence, and the layers it draws are the same `CloudLayer`s the volumetric tiers
    /// march — which is why the weather is identical and only the picture differs.
    bool volumetric_clouds = true;
    /// Whether the cloud buffer is temporally reconstructed. Off at `Mobile`, where the march is
    /// not volumetric and there is nothing to reconstruct.
    bool temporal_reconstruction = true;
};

[[nodiscard]] SkyQualitySettings sky_quality(SkyQualityTier tier) noexcept;

/// The sky's ladder, PRICED, for `rendering::BudgetArbiter`.
///
/// THE SKY IS NOT A `BudgetSubsystem` AND THIS DOES NOT ADD ONE. `rendering-architecture` names the
/// arbiter's subsystems — "geometry, shadows, global illumination, reflections, material
/// evaluation, VFX, post-processing, and resolution scale" — and adding a ninth would be a change
/// to that specification rather than to this module. So the sky declares a ladder in the arbiter's
/// own currency and the renderer folds it into whichever allocation it belongs to, exactly as
/// `virtual-shadows`' five levers fold into one composite position. `apply_ladder_position()` is
/// the actuator.
///
/// The prices are DECLARED, at a stated reference, and they are relative to `Cinematic` at position
/// zero — the arbiter's convention, where position 0 is the most expensive and reduction walks
/// upward. They are the engine's defaults and not measurements of any particular machine; a project
/// that measures its own replaces them, which is why they are one object.
[[nodiscard]] QualityLadder sky_quality_ladder() noexcept;

/// The tier at a ladder position. Position 0 is `Cinematic`; the last position is `Mobile`.
[[nodiscard]] SkyQualityTier tier_at_ladder_position(u8 position) noexcept;

// ================================================================================================
// PROFILES
// ================================================================================================

/// What a profile describes: THE WORLD'S SKY. Not today's weather — see the header.
struct EnvironmentProfile {
    const char* name = "";
    Atmosphere atmosphere;
    CelestialModel celestial;
    /// The day's length and which clock it runs on. A world property: a planet whose day is ninety
    /// minutes has a ninety-minute day in every weather.
    TimeOfDay time;
    /// The layers the world HAS — their altitudes, thicknesses, scales and which of them exist.
    /// Their coverage and density come from the weather through `drive_cloud_layers()`, which is
    /// the seam between the two halves.
    CloudLayerSet clouds;
    StarSource star_source = StarSource::Procedural;
    u32 star_count = 2000;
    f32 star_intensity = 1.0F;
    Aurora aurora;
    StylisedDistance stylised_distance;
};

/// The four the requirement's own scenarios name.
[[nodiscard]] EnvironmentProfile earthlike_profile() noexcept;
[[nodiscard]] EnvironmentProfile dusty_desert_profile() noexcept;
[[nodiscard]] EnvironmentProfile toxic_volcanic_profile() noexcept;

/// A stage of a terraforming process, `stage` of `stages`, from the toxic world at 0 to the
/// Earth-like one at `stages`.
///
/// Here because "successive stages of a terraforming process" is a scenario, and a scenario with no
/// example in the tree is a scenario nobody runs. It is a BLEND of the two named profiles rather
/// than a table of hand-written stages, which is the same argument `blend_atmospheres()` makes: a
/// stage is a point on a path, and a path expressed as a table has gaps between its rows.
[[nodiscard]] EnvironmentProfile terraforming_profile(u32 stage, u32 stages) noexcept;

// ================================================================================================
// TRANSITIONS
// ================================================================================================

/// How one group's parameters travel between two profiles.
enum class TransitionCurve : u8 {
    /// Snap at the start. The one value that IS a preset switch, kept because a scripted cut needs
    /// it and because naming it makes it visible in an asset rather than accidental.
    Immediate = 0,
    Linear,
    /// Ease in and out. The default, because an atmosphere that starts and stops moving abruptly
    /// reads as a bug even when the endpoints are right.
    SmoothStep,
    /// Stay at the source until the transition ends, then snap. What a group that must not move
    /// until the rest has arrived uses.
    Hold,
    Count,
};

[[nodiscard]] const char* transition_curve_name(TransitionCurve curve) noexcept;

/// The groups a curve may be given, in the order `EnvironmentProfile` declares them.
enum class ProfileGroup : u8 {
    Atmosphere = 0,
    Celestial,
    Clouds,
    Stars,
    Aurora,
    Stylisation,
    Count
};

inline constexpr u32 kProfileGroupCount = static_cast<u32>(ProfileGroup::Count);

[[nodiscard]] const char* profile_group_name(ProfileGroup group) noexcept;

/// One group's behaviour.
struct ParameterTransition {
    TransitionCurve curve = TransitionCurve::SmoothStep;
    /// Seconds the group takes to travel, once it has started.
    f32 duration_seconds = 60.0F;
    /// Seconds before it starts. What lets the ozone arrive before the clouds thicken without two
    /// transitions.
    f32 delay_seconds = 0.0F;
};

/// The whole transition: one behaviour per group.
struct ProfileTransition {
    ParameterTransition groups[kProfileGroupCount];

    /// The longest (delay + duration) over every group: when the whole transition is finished.
    [[nodiscard]] f32 total_seconds() const noexcept;
};

/// A transition in progress. Holds the two endpoints and produces the profile at the current
/// moment; it does not own the sky and it writes nothing.
class ProfileBlender {
public:
    ProfileBlender() = default;

    /// Start travelling from `from` to `to`. Restarting mid-transition starts from the CURRENT
    /// blend rather than from the original source, so a second event during a terraforming stage
    /// does not snap the sky back.
    void begin(const EnvironmentProfile& from, const EnvironmentProfile& to,
               const ProfileTransition& transition) noexcept;

    void advance(f32 delta_seconds) noexcept;

    /// The profile as it is now.
    [[nodiscard]] const EnvironmentProfile& current() const noexcept { return current_; }
    [[nodiscard]] const EnvironmentProfile& target() const noexcept { return target_; }
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] f32 elapsed_seconds() const noexcept { return elapsed_; }
    /// One group's progress in [0, 1], after its own delay and curve.
    [[nodiscard]] f32 progress(ProfileGroup group) const noexcept;

private:
    void rebuild() noexcept;

    EnvironmentProfile source_;
    EnvironmentProfile target_;
    EnvironmentProfile current_;
    ProfileTransition transition_;
    f32 elapsed_ = 0.0F;
    bool active_ = false;
};

/// Blend two profiles by a per-group set of weights. The arithmetic `ProfileBlender` uses, exposed
/// because a cooker and an editor preview want it without a clock.
[[nodiscard]] EnvironmentProfile blend_profiles(const EnvironmentProfile& from,
                                                const EnvironmentProfile& to,
                                                const f32 (&weights)[kProfileGroupCount]) noexcept;

// ================================================================================================
// COMPOSING A PROFILE WITH A WEATHER PRESET
// ================================================================================================

/// The runtime configuration: what the profile says, with what the weather says applied to it.
struct SkyConfiguration {
    Atmosphere atmosphere;
    CelestialModel celestial;
    TimeOfDay time;
    /// The profile's layers with the weather's coverage, density, type and wind applied.
    CloudLayerSet clouds;
    Aurora aurora;
    StylisedDistance stylised_distance;
    StarSource star_source = StarSource::Procedural;
    f32 star_intensity = 1.0F;
    /// The state weather publishes into the local volumetrics, derived here so that the one path
    /// from humidity to visibility runs whether or not anybody asked for fog.
    AtmosphericMedium medium;
};

/// Compose the two. The profile is not modified and the weather is not modified; this is the only
/// function in the module that reads both.
[[nodiscard]] SkyConfiguration configure_sky(const EnvironmentProfile& profile,
                                             const CloudWeatherState& weather) noexcept;

}  // namespace cy::rendering::sky

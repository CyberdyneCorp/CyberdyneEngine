#pragma once
// Spatialisation: listeners, distance attenuation, cones, panning and Doppler. Task 4.3.5.
//
// `audio` — "Spatial audio" requires 2D and 3D spatialisation with distance attenuation models
// (inverse, inverse-squared, linear, logarithmic and a custom curve, with a reference and a maximum
// distance), cone directionality, panning appropriate to the output layout, Doppler from relative
// velocity with a configurable scale, filter-based occlusion, and per-listener rendering with more
// than one listener for split screen.
//
// AT SEED THIS IS THE FALLBACK PATH, AND THAT IS THE POINT. `audio` puts HRTF, geometry-driven
// occlusion, reflections and propagation behind an `AcousticsBackend` that arrives at M8, and it
// requires the engine to work without one: "**WHEN** the engine is built without Steam Audio
// **THEN** all audio SHALL still play, spatialised by the fallback path, with no missing sounds and
// no gameplay difference." Everything in this file is that fallback. Writing it first is what makes
// the acoustics backend an improvement rather than a dependency.
//
// EVERY FUNCTION HERE IS PURE. No state, no server, nothing to mock — which is what lets the
// attenuation curves and the panning law be asserted as numbers in a unit test rather than listened
// to.

#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/math/vec.h>

namespace cy::audio {

/// How loudness falls with distance. `audio` names all five.
enum class AttenuationModel : u8 {
    /// gain = reference / (reference + rolloff * (d − reference)). The physically reasonable
    /// default and the one most sound designers expect from a game engine.
    Inverse = 0,
    /// The inverse square law: what a point source in free air actually does.
    InverseSquare,
    /// Linear from `reference_distance` to `max_distance`. Not physical; predictable, which is what
    /// a designer placing an ambience wants.
    Linear,
    Logarithmic,
    /// A project-supplied curve, sampled by `AttenuationCurveFn`.
    Custom,
    Count,
};

[[nodiscard]] const char* attenuation_model_name(AttenuationModel model) noexcept;

/// A project's own curve: distance in, linear gain out. A function pointer rather than an
/// interface, for the reason the rig's custom nodes are: this is called per voice per update and a
/// virtual call would be a dispatch on a path that has thousands of them.
using AttenuationCurveFn = f32 (*)(f32 normalised_distance, void* user) noexcept;

struct Attenuation {
    AttenuationModel model = AttenuationModel::Inverse;
    /// Inside this distance the source is at full gain. Never zero: every model divides by it.
    f32 reference_distance = 1.0F;
    /// Beyond this the source is silent. What makes a voice inaudible and therefore virtualisable.
    f32 max_distance = 100.0F;
    f32 rolloff = 1.0F;
    AttenuationCurveFn curve = nullptr;
    void* curve_user = nullptr;
};

/// Linear gain for a source at `distance`.
[[nodiscard]] f32 attenuation_gain(const Attenuation& attenuation, f32 distance) noexcept;

/// Directionality. `audio`: "a cone with inner and outer angles and outer gain".
///
/// A default cone is omnidirectional — `inner_angle` of a full turn — so a zeroed struct is a
/// source that sounds the same in every direction rather than one that is silent everywhere.
struct Cone {
    f32 inner_angle_radians = 6.2831853F;
    f32 outer_angle_radians = 6.2831853F;
    f32 outer_gain = 0.0F;
};

/// Gain from the cone, given the source's forward direction and the direction from source to
/// listener. Both are normalised internally, so a caller need not.
[[nodiscard]] f32 cone_gain(const Cone& cone, Vec3 source_forward,
                            Vec3 source_to_listener) noexcept;

/// One set of ears. `audio` requires "per-listener rendering, with support for more than one
/// listener (split screen)", and the camera publishes one of these per active player camera — see
/// `camera-system`'s "Listener and streaming source".
struct Listener {
    Transform transform;
    Vec3 velocity{0.0F, 0.0F, 0.0F};
    /// Higher wins when a mix has more listeners than it renders for.
    f32 priority = 1.0F;
    bool active = true;
};

/// Per-channel gains for one voice into one bus, before the block's ramp.
struct PanGains {
    f32 left = 1.0F;
    f32 right = 1.0F;

    [[nodiscard]] f32 channel(u32 index) const noexcept { return (index == 0) ? left : right; }
};

/// Constant-power stereo panning of a direction expressed in the LISTENER's space.
///
/// Constant power (sin/cos of a quarter turn) rather than linear: a linear pan drops by three
/// decibels in the middle, which is audible as a hole as a source crosses in front of the listener.
/// A source directly behind produces the same gains as one directly in front — that is the honest
/// limit of amplitude panning, and it is what HRTF at M8 fixes; stating it here is better than a
/// front-back cue invented from nothing.
[[nodiscard]] PanGains pan_stereo(Vec3 direction_in_listener_space) noexcept;

/// The Doppler pitch multiplier.
///
/// `speed_of_sound` is metres per second; `factor` scales the effect, with 0 disabling it. Clamped
/// so a source moving faster than sound produces a very high pitch rather than a negative one,
/// which would play the clip backwards.
[[nodiscard]] f32 doppler_pitch(Vec3 listener_position, Vec3 listener_velocity,
                                Vec3 source_position, Vec3 source_velocity, f32 speed_of_sound,
                                f32 factor) noexcept;

/// Filter-based occlusion, which is the fallback for the geometry-driven occlusion an
/// `AcousticsBackend` provides. `occlusion` in [0, 1]: 0 is a clear path.
///
/// Returns the low-pass cutoff in hertz and writes the attenuation into `gain_out`. Two results
/// because occlusion is both — "the source SHALL be low-pass filtered **and** attenuated by the
/// occlusion parameters" — and returning only the cutoff is how the attenuation half gets
/// forgotten.
[[nodiscard]] f32 occlusion_filter(f32 occlusion, f32 sample_rate, f32& gain_out) noexcept;

}  // namespace cy::audio

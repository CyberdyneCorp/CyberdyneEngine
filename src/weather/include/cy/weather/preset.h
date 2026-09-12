#pragma once
// PRESETS AND TRANSITIONS: weather arrives, it does not appear. M10 task 3.2.
//
// `weather-and-wind` — "Weather presets and transitions": weather "SHALL be authorable as PRESETS
// describing a target environmental state — clear, overcast, light rain, storm, snowstorm,
// sandstorm, and project-defined — not merely a visual configuration"; "Applying a preset SHALL
// TRANSITION toward it rather than switching, with PER-PROPERTY DURATIONS: cloud coverage over
// minutes, precipitation over a shorter period, wind over another"; "Transitions SHALL be
// deterministic where the session's determinism profile requires it, and SCHEDULABLE IN ADVANCE so
// that a designed weather sequence is reproducible"; "`sequencing-and-cinematics` drives weather by
// SETTING THIS STATE through environment tracks — target preset, transition timing, and individual
// parameters — and a separate cinematic-only weather implementation SHALL NOT exist. A weather
// change authored on a timeline and one triggered by gameplay SHALL be the same operation."
//
// ================================================================================================
// THERE IS ONE ENTRY POINT, AND THAT IS HOW THE CINEMATIC REQUIREMENT IS MET
// ================================================================================================
//
// `WeatherDirector::apply()` is the only way to move the weather, and `schedule()` is `apply()`
// with a tick attached. A sequence's environment track calls `apply()`; a gameplay trigger calls
// `apply()`; the editor's preview calls `apply()`. There is no cinematic path to be parallel to,
// because there is no second function — and `test_presets.cpp` drives the same preset through a
// "timeline" and a "gameplay" caller and requires the two resulting state curves to be identical at
// every tick, not merely at the end.
//
// ================================================================================================
// DETERMINISM: A TRANSITION IS A PURE FUNCTION OF THE TICK
// ================================================================================================
//
// A transition stores its start VALUES and its start TICK, and evaluates from them. It holds no
// accumulated position and integrates nothing, so a peer that joined late, a rollback that rewound
// three ticks and a replay that fast-forwards all evaluate the same expression and get the same
// answer. "Deterministic where the session's determinism profile requires it" then needs no profile
// switch here: it is deterministic always, which is the cheaper of the two options and the one that
// cannot be configured wrong.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/weather/cells.h>

namespace cy::weather {

/// The properties a transition moves, each on its own clock. An enumerator rather than a struct of
/// durations, so that adding one is a row in a table and `Transition` iterates rather than
/// enumerates.
enum class WeatherProperty : u8 {
    Temperature = 0,
    Humidity,
    Pressure,
    WindSpeed,
    WindDirection,
    CloudCoverage,
    Precipitation,
    Visibility,
    kCount,
};

inline constexpr u32 kWeatherPropertyCount = static_cast<u32>(WeatherProperty::kCount);

[[nodiscard]] const char* weather_property_name(WeatherProperty property) noexcept;

/// The target a preset describes. It is a WEATHER STATE and not a visual configuration — the
/// specification's own distinction — so it is expressed in the same units `WeatherState` is.
struct WeatherTarget {
    f32 temperature_celsius = 14.0F;
    f32 humidity = 0.6F;
    f32 pressure_hpa = 1013.25F;
    /// Metres per second, and a bearing in radians measured from +x toward +z. Speed and direction
    /// rather than a vector, because they transition on DIFFERENT clocks: a veering wind and a
    /// freshening wind are two things a designer times separately, and interpolating a vector would
    /// make a 180-degree veer pass through calm.
    f32 wind_speed_mps = 4.0F;
    f32 wind_bearing_radians = 0.0F;
    f32 cloud_coverage = 0.25F;
    f32 precipitation_mm_per_hour = 0.0F;
    PrecipitationType precipitation_type = PrecipitationType::None;
    f32 visibility_metres = 20'000.0F;

    /// The target as a `WeatherState`, which is what `WeatherCells` relaxes toward.
    [[nodiscard]] WeatherState to_state() const noexcept;
    /// The state read back as a target, for the start of a transition.
    [[nodiscard]] static WeatherTarget from_state(const WeatherState& state) noexcept;
    [[nodiscard]] f32 property(WeatherProperty which) const noexcept;
    void set_property(WeatherProperty which, f32 value) noexcept;
};

/// One authored preset. A name and a target: the specification's list is a set of these, and a
/// project's own is another row rather than another type.
struct WeatherPreset {
    const char* name = "";
    WeatherTarget target;
};

/// The engine's presets, as the specification names them. Free functions rather than a table, so
/// that a project can take one, edit two fields and register it without a copy of the whole set.
[[nodiscard]] WeatherPreset preset_clear() noexcept;
[[nodiscard]] WeatherPreset preset_overcast() noexcept;
[[nodiscard]] WeatherPreset preset_light_rain() noexcept;
[[nodiscard]] WeatherPreset preset_storm() noexcept;
[[nodiscard]] WeatherPreset preset_snowstorm() noexcept;
[[nodiscard]] WeatherPreset preset_sandstorm() noexcept;

/// Per-property durations. "cloud coverage over minutes, precipitation over a shorter period, wind
/// over another" — one number each, in seconds, and the defaults are those three sentences.
struct TransitionProfile {
    f32 seconds[kWeatherPropertyCount] = {};

    /// The defaults the specification describes.
    [[nodiscard]] static TransitionProfile standard() noexcept;
    /// Every property at one duration. What a test that is not about timing wants.
    [[nodiscard]] static TransitionProfile uniform(f32 duration_seconds) noexcept;

    [[nodiscard]] f32 duration(WeatherProperty which) const noexcept {
        return seconds[static_cast<u32>(which)];
    }
};

/// A transition in flight: where it started, where it is going, and when.
struct Transition {
    WeatherTarget from;
    WeatherTarget to;
    TransitionProfile profile;
    /// The tick the transition started on, and the tick rate it was started at. Both are needed to
    /// turn a duration in seconds into a tick count without this file holding a clock.
    u64 start_tick = 0;
    f64 ticks_per_second = 60.0;
    bool active = false;

    /// The target at a tick. A pure function; see the header note.
    [[nodiscard]] WeatherTarget evaluate(u64 tick) const noexcept;
    /// Whether every property has arrived.
    [[nodiscard]] bool complete(u64 tick) const noexcept;
    /// How far one property has travelled, [0, 1].
    [[nodiscard]] f32 progress(WeatherProperty which, u64 tick) const noexcept;
};

/// A preset queued to be applied at a tick. "Schedulable in advance so that a designed weather
/// sequence is reproducible."
struct ScheduledChange {
    WeatherTarget target;
    TransitionProfile profile;
    u64 apply_tick = 0;
    const char* name = "";
};

/// Applies presets, runs transitions, and is the ONE entry point weather changes come through.
class WeatherDirector {
public:
    explicit WeatherDirector(Allocator& allocator) noexcept;

    WeatherDirector(const WeatherDirector&) = delete;
    WeatherDirector& operator=(const WeatherDirector&) = delete;

    /// The tick rate transitions are timed against. Set once by `WeatherSystem`.
    void set_tick_rate(f64 ticks_per_second) noexcept;

    /// Set the current state without transitioning. What a save restore and a level load do, and
    /// the only place in the module where the weather switches rather than arrives.
    void set_immediate(const WeatherTarget& target, u64 tick) noexcept;

    /// **THE ENTRY POINT.** A sequence's environment track, a gameplay trigger and the editor all
    /// call this. See the header note.
    [[nodiscard]] Status apply(const WeatherTarget& target, const TransitionProfile& profile,
                               u64 tick) noexcept;
    /// The same, from a preset.
    [[nodiscard]] Status apply(const WeatherPreset& preset, const TransitionProfile& profile,
                               u64 tick) noexcept;
    /// Set ONE parameter, leaving the rest of the transition alone. "target preset, transition
    /// timing, and INDIVIDUAL PARAMETERS" — this is the third of those.
    [[nodiscard]] Status apply_property(WeatherProperty which, f32 value, f32 seconds,
                                        u64 tick) noexcept;

    /// Queue a change for a future tick. Applied by `advance()` when the tick arrives, through
    /// `apply()` — the same entry point, so a scheduled change and an immediate one cannot differ.
    [[nodiscard]] Status schedule(const WeatherPreset& preset, const TransitionProfile& profile,
                                  u64 apply_tick) noexcept;
    [[nodiscard]] usize scheduled() const noexcept { return schedule_.size(); }

    /// Run the schedule up to `tick` and report the current target. Idempotent in `tick`: calling
    /// it twice with the same tick applies nothing twice, which is what a rollback needs.
    [[nodiscard]] Status advance(u64 tick) noexcept;

    /// The target the cells should be relaxing toward right now.
    [[nodiscard]] WeatherTarget current(u64 tick) const noexcept;
    [[nodiscard]] const Transition& transition() const noexcept { return transition_; }
    [[nodiscard]] bool transitioning(u64 tick) const noexcept;
    [[nodiscard]] const char* last_preset() const noexcept { return last_preset_; }

private:
    Allocator* allocator_;
    Transition transition_;
    Array<ScheduledChange> schedule_;
    f64 ticks_per_second_ = 60.0;
    u64 applied_through_ = 0;
    const char* last_preset_ = "";
};

}  // namespace cy::weather

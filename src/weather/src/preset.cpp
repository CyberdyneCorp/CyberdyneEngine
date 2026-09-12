// Presets and transitions: one entry point, per-property clocks, a pure function of the tick.
// M10 task 3.2.

#include <cy/weather/preset.h>

#include <algorithm>
#include <cmath>

namespace cy::weather {

namespace {

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] f32 smoothstep(f32 t) noexcept {
    const f32 clamped = clamp01(t);
    return clamped * clamped * (3.0F - (2.0F * clamped));
}

constexpr f32 kTwoPi = 6.283185307F;

/// Interpolate a BEARING the short way round. A veering wind that passed through the long arc would
/// swing the whole world's trees backwards for half a minute, and a linear interpolation of a
/// vector would pass through calm instead — which is why `WeatherTarget` stores speed and bearing
/// separately rather than a wind vector.
[[nodiscard]] f32 lerp_angle(f32 from, f32 to, f32 t) noexcept {
    f32 delta = std::fmod(to - from + kTwoPi + (kTwoPi / 2.0F), kTwoPi) - (kTwoPi / 2.0F);
    return from + (delta * t);
}

}  // namespace

const char* weather_property_name(WeatherProperty property) noexcept {
    switch (property) {
        case WeatherProperty::Temperature:
            return "temperature";
        case WeatherProperty::Humidity:
            return "humidity";
        case WeatherProperty::Pressure:
            return "pressure";
        case WeatherProperty::WindSpeed:
            return "wind-speed";
        case WeatherProperty::WindDirection:
            return "wind-direction";
        case WeatherProperty::CloudCoverage:
            return "cloud-coverage";
        case WeatherProperty::Precipitation:
            return "precipitation";
        case WeatherProperty::Visibility:
            return "visibility";
        default:
            return "unknown";
    }
}

f32 WeatherTarget::property(WeatherProperty which) const noexcept {
    switch (which) {
        case WeatherProperty::Temperature:
            return temperature_celsius;
        case WeatherProperty::Humidity:
            return humidity;
        case WeatherProperty::Pressure:
            return pressure_hpa;
        case WeatherProperty::WindSpeed:
            return wind_speed_mps;
        case WeatherProperty::WindDirection:
            return wind_bearing_radians;
        case WeatherProperty::CloudCoverage:
            return cloud_coverage;
        case WeatherProperty::Precipitation:
            return precipitation_mm_per_hour;
        case WeatherProperty::Visibility:
            return visibility_metres;
        default:
            return 0.0F;
    }
}

void WeatherTarget::set_property(WeatherProperty which, f32 value) noexcept {
    switch (which) {
        case WeatherProperty::Temperature:
            temperature_celsius = value;
            break;
        case WeatherProperty::Humidity:
            humidity = value;
            break;
        case WeatherProperty::Pressure:
            pressure_hpa = value;
            break;
        case WeatherProperty::WindSpeed:
            wind_speed_mps = value;
            break;
        case WeatherProperty::WindDirection:
            wind_bearing_radians = value;
            break;
        case WeatherProperty::CloudCoverage:
            cloud_coverage = value;
            break;
        case WeatherProperty::Precipitation:
            precipitation_mm_per_hour = value;
            break;
        case WeatherProperty::Visibility:
            visibility_metres = value;
            break;
        default:
            break;
    }
}

WeatherState WeatherTarget::to_state() const noexcept {
    WeatherState state;
    state.temperature_celsius = temperature_celsius;
    state.humidity = humidity;
    state.pressure_hpa = pressure_hpa;
    state.wind = Vec2{wind_speed_mps * std::cos(wind_bearing_radians),
                      wind_speed_mps * std::sin(wind_bearing_radians)};
    state.cloud_coverage = cloud_coverage;
    state.precipitation_mm_per_hour = precipitation_mm_per_hour;
    state.precipitation_type = precipitation_type;
    state.visibility_metres = visibility_metres;
    return state;
}

WeatherTarget WeatherTarget::from_state(const WeatherState& state) noexcept {
    WeatherTarget target;
    target.temperature_celsius = state.temperature_celsius;
    target.humidity = state.humidity;
    target.pressure_hpa = state.pressure_hpa;
    target.wind_speed_mps =
        std::sqrt((state.wind.x * state.wind.x) + (state.wind.y * state.wind.y));
    target.wind_bearing_radians = std::atan2(state.wind.y, state.wind.x);
    target.cloud_coverage = state.cloud_coverage;
    target.precipitation_mm_per_hour = state.precipitation_mm_per_hour;
    target.precipitation_type = state.precipitation_type;
    target.visibility_metres = state.visibility_metres;
    return target;
}

WeatherPreset preset_clear() noexcept {
    WeatherPreset preset;
    preset.name = "clear";
    preset.target.temperature_celsius = 21.0F;
    preset.target.humidity = 0.35F;
    preset.target.pressure_hpa = 1021.0F;
    preset.target.wind_speed_mps = 3.0F;
    preset.target.cloud_coverage = 0.05F;
    preset.target.precipitation_mm_per_hour = 0.0F;
    preset.target.precipitation_type = PrecipitationType::None;
    preset.target.visibility_metres = 38'000.0F;
    return preset;
}

WeatherPreset preset_overcast() noexcept {
    WeatherPreset preset = preset_clear();
    preset.name = "overcast";
    preset.target.temperature_celsius = 15.0F;
    preset.target.humidity = 0.72F;
    preset.target.pressure_hpa = 1010.0F;
    preset.target.wind_speed_mps = 5.0F;
    preset.target.cloud_coverage = 0.92F;
    preset.target.visibility_metres = 20'000.0F;
    return preset;
}

WeatherPreset preset_light_rain() noexcept {
    WeatherPreset preset = preset_overcast();
    preset.name = "light-rain";
    preset.target.humidity = 0.88F;
    preset.target.pressure_hpa = 1005.0F;
    preset.target.precipitation_mm_per_hour = 1.6F;
    preset.target.precipitation_type = PrecipitationType::Rain;
    preset.target.visibility_metres = 9'000.0F;
    return preset;
}

WeatherPreset preset_storm() noexcept {
    WeatherPreset preset = preset_light_rain();
    preset.name = "storm";
    preset.target.temperature_celsius = 13.0F;
    preset.target.humidity = 0.96F;
    preset.target.pressure_hpa = 981.0F;
    preset.target.wind_speed_mps = 22.0F;
    preset.target.cloud_coverage = 1.0F;
    preset.target.precipitation_mm_per_hour = 26.0F;
    preset.target.visibility_metres = 1'800.0F;
    return preset;
}

WeatherPreset preset_snowstorm() noexcept {
    WeatherPreset preset = preset_storm();
    preset.name = "snowstorm";
    preset.target.temperature_celsius = -7.0F;
    preset.target.humidity = 0.9F;
    preset.target.precipitation_mm_per_hour = 9.0F;
    preset.target.precipitation_type = PrecipitationType::Snow;
    preset.target.visibility_metres = 700.0F;
    return preset;
}

WeatherPreset preset_sandstorm() noexcept {
    WeatherPreset preset = preset_clear();
    preset.name = "sandstorm";
    preset.target.temperature_celsius = 36.0F;
    preset.target.humidity = 0.08F;
    preset.target.pressure_hpa = 1004.0F;
    preset.target.wind_speed_mps = 24.0F;
    preset.target.cloud_coverage = 0.2F;
    preset.target.precipitation_mm_per_hour = 3.0F;
    preset.target.precipitation_type = PrecipitationType::Dust;
    preset.target.visibility_metres = 400.0F;
    return preset;
}

TransitionProfile TransitionProfile::standard() noexcept {
    TransitionProfile profile;
    // The specification's own sentence, as numbers: "cloud coverage over MINUTES, precipitation
    // over a SHORTER period, wind over ANOTHER". Temperature and pressure are slower still,
    // because air does not change temperature as fast as it changes how wet it is.
    profile.seconds[static_cast<u32>(WeatherProperty::Temperature)] = 900.0F;
    profile.seconds[static_cast<u32>(WeatherProperty::Humidity)] = 420.0F;
    profile.seconds[static_cast<u32>(WeatherProperty::Pressure)] = 1'200.0F;
    profile.seconds[static_cast<u32>(WeatherProperty::WindSpeed)] = 240.0F;
    profile.seconds[static_cast<u32>(WeatherProperty::WindDirection)] = 360.0F;
    profile.seconds[static_cast<u32>(WeatherProperty::CloudCoverage)] = 480.0F;
    profile.seconds[static_cast<u32>(WeatherProperty::Precipitation)] = 120.0F;
    profile.seconds[static_cast<u32>(WeatherProperty::Visibility)] = 180.0F;
    return profile;
}

TransitionProfile TransitionProfile::uniform(f32 duration_seconds) noexcept {
    TransitionProfile profile;
    for (float& second : profile.seconds) {
        second = duration_seconds;
    }
    return profile;
}

f32 Transition::progress(WeatherProperty which, u64 tick) const noexcept {
    if (!active) {
        return 1.0F;
    }
    const f32 duration = profile.duration(which);
    if (duration <= 0.0F) {
        return 1.0F;
    }
    if (tick <= start_tick) {
        return 0.0F;
    }
    const f64 elapsed = static_cast<f64>(tick - start_tick) / ticks_per_second;
    return clamp01(static_cast<f32>(elapsed / static_cast<f64>(duration)));
}

WeatherTarget Transition::evaluate(u64 tick) const noexcept {
    if (!active) {
        return to;
    }
    WeatherTarget out = to;
    for (u32 index = 0; index < kWeatherPropertyCount; ++index) {
        const auto which = static_cast<WeatherProperty>(index);
        // SMOOTHSTEP, not linear: a preset that arrives linearly arrives with a visible corner at
        // both ends, and a designer timing a storm against a cut is timing the middle of it.
        const f32 t = smoothstep(progress(which, tick));
        if (which == WeatherProperty::WindDirection) {
            out.set_property(which, lerp_angle(from.property(which), to.property(which), t));
            continue;
        }
        const f32 a = from.property(which);
        const f32 b = to.property(which);
        out.set_property(which, a + ((b - a) * t));
    }
    // The TYPE switches when the precipitation is more than half way, rather than interpolating:
    // there is no value between rain and snow, which is the same reason the field is a `Category`
    // sampled `Nearest`.
    out.precipitation_type = (progress(WeatherProperty::Precipitation, tick) >= 0.5F)
                                 ? to.precipitation_type
                                 : from.precipitation_type;
    return out;
}

bool Transition::complete(u64 tick) const noexcept {
    if (!active) {
        return true;
    }
    for (u32 index = 0; index < kWeatherPropertyCount; ++index) {
        if (progress(static_cast<WeatherProperty>(index), tick) < 1.0F) {
            return false;
        }
    }
    return true;
}

WeatherDirector::WeatherDirector(Allocator& allocator) noexcept
    : allocator_(&allocator), schedule_(allocator) {}

void WeatherDirector::set_tick_rate(f64 ticks_per_second) noexcept {
    ticks_per_second_ = (ticks_per_second > 0.0) ? ticks_per_second : 60.0;
    transition_.ticks_per_second = ticks_per_second_;
}

void WeatherDirector::set_immediate(const WeatherTarget& target, u64 tick) noexcept {
    transition_.from = target;
    transition_.to = target;
    transition_.profile = TransitionProfile::uniform(0.0F);
    transition_.start_tick = tick;
    transition_.ticks_per_second = ticks_per_second_;
    transition_.active = false;
}

Status WeatherDirector::apply(const WeatherTarget& target, const TransitionProfile& profile,
                              u64 tick) noexcept {
    // **THE ENTRY POINT.** A sequence's environment track, a gameplay trigger and the editor all
    // arrive here, which is the whole of "a separate cinematic-only weather implementation SHALL
    // NOT exist" — there is no second function for one to be parallel to.
    //
    // The new transition starts from where the CURRENT one has reached, not from its own target, so
    // interrupting a storm halfway back to clear starts from the half-storm that is actually
    // outside rather than snapping.
    transition_.from = current(tick);
    transition_.to = target;
    transition_.profile = profile;
    transition_.start_tick = tick;
    transition_.ticks_per_second = ticks_per_second_;
    transition_.active = true;
    return ok();
}

Status WeatherDirector::apply(const WeatherPreset& preset, const TransitionProfile& profile,
                              u64 tick) noexcept {
    last_preset_ = preset.name;
    return apply(preset.target, profile, tick);
}

Status WeatherDirector::apply_property(WeatherProperty which, f32 value, f32 seconds,
                                       u64 tick) noexcept {
    // "target preset, transition timing, and INDIVIDUAL PARAMETERS" — the third of those. The other
    // properties keep travelling toward where they were already going, because a designer nudging
    // the wind should not cancel the storm that is arriving.
    WeatherTarget target = transition_.active ? transition_.to : current(tick);
    target.set_property(which, value);
    TransitionProfile profile = transition_.profile;
    const WeatherTarget started_from = current(tick);
    for (u32 index = 0; index < kWeatherPropertyCount; ++index) {
        // Every other property's remaining duration is preserved by restarting it from where it is
        // with what is left of its clock.
        const auto other = static_cast<WeatherProperty>(index);
        const f32 elapsed = profile.duration(other) * transition_.progress(other, tick);
        profile.seconds[index] = std::max(0.0F, profile.duration(other) - elapsed);
    }
    profile.seconds[static_cast<u32>(which)] = seconds;
    transition_.from = started_from;
    transition_.to = target;
    transition_.profile = profile;
    transition_.start_tick = tick;
    transition_.ticks_per_second = ticks_per_second_;
    transition_.active = true;
    return ok();
}

Status WeatherDirector::schedule(const WeatherPreset& preset, const TransitionProfile& profile,
                                 u64 apply_tick) noexcept {
    ScheduledChange change;
    change.target = preset.target;
    change.profile = profile;
    change.apply_tick = apply_tick;
    change.name = preset.name;
    if (Status pushed = schedule_.push_back(change); !pushed) {
        return pushed;
    }
    // Sorted by tick so that `advance()` can apply them in time order whatever order they were
    // queued in — "schedulable in advance so that a designed weather sequence is REPRODUCIBLE"
    // means the sequence must not depend on the order a script happened to register it.
    std::ranges::sort(schedule_, [](const ScheduledChange& a, const ScheduledChange& b) {
        return a.apply_tick < b.apply_tick;
    });
    return ok();
}

Status WeatherDirector::advance(u64 tick) noexcept {
    usize remaining = 0;
    for (auto change : schedule_) {
        // Idempotent in `tick`: a change is applied when its tick is reached and only once, so a
        // rollback that re-runs a tick does not re-apply it and a frame that ticked twice does not
        // either. `applied_through_` is that memory.
        if (change.apply_tick <= tick && change.apply_tick > applied_through_) {
            last_preset_ = change.name;
            if (Status applied = apply(change.target, change.profile, change.apply_tick);
                !applied) {
                return applied;
            }
            continue;
        }
        if (change.apply_tick > tick) {
            schedule_[remaining] = change;
            ++remaining;
        }
    }
    if (Status shrunk = schedule_.resize(remaining); !shrunk) {
        return shrunk;
    }
    applied_through_ = tick;
    return ok();
}

WeatherTarget WeatherDirector::current(u64 tick) const noexcept {
    return transition_.evaluate(tick);
}

bool WeatherDirector::transitioning(u64 tick) const noexcept {
    return transition_.active && !transition_.complete(tick);
}

}  // namespace cy::weather

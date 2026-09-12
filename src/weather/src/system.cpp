// CyberWeather assembled: one tick, one declared order, one set of budgets. M10 tasks 3.1 and 3.2.

#include <cy/weather/system.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cy::weather {

namespace {

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

void write_u64(Array<u8>& out, u64 value, bool& ok_flag) noexcept {
    for (u32 byte = 0; byte < 8; ++byte) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> (byte * 8U)) & 0xFFU));
            !pushed) {
            ok_flag = false;
            return;
        }
    }
}

[[nodiscard]] u64 read_u64(Span<const u8> bytes, usize offset) noexcept {
    u64 value = 0;
    for (u32 byte = 0; byte < 8; ++byte) {
        value |= static_cast<u64>(bytes[offset + byte]) << (byte * 8U);
    }
    return value;
}

/// One weather state on the wire. 37 bytes: nine numbers and a type byte, and not one bit of cloud
/// or particle. "A replay SHALL NEVER need to record cloud volumes, particle state, or rendered
/// output" is a property of `write_state()`'s body — there is nothing there to record them with.
constexpr u32 kStateWireBytes = 37;

void write_f32(Array<u8>& out, f32 value, bool& ok_flag) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (u32 byte = 0; byte < 4; ++byte) {
        if (Status pushed = out.push_back(static_cast<u8>((bits >> (byte * 8U)) & 0xFFU));
            !pushed) {
            ok_flag = false;
            return;
        }
    }
}

[[nodiscard]] f32 read_f32(Span<const u8> bytes, usize offset) noexcept {
    u32 bits = 0;
    for (u32 byte = 0; byte < 4; ++byte) {
        bits |= static_cast<u32>(bytes[offset + byte]) << (byte * 8U);
    }
    f32 value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void write_state(Array<u8>& out, const WeatherState& state, bool& ok_flag) noexcept {
    write_f32(out, state.temperature_celsius, ok_flag);
    write_f32(out, state.humidity, ok_flag);
    write_f32(out, state.pressure_hpa, ok_flag);
    write_f32(out, state.wind.x, ok_flag);
    write_f32(out, state.wind.y, ok_flag);
    write_f32(out, state.vertical_wind, ok_flag);
    write_f32(out, state.cloud_coverage, ok_flag);
    write_f32(out, state.precipitation_mm_per_hour, ok_flag);
    write_f32(out, state.visibility_metres, ok_flag);
    if (Status pushed = out.push_back(static_cast<u8>(state.precipitation_type)); !pushed) {
        ok_flag = false;
    }
}

[[nodiscard]] WeatherState read_state(Span<const u8> bytes, usize offset) noexcept {
    WeatherState state;
    state.temperature_celsius = read_f32(bytes, offset);
    state.humidity = read_f32(bytes, offset + 4);
    state.pressure_hpa = read_f32(bytes, offset + 8);
    state.wind.x = read_f32(bytes, offset + 12);
    state.wind.y = read_f32(bytes, offset + 16);
    state.vertical_wind = read_f32(bytes, offset + 20);
    state.cloud_coverage = read_f32(bytes, offset + 24);
    state.precipitation_mm_per_hour = read_f32(bytes, offset + 28);
    state.visibility_metres = read_f32(bytes, offset + 32);
    state.precipitation_type = static_cast<PrecipitationType>(bytes[offset + 36]);
    return state;
}

}  // namespace

WeatherSystem::WeatherSystem(Allocator& allocator) noexcept
    : allocator_(&allocator),
      cells_(allocator),
      wind_(allocator),
      storms_(allocator),
      director_(allocator),
      fields_(allocator),
      accumulation_(allocator),
      ecosystem_(allocator),
      lightning_(allocator) {}

Status WeatherSystem::configure(const WeatherConfig& config, const ClimateMap& climate) noexcept {
    if (Status configured = cells_.configure(config.grid, climate, config.seed); !configured) {
        return configured;
    }
    config_ = config;
    climate_ = &climate;
    cells_.set_orographic_model(config.orographic);
    cells_.set_storms(&storms_);
    wind_.set_cells(&cells_);
    wind_.set_storms(&storms_);
    wind_.set_gust_model(config.gust);
    wind_.set_turbulence_model(config.turbulence);
    wind_.set_seed(config.seed);
    accumulation_.set_model(config.accumulation);
    ecosystem_.set_model(config.ecosystem);
    director_.set_tick_rate(config.ticks_per_second);
    director_.set_immediate(WeatherTarget::from_state(cells_.global_state()), 0);
    presentation_ = config.presentation;
    sampler_ = EnvironmentSampler(cells_, wind_);
    sampler_.set_moment(determinism::SimulationPoint{}, 0.0);
    configured_ = true;
    return ok();
}

Status WeatherSystem::bind_fields(environment::FieldRegistry& registry,
                                  environment::FieldStore& store) noexcept {
    if (!configured_) {
        return fail(ErrorCode::Unavailable, "weather: configure the system before binding fields");
    }
    if (Status declared = fields_.declare(registry, config_.fields); !declared) {
        return declared;
    }
    // The claim is where a second producer for one of these fields is refused, by the substrate,
    // naming both. Passed through unchanged: nothing here can say "water.shoreline already produces
    // wetness" better than the registry that holds them both.
    if (Status claimed = fields_.claim(registry, store); !claimed) {
        return claimed;
    }
    if (Status consumers = fields_.declare_consumers(registry); !consumers) {
        return consumers;
    }
    return ecosystem_.set_default_biome_rules(fields_);
}

void WeatherSystem::set_terrain(const TerrainProfile& terrain) noexcept {
    cells_.set_terrain(terrain);
    wind_.set_terrain(terrain);
}

void WeatherSystem::set_occlusion(const SkyOcclusion* occlusion) noexcept {
    occlusion_ = occlusion;
    accumulation_.set_occlusion(occlusion);
}

Status WeatherSystem::publish(WeatherTickReport& report) noexcept {
    if (!fields_.claimed() || !publish_.is_valid()) {
        return ok();
    }
    // THE FIELD UPDATE BANDWIDTH BUDGET, and the only lever in this module that actually throttles.
    // A region whose lattice would exceed the tick's allowance is deferred rather than trimmed:
    // publishing half a region would leave the world with a seam between this tick's weather and
    // the last, which is worse than publishing it a tick later.
    const auto cell_metres = static_cast<f64>(fields_.level_metres(publish_.level));
    const auto across = static_cast<u64>((publish_.max_x - publish_.min_x) / cell_metres) + 1;
    const auto down = static_cast<u64>((publish_.max_z - publish_.min_z) / cell_metres) + 1;
    if (across * down > config_.budget.field_points_per_tick) {
        report.publication_throttled = true;
        return ok();
    }
    Expected<PublishReport, Error> published = fields_.publish_atmosphere(sampler_, publish_);
    if (!published) {
        return make_unexpected(published.error());
    }
    report.atmosphere = *published;
    return ok();
}

Expected<WeatherTickReport, Error> WeatherSystem::advance(determinism::SimulationPoint at,
                                                          f64 seconds) noexcept {
    WeatherTickReport report;
    if (!configured_) {
        return fail(ErrorCode::Unavailable, "weather: the system has not been configured");
    }
    tick_ = at.tick;
    moment_ = at;
    seconds_ += seconds;
    sampler_.set_moment(at, seconds_);

    // THE DECLARED ORDER. Each step reads the output of the ones before it and none reads the
    // output of one after: the director sets where the world is heading, the storms move, the cells
    // relax and advect toward it, the wind is composed from the result, the fields are published,
    // and the accumulation and the ecosystem consume what was published.
    if (Status advanced = director_.advance(at.tick); !advanced) {
        return make_unexpected(advanced.error());
    }
    cells_.set_global_target(director_.current(at.tick).to_state());

    if (Status advanced = storms_.advance(at, static_cast<f32>(seconds), config_.seed); !advanced) {
        return make_unexpected(advanced.error());
    }
    report.storms = static_cast<u32>(storms_.size());
    lightning_.clear();
    if (Status drained = storms_.drain_lightning(lightning_); !drained) {
        return make_unexpected(drained.error());
    }
    report.lightning_events = static_cast<u32>(lightning_.size());

    Expected<StepReport, Error> stepped = cells_.advance(at, seconds);
    if (!stepped) {
        return make_unexpected(stepped.error());
    }
    report.cells = *stepped;

    if (Status aged = wind_.advance(static_cast<f32>(seconds)); !aged) {
        return make_unexpected(aged.error());
    }

    if (Status published = publish(report); !published) {
        return make_unexpected(published.error());
    }

    if (fields_.claimed() && publish_.is_valid()) {
        Expected<AccumulationReport, Error> accumulated =
            accumulation_.advance(fields_, sampler_, publish_, static_cast<f32>(seconds));
        if (!accumulated) {
            return make_unexpected(accumulated.error());
        }
        report.accumulation = *accumulated;

        Expected<EcosystemReport, Error> evolved = ecosystem_.advance(fields_, publish_, seconds);
        if (!evolved) {
            return make_unexpected(evolved.error());
        }
        report.ecosystem = *evolved;
    }
    ++cloud_epoch_;
    return report;
}

Expected<WeatherTickReport, Error> WeatherSystem::advance_days(determinism::SimulationPoint at,
                                                               f64 days) noexcept {
    WeatherTickReport report;
    if (!configured_) {
        return fail(ErrorCode::Unavailable, "weather: the system has not been configured");
    }
    const f64 seconds = days * kSecondsPerDay;
    moment_ = at;
    tick_ = at.tick;
    seconds_ += seconds;
    sampler_.set_moment(at, seconds_);

    // THE EDITOR'S FAST-FORWARD IS THE RUNTIME'S OWN MODEL. It calls the same `WeatherCells::
    // advance()`, the same `Accumulation::advance()` and the same `Ecosystem::advance()`; what it
    // skips is the atmosphere PUBLICATION, because writing a tick's worth of wind ninety days'
    // worth of times would take a minute to produce a field the last step overwrites.
    Expected<StepReport, Error> stepped = cells_.advance(at, seconds);
    if (!stepped) {
        return make_unexpected(stepped.error());
    }
    report.cells = *stepped;

    if (fields_.claimed() && publish_.is_valid()) {
        Expected<AccumulationReport, Error> accumulated =
            accumulation_.advance(fields_, sampler_, publish_, static_cast<f32>(seconds));
        if (!accumulated) {
            return make_unexpected(accumulated.error());
        }
        report.accumulation = *accumulated;

        Expected<EcosystemReport, Error> evolved = ecosystem_.advance(fields_, publish_, seconds);
        if (!evolved) {
            return make_unexpected(evolved.error());
        }
        report.ecosystem = *evolved;
    }
    return report;
}

EnvironmentSample WeatherSystem::sample(const world::WorldVec3d& at, SampleQuality quality,
                                        determinism::SimulationClass reader) const noexcept {
    return sampler_.sample(at, quality, reader);
}

CloudDrive WeatherSystem::cloud_drive(const world::WorldVec3d& at) const noexcept {
    CloudDrive drive;
    const world::WorldVec3d reference{
        at.x, static_cast<f64>(config_.cloud_reference_altitude_metres), at.z};
    // The AUTHORITATIVE wind, deliberately: what the clouds are driven by is state, and a cloud
    // layer blown by a presentation-only residual would be a cloud layer a replay could not
    // reproduce. `atmosphere-sky-and-clouds` makes the same point from its own side — the map and
    // the state are identical at every tier and only the reconstruction is not.
    const EnvironmentSample sample = sampler_.sample(reference, SampleQuality::Gameplay,
                                                     determinism::SimulationClass::Authoritative);
    drive.wind = sample.wind.authoritative();
    drive.humidity = clamp01(sample.humidity);
    drive.precipitation_mm_per_hour = sample.precipitation_mm_per_hour;
    drive.temperature_celsius = sample.temperature_celsius;
    drive.epoch = cloud_epoch_;

    const StormContribution storm = storms_.contribution_at(at.x, at.z);
    drive.storm_intensity = clamp01(storm.influence);

    // The shear between the ground and the reference altitude, per kilometre, measured rather than
    // declared — a canyon wind at the surface and a steady wind aloft are exactly the shear that
    // separates the cloud layers.
    const EnvironmentSample ground =
        sampler_.sample(world::WorldVec3d{at.x, 0.0, at.z}, SampleQuality::Gameplay,
                        determinism::SimulationClass::Authoritative);
    const Vec3 low = ground.wind.authoritative();
    const f32 altitude_km = std::max(0.1F, config_.cloud_reference_altitude_metres / 1000.0F);
    const f32 high_speed = std::sqrt((drive.wind.x * drive.wind.x) + (drive.wind.z * drive.wind.z));
    const f32 low_speed = std::sqrt((low.x * low.x) + (low.z * low.z));
    drive.shear_per_km = (high_speed - low_speed) / altitude_km;
    return drive;
}

PrecipitationPlan WeatherSystem::precipitation_plan(const world::WorldVec3d& at) const noexcept {
    // PRESENTATION, computed FROM authoritative state and read back by nothing. `presentation_` is
    // the only input here a quality tier moves, and it reaches no other function in this module —
    // which is how "reducing them SHALL NOT alter authoritative weather state" is structural rather
    // than disciplined.
    const WeatherCellSample cell = cells_.sample(at, WeatherScale::Local);
    return plan_precipitation(cell.state, cell.state.wind, presentation_);
}

usize WeatherSystem::encoded_state_bytes() const noexcept {
    const usize cells = static_cast<usize>(config_.grid.width) * config_.grid.height;
    // count + global + the grid, plus the storms' own message.
    return 8 + kStateWireBytes + (cells * kStateWireBytes) + 8 +
           (storms_.size() * StormRegistry::encoded_storm_bytes());
}

Status WeatherSystem::encode_state(Array<u8>& out) const noexcept {
    bool ok_flag = true;
    write_u64(out, static_cast<u64>(config_.grid.width) << 32U | config_.grid.height, ok_flag);
    write_state(out, cells_.global_state(), ok_flag);
    for (u32 k = 0; k < config_.grid.height; ++k) {
        for (u32 i = 0; i < config_.grid.width; ++i) {
            const WeatherState* cell =
                cells_.regional_cell(static_cast<i32>(i), static_cast<i32>(k));
            write_state(out, (cell == nullptr) ? WeatherState{} : *cell, ok_flag);
        }
    }
    if (!ok_flag) {
        return fail(ErrorCode::OutOfMemory, "weather: the state encoding ran out of room");
    }
    return storms_.encode(out);
}

Status WeatherSystem::decode_state(Span<const u8> bytes) noexcept {
    if (!configured_) {
        return fail(ErrorCode::Unavailable, "weather: configure the system before decoding state");
    }
    if (bytes.size() < 8 + kStateWireBytes) {
        return fail(ErrorCode::InvalidArgument, "weather: a weather message needs a header");
    }
    const u64 shape = read_u64(bytes, 0);
    if (static_cast<u32>(shape >> 32U) != config_.grid.width ||
        static_cast<u32>(shape & 0xFFFF'FFFFULL) != config_.grid.height) {
        // A snapshot carries STATE, not shape. Refusing here rather than resizing is what keeps its
        // size proportional to the world rather than to the world plus its configuration.
        return fail(ErrorCode::InvalidArgument,
                    "weather: the message was encoded for a different grid");
    }
    const usize cells = static_cast<usize>(config_.grid.width) * config_.grid.height;
    const usize needed = 8 + kStateWireBytes + (cells * kStateWireBytes);
    if (bytes.size() < needed) {
        return fail(ErrorCode::InvalidArgument, "weather: the weather message is short");
    }
    cells_.set_global_state(read_state(bytes, 8));
    for (usize index = 0; index < cells; ++index) {
        if (Status set = cells_.set_regional_cell(
                static_cast<u32>(index),
                read_state(bytes, 8 + kStateWireBytes + (index * kStateWireBytes)));
            !set) {
            return set;
        }
    }
    return storms_.decode(Span<const u8>(bytes.data() + needed, bytes.size() - needed));
}

}  // namespace cy::weather

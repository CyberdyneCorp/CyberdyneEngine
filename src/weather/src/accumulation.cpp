// Wetness and snow: accumulation, decay, and the fields that carry them. M10 task 3.2.

#include <cy/weather/accumulation.h>

#include <cy/weather/sample.h>

#include <algorithm>
#include <cmath>

namespace cy::weather {

namespace {

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

/// Which tile a lattice index belongs to. Floor division; see fields.cpp for why it is spelled out.
[[nodiscard]] i64 tile_of_lattice(i64 lattice) noexcept {
    const auto span = static_cast<i64>(environment::kTileCells);
    return (lattice >= 0) ? (lattice / span) : (((lattice + 1) / span) - 1);
}

[[nodiscard]] Status stage_scalar(environment::FieldWriter& writer, environment::FieldId field,
                                  environment::FieldResidency level, i64 lattice_x, i64 lattice_z,
                                  f32 value) noexcept {
    const auto span = static_cast<i64>(environment::kTileCells);
    const i64 tile_x = tile_of_lattice(lattice_x);
    const i64 tile_z = tile_of_lattice(lattice_z);
    environment::TileAddress address;
    address.field = field;
    address.level = static_cast<u8>(level);
    address.layer = static_cast<u8>(environment::FieldLayer::Delta);
    address.x = static_cast<i32>(tile_x);
    address.z = static_cast<i32>(tile_z);
    if (Status staged = writer.stage(address); !staged) {
        return staged;
    }
    return writer.set(address, static_cast<u32>(lattice_x - (tile_x * span)), 0,
                      static_cast<u32>(lattice_z - (tile_z * span)),
                      environment::FieldValue::scalar(value));
}

}  // namespace

f32 sun_exposure_of(const WeatherState& state, const ShelterSample& shelter) noexcept {
    // Cloud and cover compose multiplicatively: a courtyard under half a canopy on an overcast day
    // gets a quarter of the sun, which is what both of them independently let through.
    return clamp01((1.0F - state.cloud_coverage) * shelter.sky_visibility);
}

AccumulationPoint step_accumulation(const AccumulationModel& model,
                                    const AccumulationPoint& current, const WeatherState& state,
                                    f32 sun, f32 wind_speed, f32 shelter_visibility,
                                    f32 seconds) noexcept {
    AccumulationPoint out = current;
    if (seconds <= 0.0F) {
        return out;
    }
    const PrecipitationProperties properties = precipitation_properties(state.precipitation_type);
    // Only the rain that REACHES the surface wets it. The shelter's sky visibility is that fraction
    // — the occlusion representation, not a ray per drop, which is what precipitation.h exists to
    // provide. "Indoors is dry" is this multiplication.
    const f32 reaching = state.precipitation_mm_per_hour * shelter_visibility;

    // --- Wetness
    // ----------------------------------------------------------------------------------
    //
    // The wetting approaches the conditions' own equilibrium — `wetness_potential_of()`, which is
    // literally the value `EnvironmentSample` reports — and the drying approaches zero. Both are
    // exponential in the gap, so two half steps and one whole step agree, which is what makes the
    // editor's ninety-day advance the runtime's own model rather than a preview of it.
    const f32 target = wetness_potential_of(state) * shelter_visibility;
    const f32 wetting = model.wetting_per_mm_hour * reaching * properties.wetness_yield;
    if (wetting > 0.0F) {
        const f32 fraction = 1.0F - std::exp(-wetting * seconds);
        out.wetness += (target - out.wetness) * fraction;
    }
    const f32 warmth = std::max(0.0F, state.temperature_celsius);
    const f32 evaporation = model.evaporation_base *
                            (1.0F + (model.evaporation_per_celsius * warmth)) *
                            (1.0F + (model.evaporation_sun_gain * sun)) *
                            (1.0F + (model.evaporation_wind_gain * wind_speed));
    if (evaporation > 0.0F) {
        // Drying never goes below the standing target: a surface in a downpour does not dry.
        const f32 floor_value = (wetting > 0.0F) ? target : 0.0F;
        const f32 fraction = 1.0F - std::exp(-evaporation * seconds);
        out.wetness -= (out.wetness - floor_value) * fraction;
    }
    out.wetness = clamp01(out.wetness);

    // --- Snow
    // -------------------------------------------------------------------------------------
    if (properties.frozen && reaching > 0.0F) {
        out.snow_depth_metres += model.snow_per_mm_hour * reaching * seconds;
    }
    const f32 above_melting = state.temperature_celsius - model.melting_point_celsius;
    if (above_melting > 0.0F && out.snow_depth_metres > 0.0F) {
        const f32 melt =
            model.melt_per_celsius * above_melting * (1.0F + (model.melt_sun_gain * sun));
        out.snow_depth_metres = std::max(0.0F, out.snow_depth_metres - (melt * seconds));
        // Melting snow wets what is under it, which is why a thaw leaves a wet road rather than a
        // dry one. It is the same wetness field, so a material needs no third quantity.
        out.wetness = clamp01(out.wetness + (melt * seconds * 60.0F));
    }
    return out;
}

Accumulation::Accumulation(Allocator& allocator) noexcept : allocator_(&allocator) {}

Status Accumulation::step_point(environment::FieldWriter& wetness, environment::FieldWriter& snow,
                                WeatherFields& fields, const EnvironmentSampler& sampler,
                                environment::FieldResidency level, i64 lattice_x, i64 lattice_z,
                                f32 cell_metres, f32 seconds, AccumulationReport& report) noexcept {
    const f64 x = (static_cast<f64>(lattice_x) + 0.5) * static_cast<f64>(cell_metres);
    const f64 z = (static_cast<f64>(lattice_z) + 0.5) * static_cast<f64>(cell_metres);
    const world::WorldVec3d at{x, 0.0, z};

    const EnvironmentSample sample =
        sampler.sample(at, SampleQuality::Gameplay, determinism::SimulationClass::Persistent);
    const ShelterSample shelter =
        (occlusion_ == nullptr) ? ShelterSample{} : occlusion_->shelter_at(at);

    // THE FIELD IS THE STATE. The current values are read back out of the store rather than kept in
    // a shadow copy here, which is what makes "a region that was snowed on and unloaded retains its
    // snow" a consequence of the substrate's persistence rather than of a second record.
    environment::FieldStore* store = fields.store();
    AccumulationPoint current;
    current.wetness = store->sample_at(fields.id(WeatherField::Wetness), at, level).value.x();
    current.snow_depth_metres =
        store->sample_at(fields.id(WeatherField::SnowDepth), at, level).value.x();

    WeatherState state;
    state.temperature_celsius = sample.temperature_celsius;
    state.humidity = sample.humidity;
    state.cloud_coverage = sample.cloud_coverage;
    state.precipitation_mm_per_hour = sample.precipitation_mm_per_hour;
    state.precipitation_type = sample.precipitation_type;

    const Vec3 wind = sample.wind.authoritative();
    const f32 wind_speed = std::sqrt((wind.x * wind.x) + (wind.z * wind.z));
    const f32 sun = sun_exposure_of(state, shelter);
    const AccumulationPoint next =
        step_accumulation(model_, current, state, sun, wind_speed, shelter.sky_visibility, seconds);

    f32 published_wetness = next.wetness;
    if (fields.options().wetness == WetnessSource::ComposeShore) {
        // COMPOSE, do not overwrite. src/water/ publishes `water-shore-wetness` when it does not
        // own `wetness`; the maximum is the composition, and it is the rule both rows independently
        // chose for the same reason — a shore that is wet from the sea and wet from the rain is
        // wet, not twice wet. See fields.h's header note.
        const environment::FieldId shore = environment::field_id(fields::kShoreWetness);
        if (store->registry().declaration(shore) != nullptr) {
            published_wetness =
                std::max(published_wetness, store->sample_at(shore, at, level).value.x());
        }
    }

    if (next.wetness > current.wetness) {
        ++report.wetted;
    } else if (next.wetness < current.wetness) {
        ++report.dried;
    }
    if (next.snow_depth_metres > current.snow_depth_metres) {
        ++report.snowed;
    } else if (next.snow_depth_metres < current.snow_depth_metres) {
        ++report.melted;
    }
    ++report.lattice_points;

    if (Status staged = stage_scalar(wetness, fields.id(WeatherField::Wetness), level, lattice_x,
                                     lattice_z, published_wetness);
        !staged) {
        return staged;
    }
    return stage_scalar(snow, fields.id(WeatherField::SnowDepth), level, lattice_x, lattice_z,
                        next.snow_depth_metres);
}

Expected<AccumulationReport, Error> Accumulation::advance(WeatherFields& fields,
                                                          const EnvironmentSampler& sampler,
                                                          const PublishRegion& region,
                                                          f32 seconds) noexcept {
    AccumulationReport report;
    if (!fields.claimed() || fields.store() == nullptr) {
        return fail(ErrorCode::Unavailable,
                    "weather: accumulation needs the weather fields claimed");
    }
    if (!fields.declared(WeatherField::Wetness) || !fields.declared(WeatherField::SnowDepth)) {
        // A configuration that declined the accumulation half. Not an error: a project whose water
        // row owns wetness outright says so by clearing `WeatherFieldOptions::accumulation`.
        return report;
    }
    if (!region.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "weather: an empty region accumulates nothing");
    }
    if (seconds <= 0.0F) {
        return report;
    }

    const environment::ProducerToken* wetness_token = fields.token(WeatherField::Wetness);
    const environment::ProducerToken* snow_token = fields.token(WeatherField::SnowDepth);
    if (wetness_token == nullptr || snow_token == nullptr) {
        return fail(ErrorCode::PermissionDenied,
                    "weather: accumulation needs producer tokens for wetness and snow depth");
    }
    Expected<environment::FieldWriter, Error> wetness = fields.store()->open_writer(*wetness_token);
    if (!wetness) {
        return make_unexpected(wetness.error());
    }
    Expected<environment::FieldWriter, Error> snow = fields.store()->open_writer(*snow_token);
    if (!snow) {
        return make_unexpected(snow.error());
    }

    const f32 cell_metres = fields.level_metres(region.level);
    const auto metres = static_cast<f64>(cell_metres);
    const auto min_i = static_cast<i64>(std::floor(region.min_x / metres));
    const auto max_i = static_cast<i64>(std::floor(region.max_x / metres));
    const auto min_k = static_cast<i64>(std::floor(region.min_z / metres));
    const auto max_k = static_cast<i64>(std::floor(region.max_z / metres));
    for (i64 k = min_k; k <= max_k; ++k) {
        for (i64 i = min_i; i <= max_i; ++i) {
            if (Status stepped = step_point(*wetness, *snow, fields, sampler, region.level, i, k,
                                            cell_metres, seconds, report);
                !stepped) {
                return make_unexpected(stepped.error());
            }
        }
    }
    if (Status published = wetness->publish(); !published) {
        return make_unexpected(published.error());
    }
    if (Status published = snow->publish(); !published) {
        return make_unexpected(published.error());
    }
    return report;
}

}  // namespace cy::weather

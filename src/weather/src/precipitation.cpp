// Precipitation: the types, the occlusion that answers "am I sheltered", and the tiered plan.
// M10 task 3.2.

#include <cy/weather/precipitation.h>

#include <algorithm>
#include <cmath>

namespace cy::weather {

namespace {

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] u64 cover_key(i64 i, i64 k) noexcept {
    return (static_cast<u64>(static_cast<u32>(static_cast<i32>(i))) << 32U) |
           static_cast<u64>(static_cast<u32>(static_cast<i32>(k)));
}

}  // namespace

PrecipitationProperties precipitation_properties(PrecipitationType type) noexcept {
    switch (type) {
        case PrecipitationType::None:
            return {0.0F, 0.0F, 0.0F, 0.0F, false};
        case PrecipitationType::Rain:
            return {7.0F, 1.0F, 0.0F, 900.0F, false};
        case PrecipitationType::Snow:
            // Slow, and it is the one type that piles up. `depth_yield` is metres of depth per
            // millimetre per hour per second, at the ten-to-one fresh-snow ratio.
            return {1.2F, 0.25F, 2.8e-6F, 2'400.0F, true};
        case PrecipitationType::Hail:
            return {14.0F, 0.6F, 4.0e-7F, 1'400.0F, true};
        case PrecipitationType::Ash:
            return {0.6F, 0.0F, 1.0e-6F, 3'200.0F, false};
        case PrecipitationType::Dust:
            return {0.4F, 0.0F, 2.0e-7F, 4'000.0F, false};
        default:
            return {5.0F, 0.5F, 0.0F, 1'000.0F, false};
    }
}

PrecipitationType precipitation_type_for(f32 temperature_celsius,
                                         PrecipitationType storm_type) noexcept {
    // A storm that declares a type owns it: an ash fall and a sandstorm are not decided by how cold
    // it is. Everything else is the freezing line.
    if (storm_type == PrecipitationType::Ash || storm_type == PrecipitationType::Dust ||
        storm_type == PrecipitationType::Hail) {
        return storm_type;
    }
    return (temperature_celsius <= 0.4F) ? PrecipitationType::Snow : PrecipitationType::Rain;
}

SkyOcclusion::SkyOcclusion(Allocator& allocator) noexcept
    : allocator_(&allocator), cells_(allocator) {}

Status SkyOcclusion::configure(f32 cell_metres) noexcept {
    if (cell_metres <= 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "weather: the occlusion raster needs a positive cell size");
    }
    cells_.clear();
    cell_metres_ = cell_metres;
    return ok();
}

Status SkyOcclusion::add_cover(f64 min_x, f64 min_z, f64 max_x, f64 max_z, f32 height,
                               f32 occlusion) noexcept {
    if (max_x <= min_x || max_z <= min_z) {
        return fail(ErrorCode::InvalidArgument, "weather: a cover needs a positive extent");
    }
    const auto metres = static_cast<f64>(cell_metres_);
    const auto first_i = static_cast<i64>(std::floor(min_x / metres));
    const auto last_i = static_cast<i64>(std::floor((max_x - 1e-6) / metres));
    const auto first_k = static_cast<i64>(std::floor(min_z / metres));
    const auto last_k = static_cast<i64>(std::floor((max_z - 1e-6) / metres));
    for (i64 k = first_k; k <= last_k; ++k) {
        for (i64 i = first_i; i <= last_i; ++i) {
            const u64 key = cover_key(i, k);
            SkyCoverCell* existing = cells_.find(key);
            if (existing == nullptr) {
                SkyCoverCell cell;
                cell.cover_height = height;
                cell.occlusion = clamp01(occlusion);
                if (Expected<SkyCoverCell*, Error> inserted = cells_.insert(key, cell); !inserted) {
                    return make_unexpected(inserted.error());
                }
                continue;
            }
            // Two covers over one cell: the LOWEST wins for the height — a position under a bridge
            // under a roof is sheltered by the bridge — and the occlusions compose as one minus the
            // product, which is what two independent screens actually let through.
            existing->cover_height = std::min(existing->cover_height, height);
            existing->occlusion =
                1.0F - ((1.0F - existing->occlusion) * (1.0F - clamp01(occlusion)));
        }
    }
    return ok();
}

void SkyOcclusion::clear() noexcept {
    cells_.clear();
}

ShelterSample SkyOcclusion::shelter_at(const world::WorldVec3d& at) const noexcept {
    ++queries_;
    ShelterSample out;
    const auto metres = static_cast<f64>(cell_metres_);
    const auto i = static_cast<i64>(std::floor(at.x / metres));
    const auto k = static_cast<i64>(std::floor(at.z / metres));
    const SkyCoverCell* cell = cells_.find(cover_key(i, k));
    if (cell == nullptr || at.y > static_cast<f64>(cell->cover_height)) {
        return out;
    }
    out.sheltered = true;
    out.sky_visibility = 1.0F - cell->occlusion;
    out.cover_height_metres = cell->cover_height - static_cast<f32>(at.y);
    return out;
}

const char* precipitation_tier_name(PrecipitationTier tier) noexcept {
    switch (tier) {
        case PrecipitationTier::Particles:
            return "particles";
        case PrecipitationTier::Approximation:
            return "approximation";
        case PrecipitationTier::FieldOnly:
            return "field-only";
        default:
            return "unknown";
    }
}

PrecipitationPlan plan_precipitation(const WeatherState& state, const Vec2& wind,
                                     const PrecipitationLevers& levers) noexcept {
    PrecipitationPlan plan;
    plan.rate_mm_per_hour = state.precipitation_mm_per_hour;
    plan.type = state.precipitation_type;

    plan.tier_start_metres[0] = 0.0F;
    plan.tier_end_metres[0] = levers.particle_distance_metres;
    plan.tier_start_metres[1] = levers.particle_distance_metres;
    plan.tier_end_metres[1] = levers.approximation_distance_metres;
    plan.tier_start_metres[2] = levers.approximation_distance_metres;
    plan.tier_end_metres[2] = 0.0F;  // unbounded: field state carries the rest of the world

    if (state.precipitation_type == PrecipitationType::None ||
        state.precipitation_mm_per_hour <= 0.0F) {
        return plan;
    }

    // THE CAP IS THE REQUIREMENT. "WHEN heavy rain falls across a large view THEN it SHALL be
    // rendered in tiers rather than as millions of simulated drops": the particle count saturates
    // at `max_particles` however hard it rains, and the extra rate is carried by the approximation
    // tier's density instead. `test_precipitation.cpp` raises the rate a hundredfold and requires
    // the particle count not to pass the cap.
    // The apparent density the rate asks for, UNCLAMPED — twenty-five millimetres an hour is a
    // full complement of particles and a hundred asks for four times that. Clamping here instead
    // would make `max_particles` dead code that reads like a cap, which is worse than no cap: a
    // reviewer would believe it and a mutation of it would not be caught.
    const f32 density = state.precipitation_mm_per_hour / 25.0F;
    const auto wanted = static_cast<f32>(levers.max_particles) * density * levers.density_scale;
    const auto capped = static_cast<u32>(
        std::min(static_cast<f32>(levers.max_particles), wanted < 0.0F ? 0.0F : wanted));
    plan.particles[static_cast<u32>(PrecipitationTier::Particles)] = capped;
    plan.approximation_density = clamp01(state.precipitation_mm_per_hour / 60.0F);

    // The slant: how far the wind carries a particle sideways per metre of fall. One number, shared
    // by every tier, so the near rain and the distant sheet lean the same way.
    const PrecipitationProperties properties = precipitation_properties(state.precipitation_type);
    if (properties.fall_speed > 0.01F) {
        plan.slant = Vec2{wind.x / properties.fall_speed, wind.y / properties.fall_speed};
    }
    return plan;
}

InteractionRates interaction_rates(const WeatherState& state, const ShelterSample& shelter,
                                   f32 wetness) noexcept {
    InteractionRates out;
    const f32 exposed = shelter.sky_visibility;
    // SPLASHES scale with the rain that actually reaches the ground — the occlusion, not a
    // collision — and with how wet the ground already is, because a dry dusty surface does not
    // splash. Both inputs are the ones the specification names.
    out.splash_rate = state.precipitation_mm_per_hour * exposed * (0.3F + (0.7F * wetness)) * 0.8F;

    // DRIPS come from the edge of cover and need the opposite: rain above, shelter below, and a wet
    // surface to drip off. A position in the open drips from nothing.
    if (shelter.sheltered) {
        out.drip_rate = state.precipitation_mm_per_hour * (1.0F - exposed) * wetness * 0.35F;
    }
    out.impact_strength = clamp01(state.precipitation_mm_per_hour * exposed * 0.08F);
    return out;
}

}  // namespace cy::weather

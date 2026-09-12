// The weather cells: the hierarchy, the relaxation, the advection and the terrain. M10 task 3.1.

#include <cy/weather/cells.h>

#include <cy/core/math/scalar.h>
#include <cy/weather/storm.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cy::weather {

namespace {

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] f32 approach(f32 current, f32 target, f32 fraction) noexcept {
    return current + ((target - current) * fraction);
}

/// The key a local cell is indexed by. Two 32-bit coordinates folded into one 64-bit key, which is
/// exact: a local lattice of 2^32 cells per axis is past the point where f64 world coordinates are
/// the constraint.
[[nodiscard]] u64 local_key(i32 i, i32 k) noexcept {
    return (static_cast<u64>(static_cast<u32>(i)) << 32U) | static_cast<u64>(static_cast<u32>(k));
}

}  // namespace

const char* precipitation_type_name(PrecipitationType type) noexcept {
    switch (type) {
        case PrecipitationType::None:
            return "none";
        case PrecipitationType::Rain:
            return "rain";
        case PrecipitationType::Snow:
            return "snow";
        case PrecipitationType::Hail:
            return "hail";
        case PrecipitationType::Ash:
            return "ash";
        case PrecipitationType::Dust:
            return "dust";
        default:
            return "project";
    }
}

const char* weather_scale_name(WeatherScale scale) noexcept {
    switch (scale) {
        case WeatherScale::Global:
            return "global";
        case WeatherScale::Regional:
            return "regional";
        case WeatherScale::Local:
            return "local";
        default:
            return "unknown";
    }
}

bool WeatherGridConfig::is_valid() const noexcept {
    return regional_cell_metres > 0.0F && width > 0 && height > 0 && refine_ratio >= 1 &&
           macro_step_seconds > 0.0F && relaxation_per_second > 0.0F;
}

bool WeatherGridConfig::aligns_with(const world::PartitionConfig& partition) const noexcept {
    // Alignment is a coincidence this reports, never a property anything requires. The origins must
    // coincide AND one cell size must be an integer multiple of the other; anything less and a
    // weather cell boundary falls inside a world cell, which is the ordinary case and is fine.
    const f64 world_cell = partition.cell_size(0);
    if (world_cell <= 0.0) {
        return false;
    }
    if (origin_x != partition.origin.x || origin_z != partition.origin.z) {
        return false;
    }
    const f64 ratio = static_cast<f64>(regional_cell_metres) / world_cell;
    const f64 inverse = world_cell / static_cast<f64>(regional_cell_metres);
    const f64 ratio_error = ratio - std::floor(ratio + 0.5);
    const f64 inverse_error = inverse - std::floor(inverse + 0.5);
    return (ratio >= 1.0 && std::fabs(ratio_error) < 1e-9) ||
           (inverse >= 1.0 && std::fabs(inverse_error) < 1e-9);
}

WeatherCells::WeatherCells(Allocator& allocator) noexcept
    : allocator_(&allocator),
      climate_(allocator),
      regional_(allocator),
      scratch_(allocator),
      local_(allocator),
      local_index_(allocator),
      sources_(allocator) {}

Status WeatherCells::configure(const WeatherGridConfig& config, const ClimateMap& climate,
                               u64 seed) noexcept {
    if (!config.is_valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "weather: the grid needs a positive cell size, extent, step and relaxation");
    }
    const usize count = static_cast<usize>(config.width) * config.height;
    if (Status sized = climate_.resize(count); !sized) {
        return sized;
    }
    if (Status sized = regional_.resize(count); !sized) {
        return sized;
    }
    if (Status sized = scratch_.resize(count); !sized) {
        return sized;
    }
    config_ = config;
    seed_ = seed;
    local_.clear();
    local_index_.clear();
    macro_steps_ = 0;
    carry_ = 0.0;

    // THE ONE PLACE CLIMATE REACHES THE CURRENT STATE. Every cell is seeded here and the map is not
    // consulted again — see climate.h's header note, and `climate_at()` below, which is how the
    // wind's prevailing term reads the climate without touching the map.
    f32 mean_temperature = 0.0F;
    f32 mean_humidity = 0.0F;
    Vec2 mean_wind{0.0F, 0.0F};
    const auto cell_metres = static_cast<f64>(config_.regional_cell_metres);
    for (u32 k = 0; k < config_.height; ++k) {
        for (u32 i = 0; i < config_.width; ++i) {
            const usize index = (static_cast<usize>(k) * config_.width) + i;
            const f64 x = config_.origin_x + (static_cast<f64>(i) * cell_metres);
            const f64 z = config_.origin_z + (static_cast<f64>(k) * cell_metres);
            climate_[index] = climate.sample(x, z);
            regional_[index] = climate_target(static_cast<u32>(index));
            mean_temperature += climate_[index].mean_temperature_celsius;
            mean_humidity += climate_[index].humidity;
            mean_wind.x += climate_[index].prevailing_wind.x;
            mean_wind.y += climate_[index].prevailing_wind.y;
        }
    }
    const auto divisor = static_cast<f32>(count);
    global_ = WeatherState{};
    global_.temperature_celsius = mean_temperature / divisor;
    global_.humidity = mean_humidity / divisor;
    global_.wind = Vec2{mean_wind.x / divisor, mean_wind.y / divisor};
    global_target_ = global_;
    configured_ = true;
    return ok();
}

WeatherState WeatherCells::climate_target(u32 index) const noexcept {
    const ClimateSample& climate = climate_[index];
    WeatherState state;
    state.temperature_celsius = climate.mean_temperature_celsius;
    state.humidity = climate.humidity;
    state.pressure_hpa = 1013.25F;
    state.wind = climate.prevailing_wind;
    state.vertical_wind = 0.0F;
    // Cloud and rain follow the climate's own humidity and rainfall potential: a rainforest cell is
    // cloudy at rest and a desert cell is not, without anyone painting either.
    state.cloud_coverage = clamp01(climate.humidity * 0.8F);
    state.precipitation_mm_per_hour = climate.rainfall_potential_mm / 8760.0F;
    state.precipitation_type =
        (state.temperature_celsius <= 0.4F) ? PrecipitationType::Snow : PrecipitationType::Rain;
    if (state.precipitation_mm_per_hour < 0.005F) {
        state.precipitation_type = PrecipitationType::None;
    }
    state.visibility_metres = 40'000.0F * (1.0F - (0.5F * climate.humidity));
    return state;
}

ClimateSample WeatherCells::climate_at(f64 x, f64 z) const noexcept {
    if (!configured_ || climate_.empty()) {
        return ClimateSample{};
    }
    const i32 i = cell_x_of(x);
    const i32 k = cell_z_of(z);
    return climate_[(static_cast<usize>(k) * config_.width) + static_cast<usize>(i)];
}

i32 WeatherCells::cell_x_of(f64 x) const noexcept {
    const f64 fi = ((x - config_.origin_x) / static_cast<f64>(config_.regional_cell_metres)) + 0.5;
    const auto i = static_cast<i64>(std::floor(fi));
    return static_cast<i32>(std::clamp<i64>(i, 0, static_cast<i64>(config_.width) - 1));
}

i32 WeatherCells::cell_z_of(f64 z) const noexcept {
    const f64 fk = ((z - config_.origin_z) / static_cast<f64>(config_.regional_cell_metres)) + 0.5;
    const auto k = static_cast<i64>(std::floor(fk));
    return static_cast<i32>(std::clamp<i64>(k, 0, static_cast<i64>(config_.height) - 1));
}

Status WeatherCells::set_regional_cell(u32 index, const WeatherState& state) noexcept {
    if (index >= regional_.size()) {
        return fail(ErrorCode::OutOfRange, "weather: no regional cell with that index");
    }
    regional_[index] = state;
    return ok();
}

const WeatherState* WeatherCells::regional_cell(i32 i, i32 k) const noexcept {
    if (!configured_ || i < 0 || k < 0 || std::cmp_greater_equal(i, config_.width) ||
        std::cmp_greater_equal(k, config_.height)) {
        return nullptr;
    }
    return &regional_[(static_cast<usize>(k) * config_.width) + static_cast<usize>(i)];
}

Status WeatherCells::set_refinement_sources(Span<const RefinementSource> sources) noexcept {
    if (Status sized = sources_.resize(sources.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < sources.size(); ++index) {
        sources_[index] = sources[index];
    }
    return ok();
}

f32 WeatherCells::uplift_at(f64 x, f64 z, const Vec2& wind) const noexcept {
    if (!terrain_.installed()) {
        return 0.0F;
    }
    // The slope ALONG the wind, sampled over one regional cell. Sampling over a shorter baseline
    // would make the uplift a function of the terrain's own noise rather than of the range the wind
    // has to climb, and the rain shadow is a property of the range.
    const f64 step = static_cast<f64>(config_.regional_cell_metres) * 0.5;
    const f32 speed = std::sqrt((wind.x * wind.x) + (wind.y * wind.y));
    if (speed < 0.01F) {
        return 0.0F;
    }
    const f64 dx = static_cast<f64>(wind.x / speed) * step;
    const f64 dz = static_cast<f64>(wind.y / speed) * step;
    const f64 behind = terrain_.elevation(x - dx, z - dz);
    const f64 ahead = terrain_.elevation(x + dx, z + dz);
    const auto slope = static_cast<f32>((ahead - behind) / (2.0 * step));
    return orographic_.uplift_gain * speed * slope;
}

void WeatherCells::apply_terrain(u32 index, WeatherState& target) const noexcept {
    const u32 i = index % config_.width;
    const u32 k = index / config_.width;
    const auto cell_metres = static_cast<f64>(config_.regional_cell_metres);
    const f64 x = config_.origin_x + (static_cast<f64>(i) * cell_metres);
    const f64 z = config_.origin_z + (static_cast<f64>(k) * cell_metres);

    const f32 uplift = uplift_at(x, z, target.wind);
    target.vertical_wind = uplift;

    if (uplift > 0.0F) {
        // WINDWARD. Air forced up cools, condenses and rains, and the humidity it loses is what
        // leaves the far side dry. The two are one term: the rain added here IS the humidity
        // removed, which is why a leeward cell needs no rule of its own beyond the descent below.
        const f32 gain = orographic_.orographic_rain_gain * uplift * target.humidity;
        target.precipitation_mm_per_hour += gain;
        target.humidity = clamp01(target.humidity - (orographic_.humidity_loss_per_mm * gain));
        if (target.precipitation_type == PrecipitationType::None &&
            target.precipitation_mm_per_hour > 0.01F) {
            target.precipitation_type = (target.temperature_celsius <= 0.4F)
                                            ? PrecipitationType::Snow
                                            : PrecipitationType::Rain;
        }
    } else if (uplift < 0.0F) {
        // LEE. Descending air warms and its relative humidity falls, so what rain is left is
        // suppressed. This is the rain SHADOW, and it reaches further than the slope because the
        // air arrived already wrung out by the windward side above.
        const f32 suppression = 1.0F / (1.0F + (orographic_.rain_shadow_gain * (-uplift)));
        target.precipitation_mm_per_hour *= suppression;
        target.humidity *= suppression;
    }

    // The lapse rate between the grid's reference and the ground, applied to the TARGET so that
    // everything reading the cell — rain-or-snow, melt, the accumulation — agrees about how cold
    // the mountain is.
    const auto elevation_km = static_cast<f32>(terrain_.elevation(x, z) / 1000.0);
    target.temperature_celsius -= orographic_.lapse_rate_per_km * elevation_km;
}

void WeatherCells::step_global(f32 dt) noexcept {
    const f32 fraction = 1.0F - std::exp(-config_.relaxation_per_second * dt);
    global_.temperature_celsius =
        approach(global_.temperature_celsius, global_target_.temperature_celsius, fraction);
    global_.humidity = approach(global_.humidity, global_target_.humidity, fraction);
    global_.pressure_hpa = approach(global_.pressure_hpa, global_target_.pressure_hpa, fraction);
    global_.wind.x = approach(global_.wind.x, global_target_.wind.x, fraction);
    global_.wind.y = approach(global_.wind.y, global_target_.wind.y, fraction);
    global_.cloud_coverage =
        approach(global_.cloud_coverage, global_target_.cloud_coverage, fraction);
    global_.precipitation_mm_per_hour = approach(
        global_.precipitation_mm_per_hour, global_target_.precipitation_mm_per_hour, fraction);
    global_.visibility_metres =
        approach(global_.visibility_metres, global_target_.visibility_metres, fraction);
    global_.precipitation_type = global_target_.precipitation_type;
}

/// The target one regional cell relaxes toward: its own climate, informed by the global tendency.
///
/// The specification's hierarchy in one expression — "global and continental tendencies inform
/// regional conditions". The weights are the informing: temperature and humidity are half the
/// place's own and half the world's mood, cloud and rain lean on the global term because that is
/// what a preset moves, and the wind leans on it because a front crosses a map as one wind. The
/// climate share is what keeps a desert cell drier than a forest cell under one storm preset — a
/// target that was purely global would make every cell the same weather.
WeatherState WeatherCells::regional_target(const WeatherState& climate) const noexcept {
    WeatherState target = climate;
    target.temperature_celsius =
        (climate.temperature_celsius * 0.5F) + (global_.temperature_celsius * 0.5F);
    target.humidity = clamp01((climate.humidity * 0.5F) + (global_.humidity * 0.5F));
    target.cloud_coverage =
        clamp01((climate.cloud_coverage * 0.35F) + (global_.cloud_coverage * 0.65F));
    target.precipitation_mm_per_hour =
        (climate.precipitation_mm_per_hour * 0.25F) + (global_.precipitation_mm_per_hour * 0.75F);
    target.precipitation_type = (global_.precipitation_type == PrecipitationType::None)
                                    ? climate.precipitation_type
                                    : global_.precipitation_type;
    target.wind.x = (climate.wind.x * 0.4F) + (global_.wind.x * 0.6F);
    target.wind.y = (climate.wind.y * 0.4F) + (global_.wind.y * 0.6F);
    target.pressure_hpa = global_.pressure_hpa;
    target.visibility_metres =
        (climate.visibility_metres * 0.4F) + (global_.visibility_metres * 0.6F);
    return target;
}

/// The storms' contribution to a cell's TARGET.
///
/// It goes into the target rather than onto the state for a reason worth stating plainly: a
/// contribution added to the state after every relaxation step is AMPLIFIED by the reciprocal of
/// the relaxation fraction, because the state relaxes away from it and it is added again next step.
/// A storm worth three metres per second of wind settles at thirty, and the sign of the cell's wind
/// flips under a storm whose inflow opposes the prevailing. Putting it in the target gives the
/// storm its own magnitude at equilibrium; and a storm that has moved on simply stops appearing in
/// the target, so the cell relaxes back without anything having to remember it.
void WeatherCells::apply_storms(u32 index, WeatherState& target) const noexcept {
    if (storms_ == nullptr) {
        return;
    }
    const u32 i = index % config_.width;
    const u32 k = index / config_.width;
    const auto cell_metres = static_cast<f64>(config_.regional_cell_metres);
    const f64 x = config_.origin_x + (static_cast<f64>(i) * cell_metres);
    const f64 z = config_.origin_z + (static_cast<f64>(k) * cell_metres);
    const StormContribution storm = storms_->contribution_at(x, z);
    if (storm.influence <= 0.0F) {
        return;
    }
    target.pressure_hpa += storm.pressure_delta;
    target.wind.x += storm.wind.x;
    target.wind.y += storm.wind.y;
    target.precipitation_mm_per_hour += storm.precipitation_mm_per_hour;
    target.cloud_coverage = clamp01(target.cloud_coverage + storm.cloud_coverage);
    target.visibility_metres =
        std::max(50.0F, target.visibility_metres - storm.visibility_loss_metres);
    if (storm.precipitation_mm_per_hour > 0.01F &&
        target.precipitation_type == PrecipitationType::None) {
        target.precipitation_type = (target.temperature_celsius <= 0.4F) ? PrecipitationType::Snow
                                                                         : PrecipitationType::Rain;
    }
}

void WeatherCells::step_regional(f32 dt) noexcept {
    const f32 fraction = 1.0F - std::exp(-config_.relaxation_per_second * dt);
    const usize count = regional_.size();
    for (usize index = 0; index < count; ++index) {
        WeatherState& state = regional_[index];

        // ONE TARGET, BUILT IN THE HIERARCHY'S ORDER, then one relaxation toward it. Climate
        // informed by the global tendency, the storms over it, the terrain under it — and nothing
        // is written onto the state except through the relaxation, which is what stops any
        // contribution from accumulating across steps.
        WeatherState target = regional_target(climate_target(static_cast<u32>(index)));
        apply_storms(static_cast<u32>(index), target);
        apply_terrain(static_cast<u32>(index), target);

        state.temperature_celsius =
            approach(state.temperature_celsius, target.temperature_celsius, fraction);
        state.humidity = clamp01(approach(state.humidity, target.humidity, fraction));
        state.pressure_hpa = approach(state.pressure_hpa, target.pressure_hpa, fraction);
        state.wind.x = approach(state.wind.x, target.wind.x, fraction);
        state.wind.y = approach(state.wind.y, target.wind.y, fraction);
        state.vertical_wind = approach(state.vertical_wind, target.vertical_wind, fraction);
        state.cloud_coverage =
            clamp01(approach(state.cloud_coverage, target.cloud_coverage, fraction));
        state.precipitation_mm_per_hour =
            approach(state.precipitation_mm_per_hour, target.precipitation_mm_per_hour, fraction);
        state.visibility_metres =
            approach(state.visibility_metres, target.visibility_metres, fraction);
        if (state.precipitation_mm_per_hour < 0.005F) {
            state.precipitation_type = PrecipitationType::None;
        } else {
            state.precipitation_type = target.precipitation_type;
        }
    }
}

void WeatherCells::advect(f32 dt) noexcept {
    // SEMI-LAGRANGIAN: every cell asks where the air over it came from and takes that state. It is
    // unconditionally stable — a wind that crosses ten cells in one step is fine — which is what
    // lets `advance_days()` run hour-long steps without the model exploding. An Eulerian scheme
    // would need the step bounded by the wind speed, and the editor's fast-forward would then be a
    // different model from the runtime's.
    const usize count = regional_.size();
    for (usize index = 0; index < count; ++index) {
        scratch_[index] = regional_[index];
    }
    const f32 cell = config_.regional_cell_metres;
    for (u32 k = 0; k < config_.height; ++k) {
        for (u32 i = 0; i < config_.width; ++i) {
            const usize index = (static_cast<usize>(k) * config_.width) + i;
            const WeatherState& here = scratch_[index];
            const f32 back_i = static_cast<f32>(i) - (here.wind.x * dt / cell);
            const f32 back_k = static_cast<f32>(k) - (here.wind.y * dt / cell);
            const auto si = static_cast<i64>(std::floor(back_i + 0.5F));
            const auto sk = static_cast<i64>(std::floor(back_k + 0.5F));
            const i64 ci = std::clamp<i64>(si, 0, static_cast<i64>(config_.width) - 1);
            const i64 ck = std::clamp<i64>(sk, 0, static_cast<i64>(config_.height) - 1);
            const WeatherState& source =
                scratch_[(static_cast<usize>(ck) * config_.width) + static_cast<usize>(ci)];

            // Only the TRANSPORTED quantities move. Temperature and pressure are dominated by the
            // cell's own climate and its storms rather than by what blew in, and advecting them
            // would slide a whole world's temperature downwind over a long fast-forward.
            WeatherState& target = regional_[index];
            target.humidity = source.humidity;
            target.cloud_coverage = source.cloud_coverage;
            target.precipitation_mm_per_hour = source.precipitation_mm_per_hour;
            target.precipitation_type = source.precipitation_type;
        }
    }
}

WeatherState WeatherCells::local_state_of(i32 i, i32 k) const noexcept {
    // A local cell IS its parent regional cell, re-evaluated against the terrain at the finer
    // scale. "local conditions, which are modified by terrain" — so the only thing that differs
    // between a local cell and its parent is what the ground under it does.
    const f32 local_metres = config_.regional_cell_metres / static_cast<f32>(config_.refine_ratio);
    const f64 x = config_.origin_x + (static_cast<f64>(i) * static_cast<f64>(local_metres));
    const f64 z = config_.origin_z + (static_cast<f64>(k) * static_cast<f64>(local_metres));
    const WeatherState* parent = regional_cell(cell_x_of(x), cell_z_of(z));
    WeatherState state = (parent == nullptr) ? global_ : *parent;
    if (!terrain_.installed()) {
        return state;
    }
    const f32 uplift = uplift_at(x, z, state.wind);
    state.vertical_wind = uplift;
    if (uplift > 0.0F) {
        state.precipitation_mm_per_hour +=
            orographic_.orographic_rain_gain * uplift * state.humidity;
    } else if (uplift < 0.0F) {
        state.precipitation_mm_per_hour /= (1.0F + (orographic_.rain_shadow_gain * (-uplift)));
    }
    const auto elevation_km = static_cast<f32>(terrain_.elevation(x, z) / 1000.0);
    state.temperature_celsius -= orographic_.lapse_rate_per_km * elevation_km;
    return state;
}

/// One candidate local cell: inside the source's radius, not already present, and within budget.
///
/// Split out of `refine()` because the ring walk and the admission are separate claims, and because
/// the admission is the one a reader checks against `max_local_cells`.
Status WeatherCells::place_local(const RefinementSource& source, i64 i, i64 k,
                                 StepReport& report) noexcept {
    const f32 local_metres = config_.regional_cell_metres / static_cast<f32>(config_.refine_ratio);
    const auto metres = static_cast<f64>(local_metres);
    const f64 x = config_.origin_x + (static_cast<f64>(i) * metres);
    const f64 z = config_.origin_z + (static_cast<f64>(k) * metres);
    const f64 dx = x - source.x;
    const f64 dz = z - source.z;
    // A CIRCLE, not the bounding square: a source declares a radius, and refining the corners of
    // its box would spend a fifth of the budget on cells it did not ask for.
    const auto radius = static_cast<f64>(source.radius_metres);
    if ((dx * dx) + (dz * dz) > radius * radius) {
        return ok();
    }
    const u64 key = local_key(static_cast<i32>(i), static_cast<i32>(k));
    if (local_index_.contains(key)) {
        return ok();
    }
    if (local_.size() >= config_.max_local_cells) {
        ++report.refinements_dropped;
        return ok();
    }
    LocalCell cell;
    cell.i = static_cast<i32>(i);
    cell.k = static_cast<i32>(k);
    cell.state = local_state_of(cell.i, cell.k);
    if (Status pushed = local_.push_back(cell); !pushed) {
        return pushed;
    }
    if (Expected<usize*, Error> inserted = local_index_.insert(key, local_.size() - 1); !inserted) {
        return make_unexpected(inserted.error());
    }
    return ok();
}

Status WeatherCells::refine(StepReport& report) noexcept {
    local_.clear();
    local_index_.clear();
    if (sources_.empty() || config_.refine_ratio <= 1 || config_.max_local_cells == 0) {
        return ok();
    }
    const f32 local_metres = config_.regional_cell_metres / static_cast<f32>(config_.refine_ratio);
    const auto metres = static_cast<f64>(local_metres);

    // Sources in a DECLARED order: priority first, then the source's index. A budget that refused
    // in array order would refuse differently on a peer that registered its cameras in another
    // order, and the refined cells are read by a gameplay sample.
    Array<u32> order(*allocator_);
    if (Status sized = order.resize(sources_.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < sources_.size(); ++index) {
        order[index] = static_cast<u32>(index);
    }
    std::ranges::sort(order, [this](u32 a, u32 b) {
        if (sources_[a].priority != sources_[b].priority) {
            return sources_[a].priority > sources_[b].priority;
        }
        return a < b;
    });

    for (u32 slot : order.span()) {
        const RefinementSource& source = sources_[slot];
        const auto reach = static_cast<i64>(std::ceil(source.radius_metres / local_metres));
        const auto centre_i =
            static_cast<i64>(std::floor(((source.x - config_.origin_x) / metres) + 0.5));
        const auto centre_k =
            static_cast<i64>(std::floor(((source.z - config_.origin_z) / metres) + 0.5));
        // OUTWARD FROM THE SOURCE, ring by ring. A raster scan of the bounding box spends the
        // budget on the box's top-left corner and leaves the source's own position coarse, which is
        // the opposite of what a refinement source asked for — and it is the shape of bug a test
        // that only counted cells would never see.
        for (i64 ring = 0; ring <= reach; ++ring) {
            for (i64 k = centre_k - ring; k <= centre_k + ring; ++k) {
                for (i64 i = centre_i - ring; i <= centre_i + ring; ++i) {
                    const bool on_ring = (i == centre_i - ring) || (i == centre_i + ring) ||
                                         (k == centre_k - ring) || (k == centre_k + ring);
                    if (!on_ring) {
                        continue;
                    }
                    if (Status placed = place_local(source, i, k, report); !placed) {
                        return placed;
                    }
                }
            }
        }
    }
    report.local_cells = static_cast<u32>(local_.size());
    return ok();
}

Status WeatherCells::step_once(determinism::SimulationPoint at, f32 dt,
                               StepReport& report) noexcept {
    // The moment is carried for the storms' own draws, which `StormRegistry::advance()` makes from
    // it; nothing in the cell step itself is random, deliberately — a grid that drew per cell per
    // step would put 1 024 draws a step into the state hash to no visible end.
    (void)at;
    step_global(dt);
    step_regional(dt);
    advect(dt);
    ++macro_steps_;
    report.cells_stepped += regional_.size();
    return ok();
}

Expected<StepReport, Error> WeatherCells::advance(determinism::SimulationPoint at,
                                                  f64 seconds) noexcept {
    if (!configured_) {
        return fail(ErrorCode::Unavailable, "weather: the cell grid has not been configured");
    }
    StepReport report;
    if (seconds < 0.0) {
        return report;
    }
    // THE CARRY IS WHAT MAKES THE FAST-FORWARD THE RUNTIME'S OWN MODEL. Whole steps only, remainder
    // kept: ninety one-day calls and one ninety-day call run exactly the same number of steps in
    // exactly the same order. See cells.h's note on `advance()`.
    carry_ += seconds;
    const auto step = static_cast<f64>(config_.macro_step_seconds);
    while (carry_ >= step) {
        if (Status stepped = step_once(at, config_.macro_step_seconds, report); !stepped) {
            return make_unexpected(stepped.error());
        }
        carry_ -= step;
        ++report.steps;
        report.seconds_advanced += step;
    }
    if (Status refined = refine(report); !refined) {
        return make_unexpected(refined.error());
    }
    report.cells_stepped += local_.size();
    report.local_cells = static_cast<u32>(local_.size());
    return report;
}

const WeatherCells::LocalCell* WeatherCells::find_local(i32 i, i32 k) const noexcept {
    const usize* slot = local_index_.find(local_key(i, k));
    return (slot == nullptr) ? nullptr : &local_[*slot];
}

WeatherCellSample WeatherCells::sample(f64 x, f64 z, WeatherScale finest) const noexcept {
    WeatherCellSample out;
    if (!configured_) {
        out.state = global_;
        out.scale = WeatherScale::Global;
        out.inside_grid = false;
        return out;
    }
    const auto cell_metres = static_cast<f64>(config_.regional_cell_metres);
    const bool inside = x >= config_.origin_x - (cell_metres * 0.5) &&
                        z >= config_.origin_z - (cell_metres * 0.5) &&
                        x < config_.origin_x + (static_cast<f64>(config_.width) * cell_metres) &&
                        z < config_.origin_z + (static_cast<f64>(config_.height) * cell_metres);

    if (finest == WeatherScale::Local && config_.refine_ratio > 1) {
        const f32 local_metres =
            config_.regional_cell_metres / static_cast<f32>(config_.refine_ratio);
        const auto metres = static_cast<f64>(local_metres);
        const auto i = static_cast<i32>(std::floor(((x - config_.origin_x) / metres) + 0.5));
        const auto k = static_cast<i32>(std::floor(((z - config_.origin_z) / metres) + 0.5));
        if (const LocalCell* cell = find_local(i, k); cell != nullptr) {
            out.state = cell->state;
            out.scale = WeatherScale::Local;
            out.cell_metres = local_metres;
            out.inside_grid = inside;
            return out;
        }
        // "SHALL return the coarsest resident value with a resolution indicator, and SHALL NOT
        // block": no local cell here, so the regional one answers and `scale` says so.
    }

    if (finest != WeatherScale::Global) {
        const WeatherState* cell = regional_cell(cell_x_of(x), cell_z_of(z));
        if (cell != nullptr) {
            out.state = *cell;
            out.scale = WeatherScale::Regional;
            out.cell_metres = config_.regional_cell_metres;
            out.inside_grid = inside;
            return out;
        }
    }
    out.state = global_;
    out.scale = WeatherScale::Global;
    out.cell_metres = 0.0F;
    out.inside_grid = inside;
    return out;
}

WeatherCellSample WeatherCells::sample(const world::WorldVec3d& at,
                                       WeatherScale finest) const noexcept {
    return sample(at.x, at.z, finest);
}

}  // namespace cy::weather

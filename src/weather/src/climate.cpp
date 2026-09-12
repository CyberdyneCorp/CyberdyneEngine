// The climate layer: the tendencies a world has, evaluated rarely. M10 task 3.1.

#include <cy/weather/climate.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cy::weather {

namespace {

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] f32 lerp(f32 a, f32 b, f32 t) noexcept {
    return a + ((b - a) * t);
}

[[nodiscard]] ClimateSample blend(const ClimateSample& a, const ClimateSample& b, f32 t) noexcept {
    ClimateSample out;
    out.mean_temperature_celsius = lerp(a.mean_temperature_celsius, b.mean_temperature_celsius, t);
    out.temperature_range_celsius =
        lerp(a.temperature_range_celsius, b.temperature_range_celsius, t);
    out.humidity = lerp(a.humidity, b.humidity, t);
    out.prevailing_wind.x = lerp(a.prevailing_wind.x, b.prevailing_wind.x, t);
    out.prevailing_wind.y = lerp(a.prevailing_wind.y, b.prevailing_wind.y, t);
    out.rainfall_potential_mm = lerp(a.rainfall_potential_mm, b.rainfall_potential_mm, t);
    out.solar_exposure = lerp(a.solar_exposure, b.solar_exposure, t);
    out.ocean_influence = lerp(a.ocean_influence, b.ocean_influence, t);
    return out;
}

}  // namespace

const char* biome_name(u32 biome) noexcept {
    switch (biome) {
        case biomes::kBarren:
            return "barren";
        case biomes::kDesert:
            return "desert";
        case biomes::kGrassland:
            return "grassland";
        case biomes::kSavanna:
            return "savanna";
        case biomes::kShrubland:
            return "shrubland";
        case biomes::kForest:
            return "forest";
        case biomes::kRainforest:
            return "rainforest";
        case biomes::kTaiga:
            return "taiga";
        case biomes::kTundra:
            return "tundra";
        default:
            return "project";
    }
}

BiomePotential climate_biome_potential(const ClimateSample& climate) noexcept {
    BiomePotential out;

    // Moisture is rainfall against the evaporative demand temperature creates. 800 mm a year in a
    // cold place is a bog and in a hot one is a savanna, which is why this is a ratio rather than
    // the rainfall itself — the same reasoning behind every aridity index in the literature.
    const f32 demand = 250.0F + (38.0F * (climate.mean_temperature_celsius + 10.0F));
    out.moisture = clamp01(climate.rainfall_potential_mm / (demand <= 1.0F ? 1.0F : demand));

    // Vegetation is limited by whichever of moisture and warmth is scarcer: a wet tundra and a hot
    // desert both carry little, for opposite reasons, and a product would make a place that is
    // merely cool and merely dry look like both at once.
    const f32 warmth = clamp01((climate.mean_temperature_celsius + 20.0F) / 38.0F);
    out.vegetation = clamp01((out.moisture < warmth ? out.moisture : warmth) * 1.15F);

    // Whittaker's axes: temperature against moisture. The thresholds are the ones a biome diagram
    // draws and nothing here is tuned to a particular world — a project that wants its own biomes
    // declares `BiomeRule`s over fields (ecosystem.h) rather than editing this.
    const f32 temperature = climate.mean_temperature_celsius;
    if (temperature < -5.0F) {
        out.biome = biomes::kTundra;
    } else if (temperature < 5.0F) {
        out.biome = (out.moisture > 0.35F) ? biomes::kTaiga : biomes::kTundra;
    } else if (out.moisture < 0.12F) {
        out.biome = biomes::kDesert;
    } else if (out.moisture < 0.3F) {
        out.biome = (temperature > 20.0F) ? biomes::kSavanna : biomes::kGrassland;
    } else if (out.moisture < 0.55F) {
        out.biome = (temperature > 22.0F) ? biomes::kSavanna : biomes::kShrubland;
    } else if (temperature > 22.0F && out.moisture > 0.8F) {
        out.biome = biomes::kRainforest;
    } else {
        out.biome = biomes::kForest;
    }
    if (out.vegetation < 0.03F) {
        out.biome = biomes::kBarren;
    }
    return out;
}

ClimateSample derive_climate(const ClimateModel& model, const ClimateTerrain& terrain, f64 x,
                             f64 z) noexcept {
    ClimateSample out;

    const f64 latitude = model.latitude_at_origin + (z / model.metres_per_degree);
    const f64 magnitude = (latitude < 0.0) ? -latitude : latitude;
    const f32 band = clamp01(static_cast<f32>(magnitude / 90.0));

    // Sea-level temperature across the latitude band, then the lapse rate down to the ground. Two
    // separate terms because a mountain at the equator and a plain at the pole are cold for
    // different reasons, and a consumer asking why needs them apart.
    const f32 sea_level =
        lerp(model.equator_temperature_celsius, model.pole_temperature_celsius, band * band);
    const f64 elevation =
        (terrain.elevation_at == nullptr) ? 0.0 : terrain.elevation_at(terrain.user, x, z);
    const auto elevation_km = static_cast<f32>((elevation > 0.0 ? elevation : 0.0) / 1000.0);
    out.mean_temperature_celsius = sea_level - (model.lapse_rate_per_km * elevation_km);

    const f64 ocean = (terrain.ocean_distance_at == nullptr)
                          ? static_cast<f64>(model.ocean_reach_metres)
                          : terrain.ocean_distance_at(terrain.user, x, z);
    out.ocean_influence =
        1.0F - clamp01(static_cast<f32>(ocean) /
                       (model.ocean_reach_metres <= 0.0F ? 1.0F : model.ocean_reach_metres));

    // The sea moderates the annual swing and raises the humidity. Both are the same physical fact —
    // water's heat capacity and its vapour — expressed in the two members a designer reads.
    out.temperature_range_celsius = lerp(22.0F, 6.0F, out.ocean_influence);
    out.humidity = clamp01(lerp(0.35F, 0.8F, out.ocean_influence) * lerp(1.0F, 0.7F, band));

    // Rainfall: the sea supplies it, the latitude bands modulate it (a subtropical high at 30
    // degrees is a desert belt), and elevation adds the orographic share the weather layer will
    // resolve properly cell by cell.
    const f32 belt =
        1.0F - (0.55F * std::exp(-(((band * 90.0F) - 28.0F) * ((band * 90.0F) - 28.0F)) / 200.0F));
    out.rainfall_potential_mm =
        clamp01(out.humidity) * 1600.0F * belt * (1.0F + (0.35F * elevation_km));
    out.solar_exposure = clamp01(0.85F - (0.4F * out.humidity));
    out.prevailing_wind = model.prevailing_wind;
    return out;
}

ClimateMap::ClimateMap(Allocator& allocator) noexcept : allocator_(&allocator), cells_(allocator) {}

Status ClimateMap::set_uniform(const ClimateSample& climate) noexcept {
    cells_.clear();
    uniform_ = climate;
    kind_ = ClimateSourceKind::Uniform;
    width_ = 0;
    height_ = 0;
    return ok();
}

Status ClimateMap::set_grid(f64 origin_x, f64 origin_z, f32 cell_metres, u32 width, u32 height,
                            Span<const ClimateSample> cells) noexcept {
    if (width == 0 || height == 0 || cell_metres <= 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "weather: a climate grid needs a positive size and cell");
    }
    if (cells.size() != static_cast<usize>(width) * height) {
        return fail(ErrorCode::InvalidArgument,
                    "weather: the climate grid's cell count must be width * height");
    }
    if (Status sized = cells_.resize(cells.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < cells.size(); ++index) {
        cells_[index] = cells[index];
    }
    origin_x_ = origin_x;
    origin_z_ = origin_z;
    cell_metres_ = cell_metres;
    width_ = width;
    height_ = height;
    kind_ = ClimateSourceKind::Grid;
    return ok();
}

Status ClimateMap::derive(const ClimateModel& model, const ClimateTerrain& terrain, f64 origin_x,
                          f64 origin_z, f32 cell_metres, u32 width, u32 height) noexcept {
    if (width == 0 || height == 0 || cell_metres <= 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "weather: a derived climate needs a positive size and cell");
    }
    if (Status sized = cells_.resize(static_cast<usize>(width) * height); !sized) {
        return sized;
    }
    // EVERY EVALUATION OF THE DERIVED MODEL HAPPENS HERE, ONCE. See climate.h's header note: the
    // counter below is what a suite requires not to move while the current state is being queried.
    for (u32 k = 0; k < height; ++k) {
        for (u32 i = 0; i < width; ++i) {
            const f64 x = origin_x + (static_cast<f64>(i) * static_cast<f64>(cell_metres));
            const f64 z = origin_z + (static_cast<f64>(k) * static_cast<f64>(cell_metres));
            cells_[(static_cast<usize>(k) * width) + i] = derive_climate(model, terrain, x, z);
            ++evaluations_;
        }
    }
    origin_x_ = origin_x;
    origin_z_ = origin_z;
    cell_metres_ = cell_metres;
    width_ = width;
    height_ = height;
    kind_ = ClimateSourceKind::Derived;
    return ok();
}

ClimateSample ClimateMap::fetch(i64 i, i64 k) const noexcept {
    // Saturating rather than defaulting: a position outside the authored grid gets the nearest
    // authored climate, because a world's climate does not stop at the edge of the map somebody
    // painted.
    const i64 clamped_i = std::clamp<i64>(i, 0, static_cast<i64>(width_) - 1);
    const i64 clamped_k = std::clamp<i64>(k, 0, static_cast<i64>(height_) - 1);
    return cells_[(static_cast<usize>(clamped_k) * width_) + static_cast<usize>(clamped_i)];
}

ClimateSample ClimateMap::sample(f64 x, f64 z) const noexcept {
    // EVERY read of the climate is counted, not only the derived model's own evaluations. The
    // requirement is that climate is not consulted to answer a question about the CURRENT state,
    // and a counter that only saw the expensive path would report clean on a sampler that consulted
    // an authored grid ten thousand times a frame. See climate.h's header note.
    ++evaluations_;
    if (kind_ == ClimateSourceKind::Uniform || width_ == 0 || height_ == 0) {
        return uniform_;
    }
    const f64 fi = (x - origin_x_) / static_cast<f64>(cell_metres_);
    const f64 fk = (z - origin_z_) / static_cast<f64>(cell_metres_);
    const auto i = static_cast<i64>(std::floor(fi));
    const auto k = static_cast<i64>(std::floor(fk));
    const auto tx = static_cast<f32>(fi - static_cast<f64>(i));
    const auto tz = static_cast<f32>(fk - static_cast<f64>(k));
    const ClimateSample low = blend(fetch(i, k), fetch(i + 1, k), tx);
    const ClimateSample high = blend(fetch(i, k + 1), fetch(i + 1, k + 1), tx);
    return blend(low, high, tz);
}

ClimateSample ClimateMap::sample(const world::WorldVec3d& at) const noexcept {
    return sample(at.x, at.z);
}

BiomePotential ClimateMap::potential(f64 x, f64 z) const noexcept {
    return climate_biome_potential(sample(x, z));
}

}  // namespace cy::weather

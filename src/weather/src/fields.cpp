// Weather as a producer into CyberField: the declaration table, the claims, and the publication.
// M10 tasks 3.1 and 3.2.

#include <cy/weather/fields.h>

#include <cy/weather/sample.h>

#include <cmath>
#include <utility>

namespace cy::weather {

namespace {

using environment::FieldDeclaration;
using environment::FieldEncoding;
using environment::FieldInterpolation;
using environment::FieldLayer;
using environment::FieldLevel;
using environment::FieldResidency;
using environment::FieldType;
using environment::FieldValue;

/// Which tile a lattice index belongs to. Floor division, spelled out because C++'s `/` truncates
/// toward zero and a world west of the origin has negative lattice indices — where truncation would
/// put two different columns in one tile. src/water/ spells the same helper for the same reason.
[[nodiscard]] i64 tile_of_lattice(i64 lattice) noexcept {
    const auto span = static_cast<i64>(environment::kTileCells);
    return (lattice >= 0) ? (lattice / span) : (((lattice + 1) / span) - 1);
}

/// The declaration every weather field shares: three resolutions with the macro level resident
/// everywhere, CPU-produced, slowly varying.
///
/// **Macro resident everywhere is not a style choice.** Every authoritative field here is
/// gameplay-visible, and `environment::validate_declaration()` refuses a gameplay-visible field
/// whose `gameplay_level` is not declared `resident_everywhere` — because its value would otherwise
/// depend on what streamed. The ecosystem half needs it for a second reason the specification
/// states outright: state evolves "whether or not a region is resident".
[[nodiscard]] FieldDeclaration base_declaration(const WeatherFieldOptions& options) noexcept {
    FieldDeclaration declaration;
    declaration.type = FieldType::Scalar;
    declaration.encoding = FieldEncoding::UNorm16;
    declaration.interpolation = FieldInterpolation::Linear;
    declaration.cadence = environment::FieldCadence::SlowlyVarying;
    declaration.production = environment::FieldProduction::Cpu;
    declaration.levels[static_cast<u32>(FieldResidency::Local)] =
        FieldLevel{options.local_cell_metres, false};
    declaration.levels[static_cast<u32>(FieldResidency::Regional)] =
        FieldLevel{options.regional_cell_metres, false};
    declaration.levels[static_cast<u32>(FieldResidency::Macro)] =
        FieldLevel{options.macro_cell_metres, true};
    declaration.classification = determinism::SimulationClass::Authoritative;
    declaration.gameplay_level = FieldResidency::Macro;
    return declaration;
}

/// A field the world persistence overlay carries. Accumulation and ecosystem state both are: "a
/// region that was snowed on and unloaded SHALL retain its snow state", and a burned forest that
/// forgot it had burned would regrow from nothing.
[[nodiscard]] FieldDeclaration persistent_declaration(const WeatherFieldOptions& options) noexcept {
    FieldDeclaration declaration = base_declaration(options);
    declaration.classification = determinism::SimulationClass::Persistent;
    declaration.persistent = true;
    return declaration;
}

}  // namespace

const char* weather_field_name(WeatherField field) noexcept {
    switch (field) {
        case WeatherField::Wind:
            return environment::fields::kWind;
        case WeatherField::WindTurbulence:
            return fields::kWindTurbulence;
        case WeatherField::Temperature:
            return environment::fields::kTemperature;
        case WeatherField::PrecipitationRate:
            return fields::kPrecipitationRate;
        case WeatherField::PrecipitationType:
            return fields::kPrecipitationType;
        case WeatherField::Visibility:
            return fields::kVisibility;
        case WeatherField::Wetness:
            return environment::fields::kWetness;
        case WeatherField::SnowDepth:
            return environment::fields::kSnowDepth;
        case WeatherField::VegetationDensity:
            return fields::kVegetationDensity;
        case WeatherField::VegetationPotential:
            return fields::kVegetationPotential;
        case WeatherField::Moisture:
            return environment::fields::kMoisture;
        case WeatherField::MoisturePotential:
            return fields::kMoisturePotential;
        case WeatherField::SoilHealth:
            return fields::kSoilHealth;
        case WeatherField::Biomass:
            return fields::kBiomass;
        case WeatherField::ForestAge:
            return fields::kForestAge;
        case WeatherField::BurnState:
            return environment::fields::kBurnState;
        case WeatherField::Biome:
            return environment::fields::kBiome;
        case WeatherField::BiomePotential:
            return fields::kBiomePotential;
        default:
            return "";
    }
}

const char* wetness_source_name(WetnessSource source) noexcept {
    return (source == WetnessSource::Own) ? "own" : "compose-shore";
}

bool WeatherFieldOptions::is_valid() const noexcept {
    return local_cell_metres > 0.0F && regional_cell_metres > local_cell_metres &&
           macro_cell_metres > regional_cell_metres && wind_vertical_cells >= 1 &&
           wind_vertical_metres > 0.0F && max_wind_mps > 0.0F &&
           max_temperature_celsius > min_temperature_celsius &&
           max_precipitation_mm_per_hour > 0.0F && max_visibility_metres > 0.0F &&
           max_snow_depth_metres > 0.0F;
}

FieldDeclaration weather_field_declaration(WeatherField field,
                                           const WeatherFieldOptions& options) noexcept {
    FieldDeclaration declaration = base_declaration(options);
    declaration.name = weather_field_name(field);

    switch (field) {
        case WeatherField::Wind:
            // THE WIND FIELD: volumetric, authoritative, and f32. Quantising it would put the zero
            // of a signed quantity between two representable values, so a calm day would drift —
            // the same reasoning src/water/ gives for its flow field.
            declaration.unit = "m/s";
            declaration.semantics =
                "authoritative wind: mean flow plus the deterministic gust, world axes, vertical "
                "in .y";
            declaration.type = FieldType::Vec3;
            declaration.encoding = FieldEncoding::F32;
            declaration.range_min = -options.max_wind_mps;
            declaration.range_max = options.max_wind_mps;
            declaration.vertical_cells = options.wind_vertical_cells;
            declaration.vertical_metres = options.wind_vertical_metres;
            declaration.vertical_origin_metres = options.wind_vertical_origin_metres;
            declaration.cadence = environment::FieldCadence::PerFrame;
            declaration.default_value = FieldValue::vec3(0.0F, 0.0F, 0.0F);
            break;
        case WeatherField::WindTurbulence:
            // THE PRESENTATION HALF. `Presentation` here is the whole firewall: an authoritative
            // reader opening this field is refused by `determinism::may_read()` at
            // `FieldReader::open()`, and by `FieldRegistry::validate()` over the configuration.
            declaration.unit = "m/s";
            declaration.semantics =
                "presentation-only high-frequency wind residual; never read by authoritative state";
            declaration.type = FieldType::Vec3;
            declaration.encoding = FieldEncoding::F32;
            declaration.range_min = -options.max_turbulence_mps;
            declaration.range_max = options.max_turbulence_mps;
            declaration.vertical_cells = options.wind_vertical_cells;
            declaration.vertical_metres = options.wind_vertical_metres;
            declaration.vertical_origin_metres = options.wind_vertical_origin_metres;
            declaration.cadence = environment::FieldCadence::PerFrame;
            declaration.classification = determinism::SimulationClass::Presentation;
            declaration.default_value = FieldValue::vec3(0.0F, 0.0F, 0.0F);
            break;
        case WeatherField::Temperature:
            declaration.unit = "celsius";
            declaration.semantics = "air temperature at the ground";
            declaration.range_min = options.min_temperature_celsius;
            declaration.range_max = options.max_temperature_celsius;
            declaration.default_value = FieldValue::scalar(14.0F);
            break;
        case WeatherField::PrecipitationRate:
            declaration.unit = "mm/h";
            declaration.semantics = "precipitation rate, of the type the companion field names";
            declaration.range_min = 0.0F;
            declaration.range_max = options.max_precipitation_mm_per_hour;
            declaration.default_value = FieldValue::scalar(0.0F);
            break;
        case WeatherField::PrecipitationType:
            // A CATEGORY, and therefore `Nearest`: an interpolated precipitation type would be a
            // value between rain and snow, which names nothing. `validate_declaration()` refuses
            // the other pairing, so this is checked rather than remembered.
            declaration.unit = "index";
            declaration.semantics = "which kind of precipitation is falling; see PrecipitationType";
            declaration.type = FieldType::Category;
            declaration.encoding = FieldEncoding::Uint8;
            declaration.interpolation = FieldInterpolation::Nearest;
            declaration.range_min = 0.0F;
            declaration.range_max = 255.0F;
            declaration.default_value = FieldValue::category(0);
            break;
        case WeatherField::Visibility:
            declaration.unit = "metres";
            declaration.semantics = "how far one can see; artificial intelligence reads it";
            declaration.range_min = 0.0F;
            declaration.range_max = options.max_visibility_metres;
            declaration.default_value = FieldValue::scalar(options.max_visibility_metres);
            break;
        case WeatherField::Wetness:
            declaration = persistent_declaration(options);
            declaration.name = weather_field_name(field);
            declaration.unit = "fraction";
            declaration.semantics = "surface wetness, 0 dry to 1 saturated";
            // UNorm16 for the reason vegetation is: wetness is read back and rewritten every tick,
            // and a drying step removes well under a UNorm8 quantum of it. See the vegetation case.
            declaration.encoding = FieldEncoding::UNorm16;
            declaration.range_min = 0.0F;
            declaration.range_max = 1.0F;
            // MAX, and it is the same rule src/water/ chose for the same field: a shore that is wet
            // from the sea and wet from the rain is wet, not twice wet. The two rows agreeing is
            // not a coincidence — it is the only rule under which the composition is idempotent.
            declaration.layer_rule = environment::FieldLayerRule::Max;
            declaration.default_value = FieldValue::scalar(0.0F);
            break;
        case WeatherField::SnowDepth:
            declaration = persistent_declaration(options);
            declaration.name = weather_field_name(field);
            declaration.unit = "metres";
            declaration.semantics = "depth of lying snow; composited by materials, never geometry";
            declaration.range_min = 0.0F;
            declaration.range_max = options.max_snow_depth_metres;
            declaration.layer_rule = environment::FieldLayerRule::Add;
            declaration.default_value = FieldValue::scalar(0.0F);
            break;
        case WeatherField::VegetationDensity:
            declaration = persistent_declaration(options);
            declaration.name = weather_field_name(field);
            declaration.unit = "fraction";
            declaration.semantics = "how much vegetation stands here, 0 bare to 1 closed canopy";
            // UNorm16 AND NOT UNorm8, and the reason is the one thing about this module that a
            // reader has to know before changing an encoding. Every ecosystem field is
            // READ-MODIFY-WRITTEN once per macro step by a change that is small: the substrate's
            // recovery closes ~3.6e-4 of the gap per hour-long step, and a UNorm8's quantum is
            // 3.9e-3. A quantised accumulator whose increment is below its own quantum does not
            // accumulate slowly — it does not accumulate AT ALL, because every write rounds back to
            // the value it started from. A burned forest stays burned for ever and nothing reports
            // an error. `Ecosystem::check_resolution()` now refuses that configuration rather than
            // leaving it to be discovered.
            declaration.encoding = FieldEncoding::UNorm16;
            // THE RECOVERY IS THE SUBSTRATE'S. `potential` plus `recovery_per_second` is the whole
            // of "a burned forest regrows toward its potential" — see ecosystem.h's header note.
            declaration.potential = environment::field_id(fields::kVegetationPotential);
            declaration.recovery_per_second = options.vegetation_recovery_per_second;
            declaration.default_value = FieldValue::scalar(0.0F);
            break;
        case WeatherField::VegetationPotential:
            declaration.name = weather_field_name(field);
            declaration.unit = "fraction";
            declaration.semantics =
                "the vegetation this climate supports; what the current state "
                "recovers toward";
            // The same encoding as the field that recovers toward it: a potential quantised more
            // coarsely than the current state would make the gap jump between two representable
            // values as the potential is re-seeded.
            declaration.encoding = FieldEncoding::UNorm16;
            declaration.default_value = FieldValue::scalar(0.0F);
            break;
        case WeatherField::Moisture:
            declaration = persistent_declaration(options);
            declaration.name = weather_field_name(field);
            declaration.unit = "fraction";
            declaration.semantics = "soil moisture, 0 parched to 1 saturated";
            declaration.encoding = FieldEncoding::UNorm16;
            declaration.potential = environment::field_id(fields::kMoisturePotential);
            declaration.recovery_per_second = options.moisture_recovery_per_second;
            declaration.default_value = FieldValue::scalar(0.4F);
            break;
        case WeatherField::MoisturePotential:
            declaration.name = weather_field_name(field);
            declaration.unit = "fraction";
            declaration.semantics = "the soil moisture this climate tends to";
            declaration.encoding = FieldEncoding::UNorm16;
            declaration.default_value = FieldValue::scalar(0.4F);
            break;
        case WeatherField::SoilHealth:
            declaration = persistent_declaration(options);
            declaration.name = weather_field_name(field);
            declaration.unit = "fraction";
            declaration.semantics = "soil fertility and structure, 0 dead to 1 rich";
            declaration.encoding = FieldEncoding::UNorm16;
            declaration.default_value = FieldValue::scalar(0.6F);
            break;
        case WeatherField::Biomass:
            declaration = persistent_declaration(options);
            declaration.name = weather_field_name(field);
            declaration.unit = "kg/m2";
            declaration.semantics = "standing biomass; density and age together";
            declaration.range_min = 0.0F;
            declaration.range_max = options.max_biomass_kg_per_m2;
            declaration.default_value = FieldValue::scalar(0.0F);
            break;
        case WeatherField::ForestAge:
            declaration = persistent_declaration(options);
            declaration.name = weather_field_name(field);
            declaration.unit = "years";
            declaration.semantics = "years since the stand last reset; a fire returns it to zero";
            // F32, not a quantisation of [0, 400]. Age is the one ecosystem field that ACCUMULATES
            // rather than approaching a target: an hour-long step adds 1.1e-4 years, and even a
            // UNorm16 over four centuries has a quantum of 6.1e-3 — sixty steps of increment
            // rounded away. Four bytes a macro lattice point is the price of an accumulator that
            // actually accumulates.
            declaration.encoding = FieldEncoding::F32;
            declaration.range_min = 0.0F;
            declaration.range_max = options.max_forest_age_years;
            declaration.default_value = FieldValue::scalar(0.0F);
            break;
        case WeatherField::BurnState:
            declaration = persistent_declaration(options);
            declaration.name = weather_field_name(field);
            declaration.unit = "fraction";
            declaration.semantics = "how recently and completely this burned, 1 fresh to 0 healed";
            declaration.encoding = FieldEncoding::UNorm16;
            declaration.default_value = FieldValue::scalar(0.0F);
            break;
        case WeatherField::Biome:
            declaration = persistent_declaration(options);
            declaration.name = weather_field_name(field);
            declaration.unit = "index";
            declaration.semantics = "the biome this region currently is; see cy::weather::biomes";
            declaration.type = FieldType::Category;
            declaration.encoding = FieldEncoding::Uint8;
            declaration.interpolation = FieldInterpolation::Nearest;
            declaration.range_min = 0.0F;
            declaration.range_max = 255.0F;
            declaration.default_value = FieldValue::category(biomes::kBarren);
            break;
        case WeatherField::BiomePotential:
            declaration.name = weather_field_name(field);
            declaration.unit = "index";
            declaration.semantics =
                "the biome this climate tends toward; what procedural generation consumes";
            declaration.type = FieldType::Category;
            declaration.encoding = FieldEncoding::Uint8;
            declaration.interpolation = FieldInterpolation::Nearest;
            declaration.range_min = 0.0F;
            declaration.range_max = 255.0F;
            declaration.default_value = FieldValue::category(biomes::kBarren);
            break;
        default:
            break;
    }
    return declaration;
}

WeatherFields::WeatherFields(Allocator& allocator) noexcept : allocator_(&allocator) {}

Status WeatherFields::declare_one(environment::FieldRegistry& registry, WeatherField field,
                                  const FieldDeclaration& declaration) noexcept {
    if (Status declared = registry.declare(declaration); !declared) {
        return declared;
    }
    Entry& entry = entries_[static_cast<u32>(field)];
    entry.id = declaration.id();
    entry.declared = true;
    return ok();
}

Status WeatherFields::declare(environment::FieldRegistry& registry,
                              const WeatherFieldOptions& options) noexcept {
    if (!options.is_valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "weather: the field options need coarsening resolutions and positive ranges");
    }
    options_ = options;
    for (u32 index = 0; index < kWeatherFieldCount; ++index) {
        const auto field = static_cast<WeatherField>(index);
        if (index >= kFirstEcosystemField && !options.ecosystem) {
            continue;
        }
        if ((field == WeatherField::Wetness || field == WeatherField::SnowDepth) &&
            !options.accumulation) {
            continue;
        }
        if (Status declared =
                declare_one(registry, field, weather_field_declaration(field, options));
            !declared) {
            return declared;
        }
    }
    declared_ = true;
    return ok();
}

Status WeatherFields::claim(environment::FieldRegistry& registry,
                            environment::FieldStore& store) noexcept {
    if (!declared_) {
        return fail(ErrorCode::Unavailable,
                    "weather: the weather fields must be declared before they are claimed");
    }
    // ALL OR NOTHING. Every declared field is checked first, so a configuration where another row
    // already produces `wetness` is refused before weather has claimed eight other fields — a
    // half-claimed registry describes a configuration nobody chose. The refusal returned is the
    // SUBSTRATE'S OWN, obtained by asking for the contested field: nothing here can name both
    // producers better than the registry that holds them.
    for (auto& entrie : entries_) {
        const Entry& entry = entrie;
        if (!entry.declared) {
            continue;
        }
        const environment::FieldRecord* record = registry.find(entry.id);
        if (record == nullptr || !record->claimed) {
            continue;
        }
        Expected<environment::ProducerToken, Error> refusal =
            registry.claim(entry.id, "weather.system", environment::ProducerKind::System);
        if (!refusal) {
            return make_unexpected(refusal.error());
        }
        // Already claimed by weather itself: re-claiming its own field is not a conflict, and the
        // token just issued is the one this object keeps.
        entrie.token = std::move(*refusal);
    }
    for (auto& entry : entries_) {
        if (!entry.declared || entry.token.valid()) {
            continue;
        }
        Expected<environment::ProducerToken, Error> token =
            registry.claim(entry.id, "weather.system", environment::ProducerKind::System);
        if (!token) {
            return make_unexpected(token.error());
        }
        entry.token = std::move(*token);
    }
    store_ = &store;
    claimed_ = true;
    return ok();
}

Status WeatherFields::declare_consumers(environment::FieldRegistry& registry) const noexcept {
    // What weather READS. The shore's wetness contribution is the only field weather does not
    // produce and does consume, and declaring it is what lets `FieldRegistry::validate()` find a
    // firewall crossing over the whole configuration before a frame runs.
    if (options_.wetness != WetnessSource::ComposeShore) {
        return ok();
    }
    const environment::FieldId shore = environment::field_id(fields::kShoreWetness);
    if (registry.find(shore) == nullptr) {
        return ok();
    }
    return registry.declare_consumer("weather.accumulation",
                                     determinism::SimulationClass::Persistent, shore);
}

environment::FieldId WeatherFields::id(WeatherField field) const noexcept {
    return entries_[static_cast<u32>(field)].id;
}

bool WeatherFields::declared(WeatherField field) const noexcept {
    return entries_[static_cast<u32>(field)].declared;
}

const environment::ProducerToken* WeatherFields::token(WeatherField field) const noexcept {
    const Entry& entry = entries_[static_cast<u32>(field)];
    return entry.token.valid() ? &entry.token : nullptr;
}

f32 WeatherFields::level_metres(environment::FieldResidency level) const noexcept {
    switch (level) {
        case environment::FieldResidency::Local:
            return options_.local_cell_metres;
        case environment::FieldResidency::Regional:
            return options_.regional_cell_metres;
        case environment::FieldResidency::Macro:
        default:
            // The macro level is the fallback as well as a case: a level this enumeration gains
            // later resolves to the coarsest declared resolution rather than to zero metres.
            return options_.macro_cell_metres;
    }
}

/// The six writers one atmospheric publication holds open, as one thing to pass around. Pointers
/// because `environment::FieldWriter` is move-only and they live for the whole of the walk.
struct AtmosphereWriters {
    environment::FieldWriter* wind = nullptr;
    environment::FieldWriter* turbulence = nullptr;
    environment::FieldWriter* temperature = nullptr;
    environment::FieldWriter* rate = nullptr;
    environment::FieldWriter* type = nullptr;
    environment::FieldWriter* visibility = nullptr;
};

namespace {

/// Stage one lattice point into one planar field. Split out because the tile arithmetic is what a
/// reader checks against `environment::store.cpp`'s own convention, and it should be readable once
/// rather than six times.
[[nodiscard]] Status stage_planar(environment::FieldWriter& writer, environment::FieldId field,
                                  environment::FieldResidency level, i64 lattice_x, i64 lattice_z,
                                  const FieldValue& value) noexcept {
    const auto span = static_cast<i64>(environment::kTileCells);
    const i64 tile_x = tile_of_lattice(lattice_x);
    const i64 tile_z = tile_of_lattice(lattice_z);
    environment::TileAddress address;
    address.field = field;
    address.level = static_cast<u8>(level);
    // The DELTA layer: a published value is a runtime contribution over whatever was cooked,
    // combined by the rule the field declares. Writing the base would overwrite cooked data with a
    // value computed from whatever happens to be resident.
    address.layer = static_cast<u8>(FieldLayer::Delta);
    address.x = static_cast<i32>(tile_x);
    address.z = static_cast<i32>(tile_z);
    if (Status staged = writer.stage(address); !staged) {
        return staged;
    }
    return writer.set(address, static_cast<u32>(lattice_x - (tile_x * span)), 0,
                      static_cast<u32>(lattice_z - (tile_z * span)), value);
}

/// One lattice point's four planar fields. Split out of the walk because the tile arithmetic and
/// the four values are separate things to read, and because a walk that inlined them was past this
/// project's complexity band for systems code.
[[nodiscard]] Status publish_planar_point(const AtmosphereWriters& writers,
                                          const WeatherFields& fields,
                                          const EnvironmentSample& ground,
                                          environment::FieldResidency level, i64 lattice_x,
                                          i64 lattice_z) noexcept {
    struct Planar {
        environment::FieldWriter* writer{};
        WeatherField field{};
        FieldValue value{};
    };
    const Planar planar[4] = {{writers.temperature, WeatherField::Temperature,
                               FieldValue::scalar(ground.temperature_celsius)},
                              {writers.rate, WeatherField::PrecipitationRate,
                               FieldValue::scalar(ground.precipitation_mm_per_hour)},
                              {writers.type, WeatherField::PrecipitationType,
                               FieldValue::category(static_cast<u32>(ground.precipitation_type))},
                              {writers.visibility, WeatherField::Visibility,
                               FieldValue::scalar(ground.visibility_metres)}};
    for (const Planar& one : planar) {
        if (Status staged = stage_planar(*one.writer, fields.id(one.field), level, lattice_x,
                                         lattice_z, one.value);
            !staged) {
            return staged;
        }
    }
    return ok();
}

/// One lattice column of the two wind fields.
///
/// TWO SAMPLES, TWO CLASSES, AND THE AUTHORITATIVE HALVES ARE THE SAME NUMBER. The presentation
/// sample differs from the authoritative one only by the turbulence residual — see wind.h — and
/// that residual is exactly what is written to the presentation field and to nothing else. A
/// consumer of `wind` therefore cannot see a value that depended on presentation, and a consumer of
/// `wind-turbulence` is refused by the substrate unless it is presentation itself.
[[nodiscard]] Status publish_wind_column(const AtmosphereWriters& writers,
                                         const WeatherFields& fields,
                                         const EnvironmentSampler& sampler,
                                         environment::FieldResidency level, i64 lattice_x,
                                         i64 lattice_z, f64 x, f64 z) noexcept {
    const auto span = static_cast<i64>(environment::kTileCells);
    const i64 tile_x = tile_of_lattice(lattice_x);
    const i64 tile_z = tile_of_lattice(lattice_z);
    const auto local_x = static_cast<u32>(lattice_x - (tile_x * span));
    const auto local_z = static_cast<u32>(lattice_z - (tile_z * span));

    environment::TileAddress address;
    address.level = static_cast<u8>(level);
    address.layer = static_cast<u8>(FieldLayer::Delta);
    address.x = static_cast<i32>(tile_x);
    address.z = static_cast<i32>(tile_z);

    const WeatherFieldOptions& options = fields.options();
    for (u32 j = 0; j < options.wind_vertical_cells; ++j) {
        const f64 y =
            static_cast<f64>(options.wind_vertical_origin_metres) +
            ((static_cast<f64>(j) + 0.5) * static_cast<f64>(options.wind_vertical_metres));
        const world::WorldVec3d at{x, y, z};
        const WindSample authoritative =
            sampler.sample(at, SampleQuality::Gameplay, determinism::SimulationClass::Authoritative)
                .wind;
        const WindSample presented = sampler
                                         .sample(at, SampleQuality::HighFrequency,
                                                 determinism::SimulationClass::Presentation)
                                         .wind;
        const Vec3 wind = authoritative.authoritative();

        address.field = fields.id(WeatherField::Wind);
        if (Status staged = writers.wind->stage(address); !staged) {
            return staged;
        }
        if (Status set = writers.wind->set(address, local_x, j, local_z,
                                           FieldValue::vec3(wind.x, wind.y, wind.z));
            !set) {
            return set;
        }

        address.field = fields.id(WeatherField::WindTurbulence);
        if (Status staged = writers.turbulence->stage(address); !staged) {
            return staged;
        }
        if (Status set = writers.turbulence->set(
                address, local_x, j, local_z,
                FieldValue::vec3(presented.turbulence.x, presented.turbulence.y,
                                 presented.turbulence.z));
            !set) {
            return set;
        }
    }
    return ok();
}

}  // namespace

Expected<PublishReport, Error> WeatherFields::publish_atmosphere(
    const EnvironmentSampler& sampler, const PublishRegion& region) noexcept {
    PublishReport report;
    if (!claimed_ || store_ == nullptr) {
        return fail(ErrorCode::Unavailable,
                    "weather: the weather fields must be claimed before they are published");
    }
    if (!region.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "weather: an empty region publishes nothing");
    }
    if (!sampler.bound()) {
        return fail(ErrorCode::InvalidArgument,
                    "weather: publishing needs a sampler bound to cells and wind");
    }

    // One writer per field, opened once for the whole region and published TOGETHER at the end.
    // That is what makes a consumer sampling two of these at one position see one weather rather
    // than a temperature from this frame beside a rain rate from the last — the substrate's staged
    // writes, used for the reason store.h gives for having them.
    const WeatherField published[6] = {WeatherField::Wind,
                                       WeatherField::WindTurbulence,
                                       WeatherField::Temperature,
                                       WeatherField::PrecipitationRate,
                                       WeatherField::PrecipitationType,
                                       WeatherField::Visibility};
    Array<environment::FieldWriter> writers(*allocator_);
    for (WeatherField field : published) {
        Expected<environment::FieldWriter, Error> writer =
            store_->open_writer(entries_[static_cast<u32>(field)].token);
        if (!writer) {
            return make_unexpected(writer.error());
        }
        if (Status pushed = writers.push_back(std::move(*writer)); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    const AtmosphereWriters bound{writers.data(), &writers[1], &writers[2],
                                  &writers[3],    &writers[4], &writers[5]};

    const f32 cell_metres = level_metres(region.level);
    const auto metres = static_cast<f64>(cell_metres);
    const auto min_i = static_cast<i64>(std::floor(region.min_x / metres));
    const auto max_i = static_cast<i64>(std::floor(region.max_x / metres));
    const auto min_k = static_cast<i64>(std::floor(region.min_z / metres));
    const auto max_k = static_cast<i64>(std::floor(region.max_z / metres));

    for (i64 k = min_k; k <= max_k; ++k) {
        for (i64 i = min_i; i <= max_i; ++i) {
            // Lattice points sit at CELL CENTRES — the half-cell offset `environment::store.cpp`
            // samples with. A publication on cell corners would be half a cell out of step with
            // every reader, which is the kind of error a screenshot shows and a test does not.
            const f64 x = (static_cast<f64>(i) + 0.5) * metres;
            const f64 z = (static_cast<f64>(k) + 0.5) * metres;
            const EnvironmentSample ground =
                sampler.sample(world::WorldVec3d{x, 0.0, z}, SampleQuality::Gameplay,
                               determinism::SimulationClass::Authoritative);
            if (Status planar = publish_planar_point(bound, *this, ground, region.level, i, k);
                !planar) {
                return make_unexpected(planar.error());
            }
            if (Status column =
                    publish_wind_column(bound, *this, sampler, region.level, i, k, x, z);
                !column) {
                return make_unexpected(column.error());
            }
            ++report.lattice_points;
        }
    }

    for (environment::FieldWriter& writer : writers) {
        if (Status flushed = writer.publish(); !flushed) {
            return make_unexpected(flushed.error());
        }
        ++report.fields_written;
    }
    return report;
}

}  // namespace cy::weather

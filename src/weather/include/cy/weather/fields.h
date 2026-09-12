#pragma once
// WEATHER AS A PRODUCER: the fields it declares, the fields it claims, and the publication that is
// the whole of how weather reaches the world. M10 tasks 3.1 and 3.2.
//
// `weather-and-wind` — "Weather publishes state": weather "SHALL influence the world by writing
// environment fields. It SHALL NOT iterate materials, foliage instances, water bodies, or particle
// systems to apply itself. Consumers SHALL sample the fields they need at the fidelity they need:
// materials sample wetness, foliage samples wind, water samples precipitation, audio samples wind
// and rain, artificial intelligence samples visibility. There SHALL NOT be a central weather
// component that pushes state to subsystems."
//
// ================================================================================================
// THIS FILE IS THE ONLY OUTPUT SIDE OF THE MODULE
// ================================================================================================
//
// Everything else in `src/weather/` computes; this publishes. There is no other way out: no
// callback into a consumer, no list of subsystems to notify, no `apply_to()` anywhere in the
// module. "A new consumer needs no weather change" is therefore not a promise — there is nothing
// here for a new consumer to be added to.
//
// ================================================================================================
// WETNESS HAS TWO CANDIDATE PRODUCERS AND THE SUBSTRATE ALLOWS ONE
// ================================================================================================
//
// `water` says the shoreline writes wetness. This capability says precipitation accumulates into
// wetness. `environment-fields` says a field has exactly one producer, refused at registration.
// src/water/ already drew the line and named the far side: `WetnessOwner::External` makes water
// produce `water-shore-wetness` for "the wetness producer to compose with precipitation", and THIS
// IS THAT PRODUCER.
//
// So `WetnessSource` is declared here and the two configurations are symmetric:
//
//   `Own`            weather claims `wetness` and composes precipitation alone. A world with no
//                    water, or one whose water row runs with `WetnessOwner::Water` — in which case
//                    weather must NOT claim it, and this option is the way to say so is `Shore`
//                    with no shore field present, or simply not declaring wetness at all.
//   `ComposeShore`   weather claims `wetness`, READS `water-shore-wetness` through an ordinary
//                    `FieldReader`, and publishes the maximum of the shore's contribution and the
//                    precipitation's. A shore that is wet from the sea and wet from the rain is
//                    wet, not twice wet — which is also the layer rule src/water/ chose for the
//                    same field, and the two agreeing is not a coincidence but the same reasoning.
//
// A configuration where BOTH rows claim `wetness` fails at `claim()` with the substrate's own
// refusal naming both producers. That refusal is the design, not an accident to be worked around,
// and `test_fields.cpp` holds it as a case.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/weather/cells.h>

namespace cy::weather {

class EnvironmentSampler;

/// The names weather produces that `environment-fields` does not already list. The standard ones —
/// `wind`, `temperature`, `wetness`, `snow-depth`, `moisture`, `burn-state`, `biome` — come from
/// `environment::fields`, so that two rows naming one quantity name one field.
namespace fields {
inline constexpr const char* kWindTurbulence = "wind-turbulence";
inline constexpr const char* kPrecipitationRate = "precipitation-rate";
inline constexpr const char* kPrecipitationType = "precipitation-type";
inline constexpr const char* kVisibility = "visibility";
inline constexpr const char* kVegetationDensity = "vegetation-density";
inline constexpr const char* kVegetationPotential = "vegetation-potential";
inline constexpr const char* kMoisturePotential = "moisture-potential";
inline constexpr const char* kSoilHealth = "soil-health";
inline constexpr const char* kBiomass = "biomass";
inline constexpr const char* kForestAge = "forest-age";
inline constexpr const char* kBiomePotential = "biome-potential";
/// The field src/water/ publishes when it does not own `wetness`. Named here so the composing side
/// spells one string rather than agreeing on one.
inline constexpr const char* kShoreWetness = "water-shore-wetness";
}  // namespace fields

/// Which field of the set a handle refers to. An enumerator rather than a bag of `FieldId` members,
/// so that declaring, claiming, publishing and diagnosing all iterate one table.
enum class WeatherField : u8 {
    Wind = 0,
    WindTurbulence,
    Temperature,
    PrecipitationRate,
    PrecipitationType,
    Visibility,
    Wetness,
    SnowDepth,
    // --- the ecosystem half, declared only when `WeatherFieldOptions::ecosystem` is set ---------
    VegetationDensity,
    VegetationPotential,
    Moisture,
    MoisturePotential,
    SoilHealth,
    Biomass,
    ForestAge,
    BurnState,
    Biome,
    BiomePotential,
    kCount,
};

inline constexpr u32 kWeatherFieldCount = static_cast<u32>(WeatherField::kCount);
/// The first ecosystem field. Everything at or after it is declared only when the ecosystem half is
/// enabled, and the split is a constant rather than a condition repeated in four loops.
inline constexpr u32 kFirstEcosystemField = static_cast<u32>(WeatherField::VegetationDensity);

[[nodiscard]] const char* weather_field_name(WeatherField field) noexcept;

/// Where wetness comes from in this configuration. See the header note.
enum class WetnessSource : u8 {
    /// Precipitation alone.
    Own = 0,
    /// Precipitation composed with `water-shore-wetness`, which src/water/ publishes when it is
    /// configured with `WetnessOwner::External`.
    ComposeShore,
};

[[nodiscard]] const char* wetness_source_name(WetnessSource source) noexcept;

/// How the set is declared. Ranges matter: a quantised field's declared range IS its storage
/// contract, so widening one changes what every stored byte means.
struct WeatherFieldOptions {
    WetnessSource wetness = WetnessSource::Own;
    /// Whether the ecosystem half is declared at all. A shooter wants wind and rain and no biomass.
    bool ecosystem = true;
    /// Whether weather declares `wetness` and `snow-depth` at all. A project whose water row owns
    /// wetness outright clears this and keeps everything else.
    bool accumulation = true;

    /// The three declared resolutions, metres per cell. The macro level is resident everywhere,
    /// because every authoritative field here is gameplay-visible and the substrate refuses a
    /// gameplay-visible field whose declared level is not.
    f32 local_cell_metres = 8.0F;
    f32 regional_cell_metres = 64.0F;
    f32 macro_cell_metres = 512.0F;

    /// The vertical extent of the wind field, in cells, and the height of one. Wind is the one
    /// volumetric field here: a canyon's wind at ground level and fifty metres up are different
    /// winds, and a planar wind field would make a helicopter and a blade of grass agree.
    u32 wind_vertical_cells = 8;
    f32 wind_vertical_metres = 32.0F;
    f32 wind_vertical_origin_metres = 0.0F;

    /// Declared ranges. Every one of them is the storage contract of a quantised field.
    f32 max_wind_mps = 80.0F;
    f32 max_turbulence_mps = 20.0F;
    f32 min_temperature_celsius = -60.0F;
    f32 max_temperature_celsius = 60.0F;
    f32 max_precipitation_mm_per_hour = 120.0F;
    f32 max_visibility_metres = 40'000.0F;
    f32 max_snow_depth_metres = 4.0F;
    f32 max_biomass_kg_per_m2 = 60.0F;
    f32 max_forest_age_years = 400.0F;

    /// How fast vegetation, moisture and soil health close the gap to their potential, as a
    /// fraction of the remaining gap per second. THE SUBSTRATE'S OWN RECOVERY drives them — see
    /// `environment::FieldDeclaration::recovery_per_second` — so "a burned forest regrows toward
    /// its potential" is `FieldStore::advance_recovery()` and not a loop in this module.
    ///
    /// SOIL HEALTH DECLARES NO POTENTIAL and therefore does not recover on its own. That is a
    /// modelling decision and not an omission: soil is destroyed by pollution and rebuilt by
    /// terraforming, both of which are EVENTS, and a soil that healed itself would make a
    /// contaminated site a temporary problem.
    f32 vegetation_recovery_per_second = 1.0e-7F;
    f32 moisture_recovery_per_second = 2.0e-5F;

    [[nodiscard]] bool is_valid() const noexcept;
};

/// The rectangle of world a publication covers, at one declared level. The caller's, not this
/// module's: a streamer publishes what became resident, an editor publishes what a designer is
/// looking at, and a cook publishes the world. The answer differs for all three, so weather does
/// not guess.
struct PublishRegion {
    f64 min_x = 0.0;
    f64 min_z = 0.0;
    f64 max_x = 0.0;
    f64 max_z = 0.0;
    environment::FieldResidency level = environment::FieldResidency::Macro;

    [[nodiscard]] bool is_valid() const noexcept { return max_x > min_x && max_z > min_z; }
};

/// What one publication did. Counted, because the field-update budget is declared over exactly
/// these numbers.
struct PublishReport {
    u64 lattice_points = 0;
    u32 fields_written = 0;
    u32 tiles_touched = 0;
};

/// The fields weather declares, claims and writes.
///
/// **The only output side of the module.** It holds producer tokens and a store pointer and nothing
/// else; every value it writes is computed by `WeatherCells`, `WindComposer`, `Accumulation` or
/// `Ecosystem` and handed here.
class WeatherFields {
public:
    explicit WeatherFields(Allocator& allocator) noexcept;

    WeatherFields(const WeatherFields&) = delete;
    WeatherFields& operator=(const WeatherFields&) = delete;

    /// Declare the set into a registry. Idempotent: re-declaring identically is accepted by the
    /// substrate, so two modules that both need `wetness` to exist need not agree on which declares
    /// it.
    [[nodiscard]] Status declare(environment::FieldRegistry& registry,
                                 const WeatherFieldOptions& options) noexcept;

    /// Claim the producer tokens. **This is where a second producer is refused**, by the substrate,
    /// naming both — so a configuration in which water also claims `wetness` fails here with a
    /// message that says so rather than racing at run time.
    ///
    /// ALL OR NOTHING. Every field is checked before any is claimed, because a claim that failed
    /// halfway would leave the registry describing a configuration nobody chose.
    [[nodiscard]] Status claim(environment::FieldRegistry& registry,
                               environment::FieldStore& store) noexcept;

    /// Declare what weather READS, so that `FieldRegistry::validate()` catches a firewall crossing
    /// over the whole configuration before a frame runs rather than at the first sample.
    [[nodiscard]] Status declare_consumers(environment::FieldRegistry& registry) const noexcept;

    /// Publish the atmospheric half — wind, turbulence, temperature, precipitation, visibility —
    /// over a region, from a bound sampler.
    ///
    /// The wind field is volumetric, so the publication walks the declared column; every other
    /// field here is planar and is written at the column's base.
    [[nodiscard]] Expected<PublishReport, Error> publish_atmosphere(
        const EnvironmentSampler& sampler, const PublishRegion& region) noexcept;

    [[nodiscard]] environment::FieldId id(WeatherField field) const noexcept;
    [[nodiscard]] bool declared(WeatherField field) const noexcept;
    [[nodiscard]] bool claimed() const noexcept { return claimed_; }
    [[nodiscard]] environment::FieldStore* store() const noexcept { return store_; }
    [[nodiscard]] const WeatherFieldOptions& options() const noexcept { return options_; }
    /// The token for one field, for the two models that write their own values — `Accumulation` and
    /// `Ecosystem`. Borrowed: the token stays here, because "one producer per field" is a property
    /// of who HOLDS the token and handing it out would give it away.
    [[nodiscard]] const environment::ProducerToken* token(WeatherField field) const noexcept;

    /// The cell size of a declared level, metres. What a publication walks in.
    [[nodiscard]] f32 level_metres(environment::FieldResidency level) const noexcept;

private:
    /// One declared field: its identity, its declaration and its token.
    struct Entry {
        environment::FieldId id;
        environment::ProducerToken token;
        bool declared = false;
    };

    [[nodiscard]] Status declare_one(environment::FieldRegistry& registry, WeatherField field,
                                     const environment::FieldDeclaration& declaration) noexcept;

    Allocator* allocator_;
    WeatherFieldOptions options_;
    environment::FieldStore* store_ = nullptr;
    Entry entries_[kWeatherFieldCount];
    bool declared_ = false;
    bool claimed_ = false;
};

/// The declaration of one field of the set, as a free function so that a cooker can produce the
/// same declaration without a registry — and so that `test_fields.cpp` can compare a declaration
/// against the specification's list without constructing the module.
[[nodiscard]] environment::FieldDeclaration weather_field_declaration(
    WeatherField field, const WeatherFieldOptions& options) noexcept;

}  // namespace cy::weather

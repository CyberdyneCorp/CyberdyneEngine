#pragma once
// MACRO ECOSYSTEM STATE: a world that regrows, for regions nobody is looking at. M10 task 3.2.
//
// `weather-and-wind` — "Ecosystem state": weather and environment "SHALL maintain MACRO ECOSYSTEM
// STATE as fields — vegetation density, biomass, forest age, soil health, burn fraction, moisture —
// evolving at low resolution over long periods, WHETHER OR NOT A REGION IS RESIDENT"; "Ecosystem
// state SHALL evolve toward BIOME POTENTIAL and be knocked back by events: fire, deforestation,
// drought, pollution, or terraforming"; "Per-organism ecological simulation SHALL NOT be
// attempted"; "Thresholds between biome states SHALL be expressible as DECLARED CONDITIONS OVER
// FIELDS, so that a world can transition from desert to savanna to forest as conditions change."
//
// ================================================================================================
// THE REGROWTH IS THE SUBSTRATE'S OWN, NOT A LOOP IN THIS FILE
// ================================================================================================
//
// `environment-fields` already has the mechanism: a field declares a `potential` and a
// `recovery_per_second`, and `FieldStore::advance_recovery()` closes the gap exponentially. So
// `vegetation-density` declares `vegetation-potential`, `moisture` declares `moisture-potential`,
// and this module's "regrowth" is one call each. What is left for this file is the half the
// substrate deliberately does not do — "Reducing the current state is still the producer's" — which
// is the knock-back: fire, deforestation, drought, pollution, terraforming.
//
// That split is worth stating because it is the whole of why `Ecosystem` is small. A recovery model
// written here would be a second recovery curve beside the substrate's, and the one that drifts is
// always the one nobody is watching.
//
// ================================================================================================
// "WHETHER OR NOT A REGION IS RESIDENT" IS A RESIDENCY DECLARATION
// ================================================================================================
//
// Every ecosystem field declares its MACRO level `resident_everywhere`, which the substrate
// guarantees through `FieldStreaming::guarantee_macro()`. The evolution therefore runs over macro
// tiles that exist for the whole world by declaration, and a region nobody has streamed regrows
// exactly like one somebody is standing in. `test_ecosystem.cpp` burns a region, evicts every
// non-guaranteed tile, advances ninety days and finds the region recovered.
//
// ================================================================================================
// BIOME THRESHOLDS ARE DATA
// ================================================================================================
//
// `BiomeRule` is a set of half-open conditions over named fields. A world's biome table is an array
// of them, evaluated in order, first match wins. Nothing in this file switches on a biome index,
// which is what "expressible as declared conditions over fields" has to mean if a project is to add
// one without an engine change.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/environment/store.h>
#include <cy/weather/climate.h>
#include <cy/weather/fields.h>

namespace cy::weather {

/// What knocks the ecosystem back. The specification's own list, and `Project` for a world with its
/// own catastrophe.
enum class EcosystemEventKind : u8 {
    Fire = 0,
    Deforestation = 1,
    Drought = 2,
    Pollution = 3,
    /// A deliberate change of the LAND rather than of what grows on it: it moves soil health and
    /// moisture, and therefore moves the biome the place tends toward.
    Terraforming = 4,
    Project = 5,
};

[[nodiscard]] const char* ecosystem_event_kind_name(EcosystemEventKind kind) noexcept;

/// One knock-back, over a rectangle of world.
struct EcosystemEvent {
    EcosystemEventKind kind = EcosystemEventKind::Fire;
    f64 min_x = 0.0;
    f64 min_z = 0.0;
    f64 max_x = 0.0;
    f64 max_z = 0.0;
    /// [0, 1]. How completely the event applies: a ground fire is not a crown fire.
    f32 magnitude = 1.0F;
    /// `Terraforming` only: the soil health and moisture the operation is driving toward. Ignored
    /// by every other kind.
    ///
    /// Terraforming is the ONE event that also moves the POTENTIALS — see
    /// `terraform_potential_op()`. Every other kind knocks the current state away from a potential
    /// the climate still holds, and the substrate's recovery pulls it back; irrigating a desert
    /// changes what the land can support, so an event that only moved the current state would be a
    /// terraforming programme that evaporated.
    f32 target_soil_health = 0.0F;
    f32 target_moisture = 0.0F;

    [[nodiscard]] bool is_valid() const noexcept { return max_x > min_x && max_z > min_z; }
};

/// A half-open condition over one field: `min <= value < max`.
struct FieldCondition {
    environment::FieldId field;
    f32 min_value = 0.0F;
    f32 max_value = 1.0F;
};

/// The most conditions one biome rule carries. A fixed array, because a rule is a value in a table
/// a project authors and a rule needing more than four axes is two rules.
inline constexpr u32 kMaxBiomeConditions = 4;

/// One declared threshold between biome states. See the header note.
struct BiomeRule {
    u32 biome = 0;
    FieldCondition conditions[kMaxBiomeConditions];
    u32 condition_count = 0;
    const char* name = "";
};

/// The model's rates. Everything the substrate's recovery does not cover.
struct EcosystemModel {
    /// Fraction of the burn state that decays per second. Slower than vegetation regrowth, so a
    /// scar is visible after the grass is back — which is the behaviour a burned forest actually
    /// has.
    f32 burn_decay_per_second = 4.0e-8F;
    /// Kilograms per square metre of biomass at full vegetation density and mature forest age.
    f32 biomass_at_maturity = 40.0F;
    /// Years a forest takes to reach maturity. Biomass follows age and density together, so a dense
    /// young stand and a sparse old one carry different biomass.
    f32 maturity_years = 120.0F;
    /// Fraction of vegetation density removed at magnitude 1, per event kind.
    f32 fire_vegetation_loss = 0.95F;
    f32 deforestation_vegetation_loss = 1.0F;
    f32 drought_moisture_loss = 0.8F;
    f32 pollution_soil_loss = 0.6F;
    /// Fraction of soil health a fire removes. Less than the vegetation, because ash is fertiliser
    /// and the requirement's own scenario is that the forest comes BACK.
    f32 fire_soil_loss = 0.15F;
};

/// What one ecosystem step did.
struct EcosystemReport {
    u64 lattice_points = 0;
    /// Whole macro steps applied. The editor's fast-forward and the runtime's tick both report it,
    /// and the two are compared.
    u32 steps = 0;
    /// Points whose biome classification changed. The number the "terraforming changes a biome"
    /// scenario asserts on.
    u64 biome_changes = 0;
};

/// The macro ecosystem: evolution toward potential, knock-back by events, and biome classification
/// from declared thresholds.
class Ecosystem {
public:
    explicit Ecosystem(Allocator& allocator) noexcept;

    Ecosystem(const Ecosystem&) = delete;
    Ecosystem& operator=(const Ecosystem&) = delete;

    void set_model(const EcosystemModel& model) noexcept { model_ = model; }
    [[nodiscard]] const EcosystemModel& model() const noexcept { return model_; }

    /// Declare the biome table. Evaluated in order, first match wins; an empty table classifies
    /// nothing and leaves the `biome` field at its declared default.
    [[nodiscard]] Status set_biome_rules(Span<const BiomeRule> rules) noexcept;
    [[nodiscard]] Span<const BiomeRule> biome_rules() const noexcept { return rules_.span(); }
    /// The default table, over this module's own fields: the Whittaker axes as four rules.
    [[nodiscard]] Status set_default_biome_rules(const WeatherFields& fields) noexcept;

    /// Write the potentials from the climate. Run once when a world is created and again when its
    /// climate is edited — never per tick, because a potential that changed per tick would be a
    /// current state.
    [[nodiscard]] Expected<EcosystemReport, Error> seed_potential(
        WeatherFields& fields, const ClimateMap& climate, const PublishRegion& region) noexcept;

    /// Apply a knock-back. The producer's half of the potential mechanism; see the header note.
    [[nodiscard]] Expected<EcosystemReport, Error> apply_event(
        WeatherFields& fields, const EcosystemEvent& event) noexcept;

    /// Advance the macro state by `seconds`: the substrate's recovery, the burn decay, forest age
    /// and biomass, then the biome classification.
    ///
    /// **`advance()` is the editor's fast-forward and the runtime's tick, one function.** "This
    /// SHALL use the same macro state and generation path as the runtime, not a separate preview
    /// model" — so there is no preview entry point, and `advance_days()` below is a unit
    /// conversion.
    [[nodiscard]] Expected<EcosystemReport, Error> advance(WeatherFields& fields,
                                                           const PublishRegion& region,
                                                           f64 seconds) noexcept;

    /// Seconds, in days. Nothing more, and that is the point: the editor's "ninety days in a
    /// moment" is this call, and `test_ecosystem.cpp` requires ninety one-day calls and one
    /// ninety-day call to reach the same state.
    [[nodiscard]] Expected<EcosystemReport, Error> advance_days(WeatherFields& fields,
                                                                const PublishRegion& region,
                                                                f64 days) noexcept;

    /// The biome a position classifies to right now, through the declared rules.
    [[nodiscard]] u32 classify(const environment::FieldStore& store,
                               const world::WorldVec3d& at) const noexcept;

    /// Refuse a configuration whose macro step moves a field by less than that field can store.
    ///
    /// **THE DEFECT THIS EXISTS TO NAME.** Every field here is read back, changed by a small amount
    /// and written again. If the change is smaller than the field's own quantum, every write rounds
    /// back to the value it started from: the state does not evolve SLOWLY, it does not evolve at
    /// all, and nothing reports anything. A burned forest stays burned for ever and the only
    /// evidence is a number that never moves. It cost this module an afternoon and it is the reason
    /// `vegetation-density` is `UNorm16` and `forest-age` is `f32` — see
    /// `weather_field_declaration()`.
    ///
    /// `advance()` calls this once, on its first step, and refuses naming the field and both
    /// numbers. A project that widens `max_forest_age_years` past the point where its encoding can
    /// carry an hour of growth learns so at configuration time rather than from a world that never
    /// ages.
    [[nodiscard]] Status check_resolution(const WeatherFields& fields) const noexcept;

private:
    /// One rectangle of one field, read-modify-written through the store. The whole of the tile
    /// arithmetic, in one place, because four of this class's methods do exactly it with a
    /// different function of the value.
    using PointOp = void (*)(void* user, f64 x, f64 z, f32* values, u32 count);

    /// One macro step: the model's own edits, then the substrate's recovery.
    [[nodiscard]] Status run_step(WeatherFields& fields, const PublishRegion& region,
                                  EcosystemReport& report) noexcept;
    /// The biome pass, over the fields the steps have just moved. Once per `advance()`, not once
    /// per step — see the definition.
    [[nodiscard]] Status classify_region(WeatherFields& fields, const PublishRegion& region,
                                         EcosystemReport& report) noexcept;

    [[nodiscard]] Expected<u64, Error> edit_region(WeatherFields& fields,
                                                   Span<const WeatherField> which,
                                                   const PublishRegion& region, PointOp op,
                                                   void* user) noexcept;

    Allocator* allocator_;
    EcosystemModel model_;
    Array<BiomeRule> rules_;
    /// Simulated seconds not yet consumed by a whole macro step, so that a sequence of short
    /// advances and one long one agree. See `advance()`.
    f64 carry_ = 0.0;
    /// Whether `check_resolution()` has run. Once per object: the declarations cannot change under
    /// it, and a check per step would be a check nobody leaves on.
    bool checked_ = false;
    /// The formatted refusal. `Error::message` is a `const char*` with no ownership, so a message
    /// naming the field has to live somewhere with a longer life than the call —
    /// `environment::FieldRegistry` keeps its producer conflict the same way, for the same reason.
    mutable char resolution_message_[256] = {};
};

/// Seconds in a day, as this module counts them. Named so that "ninety days" is one multiplication
/// a reader can check rather than a literal in three files.
inline constexpr f64 kSecondsPerDay = 86'400.0;
/// The macro step the ecosystem advances in. Coarse — an hour — because the state it moves changes
/// over seasons, and because a finer step would make a ninety-day advance ninety times slower for a
/// result that differs in the sixth decimal place.
inline constexpr f64 kEcosystemStepSeconds = 3'600.0;

}  // namespace cy::weather

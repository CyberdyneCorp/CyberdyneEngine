#pragma once
// Foliage as a CITIZEN of the rest of the engine: the environment field it produces, the fields it
// reads, the regional state it derives from them, and the one call that turns a region into a
// cluster. M10 task 2.4.
//
// ================================================================================================
// FOLIAGE IS A PRODUCER, AND IT IS ONE THROUGH THE REFUSAL RATHER THAN AROUND IT
// ================================================================================================
//
// `environment-fields` puts fields BENEATH foliage exactly as it puts them beneath terrain, so
// foliage READS biome, moisture, temperature, soil, water distance, human exclusion, burn state,
// snow depth, wetness and wind, and WRITES exactly one: `vegetation`, how much plant life a piece
// of world currently carries.
//
// That field is foliage's to know and nobody else's, and it is what makes two of the
// specification's scenarios true at once:
//
//   * "A GROWN FOREST APPEARS GROWN" — "Where a region's macro ecosystem state has evolved while
//     unloaded, materialisation SHALL be consistent with that state." The macro level of
//     `vegetation` is resident for the whole world (`environment-fields` requires that of a macro
//     level a field declares), so it evolves for regions nobody has loaded, and `generate_region()`
//     scales density by it. A region that grew back while you were away materialises grown.
//   * "A FOREST BURNS" — the field declares a `potential` link and a recovery rate, so a fire that
//     writes the current state down leaves the potential alone and the substrate's own
//     `advance_recovery()` closes the gap. `environment-fields`: "an environment RECOVERS rather
//     than being repainted."
//
// `register_producer()` goes through `environment::FieldRegistry::claim()` and keeps the token. A
// second claim on `vegetation` fails naming both producers, and there is no path in this module
// that writes a field without a token.
//
// ================================================================================================
// REGIONAL STATE IS READ, NEVER STORED
// ================================================================================================
//
// `foliage` — "Regional state SHALL be READ FROM FIELDS rather than stored per instance, so that a
// BURNED FOREST COSTS A FIELD REGION rather than a million instance updates."
//
// `regional_state_at()` is a function of the field store and a position, and there is no member of
// `FoliageInstance` that could hold a state. The scenario's measurement is the absence: a fire
// writes the burn-state field, the foliage in that region reports `Burned`, and the cluster's
// instance bytes are byte-identical before and after — which `test_fields.cpp` checks with a
// memcmp rather than with a claim.
//
// The per-instance half the requirement also names — "with per-instance exceptions where gameplay
// has affected an individual plant" — is exceptions.h's, not a second state model here.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/foliage/exceptions.h>
#include <cy/foliage/grass.h>
#include <cy/foliage/instance.h>
#include <cy/foliage/placement.h>
#include <cy/foliage/species.h>

namespace cy::foliage {

/// The engine's name for the field foliage produces. Not in `environment::fields`, because that
/// namespace is the set `environment-fields` requires the engine to define and this is foliage's
/// own — declared here so two rows naming it name one field.
inline constexpr const char* kVegetationField = "vegetation";
/// Its potential: the vegetation this piece of world would carry given time. The substrate's
/// `potential` link points at it, and the recovery rate on the current field is what closes the
/// gap.
inline constexpr const char* kVegetationPotentialField = "vegetation-potential";

/// The declaration of `vegetation` as foliage produces it: how much plant life a position carries,
/// 0..1, quantised to a byte.
///
/// `Persistent`, because a burned forest must survive a save; CPU-produced, because
/// `validate_declaration()` refuses a gameplay-visible field written by a GPU pass and this one is
/// read by placement, which is gameplay-visible by construction. Its `gameplay_level` is the macro
/// level and the macro level is resident everywhere — which is both the determinism requirement and
/// the thing that lets an unloaded region's ecosystem evolve.
[[nodiscard]] environment::FieldDeclaration vegetation_field_declaration(
    f32 macro_cell_metres, f32 recovery_per_second) noexcept;

/// The potential field's declaration. Static, because what a place COULD support changes when the
/// world is authored and not while it is played.
[[nodiscard]] environment::FieldDeclaration vegetation_potential_declaration(
    f32 macro_cell_metres) noexcept;

/// `foliage` — "Regional environmental state": "normal, wet, dry, burning, burned, snow-covered".
/// The specification's own six, in its order.
enum class RegionalState : u8 {
    Normal = 0,
    Wet,
    Dry,
    Burning,
    Burned,
    SnowCovered,
    kCount,
};

inline constexpr u32 kRegionalStateCount = static_cast<u32>(RegionalState::kCount);

[[nodiscard]] const char* regional_state_name(RegionalState state) noexcept;

/// The thresholds the state is derived at. Declared rather than constant, because "wet" in a
/// rainforest is not "wet" in a desert and a project that could not say so would author around it.
struct StateThresholds {
    f32 wet_above = 0.6F;
    f32 dry_below = 0.2F;
    f32 burning_above = 0.75F;
    f32 burned_above = 0.25F;
    f32 snow_depth_above_metres = 0.05F;
};

/// Which fields the state is read from. Separate from `FieldBindings` — those are the PLACEMENT
/// inputs, evaluated once when a region is generated; these are read every frame by materials, VFX
/// and audio as well, and a project may bind them differently.
struct StateBindings {
    environment::FieldId wetness;
    environment::FieldId moisture;
    environment::FieldId burn_state;
    environment::FieldId snow_depth;

    [[nodiscard]] static StateBindings standard() noexcept;
};

/// The state at a position, and the numbers it came from — so a diagnostic can answer "why is this
/// forest reported burned" with the value rather than with the conclusion.
struct RegionalStateSample {
    RegionalState state = RegionalState::Normal;
    f32 wetness = 0.0F;
    f32 moisture = 0.0F;
    f32 burn = 0.0F;
    f32 snow_depth = 0.0F;
    /// The field version the dominant reading came from, so a consumer holding a derived value
    /// detects staleness by comparing a number.
    u64 version = 0;
};

/// Derive the regional state at a position. Reads; writes nothing. See the header note.
///
/// Deterministic samples throughout: a material that shaded a forest burned on one machine and
/// unburned on another because one had streamed the fine burn-state tile would be exactly the
/// streaming dependence `environment-fields` forbids of a gameplay-visible field.
[[nodiscard]] RegionalStateSample regional_state_at(const environment::FieldStore& fields,
                                                    const StateBindings& bindings,
                                                    const StateThresholds& thresholds,
                                                    const world::WorldVec3d& at) noexcept;

/// What one region's materialisation produced. Generation, exceptions and ground cover together —
/// the thing `foliage`'s "Exceptions are stored, instances are not" describes as a whole.
struct MaterialisedRegion {
    FoliageCluster cluster;
    ClusterBuildReport report;
    Provenance provenance;
    PlacementDiagnostic diagnostic;
    ResolutionReport exceptions;
    /// One resolution per exception of the region, in `ExceptionStore::of_cluster()`'s order.
    Array<Resolution> resolutions;
    /// The ground cover patches the region's `GroundCover` rules produced.
    Array<GrassPatch> patches;

    MaterialisedRegion(FoliageCluster&& built, Provenance&& read, Allocator& allocator) noexcept
        : cluster(static_cast<FoliageCluster&&>(built)),
          provenance(static_cast<Provenance&&>(read)),
          resolutions(allocator),
          patches(allocator) {}

    MaterialisedRegion(const MaterialisedRegion&) = delete;
    MaterialisedRegion& operator=(const MaterialisedRegion&) = delete;
    MaterialisedRegion(MaterialisedRegion&&) noexcept = default;
    MaterialisedRegion& operator=(MaterialisedRegion&&) noexcept = default;
    ~MaterialisedRegion() = default;
};

/// Foliage's seat at the table: what it produces, what it reads, and how a region becomes a
/// cluster.
class FoliageSystem {
public:
    FoliageSystem(Allocator& allocator, const SpeciesLibrary& library) noexcept;

    FoliageSystem(const FoliageSystem&) = delete;
    FoliageSystem& operator=(const FoliageSystem&) = delete;

    /// Declare and claim `vegetation` (and declare its potential, which nothing claims until a
    /// cooker does). `producer_name` must outlive the registry — it is what a refusal prints.
    [[nodiscard]] Status register_producer(environment::FieldRegistry& registry,
                                           const char* producer_name, f32 macro_cell_metres,
                                           f32 recovery_per_second) noexcept;

    /// Declare every field foliage READS, so `FieldRegistry::validate()` can check the firewall
    /// over the whole configuration before a frame runs. `rules` decides which placement inputs are
    /// actually tested, so a world whose rules never mention moisture does not declare a read of
    /// it.
    [[nodiscard]] Status declare_consumption(environment::FieldRegistry& registry,
                                             const char* consumer_name,
                                             const PlacementRuleSet& rules,
                                             const FieldBindings& bindings,
                                             const StateBindings& state) noexcept;

    /// Materialise one region: generate it, apply its exceptions, and report both.
    ///
    /// `exceptions` may be null for a world with none. The order matters and is not negotiable:
    /// ADDED exceptions go in before the cluster is finished, because they change slot numbering;
    /// every other kind is applied after, because they refer to slots.
    [[nodiscard]] Expected<MaterialisedRegion, Error> materialise(const GenerationContext& context,
                                                                  ClusterCoord region,
                                                                  const ExceptionStore* exceptions,
                                                                  const ResolutionPolicy& policy,
                                                                  ClusterId anchored_to) noexcept;

    /// Publish the vegetation this world's clusters actually carry into the field, at one level.
    /// The producer half: nothing else in the engine writes `vegetation`.
    ///
    /// Returns how many field tiles were written.
    [[nodiscard]] Expected<u32, Error> publish_vegetation(
        environment::FieldStore& fields, const ClusterStore& clusters, const ClusterPolicy& policy,
        environment::FieldResidency level) noexcept;

    [[nodiscard]] bool produces_vegetation() const noexcept { return token_.valid(); }
    [[nodiscard]] environment::FieldId vegetation() const noexcept { return vegetation_; }
    [[nodiscard]] environment::FieldId vegetation_potential() const noexcept { return potential_; }

private:
    Allocator* allocator_;
    const SpeciesLibrary* library_;
    environment::ProducerToken token_;
    environment::FieldId vegetation_;
    environment::FieldId potential_;
};

}  // namespace cy::foliage

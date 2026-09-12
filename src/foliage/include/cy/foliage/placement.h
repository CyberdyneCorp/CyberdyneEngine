#pragma once
// Procedural placement: rules evaluated against terrain and environment fields, region by region,
// deterministically. M10 task 2.4.
//
// `foliage` — "Procedural placement": "Foliage SHALL be placeable by RULES evaluated against
// environment fields and terrain: biome, slope, altitude, moisture, temperature, soil, sun
// exposure, water distance, noise, roads, and exclusion zones", producing "species selection,
// density, scale, orientation, variation, and age", and "Rule evaluation SHALL be DETERMINISTIC,
// derived from the world seed, a region identifier, and the rule graph version, so that a region
// regenerates identically rather than being serialised instance by instance."
//
// ================================================================================================
// WHAT THIS FILE IS *NOT*, AND WHERE THAT LEAVES IT
// ================================================================================================
//
// The specification is explicit: "Placement rules SHALL be PROCEDURAL PROGRAMS (see
// `procedural-content-generation`) ... Foliage SHALL NOT MAINTAIN A SEPARATE PROCEDURAL EXECUTION
// OR INVALIDATION MODEL."
//
// `src/pcg/` does not exist yet — it is M10 section 4 and this is section 2.4. So this file builds
// the two halves that are foliage's own and refuses to build the one that is not:
//
//   * THE RULES AND THE EVALUATION ARE HERE, because what a placement rule READS — slope, moisture,
//     water distance, the soil category terrain produced — is foliage's knowledge and nobody
//     else's.
//   * THE EXECUTION AND INVALIDATION MODEL IS NOT HERE. `RegionGeneration` is a PURE FUNCTION of
//     its declared inputs and reports, in `Provenance`, exactly which regions and which fields it
//     actually read. There is no cache in this module, no dirty set, no dependency graph and no
//     traversal. Those belong to `procedural-content-generation` (task 4.3), and the provenance
//     record is the input that capability needs from this one.
//
// The seam is deliberate and is the honest position: a cache written here would be the "separate
// invalidation model" the requirement forbids, and the row that owns caching has not landed.
//
// ================================================================================================
// THE FOUR CONDITIONS THE SPIKE MADE BINDING, AND WHERE EACH ONE IS IN THIS FILE
// ================================================================================================
//
// M10's PCG spike (design.md §1) measured 24 configurations over 12 trials and found exactly two
// that reproduce a full regeneration from a partial one, bit for bit, for output AND for generated
// identity. Four properties have to hold at once. Three of them constrain this file and the fourth
// constrains instance.h:
//
//   1. ORDER-FREE CONFLICT RESOLUTION. "A node may read neighbours' CANDIDATES, never their
//   ACCEPTED
//      OUTPUT." `resolve_spacing()` compares a candidate against every OTHER CANDIDATE within the
//      declared reach — including candidates from neighbouring regions, which are regenerated for
//      the comparison rather than read out of a neighbour's result. A candidate's fate is therefore
//      a function of the candidate sets alone, and the candidate sets are functions of their own
//      regions. Generating the world in any order, or one region alone, gives the same answer.
//      The `ordered` variant the spike measured reproduced 2 of 12 trials at best.
//   2. INVALIDATION IS A FIXED POINT, NOT A DECLARED RADIUS — and the fixed point is PCG's, not
//      this module's. What this file owes it is `Provenance`: what each region ACTUALLY READ, so
//      the dirty set is computed from a record rather than dilated by a radius. The spike's own
//      design note is that the cost of partial regeneration is dominated by long-range gathers
//      (179 regions invalidated to find 1 changed), and that the answer is a provenance record
//      rather than a wider radius. This is that record.
//   3. AN ITERATIVE OPERATOR DECLARES CONVERGENCE, NOT A BUDGET. **There is no iterative operator
//      in this file**, and that is a decision rather than an omission: spacing is resolved in one
//      order-free comparison pass rather than by relaxation, precisely so there is no sweep count
//      to truncate. The spike's `budget2` axis reproduced 0 of 12 trials in ALL twelve of its
//      configurations — it is the only axis with no survivor anywhere — and the cheapest way to
//      stay off it is to have nothing to budget.
//   4. IDENTITY IS DERIVED FROM STABLE IDENTIFIERS. instance.h's `instance_identity()`, and
//      `ClusterBuilder::finish()`'s canonical order, which is what makes a slot stable.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/random.h>
#include <cy/core/memory/array.h>
#include <cy/environment/store.h>
#include <cy/foliage/instance.h>
#include <cy/foliage/species.h>
#include <cy/terrain/surface.h>

namespace cy::foliage {

/// What a rule may test. `foliage`'s own list, in its order, plus the ecosystem state the
/// "A grown forest appears grown" scenario needs.
enum class RuleInput : u8 {
    /// Degrees from horizontal, from the terrain query.
    Slope = 0,
    /// Metres, from the terrain query.
    Altitude,
    /// Terrain curvature: positive on a ridge, negative in a hollow.
    Curvature,
    /// The biome category. Tested against `categories`, never against a range.
    Biome,
    Moisture,
    Temperature,
    /// The soil category terrain produces. Tested against `categories`.
    Soil,
    /// 0..1, how much sky the position sees. Derived from slope and aspect against the declared
    /// sun direction; there is no ray cast here, because a placement rule that traced a ray would
    /// be a placement rule whose answer depended on what geometry had streamed.
    SunExposure,
    /// Metres to the nearest water, from `water`'s `water-distance` field.
    WaterDistance,
    /// Value noise over the world, deterministic from the seed and the rule. What makes a forest
    /// clumpy rather than uniform.
    Noise,
    /// 0..1 human exclusion — roads, settlements, cleared ground — from `human-exclusion`.
    Road,
    /// The region's ecosystem state: how much vegetation this piece of world currently supports,
    /// which evolves while the region is unloaded. `foliage` — "Where a region's macro ecosystem
    /// state has evolved while unloaded, materialisation SHALL be consistent with that state."
    Vegetation,
    /// 0..1 burn state. A burned region's foliage is placed burned, not deleted.
    BurnState,
    kCount,
};

inline constexpr u32 kRuleInputCount = static_cast<u32>(RuleInput::kCount);

[[nodiscard]] const char* rule_input_name(RuleInput input) noexcept;

/// Whether an input is a CATEGORY — an integer naming a thing — or a continuous quantity.
/// `environment::FieldType::Category` draws the same line for the same reason: a biome index
/// interpolated between two values is an index naming nothing.
[[nodiscard]] bool is_category_input(RuleInput input) noexcept;

/// One test in a rule. A trapezoid over a continuous input, or a set membership over a category.
///
/// A trapezoid and not a step, because a hard cutoff draws a visible line across a hillside where
/// the slope crosses it. `inner_min`/`inner_max` is where the weight is 1 and `outer_min`/
/// `outer_max` is where it reaches 0.
struct RuleTest {
    RuleInput input = RuleInput::Slope;
    f32 outer_min = 0.0F;
    f32 inner_min = 0.0F;
    f32 inner_max = 1.0F;
    f32 outer_max = 1.0F;
    /// For a category input: a bitmask of the indices that pass. Index 63 and above cannot be
    /// named, which is reported by `validate_rules()` rather than silently failing every test.
    u64 categories = 0;
    /// True to invert the test — "anywhere BUT a road".
    bool negate = false;

    /// The weight this test gives a sampled value, in [0, 1].
    [[nodiscard]] f32 weight(f32 value) const noexcept;
};

/// The largest number of tests one rule may carry. Eight is every input a real rule uses at once
/// plus room; a rule needing more is two rules, and the bound keeps a rule a fixed-size record that
/// a cooker can write without an allocation per rule.
inline constexpr u32 kMaxRuleTests = 8;

/// How an instance is oriented. `foliage` — rules produce "ORIENTATION".
enum class OrientationMode : u8 {
    /// Straight up whatever the ground does. A pine.
    Upright = 0,
    /// Tilted toward the surface normal, by `slope_alignment`. A bush on a bank.
    AlignToSlope,
    /// Upright, with a random lean up to `max_tilt_degrees`. Deadfall, old trees.
    RandomLean,
};

[[nodiscard]] const char* orientation_mode_name(OrientationMode mode) noexcept;

/// One placement rule.
struct PlacementRule {
    SpeciesId species;
    /// Instances per hectare where every test gives weight 1. A density and not a count, so a rule
    /// means the same thing under any cluster policy.
    f32 density_per_hectare = 100.0F;

    RuleTest tests[kMaxRuleTests];
    u32 test_count = 0;

    /// The minimum distance between two instances of this rule, in metres. Resolved ORDER-FREE
    /// across region boundaries — see the header note's condition 1.
    f32 spacing_metres = 2.0F;

    /// Which rule wins when two candidates are too close. Higher wins; a tie is broken by the
    /// candidate's derived identity, never by traversal order.
    u32 priority = 0;

    OrientationMode orientation = OrientationMode::Upright;
    /// How far toward the surface normal an `AlignToSlope` instance leans, 0..1.
    f32 slope_alignment = 0.5F;
    /// The cap on `RandomLean` and on the tilt an alignment may produce, in degrees. Clamped into
    /// the +-32 degrees `FoliageInstance` can store, which is where the storage limit becomes a
    /// declared one rather than a silent clip.
    f32 max_tilt_degrees = 12.0F;

    /// Age drawn uniformly in this range, mapped onto `FoliageInstance::age`'s byte. An age range
    /// centred high is a mature stand; centred low is regrowth after a fire.
    f32 age_min = 0.4F;
    f32 age_max = 1.0F;

    /// The noise field this rule's `RuleInput::Noise` test reads, in metres per feature. Per rule,
    /// so two species clump at different scales rather than in the same patches.
    f32 noise_metres = 120.0F;

    [[nodiscard]] Span<const RuleTest> active_tests() const noexcept { return {tests, test_count}; }
};

/// A region where rules are suppressed, or one species is. `foliage` — placement reads "exclusion
/// zones", and "an author ... MAY SUPPRESS RULES WITHIN A REGION."
struct ExclusionZone {
    f64 min_x = 0.0;
    f64 min_z = 0.0;
    f64 max_x = 0.0;
    f64 max_z = 0.0;
    /// A zero identity excludes every species; a set one excludes only that species.
    SpeciesId species;
    /// A soft edge, in metres: within it the density falls off rather than stopping at a line.
    f32 feather_metres = 0.0F;

    /// 1 outside the zone, 0 well inside it, feathered between.
    [[nodiscard]] f32 admittance(f64 x, f64 z, SpeciesId candidate) const noexcept;
};

/// Everything placement is evaluated against, plus the version that makes a change regenerate.
struct PlacementRuleSet {
    Array<PlacementRule> rules;
    Array<ExclusionZone> exclusions;

    /// The rule graph's version. `foliage` — evaluation is "derived from the world seed, a region
    /// identifier, and THE RULE GRAPH VERSION". It participates in `cluster_identity()`, so
    /// bumping it renames every cluster and therefore regenerates every region — which is the
    /// specification's "changing a rule graph regenerates the region" as arithmetic rather than as
    /// a procedure somebody runs.
    u32 graph_version = 1;

    /// The sun direction sun exposure is computed against. A declaration and not a sample of the
    /// sky, because placement must produce the same forest at midnight as at noon.
    Vec3 sun_direction{0.0F, -1.0F, 0.0F};

    explicit PlacementRuleSet(Allocator& allocator) noexcept
        : rules(allocator), exclusions(allocator) {}

    PlacementRuleSet(const PlacementRuleSet&) = delete;
    PlacementRuleSet& operator=(const PlacementRuleSet&) = delete;
    PlacementRuleSet(PlacementRuleSet&&) noexcept = default;
    PlacementRuleSet& operator=(PlacementRuleSet&&) noexcept = default;
    ~PlacementRuleSet() = default;

    /// The largest `spacing_metres` any rule declares. The reach a region's generation may read
    /// beyond its own bounds, and the halo `generate_region()` regenerates candidates in.
    [[nodiscard]] f32 reach_metres() const noexcept;
};

/// Why a rule set was refused.
enum class RuleProblem : u8 {
    None = 0,
    NoRules,
    UnknownSpecies,
    NonPositiveDensity,
    /// A trapezoid whose corners are out of order.
    TestNotOrdered,
    /// A category test with no category selected: it can never pass.
    EmptyCategorySet,
    /// A category test spelled as a range, or a continuous test spelled as a category set.
    CategoryInputMismatch,
    NegativeSpacing,
    /// `age_min > age_max`.
    EmptyAgeRange,
    ZeroGraphVersion,
};

[[nodiscard]] const char* rule_problem_name(RuleProblem problem) noexcept;

[[nodiscard]] RuleProblem validate_rules(const PlacementRuleSet& rules,
                                         const SpeciesLibrary& library) noexcept;

// --- What a rule is evaluated against
// -------------------------------------------------------------

/// The engine-side inputs one candidate position is tested with. Sampled once per candidate and
/// then tested against every rule, because the expensive half is the terrain column and the field
/// samples and both are the same for every rule at one position.
struct PlacementInputs {
    f32 values[kRuleInputCount] = {};
    /// Whether the terrain had a surface at all. A candidate on a hole is not placed: `terrain`'s
    /// own rule is that a hole is NOT a surface, and a consumer that only checked `resolved` could
    /// plant a tree in a cave mouth.
    bool has_surface = false;
    /// The surface the candidate would stand on, as terrain answered.
    terrain::SurfaceSample surface;

    [[nodiscard]] f32 get(RuleInput input) const noexcept {
        return values[static_cast<u32>(input)];
    }
    void set(RuleInput input, f32 value) noexcept { values[static_cast<u32>(input)] = value; }
};

/// Which environment field each field-backed input reads. A DECLARATION rather than a hard-coded
/// set of names, so a project that calls its moisture field something else configures this instead
/// of editing the engine — and so `FoliageSystem::declare_consumption()` can tell the registry
/// exactly which fields foliage reads before a frame runs.
struct FieldBindings {
    environment::FieldId biome;
    environment::FieldId moisture;
    environment::FieldId temperature;
    environment::FieldId soil;
    environment::FieldId water_distance;
    environment::FieldId human_exclusion;
    environment::FieldId vegetation;
    environment::FieldId burn_state;

    /// The engine's standard names. A project overrides any member afterwards.
    [[nodiscard]] static FieldBindings standard() noexcept;

    /// The bound field for an input, or an invalid identity where the input is not field-backed
    /// (slope, altitude, curvature, sun exposure and noise are computed, not read).
    [[nodiscard]] environment::FieldId field_for(RuleInput input) const noexcept;
};

/// Gathers `PlacementInputs` from the terrain query and the field store.
///
/// **FIELD SAMPLES GO THROUGH `sample_deterministic()`, NEVER `sample()`.** A forest whose density
/// came from the finest resident level would be a forest that differs between a machine that had
/// streamed the region and one that had not — `environment::FieldStore`'s own header note, applied.
/// The terrain half goes through `terrain::TerrainQuery::column()`, so a world with mesh cliffs and
/// arches places on the surface that is actually there rather than on an assumed heightfield.
class PlacementSampler {
public:
    PlacementSampler(const terrain::TerrainQuery& terrain, const environment::FieldStore& fields,
                     const FieldBindings& bindings, const Vec3& sun_direction) noexcept;

    /// Gather every input at one position. Never blocks, never faults: both stores answer with what
    /// they have and say how good the answer is.
    [[nodiscard]] PlacementInputs at(f64 x, f64 z) const noexcept;

    /// The noise a rule's `Noise` test reads, at a position and a feature size. Value noise over a
    /// deterministic lattice, keyed by the seed and the rule, so two machines agree and two rules
    /// clump differently.
    [[nodiscard]] static f32 placement_noise(u64 seed, u32 rule_index, f64 x, f64 z,
                                             f32 metres) noexcept;

    /// The bindings this sampler reads through. `generate_region()` records them in the provenance,
    /// and a provenance that named the STANDARD bindings while the sampler read a project's own
    /// would tell the invalidation the wrong fields to watch.
    [[nodiscard]] const FieldBindings& bindings() const noexcept { return bindings_; }

private:
    const terrain::TerrainQuery* terrain_;
    const environment::FieldStore* fields_;
    FieldBindings bindings_;
    Vec3 sun_direction_;
};

// --- Generation
// -----------------------------------------------------------------------------------

/// One candidate before conflict resolution. Never leaves this module's generation path except in
/// the diagnostic: it is what condition 1 says a neighbour may read.
struct Candidate {
    world::WorldVec3d position;
    SpeciesId species;
    /// Which rule produced it, and which sample index within the rule. Together with the region
    /// they are the stable identifiers every draw for this candidate is keyed by.
    u32 rule_index = 0;
    u32 sample_index = 0;
    /// The derived tie-break. Two candidates at the same priority are resolved by comparing this,
    /// which is a function of (seed, graph version, region, rule, sample) and of nothing else.
    u64 tie_break = 0;
    u32 priority = 0;
    f32 spacing_metres = 0.0F;
    f32 weight = 0.0F;
    /// The region this candidate was generated for. A candidate from the halo carries its own.
    ClusterCoord region;
    /// Filled in by evaluation; carried so acceptance does not resample the terrain.
    f32 height = 0.0F;
    f32 slope_degrees = 0.0F;
    Vec3 normal{0.0F, 1.0F, 0.0F};
    f32 vegetation = 1.0F;
};

/// What a region's generation actually read. The record `procedural-content-generation`'s
/// invalidation needs from this module — see the header note's condition 2.
///
/// It is a record of READS and not a declared radius. A region whose rules all have 2 m spacing
/// reads one ring of neighbours whatever the declared reach of the rule set as a whole is, and the
/// spike's measurement is that a declared reach costs 179 invalidated regions to find 1 changed.
struct Provenance {
    /// Regions whose CANDIDATES this region's result depended on, including itself.
    Array<ClusterCoord> regions;
    /// Fields this region sampled. Empty where a rule set tests nothing field-backed.
    Array<environment::FieldId> fields;
    /// The bounds actually sampled, so a field change event's bounds can be intersected with it
    /// rather than with the region's own square.
    f64 min_x = 0.0;
    f64 min_z = 0.0;
    f64 max_x = 0.0;
    f64 max_z = 0.0;

    explicit Provenance(Allocator& allocator) noexcept : regions(allocator), fields(allocator) {}

    Provenance(const Provenance&) = delete;
    Provenance& operator=(const Provenance&) = delete;
    Provenance(Provenance&&) noexcept = default;
    Provenance& operator=(Provenance&&) noexcept = default;
    ~Provenance() = default;

    /// Whether a changed rectangle intersects what this region read. The predicate PCG's fixed
    /// point evaluates; it is here because only this module knows what its own generation touched.
    [[nodiscard]] bool intersects(f64 x0, f64 z0, f64 x1, f64 z1) const noexcept;
    [[nodiscard]] bool read_region(ClusterCoord region) const noexcept;
};

/// Why a candidate was not placed, counted. `foliage` — "WHEN a region generates no foliage THEN
/// the editor SHALL show WHICH RULE INPUT EXCLUDED IT."
struct PlacementDiagnostic {
    /// Candidates whose weight fell to zero because of this input, per input.
    u32 excluded_by[kRuleInputCount] = {};
    u32 candidates = 0;
    u32 accepted = 0;
    u32 rejected_no_surface = 0;
    u32 rejected_hole = 0;
    u32 rejected_exclusion_zone = 0;
    u32 rejected_spacing = 0;
    u32 rejected_weight = 0;

    /// The input that excluded the most candidates, for the editor's own answer. `kRuleInputCount`
    /// when nothing was excluded by an input.
    [[nodiscard]] u32 dominant_exclusion() const noexcept;
};

/// One region's generated population, before exceptions are applied.
///
/// `foliage` — "generation SHALL produce FOLIAGE POPULATIONS through the output adapter rather than
/// entities". This type is that population: a cluster, the report of what building it cost, the
/// provenance of what it read, and the diagnostic of what it refused.
struct FoliagePopulation {
    FoliageCluster cluster;
    ClusterBuildReport report;
    Provenance provenance;
    PlacementDiagnostic diagnostic;

    FoliagePopulation(FoliageCluster&& built, const ClusterBuildReport& built_report,
                      Provenance&& read, const PlacementDiagnostic& why) noexcept
        : cluster(static_cast<FoliageCluster&&>(built)),
          report(built_report),
          provenance(static_cast<Provenance&&>(read)),
          diagnostic(why) {}

    FoliagePopulation(const FoliagePopulation&) = delete;
    FoliagePopulation& operator=(const FoliagePopulation&) = delete;
    FoliagePopulation(FoliagePopulation&&) noexcept = default;
    FoliagePopulation& operator=(FoliagePopulation&&) noexcept = default;
    ~FoliagePopulation() = default;
};

/// Everything a generation needs that is not the region itself.
struct GenerationContext {
    u64 seed = 0;
    const SpeciesLibrary* library = nullptr;
    const PlacementRuleSet* rules = nullptr;
    const PlacementSampler* sampler = nullptr;
    ClusterPolicy policy;

    /// Whether the region's own ecosystem state scales density. Off in a cook that wants the
    /// potential vegetation rather than the current one.
    bool apply_ecosystem_state = true;
};

/// The candidates one region produces. **A pure function of the context and the region**, which is
/// what condition 1 needs of it: a neighbour reads this, and reading it must not depend on anything
/// the neighbour has done.
///
/// Exposed rather than private because that is what makes the property testable:
/// `test_placement.cpp` generates one region's candidates a thousand times in different orders and
/// against different neighbours and requires the same array every time.
[[nodiscard]] Status generate_candidates(const GenerationContext& context, ClusterCoord region,
                                         Array<Candidate>& out, PlacementDiagnostic& why) noexcept;

/// Resolve spacing across a candidate set. **ORDER-FREE**: a candidate survives iff no OTHER
/// CANDIDATE within its spacing distance beats it on (priority, tie_break). Nothing here consults
/// an accepted set, so the answer does not depend on the order the array is in — and
/// `test_placement.cpp` shuffles the array and requires the same survivors.
///
/// `accepted` is filled with the indices into `candidates` that survive, ascending.
[[nodiscard]] Status resolve_spacing(Span<const Candidate> candidates, Array<u32>& accepted,
                                     PlacementDiagnostic& why) noexcept;

/// Generate one region: candidates for it and for every region in the halo its rules reach, one
/// order-free spacing resolution over the union, and a cluster built from the survivors that fall
/// inside this region.
///
/// A halo candidate is REGENERATED rather than read out of a neighbour's cluster. That costs the
/// neighbour's candidates again and buys the property the whole design rests on: the result does
/// not depend on whether the neighbour has been generated, on what it accepted, or on when.
[[nodiscard]] Expected<FoliagePopulation, Error> generate_region(Allocator& allocator,
                                                                 const GenerationContext& context,
                                                                 ClusterCoord region) noexcept;

/// The bounds of one region under a policy. One function, so the cluster grid is defined in exactly
/// one place and a region's square and its cluster's bounds cannot drift apart.
[[nodiscard]] ClusterBounds region_bounds(const ClusterPolicy& policy, ClusterCoord region,
                                          f32 min_height, f32 max_height) noexcept;

/// The region a world position falls in, at level 0.
[[nodiscard]] ClusterCoord region_of(const ClusterPolicy& policy, f64 x, f64 z) noexcept;

}  // namespace cy::foliage

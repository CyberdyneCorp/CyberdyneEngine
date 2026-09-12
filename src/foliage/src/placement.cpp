// Rules, their inputs, and the order-free region generation. See placement.h for the two design
// notes: what this file deliberately is not, and where each of the spike's four conditions lives.

#include <cy/foliage/placement.h>

#include <cy/core/math/scalar.h>
#include <cy/core/memory/hash_map.h>

#include <algorithm>
#include <cmath>

namespace cy::foliage {

const char* rule_input_name(RuleInput input) noexcept {
    switch (input) {
        case RuleInput::Slope:
            return "slope";
        case RuleInput::Altitude:
            return "altitude";
        case RuleInput::Curvature:
            return "curvature";
        case RuleInput::Biome:
            return "biome";
        case RuleInput::Moisture:
            return "moisture";
        case RuleInput::Temperature:
            return "temperature";
        case RuleInput::Soil:
            return "soil";
        case RuleInput::SunExposure:
            return "sun-exposure";
        case RuleInput::WaterDistance:
            return "water-distance";
        case RuleInput::Noise:
            return "noise";
        case RuleInput::Road:
            return "road";
        case RuleInput::Vegetation:
            return "vegetation";
        case RuleInput::BurnState:
            return "burn-state";
        case RuleInput::kCount:
            break;
    }
    return "unknown";
}

bool is_category_input(RuleInput input) noexcept {
    return input == RuleInput::Biome || input == RuleInput::Soil;
}

const char* orientation_mode_name(OrientationMode mode) noexcept {
    switch (mode) {
        case OrientationMode::Upright:
            return "upright";
        case OrientationMode::AlignToSlope:
            return "align-to-slope";
        case OrientationMode::RandomLean:
            return "random-lean";
    }
    return "unknown";
}

const char* rule_problem_name(RuleProblem problem) noexcept {
    switch (problem) {
        case RuleProblem::None:
            return "none";
        case RuleProblem::NoRules:
            return "no-rules";
        case RuleProblem::UnknownSpecies:
            return "unknown-species";
        case RuleProblem::NonPositiveDensity:
            return "non-positive-density";
        case RuleProblem::TestNotOrdered:
            return "test-not-ordered";
        case RuleProblem::EmptyCategorySet:
            return "empty-category-set";
        case RuleProblem::CategoryInputMismatch:
            return "category-input-mismatch";
        case RuleProblem::NegativeSpacing:
            return "negative-spacing";
        case RuleProblem::EmptyAgeRange:
            return "empty-age-range";
        case RuleProblem::ZeroGraphVersion:
            return "zero-graph-version";
    }
    return "unknown";
}

f32 RuleTest::weight(f32 value) const noexcept {
    f32 result = 0.0F;
    if (is_category_input(input)) {
        const u32 index = static_cast<u32>(value < 0.0F ? 0.0F : value);
        // Index 64 and above cannot be named by a 64-bit mask. `validate_rules()` reports the test
        // that would need one; here it simply fails, which is the conservative direction — a rule
        // that placed on every unnameable biome would be worse than one that placed on none.
        result = (index < 64U && ((categories >> index) & 1ULL) != 0ULL) ? 1.0F : 0.0F;
    } else if (value < outer_min || value > outer_max) {
        result = 0.0F;
    } else if (value >= inner_min && value <= inner_max) {
        // The plateau is checked BEFORE the outer bounds are treated as exclusive, because a test
        // whose outer and inner bounds coincide is a HARD EDGE rather than a ramp of zero width. A
        // `value <= outer_min` test rejected a perfectly flat surface for a rule declaring
        // `outer_min = inner_min = 0`, which is every "slope at most N degrees" rule ever written;
        // `test_placement.cpp`'s mesh-ledge case is the regression.
        result = 1.0F;
    } else if (value < inner_min) {
        const f32 span = inner_min - outer_min;
        result = span > 0.0F ? (value - outer_min) / span : 1.0F;
    } else {
        const f32 span = outer_max - inner_max;
        result = span > 0.0F ? (outer_max - value) / span : 1.0F;
    }
    return negate ? 1.0F - result : result;
}

namespace {

/// How far a coordinate lies outside an interval, in metres. Zero inside it. Named because the
/// nested conditional it replaces appeared once per axis and read as two different rules.
[[nodiscard]] f64 axis_gap(f64 value, f64 low, f64 high) noexcept {
    if (value < low) {
        return low - value;
    }
    if (value >= high) {
        return value - high;
    }
    return 0.0;
}

}  // namespace

f32 ExclusionZone::admittance(f64 x, f64 z, SpeciesId candidate) const noexcept {
    if (species.is_valid() && !(species == candidate)) {
        return 1.0F;
    }
    if (x < min_x || x >= max_x || z < min_z || z >= max_z) {
        if (feather_metres <= 0.0F) {
            return 1.0F;
        }
        // Outside, but possibly inside the feather. The distance to the rectangle, in metres.
        const f64 dx = axis_gap(x, min_x, max_x);
        const f64 dz = axis_gap(z, min_z, max_z);
        const f64 distance = std::sqrt((dx * dx) + (dz * dz));
        const f64 feather = static_cast<f64>(feather_metres);
        return distance >= feather ? 1.0F : static_cast<f32>(distance / feather);
    }
    return 0.0F;
}

f32 PlacementRuleSet::reach_metres() const noexcept {
    f32 reach = 0.0F;
    for (const PlacementRule& rule : rules) {
        reach = rule.spacing_metres > reach ? rule.spacing_metres : reach;
    }
    return reach;
}

namespace {

[[nodiscard]] RuleProblem validate_test(const RuleTest& test) noexcept {
    if (is_category_input(test.input)) {
        if (test.categories == 0) {
            return RuleProblem::EmptyCategorySet;
        }
        return RuleProblem::None;
    }
    if (test.categories != 0) {
        // A continuous input with a category mask is a rule the author wrote for a different
        // input; refusing it is how that is caught rather than silently ignored.
        return RuleProblem::CategoryInputMismatch;
    }
    if (test.outer_min > test.inner_min || test.inner_min > test.inner_max ||
        test.inner_max > test.outer_max) {
        return RuleProblem::TestNotOrdered;
    }
    return RuleProblem::None;
}

}  // namespace

RuleProblem validate_rules(const PlacementRuleSet& rules, const SpeciesLibrary& library) noexcept {
    if (rules.graph_version == 0) {
        return RuleProblem::ZeroGraphVersion;
    }
    if (rules.rules.empty()) {
        return RuleProblem::NoRules;
    }
    for (const PlacementRule& rule : rules.rules) {
        if (library.find(rule.species) == nullptr) {
            return RuleProblem::UnknownSpecies;
        }
        if (!(rule.density_per_hectare > 0.0F)) {
            return RuleProblem::NonPositiveDensity;
        }
        if (rule.spacing_metres < 0.0F) {
            return RuleProblem::NegativeSpacing;
        }
        if (!(rule.age_max >= rule.age_min)) {
            return RuleProblem::EmptyAgeRange;
        }
        for (const RuleTest& test : rule.active_tests()) {
            if (const RuleProblem problem = validate_test(test); problem != RuleProblem::None) {
                return problem;
            }
        }
    }
    return RuleProblem::None;
}

// --- Bindings and sampling
// ------------------------------------------------------------------------

FieldBindings FieldBindings::standard() noexcept {
    FieldBindings bindings;
    bindings.biome = environment::field_id(environment::fields::kBiome);
    bindings.moisture = environment::field_id(environment::fields::kMoisture);
    bindings.temperature = environment::field_id(environment::fields::kTemperature);
    bindings.soil = environment::field_id(environment::fields::kSoil);
    bindings.water_distance = environment::field_id(environment::fields::kWaterDistance);
    bindings.human_exclusion = environment::field_id(environment::fields::kHumanExclusion);
    bindings.vegetation = environment::field_id("vegetation");
    bindings.burn_state = environment::field_id(environment::fields::kBurnState);
    return bindings;
}

environment::FieldId FieldBindings::field_for(RuleInput input) const noexcept {
    switch (input) {
        case RuleInput::Biome:
            return biome;
        case RuleInput::Moisture:
            return moisture;
        case RuleInput::Temperature:
            return temperature;
        case RuleInput::Soil:
            return soil;
        case RuleInput::WaterDistance:
            return water_distance;
        case RuleInput::Road:
            return human_exclusion;
        case RuleInput::Vegetation:
            return vegetation;
        case RuleInput::BurnState:
            return burn_state;
        // Slope, altitude, curvature, sun exposure and noise are COMPUTED. Returning an invalid
        // identity for them is what stops `declare_consumption()` from telling the registry that
        // foliage reads a field called "slope".
        case RuleInput::Slope:
        case RuleInput::Altitude:
        case RuleInput::Curvature:
        case RuleInput::SunExposure:
        case RuleInput::Noise:
        case RuleInput::kCount:
            break;
    }
    return environment::FieldId{};
}

PlacementSampler::PlacementSampler(const terrain::TerrainQuery& terrain,
                                   const environment::FieldStore& fields,
                                   const FieldBindings& bindings,
                                   const Vec3& sun_direction) noexcept
    : terrain_(&terrain), fields_(&fields), bindings_(bindings), sun_direction_(sun_direction) {}

namespace {

/// One field's deterministic value at a position, or zero where the field is not declared. Zero and
/// not "skip the test": a rule that tests a field the world does not have should place nothing,
/// which is what a zero produces through the test's own trapezoid, rather than placing everywhere.
[[nodiscard]] f32 field_value(const environment::FieldStore& fields, environment::FieldId field,
                              const world::WorldVec3d& at) noexcept {
    if (!field.is_valid() || fields.registry().declaration(field) == nullptr) {
        return 0.0F;
    }
    // `sample_deterministic()` and never `sample()`: see placement.h's note on why.
    return fields.sample_deterministic(field, at).value.x();
}

/// Value noise over a lattice, keyed by the seed and the rule. A hash-and-interpolate rather than a
/// gradient noise: what a placement rule needs is a smooth clumping field that is the same on every
/// machine, and the cheapest such thing is this.
[[nodiscard]] f32 lattice_value(u64 seed, u32 rule_index, i64 ix, i64 iz) noexcept {
    u64 mixed = hash_integer(seed ^ 0x9e37'79b9'7f4a'7c15ULL, 0x6379'6265'726e'7365ULL);
    mixed = hash_combine(mixed, rule_index);
    mixed = hash_combine(mixed, static_cast<u64>(ix));
    mixed = hash_combine(mixed, static_cast<u64>(iz));
    return static_cast<f32>(mixed >> 40U) * 0x1.0p-24F;
}

[[nodiscard]] f32 smoothstep_unit(f32 t) noexcept {
    return t * t * (3.0F - (2.0F * t));
}

}  // namespace

f32 PlacementSampler::placement_noise(u64 seed, u32 rule_index, f64 x, f64 z, f32 metres) noexcept {
    const f64 scale = metres > 0.0F ? static_cast<f64>(metres) : 1.0;
    const f64 fx = x / scale;
    const f64 fz = z / scale;
    const f64 bx = std::floor(fx);
    const f64 bz = std::floor(fz);
    const auto ix = static_cast<i64>(bx);
    const auto iz = static_cast<i64>(bz);
    const f32 tx = smoothstep_unit(static_cast<f32>(fx - bx));
    const f32 tz = smoothstep_unit(static_cast<f32>(fz - bz));
    const f32 v00 = lattice_value(seed, rule_index, ix, iz);
    const f32 v10 = lattice_value(seed, rule_index, ix + 1, iz);
    const f32 v01 = lattice_value(seed, rule_index, ix, iz + 1);
    const f32 v11 = lattice_value(seed, rule_index, ix + 1, iz + 1);
    const f32 a = v00 + ((v10 - v00) * tx);
    const f32 b = v01 + ((v11 - v01) * tx);
    return a + ((b - a) * tz);
}

PlacementInputs PlacementSampler::at(f64 x, f64 z) const noexcept {
    PlacementInputs inputs;
    inputs.surface = terrain_->sample(x, z);
    // A hole is NOT a surface: `terrain`'s own rule, and the reason `resolved` alone is not the
    // test. A candidate on a cave mouth is rejected before any rule is evaluated.
    inputs.has_surface = inputs.surface.resolved && !inputs.surface.hole;
    if (!inputs.has_surface) {
        return inputs;
    }
    const world::WorldVec3d position{x, static_cast<f64>(inputs.surface.height), z};
    inputs.set(RuleInput::Slope, inputs.surface.slope_degrees);
    inputs.set(RuleInput::Altitude, inputs.surface.height);
    inputs.set(RuleInput::Curvature, inputs.surface.curvature);
    // Sun exposure from the surface normal against the declared direction. No ray cast: see
    // placement.h — an exposure that traced geometry would depend on what had streamed.
    const Vec3 normal = inputs.surface.normal;
    const f32 facing = -((normal.x * sun_direction_.x) + (normal.y * sun_direction_.y) +
                         (normal.z * sun_direction_.z));
    inputs.set(RuleInput::SunExposure, math::clamp(facing, 0.0F, 1.0F));
    inputs.set(RuleInput::Biome, field_value(*fields_, bindings_.biome, position));
    inputs.set(RuleInput::Moisture, field_value(*fields_, bindings_.moisture, position));
    inputs.set(RuleInput::Temperature, field_value(*fields_, bindings_.temperature, position));
    inputs.set(RuleInput::Soil, field_value(*fields_, bindings_.soil, position));
    inputs.set(RuleInput::WaterDistance, field_value(*fields_, bindings_.water_distance, position));
    inputs.set(RuleInput::Road, field_value(*fields_, bindings_.human_exclusion, position));
    inputs.set(RuleInput::BurnState, field_value(*fields_, bindings_.burn_state, position));
    // The ecosystem state. Defaulted to 1 where the world declares no vegetation field, so a world
    // without one places at its rules' full density rather than at nothing.
    const bool has_vegetation = bindings_.vegetation.is_valid() &&
                                fields_->registry().declaration(bindings_.vegetation) != nullptr;
    inputs.set(RuleInput::Vegetation,
               has_vegetation ? field_value(*fields_, bindings_.vegetation, position) : 1.0F);
    return inputs;
}

// --- Provenance and diagnostics
// --------------------------------------------------------------------

bool Provenance::intersects(f64 x0, f64 z0, f64 x1, f64 z1) const noexcept {
    return x1 >= min_x && x0 <= max_x && z1 >= min_z && z0 <= max_z;
}

bool Provenance::read_region(ClusterCoord region) const noexcept {
    return std::ranges::any_of(regions,
                               [region](const ClusterCoord& read) { return read == region; });
}

u32 PlacementDiagnostic::dominant_exclusion() const noexcept {
    u32 best = kRuleInputCount;
    u32 most = 0;
    for (u32 index = 0; index < kRuleInputCount; ++index) {
        if (excluded_by[index] > most) {
            most = excluded_by[index];
            best = index;
        }
    }
    return best;
}

// --- Region geometry
// -------------------------------------------------------------------------------

ClusterBounds region_bounds(const ClusterPolicy& policy, ClusterCoord region, f32 min_height,
                            f32 max_height) noexcept {
    const f64 edge = static_cast<f64>(policy.edge_metres) *
                     static_cast<f64>(1U << (region.level > 16 ? 16 : region.level));
    ClusterBounds bounds;
    bounds.min_x = static_cast<f64>(region.x) * edge;
    bounds.min_z = static_cast<f64>(region.z) * edge;
    bounds.max_x = bounds.min_x + edge;
    bounds.max_z = bounds.min_z + edge;
    bounds.min_y = min_height;
    bounds.max_y = max_height;
    return bounds;
}

ClusterCoord region_of(const ClusterPolicy& policy, f64 x, f64 z) noexcept {
    const f64 edge = static_cast<f64>(policy.edge_metres);
    ClusterCoord coord;
    coord.x = static_cast<i32>(std::floor(x / edge));
    coord.z = static_cast<i32>(std::floor(z / edge));
    coord.level = 0;
    return coord;
}

// --- Candidates
// -----------------------------------------------------------------------------------

namespace {

/// Sample indices a candidate draws, so a call site names them rather than counting.
enum : u64 {
    kDrawX = 0,
    kDrawZ,
    kDrawAccept,
    kDrawTieBreak,
    kDrawYaw,
    kDrawScale,
    kDrawVariation,
    kDrawAge,
    kDrawLean,
    kDrawsPerCandidate,
};

/// The stream one rule draws in, for one region. **Stable identifiers only**: the region's cluster
/// identity (which already folds the seed, the coordinate and the graph version) and the rule's
/// index. Nothing here is a counter and nothing depends on what has been generated before.
[[nodiscard]] determinism::RandomStream rule_stream(u64 seed, ClusterId region_identity,
                                                    u32 rule_index) noexcept {
    const determinism::StreamId base =
        determinism::substream(determinism::stream_id(kPlacementStream), region_identity.value);
    return {seed, determinism::substream(base, rule_index),
            determinism::StreamPurpose::Authoritative};
}

/// How many candidates a rule offers in one region. A function of the declared density and the
/// region's area alone — deliberately NOT of the field values, because a count that depended on
/// what the fields said would make a region's candidate set depend on a neighbour's field tile
/// residency the moment the sampler's answer varied.
[[nodiscard]] u32 candidate_count(const PlacementRule& rule, f64 edge_metres,
                                  u32 ceiling) noexcept {
    const f64 hectares = (edge_metres * edge_metres) / 10'000.0;
    const f64 wanted = static_cast<f64>(rule.density_per_hectare) * hectares;
    const f64 clamped = wanted < 0.0 ? 0.0 : wanted;
    const auto count = static_cast<u64>(std::llround(clamped));
    return static_cast<u32>(count > ceiling ? ceiling : count);
}

/// The density a rule's tests give one candidate, in [0, 1]. Zero where any test excluded it, and
/// the diagnostic records WHICH — `foliage`'s "the editor SHALL show which rule input excluded it".
[[nodiscard]] f32 rule_weight(const GenerationContext& context, const PlacementRule& rule,
                              u32 rule_index, const PlacementInputs& inputs,
                              const world::WorldVec3d& at, PlacementDiagnostic& why) noexcept {
    f32 weight = 1.0F;
    for (const RuleTest& test : rule.active_tests()) {
        const f32 value = test.input == RuleInput::Noise
                              ? PlacementSampler::placement_noise(context.seed, rule_index, at.x,
                                                                  at.z, rule.noise_metres)
                              : inputs.get(test.input);
        const f32 contribution = test.weight(value);
        if (contribution <= 0.0F) {
            ++why.excluded_by[static_cast<u32>(test.input)];
            return 0.0F;
        }
        weight *= contribution;
    }
    return weight;
}

/// Everything about ONE candidate after its position is drawn: the terrain under it, the rules over
/// it, the exclusion zones around it, and the ecosystem state it grew in. Returns false where the
/// candidate is not placed, having counted the reason.
///
/// Split out of `generate_candidates()` because the loop that OFFERS candidates and the test that
/// ACCEPTS one are two things a reader checks separately.
[[nodiscard]] bool evaluate_candidate(const GenerationContext& context, const PlacementRule& rule,
                                      const determinism::RandomStream& draws, u64 base,
                                      Candidate& candidate, PlacementDiagnostic& why) noexcept {
    const PlacementInputs inputs = context.sampler->at(candidate.position.x, candidate.position.z);
    if (!inputs.has_surface) {
        // A hole is NOT a surface — `terrain`'s own rule, and the reason `resolved` alone is not
        // the test. The two are counted apart so a diagnostic can tell unstreamed ground from a
        // cave mouth.
        if (inputs.surface.hole) {
            ++why.rejected_hole;
        } else {
            ++why.rejected_no_surface;
        }
        return false;
    }
    candidate.position.y = static_cast<f64>(inputs.surface.height);
    candidate.height = inputs.surface.height;
    candidate.slope_degrees = inputs.surface.slope_degrees;
    candidate.normal = inputs.surface.normal;
    candidate.vegetation = inputs.get(RuleInput::Vegetation);

    f32 weight = rule_weight(context, rule, candidate.rule_index, inputs, candidate.position, why);
    if (weight <= 0.0F) {
        ++why.rejected_weight;
        return false;
    }
    for (const ExclusionZone& zone : context.rules->exclusions) {
        weight *= zone.admittance(candidate.position.x, candidate.position.z, rule.species);
    }
    if (weight <= 0.0F) {
        ++why.rejected_exclusion_zone;
        return false;
    }
    // The ecosystem state, applied last so a region that has grown back materialises grown and a
    // region that burned materialises sparse, without either changing which candidates were
    // offered.
    if (context.apply_ecosystem_state) {
        weight *= math::clamp(candidate.vegetation, 0.0F, 1.0F);
    }
    if (draws.unit_float(generation_point(), candidate.sample_index, base + kDrawAccept) >=
        weight) {
        ++why.rejected_weight;
        return false;
    }
    candidate.weight = weight;
    return true;
}

}  // namespace

Status generate_candidates(const GenerationContext& context, ClusterCoord region,
                           Array<Candidate>& out, PlacementDiagnostic& why) noexcept {
    if (context.rules == nullptr || context.sampler == nullptr || context.library == nullptr) {
        return fail(ErrorCode::InvalidArgument, "generation context is incomplete");
    }
    const PlacementRuleSet& rules = *context.rules;
    const ClusterBounds bounds = region_bounds(context.policy, region, 0.0F, 0.0F);
    const f64 edge = bounds.span_x();
    const ClusterId identity = cluster_identity(context.seed, region, rules.graph_version);

    for (usize index = 0; index < rules.rules.size(); ++index) {
        const PlacementRule& rule = rules.rules[index];
        const determinism::RandomStream draws =
            rule_stream(context.seed, identity, static_cast<u32>(index));
        const u32 count = candidate_count(rule, edge, context.policy.max_instances);
        for (u32 sample = 0; sample < count; ++sample) {
            const u64 base = static_cast<u64>(sample) * kDrawsPerCandidate;
            Candidate candidate;
            candidate.region = region;
            candidate.rule_index = static_cast<u32>(index);
            candidate.sample_index = sample;
            candidate.species = rule.species;
            candidate.priority = rule.priority;
            candidate.spacing_metres = rule.spacing_metres;
            candidate.tie_break = draws.draw(generation_point(), sample, base + kDrawTieBreak);
            candidate.position.x =
                bounds.min_x +
                (draws.unit_double(generation_point(), sample, base + kDrawX) * edge);
            candidate.position.z =
                bounds.min_z +
                (draws.unit_double(generation_point(), sample, base + kDrawZ) * edge);

            ++why.candidates;
            if (!evaluate_candidate(context, rule, draws, base, candidate, why)) {
                continue;
            }
            if (Status pushed = out.push_back(candidate); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

// --- Order-free spacing
// ----------------------------------------------------------------------------

namespace {

/// The total order two candidates are compared by. Priority first, then the derived tie-break, then
/// the stable identifiers — so two candidates that drew the same tie-break (a 2^-64 event, and one
/// a test can construct on purpose) still have exactly one winner rather than eliminating each
/// other or both surviving.
[[nodiscard]] bool beats(const Candidate& a, const Candidate& b) noexcept {
    if (a.priority != b.priority) {
        return a.priority > b.priority;
    }
    if (a.tie_break != b.tie_break) {
        return a.tie_break > b.tie_break;
    }
    if (a.region.x != b.region.x) {
        return a.region.x > b.region.x;
    }
    if (a.region.z != b.region.z) {
        return a.region.z > b.region.z;
    }
    if (a.rule_index != b.rule_index) {
        return a.rule_index > b.rule_index;
    }
    return a.sample_index > b.sample_index;
}

[[nodiscard]] u64 cell_key(i64 cx, i64 cz) noexcept {
    return hash_combine(hash_integer(static_cast<u64>(cx), 0x6379'6265'7263'656cULL),
                        static_cast<u64>(cz));
}

constexpr u32 kNoNext = 0xFFFF'FFFFU;

/// The uniform grid a spacing resolution walks. A bucket-per-cell linked list: one array of `next`
/// indices and one map of heads, which is the whole of it.
///
/// Split out of `resolve_spacing()` because building the grid and walking it are two things a
/// reader checks separately, and together they were fifty of cognitive complexity for a function
/// whose ORDER-FREEDOM is the property this module is judged on — the one place in the file where
/// reading has to be easy.
class SpacingGrid {
public:
    SpacingGrid(Allocator& allocator, f64 cell) noexcept
        : next_(allocator), heads_(allocator), cell_(cell) {}

    [[nodiscard]] Status build(Span<const Candidate> candidates) noexcept {
        if (Status sized = next_.resize(candidates.size()); !sized) {
            return sized;
        }
        for (u32 index = 0; index < static_cast<u32>(candidates.size()); ++index) {
            const u64 key = key_of(candidates[index]);
            if (u32* head = heads_.find(key); head != nullptr) {
                next_[index] = *head;
                *head = index;
                continue;
            }
            next_[index] = kNoNext;
            if (Expected<u32*, Error> placed = heads_.insert(key, index); !placed) {
                return fail(placed.error().code, placed.error().message);
            }
        }
        return ok();
    }

    /// Whether any OTHER candidate within the pair's spacing beats `index`.
    ///
    /// THE ORDER-FREE COMPARISON. A rival is a CANDIDATE — it may itself be rejected by a third
    /// candidate, and that does not revive this one. A greedy pack that consulted an ACCEPTED set
    /// would be denser and would depend on traversal order, which the M10 spike measured at 2 of 12
    /// trials reproduced.
    [[nodiscard]] bool rejected(Span<const Candidate> candidates, u32 index) const noexcept {
        const Candidate& candidate = candidates[index];
        const i64 cx = axis_cell(candidate.position.x);
        const i64 cz = axis_cell(candidate.position.z);
        for (i64 dz = -1; dz <= 1; ++dz) {
            for (i64 dx = -1; dx <= 1; ++dx) {
                if (beaten_in_cell(candidates, index, cx + dx, cz + dz)) {
                    return true;
                }
            }
        }
        return false;
    }

private:
    [[nodiscard]] i64 axis_cell(f64 value) const noexcept {
        return static_cast<i64>(std::floor(value / cell_));
    }
    [[nodiscard]] u64 key_of(const Candidate& candidate) const noexcept {
        return cell_key(axis_cell(candidate.position.x), axis_cell(candidate.position.z));
    }

    [[nodiscard]] bool beaten_in_cell(Span<const Candidate> candidates, u32 index, i64 cx,
                                      i64 cz) const noexcept {
        const Candidate& candidate = candidates[index];
        const u32* head = heads_.find(cell_key(cx, cz));
        for (u32 other = head == nullptr ? kNoNext : *head; other != kNoNext;
             other = next_[other]) {
            if (other == index) {
                continue;
            }
            const Candidate& rival = candidates[other];
            // Symmetric: the distance a pair is judged at is the LARGER of their two declared
            // spacings, so the test does not depend on which of the two is being examined.
            const f64 limit =
                static_cast<f64>(math::max(rival.spacing_metres, candidate.spacing_metres));
            const f64 ddx = rival.position.x - candidate.position.x;
            const f64 ddz = rival.position.z - candidate.position.z;
            if ((ddx * ddx) + (ddz * ddz) < limit * limit && beats(rival, candidate)) {
                return true;
            }
        }
        return false;
    }

    Array<u32> next_;
    HashMap<u64, u32> heads_;
    f64 cell_ = 1.0;
};

/// Accept every candidate. What a rule set with no spacing at all resolves to.
[[nodiscard]] Status accept_all(Span<const Candidate> candidates, Array<u32>& accepted,
                                PlacementDiagnostic& why) noexcept {
    for (u32 index = 0; index < static_cast<u32>(candidates.size()); ++index) {
        if (Status pushed = accepted.push_back(index); !pushed) {
            return pushed;
        }
    }
    why.accepted += static_cast<u32>(candidates.size());
    return ok();
}

}  // namespace

Status resolve_spacing(Span<const Candidate> candidates, Array<u32>& accepted,
                       PlacementDiagnostic& why) noexcept {
    if (candidates.empty()) {
        return ok();
    }
    f32 largest = 0.0F;
    for (const Candidate& candidate : candidates) {
        largest = math::max(candidate.spacing_metres, largest);
    }
    if (!(largest > 0.0F)) {
        return accept_all(candidates, accepted, why);
    }

    // A uniform grid at the largest declared spacing, so a candidate's competitors are the nine
    // cells around it and the resolution is linear in the candidate count rather than quadratic.
    SpacingGrid grid(accepted.allocator(), static_cast<f64>(largest));
    if (Status built = grid.build(candidates); !built) {
        return built;
    }
    for (u32 index = 0; index < static_cast<u32>(candidates.size()); ++index) {
        if (grid.rejected(candidates, index)) {
            ++why.rejected_spacing;
            continue;
        }
        if (Status pushed = accepted.push_back(index); !pushed) {
            return pushed;
        }
        ++why.accepted;
    }
    return ok();
}

// --- Region generation
// -----------------------------------------------------------------------------

namespace {

/// How many regions of halo a rule set's reach needs. A candidate in this region can only conflict
/// with one within `reach` metres of it, so a ring of `ceil(reach / edge)` regions is EXACT rather
/// than conservative — and it is derived from the rules rather than declared, which is the
/// difference the spike's provenance note is about.
[[nodiscard]] i32 halo_rings(f32 reach_metres, f32 edge_metres) noexcept {
    if (!(reach_metres > 0.0F) || !(edge_metres > 0.0F)) {
        return 0;
    }
    return static_cast<i32>(
        std::ceil(static_cast<f64>(reach_metres) / static_cast<f64>(edge_metres)));
}

/// The scale a candidate's unit draw maps onto the species' declared range.
[[nodiscard]] f32 scale_unit_of(const SpeciesDeclaration& species, f32 draw) noexcept {
    const f32 span = species.scale_max - species.scale_min;
    if (!(span > 0.0F)) {
        return 0.0F;
    }
    // `FoliageInstance::scale` stores the position within the species' own range, so the draw IS
    // the stored value; keeping the metres out of the record is what lets a species be rescaled
    // without every stored instance changing meaning.
    return math::clamp(draw, 0.0F, 1.0F);
}

/// The tilt a candidate gets, in radians about the two horizontal axes.
void orientation_of(const PlacementRule& rule, const Candidate& candidate, f32 lean_draw,
                    f32& tilt_x, f32& tilt_z) noexcept {
    const f32 cap = rule.max_tilt_degrees * math::kDegToRad;
    switch (rule.orientation) {
        case OrientationMode::Upright:
            tilt_x = 0.0F;
            tilt_z = 0.0F;
            return;
        case OrientationMode::AlignToSlope: {
            // Toward the surface normal, by the declared fraction. The normal's horizontal
            // components ARE the tilt for a small angle, which is every angle inside the cap.
            tilt_x = math::clamp(-candidate.normal.z * rule.slope_alignment, -cap, cap);
            tilt_z = math::clamp(candidate.normal.x * rule.slope_alignment, -cap, cap);
            return;
        }
        case OrientationMode::RandomLean: {
            const f32 angle = lean_draw * math::kTwoPi;
            tilt_x = std::cos(angle) * cap;
            tilt_z = std::sin(angle) * cap;
            return;
        }
    }
}

}  // namespace

Expected<FoliagePopulation, Error> generate_region(Allocator& allocator,
                                                   const GenerationContext& context,
                                                   ClusterCoord region) noexcept {
    if (context.rules == nullptr || context.sampler == nullptr || context.library == nullptr) {
        return fail(ErrorCode::InvalidArgument, "generation context is incomplete");
    }
    const PlacementRuleSet& rules = *context.rules;
    const i32 rings = halo_rings(rules.reach_metres(), context.policy.edge_metres);

    PlacementDiagnostic why;
    Provenance provenance(allocator);
    Array<Candidate> candidates(allocator);

    // The halo's candidates are REGENERATED rather than read out of a neighbour's cluster — see
    // placement.h's condition 1. It costs the neighbour's candidates again and buys independence
    // from whether the neighbour exists.
    for (i32 dz = -rings; dz <= rings; ++dz) {
        for (i32 dx = -rings; dx <= rings; ++dx) {
            ClusterCoord neighbour = region;
            neighbour.x += dx;
            neighbour.z += dz;
            PlacementDiagnostic ignored;
            PlacementDiagnostic& sink = (dx == 0 && dz == 0) ? why : ignored;
            if (Status generated = generate_candidates(context, neighbour, candidates, sink);
                !generated) {
                return make_unexpected(generated.error());
            }
            if (Status pushed = provenance.regions.push_back(neighbour); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
    }

    Array<u32> accepted(allocator);
    if (Status resolved = resolve_spacing(candidates.span(), accepted, why); !resolved) {
        return make_unexpected(resolved.error());
    }

    // The cluster's vertical extent comes from what was actually placed, so the quantisation is as
    // fine as the terrain under this region allows rather than as fine as a world-wide range.
    const ClusterBounds flat = region_bounds(context.policy, region, 0.0F, 0.0F);
    f32 min_height = 0.0F;
    f32 max_height = 1.0F;
    bool first = true;
    for (u32 index : accepted) {
        const Candidate& candidate = candidates[index];
        if (!flat.contains(candidate.position.x, candidate.position.z)) {
            continue;
        }
        if (first || candidate.height < min_height) {
            min_height = candidate.height;
        }
        if (first || candidate.height > max_height) {
            max_height = candidate.height;
        }
        first = false;
    }
    if (max_height <= min_height) {
        max_height = min_height + 1.0F;
    }

    const ClusterId identity = cluster_identity(context.seed, region, rules.graph_version);
    const ClusterBounds bounds = region_bounds(context.policy, region, min_height, max_height);
    ClusterBuilder builder(allocator, context.policy, identity, region, bounds);

    for (u32 index : accepted) {
        const Candidate& candidate = candidates[index];
        if (!flat.contains(candidate.position.x, candidate.position.z)) {
            continue;  // A halo survivor. It belongs to its own region's cluster.
        }
        const PlacementRule& rule = rules.rules[candidate.rule_index];
        const SpeciesDeclaration* species = context.library->find(rule.species);
        if (species == nullptr) {
            continue;
        }
        const determinism::RandomStream draws = rule_stream(
            context.seed, cluster_identity(context.seed, candidate.region, rules.graph_version),
            candidate.rule_index);
        const u64 base = static_cast<u64>(candidate.sample_index) * kDrawsPerCandidate;
        const f32 yaw =
            draws.unit_float(generation_point(), candidate.sample_index, base + kDrawYaw) *
            math::kTwoPi;
        const f32 scale = scale_unit_of(
            *species,
            draws.unit_float(generation_point(), candidate.sample_index, base + kDrawScale));
        const u32 variation = draws.below(species->variations, generation_point(),
                                          candidate.sample_index, base + kDrawVariation);
        const f32 age_unit =
            draws.unit_float(generation_point(), candidate.sample_index, base + kDrawAge);
        const f32 age = rule.age_min + ((rule.age_max - rule.age_min) * age_unit);
        const f32 lean =
            draws.unit_float(generation_point(), candidate.sample_index, base + kDrawLean);
        f32 tilt_x = 0.0F;
        f32 tilt_z = 0.0F;
        orientation_of(rule, candidate, lean, tilt_x, tilt_z);
        if (Status added = builder.add(rule.species, candidate.position, yaw, scale,
                                       static_cast<u8>(variation),
                                       static_cast<u8>(math::clamp(age, 0.0F, 1.0F) * 255.0F),
                                       tilt_x, tilt_z, InstanceFlags{});
            !added) {
            return make_unexpected(added.error());
        }
    }

    provenance.min_x = flat.min_x - (static_cast<f64>(rings) * flat.span_x());
    provenance.max_x = flat.max_x + (static_cast<f64>(rings) * flat.span_x());
    provenance.min_z = flat.min_z - (static_cast<f64>(rings) * flat.span_z());
    provenance.max_z = flat.max_z + (static_cast<f64>(rings) * flat.span_z());
    const FieldBindings& bindings = context.sampler->bindings();
    for (const PlacementRule& rule : rules.rules) {
        for (const RuleTest& test : rule.active_tests()) {
            const environment::FieldId field = bindings.field_for(test.input);
            if (!field.is_valid()) {
                continue;
            }
            bool seen = false;
            for (environment::FieldId already : provenance.fields) {
                seen = seen || already == field;
            }
            if (!seen) {
                if (Status pushed = provenance.fields.push_back(field); !pushed) {
                    return make_unexpected(pushed.error());
                }
            }
        }
    }

    Expected<FoliageCluster, Error> built = builder.finish();
    if (!built) {
        return make_unexpected(built.error());
    }
    return FoliagePopulation(static_cast<FoliageCluster&&>(built.value()), builder.report(),
                             static_cast<Provenance&&>(provenance), why);
}

}  // namespace cy::foliage

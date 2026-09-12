// Derivation keys and the region cache. M10 tasks.md 4.3.
//
// `procedural-content-generation` — "Caching and distribution": "Each region's generation SHALL
// have a derivation key hashing: the compiled program, the generator version, region identity,
// input dataset hashes, field versions, parameters, and platform where relevant."
//
// ================================================================================================
// AND THE ONE AXIS OF THE SPIKE'S MATRIX WITH NO SURVIVOR ANYWHERE
// ================================================================================================
//
// design.md §1.2, third condition: an iterative solve run to a SWEEP BUDGET rather than to
// convergence reproduced a full regeneration in **0 of 12 trials, in all 12 of its
// configurations**, because a partial run sweeps a different set of regions a different number of
// times and a budgeted result is a function of exactly that. A budget is still a legitimate runtime
// lever; what is not legitimate is RECORDING the result as though it were reproducible.
// `RegionCache::store()` refuses it, and this suite is that refusal.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL: the `if (!cacheable)` branch in
// `RegionCache::store()` was deleted, and "a budgeted program's region is refused by the cache"
// went red on its `CY_REQUIRE_FALSE(stored.has_value())` — the budgeted region was admitted and
// would have been served later as though it were the converged answer. The branch was then
// restored. Separately, the `digest.u64_value(fields.digest())` line in `derivation_key()` was
// removed and "a field version change changes the key" went red. Both are reported in this
// milestone's `verified_failing`.

#include <cy/test/test.h>

#include <cy/pcg/cache.h>
#include <cy/pcg/graph.h>
#include <cy/pcg/program.h>

#include "fixtures.h"

using cy::pcg::AttributeDecl;
using cy::pcg::AttributeId;
using cy::pcg::AttributeType;
using cy::pcg::CachedRegion;
using cy::pcg::CacheRefusal;
using cy::pcg::DerivationKey;
using cy::pcg::FieldVersions;
using cy::pcg::IterationPolicy;
using cy::pcg::PointSet;
using cy::pcg::Program;
using cy::pcg::RegionCache;
using cy::pcg::RegionCoord;
namespace test = cy::pcg::test;

namespace {

constexpr RegionCoord kRegion{3, 4, 0};

[[nodiscard]] PointSet one_point() noexcept {
    PointSet points(test::allocator());
    const AttributeDecl columns[1] = {AttributeDecl{"w", AttributeId{1}, AttributeType::F32}};
    CY_REQUIRE(points.reserve(1, cy::Span<const AttributeDecl>(columns, 1)));
    CY_REQUIRE(points.add(1.0F, 2.0F, 3.0F, 0).has_value());
    points.set_identity(0, 0xdeadbeef);
    return points;
}

[[nodiscard]] FieldVersions versions(cy::u64 field, cy::u64 version) noexcept {
    FieldVersions out(test::allocator());
    CY_REQUIRE(out.observe(field, version));
    return out;
}

}  // namespace

CY_TEST_CASE("every contribution the specification names moves the derivation key") {
    const FieldVersions fields = versions(0x11, 7);
    const DerivationKey base =
        cy::pcg::derivation_key(0xaaaa, 1, 0xbbbb, kRegion, 0xcccc, fields, 0xdddd);
    CY_CHECK(base.is_valid());
    // The same inputs, twice: the same key. A key that varied would make every region a miss.
    CY_CHECK(cy::pcg::derivation_key(0xaaaa, 1, 0xbbbb, kRegion, 0xcccc, fields, 0xdddd) == base);

    // The compiled program.
    CY_CHECK_NE(cy::pcg::derivation_key(0xaaab, 1, 0xbbbb, kRegion, 0xcccc, fields, 0xdddd).value,
                base.value);
    // The generator version — which is what makes a rule change regenerate rather than serve the
    // old forest.
    CY_CHECK_NE(cy::pcg::derivation_key(0xaaaa, 2, 0xbbbb, kRegion, 0xcccc, fields, 0xdddd).value,
                base.value);
    // The seed.
    CY_CHECK_NE(cy::pcg::derivation_key(0xaaaa, 1, 0xbbbc, kRegion, 0xcccc, fields, 0xdddd).value,
                base.value);
    // Region identity, in each of its three components.
    CY_CHECK_NE(
        cy::pcg::derivation_key(0xaaaa, 1, 0xbbbb, RegionCoord{4, 4, 0}, 0xcccc, fields, 0xdddd)
            .value,
        base.value);
    CY_CHECK_NE(
        cy::pcg::derivation_key(0xaaaa, 1, 0xbbbb, RegionCoord{3, 5, 0}, 0xcccc, fields, 0xdddd)
            .value,
        base.value);
    CY_CHECK_NE(
        cy::pcg::derivation_key(0xaaaa, 1, 0xbbbb, RegionCoord{3, 4, 1}, 0xcccc, fields, 0xdddd)
            .value,
        base.value);
    // Input dataset hashes, and the parameters.
    CY_CHECK_NE(cy::pcg::derivation_key(0xaaaa, 1, 0xbbbb, kRegion, 0xcccd, fields, 0xdddd).value,
                base.value);
    CY_CHECK_NE(cy::pcg::derivation_key(0xaaaa, 1, 0xbbbb, kRegion, 0xcccc, fields, 0xddde).value,
                base.value);
}

CY_TEST_CASE("a field version change changes the key, and the field order does not") {
    // "Field reads SHALL participate in dependency tracking", and a cache key that omitted them
    // would serve a cached forest for a region whose moisture had changed — silently.
    const FieldVersions before = versions(0x11, 7);
    const FieldVersions after = versions(0x11, 8);
    CY_CHECK_NE(cy::pcg::derivation_key(1, 1, 1, kRegion, 0, before, 0).value,
                cy::pcg::derivation_key(1, 1, 1, kRegion, 0, after, 0).value);

    // The ORDER a region sampled its fields in is not part of the derivation: the entries are kept
    // sorted, so two runs that read the same fields agree whatever order they read them in.
    FieldVersions ascending(test::allocator());
    CY_REQUIRE(ascending.observe(0x11, 7));
    CY_REQUIRE(ascending.observe(0x22, 9));
    FieldVersions descending(test::allocator());
    CY_REQUIRE(descending.observe(0x22, 9));
    CY_REQUIRE(descending.observe(0x11, 7));
    CY_CHECK_EQ(ascending.digest(), descending.digest());
    CY_CHECK_EQ(ascending.entries.size(), 2u);
}

CY_TEST_CASE("the key carries a platform tag, because this host cannot claim the other one") {
    // design.md §1.5 refuses to claim that a region generated on one architecture reproduces on
    // another: this host has one. So the tag participates, and a cache populated on one target is a
    // MISS on another rather than a silently-wrong hit. When `pcg-regeneration-cross-platform` goes
    // green in continuous integration the honest change is to DELETE the contribution.
    CY_CHECK_NE(cy::pcg::platform_tag(), 0u);
    CY_CHECK_EQ(cy::pcg::platform_tag(), cy::pcg::platform_tag());
}

CY_TEST_CASE("a stored region is a hit, an unknown key is a miss, and both are counted") {
    RegionCache cache(test::allocator(), 0);
    const PointSet points = one_point();
    const cy::u64 digests[2] = {11, 22};
    const DerivationKey key{0x1234};

    CY_CHECK(cache.find(key) == nullptr);
    CY_CHECK_EQ(cache.misses(), 1u);

    cy::Expected<const CachedRegion*, cy::Error> stored =
        cache.store(key, kRegion, points, cy::Span<const cy::u64>(digests, 2), true);
    CY_REQUIRE(stored.has_value());
    CY_CHECK_EQ(cache.size(), 1u);

    const CachedRegion* found = cache.find(key);
    CY_REQUIRE(found != nullptr);
    CY_CHECK_EQ(cache.hits(), 1u);
    CY_CHECK_EQ(found->points.size(), 1u);
    // The hit carries the IDENTITIES, which is what an override binds to after a cache hit.
    CY_CHECK_EQ(found->points.identity(0), 0xdeadbeefu);
    CY_REQUIRE_EQ(found->stage_digests.size(), 2u);
    CY_CHECK_EQ(found->stage_digests[1], 22u);
}

CY_TEST_CASE("a budgeted program's region is refused by the cache, and the refusal says why") {
    // THE SPIKE'S THIRD CONDITION. `budget2` reproduced a full regeneration in 0 of 12 trials of
    // all 12 of its configurations — the only axis of the matrix with no survivor anywhere.
    RegionCache cache(test::allocator(), 0);
    const PointSet points = one_point();
    cy::Expected<const CachedRegion*, cy::Error> stored = cache.store(
        DerivationKey{0x1234}, kRegion, points, cy::Span<const cy::u64>(), /*cacheable=*/false);
    CY_REQUIRE_FALSE(stored.has_value());
    CY_CHECK(stored.error().code == cy::ErrorCode::PermissionDenied);
    CY_CHECK(cache.last_refusal() == CacheRefusal::NotCacheable);
    CY_CHECK_EQ(cache.refusals(), 1u);
    CY_CHECK_EQ(cache.size(), 0u);
    CY_CHECK(
        test::same_text(cy::pcg::cache_refusal_name(CacheRefusal::NotCacheable), "not-cacheable"));
}

CY_TEST_CASE(
    "a program with a budgeted iterative stage is not cacheable, and one with convergence is") {
    cy::Expected<test::ForestGraph, cy::Error> converged = test::build_forest(test::allocator());
    CY_REQUIRE(converged.has_value());
    cy::pcg::CompileReport report(test::allocator());
    cy::Array<cy::pcg::CompileDiagnostic> diagnostics(test::allocator());
    cy::Expected<Program, cy::Error> a =
        cy::pcg::compile(test::allocator(), converged->graph, report, diagnostics);
    CY_REQUIRE(a.has_value());
    CY_CHECK(a->cacheable());

    // A graph whose ONLY iterative node runs to a sweep budget. Built beside the forest rather
    // than by editing it, because what is being measured is the program's `cacheable` flag and a
    // half-rewritten graph would measure the rewrite.
    cy::pcg::Graph small(test::allocator());
    small.declare("budgeted", 1);
    small.declare_domains(cy::pcg::DomainMask::of(cy::pcg::ExecutionDomain::Cook));
    small.declare_region_size(64.0, 0);
    cy::Expected<AttributeId, cy::Error> height =
        small.attributes().intern("h", AttributeType::F32);
    cy::Expected<AttributeId, cy::Error> flow =
        small.attributes().intern("pcg.density", AttributeType::F32);
    CY_REQUIRE(height.has_value());
    CY_REQUIRE(flow.has_value());
    cy::pcg::GraphNode noise;
    noise.name = "budgeted.noise";
    noise.kind = cy::pcg::NodeKind::Noise;
    noise.output = *height;
    noise.params.frequency = 0.01F;
    noise.params.amplitude = 10.0F;
    noise.params.count = 1;
    cy::Expected<cy::pcg::NodeId, cy::Error> noise_id = small.add(noise);
    CY_REQUIRE(noise_id.has_value());
    cy::pcg::GraphNode propagate;
    propagate.name = "budgeted.flow";
    propagate.kind = cy::pcg::NodeKind::Propagate;
    propagate.inputs[0] = *noise_id;
    propagate.input_count = 1;
    propagate.output = *flow;
    propagate.neighbour_access = cy::pcg::NeighbourAccess::Raster;
    propagate.reach_regions = 1;
    propagate.iteration = IterationPolicy::Budget;
    propagate.iteration_bound = 2;
    cy::Expected<cy::pcg::NodeId, cy::Error> flow_id = small.add(propagate);
    CY_REQUIRE(flow_id.has_value());
    cy::pcg::GraphNode scatter;
    scatter.name = "budgeted.scatter";
    scatter.kind = cy::pcg::NodeKind::Scatter;
    scatter.inputs[0] = *flow_id;
    scatter.input_count = 1;
    scatter.params.count = 4;
    cy::Expected<cy::pcg::NodeId, cy::Error> scatter_id = small.add(scatter);
    CY_REQUIRE(scatter_id.has_value());
    cy::pcg::GraphNode output;
    output.name = "budgeted.output";
    output.kind = cy::pcg::NodeKind::Output;
    output.inputs[0] = *scatter_id;
    output.input_count = 1;
    CY_REQUIRE(small.add(output).has_value());

    cy::pcg::CompileReport second_report(test::allocator());
    cy::Array<cy::pcg::CompileDiagnostic> second_diagnostics(test::allocator());
    cy::Expected<Program, cy::Error> b =
        cy::pcg::compile(test::allocator(), small, second_report, second_diagnostics);
    CY_REQUIRE(b.has_value());
    // A BUDGET IS NOT REFUSED — it is a legitimate runtime lever — and its result is not recorded
    // as though it were reproducible.
    CY_CHECK_FALSE(b->cacheable());
}

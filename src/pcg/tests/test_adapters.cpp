// Output adapters: the representation a generated result becomes. M10 tasks.md 4.3.
//
// `procedural-content-generation` — "Output adapters": "Generators SHALL produce results through
// output adapters, and the adapter SHALL determine the representation ... **A procedural result
// SHALL NOT be an entity by default.** A generator producing ten million trees SHALL produce a
// FOLIAGE POPULATION, not ten million spawn operations", and its scenario: "WHEN a forest generator
// produces ten million instances THEN they SHALL become foliage clusters, AND NO ENTITIES SHALL BE
// CREATED."
//
// THE STRONGEST FORM OF THAT LAST SENTENCE IS A LINK GRAPH: neither `cy::pcg` nor
// `cy::pcg-adapters` depends on `cy::ecs`, so nothing in this path can spell `Entity`. What this
// suite adds is the behaviour: a whole region becomes ONE cluster in ONE call, and emitting to
// `Entities` on a registry nobody bound one to fails with a diagnostic rather than doing something.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL: `OutputRegistry::emit()`'s null-adapter branch was
// replaced with `return ok();` — the shape a registry has when an unbound target is treated as a
// no-op — and "a generated result is not an entity by default" went red on its
// `CY_REQUIRE_FALSE(emitted)`: a generator asking for ten million entities was silently answered
// with success. The branch was then restored.
//
// And separately, `FieldOutputAdapter`'s falloff was flattened to a constant — the shape a field
// written one lattice point at a time has — and "a generator writes a field through the producer
// token its caller holds" went red on the monotonicity of the splat. Restored. Both runs are
// reported in this milestone's `verified_failing`.

#include <cy/test/test.h>

#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/pcg/adapters.h>
#include <cy/pcg/execute.h>
#include <cy/pcg/foliage_adapter.h>
#include <cy/terrain/stack.h>

#include "fixtures.h"

using cy::pcg::EmitContext;
using cy::pcg::ExecutionDomain;
using cy::pcg::FlatSpatialQuery;
using cy::pcg::FoliageOutputAdapter;
using cy::pcg::GenerationContext;
using cy::pcg::GenerationWorld;
using cy::pcg::Generator;
using cy::pcg::OutputRegistry;
using cy::pcg::OutputTarget;
using cy::pcg::PointSet;
using cy::pcg::RecordingAdapter;
using cy::pcg::RegionCoord;
using cy::pcg::TerrainSpatialQuery;
using cy::pcg::TerrainStampAdapter;
namespace test = cy::pcg::test;

namespace {

constexpr cy::i32 kEdge = 4;
constexpr cy::u64 kSeed = 0x515e'0dd1'2345ULL;

/// An analytic surface. `procedural-content-generation` — "Build-time generation SHALL NOT require
/// a running physics world in order to perform geometric queries": this is a canonical geometric
/// representation and there is no physics world anywhere near it.
class RampSource final : public cy::terrain::SurfaceSource {
public:
    [[nodiscard]] cy::terrain::Representation representation() const noexcept override {
        return cy::terrain::Representation::Heightfield;
    }

    [[nodiscard]] cy::u32 column(cy::f64 x, cy::f64 z,
                                 cy::Span<cy::terrain::SurfaceSample> out) const noexcept override {
        if (out.empty()) {
            return 0;
        }
        cy::terrain::SurfaceSample sample;
        sample.height = static_cast<cy::f32>(x * 0.1 + z * 0.05);
        sample.slope_degrees = 30.0F;
        sample.resolved = true;
        // A HOLE past x = 500, so "a hole is not a surface" has something to be true of.
        sample.hole = x > 500.0;
        sample.resolved = !sample.hole;
        out[0] = sample;
        return 1;
    }
};

}  // namespace

CY_TEST_CASE("a generated result is not an entity by default") {
    // Nothing is bound to `Entities` unless an application binds it, so a generator that asks for
    // one FAILS rather than spawning ten million of them.
    OutputRegistry registry(test::allocator());
    PointSet points(test::allocator());
    EmitContext context;
    cy::Status emitted = registry.emit(OutputTarget::Entities, context, points);
    CY_REQUIRE_FALSE(emitted);
    CY_CHECK(emitted.error().code == cy::ErrorCode::NotFound);
    CY_CHECK_EQ(registry.size(), 0u);
    CY_CHECK(registry.find(OutputTarget::Entities) == nullptr);
}

CY_TEST_CASE("a second adapter for one target is refused") {
    // Two adapters writing one representation is the same defect `environment-fields` refuses for a
    // second producer, one level up: the representation would depend on which ran last.
    OutputRegistry registry(test::allocator());
    RecordingAdapter first(test::allocator(), OutputTarget::Foliage);
    RecordingAdapter second(test::allocator(), OutputTarget::Foliage);
    CY_REQUIRE(registry.register_adapter(first));
    CY_CHECK_EQ(registry.size(), 1u);
    cy::Status again = registry.register_adapter(second);
    CY_REQUIRE_FALSE(again);
    CY_CHECK(again.error().code == cy::ErrorCode::AlreadyExists);
    CY_CHECK(registry.find(OutputTarget::Foliage) == &first);
}

CY_TEST_CASE("a whole region becomes one foliage cluster, in one call") {
    const FlatSpatialQuery surface;
    cy::foliage::ClusterStore store(test::allocator());
    cy::foliage::ClusterPolicy policy;
    policy.edge_metres = static_cast<cy::f32>(test::kRegionMetres);
    policy.target_instances = 256;
    policy.max_instances = 4096;
    FoliageOutputAdapter adapter(test::allocator(), store, policy,
                                 cy::foliage::species_id("test.pine"), kSeed);

    OutputRegistry registry(test::allocator());
    CY_REQUIRE(registry.register_adapter(adapter));

    GenerationWorld world(test::allocator(), test::extent_of(kEdge));
    cy::Expected<Generator, cy::Error> generator = test::make_generator(world);
    CY_REQUIRE(generator.has_value());
    GenerationContext context = test::context_of(kSeed, surface);
    context.outputs = &registry;
    context.output_target = OutputTarget::Foliage;
    CY_REQUIRE(generator->generate_all(ExecutionDomain::Cook, context).has_value());

    // ONE cluster per region that produced anything, never one object per instance.
    CY_CHECK_GT(adapter.clusters(), 0u);
    CY_CHECK_LE(adapter.clusters(), static_cast<cy::u64>(kEdge) * kEdge);
    CY_CHECK_GT(adapter.instances(), adapter.clusters());
    CY_CHECK_EQ(store.size(), adapter.clusters());
    // And the population is compact: `foliage`'s own "tens of bytes rather than hundreds".
    CY_CHECK_LT(adapter.last_report().bytes_per_instance(), 64.0F);

    // A demotion drops the cluster and keeps the region's macro summary — the round trip the
    // specification asks for, rather than a drop that loses what happened.
    const RegionCoord region{0, 0, 0};
    const cy::pcg::RegionState* state = world.find(region);
    CY_REQUIRE(state != nullptr);
    const cy::u32 before = static_cast<cy::u32>(state->accepted.size());
    CY_REQUIRE(world.demote(region));
    const cy::pcg::RegionState* after = world.find(region);
    CY_REQUIRE(after != nullptr);
    CY_CHECK_EQ(after->accepted.size(), 0u);
    CY_CHECK_EQ(after->macro_instances, before);
    CY_CHECK_GT(after->macro_density, 0.0F);
}

CY_TEST_CASE("a generated point set becomes terrain modifiers, appended not edited") {
    cy::terrain::TileLayout layout;
    layout.terrain = 1;
    layout.tile_metres = 256.0F;
    cy::terrain::ModifierStack stack(test::allocator(), layout, kSeed);
    TerrainStampAdapter adapter(stack, 6.0F, -1.5F, 2);

    PointSet points(test::allocator());
    const cy::pcg::AttributeDecl columns[1] = {
        cy::pcg::AttributeDecl{"w", cy::pcg::AttributeId{1}, cy::pcg::AttributeType::F32}};
    CY_REQUIRE(points.reserve(3, cy::Span<const cy::pcg::AttributeDecl>(columns, 1)));
    for (cy::u32 index = 0; index < 3; ++index) {
        CY_REQUIRE(points.add(static_cast<cy::f32>(index) * 10.0F, 0.0F, 5.0F, index).has_value());
    }

    EmitContext context;
    context.region = RegionCoord{1, 0, 0};
    context.origin_x = 64.0;
    context.origin_z = 0.0;
    context.region_metres = test::kRegionMetres;
    context.generator_name = "forest";
    CY_REQUIRE(adapter.emit(context, points));
    CY_CHECK_EQ(adapter.stamps(), 3u);
    CY_CHECK_EQ(stack.size(), 3u);

    // The stamp's DECLARED RADIUS is zero because its bounds already cover everything it touches: a
    // second radius would widen terrain's dirty set past what the modifier reads, and `terrain`'s
    // `dirty_tiles()` is bounds-plus-radius exactly.
    CY_REQUIRE_EQ(stack.modifiers().size(), 3u);
    const cy::terrain::Modifier& first = stack.modifiers()[0];
    CY_CHECK_NEAR(first.radius, 0.0F, 1e-6F);
    // Absolute world metres, not region-local: the adapter added the region's origin.
    CY_CHECK_NEAR(static_cast<cy::f32>(first.bounds.min_x), 58.0F, 1e-3F);
    CY_CHECK_NEAR(static_cast<cy::f32>(first.bounds.max_x), 70.0F, 1e-3F);
}

CY_TEST_CASE("a generator writes a field through the producer token its caller holds") {
    // `procedural-content-generation` — "Field integration": "Generators SHALL be able to WRITE
    // fields where they are the declared producer of one: a road generator writing road distance, a
    // fire simulation writing burn state, a terraforming system writing soil health."
    //
    // THE TOKEN IS THE CALLER'S. `environment-fields` refuses a second producer at REGISTRATION,
    // and an adapter that claimed on the caller's behalf would move that refusal somewhere the
    // caller cannot see it. A generator writing `road distance` IS the road system's claim, held by
    // the road system — so the claim happens here, in the test, exactly as it would in an
    // application.
    cy::environment::FieldRegistry registry(test::allocator());
    cy::environment::FieldDeclaration declaration;
    declaration.name = "test.road-distance";
    declaration.unit = "fraction";
    declaration.semantics = "proximity to a generated road, 1 on it and 0 beyond its reach";
    declaration.type = cy::environment::FieldType::Scalar;
    declaration.encoding = cy::environment::FieldEncoding::UNorm8;
    declaration.interpolation = cy::environment::FieldInterpolation::Linear;
    declaration.cadence = cy::environment::FieldCadence::Static;
    declaration.production = cy::environment::FieldProduction::Cpu;
    declaration.range_min = 0.0F;
    declaration.range_max = 1.0F;
    declaration.default_value = cy::environment::FieldValue::scalar(0.0F);
    declaration.levels[0] = cy::environment::FieldLevel{4.0F, false};
    declaration.classification = cy::determinism::SimulationClass::Presentation;
    declaration.gameplay_level = cy::environment::FieldResidency::Local;
    CY_REQUIRE(registry.declare(declaration));

    cy::Expected<cy::environment::ProducerToken, cy::Error> token =
        registry.claim(declaration.id(), "test.roads", cy::environment::ProducerKind::Generator);
    CY_REQUIRE(token.has_value());

    cy::world::PartitionConfig partition;
    partition.partition = 1;
    partition.base_cell_size = 128.0F;
    partition.levels = 3;
    partition.level_ratio = 4;
    cy::environment::FieldStore store(test::allocator(), registry, partition);

    cy::pcg::FieldOutputAdapter adapter(store, *token, /*radius_metres=*/12.0F, /*value=*/1.0F);
    OutputRegistry registry_of_adapters(test::allocator());
    CY_REQUIRE(registry_of_adapters.register_adapter(adapter));

    PointSet points(test::allocator());
    const cy::pcg::AttributeDecl columns[1] = {
        cy::pcg::AttributeDecl{"w", cy::pcg::AttributeId{1}, cy::pcg::AttributeType::F32}};
    CY_REQUIRE(points.reserve(1, cy::Span<const cy::pcg::AttributeDecl>(columns, 1)));
    CY_REQUIRE(points.add(32.0F, 0.0F, 32.0F, 0).has_value());

    EmitContext context;
    context.region = RegionCoord{0, 0, 0};
    context.origin_x = 0.0;
    context.origin_z = 0.0;
    context.region_metres = test::kRegionMetres;
    CY_REQUIRE(registry_of_adapters.emit(OutputTarget::Fields, context, points));
    CY_CHECK_GT(adapter.written(), 0u);

    // A SPLAT, not a spike: "distance to road" written one lattice point at a time would be a field
    // of spikes. The property asserted is the FALLOFF — strictly decreasing away from the point —
    // rather than an exact value at the centre, because what a sample returns there is the
    // substrate's lattice and interpolation convention rather than this adapter's arithmetic.
    const cy::environment::FieldSample on_it =
        store.sample(declaration.id(), cy::world::WorldVec3d{32.0, 0.0, 32.0});
    const cy::environment::FieldSample near =
        store.sample(declaration.id(), cy::world::WorldVec3d{40.0, 0.0, 32.0});
    const cy::environment::FieldSample beyond =
        store.sample(declaration.id(), cy::world::WorldVec3d{200.0, 0.0, 200.0});
    CY_CHECK(on_it.resolved);
    CY_CHECK_GT(on_it.value.x(), 0.5F);
    CY_CHECK_GT(on_it.value.x(), near.value.x());
    CY_CHECK_GT(near.value.x(), beyond.value.x());
    CY_CHECK_NEAR(beyond.value.x(), 0.0F, 1e-3F);
    // Nothing streamed there, so the sample is the DECLARED DEFAULT rather than a fault — which is
    // the substrate's own guarantee and the reason a generator may read any field at any position.
    CY_CHECK_FALSE(beyond.resolved);
}

CY_TEST_CASE("a terrain-backed spatial query answers in batches, and a hole is not a surface") {
    // "Queries SHALL be batchable, since generation issues them in bulk", and "Build-time
    // generation SHALL NOT require a running physics world."
    cy::terrain::TerrainQuery query(test::allocator());
    const RampSource ramp;
    CY_REQUIRE(query.add_source(ramp));
    const TerrainSpatialQuery bridge(query);

    const cy::f64 xs[3] = {0.0, 100.0, 600.0};
    const cy::f64 zs[3] = {0.0, 100.0, 0.0};
    cy::f32 heights[3] = {};
    cy::f32 slopes[3] = {};
    cy::u8 surfaces[3] = {};
    bridge.height_batch(cy::Span<const cy::f64>(xs, 3), cy::Span<const cy::f64>(zs, 3),
                        cy::Span<cy::f32>(heights, 3));
    bridge.slope_batch(cy::Span<const cy::f64>(xs, 3), cy::Span<const cy::f64>(zs, 3),
                       cy::Span<cy::f32>(slopes, 3));
    bridge.surface_batch(cy::Span<const cy::f64>(xs, 3), cy::Span<const cy::f64>(zs, 3),
                         cy::Span<cy::u8>(surfaces, 3));

    CY_CHECK_NEAR(heights[1], 15.0F, 1e-3F);
    // Radians, converted at the seam, because `SpatialQuery` declares radians and `terrain` answers
    // in degrees — a mismatch converted once is a mismatch nobody has to remember.
    CY_CHECK_NEAR(slopes[1], 30.0F * 3.14159265F / 180.0F, 1e-4F);
    CY_CHECK_EQ(surfaces[0], 1u);
    CY_CHECK_EQ(surfaces[1], 1u);
    // A HOLE IS NOT A SURFACE: this is what stops a tree being planted in a cave mouth, and it is
    // terrain's own distinction rather than a threshold invented in this module.
    CY_CHECK_EQ(surfaces[2], 0u);
}

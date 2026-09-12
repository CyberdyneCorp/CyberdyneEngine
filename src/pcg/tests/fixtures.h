#pragma once
// The generator CyberPCG's suites are written against.
//
// ONE GRAPH, AND IT CARRIES THE THREE KINDS OF EDGE THE SPIKE MEASURED — because an answer about a
// partial regeneration has to be attributable to an EDGE rather than to a graph. design.md §1:
//
//   `forest.noise`     no edge at all: a pure function of world coordinates.
//   `forest.stamp`     the authored edit, and the seed of every dirty set.
//   `forest.smooth`    a BOUNDED GATHER. Three relaxation passes need three cells of halo, so its
//                      region's own cells are exact — the control node. It declares a reach of TWO
//                      regions and reads ONE, which is what gives the read ledger something to
//                      narrow.
//   `forest.flow`      the TRANSITIVE edge and the whole of M10's named risk: a region reads only
//                      its four neighbours' boundary outflow — a declared radius of one — but water
//                      crosses the world, so the reachable set is unbounded and data-dependent.
//   `forest.density`   an elementwise compute over what the flow deposited.
//   `forest.scatter`   candidate generation, and the only node that mints identity.
//   `forest.filter`    a threshold test on the density each candidate landed in.
//   `forest.spacing`   CROSS-REGION CONFLICT RESOLUTION, order-free: a candidate loses to a
//                      strictly higher priority within four metres, wherever that candidate is.
//   `forest.output`    the terminal.
//
// The numbers are chosen so the graph CONTENDS and so its water CROSSES REGIONS. A scatter of 64
// candidates into a 64 m region with 4 m spacing fights at every boundary, and
// `test_invalidation.cpp` counts the cross-region rejections rather than assuming them — this
// project has shipped a determinism test that passed on the very defect it was written for, its
// scene having never contended.

#include <cy/core/memory/system_allocator.h>
#include <cy/pcg/dataset.h>
#include <cy/pcg/execute.h>
#include <cy/pcg/graph.h>
#include <cy/pcg/invalidation.h>
#include <cy/pcg/program.h>

#include <utility>

namespace cy::pcg::test {

[[nodiscard]] inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// Two names are the same name. Every diagnostic in this module carries a `const char*` the caller
/// owns, so a suite asserting that a refusal NAMED a node has to compare the text rather than the
/// pointer — a refusal that named the wrong node would otherwise read as a pass.
[[nodiscard]] inline bool same_text(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

/// The authored names, as literals, because identity derives from them and a test that wants to
/// stamp `forest.stamp` has to name it exactly.
inline constexpr const char* kNoiseNode = "forest.noise";
inline constexpr const char* kStampNode = "forest.stamp";
inline constexpr const char* kSmoothNode = "forest.smooth";
inline constexpr const char* kFlowNode = "forest.flow";
inline constexpr const char* kDensityNode = "forest.density";
inline constexpr const char* kScatterNode = "forest.scatter";
inline constexpr const char* kFilterNode = "forest.filter";
inline constexpr const char* kSpacingNode = "forest.spacing";
inline constexpr const char* kOutputNode = "forest.output";

/// The region edge, in metres. With `kRegionCells` at sixteen this is a four-metre raster cell.
inline constexpr f64 kRegionMetres = 64.0;

/// The candidate spacing, in metres. Small enough that neighbouring regions fight and large enough
/// that a meaningful fraction is rejected.
inline constexpr f32 kSpacingMetres = 8.0F;

struct ForestAttributes {
    AttributeId height;
    AttributeId stamped;
    AttributeId eroded;
    AttributeId flow;
    AttributeId density;
};

struct ForestGraph {
    explicit ForestGraph(Allocator& alloc) noexcept : graph(alloc) {}

    ForestGraph(const ForestGraph&) = delete;
    ForestGraph& operator=(const ForestGraph&) = delete;
    ForestGraph(ForestGraph&&) noexcept = default;
    ForestGraph& operator=(ForestGraph&&) noexcept = default;

    Graph graph;
    ForestAttributes attributes;
    NodeId noise;
    NodeId stamp;
    NodeId smooth;
    NodeId flow;
    NodeId density;
    NodeId scatter;
    NodeId filter;
    NodeId spacing;
    NodeId output;
};

/// Build the graph above. `version` is the generator version, which participates in every
/// derivation key and in the cluster identities the foliage adapter mints.
[[nodiscard]] inline Expected<ForestGraph, Error> build_forest(Allocator& alloc,
                                                               u32 version = 1) noexcept {
    ForestGraph forest(alloc);
    forest.graph.declare("forest", version);
    DomainMask domains;
    domains.add(ExecutionDomain::Editor);
    domains.add(ExecutionDomain::Cook);
    forest.graph.declare_domains(domains);
    forest.graph.declare_region_size(kRegionMetres, 0);
    forest.graph.declare_determinism(DeterminismLevel::Gameplay);

    AttributeTable& attributes = forest.graph.attributes();
    Expected<AttributeId, Error> height = attributes.intern("forest.height", AttributeType::F32);
    if (!height) {
        return make_unexpected(height.error());
    }
    Expected<AttributeId, Error> stamped = attributes.intern("forest.stamped", AttributeType::F32);
    if (!stamped) {
        return make_unexpected(stamped.error());
    }
    Expected<AttributeId, Error> eroded = attributes.intern("forest.eroded", AttributeType::F32);
    if (!eroded) {
        return make_unexpected(eroded.error());
    }
    Expected<AttributeId, Error> flow = attributes.intern("forest.flow", AttributeType::F32);
    if (!flow) {
        return make_unexpected(flow.error());
    }
    // `pcg.density` by name, so the raster channel the compute writes and the candidate column the
    // scatter records ARE the same compiled identifier — which is what lets `forest.filter` test
    // the density a candidate landed in without the graph having to know the evaluator's own
    // columns.
    Expected<AttributeId, Error> density = attributes.intern("pcg.density", AttributeType::F32);
    if (!density) {
        return make_unexpected(density.error());
    }
    forest.attributes = ForestAttributes{*height, *stamped, *eroded, *flow, *density};

    GraphNode noise;
    noise.name = kNoiseNode;
    noise.kind = NodeKind::Noise;
    noise.output = *height;
    noise.params.frequency = 1.0F / 96.0F;
    noise.params.amplitude = 48.0F;
    noise.params.count = 3;
    Expected<NodeId, Error> added = forest.graph.add(noise);
    if (!added) {
        return make_unexpected(added.error());
    }
    forest.noise = *added;

    GraphNode stamp;
    stamp.name = kStampNode;
    stamp.kind = NodeKind::Stamp;
    stamp.inputs[0] = forest.noise;
    stamp.input_count = 1;
    stamp.output = *stamped;
    added = forest.graph.add(stamp);
    if (!added) {
        return make_unexpected(added.error());
    }
    forest.stamp = *added;

    GraphNode smooth;
    smooth.name = kSmoothNode;
    smooth.kind = NodeKind::Smooth;
    smooth.inputs[0] = forest.stamp;
    smooth.input_count = 1;
    smooth.output = *eroded;
    // TWO regions declared, THREE cells actually read. The gap is deliberate: it is what the read
    // ledger narrows, and design.md §1.3 says the answer to a long-range gather is a record of what
    // was read rather than a different declaration.
    smooth.neighbour_access = NeighbourAccess::Raster;
    smooth.reach_regions = 2;
    smooth.iteration_bound = 3;
    smooth.params.amplitude = 0.25F;
    added = forest.graph.add(smooth);
    if (!added) {
        return make_unexpected(added.error());
    }
    forest.smooth = *added;

    GraphNode propagate;
    propagate.name = kFlowNode;
    propagate.kind = NodeKind::Propagate;
    propagate.inputs[0] = forest.smooth;
    propagate.input_count = 1;
    propagate.output = *flow;
    propagate.neighbour_access = NeighbourAccess::Raster;
    propagate.reach_regions = 1;
    // CONVERGENCE, not a budget. The bound is a refusal point: reaching it means the solve did not
    // converge, which is a defect in the generator rather than a result.
    propagate.iteration = IterationPolicy::Convergence;
    propagate.iteration_bound = 256;
    added = forest.graph.add(propagate);
    if (!added) {
        return make_unexpected(added.error());
    }
    forest.flow = *added;

    GraphNode compute;
    compute.name = kDensityNode;
    compute.kind = NodeKind::Compute;
    compute.inputs[0] = forest.flow;
    compute.input_count = 1;
    compute.output = *density;
    compute.params.value = 0.02F;
    compute.params.second = 0.30F;
    added = forest.graph.add(compute);
    if (!added) {
        return make_unexpected(added.error());
    }
    forest.density = *added;

    GraphNode scatter;
    scatter.name = kScatterNode;
    scatter.kind = NodeKind::Scatter;
    scatter.inputs[0] = forest.density;
    scatter.input_count = 1;
    scatter.params.count = 160;
    added = forest.graph.add(scatter);
    if (!added) {
        return make_unexpected(added.error());
    }
    forest.scatter = *added;

    GraphNode filter;
    filter.name = kFilterNode;
    filter.kind = NodeKind::Filter;
    filter.inputs[0] = forest.scatter;
    filter.input_count = 1;
    filter.reads = *density;
    filter.params.value = 0.40F;
    added = forest.graph.add(filter);
    if (!added) {
        return make_unexpected(added.error());
    }
    forest.filter = *added;

    GraphNode spacing;
    spacing.name = kSpacingNode;
    spacing.kind = NodeKind::Spacing;
    spacing.inputs[0] = forest.filter;
    spacing.input_count = 1;
    // CANDIDATES, never accepted output. `program.h` refuses the other one by name.
    spacing.neighbour_access = NeighbourAccess::Candidates;
    spacing.reach_regions = 1;
    spacing.params.frequency = kSpacingMetres;
    added = forest.graph.add(spacing);
    if (!added) {
        return make_unexpected(added.error());
    }
    forest.spacing = *added;

    GraphNode output;
    output.name = kOutputNode;
    output.kind = NodeKind::Output;
    output.inputs[0] = forest.spacing;
    output.input_count = 1;
    added = forest.graph.add(output);
    if (!added) {
        return make_unexpected(added.error());
    }
    forest.output = *added;
    return forest;
}

/// Compile the forest graph and bind it to `world`. The four lines every suite would otherwise
/// repeat, and the one place a compile diagnostic is turned into an error rather than ignored.
[[nodiscard]] inline Expected<Generator, Error> make_generator(GenerationWorld& world,
                                                               u32 version = 1) noexcept {
    Expected<ForestGraph, Error> forest = build_forest(allocator(), version);
    if (!forest) {
        return make_unexpected(forest.error());
    }
    CompileReport report(allocator());
    Array<CompileDiagnostic> diagnostics(allocator());
    Expected<Program, Error> program = compile(allocator(), forest->graph, report, diagnostics);
    if (!program) {
        return make_unexpected(program.error());
    }
    return Generator::create(allocator(), std::move(*program), world);
}

/// A context with a flat surface bound, because leaving `geometry` null would have the evaluator
/// substitute one and the suite would then be measuring a default rather than a decision.
[[nodiscard]] inline GenerationContext context_of(u64 seed, const FlatSpatialQuery& surface,
                                                  Span<const AuthoredStamp> stamps = {}) noexcept {
    GenerationContext context;
    context.seed = seed;
    context.geometry = &surface;
    context.stamps = stamps;
    return context;
}

/// The authored edit every invalidation case starts from: a hill in the middle of one region,
/// large enough to move the water that crosses the regions downhill of it.
[[nodiscard]] inline AuthoredStamp hill(i32 region_x, i32 region_z, f64 radius = 40.0,
                                        f32 amount = 30.0F) noexcept {
    AuthoredStamp stamp;
    stamp.x = static_cast<f64>(region_x) * kRegionMetres + kRegionMetres * 0.5;
    stamp.z = static_cast<f64>(region_z) * kRegionMetres + kRegionMetres * 0.5;
    stamp.radius = radius;
    stamp.amount = amount;
    stamp.node = node_identity(kStampNode);
    return stamp;
}

/// Regions evaluated across every stage of the last run. The number that separates "only the dirty
/// set regenerated" from "a partial regeneration is a full one with extra bookkeeping".
[[nodiscard]] inline u64 regions_evaluated(const GenerationProfile& profile) noexcept {
    u64 total = 0;
    for (const StageProfile& stage : profile.stages) {
        total += stage.regions_evaluated;
    }
    return total;
}

/// A square extent of `edge` regions at level 0, with its minimum corner at the origin.
[[nodiscard]] inline RegionExtent extent_of(i32 edge) noexcept {
    RegionExtent extent;
    extent.min_x = 0;
    extent.min_z = 0;
    extent.max_x = edge - 1;
    extent.max_z = edge - 1;
    extent.level = 0;
    return extent;
}

/// A digest of the WHOLE generated world: EVERY STAGE'S output in every region, in a canonical
/// region order, plus the accepted points with their positions AND their identities.
///
/// EVERY STAGE, not only the output, and the reason is a mutation that survived the narrower
/// version: dropping the declared reach from the invalidation left two regions' relaxation stale
/// three cells deep, and the difference was real but too small to flip any candidate's acceptance —
/// so a comparison of the accepted sets alone called a broken invalidation sound. "Bit for bit"
/// means the intermediate rasters too.
///
/// The IDENTITY half matters separately and fails differently: the spike measured a configuration
/// whose OUTPUT was bit-identical and whose identities had all moved — `counter`, in all twelve
/// trials of all eight of its configurations — so a comparison that digested positions alone would
/// have called that one sound as well.
[[nodiscard]] inline u64 world_digest(const GenerationWorld& world) noexcept {
    Digest digest;
    const RegionExtent& extent = world.extent();
    for (i32 z = extent.min_z; z <= extent.max_z; ++z) {
        for (i32 x = extent.min_x; x <= extent.max_x; ++x) {
            const RegionState* state = world.find(RegionCoord{x, z, extent.level});
            if (state == nullptr) {
                digest.u64_value(0);
                continue;
            }
            for (u64 stage : state->stage_digests) {
                digest.u64_value(stage);
            }
            digest.u64_value(state->accepted.digest());
        }
    }
    return digest.value();
}

/// The identities in one region, in order. What an override binds to.
[[nodiscard]] inline u64 region_identity_digest(const RegionState& state) noexcept {
    Digest digest;
    for (usize index = 0; index < state.accepted.size(); ++index) {
        digest.u64_value(state.accepted.identity(index));
    }
    return digest.value();
}

/// How many regions of `extent` disagree between two worlds, comparing EVERY STAGE'S output and the
/// accepted points with their identities. The number every claim in `test_invalidation.cpp` is made
/// of, and it is stage-wise for the reason `world_digest()` gives.
[[nodiscard]] inline u32 regions_disagreeing(const GenerationWorld& a,
                                             const GenerationWorld& b) noexcept {
    u32 total = 0;
    const RegionExtent& extent = a.extent();
    for (i32 z = extent.min_z; z <= extent.max_z; ++z) {
        for (i32 x = extent.min_x; x <= extent.max_x; ++x) {
            const RegionCoord region{x, z, extent.level};
            const RegionState* left = a.find(region);
            const RegionState* right = b.find(region);
            if (left == nullptr || right == nullptr) {
                total += left != right ? 1U : 0U;
                continue;
            }
            bool differs = left->accepted.digest() != right->accepted.digest() ||
                           left->stage_digests.size() != right->stage_digests.size();
            for (usize stage = 0; stage < left->stage_digests.size() && !differs; ++stage) {
                differs = left->stage_digests[stage] != right->stage_digests[stage];
            }
            total += differs ? 1U : 0U;
        }
    }
    return total;
}

}  // namespace cy::pcg::test

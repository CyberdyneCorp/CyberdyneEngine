// The compiler: validation, the refusals the spike demands, the optimisation passes, and the flat
// program the evaluator runs. See include/cy/pcg/program.h.

#include <cy/pcg/program.h>

#include <cy/pcg/graph.h>

#include <cstring>
#include <utility>

namespace cy::pcg {

const char* compile_problem_name(CompileProblem problem) noexcept {
    switch (problem) {
        case CompileProblem::None:
            return "none";
        case CompileProblem::UnknownInput:
            return "unknown-input";
        case CompileProblem::DuplicateName:
            return "duplicate-name";
        case CompileProblem::Cycle:
            return "cycle";
        case CompileProblem::ReadsSiblingOutput:
            return "reads-sibling-output";
        case CompileProblem::IdentityFromTraversal:
            return "identity-from-traversal";
        case CompileProblem::IterationWithoutBound:
            return "iteration-without-bound";
        case CompileProblem::HaloTooSmallForIteration:
            return "halo-too-small-for-iteration";
        case CompileProblem::UnbudgetedRuntimeDomain:
            return "unbudgeted-runtime-domain";
        case CompileProblem::NoDomainDeclared:
            return "no-domain-declared";
        case CompileProblem::NoOutput:
            return "no-output";
        case CompileProblem::TypeMismatch:
            return "type-mismatch";
        case CompileProblem::UnknownAttribute:
            return "unknown-attribute";
        case CompileProblem::DuplicateChannel:
            return "duplicate-channel";
        case CompileProblem::ReachWithoutAccess:
            return "reach-without-access";
        case CompileProblem::TooManyNodes:
            return "too-many-nodes";
        case CompileProblem::kCount:
            break;
    }
    return "unknown";
}

const char* parallelism_name(Parallelism level) noexcept {
    switch (level) {
        case Parallelism::PerRegion:
            return "per-region";
        case Parallelism::Swept:
            return "swept";
        case Parallelism::Barrier:
            return "barrier";
    }
    return "unknown";
}

const char* gpu_eligibility_name(GpuEligibility eligibility) noexcept {
    switch (eligibility) {
        case GpuEligibility::Eligible:
            return "eligible";
        case GpuEligibility::CpuOnly:
            return "cpu-only";
        case GpuEligibility::RefusedByDeterminism:
            return "refused-by-determinism";
    }
    return "unknown";
}

GpuEligibility classify_gpu(NodeKind kind, bool gameplay_deterministic) noexcept {
    // "GPU-suitable work SHALL include large-scale candidate generation, field sampling, noise,
    // density evaluation, and filtering. CPU work SHALL include entity template construction,
    // complex constraint solving, navigation queries, and world persistence integration."
    bool suitable = false;
    switch (kind) {
        case NodeKind::Constant:
        case NodeKind::Noise:
        case NodeKind::FieldRead:
        case NodeKind::Stamp:
        case NodeKind::Smooth:
        case NodeKind::Compute:
        case NodeKind::Scatter:
        case NodeKind::Filter:
            suitable = true;
            break;
        case NodeKind::Propagate:  // an ordered sweep with a cross-region fixed point
        case NodeKind::Spacing:    // constraint solving across a neighbourhood
        case NodeKind::Script:     // a barrier by declaration
        case NodeKind::Output:     // adapter emission, which is world integration
        case NodeKind::kCount:
            suitable = false;
            break;
    }
    if (!suitable) {
        return GpuEligibility::CpuOnly;
    }
    // "A generator declared deterministic SHALL only use GPU execution where that execution meets
    // its declared determinism level." design.md §1.5 refuses to claim on this host that it does —
    // one GPU vendor and one driver is not the claim — so a gameplay-deterministic generator's
    // eligible nodes are recorded as REFUSED rather than scheduled. The criterion that would lift
    // this is `pcg-gpu-domain-agreement`, and it is NOT EVALUATED here on purpose.
    return gameplay_deterministic ? GpuEligibility::RefusedByDeterminism : GpuEligibility::Eligible;
}

Parallelism classify_parallelism(NodeKind kind, IterationPolicy iteration) noexcept {
    if (kind == NodeKind::Script) {
        return Parallelism::Barrier;
    }
    if (iteration != IterationPolicy::None) {
        return Parallelism::Swept;
    }
    return Parallelism::PerRegion;
}

namespace {

[[nodiscard]] bool same_name(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return std::strcmp(a, b) == 0;
}

/// A node's state through the passes. One entry per authored node, indexed by (NodeId - 1).
struct Working {
    NodeKind kind = NodeKind::Constant;
    NodeParams params;
    bool live = false;
    /// Fused into a later filter, so it emits no stage of its own.
    bool absorbed = false;
    /// This node's sampling was fused into an earlier identical one, and which one.
    bool query_fused = false;
    u32 fused_into = 0;
    u32 consumers = 0;
    /// The stage index this node became, or `Program::kNoStage`.
    u8 stage = Program::kNoStage;
};

[[nodiscard]] Status refuse(Array<CompileDiagnostic>& diagnostics, CompileProblem problem,
                            NodeId node, const char* name, const char* other = "") noexcept {
    CompileDiagnostic diagnostic;
    diagnostic.problem = problem;
    diagnostic.node = node;
    diagnostic.node_name = name;
    diagnostic.other_name = other;
    return diagnostics.push_back(diagnostic);
}

/// How many raster-shaped and point-shaped inputs a kind admits. A RANGE rather than a count,
/// because `Compute` blends one channel or two and a fixed arity would have made the second case a
/// second node kind. `-1` means "anything", which only a `Script` is.
struct Arity {
    i8 rasters_min = 0;
    i8 rasters_max = 0;
    i8 points_min = 0;
    i8 points_max = 0;

    [[nodiscard]] bool any() const noexcept { return rasters_min < 0; }
    [[nodiscard]] bool admits(i32 rasters, i32 points) const noexcept {
        return any() || (rasters >= rasters_min && rasters <= rasters_max && points >= points_min &&
                         points <= points_max);
    }
};

[[nodiscard]] Arity arity_of(NodeKind kind) noexcept {
    switch (kind) {
        case NodeKind::Constant:
        case NodeKind::Noise:
        case NodeKind::FieldRead:
            return Arity{0, 0, 0, 0};
        case NodeKind::Stamp:
        case NodeKind::Smooth:
        case NodeKind::Propagate:
        case NodeKind::Scatter:
            return Arity{1, 1, 0, 0};
        case NodeKind::Compute:
            return Arity{1, 2, 0, 0};
        case NodeKind::Filter:
        case NodeKind::Spacing:
        case NodeKind::Output:
            return Arity{0, 0, 1, 1};
        case NodeKind::Script:
        case NodeKind::kCount:
            return Arity{-1, -1, -1, -1};
    }
    return Arity{-1, -1, -1, -1};
}

// --- Pass 1: validation and the refusals ---------------------------------------------------------

[[nodiscard]] Status check_names(const Graph& graph,
                                 Array<CompileDiagnostic>& diagnostics) noexcept {
    const Span<const GraphNode> nodes = graph.nodes();
    for (usize outer = 0; outer < nodes.size(); ++outer) {
        for (usize inner = 0; inner < outer; ++inner) {
            // Two raster nodes on one channel. See `DuplicateChannel` — the invalidation's digest
            // gate is only sound while each stage owns what it writes.
            if (produces_raster(nodes[outer].kind) && produces_raster(nodes[inner].kind) &&
                nodes[outer].output.is_valid() && nodes[outer].output == nodes[inner].output) {
                if (Status pushed = refuse(diagnostics, CompileProblem::DuplicateChannel,
                                           NodeId{static_cast<u32>(outer + 1)}, nodes[outer].name,
                                           nodes[inner].name);
                    !pushed) {
                    return pushed;
                }
            }
            if (same_name(nodes[outer].name, nodes[inner].name)) {
                // Identity derives from the name, so two nodes sharing one would mint the same
                // identities for different instances — a collision with no diagnostic, which is the
                // failure mode identity.h exists to avoid.
                if (Status pushed = refuse(diagnostics, CompileProblem::DuplicateName,
                                           NodeId{static_cast<u32>(outer + 1)}, nodes[outer].name,
                                           nodes[inner].name);
                    !pushed) {
                    return pushed;
                }
            }
        }
    }
    return ok();
}

/// The four refusals design.md §1.6 makes requirements on this row, plus the typing and the edges.
[[nodiscard]] Status check_node(const Graph& graph, const GraphNode& node, NodeId id,
                                Array<CompileDiagnostic>& diagnostics) noexcept {
    // THE SPIKE'S FIRST CONDITION. A node that reads a sibling's ACCEPTED OUTPUT is
    // order-dependent: in a full run the region later in traversal has nothing accepted yet, and in
    // a partial run the cache is already holding its points. Reproduced 2 of 12 trials at best.
    if (node.neighbour_access == NeighbourAccess::AcceptedOutput) {
        if (Status pushed = refuse(diagnostics, CompileProblem::ReadsSiblingOutput, id, node.name);
            !pushed) {
            return pushed;
        }
    }
    // THE SPIKE'S FOURTH CONDITION. 43% and 3.7% of overrides silently mis-bound, respectively.
    if (node.identity_source != IdentitySource::Derived) {
        if (Status pushed = refuse(diagnostics, CompileProblem::IdentityFromTraversal, id,
                                   node.name, identity_source_name(node.identity_source));
            !pushed) {
            return pushed;
        }
    }
    // "Every iterative node SHALL declare a bound, so that generation cannot fail to terminate."
    // Both shapes of iteration are covered: a CROSS-REGION solve declares a policy and a bound, and
    // a node whose iteration is bounded inside its own halo — `Smooth` — declares the pass count in
    // the same field. A `Propagate` with no policy is the second half of the same sentence: its
    // fixed point spans regions, so a single sweep is not a result.
    const bool iterates = node.iteration != IterationPolicy::None ||
                          node.kind == NodeKind::Smooth || node.kind == NodeKind::Propagate;
    const bool undeclared_policy =
        node.kind == NodeKind::Propagate && node.iteration == IterationPolicy::None;
    if ((iterates && node.iteration_bound == 0) || undeclared_policy) {
        if (Status pushed =
                refuse(diagnostics, CompileProblem::IterationWithoutBound, id, node.name);
            !pushed) {
            return pushed;
        }
    }
    // A relaxation's halo must be at least its pass count IN CELLS, or the region's own cells are
    // not what a whole-world pass would have produced. The halo is declared in REGIONS, so this is
    // the one place the two units meet — and it is a refusal rather than a clamp, because a clamped
    // halo produces a plausible wrong answer at every region boundary in the world.
    if (node.kind == NodeKind::Smooth &&
        node.iteration_bound > static_cast<u32>(node.reach_regions) * kRegionCells) {
        if (Status pushed =
                refuse(diagnostics, CompileProblem::HaloTooSmallForIteration, id, node.name);
            !pushed) {
            return pushed;
        }
    }
    // A declared reach with nothing to reach for, or an access with no distance, is a declaration
    // the invalidation cannot use — and the invalidation is what the declaration is FOR.
    const bool reaches = node.reach_regions != 0;
    const bool accesses = node.neighbour_access != NeighbourAccess::None;
    if (reaches != accesses) {
        if (Status pushed = refuse(diagnostics, CompileProblem::ReachWithoutAccess, id, node.name,
                                   neighbour_access_name(node.neighbour_access));
            !pushed) {
            return pushed;
        }
    }
    if (node.output.is_valid() && graph.attributes().find(node.output) == nullptr) {
        if (Status pushed = refuse(diagnostics, CompileProblem::UnknownAttribute, id, node.name);
            !pushed) {
            return pushed;
        }
    }
    return ok();
}

[[nodiscard]] Status check_edges(const Graph& graph, const GraphNode& node, NodeId id,
                                 Array<CompileDiagnostic>& diagnostics) noexcept {
    const Arity arity = arity_of(node.kind);
    i32 rasters = 0;
    i32 points = 0;
    for (u8 index = 0; index < node.input_count; ++index) {
        const GraphNode* input = graph.find(node.inputs[index]);
        if (input == nullptr) {
            if (Status pushed = refuse(diagnostics, CompileProblem::UnknownInput, id, node.name);
                !pushed) {
                return pushed;
            }
            continue;
        }
        (produces_raster(input->kind) ? rasters : points) += 1;
    }
    if (!arity.admits(rasters, points)) {
        if (Status pushed = refuse(diagnostics, CompileProblem::TypeMismatch, id, node.name,
                                   node_kind_name(node.kind));
            !pushed) {
            return pushed;
        }
    }
    return ok();
}

[[nodiscard]] Status check_graph(const Graph& graph,
                                 Array<CompileDiagnostic>& diagnostics) noexcept {
    if (graph.domains().empty()) {
        if (Status pushed =
                refuse(diagnostics, CompileProblem::NoDomainDeclared, NodeId{}, graph.name());
            !pushed) {
            return pushed;
        }
    }
    const bool runtime = graph.domains().has(ExecutionDomain::Runtime) ||
                         graph.domains().has(ExecutionDomain::Streaming) ||
                         graph.domains().has(ExecutionDomain::Dynamic);
    if (runtime && !graph.budget().declared()) {
        // "Runtime and streaming domains SHALL declare their budgets, and execution SHALL be
        // scheduled through the task system under those budgets" — and "Runtime generation
        // bypassing task scheduling, memory budgets, or streaming budgets" is a forbidden pattern
        // the specification asks to be checkable. This is the check.
        if (Status pushed = refuse(diagnostics, CompileProblem::UnbudgetedRuntimeDomain, NodeId{},
                                   graph.name());
            !pushed) {
            return pushed;
        }
    }
    bool has_output = false;
    for (const GraphNode& node : graph.nodes()) {
        has_output = has_output || node.kind == NodeKind::Output;
    }
    if (!has_output) {
        if (Status pushed = refuse(diagnostics, CompileProblem::NoOutput, NodeId{}, graph.name());
            !pushed) {
            return pushed;
        }
    }
    // Sixty-four, because `RegionState::evaluated_mask` is one bit per stage: a region carries
    // which stages it has been evaluated at, and that is what a partial regeneration over an
    // incomplete world is refused on. A generator larger than this is a generator that should be
    // subgraphs, and widening the mask is the change to make if one ever is.
    if (graph.size() > 64) {
        return refuse(diagnostics, CompileProblem::TooManyNodes, NodeId{}, graph.name());
    }
    return ok();
}

// --- Pass 2: topological order ----------------------------------------------------------------

/// Kahn's algorithm over the declared edges, emitting in AUTHORED order among ready nodes so that
/// the compiled order is a function of the graph rather than of a queue's shape.
[[nodiscard]] Expected<bool, Error> topological_order(const Graph& graph,
                                                      Array<u32>& out) noexcept {
    const Span<const GraphNode> nodes = graph.nodes();
    Array<u32> remaining(out.allocator());
    Array<u8> emitted(out.allocator());
    if (Status sized = remaining.resize(nodes.size()); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status sized = emitted.resize(nodes.size()); !sized) {
        return make_unexpected(sized.error());
    }
    for (usize index = 0; index < nodes.size(); ++index) {
        remaining[index] = nodes[index].input_count;
        emitted[index] = 0;
    }
    for (usize pass = 0; pass < nodes.size(); ++pass) {
        bool progressed = false;
        for (usize index = 0; index < nodes.size(); ++index) {
            if (emitted[index] != 0) {
                continue;
            }
            u32 unresolved = 0;
            for (u8 slot = 0; slot < nodes[index].input_count; ++slot) {
                const NodeId input = nodes[index].inputs[slot];
                if (!input.is_valid() || input.value > nodes.size()) {
                    continue;  // already refused as an unknown input
                }
                unresolved += emitted[input.value - 1] == 0 ? 1U : 0U;
            }
            if (unresolved != 0) {
                continue;
            }
            emitted[index] = 1;
            if (Status pushed = out.push_back(static_cast<u32>(index)); !pushed) {
                return make_unexpected(pushed.error());
            }
            progressed = true;
        }
        if (!progressed) {
            break;
        }
    }
    return out.size() == nodes.size();
}

// --- Pass 3: dead-node elimination ------------------------------------------------------------

[[nodiscard]] Status eliminate_dead(const Graph& graph, Span<const u32> order,
                                    Array<Working>& working, CompileReport& report) noexcept {
    const Span<const GraphNode> nodes = graph.nodes();
    for (const GraphNode& node : nodes) {
        if (node.kind == NodeKind::Output) {
            working[static_cast<usize>(&node - nodes.data())].live = true;
        }
    }
    // Backwards over the topological order: a node is live when something live reads it.
    for (usize position = order.size(); position > 0; --position) {
        const u32 index = order[position - 1];
        if (!working[index].live) {
            continue;
        }
        for (u8 slot = 0; slot < nodes[index].input_count; ++slot) {
            const NodeId input = nodes[index].inputs[slot];
            if (input.is_valid() && input.value <= nodes.size()) {
                working[input.value - 1].live = true;
                working[input.value - 1].consumers += 1;
            }
        }
    }
    for (usize index = 0; index < nodes.size(); ++index) {
        if (working[index].live) {
            continue;
        }
        report.eliminated += 1;
        // "THEN the compiler SHALL eliminate it AND BE ABLE TO REPORT THAT IT DID." A count alone
        // cannot answer "which attribute did I lose", so the names go with it.
        if (Status pushed = report.eliminated_names.push_back(nodes[index].name); !pushed) {
            return pushed;
        }
    }
    return ok();
}

// --- Pass 4: constant folding -----------------------------------------------------------------

/// `Compute` of a constant is a constant; relaxing a constant is that constant.
///
/// Folded in topological order, so a chain of three computes over one constant folds in one sweep.
void fold_constants(const Graph& graph, Span<const u32> order, Array<Working>& working,
                    CompileReport& report) noexcept {
    const Span<const GraphNode> nodes = graph.nodes();
    for (u32 index : order) {
        Working& node = working[index];
        if (!node.live || nodes[index].input_count != 1) {
            continue;
        }
        const NodeId input = nodes[index].inputs[0];
        if (!input.is_valid() || input.value > nodes.size()) {
            continue;
        }
        const Working& source = working[input.value - 1];
        if (source.kind != NodeKind::Constant) {
            continue;
        }
        if (node.kind == NodeKind::Compute) {
            const f32 base = source.params.value;
            node.params.value = nodes[index].params.invert
                                    ? (base != 0.0F ? 1.0F / base : 0.0F)
                                    : base * nodes[index].params.value + nodes[index].params.second;
            node.kind = NodeKind::Constant;
            report.folded += 1;
        } else if (node.kind == NodeKind::Smooth) {
            // A relaxation of a uniform field is that field, whatever the halo and the pass count.
            node.params.value = source.params.value;
            node.kind = NodeKind::Constant;
            report.folded += 1;
        }
    }
}

// --- Pass 5: filter fusion and spatial query fusion -------------------------------------------

/// A filter whose SOLE consumer is another filter is folded into it: four threshold tests become
/// one pass over the point set rather than four.
void fuse_filters(const Graph& graph, Span<const u32> order, Array<Working>& working,
                  CompileReport& report) noexcept {
    const Span<const GraphNode> nodes = graph.nodes();
    for (u32 index : order) {
        if (!working[index].live || nodes[index].kind != NodeKind::Filter) {
            continue;
        }
        const NodeId input = nodes[index].inputs[0];
        if (!input.is_valid() || input.value > nodes.size()) {
            continue;
        }
        const u32 source = input.value - 1;
        const bool fusable = working[source].live && !working[source].absorbed &&
                             nodes[source].kind == NodeKind::Filter &&
                             working[source].consumers == 1;
        if (!fusable) {
            continue;
        }
        working[source].absorbed = true;
        report.filters_fused += 1;
    }
}

/// Two `FieldRead` nodes sampling the SAME field at the same reach sample it once: the second is
/// marked fused and copies the first's channel. The specification's "spatial query fusion".
void fuse_queries(const Graph& graph, Span<const u32> order, Array<Working>& working,
                  CompileReport& report) noexcept {
    const Span<const GraphNode> nodes = graph.nodes();
    for (usize position = 0; position < order.size(); ++position) {
        const u32 index = order[position];
        if (!working[index].live || nodes[index].kind != NodeKind::FieldRead) {
            continue;
        }
        for (usize earlier = 0; earlier < position; ++earlier) {
            const u32 other = order[earlier];
            if (!working[other].live || working[other].query_fused ||
                nodes[other].kind != NodeKind::FieldRead) {
                continue;
            }
            if (nodes[other].params.field == nodes[index].params.field) {
                working[index].query_fused = true;
                working[index].fused_into = other;
                report.queries_fused += 1;
                break;
            }
        }
    }
}

/// An attribute written by a live node and read by none is PROJECTED AWAY: the column is not
/// allocated and the write is not emitted. Reported, because it is usually a rule an author is
/// still writing rather than a mistake.
void project_attributes(const Graph& graph, Span<const u32> order, const Array<Working>& working,
                        CompileReport& report) noexcept {
    const Span<const GraphNode> nodes = graph.nodes();
    for (u32 index : order) {
        if (!working[index].live || !nodes[index].output.is_valid()) {
            continue;
        }
        bool read = false;
        for (const GraphNode& consumer : nodes) {
            const usize consumer_index = static_cast<usize>(&consumer - nodes.data());
            if (!working[consumer_index].live) {
                continue;
            }
            read = read || consumer.reads == nodes[index].output;
            for (u8 slot = 0; slot < consumer.input_count && !read; ++slot) {
                read = consumer.inputs[slot].value == index + 1;
            }
            if (read) {
                break;
            }
        }
        if (!read && nodes[index].kind != NodeKind::Output) {
            report.attributes_projected += 1;
        }
    }
}

}  // namespace

// --- The compiler -----------------------------------------------------------------------------

Expected<Program, Error> compile(Allocator& allocator, const Graph& graph, CompileReport& report,
                                 Array<CompileDiagnostic>& diagnostics) noexcept {
    const usize refusals_before = diagnostics.size();
    report.nodes_in = static_cast<u32>(graph.size());

    if (Status checked = check_graph(graph, diagnostics); !checked) {
        return make_unexpected(checked.error());
    }
    if (Status checked = check_names(graph, diagnostics); !checked) {
        return make_unexpected(checked.error());
    }
    const Span<const GraphNode> nodes = graph.nodes();
    for (usize index = 0; index < nodes.size(); ++index) {
        const NodeId id{static_cast<u32>(index + 1)};
        if (Status checked = check_node(graph, nodes[index], id, diagnostics); !checked) {
            return make_unexpected(checked.error());
        }
        if (Status checked = check_edges(graph, nodes[index], id, diagnostics); !checked) {
            return make_unexpected(checked.error());
        }
    }

    Array<u32> order(allocator);
    Expected<bool, Error> acyclic = topological_order(graph, order);
    if (!acyclic) {
        return make_unexpected(acyclic.error());
    }
    if (!*acyclic) {
        if (Status pushed = refuse(diagnostics, CompileProblem::Cycle, NodeId{}, graph.name());
            !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    if (diagnostics.size() != refusals_before) {
        return make_unexpected(Error{
            ErrorCode::InvalidArgument,
            "pcg: the graph was refused; see the compile diagnostics for which node and why"});
    }

    Array<Working> working(allocator);
    if (Status sized = working.resize(nodes.size()); !sized) {
        return make_unexpected(sized.error());
    }
    for (usize index = 0; index < nodes.size(); ++index) {
        working[index].kind = nodes[index].kind;
        working[index].params = nodes[index].params;
    }

    if (Status eliminated = eliminate_dead(graph, order.span(), working, report); !eliminated) {
        return make_unexpected(eliminated.error());
    }
    fold_constants(graph, order.span(), working, report);
    fuse_filters(graph, order.span(), working, report);
    fuse_queries(graph, order.span(), working, report);
    project_attributes(graph, order.span(), working, report);

    Program program(allocator);
    program.name_ = graph.name();
    program.version_ = graph.version();
    program.domains_ = graph.domains();
    program.budget_ = graph.budget();
    program.determinism_ = graph.determinism();
    program.region_metres_ = graph.region_metres();
    program.region_level_ = graph.region_level();

    Expected<AttributeTable, Error> attributes = graph.attributes().clone();
    if (!attributes) {
        return make_unexpected(attributes.error());
    }
    program.attributes_ = std::move(*attributes);

    // The two columns the evaluator owns. Interned HERE rather than by an author, so that a graph
    // that never mentions priority still has one and no author can give `pcg.priority` a different
    // type underneath the conflict resolution.
    Expected<AttributeId, Error> priority =
        program.attributes_.intern("pcg.priority", AttributeType::U64);
    if (!priority) {
        return make_unexpected(priority.error());
    }
    program.priority_ = *priority;
    Expected<AttributeId, Error> density =
        program.attributes_.intern("pcg.density", AttributeType::F32);
    if (!density) {
        return make_unexpected(density.error());
    }
    program.density_ = *density;

    const bool gameplay = graph.determinism() == DeterminismLevel::Gameplay;
    for (u32 index : order) {
        if (!working[index].live || working[index].absorbed) {
            continue;
        }
        const GraphNode& node = nodes[index];
        Stage stage;
        stage.kind = working[index].kind;
        stage.identity = node_identity(node.name);
        stage.name = node.name;
        stage.output = node.output;
        stage.reads = node.reads;
        stage.neighbour_access = node.neighbour_access;
        stage.reach_regions = node.reach_regions;
        stage.iteration = node.iteration;
        stage.iteration_bound = node.iteration_bound;
        stage.params = working[index].params;
        stage.query_fused = working[index].query_fused;
        stage.parallelism = classify_parallelism(stage.kind, stage.iteration);
        stage.gpu = classify_gpu(stage.kind, gameplay);

        // A folded node has no inputs left: it computes its own value. Emitting its edge would make
        // the evaluator read a channel the fold made unnecessary and would defeat the fold.
        if (stage.kind != NodeKind::Constant || node.kind == NodeKind::Constant) {
            for (u8 slot = 0; slot < node.input_count; ++slot) {
                const NodeId input = node.inputs[slot];
                // An absorbed filter's stage does not exist, so its consumer inherits ITS input.
                u32 resolved = input.value - 1;
                while (working[resolved].absorbed && nodes[resolved].input_count == 1) {
                    resolved = nodes[resolved].inputs[0].value - 1;
                }
                stage.inputs[stage.input_count++] = working[resolved].stage;
            }
        }

        // A FUSED FIELD READ takes the earlier read's stage as its one input, so the evaluator
        // COPIES the channel rather than sampling the same field twice. Recording the fusion
        // without skipping the work would have been a report of an optimisation that did not
        // happen.
        if (working[index].query_fused) {
            stage.input_count = 0;
            stage.inputs[stage.input_count++] = working[working[index].fused_into].stage;
        }

        // The absorbed filters, innermost first, so that the fused pass applies them in the order
        // the author wrote — which matters for the rejection provenance, not for the result.
        if (stage.kind == NodeKind::Filter) {
            u32 absorbed = node.inputs[0].value - 1;
            while (working[absorbed].absorbed && stage.fused_count < Stage::kMaxFusedFilters) {
                Stage::FusedFilter fused;
                fused.reads = nodes[absorbed].reads;
                fused.lower = nodes[absorbed].params.value;
                fused.upper = nodes[absorbed].params.second;
                fused.range_test = nodes[absorbed].params.range_test;
                stage.fused[stage.fused_count++] = fused;
                if (nodes[absorbed].input_count != 1) {
                    break;
                }
                absorbed = nodes[absorbed].inputs[0].value - 1;
            }
        }

        if (stage.kind == NodeKind::FieldRead && node.params.field != 0) {
            bool known = false;
            for (u64 field : program.fields_) {
                known = known || field == node.params.field;
            }
            if (!known) {
                if (Status pushed = program.fields_.push_back(node.params.field); !pushed) {
                    return make_unexpected(pushed.error());
                }
            }
        }

        if (stage.parallelism == Parallelism::Barrier) {
            report.barriers += 1;
            if (Status pushed = report.barrier_names.push_back(stage.name); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
        report.gpu_eligible += stage.gpu == GpuEligibility::Eligible ? 1U : 0U;
        report.gpu_refused_by_determinism +=
            stage.gpu == GpuEligibility::RefusedByDeterminism ? 1U : 0U;
        report.gpu_hint_disagreements +=
            (node.gpu_hint && stage.gpu == GpuEligibility::CpuOnly) ? 1U : 0U;

        // A budgeted iteration makes the whole program uncacheable. cache.h says why, and it is the
        // only axis of the spike's matrix with no survivor anywhere.
        if (stage.iteration == IterationPolicy::Budget) {
            program.cacheable_ = false;
        }
        program.total_reach_ += stage.reach_regions;

        // Every raster channel the program writes, once, in emission order: the region raster's
        // whole allocation is made from this list rather than grown as stages discover channels.
        if (produces_raster(stage.kind) && stage.output.is_valid()) {
            bool known = false;
            for (AttributeId channel : program.channels_) {
                known = known || channel == stage.output;
            }
            if (!known) {
                if (Status pushed = program.channels_.push_back(stage.output); !pushed) {
                    return make_unexpected(pushed.error());
                }
            }
            if (!program.macro_height_.is_valid()) {
                program.macro_height_ = stage.output;
            }
        }

        working[index].stage = static_cast<u8>(program.stages_.size());
        if (stage.kind == NodeKind::Scatter) {
            program.scatter_stage_ = working[index].stage;
            // The channel the scatter reads IS the region's macro density: a region whose detail is
            // gone still knows how much would be there, which is what makes materialisation
            // consistent with macro state rather than a reconciliation after the fact.
            if (stage.input_count == 1) {
                program.macro_density_ = program.stages_[stage.inputs[0]].output;
            }
        }
        if (stage.kind == NodeKind::Spacing) {
            program.spacing_stage_ = working[index].stage;
        }
        if (stage.kind == NodeKind::Output) {
            program.output_stage_ = working[index].stage;
        }
        if (Status pushed = program.stages_.push_back(stage); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    report.nodes_out = static_cast<u32>(program.stages_.size());

    Digest digest;
    digest.u32_value(program.version_);
    digest.u64_value(static_cast<u64>(program.domains_.bits));
    digest.u32_value(static_cast<u32>(program.determinism_));
    digest.u64_value(static_cast<u64>(program.region_metres_ * 1000.0));
    digest.u32_value(program.region_level_);
    for (const Stage& stage : program.stages_) {
        digest.u32_value(static_cast<u32>(stage.kind));
        digest.u64_value(stage.identity.value);
        digest.u32_value(stage.output.value);
        digest.u32_value(stage.reads.value);
        digest.u32_value(static_cast<u32>(stage.neighbour_access));
        digest.u32_value(stage.reach_regions);
        digest.u32_value(static_cast<u32>(stage.iteration));
        digest.u32_value(stage.iteration_bound);
        digest.f32_value(stage.params.value);
        digest.f32_value(stage.params.second);
        digest.f32_value(stage.params.frequency);
        digest.f32_value(stage.params.amplitude);
        digest.u32_value(stage.params.count);
        digest.u64_value(stage.params.field);
        digest.u32_value(stage.params.range_test ? 1U : 0U);
        digest.u32_value(stage.params.invert ? 1U : 0U);
        digest.u32_value(stage.fused_count);
        for (u8 fused = 0; fused < stage.fused_count; ++fused) {
            digest.u32_value(stage.fused[fused].reads.value);
            digest.f32_value(stage.fused[fused].lower);
            digest.f32_value(stage.fused[fused].upper);
        }
        for (u8 slot = 0; slot < stage.input_count; ++slot) {
            digest.u32_value(stage.inputs[slot]);
        }
    }
    program.digest_ = digest.value();
    return program;
}

}  // namespace cy::pcg

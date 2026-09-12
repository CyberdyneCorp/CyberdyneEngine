#pragma once
// The vocabulary a compiled program and an authored graph share: the operator set, what a node may
// read of its neighbours, how an iterative node terminates, and the domains and budgets a generator
// declares. M10 task 4.1.
//
// WHY THIS IS ITS OWN HEADER. `procedural-content-generation` requires that "Execution SHALL run
// the compiled program. Interpreted graphs of virtual node objects SHALL NOT be executed in hot
// paths." The way to make that a fact about the build rather than a promise is for the evaluator
// never to see an authored graph at all: `execute.h` includes `program.h`, `program.h` includes
// THIS file and forward-declares `Graph`, and `graph.h` — the authoring layer, with its owned
// strings, its subgraph instantiation and its edge list — is included by the compiler and by
// nothing below it.
//
// ================================================================================================
// THE FOUR DECLARATIONS THAT EXIST BECAUSE THE SPIKE MEASURED WHAT HAPPENS WITHOUT THEM
// ================================================================================================
//
// design.md §1.2 and §1.6. Each is a field a node declares, and each is refused or recorded by the
// compiler rather than trusted:
//
//   `neighbour_access`  What a node may read of its NEIGHBOURS. `Candidates` is sound — a candidate
//                       list is a pure function of the neighbour's own inputs. `AcceptedOutput` is
//                       the order-dependent one, and the compiler refuses it: the spike's `ordered`
//                       resolution reproduced a full regeneration in 2 of 12 trials at best, and
//                       the 36 to 48 rejections whose only cause was a region the traversal had not
//                       reached are what it costs.
//   `reach_regions`     How far a node reads. The invalidation dilates by it — and expands past it
//                       to a FIXED POINT, because a declared radius applied once reproduced in 7 of
//                       12, which is the dangerous cell: a casual test calls it sound.
//   `iteration`         `Convergence` or `Budget`. A budget is a legitimate runtime lever and a
//                       budgeted result is NOT CACHEABLE; the spike's `budget2` reproduced in 0 of
//                       12 trials of all 12 of its configurations, the only axis with no survivor
//                       anywhere. `program.h` marks the program uncacheable and `cache.h` refuses
//                       to store it.
//   `identity_source`   `Derived` only. `TraversalCounter` and `SurvivorRank` are spellable so they
//                       can be refused by name — identity.h says why.

#include <cy/core/base/types.h>
#include <cy/pcg/dataset.h>

namespace cy::pcg {

/// Raster cells along a region's edge.
///
/// A constant rather than a per-generator declaration, for the reason `environment`'s `kTileCells`
/// is one: a resolution that varied per generator would make "the region that changed" mean a
/// different amount of world for every consumer of it, and would put a division in the innermost
/// loop of every gather. The REGION'S SIZE IN METRES is the per-generator property the
/// specification requires (`Graph::declare_region_size`), so a 16 m grass region and a 2 km road
/// region both have sixteen cells and different cell sizes — which is the composition the
/// specification's hierarchy scenario asks for.
///
/// It lives in this header rather than in `execute.h` because the COMPILER needs it: a relaxation
/// declaring more passes than its declared halo has cells is a node that reads outside what it
/// declared, and `program.h` refuses that rather than producing a region whose edge is wrong.
inline constexpr u32 kRegionCells = 16;
inline constexpr u32 kRegionCellCount = kRegionCells * kRegionCells;

/// A node's place in the authored graph. An index, and deliberately NOT what identity derives
/// from — `identity.h` derives from the node's name for the reason that file gives.
struct NodeId {
    u32 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(NodeId, NodeId) noexcept = default;
};

/// The operator set. Small on purpose: every one of these has a declared reach, a deterministic
/// derivation and a digestible output, and an operator with none of those is an operator the cache
/// and the invalidation cannot reason about.
///
/// The three shapes of edge the spike names are all here, so that what it measured is what this
/// module runs: `Smooth` is the BOUNDED GATHER (its halo is provably exact), `Propagate` is the
/// TRANSITIVE edge (a region reads four neighbours and water crosses the map), and `Spacing` is
/// CROSS-REGION CONFLICT RESOLUTION.
enum class NodeKind : u8 {
    /// A raster channel filled with a constant. Foldable, and the compiler's constant-folding pass
    /// is measured against it.
    Constant = 0,
    /// Value noise over world coordinates. A pure function of position and seed, so two regions
    /// agree on their shared boundary by construction rather than by a seam pass.
    Noise,
    /// An environment field, sampled at each raster cell. The read participates in dependency
    /// tracking — `procedural-content-generation`'s "Field integration".
    FieldRead,
    /// Authored stamps added to a raster channel: the edit source, and the seed of every dirty set.
    Stamp,
    /// A bounded relaxation over a raster channel. Declared halo, declared iteration bound.
    Smooth,
    /// Downhill accumulation with boundary outflow — THE TRANSITIVE EDGE. A region reads only its
    /// four neighbours' outflow, but water crosses the world, so the reachable set is unbounded and
    /// data-dependent. Solved by sweeping to a fixed point; see `execute.h`.
    Propagate,
    /// An arithmetic expression over raster channels. Foldable when its inputs are constant.
    Compute,
    /// Candidate generation: `count` candidates per region from a density channel, each carrying
    /// its own SLOT. The only node that mints identity.
    Scatter,
    /// A threshold test on a point attribute. Fusable with an adjacent filter.
    Filter,
    /// Order-free conflict resolution over the neighbourhood's CANDIDATES. Never over their
    /// accepted output — see the header comment.
    Spacing,
    /// A scripted node: an optimisation and parallelisation BARRIER, reported as such.
    /// `procedural-content-generation` — "Bounded iteration": "A general scripted node MAY exist
    /// and SHALL be marked as an optimisation and parallelisation barrier, so its cost is visible."
    Script,
    /// The terminal: a dataset handed to an output adapter.
    Output,
    kCount,
};

[[nodiscard]] const char* node_kind_name(NodeKind kind) noexcept;

/// Whether a node's output is a raster channel (as opposed to a point set). Decided by the kind
/// alone, which is what makes the compiler's type check a table lookup rather than an inference.
[[nodiscard]] bool produces_raster(NodeKind kind) noexcept;

/// What a node reads of its NEIGHBOURS. The spike's first condition, made a declaration.
enum class NeighbourAccess : u8 {
    /// Nothing outside its own region. A pure function of world coordinates or of its own inputs.
    None = 0,
    /// A neighbour's raster channel, within `reach_regions`. A gather.
    Raster,
    /// A neighbour's CANDIDATE list — the points it generated before any rejection. Sound: a
    /// candidate list is a pure function of the neighbour's own inputs, so it is the same in a full
    /// run and in a partial one.
    Candidates,
    /// A neighbour's ACCEPTED output. ORDER-DEPENDENT, and refused by the compiler. Spellable so
    /// the refusal can name it.
    AcceptedOutput,
};

[[nodiscard]] const char* neighbour_access_name(NeighbourAccess access) noexcept;

/// How an iterative node terminates. The spike's third condition.
enum class IterationPolicy : u8 {
    /// Not iterative.
    None = 0,
    /// Sweep until nothing changes. `iteration_bound` is then a REFUSAL POINT rather than a budget:
    /// reaching it is a diagnostic, not a result.
    Convergence,
    /// Exactly `iteration_bound` sweeps, whatever the state after them. A legitimate runtime lever,
    /// and the result is not cacheable.
    Budget,
};

[[nodiscard]] const char* iteration_policy_name(IterationPolicy policy) noexcept;

/// The domains a generator may execute in. `procedural-content-generation` — "Execution domains":
/// "A generator not declared for a domain SHALL NOT run in it. A city generator intended for
/// cooking SHALL NOT be invocable at runtime by accident."
enum class ExecutionDomain : u8 {
    Editor = 0,
    Cook,
    Runtime,
    Streaming,
    Dynamic,
    kCount,
};

[[nodiscard]] const char* execution_domain_name(ExecutionDomain domain) noexcept;

/// A set of domains, as one byte.
struct DomainMask {
    u8 bits = 0;

    [[nodiscard]] constexpr bool has(ExecutionDomain domain) const noexcept {
        return (bits & static_cast<u8>(1U << static_cast<u8>(domain))) != 0;
    }
    constexpr void add(ExecutionDomain domain) noexcept {
        bits = static_cast<u8>(bits | (1U << static_cast<u8>(domain)));
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return bits == 0; }

    [[nodiscard]] static constexpr DomainMask of(ExecutionDomain domain) noexcept {
        DomainMask mask;
        mask.add(domain);
        return mask;
    }

    friend constexpr bool operator==(DomainMask, DomainMask) noexcept = default;
};

/// How reproducible a generator's results have to be.
///
/// `procedural-content-generation` — "Deterministic derivation": "Each generator SHALL declare a
/// determinism level consistent with the session's determinism profile, SO THAT DECORATIVE
/// GENERATION NEED NOT PAY FOR REPRODUCIBILITY THAT GAMEPLAY-RELEVANT GENERATION REQUIRES."
///
/// It is what decides whether an otherwise GPU-eligible node may actually be scheduled on a device:
/// "A generator declared deterministic SHALL only use GPU execution where that execution meets its
/// declared determinism level", and design.md §1.5 refuses to claim on this host that it does.
enum class DeterminismLevel : u8 {
    /// Decorative. Reproducibility is not required, so a GPU path is available to it.
    Presentation = 0,
    /// Gameplay-relevant: resource placement, navigation-bearing geometry, anything a peer or a
    /// save has to agree on.
    Gameplay,
};

[[nodiscard]] const char* determinism_level_name(DeterminismLevel level) noexcept;

/// What a runtime or streaming domain is allowed to spend. `procedural-content-generation` —
/// "Runtime generation": "scheduled through the task system, cancellable, and bounded by declared
/// processor, GPU, and memory budgets."
///
/// Zero means undeclared, and `program.h` refuses a program that declares `Runtime` or `Streaming`
/// with no budget — an unbudgeted runtime generator is the "bypassing ... memory budgets" the
/// forbidden-patterns requirement names.
struct GenerationBudget {
    /// Microseconds of processor time one incremental step may take.
    u32 cpu_micros = 0;
    /// Microseconds of GPU time. Zero is legitimate for a CPU-only generator.
    u32 gpu_micros = 0;
    /// Bytes of working memory the run may hold.
    u64 bytes = 0;

    [[nodiscard]] constexpr bool declared() const noexcept { return cpu_micros != 0 && bytes != 0; }
};

/// The most inputs a node takes. Four covers every operator above with room; a node needing more
/// would be a node the compiler cannot fuse, and is a `Script`.
inline constexpr u32 kMaxNodeInputs = 4;

/// Per-node parameters. One flat struct rather than a variant, because the compiler folds and
/// digests every field of it and a variant would need a visitor per pass to do the same work.
/// Fields a kind does not use are zero and are digested as zero, which keeps the program digest a
/// function of the whole declaration.
struct NodeParams {
    /// `Constant`: the value. `Filter`: the threshold. `Compute`: the scale.
    f32 value = 0.0F;
    /// `Compute`: the bias. `Filter`: the upper bound when `range_test` is set.
    f32 second = 0.0F;
    /// `Noise`: the base frequency in cycles per metre. `Spacing`: the spacing in metres.
    /// `Stamp`: unused — stamps carry their own radius.
    f32 frequency = 0.0F;
    /// `Noise`: the amplitude. `Smooth`: the relaxation rate in [0, 1].
    f32 amplitude = 1.0F;
    /// `Noise`: octaves. `Scatter`: candidates per region.
    u32 count = 0;
    /// `FieldRead`: the field. Zero for every other kind.
    u64 field = 0;
    /// `Filter`: reject below `value` (false) or outside [`value`, `second`] (true).
    bool range_test = false;
    /// `Compute`: multiply the input by `value` and add `second`; when set, take the input's
    /// reciprocal slope instead. Kept as a flag rather than an expression tree because an
    /// expression tree is a `Script`.
    bool invert = false;
};

}  // namespace cy::pcg

#pragma once
// The optimisation pipeline over the shared expression core. M8.b task 2.2.
//
// ================================================================================================
// THE PIPELINE IS A REBUILD, ITERATED TO A FIXED POINT
// ================================================================================================
//
// Every pass is a bottom-up rebuild through `Builder`, so a pass's output is in the same canonical
// form as its input by construction and the pipeline can be iterated until the module's digest
// stops moving. That is what makes the fixed point reachable from either front-end rather than
// only from the one whose traversal the passes were written against.
//
// TYPE PROPAGATION IS NOT A PASS. It is `Builder::make`, which types every node as it is
// constructed and refuses one it cannot type. A separate pass would be a second implementation of
// the same rules, and the two would disagree the first time a domain added an op.
//
// DEAD-NODE ELIMINATION IS NOT A PASS EITHER — it is a property of a rebuild that starts at the
// roots, which cannot reach an orphan. So the switch does the opposite of what a switch usually
// does: when it is OFF the pipeline explicitly carries the orphans across, which is the only way
// the flag can mean anything at all. The material spike found its own `pass_dce` switch to be a
// placebo for exactly this reason.
//
// A SWITCH REACHES THE FRONT-END'S BUILDER TOO. Interning happens in `Builder::make`, which is
// where a front-end constructs its nodes — so a front-end that ignored these switches would merge
// the values a bisection is trying to keep apart before the pipeline ever ran. `builder_policy` is
// the one translation from the switches into `BuilderPolicy`.

#include <cy/core/base/expected.h>
#include <cy/graph/expr.h>

namespace cy::graph {

/// The nine switches. All on is the shipping pipeline.
struct PassSwitches {
    /// Hash-consing in the builder: two structurally identical values become one.
    bool interning = true;
    /// Canonical ordering of a commutative operand list, by content hash.
    bool canonical_commutative = true;
    /// Constant folding, including the algebraic identities that folding exposes.
    bool constant_folding = true;
    /// Merging of identical interior expressions. Distinct from `interning`: with `interning` on
    /// and this off, leaves still merge and interior expressions do not.
    bool common_subexpression = true;
    /// Merging of identical samples — the `MergeClass::Sample` ops.
    bool sample_dedup = true;
    /// The domain's algebra over aggregate values.
    bool aggregate_simplification = true;
    /// Off carries orphans across explicitly. See the note above.
    bool dead_node_elimination = true;
    /// Marks the values that are constant across a dispatch, so the emitter can hoist them.
    bool uniform_varying = true;
    /// Emission order — canonical visit order rather than node id. Not a rebuild pass; it is read
    /// by the emitter, and it lives here so that the nine switches are one structure.
    bool canonical_emission_order = true;

    [[nodiscard]] bool all_enabled() const noexcept;
    /// Which switch is off, for the diagnostic. `nullptr` when they are all on.
    [[nodiscard]] const char* first_disabled() const noexcept;
};

/// What the pipeline did. Every count is a fact a cook report or an editor shows, and the two
/// `dropped` members are answerable BY AUTHORING NODE, which is what an editor has.
struct OptimiseReport {
    explicit OptimiseReport(Allocator& allocator) noexcept : dropped_origins(allocator) {}

    u32 iterations = 0;
    u32 nodes_before = 0;
    u32 nodes_after = 0;
    /// Nodes the rebuild could not reach from the roots.
    u32 dropped_nodes = 0;
    /// The authoring nodes behind them.
    Array<u32> dropped_origins;
    u32 folded_constants = 0;
    u32 simplified_aggregates = 0;
    /// Values that two or more source nodes collapsed into.
    u32 merged_values = 0;
    /// Samples removed by deduplication.
    u32 duplicate_samples_removed = 0;
    /// Values marked constant across a dispatch.
    u32 uniform_values = 0;
    /// True when any switch is off: the program this pipeline produced is NOT the shipping one.
    bool bisection_build = false;
};

/// The builder policy these switches imply. ONE TRANSLATION, used by the front-ends and by the
/// pipeline, for the reason at the top of this file.
[[nodiscard]] BuilderPolicy builder_policy(const PassSwitches& switches) noexcept;

/// Run the pipeline to a fixed point. The result is a new module; `source` is untouched. `report`
/// is appended to rather than cleared, so a caller running the pipeline over a family of programs
/// accumulates one report.
[[nodiscard]] Expected<Module, Error> optimise(const Module& source, const PassSwitches& switches,
                                               OptimiseReport& report) noexcept;

/// The largest number of iterations the fixed point is allowed. Reaching it is a defect in a pass —
/// a pipeline that does not converge is one whose passes undo each other — so it is reported as an
/// error rather than accepted quietly.
inline constexpr u32 kMaxOptimiseIterations = 8;

}  // namespace cy::graph

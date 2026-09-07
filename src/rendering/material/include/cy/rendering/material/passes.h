#pragma once
// The optimisation pipeline. M7 task 6.2.
//
// `material-compiler` — "Optimisation passes": "type propagation, constant folding, dead-node
// elimination, common subexpression elimination, texture sample deduplication, closure
// simplification, and uniform versus varying analysis", each "individually disableable in
// development builds, so a suspected miscompilation can be bisected".
//
// ================================================================================================
// THE PIPELINE IS A REBUILD, ITERATED TO A FIXED POINT
// ================================================================================================
//
// design.md §1.3. Every pass is a bottom-up rebuild through `Builder`, so a pass's output is in the
// same canonical form as its input by construction and the pipeline can be iterated until the
// module's digest stops moving. The 24-node reference material reaches its fixed point in three
// iterations, which the suite asserts rather than assumes.
//
// TYPE PROPAGATION IS NOT A PASS HERE. It is `Builder::make`, which types every node as it is
// constructed and refuses one it cannot type. A separate pass would be a second implementation of
// the same rules, and the two would disagree the first time an op was added.
//
// DEAD-NODE ELIMINATION IS NOT A PASS EITHER — it is a property of a rebuild that starts at the
// roots, which cannot reach an orphan. The spike found its own `pass_dce` switch to be a placebo
// for exactly this reason. So the switch here does the opposite of what a switch usually does: when
// it is OFF the pipeline explicitly carries the orphans across, which is the only way the flag can
// mean anything at all.
//
// ================================================================================================
// EIGHT OF NINE SWITCHES CHANGE THE COMPILED PROGRAM, AND THE DIAGNOSTIC HAS TO SAY SO
// ================================================================================================
//
// design.md §1.4 measured this on the spike and found eight of nine. This implementation measures
// it too — `material_passes: every switch is load-bearing` runs the whole table on every build —
// and also finds eight of nine, WITH A DIFFERENT ROW AS THE EXCEPTION, for a reason worth stating:
//
//   * the spike's neutral row was uniform/varying analysis. Here it is not neutral, because the
//     emitter ACTS on the analysis — it groups the hoistable values into their own block rather
//     than merely recording them (see emit.h).
//   * the neutral row here is dead-node elimination, because emission walks the canonical order
//     FROM THE ROOTS: an orphan cannot reach the generated source however many of them the module
//     carries. Switching it off changes the module — the node count, and the drop report an editor
//     greys nodes out with — and not the program. The spike's emitter walked the module instead.
//
// Either way the consequence for `material-compiler`'s "Each pass SHALL be individually disableable
// in development builds" is the same and it has to be said out loud: a bisection build is NOT the
// same material compiled more slowly. `OptimiseReport::bisection_build` is set whenever any switch
// is off, and `compile_material` turns it into a diagnostic.
//
// A SWITCH REACHES THE FRONT-END'S BUILDER TOO. Interning happens in `Builder::make`, which is
// where a front-end constructs its nodes — so a front-end that ignored these switches would merge
// the values a bisection is trying to keep apart before the pipeline ever ran, and four of the nine
// rows above would report "no change" for a pass that had simply already happened. `lower_graph`
// and `parse_material` therefore take the switches, and `builder_policy` is the one translation.

#include <cy/core/base/expected.h>
#include <cy/rendering/material/ir.h>

namespace cy::rendering::material {

/// The nine switches. All on is the shipping pipeline.
struct PassSwitches {
    /// Hash-consing in the builder: two structurally identical values become one.
    bool interning = true;
    /// Canonical ordering of a commutative operand list, by content hash.
    bool canonical_commutative = true;
    /// Constant folding, including the algebraic identities that folding exposes.
    bool constant_folding = true;
    /// Merging of identical interior expressions. Distinct from `interning`: with `interning` on
    /// and this off, leaves still merge and interior expressions do not, which is the state a
    /// bisection wants when it suspects an expression was merged that should not have been.
    bool common_subexpression = true;
    /// Merging of identical texture samples. `material-compiler`: "WHEN a graph samples one texture
    /// with identical coordinates in three places THEN the compiled program SHALL contain one
    /// sample."
    bool texture_sample_dedup = true;
    /// Closure algebra: a weight of one disappears, a weight of zero removes the closure, nested
    /// sums flatten. This is what makes an editor's weight port on every closure cost nothing.
    bool closure_simplification = true;
    /// Off carries orphans across explicitly. See the note above.
    bool dead_node_elimination = true;
    /// Marks the values that are constant across a draw, so the emitter can hoist them.
    bool uniform_varying = true;

    /// Emission order — canonical visit order rather than node id. Not a rebuild pass; it is read
    /// by `emit_program`, and it lives here so that the nine switches are one structure.
    bool canonical_emission_order = true;

    [[nodiscard]] bool all_enabled() const noexcept;
    /// Which switch is off, for the diagnostic. `nullptr` when they are all on.
    [[nodiscard]] const char* first_disabled() const noexcept;
};

/// What the pipeline did. Every count is a fact a cook report or an editor shows, and the two
/// `dropped` members are `material-compiler`'s "the editor SHALL be able to show which nodes were
/// dropped" — by AUTHORING node id, which is what the editor has.
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
    u32 simplified_closures = 0;
    /// Values that two or more source nodes collapsed into.
    u32 merged_values = 0;
    /// Texture samples removed by deduplication.
    u32 duplicate_samples_removed = 0;
    /// Values marked constant across a draw.
    u32 uniform_values = 0;
    /// True when any switch is off: the program this pipeline produced is NOT the shipping one.
    bool bisection_build = false;
};

/// The builder policy these switches imply.
///
/// ONE TRANSLATION, USED BY THE FRONT-ENDS AND BY THE PIPELINE. Interning happens in the builder,
/// so a front-end that ignored the switches would merge the values a bisection is trying to keep
/// apart before the pipeline ever ran.
[[nodiscard]] BuilderPolicy builder_policy(const PassSwitches& switches) noexcept;

/// Run the pipeline to a fixed point.
///
/// The result is a new module; `source` is untouched. `report` is appended to rather than cleared,
/// so a caller running the pipeline over a family of programs accumulates one report.
[[nodiscard]] Expected<Module, Error> optimise(const Module& source, const PassSwitches& switches,
                                               OptimiseReport& report) noexcept;

/// The largest number of iterations the fixed point is allowed. Reaching it is a defect in a pass —
/// a pipeline that does not converge is one whose passes undo each other — so it is reported as an
/// error rather than accepted quietly.
inline constexpr u32 kMaxOptimiseIterations = 8;

}  // namespace cy::rendering::material

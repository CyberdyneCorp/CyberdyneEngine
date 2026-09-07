#pragma once
// Material cost analysis, attributed back to the graph nodes that caused it. M7 task 6.3.
//
// `material-compiler` — "Material cost analysis": "The compiler SHALL produce, per material and per
// tier, a cost report: texture sample count, arithmetic instruction count, branch count, closure
// count, estimated register pressure, variant count, and an estimated full-screen cost for a named
// profile. Costs SHALL be attributed back to graph nodes, so an author sees which nodes are
// expensive rather than only that the material is expensive."
//
// ATTRIBUTION READS THE PROVENANCE SIDE TABLE AND NOTHING ELSE. That is what makes it survive
// optimisation: interning merges two authoring nodes into one value, so a value's cost belongs to a
// SET of authoring nodes rather than to one, and design.md §1.2 decision 4 is the reason the set
// exists. A value that no authoring node claims — one the compiler introduced, or one from a text
// definition that was never a graph — is attributed to `kUnattributed` rather than to whichever
// node happened to be nearby.
//
// THE FULL-SCREEN ESTIMATE IS A DECLARED MODEL, NOT A MEASUREMENT. Its constants are named in
// `CostModel` and shipped with a profile; nothing here times a GPU. An estimate that pretended to
// be a measurement is the failure mode this comment exists to prevent, and the cook report prints
// the model's name beside the number.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/material/emit.h>
#include <cy/rendering/material/lowering.h>

namespace cy::rendering::material {

/// The origin id used for values no authoring node claims.
inline constexpr u32 kUnattributed = 0xFFFFFFFFU;

/// What one authoring node cost, summed over every value it produced.
struct NodeCost {
    u32 origin = kUnattributed;
    u32 texture_samples = 0;
    u32 arithmetic = 0;
    u32 closures = 0;

    [[nodiscard]] u32 weight() const noexcept;
};

/// The declared cost model a full-screen estimate is computed from. Named so the report can say
/// which one produced the number.
struct CostModel {
    const char* name = "desktop-declared";
    /// Nanoseconds per full-screen texture sample and per full-screen arithmetic instruction, at
    /// the declared resolution. Declared values, not measurements.
    f32 sample_ns = 0.0026F;
    f32 arithmetic_ns = 0.00042F;
    f32 branch_ns = 0.0031F;
    u32 pixels = 1920U * 1080U;
};

/// One material's cost, for one program and one tier.
struct CostReport {
    explicit CostReport(Allocator& allocator) noexcept : per_node(allocator) {}

    CostReport(const CostReport&) = delete;
    CostReport& operator=(const CostReport&) = delete;
    CostReport(CostReport&&) noexcept = default;
    CostReport& operator=(CostReport&&) noexcept = default;

    /// Sorted by descending weight, so "the expensive node" is the first entry.
    Array<NodeCost> per_node;
    u32 texture_samples = 0;
    u32 arithmetic = 0;
    u32 branches = 0;
    u32 closures = 0;
    u32 statements = 0;
    /// SSA values live at once — the stand-in for register pressure, measured over the emitted
    /// order rather than guessed from the node count.
    u32 estimated_registers = 0;
    /// Tiers x static parameter values x geometry variants. The number
    /// `material-compiler`'s "three tiers and two static parameters with two values each THEN the
    /// reported permutation count SHALL be twelve" refers to.
    u32 permutation_count = 0;
    /// The program family — primary, secondary, far field, shadow — as its own axis.
    u32 program_count = 0;
    /// `permutation_count * program_count`: what a cook actually compiles.
    u32 total_programs = 0;
    /// Under a vertex-stage pipeline one variant per geometry source. Reported rather than hidden:
    /// "This limitation SHALL be documented, and the variant count SHALL be reported."
    u32 geometry_variants = 1;
    /// What the generic layered evaluator costs relative to a matched model, when one was used.
    f32 generic_cost_multiple = 1.0F;
    f32 full_screen_ms = 0.0F;
    const char* model_name = "";
};

/// Analyse one emitted program.
[[nodiscard]] Status analyse_cost(const Module& module, const GeneratedSource& source,
                                  const Lowered& lowered, const CostModel& model,
                                  CostReport& report) noexcept;

/// Fill in the permutation counts. Separate from `analyse_cost` because they are a property of the
/// material's whole family and of the profile, not of one emitted program.
void count_permutations(u32 static_bool_parameters, u32 program_count, const Profile& profile,
                        u32 geometry_sources, CostReport& report) noexcept;

}  // namespace cy::rendering::material

#pragma once
// Slang emission: SSA values numbered by canonical visit order. M7 tasks 6.1 and 6.4.
//
// `material-compiler` — "The compiler SHALL emit Slang and SHALL pass through the shader pipeline
// defined in `shader-system`. It SHALL NOT introduce a second shader toolchain, a second cache, or
// backend-specific source", and "the editor SHALL show the generated Slang, and that source SHALL
// be what is compiled — not a separate editor-only approximation".
//
// ================================================================================================
// ONE EMITTER, AND THAT IS WHAT TASK 6.4 IS ABOUT
// ================================================================================================
//
// A node preview is this function with `preview_root` set. There is no second code generator, no
// editor-only path and no approximation, which is the same rule `editor-viewport-and-gizmos` states
// for the viewport: a second renderer is a second answer.
//
// The relationship is checkable, and the suite checks it rather than asserting it:
//
//   * `preview(surface_root)` produces the primary program's body with the opacity assignment
//     removed — a PREFIX, statement for statement and byte for byte;
//   * `preview(any interior node)` produces a SUBSEQUENCE of the primary program's statements, in
//     the same order and with the same right-hand sides.
//
// Both hold because emission is a function of the canonical order over the sub-DAG rooted at the
// node, and the canonical order of a sub-DAG is the induced order of the whole.
//
// ================================================================================================
// WHAT IS A STATEMENT AND WHAT IS NOT
// ================================================================================================
//
// Constants, parameters, attributes and fields are emitted INLINE at their use sites; every
// computed value — arithmetic, a texture sample, a closure — gets an SSA statement. That is not
// cosmetic: it is what makes an opaque material's folded `opacity = 1` produce no statement at all,
// so "an opaque material produces no fragment work in the shadow program" is visible in the source
// rather than asserted about it (design.md §1.5).
//
// SSA VALUES ARE NUMBERED BY POSITION IN THE EMITTED ORDER, never by `NodeId`. Decision 6 of
// design.md §1.2: numbering by node id leaks construction order into the generated source, which
// the spike proved with an id-shifted module producing a different program.
//
// ================================================================================================
// SCOPE: NOTHING HERE INVOKES THE SLANG COMPILER
// ================================================================================================
//
// This module produces Slang TEXT, exactly as the spike did. The text names an interface the
// engine's shader standard library supplies — `CyMaterialContext`, `CyClosure`, `cy_closure_*` —
// and `shader-system` owns compiling it. Keeping the compiler's own suite free of a shader
// toolchain is deliberate: the material compiler's properties are properties of the text.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/material/ir.h>
#include <cy/rendering/material/passes.h>

#include <string_view>

namespace cy::rendering::material {

/// The family of programs one material compiles to. `material-compiler`, "Secondary material
/// programs": a consumer must not pay for camera-visible shading it does not need.
enum class ProgramKind : u8 {
    /// Camera-visible shading: the full graph.
    Primary = 0,
    /// Filling the surface cache and answering illumination queries.
    Secondary,
    /// Distant illumination: constants and averaged values.
    FarField,
    /// Shadow rasterisation: opacity and declared displacement only.
    Shadow,
    Count,
};

[[nodiscard]] const char* program_kind_name(ProgramKind kind) noexcept;

/// The quality tiers `material-compiler` requires: "a declared permutation axis of small, fixed
/// cardinality (default 3)".
enum class QualityTier : u8 { High = 0, Medium, Low, Count };

[[nodiscard]] const char* quality_tier_name(QualityTier tier) noexcept;
inline constexpr u32 kQualityTierCount = static_cast<u32>(QualityTier::Count);

struct EmitOptions {
    ProgramKind kind = ProgramKind::Primary;
    QualityTier tier = QualityTier::High;
    /// The lowered shading model, for the header comment and the evaluator call. It is part of the
    /// emitted text and therefore of the program digest, which is what makes "this material lowered
    /// to the generic evaluator" a cook-key difference rather than a note.
    const char* shading_model = "Lit";
    /// Emit the sub-DAG rooted here instead of the module's roots. This is the node preview.
    NodeId preview_root = kInvalidNode;
    /// Decision 6. Off numbers SSA values by node id, which is what makes the switch observable.
    bool canonical_order = true;
    /// Group the values that are constant across a draw into their own block, so they can be
    /// hoisted into parameter data. `material-compiler`: "a subexpression depends only on material
    /// parameters THEN it SHALL be evaluated once into parameter data rather than per pixel".
    bool hoist_uniform = true;
};

/// Generated source, and the identity a cook key is built from.
struct GeneratedSource {
    explicit GeneratedSource(Allocator& allocator) noexcept
        : text(allocator), value_nodes(allocator) {}

    GeneratedSource(const GeneratedSource&) = delete;
    GeneratedSource& operator=(const GeneratedSource&) = delete;
    GeneratedSource(GeneratedSource&&) noexcept = default;
    GeneratedSource& operator=(GeneratedSource&&) noexcept = default;

    Array<char> text;
    /// The IR node behind each SSA value, indexed by the number in its `vN` name. It is what lets a
    /// preview and a final program be compared statement by statement rather than by eye — see the
    /// preview suite — and it is what an editor's "which node is this line?" reads.
    Array<NodeId> value_nodes;
    /// A content hash over `text`. Two compilations that produce this digest produce byte-identical
    /// source, which is the exit criterion's "identical programs".
    u64 digest = 0;
    /// Where the function body begins and ends within `text`, so a preview can be compared against
    /// a final program without the differing header getting in the way.
    usize body_begin = 0;
    usize body_end = 0;
    u32 statements = 0;
    /// Statements emitted into the hoisted block.
    u32 hoisted_statements = 0;
    u32 texture_samples = 0;
    u32 arithmetic = 0;
    u32 branches = 0;
    u32 closures = 0;
    /// The most SSA values live at once, which is what stands in for register pressure. Measured
    /// over the emitted order rather than guessed from the node count.
    u32 peak_live_values = 0;

    [[nodiscard]] std::string_view view() const noexcept { return {text.data(), text.size()}; }
    [[nodiscard]] std::string_view body() const noexcept {
        return view().substr(body_begin, body_end - body_begin);
    }
};

/// Emit one program.
[[nodiscard]] Expected<GeneratedSource, Error> emit_program(const Module& module,
                                                            const EmitOptions& options) noexcept;

/// The generated entry point's name, as `emit_program` spells it. Exposed because the shader
/// pipeline and the material table both need to name it and two spellings would diverge.
[[nodiscard]] Status entry_point_name(Name material, ProgramKind kind, QualityTier tier,
                                      Array<char>& out) noexcept;

}  // namespace cy::rendering::material

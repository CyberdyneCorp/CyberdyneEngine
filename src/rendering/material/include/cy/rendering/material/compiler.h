#pragma once
// The material compiler: the join. M7 tasks 6.1, 6.2, 6.3 and 6.4.
//
// Graph or text -> IR -> optimisation -> closure lowering -> the program family and its tiers ->
// Slang -> the cost report -> the parameter layout the GPU material table is addressed by.
//
// ================================================================================================
// WHAT THIS FILE ADDS THAT THE LAYERS BENEATH IT DO NOT HAVE
// ================================================================================================
//
// 1. PARAMETER CLASSIFICATION, DERIVED FROM USE. `material-compiler`: "Classification SHALL be
//    derived from use, not declared by the author: a parameter is static only when it feeds a
//    control-flow or resource decision that cannot be expressed as data." So a parameter reaching a
//    `Select` condition is static and everything else is runtime — and an author who asked for
//    `static param roughness : float` is REFUSED with a diagnostic naming the parameter, because
//    the annotation would multiply programs for no structural difference.
//
// 2. THE FAMILY. Four programs (`Primary`, `Secondary`, `FarField`, `Shadow`) times three tiers,
//    derived automatically and each reported. The shadow program of an opaque material is ABSENT
//    rather than empty.
//
// 3. THE COOK KEY. One digest over the IR, the compiler version, the profile, the pass switches and
//    every emitted program — which is what `material-compiler`'s "Cook keys SHALL include the
//    material graph, the compiler version, the IR version, the target profile, the feature set, and
//    the quality tier" asks for, computed rather than assembled by a caller who might forget one.
//
// 4. THE PARAMETER LAYOUT. A `cy::rendering::MaterialProgram` — M3's type, unchanged — filled in
//    from the IR. That is the seam M3's material.h described and left open: "A material authored as
//    data at M3 fills that shape directly; a material compiled from a closure graph at M7 fills the
//    same shape, and nothing downstream changes."
//
// 5. NODE PREVIEWS, through `preview_node`, which is `emit_program` with a root. There is no second
//    code generator — see emit.h, and see the preview suite, which proves it rather than saying it.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/material/cost.h>
#include <cy/rendering/material/emit.h>
#include <cy/rendering/material/graph.h>
#include <cy/rendering/material/lowering.h>
#include <cy/rendering/material/material.h>
#include <cy/rendering/material/passes.h>
#include <cy/rendering/material/text.h>

namespace cy::rendering::material {

/// The compiler's own version. Part of every cook key: "WHEN the material compiler version
/// increases THEN compiled programs SHALL be recooked and the authored material assets SHALL be
/// untouched."
inline constexpr u32 kCompilerVersion = 1;

enum class DiagnosticSeverity : u8 { Info = 0, Warning = 1, Error = 2 };

[[nodiscard]] const char* diagnostic_severity_name(DiagnosticSeverity severity) noexcept;

/// One thing the compiler has to say. `code` is stable and groupable; `detail` is a sentence.
struct CompileDiagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Info;
    /// A stable identifier — "static-annotation-refused", "generic-evaluator-unsupported" — rather
    /// than a sentence somebody will reword.
    const char* code = "";
    const char* detail = "";
    /// The parameter, texture, field or closure the finding is about.
    Name subject;
    /// The AUTHORING node responsible, or `kUnattributed`. What an editor highlights.
    u32 origin = kUnattributed;
};

/// One compiled program of the family.
struct CompiledProgram {
    explicit CompiledProgram(Allocator& allocator) noexcept
        : module(allocator), source(allocator), cost(allocator) {}

    CompiledProgram(const CompiledProgram&) = delete;
    CompiledProgram& operator=(const CompiledProgram&) = delete;
    CompiledProgram(CompiledProgram&&) noexcept = default;
    CompiledProgram& operator=(CompiledProgram&&) noexcept = default;

    ProgramKind kind = ProgramKind::Primary;
    QualityTier tier = QualityTier::High;
    /// The derived, optimised module this program was emitted from. Kept because a node preview and
    /// the editor's "show me the IR" both need it, and re-deriving it would be a second answer.
    Module module;
    GeneratedSource source;
    CostReport cost;
    ShadingModel model = ShadingModel::Lit;
    bool generic_evaluator = false;
    /// The derivation has no roots: an opaque material's shadow program. It is REPORTED and not
    /// emitted, which is "Opaque materials SHALL produce no fragment work in the shadow program".
    bool absent = false;
    DerivationDifference difference;
};

struct CompileOptions {
    PassSwitches passes;
    Profile profile = desktop_profile();
    CostModel cost_model;
    /// Derive `Secondary`, `FarField` and `Shadow` beside `Primary`.
    bool derive_family = true;
    /// Emit all three quality tiers.
    bool derive_tiers = true;
    /// How many geometry sources the material is used with. Only matters under a vertex-stage
    /// pipeline, where it multiplies the variant count.
    u32 geometry_sources = 1;
    /// The environment fields the project declares. When `check_fields` is set, a material sampling
    /// a field outside this list fails to cook naming the field — "rather than silently
    /// substituting a default".
    Span<const Name> declared_fields;
    bool check_fields = false;
};

/// Everything one cook of one material produced.
class CompiledMaterial {
public:
    explicit CompiledMaterial(Allocator& allocator) noexcept;

    CompiledMaterial(const CompiledMaterial&) = delete;
    CompiledMaterial& operator=(const CompiledMaterial&) = delete;
    CompiledMaterial(CompiledMaterial&&) noexcept = default;
    CompiledMaterial& operator=(CompiledMaterial&&) noexcept = default;

    [[nodiscard]] Span<const CompiledProgram> programs() const noexcept { return programs_.span(); }
    [[nodiscard]] const CompiledProgram* find(ProgramKind kind, QualityTier tier) const noexcept;
    [[nodiscard]] Span<const CompileDiagnostic> diagnostics() const noexcept {
        return diagnostics_.span();
    }
    /// True when any diagnostic is an error. What a cook branches on.
    [[nodiscard]] bool failed() const noexcept;

    /// The parameter layout, ready for `MaterialTable`.
    [[nodiscard]] const MaterialProgram& layout() const noexcept { return layout_; }
    [[nodiscard]] const Module& primary() const noexcept;

    /// One digest over everything a cook of this material depends on.
    [[nodiscard]] u64 cook_key() const noexcept { return cook_key_; }
    [[nodiscard]] const OptimiseReport& optimisation() const noexcept { return optimisation_; }

private:
    friend Expected<CompiledMaterial, Error> compile_material(const Module&, const CompileOptions&,
                                                              Allocator&) noexcept;

    Array<CompiledProgram> programs_;
    Array<CompileDiagnostic> diagnostics_;
    MaterialProgram layout_;
    OptimiseReport optimisation_;
    u64 cook_key_ = 0;
};

/// Compile an authored module into its family of programs.
///
/// `authored` is what a front-end produced, un-optimised. The pipeline runs here, once, and every
/// derived program is derived from the OPTIMISED primary module — deriving from the authored one
/// would mean four modules that had each been through a different set of passes.
[[nodiscard]] Expected<CompiledMaterial, Error> compile_material(const Module& authored,
                                                                 const CompileOptions& options,
                                                                 Allocator& allocator) noexcept;

/// A node preview: the same emitter, rooted at `node`.
///
/// `material-compiler`: "Every graph node SHALL be previewable, and previews SHALL be generated
/// through the same compiler, lowering, and shader pipeline as runtime. There SHALL be no separate
/// editor-only shading path."
[[nodiscard]] Expected<GeneratedSource, Error> preview_node(const CompiledProgram& program,
                                                            NodeId node) noexcept;

/// The IR value an authoring node produced, or `kInvalidNode` when the optimiser removed it. What
/// an editor calls before `preview_node`, because it holds a graph node id and not an IR one.
[[nodiscard]] NodeId value_of_origin(const Module& module, u32 authoring_node) noexcept;

}  // namespace cy::rendering::material

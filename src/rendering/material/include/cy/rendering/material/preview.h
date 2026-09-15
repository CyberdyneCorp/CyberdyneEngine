#pragma once
// A NODE PREVIEW, THROUGH THE RUNTIME COMPILER AND THROUGH NOTHING ELSE. M11.c task 1.1.
//
// `material-compiler` — "Node previews use the real compiler": "Every graph node SHALL be
// previewable, and previews SHALL be generated through the same compiler, lowering, AND SHADER
// PIPELINE as runtime. There SHALL be no separate editor-only shading path."
//
// ================================================================================================
// WHAT WAS ALREADY TRUE, AND THE HALF OF THE REQUIREMENT THAT WAS NOT
// ================================================================================================
//
// `preview_node` is `emit_program` with a root, and `unit.material_compiler` proves the TEXT is the
// final program's own — byte for byte for the surface root, statement for statement for an interior
// node. That is the "same compiler, same lowering" half and it is genuinely proved.
//
// **The "same shader pipeline" half had nothing behind it.** Emitted text is not a preview; a
// preview is a picture, and a picture comes from a compiled program. Nothing in this tree ever took
// a preview's text to a shader compiler, so the requirement's own sentence stopped one word early,
// and the failure mode it names — an editor that draws previews some other way — would have been
// invisible to every case in the suite.
//
// ================================================================================================
// THE NEGATIVE CONTROL IS THE DESIGN, NOT A TEST WRITTEN AFTERWARDS
// ================================================================================================
//
// `material-compiler`'s new requirement asks for this in as many words: "with the compiler behind
// the preview disabled, the preview SHALL fail visibly and the test that asserts it SHALL go red".
//
// So `compile_preview` takes the `ShaderCompiler` as an ARGUMENT and refuses, first thing, when it
// cannot compile source — `ShaderCompiler::compiles_source()` is false for the SPIR-V passthrough
// and for `unavailable_compiler()`, which is exactly the "the compiler is gone" state a shipping
// build is in. THERE IS NO SECOND BRANCH. A preview is a compilation or it is an error naming the
// front end that could not be reached; there is nowhere in this file for a cached thumbnail, an
// approximation or a stand-in renderer to be returned from, which is what makes the prohibition
// structural rather than a promise.

#include <cy/backends/shader/compiler.h>
#include <cy/backends/shader/source.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/material/compiler.h>
#include <cy/rendering/material/slang_program.h>
#include <cy/rendering/material/stages.h>

namespace cy::rendering::material {

/// The entry point a preview translation unit exposes. The generated preview function is
/// `<entry>_preview`; this is the probe around it that a compilation has something to compile.
inline constexpr const char* kMaterialPreviewEntryPoint = "cyMaterialPreviewProbe";

struct PreviewOptions {
    PreludeOptions prelude;
    /// The module name the preview is published under. Distinct per material and per node, so two
    /// previews do not overwrite each other in the registry.
    Name module_prefix = Name::intern("material.preview");
};

/// What a preview compilation produced: the same three artefacts a shipping program produces.
class CompiledPreview {
public:
    explicit CompiledPreview(Allocator& allocator) noexcept
        : source(allocator), unit(allocator), shader(allocator) {}

    CompiledPreview(const CompiledPreview&) = delete;
    CompiledPreview& operator=(const CompiledPreview&) = delete;
    CompiledPreview(CompiledPreview&&) noexcept = default;
    CompiledPreview& operator=(CompiledPreview&&) noexcept = default;

    /// The IR value previewed.
    NodeId node = kInvalidNode;
    /// The emitter's own output, rooted at `node`. `preview_node`'s, unmodified.
    GeneratedSource source;
    /// prelude + that text + the preview probe, as one compilable unit.
    Array<char> unit;
    /// What `shader-system` made of it.
    shader::CompiledShader shader;
    PreludeReport prelude;

    [[nodiscard]] std::string_view text() const noexcept { return {unit.data(), unit.size()}; }
};

/// prelude + a preview program + a probe entry point, as ONE compilable Slang unit.
///
/// The emitter's text is copied byte for byte, exactly as `assemble_translation_unit` copies a
/// shipping program's — a preview that was rewritten on its way to the compiler would be a preview
/// of the rewrite.
[[nodiscard]] Expected<PreludeReport, Error> assemble_preview_unit(const Module& module,
                                                                   const GeneratedSource& generated,
                                                                   NodeId preview_root,
                                                                   ProgramKind kind,
                                                                   QualityTier tier,
                                                                   const PreludeOptions& options,
                                                                   Array<char>& out) noexcept;

/// Preview one value of a compiled program, through the runtime compiler and the shader pipeline.
///
/// Fails, naming the front end, when `compiler` cannot compile source. See the header: that refusal
/// IS the requirement's negative control, and there is no other path out of this function.
[[nodiscard]] Expected<CompiledPreview, Error> compile_preview(
    const CompiledProgram& program, NodeId node, const PreviewOptions& options,
    shader::ShaderCompiler& compiler, shader::SourceRegistry& sources,
    const shader::SourceResolver& resolver, shader::DiagnosticLog& diagnostics,
    Allocator& allocator) noexcept;

/// Fill `LoweringStage::CompiledProgram` — the fifth stage, which `cy::rendering-material` cannot
/// produce because it does not link the shader toolchain and must not.
///
/// The dump is what a person reads about a compiled module: the front end that produced it, the
/// entry point, its stage, the SPIR-V word and instruction counts, and the content hash the shader
/// library keys it by. Not a disassembly — `spirv-dis` is a tool, and inventing a second one here
/// would be this module writing a backend after all.
[[nodiscard]] Status attach_backend_stage(LoweringInspection& inspection, const Module& module,
                                          shader::ShaderCompiler& compiler,
                                          shader::SourceRegistry& sources,
                                          const shader::SourceResolver& resolver,
                                          shader::DiagnosticLog& diagnostics) noexcept;

}  // namespace cy::rendering::material

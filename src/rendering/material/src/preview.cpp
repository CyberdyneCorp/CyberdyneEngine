// A node preview, through the runtime compiler and through nothing else. M11.c task 1.1.

#include <cy/rendering/material/preview.h>

#include <cstdio>
#include <string_view>
#include <utility>

namespace cy::rendering::material {
namespace {

/// The same append-only writer the prelude uses. Kept local for the reason the prelude's is:
/// `emit.cpp` is M7's closed work and every byte it produces is part of a cook key.
class Writer {
public:
    explicit Writer(Array<char>& out) noexcept : out_(&out) {}

    void text(std::string_view value) noexcept {
        if (!status_) {
            return;
        }
        for (const char character : value) {
            if (Status pushed = out_->push_back(character); !pushed) {
                status_ = pushed;
                return;
            }
        }
    }

    void number(u32 value) noexcept {
        char digits[12] = {};
        u32 length = 0;
        do {
            digits[length++] = static_cast<char>('0' + (value % 10U));
            value /= 10U;
        } while (value != 0);
        while (length > 0) {
            text(std::string_view(&digits[--length], 1));
        }
    }

    [[nodiscard]] Status status() const noexcept { return status_; }

private:
    Array<char>* out_;
    Status status_ = ok();
};

/// A module name that is unique per material, per program and per previewed value.
///
/// Distinct names rather than one reused name, because `SourceRegistry::add_generated` REPLACES a
/// unit published under a name it already holds: previewing two nodes under one name would leave
/// the registry holding the second and a compilation of the first resolving to it.
[[nodiscard]] Status preview_module_name(const CompiledProgram& program, NodeId node,
                                         const PreviewOptions& options, Array<char>& out) noexcept {
    Writer writer(out);
    writer.text(options.module_prefix.text());
    writer.text(".");
    writer.text(program.module.name().text());
    writer.text(".");
    writer.text(program_kind_name(program.kind));
    writer.text(".");
    writer.text(quality_tier_name(program.tier));
    writer.text(".n");
    writer.number(node);
    return writer.status();
}

}  // namespace

Expected<PreludeReport, Error> assemble_preview_unit(const Module& module,
                                                     const GeneratedSource& generated,
                                                     NodeId preview_root, ProgramKind kind,
                                                     QualityTier tier,
                                                     const PreludeOptions& options,
                                                     Array<char>& out) noexcept {
    out.clear();
    Expected<PreludeReport, Error> report = emit_prelude(module, options, out);
    if (!report.has_value()) {
        return report;
    }

    Writer writer(out);
    // THE EMITTER'S TEXT, BYTE FOR BYTE — the same rule `assemble_translation_unit` follows. A
    // preview that was rewritten on its way to the compiler would be a preview of the rewrite.
    writer.text(std::string_view(generated.text.data(), generated.text.size()));
    writer.text("\n");

    Array<char> entry(module.allocator());
    if (Status named = entry_point_name(module.name(), kind, tier, entry); !named) {
        return make_unexpected(named.error());
    }

    // WHICH FIELD THE PREVIEW LANDED IN, decided the way the emitter decided it: `emit.cpp` writes
    // `surface.closures` for a closure-typed root and `surface.preview` for every other value. A
    // probe that read the wrong one would compile and report zero, which is the shape of a green
    // that means nothing.
    const bool closure = preview_root < module.size() &&
                         module.node(preview_root).type == ValueType::Closure;

    writer.text("[[vk::binding(1, ");
    writer.number(options.material_set);
    writer.text(")]]\nRWStructuredBuffer<float4> cyMaterialPreviewOutput;\n\n");
    writer.text("[shader(\"compute\")]\n[numthreads(1, 1, 1)]\nvoid ");
    writer.text(kMaterialPreviewEntryPoint);
    writer.text("(uint3 id: SV_DispatchThreadID)\n{\n");
    writer.text("    CyMaterialContext ctx;\n");
    writer.text("    ctx.params = cyMaterialParameters;\n");
    writer.text("    ctx.attributes = cyZeroAttributes();\n");
    writer.text("    CySurface previewed = cyDefaultSurface();\n    ");
    writer.text(std::string_view(entry.data(), entry.size()));
    writer.text("_preview(ctx, previewed);\n");
    if (closure) {
        writer.text("    let resolved = cyResolveSurface(previewed);\n");
        writer.text("    cyMaterialPreviewOutput[id.x] = float4(resolved.albedo, 1.0);\n}\n");
    } else {
        writer.text("    cyMaterialPreviewOutput[id.x] = float4(previewed.preview, 1.0);\n}\n");
    }

    if (!writer.status()) {
        return make_unexpected(writer.status().error());
    }
    return report;
}

Expected<CompiledPreview, Error> compile_preview(const CompiledProgram& program, NodeId node,
                                                 const PreviewOptions& options,
                                                 shader::ShaderCompiler& compiler,
                                                 shader::SourceRegistry& sources,
                                                 const shader::SourceResolver& resolver,
                                                 shader::DiagnosticLog& diagnostics,
                                                 Allocator& allocator) noexcept {
    // THE REFUSAL IS FIRST AND IT IS THE REQUIREMENT. `compiles_source()` is false for
    // `unavailable_compiler()` and for the SPIR-V passthrough — the two states a build is in when
    // the compiler behind a preview is gone — and there is nothing below this line that could
    // produce a preview anyway. See preview.h.
    if (!compiler.compiles_source()) {
        return make_unexpected(
            Error{ErrorCode::Unsupported,
                  "the shader front end in this build cannot compile source, so a node preview "
                  "cannot be produced: `material-compiler` forbids a second editor-only shading "
                  "path, so this is an error rather than a fallback",
                  0});
    }

    CompiledPreview preview(allocator);
    preview.node = node;

    auto emitted = preview_node(program, node);
    if (!emitted) {
        return make_unexpected(emitted.error());
    }
    preview.source = std::move(emitted.value());

    PreludeOptions prelude = options.prelude;
    auto report = assemble_preview_unit(program.module, preview.source, node, program.kind,
                                        program.tier, prelude, preview.unit);
    if (!report) {
        return make_unexpected(report.error());
    }
    preview.prelude = report.value();

    Array<char> module_name(allocator);
    if (Status named = preview_module_name(program, node, options, module_name); !named) {
        return make_unexpected(named.error());
    }
    auto published = sources.add_generated(
        Name::intern(std::string_view(module_name.data(), module_name.size())),
        Name::intern("material-compiler"),
        std::string_view(preview.unit.data(), preview.unit.size()));
    if (!published) {
        return make_unexpected(published.error());
    }

    shader::CompileRequest request;
    request.source = published.value();
    request.entry_point = Name::intern(kMaterialPreviewEntryPoint);
    request.stage = rhi::ShaderStage::Compute;
    request.resolver = resolver;

    auto compiled = compiler.compile(request, diagnostics);
    if (!compiled) {
        return make_unexpected(compiled.error());
    }
    if (diagnostics.has_errors()) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "the preview's generated Slang did not compile; the diagnostics say where",
                  0});
    }
    preview.shader = std::move(compiled.value());
    return preview;
}

Status attach_backend_stage(LoweringInspection& inspection, const Module& module,
                            shader::ShaderCompiler& compiler, shader::SourceRegistry& sources,
                            const shader::SourceResolver& resolver,
                            shader::DiagnosticLog& diagnostics) noexcept {
    StageDump& stage = inspection.stage(LoweringStage::CompiledProgram);
    if (!compiler.compiles_source()) {
        stage.available = false;
        stage.reason =
            "the shader front end in this build cannot compile source, so there is no compiled "
            "backend output to show — reported rather than left blank";
        return fail(ErrorCode::Unsupported,
                    "the shader front end in this build cannot compile source");
    }

    const StageDump& slang = inspection.stage(LoweringStage::GeneratedSlang);
    if (!slang.available) {
        return fail(ErrorCode::Unavailable,
                    "the generated Slang stage is not there, so there is nothing to compile: a "
                    "backend stage attached to an absent fourth stage would be a compilation of "
                    "something this inspection never saw");
    }

    Array<char> unit(module.allocator());
    GeneratedSource carrier(module.allocator());
    // The assembled unit is built from the SAME text stage four holds, which is the property that
    // makes the fifth stage a continuation of the fourth rather than a second compilation.
    for (const char character : slang.view()) {
        if (!carrier.text.push_back(character)) {
            return fail(ErrorCode::OutOfMemory, "the generated source could not be copied");
        }
    }
    PreludeOptions prelude;
    auto report = assemble_translation_unit(module, carrier, inspection.kind, inspection.tier,
                                            prelude, unit);
    if (!report) {
        return make_unexpected(report.error());
    }

    Array<char> module_name(module.allocator());
    {
        Writer writer(module_name);
        writer.text("material.stage.");
        writer.text(module.name().text());
        writer.text(".");
        writer.text(program_kind_name(inspection.kind));
        writer.text(".");
        writer.text(quality_tier_name(inspection.tier));
        if (!writer.status()) {
            return writer.status();
        }
    }
    auto published = sources.add_generated(
        Name::intern(std::string_view(module_name.data(), module_name.size())),
        Name::intern("material-compiler"), std::string_view(unit.data(), unit.size()));
    if (!published) {
        return make_unexpected(published.error());
    }

    shader::CompileRequest request;
    request.source = published.value();
    request.entry_point = Name::intern(kMaterialProbeEntryPoint);
    request.stage = rhi::ShaderStage::Compute;
    request.resolver = resolver;

    auto compiled = compiler.compile(request, diagnostics);
    if (!compiled) {
        return make_unexpected(compiled.error());
    }
    if (diagnostics.has_errors()) {
        return fail(ErrorCode::InvalidArgument,
                    "the material's generated Slang did not compile; the diagnostics say where");
    }

    Writer writer(stage.text);
    writer.text("backend ");
    writer.text(compiler.name());
    writer.text(" ");
    writer.text(compiler.version());
    writer.text("\n  entry ");
    writer.text(kMaterialProbeEntryPoint);
    writer.text("\n  words ");
    writer.number(compiled.value().stats().spirv_words);
    writer.text("\n  instructions ");
    writer.number(compiled.value().stats().instruction_count);
    writer.text("\n  hash ");
    {
        char text[assets::ContentHash::kTextLength + 1] = {};
        compiled.value().hash().format(text);
        writer.text(text);
    }
    writer.text("\n");
    if (!writer.status()) {
        return writer.status();
    }
    stage.available = true;
    stage.reason = "";
    stage.digest = hash_bytes(kHashSeed, stage.text.data(), stage.text.size());
    return ok();
}

}  // namespace cy::rendering::material

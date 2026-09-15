// A NODE PREVIEW, THROUGH THE RUNTIME COMPILER AND THE SHADER PIPELINE. M11.c task 1.1.
//
// `material-compiler` — "Node previews use the real compiler": "previews SHALL be generated through
// the same compiler, lowering, AND SHADER PIPELINE as runtime. There SHALL be no separate
// editor-only shading path." And the new requirement's own negative control, in its own words:
// "with the compiler behind the preview disabled, the preview SHALL fail visibly and the test that
// asserts it SHALL go red".
//
// ================================================================================================
// WHAT THIS SUITE OBSERVES THAT `unit.material_compiler` DOES NOT
// ================================================================================================
//
// `material_preview:` in the unit suite proves the preview's TEXT is the shipping program's own,
// byte for byte. That is real and it is half the requirement. THE OTHER HALF HAD NOTHING BEHIND IT:
// nothing in this tree ever took a preview's text to a shader compiler, so "the same shader
// pipeline as runtime" was a sentence about a path with no caller. A preview is a picture, and a
// picture comes from a compiled program.
//
// So each case here RUNS something and compares two answers:
//
//   1. the preview compiles to SPIR-V through `cy::shader`, against the engine's own standard
//      library, and the words it produced are the words a compilation produced rather than a
//      number this file chose;
//   2. with the compiler behind it GONE, the same call FAILS — and the case would go red if a
//      second path ever answered instead. The control is run with the SPIR-V passthrough, which is
//      not a stub: it is the front end a SHIPPING build has, which is precisely the state in which
//      an editor would be tempted to draw a preview some other way.
//
// WHY SMOKE AND NOT INTEGRATION. Creating a Slang global session loads its core module and costs
// about a second of CPU before anything is compiled at all, which is the whole of the integration
// budget. `smoke.material_slang` beside it is smoke for the same reason and says so.

#include <cy/backends/shader/compiler.h>
#include <cy/backends/shader/slang/slang_compiler.h>
#include <cy/backends/shader/source.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/memory/scope.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/material/preview.h>
#include <cy/test/test.h>

#include <cstdio>
#include <string_view>
#include <utility>

#include "fixtures.h"

using namespace cy;
using namespace cy::rendering::material;
using cy::rendering::material::testing::build_reference_graph;
using cy::rendering::material::testing::GraphIds;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

assets::VirtualPath path_of(const char* raw) {
    auto path = assets::VirtualPath::normalise(raw);
    CY_REQUIRE(path.has_value());
    return path.value();
}

/// The engine's own Slang, mounted the way a tool mounts it.
///
/// The doubled-name workaround is `smoke.material_slang`'s and is copied deliberately rather than
/// shared: it is a workaround for `RegistryFileSystem` implementing `ISlangFileSystem` rather than
/// `ISlangFileSystemExt`, a defect one layer down that `slang_program.h` names, and a helper shared
/// between two suites would make it look like a feature.
struct StandardLibrary {
    assets::VirtualFileSystem files;
    shader::SourceRegistry registry{current_allocator()};

    static bool resolve(void* user, std::string_view module_name,
                        shader::SourceUnit& out) noexcept {
        auto* self = static_cast<StandardLibrary*>(user);
        const shader::SourceResolver inner = self->registry.resolver();
        if (inner(module_name, out)) {
            return true;
        }
        std::string_view name = module_name;
        while (true) {
            const usize dot = name.find('.');
            if (dot == std::string_view::npos) {
                return false;
            }
            name = name.substr(dot + 1);
            if (inner(name, out)) {
                return true;
            }
        }
    }

    [[nodiscard]] shader::SourceResolver resolver() noexcept {
        shader::SourceResolver value;
        value.resolve = &StandardLibrary::resolve;
        value.user = this;
        return value;
    }

    StandardLibrary() {
        auto mount = assets::DirectoryMount::create(CY_SHADER_STAGE_PARENT,
                                                    assets::MountKind::Project, false);
        CY_REQUIRE(mount.has_value());
        CY_REQUIRE(files.mount_owned(std::move(mount.value()), 0).has_value());
        CY_REQUIRE(registry.start(files, path_of("shaders")).has_value());
        CY_REQUIRE(registry.load(Name::intern("cy.brdf")).has_value());
    }
};

struct Front {
    shader::ShaderCompiler* handle = nullptr;
    shader::CompilerSelection selection;

    explicit Front(const char* requested) {
        auto created = shader::create_compiler(current_allocator(), requested, selection);
        CY_REQUIRE(created.has_value());
        handle = created.value();
    }
    ~Front() { shader::destroy_compiler(current_allocator(), handle); }

    Front(const Front&) = delete;
    Front& operator=(const Front&) = delete;
};

/// The reference material, compiled to one primary program.
struct Material {
    explicit Material(Allocator& memory) noexcept : compiled(memory) {}

    CompiledMaterial compiled;
    GraphIds ids;

    void build() {
        MaterialGraph graph(allocator(), Name::intern("worn_metal"));
        CY_REQUIRE(build_reference_graph(graph, ids));
        auto authored = lower_graph(graph, allocator());
        CY_REQUIRE(authored.has_value());
        CompileOptions options;
        options.derive_family = false;
        options.derive_tiers = false;
        auto material = compile_material(authored.value(), options, allocator());
        CY_REQUIRE(material.has_value());
        compiled = std::move(material.value());
    }

    [[nodiscard]] const CompiledProgram& primary() const noexcept {
        const CompiledProgram* found = compiled.find(ProgramKind::Primary, QualityTier::High);
        CY_REQUIRE(found != nullptr);
        return *found;
    }
};

void print_diagnostics(const shader::DiagnosticLog& log) {
    for (usize index = 0; index < log.size(); ++index) {
        std::printf("slang: %s(%u): %s\n", log.at(index).location.file, log.at(index).location.line,
                    log.at(index).message);
    }
}

}  // namespace

CY_TEST_CASE("a node previews through the runtime compiler, and refuses when it is gone") {
    CY_REQUIRE(shader::slang::slang_available());
    Material material(allocator());
    material.build();
    const CompiledProgram& primary = material.primary();

    StandardLibrary library;
    PreviewOptions options;

    // --- 1. THE PREVIEW IS A COMPILED PROGRAM ---------------------------------------------------
    {
        Front slang(shader::kSlangBackendName);
        CY_REQUIRE(slang.handle->compiles_source());
        shader::DiagnosticLog diagnostics(allocator());

        const NodeId root = primary.module.surface();
        auto preview = compile_preview(primary, root, options, *slang.handle, library.registry,
                                       library.resolver(), diagnostics, allocator());
        if (!preview.has_value()) {
            print_diagnostics(diagnostics);
        }
        CY_REQUIRE(preview.has_value());
        CY_CHECK_GT(preview.value().shader.spirv().size(), usize{5});
        CY_CHECK_GT(preview.value().shader.stats().instruction_count, 0U);
        std::printf("surface-root preview compiled to %zu SPIR-V words\n",
                    preview.value().shader.spirv().size());

        // THE TEXT THAT WAS COMPILED IS THE EMITTER'S OWN, byte for byte. If a second generator
        // ever produced a preview, this is the character it would differ at.
        const std::string_view unit = preview.value().text();
        const std::string_view emitted(preview.value().source.text.data(),
                                       preview.value().source.text.size());
        CY_CHECK(unit.find(emitted) != std::string_view::npos);
        CY_CHECK(unit.find("cy_material_worn_metal_primary_high_preview(") !=
                 std::string_view::npos);
        CY_CHECK(unit.find(kMaterialPreviewEntryPoint) != std::string_view::npos);

        // AN INTERIOR VALUE PREVIEWS TOO, and it is a colour rather than a closure — so the probe
        // reads `surface.preview` and not the resolved surface. A probe that read the wrong field
        // would compile and report zero, which is a green that means nothing.
        const NodeId interior = value_of_origin(primary.module, material.ids.albedo);
        CY_REQUIRE_NE(interior, kInvalidNode);
        shader::DiagnosticLog second(allocator());
        auto inner = compile_preview(primary, interior, options, *slang.handle, library.registry,
                                     library.resolver(), second, allocator());
        if (!inner.has_value()) {
            print_diagnostics(second);
        }
        CY_REQUIRE(inner.has_value());
        CY_CHECK(inner.value().text().find("previewed.preview") != std::string_view::npos);
        // TWO DIFFERENT PREVIEWS ARE TWO DIFFERENT PROGRAMS. Equal hashes here would mean the node
        // argument reached nothing, which is what a preview path that ignores its root looks like.
        CY_CHECK_NE(preview.value().shader.hash(), inner.value().shader.hash());
    }

    // --- 2. THE NEGATIVE CONTROL ----------------------------------------------------------------
    //
    // The SPIR-V passthrough is the front end a SHIPPING build has — `shader-system` requires a
    // shipping build to contain no Slang compiler — so this is not a contrived state. The preview
    // must FAIL, and it must fail saying so.
    {
        Front passthrough(shader::kSpirvBackendName);
        CY_CHECK_FALSE(passthrough.handle->compiles_source());
        shader::DiagnosticLog diagnostics(allocator());
        auto preview =
            compile_preview(primary, primary.module.surface(), options, *passthrough.handle,
                            library.registry, library.resolver(), diagnostics, allocator());
        CY_REQUIRE_FALSE(preview.has_value());
        CY_CHECK_EQ(preview.error().code, ErrorCode::Unsupported);
        CY_CHECK(std::string_view(preview.error().message).find("cannot compile source") !=
                 std::string_view::npos);
    }
    {
        // And the other spelling of "the compiler is gone": the default one a build with
        // CY_SHADER_SLANG off is left holding.
        shader::DiagnosticLog diagnostics(allocator());
        auto preview = compile_preview(primary, primary.module.surface(), options,
                                       shader::unavailable_compiler(), library.registry,
                                       library.resolver(), diagnostics, allocator());
        CY_REQUIRE_FALSE(preview.has_value());
    }
}

CY_TEST_CASE("the fifth lowering stage is a compilation, and is absent until one happens") {
    // Task 1.3's other half. `inspect_lowering` cannot produce the compiled backend output —
    // `cy::rendering-material` does not link the shader toolchain and must not — so the stage comes
    // back UNAVAILABLE with a reason, and this is where it is filled in from a real compilation.
    CY_REQUIRE(shader::slang::slang_available());
    MaterialGraph graph(allocator(), Name::intern("worn_metal"));
    GraphIds ids;
    CY_REQUIRE(build_reference_graph(graph, ids));

    CompileOptions options;
    options.derive_family = false;
    options.derive_tiers = false;
    auto inspected =
        inspect_lowering(graph, options, ProgramKind::Primary, QualityTier::High, allocator());
    CY_REQUIRE(inspected.has_value());
    LoweringInspection& stages = inspected.value();
    CY_REQUIRE_FALSE(stages.complete());
    CY_CHECK_EQ(stages.available(), 4U);

    StandardLibrary library;
    shader::DiagnosticLog diagnostics(allocator());
    {
        Front slang(shader::kSlangBackendName);
        auto lowered = lower_graph(graph, allocator());
        CY_REQUIRE(lowered.has_value());
        auto compiled = compile_material(lowered.value(), options, allocator());
        CY_REQUIRE(compiled.has_value());
        const CompiledProgram* primary =
            compiled.value().find(ProgramKind::Primary, QualityTier::High);
        CY_REQUIRE(primary != nullptr);

        Status attached =
            attach_backend_stage(stages, primary->module, *slang.handle, library.registry,
                                 library.resolver(), diagnostics);
        if (!attached) {
            print_diagnostics(diagnostics);
        }
        CY_REQUIRE(attached.has_value());
    }
    CY_CHECK(stages.complete());
    CY_CHECK_EQ(stages.available(), 5U);
    const StageDump& backend = stages.stage(LoweringStage::CompiledProgram);
    CY_CHECK(backend.view().find("backend slang") != std::string_view::npos);
    CY_CHECK(backend.view().find("words ") != std::string_view::npos);
    CY_CHECK_NE(backend.digest, 0ULL);

    // AND IT REFUSES WHERE THERE IS NO COMPILER, rather than leaving a blank stage behind that a
    // panel would show as the fifth one.
    auto second =
        inspect_lowering(graph, options, ProgramKind::Primary, QualityTier::High, allocator());
    CY_REQUIRE(second.has_value());
    auto module = lower_graph(graph, allocator());
    CY_REQUIRE(module.has_value());
    Front passthrough(shader::kSpirvBackendName);
    shader::DiagnosticLog quiet(allocator());
    const Status refused =
        attach_backend_stage(second.value(), module.value(), *passthrough.handle, library.registry,
                             library.resolver(), quiet);
    CY_CHECK_FALSE(refused.has_value());
    CY_CHECK_FALSE(second.value().complete());
    CY_CHECK(std::string_view(second.value().stage(LoweringStage::CompiledProgram).reason)
                 .find("cannot compile source") != std::string_view::npos);
}

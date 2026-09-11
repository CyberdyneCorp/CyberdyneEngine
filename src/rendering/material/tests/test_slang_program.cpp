// A MATERIAL THE COMPILER EMITTED, COMPILED. M8.c task 1b.3.
//
// M7 recorded the gap and it survived M8.b: the bundle carries the IR, the generated source, the
// cost report and the cook key, and nothing invoked the compiler. This suite is what invokes it —
// it takes the SAME text `cy_material compile --slang` writes, wraps it in the generated prelude,
// and hands the result to `cy::shader`'s Slang front end with `src/rendering/shaders/` mounted as
// the standard library.
//
// It is SMOKE, for the reason `smoke.shader_slang` is: creating a Slang global session loads its
// core module and costs about a second before anything is compiled at all. It is declared only when
// `CY_SHADER_SLANG` is on, because that is the only configuration in which the front end exists —
// `shader-system` requires a shipping build to contain no Slang compiler.
//
// THE CONTROL IS THE THIRD CASE, and it is the one that would have caught this in M7: the emitter's
// output WITHOUT the prelude is compiled too, and it must FAIL naming an undefined identifier. A
// suite that only compiled the assembled unit would pass just as happily on a day when the prelude
// had quietly become unnecessary — which is the day this whole file could be deleted, and a test
// should say which day that is.

#include <cy/backends/shader/compiler.h>
#include <cy/backends/shader/slang/slang_compiler.h>
#include <cy/backends/shader/source.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/memory/scope.h>
#include <cy/rendering/material/slang_program.h>
#include <cy/rendering/material/text.h>
#include <cy/test/test.h>

#include <cstdio>
#include <string_view>
#include <utility>

using namespace cy;
using namespace cy::rendering::material;

namespace {

/// The material `tools/material/README.md` uses in its own example: a texture, three parameters, an
/// attribute, two closures and a sum. Every kind of leaf the prelude has to declare.
constexpr std::string_view kWornMetal = R"(
material worn_metal {
    param base_color : float3 = (0.82, 0.78, 0.74);
    param metallic   : float  = 1.0;
    param roughness  : float  = 0.35;
    texture base_color_map average (0.5, 0.5, 0.5, 1.0);
    attribute uv0 : float2;
    let albedo = sample(base_color_map, uv0).xyz * base_color;
    surface = diffuse(albedo * (1 - metallic)) + specular(albedo, roughness);
    opacity = 1.0;
}
)";

assets::VirtualPath path_of(const char* raw) {
    auto path = assets::VirtualPath::normalise(raw);
    CY_REQUIRE(path.has_value());
    return path.value();
}

/// The engine's own Slang mounted at `shaders/`, exactly as a tool or the sample mounts it: the
/// staged tree `src/rendering/shaders/` produces, reached by the virtual paths a shipped build
/// uses.
struct StandardLibrary {
    assets::VirtualFileSystem files;
    shader::SourceRegistry registry{current_allocator()};

    /// A resolver that also answers a name the front end DOUBLED, and it is a workaround for a
    /// defect one layer down rather than a convenience.
    ///
    /// MEASURED, on this tree: compiling a generated module whose `import cy.material` resolves to
    /// an authored module which itself imports `cy.brdf`, the front end asks this resolver for
    ///
    ///     resolve: 'cy.material'
    ///     resolve: 'cy.cy.brdf'      <-- the importing module's directory, combined again
    ///     resolve: 'cy.cy.light'
    ///
    /// `src/backends/shader/slang/`'s `RegistryFileSystem` implements `ISlangFileSystem` and not
    /// `ISlangFileSystemExt`, so it supplies no `calcCombinedPath` and Slang combines the importing
    /// file's directory with the imported name itself. A NESTED AUTHORED IMPORT HAS NEVER BEEN
    /// COMPILED THROUGH THE ENGINE'S OWN FRONT END: `smoke.shader_slang` compiles one generated
    /// module importing one authored module, one level deep, and every module of the standard
    /// library imports at least one other. The fix is `calcCombinedPath` in the front end, which is
    /// below this layer and not this milestone's to change; this strips the duplicated segment so
    /// the case can measure what it is about.
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
        // The staged tree is really there and really readable, checked before a compile failure can
        // be mistaken for a missing mount.
        CY_REQUIRE(registry.load(Name::intern("cy.brdf")).has_value());
    }
};

struct SlangHandle {
    shader::ShaderCompiler* handle = nullptr;
    shader::CompilerSelection selection;

    SlangHandle() {
        auto created =
            shader::create_compiler(current_allocator(), shader::kSlangBackendName, selection);
        CY_REQUIRE(created.has_value());
        handle = created.value();
    }
    ~SlangHandle() { shader::destroy_compiler(current_allocator(), handle); }

    SlangHandle(const SlangHandle&) = delete;
    SlangHandle& operator=(const SlangHandle&) = delete;
};

void print_diagnostics(const shader::DiagnosticLog& log) {
    for (usize index = 0; index < log.size(); ++index) {
        std::printf("slang: %s(%u): %s\n", log.at(index).location.file, log.at(index).location.line,
                    log.at(index).message);
    }
}

/// Compile one Slang unit as the probe entry point, and say whether it succeeded.
[[nodiscard]] bool compiles(StandardLibrary& library, SlangHandle& slang, const char* module_name,
                            std::string_view text, shader::DiagnosticLog& diagnostics,
                            u32& out_words) {
    auto generated = library.registry.add_generated(Name::intern(module_name),
                                                    Name::intern("material-compiler"), text);
    CY_REQUIRE(generated.has_value());

    shader::CompileRequest request;
    request.source = *generated;
    request.entry_point = Name::intern(kMaterialProbeEntryPoint);
    request.stage = rhi::ShaderStage::Compute;
    request.resolver = library.resolver();

    auto compiled = slang.handle->compile(request, diagnostics);
    if (!compiled.has_value()) {
        return false;
    }
    out_words = static_cast<u32>(compiled->spirv().size());
    return !diagnostics.has_errors();
}

/// Compile the material and return its primary/high program.
struct Compiled {
    explicit Compiled(Allocator& allocator) noexcept : unit(allocator) {}

    Array<char> unit;
    PreludeReport prelude;
    u64 emitted_digest = 0;
    usize emitted_bytes = 0;
};

void build(Compiled& out) {
    ParseDiagnostic sink(current_allocator());
    Expected<Module, Error> module = parse_material(kWornMetal, current_allocator(), sink);
    CY_REQUIRE(module.has_value());

    EmitOptions emit;
    emit.kind = ProgramKind::Primary;
    emit.tier = QualityTier::High;
    Expected<GeneratedSource, Error> source = emit_program(*module, emit);
    CY_REQUIRE(source.has_value());
    out.emitted_digest = source->digest;
    out.emitted_bytes = source->text.size();

    PreludeOptions prelude;
    Expected<PreludeReport, Error> report = assemble_translation_unit(
        *module, *source, ProgramKind::Primary, QualityTier::High, prelude, out.unit);
    CY_REQUIRE(report.has_value());
    out.prelude = *report;
}

}  // namespace

CY_TEST_CASE("the prelude declares exactly what the emitted program refers to and does not") {
    Compiled compiled(current_allocator());
    build(compiled);

    // Three parameters, one attribute, one texture, no fields — read off the module, and the
    // numbers a reader can check against the material at the top of this file.
    CY_CHECK_EQ(compiled.prelude.parameters, 3U);
    CY_CHECK_EQ(compiled.prelude.attributes, 1U);
    CY_CHECK_EQ(compiled.prelude.textures, 1U);
    CY_CHECK_EQ(compiled.prelude.fields, 0U);

    const std::string_view unit(compiled.unit.data(), compiled.unit.size());
    CY_CHECK(unit.find("struct CyMaterialParams") != std::string_view::npos);
    CY_CHECK(unit.find("struct CyMaterialAttributes") != std::string_view::npos);
    CY_CHECK(unit.find("struct CyMaterialContext") != std::string_view::npos);
    CY_CHECK(unit.find("static const uint CY_TEXTURE_base_color_map = 0;") !=
             std::string_view::npos);
    CY_CHECK(unit.find("float2 uv0;") != std::string_view::npos);

    // THE EMITTER'S OUTPUT IS COPIED BYTE FOR BYTE, which is what keeps this step outside M7's cook
    // key. If the prelude rewrote so much as a space of it, this would fail.
    CY_CHECK(unit.find("void cy_material_worn_metal_primary_high(in CyMaterialContext ctx, inout "
                       "CySurface surface) {") != std::string_view::npos);
    CY_CHECK(unit.find("CyClosure v5 = cy_closure_diffuse(v4);") != std::string_view::npos);
    CY_CHECK_GT(compiled.emitted_bytes, usize{0});
}

CY_TEST_CASE("the assembled unit compiles to SPIR-V against the engine's standard library") {
    CY_REQUIRE(shader::slang::slang_available());
    StandardLibrary library;
    SlangHandle slang;
    Compiled compiled(current_allocator());
    build(compiled);

    shader::DiagnosticLog diagnostics(current_allocator());
    u32 words = 0;
    const bool ok =
        compiles(library, slang, "material.worn_metal",
                 std::string_view(compiled.unit.data(), compiled.unit.size()), diagnostics, words);
    if (!ok) {
        print_diagnostics(diagnostics);
    }
    CY_REQUIRE(ok);
    CY_CHECK_GT(words, 5U);
    std::printf("worn_metal primary/high compiled to %u SPIR-V words\n", words);
}

CY_TEST_CASE("a material with no textures, no attributes and no parameters still compiles") {
    // THE DEGENERATE SHAPE, and it is a real one: an unlit constant material is what a placeholder,
    // a debug surface and a far-field program all lower to. Every conditional in `emit_prelude` is
    // off here — no texture array, no field array, no sampler accessor and an empty attribute
    // struct — which is exactly the combination that produces an empty `struct` and an empty
    // constant buffer if the trailers are not there.
    CY_REQUIRE(shader::slang::slang_available());
    StandardLibrary library;
    SlangHandle slang;

    constexpr std::string_view kFlat = R"(
material flat_grey {
    surface = diffuse((0.5, 0.5, 0.5));
    opacity = 1.0;
}
)";
    ParseDiagnostic sink(current_allocator());
    Expected<Module, Error> module = parse_material(kFlat, current_allocator(), sink);
    CY_REQUIRE(module.has_value());
    Expected<GeneratedSource, Error> source = emit_program(*module, EmitOptions{});
    CY_REQUIRE(source.has_value());

    Array<char> unit(current_allocator());
    Expected<PreludeReport, Error> report = assemble_translation_unit(
        *module, *source, ProgramKind::Primary, QualityTier::High, PreludeOptions{}, unit);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report->parameters, 0U);
    CY_CHECK_EQ(report->attributes, 0U);
    CY_CHECK_EQ(report->textures, 0U);
    CY_CHECK_EQ(report->fields, 0U);

    shader::DiagnosticLog diagnostics(current_allocator());
    u32 words = 0;
    const bool ok = compiles(library, slang, "material.flat_grey",
                             std::string_view(unit.data(), unit.size()), diagnostics, words);
    if (!ok) {
        print_diagnostics(diagnostics);
    }
    CY_REQUIRE(ok);
    CY_CHECK_GT(words, 5U);
}

CY_TEST_CASE("the emitter's output WITHOUT the prelude does not compile, and names why") {
    // THE CONTROL, and the case that would have caught M7's gap. Everything the emitter writes
    // beyond `import cy.material;` names something no file declares.
    CY_REQUIRE(shader::slang::slang_available());
    StandardLibrary library;
    SlangHandle slang;

    ParseDiagnostic sink(current_allocator());
    Expected<Module, Error> module = parse_material(kWornMetal, current_allocator(), sink);
    CY_REQUIRE(module.has_value());
    EmitOptions emit;
    Expected<GeneratedSource, Error> source = emit_program(*module, emit);
    CY_REQUIRE(source.has_value());

    shader::DiagnosticLog diagnostics(current_allocator());
    u32 words = 0;
    const bool ok =
        compiles(library, slang, "material.worn_metal_bare",
                 std::string_view(source->text.data(), source->text.size()), diagnostics, words);
    CY_CHECK_FALSE(ok);
    CY_CHECK(diagnostics.has_errors());
    print_diagnostics(diagnostics);
}

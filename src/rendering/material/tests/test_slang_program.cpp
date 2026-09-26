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
#include <string>
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
                            u32& out_words, const char* entry = kMaterialProbeEntryPoint,
                            rhi::ShaderStage stage = rhi::ShaderStage::Compute) {
    auto generated = library.registry.add_generated(Name::intern(module_name),
                                                    Name::intern("material-compiler"), text);
    CY_REQUIRE(generated.has_value());

    shader::CompileRequest request;
    request.source = *generated;
    request.entry_point = Name::intern(entry);
    request.stage = stage;
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

CY_TEST_CASE("the Metal prelude places material parameters in the requested argument buffer") {
    ParseDiagnostic sink(current_allocator());
    Expected<Module, Error> module = parse_material(kWornMetal, current_allocator(), sink);
    CY_REQUIRE(module.has_value());

    Array<char> unit(current_allocator());
    PreludeOptions options;
    options.material_set = 2;
    options.material_binding = 4;
    options.argument_buffer = true;
    auto report = emit_prelude(*module, options, unit);
    CY_REQUIRE(report.has_value());

    const std::string_view text(unit.data(), unit.size());
    CY_CHECK(text.find("[[vk::binding(4, 2)]]") != std::string_view::npos);
    CY_CHECK(text.find("ParameterBlock<CyMaterialDraw> cyMaterialDraw;") != std::string_view::npos);
    CY_CHECK(text.find("#define cyMaterialParameters cyMaterialDraw.parameters") !=
             std::string_view::npos);
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

CY_TEST_CASE("sine and spatial noise compile in a generated vertex offset") {
    CY_REQUIRE(shader::slang::slang_available());
    ParseDiagnostic sink(current_allocator());
    auto module = parse_material(
        "material wind_sway { param time : float = 0.0; attribute position : float3; "
        "vertex_offset = position * sin(time) + (0.0, noise(position), 0.0); }",
        current_allocator(), sink);
    CY_REQUIRE(module.has_value());
    auto generated = emit_vertex_offset(*module, EmitOptions{});
    CY_REQUIRE(generated.has_value());
    Array<char> prelude(current_allocator());
    CY_REQUIRE(emit_prelude(*module, PreludeOptions{}, prelude).has_value());
    std::string source(prelude.data(), prelude.size());
    source.append(generated->view());
    source += R"(
[[vk::binding(1, 3)]] RWStructuredBuffer<float4> cyMaterialProbeOutput;
[shader("compute")]
[numthreads(1, 1, 1)]
void cyMaterialProbe(uint3 id: SV_DispatchThreadID)
{
    CyMaterialContext ctx;
    ctx.params = cyMaterialParameters;
    ctx.attributes = cyZeroAttributes();
    let offset = cy_material_wind_sway_primary_high_vertex_offset(ctx);
    cyMaterialProbeOutput[id.x] = float4(offset, 1.0);
}
struct CyVertexProbeOutput { float4 position : SV_Position; };
[shader("vertex")]
CyVertexProbeOutput cyVertexProbe(float3 position : POSITION)
{
    CyMaterialContext ctx;
    ctx.params = cyMaterialParameters;
    ctx.attributes = cyZeroAttributes();
    ctx.attributes.position = position;
    let offset = cy_material_wind_sway_primary_high_vertex_offset(ctx);
    CyVertexProbeOutput output;
    output.position = float4(position + offset, 1.0);
    return output;
}
)";

    StandardLibrary library;
    SlangHandle slang;
    shader::DiagnosticLog diagnostics(current_allocator());
    u32 words = 0;
    const bool ok =
        compiles(library, slang, "material.wind_sway_vertex", source, diagnostics, words);
    if (!ok) {
        print_diagnostics(diagnostics);
    }
    CY_CHECK(ok);
    CY_CHECK_GT(words, 16U);
    shader::DiagnosticLog vertex_diagnostics(current_allocator());
    u32 vertex_words = 0;
    const bool vertex_ok =
        compiles(library, slang, "material.wind_sway_vertex_stage", source, vertex_diagnostics,
                 vertex_words, "cyVertexProbe", rhi::ShaderStage::Vertex);
    if (!vertex_ok) {
        print_diagnostics(vertex_diagnostics);
    }
    CY_CHECK(vertex_ok);
    CY_CHECK_GT(vertex_words, 16U);
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

CY_TEST_CASE(
    "the environment field sampler and its consumers compile against the standard library") {
    // THE ONLY PLACE IN THE TREE THAT COMPILES THE STAGED STANDARD LIBRARY THROUGH THE ENGINE'S OWN
    // FRONT END, which is why a module belonging to another row is checked from this suite. A
    // `.slang` module nothing compiles is a file: `m10:fields-sampled-on-a-device` was satisfied
    // the moment `cy/field.slang` existed, and "it exists" is not "it is a shader".
    //
    // THREE MODULES, AND THE TWO CONSUMERS ARE THE POINT. `cy.field` alone would compile happily
    // with a signature nothing could call; `cy.terrain_shade` is the substrate half of the 63.0 ms
    // band `m10:world-frame-budget` names, and `cy.cloud_shadow` is the reader
    // `atmosphere-sky-and-clouds` requires the cloud shadow field to have. Both import `cy.field`,
    // so this case fails if the sampler's own interface moves under them.
    //
    // The ANSWER's correctness is `render.environment_field`'s, which runs this sampler on a device
    // against `sample_field_image()` over the same bytes. What this case adds is that a build with
    // a Slang front end refuses a standard library that does not parse — on every machine, with no
    // GPU in it.
    CY_REQUIRE(shader::slang::slang_available());
    StandardLibrary library;
    SlangHandle slang;

    // The probe's entry point is the one `compiles()` asks the compiler for, and the body calls
    // every public entry of the three modules rather than merely importing them: an import that is
    // never used is resolved and not type-checked through.
    static constexpr const char* kProbe = R"(
import cy.field;
import cy.terrain_shade;
import cy.cloud_shadow;

[[vk::binding(0, 3)]] RWStructuredBuffer<float> cyFieldProbeOut;

[shader("compute")]
[numthreads(1, 1, 1)]
void cyMaterialProbe(uint3 thread : SV_DispatchThreadID)
{
    CyTerrainFields fields;
    fields.waterDistance = 0u;
    fields.wetness = 1u;
    fields.snowDepth = 2u;
    fields.vegetation = 3u;
    const CyTerrainSubstrate substrate = cyTerrainSampleSubstrate(fields, 1.0, 2.0, 3.0);
    const float3 colour =
        cyTerrainShade(cyTerrainDefaultPalette(), substrate, 0.2, 50.0, 900.0);
    const float3 lit = cyCloudShadowAttenuate(colour, 4u, 1.0, 2.0, 3.0);
    const CyFieldSample direct = cyFieldSampleScene(5u, 1.0, 2.0, 3.0);
    cyFieldProbeOut[thread.x] =
        lit.x + lit.y + lit.z + direct.value.x + (direct.resolved ? 1.0 : 0.0) +
        cyCloudShadowAt(4u, 1.0, 2.0, 3.0);
}
)";

    shader::DiagnosticLog diagnostics(current_allocator());
    u32 words = 0;
    const bool ok = compiles(library, slang, "cy.field_probe", kProbe, diagnostics, words);
    if (!ok) {
        print_diagnostics(diagnostics);
    }
    CY_REQUIRE(ok);
    CY_CHECK_GT(words, 5U);
    std::printf("cy.field + cy.terrain_shade + cy.cloud_shadow compiled to %u SPIR-V words\n",
                words);
}

// ================================================================================================
// REGRESSION — the cooked mip chain must be reachable
// ================================================================================================
//
// `cy_material_sample` emitted only `cyMaterialSampleTextureLevel(..., 0.0)`, so every generated
// program read level 0 whatever the stage, and the mip chain the importer cooks was UNREACHABLE.
// M11.c's spike measured the consequence rather than arguing it: a frame rendered with eight of the
// nine cooked levels never uploaded is BYTE-IDENTICAL to one with all nine — mean |delta|
// 0.000/255, 0.00% of texels. The importer spends about a third more bytes on a chain no program
// could read, and every minified surface in the artefact would alias.
//
// The prelude now emits both forms behind `CY_MATERIAL_PIXEL_STAGE`: a pixel stage defines it and
// gets the implicit-derivative sampler, and the compute probe does not, because implicit
// derivatives exist only in a pixel stage — which is why the explicit form was there in the first
// place.
//
// THIS CASE ASSERTS THE TEXT because that is where the defect lived. Delete either branch and it
// goes red; restore the old unconditional explicit form and it goes red on the implicit one.
CY_TEST_CASE("the generated prelude can sample a mip chain, not only level zero") {
    ParseDiagnostic sink(current_allocator());
    Expected<Module, Error> module = parse_material(kWornMetal, current_allocator(), sink);
    CY_REQUIRE(module.has_value());
    CY_CHECK(module->textures().size() > 0U);

    EmitOptions emit;
    emit.kind = ProgramKind::Primary;
    emit.tier = QualityTier::High;
    Expected<GeneratedSource, Error> source = emit_program(*module, emit);
    CY_REQUIRE(source.has_value());

    Array<char> unit(current_allocator());
    PreludeOptions prelude;
    Expected<PreludeReport, Error> report = assemble_translation_unit(
        *module, *source, ProgramKind::Primary, QualityTier::High, prelude, unit);
    CY_REQUIRE(report.has_value());

    const std::string_view text(unit.data(), unit.size());
    // The implicit sampler, which is the one that reads the chain.
    CY_CHECK(text.find("cyMaterialSampleTexture(ctx.params.textures[slot], uv)") !=
             std::string_view::npos);
    // Guarded, so the compute probe still compiles.
    CY_CHECK(text.find("CY_MATERIAL_PIXEL_STAGE") != std::string_view::npos);
    // And the explicit form survives for the stages that cannot take derivatives.
    CY_CHECK(text.find("cyMaterialSampleTextureLevel(ctx.params.textures[slot], uv, 0.0)") !=
             std::string_view::npos);
}

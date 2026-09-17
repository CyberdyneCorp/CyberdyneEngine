// The Slang front end, end to end. Task 3.1.
//
// Built only when CY_SHADER_SLANG is on, because that is the only configuration in which the front
// end exists at all — `shader-system` requires a shipping build to contain no Slang compiler, and
// the target is excluded from the link rather than stubbed out.
//
// WHAT THIS PROVES THAT THE REST OF THE SUITE CANNOT. Everything else runs through the SPIR-V
// passthrough and therefore tests the pipeline around a compiler. These cases test the compiler
// seam itself: that Slang source becomes SPIR-V, that an `import` resolves through
// cy::shader::SourceRegistry rather than through the operating system — so a *generated* module is
// importable by exactly the same syntax as an authored one — and that a compilation failure comes
// back as a diagnostic with a file and a line rather than as a bare error code.

#include <cy/backends/shader/compiler.h>
#include <cy/backends/shader/slang/slang_compiler.h>
#include <cy/backends/shader/source.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

#include <cstdio>
#include <string_view>
#include <utility>

using cy::usize;
using namespace cy::shader;

namespace {

constexpr std::string_view kHelperModule = R"(module helper;

public float3 tinted(float3 value)
{
    return value * float3(0.25, 0.5, 0.75);
}
)";

constexpr std::string_view kKernelSource = R"(import cy.helper;

[[vk::binding(0, 2)]] RWStructuredBuffer<float4> output;

[SpecializationConstant]
const int kIterations = 3;

[shader("compute")]
[numthreads(16, 2, 1)]
void kernelMain(uint3 id: SV_DispatchThreadID)
{
    float3 total = float3(0.0);
    for (int index = 0; index < kIterations; ++index)
    {
        total += tinted(float3(id) * float(index));
    }
    output[id.x] = float4(total, 1.0);
}
)";

constexpr std::string_view kBrokenSource = R"([shader("compute")]
[numthreads(1, 1, 1)]
void kernelMain(uint3 id: SV_DispatchThreadID)
{
    thisIdentifierDoesNotExist(id);
}
)";

cy::assets::VirtualPath path_of(const char* raw) {
    auto path = cy::assets::VirtualPath::normalise(raw);
    CY_REQUIRE(path.has_value());
    return path.value();
}

struct Fixture {
    cy::assets::VirtualFileSystem files;
    cy::assets::MemoryMount* memory = nullptr;
    SourceRegistry registry{cy::current_allocator()};

    Fixture() {
        auto mount = cy::make_unique<cy::assets::MemoryMount>(cy::current_allocator(), "memory");
        CY_REQUIRE(mount.has_value());
        memory = mount.value().get();
        CY_REQUIRE(files.mount_owned(std::move(mount.value()), 0).has_value());
        CY_REQUIRE(registry.start(files, path_of("shaders")).has_value());
        CY_REQUIRE(memory
                       ->add(path_of("shaders/cy/helper.slang"), kHelperModule.data(),
                             kHelperModule.size())
                       .has_value());
    }
};

struct SlangCompilerHandle {
    ShaderCompiler* handle = nullptr;
    CompilerSelection selection;

    SlangCompilerHandle() {
        auto created = create_compiler(cy::current_allocator(), kSlangBackendName, selection);
        CY_REQUIRE(created.has_value());
        handle = created.value();
    }
    ~SlangCompilerHandle() { destroy_compiler(cy::current_allocator(), handle); }

    SlangCompilerHandle(const SlangCompilerHandle&) = delete;
    SlangCompilerHandle& operator=(const SlangCompilerHandle&) = delete;
};

}  // namespace

CY_TEST_CASE("the Slang front end registers itself and reports its own version") {
    CY_REQUIRE(slang::slang_available());
    CY_REQUIRE(find_compiler(kSlangBackendName) != nullptr);

    SlangCompilerHandle compiler;
    CY_CHECK_FALSE(compiler.selection.fell_back);
    CY_CHECK(compiler.handle->compiles_source());
    // The version goes straight into the cache key, and being wrong there means serving a stale
    // binary after a compiler upgrade. It is asked of the loaded library rather than taken from a
    // header that may not be the one that built it.
    CY_CHECK(std::string_view(compiler.handle->version()).size() > 0);
    CY_CHECK(std::string_view(compiler.handle->version()) != "unknown");
}

CY_TEST_CASE("a generated module importing an authored one compiles to reflectable SPIR-V") {
    Fixture fixture;
    SlangCompilerHandle compiler;

    // THE SEAM, EXERCISED: the kernel is *generated* source — the shape M7's material compiler
    // produces — and its `import cy.helper` resolves to a file, through the registry's resolver,
    // with no path on disk for the kernel itself.
    auto generated = fixture.registry.add_generated(
        cy::Name::intern("material.kernel"), cy::Name::intern("test-generator"), kKernelSource);
    CY_REQUIRE(generated.has_value());

    CompileRequest request;
    request.source = *generated;
    request.entry_point = cy::Name::intern("kernelMain");
    request.stage = cy::rhi::ShaderStage::Compute;
    request.resolver = fixture.registry.resolver();

    DiagnosticLog diagnostics(cy::current_allocator());
    auto compiled = compiler.handle->compile(request, diagnostics);
    if (!compiled) {
        for (usize index = 0; index < diagnostics.size(); ++index) {
            std::printf("slang: %s(%u): %s\n", diagnostics.at(index).location.file,
                        diagnostics.at(index).location.line, diagnostics.at(index).message);
        }
    }
    CY_REQUIRE(compiled.has_value());
    CY_CHECK_FALSE(diagnostics.has_errors());

    // A real SPIR-V module: the magic number, and reflection read out of it rather than out of
    // Slang's own representation.
    CY_REQUIRE(compiled->spirv().size() > 5);
    CY_CHECK_EQ(compiled->spirv()[0], 0x07230203U);
    CY_CHECK(compiled->stats().compile_ns > 0);
    CY_CHECK(std::string_view(compiled->stats().backend) == kSlangBackendName);

    const Reflection& reflection = compiled->reflection();
    CY_REQUIRE_EQ(reflection.entry_points().size(), usize{1});
    CY_CHECK(reflection.entry_points()[0].stage == cy::rhi::ShaderStage::Compute);
    CY_CHECK_EQ(reflection.entry_points()[0].workgroup_size[0], 16U);
    CY_CHECK_EQ(reflection.entry_points()[0].workgroup_size[1], 2U);
    CY_CHECK_EQ(reflection.entry_points()[0].workgroup_size[2], 1U);

    CY_REQUIRE_EQ(reflection.bindings().size(), usize{1});
    CY_CHECK_EQ(reflection.bindings()[0].set, kSetPass);
    CY_CHECK(reflection.bindings()[0].kind == cy::rhi::DescriptorKind::StorageBuffer);
    CY_REQUIRE_EQ(reflection.spec_constants().size(), usize{1});
    CY_CHECK_EQ(reflection.spec_constants()[0].default_value, 3U);

    // The convention holds, checked against the compiler's own output rather than against a
    // hand-written module.
    DiagnosticLog convention(cy::current_allocator());
    CY_CHECK(validate_set_convention(reflection, convention, "material.kernel"));
}

CY_TEST_CASE("a compile error comes back with a file and a line") {
    Fixture fixture;
    SlangCompilerHandle compiler;

    auto broken = fixture.registry.add_generated(cy::Name::intern("material.broken"),
                                                 cy::Name::intern("test-generator"), kBrokenSource);
    CY_REQUIRE(broken.has_value());

    CompileRequest request;
    request.source = *broken;
    request.entry_point = cy::Name::intern("kernelMain");
    request.stage = cy::rhi::ShaderStage::Compute;
    request.resolver = fixture.registry.resolver();

    DiagnosticLog diagnostics(cy::current_allocator());
    CY_CHECK_FALSE(compiler.handle->compile(request, diagnostics).has_value());
    CY_REQUIRE(diagnostics.has_errors());

    // `shader-system`: the error carries the source file and the line, which is what puts a
    // squiggle in an editor rather than a paragraph in a console.
    bool located = false;
    for (usize index = 0; index < diagnostics.size(); ++index) {
        const Diagnostic entry = diagnostics.at(index);
        if (entry.severity == Severity::Error && entry.location.line != 0) {
            located = true;
        }
    }
    CY_CHECK(located);
}

CY_TEST_CASE("an entry point that is not there is reported, not guessed at") {
    Fixture fixture;
    SlangCompilerHandle compiler;

    auto generated = fixture.registry.add_generated(
        cy::Name::intern("material.kernel"), cy::Name::intern("test-generator"), kKernelSource);
    CY_REQUIRE(generated.has_value());

    CompileRequest request;
    request.source = *generated;
    request.entry_point = cy::Name::intern("noSuchEntryPoint");
    request.stage = cy::rhi::ShaderStage::Compute;
    request.resolver = fixture.registry.resolver();

    DiagnosticLog diagnostics(cy::current_allocator());
    CY_CHECK_FALSE(compiler.handle->compile(request, diagnostics).has_value());
    CY_CHECK(diagnostics.has_errors());
}

// --- Pipeline step 4: the three targets ---------------------------------------------------------
//
// M11.c tasks 1.5-1.7. These are the cases behind `m11c:shader-targets-for-the-next-rung` and
// `m11d:shader-targets-emitted`, and they are written so that the cheap ways of appearing to
// deliver a second target all fail them:
//
//   * emitting the first target's bytes under the second's name    -> `bytes_match_target`
//   * emitting an empty or truncated file                          -> `bytes_match_target`
//   * emitting something for one entry point and reflecting another -> the agreement cases
//   * dropping a binding on the target nobody looks at             -> the agreement cases
//
// THE MSL AND DXIL CASES RUN ONLY WHERE THE TOOLCHAIN EMITS THEM, and say so out loud when it does
// not. DXIL is produced by a compiler Slang loads at compile time: a machine configured with
// CY_SHADER_DXIL off, or one where that library is not beside libslang, genuinely cannot emit it,
// and a case that passed there would be a case that never failed anywhere.

CY_TEST_CASE("the front end emits SPIR-V, and says which other targets this build can emit") {
    SlangCompilerHandle compiler;

    // SPIR-V is not optional: it is the interchange form, and a Slang front end that could not emit
    // it would have failed every case above.
    CY_CHECK(compiler.handle->emits(Target::SpirV));

    // Reported rather than required, so the record says what this machine could do.
    std::printf("targets: %s=%d %s=%d %s=%d\n", target_name(Target::SpirV),
                static_cast<int>(compiler.handle->emits(Target::SpirV)), target_name(Target::Msl),
                static_cast<int>(compiler.handle->emits(Target::Msl)), target_name(Target::Dxil),
                static_cast<int>(compiler.handle->emits(Target::Dxil)));

    // The names are the cache key's `target_platform` strings, and a key that could not tell two
    // targets apart would serve one target's artefact to the other.
    CY_CHECK_EQ(std::string_view(target_name(Target::SpirV)), std::string_view("vulkan-spirv"));
    CY_CHECK_EQ(std::string_view(target_name(Target::Msl)), std::string_view("metal-msl"));
    CY_CHECK_EQ(std::string_view(target_name(Target::Dxil)), std::string_view("d3d12-dxil"));

    Target parsed = Target::SpirV;
    CY_CHECK(parse_target("msl", parsed));
    CY_CHECK(parsed == Target::Msl);
    CY_CHECK(parse_target("d3d12-dxil", parsed));
    CY_CHECK(parsed == Target::Dxil);
    CY_CHECK_FALSE(parse_target("metallib", parsed));
}

CY_TEST_CASE("the same graph compiled for two targets declares the same interface") {
    Fixture fixture;
    SlangCompilerHandle compiler;

    auto generated = fixture.registry.add_generated(
        cy::Name::intern("material.kernel"), cy::Name::intern("test-generator"), kKernelSource);
    CY_REQUIRE(generated.has_value());

    CompileRequest request;
    request.source = *generated;
    request.entry_point = cy::Name::intern("kernelMain");
    request.stage = cy::rhi::ShaderStage::Compute;
    request.resolver = fixture.registry.resolver();

    DiagnosticLog diagnostics(cy::current_allocator());
    auto spirv = compiler.handle->compile_for(request, Target::SpirV, diagnostics);
    CY_REQUIRE(spirv.has_value());
    CY_CHECK(bytes_match_target(Target::SpirV, spirv->bytes()));
    CY_CHECK_EQ(spirv->entry_point(), cy::Name::intern("kernelMain"));
    // The module declares one binding and one specialization constant, and the layout reports both
    // — which is why this looks for the binding by name rather than asserting a count.
    bool found_output = false;
    for (const TargetParameter& parameter : spirv->parameters()) {
        if (parameter.name == cy::Name::intern("output")) {
            found_output = true;
            CY_CHECK(parameter.kind == cy::rhi::DescriptorKind::StorageBuffer);
        }
    }
    CY_CHECK(found_output);
    // [numthreads(16, 2, 1)] on the entry point above, read back out of the program layout.
    CY_CHECK_EQ(spirv->thread_group()[0], 16U);
    CY_CHECK_EQ(spirv->thread_group()[1], 2U);

    usize compared = 0;
    for (const Target target : {Target::Msl, Target::Dxil}) {
        if (!compiler.handle->emits(target)) {
            std::printf("  %s: this build does not emit it; not compared\n", target_name(target));
            continue;
        }
        DiagnosticLog second(cy::current_allocator());
        auto other = compiler.handle->compile_for(request, target, second);
        CY_REQUIRE(other.has_value());

        // IN THE FORM ITS TARGET NAMES. The first thing a stub gets wrong.
        CY_CHECK(bytes_match_target(target, other->bytes()));
        // AND NOT THE SPIR-V UNDER A DIFFERENT NAME. The second thing a stub gets wrong.
        CY_CHECK(other->hash() != spirv->hash());
        CY_CHECK_FALSE(bytes_match_target(Target::SpirV, other->bytes()));

        // AND IT DECLARES THE SAME INTERFACE: the same entry point, stage, workgroup and
        // parameters. Where each target PUT them differs and is deliberately not compared.
        DiagnosticLog differences(cy::current_allocator());
        const bool agree = interfaces_agree(*spirv, *other, differences);
        for (usize index = 0; index < differences.size(); ++index) {
            std::printf("  %s\n", differences.at(index).message);
        }
        CY_CHECK(agree);
        ++compared;
    }
    std::printf("compared %zu target(s) against SPIR-V\n", compared);
}

CY_TEST_CASE("an artefact that is not in its target's form is not an artefact") {
    // The check the front end applies to its own output, applied here to the three things a stub
    // hands back: the wrong target's bytes, a plausible-looking prefix, and nothing at all.
    const cy::u8 spirv_magic[] = {0x03, 0x02, 0x23, 0x07, 0x00, 0x00, 0x01, 0x00};
    CY_CHECK(bytes_match_target(Target::SpirV, cy::Span<const cy::u8>(spirv_magic, 8)));
    CY_CHECK_FALSE(bytes_match_target(Target::Msl, cy::Span<const cy::u8>(spirv_magic, 8)));
    CY_CHECK_FALSE(bytes_match_target(Target::Dxil, cy::Span<const cy::u8>(spirv_magic, 8)));

    // "DXBC" alone is a D3D11 bytecode container; a DXIL one carries a DXIL chunk as well.
    const auto* header = reinterpret_cast<const cy::u8*>("DXBC and nothing else");
    CY_CHECK_FALSE(bytes_match_target(Target::Dxil, cy::Span<const cy::u8>(header, 21)));

    const auto* metal = reinterpret_cast<const cy::u8*>(
        "#include <metal_stdlib>\nusing namespace metal;\nkernel void k() {}\n");
    CY_CHECK(bytes_match_target(Target::Msl, cy::Span<const cy::u8>(metal, 66)));
    CY_CHECK_FALSE(bytes_match_target(Target::Msl, cy::Span<const cy::u8>(metal, 4)));

    const cy::Span<const cy::u8> nothing;
    CY_CHECK_FALSE(bytes_match_target(Target::SpirV, nothing));
    CY_CHECK_FALSE(bytes_match_target(Target::Msl, nothing));
    CY_CHECK_FALSE(bytes_match_target(Target::Dxil, nothing));
}

CY_TEST_CASE("the passthrough front end says it emits nothing rather than pretending") {
    CompilerSelection selection;
    auto created = create_compiler(cy::current_allocator(), kSpirvBackendName, selection);
    CY_REQUIRE(created.has_value());
    ShaderCompiler* passthrough = created.value();

    CY_CHECK_FALSE(passthrough->emits(Target::SpirV));
    CY_CHECK_FALSE(passthrough->emits(Target::Msl));
    CY_CHECK_FALSE(passthrough->emits(Target::Dxil));

    CompileRequest request;
    DiagnosticLog diagnostics(cy::current_allocator());
    auto refused = passthrough->compile_for(request, Target::Msl, diagnostics);
    CY_CHECK_FALSE(refused.has_value());
    CY_CHECK(diagnostics.has_errors());

    destroy_compiler(cy::current_allocator(), passthrough);
}

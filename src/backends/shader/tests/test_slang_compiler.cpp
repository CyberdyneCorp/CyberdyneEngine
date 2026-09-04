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

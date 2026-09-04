// Shader hot reload, and the front-end registry it runs through. Tasks 3.1 and 3.5.
//
// `shader-system`'s two hot-reload scenarios are the two cases below that matter most:
//
//   "Live shader iteration"       an edit becomes a replaced library entry with no restart.
//   "Broken shader does not       a failed rebuild calls replace() zero times, so the old artefact
//    break the frame"             is still there and the pipeline built from it is still valid.
//
// NEITHER NEEDS A GPU OR A SHADER COMPILER. The front end here is the SPIR-V passthrough, which is
// not a stub: it is the shipping path, and it is what lets continuous integration exercise
// reflection, the library and hot reload on a machine with no toolchain. The "source" a test writes
// is therefore a compiled module, and that is the point — the pipeline does not care which front
// end produced the bytes.

#include <cy/backends/shader/compiler.h>
#include <cy/backends/shader/hot_reload.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

#include "fixtures/spirv_probe.h"

#include <cstring>
#include <utility>
#include <vector>

using cy::u32;
using cy::u8;
using cy::usize;
using namespace cy::shader;

namespace {

constexpr cy::i64 kSettleNs = 100'000'000;

cy::assets::VirtualPath path_of(const char* raw) {
    auto path = cy::assets::VirtualPath::normalise(raw);
    CY_REQUIRE(path.has_value());
    return path.value();
}

struct Fixture {
    cy::assets::VirtualFileSystem files;
    cy::assets::MemoryMount* memory = nullptr;

    Fixture() {
        auto mount = cy::make_unique<cy::assets::MemoryMount>(cy::current_allocator(), "memory");
        CY_REQUIRE(mount.has_value());
        memory = mount.value().get();
        CY_REQUIRE(files.mount_owned(std::move(mount.value()), 0).has_value());
    }

    void write_module(const char* raw, cy::Span<const u32> words) const {
        CY_REQUIRE(memory->add(path_of(raw), words.data(), words.size() * sizeof(u32)).has_value());
    }
    void write_rubbish(const char* raw) const {
        CY_REQUIRE(memory->add(path_of(raw), "not a shader", 12).has_value());
    }
};

cy::Span<const u32> vertex_words() {
    return {test::kProbeVertexSpirv, std::size(test::kProbeVertexSpirv)};
}
cy::Span<const u32> fragment_words() {
    return {test::kProbeFragmentSpirv, std::size(test::kProbeFragmentSpirv)};
}

struct Compiler {
    ShaderCompiler* handle = nullptr;
    CompilerSelection selection;

    Compiler() {
        auto created = create_compiler(cy::current_allocator(), kSpirvBackendName, selection);
        CY_REQUIRE(created.has_value());
        handle = created.value();
    }
    ~Compiler() { destroy_compiler(cy::current_allocator(), handle); }

    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;
};

/// Compile one module through the passthrough and put it in the library under `key`.
ShaderVariantId seed(ShaderCompiler& compiler, SourceRegistry& registry, ShaderLibrary& library,
                     const char* module_name, cy::rhi::ShaderStage stage) {
    auto source = registry.load(cy::Name::intern(module_name));
    CY_REQUIRE(source.has_value());

    CompileRequest request;
    request.source = *source;
    request.entry_point = cy::Name::intern("main");
    request.stage = stage;
    request.resolver = registry.resolver();

    DiagnosticLog diagnostics(cy::current_allocator());
    auto compiled = compiler.compile(request, diagnostics);
    CY_REQUIRE(compiled.has_value());

    VariantKey key;
    key.module_name = cy::Name::intern(module_name);
    key.entry_point = cy::Name::intern("main");
    key.stage = stage;
    auto id = library.insert(key, std::move(compiled.value()));
    CY_REQUIRE(id.has_value());
    return *id;
}

}  // namespace

CY_TEST_CASE("the SPIR-V passthrough is always registered and is the fallback") {
    CompilerSelection selection;
    auto compiler = create_compiler(cy::current_allocator(), nullptr, selection);
    CY_REQUIRE(compiler.has_value());
    CY_CHECK(std::strcmp(selection.selected, "") != 0);
    destroy_compiler(cy::current_allocator(), compiler.value());

    // Asking for a front end this build does not have falls back and says why, rather than failing.
    auto absent = create_compiler(cy::current_allocator(), "there-is-no-such-compiler", selection);
    CY_REQUIRE(absent.has_value());
    CY_CHECK(selection.fell_back);
    CY_CHECK(std::strcmp(selection.selected, kSpirvBackendName) == 0);
    CY_CHECK(selection.reason[0] != '\0');
    destroy_compiler(cy::current_allocator(), absent.value());

    CY_REQUIRE(find_compiler(kSpirvBackendName) != nullptr);
    CY_CHECK_FALSE(find_compiler(kSpirvBackendName)->create == nullptr);
}

CY_TEST_CASE("the passthrough reflects what it is handed and refuses what it is not") {
    Compiler compiler;
    CY_CHECK_FALSE(compiler.handle->compiles_source());

    SourceRegistry registry(cy::current_allocator());
    CY_REQUIRE(
        registry
            .add_generated(cy::Name::intern("probe"), cy::Name::intern("test"),
                           std::string_view(reinterpret_cast<const char*>(test::kProbeComputeSpirv),
                                            std::size(test::kProbeComputeSpirv) * sizeof(u32)))
            .has_value());

    SourceUnit unit;
    CY_REQUIRE(registry.find(cy::Name::intern("probe"), unit));

    CompileRequest request;
    request.source = unit;
    request.entry_point = cy::Name::intern("main");
    DiagnosticLog diagnostics(cy::current_allocator());
    auto compiled = compiler.handle->compile(request, diagnostics);
    CY_REQUIRE(compiled.has_value());
    // The stage was not stated in the request; the module said so.
    CY_CHECK(compiled->stage() == cy::rhi::ShaderStage::Compute);
    CY_CHECK_EQ(compiled->reflection().entry_points()[0].workgroup_size[0], 8U);
    CY_CHECK(compiled->stats().compile_ns > 0);
    CY_CHECK_FALSE(compiled->hash().is_zero());

    SourceUnit rubbish;
    rubbish.module_name = cy::Name::intern("rubbish");
    rubbish.text = cy::Span<const char>("not a shader", 12);
    CompileRequest bad = request;
    bad.source = rubbish;
    DiagnosticLog complaints(cy::current_allocator());
    CY_CHECK_FALSE(compiler.handle->compile(bad, complaints).has_value());
    CY_CHECK(complaints.has_errors());
}

CY_TEST_CASE("an edit becomes a replaced library entry, with the id unchanged") {
    Fixture fixture;
    fixture.write_module("shaders/probe.slang", vertex_words());

    SourceRegistry registry(cy::current_allocator());
    CY_REQUIRE(registry.start(fixture.files, path_of("shaders")).has_value());

    Compiler compiler;
    ShaderLibrary library(cy::current_allocator());
    const ShaderVariantId id =
        seed(*compiler.handle, registry, library, "probe", cy::rhi::ShaderStage::Vertex);
    const auto before = library.shader_at(id)->hash();

    ShaderHotReload reload(cy::current_allocator());
    CY_REQUIRE(reload.start(registry, fixture.files, HotReloadConfig{}).has_value());
    CY_REQUIRE(reload.prime(0).has_value());

    fixture.write_module("shaders/probe.slang", fragment_words());

    // The poll that finds the change starts the settle period; it does not report. That is what
    // stops a half-written file being compiled.
    auto first = reload.poll(kSettleNs);
    CY_REQUIRE(first.has_value());
    CY_CHECK_EQ(*first, 0U);

    auto second = reload.poll(kSettleNs * 3);
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(*second, 1U);
    CY_CHECK(reload.is_pending(cy::Name::intern("probe")));

    DiagnosticLog diagnostics(cy::current_allocator());
    auto result = reload.rebuild(*compiler.handle, library, RebuildOptions{}, diagnostics);
    CY_REQUIRE(result.has_value());
    CY_CHECK_EQ(result->modules_rebuilt, 1U);
    CY_CHECK_EQ(result->variants_replaced, 1U);
    CY_CHECK_EQ(result->failures, 0U);

    // The consumer's id still resolves; it resolves to new code, and the generation says so.
    CY_CHECK_EQ(library.generation(id), 1U);
    CY_CHECK(library.shader_at(id)->hash() != before);
    CY_CHECK(reload.pending().empty());
}

CY_TEST_CASE("a broken edit keeps the previous artefact and reports the error") {
    Fixture fixture;
    fixture.write_module("shaders/probe.slang", vertex_words());

    SourceRegistry registry(cy::current_allocator());
    CY_REQUIRE(registry.start(fixture.files, path_of("shaders")).has_value());

    Compiler compiler;
    ShaderLibrary library(cy::current_allocator());
    const ShaderVariantId id =
        seed(*compiler.handle, registry, library, "probe", cy::rhi::ShaderStage::Vertex);
    const auto before = library.shader_at(id)->hash();

    ShaderHotReload reload(cy::current_allocator());
    CY_REQUIRE(reload.start(registry, fixture.files, HotReloadConfig{}).has_value());
    CY_REQUIRE(reload.prime(0).has_value());

    fixture.write_rubbish("shaders/probe.slang");
    CY_REQUIRE(reload.poll(kSettleNs).has_value());
    auto reported = reload.poll(kSettleNs * 3);
    CY_REQUIRE(reported.has_value());
    CY_CHECK_EQ(*reported, 1U);

    DiagnosticLog diagnostics(cy::current_allocator());
    auto result = reload.rebuild(*compiler.handle, library, RebuildOptions{}, diagnostics);
    CY_REQUIRE(result.has_value());
    CY_CHECK_EQ(result->failures, 1U);
    CY_CHECK_EQ(result->variants_replaced, 0U);
    CY_CHECK(diagnostics.has_errors());

    // The frame is still rendering: same artefact, same generation.
    CY_CHECK_EQ(library.generation(id), 0U);
    CY_CHECK(library.shader_at(id)->hash() == before);
    // And it left the queue, so the failure is reported once per edit rather than once per frame.
    CY_CHECK(reload.pending().empty());
    CY_CHECK_EQ(reload.stats().rebuild_failures, cy::u64{1});
}

CY_TEST_CASE("a rebuild invalidates the pipelines that named the old program") {
    Fixture fixture;
    fixture.write_module("shaders/probe.slang", vertex_words());

    SourceRegistry registry(cy::current_allocator());
    CY_REQUIRE(registry.start(fixture.files, path_of("shaders")).has_value());

    Compiler compiler;
    ShaderLibrary library(cy::current_allocator());
    const ShaderVariantId id =
        seed(*compiler.handle, registry, library, "probe", cy::rhi::ShaderStage::Vertex);
    const cy::assets::ContentHash program = library.shader_at(id)->hash();

    struct Builder {
        static cy::Expected<PipelineObject, cy::Error> build(
            void*, const PipelineStateKey&, cy::Span<const cy::assets::ContentHash>) noexcept {
            PipelineObject object;
            object.graphics = cy::rhi::GraphicsPipelineHandle::from_slot(1, 1);
            return object;
        }
    };

    PipelineStateCache pipelines(cy::current_allocator());
    pipelines.set_builder(&Builder::build, nullptr);
    PipelineStateInputs inputs;
    inputs.programs = {&program, 1};
    const PipelineStateKey key = derive_pipeline_state_key(inputs);
    (void)pipelines.request(key, {&program, 1});
    CY_REQUIRE(pipelines.build_pending().has_value());
    CY_CHECK(pipelines.status_of(key) == PipelineStatus::Ready);

    ShaderHotReload reload(cy::current_allocator());
    CY_REQUIRE(reload.start(registry, fixture.files, HotReloadConfig{}).has_value());
    CY_REQUIRE(reload.prime(0).has_value());
    fixture.write_module("shaders/probe.slang", fragment_words());
    CY_REQUIRE(reload.poll(kSettleNs).has_value());
    CY_REQUIRE(reload.poll(kSettleNs * 3).has_value());

    RebuildOptions options;
    options.pipelines = &pipelines;
    DiagnosticLog diagnostics(cy::current_allocator());
    auto result = reload.rebuild(*compiler.handle, library, options, diagnostics);
    CY_REQUIRE(result.has_value());
    CY_CHECK_EQ(result->pipelines_invalidated, 1U);
    // The state is rebuilt on next use rather than left pointing at code that no longer exists.
    CY_CHECK(pipelines.status_of(key) == PipelineStatus::Missing);
}

CY_TEST_CASE("a generated module joins the same rebuild path by hand") {
    // A generated module has no file for the watcher to see. `invalidate` is how a generator
    // re-enters the queue — the same queue, not one of its own.
    Fixture fixture;
    SourceRegistry registry(cy::current_allocator());
    CY_REQUIRE(registry.start(fixture.files, path_of("shaders")).has_value());
    CY_REQUIRE(
        registry
            .add_generated(cy::Name::intern("material.4f3a"), cy::Name::intern("gen"),
                           std::string_view(reinterpret_cast<const char*>(test::kProbeVertexSpirv),
                                            std::size(test::kProbeVertexSpirv) * sizeof(u32)))
            .has_value());

    Compiler compiler;
    ShaderLibrary library(cy::current_allocator());
    const ShaderVariantId id =
        seed(*compiler.handle, registry, library, "material.4f3a", cy::rhi::ShaderStage::Vertex);

    ShaderHotReload reload(cy::current_allocator());
    CY_REQUIRE(reload.start(registry, fixture.files, HotReloadConfig{}).has_value());
    CY_REQUIRE(registry
                   .add_generated(
                       cy::Name::intern("material.4f3a"), cy::Name::intern("gen"),
                       std::string_view(reinterpret_cast<const char*>(test::kProbeFragmentSpirv),
                                        std::size(test::kProbeFragmentSpirv) * sizeof(u32)))
                   .has_value());
    CY_REQUIRE(reload.invalidate(cy::Name::intern("material.4f3a")).has_value());

    DiagnosticLog diagnostics(cy::current_allocator());
    auto result = reload.rebuild(*compiler.handle, library, RebuildOptions{}, diagnostics);
    CY_REQUIRE(result.has_value());
    CY_CHECK_EQ(result->variants_replaced, 1U);
    CY_CHECK_EQ(library.generation(id), 1U);
}

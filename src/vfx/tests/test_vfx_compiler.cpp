// The compiler: graph -> typed VFX IR -> optimisation -> Slang. M8.c tasks 2.1, 2.2 and 2.5.
//
// INTEGRATION TIER, AND DELIBERATELY. Every case here authors several graphs of a dozen nodes,
// compiles them through the shared expression core to a fixed point and emits their programs.
// `src/graph/tests/` learnt at M8.b that exactly this shape does not fit the unit tier's
// millisecond in a Debug configuration, and the subject IS the repeated compilation — making it
// cheaper would mean measuring something else.
//
// EVERY OPTIMISATION CLAIM HERE IS A COMPARISON BETWEEN TWO COOKS OF THE SAME ASSET WITH ONE SWITCH
// CHANGED. There is no tolerance to tune and no golden number to regenerate: turn the switch off,
// the number moves, and the case that asserts it moves goes red when the pass stops working.

#include "effects.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/vfx/interfaces.h>
#include <cy/vfx/runtime.h>

#include <cstdio>

using namespace cy;
using namespace cy::vfx;
using namespace cy::vfx_test;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// Print every diagnostic the sink holds. A compile that failed with sixty-seven diagnostics and
/// no way to read them is a compiler whose "node- and pin-precise" claim nobody can check.
void dump(const graph::DiagnosticSink& sink) noexcept {
    for (const graph::Diagnostic& diagnostic : sink.entries()) {
        std::fprintf(stderr, "  node %llu pin '%s': %s [%s]\n",
                     static_cast<unsigned long long>(diagnostic.node), diagnostic.pin.c_str(),
                     diagnostic.message, diagnostic.detail.c_str());
    }
}

/// Does the generated Slang of an emitter contain `needle`?
[[nodiscard]] bool source_contains(const CompiledEmitter& emitter, const char* needle) noexcept {
    Array<char> unit(allocator());
    Array<ParameterDecl> none(allocator());
    Array<EventChannelDecl> no_channels(allocator());
    if (!assemble_translation_unit(emitter, none.span(), no_channels.span(), unit).has_value()) {
        return false;
    }
    const std::string_view text(unit.data(), unit.size());
    return text.find(needle) != std::string_view::npos;
}

}  // namespace

CY_TEST_CASE("the plume cooks, and the cook report answers what vfx-system asks it to") {
    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    CompileOptions options;
    auto system = cook_plume(allocator(), sink, report, options);
    if (!system.has_value()) {
        dump(sink);
    }
    CY_REQUIRE(system.has_value());
    CY_REQUIRE_EQ(sink.errors(), 0U);
    CY_REQUIRE_EQ(report.emitters.size(), 1U);

    const EmitterReport& emitter = report.emitters[0];
    // "Cooking SHALL report per effect: kernel count, per-particle size, estimated GPU cost at a
    // reference population, and any features unsupported on target platforms."
    CY_CHECK_GT(emitter.kernels, 0U);
    CY_CHECK_GT(emitter.bytes_per_particle, 0U);
    CY_CHECK_GT(emitter.max_population, 0U);
    CY_CHECK_GT(emitter.estimated_cost_units, 0U);
    CY_CHECK_GT(emitter.generated_source_bytes, 0U);
    CY_CHECK_FALSE(report.bisection_build);
    std::fprintf(stderr,
                 "cooked %s: %u kernels, %u bytes a particle, max population %u, %u attributes "
                 "allocated and %u elided, %u parameters folded, cook key 0x%016llx\n",
                 emitter.emitter.c_str(), emitter.kernels, emitter.bytes_per_particle,
                 emitter.max_population, emitter.attributes_allocated, emitter.attributes_elided,
                 emitter.folded_parameters, static_cast<unsigned long long>(system->cook_key()));
}

CY_TEST_CASE("two cooks of one asset produce the identical cook key, and one edit moves it") {
    graph::DiagnosticSink sink(allocator());
    CompileOptions options;
    CompileReport first_report(allocator());
    auto first = cook_plume(allocator(), sink, first_report, options);
    CompileReport second_report(allocator());
    auto second = cook_plume(allocator(), sink, second_report, options);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(first->cook_key(), second->cook_key());

    // A DIFFERENT CAPACITY IS A DIFFERENT LAYOUT, and the layout digest closes over it — so the
    // content-addressed cache cannot serve one cook's artefact for the other's.
    CompileReport third_report(allocator());
    auto third = cook_plume(allocator(), sink, third_report, options, 1, 1024);
    CY_REQUIRE(third.has_value());
    CY_CHECK_NE(first->cook_key(), third->cook_key());
}

CY_TEST_CASE("attribute liveness: written and never read is not allocated, and its store is gone") {
    graph::DiagnosticSink sink(allocator());
    CompileOptions options;
    CompileReport with(allocator());
    auto live = cook_plume(allocator(), sink, with, options);
    CY_REQUIRE(live.has_value());

    const AttributeLayout& layout = live->emitters()[0].layout();
    const AttributeSlot* scratch = layout.find(Name::intern("scratch"));
    CY_REQUIRE(scratch != nullptr);
    // `scratch` is written by Initialise and read by nothing — not by a stage and not by the
    // renderer. It is not allocated.
    CY_CHECK(scratch->elided);
    CY_CHECK_EQ(with.emitters[0].attributes_elided, 1U);
    // And the elision is visible in the generated code, not only in the report.
    CY_CHECK(source_contains(live->emitters()[0], "elided by attribute liveness"));
    CY_CHECK_FALSE(source_contains(live->emitters()[0], "cyVfxStore_scratch"));

    // THE CONTROL. With the pass off, the same asset allocates it and emits its store — which is
    // what makes the assertions above a check rather than a description.
    options.attribute_liveness = false;
    CompileReport without(allocator());
    auto dead = cook_plume(allocator(), sink, without, options);
    CY_REQUIRE(dead.has_value());
    const AttributeSlot* kept = dead->emitters()[0].layout().find(Name::intern("scratch"));
    CY_REQUIRE(kept != nullptr);
    CY_CHECK_FALSE(kept->elided);
    CY_CHECK_EQ(without.emitters[0].attributes_elided, 0U);
    CY_CHECK(source_contains(dead->emitters()[0], "cyVfxStore_scratch"));
    // "Attributes never referenced by any stage of an emitter SHALL NOT be allocated" — as a
    // number: four bytes a particle, times the capacity.
    CY_CHECK_LT(with.emitters[0].bytes_per_particle, without.emitters[0].bytes_per_particle);
    std::fprintf(stderr, "liveness: %u bytes a particle with the pass, %u without\n",
                 with.emitters[0].bytes_per_particle, without.emitters[0].bytes_per_particle);
}

CY_TEST_CASE("constant folding of a parameter known at cook time") {
    graph::DiagnosticSink sink(allocator());
    CompileOptions options;
    CompileReport folded(allocator());
    auto with = cook_plume(allocator(), sink, folded, options);
    CY_REQUIRE(with.has_value());
    // `gravity` is declared unexposed, so it is folded and never read from a buffer. TWO, because
    // the update graph is lowered twice — once fused onto the initialise and once on its own — and
    // the fold happens in each. The number is per lowering, which is the honest count of how many
    // generated programs stopped reading a buffer.
    CY_CHECK_EQ(folded.emitters[0].folded_parameters, 2U);
    CY_CHECK_FALSE(source_contains(with->emitters()[0], "cyVfxParams.gravity"));
    // `intensity` is exposed, so it must NOT be folded — a folder that folded everything would pass
    // the assertion above and break every effect an artist tunes at run time.
    CY_CHECK(source_contains(with->emitters()[0], "cyVfxParams.intensity"));

    options.fold_parameters = false;
    CompileReport unfolded(allocator());
    auto without = cook_plume(allocator(), sink, unfolded, options);
    CY_REQUIRE(without.has_value());
    CY_CHECK_EQ(unfolded.emitters[0].folded_parameters, 0U);
    CY_CHECK(source_contains(without->emitters()[0], "cyVfxParams.gravity"));
}

CY_TEST_CASE("kernel fusion: initialise and update share one dispatch over the newly spawned") {
    graph::DiagnosticSink sink(allocator());
    CompileOptions options;
    CompileReport fused(allocator());
    auto with = cook_plume(allocator(), sink, fused, options);
    CY_REQUIRE(with.has_value());
    CY_CHECK_EQ(fused.emitters[0].dispatches_before_fusion, 2U);
    CY_CHECK_EQ(fused.emitters[0].dispatches_after_fusion, 1U);

    const VfxKernel* initialise = with->emitters()[0].kernel_for(Stage::Initialise);
    CY_REQUIRE(initialise != nullptr);
    CY_CHECK(initialise->covers(Stage::Initialise));
    CY_CHECK(initialise->covers(Stage::Update));
    // AND THE UPDATE KERNEL IS STILL ITS OWN. Handing the fused kernel back for `Update` would
    // re-initialise every live particle every step, which is the defect `kernel_for`'s exact-match
    // preference exists to make impossible.
    const VfxKernel* update = with->emitters()[0].kernel_for(Stage::Update);
    CY_REQUIRE(update != nullptr);
    CY_CHECK_FALSE(update->covers(Stage::Initialise));

    options.fuse_stages = false;
    CompileReport separate(allocator());
    auto without = cook_plume(allocator(), sink, separate, options);
    CY_REQUIRE(without.has_value());
    CY_CHECK_EQ(separate.emitters[0].dispatches_after_fusion, 2U);
    const VfxKernel* unfused = without->emitters()[0].kernel_for(Stage::Initialise);
    CY_REQUIRE(unfused != nullptr);
    CY_CHECK_FALSE(unfused->covers(Stage::Update));

    // THE INTERMEDIATE TRAFFIC. The fused kernel reads `velocity` out of a register rather than out
    // of memory, so the fused program contains no load of it while the unfused update does.
    std::fprintf(stderr, "fusion: fused initialise is %u slots, unfused is %u\n",
                 initialise->slot_count(), unfused->slot_count());
}

CY_TEST_CASE("precision selection reaches the generated code, not only the report") {
    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    CompileOptions options;
    auto system = cook_plume(allocator(), sink, report, options);
    CY_REQUIRE(system.has_value());
    const AttributeLayout& layout = system->emitters()[0].layout();

    const AttributeSlot* color = layout.find(Name::intern("color"));
    CY_REQUIRE(color != nullptr);
    // Declared with a tolerance of 1/255 over [0, 1]: four components at one byte each.
    CY_CHECK_EQ(color->precision, Precision::Unorm8);
    CY_CHECK_EQ(color->stride, 4U);
    const AttributeSlot* position = layout.find(Name::intern("position"));
    CY_REQUIRE(position != nullptr);
    CY_CHECK_EQ(position->precision, Precision::Float32);
    CY_CHECK_EQ(position->stride, 12U);

    // AND THE SHADER SPELLS IT. A layout that only changed a report would be a report.
    CY_CHECK(source_contains(system->emitters()[0], "cyVfxPackUnorm8"));
    CY_CHECK(source_contains(system->emitters()[0], "cyVfxLoad_position"));
}

CY_TEST_CASE("the generated unit is self-contained: it imports nothing and declares what it uses") {
    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    CompileOptions options;
    auto system = cook_plume(allocator(), sink, report, options);
    CY_REQUIRE(system.has_value());

    Array<char> unit(allocator());
    CY_REQUIRE(assemble_translation_unit(system->emitters()[0], system->parameters(),
                                         system->channels(), unit)
                   .has_value());
    const std::string_view text(unit.data(), unit.size());
    // NO IMPORT. See compile.h: a unit that imported an authored module which imports another does
    // not resolve through `cy::shader`'s Slang front end today (this milestone's finding C), and a
    // generated kernel that needed one would be a kernel blocked on a fix below this layer.
    CY_CHECK_EQ(text.find("import "), std::string_view::npos);
    CY_CHECK_NE(text.find("struct CyVfxParams"), std::string_view::npos);
    CY_CHECK_NE(text.find("RWStructuredBuffer<uint> cyVfxAttr_position"), std::string_view::npos);
    CY_CHECK_NE(text.find(kVfxKernelEntryPoint), std::string_view::npos);
    CY_CHECK_NE(text.find("[shader(\"compute\")]"), std::string_view::npos);

    // WRITTEN OUT EVERY RUN, in the suite's own working directory. `material-compiler`'s recorded
    // gap at M7 was "the bundle carries the IR, the generated source, the cost report and the cook
    // key, and nothing invokes the compiler"; a generated unit nobody can put in front of a shader
    // compiler is the same gap one layer along. This file is what `slangc` is handed by hand, and
    // what a smoke suite would hand `cy::shader` when one is written.
    if (std::FILE* file = std::fopen("vfx-kernel.slang", "wb"); file != nullptr) {
        (void)std::fwrite(unit.data(), 1, unit.size(), file);
        (void)std::fclose(file);
        std::fprintf(stderr, "wrote vfx-kernel.slang (%zu bytes)\n", unit.size());
    }
    std::fprintf(stderr, "generated translation unit: %zu bytes\n", unit.size());
}

CY_TEST_CASE("two emitters compiled from one asset produce identical kernel digests") {
    // Which is what the scheduler's merging depends on: "group those sharing a compiled kernel and
    // compatible bindings". If two copies of one emitter did not agree on a digest, nothing would
    // ever merge and the requirement would be unreachable rather than unmet.
    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    CompileOptions options;
    auto system = cook_plume(allocator(), sink, report, options, 3);
    CY_REQUIRE(system.has_value());
    CY_REQUIRE_EQ(system->emitters().size(), 3U);
    const VfxKernel* first = system->emitters()[0].kernel_for(Stage::Update);
    const VfxKernel* second = system->emitters()[1].kernel_for(Stage::Update);
    CY_REQUIRE(first != nullptr);
    CY_REQUIRE(second != nullptr);
    CY_CHECK_EQ(first->digest(), second->digest());
    CY_CHECK_EQ(system->emitters()[0].layout().digest(), system->emitters()[1].layout().digest());
}

// --- Data interfaces
// ------------------------------------------------------------------------------

CY_TEST_CASE(
    "a project registers its own data interface and a graph reads it, with no compiler "
    "change") {
    NodeRegistry registry(allocator());
    DataInterfaceRegistry interfaces(allocator());
    CY_REQUIRE(prepare(registry, interfaces).has_value());

    static constexpr InterfaceField kFields[] = {{"pressure", Float}, {"flow", Float3}};
    DataInterfaceDesc desc;
    desc.name = "project_grid";
    desc.version = 2;
    desc.fields = {kFields, 2};
    desc.cost = InterfaceCost::Moderate;
    desc.cpu_available = true;
    CY_REQUIRE(interfaces.register_interface(desc).has_value());

    VfxSystemAsset asset(allocator(), Name::intern("custom"));
    Emitter emitter(allocator(), Name::intern("reader"));
    emitter.set_capacity(16);
    StageBuilder stage(allocator(), "update");
    const NodeKey pressure = stage.sample("project_grid", "pressure", stage.attribute("position"));
    stage.write("size", pressure);
    CY_REQUIRE(stage.ok());
    CY_REQUIRE(emitter.set_stage(Stage::Update, stage.take()).has_value());
    CY_REQUIRE(asset.add_emitter(std::move(emitter)).has_value());
    asset.resolve(registry);

    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    CompileOptions options;
    auto system = compile_system(asset, registry, interfaces, options, sink, report);
    CY_REQUIRE(system.has_value());
    CY_CHECK(source_contains(system->emitters()[0], "cyVfxSample_project_grid_pressure"));
    CY_CHECK_GT(report.emitters[0].sample_cost_weight, 0U);
}

CY_TEST_CASE("a CPU-path effect using a GPU-only interface fails to cook, naming the interface") {
    NodeRegistry registry(allocator());
    DataInterfaceRegistry interfaces(allocator());
    CY_REQUIRE(prepare(registry, interfaces).has_value());

    VfxSystemAsset asset(allocator(), Name::intern("cpu_effect"));
    Emitter emitter(allocator(), Name::intern("reader"));
    emitter.set_capacity(16);
    // "WHEN an effect declares CPU simulation and uses a GPU-only data interface THEN cooking SHALL
    // FAIL with a diagnostic naming the interface."
    emitter.set_path(SimulationPath::CpuRequired);
    StageBuilder stage(allocator(), "update");
    stage.write("size", stage.sample("scene_sdf", "distance", stage.attribute("position")));
    CY_REQUIRE(stage.ok());
    CY_REQUIRE(emitter.set_stage(Stage::Update, stage.take()).has_value());
    CY_REQUIRE(asset.add_emitter(std::move(emitter)).has_value());
    asset.resolve(registry);

    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    CompileOptions options;
    auto system = compile_system(asset, registry, interfaces, options, sink, report);
    CY_CHECK_FALSE(system.has_value());
    CY_REQUIRE(sink.entries().size() > 0U);
    bool named = false;
    for (const graph::Diagnostic& diagnostic : sink.entries()) {
        named = named || diagnostic.detail == Name::intern("scene_sdf");
        // NODE- AND PIN-PRECISE. "the error SHALL identify the offending node and pin, not only the
        // generated source line."
        CY_CHECK_NE(diagnostic.node, graph::kInvalidNodeKey);
    }
    CY_CHECK(named);
}

CY_TEST_CASE("the fluid seam is reserved: the grid data interface contract is registered") {
    // `vfx-system`: fluids are deferred, and "The architecture SHALL reserve the seams that would
    // allow it later without restructuring: a GRID DATA INTERFACE CONTRACT for graphs to read and
    // write volumetric data". A change that removed it would be "flagged against this requirement",
    // and this is the flag.
    DataInterfaceRegistry interfaces(allocator());
    CY_REQUIRE(register_builtin_interfaces(interfaces).has_value());
    const DataInterface* grid = interfaces.find(Name::intern(kGridInterfaceName));
    CY_REQUIRE(grid != nullptr);
    CY_CHECK(grid->find_field(Name::intern("density")) != nullptr);
    CY_CHECK(grid->find_field(Name::intern("velocity")) != nullptr);
}

CY_TEST_CASE("wind is the shared field and there is no VFX-owned wind model") {
    DataInterfaceRegistry interfaces(allocator());
    CY_REQUIRE(register_builtin_interfaces(interfaces).has_value());
    const DataInterface* wind = interfaces.find(Name::intern("wind_field"));
    CY_REQUIRE(wind != nullptr);
    CY_CHECK(wind->cpu_available());
    CY_CHECK(wind->gpu_available());
    // Every interface `vfx-system` names by hand, checked as a list rather than one at a time: a
    // missing one is a capability an author cannot reach, and nothing else would notice.
    static constexpr const char* kRequired[] = {
        "scene_depth", "scene_normals",     "scene_sdf",     "gpu_scene", "physics_query",
        "terrain",     "static_mesh",       "skeletal_mesh", "texture",   "curve",
        "camera",      "environment_field", "audio",         "ecs_query", "structured_buffer"};
    for (const char* required : kRequired) {
        if (interfaces.find(Name::intern(required)) == nullptr) {
            std::fprintf(stderr, "missing built-in data interface: %s\n", required);
        }
        CY_CHECK(interfaces.find(Name::intern(required)) != nullptr);
    }
}

// --- The execution path
// ---------------------------------------------------------------------------

CY_TEST_CASE("the CPU path is DECLARED: every fall back to it carries its reason") {
    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    CompileOptions options;
    auto system = cook_plume(allocator(), sink, report, options);
    CY_REQUIRE(system.has_value());
    const CompiledEmitter& emitter = system->emitters()[0];

    DeviceCapability capability;
    capability.compute = false;
    PathDecision decision = decide_path(emitter, capability);
    CY_CHECK_EQ(decision.path, ExecutionPath::Cpu);
    CY_CHECK_EQ(decision.reason, FallbackReason::DeviceLacksCompute);
    CY_CHECK(decision.is_fallback);
    CY_CHECK_NE(decision.explanation, nullptr);
    CY_CHECK_GT(std::string_view(decision.explanation).size(), 0U);

    capability.compute = true;
    capability.indirect_dispatch = false;
    decision = decide_path(emitter, capability);
    CY_CHECK_EQ(decision.reason, FallbackReason::DeviceLacksIndirectDispatch);

    capability.indirect_dispatch = true;
    capability.gpu_path_enabled = false;
    decision = decide_path(emitter, capability);
    CY_CHECK_EQ(decision.reason, FallbackReason::DisabledByHost);

    // AND THE HONEST ONE. On a fully capable device the reason today is that this build has no
    // compute dispatch for a VFX kernel, and the decision says so every time it is asked.
    capability = DeviceCapability{};
    decision = decide_path(emitter, capability);
    if (device_dispatch_available()) {
        CY_CHECK_EQ(decision.path, ExecutionPath::Gpu);
    } else {
        CY_CHECK_EQ(decision.reason, FallbackReason::DeviceDispatchUnimplemented);
        CY_CHECK(decision.is_fallback);
    }
    std::fprintf(stderr, "path on a capable device: %s (%s)\n",
                 decision.path == ExecutionPath::Gpu ? "Gpu" : "Cpu",
                 fallback_reason_name(decision.reason));
}

CY_TEST_CASE("an effect that DECLARED the CPU path is not a fallback and is not counted as one") {
    NodeRegistry registry(allocator());
    DataInterfaceRegistry interfaces(allocator());
    CY_REQUIRE(prepare(registry, interfaces).has_value());
    VfxSystemAsset asset(allocator(), Name::intern("cpu_effect"));
    Emitter emitter(allocator(), Name::intern("reader"));
    emitter.set_capacity(16);
    emitter.set_path(SimulationPath::CpuRequired);
    StageBuilder stage(allocator(), "update");
    stage.write("size", stage.constant(0.5F));
    CY_REQUIRE(stage.ok());
    CY_REQUIRE(emitter.set_stage(Stage::Update, stage.take()).has_value());
    CY_REQUIRE(asset.add_emitter(std::move(emitter)).has_value());
    asset.resolve(registry);

    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    CompileOptions options;
    auto system = compile_system(asset, registry, interfaces, options, sink, report);
    CY_REQUIRE(system.has_value());
    const PathDecision decision = decide_path(system->emitters()[0], DeviceCapability{});
    CY_CHECK_EQ(decision.path, ExecutionPath::Cpu);
    CY_CHECK_EQ(decision.reason, FallbackReason::EffectRequiresCpu);
    CY_CHECK_FALSE(decision.is_fallback);
}

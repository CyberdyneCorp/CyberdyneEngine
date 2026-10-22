// SPDX-License-Identifier: MIT
// Collision in the compiler: the collide nodes, what they lower to, what they generate, and what an
// effect that does not collide generates instead — exactly what it did before. `vfx-system` —
// "Collision". `integration.vfx_compiler`.

#include "collision_effects.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/vfx/catalogue.h>
#include <cy/vfx/gpu_layout.h>
#include <cy/vfx/interfaces.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>

using namespace cy;
using namespace cy::vfx;
using namespace cy::vfx_test;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

// The plume's generated units and cook key on `main` before the collide nodes existed (f692e305),
// FNV-1a over the text.
constexpr u64 kPlumeDispatchHash = 0x184a45c5e7255ae7ULL;
constexpr u64 kPlumeProbeHash = 0x489ea6ce1784b8eeULL;
constexpr u64 kPlumeCookKey = 0x7e0bfc274315679fULL;

[[nodiscard]] u64 fnv1a(Span<const char> text) noexcept {
    u64 hash = 1469598103934665603ULL;
    for (const char character : text) {
        hash = (hash ^ static_cast<u8>(character)) * 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] std::string_view view_of(const Array<char>& text) noexcept {
    return {text.data(), text.size()};
}

/// How many `op` nodes the kernel covering `stage` holds.
[[nodiscard]] u32 count_ops(const CompiledEmitter& emitter, Stage stage, OpId op) noexcept {
    const VfxKernel* kernel = emitter.kernel_for(stage);
    if (kernel == nullptr) {
        return 0;
    }
    u32 count = 0;
    for (const KernelStep& step : kernel->program()) {
        count += step.op == op ? 1U : 0U;
    }
    return count;
}

void dump(const graph::DiagnosticSink& sink) noexcept {
    for (const graph::Diagnostic& diagnostic : sink.entries()) {
        std::fprintf(stderr, "  node %llu: %s [%s]\n",
                     static_cast<unsigned long long>(diagnostic.node), diagnostic.message,
                     diagnostic.detail.c_str());
    }
}

/// Where a generated unit is written for a hand run of `slangc`: the run's own directory.
void write_unit(const char* name, std::string_view text) noexcept {
    const char* directory = std::getenv("CY_TEST_ARTEFACT_DIR");
    if (directory == nullptr || *directory == '\0') {
        directory = CY_TEST_BINARY_DIR;
    }
    char path[1024];
    const int written = std::snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (written <= 0 || static_cast<usize>(written) >= sizeof(path)) {
        return;
    }
    if (std::FILE* file = std::fopen(path, "wb"); file != nullptr) {
        (void)std::fwrite(text.data(), 1, text.size(), file);
        (void)std::fclose(file);
    }
}

}  // namespace

CY_TEST_CASE("collision: an effect that collides with nothing generates what it did before") {
    // OFF IS BYTE-IDENTICAL. The plume has no collider, and its dispatch unit, its probe unit and
    // its cook key are the ones `main` produced before the collide nodes existed — measured on
    // that tree and written here. A change to the shared prelude, the push block, the op table's
    // existing numbers or the lowering of an existing node moves one of them.
    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    auto system = cook_plume(allocator(), sink, report, CompileOptions{});
    CY_REQUIRE(system.has_value());
    const CompiledEmitter& emitter = system->emitters()[0];
    CY_CHECK_FALSE(emitter.collides());
    Array<char> dispatch(allocator());
    Array<char> probe(allocator());
    CY_REQUIRE(assemble_dispatch_unit(emitter, system->parameters(), system->channels(), dispatch)
                   .has_value());
    CY_REQUIRE(assemble_translation_unit(emitter, system->parameters(), system->channels(), probe)
                   .has_value());
    std::fprintf(stderr,
                 "plume: dispatch %016llx (%zu bytes), probe %016llx (%zu bytes), key %016llx\n",
                 static_cast<unsigned long long>(fnv1a(dispatch.span())), dispatch.size(),
                 static_cast<unsigned long long>(fnv1a(probe.span())), probe.size(),
                 static_cast<unsigned long long>(system->cook_key()));
    CY_CHECK_EQ(fnv1a(dispatch.span()), kPlumeDispatchHash);
    CY_CHECK_EQ(fnv1a(probe.span()), kPlumeProbeHash);
    CY_CHECK_EQ(system->cook_key(), kPlumeCookKey);
    CY_CHECK_EQ(view_of(dispatch).find("cyVfxCollision"), std::string_view::npos);
    CY_CHECK_EQ(view_of(dispatch).find("collision_quality"), std::string_view::npos);
}

CY_TEST_CASE("collision: a collider's outputs share one contact, however many are wired") {
    // Six outputs, one query: `position`, `velocity`, `contact` and the event's `impact` and
    // `position` are wired here, and the kernel holds one `Contact`. With sample deduplication off
    // too, because the memo is the lowering's and does not depend on the optimiser.
    DropScenario scenario;
    scenario.raise_impacts = true;
    for (const bool dedupe : {true, false}) {
        graph::DiagnosticSink sink(allocator());
        CompileReport report(allocator());
        CompileOptions options;
        options.switches.sample_dedup = dedupe;
        NodeRegistry registry(allocator());
        DataInterfaceRegistry interfaces(allocator());
        CY_REQUIRE(prepare(registry, interfaces).has_value());
        auto asset = build_drop(allocator(), scenario);
        CY_REQUIRE(asset.has_value());
        asset->resolve(registry);
        auto system = compile_system(*asset, registry, interfaces, options, sink, report);
        if (!system.has_value()) {
            dump(sink);
        }
        CY_REQUIRE(system.has_value());
        const CompiledEmitter& emitter = system->emitters()[0];
        CY_CHECK(emitter.collides());
        CY_CHECK_EQ(count_ops(emitter, Stage::Update, Contact), 1U);
        CY_CHECK_EQ(count_ops(emitter, Stage::Update, Bounce), 1U);
        CY_CHECK_EQ(count_ops(emitter, Stage::Update, Resolve), 1U);
        CY_CHECK_EQ(count_ops(emitter, Stage::Update, Impact), 1U);
    }
}

CY_TEST_CASE("collision: a colliding emitter's unit carries the collision set and nothing else's") {
    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    DropScenario scenario;
    scenario.shape = "sdf";
    auto system = cook_drop(allocator(), scenario, sink, report);
    CY_REQUIRE(system.has_value());
    const CompiledEmitter& emitter = system->emitters()[0];
    Array<char> dispatch(allocator());
    CY_REQUIRE(assemble_dispatch_unit(emitter, system->parameters(), system->channels(), dispatch)
                   .has_value());
    const std::string_view text = view_of(dispatch);
    CY_CHECK_NE(text.find("[[vk::binding(0, 1)]] ParameterBlock<CyVfxCollisionSet>"),
                std::string_view::npos);
    CY_CHECK_NE(text.find("uint collision_quality;"), std::string_view::npos);
    CY_CHECK_NE(text.find("cyVfxContact_sdf("), std::string_view::npos);
    CY_CHECK_NE(text.find("cyVfxBounce("), std::string_view::npos);
    // THE ROW NUMBERS COME FROM THE CONTRACT, not from a second spelling of it.
    char row[96];
    std::snprintf(row, sizeof(row), "static const uint CY_VFX_COLLISION_LEVELS = %uu;",
                  static_cast<unsigned>(kGpuCollisionLevels));
    CY_CHECK_NE(text.find(row), std::string_view::npos);
    // The cook report charges the field what a scene-field sample costs.
    CY_CHECK_GE(report.emitters[0].sample_cost_weight,
                interface_cost_weight(InterfaceCost::Expensive));
    write_unit("vfx-collision.slang", text);
}

CY_TEST_CASE("collision: every collider and both event reads are in the editor's palette") {
    // The palette IS the registry (`vfx.catalogue.get`, served over the editor protocol and MCP),
    // so a node registered here is a node an author and an agent can place.
    NodeRegistry registry(allocator());
    CY_REQUIRE(register_vfx_nodes(registry).has_value());
    for (const char* name : {"vfx.collide.plane", "vfx.collide.sphere", "vfx.collide.box",
                             "vfx.collide.capsule", "vfx.collide.sdf", "vfx.collide.depth"}) {
        const graph::NodeType* type = registry.find(Name::intern(name));
        CY_REQUIRE(type != nullptr);
        u32 outputs = 0;
        u32 required = 0;
        for (const graph::PinDesc& pin : type->pins()) {
            outputs += pin.direction == graph::PinDirection::Output ? 1U : 0U;
            required += pin.required ? 1U : 0U;
        }
        CY_CHECK_EQ(outputs, 6U);
        CY_CHECK_EQ(required, 2U);  // position and velocity; everything else has a default
    }
    CY_CHECK(registry.find(Name::intern("vfx.event_count")) != nullptr);
    CY_CHECK(registry.find(Name::intern("vfx.event_payload")) != nullptr);
    Array<u8> catalogue(allocator());
    CY_REQUIRE(encode_vfx_catalogue(catalogue).has_value());
    const std::string_view bytes(reinterpret_cast<const char*>(catalogue.data()), catalogue.size());
    CY_CHECK_NE(bytes.find("vfx.collide.capsule"), std::string_view::npos);
    CY_CHECK_NE(bytes.find("restitution"), std::string_view::npos);
}

CY_TEST_CASE("collision: event reads are refused outside the stage that can mean them") {
    // `vfx.event_count` is a spawn request and `vfx.event_payload` is read by the particle an event
    // spawned. Anywhere else each is a diagnostic naming the node, not a zero.
    for (const bool payload_in_update : {false, true}) {
        NodeRegistry registry(allocator());
        DataInterfaceRegistry interfaces(allocator());
        CY_REQUIRE(prepare(registry, interfaces).has_value());
        DropScenario scenario;
        scenario.raise_impacts = true;
        auto asset = build_drop(allocator(), scenario);
        CY_REQUIRE(asset.has_value());
        Emitter& emitter = asset->edit_emitters()[0];
        // On the CPU path, so the stage is the only thing wrong.
        emitter.set_path(SimulationPath::CpuRequired);
        StageBuilder stage(allocator(), payload_in_update ? "update" : "initialise");
        const NodeKey read = payload_in_update ? stage.event_read("vfx.event_payload", "impact")
                                               : stage.event_read("vfx.event_count", "impact");
        stage.write(payload_in_update ? "color" : "size", read);
        CY_REQUIRE(stage.ok());
        CY_REQUIRE(
            emitter.set_stage(payload_in_update ? Stage::Update : Stage::Initialise, stage.take())
                .has_value());
        asset->resolve(registry);
        graph::DiagnosticSink sink(allocator());
        CompileReport report(allocator());
        auto system = compile_system(*asset, registry, interfaces, CompileOptions{}, sink, report);
        CY_CHECK_FALSE(system.has_value());
        bool named = false;
        for (const graph::Diagnostic& diagnostic : sink.entries()) {
            named = named ||
                    std::string_view(diagnostic.message).find("stage") != std::string_view::npos;
        }
        CY_CHECK(named);
    }
}

CY_TEST_CASE("collision: an event read in a GPU-path emitter fails to cook, naming the channel") {
    // Like a CPU-only data interface: the device cannot read a channel's contents, so an emitter
    // that spawns from one must declare the CPU path.
    NodeRegistry registry(allocator());
    DataInterfaceRegistry interfaces(allocator());
    CY_REQUIRE(prepare(registry, interfaces).has_value());
    DropScenario scenario;
    scenario.raise_impacts = true;
    scenario.sparks = true;
    auto asset = build_drop(allocator(), scenario);
    CY_REQUIRE(asset.has_value());
    asset->edit_emitters()[1].set_path(SimulationPath::GpuPreferred);
    asset->resolve(registry);
    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    auto system = compile_system(*asset, registry, interfaces, CompileOptions{}, sink, report);
    CY_CHECK_FALSE(system.has_value());
    bool named = false;
    for (const graph::Diagnostic& diagnostic : sink.entries()) {
        named = named || diagnostic.detail == Name::intern("impact");
    }
    CY_CHECK(named);
}

CY_TEST_CASE("an event raise does not end its stage's lowering") {
    // REGRESSION. `lower_stage` returned from the loop over a stage's nodes at its first event
    // raise, so every write, kill and raise authored after one was silently dropped: the stage
    // compiled, and did less than its graph said. Here the raise comes first and the write after
    // it must still be in the kernel.
    NodeRegistry registry(allocator());
    DataInterfaceRegistry interfaces(allocator());
    CY_REQUIRE(prepare(registry, interfaces).has_value());
    auto asset = build_plume(allocator());
    CY_REQUIRE(asset.has_value());
    Emitter& emitter = asset->edit_emitters()[0];
    StageBuilder stage(allocator(), "update");
    stage.emit_event("collision", stage.constant(1.0F));
    stage.write("size", stage.constant(0.5F));
    stage.kill_if(stage.binary("vfx.greater", stage.attribute("age"), stage.constant(1.0F)));
    CY_REQUIRE(stage.ok());
    CY_REQUIRE(emitter.set_stage(Stage::Update, stage.take()).has_value());
    asset->resolve(registry);
    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    auto system = compile_system(*asset, registry, interfaces, CompileOptions{}, sink, report);
    CY_REQUIRE(system.has_value());
    const VfxKernel* update = system->emitters()[0].kernel_for(Stage::Update);
    CY_REQUIRE(update != nullptr);
    CY_CHECK_EQ(update->events().size(), 1U);
    bool writes_size = false;
    for (const KernelWrite& write : update->writes()) {
        writes_size = writes_size || write.attribute == Name::intern("size");
    }
    CY_CHECK(writes_size);
    CY_CHECK_NE(update->kill_slot(), 0xFFFFU);
}

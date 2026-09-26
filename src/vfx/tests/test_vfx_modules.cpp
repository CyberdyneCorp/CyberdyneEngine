// SPDX-License-Identifier: MIT
// Reusable VFX module assets, resolved and composed at cook time. Issue #19.
//
// Every case authors the documents the desktop saves — a `.cyvfxdoc` system that maps each module
// name to an explicit `.cyvfxmodule` path, and the modules themselves — and cooks them through
// `read_authoring_document`, `resolve_authoring_modules` and `compile_system`, which is the whole
// cook the editor service and the build graph's `vfx` node run. Each claim is a comparison between
// two cooks that differ in one thing.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/vfx/authoring.h>
#include <cy/vfx/interfaces.h>
#include <cy/vfx/runtime.h>
#include <cy/vfx/world.h>

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cy;
using namespace cy::vfx;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// The editor's little-endian payload writer, and the hexadecimal envelope it saves.
class Payload {
public:
    Payload& u8v(u8 value) {
        bytes_.push_back(value);
        return *this;
    }
    Payload& u32v(u32 value) {
        for (u32 shift = 0; shift < 32; shift += 8) {
            bytes_.push_back(static_cast<u8>(value >> shift));
        }
        return *this;
    }
    Payload& text(std::string_view value) {
        u32v(static_cast<u32>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return *this;
    }
    [[nodiscard]] std::string envelope(std::string_view header) const {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string out(header);
        for (const u8 byte : bytes_) {
            out.push_back(kDigits[byte >> 4U]);
            out.push_back(kDigits[byte & 15U]);
        }
        return out;
    }

private:
    std::vector<u8> bytes_;
};

struct ModuleSpec {
    std::string name;
    Stage stage = Stage::Update;
    std::vector<std::pair<std::string, std::string>> inputs;
    std::vector<std::string> uses;
    /// Canvas statements after the header line.
    std::string body;
};

[[nodiscard]] std::string module_text(const ModuleSpec& spec) {
    Payload payload;
    payload.u32v(1).text(spec.name).u8v(static_cast<u8>(spec.stage));
    payload.u32v(static_cast<u32>(spec.inputs.size()));
    for (const auto& [name, type] : spec.inputs) {
        payload.text(name).text(type);
    }
    payload.u32v(static_cast<u32>(spec.uses.size()));
    for (const std::string& used : spec.uses) {
        payload.text(used);
    }
    payload.text("cyvfxcanvas 1\nmodule " + spec.name + "\n" + spec.body);
    return payload.envelope("cyvfxmodule 1\n");
}

/// A system of two CPU emitters, `calm` and `smoke`. Spawn emits a constant count and Initialise
/// gives every particle a size of 1. Only `smoke`, the SECOND emitter, references `modules`, so a
/// diagnostic scoped to emitter 1 is scoped to the emitter that asked. Every name in `mapped` is
/// mapped to `<directory><name>.cyvfxmodule`.
[[nodiscard]] std::string system_text(const std::vector<std::string>& modules,
                                      const std::vector<std::string>& mapped,
                                      std::string_view directory = "effects/modules/") {
    const std::string initialise =
        "node 1 vfx.constant\nprop 1 value 0 0 0\nnode 2 vfx.set_attribute\n"
        "prop 2 attribute position\nlink 1 out 2 value\n"
        "node 3 vfx.constant\nprop 3 value 1\nnode 4 vfx.set_attribute\n"
        "prop 4 attribute size\nlink 3 out 4 value\n"
        "node 5 vfx.constant\nprop 5 value 5\nnode 6 vfx.set_attribute\n"
        "prop 6 attribute lifetime\nlink 5 out 6 value\n";
    Payload payload;
    payload.u32v(3).text("puff").u32v(2);
    for (const std::string_view emitter : {"calm", "smoke"}) {
        const std::string header = "cyvfxcanvas 1\nemitter " + std::string(emitter) + "\n";
        payload.text(emitter).u8v(1).text("Sprite").u32v(2);
        payload.u8v(static_cast<u8>(Stage::Spawn))
            .text(header +
                  "node 1 vfx.constant\nprop 1 value 4\nnode 2 vfx.spawn_count\n"
                  "link 1 out 2 value\n");
        payload.u8v(static_cast<u8>(Stage::Initialise)).text(header + initialise);
        const std::vector<std::string> referenced =
            emitter == "smoke" ? modules : std::vector<std::string>{};
        payload.u32v(static_cast<u32>(referenced.size()));
        for (const std::string& module : referenced) {
            payload.text(module);
        }
        payload.u32v(0);            // interfaces
        payload.u32v(256).u32v(0);  // capacity, attributes
    }
    payload.u32v(0);  // parameters
    payload.u32v(0);  // channels
    payload.u32v(static_cast<u32>(mapped.size()));
    for (const std::string& name : mapped) {
        payload.text(name).text(std::string(directory) + name + ".cyvfxmodule");
    }
    return payload.envelope("cyvfxdoc 1\n");
}

/// Writes a constant `value` to every live particle's size in Update.
[[nodiscard]] ModuleSpec shrink_module(std::string name = "shrink",
                                       const std::string& value = "0.5") {
    ModuleSpec spec;
    spec.name = std::move(name);
    spec.body = "node 1 vfx.constant\nprop 1 value " + value +
                "\n# layout 1 10 20\n"
                "node 2 vfx.set_attribute\nprop 2 attribute size\nlink 1 out 2 value\n";
    return spec;
}

/// One cook, from documents to a `CompiledSystem`, the way the editor service runs it.
struct Cook {
    Cook() : nodes(allocator()), interfaces(allocator()), sink(allocator()), report(allocator()) {
        ok = register_vfx_nodes(nodes).has_value() &&
             register_builtin_interfaces(interfaces).has_value();
    }

    [[nodiscard]] bool run(const std::string& system, const std::vector<ModuleSpec>& modules) {
        texts.clear();
        for (const ModuleSpec& module : modules) {
            texts.emplace_back(module.name, module_text(module));
        }
        auto asset = read_authoring_document(system, allocator());
        if (!asset) {
            return false;
        }
        std::vector<ModuleSource> sources;
        sources.reserve(texts.size());
        for (const auto& [name, text] : texts) {
            sources.push_back({Name::intern(name), text});
        }
        if (!resolve_authoring_modules(*asset, sources, sink, report, allocator())) {
            return false;
        }
        asset->resolve(nodes);
        auto compiled = compile_system(*asset, nodes, interfaces, CompileOptions{}, sink, report);
        if (!compiled) {
            return false;
        }
        cooked = std::make_unique<CompiledSystem>(std::move(*compiled));
        return true;
    }

    [[nodiscard]] std::string slang() const {
        std::string out;
        for (const CompiledEmitter& emitter : cooked->emitters()) {
            for (const graph::GeneratedSource& source : emitter.sources()) {
                out.append(source.text.data(), source.text.size());
            }
        }
        return out;
    }

    [[nodiscard]] const graph::Diagnostic* diagnostic(std::string_view code) const {
        for (const graph::Diagnostic& entry : sink.entries()) {
            if (std::string_view(entry.code) == code) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const DiagnosticScope* scope_of(const graph::Diagnostic* entry) const {
        for (const DiagnosticScope& scope : report.diagnostic_scopes) {
            if (&sink.entries()[scope.diagnostic_index] == entry) {
                return &scope;
            }
        }
        return nullptr;
    }

    graph::NodeRegistry nodes;
    DataInterfaceRegistry interfaces;
    graph::DiagnosticSink sink;
    CompileReport report;
    std::vector<std::pair<std::string, std::string>> texts;
    std::unique_ptr<CompiledSystem> cooked;
    bool ok = false;
};

/// The smallest published particle size, across both emitters, after a short simulation.
[[nodiscard]] f32 simulated_size(const CompiledSystem& system) {
    SimulationWorld world(allocator());
    WorldDescription description;
    description.pool_bytes = 1ULL * 1024ULL * 1024ULL;
    description.max_instances = 8;
    CY_REQUIRE(world.initialize(description).has_value());
    CY_REQUIRE(world.play(system, EffectSpawn{}).has_value());
    StepReport stepped;
    for (u32 frame = 0; frame < 4; ++frame) {
        CY_REQUIRE(world.step(1.0F / 60.0F, stepped).has_value());
    }
    CY_REQUIRE(stepped.live_particles > 0U);
    Array<rendering::particles::ParticleInstance> particles(allocator());
    PublishReport published;
    CY_REQUIRE(
        publish_sprites(world, Vec3{0.0F, 0.0F, 10.0F}, 64, particles, published).has_value());
    CY_REQUIRE(!particles.empty());
    f32 smallest = particles[0].size;
    for (const auto& particle : particles) {
        smallest = particle.size < smallest ? particle.size : smallest;
    }
    return smallest;
}

/// Scoped to `smoke`, the emitter that referenced the module, and naming the module.
void check_names_emitter_and_module(const Cook& cook, std::string_view code,
                                    std::string_view module) {
    const graph::Diagnostic* entry = cook.diagnostic(code);
    CY_CHECK(entry != nullptr);
    if (entry == nullptr) {
        return;
    }
    CY_CHECK_EQ(entry->detail, Name::intern(std::string(module)));
    const DiagnosticScope* scope = cook.scope_of(entry);
    CY_CHECK(scope != nullptr);
    if (scope != nullptr) {
        CY_CHECK_EQ(scope->emitter_index, 1U);
    }
}

}  // namespace

CY_TEST_CASE("a system referencing a saved module compiles with the module's nodes contributing") {
    Cook without;
    CY_REQUIRE(without.ok);
    CY_REQUIRE(without.run(system_text({}, {}), {}));

    Cook with;
    CY_REQUIRE(with.ok);
    CY_REQUIRE(with.run(system_text({"shrink"}, {"shrink"}), {shrink_module()}));

    // The module's Update write is a kernel of its own: neither emitter authored an Update stage.
    CY_CHECK(without.cooked->emitters()[1].kernel_for(Stage::Update) == nullptr);
    CY_CHECK(with.cooked->emitters()[0].kernel_for(Stage::Update) == nullptr);
    CY_REQUIRE(with.cooked->emitters()[1].kernel_for(Stage::Update) != nullptr);
    CY_CHECK(with.slang() != without.slang());
    CY_CHECK(with.cooked->cook_key() != without.cooked->cook_key());

    // And it is what the particles do: size 1 from Initialise, overwritten by the module's 0.5.
    CY_CHECK_EQ(simulated_size(*without.cooked), 1.0F);
    CY_CHECK_EQ(simulated_size(*with.cooked), 0.5F);
}

CY_TEST_CASE("the cook key follows a module's content, transitively, and not its layout or path") {
    ModuleSpec base = shrink_module("base");
    ModuleSpec outer;
    outer.name = "outer";
    outer.uses = {"base"};
    outer.body =
        "node 1 vfx.constant\nprop 1 value 3\nnode 2 vfx.set_attribute\n"
        "prop 2 attribute emission\nlink 1 out 2 value\n";
    const std::string system = system_text({"outer"}, {"outer", "base"});

    Cook first;
    CY_REQUIRE(first.ok);
    CY_REQUIRE(first.run(system, {outer, base}));
    const u64 key = first.cooked->cook_key();

    // A moved node and a module stored somewhere else: neither is content.
    ModuleSpec moved = base;
    moved.body = shrink_module("base").body + "# layout 2 400 400\n";
    Cook cosmetic;
    CY_REQUIRE(cosmetic.ok);
    CY_REQUIRE(cosmetic.run(system, {outer, moved}));
    CY_CHECK_EQ(cosmetic.cooked->cook_key(), key);
    Cook relocated;
    CY_REQUIRE(relocated.ok);
    CY_REQUIRE(
        relocated.run(system_text({"outer"}, {"outer", "base"}, "effects/shared/"), {outer, base}));
    CY_CHECK_EQ(relocated.cooked->cook_key(), key);

    // The USED module's content changes and neither the system nor `outer` does.
    Cook edited;
    CY_REQUIRE(edited.ok);
    CY_REQUIRE(edited.run(system, {outer, shrink_module("base", "0.25")}));
    CY_CHECK(edited.cooked->cook_key() != key);

    // An edit the lowering discards — a node nothing reads — still re-keys, because the key is
    // over the module's content and not only over what reached a kernel.
    ModuleSpec discarded = base;
    discarded.body += "node 9 vfx.constant\nprop 9 value 7\n";
    Cook dead;
    CY_REQUIRE(dead.ok);
    CY_REQUIRE(dead.run(system, {outer, discarded}));
    CY_CHECK_EQ(dead.slang(), first.slang());
    CY_CHECK(dead.cooked->cook_key() != key);
}

CY_TEST_CASE("a missing module fails with a diagnostic naming the emitter and the module") {
    Cook cook;
    CY_REQUIRE(cook.ok);
    CY_CHECK_FALSE(cook.run(system_text({"absent"}, {"absent"}), {}));
    check_names_emitter_and_module(cook, "vfx.module.missing", "absent");

    Cook unmapped;
    CY_REQUIRE(unmapped.ok);
    CY_CHECK_FALSE(unmapped.run(system_text({"absent"}, {}), {}));
    check_names_emitter_and_module(unmapped, "vfx.module.mapping", "absent");
}

CY_TEST_CASE("a module cycle fails with a diagnostic naming the emitter and the module") {
    ModuleSpec first = shrink_module("cycle_a");
    first.uses = {"cycle_b"};
    ModuleSpec second = shrink_module("cycle_b");
    second.uses = {"cycle_a"};
    Cook cook;
    CY_REQUIRE(cook.ok);
    CY_CHECK_FALSE(cook.run(system_text({"cycle_a"}, {"cycle_a", "cycle_b"}), {first, second}));
    check_names_emitter_and_module(cook, "vfx.module.cycle", "cycle_a");
}

CY_TEST_CASE("a stage-incompatible module fails naming the emitter, the module and its node") {
    // A spawn count in a module declared for Update.
    ModuleSpec burst;
    burst.name = "burst";
    burst.body =
        "node 1 vfx.constant\nprop 1 value 8\nnode 7 vfx.spawn_count\nlink 1 out 7 value\n";
    Cook cook;
    CY_REQUIRE(cook.ok);
    CY_CHECK_FALSE(cook.run(system_text({"burst"}, {"burst"}), {burst}));
    check_names_emitter_and_module(cook, "vfx.module.stage", "burst");
    const graph::Diagnostic* offending = cook.diagnostic("vfx.module.stage");
    const DiagnosticScope* scope = offending != nullptr ? cook.scope_of(offending) : nullptr;
    if (offending != nullptr && scope != nullptr) {
        CY_CHECK_EQ(offending->node, 7U);
        CY_CHECK_EQ(scope->stage, Stage::Update);
    }

    // A Spawn module used by an Update module.
    ModuleSpec spawner;
    spawner.name = "spawner";
    spawner.stage = Stage::Spawn;
    spawner.body =
        "node 1 vfx.constant\nprop 1 value 8\nnode 2 vfx.spawn_count\nlink 1 out 2 value\n";
    ModuleSpec user = shrink_module("user");
    user.uses = {"spawner"};
    Cook nested;
    CY_REQUIRE(nested.ok);
    CY_CHECK_FALSE(nested.run(system_text({"user"}, {"user", "spawner"}), {user, spawner}));
    check_names_emitter_and_module(nested, "vfx.module.stage", "spawner");
}

// SPDX-License-Identifier: MIT
// ABI 1.5's `register_system` and `cy::abi::ScriptSystems`. `add-swift-m12-gaps`.
//
// The registry's refusals, then the scheduling claim `swift-scripting` makes: a module's system is
// ordered against a native one "by the same conflict rules", runs in the stage it named and in that
// stage's phase, cannot make a structural change while it runs, and reads a column through
// `world_chunks` with no per-entity call. The run functions here are C++ with C linkage standing in
// for a module's; the reload half, over real C modules, is integration.abi_reload.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/abi/systems.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/system.h>
#include <cy/ecs/world.h>
#include <cy/test/test.h>

#include <cstring>

namespace {

using cy::u32;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

struct Velocity {
    float value;
};

/// What the stand-in run functions saw.
struct Probe {
    u32 runs = 0;
    CyPhase phase = CY_PHASE_NONE;
    CyEntity created = 1;  // what `world_create_entity` returned from inside a run
    CyResult created_error = CY_RESULT_OK;
    u32 rows = 0;
};

Probe g_probe;
CyComponentTypeId g_velocity = CY_COMPONENT_TYPE_INVALID;

extern "C" void probe_run(CyEngine engine, CyWorld world, void* user_data) {
    (void)user_data;
    const CyInterface& iface = table();
    ++g_probe.runs;
    g_probe.phase = engine->game.clock.phase;
    g_probe.created = iface.world_create_entity(world);
    g_probe.created_error = iface.get_last_error_code();
}

/// A module-shaped inner loop: one `world_chunks` call, then the column, with no per-entity call.
extern "C" void accelerate(CyEngine engine, CyWorld world, void* user_data) {
    (void)engine;
    const float step = *static_cast<const float*>(user_data);
    const CyInterface& iface = table();
    CyChunk chunks[4] = {};
    u32 count = 0;
    if (iface.world_chunks(world, g_velocity, chunks, 4, &count) != CY_RESULT_OK) {
        return;
    }
    for (u32 index = 0; index < count; ++index) {
        auto* rows = static_cast<Velocity*>(chunks[index].data);
        for (u32 row = 0; row < chunks[index].entity_count; ++row) {
            rows[row].value += step;
            ++g_probe.rows;
        }
    }
}

void native_reader(const cy::ecs::SystemContext& context) noexcept {
    (void)context;
}

struct Fixture {
    cy::ecs::World world{allocator()};
    cy::abi::World binding{allocator(), world};
    cy::abi::Host host{allocator()};
    cy::ecs::Schedule schedule{world};
    cy::abi::ScriptSystems systems{allocator(), host};

    Fixture() {
        g_probe = Probe{};
        CY_REQUIRE(world.initialize().has_value());
        host.bind_world(&binding);
        static const CyFieldDesc fields[] = {
            {static_cast<uint32_t>(sizeof(CyFieldDesc)), CY_VAR_F32, 0, 4, "value"}};
        CyComponentTypeDesc desc{};
        desc.struct_size = sizeof(desc);
        desc.size = sizeof(Velocity);
        desc.alignment = alignof(Velocity);
        desc.field_count = 1;
        desc.name = "Velocity";
        desc.fields = fields;
        const cy::Expected<CyComponentTypeId, cy::Error> id = binding.register_component(desc);
        CY_REQUIRE(id.has_value());
        g_velocity = id.value();
    }

    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

CySystemDesc system_desc(const char* name, CyStage stage, const CySystemAccess* access, u32 count,
                         void (*run)(CyEngine, CyWorld, void*) = &probe_run,
                         void* user = nullptr) noexcept {
    CySystemDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.stage = stage;
    desc.name = name;
    desc.access = access;
    desc.access_count = count;
    desc.run = run;
    desc.user_data = user;
    return desc;
}

}  // namespace

CY_TEST_CASE("register_system is refused outside N, with no world, and for a malformed desc") {
    Fixture fixture;
    const CyInterface& iface = table();
    const CySystemAccess write{g_velocity, CY_ACCESS_WRITE};
    CySystemDesc desc = system_desc("move", CY_STAGE_SIMULATION, &write, 1);

    {
        const cy::abi::game::PhaseScope fixed(fixture.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_CHECK_EQ(iface.register_system(fixture.engine(), &desc), CY_RESULT_PERMISSION_DENIED);
    }
    CY_CHECK_EQ(iface.register_system(nullptr, &desc), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.register_system(fixture.engine(), nullptr), CY_RESULT_INVALID_ARGUMENT);

    CySystemDesc bad = desc;
    bad.stage = CY_STAGE_RENDER + 1U;
    CY_CHECK_EQ(iface.register_system(fixture.engine(), &bad), CY_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.run = nullptr;
    CY_CHECK_EQ(iface.register_system(fixture.engine(), &bad), CY_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.name = "";
    CY_CHECK_EQ(iface.register_system(fixture.engine(), &bad), CY_RESULT_INVALID_ARGUMENT);

    const CySystemAccess unknown{g_velocity + 1000U, CY_ACCESS_READ};
    bad = system_desc("move", CY_STAGE_SIMULATION, &unknown, 1);
    CY_CHECK_EQ(iface.register_system(fixture.engine(), &bad), CY_RESULT_INVALID_ARGUMENT);
    const CySystemAccess no_mode{g_velocity, 7U};
    bad = system_desc("move", CY_STAGE_SIMULATION, &no_mode, 1);
    CY_CHECK_EQ(iface.register_system(fixture.engine(), &bad), CY_RESULT_INVALID_ARGUMENT);
    const CySystemAccess twice[] = {{g_velocity, CY_ACCESS_READ}, {g_velocity, CY_ACCESS_WRITE}};
    bad = system_desc("move", CY_STAGE_SIMULATION, twice, 2);
    CY_CHECK_EQ(iface.register_system(fixture.engine(), &bad), CY_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.access = nullptr;
    CY_CHECK_EQ(iface.register_system(fixture.engine(), &bad), CY_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.struct_size = 2U;
    CY_CHECK_EQ(iface.register_system(fixture.engine(), &bad), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK(fixture.host.systems.empty());

    fixture.host.bind_world(nullptr);
    CY_CHECK_EQ(iface.register_system(fixture.engine(), &desc), CY_RESULT_UNAVAILABLE);
    fixture.host.bind_world(&fixture.binding);
    CY_CHECK_EQ(iface.register_system(fixture.engine(), &desc), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.host.systems.size(), 1U);
    // The same name in the same generation replaces rather than duplicates.
    CY_CHECK_EQ(iface.register_system(fixture.engine(), &desc), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.host.systems.size(), 1U);
}

CY_TEST_CASE("a script system is ordered against a native one by the same conflict rules") {
    Fixture fixture;
    const CyInterface& iface = table();
    const CySystemAccess write{g_velocity, CY_ACCESS_WRITE};
    const CySystemAccess read{g_velocity, CY_ACCESS_READ};
    CySystemDesc writer = system_desc("script.write", CY_STAGE_SIMULATION, &write, 1);
    CySystemDesc reader = system_desc("script.read", CY_STAGE_SIMULATION, &read, 1);
    CY_REQUIRE_EQ(iface.register_system(fixture.engine(), &writer), CY_RESULT_OK);
    CY_REQUIRE_EQ(iface.register_system(fixture.engine(), &reader), CY_RESULT_OK);

    // A native system reading the same component, registered BEFORE the script ones are installed.
    cy::ecs::SystemDesc native;
    native.name = "native.read";
    native.body = &native_reader;
    CY_REQUIRE(native.access.read(g_velocity).has_value());
    const cy::Expected<cy::ecs::SystemId, cy::Error> native_id =
        fixture.schedule.add(cy::ecs::Stage::Simulation, native);
    CY_REQUIRE(native_id.has_value());

    const cy::Expected<u32, cy::Error> installed = fixture.systems.install(fixture.schedule);
    CY_REQUIRE(installed.has_value());
    CY_CHECK_EQ(installed.value(), 2U);
    const cy::ecs::SystemId script_write = fixture.systems.id_of("script.write");
    const cy::ecs::SystemId script_read = fixture.systems.id_of("script.read");
    CY_REQUIRE(script_write != cy::ecs::kInvalidSystem);

    // Write against read conflicts, whichever side is the script; two readers do not.
    cy::jobs::AccessConflict why;
    CY_CHECK(fixture.schedule.conflicts(cy::ecs::Stage::Simulation, native_id.value(), script_write,
                                        why));
    CY_CHECK(
        fixture.schedule.conflicts(cy::ecs::Stage::Simulation, script_write, script_read, why));
    CY_CHECK_FALSE(fixture.schedule.conflicts(cy::ecs::Stage::Simulation, native_id.value(),
                                              script_read, why));
    CY_CHECK(fixture.schedule.ordered_before(cy::ecs::Stage::Simulation, native_id.value(),
                                             script_write));
    CY_CHECK(
        fixture.schedule.ordered_before(cy::ecs::Stage::Simulation, script_write, script_read));
    // A second install with nothing new adds nothing.
    const cy::Expected<u32, cy::Error> again = fixture.systems.install(fixture.schedule);
    CY_REQUIRE(again.has_value());
    CY_CHECK_EQ(again.value(), 0U);
}

CY_TEST_CASE("a script system runs in its stage's phase and cannot change the world's structure") {
    Fixture fixture;
    const CyInterface& iface = table();
    CySystemDesc fixed = system_desc("fixed", CY_STAGE_PRE_SIMULATION, nullptr, 0);
    CySystemDesc frame = system_desc("frame", CY_STAGE_UI, nullptr, 0);
    CY_REQUIRE_EQ(iface.register_system(fixture.engine(), &fixed), CY_RESULT_OK);
    CY_REQUIRE_EQ(iface.register_system(fixture.engine(), &frame), CY_RESULT_OK);
    CY_REQUIRE(fixture.systems.install(fixture.schedule).has_value());

    CY_REQUIRE(
        fixture.systems.run(fixture.schedule, cy::ecs::Stage::PreSimulation, nullptr).has_value());
    CY_CHECK_EQ(g_probe.runs, 1U);
    CY_CHECK_EQ(g_probe.phase, CY_PHASE_FIXED_UPDATE);
    // The world is iterating while a body runs: a structural entry is refused, not performed.
    CY_CHECK_EQ(g_probe.created, CY_ENTITY_NULL);
    CY_CHECK_EQ(g_probe.created_error, CY_RESULT_UNAVAILABLE);

    CY_REQUIRE(fixture.systems.run(fixture.schedule, cy::ecs::Stage::UI, nullptr).has_value());
    CY_CHECK_EQ(g_probe.runs, 2U);
    CY_CHECK_EQ(g_probe.phase, CY_PHASE_FRAME_UPDATE);
    // And the phase is restored once the stage is over.
    CY_CHECK_EQ(fixture.host.game.clock.phase, CY_PHASE_NONE);
    // A stage with no script system in it runs none.
    CY_REQUIRE(
        fixture.systems.run(fixture.schedule, cy::ecs::Stage::Simulation, nullptr).has_value());
    CY_CHECK_EQ(g_probe.runs, 2U);
    CY_CHECK_EQ(fixture.systems.runs("fixed"), 1U);
    CY_CHECK_EQ(fixture.systems.runs("frame"), 1U);
}

CY_TEST_CASE("a script system writes a column it declared through world_chunks") {
    Fixture fixture;
    const CyInterface& iface = table();
    for (int index = 0; index < 5; ++index) {
        const cy::Expected<cy::ecs::Entity, cy::Error> entity = fixture.world.create();
        CY_REQUIRE(entity.has_value());
        const Velocity zero{0.0F};
        CY_REQUIRE(fixture.world.add(entity.value(), g_velocity, &zero).has_value());
    }
    static float step = 0.5F;
    const CySystemAccess write{g_velocity, CY_ACCESS_WRITE};
    CySystemDesc desc =
        system_desc("accelerate", CY_STAGE_SIMULATION, &write, 1, &accelerate, &step);
    CY_REQUIRE_EQ(iface.register_system(fixture.engine(), &desc), CY_RESULT_OK);
    CY_REQUIRE(fixture.systems.install(fixture.schedule).has_value());
    CY_REQUIRE(
        fixture.systems.run(fixture.schedule, cy::ecs::Stage::Simulation, nullptr).has_value());
    CY_REQUIRE(
        fixture.systems.run(fixture.schedule, cy::ecs::Stage::Simulation, nullptr).has_value());
    CY_CHECK_EQ(g_probe.rows, 10U);
    u32 checked = 0;
    CyChunk chunk{};
    u32 count = 0;
    CY_REQUIRE_EQ(iface.world_chunks(&fixture.binding, g_velocity, &chunk, 1, &count),
                  CY_RESULT_OK);
    CY_REQUIRE_EQ(count, 1U);
    for (u32 row = 0; row < chunk.entity_count; ++row) {
        CY_CHECK_EQ(static_cast<const Velocity*>(chunk.data)[row].value, 1.0F);
        ++checked;
    }
    CY_CHECK_EQ(checked, 5U);
}

CY_TEST_CASE("a later generation may re-register a system only with the same declaration") {
    Fixture fixture;
    const CyInterface& iface = table();
    const CySystemAccess write{g_velocity, CY_ACCESS_WRITE};
    const CySystemAccess read{g_velocity, CY_ACCESS_READ};
    CySystemDesc first = system_desc("move", CY_STAGE_SIMULATION, &write, 1);
    CY_REQUIRE_EQ(iface.register_system(fixture.engine(), &first), CY_RESULT_OK);

    fixture.host.open_generation();
    // The same declaration in the next generation is a new record, which `find_system` now finds.
    CY_CHECK_EQ(iface.register_system(fixture.engine(), &first), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.host.systems.size(), 2U);
    CY_REQUIRE(fixture.host.find_system("move") != nullptr);
    CY_CHECK_EQ(fixture.host.find_system("move")->generation, 1U);

    fixture.host.open_generation();
    CySystemDesc other_stage = system_desc("move", CY_STAGE_FRAME, &write, 1);
    CY_CHECK_EQ(iface.register_system(fixture.engine(), &other_stage), CY_RESULT_UNSUPPORTED);
    CySystemDesc other_access = system_desc("move", CY_STAGE_SIMULATION, &read, 1);
    CY_CHECK_EQ(iface.register_system(fixture.engine(), &other_access), CY_RESULT_UNSUPPORTED);
    CY_CHECK_EQ(fixture.host.refused_systems, 2U);
    CY_CHECK(fixture.host.find_system("move") == nullptr);

    // Abandoning the generation forgets its refusals and its records; generation 1's is current.
    fixture.host.abandon_generation();
    CY_CHECK_EQ(fixture.host.refused_systems, 0U);
    CY_REQUIRE(fixture.host.find_system("move") != nullptr);
    CY_CHECK_EQ(fixture.host.find_system("move")->generation, 1U);
}

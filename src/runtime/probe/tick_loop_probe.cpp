// The tick-loop probe. Tasks 4.1.2 and 4.2.6.
//
// It starts a runtime with a simulation attached, runs a fixed number of frames in `--fixed-step`
// mode, and prints what the run produced:
//
//   frames <n> ticks <n> epoch <n> tick <n> version <n>
//   hash <16 hex digits>
//   entities <n> fields <n> undeclared <n>
//
// A test runs it several times and compares the output. Across *processes*, not across iterations
// of one loop, and that is the whole point: `simulation-and-determinism`'s reproducibility claim is
// about a fresh address space — a hash that depended on an allocation address, a container's
// iteration order or a per-process hash seed would differ between runs rather than being reproduced
// faithfully by all of them. It is the same argument tests/smoke/test_startup_order.cpp makes about
// the startup sequence, applied to the state hash.
//
// `--seed <n>` changes the session seed, which must change the hash — the random source is a hashed
// state provider, so two sessions with different seeds are different states before a single random
// value has been drawn.
//
// Headless, with no window and no trace: this measures the simulation, not what the display does.
// Fixed-step mode, because a realtime clock would make the tick count a function of how fast this
// machine is, and then the comparison would be measuring the machine.

#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/type_info.h>
#include <cy/ecs/query.h>
#include <cy/platform/headless_display_server.h>
#include <cy/platform/sdl3_platform.h>
#include <cy/runtime/runtime.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {

/// One reflected component with two authoritative fields and one derived one. Hand-written for the
/// reason src/runtime/tests/fixtures.h gives, and duplicated rather than shared because a probe is
/// a program and a test fixture header is a test's.
struct Body {
    cy::f32 x = 0.0F;
    cy::f32 y = 0.0F;
    cy::u32 scratch = 0;  // derived: recomputed, never hashed
};

const cy::reflect::TypeInfo& body_type() noexcept {
    static const cy::reflect::FieldInfo fields[] = {
        [] {
            cy::reflect::FieldInfo field;
            field.name = "x";
            field.id = cy::reflect::FieldId(1);
            field.kind = cy::reflect::FieldKind::F32;
            field.offset = offsetof(Body, x);
            field.size = sizeof(cy::f32);
            field.attributes.persistence = cy::reflect::PersistenceKind::RuntimeState;
            return field;
        }(),
        [] {
            cy::reflect::FieldInfo field;
            field.name = "y";
            field.id = cy::reflect::FieldId(2);
            field.kind = cy::reflect::FieldKind::F32;
            field.offset = offsetof(Body, y);
            field.size = sizeof(cy::f32);
            field.attributes.persistence = cy::reflect::PersistenceKind::RuntimeState;
            return field;
        }(),
        [] {
            cy::reflect::FieldInfo field;
            field.name = "scratch";
            field.id = cy::reflect::FieldId(3);
            field.kind = cy::reflect::FieldKind::U32;
            field.offset = offsetof(Body, scratch);
            field.size = sizeof(cy::u32);
            field.attributes.persistence = cy::reflect::PersistenceKind::Derived;
            return field;
        }(),
    };
    static const cy::reflect::TypeInfo info = [] {
        cy::reflect::TypeInfo type;
        type.name = "cy::probe::Body";
        type.id = cy::reflect::TypeId(9201);
        type.size = sizeof(Body);
        type.alignment = alignof(Body);
        type.trivially_relocatable = true;
        type.fields = fields;
        type.field_count = 3;
        return type;
    }();
    return info;
}

/// What the integrating system needs. Held beside the simulation rather than inside it, because a
/// system's state is the game's and the runtime does not own one.
struct ProbeState {
    cy::ecs::ComponentTypeId body = 0;
    cy::determinism::RandomStream drift;
    const cy::runtime::Simulation* simulation = nullptr;
    /// The query *is* the access declaration — see `<cy/ecs/system.h>`'s header — so it is built
    /// once and both the system's declaration and its body come from the same object. Two of them
    /// could drift, and nothing would catch the drift.
    cy::ecs::Query* query = nullptr;
};

ProbeState g_state;

/// A system that integrates, and draws its jitter from a named seeded stream keyed by the tick and
/// the entity. Every property `simulation-and-determinism` asks of a deterministic system is
/// visible in these six lines: time comes from the clock, randomness comes from a named stream, and
/// the draw is keyed by (stream, point, entity, index) rather than by a hidden counter.
void integrate(const cy::ecs::SystemContext& /*context*/) noexcept {
    const cy::determinism::SimulationPoint now = g_state.simulation->clock().now();
    const cy::f32 step = g_state.simulation->clock().delta_seconds();

    (void)g_state.query->for_each_chunk([&](cy::ecs::QueryChunk& chunk) noexcept {
        const cy::Span<const cy::ecs::Entity> keys = chunk.entities();
        const cy::Span<Body> bodies = chunk.write<Body>(g_state.body);
        for (cy::usize row = 0; row < bodies.size(); ++row) {
            // Time from the clock, randomness from a named stream keyed by (point, entity, index).
            // Every property `simulation-and-determinism` asks of a deterministic system is visible
            // in these three lines, and none of them is a convention: there is no wall clock
            // reachable from here and no generator with a hidden counter.
            const cy::f32 jitter = g_state.drift.unit_float(now, keys[row].index(), 0) - 0.5F;
            bodies[row].x += step * (1.0F + jitter);
            bodies[row].y += step * jitter;
            bodies[row].scratch = static_cast<cy::u32>(now.tick);
        }
    });
}

int fail(const char* what, const cy::Error& error) {
    std::fprintf(stderr, "%s: %s\n", what, error.message);
    return 1;
}

}  // namespace

int main(int argument_count, char** arguments) {
    cy::u64 seed = 1;
    cy::u32 frames = 64;
    cy::u32 entities = 16;
    for (int index = 1; index < argument_count; ++index) {
        if (std::strcmp(arguments[index], "--seed") == 0 && index + 1 < argument_count) {
            seed = std::strtoull(arguments[++index], nullptr, 10);
        } else if (std::strcmp(arguments[index], "--frames") == 0 && index + 1 < argument_count) {
            frames = static_cast<cy::u32>(std::strtoul(arguments[++index], nullptr, 10));
        }
    }

    cy::Sdl3Platform platform;
    if (const cy::Status started = platform.initialise(argument_count, arguments); !started) {
        return fail("platform", started.error());
    }
    cy::HeadlessDisplayServer display;
    if (const cy::Status started = display.initialise(); !started) {
        return fail("display", started.error());
    }

    cy::runtime::SimulationConfig simulation_config;
    simulation_config.world_name = "probe";
    simulation_config.session_seed = seed;
    simulation_config.create_scene_tree = false;
    simulation_config.clock.mode = cy::determinism::TickMode::FixedStep;
    simulation_config.clock.fixed_ticks_per_frame = 1;

    cy::runtime::Simulation simulation(cy::system_allocator(cy::MemoryDomain::Ecs),
                                       simulation_config);
    if (const cy::Status ready = simulation.initialize(); !ready) {
        return fail("simulation", ready.error());
    }

    const auto registered = simulation.world().components().register_reflected(body_type());
    if (!registered) {
        return fail("component", registered.error());
    }
    g_state.body = registered.value();
    g_state.drift = simulation.random().stream("probe.drift");
    g_state.simulation = &simulation;

    cy::ecs::QueryDesc desc(simulation.world().allocator());
    if (const cy::Status declared = desc.write(g_state.body); !declared) {
        return fail("query", declared.error());
    }
    const cy::jobs::AccessSet access = desc.access();
    cy::ecs::Query query(simulation.world(), std::move(desc));
    g_state.query = &query;

    const cy::ecs::ComponentTypeId components[] = {g_state.body};
    for (cy::u32 index = 0; index < entities; ++index) {
        const auto entity =
            simulation.world().create(cy::Span<const cy::ecs::ComponentTypeId>(components, 1));
        if (!entity) {
            return fail("entity", entity.error());
        }
        const Body body{static_cast<cy::f32>(index), 0.0F, 0};
        if (const cy::Status set = simulation.world().set(entity.value(), g_state.body, body);
            !set) {
            return fail("component value", set.error());
        }
    }

    cy::ecs::SystemDesc system;
    system.name = "probe.integrate";
    system.body = &integrate;
    system.access = access;
    if (const auto added = simulation.schedule().add(cy::ecs::Stage::Simulation, system); !added) {
        return fail("system", added.error());
    }

    cy::RuntimeConfig config;
    config.platform = &platform;
    config.display = &display;
    config.create_window = false;
    config.create_surface = false;
    config.install_crash_handler = false;
    config.tick_mode = cy::determinism::TickMode::FixedStep;
    config.fixed_ticks_per_frame = 1;
    config.simulation = &simulation;

    cy::Runtime runtime;
    if (const cy::Status started = runtime.startup(config); !started) {
        return fail("runtime", started.error());
    }
    for (cy::u32 frame = 0; frame < frames; ++frame) {
        if (const cy::Status ticked = runtime.tick(); !ticked) {
            return fail("tick", ticked.error());
        }
    }

    const auto hash = simulation.hash_now();
    if (!hash) {
        return fail("hash", hash.error());
    }
    const cy::runtime::WorldHashReport& report = simulation.last_hash_report();
    const cy::FrameStats stats = runtime.frame();

    std::fprintf(stdout, "frames %llu ticks %llu epoch %u tick %llu version %llu\n",
                 static_cast<unsigned long long>(stats.frame_index),
                 static_cast<unsigned long long>(stats.total_ticks), stats.committed.epoch.value,
                 static_cast<unsigned long long>(stats.committed.tick),
                 static_cast<unsigned long long>(stats.state_version));
    std::fprintf(stdout, "hash %016llx\n", static_cast<unsigned long long>(hash.value()));
    std::fprintf(stdout, "entities %u fields %u undeclared %u\n", report.entities_hashed,
                 report.fields_hashed, report.subjects_undeclared);

    runtime.shutdown();
    display.shutdown();
    platform.shutdown();
    return 0;
}

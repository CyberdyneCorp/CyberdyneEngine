// The unified simulation world, the shared pool and its reservations, the global scheduler, the
// bounded readback and the budget under overload. M8.c tasks 2.3 through 2.6.
//
// INTEGRATION TIER. Every case here builds a world and cooks an effect — the taxonomy names both as
// what does not fit the unit tier's millisecond, and this milestone's brief names a particle system
// specifically.

#include "effects.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/firewall.h>
#include <cy/test/test.h>
#include <cy/vfx/runtime.h>
#include <cy/vfx/world.h>

#include <cstdio>
#include <ctime>

using namespace cy;
using namespace cy::vfx;
using namespace cy::vfx_test;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// A cooked plume, kept alive for as long as the world that plays it.
struct Cooked {
    explicit Cooked(u32 emitters = 1, u32 capacity = 256) noexcept
        : sink(allocator()), report(allocator()) {
        auto compiled = cook_plume(allocator(), sink, report, options, emitters, capacity);
        ok = compiled.has_value();
        if (ok) {
            system = Expected<CompiledSystem, Error>(std::move(compiled.value()));
        }
    }

    [[nodiscard]] const CompiledSystem& operator*() const noexcept { return system.value(); }

    graph::DiagnosticSink sink;
    CompileReport report;
    CompileOptions options;
    Expected<CompiledSystem, Error> system = fail(ErrorCode::Unavailable, "not cooked");
    bool ok = false;
};

/// This thread's own CPU time. Wall clock would measure the machine's other agents rather than this
/// step, which is the defect `cy/test/test.h` records about the taxonomy's budget and fixed there
/// for the same reason.
[[nodiscard]] u64 thread_cpu_ns() noexcept {
    timespec now{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) {
        return 0;
    }
    return (static_cast<u64>(now.tv_sec) * 1000000000ULL) + static_cast<u64>(now.tv_nsec);
}

[[nodiscard]] WorldDescription small_world() noexcept {
    WorldDescription description;
    description.pool_bytes = 4ULL * 1024ULL * 1024ULL;
    description.max_instances = 512;
    return description;
}

}  // namespace

CY_TEST_CASE("an effect plays, spawns, ages and dies, and the step report says so") {
    Cooked cooked;
    CY_REQUIRE(cooked.ok);
    SimulationWorld world(allocator());
    CY_REQUIRE(world.initialize(small_world()).has_value());

    EffectSpawn spawn;
    spawn.position = Vec3{0.0F, 0.0F, -4.0F};
    auto handle = world.play(*cooked, spawn);
    CY_REQUIRE(handle.has_value());

    StepReport report;
    u32 total_spawned = 0;
    for (u32 frame = 0; frame < 30U; ++frame) {
        CY_REQUIRE(world.step(1.0F / 60.0F, report).has_value());
        total_spawned += report.spawned;
    }
    // CUMULATIVE, because `StepReport` is one step's numbers and by frame 30 the block is full:
    // sixteen a step into a 256-particle block fills it in sixteen frames, and nothing has reached
    // its 1.6-second lifetime yet. A per-frame assertion here would be asserting that the pool
    // never fills, which is the opposite of what a bounded pool is for.
    CY_CHECK_GT(total_spawned, 0U);
    CY_CHECK_GT(report.live_particles, 0U);
    CY_CHECK_EQ(report.instances, 1U);
    std::fprintf(stderr, "after 30 frames: %u live, %u spawned this frame, %u killed\n",
                 report.live_particles, report.spawned, report.killed);

    // Stop and let it complete: spawning ceases and the population drains as lifetimes expire.
    CY_REQUIRE(world.stop(*handle, true).has_value());
    u32 drained = 0;
    for (u32 frame = 0; frame < 240U && drained == 0; ++frame) {
        CY_REQUIRE(world.step(1.0F / 60.0F, report).has_value());
        drained = report.live_particles == 0 ? 1U : 0U;
    }
    CY_CHECK_EQ(drained, 1U);
    CY_CHECK_EQ(report.spawned, 0U);
}

CY_TEST_CASE("particles move: the update kernel's arithmetic is visible in the stored attributes") {
    Cooked cooked;
    CY_REQUIRE(cooked.ok);
    SimulationWorld world(allocator());
    CY_REQUIRE(world.initialize(small_world()).has_value());
    EffectSpawn spawn;
    auto handle = world.play(*cooked, spawn);
    CY_REQUIRE(handle.has_value());

    StepReport report;
    CY_REQUIRE(world.step(1.0F / 60.0F, report).has_value());
    const EffectInstance* instance = world.find(*handle);
    CY_REQUIRE(instance != nullptr);

    // The fused initialise draws an upward speed in [2.6, 6.2] and IMMEDIATELY advances it by
    // gravity for one sub-step, so the first frame's velocity is the draw minus 9.81/60 rather than
    // the draw itself. That offset is the fusion happening: unfused, the update would run over the
    // live set on the NEXT step and the first frame's velocity would be the draw exactly.
    const f32 velocity_y = world.read_attribute(*instance, 0, 0, Name::intern("velocity"), 1);
    CY_CHECK_GT(velocity_y, 2.6F - (9.81F / 60.0F) - 0.001F);
    CY_CHECK_LT(velocity_y, 6.2F - (9.81F / 60.0F) + 0.001F);
    // The draw is deterministic, so the offset is checkable exactly: the same particle's velocity
    // one sub-step later is another 9.81/60 lower.
    StepReport again;
    CY_REQUIRE(world.step(1.0F / 60.0F, again).has_value());
    const f32 later = world.read_attribute(*instance, 0, 0, Name::intern("velocity"), 1);
    CY_CHECK_NEAR(later, velocity_y - (9.81F / 60.0F), 0.001F);

    for (u32 frame = 0; frame < 20U; ++frame) {
        CY_REQUIRE(world.step(1.0F / 60.0F, report).has_value());
    }
    const f32 age = world.read_attribute(*instance, 0, 0, Name::intern("age"), 0);
    CY_CHECK_GT(age, 0.0F);
    // `size` fades from 0.22 towards 0.02 over the lifetime, so it is strictly below its initial
    // value once the particle has aged.
    const f32 size = world.read_attribute(*instance, 0, 0, Name::intern("size"), 0);
    CY_CHECK_LT(size, 0.22F);
    CY_CHECK_GT(size, 0.0F);
}

CY_TEST_CASE("a quantised attribute comes back quantised out of the simulation") {
    // The layout is not a report: `color` was declared at a tolerance of 1/255 and the compiler
    // chose eight bits, so what the simulation stores and what it reads back differ by less than a
    // step and are not the authored float.
    Cooked cooked;
    CY_REQUIRE(cooked.ok);
    SimulationWorld world(allocator());
    CY_REQUIRE(world.initialize(small_world()).has_value());
    EffectSpawn spawn;
    auto handle = world.play(*cooked, spawn);
    CY_REQUIRE(handle.has_value());
    StepReport report;
    CY_REQUIRE(world.step(1.0F / 60.0F, report).has_value());
    const EffectInstance* instance = world.find(*handle);
    CY_REQUIRE(instance != nullptr);

    const f32 green = world.read_attribute(*instance, 0, 0, Name::intern("color"), 1);
    CY_CHECK_NE(green, 0.42F);
    CY_CHECK_NEAR(green, 0.42F, 1.0F / 255.0F);
}

CY_TEST_CASE("many instances of one effect are merged into few dispatches") {
    // `vfx-system`: "WHEN 400 instances of the same explosion effect are active THEN they SHALL be
    // simulated by a SMALL NUMBER of merged dispatches, not 400 separate ones."
    Cooked cooked(1, 32);
    CY_REQUIRE(cooked.ok);
    SimulationWorld world(allocator());
    WorldDescription description = small_world();
    description.pool_bytes = 16ULL * 1024ULL * 1024ULL;
    CY_REQUIRE(world.initialize(description).has_value());

    for (u32 which = 0; which < 400U; ++which) {
        EffectSpawn spawn;
        spawn.position = Vec3{static_cast<f32>(which % 20U), 0.0F, static_cast<f32>(which) / 20.0F};
        CY_REQUIRE(world.play(*cooked, spawn).has_value());
    }
    StepReport report;
    CY_REQUIRE(world.step(1.0F / 60.0F, report).has_value());
    std::fprintf(stderr, "400 instances: %u dispatches unmerged, %u merged\n",
                 report.dispatches_unmerged, report.dispatches_merged);
    CY_CHECK_GE(report.dispatches_unmerged, 400U);
    // Three kernels — spawn, the fused initialise, and update — one group each, however many
    // instances share them.
    CY_CHECK_LE(report.dispatches_merged, 8U);
    CY_CHECK_GT(report.dispatches_merged, 0U);
}

CY_TEST_CASE("the reservation protects Critical from a decorative effect that wants everything") {
    // `vfx-system`: "WHEN decorative effects request more particles than remain THEN the
    // reservation for Critical effects SHALL remain available, and the decorative request SHALL be
    // reduced."
    ParticlePool pool(allocator());
    const f32 reserved[kImportanceCount] = {0.30F, 0.20F, 0.10F, 0.0F};
    CY_REQUIRE(pool.initialize(1000000, reserved).has_value());

    const u64 critical_reservation = pool.report().reserved_bytes[0];
    CY_REQUIRE(critical_reservation > 0U);

    // A decorative effect asks for the whole pool, one byte a particle.
    auto decorative = pool.acquire(ImportanceClass::Decorative, 1000000, 1);
    CY_REQUIRE(decorative.has_value());
    CY_CHECK_LT(decorative->particles, 1000000U);
    CY_CHECK_GT(pool.report().shortfall_particles, 0U);
    CY_CHECK_EQ(pool.report().reduced_requests, 1U);

    // And Critical can still have every byte of its reservation.
    auto critical =
        pool.acquire(ImportanceClass::Critical, static_cast<u32>(critical_reservation), 1);
    CY_REQUIRE(critical.has_value());
    CY_CHECK_EQ(critical->particles, static_cast<u32>(critical_reservation));
    std::fprintf(stderr, "decorative got %u of 1000000; critical then got its full %llu\n",
                 decorative->particles, static_cast<unsigned long long>(critical_reservation));
}

CY_TEST_CASE("pool exhaustion reduces a request rather than failing the frame") {
    ParticlePool pool(allocator());
    const f32 none[kImportanceCount] = {0.0F, 0.0F, 0.0F, 0.0F};
    CY_REQUIRE(pool.initialize(4096, none).has_value());
    auto first = pool.acquire(ImportanceClass::Ambient, 4096, 1);
    CY_REQUIRE(first.has_value());
    CY_CHECK_EQ(first->particles, 4096U);
    // The pool is empty. A second request is granted zero particles and reports the shortfall — it
    // is NOT an error, because "rather than overwriting live particles or FAILING THE FRAME".
    auto second = pool.acquire(ImportanceClass::Ambient, 100, 1);
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(second->particles, 0U);
    CY_CHECK_EQ(pool.report().shortfall_particles, 100U);
    // And a release makes the room available again.
    pool.release(*first);
    auto third = pool.acquire(ImportanceClass::Ambient, 100, 1);
    CY_REQUIRE(third.has_value());
    CY_CHECK_EQ(third->particles, 100U);
}

CY_TEST_CASE("decoupled simulation frequency: a low-rate effect steps less often than the frame") {
    Cooked cooked;
    CY_REQUIRE(cooked.ok);
    SimulationWorld world(allocator());
    CY_REQUIRE(world.initialize(small_world()).has_value());

    EffectSpawn fast;
    fast.simulation_hz = 60.0F;
    CY_REQUIRE(world.play(*cooked, fast).has_value());

    StepReport at_sixty;
    for (u32 frame = 0; frame < 60U; ++frame) {
        CY_REQUIRE(world.step(1.0F / 120.0F, at_sixty).has_value());
    }

    SimulationWorld slow_world(allocator());
    CY_REQUIRE(slow_world.initialize(small_world()).has_value());
    EffectSpawn slow;
    // `vfx-system`'s own scenario: 8 Hz simulation while rendering runs far faster.
    slow.simulation_hz = 8.0F;
    CY_REQUIRE(slow_world.play(*cooked, slow).has_value());
    u32 slow_substeps = 0;
    StepReport report;
    for (u32 frame = 0; frame < 60U; ++frame) {
        CY_REQUIRE(slow_world.step(1.0F / 120.0F, report).has_value());
        slow_substeps += report.substeps;
    }
    std::fprintf(stderr, "60 frames at 120 FPS: an 8 Hz effect ran %u sub-steps\n", slow_substeps);
    CY_CHECK_LT(slow_substeps, 10U);
    CY_CHECK_GT(slow_substeps, 0U);
    // And the interpolation factor the renderer would use is inside one step.
    const EffectInstance& instance = slow_world.instances()[0];
    CY_CHECK_GE(instance.interpolation_alpha, 0.0F);
    CY_CHECK_LE(instance.interpolation_alpha, 1.0F);
}

CY_TEST_CASE("every emitter reports which path it ran on, and a fallback is a number") {
    Cooked cooked;
    CY_REQUIRE(cooked.ok);
    SimulationWorld world(allocator());
    CY_REQUIRE(world.initialize(small_world()).has_value());
    EffectSpawn spawn;
    CY_REQUIRE(world.play(*cooked, spawn).has_value());
    StepReport report;
    CY_REQUIRE(world.step(1.0F / 60.0F, report).has_value());
    CY_CHECK_EQ(report.gpu_emitters + report.cpu_emitters, report.active_emitters);
    // On this build the compute dispatch does not exist, so every GPU-preferred emitter is a
    // COUNTED fallback rather than a silent one. When the dispatch lands, this number goes to zero
    // and the case still passes — it asserts the accounting, not the outcome.
    if (!device_dispatch_available()) {
        CY_CHECK_EQ(report.cpu_fallbacks, report.cpu_emitters);
        CY_CHECK_GT(report.cpu_fallbacks, 0U);
    }
    std::fprintf(stderr, "paths: %u gpu, %u cpu, %u of them fallbacks\n", report.gpu_emitters,
                 report.cpu_emitters, report.cpu_fallbacks);
}

// --- The budget under a synthetic overload
// ---------------------------------------------------------

CY_TEST_CASE("the controller holds a target under overload and Critical survives") {
    // `vfx-system`: "WHEN a synthetic scene demands far more VFX than the budget allows THEN the
    // controller SHALL converge to the target and Critical effects SHALL remain at their minimum
    // quality."
    SimulationWorld world(allocator());
    CY_REQUIRE(world.initialize(small_world()).has_value());
    world.budget().set_allocation(2.0F);

    // A synthetic cost model: the frame's VFX time is proportional to what the levers permit. The
    // controller never sees the model, only its output — which is the requirement.
    // The model's floor is 8.0 * the smallest spawn scale = 1.2 ms, which is BELOW the allocation —
    // so convergence is reachable and the assertion below is about the controller rather than about
    // the model. A model whose floor is above the allocation is the other case, and what it
    // produces is `at_reserved_minimum`, which `unit.vfx_values` checks.
    f32 measured = 8.0F;
    for (u32 frame = 0; frame < 240U; ++frame) {
        world.budget().measure(measured);
        world.budget().update(1.0F / 60.0F);
        const BudgetLevers levers = world.budget().levers(ImportanceClass::Decorative);
        measured = 8.0F * levers.spawn_scale;
    }
    std::fprintf(stderr, "overload converged to %.2f ms against a 2.00 ms allocation\n",
                 static_cast<double>(measured));
    CY_CHECK_LE(measured, 2.0F * 1.1F);
    CY_CHECK_GT(world.budget().state().quality[static_cast<u32>(ImportanceClass::Critical)], 0.0F);
}

CY_TEST_CASE(
    "cost is bounded by configuration: four times the population, the same per-unit cost") {
    // The measurement this milestone's brief asks for, in the shape M8.b used for agents and
    // effects: a per-unit cost at four times the population. What is asserted is that the cost is
    // LINEAR in the population — bounded by configuration rather than by the frame.
    const u32 populations[2] = {256, 1024};
    f64 per_particle[2] = {0.0, 0.0};
    for (u32 which = 0; which < 2U; ++which) {
        Cooked cooked(1, populations[which]);
        CY_REQUIRE(cooked.ok);
        SimulationWorld world(allocator());
        WorldDescription description = small_world();
        description.pool_bytes = 16ULL * 1024ULL * 1024ULL;
        CY_REQUIRE(world.initialize(description).has_value());
        EffectSpawn spawn;
        CY_REQUIRE(world.play(*cooked, spawn).has_value());

        StepReport report;
        // Fill the block first: what is measured is the steady state, not the ramp.
        for (u32 frame = 0; frame < 40U; ++frame) {
            CY_REQUIRE(world.step(1.0F / 60.0F, report).has_value());
        }
        const u32 live = report.live_particles;
        CY_REQUIRE(live > 0U);

        const u64 before = thread_cpu_ns();
        for (u32 frame = 0; frame < 30U; ++frame) {
            CY_REQUIRE(world.step(1.0F / 60.0F, report).has_value());
        }
        const u64 elapsed = thread_cpu_ns() - before;
        per_particle[which] = static_cast<f64>(elapsed) / (30.0 * static_cast<f64>(live));
        std::fprintf(stderr, "population %u: %u live, %.1f ns a particle a step\n",
                     populations[which], live, per_particle[which]);
    }
    // Four times the population at no worse than twice the per-unit cost. A quadratic step would
    // land at four times and fail this.
    CY_CHECK_LT(per_particle[1], per_particle[0] * 2.0);
}

CY_TEST_CASE("the controller's levers reach the simulation, and the population is what moves") {
    // MUTATION E OF THIS MILESTONE'S ADVERSARIAL PASS lives here rather than only in `render.vfx`:
    // ignoring `levers_for` in `simulate_instance` left every device assertion about counts green
    // and only the PICTURE went flat. A machine with no GPU must still catch it, so the same claim
    // is a number here.
    const auto populate = [](bool overloaded) noexcept {
        Cooked cooked(1, 256);
        if (!cooked.ok) {
            return 0U;
        }
        SimulationWorld world(allocator());
        if (!world.initialize(small_world())) {
            return 0U;
        }
        EffectSpawn spawn;
        if (!world.play(*cooked, spawn).has_value()) {
            return 0U;
        }
        world.budget().set_allocation(2.0F);
        world.budget().measure(overloaded ? 9.0F : 0.4F);
        // Let the controller reach its floor first: adjustment walks the ranks at a bounded rate,
        // and a plume declared `Important` is the third class it reaches.
        for (u32 frame = 0; frame < 200U; ++frame) {
            world.budget().update(1.0F / 60.0F);
        }
        StepReport report;
        for (u32 frame = 0; frame < 60U; ++frame) {
            world.budget().update(1.0F / 60.0F);
            if (!world.step(1.0F / 60.0F, report)) {
                return 0U;
            }
        }
        return report.live_particles;
    };

    const u32 full = populate(false);
    const u32 degraded = populate(true);
    std::fprintf(stderr, "levers: %u live at the allocation, %u under a 4.5x overload\n", full,
                 degraded);
    CY_CHECK_GT(full, 0U);
    CY_CHECK_GT(degraded, 0U);
    // Bounded by configuration rather than by the frame: reduced, and still alive.
    CY_CHECK_LT(degraded, full);
}

CY_TEST_CASE("an authored graph raises events, and the channel's declared bound holds") {
    // The end-to-end half of `vfx-system`'s event requirement: `unit.vfx_values` proves the ROUTER
    // drops by rank and truncates a chain, and this proves a compiled kernel can put anything on a
    // channel at all. A bound nothing ever reaches is a bound nobody has tested.
    Cooked cooked(1, 256);
    CY_REQUIRE(cooked.ok);
    const VfxKernel* update = (*cooked).emitters()[0].kernel_for(Stage::Update);
    CY_REQUIRE(update != nullptr);
    CY_REQUIRE_EQ(update->events().size(), 1U);
    CY_CHECK_EQ(update->events()[0].channel, Name::intern("collision"));

    SimulationWorld world(allocator());
    CY_REQUIRE(world.initialize(small_world()).has_value());
    EffectSpawn spawn;
    CY_REQUIRE(world.play(*cooked, spawn).has_value());

    StepReport report;
    for (u32 frame = 0; frame < 40U; ++frame) {
        CY_REQUIRE(world.step(1.0F / 60.0F, report).has_value());
    }
    // PLAYING THE SYSTEM DECLARED ITS CHANNELS. A compiled system carries them with both of their
    // bounds; a caller that had to declare them by hand is a caller that forgets one.
    const ChannelReport* channel = world.events().report(Name::intern("collision"));
    CY_REQUIRE(channel != nullptr);
    std::fprintf(stderr, "collision channel: %u raised, %u delivered, %u dropped at a cap of %u\n",
                 channel->raised, channel->delivered, channel->dropped,
                 channel->max_events_per_frame);
    CY_CHECK_GT(channel->raised, 0U);
    // THE BOUND HOLDS. More raises than the channel's declared maximum, and exactly the maximum
    // survives — the rest are counted, which is "report the overflow rather than compounding".
    CY_CHECK_LE(world.events().consume(Name::intern("collision")).size(),
                channel->max_events_per_frame);
    CY_CHECK_EQ(channel->delivered + channel->dropped + channel->truncated, channel->raised);
}

// --- The bounded readback path, and the firewall scope
// ----------------------------------------------

namespace {

struct SinkState {
    u32 calls = 0;
    u32 events = 0;
};

/// What the firewall scope case observes. A file-scope value rather than a capture, because
/// `ReadbackSink` is a plain function pointer and a capturing lambda cannot become one.
cy::ecs::WriteOrigin observed_origin = cy::ecs::WriteOrigin::Simulation;

void observe_origin(Name /*channel*/, Span<const EventRecord> /*events*/, void* /*user*/) noexcept {
    observed_origin = cy::ecs::current_write_origin();
}

void count_events(Name /*channel*/, Span<const EventRecord> events, void* user) noexcept {
    auto* state = static_cast<SinkState*>(user);
    ++state->calls;
    state->events += static_cast<u32>(events.size());
}

}  // namespace

CY_TEST_CASE("readback has at least one frame of latency and never waits for one") {
    ReadbackQueue queue(allocator());
    queue.set_budget(4096);
    EventRecord events[4] = {};
    const Name channel = Name::intern("collision");

    // Nothing has been published: the reader proceeds with nothing rather than blocking, and the
    // stale read is counted.
    SinkState state;
    CY_REQUIRE(queue.deliver(&count_events, &state).has_value());
    CY_CHECK_EQ(state.events, 0U);
    CY_CHECK_EQ(queue.report().stale_reads, 1U);

    CY_REQUIRE(queue.publish(channel, Span<const EventRecord>(events, 4)).has_value());
    // Published this frame is NOT deliverable this frame.
    CY_REQUIRE(queue.deliver(&count_events, &state).has_value());
    CY_CHECK_EQ(state.events, 0U);

    CY_REQUIRE(queue.begin_frame().has_value());
    CY_REQUIRE(queue.deliver(&count_events, &state).has_value());
    CY_CHECK_EQ(state.events, 4U);
    CY_CHECK_EQ(queue.report().latency_frames, 1U);
}

CY_TEST_CASE("readback over its byte budget is deferred and reported, never truncated") {
    ReadbackQueue queue(allocator());
    // Room for exactly two records.
    queue.set_budget(sizeof(EventRecord) * 2);
    Array<EventRecord> events(allocator());
    CY_REQUIRE(events.resize(10).has_value());
    for (usize index = 0; index < events.size(); ++index) {
        events[index].source = static_cast<u32>(index);
    }
    CY_REQUIRE(queue.publish(Name::intern("collision"), events.span()).has_value());
    CY_REQUIRE(queue.begin_frame().has_value());

    SinkState state;
    CY_REQUIRE(queue.deliver(&count_events, &state).has_value());
    CY_CHECK_EQ(state.events, 2U);
    CY_CHECK_EQ(queue.report().deferred, 8U);
    CY_CHECK_EQ(queue.report().bytes_delivered, sizeof(EventRecord) * 2);

    // The eight that did not fit are still there next frame — not dropped.
    u32 total = state.events;
    for (u32 frame = 0; frame < 8U; ++frame) {
        CY_REQUIRE(queue.begin_frame().has_value());
        SinkState round;
        CY_REQUIRE(queue.deliver(&count_events, &round).has_value());
        total += round.events;
    }
    CY_CHECK_EQ(total, 10U);
}

CY_TEST_CASE("the readback delivery runs inside a VFX write scope") {
    // `vfx-system` requires development builds to "detect and report attempts to write replicated
    // or physics-owned components from VFX-driven code paths", and M8.c section 1 built the
    // enforcement point. VFX's half is to DECLARE its origin, and this is the check that it does:
    // the sink observes the thread-local origin the ECS firewall reads.
    ReadbackQueue queue(allocator());
    queue.set_budget(4096);
    EventRecord event{};
    CY_REQUIRE(
        queue.publish(Name::intern("collision"), Span<const EventRecord>(&event, 1)).has_value());
    CY_REQUIRE(queue.begin_frame().has_value());

    observed_origin = cy::ecs::WriteOrigin::Simulation;
    CY_REQUIRE(queue.deliver(&observe_origin, nullptr).has_value());
    CY_CHECK_EQ(observed_origin, cy::ecs::WriteOrigin::Vfx);
    // And the scope closes: the origin outside the delivery is the simulation's again.
    CY_CHECK_EQ(cy::ecs::current_write_origin(), cy::ecs::WriteOrigin::Simulation);
}

// --- Publication to the renderer ---------------------------------------------------------------

CY_TEST_CASE("the world publishes camera-relative sprite records, and the ring bound is counted") {
    Cooked cooked;
    CY_REQUIRE(cooked.ok);
    SimulationWorld world(allocator());
    CY_REQUIRE(world.initialize(small_world()).has_value());
    EffectSpawn spawn;
    spawn.position = Vec3{2.0F, 0.0F, -6.0F};
    CY_REQUIRE(world.play(*cooked, spawn).has_value());
    StepReport report;
    for (u32 frame = 0; frame < 40U; ++frame) {
        CY_REQUIRE(world.step(1.0F / 60.0F, report).has_value());
    }

    Array<rendering::particles::ParticleInstance> records(allocator());
    PublishReport published;
    CY_REQUIRE(
        publish_sprites(world, Vec3{0.0F, 0.0F, 0.0F}, 4096, records, published).has_value());
    CY_CHECK_GT(published.particles, 0U);
    CY_CHECK_EQ(published.dropped, 0U);
    CY_CHECK_EQ(records.size(), published.particles);
    // Camera-relative: the effect is at z = -6 and the camera at the origin, so every record's z is
    // near -6 rather than near zero.
    CY_CHECK_LT(records[0].position[2], -3.0F);
    CY_CHECK_GT(records[0].size, 0.0F);

    // A ring that cannot hold the world drops the tail and COUNTS it.
    Array<rendering::particles::ParticleInstance> small(allocator());
    PublishReport bounded;
    CY_REQUIRE(publish_sprites(world, Vec3{0.0F, 0.0F, 0.0F}, 8, small, bounded).has_value());
    CY_CHECK_EQ(bounded.particles, 8U);
    CY_CHECK_EQ(bounded.dropped, published.particles - 8U);
    std::fprintf(stderr, "published %u sprites; an 8-record ring dropped %u\n", published.particles,
                 bounded.dropped);
}

CY_TEST_CASE("mesh particles are published as instance rows with no entity anywhere") {
    // `vfx-system`: mesh particles "SHALL NOT require ECS entities, per-particle CPU submission, or
    // CPU readback". The plume declares no `mesh` attribute, so it publishes no mesh rows — which
    // is the honest answer and is what makes the emitter's own layout the thing that decides.
    Cooked cooked;
    CY_REQUIRE(cooked.ok);
    SimulationWorld world(allocator());
    CY_REQUIRE(world.initialize(small_world()).has_value());
    EffectSpawn spawn;
    CY_REQUIRE(world.play(*cooked, spawn).has_value());
    StepReport report;
    CY_REQUIRE(world.step(1.0F / 60.0F, report).has_value());

    Array<MeshParticleInstance> rows(allocator());
    PublishReport published;
    CY_REQUIRE(
        publish_mesh_instances(world, Vec3{0.0F, 0.0F, 0.0F}, 1024, rows, published).has_value());
    CY_CHECK_EQ(published.emitters, 0U);
    CY_CHECK_EQ(rows.size(), 0U);
}

CY_TEST_CASE("teardown with a full world, and again while every instance is still spawning") {
    // Rule 4 of this milestone's brief, and the shape M4's `render.frames` suite argues for: a
    // teardown that only ever runs on an empty world is a teardown nobody has tested.
    Cooked cooked(2, 128);
    CY_REQUIRE(cooked.ok);
    for (u32 attempt = 0; attempt < 3U; ++attempt) {
        auto* world = new SimulationWorld(allocator());
        WorldDescription description = small_world();
        description.pool_bytes = 8ULL * 1024ULL * 1024ULL;
        CY_REQUIRE(world->initialize(description).has_value());
        for (u32 which = 0; which < 40U; ++which) {
            EffectSpawn spawn;
            spawn.release_on_completion = which % 2U == 0U;
            CY_REQUIRE(world->play(*cooked, spawn).has_value());
        }
        StepReport report;
        for (u32 frame = 0; frame < 10U + attempt; ++frame) {
            CY_REQUIRE(world->step(1.0F / 60.0F, report).has_value());
        }
        CY_CHECK_GT(report.live_particles, 0U);
        // Deleted mid-flight, with every instance still spawning and the pool fully committed.
        delete world;
    }
}

CY_TEST_CASE("a fire-and-forget effect releases itself and its pool block comes back") {
    Cooked cooked(1, 64);
    CY_REQUIRE(cooked.ok);
    SimulationWorld world(allocator());
    CY_REQUIRE(world.initialize(small_world()).has_value());

    EffectSpawn spawn;
    spawn.release_on_completion = true;
    auto handle = world.play(*cooked, spawn);
    CY_REQUIRE(handle.has_value());
    CY_REQUIRE(world.stop(*handle, true).has_value());

    StepReport report;
    for (u32 frame = 0; frame < 300U; ++frame) {
        CY_REQUIRE(world.step(1.0F / 60.0F, report).has_value());
        if (world.find(*handle) == nullptr) {
            break;
        }
    }
    CY_CHECK(world.find(*handle) == nullptr);
    CY_CHECK_EQ(world.pool().report().used_bytes, 0U);
}

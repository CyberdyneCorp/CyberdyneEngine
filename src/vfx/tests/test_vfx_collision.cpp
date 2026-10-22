// SPDX-License-Identifier: MIT
// Particle collision on the CPU path: contact against every collider, and every response the
// requirement lists. `vfx-system` — "Collision". `integration.vfx`.
//
// Each case runs the same authored effect (`collision_effects.h`) through a `SimulationWorld` and
// reads the particles back through `read_attribute`, so what is checked is what a renderer or a
// gameplay readback would see. Each claim has its control: the same run with the mechanism that
// answers it taken away — the collider muted, the restitution changed, the event's `began` gate
// replaced by the raw contact — and the control must fail the claim.

#include "collision_effects.h"
#include "collision_scene.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/vfx/collision.h>
#include <cy/vfx/runtime.h>
#include <cy/vfx/world.h>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace cy;
using namespace cy::vfx;
using namespace cy::vfx_test;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

constexpr f32 kStep = 1.0F / 60.0F;
constexpr f32 kGravity = 9.81F;

/// One particle's state, read back the way a renderer reads it.
struct ParticleState {
    bool alive = false;
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 velocity{0.0F, 0.0F, 0.0F};
    f32 collided = 0.0F;
};

/// A scenario cooked and playing in a world of its own, stepped one sub-step a frame.
class Run {
public:
    explicit Run(const DropScenario& scenario, const CollisionField* field = nullptr,
                 const DepthCollisionView* depth = nullptr) noexcept
        : sink_(allocator()), report_(allocator()), world_(allocator()) {
        auto compiled = cook_drop(allocator(), scenario, sink_, report_);
        if (!compiled.has_value()) {
            for (const graph::Diagnostic& diagnostic : sink_.entries()) {
                std::fprintf(stderr, "cook: %s (%s)\n", diagnostic.message,
                             diagnostic.detail.text().data());
            }
            return;
        }
        system_ = Expected<CompiledSystem, Error>(std::move(compiled.value()));
        if (!world_.initialize(WorldDescription{}).has_value()) {
            return;
        }
        world_.set_collision(field, depth);
        EffectSpawn spawn;
        spawn.simulation_hz = 60.0F;
        ready_ = world_.play(system_.value(), spawn).has_value();
    }

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] const CompiledSystem& system() const noexcept { return system_.value(); }
    [[nodiscard]] SimulationWorld& world() noexcept { return world_; }

    /// One frame; returns the events raised on `impact` during it.
    u32 step() noexcept {
        StepReport report;
        ok_ = ok_ && world_.step(kStep, report).has_value();
        const ChannelReport* impacts = world_.events().report(Name::intern("impact"));
        return impacts != nullptr ? impacts->raised : 0U;
    }

    void steps(u32 count) noexcept {
        for (u32 index = 0; index < count; ++index) {
            (void)step();
        }
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] u32 capacity(u32 emitter = 0) const noexcept {
        return world_.blocks()[world_.instances()[0].first_block + emitter].particles;
    }

    [[nodiscard]] ParticleState particle(u32 index, u32 emitter = 0) const noexcept {
        static const Name kPosition = Name::intern("position");
        static const Name kVelocity = Name::intern("velocity");
        static const Name kCollided = Name::intern("collided");
        const EffectInstance& instance = world_.instances()[0];
        ParticleState state;
        state.alive = world_.alive_flags(instance.first_block + emitter)[index] != 0;
        state.position = Vec3{world_.read_attribute(instance, emitter, index, kPosition, 0),
                              world_.read_attribute(instance, emitter, index, kPosition, 1),
                              world_.read_attribute(instance, emitter, index, kPosition, 2)};
        state.velocity = Vec3{world_.read_attribute(instance, emitter, index, kVelocity, 0),
                              world_.read_attribute(instance, emitter, index, kVelocity, 1),
                              world_.read_attribute(instance, emitter, index, kVelocity, 2)};
        state.collided = world_.read_attribute(instance, emitter, index, kCollided, 0);
        return state;
    }

    [[nodiscard]] u32 live(u32 emitter = 0) const noexcept {
        u32 count = 0;
        for (u32 index = 0; index < capacity(emitter); ++index) {
            count += particle(index, emitter).alive ? 1U : 0U;
        }
        return count;
    }

private:
    graph::DiagnosticSink sink_;
    CompileReport report_;
    Expected<CompiledSystem, Error> system_ = fail(ErrorCode::Unavailable, "not cooked");
    SimulationWorld world_;
    bool ready_ = false;
    bool ok_ = true;
};

/// Sixty-four particles dropped onto the ground plane from two metres, spread over a square metre.
[[nodiscard]] DropScenario ground_drop() noexcept {
    DropScenario scenario;
    scenario.radius = 0.05F;
    scenario.spread = Vec3{0.5F, 0.25F, 0.5F};
    scenario.restitution = 0.5F;
    scenario.rest_speed = 0.3F;
    return scenario;
}

}  // namespace

CY_TEST_CASE("collision: a particle falling on a plane comes to rest on it, within a bound") {
    // THE BOUND: resolved to separation zero every step it touches, so a resting particle's centre
    // is one radius above the plane to within float rounding. Measured: worst 0 m after three
    // seconds; the bound is a tenth of a millimetre, a thirtieth of one step's fall under gravity.
    Run run(ground_drop());
    CY_REQUIRE(run.ready());
    run.steps(180);
    CY_REQUIRE(run.ok());
    CY_REQUIRE_EQ(run.live(), 64U);
    f32 worst_height = 0.0F;
    f32 worst_speed = 0.0F;
    for (u32 index = 0; index < run.capacity(); ++index) {
        const ParticleState state = run.particle(index);
        worst_height = std::max(worst_height, std::fabs(state.position.y - 0.05F));
        worst_speed = std::max(worst_speed, length(state.velocity));
        CY_CHECK_EQ(state.collided, 1.0F);
    }
    std::fprintf(stderr, "plane rest: worst height error %.3g m, worst speed %.3g m/s\n",
                 static_cast<double>(worst_height), static_cast<double>(worst_speed));
    CY_CHECK_LT(worst_height, 1.0e-4F);
    CY_CHECK_LT(worst_speed, 1.0e-3F);

    // CONTROL: the same effect with the collider muted falls straight through the floor.
    DropScenario muted = ground_drop();
    muted.mute_collider = true;
    Run control(muted);
    CY_REQUIRE(control.ready());
    control.steps(180);
    CY_CHECK_LT(control.particle(0).position.y, -10.0F);
    CY_CHECK_EQ(control.particle(0).collided, 0.0F);
}

CY_TEST_CASE("collision: the bounce keeps exactly the restitution of the closing speed") {
    // THE CONTACT STEP's own arithmetic: the velocity integrated INTO the floor this step is
    // `v_before - g dt`, and the bounce sends back `-e` times it. Read off the step before and the
    // step of first contact, for two restitutions — and each run must miss the other's ratio.
    for (const f32 restitution : {0.3F, 0.8F}) {
        DropScenario scenario;
        scenario.spawn_per_step = 1;
        scenario.capacity = 1;
        scenario.restitution = restitution;
        Run run(scenario);
        CY_REQUIRE(run.ready());
        f32 before = 0.0F;
        bool bounced = false;
        for (u32 frame = 0; frame < 120 && !bounced; ++frame) {
            before = run.particle(0).velocity.y;
            run.step();
            const ParticleState state = run.particle(0);
            if (state.collided == 1.0F) {
                const f32 closing = before - (kGravity * kStep);
                const f32 ratio = state.velocity.y / -closing;
                std::fprintf(stderr,
                             "restitution %.2f: closing %.4f m/s, left at %.4f, ratio %.6f\n",
                             static_cast<double>(restitution), static_cast<double>(closing),
                             static_cast<double>(state.velocity.y), static_cast<double>(ratio));
                CY_CHECK_NEAR(ratio, restitution, 1.0e-5F);
                CY_CHECK_GT(std::fabs(ratio - (restitution == 0.3F ? 0.8F : 0.3F)), 0.4F);
                bounced = true;
            }
        }
        CY_CHECK(bounced);
    }
}

CY_TEST_CASE("collision: an impact event fires once per contact, and not while resting") {
    // EVERY 0 -> 1 TRANSITION OF THE CONTACT FLAG IS ONE CONTACT, read particle by particle, and
    // the channel must have raised exactly that many over the run. Then the particles rest — the
    // flag stays 1 — and a second of resting must raise nothing.
    DropScenario scenario = ground_drop();
    scenario.restitution = 0.6F;
    scenario.rest_speed = 0.5F;
    scenario.raise_impacts = true;
    Run run(scenario);
    CY_REQUIRE(run.ready());
    std::vector<f32> previous(run.capacity(), 0.0F);
    u32 contacts = 0;
    u32 raised = 0;
    for (u32 frame = 0; frame < 240; ++frame) {
        raised += run.step();
        for (u32 index = 0; index < run.capacity(); ++index) {
            const f32 now = run.particle(index).collided;
            contacts += (now == 1.0F && previous[index] == 0.0F) ? 1U : 0U;
            previous[index] = now;
        }
    }
    u32 resting = 0;
    for (u32 frame = 0; frame < 60; ++frame) {
        resting += run.step();
    }
    std::fprintf(stderr, "impacts: %u contacts began, %u events raised, %u while resting\n",
                 contacts, raised, resting);
    CY_CHECK_GT(contacts, 64U);  // every particle bounced at least twice before resting
    CY_CHECK_EQ(raised, contacts);
    CY_CHECK_EQ(resting, 0U);
}

CY_TEST_CASE("collision: kill on contact kills every particle at the surface and none below it") {
    DropScenario scenario = ground_drop();
    scenario.kill_on_contact = true;
    Run run(scenario);
    CY_REQUIRE(run.ready());
    f32 lowest = 10.0F;
    for (u32 frame = 0; frame < 120; ++frame) {
        run.step();
        for (u32 index = 0; index < run.capacity(); ++index) {
            const ParticleState state = run.particle(index);
            if (state.alive) {
                lowest = std::min(lowest, state.position.y);
            }
        }
    }
    CY_CHECK_EQ(run.live(), 0U);
    // A live particle was never seen inside the floor: the kill happens on the contact's step.
    CY_CHECK_GE(lowest, 0.05F - 1.0e-4F);
}

CY_TEST_CASE("collision: a sticking particle stops where it hit a wall and stays there") {
    // A wall at x = 1, the solid half-space x > 1: `dot((-1, 0, 0), p) < -1`. Thrown at it
    // sideways, a particle with `stick` stops on contact; gated integration keeps it there against
    // gravity, a second later, to the float.
    DropScenario scenario;
    scenario.spawn_per_step = 1;
    scenario.capacity = 1;
    scenario.start = Vec3{0.0F, 2.0F, 0.0F};
    scenario.velocity = Vec3{4.0F, 1.0F, 0.0F};
    scenario.a[0] = -1.0F;
    scenario.a[1] = 0.0F;
    scenario.a[2] = 0.0F;
    scenario.a[3] = -1.0F;
    scenario.radius = 0.05F;
    scenario.stick = 1.0F;
    scenario.hold_when_stuck = true;
    Run run(scenario);
    CY_REQUIRE(run.ready());
    // THE STEP OF CONTACT ITSELF: `stick` stops the particle there, before the gate — which only
    // acts on the next step — has had a say. A bounce would leave it heading back at -x.
    u32 frames = 0;
    while (run.particle(0).collided == 0.0F && frames < 60) {
        run.step();
        ++frames;
    }
    const ParticleState stuck = run.particle(0);
    CY_REQUIRE_EQ(stuck.collided, 1.0F);
    CY_CHECK_EQ(length(stuck.velocity), 0.0F);
    CY_CHECK_NEAR(stuck.position.x, 0.95F, 1.0e-4F);
    run.steps(60);
    const ParticleState later = run.particle(0);
    CY_CHECK_EQ(later.position.x, stuck.position.x);
    CY_CHECK_EQ(later.position.y, stuck.position.y);
    CY_CHECK_EQ(length(later.velocity), 0.0F);

    // CONTROL: without `stick` (and without the gate it feeds) the particle bounces off the wall.
    scenario.stick = 0.0F;
    scenario.hold_when_stuck = false;
    Run control(scenario);
    CY_REQUIRE(control.ready());
    control.steps(90);
    CY_CHECK_LT(control.particle(0).position.x, 0.9F);
}

CY_TEST_CASE("collision: friction stops a sliding particle where Coulomb says it should") {
    // Sliding at 4 m/s along the floor, a friction coefficient of 0.5 takes `mu g dt` off the
    // tangential speed each step: it stops after v^2 / (2 mu g) = 1.63 m. Without friction it is
    // still sliding at 4 m/s a second later — which is the "sliding" response.
    DropScenario scenario;
    scenario.spawn_per_step = 1;
    scenario.capacity = 1;
    scenario.start = Vec3{0.0F, 0.05F, 0.0F};
    scenario.velocity = Vec3{4.0F, 0.0F, 0.0F};
    scenario.radius = 0.05F;
    scenario.rest_speed = 1.0F;
    scenario.friction = 0.5F;
    Run run(scenario);
    CY_REQUIRE(run.ready());
    run.steps(120);
    const ParticleState stopped = run.particle(0);
    const f32 expected = (4.0F * 4.0F) / (2.0F * 0.5F * kGravity);
    std::fprintf(stderr, "friction: stopped after %.3f m, Coulomb says %.3f m\n",
                 static_cast<double>(stopped.position.x), static_cast<double>(expected));
    CY_CHECK_NEAR_REL(stopped.position.x, expected, 0.05F);
    CY_CHECK_EQ(stopped.velocity.x, 0.0F);

    scenario.friction = 0.0F;
    Run sliding(scenario);
    CY_REQUIRE(sliding.ready());
    sliding.steps(60);
    CY_CHECK_NEAR(sliding.particle(0).velocity.x, 4.0F, 1.0e-5F);
    CY_CHECK_NEAR(sliding.particle(0).position.y, 0.05F, 1.0e-4F);
}

CY_TEST_CASE("collision: particles rest on a sphere, a box and a capsule") {
    struct Row {
        const char* shape;
        f32 a[4];
        f32 b[4];
        f32 top;
    };
    // Each shape's top is one metre up; a particle dropped on its crown with no bounce rests there.
    const Row rows[] = {
        {"sphere", {0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 0.0F}, 1.0F},
        {"box", {0.0F, 0.5F, 0.0F, 0.0F}, {1.0F, 0.5F, 1.0F, 0.0F}, 1.0F},
        {"capsule", {-1.0F, 0.5F, 0.0F, 0.5F}, {1.0F, 0.5F, 0.0F, 0.0F}, 1.0F},
    };
    for (const Row& row : rows) {
        DropScenario scenario;
        scenario.shape = row.shape;
        for (u32 index = 0; index < 4; ++index) {
            scenario.a[index] = row.a[index];
            scenario.b[index] = row.b[index];
        }
        scenario.spawn_per_step = 1;
        scenario.capacity = 1;
        scenario.start = Vec3{0.0F, 3.0F, 0.0F};
        scenario.radius = 0.05F;
        scenario.restitution = 0.0F;
        Run run(scenario);
        CY_REQUIRE(run.ready());
        run.steps(120);
        const ParticleState state = run.particle(0);
        std::fprintf(stderr, "%s: rests at %.5f m\n", row.shape,
                     static_cast<double>(state.position.y));
        CY_CHECK_NEAR(state.position.y, row.top + 0.05F, 1.0e-4F);
        CY_CHECK_EQ(state.collided, 1.0F);
    }
}

CY_TEST_CASE("collision: a particle thrown at a box's side bounces back off it") {
    DropScenario scenario;
    scenario.shape = "box";
    scenario.a[0] = 2.0F;
    scenario.a[1] = 1.0F;
    scenario.a[2] = 0.0F;
    scenario.b[0] = 0.5F;
    scenario.b[1] = 1.0F;
    scenario.b[2] = 0.5F;
    scenario.spawn_per_step = 1;
    scenario.capacity = 1;
    scenario.start = Vec3{0.0F, 1.0F, 0.0F};
    scenario.velocity = Vec3{6.0F, 0.0F, 0.0F};
    scenario.gravity = Vec3{0.0F, 0.0F, 0.0F};
    scenario.restitution = 0.5F;
    Run run(scenario);
    CY_REQUIRE(run.ready());
    run.steps(30);
    const ParticleState state = run.particle(0);
    CY_CHECK_NEAR(state.velocity.x, -3.0F, 1.0e-4F);
    CY_CHECK_LT(state.position.x, 1.5F);
}

CY_TEST_CASE("collision: the scene field stops a particle on a step the camera cannot see") {
    // `vfx-system`'s two scenarios side by side. The stairs are in the field and in a depth buffer
    // taken by a camera looking AWAY from them. A particle dropped onto the third step rests on it
    // against the field — "geometry outside the view" — and falls through it against the depth
    // buffer, which is the documented limitation rather than a defect.
    StairsField field;
    const DepthScene away =
        render_depth(field, Vec3{0.0F, 2.0F, -2.0F}, Vec3{0.0F, 2.0F, -8.0F}, 64, 36);
    Vec3 centre;
    Vec3 half;
    step_box(2, centre, half);
    const f32 tread = centre.y + half.y;

    DropScenario scenario;
    scenario.spawn_per_step = 1;
    scenario.capacity = 1;
    scenario.start = Vec3{0.0F, 2.0F, centre.z};
    scenario.radius = 0.02F;
    scenario.restitution = 0.0F;
    scenario.shape = "sdf";
    scenario.a[0] = 0.0F;
    Run sdf(scenario, &field, &away.view);
    CY_REQUIRE(sdf.ready());
    sdf.steps(90);
    std::fprintf(stderr, "sdf: rests at %.5f m on a tread at %.5f m\n",
                 static_cast<double>(sdf.particle(0).position.y), static_cast<double>(tread));
    CY_CHECK_NEAR(sdf.particle(0).position.y, tread + 0.02F, 1.0e-3F);

    scenario.shape = "depth";
    Run depth(scenario, &field, &away.view);
    CY_REQUIRE(depth.ready());
    depth.steps(90);
    CY_CHECK_LT(depth.particle(0).position.y, -5.0F);
}

CY_TEST_CASE("collision: the depth buffer stops a particle on the surface the camera sees") {
    // The same step, seen this time: a camera above and in front of the stairs. Depth collision is
    // as good as the buffer it reads, so the bound is a millimetre rather than the field's tenth.
    StairsField field;
    const DepthScene seen =
        render_depth(field, Vec3{0.0F, 3.0F, -2.5F}, Vec3{0.0F, 0.0F, 1.0F}, 320, 180);
    Vec3 centre;
    Vec3 half;
    step_box(2, centre, half);
    const f32 tread = centre.y + half.y;

    DropScenario scenario;
    scenario.shape = "depth";
    scenario.a[0] = 0.0F;
    scenario.spawn_per_step = 1;
    scenario.capacity = 1;
    scenario.start = Vec3{0.0F, 2.0F, centre.z};
    scenario.radius = 0.02F;
    scenario.restitution = 0.0F;
    Run run(scenario, nullptr, &seen.view);
    CY_REQUIRE(run.ready());
    run.steps(90);
    const ParticleState state = run.particle(0);
    std::fprintf(stderr, "depth: rests at %.5f m on a tread at %.5f m\n",
                 static_cast<double>(state.position.y), static_cast<double>(tread));
    CY_CHECK_NEAR(state.position.y, tread + 0.02F, 1.0e-2F);
    CY_CHECK_EQ(state.collided, 1.0F);
}

CY_TEST_CASE("collision: a particle further behind a surface than it is thick passes behind it") {
    // THE DEPTH BUFFER HAS NO BACK. A particle half a metre under the floor the camera sees is
    // behind that surface, not inside it, once it is deeper than the surface's `thickness`: it
    // carries on falling rather than being snapped up through the floor. Thicken the surface past
    // that depth and the same particle is caught and resolved onto it — the control.
    StairsField field;
    const DepthScene seen =
        render_depth(field, Vec3{0.0F, 3.0F, -4.0F}, Vec3{0.0F, 0.0F, -1.5F}, 320, 180);
    DropScenario scenario;
    scenario.shape = "depth";
    scenario.spawn_per_step = 1;
    scenario.capacity = 1;
    scenario.start = Vec3{0.0F, -0.5F, -1.5F};
    scenario.gravity = Vec3{0.0F, 0.0F, 0.0F};
    scenario.restitution = 0.0F;
    scenario.a[0] = 0.2F;
    Run thin(scenario, nullptr, &seen.view);
    CY_REQUIRE(thin.ready());
    thin.steps(10);
    CY_CHECK_NEAR(thin.particle(0).position.y, -0.5F, 1.0e-5F);
    CY_CHECK_EQ(thin.particle(0).collided, 0.0F);

    scenario.a[0] = 2.0F;
    Run thick(scenario, nullptr, &seen.view);
    CY_REQUIRE(thick.ready());
    thick.steps(10);
    CY_CHECK_NEAR(thick.particle(0).position.y, 0.0F, 1.0e-2F);
}

CY_TEST_CASE("collision: an impact spawns one spark at the place it happened") {
    // `vfx-system`'s "Collision raises an event ... for consumption by another emitter": the drops
    // raise `impact` with the resolved position as the payload, and the sparks emitter spawns one
    // particle an event, born at it. Every impact becomes exactly one spark.
    //
    // TWO SUB-STEPS A FRAME, so the drops raise in the first and again in the second while the
    // channel still holds the first's: the sparks' cursor is what keeps the second sub-step from
    // spawning the first's events a second time.
    DropScenario scenario = ground_drop();
    scenario.restitution = 0.6F;
    scenario.rest_speed = 0.5F;
    scenario.raise_impacts = true;
    scenario.sparks = true;
    // The instance is a frame — two sub-steps — old when it first simulates.
    scenario.spawn_until = 0.05F;
    Run run(scenario);
    CY_REQUIRE(run.ready());
    u32 raised = 0;
    u32 spawned = 0;
    u32 substeps = 0;
    f32 highest = 0.0F;
    f32 widest = 0.0F;
    for (u32 frame = 0; frame < 90; ++frame) {
        StepReport report;
        CY_REQUIRE(run.world().step(2.0F * kStep, report).has_value());
        substeps += report.substeps;
        raised += run.world().events().report(Name::intern("impact"))->raised;
        // Everything spawned after the drops' first frame is a spark, born from an impact this
        // frame.
        spawned += report.spawned;
        for (u32 index = 0; index < run.capacity(1); ++index) {
            const ParticleState spark = run.particle(index, 1);
            if (spark.alive) {
                highest = std::max(highest, spark.position.y);
                widest = std::max(widest, std::fabs(spark.position.x));
            }
        }
    }
    const u32 drops = 64;
    std::fprintf(stderr,
                 "sparks: %u impacts raised, %u particles spawned (%u of them drops), highest "
                 "spark %.3f m, widest %.3f m\n",
                 raised, spawned, drops, static_cast<double>(highest), static_cast<double>(widest));
    CY_CHECK_EQ(substeps, 180U);
    CY_CHECK_GT(raised, 0U);
    CY_CHECK_EQ(spawned, raised + drops);
    // BORN AT THE IMPACT. Thrown up at most 2.5 m/s from the floor, a spark never climbs a metre;
    // and the drops landed across a square metre, so sparks born at their impacts are spread across
    // it rather than stacked on the emitter's origin.
    CY_CHECK_LT(highest, 1.0F);
    CY_CHECK_GT(widest, 0.2F);
}

CY_TEST_CASE("collision: the budget's collision quality is what the contact reads") {
    // `vfx-system`: "Collision quality SHALL be adjustable by the budget controller". Evaluated at
    // each quality on one touching particle: the field samples a contact takes, and whether the
    // field and the analytic plane are still there at all.
    StairsField field;
    DropScenario scenario;
    scenario.shape = "sdf";
    scenario.spawn_per_step = 1;
    scenario.capacity = 1;
    graph::DiagnosticSink sink(allocator());
    CompileReport report(allocator());
    auto system = cook_drop(allocator(), scenario, sink, report);
    CY_REQUIRE(system.has_value());

    const Vec3 touching{0.0F, 0.01F, -2.0F};  // on the floor, clear of the stairs
    u64 samples[4] = {};
    f32 separations[4] = {};
    for (u8 quality = 0; quality < 4; ++quality) {
        CollisionInputs inputs;
        inputs.field = &field;
        inputs.quality = quality;
        field.reset_samples();
        const Vec4 contact = collision_contact(ColliderShape::Sdf, touching, 0.02F,
                                               Vec4{0.0F, 0.0F, 0.0F, 0.0F}, Vec4{}, inputs);
        samples[quality] = field.samples();
        separations[quality] = contact.w;
    }
    CY_CHECK_EQ(samples[3], 7U);  // one to find the contact, six for the normal
    CY_CHECK_EQ(samples[2], 5U);  // one, and the four of a tetrahedron
    CY_CHECK_EQ(samples[1], 0U);  // analytic colliders only
    CY_CHECK_EQ(samples[0], 0U);
    CY_CHECK_NEAR(separations[3], -0.01F, 1.0e-6F);
    CY_CHECK_EQ(separations[1], kNoContact);

    CollisionInputs analytic;
    analytic.quality = 1;
    CY_CHECK_LT(collision_contact(ColliderShape::Plane, touching, 0.02F,
                                  Vec4{0.0F, 1.0F, 0.0F, 0.0F}, Vec4{}, analytic)
                    .w,
                0.0F);
    analytic.quality = 0;
    CY_CHECK_EQ(collision_contact(ColliderShape::Plane, touching, 0.02F,
                                  Vec4{0.0F, 1.0F, 0.0F, 0.0F}, Vec4{}, analytic)
                    .w,
                kNoContact);
}

CY_TEST_CASE("collision: the world hands the controller's quality to every contact") {
    // The same drop through the world with the controller driven to its floor by a sustained
    // overload: the class's collision lever reaches zero and the floor stops nothing. The
    // overload also slows the effect to its floor frequency, so the spawn is not tied to the first
    // frame, and the request is large enough to survive the spawn lever's reduction.
    DropScenario scenario;
    scenario.spawn_per_step = 8;
    scenario.capacity = 1;
    scenario.spawn_until = 1000.0F;
    scenario.restitution = 0.0F;
    Run run(scenario);
    CY_REQUIRE(run.ready());
    BudgetController& budget = run.world().budget();
    budget.set_allocation(0.1F);
    for (u32 frame = 0; frame < 600; ++frame) {
        budget.measure(50.0F);
        budget.update(kStep);
    }
    const BudgetLevers levers =
        budget.levers_for(run.system().importance(), run.system().scalability());
    std::fprintf(stderr, "overloaded collision quality: %u\n",
                 static_cast<unsigned>(levers.collision_quality));
    CY_REQUIRE_EQ(levers.collision_quality, 0U);
    run.steps(120);
    CY_REQUIRE(run.particle(0).alive);
    CY_CHECK_LT(run.particle(0).position.y, -1.0F);

    // CONTROL: the same world, the controller back at authored quality — the floor holds.
    budget.reset();
    Run control(scenario);
    CY_REQUIRE(control.ready());
    control.steps(120);
    CY_REQUIRE(control.particle(0).alive);
    CY_CHECK_NEAR(control.particle(0).position.y, 0.0F, 1.0e-4F);
}

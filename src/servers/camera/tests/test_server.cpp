// The camera server: definitions, rigs, stacks, the impulse bus, cuts and settings.
// Tasks 4.3.1 to 4.3.3.
//
// THE CASE THIS FILE EXISTS FOR IS THE FIRST ONE. `camera-system`: "**WHEN** a debug camera, a
// reflection capture, and an editor viewport are active **THEN** each SHALL be a camera rig with no
// requirement for a gameplay entity." This translation unit includes no world, no entity and no
// node — because layer 2 forbids all three — so that requirement is a property of the build rather
// than of a mock somebody wrote.

#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/camera/server.h>
#include <cy/test/test.h>

using cy::f32;
using cy::Name;
using cy::u32;
using cy::u64;
using namespace cy::camera;

namespace {

cy::Allocator& allocator() {
    return cy::system_allocator(cy::MemoryDomain::Engine);
}

[[nodiscard]] RigNodeDesc node(const char* id, const char* input, RigNodeKind kind) {
    RigNodeDesc desc;
    desc.id = Name::intern(id);
    desc.input = input == nullptr ? Name{} : Name::intern(input);
    desc.kind = kind;
    return desc;
}

/// A follow rig with no smoothing, so a case can assert positions rather than trends.
[[nodiscard]] RigDefinition follow_rig(cy::Allocator& memory, const char* name) {
    RigDefinition definition(memory);
    definition.name = Name::intern(name);
    (void)definition.nodes.push_back(node("target", nullptr, RigNodeKind::Target));
    RigNodeDesc follow = node("follow", "target", RigNodeKind::Follow);
    follow.follow.space = FollowSpace::World;
    follow.follow.offset = cy::Vec3{0.0F, 2.0F, 6.0F};
    follow.follow.position_half_life = 0.0F;
    (void)definition.nodes.push_back(follow);
    RigNodeDesc look = node("look", "follow", RigNodeKind::LookAt);
    look.look_at.rotation_half_life = 0.0F;
    (void)definition.nodes.push_back(look);
    (void)definition.nodes.push_back(node("output", "look", RigNodeKind::Output));
    return definition;
}

/// A rig with nothing but a noise node, so shake can be measured in isolation.
[[nodiscard]] RigDefinition shake_rig(cy::Allocator& memory) {
    RigDefinition definition(memory);
    definition.name = Name::intern("shake");
    RigNodeDesc noise = node("noise", nullptr, RigNodeKind::Noise);
    noise.noise.position_scale = 1.0F;
    noise.noise.rotation_scale = 0.0F;
    (void)definition.nodes.push_back(noise);
    (void)definition.nodes.push_back(node("output", "noise", RigNodeKind::Output));
    return definition;
}

/// The same, anchored to whatever the binding names, so two cameras can sit ninety metres apart
/// without an entity between them.
[[nodiscard]] RigDefinition anchored_shake_rig(cy::Allocator& memory) {
    RigDefinition definition(memory);
    definition.name = Name::intern("anchored-shake");
    (void)definition.nodes.push_back(node("target", nullptr, RigNodeKind::Target));
    RigNodeDesc follow = node("follow", "target", RigNodeKind::Follow);
    follow.follow.space = FollowSpace::World;
    follow.follow.offset = cy::Vec3{0.0F, 0.0F, 0.0F};
    follow.follow.position_half_life = 0.0F;
    (void)definition.nodes.push_back(follow);
    RigNodeDesc noise = node("noise", "follow", RigNodeKind::Noise);
    noise.noise.position_scale = 1.0F;
    noise.noise.rotation_scale = 0.0F;
    (void)definition.nodes.push_back(noise);
    (void)definition.nodes.push_back(node("output", "noise", RigNodeKind::Output));
    return definition;
}

struct Fixture {
    CameraServer server{allocator()};

    Fixture() {
        CameraServerConfig config;
        config.collect_trace = true;
        CY_REQUIRE(static_cast<bool>(server.configure(config)));
        CY_REQUIRE(static_cast<bool>(server.initialize()));
    }

    [[nodiscard]] RigHandle make_follow_rig(const char* name = "follow") {
        const auto definition = server.create_definition(follow_rig(allocator(), name));
        CY_REQUIRE(static_cast<bool>(definition));
        RigConfig config;
        config.name = Name::intern(name);
        const auto rig = server.create_rig(*definition, config);
        CY_REQUIRE(static_cast<bool>(rig));
        TargetBinding binding;
        binding.kind = TargetKind::Entity;
        binding.stable_id = 1;
        CY_REQUIRE(static_cast<bool>(server.set_target(*rig, binding)));
        return *rig;
    }
};

[[nodiscard]] EvaluationContext context_with(const TargetSample* samples, cy::usize count,
                                             f32 delta) {
    EvaluationContext context;
    context.delta_seconds = delta;
    context.aspect = 16.0F / 9.0F;
    context.targets = cy::Span<const TargetSample>(samples, count);
    return context;
}

[[nodiscard]] TargetSample sample_at(cy::u64 id, cy::Vec3 position) {
    TargetSample sample;
    sample.stable_id = id;
    sample.valid = true;
    sample.transform.translation = position;
    return sample;
}

}  // namespace

CY_TEST_CASE("a camera needs no entity, no node and no world") {
    // Three cameras of the three kinds the requirement names, in a translation unit that could not
    // include a world header if it wanted to.
    Fixture fixture;
    const RigHandle debug_camera = fixture.make_follow_rig("debug");
    const RigHandle reflection = fixture.make_follow_rig("reflection");
    const RigHandle editor = fixture.make_follow_rig("editor");

    CY_CHECK(fixture.server.alive(debug_camera));
    CY_CHECK(fixture.server.alive(reflection));
    CY_CHECK(fixture.server.alive(editor));
    CY_CHECK_EQ(fixture.server.live_rigs(), 3U);
}

CY_TEST_CASE("a destroyed rig's handle answers no rather than resolving to its replacement") {
    Fixture fixture;
    const RigHandle first = fixture.make_follow_rig();
    fixture.server.destroy_rig(first);
    CY_CHECK_FALSE(fixture.server.alive(first));

    const RigHandle second = fixture.make_follow_rig();
    // The slot is reused; the generation is not.
    CY_CHECK_NE(first.bits(), second.bits());
    CY_CHECK_FALSE(fixture.server.alive(first));
    CY_CHECK(fixture.server.alive(second));
}

CY_TEST_CASE("a target binding on a rig with no target node is refused") {
    // A binding nothing reads is a configuration mistake that would otherwise be invisible.
    Fixture fixture;
    const auto definition = fixture.server.create_definition(shake_rig(allocator()));
    CY_REQUIRE(static_cast<bool>(definition));
    const auto rig = fixture.server.create_rig(*definition, RigConfig{});
    CY_REQUIRE(static_cast<bool>(rig));

    TargetBinding binding;
    binding.kind = TargetKind::Entity;
    binding.stable_id = 1;
    CY_CHECK_FALSE(static_cast<bool>(fixture.server.set_target(*rig, binding)));
}

CY_TEST_CASE("the target is not implicitly the controlled entity") {
    // `camera-system`: "**WHEN** a player drives a character while the camera follows a drone
    // **THEN** the target binding SHALL be independent of control."
    Fixture fixture;
    const RigHandle rig = fixture.make_follow_rig();
    TargetBinding drone;
    drone.kind = TargetKind::Entity;
    drone.stable_id = 99;  // not the controlled entity, which the context carries separately
    CY_REQUIRE(static_cast<bool>(fixture.server.set_target(rig, drone)));

    const TargetSample samples[] = {sample_at(1, cy::Vec3{0.0F, 0.0F, 0.0F}),
                                    sample_at(99, cy::Vec3{100.0F, 0.0F, 0.0F})};
    EvaluationContext context = context_with(samples, 2, 1.0F / 60.0F);
    context.controlled = cy::Transform::from_translation(cy::Vec3{0.0F, 0.0F, 0.0F});

    const auto evaluated = fixture.server.evaluate(rig, context);
    CY_REQUIRE(static_cast<bool>(evaluated));
    // Framing the drone at x = 100, not the character at the origin.
    CY_CHECK_NEAR((*evaluated)->pose.translation.x, 100.0F, 1e-3F);
}

CY_TEST_CASE("a simulation-mode camera evaluated off the tick is an error, not a wrong camera") {
    Fixture fixture;
    const auto definition = fixture.server.create_definition(follow_rig(allocator(), "sim"));
    CY_REQUIRE(static_cast<bool>(definition));
    RigConfig config;
    config.mode = EvaluationMode::Simulation;
    const auto rig = fixture.server.create_rig(*definition, config);
    CY_REQUIRE(static_cast<bool>(rig));

    const TargetSample samples[] = {sample_at(1, cy::Vec3{})};
    EvaluationContext frame = context_with(samples, 1, 1.0F / 144.0F);
    frame.simulation = false;
    CY_CHECK_FALSE(static_cast<bool>(fixture.server.evaluate(*rig, frame)));

    EvaluationContext tick = context_with(samples, 1, 1.0F / 60.0F);
    tick.simulation = true;
    CY_CHECK(static_cast<bool>(fixture.server.evaluate(*rig, tick)));
}

CY_TEST_CASE("a hybrid camera is evaluated on a tick and on a frame, which is why it is default") {
    Fixture fixture;
    const RigHandle rig = fixture.make_follow_rig();
    const TargetSample samples[] = {sample_at(1, cy::Vec3{})};

    EvaluationContext tick = context_with(samples, 1, 1.0F / 60.0F);
    tick.simulation = true;
    CY_CHECK(static_cast<bool>(fixture.server.evaluate(rig, tick)));

    const EvaluationContext frame = context_with(samples, 1, 1.0F / 144.0F);
    CY_CHECK(static_cast<bool>(fixture.server.evaluate(rig, frame)));
}

CY_TEST_CASE("a cut resets smoothing and bumps the epoch every view's history reads") {
    Fixture fixture;
    const auto definition = fixture.server.create_definition([] {
        RigDefinition slow(allocator());
        slow.name = Name::intern("slow");
        (void)slow.nodes.push_back(node("target", nullptr, RigNodeKind::Target));
        RigNodeDesc follow = node("follow", "target", RigNodeKind::Follow);
        follow.follow.space = FollowSpace::World;
        follow.follow.offset = cy::Vec3{0.0F, 0.0F, 5.0F};
        follow.follow.position_half_life = 2.0F;  // slow enough that an ease is obvious
        (void)slow.nodes.push_back(follow);
        (void)slow.nodes.push_back(node("output", "follow", RigNodeKind::Output));
        return slow;
    }());
    CY_REQUIRE(static_cast<bool>(definition));
    const auto rig = fixture.server.create_rig(*definition, RigConfig{});
    CY_REQUIRE(static_cast<bool>(rig));
    TargetBinding binding;
    binding.kind = TargetKind::Entity;
    binding.stable_id = 1;
    CY_REQUIRE(static_cast<bool>(fixture.server.set_target(*rig, binding)));

    const TargetSample here[] = {sample_at(1, cy::Vec3{0.0F, 0.0F, 0.0F})};
    CY_REQUIRE(
        static_cast<bool>(fixture.server.evaluate(*rig, context_with(here, 1, 1.0F / 60.0F))));
    const u64 before = fixture.server.evaluated(*rig)->history_id;
    const u32 epoch_before = fixture.server.evaluated(*rig)->cut_epoch;

    // Teleport with a cut: the camera arrives rather than easing across the world.
    const TargetSample there[] = {sample_at(1, cy::Vec3{1000.0F, 0.0F, 0.0F})};
    CY_REQUIRE(static_cast<bool>(fixture.server.cut(*rig, CutReason::Teleport, false, 0.0F)));
    CY_REQUIRE(
        static_cast<bool>(fixture.server.evaluate(*rig, context_with(there, 1, 1.0F / 60.0F))));

    const EvaluatedCamera* camera = fixture.server.evaluated(*rig);
    CY_CHECK_NEAR(camera->pose.translation.x, 1000.0F, 1e-2F);
    CY_CHECK_EQ(camera->cut_epoch, epoch_before + 1U);
    CY_CHECK_EQ(camera->last_cut.reason, CutReason::Teleport);
    // The camera's own identity is stable; what changed is the epoch the view identity mixes in.
    CY_CHECK_EQ(camera->history_id, before);
    CY_CHECK_NE(history_identity(*camera, 0), before);
}

CY_TEST_CASE(
    "an impulse shakes each camera by its own distance, and the setting scales all of it") {
    // `camera-system`: "**WHEN** an explosion emits an impulse **THEN** each local camera SHALL
    // respond according to its own distance and occlusion", and "**WHEN** a player reduces camera
    // shake **THEN** every shake source SHALL scale, with no per-effect handling."
    Fixture fixture;
    const auto definition = fixture.server.create_definition(anchored_shake_rig(allocator()));
    CY_REQUIRE(static_cast<bool>(definition));
    const auto near_rig = fixture.server.create_rig(*definition, RigConfig{});
    const auto far_rig = fixture.server.create_rig(*definition, RigConfig{});
    CY_REQUIRE(static_cast<bool>(near_rig));
    CY_REQUIRE(static_cast<bool>(far_rig));

    // Two cameras, ninety metres apart, framing fixed world positions — which needs no entity and
    // no world, because a `Position` binding is resolved from the binding itself.
    TargetBinding at_origin;
    at_origin.kind = TargetKind::Position;
    at_origin.position = cy::Vec3{0.0F, 0.0F, 0.0F};
    TargetBinding far_away;
    far_away.kind = TargetKind::Position;
    far_away.position = cy::Vec3{90.0F, 0.0F, 0.0F};
    CY_REQUIRE(static_cast<bool>(fixture.server.set_target(*near_rig, at_origin)));
    CY_REQUIRE(static_cast<bool>(fixture.server.set_target(*far_rig, far_away)));

    CameraImpulse explosion;
    explosion.source = Name::intern("explosion");
    explosion.strength = 1.0F;
    explosion.radius = 100.0F;
    explosion.duration_seconds = 1.0F;
    explosion.world_position = cy::Vec3{0.0F, 0.0F, 0.0F};
    CY_REQUIRE(static_cast<bool>(fixture.server.emit_impulse(explosion)));
    CY_CHECK_EQ(fixture.server.live_impulses(), 1U);

    // Settle both cameras onto their anchors first, then advance the oscillators' phase away from
    // zero. The attenuation each camera applies is computed from its own position, which is why the
    // two have to be where they belong before the shake is compared.
    const EvaluationContext context = context_with(nullptr, 0, 1.0F / 60.0F);
    for (int i = 0; i < 6; ++i) {
        CY_REQUIRE(static_cast<bool>(fixture.server.evaluate(*near_rig, context)));
        CY_REQUIRE(static_cast<bool>(fixture.server.evaluate(*far_rig, context)));
    }
    const f32 near_shake = cy::length(fixture.server.evaluated(*near_rig)->pose.translation -
                                      cy::Vec3{0.0F, 0.0F, 0.0F});
    const f32 far_shake = cy::length(fixture.server.evaluated(*far_rig)->pose.translation -
                                     cy::Vec3{90.0F, 0.0F, 0.0F});
    CY_CHECK_GT(near_shake, 0.0F);
    CY_CHECK_GT(near_shake, far_shake);

    // The accessibility setting scales it, once, for every source.
    CameraSettings settings;
    settings.shake_scale = 0.0F;
    fixture.server.set_settings(settings);
    CY_REQUIRE(static_cast<bool>(fixture.server.evaluate(*near_rig, context)));
    CY_CHECK_NEAR(cy::length(fixture.server.evaluated(*near_rig)->pose.translation), 0.0F, 1e-5F);
}

CY_TEST_CASE("an impulse older than its duration is dropped") {
    Fixture fixture;
    CameraImpulse impulse;
    impulse.duration_seconds = 0.5F;
    CY_REQUIRE(static_cast<bool>(fixture.server.emit_impulse(impulse)));
    fixture.server.advance_impulses(0.25F);
    CY_CHECK_EQ(fixture.server.live_impulses(), 1U);
    fixture.server.advance_impulses(0.5F);
    CY_CHECK_EQ(fixture.server.live_impulses(), 0U);
}

CY_TEST_CASE("a full impulse bus is reported rather than silently louder") {
    CameraServer server(allocator());
    CameraServerConfig config;
    config.impulse_capacity = 2;
    CY_REQUIRE(static_cast<bool>(server.configure(config)));
    CY_REQUIRE(static_cast<bool>(server.initialize()));

    const CameraImpulse impulse;
    CY_CHECK(static_cast<bool>(server.emit_impulse(impulse)));
    CY_CHECK(static_cast<bool>(server.emit_impulse(impulse)));
    const cy::Status third = server.emit_impulse(impulse);
    CY_REQUIRE_FALSE(static_cast<bool>(third));
    CY_CHECK_EQ(third.error().code, cy::ErrorCode::OutOfRange);
}

CY_TEST_CASE("the field-of-view override applies once, over whatever the rig produced") {
    Fixture fixture;
    const RigHandle rig = fixture.make_follow_rig();
    const TargetSample samples[] = {sample_at(1, cy::Vec3{})};

    CameraSettings settings;
    settings.field_of_view_override_radians = 1.9F;
    fixture.server.set_settings(settings);
    CY_REQUIRE(
        static_cast<bool>(fixture.server.evaluate(rig, context_with(samples, 1, 1.0F / 60.0F))));

    const EvaluatedCamera* camera = fixture.server.evaluated(rig);
    CY_CHECK_NEAR(camera->lens.vertical_fov_radians(), 1.9F, 1e-5F);
    // And the physical half is kept in step, so a consumer reading the focal length sees the lens
    // that is actually being rendered.
    CY_CHECK_NEAR(camera->lens.physical.focal_length_mm,
                  focal_length_from_fov(1.9F, camera->lens.physical.sensor_height_mm), 1e-3F);
}

CY_TEST_CASE("a pose override is reported as an override rather than as a contribution") {
    Fixture fixture;
    const RigHandle rig = fixture.make_follow_rig();
    const TargetSample samples[] = {sample_at(1, cy::Vec3{})};
    const EvaluationContext context = context_with(samples, 1, 1.0F / 60.0F);

    CY_REQUIRE(static_cast<bool>(fixture.server.evaluate(rig, context)));
    CY_CHECK_FALSE(fixture.server.evaluated(rig)->pose_overridden);

    CY_REQUIRE(static_cast<bool>(fixture.server.override_pose(
        rig, cy::Transform::from_translation(cy::Vec3{5.0F, 5.0F, 5.0F}))));
    CY_REQUIRE(static_cast<bool>(fixture.server.evaluate(rig, context)));
    CY_CHECK(fixture.server.evaluated(rig)->pose_overridden);
    CY_CHECK_NEAR(fixture.server.evaluated(rig)->pose.translation.x, 5.0F, 1e-5F);

    CY_REQUIRE(static_cast<bool>(fixture.server.clear_pose_override(rig)));
    CY_REQUIRE(static_cast<bool>(fixture.server.evaluate(rig, context)));
    CY_CHECK_FALSE(fixture.server.evaluated(rig)->pose_overridden);
}

CY_TEST_CASE("collision queries from several cameras arrive as one batch") {
    // `camera-system`: "**WHEN** several cameras evaluate collision and occlusion in a frame
    // **THEN** their queries SHALL be batched rather than issued individually per node."
    Fixture fixture;
    RigDefinition definition(allocator());
    definition.name = Name::intern("collide");
    (void)definition.nodes.push_back(node("target", nullptr, RigNodeKind::Target));
    (void)definition.nodes.push_back(node("collide", "target", RigNodeKind::Collision));
    (void)definition.nodes.push_back(node("output", "collide", RigNodeKind::Output));
    const auto compiled = fixture.server.create_definition(definition);
    CY_REQUIRE(static_cast<bool>(compiled));

    const auto first = fixture.server.create_rig(*compiled, RigConfig{});
    const auto second = fixture.server.create_rig(*compiled, RigConfig{});
    CY_REQUIRE(static_cast<bool>(first));
    CY_REQUIRE(static_cast<bool>(second));
    TargetBinding binding;
    binding.kind = TargetKind::Entity;
    binding.stable_id = 1;
    CY_REQUIRE(static_cast<bool>(fixture.server.set_target(*first, binding)));
    CY_REQUIRE(static_cast<bool>(fixture.server.set_target(*second, binding)));

    const TargetSample samples[] = {sample_at(1, cy::Vec3{})};
    const EvaluationContext context = context_with(samples, 1, 1.0F / 60.0F);

    fixture.server.begin_frame();
    CY_REQUIRE(static_cast<bool>(fixture.server.evaluate(*first, context)));
    CY_REQUIRE(static_cast<bool>(fixture.server.evaluate(*second, context)));
    // Two cameras, two queries each, in one batch that a physics server resolves in one call.
    CY_CHECK_EQ(fixture.server.queries().size(), 4U);
    CY_CHECK_EQ(fixture.server.queries()[0].rig_bits, first->bits());
    CY_CHECK_EQ(fixture.server.queries()[2].rig_bits, second->bits());

    // And the next frame's batch starts empty.
    fixture.server.begin_frame();
    CY_CHECK_EQ(fixture.server.queries().size(), 0U);
}

CY_TEST_CASE("a stack evaluates every rig it holds and blends the results") {
    Fixture fixture;
    const RigHandle gameplay = fixture.make_follow_rig("gameplay");
    const RigHandle cinematic = fixture.make_follow_rig("cinematic");
    TargetBinding elsewhere;
    elsewhere.kind = TargetKind::Entity;
    elsewhere.stable_id = 2;
    CY_REQUIRE(static_cast<bool>(fixture.server.set_target(cinematic, elsewhere)));

    const auto stack_handle = fixture.server.create_stack();
    CY_REQUIRE(static_cast<bool>(stack_handle));
    CameraStack* stack = fixture.server.stack(*stack_handle);
    CY_REQUIRE(stack != nullptr);

    StackEntry base;
    base.rig = gameplay;
    base.kind = ContributionKind::Base;
    base.blend_in.duration_seconds = 0.0F;
    CY_REQUIRE(static_cast<bool>(stack->push(base)));

    StackEntry take_over;
    take_over.rig = cinematic;
    take_over.kind = ContributionKind::Cinematic;
    take_over.priority = 100;
    take_over.blend_in.duration_seconds = 0.0F;
    CY_REQUIRE(static_cast<bool>(stack->push(take_over)));

    const TargetSample samples[] = {sample_at(1, cy::Vec3{0.0F, 0.0F, 0.0F}),
                                    sample_at(2, cy::Vec3{50.0F, 0.0F, 0.0F})};
    cy::Array<StackContribution> report(allocator());
    EvaluatedCamera out;
    CY_REQUIRE(static_cast<bool>(fixture.server.evaluate_stack(
        *stack_handle, context_with(samples, 2, 1.0F / 60.0F), out, &report)));

    CY_CHECK_NEAR(out.pose.translation.x, 50.0F, 1e-2F);
    CY_REQUIRE_EQ(report.size(), 2U);
    CY_CHECK_EQ(report[1].kind, ContributionKind::Cinematic);
    CY_CHECK_GT(report[1].position_delta, 40.0F);
}

CY_TEST_CASE("an empty stack is an error rather than a camera at the origin") {
    Fixture fixture;
    const auto stack_handle = fixture.server.create_stack();
    CY_REQUIRE(static_cast<bool>(stack_handle));
    EvaluatedCamera out;
    CY_CHECK_FALSE(static_cast<bool>(fixture.server.evaluate_stack(
        *stack_handle, context_with(nullptr, 0, 1.0F / 60.0F), out, nullptr)));
}

CY_TEST_CASE("the trace is collected only when it was asked for") {
    CameraServer server(allocator());
    CY_REQUIRE(static_cast<bool>(server.configure(CameraServerConfig{})));  // collect_trace is off
    CY_REQUIRE(static_cast<bool>(server.initialize()));

    const auto definition = server.create_definition(follow_rig(allocator(), "plain"));
    CY_REQUIRE(static_cast<bool>(definition));
    const auto rig = server.create_rig(*definition, RigConfig{});
    CY_REQUIRE(static_cast<bool>(rig));
    TargetBinding binding;
    binding.kind = TargetKind::Entity;
    binding.stable_id = 1;
    CY_REQUIRE(static_cast<bool>(server.set_target(*rig, binding)));

    const TargetSample samples[] = {sample_at(1, cy::Vec3{})};
    CY_REQUIRE(static_cast<bool>(server.evaluate(*rig, context_with(samples, 1, 0.016F))));
    CY_CHECK_EQ(server.trace(*rig).size(), 0U);
}

CY_TEST_CASE("intent is consumed by the evaluation it was accumulated for") {
    // Leaving it would apply one frame's look delta on every subsequent frame — a camera that keeps
    // turning after the stick is released.
    Fixture fixture;
    RigDefinition definition(allocator());
    definition.name = Name::intern("orbit-only");
    (void)definition.nodes.push_back(node("target", nullptr, RigNodeKind::Target));
    RigNodeDesc orbit = node("orbit", "target", RigNodeKind::Orbit);
    orbit.orbit.near_distance = 5.0F;
    orbit.orbit.far_distance = 5.0F;
    orbit.orbit.distance_half_life = 0.0F;
    (void)definition.nodes.push_back(orbit);
    (void)definition.nodes.push_back(node("output", "orbit", RigNodeKind::Output));

    const auto compiled = fixture.server.create_definition(definition);
    CY_REQUIRE(static_cast<bool>(compiled));
    const auto rig = fixture.server.create_rig(*compiled, RigConfig{});
    CY_REQUIRE(static_cast<bool>(rig));
    TargetBinding binding;
    binding.kind = TargetKind::Entity;
    binding.stable_id = 1;
    CY_REQUIRE(static_cast<bool>(fixture.server.set_target(*rig, binding)));

    CameraIntent turn;
    turn.look = cy::Vec2{0.5F, 0.0F};
    CY_REQUIRE(static_cast<bool>(fixture.server.apply_intent(*rig, turn)));

    const TargetSample samples[] = {sample_at(1, cy::Vec3{})};
    const EvaluationContext context = context_with(samples, 1, 1.0F / 60.0F);
    CY_REQUIRE(static_cast<bool>(fixture.server.evaluate(*rig, context)));
    const cy::Vec3 after_turn = fixture.server.evaluated(*rig)->pose.translation;

    CY_REQUIRE(static_cast<bool>(fixture.server.evaluate(*rig, context)));
    CY_CHECK_NEAR(cy::length(fixture.server.evaluated(*rig)->pose.translation - after_turn), 0.0F,
                  1e-4F);
}

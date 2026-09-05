// The rig graph, its compiler, and what the compiled program does. Tasks 4.3.1 and 4.3.2.
//
// `camera-system` — "Rig graphs compile to programs" and "Composition, not inheritance": a
// third-person camera "SHALL compose follow, orbit, shoulder offset, collision, aim, and noise
// nodes", and evaluation "SHALL execute a compiled program with no per-node allocation or virtual
// dispatch".
//
// THE COMPILE CASES ARE THE HALF WORTH THE MOST. A rig with a node that reaches nothing, or with a
// `Follow` before its `Target`, produces a camera that is *almost* right — which is the hardest
// kind of camera bug to see. Each of those is a compile error here, and each has a case.

#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/camera/rig.h>
#include <cy/test/test.h>

#include <cmath>

using cy::f32;
using cy::Name;
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

/// The third-person composition `camera-system`'s own scenario names.
[[nodiscard]] RigDefinition third_person(cy::Allocator& memory) {
    RigDefinition definition(memory);
    definition.name = Name::intern("third-person");

    RigNodeDesc target = node("target", nullptr, RigNodeKind::Target);
    target.target.anchor_offset = cy::Vec3{0.0F, 1.6F, 0.0F};

    RigNodeDesc orbit = node("orbit", "target", RigNodeKind::Orbit);
    orbit.orbit.near_distance = 4.0F;
    orbit.orbit.far_distance = 4.0F;
    orbit.orbit.distance_half_life = 0.0F;

    RigNodeDesc shoulder = node("shoulder", "orbit", RigNodeKind::Offset);
    shoulder.offset.offset = cy::Vec3{0.5F, 0.0F, 0.0F};

    RigNodeDesc look = node("look", "shoulder", RigNodeKind::LookAt);
    look.look_at.rotation_half_life = 0.0F;

    const RigNodeDesc output = node("output", "look", RigNodeKind::Output);

    (void)definition.nodes.push_back(target);
    (void)definition.nodes.push_back(orbit);
    (void)definition.nodes.push_back(shoulder);
    (void)definition.nodes.push_back(look);
    (void)definition.nodes.push_back(output);
    return definition;
}

[[nodiscard]] RigEvaluationInput input_at(cy::Vec3 target_position, f32 delta) {
    RigEvaluationInput input;
    input.delta_seconds = delta;
    input.binding.kind = TargetKind::Entity;
    input.binding.stable_id = 7;
    input.sample.stable_id = 7;
    input.sample.valid = true;
    input.sample.transform.translation = target_position;
    return input;
}

}  // namespace

CY_TEST_CASE("a definition with no output node is refused") {
    const RigNodeRegistry registry(allocator());
    RigDefinition definition(allocator());
    (void)definition.nodes.push_back(node("target", nullptr, RigNodeKind::Target));

    const auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE_FALSE(static_cast<bool>(compiled));
    CY_CHECK_EQ(compiled.error().code, cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("two output nodes are refused") {
    const RigNodeRegistry registry(allocator());
    RigDefinition definition(allocator());
    (void)definition.nodes.push_back(node("a", nullptr, RigNodeKind::Output));
    (void)definition.nodes.push_back(node("b", "a", RigNodeKind::Output));

    CY_CHECK_FALSE(static_cast<bool>(compile(definition, registry, allocator())));
}

CY_TEST_CASE("a node naming an input that does not exist is refused") {
    const RigNodeRegistry registry(allocator());
    RigDefinition definition(allocator());
    (void)definition.nodes.push_back(node("output", "missing", RigNodeKind::Output));

    const auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE_FALSE(static_cast<bool>(compiled));
    CY_CHECK_EQ(compiled.error().code, cy::ErrorCode::NotFound);
}

CY_TEST_CASE("a cycle is reported rather than looped over") {
    const RigNodeRegistry registry(allocator());
    RigDefinition definition(allocator());
    (void)definition.nodes.push_back(node("a", "b", RigNodeKind::Offset));
    (void)definition.nodes.push_back(node("b", "a", RigNodeKind::Offset));
    (void)definition.nodes.push_back(node("output", "a", RigNodeKind::Output));

    const auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE_FALSE(static_cast<bool>(compiled));
    CY_CHECK_EQ(compiled.error().code, cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("a node that reaches nothing is refused rather than silently dropped") {
    // The camera would look ALMOST right, which is the whole reason this is an error.
    const RigNodeRegistry registry(allocator());
    RigDefinition definition(allocator());
    (void)definition.nodes.push_back(node("target", nullptr, RigNodeKind::Target));
    (void)definition.nodes.push_back(node("orphan", "target", RigNodeKind::Offset));
    (void)definition.nodes.push_back(node("output", "target", RigNodeKind::Output));

    CY_CHECK_FALSE(static_cast<bool>(compile(definition, registry, allocator())));
}

CY_TEST_CASE("two nodes sharing one identity are refused") {
    const RigNodeRegistry registry(allocator());
    RigDefinition definition(allocator());
    (void)definition.nodes.push_back(node("same", nullptr, RigNodeKind::Target));
    (void)definition.nodes.push_back(node("same", "same", RigNodeKind::Output));

    const auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE_FALSE(static_cast<bool>(compiled));
    CY_CHECK_EQ(compiled.error().code, cy::ErrorCode::AlreadyExists);
}

CY_TEST_CASE("a node that frames a target before any target node is refused") {
    const RigNodeRegistry registry(allocator());
    RigDefinition definition(allocator());
    (void)definition.nodes.push_back(node("follow", nullptr, RigNodeKind::Follow));
    (void)definition.nodes.push_back(node("output", "follow", RigNodeKind::Output));

    CY_CHECK_FALSE(static_cast<bool>(compile(definition, registry, allocator())));
}

CY_TEST_CASE("the compiled program is in evaluation order, source first") {
    const RigNodeRegistry registry(allocator());
    const RigDefinition definition = third_person(allocator());
    auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE(static_cast<bool>(compiled));

    const RigProgram& program = *compiled;
    CY_REQUIRE_EQ(program.ops.size(), 5U);
    CY_CHECK_EQ(program.ops[0].kind, RigNodeKind::Target);
    CY_CHECK_EQ(program.ops[1].kind, RigNodeKind::Orbit);
    CY_CHECK_EQ(program.ops[2].kind, RigNodeKind::Offset);
    CY_CHECK_EQ(program.ops[3].kind, RigNodeKind::LookAt);
    CY_CHECK_EQ(program.ops[4].kind, RigNodeKind::Output);
    CY_CHECK(program.has_target);
    CY_CHECK_FALSE(program.emits_queries);
}

CY_TEST_CASE("the compiled order is the graph's, not the declaration's") {
    // Declared output-first. A compiler that emitted the array as authored would evaluate the look
    // before the orbit and produce a camera pointing the wrong way.
    const RigNodeRegistry registry(allocator());
    RigDefinition definition(allocator());
    (void)definition.nodes.push_back(node("output", "orbit", RigNodeKind::Output));
    (void)definition.nodes.push_back(node("orbit", "target", RigNodeKind::Orbit));
    (void)definition.nodes.push_back(node("target", nullptr, RigNodeKind::Target));

    auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE(static_cast<bool>(compiled));
    CY_CHECK_EQ(compiled->ops[0].kind, RigNodeKind::Target);
    CY_CHECK_EQ(compiled->ops[1].kind, RigNodeKind::Orbit);
    CY_CHECK_EQ(compiled->ops[2].kind, RigNodeKind::Output);
}

CY_TEST_CASE("a third-person rig places the camera behind and above its target, looking at it") {
    const RigNodeRegistry registry(allocator());
    const RigDefinition definition = third_person(allocator());
    auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE(static_cast<bool>(compiled));

    RigState state;
    RigFrame frame;
    const RigEvaluationInput input = input_at(cy::Vec3{0.0F, 0.0F, 0.0F}, 1.0F / 60.0F);
    CY_REQUIRE(static_cast<bool>(evaluate(*compiled, registry, state, input, frame, nullptr)));

    // The anchor is the target plus the head offset; the camera is four metres behind it along +Z
    // (the camera looks down −Z), shifted half a metre onto the right shoulder.
    CY_CHECK_NEAR(frame.anchor.y, 1.6F, 1e-4F);
    CY_CHECK_NEAR(frame.position.z, 4.0F, 1e-3F);
    CY_CHECK_NEAR(frame.position.x, 0.5F, 1e-3F);
    // And it is looking back at the anchor.
    const cy::Vec3 forward = frame.rotation * cy::kAxisForward;
    const cy::Vec3 to_anchor = cy::normalize(frame.anchor - frame.position);
    CY_CHECK_GT(cy::dot(forward, to_anchor), 0.99F);
}

CY_TEST_CASE("look intent turns the camera and pitch is clamped to its limits") {
    const RigNodeRegistry registry(allocator());
    const RigDefinition definition = third_person(allocator());
    auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE(static_cast<bool>(compiled));

    RigState state;
    RigFrame frame;
    RigEvaluationInput input = input_at(cy::Vec3{0.0F, 0.0F, 0.0F}, 1.0F / 60.0F);
    input.intent.look = cy::Vec2{0.0F, 100.0F};  // far past the limit
    CY_REQUIRE(static_cast<bool>(evaluate(*compiled, registry, state, input, frame, nullptr)));
    CY_CHECK_NEAR(frame.pitch_radians, 1.4F, 1e-4F);

    input.intent.look = cy::Vec2{0.0F, -100.0F};
    CY_REQUIRE(static_cast<bool>(evaluate(*compiled, registry, state, input, frame, nullptr)));
    CY_CHECK_NEAR(frame.pitch_radians, -1.4F, 1e-4F);
}

CY_TEST_CASE("a dead zone means small target movement produces no camera movement") {
    // `camera-system`'s "A dead zone avoids jitter" scenario.
    const RigNodeRegistry registry(allocator());
    RigDefinition definition(allocator());
    (void)definition.nodes.push_back(node("target", nullptr, RigNodeKind::Target));
    RigNodeDesc follow = node("follow", "target", RigNodeKind::Follow);
    follow.follow.space = FollowSpace::World;
    follow.follow.offset = cy::Vec3{0.0F, 0.0F, 5.0F};
    follow.follow.position_half_life = 0.0F;
    follow.follow.dead_zone = 0.5F;
    (void)definition.nodes.push_back(follow);
    (void)definition.nodes.push_back(node("output", "follow", RigNodeKind::Output));

    auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE(static_cast<bool>(compiled));

    RigState state;
    RigFrame frame;
    CY_REQUIRE(static_cast<bool>(evaluate(
        *compiled, registry, state, input_at(cy::Vec3{0.0F, 0.0F, 0.0F}, 0.016F), frame, nullptr)));
    const cy::Vec3 settled = frame.position;

    // Inside the dead zone: nothing moves.
    CY_REQUIRE(static_cast<bool>(evaluate(
        *compiled, registry, state, input_at(cy::Vec3{0.2F, 0.0F, 0.0F}, 0.016F), frame, nullptr)));
    CY_CHECK_NEAR(cy::length(frame.position - settled), 0.0F, 1e-5F);

    // Outside it: it does.
    CY_REQUIRE(static_cast<bool>(evaluate(
        *compiled, registry, state, input_at(cy::Vec3{2.0F, 0.0F, 0.0F}, 0.016F), frame, nullptr)));
    CY_CHECK_GT(cy::length(frame.position - settled), 1.0F);
}

CY_TEST_CASE("a constraint clamps the camera into its region without a second camera type") {
    const RigNodeRegistry registry(allocator());
    RigDefinition definition(allocator());
    (void)definition.nodes.push_back(node("target", nullptr, RigNodeKind::Target));
    RigNodeDesc follow = node("follow", "target", RigNodeKind::Follow);
    follow.follow.space = FollowSpace::World;
    follow.follow.offset = cy::Vec3{0.0F, 0.0F, 5.0F};
    follow.follow.position_half_life = 0.0F;
    (void)definition.nodes.push_back(follow);
    RigNodeDesc rail = node("rail", "follow", RigNodeKind::Constraint);
    // A side-scroller: one axis constrained, the others free. Zero extents leave an axis alone.
    rail.constraint.region_center = cy::Vec3{0.0F, 0.0F, 0.0F};
    rail.constraint.region_extents = cy::Vec3{3.0F, 0.0F, 0.0F};
    (void)definition.nodes.push_back(rail);
    (void)definition.nodes.push_back(node("output", "rail", RigNodeKind::Output));

    auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE(static_cast<bool>(compiled));

    RigState state;
    RigFrame frame;
    CY_REQUIRE(
        static_cast<bool>(evaluate(*compiled, registry, state,
                                   input_at(cy::Vec3{50.0F, 0.0F, 0.0F}, 0.016F), frame, nullptr)));
    CY_CHECK_NEAR(frame.position.x, 3.0F, 1e-4F);
    CY_CHECK_NEAR(frame.position.z, 5.0F, 1e-4F);  // unconstrained axis untouched
}

CY_TEST_CASE("a collision node publishes queries and never casts one") {
    const RigNodeRegistry registry(allocator());
    RigDefinition definition(allocator());
    (void)definition.nodes.push_back(node("target", nullptr, RigNodeKind::Target));
    RigNodeDesc orbit = node("orbit", "target", RigNodeKind::Orbit);
    orbit.orbit.near_distance = 6.0F;
    orbit.orbit.far_distance = 6.0F;
    orbit.orbit.distance_half_life = 0.0F;
    (void)definition.nodes.push_back(orbit);
    (void)definition.nodes.push_back(node("collide", "orbit", RigNodeKind::Collision));
    (void)definition.nodes.push_back(node("output", "collide", RigNodeKind::Output));

    auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE(static_cast<bool>(compiled));
    CY_CHECK(compiled->emits_queries);

    cy::Array<CameraQuery> queries(allocator());
    RigState state;
    RigFrame frame;
    RigEvaluationInput input = input_at(cy::Vec3{0.0F, 0.0F, 0.0F}, 0.016F);
    input.queries = &queries;
    input.rig_bits = 0x1234;
    CY_REQUIRE(static_cast<bool>(evaluate(*compiled, registry, state, input, frame, nullptr)));

    // Two queries, batched, both tagged with the rig that asked.
    CY_REQUIRE_EQ(queries.size(), 2U);
    CY_CHECK_EQ(queries[0].kind, CameraQuery::Kind::Collision);
    CY_CHECK_EQ(queries[1].kind, CameraQuery::Kind::Occlusion);
    CY_CHECK_EQ(queries[0].rig_bits, 0x1234U);
    CY_CHECK_NEAR(cy::length(frame.position - frame.anchor), 6.0F, 1e-3F);
}

CY_TEST_CASE("a collision result pulls the camera in, and glass occludes without colliding") {
    // `camera-system`: "**WHEN** a transparent surface stands between camera and target **THEN**
    // the occlusion response SHALL apply, and the camera SHALL NOT be pushed as though it had
    // collided."
    const RigNodeRegistry registry(allocator());
    RigDefinition definition(allocator());
    (void)definition.nodes.push_back(node("target", nullptr, RigNodeKind::Target));
    RigNodeDesc orbit = node("orbit", "target", RigNodeKind::Orbit);
    orbit.orbit.near_distance = 6.0F;
    orbit.orbit.far_distance = 6.0F;
    orbit.orbit.distance_half_life = 0.0F;
    (void)definition.nodes.push_back(orbit);
    RigNodeDesc collide = node("collide", "orbit", RigNodeKind::Collision);
    collide.collision.collision_response = CollisionResponse::PullIn;
    collide.collision.occlusion_response = CollisionResponse::FadeObstacle;
    (void)definition.nodes.push_back(collide);
    (void)definition.nodes.push_back(node("output", "collide", RigNodeKind::Output));

    auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE(static_cast<bool>(compiled));

    RigState state;
    RigFrame frame;
    RigEvaluationInput input = input_at(cy::Vec3{0.0F, 0.0F, 0.0F}, 0.016F);
    input.rig_bits = 9;

    CY_TEST_SUBCASE("an opaque wall pulls the camera in") {
        CameraQueryResult hit;
        hit.kind = CameraQuery::Kind::Collision;
        hit.rig_bits = 9;
        hit.fraction = 0.5F;
        const CameraQueryResult results[] = {hit};
        input.query_results = cy::Span<const CameraQueryResult>(results, 1);
        CY_REQUIRE(static_cast<bool>(evaluate(*compiled, registry, state, input, frame, nullptr)));
        CY_CHECK_NEAR(cy::length(frame.position - frame.anchor), 3.0F, 1e-3F);
        CY_CHECK_NEAR(frame.obstacle_fade, 0.0F, 1e-6F);
    }

    CY_TEST_SUBCASE("glass occludes without colliding") {
        CameraQueryResult glass;
        glass.kind = CameraQuery::Kind::Collision;
        glass.rig_bits = 9;
        glass.fraction = 0.5F;
        glass.transparent = true;
        CameraQueryResult occluded;
        occluded.kind = CameraQuery::Kind::Occlusion;
        occluded.rig_bits = 9;
        occluded.blocked_fraction = 1.0F;
        const CameraQueryResult results[] = {glass, occluded};
        input.query_results = cy::Span<const CameraQueryResult>(results, 2);
        CY_REQUIRE(static_cast<bool>(evaluate(*compiled, registry, state, input, frame, nullptr)));
        // NOT pushed: the transparent hit is dropped from the collision answer.
        CY_CHECK_NEAR(cy::length(frame.position - frame.anchor), 6.0F, 1e-3F);
        // But the obstacle is asked to fade.
        CY_CHECK_GT(frame.obstacle_fade, 0.0F);
    }
}

namespace {

/// A project-supplied node kind: raises the camera by a metre. The extension point
/// `project-and-plugins` requires, exercised rather than asserted to exist.
void raise_by_one(const RigOp& op, const RigEvaluationInput& input, RigFrame& frame,
                  void* user) noexcept {
    (void)op;
    (void)input;
    auto* count = static_cast<int*>(user);
    if (count != nullptr) {
        ++(*count);
    }
    frame.position.y += 1.0F;
}

}  // namespace

CY_TEST_CASE("a project registers a custom node kind and the program executes it") {
    int calls = 0;
    RigNodeRegistry registry(allocator());
    CY_REQUIRE(
        static_cast<bool>(registry.register_kind(Name::intern("raise"), &raise_by_one, &calls)));
    // A second registration under one name is refused: which one ran would otherwise depend on
    // registration order.
    CY_CHECK_FALSE(
        static_cast<bool>(registry.register_kind(Name::intern("raise"), &raise_by_one, &calls)));

    RigDefinition definition(allocator());
    (void)definition.nodes.push_back(node("target", nullptr, RigNodeKind::Target));
    RigNodeDesc custom = node("raise", "target", RigNodeKind::Custom);
    custom.custom_kind = Name::intern("raise");
    (void)definition.nodes.push_back(custom);
    (void)definition.nodes.push_back(node("output", "raise", RigNodeKind::Output));

    auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE(static_cast<bool>(compiled));

    RigState state;
    RigFrame frame;
    CY_REQUIRE(static_cast<bool>(evaluate(
        *compiled, registry, state, input_at(cy::Vec3{0.0F, 0.0F, 0.0F}, 0.016F), frame, nullptr)));
    CY_CHECK_EQ(calls, 1);
    CY_CHECK_NEAR(frame.position.y, 1.0F, 1e-5F);
}

CY_TEST_CASE("a custom node naming an unregistered kind is refused at compile time") {
    const RigNodeRegistry registry(allocator());
    RigDefinition definition(allocator());
    RigNodeDesc custom = node("custom", nullptr, RigNodeKind::Custom);
    custom.custom_kind = Name::intern("never-registered");
    (void)definition.nodes.push_back(custom);
    (void)definition.nodes.push_back(node("output", "custom", RigNodeKind::Output));

    const auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE_FALSE(static_cast<bool>(compiled));
    CY_CHECK_EQ(compiled.error().code, cy::ErrorCode::NotFound);
}

CY_TEST_CASE("the trace says which node moved the camera and by how much") {
    // `camera-system`: "**WHEN** the camera is not where expected **THEN** the inspector SHALL show
    // which node or contribution moved it and by how much."
    const RigNodeRegistry registry(allocator());
    const RigDefinition definition = third_person(allocator());
    auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE(static_cast<bool>(compiled));

    cy::Array<RigOpTrace> trace(allocator());
    RigState state;
    RigFrame frame;
    CY_REQUIRE(static_cast<bool>(evaluate(
        *compiled, registry, state, input_at(cy::Vec3{0.0F, 0.0F, 0.0F}, 0.016F), frame, &trace)));

    CY_REQUIRE_EQ(trace.size(), 5U);
    CY_CHECK_EQ(trace[1].kind, RigNodeKind::Orbit);
    CY_CHECK_GT(cy::length(trace[1].position_after - trace[1].position_before), 1.0F);
    // The shoulder offset moved it by exactly half a metre, which is what it was authored to do.
    CY_CHECK_EQ(trace[2].id, Name::intern("shoulder"));
    CY_CHECK_NEAR(cy::length(trace[2].position_after - trace[2].position_before), 0.5F, 1e-3F);
    // And the look-at turned it without moving it.
    CY_CHECK_NEAR(cy::length(trace[3].position_after - trace[3].position_before), 0.0F, 1e-5F);
    CY_CHECK_GT(trace[3].rotation_delta_radians, 0.0F);
}

CY_TEST_CASE("a reset re-primes the rig, so the next evaluation snaps rather than eases") {
    const RigNodeRegistry registry(allocator());
    RigDefinition definition(allocator());
    (void)definition.nodes.push_back(node("target", nullptr, RigNodeKind::Target));
    RigNodeDesc follow = node("follow", "target", RigNodeKind::Follow);
    follow.follow.space = FollowSpace::World;
    follow.follow.offset = cy::Vec3{0.0F, 0.0F, 5.0F};
    follow.follow.position_half_life = 1.0F;  // very slow
    (void)definition.nodes.push_back(follow);
    (void)definition.nodes.push_back(node("output", "follow", RigNodeKind::Output));

    auto compiled = compile(definition, registry, allocator());
    CY_REQUIRE(static_cast<bool>(compiled));

    RigState state;
    RigFrame frame;
    CY_REQUIRE(static_cast<bool>(evaluate(
        *compiled, registry, state, input_at(cy::Vec3{0.0F, 0.0F, 0.0F}, 0.016F), frame, nullptr)));

    // Without the reset the camera would take a second to reach a target a hundred metres away.
    state.reset(cy::Transform::from_translation(cy::Vec3{100.0F, 0.0F, 5.0F}), Lens{});
    CY_REQUIRE(static_cast<bool>(evaluate(*compiled, registry, state,
                                          input_at(cy::Vec3{100.0F, 0.0F, 0.0F}, 0.016F), frame,
                                          nullptr)));
    CY_CHECK_NEAR(frame.position.x, 100.0F, 1e-3F);
}

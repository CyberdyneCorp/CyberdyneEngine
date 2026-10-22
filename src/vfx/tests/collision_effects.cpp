// SPDX-License-Identifier: MIT
// The colliding effects. See collision_effects.h.

#include "collision_effects.h"

#include <cstring>
#include <utility>

namespace cy::vfx_test {
namespace {

[[nodiscard]] Status declare(Emitter& emitter, const char* name, const char* type) noexcept {
    AttributeDecl decl;
    decl.name = Name::intern(name);
    decl.type = Name::intern(type);
    decl.minimum = -1000.0F;
    decl.maximum = 1000.0F;
    return emitter.declare_attribute(decl);
}

[[nodiscard]] Status declare_attributes(Emitter& emitter) noexcept {
    static constexpr const char* kRows[][2] = {
        {"position", "float3"}, {"velocity", "float3"}, {"collided", "float"},
        {"age", "float"},       {"lifetime", "float"},  {"size", "float"},
        {"color", "float4"},    {"emission", "float"},
    };
    for (const auto& row : kRows) {
        if (Status declared = declare(emitter, row[0], row[1]); !declared) {
            return declared;
        }
    }
    return ok();
}

/// `base + spread * (2r - 1)`, a component at a time, each from its own random stream.
[[nodiscard]] NodeKey scattered(StageBuilder& stage, Vec3 base, Vec3 spread) noexcept {
    const NodeKey one = stage.constant(1.0F);
    const NodeKey two = stage.constant(2.0F);
    const auto axis = [&](f32 centre, f32 width) noexcept {
        const NodeKey signed_random =
            stage.binary("vfx.sub", stage.binary("vfx.mul", stage.random(), two), one);
        return stage.binary("vfx.add", stage.constant(centre),
                            stage.binary("vfx.mul", signed_random, stage.constant(width)));
    };
    return stage.make3(axis(base.x, spread.x), axis(base.y, spread.y), axis(base.z, spread.z));
}

[[nodiscard]] Status build_spawn(Allocator& allocator, Emitter& emitter,
                                 const DropScenario& scenario) noexcept {
    StageBuilder stage(allocator, "spawn");
    const NodeKey young = stage.binary("vfx.less", stage.input(input::kEmitterAge),
                                       stage.constant(scenario.spawn_until));
    stage.spawn_count(stage.ternary("vfx.select", young,
                                    stage.constant(static_cast<f32>(scenario.spawn_per_step)),
                                    stage.constant(0.0F)));
    if (!stage.ok()) {
        return fail(ErrorCode::Internal, "collision test: the spawn graph did not author");
    }
    return emitter.set_stage(Stage::Spawn, stage.take());
}

void write_presentation(StageBuilder& stage, const DropScenario& scenario) noexcept {
    stage.write("size", stage.constant(scenario.size));
    stage.write("color",
                stage.make4(stage.constant(scenario.tint.x), stage.constant(scenario.tint.y),
                            stage.constant(scenario.tint.z), stage.constant(1.0F)));
    stage.write("emission", stage.constant(scenario.emission));
}

[[nodiscard]] Status build_initialise(Allocator& allocator, Emitter& emitter,
                                      const DropScenario& scenario) noexcept {
    StageBuilder stage(allocator, "initialise");
    stage.write("position", scattered(stage, scenario.start, scenario.spread));
    stage.write("velocity", scattered(stage, scenario.velocity, scenario.velocity_spread));
    stage.write("collided", stage.constant(0.0F));
    stage.write("age", stage.constant(0.0F));
    stage.write("lifetime", stage.constant(scenario.lifetime));
    write_presentation(stage, scenario);
    if (!stage.ok()) {
        return fail(ErrorCode::Internal, "collision test: the initialise graph did not author");
    }
    return emitter.set_stage(Stage::Initialise, stage.take());
}

/// The shape's own inputs, by the names its node declares.
void set_shape(StageBuilder& stage, NodeKey collider, const DropScenario& scenario) noexcept {
    const f32* a = scenario.a;
    const f32* b = scenario.b;
    const std::string_view shape = scenario.shape;
    if (shape == "plane") {
        stage.number(collider, "normal", a[0], a[1], a[2], 0.0F, "vec3");
        stage.number(collider, "offset", a[3]);
    } else if (shape == "sphere") {
        stage.number(collider, "centre", a[0], a[1], a[2], 0.0F, "vec3");
        stage.number(collider, "sphere_radius", a[3]);
    } else if (shape == "box") {
        stage.number(collider, "centre", a[0], a[1], a[2], 0.0F, "vec3");
        stage.number(collider, "half_extents", b[0], b[1], b[2], 0.0F, "vec3");
    } else if (shape == "capsule") {
        stage.number(collider, "start", a[0], a[1], a[2], 0.0F, "vec3");
        stage.number(collider, "capsule_radius", a[3]);
        stage.number(collider, "end", b[0], b[1], b[2], 0.0F, "vec3");
    } else if (shape == "sdf") {
        stage.number(collider, "step", a[0]);
    } else if (shape == "depth") {
        stage.number(collider, "thickness", a[0]);
    }
}

[[nodiscard]] Status build_update(Allocator& allocator, Emitter& emitter,
                                  const DropScenario& scenario) noexcept {
    StageBuilder stage(allocator, "update");
    const NodeKey dt = stage.input(input::kDeltaTime);
    const NodeKey velocity = stage.attribute("velocity");
    const NodeKey position = stage.attribute("position");
    const NodeKey collided = stage.attribute("collided");
    const NodeKey gravity =
        stage.constant(scenario.gravity.x, scenario.gravity.y, scenario.gravity.z, 0.0F, "float3");

    NodeKey moving = stage.binary("vfx.add", velocity, stage.binary("vfx.mul", gravity, dt));
    if (scenario.hold_when_stuck) {
        // A stuck particle does not integrate: its velocity is multiplied by `1 - collided`.
        moving = stage.binary("vfx.mul", moving,
                              stage.binary("vfx.sub", stage.constant(1.0F), collided));
    }
    const NodeKey moved = stage.binary("vfx.add", position, stage.binary("vfx.mul", moving, dt));

    const NodeKey collider = stage.collide(scenario.shape);
    stage.connect(moved, "out", collider, "position");
    stage.connect(moving, "out", collider, "velocity");
    stage.connect(collided, "out", collider, "was_in_contact");
    stage.number(collider, "radius", scenario.radius);
    stage.number(collider, "restitution", scenario.restitution);
    stage.number(collider, "friction", scenario.friction);
    stage.number(collider, "stick", scenario.stick);
    stage.number(collider, "rest_speed", scenario.rest_speed);
    set_shape(stage, collider, scenario);
    if (scenario.mute_collider && !stage.graph().mute(collider, true).has_value()) {
        return fail(ErrorCode::Internal, "collision test: the collider would not mute");
    }

    stage.write("position", collider, "position");
    stage.write("velocity", collider, "velocity");
    stage.write("collided", collider, "contact");
    // A PARTICLE TOUCHING SOMETHING GLOWS TWICE AS BRIGHT. It is what the pictures show, and it is
    // also what keeps `collided` live in an effect whose collider does not read it back: liveness
    // elides an attribute nothing reads, and the suites read this one.
    stage.write("emission", stage.binary("vfx.mul", stage.constant(scenario.emission),
                                         stage.binary("vfx.add", stage.constant(1.0F), collided)));
    const NodeKey age = stage.binary("vfx.add", stage.attribute("age"), dt);
    stage.write("age", age);
    const NodeKey expired = stage.binary("vfx.greater", age, stage.attribute("lifetime"));
    if (scenario.kill_on_contact) {
        // ONE KILL ROOT A KERNEL, so the two reasons are one predicate: expired, or touching.
        const NodeKey old_enough =
            stage.ternary("vfx.select", expired, stage.constant(1.0F), stage.constant(0.0F));
        const NodeKey either = stage.node("vfx.max");
        stage.connect(old_enough, "out", either, "a");
        stage.connect(collider, "contact", either, "b");
        stage.kill_if(either);
    } else {
        stage.kill_if(expired);
    }
    if (scenario.raise_impacts) {
        stage.emit_event_with("impact", collider, "impact", collider, "position");
    }
    if (!stage.ok()) {
        return fail(ErrorCode::Internal, "collision test: the update graph did not author");
    }
    return emitter.set_stage(Stage::Update, stage.take());
}

/// The second emitter: one spark an impact, born where the impact was and thrown upwards.
[[nodiscard]] Expected<Emitter, Error> build_sparks(Allocator& allocator,
                                                    const DropScenario& scenario) noexcept {
    Emitter emitter(allocator, Name::intern("sparks"));
    // Room for every spark the drops' impacts ask for: a full block would spawn fewer, by design,
    // and the suite asks about the mapping from impacts to sparks rather than about the cap.
    emitter.set_capacity(scenario.capacity * 8U);
    emitter.set_path(SimulationPath::CpuRequired);
    if (Status declared = declare_attributes(emitter); !declared) {
        return make_unexpected(declared.error());
    }
    StageBuilder spawn(allocator, "spawn");
    spawn.spawn_count(spawn.event_read("vfx.event_count", "impact"));
    if (Status set = emitter.set_stage(Stage::Spawn, spawn.take()); !set) {
        return make_unexpected(set.error());
    }
    StageBuilder initialise(allocator, "initialise");
    // The payload is a float4 and a position is a float3: `dot` against an axis is how this node
    // library reads one component, so three of them make the position.
    const NodeKey payload = initialise.event_read("vfx.event_payload", "impact");
    const auto axis = [&initialise, payload](f32 x, f32 y, f32 z) noexcept {
        return initialise.binary("vfx.dot", payload, initialise.constant(x, y, z, 0.0F, "float4"));
    };
    initialise.write("position", initialise.make3(axis(1.0F, 0.0F, 0.0F), axis(0.0F, 1.0F, 0.0F),
                                                  axis(0.0F, 0.0F, 1.0F)));
    initialise.write("velocity",
                     scattered(initialise, Vec3{0.0F, 2.0F, 0.0F}, Vec3{1.0F, 0.5F, 1.0F}));
    initialise.write("collided", initialise.constant(0.0F));
    initialise.write("age", initialise.constant(0.0F));
    initialise.write("lifetime", initialise.constant(0.4F));
    DropScenario spark = scenario;
    spark.tint = Vec3{1.0F, 0.8F, 0.3F};
    spark.size = scenario.size * 0.5F;
    write_presentation(initialise, spark);
    if (!initialise.ok()) {
        return make_unexpected(Error{ErrorCode::Internal, "sparks: initialise did not author"});
    }
    if (Status set = emitter.set_stage(Stage::Initialise, initialise.take()); !set) {
        return make_unexpected(set.error());
    }
    StageBuilder update(allocator, "update");
    const NodeKey dt = update.input(input::kDeltaTime);
    const NodeKey moving = update.binary(
        "vfx.add", update.attribute("velocity"),
        update.binary("vfx.mul", update.constant(0.0F, -9.81F, 0.0F, 0.0F, "float3"), dt));
    update.write("velocity", moving);
    update.write("position", update.binary("vfx.add", update.attribute("position"),
                                           update.binary("vfx.mul", moving, dt)));
    const NodeKey age = update.binary("vfx.add", update.attribute("age"), dt);
    update.write("age", age);
    update.kill_if(update.binary("vfx.greater", age, update.attribute("lifetime")));
    if (!update.ok()) {
        return make_unexpected(Error{ErrorCode::Internal, "sparks: update did not author"});
    }
    if (Status set = emitter.set_stage(Stage::Update, update.take()); !set) {
        return make_unexpected(set.error());
    }
    return emitter;
}

}  // namespace

Expected<VfxSystemAsset, Error> build_drop(Allocator& allocator,
                                           const DropScenario& scenario) noexcept {
    VfxSystemAsset asset(allocator, Name::intern("collision_drop"));
    asset.set_importance(ImportanceClass::Important);
    if (scenario.raise_impacts || scenario.sparks) {
        EventChannelDecl impact;
        impact.name = Name::intern("impact");
        impact.max_events_per_frame = kImpactEventsPerFrame;
        impact.max_chain_depth = 2;
        if (Status declared = asset.declare_channel(impact); !declared) {
            return make_unexpected(declared.error());
        }
    }
    Emitter emitter(allocator, Name::intern("drops"));
    emitter.set_capacity(scenario.capacity);
    emitter.set_path(scenario.path);
    if (Status declared = declare_attributes(emitter); !declared) {
        return make_unexpected(declared.error());
    }
    if (Status built = build_spawn(allocator, emitter, scenario); !built) {
        return make_unexpected(built.error());
    }
    if (Status built = build_initialise(allocator, emitter, scenario); !built) {
        return make_unexpected(built.error());
    }
    if (Status built = build_update(allocator, emitter, scenario); !built) {
        return make_unexpected(built.error());
    }
    if (Status added = asset.add_emitter(std::move(emitter)); !added) {
        return make_unexpected(added.error());
    }
    if (scenario.sparks) {
        auto sparks = build_sparks(allocator, scenario);
        if (!sparks) {
            return make_unexpected(sparks.error());
        }
        if (Status added = asset.add_emitter(std::move(*sparks)); !added) {
            return make_unexpected(added.error());
        }
    }
    return asset;
}

Expected<CompiledSystem, Error> cook_drop(Allocator& allocator, const DropScenario& scenario,
                                          graph::DiagnosticSink& sink,
                                          CompileReport& report) noexcept {
    NodeRegistry registry(allocator);
    DataInterfaceRegistry interfaces(allocator);
    if (Status prepared = prepare(registry, interfaces); !prepared) {
        return make_unexpected(prepared.error());
    }
    auto asset = build_drop(allocator, scenario);
    if (!asset) {
        return make_unexpected(asset.error());
    }
    asset->resolve(registry);
    return compile_system(*asset, registry, interfaces, CompileOptions{}, sink, report);
}

}  // namespace cy::vfx_test

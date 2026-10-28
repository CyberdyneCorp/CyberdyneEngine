// SPDX-License-Identifier: MIT
// battle_effects.cpp — the battle's five effects. See battle_effects.h.

#include "battle_effects.h"

#include <cy/core/math/scalar.h>
#include <cy/vfx/asset.h>
#include <cy/vfx/interfaces.h>
#include <cy/vfx/ir.h>

#include <utility>

// src/vfx/tests/effects.h: `StageBuilder`, the node-by-node graph builder every VFX suite and the
// first-person sample author with. Compiled into this sample rather than copied.
#include "effects.h"

namespace sample::rts {
namespace {

using cy::Error;
using cy::Expected;
using cy::f32;
using cy::Name;
using cy::Status;
using cy::u32;
using cy::graph::NodeKey;
using cy::vfx_test::StageBuilder;

/// Where a particle is thrown.
enum class Throw : cy::u8 {
    /// Along the `direction` parameter: a launch.
    Along,
    /// Off the struck surface, along the `normal` parameter: sparks.
    Normal,
    /// Every way at once: a blast.
    Radial,
    /// Up: smoke.
    Up,
};

/// One emitter's look: how many, where, how fast, how long, how big and how bright.
struct Look {
    const char* emitter;
    u32 capacity;
    f32 burst;
    Throw thrown;
    f32 jitter;  ///< metres of random spread at birth
    f32 speed;   ///< metres a second along the throw, plus up to `speed_span`
    f32 speed_span;
    f32 scatter;  ///< metres a second of random sideways speed
    f32 life;     ///< seconds, plus up to `life_span`
    f32 life_span;
    f32 size_from;
    f32 size_to;
    f32 colour[4];
    f32 glow_from;  ///< the radiance multiplier at birth and at death
    f32 glow_to;
    f32 gravity;  ///< metres a second squared, down
    f32 drag;     ///< the share of speed lost a second
    bool bounce;  ///< off the `floor` parameter
};

// The looks. Radiance is the frame's: under a 22 000 lux sun and an exposure of -11 stops, a few
// thousand is a sunlit grey and tens of thousands glow.
constexpr Look kLaunch = {"dust",
                          24,
                          12.0F,
                          Throw::Along,
                          0.05F,
                          2.0F,
                          3.0F,
                          0.9F,
                          0.18F,
                          0.2F,
                          0.07F,
                          0.16F,
                          {0.92F, 0.78F, 0.55F, 0.7F},
                          9000.0F,
                          1500.0F,
                          2.0F,
                          4.0F,
                          false};
constexpr Look kTrailHead = {"head",
                             2,
                             1.0F,
                             Throw::Up,
                             0.0F,
                             0.0F,
                             0.0F,
                             0.0F,
                             0.05F,
                             0.02F,
                             0.11F,
                             0.06F,
                             {1.0F, 0.74F, 0.38F, 0.9F},
                             26000.0F,
                             9000.0F,
                             0.0F,
                             0.0F,
                             false};
constexpr Look kTrailWisp = {"wisp",
                             4,
                             2.0F,
                             Throw::Up,
                             0.05F,
                             0.1F,
                             0.15F,
                             0.1F,
                             0.45F,
                             0.3F,
                             0.16F,
                             0.42F,
                             {0.72F, 0.7F, 0.66F, 0.4F},
                             7000.0F,
                             3500.0F,
                             -0.3F,
                             1.5F,
                             false};
constexpr Look kSparks = {"sparks",
                          32,
                          22.0F,
                          Throw::Normal,
                          0.03F,
                          2.0F,
                          3.0F,
                          2.2F,
                          0.35F,
                          0.25F,
                          0.035F,
                          0.012F,
                          {1.0F, 0.6F, 0.2F, 1.0F},
                          60000.0F,
                          2000.0F,
                          9.81F,
                          0.5F,
                          true};
constexpr Look kFireball = {"fireball",
                            48,
                            36.0F,
                            Throw::Radial,
                            0.4F,
                            1.5F,
                            3.0F,
                            1.0F,
                            0.3F,
                            0.35F,
                            0.45F,
                            1.1F,
                            {1.0F, 0.55F, 0.18F, 0.85F},
                            70000.0F,
                            4000.0F,
                            -1.5F,
                            2.5F,
                            false};
constexpr Look kEmbers = {"embers",
                          40,
                          30.0F,
                          Throw::Radial,
                          0.2F,
                          4.0F,
                          5.0F,
                          1.5F,
                          0.6F,
                          0.6F,
                          0.05F,
                          0.02F,
                          {1.0F, 0.5F, 0.15F, 1.0F},
                          50000.0F,
                          3000.0F,
                          9.81F,
                          0.3F,
                          true};
constexpr Look kSmoke = {"smoke",
                         24,
                         10.0F,
                         Throw::Up,
                         0.6F,
                         0.6F,
                         0.6F,
                         0.35F,
                         1.6F,
                         1.2F,
                         0.5F,
                         1.8F,
                         {0.62F, 0.6F, 0.58F, 0.5F},
                         7000.0F,
                         4500.0F,
                         -0.15F,
                         0.4F,
                         false};

/// The attributes every emitter carries, as the first-person sample declares them.
[[nodiscard]] Status declare_attributes(cy::vfx::Emitter& emitter) noexcept {
    struct Row {
        const char* name;
        const char* type;
        f32 minimum;
        f32 maximum;
        f32 tolerance;
    };
    static constexpr Row kRows[] = {
        {"position", "float3", -64.0F, 64.0F, 0.0F},
        {"velocity", "float3", -64.0F, 64.0F, 0.0F},
        {"age", "float", 0.0F, 8.0F, 0.0F},
        {"lifetime", "float", 0.0F, 8.0F, 0.0F},
        {"size", "float", 0.0F, 4.0F, 0.0F},
        {"color", "float4", 0.0F, 1.0F, 1.0F / 255.0F},
        {"emission", "float", 0.0F, 200000.0F, 0.0F},
    };
    for (const Row& row : kRows) {
        cy::vfx::AttributeDecl decl;
        decl.name = Name::intern(row.name);
        decl.type = Name::intern(row.type);
        decl.minimum = row.minimum;
        decl.maximum = row.maximum;
        decl.tolerance = row.tolerance;
        if (Status declared = emitter.declare_attribute(decl); !declared) {
            return declared;
        }
    }
    return cy::ok();
}

[[nodiscard]] Status declare(cy::vfx::VfxSystemAsset& asset, const char* name, const char* type,
                             f32 x, f32 y, f32 z) noexcept {
    cy::vfx::ParameterDecl parameter;
    parameter.name = Name::intern(name);
    parameter.type = Name::intern(type);
    parameter.value[0] = x;
    parameter.value[1] = y;
    parameter.value[2] = z;
    parameter.exposed = true;
    return asset.declare_parameter(parameter);
}

/// 2r - 1 for each component, three independent streams.
[[nodiscard]] NodeKey signed_random3(StageBuilder& stage) noexcept {
    const NodeKey one = stage.constant(1.0F);
    const NodeKey two = stage.constant(2.0F);
    const auto component = [&]() noexcept {
        return stage.binary("vfx.sub", stage.binary("vfx.mul", stage.random(), two), one);
    };
    const NodeKey x = component();
    const NodeKey y = component();
    const NodeKey z = component();
    return stage.make3(x, y, z);
}

/// `low + r * span`.
[[nodiscard]] NodeKey ranged(StageBuilder& stage, f32 low, f32 span) noexcept {
    return stage.binary("vfx.add", stage.constant(low),
                        stage.binary("vfx.mul", stage.random(), stage.constant(span)));
}

[[nodiscard]] NodeKey thrown_along(StageBuilder& stage, Throw thrown) noexcept {
    switch (thrown) {
        case Throw::Along:
            return stage.parameter("direction");
        case Throw::Normal:
            return stage.parameter("normal");
        case Throw::Radial:
            return signed_random3(stage);
        case Throw::Up:
            break;
    }
    return stage.constant(0.0F, 1.0F, 0.0F, 0.0F, "float3");
}

[[nodiscard]] Status build_spawn(cy::Allocator& allocator, cy::vfx::Emitter& emitter,
                                 const Look& look) noexcept {
    StageBuilder stage(allocator, "spawn");
    const NodeKey first =
        stage.binary("vfx.less", stage.input(cy::vfx::input::kEmitterAge), stage.constant(0.03F));
    stage.spawn_count(
        stage.ternary("vfx.select", first, stage.constant(look.burst), stage.constant(0.0F)));
    if (!stage.ok()) {
        return cy::fail(cy::ErrorCode::Internal, "battle effects: a burst did not author");
    }
    return emitter.set_stage(cy::vfx::Stage::Spawn, stage.take());
}

[[nodiscard]] Status build_initialise(cy::Allocator& allocator, cy::vfx::Emitter& emitter,
                                      const Look& look) noexcept {
    StageBuilder stage(allocator, "initialise");
    stage.write("position",
                stage.binary("vfx.mul", signed_random3(stage), stage.constant(look.jitter)));
    const NodeKey along = stage.binary("vfx.mul", thrown_along(stage, look.thrown),
                                       ranged(stage, look.speed, look.speed_span));
    const NodeKey spread =
        stage.binary("vfx.mul", signed_random3(stage), stage.constant(look.scatter));
    stage.write("velocity", stage.binary("vfx.add", along, spread));
    stage.write("age", stage.constant(0.0F));
    stage.write("lifetime", ranged(stage, look.life, look.life_span));
    stage.write("size", stage.constant(look.size_from));
    stage.write("color",
                stage.make4(stage.constant(look.colour[0]), stage.constant(look.colour[1]),
                            stage.constant(look.colour[2]), stage.constant(look.colour[3])));
    stage.write("emission", stage.constant(look.glow_from));
    if (!stage.ok()) {
        return cy::fail(cy::ErrorCode::Internal, "battle effects: an initialise did not author");
    }
    return emitter.set_stage(cy::vfx::Stage::Initialise, stage.take());
}

/// Put a particle that went through the floor back on it, its fall reversed and mostly lost.
void bounce(StageBuilder& stage, NodeKey moved, NodeKey velocity) noexcept {
    const NodeKey up = stage.constant(0.0F, 1.0F, 0.0F, 0.0F, "float3");
    const NodeKey floor = stage.parameter("floor");
    const NodeKey height = stage.binary("vfx.dot", moved, up);
    const NodeKey below = stage.binary("vfx.less", height, floor);
    const NodeKey zero = stage.constant(0.0F);
    const NodeKey lift = stage.make3(zero, stage.binary("vfx.sub", floor, height), zero);
    stage.write("position",
                stage.ternary("vfx.select", below, stage.binary("vfx.add", moved, lift), moved));
    const NodeKey damped = stage.constant(0.55F, -0.35F, 0.55F, 0.0F, "float3");
    stage.write("velocity", stage.ternary("vfx.select", below,
                                          stage.binary("vfx.mul", velocity, damped), velocity));
}

[[nodiscard]] Status build_update(cy::Allocator& allocator, cy::vfx::Emitter& emitter,
                                  const Look& look) noexcept {
    StageBuilder stage(allocator, "update");
    const NodeKey dt = stage.input(cy::vfx::input::kDeltaTime);
    const NodeKey slowed =
        stage.binary("vfx.mul", stage.attribute("velocity"),
                     stage.binary("vfx.sub", stage.constant(1.0F),
                                  stage.binary("vfx.mul", dt, stage.constant(look.drag))));
    const NodeKey gravity = stage.constant(0.0F, -look.gravity, 0.0F, 0.0F, "float3");
    const NodeKey velocity = stage.binary("vfx.add", slowed, stage.binary("vfx.mul", gravity, dt));
    const NodeKey moved =
        stage.binary("vfx.add", stage.attribute("position"), stage.binary("vfx.mul", velocity, dt));
    if (look.bounce) {
        bounce(stage, moved, velocity);
    } else {
        stage.write("velocity", velocity);
        stage.write("position", moved);
    }
    const NodeKey age = stage.binary("vfx.add", stage.attribute("age"), dt);
    stage.write("age", age);
    const NodeKey fraction =
        stage.unary("vfx.saturate", stage.binary("vfx.div", age, stage.attribute("lifetime")));
    stage.write("size", stage.ternary("vfx.lerp", stage.constant(look.size_from),
                                      stage.constant(look.size_to), fraction));
    stage.write("emission", stage.ternary("vfx.lerp", stage.constant(look.glow_from),
                                          stage.constant(look.glow_to), fraction));
    stage.kill_if(stage.binary("vfx.greater", age, stage.attribute("lifetime")));
    if (!stage.ok()) {
        return cy::fail(cy::ErrorCode::Internal, "battle effects: an update did not author");
    }
    return emitter.set_stage(cy::vfx::Stage::Update, stage.take());
}

[[nodiscard]] Status add_emitter(cy::Allocator& allocator, cy::vfx::VfxSystemAsset& asset,
                                 const Look& look) noexcept {
    cy::vfx::Emitter emitter(allocator, Name::intern(look.emitter));
    emitter.set_capacity(look.capacity);
    using Step = Status (*)(cy::Allocator&, cy::vfx::Emitter&, const Look&) noexcept;
    if (Status declared = declare_attributes(emitter); !declared) {
        return declared;
    }
    for (const Step step : {Step{&build_spawn}, Step{&build_initialise}, Step{&build_update}}) {
        if (Status built = step(allocator, emitter, look); !built) {
            return built;
        }
    }
    return asset.add_emitter(std::move(emitter));
}

/// One effect: its asset name, its looks, and the parameters it exposes.
struct Recipe {
    const char* asset;
    const Look* looks[2];
    u32 look_count;
    bool direction;
    bool normal;
    bool floor;
    f32 spawn_seconds;
};

constexpr Recipe kRecipes[kBattleEffects] = {
    {"battle_launch", {&kLaunch, nullptr}, 1, true, false, false, 0.05F},
    {"battle_trail", {&kTrailHead, &kTrailWisp}, 2, false, false, false, 0.04F},
    {"battle_sparks", {&kSparks, nullptr}, 1, false, true, true, 0.05F},
    {"battle_explosion", {&kFireball, &kEmbers}, 2, false, false, true, 0.05F},
    {"battle_smoke", {&kSmoke, nullptr}, 1, false, false, false, 0.05F},
};

[[nodiscard]] Expected<cy::vfx::CompiledSystem, Error> cook(cy::Allocator& allocator,
                                                            cy::graph::DiagnosticSink& sink,
                                                            const Recipe& recipe) noexcept {
    cy::vfx::VfxSystemAsset asset(allocator, Name::intern(recipe.asset));
    asset.set_importance(cy::vfx::ImportanceClass::Important);
    if (recipe.direction) {
        if (Status declared = declare(asset, "direction", "float3", 0.0F, 0.0F, -1.0F); !declared) {
            return cy::make_unexpected(declared.error());
        }
    }
    if (recipe.normal) {
        if (Status declared = declare(asset, "normal", "float3", 0.0F, 1.0F, 0.0F); !declared) {
            return cy::make_unexpected(declared.error());
        }
    }
    if (recipe.floor) {
        // Far below anything: a particle falls freely unless the game says where the ground is.
        if (Status declared = declare(asset, "floor", "float", -100.0F, 0.0F, 0.0F); !declared) {
            return cy::make_unexpected(declared.error());
        }
    }
    for (u32 index = 0; index < recipe.look_count; ++index) {
        if (Status added = add_emitter(allocator, asset, *recipe.looks[index]); !added) {
            return cy::make_unexpected(added.error());
        }
    }
    cy::graph::NodeRegistry registry(allocator);
    cy::vfx::DataInterfaceRegistry interfaces(allocator);
    if (Status registered = cy::vfx::register_vfx_nodes(registry); !registered) {
        return cy::make_unexpected(registered.error());
    }
    if (Status registered = cy::vfx::register_builtin_interfaces(interfaces); !registered) {
        return cy::make_unexpected(registered.error());
    }
    asset.resolve(registry);
    cy::vfx::CompileReport report(allocator);
    return cy::vfx::compile_system(asset, registry, interfaces, cy::vfx::CompileOptions{}, sink,
                                   report);
}

constexpr const char* kNames[kBattleEffects] = {kLaunchEffect, kTrailEffect, kSparksEffect,
                                                kExplosionEffect, kSmokeEffect};

}  // namespace

BattleEffects::BattleEffects(cy::Allocator& allocator) noexcept
    : allocator_(&allocator), sink_(allocator), world_(allocator) {}

BattleEffects::~BattleEffects() {
    world_.shutdown();
}

const char* BattleEffects::name(u32 index) noexcept {
    return index < kBattleEffects ? kNames[index] : "";
}

f32 BattleEffects::spawn_seconds(u32 index) noexcept {
    return index < kBattleEffects ? kRecipes[index].spawn_seconds : 0.0F;
}

Status BattleEffects::build() noexcept {
    for (u32 index = 0; index < kBattleEffects; ++index) {
        auto cooked = cook(*allocator_, sink_, kRecipes[index]);
        if (!cooked) {
            return cy::make_unexpected(cooked.error());
        }
        systems_[index] = std::make_unique<cy::vfx::CompiledSystem>(std::move(*cooked));
    }
    cy::vfx::WorldDescription description;
    description.pool_bytes = 24ULL * 1024ULL * 1024ULL;
    description.max_instances = 2048;
    return world_.initialize(description);
}

Status BattleEffects::step(f32 dt) noexcept {
    if (Status stepped = world_.step(dt, stepped_); !stepped) {
        return stepped;
    }
    particle_frames_ += stepped_.live_particles;
    peak_particles_ = cy::math::max(peak_particles_, stepped_.live_particles);
    return cy::ok();
}

Status BattleEffects::publish(
    cy::Vec3 eye, cy::Array<cy::rendering::particles::ParticleInstance>& out) const noexcept {
    cy::vfx::PublishReport report;
    return cy::vfx::publish_sprites(world_, eye, kBattleParticleRing, out, report);
}

}  // namespace sample::rts

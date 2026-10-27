// SPDX-License-Identifier: MIT
// shot_effects.cpp — the muzzle burst, the sparks and the scorch. See shot_effects.h.

#include "shot_effects.h"

#include <cy/vfx/asset.h>
#include <cy/vfx/interfaces.h>
#include <cy/vfx/ir.h>

#include <cy/core/math/scalar.h>

#include <utility>

// src/vfx/tests/effects.h: `StageBuilder`, the node-by-node graph builder every VFX suite and the
// vertical slice author with. Compiled into this sample rather than copied.
#include "effects.h"

namespace sample::fps {
namespace {

using cy::Error;
using cy::Expected;
using cy::f32;
using cy::Name;
using cy::Status;
using cy::u32;
using cy::Vec3;
using cy::graph::NodeKey;
using cy::vfx_test::StageBuilder;

/// The attributes both effects carry. The tolerances are the precision claim: the colour accepts
/// one 8-bit step, so the compiler stores it in eight bits; the magnitude is `emission`'s, a float.
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
        {"size", "float", 0.0F, 1.0F, 0.0F},
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

/// 2r − 1 for each component, three independent streams: `vfx.random` nodes never merge.
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

/// `count` the first sub-step, none after: a burst.
[[nodiscard]] Status build_burst(cy::Allocator& allocator, cy::vfx::Emitter& emitter,
                                 f32 count) noexcept {
    StageBuilder stage(allocator, "spawn");
    const NodeKey first =
        stage.binary("vfx.less", stage.input(cy::vfx::input::kEmitterAge), stage.constant(0.03F));
    stage.spawn_count(
        stage.ternary("vfx.select", first, stage.constant(count), stage.constant(0.0F)));
    if (!stage.ok()) {
        return cy::fail(cy::ErrorCode::Internal, "fps effects: a burst did not author");
    }
    return emitter.set_stage(cy::vfx::Stage::Spawn, stage.take());
}

/// Age, fade and kill: the size and the radiance fall from their start to their end over the
/// particle's life.
void age_and_fade(StageBuilder& stage, f32 size_from, f32 size_to, f32 glow_from,
                  f32 glow_to) noexcept {
    const NodeKey dt = stage.input(cy::vfx::input::kDeltaTime);
    const NodeKey age = stage.binary("vfx.add", stage.attribute("age"), dt);
    stage.write("age", age);
    const NodeKey lifetime = stage.attribute("lifetime");
    const NodeKey fraction = stage.unary("vfx.saturate", stage.binary("vfx.div", age, lifetime));
    stage.write("size", stage.ternary("vfx.lerp", stage.constant(size_from),
                                      stage.constant(size_to), fraction));
    stage.write("emission", stage.ternary("vfx.lerp", stage.constant(glow_from),
                                          stage.constant(glow_to), fraction));
    stage.kill_if(stage.binary("vfx.greater", age, lifetime));
}

// --- weapon.muzzle --------------------------------------------------------------------------

[[nodiscard]] Status build_muzzle_stages(cy::Allocator& allocator,
                                         cy::vfx::Emitter& emitter) noexcept {
    if (Status built = build_burst(allocator, emitter, 14.0F); !built) {
        return built;
    }
    {
        StageBuilder stage(allocator, "initialise");
        stage.write("position",
                    stage.binary("vfx.mul", signed_random3(stage), stage.constant(0.008F)));
        // Thrown along the barrel, three to eight metres a second, and a little to each side.
        const NodeKey along =
            stage.binary("vfx.mul", stage.parameter("direction"), ranged(stage, 3.0F, 5.0F));
        const NodeKey spread = stage.binary("vfx.mul", signed_random3(stage), stage.constant(1.1F));
        stage.write("velocity", stage.binary("vfx.add", along, spread));
        stage.write("age", stage.constant(0.0F));
        stage.write("lifetime", ranged(stage, 0.04F, 0.07F));
        stage.write("size", stage.constant(0.035F));
        stage.write("color", stage.make4(stage.constant(1.0F), stage.constant(0.62F),
                                         stage.constant(0.24F), stage.constant(0.8F)));
        stage.write("emission", stage.constant(30000.0F));
        if (!stage.ok()) {
            return cy::fail(cy::ErrorCode::Internal, "fps effects: the muzzle did not author");
        }
        if (Status set = emitter.set_stage(cy::vfx::Stage::Initialise, stage.take()); !set) {
            return set;
        }
    }
    StageBuilder stage(allocator, "update");
    const NodeKey dt = stage.input(cy::vfx::input::kDeltaTime);
    const NodeKey velocity = stage.attribute("velocity");
    // Air drag: a hot gas slows fast.
    const NodeKey slowed =
        stage.binary("vfx.mul", velocity,
                     stage.binary("vfx.sub", stage.constant(1.0F),
                                  stage.binary("vfx.mul", dt, stage.constant(6.0F))));
    stage.write("velocity", slowed);
    stage.write("position", stage.binary("vfx.add", stage.attribute("position"),
                                         stage.binary("vfx.mul", slowed, dt)));
    age_and_fade(stage, 0.035F, 0.01F, 30000.0F, 2000.0F);
    if (!stage.ok()) {
        return cy::fail(cy::ErrorCode::Internal, "fps effects: the muzzle update did not author");
    }
    return emitter.set_stage(cy::vfx::Stage::Update, stage.take());
}

// --- impact.sparks --------------------------------------------------------------------------

[[nodiscard]] Status build_spark_stages(cy::Allocator& allocator,
                                        cy::vfx::Emitter& emitter) noexcept {
    if (Status built = build_burst(allocator, emitter, 28.0F); !built) {
        return built;
    }
    {
        StageBuilder stage(allocator, "initialise");
        const NodeKey normal = stage.parameter("normal");
        stage.write("position", stage.binary("vfx.mul", normal, stage.constant(0.02F)));
        // Off the surface along its normal, and scattered: a cone, not a jet.
        const NodeKey off = stage.binary("vfx.mul", normal, ranged(stage, 1.5F, 3.5F));
        const NodeKey scatter =
            stage.binary("vfx.mul", signed_random3(stage), stage.constant(2.2F));
        stage.write("velocity", stage.binary("vfx.add", off, scatter));
        stage.write("age", stage.constant(0.0F));
        stage.write("lifetime", ranged(stage, 0.35F, 0.5F));
        stage.write("size", stage.constant(0.02F));
        stage.write("color", stage.make4(stage.constant(1.0F), stage.constant(0.56F),
                                         stage.constant(0.16F), stage.constant(1.0F)));
        stage.write("emission", stage.constant(60000.0F));
        if (!stage.ok()) {
            return cy::fail(cy::ErrorCode::Internal, "fps effects: the sparks did not author");
        }
        if (Status set = emitter.set_stage(cy::vfx::Stage::Initialise, stage.take()); !set) {
            return set;
        }
    }
    StageBuilder stage(allocator, "update");
    const NodeKey dt = stage.input(cy::vfx::input::kDeltaTime);
    const NodeKey gravity = stage.constant(0.0F, -9.81F, 0.0F, 0.0F, "float3");
    const NodeKey velocity =
        stage.binary("vfx.add", stage.attribute("velocity"), stage.binary("vfx.mul", gravity, dt));
    const NodeKey moved =
        stage.binary("vfx.add", stage.attribute("position"), stage.binary("vfx.mul", velocity, dt));
    // THE FLOOR: a spark that went through it this step is put back on it and bounced — its
    // vertical speed reversed and most of it lost, its run along the floor damped.
    const NodeKey up = stage.constant(0.0F, 1.0F, 0.0F, 0.0F, "float3");
    const NodeKey floor = stage.parameter("floor");
    const NodeKey height = stage.binary("vfx.dot", moved, up);
    const NodeKey below = stage.binary("vfx.less", height, floor);
    const NodeKey zero = stage.constant(0.0F);
    const NodeKey lift = stage.make3(zero, stage.binary("vfx.sub", floor, height), zero);
    stage.write("position",
                stage.ternary("vfx.select", below, stage.binary("vfx.add", moved, lift), moved));
    const NodeKey bounce = stage.constant(0.55F, -0.35F, 0.55F, 0.0F, "float3");
    stage.write("velocity", stage.ternary("vfx.select", below,
                                          stage.binary("vfx.mul", velocity, bounce), velocity));
    age_and_fade(stage, 0.02F, 0.008F, 60000.0F, 1500.0F);
    if (!stage.ok()) {
        return cy::fail(cy::ErrorCode::Internal, "fps effects: the sparks update did not author");
    }
    return emitter.set_stage(cy::vfx::Stage::Update, stage.take());
}

using StageBuilderFn = Status (*)(cy::Allocator&, cy::vfx::Emitter&) noexcept;

[[nodiscard]] Expected<cy::vfx::CompiledSystem, Error> cook(cy::Allocator& allocator,
                                                            cy::graph::DiagnosticSink& sink,
                                                            cy::vfx::VfxSystemAsset& asset,
                                                            const char* emitter_name,
                                                            StageBuilderFn stages) noexcept {
    cy::vfx::Emitter emitter(allocator, Name::intern(emitter_name));
    emitter.set_capacity(64);
    if (Status declared = declare_attributes(emitter); !declared) {
        return cy::make_unexpected(declared.error());
    }
    if (Status built = stages(allocator, emitter); !built) {
        return cy::make_unexpected(built.error());
    }
    if (Status added = asset.add_emitter(std::move(emitter)); !added) {
        return cy::make_unexpected(added.error());
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

}  // namespace

ShotEffects::ShotEffects(cy::Allocator& allocator) noexcept
    : allocator_(&allocator), sink_(allocator), world_(allocator), decals_(allocator) {}

ShotEffects::~ShotEffects() {
    world_.shutdown();
}

cy::Status ShotEffects::build() noexcept {
    {
        cy::vfx::VfxSystemAsset asset(*allocator_, Name::intern("weapon_muzzle"));
        asset.set_importance(cy::vfx::ImportanceClass::Important);
        if (Status declared = declare(asset, "direction", "float3", 0.0F, 0.0F, -1.0F); !declared) {
            return declared;
        }
        auto cooked = cook(*allocator_, sink_, asset, "flash", &build_muzzle_stages);
        if (!cooked) {
            return cy::make_unexpected(cooked.error());
        }
        muzzle_ = std::make_unique<cy::vfx::CompiledSystem>(std::move(*cooked));
    }
    {
        cy::vfx::VfxSystemAsset asset(*allocator_, Name::intern("impact_sparks"));
        asset.set_importance(cy::vfx::ImportanceClass::Important);
        if (Status declared = declare(asset, "normal", "float3", 0.0F, 1.0F, 0.0F); !declared) {
            return declared;
        }
        // Far below anything: a spark falls freely unless the game says where the floor is.
        if (Status declared = declare(asset, "floor", "float", -100.0F, 0.0F, 0.0F); !declared) {
            return declared;
        }
        auto cooked = cook(*allocator_, sink_, asset, "sparks", &build_spark_stages);
        if (!cooked) {
            return cy::make_unexpected(cooked.error());
        }
        sparks_ = std::make_unique<cy::vfx::CompiledSystem>(std::move(*cooked));
    }
    cy::vfx::WorldDescription description;
    description.pool_bytes = 2ULL * 1024ULL * 1024ULL;
    description.max_instances = 96;
    if (Status made = world_.initialize(description); !made) {
        return made;
    }

    // THE SCORCH: soot and char reflect two to four per cent; rough, a couple of millimetres
    // proud, its rim broken up.
    cy::rendering::decals::DecalMaterial scorch;
    scorch.albedo = Vec3{0.028F, 0.026F, 0.024F};
    scorch.roughness = 0.95F;
    scorch.shape = cy::rendering::decals::DecalShape::Splat;
    scorch.shape_a = 7.0F;
    scorch.shape_b = 0.35F;
    scorch.relief_metres = 0.002F;
    scorch.edge_softness = 0.3F;
    scorch.seed = 0x5C0CU;
    decal_materials_.assign(1, scorch);
    decals_.set_capacity(kDecalCapacity);
    decals_.set_age_half_life(60.0F);
    return cy::ok();
}

cy::Status ShotEffects::step(f32 dt) noexcept {
    if (Status stepped = world_.step(dt, stepped_); !stepped) {
        return stepped;
    }
    particle_frames_ += stepped_.live_particles;
    peak_particles_ = cy::math::max(peak_particles_, stepped_.live_particles);
    return cy::ok();
}

cy::Status ShotEffects::publish(
    Vec3 eye, cy::Array<cy::rendering::particles::ParticleInstance>& out) const noexcept {
    cy::vfx::PublishReport report;
    return cy::vfx::publish_sprites(world_, eye, kParticleRing, out, report);
}

}  // namespace sample::fps

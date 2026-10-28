// SPDX-License-Identifier: MIT
// integration.game_backend_effects — ABI 1.15's effects over a real `cy::vfx::SimulationWorld`.
//
// The effect is the spark plume every VFX suite cooks (src/vfx/tests/effects.cpp), compiled by the
// shipping compiler. What is held here: a name resolves to the handle the adapter registered it
// under; a play through the ABI table lands in the world where and as large as the game said, with
// the game's parameter overriding the declared one and the rest authored; it is fire-and-forget; a
// full world refuses with OUT_OF_RANGE and counts it; and a fixed step can resolve but not play.

#include <cy/abi/cy_abi.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/effects_backend.h>
#include <cy/test/test.h>

#include "effects.h"

namespace {

using cy::f32;
using cy::u32;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

struct Rig {
    cy::graph::DiagnosticSink sink{allocator()};
    cy::vfx::CompileReport report{allocator()};
    cy::vfx::CompileOptions options;
    cy::Expected<cy::vfx::CompiledSystem, cy::Error> plume =
        cy::vfx_test::cook_plume(allocator(), sink, report, options);
    cy::vfx::SimulationWorld world{allocator()};
    cy::game_backend::EffectsAdapter adapter{allocator(), world};
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};

    explicit Rig(u32 max_instances) {
        CY_REQUIRE(plume.has_value());
        cy::vfx::WorldDescription description;
        description.max_instances = max_instances;
        CY_REQUIRE(world.initialize(description).has_value());
        // A burst: it spawns for a twentieth of a second, then its sparks live out their lives.
        CY_REQUIRE(adapter.add_effect("impact.sparks", plume.value(), 0.05F).has_value());
        cy::game_backend::bind_effects(host, &adapter);
    }
    ~Rig() {
        cy::game_backend::bind_effects(host, nullptr);
        world.shutdown();
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
};

[[nodiscard]] CyEffectPlay at(f32 x, f32 y, f32 z) noexcept {
    CyEffectPlay play{};
    play.struct_size = sizeof(play);
    play.position[0] = x;
    play.position[1] = y;
    play.position[2] = z;
    play.scale = 1.0F;
    return play;
}

}  // namespace

CY_TEST_CASE("effects: a name resolves to its registration, and a second registration is refused") {
    Rig rig(4);
    CyEffect effect = CY_EFFECT_NULL;
    CY_REQUIRE_EQ(table().effect_find(&rig.host, "impact.sparks", &effect), CY_RESULT_OK);
    CY_CHECK_EQ(effect, 1U);
    CY_CHECK_EQ(table().effect_find(&rig.host, "muzzle.smoke", &effect), CY_RESULT_NOT_FOUND);
    CY_CHECK_FALSE(rig.adapter.add_effect("impact.sparks", rig.plume.value()).has_value());
    CY_CHECK_FALSE(rig.adapter.add_effect("", rig.plume.value()).has_value());
}

CY_TEST_CASE("effects: a play lands in the world where the game said, its parameter overridden") {
    Rig rig(4);
    CyEffect effect = CY_EFFECT_NULL;
    CY_REQUIRE_EQ(table().effect_find(&rig.host, "impact.sparks", &effect), CY_RESULT_OK);
    CyEffectPlay play = at(3.0F, 1.5F, -2.0F);
    play.scale = 0.5F;
    play.parameter_count = 1;  // `intensity`, the plume's first declared parameter
    play.parameters[0] = 2.5F;
    {
        cy::abi::game::PhaseScope frame(rig.host.game.clock, CY_PHASE_FRAME_UPDATE);
        CY_REQUIRE_EQ(table().effect_play(&rig.host, effect, &play), CY_RESULT_OK);
    }
    CY_REQUIRE_EQ(rig.world.instances().size(), 1U);
    const cy::vfx::EffectInstance& instance = rig.world.instances()[0];
    CY_CHECK_EQ(instance.position.x, 3.0F);
    CY_CHECK_EQ(instance.position.y, 1.5F);
    CY_CHECK_EQ(instance.position.z, -2.0F);
    CY_CHECK_EQ(instance.scale, 0.5F);
    // FIRE AND FORGET: the world releases it when its last particle dies.
    CY_CHECK(instance.release_on_completion);
    const auto intensity = rig.world.get_parameter(instance.handle, cy::Name::intern("intensity"));
    CY_REQUIRE(intensity.has_value());
    CY_CHECK_EQ(intensity->lanes[0], 2.5F);
    CY_CHECK_EQ(rig.adapter.played(), 1U);
    CY_CHECK_EQ(rig.adapter.played(effect), 1U);

    // A play with no parameters keeps the authored value.
    const CyEffectPlay plain = at(0.0F, 0.0F, 0.0F);
    CY_REQUIRE_EQ(table().effect_play(&rig.host, effect, &plain), CY_RESULT_OK);
    const auto authored =
        rig.world.get_parameter(rig.world.instances()[1].handle, cy::Name::intern("intensity"));
    CY_REQUIRE(authored.has_value());
    CY_CHECK_EQ(authored->lanes[0], 1.0F);

    // And it simulates: a step later the instance has particles.
    cy::vfx::StepReport stepped;
    CY_REQUIRE(rig.world.step(1.0F / 60.0F, stepped).has_value());
    CY_CHECK_GT(stepped.live_particles, 0U);
    CY_CHECK_EQ(rig.world.instances()[0].spawn_seconds, 0.05F);
    // FIRE AND FORGET, ENDED: a burst nobody holds stops spawning and releases itself.
    for (u32 frame = 0; frame < 240U; ++frame) {
        CY_REQUIRE(rig.world.step(1.0F / 60.0F, stepped).has_value());
    }
    CY_CHECK_EQ(stepped.instances, 0U);
    CY_CHECK_EQ(rig.world.pool().report().used_bytes, 0U);
}

CY_TEST_CASE("effects: a full world refuses with OUT_OF_RANGE, and a fixed step may not play") {
    Rig rig(2);
    const CyEffectPlay play = at(0.0F, 1.0F, 0.0F);
    {
        cy::abi::game::PhaseScope fixed(rig.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CyEffect effect = CY_EFFECT_NULL;
        CY_CHECK_EQ(table().effect_find(&rig.host, "impact.sparks", &effect), CY_RESULT_OK);
        CY_CHECK_EQ(table().effect_play(&rig.host, effect, &play), CY_RESULT_PERMISSION_DENIED);
    }
    CY_CHECK_EQ(rig.world.instances().size(), 0U);
    CY_REQUIRE_EQ(table().effect_play(&rig.host, 1, &play), CY_RESULT_OK);
    CY_REQUIRE_EQ(table().effect_play(&rig.host, 1, &play), CY_RESULT_OK);
    CY_CHECK_EQ(table().effect_play(&rig.host, 1, &play), CY_RESULT_OUT_OF_RANGE);
    CY_CHECK_EQ(rig.adapter.played(), 2U);
    CY_CHECK_EQ(rig.adapter.refused(), 1U);
    CY_CHECK_EQ(table().effect_play(&rig.host, 2, &play), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().effect_play(&rig.host, CY_EFFECT_NULL, &play), CY_RESULT_NOT_FOUND);
}

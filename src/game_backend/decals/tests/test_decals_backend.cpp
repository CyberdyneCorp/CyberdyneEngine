// SPDX-License-Identifier: MIT
// integration.game_backend_decals — ABI 1.15's decals over a real `cy::rendering::DecalBudget`.
//
// What is held here: a material resolves to its registration; a spawn through the ABI table lands
// in the budget as the engine's decal — the box turned by the pose so its +Z is the surface's
// normal, the fades' zero defaults taken, the material's index, stamped with the frame's time —
// and a full budget evicts deterministically, the new mark included, reporting CY_DECAL_NULL and
// counting it; a fixed step can resolve but not spawn.

#include <cy/abi/cy_abi.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/decals_backend.h>
#include <cy/test/test.h>

#include <cmath>

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
    cy::rendering::DecalBudget budget{allocator()};
    cy::game_backend::DecalsAdapter adapter{allocator(), budget};
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};

    explicit Rig(u32 capacity) {
        budget.set_capacity(capacity);
        CY_REQUIRE(adapter.add_material("impact.scorch", 0).has_value());
        CY_REQUIRE(adapter.add_material("impact.glow", 2, true).has_value());
        cy::game_backend::bind_decals(host, &adapter);
    }
    ~Rig() { cy::game_backend::bind_decals(host, nullptr); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
};

/// A mark on a wall facing +X: +Z turned onto +X, a quarter turn about +Y.
[[nodiscard]] CyDecalDesc on_wall(CyDecalMaterial material, f32 importance) noexcept {
    CyDecalDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.material = material;
    desc.pose.position[0] = 4.0F;
    desc.pose.position[1] = 1.2F;
    desc.pose.position[2] = -1.0F;
    const f32 half = std::sqrt(0.5F);
    desc.pose.rotation[1] = half;
    desc.pose.rotation[3] = half;
    desc.half_extent[0] = 0.08F;
    desc.half_extent[1] = 0.08F;
    desc.half_extent[2] = 0.04F;
    desc.importance = importance;
    return desc;
}

[[nodiscard]] bool near(cy::Vec3 a, cy::Vec3 b) noexcept {
    return std::fabs(a.x - b.x) < 1e-5F && std::fabs(a.y - b.y) < 1e-5F &&
           std::fabs(a.z - b.z) < 1e-5F;
}

}  // namespace

CY_TEST_CASE("decals: a spawn lands in the budget as the engine's decal, facing the surface") {
    Rig rig(8);
    CyDecalMaterial scorch = CY_DECAL_MATERIAL_NULL;
    CyDecalMaterial glow = CY_DECAL_MATERIAL_NULL;
    CY_REQUIRE_EQ(table().decal_find_material(&rig.host, "impact.scorch", &scorch), CY_RESULT_OK);
    CY_REQUIRE_EQ(table().decal_find_material(&rig.host, "impact.glow", &glow), CY_RESULT_OK);
    CY_CHECK_EQ(scorch, 1U);
    CY_CHECK_EQ(glow, 2U);
    CY_CHECK_EQ(table().decal_find_material(&rig.host, "blood", &scorch), CY_RESULT_NOT_FOUND);

    rig.adapter.begin_frame(12.5);
    CyDecal decal = CY_DECAL_NULL;
    const CyDecalDesc desc = on_wall(glow, 0.7F);
    CY_REQUIRE_EQ(table().decal_spawn(&rig.host, &desc, &decal), CY_RESULT_OK);
    CY_CHECK_NE(decal, CY_DECAL_NULL);
    CY_REQUIRE_EQ(rig.budget.size(), 1U);
    const cy::rendering::DecalInstance& made = rig.budget.decals()[0];
    CY_CHECK_EQ(made.id, decal);
    CY_CHECK(near(made.center, cy::Vec3{4.0F, 1.2F, -1.0F}));
    CY_CHECK(near(made.axis_z, cy::Vec3{1.0F, 0.0F, 0.0F}));
    CY_CHECK(near(made.axis_x, cy::Vec3{0.0F, 0.0F, -1.0F}));
    CY_CHECK(near(made.axis_y, cy::Vec3{0.0F, 1.0F, 0.0F}));
    CY_CHECK(near(made.half_extent, cy::Vec3{0.08F, 0.08F, 0.04F}));
    CY_CHECK_EQ(made.material_index, 2U);
    CY_CHECK_EQ(made.weights.emission, 1.0F);
    CY_CHECK_EQ(made.importance, 0.7F);
    CY_CHECK_EQ(made.spawn_time_seconds, 12.5);
    // The fades' zeros are the engine's defaults.
    const cy::rendering::DecalInstance defaults;
    CY_CHECK_EQ(made.fade_angle_radians, defaults.fade_angle_radians);
    CY_CHECK_EQ(made.fade_start, defaults.fade_start);
    CY_CHECK_EQ(made.fade_end, defaults.fade_end);

    CyDecalStatus status{};
    status.struct_size = sizeof(status);
    CY_REQUIRE_EQ(table().decal_status(&rig.host, &status), CY_RESULT_OK);
    CY_CHECK_EQ(status.live, 1U);
    CY_CHECK_EQ(status.capacity, 8U);
    CY_CHECK_EQ(status.spawned, 1U);
    CY_CHECK_EQ(status.evicted, 0U);
}

CY_TEST_CASE("decals: a full budget evicts deterministically, and a fixed step may not spawn") {
    Rig rig(2);
    CyDecal decal = CY_DECAL_NULL;
    {
        cy::abi::game::PhaseScope fixed(rig.host.game.clock, CY_PHASE_FIXED_UPDATE);
        const CyDecalDesc desc = on_wall(1, 0.5F);
        CY_CHECK_EQ(table().decal_spawn(&rig.host, &desc, &decal), CY_RESULT_PERMISSION_DENIED);
    }
    CY_CHECK_EQ(rig.budget.size(), 0U);
    const CyDecalDesc kept = on_wall(1, 0.9F);
    CY_REQUIRE_EQ(table().decal_spawn(&rig.host, &kept, &decal), CY_RESULT_OK);
    CY_REQUIRE_EQ(table().decal_spawn(&rig.host, &kept, &decal), CY_RESULT_OK);
    // The budget is full of important marks; a mark of no importance is the least worth keeping
    // and is the one evicted — reported as the null decal, counted, and not an error.
    const CyDecalDesc splatter = on_wall(1, 0.0F);
    CY_REQUIRE_EQ(table().decal_spawn(&rig.host, &splatter, &decal), CY_RESULT_OK);
    CY_CHECK_EQ(decal, CY_DECAL_NULL);
    CY_CHECK_EQ(rig.budget.size(), 2U);
    CY_CHECK_EQ(rig.adapter.spawned(), 3U);
    CY_CHECK_EQ(rig.adapter.evicted(), 1U);
    // An important mark into the same budget evicts an older one instead.
    rig.adapter.begin_frame(600.0);
    CY_REQUIRE_EQ(table().decal_spawn(&rig.host, &kept, &decal), CY_RESULT_OK);
    CY_CHECK_NE(decal, CY_DECAL_NULL);
    CY_CHECK_EQ(rig.adapter.evicted(), 2U);
    CY_CHECK_EQ(table().decal_spawn(&rig.host, &kept, nullptr), CY_RESULT_INVALID_ARGUMENT);
    const CyDecalDesc foreign = on_wall(3, 0.5F);
    CY_CHECK_EQ(table().decal_spawn(&rig.host, &foreign, &decal), CY_RESULT_NOT_FOUND);
}

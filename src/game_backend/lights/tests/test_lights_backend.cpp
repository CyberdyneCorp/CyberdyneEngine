// SPDX-License-Identifier: MIT
// integration.game_backend_lights — ABI 1.11 through the real pool.
//
// A light spawned through the table becomes the engine light its description says — every kind,
// area lights included, round-tripping through `desc_of` — lives as long as its lifetime, fades,
// and is appended to the frame's lights; a full pool refuses and counts.

#include <cy/abi/cy_abi.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/math/math.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/lights_backend.h>
#include <cy/test/test.h>

namespace {

using cy::f32;
using cy::u32;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

struct Bound {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    cy::game_backend::LightsAdapter lights{cy::system_allocator(cy::MemoryDomain::Renderer)};
    explicit Bound(u32 capacity) {
        CY_REQUIRE(lights.create(capacity).has_value());
        cy::game_backend::bind_lights(host, &lights);
    }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

CyLightDesc desc_of_kind(u32 kind) noexcept {
    CyLightDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.kind = kind;
    desc.pose.position[0] = 1.0F;
    desc.pose.position[1] = 2.0F;
    desc.pose.position[2] = -3.0F;
    // A quarter turn about +Y: forward (-Z) becomes -X.
    desc.pose.rotation[1] = 0.70710678F;
    desc.pose.rotation[3] = 0.70710678F;
    desc.color[0] = 0.9F;
    desc.color[1] = 0.5F;
    desc.color[2] = 0.1F;
    desc.intensity = 1200.0F;
    desc.range = 6.0F;
    desc.inner_cone = 0.2F;
    desc.outer_cone = 0.6F;
    desc.size[0] = 0.8F;
    desc.size[1] = 0.3F;
    desc.flags = CY_LIGHT_TWO_SIDED;
    desc.layer_mask = 0x3U;
    return desc;
}

}  // namespace

CY_TEST_CASE("game backend lights: every kind round-trips through the table to the engine's light") {
    Bound bound(16);
    const u32 kinds[6] = {CY_LIGHT_POINT, CY_LIGHT_SPOT,   CY_LIGHT_RECT,
                          CY_LIGHT_DISC,  CY_LIGHT_SPHERE, CY_LIGHT_TUBE};
    for (const u32 kind : kinds) {
        const CyLightDesc desc = desc_of_kind(kind);
        CyLight light = CY_LIGHT_NULL;
        CY_REQUIRE_EQ(table().light_spawn(bound.engine(), &desc, &light), CY_RESULT_OK);
        CY_CHECK_NE(light, CY_LIGHT_NULL);
        // What reaches the frame is the light the description says.
        const cy::render::LightDescription engine_light = cy::game_backend::light_of(desc);
        CY_CHECK_EQ(static_cast<u32>(engine_light.kind), kind);
        CY_CHECK(cy::length(engine_light.transform.forward() - cy::Vec3{-1.0F, 0.0F, 0.0F}) <
                 1e-5F);
        CY_CHECK_EQ(engine_light.source_width, 0.8F);
        CY_CHECK_EQ(engine_light.source_height, 0.3F);
        CY_CHECK(engine_light.two_sided);
        CY_CHECK_EQ(engine_light.layer_mask, 0x3U);
        CY_CHECK_EQ(engine_light.mobility, cy::render::LightMobility::Movable);
        // And back again, field for field.
        const CyLightDesc again =
            cy::game_backend::desc_of(engine_light, 0.0F, cy::rendering::TransientFade::None);
        CY_CHECK_EQ(again.kind, desc.kind);
        CY_CHECK_EQ(again.size[0], desc.size[0]);
        CY_CHECK_EQ(again.size[1], desc.size[1]);
        CY_CHECK_EQ(again.flags, desc.flags);
        CY_CHECK_EQ(again.intensity, desc.intensity);
        CY_CHECK_EQ(again.outer_cone, desc.outer_cone);
        CY_CHECK_EQ(again.layer_mask, desc.layer_mask);
        for (u32 index = 0; index < 4; ++index) {
            CY_CHECK_NEAR(again.pose.rotation[index], desc.pose.rotation[index], 1e-6F);
        }
    }
    cy::Array<cy::render::LightDescription> frame(cy::system_allocator(cy::MemoryDomain::Renderer));
    CY_REQUIRE(bound.lights.append_to(frame).has_value());
    CY_CHECK_EQ(frame.size(), 6U);
    CY_CHECK_EQ(frame[2].kind, cy::render::LightKind::Rect);
}

CY_TEST_CASE("game backend lights: a flash fades, ends, and its handle stops answering") {
    Bound bound(4);
    CyLightDesc desc = desc_of_kind(CY_LIGHT_POINT);
    desc.lifetime = 0.1F;
    desc.fade = CY_LIGHT_FADE_LINEAR;
    CyLight flash = CY_LIGHT_NULL;
    CY_REQUIRE_EQ(table().light_spawn(bound.engine(), &desc, &flash), CY_RESULT_OK);
    bound.lights.advance(0.05F);
    cy::Array<cy::render::LightDescription> frame(cy::system_allocator(cy::MemoryDomain::Renderer));
    CY_REQUIRE(bound.lights.append_to(frame).has_value());
    CY_REQUIRE_EQ(frame.size(), 1U);
    CY_CHECK_NEAR(frame[0].intensity, 600.0F, 1e-2F);
    bound.lights.advance(0.06F);
    u32 alive = 7;
    CY_REQUIRE_EQ(table().light_alive(bound.engine(), flash, &alive), CY_RESULT_OK);
    CY_CHECK_EQ(alive, 0U);
    CY_CHECK_EQ(table().light_destroy(bound.engine(), flash), CY_RESULT_NOT_FOUND);
    CyLightStatus status{};
    status.struct_size = sizeof(status);
    CY_REQUIRE_EQ(table().light_status(bound.engine(), &status), CY_RESULT_OK);
    CY_CHECK_EQ(status.expired, 1U);
    CY_CHECK_EQ(status.live, 0U);
    CY_CHECK_EQ(status.capacity, 4U);
}

CY_TEST_CASE("game backend lights: a full pool refuses, counts, and grows nothing") {
    Bound bound(2);
    const CyLightDesc desc = desc_of_kind(CY_LIGHT_SPHERE);
    CyLight light = CY_LIGHT_NULL;
    CY_REQUIRE_EQ(table().light_spawn(bound.engine(), &desc, &light), CY_RESULT_OK);
    CY_REQUIRE_EQ(table().light_spawn(bound.engine(), &desc, &light), CY_RESULT_OK);
    CyLight refused = CY_LIGHT_NULL;
    CY_CHECK_EQ(table().light_spawn(bound.engine(), &desc, &refused), CY_RESULT_OUT_OF_RANGE);
    CY_CHECK_EQ(refused, CY_LIGHT_NULL);
    CY_CHECK_EQ(bound.lights.pool().status().refused, 1U);
    // A torch moves; destroyed, its slot is free again.
    CyLightDesc moved = desc;
    moved.pose.position[0] = 9.0F;
    CY_REQUIRE_EQ(table().light_update(bound.engine(), light, &moved), CY_RESULT_OK);
    CY_REQUIRE_EQ(table().light_destroy(bound.engine(), light), CY_RESULT_OK);
    CY_CHECK_EQ(table().light_spawn(bound.engine(), &desc, &refused), CY_RESULT_OK);
}

// SPDX-License-Identifier: MIT
// ABI 1.11's light thunks against a fake backend.
//
// What the thunks own: the phase rule — every entry `[N U]` — the argument and range checks,
// `struct_size` both ways, and UNAVAILABLE with no pool bound. The pool itself is
// integration.game_backend_lights's.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/lights.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstddef>
#include <limits>

namespace {

using cy::u32;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

class FakeLights final : public cy::abi::game::LightsBackend {
public:
    CyLightDesc spawned{};
    CyLightDesc updated{};
    CyLight destroyed = CY_LIGHT_NULL;
    u32 calls = 0;

    CyResult spawn(const CyLightDesc& desc, CyLight& out) noexcept override {
        spawned = desc;
        ++calls;
        out = 0x1'0000'0003ULL;
        return CY_RESULT_OK;
    }
    CyResult update(CyLight light, const CyLightDesc& desc) noexcept override {
        if (light != 0x1'0000'0003ULL) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "the light ended");
        }
        updated = desc;
        ++calls;
        return CY_RESULT_OK;
    }
    CyResult destroy(CyLight light) noexcept override {
        destroyed = light;
        ++calls;
        return CY_RESULT_OK;
    }
    CyResult alive(CyLight light, u32& out) const noexcept override {
        out = light == 0x1'0000'0003ULL ? 1U : 0U;
        return CY_RESULT_OK;
    }
    CyResult status(CyLightStatus& out) const noexcept override {
        out.live = 3;
        out.capacity = 256;
        out.spawned = 40;
        out.expired = 37;
        out.refused = 2;
        return CY_RESULT_OK;
    }
};

struct Bound {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakeLights lights;
    Bound() { host.game.lights = &lights; }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

[[nodiscard]] CyLightDesc flash() noexcept {
    CyLightDesc made{};
    made.struct_size = sizeof(made);
    made.kind = CY_LIGHT_POINT;
    made.pose.position[1] = 1.5F;
    made.color[0] = 1.0F;
    made.color[1] = 0.6F;
    made.color[2] = 0.2F;
    made.intensity = 8000.0F;
    made.range = 4.0F;
    made.lifetime = 0.05F;
    made.fade = CY_LIGHT_FADE_QUADRATIC;
    return made;
}

}  // namespace

CY_TEST_CASE("lights: without a bound pool every entry is UNAVAILABLE") {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    const CyLightDesc desc = flash();
    CyLight light = CY_LIGHT_NULL;
    CY_CHECK_EQ(table().light_spawn(&host, &desc, &light), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().light_destroy(&host, 1), CY_RESULT_UNAVAILABLE);
    u32 alive = 0;
    CY_CHECK_EQ(table().light_alive(&host, 1, &alive), CY_RESULT_UNAVAILABLE);
    CyLightStatus status{};
    status.struct_size = sizeof(status);
    CY_CHECK_EQ(table().light_status(&host, &status), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().light_spawn(nullptr, &desc, &light), CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("lights: a description is whole and in range before the backend sees it") {
    Bound bound;
    CyLightDesc desc = flash();
    desc.kind = CY_LIGHT_RECT;
    desc.size[0] = 0.4F;
    desc.size[1] = 0.2F;
    desc.flags = CY_LIGHT_TWO_SIDED;
    CyLight light = CY_LIGHT_NULL;
    CY_REQUIRE_EQ(table().light_spawn(bound.engine(), &desc, &light), CY_RESULT_OK);
    CY_CHECK_EQ(light, 0x1'0000'0003ULL);
    CY_CHECK_EQ(bound.lights.spawned.kind, static_cast<u32>(CY_LIGHT_RECT));
    CY_CHECK_EQ(bound.lights.spawned.size[1], 0.2F);
    CY_CHECK_EQ(bound.lights.spawned.flags, CY_LIGHT_TWO_SIDED);
    CY_CHECK_EQ(bound.lights.spawned.struct_size, static_cast<u32>(sizeof(CyLightDesc)));

    // An older module's shorter struct: what it did not know reads as zero.
    CyLightDesc shorter = flash();
    shorter.struct_size = offsetof(CyLightDesc, lifetime);
    shorter.lifetime = 3.0F;
    shorter.fade = CY_LIGHT_FADE_LINEAR;
    CY_REQUIRE_EQ(table().light_spawn(bound.engine(), &shorter, &light), CY_RESULT_OK);
    CY_CHECK_EQ(bound.lights.spawned.lifetime, 0.0F);
    CY_CHECK_EQ(bound.lights.spawned.fade, 0U);
    CY_CHECK_EQ(bound.lights.spawned.intensity, 8000.0F);

    const u32 before = bound.lights.calls;
    const auto refused = [&](CyLightDesc bad) {
        CyLight none = CY_LIGHT_NULL;
        CY_CHECK_EQ(table().light_spawn(bound.engine(), &bad, &none), CY_RESULT_INVALID_ARGUMENT);
        CY_CHECK_EQ(none, CY_LIGHT_NULL);
    };
    CyLightDesc bad = flash();
    bad.kind = 0;  // a directional light is not one a game spawns
    refused(bad);
    bad = flash();
    bad.kind = 7;
    refused(bad);
    bad = flash();
    bad.fade = 3;
    refused(bad);
    bad = flash();
    bad.flags = 0x80U;
    refused(bad);
    bad = flash();
    bad.reserved[1] = 1;
    refused(bad);
    bad = flash();
    bad.intensity = -1.0F;
    refused(bad);
    bad = flash();
    bad.size[0] = std::numeric_limits<float>::quiet_NaN();
    refused(bad);
    bad = flash();
    bad.lifetime = -0.5F;
    refused(bad);
    bad = flash();
    bad.struct_size = 2;
    refused(bad);
    CY_CHECK_EQ(table().light_spawn(bound.engine(), nullptr, &light), CY_RESULT_INVALID_ARGUMENT);
    const CyLightDesc good = flash();
    CY_CHECK_EQ(table().light_spawn(bound.engine(), &good, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(bound.lights.calls, before);
}

CY_TEST_CASE("lights: a frame update may spawn, move and end a light; a fixed step may not") {
    Bound bound;
    const CyLightDesc desc = flash();
    CyLight light = CY_LIGHT_NULL;
    {
        cy::abi::game::PhaseScope fixed(bound.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_CHECK_EQ(table().light_spawn(bound.engine(), &desc, &light),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(table().light_destroy(bound.engine(), 1), CY_RESULT_PERMISSION_DENIED);
        u32 alive = 0;
        CY_CHECK_EQ(table().light_alive(bound.engine(), 1, &alive), CY_RESULT_PERMISSION_DENIED);
    }
    CY_CHECK_EQ(bound.lights.calls, 0U);
    cy::abi::game::PhaseScope frame(bound.host.game.clock, CY_PHASE_FRAME_UPDATE);
    CY_REQUIRE_EQ(table().light_spawn(bound.engine(), &desc, &light), CY_RESULT_OK);
    CyLightDesc moved = desc;
    moved.pose.position[0] = 4.0F;
    CY_REQUIRE_EQ(table().light_update(bound.engine(), light, &moved), CY_RESULT_OK);
    CY_CHECK_EQ(bound.lights.updated.pose.position[0], 4.0F);
    CY_CHECK_EQ(table().light_update(bound.engine(), 0x9U, &moved), CY_RESULT_NOT_FOUND);
    u32 alive = 7;
    CY_REQUIRE_EQ(table().light_alive(bound.engine(), light, &alive), CY_RESULT_OK);
    CY_CHECK_EQ(alive, 1U);
    CY_CHECK_EQ(table().light_alive(bound.engine(), light, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_REQUIRE_EQ(table().light_destroy(bound.engine(), light), CY_RESULT_OK);
    CY_CHECK_EQ(bound.lights.destroyed, light);
    // A status written into an older, shorter struct stops at its end.
    CyLightStatus status{};
    status.struct_size = offsetof(CyLightStatus, spawned);
    CY_REQUIRE_EQ(table().light_status(bound.engine(), &status), CY_RESULT_OK);
    CY_CHECK_EQ(status.live, 3U);
    CY_CHECK_EQ(status.capacity, 256U);
    CY_CHECK_EQ(status.spawned, 0U);
    CY_CHECK_EQ(status.struct_size, static_cast<u32>(offsetof(CyLightStatus, spawned)));
}

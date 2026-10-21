// SPDX-License-Identifier: MIT
// ABI 1.10's fog of war thunks against a fake backend.
//
// What the thunks own: the phase rule — `fog_set_vision` `[N F]`, the questions `[N F U]` — the
// argument and range checks, `struct_size` both ways, and UNAVAILABLE with no fog bound. The grid
// itself is integration.game_backend_fog's.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/fog.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstddef>

namespace {

using cy::u32;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

class FakeFog final : public cy::abi::game::FogBackend {
public:
    CyFogVision given{};
    CyEntity given_to = CY_ENTITY_NULL;
    u32 calls = 0;
    CyFixedVec2 asked{};

    CyResult set_vision(CyEntity entity, const CyFogVision& vision) noexcept override {
        given = vision;
        given_to = entity;
        ++calls;
        return CY_RESULT_OK;
    }
    CyResult vision(CyEntity entity, CyFogVision& out) const noexcept override {
        if (entity != given_to) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no vision");
        }
        out = given;
        return CY_RESULT_OK;
    }
    CyResult cell(u32 /*team*/, const CyFixedVec2& position, u32& out) const noexcept override {
        const_cast<FakeFog*>(this)->asked = position;
        out = CY_FOG_CELL_EXPLORED;
        return CY_RESULT_OK;
    }
    CyResult sight(u32 team, CyEntity /*entity*/, u32& out) const noexcept override {
        out = team == 1U ? CY_FOG_SIGHT_GHOST : CY_FOG_SIGHT_HIDDEN;
        return CY_RESULT_OK;
    }
    CyResult status(CyFogStatus& out) const noexcept override {
        out.width = 8;
        out.height = 4;
        out.tick = 12;
        out.digest = 0xABCDU;
        return CY_RESULT_OK;
    }
};

struct Bound {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakeFog fog;
    Bound() { host.game.fog = &fog; }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

constexpr CyEntity kUnit = 0x1'0000'0007ULL;

[[nodiscard]] CyFogVision vision(u32 team, CyFixed radius) noexcept {
    CyFogVision made{};
    made.struct_size = sizeof(made);
    made.team = team;
    made.reveal_radius = radius;
    return made;
}

}  // namespace

CY_TEST_CASE("fog: without a bound fog every entry is UNAVAILABLE") {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    const CyFogVision given = vision(1, CyFixed{1} << 32);
    CY_CHECK_EQ(table().fog_set_vision(&host, kUnit, &given), CY_RESULT_UNAVAILABLE);
    u32 out = 0;
    CY_CHECK_EQ(table().fog_sight(&host, 1, kUnit, &out), CY_RESULT_UNAVAILABLE);
    CyFogStatus status{};
    status.struct_size = sizeof(status);
    CY_CHECK_EQ(table().fog_status(&host, &status), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().fog_set_vision(nullptr, kUnit, &given), CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("fog: a vision is whole and in range before the backend sees it") {
    Bound bound;
    CyFogVision given = vision(32, CyFixed{5} << 32);
    given.flags = CY_FOG_LEAVES_GHOST;
    CY_REQUIRE_EQ(table().fog_set_vision(bound.engine(), kUnit, &given), CY_RESULT_OK);
    CY_CHECK_EQ(bound.fog.given.team, 32U);
    CY_CHECK_EQ(bound.fog.given.struct_size, static_cast<u32>(sizeof(CyFogVision)));
    CY_CHECK_EQ(bound.fog.given.flags, CY_FOG_LEAVES_GHOST);

    // An older module's shorter struct: what it did not know reads as zero.
    CyFogVision shorter = vision(3, CyFixed{2} << 32);
    shorter.struct_size = offsetof(CyFogVision, flags);
    shorter.flags = CY_FOG_LEAVES_GHOST;
    CY_REQUIRE_EQ(table().fog_set_vision(bound.engine(), kUnit, &shorter), CY_RESULT_OK);
    CY_CHECK_EQ(bound.fog.given.flags, 0U);
    CY_CHECK_EQ(bound.fog.given.reveal_radius, CyFixed{2} << 32);

    const u32 before = bound.fog.calls;
    CyFogVision refused = vision(33, 0);
    CY_CHECK_EQ(table().fog_set_vision(bound.engine(), kUnit, &refused),
                CY_RESULT_INVALID_ARGUMENT);
    refused = vision(1, -1);
    CY_CHECK_EQ(table().fog_set_vision(bound.engine(), kUnit, &refused),
                CY_RESULT_INVALID_ARGUMENT);
    refused = vision(1, 0);
    refused.flags = 0x80U;
    CY_CHECK_EQ(table().fog_set_vision(bound.engine(), kUnit, &refused),
                CY_RESULT_INVALID_ARGUMENT);
    refused = vision(1, 0);
    refused.reserved = 7;
    CY_CHECK_EQ(table().fog_set_vision(bound.engine(), kUnit, &refused),
                CY_RESULT_INVALID_ARGUMENT);
    refused.struct_size = 2;
    CY_CHECK_EQ(table().fog_set_vision(bound.engine(), kUnit, &refused),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().fog_set_vision(bound.engine(), kUnit, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(bound.fog.calls, before);
}

CY_TEST_CASE("fog: a frame update may ask but not give a vision") {
    Bound bound;
    const CyFogVision given = vision(1, CyFixed{1} << 32);
    {
        cy::abi::game::PhaseScope fixed(bound.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_CHECK_EQ(table().fog_set_vision(bound.engine(), kUnit, &given), CY_RESULT_OK);
    }
    cy::abi::game::PhaseScope frame(bound.host.game.clock, CY_PHASE_FRAME_UPDATE);
    CY_CHECK_EQ(table().fog_set_vision(bound.engine(), kUnit, &given), CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(bound.fog.calls, 1U);
    const CyFixedVec2 where{CyFixed{3} << 32, -(CyFixed{4} << 32)};
    u32 cell = 99;
    CY_REQUIRE_EQ(table().fog_cell(bound.engine(), 1, &where, &cell), CY_RESULT_OK);
    CY_CHECK_EQ(cell, static_cast<u32>(CY_FOG_CELL_EXPLORED));
    CY_CHECK_EQ(bound.fog.asked.x, where.x);
    CY_CHECK_EQ(bound.fog.asked.y, where.y);
    u32 sight = 99;
    CY_REQUIRE_EQ(table().fog_sight(bound.engine(), 1, kUnit, &sight), CY_RESULT_OK);
    CY_CHECK_EQ(sight, static_cast<u32>(CY_FOG_SIGHT_GHOST));
    CyFogVision read{};
    read.struct_size = sizeof(read);
    CY_REQUIRE_EQ(table().fog_get_vision(bound.engine(), kUnit, &read), CY_RESULT_OK);
    CY_CHECK_EQ(read.team, 1U);
    CY_CHECK_EQ(table().fog_get_vision(bound.engine(), 0x99U, &read), CY_RESULT_NOT_FOUND);
    // The outputs are required.
    CY_CHECK_EQ(table().fog_cell(bound.engine(), 1, &where, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().fog_cell(bound.engine(), 1, nullptr, &cell), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().fog_sight(bound.engine(), 1, kUnit, nullptr), CY_RESULT_INVALID_ARGUMENT);
    // A status written into an older, shorter struct stops at its end.
    CyFogStatus status{};
    status.struct_size = offsetof(CyFogStatus, tick);
    CY_REQUIRE_EQ(table().fog_status(bound.engine(), &status), CY_RESULT_OK);
    CY_CHECK_EQ(status.width, 8U);
    CY_CHECK_EQ(status.tick, 0U);
    CY_CHECK_EQ(status.struct_size, static_cast<u32>(offsetof(CyFogStatus, tick)));
}

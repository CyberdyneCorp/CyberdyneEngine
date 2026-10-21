// SPDX-License-Identifier: MIT
// `integration.game_backend_fog`: fog of war through ABI 1.10's table.
//
// A `FogAdapter` over a 32 x 32 grid of 1 m cells, its entities placed by a small table the test
// owns, driven only through `cy_get_interface`'s entries as a Swift game drives it: visions given
// and read back, the grid's answers for cells and entities, the phase rule, the refusals that
// change nothing, and the ordering that makes the digest a function of the state.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/fog_backend.h>
#include <cy/test/test.h>

#include <map>
#include <utility>

using namespace cy;
using cy::game_backend::FogAdapter;
namespace gfog = cy::gameplay::fog;

namespace {

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

constexpr CyFixed kMetre = CyFixed{1} << 32;
constexpr CyEntity kScout = 0x1'0000'0010ULL;
constexpr CyEntity kRaider = 0x1'0000'0020ULL;
constexpr CyEntity kBarracks = 0x1'0000'0030ULL;
constexpr CyEntity kStranger = 0x1'0000'0099ULL;

/// The host's placement table: where each entity stands, Q32.32 metres.
using Places = std::map<CyEntity, std::pair<i64, i64>>;

bool place(CyEntity entity, i64& x_raw, i64& z_raw, void* user) noexcept {
    const auto* places = static_cast<const Places*>(user);
    const auto found = places->find(entity);
    if (found == places->end()) {
        return false;
    }
    x_raw = found->second.first;
    z_raw = found->second.second;
    return true;
}

[[nodiscard]] std::pair<i64, i64> at(i64 x, i64 z) noexcept {
    return {(x * kMetre) + (kMetre / 2), (z * kMetre) + (kMetre / 2)};
}

[[nodiscard]] CyFogVision vision(u32 team, i64 radius_metres, u32 flags = 0) noexcept {
    CyFogVision made{};
    made.struct_size = sizeof(made);
    made.team = team;
    made.reveal_radius = radius_metres * kMetre;
    made.flags = flags;
    return made;
}

struct Session {
    Places places;
    FogAdapter adapter{system_allocator(MemoryDomain::World)};
    abi::Host host{system_allocator(MemoryDomain::Scripting)};

    Session() {
        gfog::FogGridDescription grid;
        grid.width = 32;
        grid.height = 32;
        CY_REQUIRE(adapter.create(grid).has_value());
        adapter.set_placer(&place, &places);
        game_backend::bind_fog(host, &adapter);
        places[kScout] = at(4, 4);
        places[kRaider] = at(6, 4);
        places[kBarracks] = at(20, 20);
    }

    [[nodiscard]] CyEngine engine() noexcept { return &host; }

    [[nodiscard]] u32 cell(u32 team, i64 x, i64 z) noexcept {
        const auto [px, pz] = at(x, z);
        const CyFixedVec2 position{px, pz};
        u32 out = 99;
        CY_REQUIRE_EQ(table().fog_cell(engine(), team, &position, &out), CY_RESULT_OK);
        return out;
    }

    [[nodiscard]] u32 sight(u32 team, CyEntity entity) noexcept {
        u32 out = 99;
        CY_REQUIRE_EQ(table().fog_sight(engine(), team, entity, &out), CY_RESULT_OK);
        return out;
    }

    [[nodiscard]] CyFogStatus status() noexcept {
        CyFogStatus out{};
        out.struct_size = sizeof(out);
        CY_REQUIRE_EQ(table().fog_status(engine(), &out), CY_RESULT_OK);
        return out;
    }
};

}  // namespace

CY_TEST_CASE("fog: a vision given through the table is read back and sees on the next update") {
    Session session;
    const CyFogVision scout = vision(1, 3);
    CY_REQUIRE_EQ(table().fog_set_vision(session.engine(), kScout, &scout), CY_RESULT_OK);
    CyFogVision read{};
    read.struct_size = sizeof(read);
    CY_REQUIRE_EQ(table().fog_get_vision(session.engine(), kScout, &read), CY_RESULT_OK);
    CY_CHECK_EQ(read.team, 1U);
    CY_CHECK_EQ(read.reveal_radius, 3 * kMetre);
    CY_CHECK_EQ(read.flags, 0U);

    // Nothing is seen until the host's fixed step updates the grid.
    CY_CHECK_EQ(session.cell(1, 4, 4), static_cast<u32>(CY_FOG_CELL_UNEXPLORED));
    CY_REQUIRE(session.adapter.update(1).has_value());
    CY_CHECK_EQ(session.cell(1, 4, 4), static_cast<u32>(CY_FOG_CELL_VISIBLE));
    CY_CHECK_EQ(session.cell(1, 7, 4), static_cast<u32>(CY_FOG_CELL_VISIBLE));
    CY_CHECK_EQ(session.cell(1, 8, 4), static_cast<u32>(CY_FOG_CELL_UNEXPLORED));
    CY_CHECK_EQ(session.cell(2, 4, 4), static_cast<u32>(CY_FOG_CELL_UNEXPLORED));

    // The scout walks on: what it saw stays explored.
    session.places[kScout] = at(14, 4);
    CY_REQUIRE(session.adapter.update(2).has_value());
    CY_CHECK_EQ(session.cell(1, 4, 4), static_cast<u32>(CY_FOG_CELL_EXPLORED));
    const CyFogStatus status = session.status();
    CY_CHECK_EQ(status.width, 32U);
    CY_CHECK_EQ(status.units, 1U);
    CY_CHECK_EQ(status.tick, 2U);
    CY_CHECK_EQ(status.visible_cells, 29U);
    CY_CHECK_EQ(status.digest, session.adapter.fog().digest());
}

CY_TEST_CASE("fog: another team's units by sight — hidden, visible, and a structure's ghost") {
    Session session;
    const CyFogVision scout = vision(1, 3);
    const CyFogVision raider = vision(2, 2);
    const CyFogVision barracks = vision(2, 2, CY_FOG_LEAVES_GHOST);
    CY_REQUIRE_EQ(table().fog_set_vision(session.engine(), kScout, &scout), CY_RESULT_OK);
    CY_REQUIRE_EQ(table().fog_set_vision(session.engine(), kRaider, &raider), CY_RESULT_OK);
    CY_REQUIRE_EQ(table().fog_set_vision(session.engine(), kBarracks, &barracks), CY_RESULT_OK);
    CY_REQUIRE(session.adapter.update(1).has_value());
    CY_CHECK_EQ(session.sight(1, kRaider), static_cast<u32>(CY_FOG_SIGHT_VISIBLE));
    CY_CHECK_EQ(session.sight(1, kBarracks), static_cast<u32>(CY_FOG_SIGHT_HIDDEN));
    CY_CHECK_EQ(session.sight(2, kScout), static_cast<u32>(CY_FOG_SIGHT_VISIBLE));

    // The scout reaches the barracks, then leaves: blue remembers it as a ghost.
    session.places[kScout] = at(18, 20);
    CY_REQUIRE(session.adapter.update(2).has_value());
    CY_CHECK_EQ(session.sight(1, kBarracks), static_cast<u32>(CY_FOG_SIGHT_VISIBLE));
    CY_CHECK_EQ(session.sight(1, kRaider), static_cast<u32>(CY_FOG_SIGHT_HIDDEN));
    session.places[kScout] = at(4, 28);
    CY_REQUIRE(session.adapter.update(3).has_value());
    CY_CHECK_EQ(session.sight(1, kBarracks), static_cast<u32>(CY_FOG_SIGHT_GHOST));
    CY_CHECK_EQ(session.status().ghosts, 1U);
    // A despawned structure is still a ghost until its place is seen again.
    session.places.erase(kBarracks);
    session.adapter.forget(kBarracks);
    CY_REQUIRE(session.adapter.update(4).has_value());
    CY_CHECK_EQ(session.sight(1, kBarracks), static_cast<u32>(CY_FOG_SIGHT_GHOST));
    session.places[kScout] = at(19, 20);
    CY_REQUIRE(session.adapter.update(5).has_value());
    CY_CHECK_EQ(session.sight(1, kBarracks), static_cast<u32>(CY_FOG_SIGHT_HIDDEN));
    // An entity the fog never placed is hidden, not an error.
    CY_CHECK_EQ(session.sight(1, kStranger), static_cast<u32>(CY_FOG_SIGHT_HIDDEN));
}

CY_TEST_CASE("fog: a refused vision changes nothing, and a frame update may not give one") {
    Session session;
    const CyFogVision good = vision(1, 3);
    CY_REQUIRE_EQ(table().fog_set_vision(session.engine(), kScout, &good), CY_RESULT_OK);
    CyFogVision bad = vision(33, 3);
    CY_CHECK_EQ(table().fog_set_vision(session.engine(), kScout, &bad), CY_RESULT_INVALID_ARGUMENT);
    bad = vision(1, -1);
    CY_CHECK_EQ(table().fog_set_vision(session.engine(), kScout, &bad), CY_RESULT_INVALID_ARGUMENT);
    bad = vision(1, 3, 0x2U);
    CY_CHECK_EQ(table().fog_set_vision(session.engine(), kScout, &bad), CY_RESULT_INVALID_ARGUMENT);
    bad = vision(1, 3);
    bad.reserved = 1;
    CY_CHECK_EQ(table().fog_set_vision(session.engine(), kScout, &bad), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().fog_set_vision(session.engine(), kScout, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().fog_set_vision(session.engine(), kStranger, &good), CY_RESULT_NOT_FOUND);
    CyFogVision read{};
    read.struct_size = sizeof(read);
    CY_REQUIRE_EQ(table().fog_get_vision(session.engine(), kScout, &read), CY_RESULT_OK);
    CY_CHECK_EQ(read.team, 1U);
    CY_CHECK_EQ(read.reveal_radius, 3 * kMetre);
    CY_CHECK_EQ(table().fog_get_vision(session.engine(), kRaider, &read), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(session.adapter.vision_count(), 1U);
    CY_REQUIRE(session.adapter.update(1).has_value());

    // THE PHASE RULE: a fixed step may give a vision, a frame update may only ask.
    {
        abi::game::PhaseScope fixed(session.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_CHECK_EQ(table().fog_set_vision(session.engine(), kRaider, &good), CY_RESULT_OK);
    }
    {
        abi::game::PhaseScope frame(session.host.game.clock, CY_PHASE_FRAME_UPDATE);
        CY_CHECK_EQ(table().fog_set_vision(session.engine(), kBarracks, &good),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(session.sight(1, kScout), static_cast<u32>(CY_FOG_SIGHT_VISIBLE));
        CY_CHECK_EQ(session.status().width, 32U);
    }
    CY_CHECK_EQ(session.adapter.vision_count(), 2U);

    // No adapter bound: every entry is UNAVAILABLE.
    game_backend::bind_fog(session.host, nullptr);
    CY_CHECK_EQ(table().fog_set_vision(session.engine(), kScout, &good), CY_RESULT_UNAVAILABLE);
    u32 out = 0;
    CY_CHECK_EQ(table().fog_sight(session.engine(), 1, kScout, &out), CY_RESULT_UNAVAILABLE);
}

CY_TEST_CASE("fog: the digest is the state's, whatever order the visions were given in") {
    Session first;
    Session second;
    const CyFogVision visions[3] = {vision(1, 3), vision(2, 1), vision(2, 2, CY_FOG_LEAVES_GHOST)};
    const CyEntity entities[3] = {kScout, kRaider, kBarracks};
    for (u32 index = 0; index < 3U; ++index) {
        CY_REQUIRE_EQ(table().fog_set_vision(first.engine(), entities[index], &visions[index]),
                      CY_RESULT_OK);
        CY_REQUIRE_EQ(
            table().fog_set_vision(second.engine(), entities[2U - index], &visions[2U - index]),
            CY_RESULT_OK);
    }
    for (u64 tick = 1; tick <= 4U; ++tick) {
        first.places[kScout] = at(static_cast<i64>(4U + (tick * 4U)), 4 + static_cast<i64>(tick));
        second.places[kScout] = first.places[kScout];
        CY_REQUIRE(first.adapter.update(tick).has_value());
        CY_REQUIRE(second.adapter.update(tick).has_value());
        CY_CHECK_EQ(first.status().digest, second.status().digest);
    }
}

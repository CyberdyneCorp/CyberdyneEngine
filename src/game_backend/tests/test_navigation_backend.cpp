// SPDX-License-Identifier: MIT
// integration.game_backend_navigation — `NavigationAdapter` against a real navigation mesh, path
// queue and crowd, through the ABI table. `add-swift-game-api`, task 2.3.
//
// The unit suite (src/abi/tests/test_game_navigation.cpp) proves the thunks; this proves the
// adapter's determinism claims — a synchronous path is the same bytes twice, an asynchronous one
// completes on the same tick on two runs, the order a tick's scripts issue moves in cannot change
// where agents end up — and the agent lifecycle: COMPUTING, FOLLOWING, then ARRIVED with the event
// flag for exactly one tick; stop is IDLE; the first configure is structural.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/game_backend/navigation_backend.h>
#include <cy/navigation/components.h>
#include <cy/navigation/navmesh.h>
#include <cy/navigation/query.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

using namespace cy;
using namespace cy::navigation;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

/// Bit-for-bit equality. The determinism claims are about bytes, not values within a tolerance,
/// so this compares object representations on purpose.
bool same_bytes(const void* a, const void* b, std::size_t size) noexcept {
    return std::memcmp(a, b, size) == 0;
}

constexpr f32 kTileSize = 16.0F;
constexpr u32 kCells = 8;
constexpr u32 kLatency = 3;
constexpr f32 kDelta = 1.0F / 60.0F;

/// One flat tile of `cells` x `cells` quads, wound counter-clockwise from above so the internal
/// edges match. The same construction as src/navigation/tests/nav_fixture.h, which this suite
/// cannot include.
NavTileData grid_tile() noexcept {
    NavTileData data(allocator());
    data.coord = TileCoord{0, 0, 0};
    const f32 step = kTileSize / static_cast<f32>(kCells);
    for (u32 row = 0; row <= kCells; ++row) {
        for (u32 column = 0; column <= kCells; ++column) {
            CY_REQUIRE(data.vertices()
                           .push_back(Vec3{static_cast<f32>(column) * step, 0.0F,
                                           static_cast<f32>(row) * step})
                           .has_value());
        }
    }
    const auto vertex_of = [](u32 row, u32 column) noexcept {
        return (row * (kCells + 1U)) + column;
    };
    for (u32 row = 0; row < kCells; ++row) {
        for (u32 column = 0; column < kCells; ++column) {
            NavPoly poly;
            poly.first_corner = static_cast<u32>(data.corners().size());
            poly.corner_count = 4;
            poly.cost = 1.0F;
            CY_REQUIRE(data.polys().push_back(poly).has_value());
            const u32 corner[4] = {vertex_of(row, column), vertex_of(row, column + 1U),
                                   vertex_of(row + 1U, column + 1U), vertex_of(row + 1U, column)};
            for (const u32 index : corner) {
                CY_REQUIRE(data.corners().push_back(index).has_value());
            }
        }
    }
    data.finalise();
    return data;
}

/// An ECS world, a mesh, its queue, the registry, a host with the adapter bound, and a clock.
struct Level {
    ecs::World world{allocator()};
    NavComponents components;
    NavMesh mesh{allocator(), Name::intern("game-backend.nav"), kTileSize};
    PathQueue queue{allocator(), mesh, kLatency};
    NavWorlds worlds{allocator()};
    abi::Host host{system_allocator(MemoryDomain::Scripting)};
    abi::World binding{allocator(), world};
    game_backend::NavigationAdapter* adapter = nullptr;
    alignas(game_backend::NavigationAdapter) unsigned char storage[sizeof(
        game_backend::NavigationAdapter)] = {};

    Level() noexcept {
        CY_REQUIRE(world.initialize().has_value());
        const Expected<NavComponents, Error> ids = NavComponents::register_all(world);
        CY_REQUIRE(ids.has_value());
        components = *ids;
        CY_REQUIRE(mesh.add_tile(grid_tile()).has_value());
        CY_REQUIRE(worlds.bind(0, mesh, queue).has_value());
        adapter = new (storage) game_backend::NavigationAdapter(allocator(), world, components,
                                                                worlds, host.game.clock);
        game_backend::bind(host, adapter);
        host.bind_world(&binding);
        host.game.clock.tick = 1;
    }

    ~Level() {
        game_backend::bind(host, nullptr);
        host.bind_world(nullptr);
        adapter->~NavigationAdapter();
    }

    Level(const Level&) = delete;
    Level& operator=(const Level&) = delete;

    [[nodiscard]] CyEngine engine() noexcept { return &host; }

    /// A live entity with a `NavAgent` at `position`, configured through the table.
    CyEntity agent(Vec3 position) noexcept {
        const Expected<ecs::Entity, Error> entity = world.create();
        CY_REQUIRE(entity.has_value());
        NavAgent initial;
        initial.position = position;
        CY_REQUIRE(world.add(*entity, components.agent, &initial).has_value());
        CyNavAgentParams params{};
        params.struct_size = sizeof(CyNavAgentParams);
        params.max_speed = 4.0F;
        params.arrival_distance = 0.25F;
        CY_REQUIRE(table().nav_agent_configure(engine(), entity->bits(), &params) == CY_RESULT_OK);
        return entity->bits();
    }

    /// One fixed tick: the scripts' calls (`scripts`) in F, then the navigation update.
    template <class Scripts>
    void tick(Scripts&& scripts) noexcept {
        {
            const abi::game::PhaseScope fixed(host.game.clock, CY_PHASE_FIXED_UPDATE);
            scripts();
        }
        CY_REQUIRE(adapter->update(kDelta).has_value());
        ++host.game.clock.tick;
    }

    void tick() noexcept {
        tick([] {});
    }

    [[nodiscard]] CyNavAgentState state(CyEntity entity) noexcept {
        CyNavAgentState out{};
        out.struct_size = sizeof(CyNavAgentState);
        CY_REQUIRE(table().nav_agent_state(engine(), entity, &out) == CY_RESULT_OK);
        return out;
    }
};

CyNavPathRequest request(Vec3 start, Vec3 end) noexcept {
    CyNavPathRequest value{};
    value.struct_size = sizeof(CyNavPathRequest);
    value.start[0] = start.x;
    value.start[1] = start.y;
    value.start[2] = start.z;
    value.end[0] = end.x;
    value.end[1] = end.y;
    value.end[2] = end.z;
    return value;
}

CyNavPathResult result() noexcept {
    CyNavPathResult value{};
    value.struct_size = sizeof(CyNavPathResult);
    return value;
}

void move_to(Level& level, CyEntity entity, Vec3 target) noexcept {
    const float xyz[3] = {target.x, target.y, target.z};
    CY_REQUIRE(table().nav_agent_move_to(level.engine(), entity, xyz) == CY_RESULT_OK);
}

}  // namespace

CY_TEST_CASE("a synchronous path runs from start to end, and is the same bytes twice") {
    Level level;
    // Down the middle of a cell row, the funnel's case: the taut path is the straight line.
    const CyNavPathRequest straight = request(Vec3{1.0F, 0.0F, 7.0F}, Vec3{15.0F, 0.0F, 7.0F});
    float line[3 * 8] = {};
    CyNavPathResult out = result();
    CY_REQUIRE(table().nav_find_path(level.engine(), &straight, line, 8, &out) == CY_RESULT_OK);
    CY_CHECK((out.flags & CY_NAV_PATH_FOUND) != 0U);
    CY_CHECK_EQ(out.flags & CY_NAV_PATH_PARTIAL, 0U);
    CY_CHECK_EQ(out.state, static_cast<u32>(CY_NAV_QUERY_READY));
    CY_REQUIRE(out.point_count == 2U);
    CY_CHECK_NEAR(line[0], 1.0F, 1e-4F);
    CY_CHECK_NEAR(line[3], 15.0F, 1e-4F);
    CY_CHECK_NEAR(line[5], 7.0F, 1e-4F);
    CY_CHECK_NEAR(out.length, 14.0F, 1e-3F);

    // Across the floor: whatever the path is, it starts and ends where asked, and asking again in
    // the same world gives the same bytes.
    const CyNavPathRequest path = request(Vec3{1.0F, 0.0F, 1.0F}, Vec3{14.0F, 0.0F, 13.0F});
    float first[3 * 32] = {};
    CY_REQUIRE(table().nav_find_path(level.engine(), &path, first, 32, &out) == CY_RESULT_OK);
    CY_REQUIRE(out.point_count >= 2U);
    const u32 last = (out.point_count - 1U) * 3U;
    CY_CHECK_NEAR(first[0], 1.0F, 1e-4F);
    CY_CHECK_NEAR(first[last], 14.0F, 1e-4F);
    CY_CHECK_NEAR(first[last + 2U], 13.0F, 1e-4F);
    CY_CHECK_GE(out.length, std::sqrt((13.0F * 13.0F) + (12.0F * 12.0F)) - 1e-3F);

    float second[3 * 32] = {};
    CyNavPathResult again = result();
    CY_REQUIRE(table().nav_find_path(level.engine(), &path, second, 32, &again) == CY_RESULT_OK);
    CY_CHECK(same_bytes(first, second, sizeof(first)));
    CY_CHECK(same_bytes(&out, &again, sizeof(out)));
}

CY_TEST_CASE("a path off the mesh is an answer, and an unbound world is NOT_FOUND") {
    Level level;
    CyNavPathRequest path = request(Vec3{1.0F, 0.0F, 1.0F}, Vec3{90.0F, 0.0F, 90.0F});
    CyNavPathResult out = result();
    CY_REQUIRE(table().nav_find_path(level.engine(), &path, nullptr, 0, &out) == CY_RESULT_OK);
    CY_CHECK_EQ(out.flags & CY_NAV_PATH_FOUND, 0U);
    CY_CHECK_EQ(out.point_count, 0U);

    path.world = 9;
    CY_CHECK_EQ(table().nav_find_path(level.engine(), &path, nullptr, 0, &out),
                CY_RESULT_NOT_FOUND);
    const abi::game::PhaseScope fixed(level.host.game.clock, CY_PHASE_FIXED_UPDATE);
    CyNavQuery query = CY_NAV_QUERY_NULL;
    CY_CHECK_EQ(table().nav_request_path(level.engine(), &path, &query), CY_RESULT_NOT_FOUND);
    abi::clear_last_error();
}

namespace {

/// Request a path on tick 1 and poll every tick; the tick it became READY and the bytes it gave.
struct AsyncRun {
    u64 ready_tick = 0;
    CyNavPathResult result{};
    float points[3 * 32] = {};
};

AsyncRun run_async() noexcept {
    Level level;
    AsyncRun run;
    const CyNavPathRequest path = request(Vec3{1.0F, 0.0F, 1.0F}, Vec3{15.0F, 0.0F, 3.0F});
    CyNavQuery query = CY_NAV_QUERY_NULL;
    level.tick([&] {
        CY_REQUIRE(table().nav_request_path(level.engine(), &path, &query) == CY_RESULT_OK);
    });
    CY_REQUIRE(query != CY_NAV_QUERY_NULL);
    for (u32 guard = 0; guard < 20U && run.ready_tick == 0U; ++guard) {
        level.tick([&] {
            CyNavPathResult out = result();
            CY_REQUIRE(table().nav_poll_path(level.engine(), query, run.points, 32, &out) ==
                       CY_RESULT_OK);
            if (out.state == CY_NAV_QUERY_READY) {
                run.ready_tick = level.host.game.clock.tick;
                run.result = out;
            }
        });
    }
    return run;
}

}  // namespace

CY_TEST_CASE("an asynchronous path completes on the same tick, with the same bytes, every run") {
    const AsyncRun first = run_async();
    const AsyncRun second = run_async();
    // Submitted on tick 1 with a latency of three: the queue delivers it on tick 4's update, so the
    // first poll that sees it is tick 5's — on both runs, whatever the searches cost.
    CY_CHECK_EQ(first.ready_tick, 1U + kLatency + 1U);
    CY_CHECK_EQ(second.ready_tick, first.ready_tick);
    CY_CHECK((first.result.flags & CY_NAV_PATH_FOUND) != 0U);
    CY_CHECK(same_bytes(&first.result, &second.result, sizeof(CyNavPathResult)));
    CY_CHECK(same_bytes(first.points, second.points, sizeof(first.points)));
}

CY_TEST_CASE("a READY path too big for the buffer stays READY until a poll has room for it") {
    Level level;
    // A detour-free path still has two points; a buffer of one cannot hold it.
    const CyNavPathRequest path = request(Vec3{1.0F, 0.0F, 1.0F}, Vec3{15.0F, 0.0F, 3.0F});
    CyNavQuery query = CY_NAV_QUERY_NULL;
    level.tick([&] {
        CY_REQUIRE(table().nav_request_path(level.engine(), &path, &query) == CY_RESULT_OK);
    });
    for (u32 index = 0; index < kLatency; ++index) {
        level.tick();
    }
    const abi::game::PhaseScope fixed(level.host.game.clock, CY_PHASE_FIXED_UPDATE);
    float one[3] = {};
    CyNavPathResult out = result();
    CY_CHECK_EQ(table().nav_poll_path(level.engine(), query, one, 1, &out),
                CY_RESULT_BUFFER_TOO_SMALL);
    CY_CHECK_EQ(out.state, static_cast<u32>(CY_NAV_QUERY_READY));
    CY_REQUIRE(out.point_count >= 2U);
    CY_CHECK_EQ(one[0], 0.0F);  // nothing written

    // A null buffer asks, and still does not consume.
    CY_CHECK_EQ(table().nav_poll_path(level.engine(), query, nullptr, 0, &out), CY_RESULT_OK);
    CY_REQUIRE(out.point_count >= 2U);
    CY_REQUIRE(out.point_count <= 16U);
    const usize needed = std::min<usize>(out.point_count, 16U);
    const usize last_x = needed > 0U ? (needed - 1U) * 3U : 0U;
    float whole[3 * 16] = {};
    CY_CHECK_EQ(table().nav_poll_path(level.engine(), query, whole, static_cast<u32>(needed), &out),
                CY_RESULT_OK);
    CY_CHECK_NEAR(whole[last_x], 15.0F, 1e-4F);

    // Taken: gone.
    CY_CHECK_EQ(table().nav_poll_path(level.engine(), query, whole, static_cast<u32>(needed), &out),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().nav_cancel_path(level.engine(), query), CY_RESULT_NOT_FOUND);
    abi::clear_last_error();
}

CY_TEST_CASE("a cancelled query answers CANCELLED once and is then retired") {
    Level level;
    const CyNavPathRequest path = request(Vec3{1.0F, 0.0F, 1.0F}, Vec3{15.0F, 0.0F, 3.0F});
    CyNavQuery query = CY_NAV_QUERY_NULL;
    level.tick([&] {
        CY_REQUIRE(table().nav_request_path(level.engine(), &path, &query) == CY_RESULT_OK);
        CY_REQUIRE(table().nav_cancel_path(level.engine(), query) == CY_RESULT_OK);
    });
    for (u32 index = 0; index < kLatency + 1U; ++index) {
        level.tick();
    }
    const abi::game::PhaseScope fixed(level.host.game.clock, CY_PHASE_FIXED_UPDATE);
    CyNavPathResult out = result();
    CY_CHECK_EQ(table().nav_poll_path(level.engine(), query, nullptr, 0, &out), CY_RESULT_OK);
    CY_CHECK_EQ(out.state, static_cast<u32>(CY_NAV_QUERY_CANCELLED));
    CY_CHECK_EQ(table().nav_poll_path(level.engine(), query, nullptr, 0, &out),
                CY_RESULT_NOT_FOUND);
    abi::clear_last_error();
}

CY_TEST_CASE("a moved agent computes, follows, and arrives with the event for exactly one tick") {
    Level level;
    const CyEntity unit = level.agent(Vec3{1.0F, 0.0F, 1.0F});
    CY_CHECK_EQ(level.state(unit).status, static_cast<u32>(CY_NAV_PATH_STATUS_IDLE));

    level.tick([&] { move_to(level, unit, Vec3{12.0F, 0.0F, 6.0F}); });
    CY_CHECK_EQ(level.state(unit).status, static_cast<u32>(CY_NAV_PATH_STATUS_COMPUTING));
    CY_CHECK(level.state(unit).remaining_distance > 10.0F);

    u32 arrived_on = 0;
    u32 following_seen = 0;
    for (u32 tick = 0; tick < 600U && arrived_on == 0U; ++tick) {
        level.tick();
        const CyNavAgentState now = level.state(unit);
        following_seen += now.status == CY_NAV_PATH_STATUS_FOLLOWING ? 1U : 0U;
        if (now.status == CY_NAV_PATH_STATUS_ARRIVED) {
            arrived_on = tick;
            CY_CHECK_EQ(now.flags & CY_NAV_AGENT_EVENT, CY_NAV_AGENT_EVENT);
            CY_CHECK_NEAR(now.position[0], 12.0F, 0.3F);
            CY_CHECK_NEAR(now.position[2], 6.0F, 0.3F);
            CY_CHECK_EQ(now.remaining_distance, 0.0F);
        }
    }
    CY_REQUIRE(arrived_on != 0U);
    CY_CHECK(following_seen > 0U);

    // The next tick: still ARRIVED, the event gone.
    level.tick();
    const CyNavAgentState after = level.state(unit);
    CY_CHECK_EQ(after.status, static_cast<u32>(CY_NAV_PATH_STATUS_ARRIVED));
    CY_CHECK_EQ(after.flags & CY_NAV_AGENT_EVENT, 0U);
}

CY_TEST_CASE("a stopped agent is IDLE where it stood, and stays there") {
    Level level;
    const CyEntity unit = level.agent(Vec3{1.0F, 0.0F, 1.0F});
    level.tick([&] { move_to(level, unit, Vec3{14.0F, 0.0F, 1.0F}); });
    for (u32 tick = 0; tick < 30U; ++tick) {
        level.tick();
    }
    CY_REQUIRE(level.state(unit).status == CY_NAV_PATH_STATUS_FOLLOWING);
    level.tick([&] { CY_REQUIRE(table().nav_agent_stop(level.engine(), unit) == CY_RESULT_OK); });
    const CyNavAgentState stopped = level.state(unit);
    CY_CHECK_EQ(stopped.status, static_cast<u32>(CY_NAV_PATH_STATUS_IDLE));
    for (u32 tick = 0; tick < 30U; ++tick) {
        level.tick();
    }
    const CyNavAgentState later = level.state(unit);
    CY_CHECK_EQ(later.status, static_cast<u32>(CY_NAV_PATH_STATUS_IDLE));
    CY_CHECK_EQ(later.position[0], stopped.position[0]);
    CY_CHECK_EQ(later.position[2], stopped.position[2]);
}

namespace {

/// Two agents ordered across each other in one tick, in the given order; every tick's state.
struct CrowdRun {
    CyNavAgentState states[2][360] = {};
};

void run_crowd(bool a_first, CrowdRun& run) noexcept {
    Level level;
    // Opposite directions down neighbouring lanes, a body width apart, so the crowd has to
    // resolve them. The lanes run down the middle of cell rows: a line along a grid edge is a
    // degenerate funnel.
    const CyEntity a = level.agent(Vec3{2.0F, 0.0F, 7.0F});
    const CyEntity b = level.agent(Vec3{14.0F, 0.0F, 7.6F});
    level.tick([&] {
        if (a_first) {
            move_to(level, a, Vec3{14.0F, 0.0F, 7.0F});
            move_to(level, b, Vec3{2.0F, 0.0F, 7.6F});
        } else {
            move_to(level, b, Vec3{2.0F, 0.0F, 7.6F});
            move_to(level, a, Vec3{14.0F, 0.0F, 7.0F});
        }
    });
    for (u32 tick = 0; tick < 360U; ++tick) {
        level.tick();
        run.states[0][tick] = level.state(a);
        run.states[1][tick] = level.state(b);
    }
}

}  // namespace

CY_TEST_CASE(
    "agents crossing are deterministic, and the order moves were issued in is irrelevant") {
    static CrowdRun first;
    static CrowdRun second;
    static CrowdRun swapped;
    run_crowd(true, first);
    run_crowd(true, second);
    run_crowd(false, swapped);
    CY_CHECK(same_bytes(&first, &second, sizeof(CrowdRun)));
    CY_CHECK(same_bytes(&first, &swapped, sizeof(CrowdRun)));
    // And they did get where they were going.
    CY_CHECK_EQ(first.states[0][359].status, static_cast<u32>(CY_NAV_PATH_STATUS_ARRIVED));
    CY_CHECK_EQ(first.states[1][359].status, static_cast<u32>(CY_NAV_PATH_STATUS_ARRIVED));
}

CY_TEST_CASE("configure is structural once, and refuses what is not an entity") {
    Level level;
    const Expected<ecs::Entity, Error> bare = level.world.create();
    CY_REQUIRE(bare.has_value());
    const u64 before = level.binding.epoch;
    CyNavAgentParams params{};
    params.struct_size = sizeof(CyNavAgentParams);
    CY_CHECK_EQ(table().nav_agent_configure(level.engine(), bare->bits(), &params), CY_RESULT_OK);
    CY_CHECK(level.world.has(*bare, level.components.agent));
    CY_CHECK_EQ(level.binding.epoch, before + 1U);
    params.max_speed = 9.0F;
    CY_CHECK_EQ(table().nav_agent_configure(level.engine(), bare->bits(), &params), CY_RESULT_OK);
    CY_CHECK_EQ(level.binding.epoch, before + 1U);
    CY_CHECK_EQ(level.world.get<NavAgent>(*bare, level.components.agent)->avoidance.max_speed,
                9.0F);
    CY_CHECK_EQ(level.adapter->agent_count(), 1U);

    CY_CHECK_EQ(table().nav_agent_configure(level.engine(), CY_ENTITY_NULL, &params),
                CY_RESULT_NOT_FOUND);
    params.world = 4;
    CY_CHECK_EQ(table().nav_agent_configure(level.engine(), bare->bits(), &params),
                CY_RESULT_NOT_FOUND);

    // Not an agent: NOT_FOUND from every agent entry.
    const Expected<ecs::Entity, Error> other = level.world.create();
    CY_REQUIRE(other.has_value());
    CyNavAgentState state{};
    CY_CHECK_EQ(table().nav_agent_state(level.engine(), other->bits(), &state),
                CY_RESULT_NOT_FOUND);
    const abi::game::PhaseScope fixed(level.host.game.clock, CY_PHASE_FIXED_UPDATE);
    const float target[3] = {1.0F, 0.0F, 1.0F};
    CY_CHECK_EQ(table().nav_agent_move_to(level.engine(), other->bits(), target),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().nav_agent_stop(level.engine(), other->bits()), CY_RESULT_NOT_FOUND);
    abi::clear_last_error();
}

CY_TEST_CASE("an agent whose entity dies is dropped by the next update") {
    Level level;
    const CyEntity unit = level.agent(Vec3{1.0F, 0.0F, 1.0F});
    CY_CHECK_EQ(level.adapter->agent_count(), 1U);
    CY_REQUIRE(level.world.destroy(ecs::Entity::from_bits(unit)).has_value());
    level.tick();
    CY_CHECK_EQ(level.adapter->agent_count(), 0U);
    CyNavAgentState state{};
    CY_CHECK_EQ(table().nav_agent_state(level.engine(), unit, &state), CY_RESULT_NOT_FOUND);
    abi::clear_last_error();
}

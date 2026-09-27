// SPDX-License-Identifier: MIT
// ABI 1.3's `nav_*` thunks against a fake backend. `add-swift-game-api`, task 2.3.
//
// What the thunks own is tested here: each entry's phase list (the queue and the orders are `F`
// only, configure is `N F`, the reads are `N F U`), UNAVAILABLE with no backend, the null, value
// and `struct_size` checks, the sizing pattern over points, the READY-but-too-small poll, and the
// epoch bump on the one structural call. The real path search, queue latency and crowd are the
// adapter's, and integration.game_backend_navigation tests them.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/navigation.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/test/test.h>

#include <cstddef>
#include <cstring>
#include <limits>

namespace {

using cy::u32;
using cy::abi::game::PhaseScope;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

/// A path of `points` points along +X, a query that is READY once `ready` is set, and an agent
/// table of exactly one entity (7).
class FakeNavigation final : public cy::abi::game::NavigationBackend {
public:
    u32 points = 4;
    bool ready = false;
    bool consumed = false;
    bool configured = false;
    u32 calls = 0;
    CyNavPathRequest last_request{};
    CyNavAgentParams last_params{};
    float last_target[3] = {};

    CyResult find_path(const CyNavPathRequest& request, cy::Span<cy::f32> out_points_xyz,
                       CyNavPathResult& out_result) noexcept override {
        ++calls;
        last_request = request;
        write_path(out_points_xyz, out_result);
        out_result.state = CY_NAV_QUERY_READY;
        return CY_RESULT_OK;
    }

    CyResult request_path(const CyNavPathRequest& request,
                          CyNavQuery& out_query) noexcept override {
        ++calls;
        last_request = request;
        out_query = 0x100000001ULL;
        return CY_RESULT_OK;
    }

    CyResult poll_path(CyNavQuery query, cy::Span<cy::f32> out_points_xyz,
                       CyNavPathResult& out_result) noexcept override {
        ++calls;
        if (query != 0x100000001ULL || consumed) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such query");
        }
        if (!ready) {
            out_result.state = CY_NAV_QUERY_PENDING;
            return CY_RESULT_OK;
        }
        out_result.state = CY_NAV_QUERY_READY;
        out_result.point_count = points;
        if (out_points_xyz.size() < static_cast<std::size_t>(points) * 3U) {
            return CY_RESULT_OK;  // left READY, nothing written
        }
        write_path(out_points_xyz, out_result);
        consumed = true;
        return CY_RESULT_OK;
    }

    CyResult cancel_path(CyNavQuery query) noexcept override {
        ++calls;
        return query == 0x100000001ULL ? CY_RESULT_OK
                                       : cy::abi::report(CY_RESULT_NOT_FOUND, "no such query");
    }

    CyResult agent_configure(CyEntity entity, const CyNavAgentParams& params,
                             bool& out_structural) noexcept override {
        ++calls;
        if (entity != 7U) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such entity");
        }
        last_params = params;
        out_structural = !configured;
        configured = true;
        return CY_RESULT_OK;
    }

    CyResult agent_move_to(CyEntity entity, const cy::f32* target_xyz) noexcept override {
        ++calls;
        if (entity != 7U || !configured) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "not an agent");
        }
        std::memcpy(last_target, target_xyz, sizeof(last_target));
        return CY_RESULT_OK;
    }

    CyResult agent_stop(CyEntity entity) noexcept override {
        ++calls;
        return entity == 7U && configured ? CY_RESULT_OK
                                          : cy::abi::report(CY_RESULT_NOT_FOUND, "not an agent");
    }

    CyResult agent_state(CyEntity entity, CyNavAgentState& out_state) const noexcept override {
        if (entity != 7U || !configured) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "not an agent");
        }
        out_state.status = CY_NAV_PATH_STATUS_ARRIVED;
        out_state.flags = CY_NAV_AGENT_EVENT;
        out_state.remaining_distance = 2.5F;
        return CY_RESULT_OK;
    }

private:
    void write_path(cy::Span<cy::f32> out, CyNavPathResult& result) const noexcept {
        result.flags = CY_NAV_PATH_FOUND;
        result.point_count = points;
        result.length = static_cast<float>(points - 1U);
        for (u32 index = 0; index < points && (index * 3U) + 2U < out.size(); ++index) {
            out[static_cast<std::size_t>(index) * 3U] = static_cast<float>(index);
        }
    }
};

struct Fixture {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakeNavigation navigation;

    Fixture() noexcept { host.game.navigation = &navigation; }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

CyNavPathRequest request() noexcept {
    CyNavPathRequest value{};
    value.struct_size = sizeof(CyNavPathRequest);
    value.end[0] = 3.0F;
    return value;
}

CyNavPathResult result() noexcept {
    CyNavPathResult value{};
    value.struct_size = sizeof(CyNavPathResult);
    return value;
}

}  // namespace

CY_TEST_CASE("nav_find_path writes the points and the whole path's size") {
    Fixture fixture;
    const CyNavPathRequest path = request();
    float points[12] = {};
    CyNavPathResult out = result();
    CY_CHECK_EQ(table().nav_find_path(fixture.engine(), &path, points, 4, &out), CY_RESULT_OK);
    CY_CHECK_EQ(out.point_count, 4U);
    CY_CHECK_EQ(out.flags, CY_NAV_PATH_FOUND);
    CY_CHECK_EQ(out.state, static_cast<u32>(CY_NAV_QUERY_READY));
    CY_CHECK_EQ(points[9], 3.0F);
    CY_CHECK_EQ(fixture.navigation.last_request.end[0], 3.0F);
}

CY_TEST_CASE("nav_find_path follows the sizing pattern over points") {
    Fixture fixture;
    const CyNavPathRequest path = request();
    CyNavPathResult out = result();
    CY_CHECK_EQ(table().nav_find_path(fixture.engine(), &path, nullptr, 0, &out), CY_RESULT_OK);
    CY_CHECK_EQ(out.point_count, 4U);

    float two[6] = {};
    out = result();
    CY_CHECK_EQ(table().nav_find_path(fixture.engine(), &path, two, 2, &out),
                CY_RESULT_BUFFER_TOO_SMALL);
    CY_CHECK_EQ(out.point_count, 4U);
    CY_CHECK_EQ(two[3], 1.0F);  // the first `capacity` points were written
}

CY_TEST_CASE("nav_find_path reads an older request whole and writes an older result short") {
    Fixture fixture;
    CyNavPathRequest path = request();
    path.node_budget = 77;
    path.area_mask = 0xFF;
    path.struct_size = static_cast<u32>(offsetof(CyNavPathRequest, node_budget));
    CyNavPathResult out{};
    std::memset(&out, 0xAB, sizeof(out));
    out.struct_size = static_cast<u32>(offsetof(CyNavPathResult, state));
    CY_CHECK_EQ(table().nav_find_path(fixture.engine(), &path, nullptr, 0, &out), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.navigation.last_request.node_budget, 0U);
    CY_CHECK_EQ(fixture.navigation.last_request.area_mask, 0U);
    CY_CHECK_EQ(fixture.navigation.last_request.struct_size, sizeof(CyNavPathRequest));
    CY_CHECK_EQ(out.struct_size, offsetof(CyNavPathResult, state));
    CY_CHECK_EQ(out.point_count, 4U);
    CY_CHECK_EQ(out.state, 0xABABABABU);  // past the caller's struct: untouched

    out.struct_size = 1;
    CY_CHECK_EQ(table().nav_find_path(fixture.engine(), &path, nullptr, 0, &out),
                CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("nav_find_path refuses malformed requests before the backend") {
    Fixture fixture;
    CyNavPathResult out = result();
    CyNavPathRequest path = request();
    path.start[1] = std::numeric_limits<float>::quiet_NaN();
    CY_CHECK_EQ(table().nav_find_path(fixture.engine(), &path, nullptr, 0, &out),
                CY_RESULT_INVALID_ARGUMENT);
    path = request();
    path.extents[0] = -1.0F;
    CY_CHECK_EQ(table().nav_find_path(fixture.engine(), &path, nullptr, 0, &out),
                CY_RESULT_INVALID_ARGUMENT);
    path = request();
    path.struct_size = 3;
    CY_CHECK_EQ(table().nav_find_path(fixture.engine(), &path, nullptr, 0, &out),
                CY_RESULT_INVALID_ARGUMENT);
    path = request();
    CY_CHECK_EQ(table().nav_find_path(fixture.engine(), nullptr, nullptr, 0, &out),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().nav_find_path(fixture.engine(), &path, nullptr, 0, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    float points[3] = {};
    CY_CHECK_EQ(table().nav_find_path(fixture.engine(), &path, points, 0x60000000U, &out),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.navigation.calls, 0U);
}

CY_TEST_CASE("the queue and the orders are fixed-update only; the reads answer everywhere") {
    Fixture fixture;
    const CyNavPathRequest path = request();
    CyNavQuery query = CY_NAV_QUERY_NULL;
    CyNavPathResult out = result();
    const float target[3] = {1.0F, 0.0F, 1.0F};
    CyNavAgentParams params{};
    CyNavAgentState state{};
    state.struct_size = sizeof(CyNavAgentState);

    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FRAME_UPDATE}) {
        const PhaseScope scope(fixture.host.game.clock, phase);
        CY_CHECK_EQ(table().nav_request_path(fixture.engine(), &path, &query),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK(std::strstr(cy::abi::last_error_message(), "nav_request_path") != nullptr);
        CY_CHECK_EQ(table().nav_poll_path(fixture.engine(), 1, nullptr, 0, &out),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(table().nav_cancel_path(fixture.engine(), 1), CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(table().nav_agent_move_to(fixture.engine(), 7, target),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(table().nav_agent_stop(fixture.engine(), 7), CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(query, CY_NAV_QUERY_NULL);
    }
    {
        // Configure is structural, so a frame update may not do it.
        const PhaseScope scope(fixture.host.game.clock, CY_PHASE_FRAME_UPDATE);
        CY_CHECK_EQ(table().nav_agent_configure(fixture.engine(), 7, &params),
                    CY_RESULT_PERMISSION_DENIED);
    }
    CY_CHECK_EQ(fixture.navigation.calls, 0U);

    CY_CHECK_EQ(table().nav_agent_configure(fixture.engine(), 7, &params), CY_RESULT_OK);
    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FIXED_UPDATE, CY_PHASE_FRAME_UPDATE}) {
        const PhaseScope scope(fixture.host.game.clock, phase);
        CY_CHECK_EQ(table().nav_find_path(fixture.engine(), &path, nullptr, 0, &out), CY_RESULT_OK);
        CY_CHECK_EQ(table().nav_agent_state(fixture.engine(), 7, &state), CY_RESULT_OK);
    }

    const PhaseScope fixed(fixture.host.game.clock, CY_PHASE_FIXED_UPDATE);
    CY_CHECK_EQ(table().nav_request_path(fixture.engine(), &path, &query), CY_RESULT_OK);
    CY_CHECK_EQ(query, 0x100000001ULL);
    CY_CHECK_EQ(table().nav_agent_move_to(fixture.engine(), 7, target), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.navigation.last_target[2], 1.0F);
    CY_CHECK_EQ(table().nav_agent_stop(fixture.engine(), 7), CY_RESULT_OK);
    CY_CHECK_EQ(table().nav_agent_configure(fixture.engine(), 7, &params), CY_RESULT_OK);
}

CY_TEST_CASE("with no navigation backend bound every entry is UNAVAILABLE") {
    cy::abi::Host host(cy::system_allocator(cy::MemoryDomain::Scripting));
    const PhaseScope fixed(host.game.clock, CY_PHASE_FIXED_UPDATE);
    const CyNavPathRequest path = request();
    CyNavPathResult out = result();
    CyNavQuery query = CY_NAV_QUERY_NULL;
    const float target[3] = {};
    const CyNavAgentParams params{};
    CyNavAgentState state{};
    CY_CHECK_EQ(table().nav_find_path(&host, &path, nullptr, 0, &out), CY_RESULT_UNAVAILABLE);
    CY_CHECK(std::strstr(cy::abi::last_error_message(), "navigation") != nullptr);
    CY_CHECK_EQ(table().nav_request_path(&host, &path, &query), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().nav_poll_path(&host, 1, nullptr, 0, &out), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().nav_cancel_path(&host, 1), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().nav_agent_configure(&host, 7, &params), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().nav_agent_move_to(&host, 7, target), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().nav_agent_stop(&host, 7), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().nav_agent_state(&host, 7, &state), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().nav_agent_stop(nullptr, 7), CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("a READY path that does not fit is left READY and can be taken with a bigger buffer") {
    Fixture fixture;
    const PhaseScope fixed(fixture.host.game.clock, CY_PHASE_FIXED_UPDATE);
    const CyNavQuery query = 0x100000001ULL;

    CyNavPathResult out = result();
    CY_CHECK_EQ(table().nav_poll_path(fixture.engine(), query, nullptr, 0, &out), CY_RESULT_OK);
    CY_CHECK_EQ(out.state, static_cast<u32>(CY_NAV_QUERY_PENDING));

    fixture.navigation.ready = true;
    float small[6] = {};
    out = result();
    CY_CHECK_EQ(table().nav_poll_path(fixture.engine(), query, small, 2, &out),
                CY_RESULT_BUFFER_TOO_SMALL);
    CY_CHECK_EQ(out.state, static_cast<u32>(CY_NAV_QUERY_READY));
    CY_CHECK_EQ(out.point_count, 4U);
    CY_CHECK_EQ(small[3], 0.0F);  // nothing written

    float whole[12] = {};
    out = result();
    CY_CHECK_EQ(table().nav_poll_path(fixture.engine(), query, whole, 4, &out), CY_RESULT_OK);
    CY_CHECK_EQ(whole[9], 3.0F);

    // Consumed: gone.
    CY_CHECK_EQ(table().nav_poll_path(fixture.engine(), query, whole, 4, &out),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().nav_poll_path(fixture.engine(), CY_NAV_QUERY_NULL, whole, 4, &out),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().nav_cancel_path(fixture.engine(), CY_NAV_QUERY_NULL), CY_RESULT_NOT_FOUND);
}

CY_TEST_CASE("a malformed out_result is refused before the poll can consume the query") {
    Fixture fixture;
    const PhaseScope fixed(fixture.host.game.clock, CY_PHASE_FIXED_UPDATE);
    fixture.navigation.ready = true;
    float whole[12] = {};
    CyNavPathResult out{};
    out.struct_size = 2;
    CY_CHECK_EQ(table().nav_poll_path(fixture.engine(), 0x100000001ULL, whole, 4, &out),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_FALSE(fixture.navigation.consumed);
    CY_CHECK_EQ(table().nav_poll_path(fixture.engine(), 0x100000001ULL, whole, 4, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_FALSE(fixture.navigation.consumed);
}

CY_TEST_CASE("the first agent configure bumps the world's epoch, and only the first") {
    Fixture fixture;
    cy::ecs::World ecs_world(cy::system_allocator(cy::MemoryDomain::Scripting));
    CY_REQUIRE(ecs_world.initialize().has_value());
    cy::abi::World binding(cy::system_allocator(cy::MemoryDomain::Scripting), ecs_world);
    fixture.host.bind_world(&binding);
    const cy::u64 before = binding.epoch;

    CyNavAgentParams params{};
    params.max_speed = 4.0F;
    CY_CHECK_EQ(table().nav_agent_configure(fixture.engine(), 7, &params), CY_RESULT_OK);
    CY_CHECK_EQ(binding.epoch, before + 1U);
    CY_CHECK_EQ(fixture.navigation.last_params.max_speed, 4.0F);
    CY_CHECK_EQ(fixture.navigation.last_params.struct_size, sizeof(CyNavAgentParams));

    params.max_speed = 5.0F;
    CY_CHECK_EQ(table().nav_agent_configure(fixture.engine(), 7, &params), CY_RESULT_OK);
    CY_CHECK_EQ(binding.epoch, before + 1U);

    // A refused configure moves nothing either.
    CY_CHECK_EQ(table().nav_agent_configure(fixture.engine(), 8, &params), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(binding.epoch, before + 1U);
    fixture.host.bind_world(nullptr);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("agent parameters and targets are checked before the backend") {
    Fixture fixture;
    CyNavAgentParams params{};
    params.radius = -1.0F;
    CY_CHECK_EQ(table().nav_agent_configure(fixture.engine(), 7, &params),
                CY_RESULT_INVALID_ARGUMENT);
    params = CyNavAgentParams{};
    params.max_speed = std::numeric_limits<float>::infinity();
    CY_CHECK_EQ(table().nav_agent_configure(fixture.engine(), 7, &params),
                CY_RESULT_INVALID_ARGUMENT);
    params = CyNavAgentParams{};
    params.struct_size = 1;
    CY_CHECK_EQ(table().nav_agent_configure(fixture.engine(), 7, &params),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().nav_agent_configure(fixture.engine(), 7, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.navigation.calls, 0U);

    const PhaseScope fixed(fixture.host.game.clock, CY_PHASE_FIXED_UPDATE);
    const float nan_target[3] = {0.0F, std::numeric_limits<float>::quiet_NaN(), 0.0F};
    CY_CHECK_EQ(table().nav_agent_move_to(fixture.engine(), 7, nan_target),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().nav_agent_move_to(fixture.engine(), 7, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    // Not an agent is the backend's NOT_FOUND, passed through.
    const float target[3] = {};
    CY_CHECK_EQ(table().nav_agent_move_to(fixture.engine(), 7, target), CY_RESULT_NOT_FOUND);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("nav_agent_state writes the caller's prefix and refuses a non-agent") {
    Fixture fixture;
    CyNavAgentState state{};
    state.struct_size = sizeof(CyNavAgentState);
    CY_CHECK_EQ(table().nav_agent_state(fixture.engine(), 7, &state), CY_RESULT_NOT_FOUND);
    const CyNavAgentParams params{};
    CY_REQUIRE(table().nav_agent_configure(fixture.engine(), 7, &params) == CY_RESULT_OK);

    CY_CHECK_EQ(table().nav_agent_state(fixture.engine(), 7, &state), CY_RESULT_OK);
    CY_CHECK_EQ(state.status, static_cast<u32>(CY_NAV_PATH_STATUS_ARRIVED));
    CY_CHECK_EQ(state.flags, CY_NAV_AGENT_EVENT);
    CY_CHECK_EQ(state.remaining_distance, 2.5F);

    CyNavAgentState shorter{};
    std::memset(&shorter, 0xCD, sizeof(shorter));
    shorter.struct_size = static_cast<u32>(offsetof(CyNavAgentState, reserved));
    CY_CHECK_EQ(table().nav_agent_state(fixture.engine(), 7, &shorter), CY_RESULT_OK);
    CY_CHECK_EQ(shorter.flags, CY_NAV_AGENT_EVENT);
    CY_CHECK_EQ(shorter.reserved, 0xCDCDCDCDU);

    shorter.struct_size = 1;
    CY_CHECK_EQ(table().nav_agent_state(fixture.engine(), 7, &shorter), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().nav_agent_state(fixture.engine(), 7, nullptr), CY_RESULT_INVALID_ARGUMENT);
}

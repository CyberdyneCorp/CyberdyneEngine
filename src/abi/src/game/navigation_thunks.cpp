// SPDX-License-Identifier: MIT
// The `navigation` thunks of ABI 1.3. `add-swift-game-api`.
//
// OWNER: implementer B (physics queries and navigation). See
// openspec/changes/add-swift-game-api/design.md.
//
// Each thunk runs cy/abi/game/services.h's steps before the backend sees anything. The phases are
// the header's: `nav_find_path` and `nav_agent_state` are `[N F U]`; the asynchronous queue and the
// agent orders are `[F]`, because their completion and their effect are tied to fixed ticks;
// `nav_agent_configure` is `[N F]`, because it is structural the first time. That first configure
// is the one call here that bumps the bound world's epoch.
//
// OUT STRUCTS ARE CHECKED BEFORE THE BACKEND RUNS. A malformed `struct_size` on `out_result` is
// refused up front rather than after the call, because `nav_poll_path` consumes a query as a side
// effect and a refusal after the fact would lose the path.

#include <cy/abi/errors.h>
#include <cy/abi/game/navigation.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include <cmath>

#include "thunks.h"

namespace cy::abi::game {
namespace {

constexpr u32 kQueryPhases = kPhaseAny;
constexpr u32 kQueuePhases = kPhaseFixed;
constexpr u32 kConfigurePhases = kPhaseNone | kPhaseFixed;
constexpr u32 kOrderPhases = kPhaseFixed;
/// The most points a caller's buffer may declare: `capacity * 3` floats must fit a u32 count.
constexpr uint32_t kMaxPointCapacity = 0xFFFFFFFFU / 3U;

/// Steps 1–3: the engine, the phase and the backend. Null `out` means the call was refused.
CyResult enter(CyEngine engine, u32 phases, const char* entry, NavigationBackend*& out) noexcept {
    out = nullptr;
    if (engine == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "engine is null");
    }
    if (const CyResult phase = require_phase(engine->game, phases, entry); phase != CY_RESULT_OK) {
        return phase;
    }
    if (engine->game.navigation == nullptr) {
        return report(CY_RESULT_UNAVAILABLE, "no navigation backend is bound to this engine");
    }
    out = engine->game.navigation;
    return CY_RESULT_OK;
}

bool finite3(const float* value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

bool finite_non_negative(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F;
}

/// A request, whole, with finite endpoints and non-negative extents.
CyResult normalise_request(const CyNavPathRequest& in, CyNavPathRequest& out) noexcept {
    if (!read_sized(in, out)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "path request has a malformed struct_size");
    }
    if (!finite3(out.start) || !finite3(out.end)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "path request endpoints must be finite");
    }
    if (!finite_non_negative(out.extents[0]) || !finite_non_negative(out.extents[1]) ||
        !finite_non_negative(out.extents[2])) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "path request extents must be finite and not negative");
    }
    return CY_RESULT_OK;
}

CyResult check_out_result(const CyNavPathResult* out_result) noexcept {
    if (out_result == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "out_result is null");
    }
    if (agreed_size<CyNavPathResult>(out_result->struct_size) == 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT, "out_result has a malformed struct_size");
    }
    return CY_RESULT_OK;
}

/// The caller's point buffer as the backend sees it; empty for a null one (a sizing question).
CyResult point_buffer(float* out_points_xyz, uint32_t capacity, Span<f32>& out) noexcept {
    out = Span<f32>();
    if (out_points_xyz == nullptr) {
        return CY_RESULT_OK;
    }
    if (capacity > kMaxPointCapacity) {
        return report(CY_RESULT_INVALID_ARGUMENT, "point capacity is too large");
    }
    out = Span<f32>(out_points_xyz, static_cast<usize>(capacity) * 3U);
    return CY_RESULT_OK;
}

/// Write the result and apply the sizing pattern: a non-null buffer shorter than the path is
/// BUFFER_TOO_SMALL, with `point_count` telling the caller what it needs.
CyResult finish_path(const CyNavPathResult& result, const float* out_points_xyz, uint32_t capacity,
                     CyNavPathResult& out_result) noexcept {
    (void)write_sized(out_result, result);
    if (out_points_xyz != nullptr && result.point_count > capacity) {
        return report(CY_RESULT_BUFFER_TOO_SMALL, "the path has more points than the buffer holds");
    }
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult check_params(const CyNavAgentParams& params) noexcept {
    const float values[] = {params.radius, params.height, params.max_speed, params.max_acceleration,
                            params.arrival_distance};
    for (const float value : values) {
        if (!finite_non_negative(value)) {
            return report(CY_RESULT_INVALID_ARGUMENT,
                          "agent parameters must be finite and not negative");
        }
    }
    return CY_RESULT_OK;
}

}  // namespace

CyResult nav_find_path(CyEngine engine, const CyNavPathRequest* request, float* out_points_xyz,
                       uint32_t capacity, CyNavPathResult* out_result) {
    NavigationBackend* backend = nullptr;
    if (const CyResult entered = enter(engine, kQueryPhases, "nav_find_path", backend);
        backend == nullptr) {
        return entered;
    }
    if (request == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "nav_find_path: request is null");
    }
    if (const CyResult checked = check_out_result(out_result); checked != CY_RESULT_OK) {
        return checked;
    }
    CyNavPathRequest whole{};
    if (const CyResult normalised = normalise_request(*request, whole);
        normalised != CY_RESULT_OK) {
        return normalised;
    }
    Span<f32> points;
    if (const CyResult sized = point_buffer(out_points_xyz, capacity, points);
        sized != CY_RESULT_OK) {
        return sized;
    }
    CyNavPathResult result{};
    result.struct_size = static_cast<u32>(sizeof(CyNavPathResult));
    if (const CyResult answered = backend->find_path(whole, points, result);
        answered != CY_RESULT_OK) {
        return answered;
    }
    return finish_path(result, out_points_xyz, capacity, *out_result);
}

CyResult nav_request_path(CyEngine engine, const CyNavPathRequest* request, CyNavQuery* out_query) {
    NavigationBackend* backend = nullptr;
    if (const CyResult entered = enter(engine, kQueuePhases, "nav_request_path", backend);
        backend == nullptr) {
        return entered;
    }
    if (request == nullptr || out_query == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "nav_request_path: a required pointer is null");
    }
    CyNavPathRequest whole{};
    if (const CyResult normalised = normalise_request(*request, whole);
        normalised != CY_RESULT_OK) {
        return normalised;
    }
    CyNavQuery query = CY_NAV_QUERY_NULL;
    if (const CyResult answered = backend->request_path(whole, query); answered != CY_RESULT_OK) {
        return answered;
    }
    *out_query = query;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult nav_poll_path(CyEngine engine, CyNavQuery query, float* out_points_xyz, uint32_t capacity,
                       CyNavPathResult* out_result) {
    NavigationBackend* backend = nullptr;
    if (const CyResult entered = enter(engine, kQueuePhases, "nav_poll_path", backend);
        backend == nullptr) {
        return entered;
    }
    if (const CyResult checked = check_out_result(out_result); checked != CY_RESULT_OK) {
        return checked;
    }
    if (query == CY_NAV_QUERY_NULL) {
        return report(CY_RESULT_NOT_FOUND, "nav_poll_path: the query is null");
    }
    Span<f32> points;
    if (const CyResult sized = point_buffer(out_points_xyz, capacity, points);
        sized != CY_RESULT_OK) {
        return sized;
    }
    CyNavPathResult result{};
    result.struct_size = static_cast<u32>(sizeof(CyNavPathResult));
    if (const CyResult answered = backend->poll_path(query, points, result);
        answered != CY_RESULT_OK) {
        return answered;
    }
    return finish_path(result, out_points_xyz, capacity, *out_result);
}

CyResult nav_cancel_path(CyEngine engine, CyNavQuery query) {
    NavigationBackend* backend = nullptr;
    if (const CyResult entered = enter(engine, kQueuePhases, "nav_cancel_path", backend);
        backend == nullptr) {
        return entered;
    }
    if (query == CY_NAV_QUERY_NULL) {
        return report(CY_RESULT_NOT_FOUND, "nav_cancel_path: the query is null");
    }
    if (const CyResult answered = backend->cancel_path(query); answered != CY_RESULT_OK) {
        return answered;
    }
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult nav_agent_configure(CyEngine engine, CyEntity entity, const CyNavAgentParams* params) {
    NavigationBackend* backend = nullptr;
    if (const CyResult entered = enter(engine, kConfigurePhases, "nav_agent_configure", backend);
        backend == nullptr) {
        return entered;
    }
    if (params == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "nav_agent_configure: params is null");
    }
    CyNavAgentParams whole{};
    if (!read_sized(*params, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "agent parameters have a malformed struct_size");
    }
    if (const CyResult checked = check_params(whole); checked != CY_RESULT_OK) {
        return checked;
    }
    bool structural = false;
    if (const CyResult answered = backend->agent_configure(entity, whole, structural);
        answered != CY_RESULT_OK) {
        return answered;
    }
    // Step 6: the agent component moved the entity to another archetype, so every borrow taken
    // before this call must read as stale.
    if (structural && engine->world != nullptr) {
        engine->world->bump_epoch();
    }
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult nav_agent_move_to(CyEngine engine, CyEntity entity, const float* target_xyz) {
    NavigationBackend* backend = nullptr;
    if (const CyResult entered = enter(engine, kOrderPhases, "nav_agent_move_to", backend);
        backend == nullptr) {
        return entered;
    }
    if (target_xyz == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "nav_agent_move_to: target is null");
    }
    if (!finite3(target_xyz)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "nav_agent_move_to: target must be finite");
    }
    if (const CyResult answered = backend->agent_move_to(entity, target_xyz);
        answered != CY_RESULT_OK) {
        return answered;
    }
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult nav_agent_stop(CyEngine engine, CyEntity entity) {
    NavigationBackend* backend = nullptr;
    if (const CyResult entered = enter(engine, kOrderPhases, "nav_agent_stop", backend);
        backend == nullptr) {
        return entered;
    }
    if (const CyResult answered = backend->agent_stop(entity); answered != CY_RESULT_OK) {
        return answered;
    }
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult nav_agent_state(CyEngine engine, CyEntity entity, CyNavAgentState* out_state) {
    NavigationBackend* backend = nullptr;
    if (const CyResult entered = enter(engine, kQueryPhases, "nav_agent_state", backend);
        backend == nullptr) {
        return entered;
    }
    if (out_state == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "nav_agent_state: out_state is null");
    }
    if (agreed_size<CyNavAgentState>(out_state->struct_size) == 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT, "out_state has a malformed struct_size");
    }
    CyNavAgentState state{};
    state.struct_size = static_cast<u32>(sizeof(CyNavAgentState));
    if (const CyResult answered = backend->agent_state(entity, state); answered != CY_RESULT_OK) {
        return answered;
    }
    (void)write_sized(*out_state, state);
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace cy::abi::game

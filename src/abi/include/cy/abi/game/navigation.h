// SPDX-License-Identifier: MIT
// cy/abi/game/navigation.h — the backend behind ABI 1.3's `nav_*` entries. `add-swift-game-api`.
//
// OWNER: implementer B (physics queries and navigation). See design.md in the change.
//
// Over cy/navigation/query.h (`find_path`, `straighten`, `PathQueue`) and cy/navigation/crowd.h
// (`Crowd`) through the `NavAgent` component (cy/navigation/components.h) and the navigation world
// registry that pairs each world id with its mesh and its deterministic query queue.
//
// DETERMINISM. `request_path` goes through `PathQueue`, whose completion latency is a fixed number
// of ticks — `ai-system`'s "async completion is deterministic" — so a poll in tick N answers the
// same on every run. Query ids are issued in submission order. Agent moves take effect in the fixed
// step's navigation update, never immediately, so the order in which a tick's scripts issue them
// cannot change the outcome.
//
// Points are written as xyz triples. `out_result.point_count` is always the whole path's; the thunk
// turns `point_count > capacity` into BUFFER_TOO_SMALL. `poll_path` additionally must not consume a
// READY query whose path does not fit, so the caller can retry with a larger buffer.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::abi::game {

/// Path queries and crowd agents as ABI 1.3's `nav_*` entries see them, keyed by entity and by a
/// deterministic query id.
class NavigationBackend {
public:
    virtual ~NavigationBackend() = default;

    /// `nav_find_path`. `request` is whole; `out_points_xyz.size()` is a multiple of three.
    [[nodiscard]] virtual CyResult find_path(const CyNavPathRequest& request,
                                             Span<f32> out_points_xyz,
                                             CyNavPathResult& out_result) noexcept = 0;

    /// `nav_request_path`: never CY_NAV_QUERY_NULL on success.
    [[nodiscard]] virtual CyResult request_path(const CyNavPathRequest& request,
                                                CyNavQuery& out_query) noexcept = 0;

    /// `nav_poll_path`. A READY query whose path does not fit `out_points_xyz` is left READY and
    /// nothing is written; one that fits is written and consumed. NOT_FOUND for a consumed or
    /// unknown query.
    [[nodiscard]] virtual CyResult poll_path(CyNavQuery query, Span<f32> out_points_xyz,
                                             CyNavPathResult& out_result) noexcept = 0;

    /// `nav_cancel_path`: NOT_FOUND once consumed.
    [[nodiscard]] virtual CyResult cancel_path(CyNavQuery query) noexcept = 0;

    /// `nav_agent_configure`. `out_structural` is set when the call added the agent component, so
    /// the thunk bumps the world's epoch exactly when storage moved.
    [[nodiscard]] virtual CyResult agent_configure(CyEntity entity, const CyNavAgentParams& params,
                                                   bool& out_structural) noexcept = 0;

    /// `nav_agent_move_to`: `target_xyz` is three floats. NOT_FOUND when not an agent.
    [[nodiscard]] virtual CyResult agent_move_to(CyEntity entity,
                                                 const f32* target_xyz) noexcept = 0;

    /// `nav_agent_stop`.
    [[nodiscard]] virtual CyResult agent_stop(CyEntity entity) noexcept = 0;

    /// `nav_agent_state`: `out_state` arrives zeroed with `struct_size == sizeof`.
    [[nodiscard]] virtual CyResult agent_state(CyEntity entity,
                                               CyNavAgentState& out_state) const noexcept = 0;
};

}  // namespace cy::abi::game

// SPDX-License-Identifier: MIT
// cy/abi/game/physics.h — the backend behind ABI 1.3's `physics_*` entries. `add-swift-game-api`.
//
// OWNER: implementer B (physics queries and navigation). See design.md in the change.
//
// Over `cy::physics::PhysicsServer`'s const queries (cy/servers/physics/queries.h): `raycast`,
// `raycast_all`, `shape_cast` and `overlap`. The adapter maps `CyQueryFilter` to `QueryFilter`
// (entities to be ignored become their bodies), a `CyShape` to a cached `ShapeHandle`, and a hit's
// `UserData` back to the entity it carries.
//
// THE ORDER IS PART OF THE CONTRACT. Every multi-result answer is sorted by distance, then entity
// value, then body creation order (overlap: entity value only, each entity once), so the same world
// gives the same list in a fixed step on every run. `total` is the whole count even when `out` is
// shorter; the thunk turns `total > out.size()` into BUFFER_TOO_SMALL.
//
// All methods are const and safe from a job worker. A query during the physics step is UNAVAILABLE
// (the server's own `reject_query_during_step`).

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::abi::game {

/// The physics server's scene queries as ABI 1.3's `physics_*` entries see them, with entities in
/// place of bodies and a total order on every list.
class PhysicsQueryBackend {
public:
    virtual ~PhysicsQueryBackend() = default;

    /// `physics_raycast`. `filter` is whole (the thunk substitutes the default for a null one).
    [[nodiscard]] virtual CyResult raycast(const CyRay& ray, const CyQueryFilter& filter,
                                           CyPhysicsHit& out_hit,
                                           bool& out_has_hit) const noexcept = 0;

    /// `physics_raycast_all`: writes `min(total, out.size())` hits, nearest first.
    [[nodiscard]] virtual CyResult raycast_all(const CyRay& ray, const CyQueryFilter& filter,
                                               Span<CyPhysicsHit> out,
                                               u32& out_total) const noexcept = 0;

    /// `physics_shape_cast`: `direction` is three floats.
    [[nodiscard]] virtual CyResult shape_cast(const CyShape& shape, const CyPose& start,
                                              const f32* direction, f32 max_distance,
                                              const CyQueryFilter& filter, CyPhysicsHit& out_hit,
                                              bool& out_has_hit) const noexcept = 0;

    /// `physics_overlap`: writes `min(total, out.size())` entities in ascending entity order.
    [[nodiscard]] virtual CyResult overlap(const CyShape& shape, const CyPose& pose,
                                           const CyQueryFilter& filter, Span<CyEntity> out,
                                           u32& out_total) const noexcept = 0;
};

}  // namespace cy::abi::game

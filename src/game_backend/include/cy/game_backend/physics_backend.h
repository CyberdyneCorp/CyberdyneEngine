// SPDX-License-Identifier: MIT
// cy/game_backend/physics_backend.h — the `physics` adapter behind ABI 1.3. `add-swift-game-api`.
//
// OWNER: implementer B. The contract is cy/abi/game/physics.h and design.md; this is the half that
// knows the physics server.
//
// WHAT IT ADDS TO THE SERVER'S OWN QUERIES:
//
//   * ENTITIES IN PLACE OF BODIES. A hit's `UserData` is the entity bits the ECS bridge stored in
//   it
//     (cy/physics/bridge.h), so it is handed back as the `CyEntity` unchanged. The other direction
//     — an ignore list of entities becoming bodies to skip — needs the embedder's entity-to-body
//     map, which is what `EntityBodies` is; this module cannot link the bridge that owns it.
//   * A TOTAL ORDER. The server sorts by distance only. Every list here is sorted by distance, then
//     entity value, then body handle (slot index, then generation) — the handle order is the
//     server's allocation order, which is the same on every run of the same world, so a fixed step
//     gets the same list every time. Overlaps are sorted by entity value, each entity once, and a
//     body that carries no entity is not reported. `physics_raycast`'s single hit is the first of
//     that order, not whatever the backend found first among equals.
//   * THE WHOLE COUNT. `PhysicsServer::raycast_all` and `overlap` write at most `out.size()`; the
//     adapter collects into its own scratch, growing it until the server stops filling it, so the
//     total the sizing pattern needs is exact.
//   * UNAVAILABLE DURING THE STEP IN EVERY BUILD. The server's `reject_query_during_step` is a
//     development-build diagnostic; the ABI promises the refusal everywhere, so the adapter checks
//     `stepping()` itself.
//   * SHAPES FROM `CyShape`. Sweeps and overlaps need a `ShapeHandle`. Shapes are created on first
//     use, cached by their `CyShape` bytes (the server shares identical shapes anyway, but creating
//     one is a mutating call), and destroyed with the adapter. The cache is guarded by a mutex, so
//     concurrent queries are safe; a query whose shape is NEW creates it, and `create_shape` is not
//     safe against a concurrent query on every backend — so the first use of each distinct shape
//     belongs on the game thread, or in `prewarm()` at load.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/abi/game/physics.h>
#include <cy/abi/host.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/physics/handles.h>

#include <mutex>

namespace cy::physics {
class PhysicsServer;
}  // namespace cy::physics

namespace cy::game_backend {

/// The embedder's entity-to-body map, for ignore lists. `cy::physics::PhysicsBridge::body_of` is
/// the usual implementation. Called from query threads, so it must be const and thread-safe.
class EntityBodies {
public:
    virtual ~EntityBodies() = default;
    /// The body `entity` owns, or a null handle when it has none.
    [[nodiscard]] virtual physics::BodyHandle body_of(CyEntity entity) const noexcept = 0;
};

/// Implements `cy::abi::game::PhysicsQueryBackend`.
class PhysicsQueryAdapter final : public abi::game::PhysicsQueryBackend {
public:
    /// Queries `world` on `server`; `bodies` resolves ignore lists. All three are borrowed and must
    /// outlive the adapter, which destroys the shapes it created in its destructor.
    PhysicsQueryAdapter(Allocator& allocator, physics::PhysicsServer& server,
                        physics::WorldHandle world, const EntityBodies& bodies) noexcept;
    ~PhysicsQueryAdapter() override;

    PhysicsQueryAdapter(const PhysicsQueryAdapter&) = delete;
    PhysicsQueryAdapter& operator=(const PhysicsQueryAdapter&) = delete;
    PhysicsQueryAdapter(PhysicsQueryAdapter&&) = delete;
    PhysicsQueryAdapter& operator=(PhysicsQueryAdapter&&) = delete;

    /// Create `shape`'s server shape now, so no query has to. For a game's load step.
    [[nodiscard]] CyResult prewarm(const CyShape& shape) noexcept;

    /// Shapes created so far. A test's handle on "a second identical query is a cache hit".
    [[nodiscard]] u32 cached_shapes() const noexcept;

    CyResult raycast(const CyRay& ray, const CyQueryFilter& filter, CyPhysicsHit& out_hit,
                     bool& out_has_hit) const noexcept override;
    CyResult raycast_all(const CyRay& ray, const CyQueryFilter& filter, Span<CyPhysicsHit> out,
                         u32& out_total) const noexcept override;
    CyResult shape_cast(const CyShape& shape, const CyPose& start, const f32* direction,
                        f32 max_distance, const CyQueryFilter& filter, CyPhysicsHit& out_hit,
                        bool& out_has_hit) const noexcept override;
    CyResult overlap(const CyShape& shape, const CyPose& pose, const CyQueryFilter& filter,
                     Span<CyEntity> out, u32& out_total) const noexcept override;

private:
    struct CachedShape {
        CyShape key{};
        physics::ShapeHandle handle;
    };

    [[nodiscard]] CyResult shape_for(const CyShape& shape,
                                     physics::ShapeHandle& out) const noexcept;
    [[nodiscard]] CyResult check_available() const noexcept;

    Allocator* allocator_;
    physics::PhysicsServer* server_;
    physics::WorldHandle world_;
    const EntityBodies* bodies_;
    mutable std::mutex shapes_mutex_;
    mutable Array<CachedShape> shapes_;
};

/// Bind `adapter` as `host`'s physics backend (`host.game.physics`), or unbind with null. The one
/// place an embedder wires the physics service; the adapter must outlive the binding.
void bind(cy::abi::Host& host, PhysicsQueryAdapter* adapter) noexcept;

/// ABI 1.5's rigid-body writes over `PhysicsServer`: `physics_apply_force`,
/// `physics_apply_impulse`, `physics_apply_torque`, `physics_set_velocity` and
/// `physics_get_velocity`.
///
/// WHAT IT ADDS TO THE SERVER'S OWN CALLS:
///
///   * ENTITIES IN PLACE OF BODIES, through the embedder's `EntityBodies` — the same map the query
///     adapter uses for ignore lists.
///   * A REFUSAL WHERE THE SERVER IS SILENT. `add_force` and friends on a static or kinematic body
///   are
///     a no-op in the server; a game that pushes a body that cannot move has a bug, so the adapter
///     answers INVALID_ARGUMENT instead. A velocity may be set on a kinematic body (that is how one
///     is driven) but not on a static one.
///   * WAKING. A force or torque wakes a sleeping body, as an impulse already does, so a script's
///     push is never swallowed by the sleep threshold.
///   * UNAVAILABLE DURING THE STEP, in every build — the bodies are mid-solve.
///
/// Not thread-safe: the writes are simulation, made from the game thread in N or F.
class PhysicsBodyAdapter final : public abi::game::PhysicsBodyBackend {
public:
    /// Writes to bodies on `server`; `bodies` maps entities to them. Both are borrowed.
    PhysicsBodyAdapter(physics::PhysicsServer& server, const EntityBodies& bodies) noexcept
        : server_(&server), bodies_(&bodies) {}

    CyResult apply_force(CyEntity entity, const f32* force) noexcept override;
    CyResult apply_impulse(CyEntity entity, const f32* impulse, const f32* point) noexcept override;
    CyResult apply_torque(CyEntity entity, const f32* torque) noexcept override;
    CyResult set_velocity(CyEntity entity, const f32* linear, const f32* angular) noexcept override;
    CyResult velocity(CyEntity entity, f32* out_linear, f32* out_angular) const noexcept override;

private:
    /// The entity's body, checked available and of a motion type `dynamic_only` allows.
    [[nodiscard]] CyResult resolve(CyEntity entity, bool dynamic_only,
                                   physics::BodyHandle& out) const noexcept;

    physics::PhysicsServer* server_;
    const EntityBodies* bodies_;
};

/// Bind `adapter` as `host.game.bodies`, or unbind with null. Named rather than another `bind`
/// overload so that `bind(host, nullptr)` for the query adapter stays unambiguous.
void bind_bodies(cy::abi::Host& host, PhysicsBodyAdapter* adapter) noexcept;

}  // namespace cy::game_backend

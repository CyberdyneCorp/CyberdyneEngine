// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_UNITS_H
#define CY_SAMPLE_RTS_API_UNITS_H
// units.h — the two pieces of engine plumbing a navigation agent needs in this host.
//
// Neither decides anything about a unit. They make "an entity Swift configured as a navigation
// agent" a thing that has a place in the scene and a body in the physics world, whatever the game
// does with it:
//
//   * `NodeMotion` is the navigation adapter's `AgentMotion`: an agent's position is its scene
//     node's translation, and the adapter's step moves the node. The agent's first position is
//     therefore where the prefab was spawned, not the origin.
//   * `UnitBodies` gives every navigation agent a kinematic capsule on collision layer 1, sized
//     from the agent's own radius and height (which Swift chose), carrying the entity in its user
//     data so a physics hit names the unit. It follows the node each tick and goes away with the
//     entity. It is also the physics adapter's `EntityBodies`, for ignore lists.
//
// A game with character controllers would bind those instead. This sample's units only walk.

#include <cy/abi/cy_abi.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/query.h>
#include <cy/game_backend/navigation_backend.h>
#include <cy/game_backend/physics_backend.h>
#include <cy/navigation/components.h>
#include <cy/scene/tree.h>
#include <cy/servers/physics/server.h>

namespace sample::rts {

/// Agent positions are scene node translations. Units are spawned without a parent, so a node's
/// local translation is its world translation.
class NodeMotion final : public cy::game_backend::AgentMotion {
public:
    explicit NodeMotion(cy::scene::SceneTree& tree) noexcept : tree_(&tree) {}

    [[nodiscard]] bool position(CyEntity entity, cy::Vec3& out) const noexcept override;
    [[nodiscard]] cy::Vec3 drive(CyEntity entity, cy::Vec3 from, cy::Vec3 velocity,
                                 cy::f32 dt) noexcept override;

private:
    cy::scene::SceneTree* tree_;
};

/// A kinematic capsule per navigation agent.
class UnitBodies final : public cy::game_backend::EntityBodies {
public:
    UnitBodies(cy::Allocator& allocator, cy::ecs::World& world,
               const cy::navigation::NavComponents& components, cy::physics::PhysicsServer& server,
               cy::physics::WorldHandle physics_world) noexcept;
    ~UnitBodies() override;

    UnitBodies(const UnitBodies&) = delete;
    UnitBodies& operator=(const UnitBodies&) = delete;
    UnitBodies(UnitBodies&&) = delete;
    UnitBodies& operator=(UnitBodies&&) = delete;

    /// Create bodies for new agents, move every body to its agent, and drop the bodies of dead
    /// ones. Once per fixed tick, after the navigation update and before the physics step.
    [[nodiscard]] cy::Status sync() noexcept;

    /// Destroy every body. Idempotent; the destructor calls it.
    void clear() noexcept;

    /// The agents that have a body, in entity order.
    [[nodiscard]] cy::u32 count() const noexcept { return static_cast<cy::u32>(units_.size()); }
    [[nodiscard]] CyEntity entity(cy::u32 index) const noexcept { return units_[index].entity; }

    [[nodiscard]] cy::physics::BodyHandle body_of(CyEntity entity) const noexcept override;

private:
    struct Unit {
        CyEntity entity = CY_ENTITY_NULL;
        cy::physics::BodyHandle body;
        cy::physics::ShapeHandle shape;
        bool seen = false;
    };

    [[nodiscard]] Unit* find(CyEntity entity) noexcept;
    [[nodiscard]] cy::Status follow(CyEntity entity,
                                    const cy::navigation::NavAgent& agent) noexcept;
    [[nodiscard]] cy::Status create(CyEntity entity,
                                    const cy::navigation::NavAgent& agent) noexcept;
    void release(Unit& unit) noexcept;
    void sweep() noexcept;

    cy::navigation::NavComponents components_;
    cy::physics::PhysicsServer* server_;
    cy::physics::WorldHandle physics_world_;
    cy::ecs::Query agents_;
    /// Sorted by entity bits.
    cy::Array<Unit> units_;
};

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_UNITS_H

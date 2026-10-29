// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_FIRST_PERSON_BODIES_H
#define CY_SAMPLE_FIRST_PERSON_BODIES_H
// bodies.h — the arena in the physics world, and the one entity-to-body map the ABI's physics
// adapters see.
//
// The static boxes are bodies on the world layer; each prop is a dynamic box on the prop layer,
// owned by its `/Arena/Props/<name>` node, so a shot names it and an impulse moves it; each target
// is a kinematic box on the target layer, owned by its `/Arena/Targets/<name>` node and moved by
// the AI (targets.h). A body carries its entity in its user data, so a raycast hit names what it
// hit. The player's character is the `CharacterAdapter`'s, and this map answers for it too: a
// query's ignore list may name the player, a prop or a target.

#include <cy/abi/cy_abi.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/transform.h>
#include <cy/game_backend/character_backend.h>
#include <cy/game_backend/physics_backend.h>
#include <cy/servers/physics/server.h>

#include "arena.h"

namespace sample::fps {

class ArenaBodies final : public cy::game_backend::EntityBodies {
public:
    ArenaBodies(cy::physics::PhysicsServer& server, cy::physics::WorldHandle world) noexcept;
    ~ArenaBodies() override;

    ArenaBodies(const ArenaBodies&) = delete;
    ArenaBodies& operator=(const ArenaBodies&) = delete;
    ArenaBodies(ArenaBodies&&) = delete;
    ArenaBodies& operator=(ArenaBodies&&) = delete;

    /// The static boxes, owned by `arena` — a query names the arena when it hits a wall, and an
    /// overlap, which reports entities, sees one — the props owned by `props` and the targets
    /// owned by `targets`, in arena.h's orders.
    [[nodiscard]] cy::Status build(CyEntity arena, const CyEntity (&props)[kPropCount],
                                   const CyEntity (&targets)[kTargetCount]) noexcept;
    /// The characters the game makes, which the map answers for after the level's own bodies.
    void set_characters(const cy::game_backend::CharacterAdapter* characters) noexcept {
        characters_ = characters;
    }
    /// Destroy every body and shape. Idempotent; the destructor calls it.
    void clear() noexcept;

    /// A prop's placement, from its body.
    [[nodiscard]] cy::Transform prop(cy::u32 index) const noexcept;
    [[nodiscard]] cy::physics::BodyHandle prop_body(cy::u32 index) const noexcept {
        return props_[index].body;
    }
    /// Move target `index`'s body to `placement` for the next step.
    [[nodiscard]] cy::Status place_target(cy::u32 index, const cy::Transform& placement) noexcept;
    /// A fallen target is shot through and walked through: its body leaves every layer a shot or
    /// a character uses.
    [[nodiscard]] cy::Status fell(cy::u32 index) noexcept;
    /// A target back on its feet is shot and walked into again: its body back on the targets'
    /// layer.
    [[nodiscard]] cy::Status rose(cy::u32 index) noexcept;
    [[nodiscard]] cy::physics::BodyHandle target_body(cy::u32 index) const noexcept {
        return targets_[index].body;
    }

    [[nodiscard]] cy::physics::BodyHandle body_of(CyEntity entity) const noexcept override;

private:
    struct Owned {
        CyEntity entity = CY_ENTITY_NULL;
        cy::physics::BodyHandle body;
        cy::physics::ShapeHandle shape;
    };

    [[nodiscard]] cy::Expected<Owned, cy::Error> make(CyEntity entity, cy::Vec3 centre,
                                                      cy::Vec3 half, cy::physics::MotionType motion,
                                                      cy::u32 layer, cy::f32 mass) noexcept;

    cy::physics::PhysicsServer* server_;
    cy::physics::WorldHandle world_;
    Owned statics_[32] = {};
    cy::u32 static_count_ = 0;
    Owned props_[kPropCount] = {};
    Owned targets_[kTargetCount] = {};
    const cy::game_backend::CharacterAdapter* characters_ = nullptr;
};

}  // namespace sample::fps

#endif  // CY_SAMPLE_FIRST_PERSON_BODIES_H

// SPDX-License-Identifier: MIT
// cy/game_backend/character_backend.h — the character adapter behind ABI 1.5. `add-swift-m12-gaps`.
//
// The contract is cy/abi/game/character.h; this is the half that knows the physics server. One
// `cy::physics::CharacterController` per entity — engine code over the server's shape cast and
// overlap, so a character walks the same over every backend (character.h says why).
//
// WHAT IT ADDS:
//
//   * ENTITIES. The controller's kinematic body carries the entity in its user data, so a ray cast
//     hits the character and `CyPhysicsHit.entity` names it, and the adapter is also an
//     `EntityBodies` so a query's ignore list can name a character.
//   * THE DESCRIPTION'S DEFAULTS. A zero field of `CyCharacterDesc` is `CharacterDescription`'s own
//     default, so a zeroed struct is a valid character.
//   * A TOTAL ORDER where one is needed: characters are kept sorted by entity, so `count()` and
//     `entity(i)` list them the same way on every run.
//   * UNAVAILABLE DURING THE STEP, in every build: a move sweeps the world, which is mid-solve.
//
// Not thread-safe: characters are simulation, created and moved from the game thread in N or F.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/abi/game/character.h>
#include <cy/abi/host.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/game_backend/physics_backend.h>
#include <cy/servers/physics/character.h>

namespace cy::game_backend {

/// `CharacterBackend` over `cy::physics::CharacterController`: a Swift character is a capsule
/// controller on the physics server, whose body carries the entity so a query names it.
class CharacterAdapter final : public abi::game::CharacterBackend, public EntityBodies {
public:
    /// Characters live in `world` on `server`; both are borrowed and outlive the adapter, which
    /// destroys its controllers (and their bodies) in its destructor.
    CharacterAdapter(Allocator& allocator, physics::PhysicsServer& server,
                     physics::WorldHandle world) noexcept;
    ~CharacterAdapter() override;

    CharacterAdapter(const CharacterAdapter&) = delete;
    CharacterAdapter& operator=(const CharacterAdapter&) = delete;
    CharacterAdapter(CharacterAdapter&&) = delete;
    CharacterAdapter& operator=(CharacterAdapter&&) = delete;

    CyResult create(CyEntity entity, const CyCharacterDesc& desc) noexcept override;
    CyResult destroy(CyEntity entity) noexcept override;
    CyResult move(CyEntity entity, const CyCharacterInput& input, f32 delta) noexcept override;
    CyResult state(CyEntity entity, CyCharacterState& out) const noexcept override;

    /// `EntityBodies`: the character's kinematic body, or a null handle.
    [[nodiscard]] physics::BodyHandle body_of(CyEntity entity) const noexcept override;

    /// The characters, in entity order.
    [[nodiscard]] u32 count() const noexcept { return static_cast<u32>(characters_.size()); }
    [[nodiscard]] CyEntity entity(u32 index) const noexcept { return characters_[index].entity; }
    /// The controller behind `entity`, or null. For a host that moves a scene node with it.
    [[nodiscard]] const physics::CharacterController* controller(CyEntity entity) const noexcept;

private:
    struct Character {
        CyEntity entity = CY_ENTITY_NULL;
        physics::CharacterController* controller = nullptr;
    };

    /// The index of `entity`, or of where it would be inserted, in the sorted array.
    [[nodiscard]] usize lower_bound(CyEntity entity) const noexcept;
    [[nodiscard]] const Character* find(CyEntity entity) const noexcept;
    void release(Character& character) noexcept;

    Allocator* allocator_;
    physics::PhysicsServer* server_;
    physics::WorldHandle world_;
    /// Sorted by entity bits.
    Array<Character> characters_;
};

/// Bind `adapter` as `host.game.characters`, or unbind with null. Named, like `bind_bodies`, so a
/// translation unit that sees several adapters can still unbind one with `nullptr`.
void bind_characters(cy::abi::Host& host, CharacterAdapter* adapter) noexcept;

}  // namespace cy::game_backend

// SPDX-License-Identifier: MIT
// cy/game_backend/animation_backend.h — the animation adapter behind ABI 1.7. Issue #76 stage 4.
//
// The contract is cy/abi/game/animation.h; this is the half that knows `cy::animation`. One
// `AnimationSystem` — the one a host already installed in its schedule — reached by entity:
//
//   * RIGS BY NAME. A host registers each rig it loaded (`add_rig`) under the name a script asks
//     for; `animation_attach` writes the entity's `Animator` and syncs the system at once, so the
//     instance exists when the call returns and can be played in the same callback.
//   * NAMES AS HASHES. States and events cross as `CY_NAME_HASH` of their names, which a script
//     computes from its own strings (`cy::abi::game::name_hash`).
//   * EVENTS ONCE PER FRAME. `begin_frame` snapshots the events of the ticks the system ran since
//     the previous frame, and is the frame's answer to every `animation_events` call; a frame that
//     ran no tick answers nothing, so no event is delivered twice.
//   * ROOT MOTION INTO A CHARACTER. `CY_ROOT_MOTION_CHARACTER` is the system's `Controller` mode
//     consumed here: `update`, once per tick after the system's advance, takes each such entity's
//     accumulated delta, turns it into the world by the entity's placement, and moves its character
//     controller by it (ABI 1.5's `CharacterBackend`) at the tick's length.
//
// Not thread-safe: animators are simulation, attached and played from the game thread in N or F.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/abi/game/animation.h>
#include <cy/abi/game/character.h>
#include <cy/abi/host.h>
#include <cy/animation/animation_system.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/world.h>

namespace cy::scene {
class SceneTree;
}

namespace cy::game_backend {

/// `AnimationBackend` over `cy::animation::AnimationSystem`.
class AnimationAdapter final : public abi::game::AnimationBackend {
public:
    /// `system` animates `world`, whose `Animator` component is `animator`. `tree` places a joint
    /// in the world (`animation_joint_pose`) and turns root motion for a character; it may be null,
    /// and those answers are then in the entity's own space. All are borrowed and outlive this.
    AnimationAdapter(Allocator& allocator, ecs::World& world, animation::AnimationSystem& system,
                     ecs::ComponentTypeId animator, scene::SceneTree* tree = nullptr) noexcept;

    /// Register `rig` under `name`, the string a script attaches by. Refused when the name is
    /// already registered or the system has no such rig.
    [[nodiscard]] Status add_rig(const char* name, animation::RigId rig) noexcept;
    /// The character controllers CY_ROOT_MOTION_CHARACTER feeds. Null refuses that mode.
    void bind_characters(abi::game::CharacterBackend* characters) noexcept {
        characters_ = characters;
    }

    /// Once per frame, before the frame's scripts read events.
    [[nodiscard]] Status begin_frame() noexcept;
    /// Once per tick, after the system's advance: CY_ROOT_MOTION_CHARACTER's moves.
    [[nodiscard]] Status update(f32 tick_seconds) noexcept;

    CyResult attach(CyEntity entity, const CyAnimatorDesc& desc) noexcept override;
    CyResult detach(CyEntity entity) noexcept override;
    CyResult play(CyEntity entity, const char* state, f32 seconds) noexcept override;
    CyResult stop(CyEntity entity, f32 seconds) noexcept override;
    CyResult set_parameter(CyEntity entity, const char* parameter, f32 value) noexcept override;
    CyResult fire_trigger(CyEntity entity, const char* parameter) noexcept override;
    CyResult parameter(CyEntity entity, const char* parameter, f32& out) const noexcept override;
    CyResult state(CyEntity entity, CyAnimatorState& out) const noexcept override;
    [[nodiscard]] Span<const CyAnimationEvent> events() const noexcept override {
        return events_.span();
    }
    CyResult root_motion(CyEntity entity, CyRootMotion& out) const noexcept override;
    CyResult take_root_motion(CyEntity entity, CyRootMotion& out) noexcept override;
    CyResult set_root_motion(CyEntity entity, CyRootMotionMode mode) noexcept override;
    CyResult joint_pose(CyEntity entity, const char* joint, CyPose& out) const noexcept override;

    /// The entities whose root motion drives a character, in entity order.
    [[nodiscard]] Span<const CyEntity> character_driven() const noexcept { return driven_.span(); }

private:
    struct Rig {
        Name name;
        animation::RigId id = animation::kNoRig;
    };

    [[nodiscard]] const animation::Animator* animator_of(CyEntity entity) const noexcept;
    [[nodiscard]] static CyResult missing() noexcept;
    [[nodiscard]] CyResult write_mode(CyEntity entity, CyRootMotionMode mode) noexcept;
    [[nodiscard]] Transform placement_of(CyEntity entity) const noexcept;
    void forget_driven(CyEntity entity) noexcept;

    ecs::World* world_;
    animation::AnimationSystem* system_;
    ecs::ComponentTypeId animator_;
    scene::SceneTree* tree_;
    abi::game::CharacterBackend* characters_ = nullptr;
    Array<Rig> rigs_;
    /// Sorted by entity bits.
    Array<CyEntity> driven_;
    Array<CyAnimationEvent> events_;
    u64 ticks_seen_ = 0;
};

/// Bind `adapter` as `host.game.animation`, or unbind with null.
void bind_animation(cy::abi::Host& host, AnimationAdapter* adapter) noexcept;

}  // namespace cy::game_backend

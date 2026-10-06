// SPDX-License-Identifier: MIT
// cy/abi/game/animation.h — the backend behind ABI 1.7's `animation_*` entries. Issue #76 stage 4.
//
// Over `cy::animation::AnimationSystem`. The thunk has checked the phase, the pointers, the numbers
// and the enumerators, and normalised every sized struct; the backend owns the rig names, maps
// entities to animators, and answers the domain facts — an unknown rig, state, parameter or joint,
// an entity with no animator.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::abi::game {

/// What `animation_*` reaches on the engine side. A host installs an implementation
/// (`cy::game_backend::AnimationAdapter`); without one the entries return `CY_RESULT_UNAVAILABLE`.
class AnimationBackend {
public:
    virtual ~AnimationBackend() = default;

    /// `animation_attach`. ALREADY_EXISTS when `entity` has an animator, NOT_FOUND for an unknown
    /// rig. `desc` is whole and its `rig` is not null.
    [[nodiscard]] virtual CyResult attach(CyEntity entity, const CyAnimatorDesc& desc) noexcept = 0;
    [[nodiscard]] virtual CyResult detach(CyEntity entity) noexcept = 0;
    /// `animation_play`: `seconds` is finite and not negative.
    [[nodiscard]] virtual CyResult play(CyEntity entity, const char* state,
                                        f32 seconds) noexcept = 0;
    [[nodiscard]] virtual CyResult stop(CyEntity entity, f32 seconds) noexcept = 0;
    /// `animation_set_float` and `animation_set_bool`, the latter as 1 or 0.
    [[nodiscard]] virtual CyResult set_parameter(CyEntity entity, const char* parameter,
                                                 f32 value) noexcept = 0;
    [[nodiscard]] virtual CyResult fire_trigger(CyEntity entity,
                                                const char* parameter) noexcept = 0;
    [[nodiscard]] virtual CyResult parameter(CyEntity entity, const char* parameter,
                                             f32& out) const noexcept = 0;
    /// `out` is whole; the thunk writes the caller's prefix.
    [[nodiscard]] virtual CyResult state(CyEntity entity, CyAnimatorState& out) const noexcept = 0;
    /// The events of the ticks since the previous frame — once per frame, empty in a frame that ran
    /// no tick — in the order `animation_events` documents.
    [[nodiscard]] virtual Span<const CyAnimationEvent> events() const noexcept = 0;
    [[nodiscard]] virtual CyResult root_motion(CyEntity entity,
                                               CyRootMotion& out) const noexcept = 0;
    [[nodiscard]] virtual CyResult take_root_motion(CyEntity entity,
                                                    CyRootMotion& out) noexcept = 0;
    [[nodiscard]] virtual CyResult set_root_motion(CyEntity entity,
                                                   CyRootMotionMode mode) noexcept = 0;
    [[nodiscard]] virtual CyResult joint_pose(CyEntity entity, const char* joint,
                                              CyPose& out) const noexcept = 0;
};

}  // namespace cy::abi::game

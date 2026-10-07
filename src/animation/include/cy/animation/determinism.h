// SPDX-License-Identifier: MIT
#pragma once
// What animation root motion guarantees. openspec/changes/add-deterministic-math, design §9.2 and
// task 7.3.
//
// Root motion is accumulated on a deterministic CPU path (evaluate.h), in `f32`: it reproduces on
// one architecture and not across two. A `CrossPlatform` or `Lockstep` session that moves units by
// root motion authoritatively is refused naming `root-motion`; such a session moves them with the
// fixed-point kinematic mover (`cy::movement`) and plays root motion as presentation.

#include <cy/core/determinism/profile.h>

namespace cy::animation {

/// The name root motion declares itself under.
inline constexpr const char* kRootMotionSubsystem = "root-motion";

/// `SamePlatform`.
[[nodiscard]] constexpr determinism::SubsystemDeterminism root_motion_determinism(
    bool authoritative = true) noexcept {
    return determinism::SubsystemDeterminism{
        kRootMotionSubsystem, determinism::DeterminismProfile::SamePlatform, authoritative};
}

}  // namespace cy::animation

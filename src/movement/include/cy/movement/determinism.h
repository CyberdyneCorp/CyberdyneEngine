// SPDX-License-Identifier: MIT
#pragma once
// What the kinematic mover guarantees. openspec/changes/add-deterministic-math, design §8 and
// §12.1.
//
// `Lockstep`: every quantity the mover holds or computes is a `Fixed`, every decision is an exact
// integer comparison with an index tie-break, and its state at tick N is a function of its state at
// tick 0 and the desired velocities it was given — which a lockstep session derives from commands
// alone. The cross-leg comparison (`detmath-movement-digest` and `detmath-lockstep-digest`) is the
// measurement behind the declaration.

#include <cy/core/determinism/profile.h>

namespace cy::movement {

/// The name the mover declares itself under.
inline constexpr const char* kMovementSubsystem = "movement";

[[nodiscard]] constexpr determinism::SubsystemDeterminism movement_determinism() noexcept {
    return determinism::SubsystemDeterminism{kMovementSubsystem,
                                             determinism::DeterminismProfile::Lockstep, true};
}

}  // namespace cy::movement

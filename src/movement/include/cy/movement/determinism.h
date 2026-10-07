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

/// The mover's declaration for `DeterminismConfiguration::declare`: authoritative, `Lockstep`.
[[nodiscard]] constexpr determinism::SubsystemDeterminism movement_determinism() noexcept {
    return determinism::SubsystemDeterminism{kMovementSubsystem,
                                             determinism::DeterminismProfile::Lockstep, true};
}

/// The build half of the profile check as the mover was compiled:
/// `BuildConfiguration::from_build()` evaluated inside `cy::movement`, whose sources carry the
/// contraction flag its profile declaration requires and the deterministic math module's
/// definition. What a session passes to `DeterminismConfiguration::require()` for the authoritative
/// code it runs, rather than what the calling translation unit happened to be compiled with.
[[nodiscard]] determinism::BuildConfiguration movement_build() noexcept;

}  // namespace cy::movement

// SPDX-License-Identifier: MIT
#pragma once
// What the ability system guarantees. openspec/changes/add-deterministic-math, design §9.2 and
// task 7.3.
//
// Attributes are `f32` (attributes.h), and modifier aggregation, effect magnitudes and costs are
// float arithmetic, so the ability system reproduces on one architecture and not across two. It
// says so EXPLICITLY rather than by omission: a `CrossPlatform` or `Lockstep` session that uses
// abilities authoritatively is refused by `DeterminismConfiguration::require()` naming `abilities`,
// before a tick runs, instead of desyncing hours later. Converting attributes to `Fixed` is
// follow-up work, and the day it lands this declaration is what changes.

#include <cy/core/determinism/profile.h>

namespace cy::gameplay::abilities {

/// The name the ability system declares itself under.
inline constexpr const char* kAbilitiesSubsystem = "abilities";

/// `SamePlatform`. `authoritative` false for a session that runs abilities only for presentation.
[[nodiscard]] constexpr determinism::SubsystemDeterminism abilities_determinism(
    bool authoritative = true) noexcept {
    return determinism::SubsystemDeterminism{
        kAbilitiesSubsystem, determinism::DeterminismProfile::SamePlatform, authoritative};
}

}  // namespace cy::gameplay::abilities

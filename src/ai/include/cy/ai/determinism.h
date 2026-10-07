// SPDX-License-Identifier: MIT
#pragma once
// What AI utility scoring guarantees. openspec/changes/add-deterministic-math, design §9.2 and
// task 7.3.
//
// Utility scores, perception weights and environment-query scores are `f32`, so a decision
// reproduces on one architecture and not across two. The declaration says so explicitly: a
// `CrossPlatform` or `Lockstep` session that lets AI decide authoritatively is refused naming
// `ai-utility`. An AI that only issues commands through the command stream — whose payloads a
// lockstep session checks — and is run by one peer is not authoritative in this sense.

#include <cy/core/determinism/profile.h>

namespace cy::ai {

/// The name AI utility scoring declares itself under.
inline constexpr const char* kAiUtilitySubsystem = "ai-utility";

/// `SamePlatform`.
[[nodiscard]] constexpr determinism::SubsystemDeterminism ai_utility_determinism(
    bool authoritative = true) noexcept {
    return determinism::SubsystemDeterminism{
        kAiUtilitySubsystem, determinism::DeterminismProfile::SamePlatform, authoritative};
}

}  // namespace cy::ai

#pragma once
// Where AI meets navigation: the tier decides the movement strategy, and nothing else in this
// module knows how to move an agent. M8.b task 6.4.
//
// ================================================================================================
// AI DRIVES MOVEMENT THROUGH NAVIGATION, AND LOCOMOTION IS SOMEBODY ELSE'S
// ================================================================================================
//
// `ai-system` states both halves:
//
//   * "AI agents SHALL drive movement through the navigation system (see `navigation`) rather than
//     implementing their own pathfinding, and SHALL consume flow fields when their LOD tier
//     specifies it."
//   * "Locomotion — translating desired velocity into animation and physics movement — SHALL remain
//     the responsibility of the character controller and animation system, NOT the AI graph."
//
// So this header is one function and no state. It reads `AIAgent::tier` and writes
// `navigation::NavAgent::follows_field`, which is the whole of the tier's effect on movement: a
// `Full` agent issues its own path query and a `Reduced` one follows a shared field.
// `navigation::update_agents()` reads that flag and issues no query for a field follower, which is
// where the saving actually lands.
//
// IT IS A FREE FUNCTION OVER TWO COLUMNS rather than a member of anything, because `ai-system`
// requires agent work to run "as scheduled systems over queries, in bulk" — and because a class
// holding both an AI runtime and a navigation world would be a place for the two to start sharing
// state they have no business sharing.

#include <cy/ai/agent.h>
#include <cy/core/base/expected.h>
#include <cy/navigation/components.h>

namespace cy::ai {

/// What one pass changed.
struct LocomotionReport {
    u32 agents = 0;
    u32 following_field = 0;
    u32 pathing_individually = 0;
    u32 changed = 0;
};

/// Set each navigation agent's movement strategy from its AI tier.
///
/// `agents` and `movers` are parallel columns. `ai-system`'s scenario: "WHEN an agent is demoted to
/// `Reduced` THEN it SHALL follow a shared flow field rather than computing an individual path."
[[nodiscard]] Status apply_tier_to_movement(Span<const AIAgent> agents,
                                            Span<navigation::NavAgent> movers,
                                            LocomotionReport& report) noexcept;

/// Which strategy a tier implies, exposed so a caller that stores its agents differently does not
/// have to restate the table. `ai-system`'s AI LOD rows: `Full` pathfinds individually; `Reduced`
/// takes a flow field; `Minimal` and `Statistical` move at a group level, which is a field too.
[[nodiscard]] bool tier_follows_field(AiTier tier) noexcept;

}  // namespace cy::ai

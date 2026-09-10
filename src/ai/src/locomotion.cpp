// The tier's effect on movement. See cy/ai/locomotion.h.

#include <cy/ai/locomotion.h>

namespace cy::ai {

bool tier_follows_field(AiTier tier) noexcept {
    return tier != AiTier::Full;
}

Status apply_tier_to_movement(Span<const AIAgent> agents, Span<navigation::NavAgent> movers,
                              LocomotionReport& report) noexcept {
    report = LocomotionReport{};
    if (agents.size() != movers.size()) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "the AI agent column and the navigation agent column are not the same length"});
    }
    for (usize index = 0; index < agents.size(); ++index) {
        ++report.agents;
        const bool field = tier_follows_field(agents[index].tier);
        if (movers[index].follows_field != field) {
            movers[index].follows_field = field;
            ++report.changed;
            // A strategy change invalidates whatever the agent was doing: an agent that was
            // computing a path and is now on a field should not have that query applied to it, and
            // one that has just been promoted has no corridor of its own yet.
            movers[index].status = navigation::NavPathStatus::Idle;
        }
        report.following_field += field ? 1u : 0u;
        report.pathing_individually += field ? 0u : 1u;
    }
    return ok();
}

}  // namespace cy::ai

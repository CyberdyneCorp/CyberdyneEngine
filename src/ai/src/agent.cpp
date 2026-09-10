// Registering AI's components in a world, and the two tables that describe a tier. See
// cy/ai/agent.h.

#include <cy/ai/agent.h>

namespace cy::ai {
namespace {

template <class T>
[[nodiscard]] Status bind(World& world, const char* name, ComponentTypeId& out) noexcept {
    Expected<ComponentTypeId, Error> id = world.components().register_builtin(
        name, static_cast<u32>(sizeof(T)), static_cast<u32>(alignof(T)));
    if (!id) {
        return make_unexpected(id.error());
    }
    out = *id;
    return ok();
}

}  // namespace

const char* ai_tier_name(AiTier tier) noexcept {
    switch (tier) {
        case AiTier::Full:
            return "Full";
        case AiTier::Reduced:
            return "Reduced";
        case AiTier::Minimal:
            return "Minimal";
        case AiTier::Statistical:
            return "Statistical";
        case AiTier::Count:
            break;
    }
    return "unknown";
}

u32 tier_think_interval(AiTier tier) noexcept {
    // At sixty ticks a second: `Full` every tick, `Reduced` at 10 Hz, `Minimal` at 1 Hz, and
    // `Statistical` at a fifth of that — the rates `ai-system`'s tier table gives, read as the
    // SLOWEST each tier is allowed to be. The scheduler's rotation spreads agents inside the
    // interval; this number is the guarantee the "no starvation" case measures against.
    switch (tier) {
        case AiTier::Full:
            return 1;
        case AiTier::Reduced:
            return 6;
        case AiTier::Minimal:
            return 60;
        case AiTier::Statistical:
            return 300;
        case AiTier::Count:
            break;
    }
    return 1;
}

Expected<AiComponents, Error> AiComponents::register_all(World& world) noexcept {
    AiComponents ids;
    // The order is the id order and therefore the serialized descriptor table's order. Fixed
    // deliberately; see the header.
    if (Status bound = bind<AIAgent>(world, kAIAgentComponentName, ids.agent); !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<AIState>(world, kAIStateComponentName, ids.state); !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<Blackboard>(world, kBlackboardComponentName, ids.blackboard); !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<PerceptionSensors>(world, kPerceptionSensorsComponentName, ids.sensors);
        !bound) {
        return make_unexpected(bound.error());
    }
    return ids;
}

}  // namespace cy::ai

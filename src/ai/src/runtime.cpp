// The AI runtime: tiers, the schedule, the budget and the think loop. See cy/ai/runtime.h.

#include <cy/ai/runtime.h>

#include <cmath>

namespace cy::ai {
namespace {

[[nodiscard]] f32 nearest_observer_distance(Vec3 position, Span<const Vec3> observers) noexcept {
    if (observers.empty()) {
        return 0.0F;  // no observer means nothing is far away; a headless server is all `Full`
    }
    f32 best = math::kInfinity;
    for (const Vec3 observer : observers) {
        const Vec3 offset = observer - position;
        const f32 distance_sq =
            (offset.x * offset.x) + (offset.y * offset.y) + (offset.z * offset.z);
        best = std::fmin(best, distance_sq);
    }
    return std::sqrt(best);
}

/// The tier distance alone would give. Hysteresis is applied by the caller against the tier the
/// agent already has, because a threshold that moved with the agent's own state would not be a
/// function of distance at all.
[[nodiscard]] AiTier tier_for_distance(const TierPolicy& policy, f32 distance,
                                       f32 margin) noexcept {
    if (distance <= policy.full_distance + margin) {
        return AiTier::Full;
    }
    if (distance <= policy.reduced_distance + margin) {
        return AiTier::Reduced;
    }
    if (distance <= policy.minimal_distance + margin) {
        return AiTier::Minimal;
    }
    return AiTier::Statistical;
}

[[nodiscard]] AiTier better_of(AiTier a, AiTier b) noexcept {
    return (static_cast<u8>(a) <= static_cast<u8>(b)) ? a : b;
}

[[nodiscard]] u64 mix(u64 hash, u64 value) noexcept {
    hash ^= value + 0x9E3779B97F4A7C15ULL + (hash << 6U) + (hash >> 2U);
    return hash;
}

}  // namespace

const char* budget_mode_name(BudgetMode mode) noexcept {
    return (mode == BudgetMode::Adaptive) ? "Adaptive" : "Deterministic";
}

AiRuntime::AiRuntime(Allocator& allocator, const AiBudget& budget,
                     const TierPolicy& policy) noexcept
    : budget_(budget), policy_(policy), states_(allocator), history_(allocator) {}

Status AiRuntime::check_lockstep(bool lockstep_enabled) const noexcept {
    if (lockstep_enabled && budget_.mode == BudgetMode::Adaptive) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "the AI budget controller is in Adaptive mode, which varies the think schedule "
                  "with measured load and therefore forfeits deterministic replay and lockstep "
                  "networking. Select BudgetMode::Deterministic, or turn lockstep off."});
    }
    return ok();
}

Expected<u32, Error> AiRuntime::reserve_slots(const BehaviourProgram& program, u32 count) noexcept {
    // ONE-BASED. `kInvalidSlot` is zero so that a zero-initialised chunk is unassigned rather than
    // pointing at the first agent's state; agent.h carries the argument.
    const u32 first = static_cast<u32>(states_.size()) + 1u;
    if (Status reserved = states_.reserve(states_.size() + count); !reserved) {
        return make_unexpected(reserved.error());
    }
    for (u32 index = 0; index < count; ++index) {
        AgentState state(states_.allocator(), program);
        if (Status pushed = states_.push_back(std::move(state)); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    return first;
}

AgentState* AiRuntime::state(u32 slot) noexcept {
    return (slot != kInvalidSlot && slot <= states_.size()) ? &states_[slot - 1u] : nullptr;
}

bool AiRuntime::due(u32 tick, u32 index, AiTier tier) noexcept {
    const u32 interval = tier_think_interval(tier);
    if (interval <= 1) {
        return true;
    }
    // THE ROTATION. Each agent owns one residue class of the interval, chosen by its stable index,
    // so the load is spread and every agent's turn comes round exactly once per interval. It is a
    // function of the tick number and the agent's order and of nothing else — no clock, no measured
    // load, no queue.
    return (tick % interval) == (index % interval);
}

Status AiRuntime::update_tiers(Span<AIAgent> agents, Span<const Vec3> positions,
                               Span<const AIState> states, Span<const Vec3> observers,
                               TierReport& report) noexcept {
    report = TierReport{};
    if (positions.size() != agents.size() || states.size() != agents.size()) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the agent columns are not the same length"});
    }

    for (usize index = 0; index < agents.size(); ++index) {
        AIAgent& agent = agents[index];
        ++report.agents;
        const AiTier previous = agent.tier;
        const f32 distance = nearest_observer_distance(positions[index], observers);

        // Hysteresis: the margin only applies to KEEPING a tier, never to gaining one, so an agent
        // walking outward holds its tier a little longer and one walking inward is promoted at the
        // stated distance. Applying it both ways would make the thresholds themselves ambiguous.
        AiTier decided = tier_for_distance(policy_, distance, 0.0F);
        const AiTier with_margin = tier_for_distance(policy_, distance, policy_.hysteresis);
        if (static_cast<u8>(with_margin) < static_cast<u8>(decided) && with_margin == previous) {
            decided = previous;
        }

        // Importance and the gameplay-critical flag each hold a tier one step better.
        if (agent.importance > policy_.important_above && decided != AiTier::Full) {
            decided = static_cast<AiTier>(static_cast<u8>(decided) - 1u);
        }
        if (agent.critical) {
            decided = AiTier::Full;
        }
        // The pin is a FLOOR on quality: `pinned` is the worst tier this agent may be given.
        if (static_cast<u8>(decided) > static_cast<u8>(agent.pinned)) {
            decided = agent.pinned;
            ++report.pinned_held;
        }
        decided = better_of(decided, AiTier::Statistical);

        if (static_cast<u8>(decided) < static_cast<u8>(previous)) {
            ++report.promoted;
            // `ai-system`: "promotion SHALL reconstruct plausible individual state so an agent
            // entering `Full` does not visibly snap into a different behaviour." The reconstruction
            // is: an agent that was not running an individual program resumes at the root rather
            // than at whatever instruction it stopped at however many thousand ticks ago, and its
            // think clock is set as if it had just thought. What it must NOT do is keep a stale
            // program counter, which is what produces the snap.
            if (previous == AiTier::Statistical || previous == AiTier::Minimal) {
                if (AgentState* execution = state(states[index].slot); execution != nullptr) {
                    execution->reset();
                }
                ++report.reconstructed;
            }
        } else if (static_cast<u8>(decided) > static_cast<u8>(previous)) {
            ++report.demoted;
        }
        agent.tier = decided;
        ++report.by_tier[static_cast<usize>(decided)];
    }
    return ok();
}

Status AiRuntime::think_one(u32 tick, const BehaviourProgram& program, Entity entity,
                            const AIAgent& agent, AIState& agent_state, Blackboard& blackboard,
                            KnowledgeStore* store, BehaviourHost& host, f32 dt,
                            ThinkReport& report) noexcept {
    AgentState* held = state(agent_state.slot);
    if (held == nullptr) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "an agent's state slot names no reserved execution state; call reserve_slots() "
                  "before think(), and write the handle it returns into AIState::slot"});
    }

    // The blackboard rides in the chunk and is copied around the think; see agent.h.
    const Span<f32> board = held->blackboard();
    const auto copied =
        static_cast<u32>((board.size() < kBlackboardSlots) ? board.size() : kBlackboardSlots);
    for (u32 slot = 0; slot < copied; ++slot) {
        board[slot] = blackboard.value[slot];
    }

    TickReport tick_report;
    const Expected<BtStatus, Error> status =
        graph::behaviour::tick(program, *held, host, dt, tick_report);
    if (!status) {
        return make_unexpected(status.error());
    }
    for (u32 slot = 0; slot < copied; ++slot) {
        blackboard.value[slot] = board[slot];
    }

    // The mirror, written once, here.
    agent_state.last_think_tick = tick;
    agent_state.running = held->running();
    agent_state.stack_depth = static_cast<u8>(held->stack().size());
    agent_state.last_status = *status;

    report.instructions += tick_report.instructions_evaluated;
    report.conditions += tick_report.conditions_tested;
    report.tasks += tick_report.tasks_run;
    report.resumed += tick_report.resumed ? 1u : 0u;
    report.plan_nodes += tick_report.plan_search_nodes;

    // The per-tick state hash: `ai-system`'s determinism test mode, "so divergence is detectable
    // and localisable". The entity is in the hash, so a mismatch names the agent.
    report.state_hash = mix(report.state_hash, entity.bits());
    report.state_hash = mix(report.state_hash, static_cast<u64>(held->running()));
    report.state_hash = mix(report.state_hash, static_cast<u64>(held->world()));
    report.state_hash = mix(report.state_hash, static_cast<u64>(*status));

    if (history_capacity_ == 0) {
        return ok();
    }
    DecisionRecord entry;
    entry.agent = entity;
    entry.tick = tick;
    entry.instruction = held->running();
    entry.status = *status;
    entry.tier = agent.tier;
    entry.plan_nodes = tick_report.plan_search_nodes;
    if (store != nullptr) {
        const KnowledgeEntry* best = store->best_target();
        entry.best_target_confidence = (best != nullptr) ? best->confidence : 0.0F;
    }
    return record(entry);
}

Status AiRuntime::think(u32 tick, const BehaviourProgram& program, Span<const Entity> entities,
                        Span<AIAgent> agents, Span<AIState> states, Span<Blackboard> blackboards,
                        Span<KnowledgeStore*> stores, BehaviourHost& host, f32 dt,
                        ThinkReport& report) noexcept {
    report = ThinkReport{};
    const usize count = agents.size();
    if (entities.size() != count || states.size() != count || blackboards.size() != count) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the agent columns are not the same length"});
    }
    if (!stores.empty() && stores.size() != count) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "knowledge stores were supplied for some agents but not for all of them"});
    }

    u32 spent[static_cast<usize>(AiTier::Count)] = {};
    for (usize index = 0; index < count; ++index) {
        const AIAgent& agent = agents[index];
        ++report.agents;
        const auto tier = static_cast<usize>(agent.tier);

        // `ai-system`'s tier table: `Statistical` agents are a population model, not per-agent
        // reasoning. They are counted and skipped, which is the whole of the saving.
        if (agent.tier == AiTier::Statistical) {
            continue;
        }
        if (!due(tick, static_cast<u32>(index), agent.tier)) {
            // Not this agent's slot. Starvation is measured against the guarantee, not against
            // whether it happened to be its turn.
            if (tick > states[index].last_think_tick + tier_think_interval(agent.tier)) {
                ++report.starved;
            }
            continue;
        }
        ++report.due[tier];

        if (spent[tier] >= budget_.thinks_per_tick[tier]) {
            // DEFERRED, not dropped. `last_think_tick` is untouched, so the next tick's starvation
            // check sees it and the rotation offers it again.
            report.budget_exceeded = true;
            ++report.deferred;
            continue;
        }

        KnowledgeStore* store = stores.empty() ? nullptr : stores[index];
        if (Status thought = think_one(tick, program, entities[index], agent, states[index],
                                       blackboards[index], store, host, dt, report);
            !thought) {
            return thought;
        }
        ++spent[tier];
        ++report.thought[tier];
    }
    if (report.plan_nodes > budget_.plan_nodes_per_tick) {
        report.budget_exceeded = true;
    }
    return ok();
}

Status AiRuntime::set_history_capacity(u32 records) noexcept {
    history_.clear();
    history_head_ = 0;
    history_capacity_ = records;
    return (records == 0) ? ok() : history_.reserve(records);
}

Status AiRuntime::record(const DecisionRecord& entry) noexcept {
    if (history_capacity_ == 0) {
        return ok();
    }
    if (history_.size() < history_capacity_) {
        return history_.push_back(entry);
    }
    // A ring, so the record is ROLLING and bounded: `ai-system` asks for "a rolling record", and an
    // unbounded one over eight thousand agents is a memory leak with a debugger attached.
    history_[history_head_] = entry;
    history_head_ = (history_head_ + 1u) % history_capacity_;
    return ok();
}

Status AiRuntime::history_of(Entity agent, Array<DecisionRecord>& out) const noexcept {
    out.clear();
    // Oldest first: the ring's head is the oldest slot once it has wrapped.
    const usize size = history_.size();
    for (usize offset = 0; offset < size; ++offset) {
        const usize index = (size < history_capacity_) ? offset : ((history_head_ + offset) % size);
        if (history_[index].agent == agent) {
            if (Status pushed = out.push_back(history_[index]); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

}  // namespace cy::ai

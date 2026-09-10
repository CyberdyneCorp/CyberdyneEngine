#pragma once
// The AI runtime: the level-of-detail policy, the deterministic think schedule, the budget
// controller and the loop that runs one compiled program over many agents. M8.b tasks 6.3 and 6.4.
//
// ================================================================================================
// THE ONE PROPERTY EVERYTHING ELSE IN THIS FILE IS SHAPED BY
// ================================================================================================
//
// `ai-system`: "AI SHALL be deterministic: given the same world state and inputs, agents SHALL make
// the same decisions, so that network reconciliation, replay, and automated testing are valid." And
// then, as consequences it states rather than leaves implied:
//
//   * "An agent's LOD tier and think schedule SHALL be a function of SIMULATION STATE — distance,
//     importance, tick number, and a stable agent ordering — and SHALL NOT depend on measured frame
//     time or thread timing."
//   * "Any randomness SHALL come from a seeded generator that is part of simulation state."
//
// SO THERE IS NO CLOCK IN THIS MODULE. `AiRuntime::think()` takes a tick; the rotation is
// `tick % interval == index % interval`; the tier policy reads distance and importance; the budget
// is a count of agents, not a millisecond. Nothing here calls `steady_clock`, and a diagnostic that
// wanted to would have to be a caller's.
//
// ================================================================================================
// THE TWO BUDGET MODES, AND WHY THE HONEST ONE IS DEFAULT
// ================================================================================================
//
// `ai-system` requires exactly two, declared at project level:
//
//   | Mode | Behaviour |
//   |---|---|
//   | `Deterministic` | thresholds are fixed configuration; overruns are REPORTED, not corrected |
//   | `Adaptive` | thresholds vary with measured load. Explicitly NOT replay-safe or lockstep-safe
//   |
//
// and it requires the documentation to "state plainly that `Adaptive` forfeits deterministic replay
// and lockstep networking". It does, here: **selecting `Adaptive` gives up replay, rollback and
// lockstep networking.** A session in `Adaptive` cannot be re-simulated to the same result, because
// which agents thought on a tick depended on how busy the machine was. `BudgetMode::Deterministic`
// is the default and `AiRuntime::check_lockstep()` is the configuration-time refusal
// `ai-system`'s "Adaptive mode is honest about its cost" scenario asks for — reported when the
// project is configured, not at desync time.
//
// ================================================================================================
// NO STARVATION IS A GUARANTEE, NOT AN AVERAGE
// ================================================================================================
//
// "the rotation SHALL guarantee that every agent thinks within a bounded number of ticks for its
// tier." The rotation gives each agent one slot in `tier_think_interval(tier)`, so its turn comes
// round on schedule; the budget can only *defer within* a tick, and a deferred agent keeps its slot
// rather than losing its place. `ThinkReport::starved` counts any agent that has gone longer than
// its tier's interval, and it is the number a gate reads.

#include <cy/ai/agent.h>
#include <cy/ai/knowledge.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/graph/lower_behaviour.h>

namespace cy::ai {

using graph::behaviour::AgentState;
using graph::behaviour::BehaviourHost;
using graph::behaviour::BehaviourProgram;
using graph::behaviour::TickReport;

/// `ai-system`'s two declared budget modes. See the header: `Adaptive` forfeits replay and
/// lockstep, and this engine says so at the declaration rather than at the desync.
enum class BudgetMode : u8 { Deterministic = 0, Adaptive };

[[nodiscard]] const char* budget_mode_name(BudgetMode mode) noexcept;

/// What decides a tier. Every input is simulation state.
struct TierPolicy {
    /// Beyond this from the nearest observer an agent may not be `Full`.
    f32 full_distance = 30.0F;
    f32 reduced_distance = 80.0F;
    f32 minimal_distance = 200.0F;
    /// `ai-system`: "Tier transitions SHALL be hysteretic." A tier is only given up once the agent
    /// is this much further out than the threshold that would have granted it.
    f32 hysteresis = 8.0F;
    /// Importance above this holds a tier one step better than distance alone would give.
    f32 important_above = 2.0F;
};

/// Per-tier and per-subsystem budgets. `ai-system`: "Budgets SHALL be declared per tier and per
/// subsystem (thinking, perception queries, planning, environment queries), and utilisation SHALL
/// be reported."
struct AiBudget {
    u32 thinks_per_tick[static_cast<usize>(AiTier::Count)] = {2000, 2000, 1000, 200};
    /// The GOAP search nodes one tick may spend across every agent.
    u32 plan_nodes_per_tick = 4000;
    /// The search nodes one agent's plan may spend.
    u32 plan_nodes_per_agent = 256;
    BudgetMode mode = BudgetMode::Deterministic;
};

/// What one `think()` did.
struct ThinkReport {
    u32 agents = 0;
    u32 due[static_cast<usize>(AiTier::Count)] = {};
    u32 thought[static_cast<usize>(AiTier::Count)] = {};
    u32 deferred = 0;
    u32 instructions = 0;
    u32 conditions = 0;
    u32 tasks = 0;
    u32 plan_nodes = 0;
    u32 resumed = 0;
    /// Agents that have not thought within their tier's guaranteed interval. Zero is the
    /// requirement; a gate reads this and nothing else.
    u32 starved = 0;
    bool budget_exceeded = false;
    /// The state hash `ai-system`'s determinism test mode asks for: "A determinism test mode SHALL
    /// hash AI state per tick so divergence is detectable and localisable."
    u64 state_hash = 0;
};

/// What a tier pass decided.
struct TierReport {
    u32 agents = 0;
    u32 promoted = 0;
    u32 demoted = 0;
    u32 pinned_held = 0;
    u32 by_tier[static_cast<usize>(AiTier::Count)] = {};
    /// `ai-system`: "promotion SHALL reconstruct plausible individual state so an agent entering
    /// `Full` does not visibly snap into a different behaviour."
    u32 reconstructed = 0;
};

/// One agent's decision, kept for the debugger. `ai-system`: "a rolling record of an agent's
/// decisions with the inputs that produced them, inspectable after the fact".
struct DecisionRecord {
    Entity agent;
    u32 tick = 0;
    u16 instruction = 0;
    BtStatus status = BtStatus::Running;
    AiTier tier = AiTier::Full;
    f32 best_target_confidence = 0.0F;
    u32 plan_nodes = 0;
};

/// The runtime: it owns the per-agent execution state, the schedule and the budget.
///
/// It does NOT own a world, a program or a knowledge store: `think()` takes them, so a project may
/// run two independent AI worlds and a test may run one with no ECS world at all.
class AiRuntime {
public:
    AiRuntime(Allocator& allocator, const AiBudget& budget, const TierPolicy& policy) noexcept;

    AiRuntime(const AiRuntime&) = delete;
    AiRuntime& operator=(const AiRuntime&) = delete;

    [[nodiscard]] const AiBudget& budget() const noexcept { return budget_; }
    [[nodiscard]] const TierPolicy& policy() const noexcept { return policy_; }
    void set_budget(const AiBudget& budget) noexcept { budget_ = budget; }

    /// `ai-system`'s "Adaptive mode is honest about its cost": a project that selects `Adaptive`
    /// and then enables lockstep is refused HERE, at configuration time, with the reason.
    [[nodiscard]] Status check_lockstep(bool lockstep_enabled) const noexcept;

    /// Give `count` agents execution state against `program`. Returns the first HANDLE, which is
    /// one-based (see `kInvalidSlot`); handles are consecutive, so an agent's is `first + index`.
    [[nodiscard]] Expected<u32, Error> reserve_slots(const BehaviourProgram& program,
                                                     u32 count) noexcept;
    [[nodiscard]] AgentState* state(u32 slot) noexcept;
    [[nodiscard]] u32 slot_count() const noexcept { return static_cast<u32>(states_.size()); }

    /// Decide every agent's tier from distance, importance and its pin. Deterministic, hysteretic,
    /// and it reconstructs state on promotion.
    [[nodiscard]] Status update_tiers(Span<AIAgent> agents, Span<const Vec3> positions,
                                      Span<const AIState> states, Span<const Vec3> observers,
                                      TierReport& report) noexcept;

    /// One tick of thinking. `agents`, `states`, `blackboards`, `entities` and `stores` are
    /// parallel columns; `stores` may be empty when no agent has knowledge.
    ///
    /// `ai-system`'s "Bulk execution" scenario is this function: one immutable program, packed
    /// columns, and no per-agent object.
    [[nodiscard]] Status think(u32 tick, const BehaviourProgram& program,
                               Span<const Entity> entities, Span<AIAgent> agents,
                               Span<AIState> states, Span<Blackboard> blackboards,
                               Span<KnowledgeStore*> stores, BehaviourHost& host, f32 dt,
                               ThinkReport& report) noexcept;

    /// The rolling decision history, oldest first. Empty until `set_history_capacity`.
    [[nodiscard]] Span<const DecisionRecord> history() const noexcept { return history_.span(); }
    [[nodiscard]] Status set_history_capacity(u32 records) noexcept;
    /// Every record for one agent, appended to `out`. What "why did it do that" asks.
    [[nodiscard]] Status history_of(Entity agent, Array<DecisionRecord>& out) const noexcept;

private:
    /// Is this agent's slot in the rotation this tick? Static because the rotation is a pure
    /// function of the tick, the agent's stable index and its tier — which is the requirement:
    /// `ai-system` puts the schedule in simulation state and nowhere else, and a member this
    /// function read would be a place for something else to creep in.
    [[nodiscard]] static bool due(u32 tick, u32 index, AiTier tier) noexcept;
    [[nodiscard]] Status record(const DecisionRecord& entry) noexcept;
    /// One agent's think: the blackboard copy, the tick, the mirror, the hash and the record.
    /// Split out of `think()` so that the loop above it is the SCHEDULE and nothing else — which is
    /// the part `ai-system` constrains, and the part a reader comes to this file for.
    [[nodiscard]] Status think_one(u32 tick, const BehaviourProgram& program, Entity entity,
                                   const AIAgent& agent, AIState& agent_state,
                                   Blackboard& blackboard, KnowledgeStore* store,
                                   BehaviourHost& host, f32 dt, ThinkReport& report) noexcept;

    AiBudget budget_;
    TierPolicy policy_;
    Array<AgentState> states_;
    Array<DecisionRecord> history_;
    u32 history_capacity_ = 0;
    u32 history_head_ = 0;
};

}  // namespace cy::ai

#pragma once
// The AI behaviour IR: a flat instruction stream plus a parameter table, over one shared register
// machine. M8.b task 2.4.
//
// ================================================================================================
// WHY ARTIFICIAL INTELLIGENCE KEEPS ITS OWN INTERMEDIATE REPRESENTATION
// ================================================================================================
//
// `ai-system` asks for one asset composing FOUR reasoning models nested arbitrarily — a
// hierarchical state tree, a behaviour tree, utility scoring, and goal-oriented action planning —
// and its compiled form is "a flat instruction stream plus a parameter table" with per-agent state
// of "program counter, execution stack, timers, and blackboard".
//
// Three of its requirements are each on their own enough to rule out the shared expression core:
//
//   * A THREE-VALUED STATUS. `Running` is not a value an expression produces; it is a statement
//     about where evaluation stopped.
//   * "EXECUTION RESUM[ES] AT THE RUNNING NODE rather than re-descending from the root each tick",
//     which is a lazy branch and a saved program counter. The shared core's conditional evaluates
//     both arms (design.md §1.3, probe P10).
//   * THE BLACKBOARD IS WRITTEN BY NODES. Identity in the shared core is content, so two reads of
//     one mutable cell are one value (probe P4). A blackboard cannot exist there.
//
// ================================================================================================
// ONE PROGRAM, MANY AGENTS — AND THAT IS THE PERMITTED SHAPE, NOT THE VIOLATION
// ================================================================================================
//
// `ai-system`: "AI graphs SHALL be compiled, not interpreted node-by-node at runtime [...] it SHALL
// contain compiled programs and no graph compiler." `visual-scripting` names the back end this is:
// "a typed register machine with a shared program and separate state. There SHALL NOT be one
// virtual machine instance per entity."
//
// So `BehaviourProgram` is immutable and shared by eight thousand agents; `AgentState` is a program
// counter, a small stack, a blackboard and some timers. `tick()` takes both. Nothing walks a graph.
//
// GOAP IS A RUNTIME SEARCH OVER A COMPILED TABLE, which is a different thing from interpreting a
// graph: the operators, their preconditions and their effects are compiled into bitsets at cook
// time, and the search is best-first over those bitsets with a per-tick node budget it resumes
// across ticks.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/graph/cybergraph.h>

namespace cy::graph::behaviour {

/// `ai-system`'s three-valued status. Not a boolean, and not an enumeration with a fourth member:
/// `Running` is what makes resumption meaningful.
enum class BtStatus : u8 { Running = 0, Success, Failure };

[[nodiscard]] const char* bt_status_name(BtStatus status) noexcept;

/// The world state GOAP plans over: a bitset of authored predicates.
using WorldState = u64;
inline constexpr u32 kMaxPredicates = 64;

enum class AiOp : u16 {
    /// Children in order; fails at the first failure, succeeds when all succeed.
    Sequence = 0,
    /// Children in order; succeeds at the first success, fails when all fail.
    Selector,
    /// Children every tick; the policy in `param` decides the verdict.
    Parallel,
    /// A leaf that does something. `task` names it; the host runs it and answers a status.
    Task,
    /// A leaf that tests something. `task` names the test.
    Condition,
    /// One child, with the decorator in `param` applied to its status.
    Inverter,
    /// Succeeds after `param` seconds, `Running` until then. Uses one agent timer.
    Wait,
    /// UTILITY SCORING. Each child carries a score expression and a response curve; the highest
    /// scorer runs, with hysteresis against the previous winner.
    UtilitySelect,
    /// A hierarchical state tree node: enters `param`'s state and runs its child.
    State,
    /// GOAP. Plans towards the goal in `param` over the operator table, then runs the plan.
    Plan,
    Count,
};

[[nodiscard]] const char* ai_op_name(AiOp op) noexcept;

/// `ai-system`'s four response curves, by name: "linear, quadratic, logistic, inverse".
enum class CurveKind : u8 { Linear = 0, Quadratic, Logistic, Inverse, Count };

struct ResponseCurve {
    CurveKind kind = CurveKind::Linear;
    f32 slope = 1.0F;
    f32 exponent = 2.0F;
    f32 midpoint = 0.5F;
    f32 steepness = 8.0F;
    /// How much better a challenger must score to displace the incumbent. Without it a utility
    /// selector oscillates between two options that score within noise of each other.
    f32 hysteresis = 0.0F;
};

[[nodiscard]] f32 apply_curve(const ResponseCurve& curve, f32 input) noexcept;

struct AiInstruction {
    AiOp op = AiOp::Task;
    /// The children, as a contiguous run in the instruction stream.
    u16 first_child = 0;
    u16 child_count = 0;
    /// The external the leaf names, or the goal a plan pursues.
    u16 task = 0;
    /// The parameter-table index this instruction reads.
    u16 param = 0;
    /// The response curve, for `UtilitySelect`'s children.
    u16 curve = 0;
    /// The blackboard slot a scoring child reads.
    u16 blackboard = 0;
    NodeKey origin = kInvalidNodeKey;
};

/// One planning operator, compiled. `ai-system`'s GOAP is a runtime search over exactly this table.
struct PlanOperator {
    u16 task = 0;
    WorldState requires_true = 0;
    WorldState requires_false = 0;
    WorldState sets_true = 0;
    WorldState sets_false = 0;
    f32 cost = 1.0F;
    NodeKey origin = kInvalidNodeKey;
};

/// A compiled behaviour program. IMMUTABLE AND SHARED.
class BehaviourProgram {
public:
    explicit BehaviourProgram(Allocator& allocator) noexcept;

    BehaviourProgram(const BehaviourProgram&) = delete;
    BehaviourProgram& operator=(const BehaviourProgram&) = delete;
    BehaviourProgram(BehaviourProgram&&) noexcept = default;
    BehaviourProgram& operator=(BehaviourProgram&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] Span<const AiInstruction> code() const noexcept { return code_.span(); }
    [[nodiscard]] Span<const f32> parameters() const noexcept { return parameters_.span(); }
    [[nodiscard]] Span<const ResponseCurve> curves() const noexcept { return curves_.span(); }
    [[nodiscard]] Span<const Name> tasks() const noexcept { return tasks_.span(); }
    [[nodiscard]] Span<const PlanOperator> operators() const noexcept { return operators_.span(); }
    /// The goal world-states `Plan` instructions pursue, by `AiInstruction::param`.
    [[nodiscard]] Span<const WorldState> goals() const noexcept { return goals_.span(); }
    [[nodiscard]] u16 root() const noexcept { return root_; }
    [[nodiscard]] u16 blackboard_size() const noexcept { return blackboard_; }
    [[nodiscard]] u64 digest() const noexcept { return digest_; }
    [[nodiscard]] const DebugMap& debug() const noexcept { return debug_; }
    [[nodiscard]] Allocator& allocator() const noexcept { return code_.allocator(); }

private:
    friend class BehaviourProgramAccess;

    Name name_;
    Array<AiInstruction> code_;
    Array<f32> parameters_;
    Array<ResponseCurve> curves_;
    Array<Name> tasks_;
    Array<PlanOperator> operators_;
    Array<WorldState> goals_;
    DebugMap debug_;
    u16 root_ = 0;
    u16 blackboard_ = 0;
    u64 digest_ = 0;
};

/// The deepest a behaviour tree may nest. A real bound: the execution stack is part of every
/// agent's state, and eight thousand agents each carrying an unbounded stack is not a budget.
inline constexpr u32 kMaxDepth = 32;
/// The longest plan GOAP will build.
inline constexpr u32 kMaxPlan = 16;

/// ONE AGENT'S STATE. `ai-system`: "program counter, execution stack, timers, and blackboard".
class AgentState {
public:
    AgentState(Allocator& allocator, const BehaviourProgram& program) noexcept;

    AgentState(const AgentState&) = delete;
    AgentState& operator=(const AgentState&) = delete;
    AgentState(AgentState&&) noexcept = default;
    AgentState& operator=(AgentState&&) noexcept = default;

    [[nodiscard]] Span<f32> blackboard() noexcept { return blackboard_.span(); }
    [[nodiscard]] Span<const f32> blackboard() const noexcept { return blackboard_.span(); }
    /// The instruction execution resumed at last tick, or `0xFFFF` for "start at the root".
    [[nodiscard]] u16 running() const noexcept { return running_; }
    [[nodiscard]] Span<const u16> stack() const noexcept { return {stack_, stack_depth_}; }
    [[nodiscard]] WorldState world() const noexcept { return world_; }
    void set_world(WorldState state) noexcept { world_ = state; }
    [[nodiscard]] Span<const u16> plan() const noexcept { return {plan_, plan_length_}; }
    [[nodiscard]] u8 plan_cursor() const noexcept { return plan_cursor_; }
    /// How many search nodes the last plan cost. Reported so a budget is a measurement.
    [[nodiscard]] u32 last_plan_nodes() const noexcept { return plan_nodes_; }
    void reset() noexcept;

private:
    friend class AgentStateAccess;

    Array<f32> blackboard_;
    u16 stack_[kMaxDepth] = {};
    u16 plan_[kMaxPlan] = {};
    WorldState world_ = 0;
    u32 plan_nodes_ = 0;
    f32 timer_ = 0.0F;
    u16 running_ = 0xFFFFU;
    u16 last_utility_choice_ = 0xFFFFU;
    u8 stack_depth_ = 0;
    u8 plan_length_ = 0;
    u8 plan_cursor_ = 0;
};

/// What the host supplies. Every leaf goes through it; the program itself does nothing to a world.
class BehaviourHost {
public:
    BehaviourHost() = default;
    virtual ~BehaviourHost() = default;
    BehaviourHost(const BehaviourHost&) = delete;
    BehaviourHost& operator=(const BehaviourHost&) = delete;
    BehaviourHost(BehaviourHost&&) = delete;
    BehaviourHost& operator=(BehaviourHost&&) = delete;

    virtual BtStatus run_task(Name task, f32 dt) = 0;
    [[nodiscard]] virtual bool test_condition(Name condition) = 0;
    /// The score a utility child reads, before its response curve.
    [[nodiscard]] virtual f32 score(Name task) = 0;
};

/// What one tick did. `evaluated` is the criterion behind "resume at the running node": a resumed
/// tick evaluates fewer instructions than a fresh descent, and the test says how many fewer.
struct TickReport {
    u32 instructions_evaluated = 0;
    u32 conditions_tested = 0;
    u32 tasks_run = 0;
    u32 plan_search_nodes = 0;
    bool resumed = false;
};

/// Tick one agent. ONE SHARED PROGRAM, ONE AGENT'S STATE.
[[nodiscard]] Expected<BtStatus, Error> tick(const BehaviourProgram& program, AgentState& state,
                                             BehaviourHost& host, f32 dt,
                                             TickReport& report) noexcept;

/// The GOAP search, exposed because it is a runtime search with a budget and a caller may want to
/// spend that budget itself. Returns the number of search nodes expanded; the plan lands in
/// `state`. `ai-system` requires the search to be budgeted and incremental, so exhausting `budget`
/// leaves the agent with no plan and is reported rather than hidden.
[[nodiscard]] u32 plan_towards(const BehaviourProgram& program, AgentState& state, WorldState goal,
                               u32 budget) noexcept;

// --- Compilation ------------------------------------------------------------------------------

[[nodiscard]] Expected<BehaviourProgram, Error> compile_behaviour(const Graph& graph,
                                                                  const NodeRegistry& registry,
                                                                  DiagnosticSink& sink) noexcept;

[[nodiscard]] Status register_behaviour_nodes(NodeRegistry& registry) noexcept;

}  // namespace cy::graph::behaviour

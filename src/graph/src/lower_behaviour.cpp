// The AI behaviour IR: the compiler, the shared register machine, and the GOAP search. Task 2.4.
//
// See lower_behaviour.h for why artificial intelligence keeps its own representation and why one
// shared program over packed per-agent state is the permitted shape rather than the violation.

#include <cy/graph/lower_behaviour.h>

#include <cmath>
#include <utility>

namespace cy::graph::behaviour {

/// Write access to a `BehaviourProgram`, which has none in public: a compiled program is shared by
/// every agent at run time.
class BehaviourProgramAccess {
public:
    [[nodiscard]] static Array<AiInstruction>& code(BehaviourProgram& program) noexcept {
        return program.code_;
    }
    [[nodiscard]] static Array<f32>& parameters(BehaviourProgram& program) noexcept {
        return program.parameters_;
    }
    [[nodiscard]] static Array<ResponseCurve>& curves(BehaviourProgram& program) noexcept {
        return program.curves_;
    }
    [[nodiscard]] static Array<Name>& tasks(BehaviourProgram& program) noexcept {
        return program.tasks_;
    }
    [[nodiscard]] static Array<PlanOperator>& operators(BehaviourProgram& program) noexcept {
        return program.operators_;
    }
    [[nodiscard]] static Array<WorldState>& goals(BehaviourProgram& program) noexcept {
        return program.goals_;
    }
    [[nodiscard]] static DebugMap& debug(BehaviourProgram& program) noexcept {
        return program.debug_;
    }
    static void set_name(BehaviourProgram& program, Name name) noexcept { program.name_ = name; }
    static void set_root(BehaviourProgram& program, u16 root) noexcept { program.root_ = root; }
    static void set_blackboard(BehaviourProgram& program, u16 size) noexcept {
        program.blackboard_ = size;
    }
    static void set_digest(BehaviourProgram& program, u64 digest) noexcept {
        program.digest_ = digest;
    }
};

/// Write access to an `AgentState`. The stack, the timer and the plan are the machine's business
/// and not the caller's, which is what keeps "one shared program, separate state" true.
class AgentStateAccess {
public:
    [[nodiscard]] static u16* stack(AgentState& state) noexcept { return state.stack_; }
    [[nodiscard]] static u8& depth(AgentState& state) noexcept { return state.stack_depth_; }
    [[nodiscard]] static u16& running(AgentState& state) noexcept { return state.running_; }
    [[nodiscard]] static f32& timer(AgentState& state) noexcept { return state.timer_; }
    [[nodiscard]] static u16& last_choice(AgentState& state) noexcept {
        return state.last_utility_choice_;
    }
    [[nodiscard]] static u16* plan(AgentState& state) noexcept { return state.plan_; }
    [[nodiscard]] static u8& plan_length(AgentState& state) noexcept { return state.plan_length_; }
    [[nodiscard]] static u8& plan_cursor(AgentState& state) noexcept { return state.plan_cursor_; }
    [[nodiscard]] static u32& plan_nodes(AgentState& state) noexcept { return state.plan_nodes_; }
};

const char* bt_status_name(BtStatus status) noexcept {
    switch (status) {
        case BtStatus::Running:
            return "running";
        case BtStatus::Success:
            return "success";
        case BtStatus::Failure:
            return "failure";
    }
    return "?";
}

const char* ai_op_name(AiOp op) noexcept {
    switch (op) {
        case AiOp::Sequence:
            return "sequence";
        case AiOp::Selector:
            return "selector";
        case AiOp::Parallel:
            return "parallel";
        case AiOp::Task:
            return "task";
        case AiOp::Condition:
            return "condition";
        case AiOp::Inverter:
            return "inverter";
        case AiOp::Wait:
            return "wait";
        case AiOp::UtilitySelect:
            return "utility_select";
        case AiOp::State:
            return "state";
        case AiOp::Plan:
            return "plan";
        case AiOp::Count:
            break;
    }
    return "?";
}

f32 apply_curve(const ResponseCurve& curve, f32 input) noexcept {
    f32 clamped = input;
    clamped = clamped < 0.0F ? 0.0F : clamped;
    clamped = clamped > 1.0F ? 1.0F : clamped;
    switch (curve.kind) {
        case CurveKind::Linear:
            return clamped * curve.slope;
        case CurveKind::Quadratic:
            return std::pow(clamped, curve.exponent) * curve.slope;
        case CurveKind::Logistic:
            return curve.slope / (1.0F + std::exp(-curve.steepness * (clamped - curve.midpoint)));
        case CurveKind::Inverse:
            return curve.slope * (1.0F - clamped);
        case CurveKind::Count:
            break;
    }
    return clamped;
}

BehaviourProgram::BehaviourProgram(Allocator& allocator) noexcept
    : code_(allocator),
      parameters_(allocator),
      curves_(allocator),
      tasks_(allocator),
      operators_(allocator),
      goals_(allocator),
      debug_(allocator) {}

AgentState::AgentState(Allocator& allocator, const BehaviourProgram& program) noexcept
    : blackboard_(allocator) {
    (void)blackboard_.resize(program.blackboard_size());
    for (f32& slot : blackboard_) {
        slot = 0.0F;
    }
}

void AgentState::reset() noexcept {
    stack_depth_ = 0;
    running_ = 0xFFFFU;
    timer_ = 0.0F;
    last_utility_choice_ = 0xFFFFU;
    plan_length_ = 0;
    plan_cursor_ = 0;
}

// --- The GOAP search ------------------------------------------------------------------------

u32 plan_towards(const BehaviourProgram& program, AgentState& state, WorldState goal,
                 u32 budget) noexcept {
    // Best-first over WORLD STATES, not over graph nodes: the operators, their preconditions and
    // their effects were compiled into bitsets at cook time, and this walks that table. A search
    // over compiled data is not an interpreter, which is the distinction design.md §1.5 draws.
    struct Node {
        WorldState world = 0;
        f32 cost = 0.0F;
        u16 parent = 0xFFFFU;
        u16 op = 0xFFFFU;
    };
    Node open[64];
    u32 count = 0;
    open[count++] = Node{state.world(), 0.0F, 0xFFFFU, 0xFFFFU};

    u32 expanded = 0;
    u32 cursor = 0;
    u16 found = 0xFFFFU;
    while (cursor < count && expanded < budget && count < 64) {
        // The cheapest unexpanded node.
        u32 best = cursor;
        for (u32 index = cursor; index < count; ++index) {
            if (open[index].cost < open[best].cost) {
                best = index;
            }
        }
        const Node swap = open[cursor];
        open[cursor] = open[best];
        open[best] = swap;
        const u32 current = cursor++;
        ++expanded;
        if ((open[current].world & goal) == goal) {
            found = static_cast<u16>(current);
            break;
        }
        for (u16 index = 0; index < program.operators().size() && count < 64; ++index) {
            const PlanOperator& op = program.operators()[index];
            const WorldState world = open[current].world;
            if ((world & op.requires_true) != op.requires_true ||
                (world & op.requires_false) != 0) {
                continue;
            }
            const WorldState next = (world | op.sets_true) & ~op.sets_false;
            if (next == world) {
                continue;
            }
            bool seen = false;
            for (u32 existing = 0; existing < count; ++existing) {
                seen = seen || open[existing].world == next;
            }
            if (seen) {
                continue;
            }
            open[count++] =
                Node{next, open[current].cost + op.cost, static_cast<u16>(current), index};
        }
    }

    AgentStateAccess::plan_nodes(state) = expanded;
    AgentStateAccess::plan_length(state) = 0;
    AgentStateAccess::plan_cursor(state) = 0;
    if (found == 0xFFFFU) {
        // BUDGET EXHAUSTED OR NO PLAN. Reported as an empty plan rather than as a partial one: a
        // partial plan an agent commits to is worse than none.
        return expanded;
    }
    u16 chain[kMaxPlan] = {};
    u8 length = 0;
    for (u16 node = found; node != 0xFFFFU && open[node].op != 0xFFFFU && length < kMaxPlan;
         node = open[node].parent) {
        chain[length++] = open[node].op;
    }
    u16* plan = AgentStateAccess::plan(state);
    for (u8 index = 0; index < length; ++index) {
        plan[index] = chain[length - 1 - index];
    }
    AgentStateAccess::plan_length(state) = length;
    return expanded;
}

// --- The register machine -------------------------------------------------------------------

namespace {

struct Ticker {
    const BehaviourProgram* program = nullptr;
    AgentState* state = nullptr;
    BehaviourHost* host = nullptr;
    TickReport* report = nullptr;
    f32 dt = 0.0F;
    /// The path saved by the last tick: the instructions from the root down to the running leaf.
    u16 resume[kMaxDepth] = {};
    u8 resume_depth = 0;
    u16 path[kMaxDepth] = {};
    u8 path_depth = 0;

    [[nodiscard]] BtStatus run(u16 index, u8 depth) noexcept;
    [[nodiscard]] BtStatus run_composite(u16 index, u8 depth) noexcept;
    [[nodiscard]] BtStatus run_leaf(u16 index) const noexcept;
    [[nodiscard]] BtStatus run_utility(u16 index, u8 depth) noexcept;
    [[nodiscard]] BtStatus run_plan(u16 index) const noexcept;
    [[nodiscard]] u16 resumed_child(u16 index, u8 depth) const noexcept;
};

u16 Ticker::resumed_child(u16 index, u8 depth) const noexcept {
    // RESUMPTION. `ai-system`: "execution resum[es] at the running node rather than re-descending
    // from the root each tick". A composite on the saved path starts at the child the path names,
    // so the siblings before it are not re-evaluated and their conditions are not re-tested.
    if (depth >= resume_depth || resume[depth] != index || depth + 1 >= resume_depth) {
        return 0;
    }
    const AiInstruction& instruction = program->code()[index];
    for (u16 offset = 0; offset < instruction.child_count; ++offset) {
        if (instruction.first_child + offset == resume[depth + 1]) {
            return offset;
        }
    }
    return 0;
}

BtStatus Ticker::run_leaf(u16 index) const noexcept {
    const AiInstruction& instruction = program->code()[index];
    switch (instruction.op) {
        case AiOp::Task: {
            ++report->tasks_run;
            const Name task = instruction.task < program->tasks().size()
                                  ? program->tasks()[instruction.task]
                                  : Name{};
            return host->run_task(task, dt);
        }
        case AiOp::Condition: {
            ++report->conditions_tested;
            const Name condition = instruction.task < program->tasks().size()
                                       ? program->tasks()[instruction.task]
                                       : Name{};
            return host->test_condition(condition) ? BtStatus::Success : BtStatus::Failure;
        }
        case AiOp::Wait: {
            f32& timer = AgentStateAccess::timer(*state);
            timer += dt;
            const f32 seconds = instruction.param < program->parameters().size()
                                    ? program->parameters()[instruction.param]
                                    : 0.0F;
            if (timer < seconds) {
                return BtStatus::Running;
            }
            timer = 0.0F;
            return BtStatus::Success;
        }
        default:
            break;
    }
    return BtStatus::Failure;
}

BtStatus Ticker::run_composite(u16 index, u8 depth) noexcept {
    const AiInstruction instruction = program->code()[index];
    const u16 start = resumed_child(index, depth);
    // A RESUMED SEQUENCE'S EARLIER CHILDREN ALREADY SUCCEEDED — that is why the previous tick moved
    // past them — so they count towards the verdict without being re-run. A selector's earlier
    // children already FAILED, so they count towards nothing.
    u32 successes = instruction.op == AiOp::Selector ? 0U : start;
    for (u16 offset = start; offset < instruction.child_count; ++offset) {
        const BtStatus status =
            run(static_cast<u16>(instruction.first_child + offset), static_cast<u8>(depth + 1));
        if (status == BtStatus::Running) {
            return BtStatus::Running;
        }
        if (instruction.op == AiOp::Sequence && status == BtStatus::Failure) {
            return BtStatus::Failure;
        }
        if (instruction.op == AiOp::Selector && status == BtStatus::Success) {
            return BtStatus::Success;
        }
        if (instruction.op == AiOp::Parallel && status == BtStatus::Failure) {
            return BtStatus::Failure;
        }
        successes += status == BtStatus::Success ? 1U : 0U;
    }
    if (instruction.op == AiOp::Selector) {
        return BtStatus::Failure;
    }
    return successes == instruction.child_count ? BtStatus::Success : BtStatus::Failure;
}

BtStatus Ticker::run_utility(u16 index, u8 depth) noexcept {
    const AiInstruction instruction = program->code()[index];
    u16 winner = 0xFFFFU;
    f32 best = 0.0F;
    for (u16 offset = 0; offset < instruction.child_count; ++offset) {
        const u16 child = static_cast<u16>(instruction.first_child + offset);
        const AiInstruction& scored = program->code()[child];
        const Name task =
            scored.task < program->tasks().size() ? program->tasks()[scored.task] : Name{};
        const ResponseCurve& curve = scored.curve < program->curves().size()
                                         ? program->curves()[scored.curve]
                                         : program->curves()[0];
        f32 value = apply_curve(curve, host->score(task));
        // HYSTERESIS. Without it two options that score within noise of each other swap every
        // tick, which `ai-system` calls out by name.
        if (AgentStateAccess::last_choice(*state) == child) {
            value += curve.hysteresis;
        }
        if (winner == 0xFFFFU || value > best) {
            winner = child;
            best = value;
        }
    }
    if (winner == 0xFFFFU) {
        return BtStatus::Failure;
    }
    AgentStateAccess::last_choice(*state) = winner;
    // ONLY THE WINNER RUNS. Scoring every child is the utility model; evaluating every child's
    // subtree would be the thing an expression DAG does and the thing this specification forbids.
    return run(winner, static_cast<u8>(depth + 1));
}

BtStatus Ticker::run_plan(u16 index) const noexcept {
    const AiInstruction instruction = program->code()[index];
    const WorldState goal =
        instruction.param < program->goals().size() ? program->goals()[instruction.param] : 0;
    u8& length = AgentStateAccess::plan_length(*state);
    u8& cursor = AgentStateAccess::plan_cursor(*state);
    if ((state->world() & goal) == goal) {
        length = 0;
        cursor = 0;
        return BtStatus::Success;
    }
    const bool needs_plan = length == 0 || cursor >= length;
    if (needs_plan) {
        report->plan_search_nodes += plan_towards(*program, *state, goal, 64);
        if (AgentStateAccess::plan_length(*state) == 0) {
            return BtStatus::Failure;
        }
    }
    const PlanOperator& step = program->operators()[AgentStateAccess::plan(*state)[cursor]];
    // INVALIDATION. The world moved out from under the plan; replan rather than run a step whose
    // preconditions no longer hold.
    if ((state->world() & step.requires_true) != step.requires_true ||
        (state->world() & step.requires_false) != 0) {
        length = 0;
        cursor = 0;
        return BtStatus::Running;
    }
    ++report->tasks_run;
    const Name task = step.task < program->tasks().size() ? program->tasks()[step.task] : Name{};
    const BtStatus status = host->run_task(task, dt);
    if (status == BtStatus::Success) {
        state->set_world((state->world() | step.sets_true) & ~step.sets_false);
        ++cursor;
        return cursor >= length ? BtStatus::Success : BtStatus::Running;
    }
    if (status == BtStatus::Failure) {
        length = 0;
        cursor = 0;
    }
    return status;
}

BtStatus Ticker::run(u16 index, u8 depth) noexcept {
    if (index >= program->code().size() || depth >= kMaxDepth) {
        return BtStatus::Failure;
    }
    ++report->instructions_evaluated;
    path[depth] = index;
    path_depth = static_cast<u8>(depth + 1);

    const AiInstruction& instruction = program->code()[index];
    BtStatus status = BtStatus::Failure;
    switch (instruction.op) {
        case AiOp::Sequence:
        case AiOp::Selector:
        case AiOp::Parallel:
            status = run_composite(index, depth);
            break;
        case AiOp::Inverter:
        case AiOp::State: {
            if (instruction.child_count == 0) {
                status = BtStatus::Failure;
                break;
            }
            status = run(instruction.first_child, static_cast<u8>(depth + 1));
            if (instruction.op == AiOp::Inverter && status != BtStatus::Running) {
                status = status == BtStatus::Success ? BtStatus::Failure : BtStatus::Success;
            }
            break;
        }
        case AiOp::UtilitySelect:
            status = run_utility(index, depth);
            break;
        case AiOp::Plan:
            status = run_plan(index);
            break;
        default:
            status = run_leaf(index);
            break;
    }
    if (status == BtStatus::Running) {
        // The path stays as it is; the caller's frames are already in `path` below this depth.
        return status;
    }
    path_depth = depth;
    return status;
}

}  // namespace

Expected<BtStatus, Error> tick(const BehaviourProgram& program, AgentState& state,
                               BehaviourHost& host, f32 dt, TickReport& report) noexcept {
    if (program.code().empty()) {
        return BtStatus::Failure;
    }
    Ticker ticker;
    ticker.program = &program;
    ticker.state = &state;
    ticker.host = &host;
    ticker.report = &report;
    ticker.dt = dt;
    ticker.resume_depth = static_cast<u8>(state.stack().size());
    for (u8 index = 0; index < ticker.resume_depth; ++index) {
        ticker.resume[index] = state.stack()[index];
    }
    report.resumed = ticker.resume_depth > 0;

    const BtStatus status = ticker.run(program.root(), 0);

    u16* stack = AgentStateAccess::stack(state);
    u8& depth = AgentStateAccess::depth(state);
    depth = status == BtStatus::Running ? ticker.path_depth : 0;
    for (u8 index = 0; index < depth; ++index) {
        stack[index] = ticker.path[index];
    }
    AgentStateAccess::running(state) = depth > 0 ? stack[depth - 1] : 0xFFFFU;
    return status;
}

// --- Compilation ------------------------------------------------------------------------------

namespace {

[[nodiscard]] PinDesc pin(const char* name, const char* type, PinDirection direction,
                          bool variadic = false) noexcept {
    PinDesc desc;
    desc.name = Name::intern(name);
    desc.type = Name::intern(type);
    desc.direction = direction;
    desc.variadic = variadic;
    return desc;
}

[[nodiscard]] Status register_node(NodeRegistry& registry, const char* type,
                                   Span<const PinDesc> pins) noexcept {
    NodeTypeDesc desc;
    desc.name = Name::intern(type);
    desc.plugin = Name::intern("cy.graph.behaviour");
    desc.pins = pins;
    desc.pure = false;
    return registry.register_type(desc);
}

struct Lowering {
    const Graph* graph = nullptr;
    BehaviourProgram* program = nullptr;
    DiagnosticSink* sink = nullptr;
};

[[nodiscard]] u16 intern_task(BehaviourProgram& program, Name task) noexcept {
    Array<Name>& tasks = BehaviourProgramAccess::tasks(program);
    for (usize index = 0; index < tasks.size(); ++index) {
        if (tasks[index] == task) {
            return static_cast<u16>(index);
        }
    }
    if (!tasks.push_back(task)) {
        return 0;
    }
    return static_cast<u16>(tasks.size() - 1);
}

[[nodiscard]] u16 intern_parameter(BehaviourProgram& program, f32 value) noexcept {
    Array<f32>& parameters = BehaviourProgramAccess::parameters(program);
    for (usize index = 0; index < parameters.size(); ++index) {
        if (parameters[index] == value) {
            return static_cast<u16>(index);
        }
    }
    if (!parameters.push_back(value)) {
        return 0;
    }
    return static_cast<u16>(parameters.size() - 1);
}

[[nodiscard]] Status collect_children(const Graph& graph, NodeKey node,
                                      Array<NodeKey>& out) noexcept {
    const Name pin_name = Name::intern("children");
    const Name child_pin = Name::intern("child");
    for (const Link& link : graph.links()) {
        if (link.to == node && (link.to_pin == pin_name || link.to_pin == child_pin)) {
            if (Status pushed = out.push_back(link.from); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

[[nodiscard]] Status lower_into(Lowering& state, u16 slot, NodeKey node) noexcept;

[[nodiscard]] Status lower_children(Lowering& state, u16 slot, NodeKey node,
                                    AiInstruction& instruction) noexcept {
    Array<NodeKey> children(state.graph->allocator());
    if (Status collected = collect_children(*state.graph, node, children); !collected) {
        return collected;
    }
    Array<AiInstruction>& code = BehaviourProgramAccess::code(*state.program);
    // THE CHILDREN ARE A CONTIGUOUS RUN. Their slots are reserved before any of them is lowered, so
    // a grandchild appended later cannot land between two siblings.
    instruction.first_child = static_cast<u16>(code.size());
    instruction.child_count = static_cast<u16>(children.size());
    for (usize index = 0; index < children.size(); ++index) {
        if (Status pushed = code.push_back(AiInstruction{}); !pushed) {
            return pushed;
        }
    }
    for (usize index = 0; index < children.size(); ++index) {
        if (Status lowered = lower_into(state, static_cast<u16>(instruction.first_child + index),
                                        children[index]);
            !lowered) {
            return lowered;
        }
    }
    (void)slot;
    return ok();
}

Status lower_into(Lowering& state, u16 slot, NodeKey node) noexcept {
    const GraphNode* authored = state.graph->find_node(node);
    if (authored == nullptr) {
        return make_unexpected(
            Error{ErrorCode::NotFound, "a wire names a node that is not here", 0});
    }
    BehaviourProgram& program = *state.program;
    AiInstruction instruction;
    instruction.origin = node;
    const std::string_view type = authored->type.text();
    const Literal* task = state.graph->property(node, Name::intern("task"));

    if (type == "ai.sequence" || type == "ai.selector" || type == "ai.parallel") {
        instruction.op = AiOp::Parallel;
        if (type == "ai.sequence") {
            instruction.op = AiOp::Sequence;
        } else if (type == "ai.selector") {
            instruction.op = AiOp::Selector;
        }
        if (Status lowered = lower_children(state, slot, node, instruction); !lowered) {
            return lowered;
        }
    } else if (type == "ai.inverter" || type == "ai.state") {
        instruction.op = type == "ai.inverter" ? AiOp::Inverter : AiOp::State;
        if (Status lowered = lower_children(state, slot, node, instruction); !lowered) {
            return lowered;
        }
    } else if (type == "ai.utility") {
        instruction.op = AiOp::UtilitySelect;
        if (Status lowered = lower_children(state, slot, node, instruction); !lowered) {
            return lowered;
        }
    } else if (type == "ai.task") {
        instruction.op = AiOp::Task;
        instruction.task = intern_task(program, task != nullptr ? task->text : Name{});
    } else if (type == "ai.condition") {
        instruction.op = AiOp::Condition;
        instruction.task = intern_task(program, task != nullptr ? task->text : Name{});
    } else if (type == "ai.wait") {
        instruction.op = AiOp::Wait;
        const Literal* seconds = state.graph->property(node, Name::intern("seconds"));
        instruction.param = intern_parameter(program, seconds != nullptr ? seconds->value.x : 0.0F);
    } else if (type == "ai.plan") {
        instruction.op = AiOp::Plan;
        const Literal* goal = state.graph->property(node, Name::intern("goal"));
        Array<WorldState>& goals = BehaviourProgramAccess::goals(program);
        instruction.param = static_cast<u16>(goals.size());
        if (Status pushed = goals.push_back(goal != nullptr ? goal->value.mask : 0U); !pushed) {
            return pushed;
        }
    } else {
        Diagnostic diagnostic;
        diagnostic.node = node;
        diagnostic.detail = authored->type;
        diagnostic.message = "this node is not a behaviour node and cannot be compiled into one";
        state.sink->report(diagnostic);
        instruction.op = AiOp::Condition;
    }

    // The response curve a utility child carries, if any.
    const Literal* curve_kind = state.graph->property(node, Name::intern("curve"));
    if (curve_kind != nullptr) {
        ResponseCurve curve;
        const std::string_view kind = curve_kind->text.text();
        curve.kind = CurveKind::Linear;
        if (kind == "quadratic") {
            curve.kind = CurveKind::Quadratic;
        } else if (kind == "logistic") {
            curve.kind = CurveKind::Logistic;
        } else if (kind == "inverse") {
            curve.kind = CurveKind::Inverse;
        }
        const Literal* hysteresis = state.graph->property(node, Name::intern("hysteresis"));
        curve.hysteresis = hysteresis != nullptr ? hysteresis->value.x : 0.0F;
        Array<ResponseCurve>& curves = BehaviourProgramAccess::curves(program);
        instruction.curve = static_cast<u16>(curves.size());
        if (Status pushed = curves.push_back(curve); !pushed) {
            return pushed;
        }
    }
    if (task != nullptr && instruction.op != AiOp::Task && instruction.op != AiOp::Condition) {
        instruction.task = intern_task(program, task->text);
    }

    BehaviourProgramAccess::code(program)[slot] = instruction;
    return BehaviourProgramAccess::debug(program).record(slot, node);
}

void finish_digest(BehaviourProgram& program) noexcept {
    u64 digest = hash_u64(kHashSeed, program.code().size());
    for (const AiInstruction& instruction : program.code()) {
        digest = hash_u64(digest, static_cast<u64>(instruction.op));
        digest = hash_u64(
            digest, (static_cast<u64>(instruction.first_child) << 16U) | instruction.child_count);
        digest = hash_u64(digest, (static_cast<u64>(instruction.task) << 16U) | instruction.param);
    }
    for (const Name task : program.tasks()) {
        digest = hash_text(digest, task.text());
    }
    for (const PlanOperator& op : program.operators()) {
        digest = hash_u64(digest, op.requires_true);
        digest = hash_u64(digest, op.sets_true);
    }
    BehaviourProgramAccess::set_digest(program, digest);
}

}  // namespace

Status register_behaviour_nodes(NodeRegistry& registry) noexcept {
    const PinDesc composite[] = {pin("children", "behaviour", PinDirection::Input, true),
                                 pin("node", "behaviour", PinDirection::Output)};
    for (const char* type : {"ai.sequence", "ai.selector", "ai.parallel", "ai.utility"}) {
        if (Status added = register_node(registry, type, Span<const PinDesc>(composite, 2));
            !added) {
            return added;
        }
    }
    const PinDesc decorator[] = {pin("child", "behaviour", PinDirection::Input),
                                 pin("node", "behaviour", PinDirection::Output)};
    for (const char* type : {"ai.inverter", "ai.state"}) {
        if (Status added = register_node(registry, type, Span<const PinDesc>(decorator, 2));
            !added) {
            return added;
        }
    }
    const PinDesc leaf[] = {pin("node", "behaviour", PinDirection::Output)};
    for (const char* type : {"ai.task", "ai.condition", "ai.wait", "ai.plan"}) {
        if (Status added = register_node(registry, type, Span<const PinDesc>(leaf, 1)); !added) {
            return added;
        }
    }
    const PinDesc root[] = {pin("child", "behaviour", PinDirection::Input)};
    if (Status added = register_node(registry, "ai.root", Span<const PinDesc>(root, 1)); !added) {
        return added;
    }
    const PinDesc op[] = {pin("operator", "plan_operator", PinDirection::Output)};
    return register_node(registry, "ai.operator", Span<const PinDesc>(op, 1));
}

Expected<BehaviourProgram, Error> compile_behaviour(const Graph& graph,
                                                    const NodeRegistry& /*registry*/,
                                                    DiagnosticSink& sink) noexcept {
    BehaviourProgram program(graph.allocator());
    BehaviourProgramAccess::set_name(program, graph.name());
    // A default curve, so a utility child that names none still has one.
    if (Status pushed = BehaviourProgramAccess::curves(program).push_back(ResponseCurve{});
        !pushed) {
        return make_unexpected(pushed.error());
    }

    // The planning operators first: they are a table rather than a tree, and the search reads them
    // by index.
    for (const GraphNode& node : graph.nodes()) {
        if (node.type != Name::intern("ai.operator")) {
            continue;
        }
        PlanOperator op;
        op.origin = node.key;
        const Literal* task = graph.property(node.key, Name::intern("task"));
        op.task = intern_task(program, task != nullptr ? task->text : Name{});
        const Literal* requires_true = graph.property(node.key, Name::intern("requires"));
        op.requires_true = requires_true != nullptr ? requires_true->value.mask : 0U;
        const Literal* sets_true = graph.property(node.key, Name::intern("sets"));
        op.sets_true = sets_true != nullptr ? sets_true->value.mask : 0U;
        const Literal* cost = graph.property(node.key, Name::intern("cost"));
        op.cost = cost != nullptr ? cost->value.x : 1.0F;
        if (Status pushed = BehaviourProgramAccess::operators(program).push_back(op); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    NodeKey root = kInvalidNodeKey;
    for (const GraphNode& node : graph.nodes()) {
        if (node.type == Name::intern("ai.root")) {
            root = node.key;
        }
    }
    if (root == kInvalidNodeKey) {
        return make_unexpected(
            Error{ErrorCode::NotFound, "a behaviour graph needs an `ai.root` node", 0});
    }
    Array<NodeKey> children(graph.allocator());
    if (Status collected = collect_children(graph, root, children); !collected) {
        return make_unexpected(collected.error());
    }
    if (children.empty()) {
        return make_unexpected(
            Error{ErrorCode::NotFound, "the behaviour root has nothing wired into it", 0});
    }

    Lowering state;
    state.graph = &graph;
    state.program = &program;
    state.sink = &sink;
    if (Status pushed = BehaviourProgramAccess::code(program).push_back(AiInstruction{}); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Status lowered = lower_into(state, 0, children[0]); !lowered) {
        return make_unexpected(lowered.error());
    }
    BehaviourProgramAccess::set_root(program, 0);

    u16 blackboard = 0;
    for (const AiInstruction& instruction : program.code()) {
        blackboard = instruction.blackboard >= blackboard
                         ? static_cast<u16>(instruction.blackboard + 1)
                         : blackboard;
    }
    BehaviourProgramAccess::set_blackboard(program, blackboard);
    finish_digest(program);
    return program;
}

}  // namespace cy::graph::behaviour

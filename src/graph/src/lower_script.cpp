// The CyberGraph IR: the compiler, the shared program, and the register machine. Tasks 2.4, 2.5.
//
// See lower_script.h for why this is not the shared expression core and for what "compiled, not
// interpreted" means concretely.

#include <cy/graph/lower_script.h>

#include <utility>

namespace cy::graph::script {

/// Write access to a `ScriptProgram`, which has none in public.
///
/// The header declares this a friend because a compiled program is SHARED BY EVERY INSTANCE at run
/// time and must be immutable there; the compiler is the one thing that fills one in, and it says
/// so by having to name this class to do it.
class ProgramBuilder {
public:
    [[nodiscard]] static Array<Instruction>& code(ScriptProgram& program) noexcept {
        return program.code_;
    }
    [[nodiscard]] static Array<BasicBlock>& blocks(ScriptProgram& program) noexcept {
        return program.blocks_;
    }
    [[nodiscard]] static Array<Value>& constants(ScriptProgram& program) noexcept {
        return program.constants_;
    }
    [[nodiscard]] static Array<AccessDecl>& accesses(ScriptProgram& program) noexcept {
        return program.accesses_;
    }
    [[nodiscard]] static Array<StateSlot>& state_slots(ScriptProgram& program) noexcept {
        return program.state_;
    }
    [[nodiscard]] static Array<SuspendPoint>& suspends(ScriptProgram& program) noexcept {
        return program.suspends_;
    }
    [[nodiscard]] static Array<ExternalRef>& externals(ScriptProgram& program) noexcept {
        return program.externals_;
    }
    [[nodiscard]] static DebugMap& debug(ScriptProgram& program) noexcept { return program.debug_; }
    static void set_registers(ScriptProgram& program, u32 count) noexcept {
        program.registers_ = count;
    }
    static void set_entry(ScriptProgram& program, BlockId block) noexcept {
        program.entry_ = block;
    }
    static void set_name(ScriptProgram& program, Name name) noexcept { program.name_ = name; }
    static void set_digest(ScriptProgram& program, u64 digest) noexcept {
        program.digest_ = digest;
    }

    [[nodiscard]] static Expected<ScriptProgram, Error> build(const Graph& graph,
                                                              const NodeRegistry& registry,
                                                              Span<const NodeKey> entries,
                                                              Array<BlockId>& entry_blocks,
                                                              DiagnosticSink& sink) noexcept;
};

namespace {

[[nodiscard]] Error invalid(const char* message) noexcept {
    return Error{ErrorCode::InvalidArgument, message, 0};
}

struct OpInfo {
    const char* name;
    bool terminator;
};

constexpr OpInfo kOps[] = {
    {"load_const", false}, {"move", false},       {"add_float", false},    {"sub_float", false},
    {"mul_float", false},  {"div_float", false},  {"add_int", false},      {"sub_int", false},
    {"less_float", false}, {"less_int", false},   {"equal_int", false},    {"not_bool", false},
    {"and_bool", false},   {"or_bool", false},    {"get_field", false},    {"set_field", false},
    {"call", false},       {"emit_event", false}, {"emit_command", false}, {"query", false},
    {"branch_if", true},   {"jump", true},        {"suspend", true},       {"return", true},
};
static_assert(sizeof(kOps) / sizeof(kOps[0]) == static_cast<usize>(ScriptOp::Count));

}  // namespace

const char* value_kind_name(ValueKind kind) noexcept {
    switch (kind) {
        case ValueKind::Void:
            return "void";
        case ValueKind::Bool:
            return "bool";
        case ValueKind::Int:
            return "int";
        case ValueKind::Float:
            return "float";
        case ValueKind::Vec3:
            return "vec3";
        case ValueKind::Entity:
            return "entity";
        case ValueKind::PersistentRef:
            return "persistent_ref";
        case ValueKind::AssetHandle:
            return "asset_handle";
        case ValueKind::GameplayTag:
            return "gameplay_tag";
        case ValueKind::Identifier:
            return "identifier";
        case ValueKind::Struct:
            return "struct";
        case ValueKind::Array:
            return "array";
        case ValueKind::Optional:
            return "optional";
        case ValueKind::Tick:
            return "tick";
        case ValueKind::Count:
            break;
    }
    return "?";
}

const char* script_op_name(ScriptOp op) noexcept {
    return op < ScriptOp::Count ? kOps[static_cast<usize>(op)].name : "?";
}

bool is_terminator(ScriptOp op) noexcept {
    return op < ScriptOp::Count && kOps[static_cast<usize>(op)].terminator;
}

const char* ability_stage_name(AbilityStage stage) noexcept {
    switch (stage) {
        case AbilityStage::ResolveOwner:
            return "resolve_owner";
        case AbilityStage::CheckState:
            return "check_state";
        case AbilityStage::CheckCost:
            return "check_cost";
        case AbilityStage::CheckCooldown:
            return "check_cooldown";
        case AbilityStage::ResolveTarget:
            return "resolve_target";
        case AbilityStage::PredictionPolicy:
            return "prediction_policy";
        case AbilityStage::Commit:
            return "commit";
        case AbilityStage::ApplyEffects:
            return "apply_effects";
        case AbilityStage::EmitCues:
            return "emit_cues";
        case AbilityStage::Count:
            break;
    }
    return "?";
}

bool stage_is_check(AbilityStage stage) noexcept {
    return stage < AbilityStage::Commit;
}

ScriptProgram::ScriptProgram(Allocator& allocator) noexcept
    : code_(allocator),
      blocks_(allocator),
      constants_(allocator),
      accesses_(allocator),
      state_(allocator),
      suspends_(allocator),
      externals_(allocator),
      debug_(allocator) {}

ScriptState::ScriptState(Allocator& allocator, const ScriptProgram& program) noexcept
    : registers_(allocator), persisted_(allocator) {
    (void)registers_.resize(program.register_count());
    for (Value& value : registers_) {
        value = Value{};
    }
}

Status ScriptState::persist(const ScriptProgram& program) noexcept {
    // THE COMPACT STATE: the live registers, not the register file.
    persisted_.clear();
    for (const StateSlot& slot : program.state_slots()) {
        if (slot.reg >= registers_.size()) {
            continue;
        }
        if (Status pushed = persisted_.push_back(registers_[slot.reg]); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status ScriptState::restore(const ScriptProgram& program) noexcept {
    usize index = 0;
    for (const StateSlot& slot : program.state_slots()) {
        if (slot.reg >= registers_.size() || index >= persisted_.size()) {
            break;
        }
        registers_[slot.reg] = persisted_[index++];
    }
    return ok();
}

// --- The register machine -----------------------------------------------------------------------

namespace {

void step_arithmetic(const Instruction& instruction, Span<Value> registers,
                     Span<const Value> constants) noexcept {
    Value& dst = registers[instruction.dst];
    const Value& a = instruction.a != kNoRegister ? registers[instruction.a] : Value{};
    const Value& b = instruction.b != kNoRegister ? registers[instruction.b] : Value{};
    switch (instruction.op) {
        case ScriptOp::LoadConst:
            dst = constants[instruction.immediate];
            return;
        case ScriptOp::Move:
            dst = a;
            return;
        case ScriptOp::AddFloat:
            dst = Value::from_float(a.x + b.x);
            return;
        case ScriptOp::SubFloat:
            dst = Value::from_float(a.x - b.x);
            return;
        case ScriptOp::MulFloat:
            dst = Value::from_float(a.x * b.x);
            return;
        case ScriptOp::DivFloat:
            dst = Value::from_float(b.x == 0.0F ? 0.0F : a.x / b.x);
            return;
        case ScriptOp::AddInt:
            dst = Value::from_int(a.integer + b.integer);
            return;
        case ScriptOp::SubInt:
            dst = Value::from_int(a.integer - b.integer);
            return;
        case ScriptOp::LessFloat:
            dst = Value::from_bool(a.x < b.x);
            return;
        case ScriptOp::LessInt:
            dst = Value::from_bool(a.integer < b.integer);
            return;
        case ScriptOp::EqualInt:
            dst = Value::from_bool(a.integer == b.integer);
            return;
        case ScriptOp::NotBool:
            dst = Value::from_bool(a.integer == 0);
            return;
        case ScriptOp::AndBool:
            dst = Value::from_bool(a.integer != 0 && b.integer != 0);
            return;
        case ScriptOp::OrBool:
            dst = Value::from_bool(a.integer != 0 || b.integer != 0);
            return;
        default:
            return;
    }
}

void step_external(const ScriptProgram& program, const Instruction& instruction,
                   Span<Value> registers, ScriptHost& host) noexcept {
    const ExternalRef& external = program.externals()[instruction.immediate];
    const Span<const Value> arguments(
        instruction.a == kNoRegister ? nullptr : registers.data() + instruction.a,
        instruction.a == kNoRegister ? 0U : instruction.b);
    switch (instruction.op) {
        case ScriptOp::GetField:
            registers[instruction.dst] = host.get_field(external, registers[instruction.a]);
            return;
        case ScriptOp::SetField:
            host.set_field(external, registers[instruction.a], registers[instruction.b]);
            return;
        case ScriptOp::Call:
            registers[instruction.dst] = host.call(external, arguments);
            return;
        case ScriptOp::Query:
            registers[instruction.dst] = host.query(external, arguments);
            return;
        case ScriptOp::EmitEvent:
            host.emit_event(external, arguments);
            return;
        case ScriptOp::EmitCommand:
            host.emit_command(external, arguments);
            return;
        default:
            return;
    }
}

[[nodiscard]] bool is_external(ScriptOp op) noexcept {
    return op == ScriptOp::GetField || op == ScriptOp::SetField || op == ScriptOp::Call ||
           op == ScriptOp::Query || op == ScriptOp::EmitEvent || op == ScriptOp::EmitCommand;
}

}  // namespace

Expected<RunOutcome, Error> execute(const ScriptProgram& program, ScriptState& state,
                                    ScriptHost& host, u32 instruction_budget) noexcept {
    if (program.blocks().empty()) {
        return RunOutcome::Finished;
    }
    BlockId block = state.suspended() ? state.resume_block() : program.entry();
    if (state.suspended()) {
        if (Status restored = state.restore(program); !restored) {
            return make_unexpected(restored.error());
        }
        state.set_resume_block(kNoBlock);
    }

    u32 executed = 0;
    while (block != kNoBlock) {
        if (block >= program.blocks().size()) {
            return make_unexpected(invalid("this program jumps to a block that is not in it"));
        }
        const BasicBlock& current = program.blocks()[block];
        BlockId next = kNoBlock;
        for (u32 index = 0; index < current.count; ++index) {
            if (++executed > instruction_budget) {
                return RunOutcome::BudgetExhausted;
            }
            const Instruction& instruction = program.code()[current.first + index];
            if (is_external(instruction.op)) {
                step_external(program, instruction, state.registers(), host);
                continue;
            }
            if (!is_terminator(instruction.op)) {
                step_arithmetic(instruction, state.registers(), program.constants());
                continue;
            }
            switch (instruction.op) {
                case ScriptOp::Jump:
                    next = instruction.target;
                    break;
                case ScriptOp::BranchIf:
                    next = state.registers()[instruction.a].integer != 0 ? instruction.immediate
                                                                         : instruction.target;
                    break;
                case ScriptOp::Suspend: {
                    const SuspendPoint& point = program.suspends()[instruction.immediate];
                    if (host.wait_satisfied(point)) {
                        next = instruction.target;
                        break;
                    }
                    state.set_resume_block(instruction.target);
                    if (Status saved = state.persist(program); !saved) {
                        return make_unexpected(saved.error());
                    }
                    return RunOutcome::Suspended;
                }
                case ScriptOp::Return:
                default:
                    return RunOutcome::Finished;
            }
            break;
        }
        block = next;
    }
    return RunOutcome::Finished;
}

// --- Compilation ------------------------------------------------------------------------------

namespace {

/// The node types this compiler understands, and what each lowers to.
struct NodeLowering {
    const char* type;
    ScriptOp op;
    ValueKind kind;
    /// The data inputs, in operand order.
    const char* inputs[3];
    /// The property naming the external, if any.
    const char* external_property;
    /// True when the node sits on an execution chain rather than in a data expression.
    bool executes;
};

constexpr NodeLowering kLowerings[] = {
    {"script.const_float",
     ScriptOp::LoadConst,
     ValueKind::Float,
     {nullptr, nullptr, nullptr},
     nullptr,
     false},
    {"script.const_int",
     ScriptOp::LoadConst,
     ValueKind::Int,
     {nullptr, nullptr, nullptr},
     nullptr,
     false},
    {"script.const_bool",
     ScriptOp::LoadConst,
     ValueKind::Bool,
     {nullptr, nullptr, nullptr},
     nullptr,
     false},
    {"script.add_float", ScriptOp::AddFloat, ValueKind::Float, {"a", "b", nullptr}, nullptr, false},
    {"script.sub_float", ScriptOp::SubFloat, ValueKind::Float, {"a", "b", nullptr}, nullptr, false},
    {"script.mul_float", ScriptOp::MulFloat, ValueKind::Float, {"a", "b", nullptr}, nullptr, false},
    {"script.less_float",
     ScriptOp::LessFloat,
     ValueKind::Bool,
     {"a", "b", nullptr},
     nullptr,
     false},
    {"script.add_int", ScriptOp::AddInt, ValueKind::Int, {"a", "b", nullptr}, nullptr, false},
    {"script.not", ScriptOp::NotBool, ValueKind::Bool, {"a", nullptr, nullptr}, nullptr, false},
    {"script.get_field",
     ScriptOp::GetField,
     ValueKind::Float,
     {"subject", nullptr, nullptr},
     "field",
     false},
    {"script.query", ScriptOp::Query, ValueKind::Float, {"arg0", "arg1", nullptr}, "query", false},
    {"script.set_field",
     ScriptOp::SetField,
     ValueKind::Void,
     {"subject", "value", nullptr},
     "field",
     true},
    {"script.call", ScriptOp::Call, ValueKind::Float, {"arg0", "arg1", nullptr}, "function", true},
    {"script.emit_event",
     ScriptOp::EmitEvent,
     ValueKind::Void,
     {"arg0", "arg1", nullptr},
     "event",
     true},
    {"script.emit_command",
     ScriptOp::EmitCommand,
     ValueKind::Void,
     {"arg0", "arg1", nullptr},
     "command",
     true},
};

[[nodiscard]] const NodeLowering* lowering_of(Name type) noexcept {
    for (const NodeLowering& entry : kLowerings) {
        if (type.text() == entry.type) {
            return &entry;
        }
    }
    return nullptr;
}

/// The node the given execution output pin leads to.
[[nodiscard]] NodeKey exec_successor(const Graph& graph, NodeKey node, Name pin) noexcept {
    for (const Link& link : graph.links()) {
        if (link.from == node && link.from_pin == pin) {
            return link.to;
        }
    }
    return kInvalidNodeKey;
}

/// The node and pin wired into a data input.
struct DataSource {
    NodeKey node = kInvalidNodeKey;
    Name pin;
};

[[nodiscard]] DataSource data_source(const Graph& graph, NodeKey node, Name pin) noexcept {
    DataSource source;
    for (const Link& link : graph.links()) {
        if (link.to == node && link.to_pin == pin) {
            source.node = link.from;
            source.pin = link.from_pin;
        }
    }
    return source;
}

/// One compilation. Blocks are emitted one at a time and each is contiguous in `code_`, which is
/// what lets a block be `(first, count)` rather than a list of instructions.
class Compiler {
public:
    Compiler(const Graph& graph, const NodeRegistry& registry, DiagnosticSink& sink,
             ScriptProgram& program) noexcept
        : graph_(&graph),
          registry_(&registry),
          sink_(&sink),
          program_(&program),
          pending_(graph.allocator()),
          starts_(graph.allocator()),
          cached_(graph.allocator()) {}

    [[nodiscard]] Expected<BlockId, Error> chain_block(NodeKey start) noexcept;
    [[nodiscard]] Status run() noexcept;
    [[nodiscard]] Reg allocate() noexcept { return static_cast<Reg>(next_register_++); }
    [[nodiscard]] u32 registers() const noexcept { return next_register_; }
    void reserve(u32 count) noexcept {
        next_register_ = count > next_register_ ? count : next_register_;
    }

private:
    struct Pending {
        BlockId id = kNoBlock;
        NodeKey start = kInvalidNodeKey;
    };
    struct ChainStart {
        NodeKey node = kInvalidNodeKey;
        BlockId block = kNoBlock;
    };
    struct Cached {
        NodeKey node = kInvalidNodeKey;
        Reg reg = kNoRegister;
    };

    [[nodiscard]] Status compile_block(const Pending& pending) noexcept;
    [[nodiscard]] Status compile_exec_node(NodeKey node, const NodeLowering& lowering) noexcept;
    [[nodiscard]] Status compile_terminator(NodeKey node, Name type) noexcept;
    [[nodiscard]] Expected<Reg, Error> compile_data(NodeKey node) noexcept;
    [[nodiscard]] Expected<Reg, Error> operand_of(NodeKey node, const char* pin) noexcept;
    [[nodiscard]] Expected<Reg, Error> pack_arguments(NodeKey node, const NodeLowering& lowering,
                                                      u32& count) noexcept;
    [[nodiscard]] Expected<u32, Error> external_index(Name name, u32 arity) noexcept;
    [[nodiscard]] Expected<u32, Error> constant_index(const Value& value) noexcept;
    [[nodiscard]] Status emit(const Instruction& instruction, NodeKey origin) noexcept;
    [[nodiscard]] Status declare_access(Name resource, AccessMode mode) noexcept;
    void report(NodeKey node, Name pin, const char* message) noexcept;

    const Graph* graph_;
    const NodeRegistry* registry_;
    DiagnosticSink* sink_;
    ScriptProgram* program_;
    Array<Pending> pending_;
    Array<ChainStart> starts_;
    Array<Cached> cached_;
    u32 next_register_ = 0;
};

Expected<BlockId, Error> Compiler::chain_block(NodeKey start) noexcept {
    for (const ChainStart& existing : starts_) {
        if (existing.node == start) {
            return existing.block;
        }
    }
    const auto id = static_cast<BlockId>(ProgramBuilder::blocks(*program_).size());
    BasicBlock block;
    block.origin = start;
    if (Status pushed = ProgramBuilder::blocks(*program_).push_back(block); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Status pushed = starts_.push_back(ChainStart{start, id}); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Status pushed = pending_.push_back(Pending{id, start}); !pushed) {
        return make_unexpected(pushed.error());
    }
    return id;
}

Status Compiler::run() noexcept {
    // A worklist rather than recursion: a graph with a thousand branches must not need a thousand
    // stack frames, and the block order the worklist produces is deterministic.
    usize cursor = 0;
    while (cursor < pending_.size()) {
        const Pending pending = pending_[cursor++];
        if (Status compiled = compile_block(pending); !compiled) {
            return compiled;
        }
    }
    ProgramBuilder::set_registers(*program_, next_register_);
    return ok();
}

void Compiler::report(NodeKey node, Name pin, const char* message) noexcept {
    Diagnostic diagnostic;
    diagnostic.node = node;
    diagnostic.pin = pin;
    diagnostic.message = message;
    sink_->report(diagnostic);
}

Status Compiler::emit(const Instruction& instruction, NodeKey origin) noexcept {
    const auto location = static_cast<u32>(ProgramBuilder::code(*program_).size());
    if (Status pushed = ProgramBuilder::code(*program_).push_back(instruction); !pushed) {
        return pushed;
    }
    return ProgramBuilder::debug(*program_).record(location, origin);
}

Status Compiler::declare_access(Name resource, AccessMode mode) noexcept {
    for (const AccessDecl& existing : ProgramBuilder::accesses(*program_)) {
        if (existing.resource == resource && existing.mode == mode) {
            return ok();
        }
    }
    return ProgramBuilder::accesses(*program_).push_back(AccessDecl{resource, mode});
}

Expected<u32, Error> Compiler::external_index(Name name, u32 arity) noexcept {
    for (usize index = 0; index < ProgramBuilder::externals(*program_).size(); ++index) {
        if (ProgramBuilder::externals(*program_)[index].name == name) {
            return static_cast<u32>(index);
        }
    }
    const auto index = static_cast<u32>(ProgramBuilder::externals(*program_).size());
    if (Status pushed = ProgramBuilder::externals(*program_).push_back(ExternalRef{name, arity});
        !pushed) {
        return make_unexpected(pushed.error());
    }
    return index;
}

Expected<u32, Error> Compiler::constant_index(const Value& value) noexcept {
    for (usize index = 0; index < ProgramBuilder::constants(*program_).size(); ++index) {
        const Value& existing = ProgramBuilder::constants(*program_)[index];
        if (existing.integer == value.integer && existing.x == value.x && existing.y == value.y &&
            existing.z == value.z && existing.handle == value.handle) {
            return static_cast<u32>(index);
        }
    }
    const auto index = static_cast<u32>(ProgramBuilder::constants(*program_).size());
    if (Status pushed = ProgramBuilder::constants(*program_).push_back(value); !pushed) {
        return make_unexpected(pushed.error());
    }
    return index;
}

Expected<Reg, Error> Compiler::operand_of(NodeKey node, const char* pin) noexcept {
    const DataSource source = data_source(*graph_, node, Name::intern(pin));
    if (source.node == kInvalidNodeKey) {
        // An unwired data input is a zero of its kind. A compiler that refused would refuse every
        // graph an author is halfway through wiring.
        auto index = constant_index(Value{});
        if (!index) {
            return make_unexpected(index.error());
        }
        const Reg reg = allocate();
        Instruction load;
        load.op = ScriptOp::LoadConst;
        load.dst = reg;
        load.immediate = index.value();
        if (Status emitted = emit(load, node); !emitted) {
            return make_unexpected(emitted.error());
        }
        return reg;
    }
    return compile_data(source.node);
}

Expected<Reg, Error> Compiler::pack_arguments(NodeKey node, const NodeLowering& lowering,
                                              u32& count) noexcept {
    // A CONTIGUOUS ARGUMENT RUN. `Call`, `Query` and the two emissions pass their arguments as a
    // base register and a count, so the operands are copied into consecutive registers rather than
    // hoped to be adjacent — an operand wired from elsewhere in the graph already has a register,
    // and it is nowhere near the one beside it.
    Reg sources[3] = {kNoRegister, kNoRegister, kNoRegister};
    count = 0;
    for (const char* pin : lowering.inputs) {
        if (pin == nullptr) {
            break;
        }
        auto reg = operand_of(node, pin);
        if (!reg) {
            return reg;
        }
        sources[count++] = reg.value();
    }
    if (count == 0) {
        return kNoRegister;
    }
    const Reg base = allocate();
    for (u32 index = 1; index < count; ++index) {
        (void)allocate();
    }
    for (u32 index = 0; index < count; ++index) {
        Instruction move;
        move.op = ScriptOp::Move;
        move.dst = static_cast<Reg>(base + index);
        move.a = sources[index];
        if (Status emitted = emit(move, node); !emitted) {
            return make_unexpected(emitted.error());
        }
    }
    return base;
}

Expected<Reg, Error> Compiler::compile_data(NodeKey node) noexcept {
    for (const Cached& entry : cached_) {
        if (entry.node == node) {
            return entry.reg;
        }
    }
    const GraphNode* authored = graph_->find_node(node);
    if (authored == nullptr) {
        return make_unexpected(invalid("a wire names a node that is not in the graph"));
    }
    const NodeLowering* lowering = lowering_of(authored->type);
    if (lowering == nullptr || lowering->executes) {
        report(node, Name{}, "this node cannot produce a value on a data pin");
        return make_unexpected(invalid("a data pin is wired to a node that produces no value"));
    }

    Instruction instruction;
    instruction.op = lowering->op;
    instruction.kind = lowering->kind;
    if (lowering->op == ScriptOp::LoadConst) {
        const Literal* literal = graph_->property(node, Name::intern("value"));
        Value value;
        if (literal != nullptr) {
            value = lowering->kind == ValueKind::Float
                        ? Value::from_float(literal->value.x)
                        : Value::from_int(static_cast<i64>(literal->value.mask));
        }
        auto index = constant_index(value);
        if (!index) {
            return make_unexpected(index.error());
        }
        instruction.immediate = index.value();
    } else if (lowering->op == ScriptOp::Query) {
        u32 count = 0;
        auto base = pack_arguments(node, *lowering, count);
        if (!base) {
            return base;
        }
        instruction.a = base.value();
        instruction.b = static_cast<Reg>(count);
        const Literal* named = graph_->property(node, Name::intern(lowering->external_property));
        const Name external = named != nullptr ? named->text : Name{};
        auto index = external_index(external, count);
        if (!index) {
            return make_unexpected(index.error());
        }
        instruction.immediate = index.value();
        if (Status declared = declare_access(external, AccessMode::Read); !declared) {
            return make_unexpected(declared.error());
        }
    } else {
        Reg operands[3] = {kNoRegister, kNoRegister, kNoRegister};
        u32 count = 0;
        for (const char* pin : lowering->inputs) {
            if (pin == nullptr) {
                break;
            }
            auto reg = operand_of(node, pin);
            if (!reg) {
                return reg;
            }
            operands[count++] = reg.value();
        }
        instruction.a = operands[0];
        instruction.b = operands[1];
        if (lowering->external_property != nullptr) {
            const Literal* named =
                graph_->property(node, Name::intern(lowering->external_property));
            const Name external = named != nullptr ? named->text : Name{};
            auto index = external_index(external, count);
            if (!index) {
                return make_unexpected(index.error());
            }
            instruction.immediate = index.value();
            if (Status declared = declare_access(external, AccessMode::Read); !declared) {
                return make_unexpected(declared.error());
            }
        }
    }
    instruction.dst = allocate();
    if (Status emitted = emit(instruction, node); !emitted) {
        return make_unexpected(emitted.error());
    }
    if (Status pushed = cached_.push_back(Cached{node, instruction.dst}); !pushed) {
        return make_unexpected(pushed.error());
    }
    return instruction.dst;
}

Status Compiler::compile_exec_node(NodeKey node, const NodeLowering& lowering) noexcept {
    const bool packed = lowering.op == ScriptOp::Call || lowering.op == ScriptOp::EmitEvent ||
                        lowering.op == ScriptOp::EmitCommand;
    Reg operands[3] = {kNoRegister, kNoRegister, kNoRegister};
    u32 count = 0;
    if (packed) {
        auto base = pack_arguments(node, lowering, count);
        if (!base) {
            return make_unexpected(base.error());
        }
        operands[0] = base.value();
    } else {
        for (const char* pin : lowering.inputs) {
            if (pin == nullptr) {
                break;
            }
            auto reg = operand_of(node, pin);
            if (!reg) {
                return make_unexpected(reg.error());
            }
            operands[count++] = reg.value();
        }
    }
    Instruction instruction;
    instruction.op = lowering.op;
    instruction.kind = lowering.kind;
    instruction.a = operands[0];
    instruction.b = lowering.op == ScriptOp::SetField ? operands[1] : static_cast<Reg>(count);
    if (lowering.external_property != nullptr) {
        const Literal* named = graph_->property(node, Name::intern(lowering.external_property));
        const Name external = named != nullptr ? named->text : Name{};
        auto index = external_index(external, count);
        if (!index) {
            return make_unexpected(index.error());
        }
        instruction.immediate = index.value();
        const AccessMode mode =
            lowering.op == ScriptOp::SetField || lowering.op == ScriptOp::EmitCommand
                ? AccessMode::Write
                : AccessMode::Read;
        if (Status declared = declare_access(external, mode); !declared) {
            return declared;
        }
    }
    if (lowering.kind != ValueKind::Void) {
        instruction.dst = allocate();
        if (Status pushed = cached_.push_back(Cached{node, instruction.dst}); !pushed) {
            return pushed;
        }
    }
    return emit(instruction, node);
}

Status Compiler::compile_terminator(NodeKey node, Name type) noexcept {
    Instruction instruction;
    if (type.text() == "script.branch" || type.text() == "script.loop") {
        auto condition = operand_of(node, "condition");
        if (!condition) {
            return make_unexpected(condition.error());
        }
        const NodeKey then_node = exec_successor(
            *graph_, node, Name::intern(type.text() == "script.loop" ? "body" : "then"));
        const NodeKey else_node = exec_successor(
            *graph_, node, Name::intern(type.text() == "script.loop" ? "done" : "else"));
        instruction.op = ScriptOp::BranchIf;
        instruction.a = condition.value();
        if (then_node != kInvalidNodeKey) {
            auto block = chain_block(then_node);
            if (!block) {
                return make_unexpected(block.error());
            }
            instruction.immediate = block.value();
        } else {
            instruction.immediate = kNoBlock;
        }
        if (else_node != kInvalidNodeKey) {
            auto block = chain_block(else_node);
            if (!block) {
                return make_unexpected(block.error());
            }
            instruction.target = block.value();
        } else {
            instruction.target = kNoBlock;
        }
        return emit(instruction, node);
    }
    if (type.text() == "script.wait") {
        const Literal* reason = graph_->property(node, Name::intern("reason"));
        SuspendPoint point;
        point.reason = reason != nullptr ? reason->text : Name{};
        point.origin = node;
        const NodeKey resume_node = exec_successor(*graph_, node, Name::intern("then"));
        BlockId resume = kNoBlock;
        if (resume_node != kInvalidNodeKey) {
            auto block = chain_block(resume_node);
            if (!block) {
                return make_unexpected(block.error());
            }
            resume = block.value();
        }
        point.resume = resume;
        const auto index = static_cast<u32>(ProgramBuilder::suspends(*program_).size());
        if (Status pushed = ProgramBuilder::suspends(*program_).push_back(point); !pushed) {
            return pushed;
        }
        instruction.op = ScriptOp::Suspend;
        instruction.immediate = index;
        instruction.target = resume;
        return emit(instruction, node);
    }
    if (type.text() == "ability.refuse") {
        // The verdict and its reason, then a return. The reason's index is the position of this
        // node among the graph's refuse nodes, and `compile_ability` builds its reason table by the
        // same scan, so the two cannot drift.
        i64 reason_index = -1;
        i64 seen = 0;
        for (const GraphNode& candidate : graph_->nodes()) {
            if (candidate.type != type) {
                continue;
            }
            if (candidate.key == node) {
                reason_index = seen;
            }
            ++seen;
        }
        auto verdict = constant_index(Value::from_bool(false));
        if (!verdict) {
            return make_unexpected(verdict.error());
        }
        auto reason = constant_index(Value::from_int(reason_index));
        if (!reason) {
            return make_unexpected(reason.error());
        }
        Instruction load_verdict;
        load_verdict.op = ScriptOp::LoadConst;
        load_verdict.kind = ValueKind::Bool;
        load_verdict.dst = kVerdictRegister;
        load_verdict.immediate = verdict.value();
        if (Status emitted = emit(load_verdict, node); !emitted) {
            return emitted;
        }
        Instruction load_reason;
        load_reason.op = ScriptOp::LoadConst;
        load_reason.kind = ValueKind::Int;
        load_reason.dst = kReasonRegister;
        load_reason.immediate = reason.value();
        if (Status emitted = emit(load_reason, node); !emitted) {
            return emitted;
        }
    }
    instruction.op = ScriptOp::Return;
    return emit(instruction, node);
}

Status Compiler::compile_block(const Pending& pending) noexcept {
    cached_.clear();
    const auto first = static_cast<u32>(ProgramBuilder::code(*program_).size());
    NodeKey node = pending.start;
    while (node != kInvalidNodeKey) {
        const GraphNode* authored = graph_->find_node(node);
        if (authored == nullptr) {
            break;
        }
        if (authored->muted) {
            // A muted node contributes nothing and does not break the chain: an editor still shows
            // it, and the author expects the wire through it to keep working.
            node = exec_successor(*graph_, node, Name::intern("then"));
            continue;
        }
        const NodeLowering* lowering = lowering_of(authored->type);
        if (lowering != nullptr && lowering->executes) {
            if (Status compiled = compile_exec_node(node, *lowering); !compiled) {
                return compiled;
            }
            node = exec_successor(*graph_, node, Name::intern("then"));
            continue;
        }
        if (registry_->find(authored->type) == nullptr) {
            report(node, Name{},
                   "this node's type is not registered, so it cannot be compiled; the graph keeps "
                   "it and the program does not");
            node = exec_successor(*graph_, node, Name::intern("then"));
            continue;
        }
        // Anything else on an execution chain is a terminator: a branch, a loop, a wait, a return.
        if (Status compiled = compile_terminator(node, authored->type); !compiled) {
            return compiled;
        }
        ProgramBuilder::blocks(*program_)[pending.id].first = first;
        ProgramBuilder::blocks(*program_)[pending.id].count =
            static_cast<u32>(ProgramBuilder::code(*program_).size()) - first;
        return ok();
    }
    // A chain that simply ran out is a return.
    Instruction instruction;
    instruction.op = ScriptOp::Return;
    if (Status emitted = emit(instruction, pending.start); !emitted) {
        return emitted;
    }
    ProgramBuilder::blocks(*program_)[pending.id].first = first;
    ProgramBuilder::blocks(*program_)[pending.id].count =
        static_cast<u32>(ProgramBuilder::code(*program_).size()) - first;
    return ok();
}

/// Registers read in a block before anything wrote them. The union of these over every block
/// reachable from a resume point is a SOUND OVER-APPROXIMATION of what must survive a suspension —
/// sound because a register live at the resume point is upward-exposed in some reachable block, and
/// an approximation because it ignores which path was taken.
[[nodiscard]] Status upward_exposed(const ScriptProgram& program, BlockId block,
                                    Array<u8>& reads) noexcept {
    Array<u8> written(program.allocator());
    if (Status sized = written.resize(program.register_count()); !sized) {
        return sized;
    }
    for (u8& mark : written) {
        mark = 0;
    }
    const BasicBlock& current = program.blocks()[block];
    const auto note_read = [&](Reg reg) noexcept {
        if (reg != kNoRegister && reg < reads.size() && written[reg] == 0) {
            reads[reg] = 1;
        }
    };
    for (u32 index = 0; index < current.count; ++index) {
        const Instruction& instruction = program.code()[current.first + index];
        note_read(instruction.a);
        if (instruction.op == ScriptOp::SetField || instruction.op == ScriptOp::AddFloat ||
            instruction.op == ScriptOp::SubFloat || instruction.op == ScriptOp::MulFloat ||
            instruction.op == ScriptOp::DivFloat || instruction.op == ScriptOp::AddInt ||
            instruction.op == ScriptOp::SubInt || instruction.op == ScriptOp::LessFloat ||
            instruction.op == ScriptOp::LessInt || instruction.op == ScriptOp::EqualInt ||
            instruction.op == ScriptOp::AndBool || instruction.op == ScriptOp::OrBool) {
            note_read(instruction.b);
        }
        if (instruction.dst != kNoRegister && instruction.dst < written.size()) {
            written[instruction.dst] = 1;
        }
    }
    return ok();
}

[[nodiscard]] Status compute_state_slots(ScriptProgram& program) noexcept {
    Allocator& allocator = program.allocator();
    Array<u8> live(allocator);
    Array<u8> seen(allocator);
    Array<BlockId> stack(allocator);
    if (Status sized = live.resize(program.register_count()); !sized) {
        return sized;
    }
    for (u8& mark : live) {
        mark = 0;
    }
    for (const SuspendPoint& point : program.suspends()) {
        if (point.resume == kNoBlock) {
            continue;
        }
        if (Status sized = seen.resize(program.blocks().size()); !sized) {
            return sized;
        }
        for (u8& mark : seen) {
            mark = 0;
        }
        if (Status pushed = stack.push_back(point.resume); !pushed) {
            return pushed;
        }
        while (!stack.empty()) {
            const BlockId block = stack.back();
            stack.pop_back();
            if (block >= program.blocks().size() || seen[block] != 0) {
                continue;
            }
            seen[block] = 1;
            if (Status exposed = upward_exposed(program, block, live); !exposed) {
                return exposed;
            }
            const BasicBlock& current = program.blocks()[block];
            if (current.count == 0) {
                continue;
            }
            const Instruction& terminator = program.code()[current.first + current.count - 1];
            for (const u32 successor : {terminator.immediate, terminator.target}) {
                const bool branches = terminator.op == ScriptOp::BranchIf ||
                                      terminator.op == ScriptOp::Jump ||
                                      terminator.op == ScriptOp::Suspend;
                if (branches && successor < program.blocks().size()) {
                    if (Status pushed = stack.push_back(successor); !pushed) {
                        return pushed;
                    }
                }
            }
        }
    }
    for (usize reg = 0; reg < live.size(); ++reg) {
        if (live[reg] != 0) {
            if (Status pushed = ProgramBuilder::state_slots(program).push_back(
                    StateSlot{static_cast<Reg>(reg), ValueKind::Void});
                !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

void finish_digest(ScriptProgram& program) noexcept {
    u64 digest = hash_u64(kHashSeed, program.code().size());
    for (const Instruction& instruction : program.code()) {
        digest = hash_u64(digest, static_cast<u64>(instruction.op));
        digest = hash_u64(digest, static_cast<u64>(instruction.kind));
        digest = hash_u64(digest, (static_cast<u64>(instruction.dst) << 32U) |
                                      (static_cast<u64>(instruction.a) << 16U) | instruction.b);
        digest =
            hash_u64(digest, (static_cast<u64>(instruction.immediate) << 32U) | instruction.target);
    }
    for (const Value& constant : program.constants()) {
        digest = hash_constant(digest, constant);
    }
    for (const ExternalRef& external : program.externals()) {
        digest = hash_text(digest, external.name.text());
    }
    ProgramBuilder::set_digest(program, digest);
}

/// The node types both compilers share.
constexpr PinDesc kExecIn{};

[[nodiscard]] Status register_node(NodeRegistry& registry, const char* type,
                                   Span<const PinDesc> pins, bool pure) noexcept {
    NodeTypeDesc desc;
    desc.name = Name::intern(type);
    desc.plugin = Name::intern("cy.graph.script");
    desc.pins = pins;
    desc.pure = pure;
    return registry.register_type(desc);
}

[[nodiscard]] PinDesc pin(const char* name, const char* type, PinDirection direction,
                          bool execution = false) noexcept {
    PinDesc desc;
    desc.name = Name::intern(name);
    desc.type = Name::intern(type);
    desc.direction = direction;
    desc.execution = execution;
    return desc;
}

}  // namespace

Expected<ScriptProgram, Error> ProgramBuilder::build(const Graph& graph,
                                                     const NodeRegistry& registry,
                                                     Span<const NodeKey> entries,
                                                     Array<BlockId>& entry_blocks,
                                                     DiagnosticSink& sink) noexcept {
    ScriptProgram program(graph.allocator());
    set_name(program, graph.name());
    Compiler compiler(graph, registry, sink, program);
    // Registers 0 and 1 are reserved for the ability pipeline's verdict and reason. Reserving them
    // in every program costs two slots and keeps one register numbering.
    compiler.reserve(2);
    for (const NodeKey entry : entries) {
        auto block = compiler.chain_block(entry);
        if (!block) {
            return make_unexpected(block.error());
        }
        if (Status pushed = entry_blocks.push_back(block.value()); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    if (Status compiled = compiler.run(); !compiled) {
        return make_unexpected(compiled.error());
    }
    if (Status computed = compute_state_slots(program); !computed) {
        return make_unexpected(computed.error());
    }
    set_entry(program, entry_blocks.empty() ? 0U : entry_blocks[0]);
    finish_digest(program);
    return program;
}

Status register_script_nodes(NodeRegistry& registry) noexcept {
    (void)kExecIn;
    const PinDesc entry_pins[] = {pin("then", "exec", PinDirection::Output, true)};
    if (Status added =
            register_node(registry, "script.entry", Span<const PinDesc>(entry_pins, 1), false);
        !added) {
        return added;
    }
    const PinDesc value_out[] = {pin("value", "float", PinDirection::Output)};
    for (const char* type : {"script.const_float", "script.const_int", "script.const_bool"}) {
        if (Status added = register_node(registry, type, Span<const PinDesc>(value_out, 1), true);
            !added) {
            return added;
        }
    }
    const PinDesc binary[] = {pin("a", "float", PinDirection::Input),
                              pin("b", "float", PinDirection::Input),
                              pin("value", "float", PinDirection::Output)};
    for (const char* type : {"script.add_float", "script.sub_float", "script.mul_float",
                             "script.less_float", "script.add_int"}) {
        if (Status added = register_node(registry, type, Span<const PinDesc>(binary, 3), true);
            !added) {
            return added;
        }
    }
    const PinDesc unary[] = {pin("a", "float", PinDirection::Input),
                             pin("value", "float", PinDirection::Output)};
    if (Status added = register_node(registry, "script.not", Span<const PinDesc>(unary, 2), true);
        !added) {
        return added;
    }
    const PinDesc get_field[] = {pin("subject", "entity", PinDirection::Input),
                                 pin("value", "float", PinDirection::Output)};
    if (Status added =
            register_node(registry, "script.get_field", Span<const PinDesc>(get_field, 2), false);
        !added) {
        return added;
    }
    const PinDesc query[] = {pin("arg0", "float", PinDirection::Input),
                             pin("arg1", "float", PinDirection::Input),
                             pin("value", "float", PinDirection::Output)};
    if (Status added =
            register_node(registry, "script.query", Span<const PinDesc>(query, 3), false);
        !added) {
        return added;
    }
    const PinDesc set_field[] = {pin("in", "exec", PinDirection::Input, true),
                                 pin("subject", "entity", PinDirection::Input),
                                 pin("value", "float", PinDirection::Input),
                                 pin("then", "exec", PinDirection::Output, true)};
    if (Status added =
            register_node(registry, "script.set_field", Span<const PinDesc>(set_field, 4), false);
        !added) {
        return added;
    }
    const PinDesc emit[] = {
        pin("in", "exec", PinDirection::Input, true), pin("arg0", "float", PinDirection::Input),
        pin("arg1", "float", PinDirection::Input), pin("then", "exec", PinDirection::Output, true)};
    for (const char* type : {"script.emit_event", "script.emit_command", "script.call"}) {
        if (Status added = register_node(registry, type, Span<const PinDesc>(emit, 4), false);
            !added) {
            return added;
        }
    }
    const PinDesc branch[] = {pin("in", "exec", PinDirection::Input, true),
                              pin("condition", "float", PinDirection::Input),
                              pin("then", "exec", PinDirection::Output, true),
                              pin("else", "exec", PinDirection::Output, true)};
    if (Status added =
            register_node(registry, "script.branch", Span<const PinDesc>(branch, 4), false);
        !added) {
        return added;
    }
    const PinDesc loop[] = {pin("in", "exec", PinDirection::Input, true),
                            pin("condition", "float", PinDirection::Input),
                            pin("body", "exec", PinDirection::Output, true),
                            pin("done", "exec", PinDirection::Output, true)};
    if (Status added = register_node(registry, "script.loop", Span<const PinDesc>(loop, 4), false);
        !added) {
        return added;
    }
    const PinDesc wait[] = {pin("in", "exec", PinDirection::Input, true),
                            pin("then", "exec", PinDirection::Output, true)};
    if (Status added = register_node(registry, "script.wait", Span<const PinDesc>(wait, 2), false);
        !added) {
        return added;
    }
    const PinDesc ret[] = {pin("in", "exec", PinDirection::Input, true)};
    return register_node(registry, "script.return", Span<const PinDesc>(ret, 1), false);
}

Expected<ScriptProgram, Error> compile_script(const Graph& graph, const NodeRegistry& registry,
                                              const ScriptCompileOptions& options,
                                              DiagnosticSink& sink) noexcept {
    const Name entry_type =
        options.entry_type == Name{} ? Name::intern("script.entry") : options.entry_type;
    NodeKey entry = kInvalidNodeKey;
    for (const GraphNode& node : graph.nodes()) {
        if (node.type == entry_type) {
            entry = node.key;
        }
    }
    if (entry == kInvalidNodeKey) {
        return make_unexpected(
            Error{ErrorCode::NotFound, "this graph has no node of the entry type", 0});
    }
    const NodeKey start = exec_successor(graph, entry, Name::intern("then"));
    Array<BlockId> entries(graph.allocator());
    const NodeKey roots[] = {start == kInvalidNodeKey ? entry : start};
    return ProgramBuilder::build(graph, registry, Span<const NodeKey>(roots, 1), entries, sink);
}

// --- The ability program ------------------------------------------------------------------------

AbilityProgram::AbilityProgram(ScriptProgram&& program) noexcept
    : program_(std::move(program)),
      reasons_(program_.allocator()),
      reason_nodes_(program_.allocator()) {
    for (BlockId& stage : stages_) {
        stage = kNoBlock;
    }
}

Status AbilityProgram::add_reason(Name reason, NodeKey node) noexcept {
    if (Status pushed = reasons_.push_back(reason); !pushed) {
        return pushed;
    }
    return reason_nodes_.push_back(node);
}

Status register_ability_nodes(NodeRegistry& registry) noexcept {
    const PinDesc stage_pins[] = {pin("then", "exec", PinDirection::Output, true)};
    NodeTypeDesc stage;
    stage.name = Name::intern("ability.stage");
    stage.plugin = Name::intern("cy.graph.script");
    stage.pins = Span<const PinDesc>(stage_pins, 1);
    stage.pure = false;
    if (Status added = registry.register_type(stage); !added) {
        return added;
    }
    const PinDesc refuse_pins[] = {pin("in", "exec", PinDirection::Input, true)};
    NodeTypeDesc refuse;
    refuse.name = Name::intern("ability.refuse");
    refuse.plugin = Name::intern("cy.graph.script");
    refuse.pins = Span<const PinDesc>(refuse_pins, 1);
    refuse.pure = false;
    return registry.register_type(refuse);
}

Expected<AbilityProgram, Error> compile_ability(const Graph& graph, const NodeRegistry& registry,
                                                DiagnosticSink& sink) noexcept {
    const Name stage_type = Name::intern("ability.stage");
    const Name stage_property = Name::intern("stage");
    Array<NodeKey> entries(graph.allocator());
    Array<u32> stage_of(graph.allocator());

    // THE ORDER IS THE SPECIFICATION'S, NOT THE GRAPH'S. Stages are collected in pipeline order
    // rather than in authoring order, so an ability whose author wired the cost check last still
    // pays it before the commit.
    for (u32 index = 0; index < static_cast<u32>(AbilityStage::Count); ++index) {
        const auto stage = static_cast<AbilityStage>(index);
        for (const GraphNode& node : graph.nodes()) {
            if (node.type != stage_type) {
                continue;
            }
            const Literal* named = graph.property(node.key, stage_property);
            if (named == nullptr || named->text.text() != ability_stage_name(stage)) {
                continue;
            }
            const NodeKey start = exec_successor(graph, node.key, Name::intern("then"));
            if (Status pushed = entries.push_back(start == kInvalidNodeKey ? node.key : start);
                !pushed) {
                return make_unexpected(pushed.error());
            }
            if (Status pushed = stage_of.push_back(index); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
    }

    Array<BlockId> blocks(graph.allocator());
    auto program = ProgramBuilder::build(graph, registry, entries.span(), blocks, sink);
    if (!program) {
        return make_unexpected(program.error());
    }
    AbilityProgram ability(std::move(program.value()));
    for (usize index = 0; index < stage_of.size() && index < blocks.size(); ++index) {
        ability.set_stage(static_cast<AbilityStage>(stage_of[index]), blocks[index]);
    }
    for (const GraphNode& node : graph.nodes()) {
        if (node.type != Name::intern("ability.refuse")) {
            continue;
        }
        const Literal* reason = graph.property(node.key, Name::intern("reason"));
        if (Status added = ability.add_reason(reason != nullptr ? reason->text : Name{}, node.key);
            !added) {
            return make_unexpected(added.error());
        }
    }
    return ability;
}

Expected<ValidationResult, Error> validate_activation(const AbilityProgram& ability,
                                                      ScriptState& state,
                                                      ScriptHost& host) noexcept {
    ValidationResult result;
    for (u32 index = 0; index < static_cast<u32>(AbilityStage::Count); ++index) {
        const auto stage = static_cast<AbilityStage>(index);
        // NOTHING FROM `Commit` ON RUNS. That is what "callable without activating" means, and it
        // is enforced here rather than trusted to the author's wiring.
        if (!stage_is_check(stage)) {
            break;
        }
        const BlockId block = ability.stage(stage);
        if (block == kNoBlock) {
            continue;
        }
        state.registers()[kVerdictRegister] = Value::from_bool(true);
        state.registers()[kReasonRegister] = Value::from_int(-1);
        state.set_resume_block(block);
        auto outcome = execute(ability.program(), state, host);
        if (!outcome) {
            return make_unexpected(outcome.error());
        }
        if (state.registers()[kVerdictRegister].integer != 0) {
            continue;
        }
        result.allowed = false;
        result.failed = stage;
        const auto reason = state.registers()[kReasonRegister].integer;
        if (reason >= 0 && static_cast<usize>(reason) < ability.reasons().size()) {
            result.reason = ability.reasons()[static_cast<usize>(reason)];
            result.node = ability.reason_nodes()[static_cast<usize>(reason)];
        }
        return result;
    }
    return result;
}

}  // namespace cy::graph::script

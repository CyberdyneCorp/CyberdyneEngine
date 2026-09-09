#pragma once
// The CyberGraph IR: basic blocks, control flow, calls, events, commands, queries and suspension
// points — and the two back ends its specification names. M8.b tasks 2.4 and 2.5.
//
// SERVES TWO CONSUMERS. `visual-scripting` names this IR, and `gameplay-abilities-and-effects`
// borrows it: "that spec SHALL additionally provide gameplay and ability graph languages, lowering
// to ECS systems and ability programs respectively". H1 and H2 of design.md §1.4 are one
// representation serving two consumers, which is why the ability program at the bottom of this file
// is a `ScriptProgram` plus a stage table rather than a second compiler.
//
// ================================================================================================
// WHY THIS IS NOT THE SHARED EXPRESSION CORE
// ================================================================================================
//
// Everything in the list above is a back edge, a write, or an ordering — and expr.h has none of
// those by construction. The spike measured each one (design.md §1.3, probes P3 to P7 and P10) and
// `visual-scripting`'s own "No universal representation" requirement had already ruled it out. So
// this is a different data structure: a typed register machine over basic blocks.
//
// ================================================================================================
// COMPILED, NOT INTERPRETED — AND THE DIFFERENCE IS NOT SUBTLE
// ================================================================================================
//
// `visual-scripting`: "A graph SHALL be a source representation compiled to an executable program.
// It SHALL NOT be the runtime object model", and the back end is "a typed register machine with a
// SHARED program and separate state. There SHALL NOT be one virtual machine instance per entity."
//
// So `ScriptProgram` is immutable, shared by every instance, and contains no graph. `ScriptState`
// is one instance's registers and program counter and contains no program. `execute()` takes both
// and allocates nothing. Nothing in this file walks a `Graph` at run time; `compile_script` is the
// only function that reads one, and it runs at cook time.
//
// ================================================================================================
// A SUSPENSION IS A STATE MACHINE WITH COMPACT STATE, NOT A COROUTINE
// ================================================================================================
//
// `visual-scripting`: "Asynchronous waits SHALL lower to an explicit state machine with compact
// generated state." `gameplay-abilities-and-effects` says the same of an ability's waits. What
// makes the state COMPACT is that only the registers LIVE ACROSS a suspension are persisted — the
// compiler works out which, and `ScriptProgram::state_slots` is that list. A design that persisted
// the whole register file would call itself compact and not be.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/graph/cybergraph.h>

namespace cy::graph::script {

/// A register. The program is typed statically, so a register's kind is a property of the
/// instruction that writes it rather than of the storage.
using Reg = u16;
inline constexpr Reg kNoRegister = 0xFFFFU;
using BlockId = u32;
inline constexpr BlockId kNoBlock = 0xFFFFFFFFU;

/// The pin types `visual-scripting` requires, as the register file stores them.
///
/// "A universal variant type SHALL NOT be the default pin type" is a statement about PINS. A
/// register file has to store something; what makes this typed rather than variant is that every
/// instruction names the kind it reads and the compiler checked it.
enum class ValueKind : u8 {
    Void = 0,
    Bool,
    Int,
    Float,
    Vec3,
    Entity,
    PersistentRef,
    AssetHandle,
    GameplayTag,
    Identifier,
    Struct,
    Array,
    Optional,
    /// `gameplay-abilities-and-effects`: "cooldowns SHALL be expressed as ticks, as a ready-tick
    /// value rather than a counting float timer".
    Tick,
    Count,
};

[[nodiscard]] const char* value_kind_name(ValueKind kind) noexcept;

/// One register's storage. Sixteen bytes, no allocation, no type tag: the program is typed.
struct Value {
    i64 integer = 0;
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 z = 0.0F;
    u64 handle = 0;

    [[nodiscard]] static Value from_float(f32 value) noexcept {
        Value result;
        result.x = value;
        return result;
    }
    [[nodiscard]] static Value from_int(i64 value) noexcept {
        Value result;
        result.integer = value;
        return result;
    }
    [[nodiscard]] static Value from_bool(bool value) noexcept { return from_int(value ? 1 : 0); }
};

enum class ScriptOp : u16 {
    /// `dst = constants[immediate]`.
    LoadConst = 0,
    Move,
    AddFloat,
    SubFloat,
    MulFloat,
    DivFloat,
    AddInt,
    SubInt,
    LessFloat,
    LessInt,
    EqualInt,
    NotBool,
    AndBool,
    OrBool,
    /// Typed field access: `dst = fields[immediate] of a`.
    GetField,
    /// `fields[immediate] of a = b`. A WRITE, which is why this IR is not an expression DAG.
    SetField,
    /// `dst = call(callees[immediate], registers[a .. a + b))`.
    Call,
    /// Emission, which is ordered and has no value. Two emissions in a stated order stay in it —
    /// probe P5's measurement of what an expression DAG does to that order is why.
    EmitEvent,
    EmitCommand,
    /// `dst = query(queries[immediate], registers[a .. a + b))`.
    Query,

    // --- Terminators. Exactly one ends every basic block. ---------------------------------------
    /// `a` is the condition; `immediate` is the then-block, `target` the else-block. A REAL branch:
    /// only one successor runs, which is the semantic an expression `select` cannot express.
    BranchIf,
    Jump,
    /// Save the live registers, record `immediate` as the resume point, and return to the
    /// scheduler. `target` is the block execution resumes in.
    Suspend,
    Return,
    Count,
};

[[nodiscard]] const char* script_op_name(ScriptOp op) noexcept;
[[nodiscard]] bool is_terminator(ScriptOp op) noexcept;

struct Instruction {
    ScriptOp op = ScriptOp::Return;
    ValueKind kind = ValueKind::Void;
    Reg dst = kNoRegister;
    Reg a = kNoRegister;
    Reg b = kNoRegister;
    u32 immediate = 0;
    u32 target = 0;
};

struct BasicBlock {
    u32 first = 0;
    u32 count = 0;
    /// The authoring node this block began at, for the debug map and for a breakpoint.
    NodeKey origin = kInvalidNodeKey;
};

/// What the scheduler needs to know before it runs this program.
///
/// `visual-scripting`: "Compilation emits the data access declaration the scheduler needs." A
/// program that reads and writes without declaring it cannot be scheduled in parallel with
/// anything, and a declaration written by hand beside the graph is a declaration that drifts.
enum class AccessMode : u8 { Read = 0, Write };

struct AccessDecl {
    Name resource;
    AccessMode mode = AccessMode::Read;
};

/// A register that must survive a suspension. The compact state is exactly this list.
struct StateSlot {
    Reg reg = kNoRegister;
    ValueKind kind = ValueKind::Void;
};

struct SuspendPoint {
    /// The block to resume in.
    BlockId resume = kNoBlock;
    /// Why the program is waiting: an event name, a tag, a duration's parameter.
    Name reason;
    /// The authoring node that asked to wait.
    NodeKey origin = kInvalidNodeKey;
};

/// An external name the program calls, emits or queries. Resolved by the host, never by the graph:
/// a program that looked a function up by string at run time would be a string-keyed node lookup,
/// which is on `visual-scripting`'s forbidden list.
struct ExternalRef {
    Name name;
    u32 arity = 0;
};

/// A compiled script. IMMUTABLE, SHARED BY EVERY INSTANCE, and containing no graph.
class ScriptProgram {
public:
    explicit ScriptProgram(Allocator& allocator) noexcept;

    ScriptProgram(const ScriptProgram&) = delete;
    ScriptProgram& operator=(const ScriptProgram&) = delete;
    ScriptProgram(ScriptProgram&&) noexcept = default;
    ScriptProgram& operator=(ScriptProgram&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] Span<const Instruction> code() const noexcept { return code_.span(); }
    [[nodiscard]] Span<const BasicBlock> blocks() const noexcept { return blocks_.span(); }
    [[nodiscard]] Span<const Value> constants() const noexcept { return constants_.span(); }
    [[nodiscard]] Span<const AccessDecl> accesses() const noexcept { return accesses_.span(); }
    [[nodiscard]] Span<const StateSlot> state_slots() const noexcept { return state_.span(); }
    [[nodiscard]] Span<const SuspendPoint> suspends() const noexcept { return suspends_.span(); }
    [[nodiscard]] Span<const ExternalRef> externals() const noexcept { return externals_.span(); }
    [[nodiscard]] u32 register_count() const noexcept { return registers_; }
    [[nodiscard]] BlockId entry() const noexcept { return entry_; }
    /// A content hash over the code, the blocks and the constants: the cook key's input.
    [[nodiscard]] u64 digest() const noexcept { return digest_; }
    /// Program location -> authoring node and pin. `visual-scripting`'s debug map.
    [[nodiscard]] const DebugMap& debug() const noexcept { return debug_; }

    [[nodiscard]] Allocator& allocator() const noexcept { return code_.allocator(); }

private:
    friend class ProgramBuilder;

    Name name_;
    Array<Instruction> code_;
    Array<BasicBlock> blocks_;
    Array<Value> constants_;
    Array<AccessDecl> accesses_;
    Array<StateSlot> state_;
    Array<SuspendPoint> suspends_;
    Array<ExternalRef> externals_;
    DebugMap debug_;
    u32 registers_ = 0;
    BlockId entry_ = 0;
    u64 digest_ = 0;
};

/// ONE INSTANCE'S STATE. No program, no graph, no virtual dispatch, and no allocation once sized.
///
/// `visual-scripting`: "There SHALL NOT be one virtual machine instance per entity." This is what
/// an entity carries instead: a register file and two integers.
class ScriptState {
public:
    ScriptState(Allocator& allocator, const ScriptProgram& program) noexcept;

    ScriptState(const ScriptState&) = delete;
    ScriptState& operator=(const ScriptState&) = delete;
    ScriptState(ScriptState&&) noexcept = default;
    ScriptState& operator=(ScriptState&&) noexcept = default;

    [[nodiscard]] Span<Value> registers() noexcept { return registers_.span(); }
    [[nodiscard]] Span<const Value> registers() const noexcept { return registers_.span(); }
    /// Where a suspended program resumes. `kNoBlock` when it has not suspended.
    [[nodiscard]] BlockId resume_block() const noexcept { return resume_; }
    void set_resume_block(BlockId block) noexcept { resume_ = block; }
    [[nodiscard]] bool suspended() const noexcept { return resume_ != kNoBlock; }
    /// The registers persisted across the last suspension. THE COMPACT STATE.
    [[nodiscard]] Span<const Value> persisted() const noexcept { return persisted_.span(); }
    [[nodiscard]] Status persist(const ScriptProgram& program) noexcept;
    [[nodiscard]] Status restore(const ScriptProgram& program) noexcept;

private:
    Array<Value> registers_;
    Array<Value> persisted_;
    BlockId resume_ = kNoBlock;
};

/// What the host supplies to a running program. Every external effect goes through it, which is
/// what makes the capability audit checkable rather than advisory.
class ScriptHost {
public:
    ScriptHost() = default;
    virtual ~ScriptHost() = default;
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;
    ScriptHost(ScriptHost&&) = delete;
    ScriptHost& operator=(ScriptHost&&) = delete;

    virtual Value call(const ExternalRef& callee, Span<const Value> arguments) = 0;
    virtual Value query(const ExternalRef& query, Span<const Value> arguments) = 0;
    virtual void emit_event(const ExternalRef& event, Span<const Value> arguments) = 0;
    virtual void emit_command(const ExternalRef& command, Span<const Value> arguments) = 0;
    virtual Value get_field(const ExternalRef& field, const Value& subject) = 0;
    virtual void set_field(const ExternalRef& field, const Value& subject, const Value& value) = 0;
    /// Whether the reason a `Suspend` named has been satisfied. Polled by the scheduler, never by
    /// the program.
    [[nodiscard]] virtual bool wait_satisfied(const SuspendPoint& point) = 0;
};

enum class RunOutcome : u8 {
    /// The program ran to a `Return`.
    Finished = 0,
    /// The program hit a `Suspend`; `state.resume_block()` says where it continues.
    Suspended,
    /// The instruction budget ran out. A program that loops forever is stopped rather than hanging
    /// the frame, and this is what the scheduler sees.
    BudgetExhausted,
};

/// Run one instance to completion, to a suspension, or to the budget.
///
/// ONE SHARED PROGRAM, ONE INSTANCE'S STATE. Calling this for eight thousand entities is eight
/// thousand calls with one `program` — not eight thousand machines.
[[nodiscard]] Expected<RunOutcome, Error> execute(const ScriptProgram& program, ScriptState& state,
                                                  ScriptHost& host,
                                                  u32 instruction_budget = 4096) noexcept;

// --- Compilation ------------------------------------------------------------------------------

struct ScriptCompileOptions {
    /// The node type that begins execution.
    Name entry_type;
    /// Fold constants and remove a `Move` whose source is never written again. Off is the bisection
    /// build: the same program compiled without the tidying, so a suspected miscompilation can be
    /// bisected.
    bool optimise = true;
};

/// Compile an authored graph into a shared program.
///
/// This is the ONLY function here that reads a `Graph`, and it runs at cook time. The node types it
/// understands are declared by `register_script_nodes`, and a graph may extend them with its own —
/// a node whose type carries a `script.op` property lowers to that instruction.
[[nodiscard]] Expected<ScriptProgram, Error> compile_script(const Graph& graph,
                                                            const NodeRegistry& registry,
                                                            const ScriptCompileOptions& options,
                                                            DiagnosticSink& sink) noexcept;

/// Register the node types `compile_script` understands. A domain adds its own beside them.
[[nodiscard]] Status register_script_nodes(NodeRegistry& registry) noexcept;

// --- The ability program ------------------------------------------------------------------------
//
// `gameplay-abilities-and-effects` states its pipeline as an ORDER: "resolve owner and context,
// check state and tag requirements, check cost, check cooldown, resolve and validate the target,
// apply the prediction and authority policy, commit the activation, apply effects, and emit cues
// and events". An order is not something an expression DAG can hold (probe P5), and it is the whole
// content of the requirement, so the stage table below is the program's spine.

enum class AbilityStage : u8 {
    ResolveOwner = 0,
    CheckState,
    CheckCost,
    CheckCooldown,
    ResolveTarget,
    PredictionPolicy,
    Commit,
    ApplyEffects,
    EmitCues,
    Count,
};

[[nodiscard]] const char* ability_stage_name(AbilityStage stage) noexcept;
/// The stages that may run without activating anything. Everything from `Commit` on may not.
[[nodiscard]] bool stage_is_check(AbilityStage stage) noexcept;

/// A structured validation result, callable WITHOUT activating. `gameplay-abilities-and-effects`
/// requires exactly that, and requires the answer to say which requirement failed rather than
/// returning false.
struct ValidationResult {
    bool allowed = true;
    AbilityStage failed = AbilityStage::Count;
    Name reason;
    /// The authoring node that refused, so an editor can point at it.
    NodeKey node = kInvalidNodeKey;
};

/// A compiled ability: one shared script program, plus where each pipeline stage begins.
///
/// TWO RESERVED REGISTERS. Register 0 is the pipeline's verdict and register 1 the index of the
/// reason it refused. They are reserved by the compiler rather than agreed by convention, because a
/// convention is what a second author breaks.
inline constexpr Reg kVerdictRegister = 0;
inline constexpr Reg kReasonRegister = 1;

class AbilityProgram {
public:
    explicit AbilityProgram(ScriptProgram&& program) noexcept;

    AbilityProgram(const AbilityProgram&) = delete;
    AbilityProgram& operator=(const AbilityProgram&) = delete;
    AbilityProgram(AbilityProgram&&) noexcept = default;
    AbilityProgram& operator=(AbilityProgram&&) noexcept = default;

    [[nodiscard]] const ScriptProgram& program() const noexcept { return program_; }
    [[nodiscard]] BlockId stage(AbilityStage which) const noexcept {
        return stages_[static_cast<usize>(which)];
    }
    void set_stage(AbilityStage which, BlockId block) noexcept {
        stages_[static_cast<usize>(which)] = block;
    }
    /// The ready tick, as a TICK and never as a counting float timer.
    [[nodiscard]] i64 cooldown_ticks() const noexcept { return cooldown_ticks_; }
    void set_cooldown_ticks(i64 ticks) noexcept { cooldown_ticks_ = ticks; }

    /// The reasons this ability can refuse for, and the authoring node behind each. Indexed by the
    /// value the program leaves in `kReasonRegister`.
    [[nodiscard]] Span<const Name> reasons() const noexcept { return reasons_.span(); }
    [[nodiscard]] Span<const NodeKey> reason_nodes() const noexcept { return reason_nodes_.span(); }
    [[nodiscard]] Status add_reason(Name reason, NodeKey node) noexcept;

private:
    ScriptProgram program_;
    Array<Name> reasons_;
    Array<NodeKey> reason_nodes_;
    BlockId stages_[static_cast<usize>(AbilityStage::Count)] = {};
    i64 cooldown_ticks_ = 0;
};

/// Compile an authored ability graph. Each `ability.stage` node begins one stage's chain, and the
/// stage table records where. A stage the graph does not author is `kNoBlock` and is skipped, which
/// is how an ability with no cost still has a cost stage in the pipeline's order.
[[nodiscard]] Expected<AbilityProgram, Error> compile_ability(const Graph& graph,
                                                              const NodeRegistry& registry,
                                                              DiagnosticSink& sink) noexcept;

/// Register the ability node types beside the script ones.
[[nodiscard]] Status register_ability_nodes(NodeRegistry& registry) noexcept;

/// Run the CHECK stages only, in the specification's order, and stop at the first refusal.
///
/// Nothing is committed, nothing is reserved and nothing is emitted: the run stops before
/// `AbilityStage::Commit` whatever the graph says. That is what "callable without activating"
/// means, and enforcing it here rather than trusting the author is why the stage table exists.
[[nodiscard]] Expected<ValidationResult, Error> validate_activation(const AbilityProgram& ability,
                                                                    ScriptState& state,
                                                                    ScriptHost& host) noexcept;

}  // namespace cy::graph::script

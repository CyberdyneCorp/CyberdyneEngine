// The native back end: the same intermediate representation, with its dispatch resolved ahead of
// time. M8.b task 2.5.
//
// `visual-scripting` requires two execution backends from one intermediate representation and
// requires them to agree. lower_script.cpp holds the bytecode one — a typed register machine that
// reads `Instruction::op` and switches on it once per instruction per instance. This file holds the
// other, and the difference between them is entirely in WHEN the decisions are made:
//
//                          bytecode                      native
//   which handler          switch, per execution         a function pointer, per compilation
//   which block            indexed, per block entry      linearised, once
//   which branch target    a block id, looked up         a step index, resolved
//   a bad branch target    an error at run time          an error at COMPILE time
//
// Everything else is shared on purpose: the same `ScriptState`, the same `ScriptHost`, the same
// `RunOutcome`, and the same digest. `test_lowering.cpp`'s "both back ends agree" case is the
// verification the specification asks for, and it compares the register file, the host's record of
// every external effect, and the suspension state — not just the outcome.

#include <cy/graph/lower_script.h>

#include <utility>

namespace cy::graph::script {

namespace {

[[nodiscard]] Unexpected<Error> invalid(const char* message) noexcept {
    return fail(ErrorCode::InvalidArgument, message);
}

/// The operand span an external call reads. Identical to the bytecode path's, deliberately.
[[nodiscard]] Span<const Value> arguments_of(const NativeStep& step, NativeFrame& frame) noexcept {
    if (step.a == kNoRegister) {
        return {};
    }
    return {frame.registers.data() + step.a, static_cast<usize>(step.b)};
}

// --- The handlers -------------------------------------------------------------------------------
//
// One per opcode, and that is the whole of the resolution: `ScriptOp` already distinguishes
// `AddFloat` from `AddInt`, so a handler never has to ask what its operands are.

void step_load_const(const NativeStep& step, NativeFrame& frame) noexcept {
    frame.registers[step.dst] = frame.program->constants()[step.immediate];
}

void step_move(const NativeStep& step, NativeFrame& frame) noexcept {
    frame.registers[step.dst] = frame.registers[step.a];
}

void step_add_float(const NativeStep& step, NativeFrame& frame) noexcept {
    frame.registers[step.dst] =
        Value::from_float(frame.registers[step.a].x + frame.registers[step.b].x);
}

void step_sub_float(const NativeStep& step, NativeFrame& frame) noexcept {
    frame.registers[step.dst] =
        Value::from_float(frame.registers[step.a].x - frame.registers[step.b].x);
}

void step_mul_float(const NativeStep& step, NativeFrame& frame) noexcept {
    frame.registers[step.dst] =
        Value::from_float(frame.registers[step.a].x * frame.registers[step.b].x);
}

void step_div_float(const NativeStep& step, NativeFrame& frame) noexcept {
    const f32 divisor = frame.registers[step.b].x;
    frame.registers[step.dst] =
        Value::from_float(divisor == 0.0F ? 0.0F : frame.registers[step.a].x / divisor);
}

void step_add_int(const NativeStep& step, NativeFrame& frame) noexcept {
    frame.registers[step.dst] =
        Value::from_int(frame.registers[step.a].integer + frame.registers[step.b].integer);
}

void step_sub_int(const NativeStep& step, NativeFrame& frame) noexcept {
    frame.registers[step.dst] =
        Value::from_int(frame.registers[step.a].integer - frame.registers[step.b].integer);
}

void step_less_float(const NativeStep& step, NativeFrame& frame) noexcept {
    frame.registers[step.dst] =
        Value::from_bool(frame.registers[step.a].x < frame.registers[step.b].x);
}

void step_less_int(const NativeStep& step, NativeFrame& frame) noexcept {
    frame.registers[step.dst] =
        Value::from_bool(frame.registers[step.a].integer < frame.registers[step.b].integer);
}

void step_equal_int(const NativeStep& step, NativeFrame& frame) noexcept {
    frame.registers[step.dst] =
        Value::from_bool(frame.registers[step.a].integer == frame.registers[step.b].integer);
}

void step_not_bool(const NativeStep& step, NativeFrame& frame) noexcept {
    frame.registers[step.dst] = Value::from_bool(frame.registers[step.a].integer == 0);
}

void step_and_bool(const NativeStep& step, NativeFrame& frame) noexcept {
    frame.registers[step.dst] = Value::from_bool(frame.registers[step.a].integer != 0 &&
                                                 frame.registers[step.b].integer != 0);
}

void step_or_bool(const NativeStep& step, NativeFrame& frame) noexcept {
    frame.registers[step.dst] = Value::from_bool(frame.registers[step.a].integer != 0 ||
                                                 frame.registers[step.b].integer != 0);
}

void step_get_field(const NativeStep& step, NativeFrame& frame) noexcept {
    const ExternalRef& external = frame.program->externals()[step.immediate];
    frame.registers[step.dst] = frame.host->get_field(external, frame.registers[step.a]);
}

void step_set_field(const NativeStep& step, NativeFrame& frame) noexcept {
    const ExternalRef& external = frame.program->externals()[step.immediate];
    frame.host->set_field(external, frame.registers[step.a], frame.registers[step.b]);
}

void step_call(const NativeStep& step, NativeFrame& frame) noexcept {
    const ExternalRef& external = frame.program->externals()[step.immediate];
    frame.registers[step.dst] = frame.host->call(external, arguments_of(step, frame));
}

void step_emit_event(const NativeStep& step, NativeFrame& frame) noexcept {
    const ExternalRef& external = frame.program->externals()[step.immediate];
    frame.host->emit_event(external, arguments_of(step, frame));
}

void step_emit_command(const NativeStep& step, NativeFrame& frame) noexcept {
    const ExternalRef& external = frame.program->externals()[step.immediate];
    frame.host->emit_command(external, arguments_of(step, frame));
}

void step_query(const NativeStep& step, NativeFrame& frame) noexcept {
    const ExternalRef& external = frame.program->externals()[step.immediate];
    frame.registers[step.dst] = frame.host->query(external, arguments_of(step, frame));
}

/// The terminators. `immediate` is the taken step and `target` the untaken one, which is the
/// bytecode form's own convention with block ids replaced by step indices.
void step_branch_if(const NativeStep& step, NativeFrame& frame) noexcept {
    frame.next = frame.registers[step.a].integer != 0 ? step.immediate : step.target;
}

void step_jump(const NativeStep& step, NativeFrame& frame) noexcept {
    frame.next = step.target;
}

void step_suspend(const NativeStep& step, NativeFrame& frame) noexcept {
    const SuspendPoint& point = frame.program->suspends()[step.immediate];
    if (frame.host->wait_satisfied(point)) {
        frame.next = step.target;
        return;
    }
    // THE STATE IS RECORDED IN THE BYTECODE BACK END'S TERMS, so an instance suspended here can be
    // resumed there and the other way round. That is what makes the choice of back end a build
    // configuration rather than a fork in the save format.
    frame.state->set_resume_block(step.resume_block);
    if (Status saved = frame.state->persist(*frame.program); !saved) {
        frame.failed = true;
        frame.error = saved.error();
        return;
    }
    frame.outcome = RunOutcome::Suspended;
    frame.stop = true;
}

void step_return(const NativeStep& /*step*/, NativeFrame& frame) noexcept {
    frame.outcome = RunOutcome::Finished;
    frame.stop = true;
}

/// THE RESOLUTION, and it is an array index rather than a switch: `compile_native` reads this table
/// once per instruction and the run-time walk never reads an opcode at all.
constexpr NativeHandler kHandlers[] = {
    step_load_const, step_move,     step_add_float,  step_sub_float,    step_mul_float,
    step_div_float,  step_add_int,  step_sub_int,    step_less_float,   step_less_int,
    step_equal_int,  step_not_bool, step_and_bool,   step_or_bool,      step_get_field,
    step_set_field,  step_call,     step_emit_event, step_emit_command, step_query,
    step_branch_if,  step_jump,     step_suspend,    step_return,
};

static_assert(sizeof(kHandlers) / sizeof(kHandlers[0]) == static_cast<usize>(ScriptOp::Count),
              "every opcode needs exactly one native handler, and the table is indexed by the "
              "opcode's value — an opcode added to ScriptOp without a handler beside it would "
              "otherwise read past the end of this table at compile time.");

/// A branch target, resolved from a block id to a step index. `kNoBlock` means the program ends
/// there — the bytecode back end's `while (block != kNoBlock)` — and becomes `kNoStep`, which ends
/// the native walk at the same point.
[[nodiscard]] Expected<u32, Error> resolve_target(Span<const u32> starts, u32 block) noexcept {
    if (block == kNoBlock) {
        return kNoStep;
    }
    if (block >= starts.size()) {
        return invalid("this program branches to a block that is not in it");
    }
    return starts[block];
}

/// Fill in the per-instruction operands a handler reads. The opcode is not among them.
void decode_operands(const Instruction& instruction, NativeStep& step) noexcept {
    step.dst = instruction.dst;
    step.a = instruction.a;
    step.b = instruction.b;
    step.immediate = instruction.immediate;
}

}  // namespace

NativeProgram::NativeProgram(Allocator& allocator) noexcept
    : steps_(allocator), starts_(allocator) {}

u32 NativeProgram::entry_step() const noexcept {
    if (source_ == nullptr || source_->entry() >= starts_.size()) {
        return kNoStep;
    }
    return starts_[source_->entry()];
}

u64 NativeProgram::digest() const noexcept {
    return source_ == nullptr ? 0ULL : source_->digest();
}

namespace {

/// Pass one: block-structured code becomes one flat step array, each step carrying the handler its
/// opcode resolves to. `starts` records where each block begins, which is what turns a branch into
/// an assignment in pass two.
[[nodiscard]] Status linearise(const ScriptProgram& program, Array<u32>& starts,
                               Array<NativeStep>& steps) noexcept {
    if (Status reserved = starts.reserve(program.blocks().size()); !reserved) {
        return reserved;
    }
    if (Status reserved = steps.reserve(program.code().size()); !reserved) {
        return reserved;
    }
    for (const BasicBlock& block : program.blocks()) {
        if (Status pushed = starts.push_back(steps.size()); !pushed) {
            return pushed;
        }
        for (u32 index = 0; index < block.count; ++index) {
            const Instruction& instruction = program.code()[block.first + index];
            const auto opcode = static_cast<usize>(instruction.op);
            if (opcode >= static_cast<usize>(ScriptOp::Count)) {
                return invalid(
                    "this program contains an opcode the native back end has no handler "
                    "for");
            }
            NativeStep step;
            step.run = kHandlers[opcode];
            decode_operands(instruction, step);
            if (Status pushed = steps.push_back(step); !pushed) {
                return pushed;
            }
        }
    }
    return {};
}

/// Pass two, for one instruction: resolve its branch targets from block ids to step indices, and
/// REFUSE a target that names no block. The bytecode back end discovers that at run time, in
/// whichever frame first takes the branch; here it is a compilation failure with nothing shipped.
[[nodiscard]] Status resolve_step(const Instruction& instruction, Span<const u32> starts,
                                  NativeStep& step) noexcept {
    if (!is_terminator(instruction.op) || instruction.op == ScriptOp::Return) {
        step.target = kNoStep;
        return {};
    }
    auto untaken = resolve_target(starts, instruction.target);
    if (!untaken) {
        return make_unexpected(untaken.error());
    }
    step.target = untaken.value();
    if (instruction.op == ScriptOp::BranchIf) {
        auto taken = resolve_target(starts, instruction.immediate);
        if (!taken) {
            return make_unexpected(taken.error());
        }
        step.immediate = taken.value();
    }
    if (instruction.op == ScriptOp::Suspend) {
        step.resume_block = static_cast<BlockId>(instruction.target);
    }
    return {};
}

[[nodiscard]] Status resolve_targets(const ScriptProgram& program, Span<const u32> starts,
                                     Array<NativeStep>& steps) noexcept {
    u32 cursor = 0;
    for (const BasicBlock& block : program.blocks()) {
        for (u32 index = 0; index < block.count; ++index) {
            const Instruction& instruction = program.code()[block.first + index];
            if (Status resolved = resolve_step(instruction, starts, steps[cursor + index]);
                !resolved) {
                return resolved;
            }
        }
        cursor += block.count;
    }
    return {};
}

}  // namespace

Expected<NativeProgram, Error> compile_native(const ScriptProgram& program,
                                              Allocator& allocator) noexcept {
    NativeProgram native(allocator);
    native.source_ = &program;
    if (Status flat = linearise(program, native.starts_, native.steps_); !flat) {
        return make_unexpected(flat.error());
    }
    if (Status resolved = resolve_targets(program, native.starts_.span(), native.steps_);
        !resolved) {
        return make_unexpected(resolved.error());
    }
    return native;
}

Expected<RunOutcome, Error> execute_native(const NativeProgram& program, ScriptState& state,
                                           ScriptHost& host, u32 instruction_budget) noexcept {
    if (program.steps().empty()) {
        return RunOutcome::Finished;
    }
    u32 step_index = program.entry_step();
    if (state.suspended()) {
        if (state.resume_block() >= program.block_starts().size()) {
            return invalid("this instance resumes in a block that is not in its program");
        }
        step_index = program.block_starts()[state.resume_block()];
        if (Status restored = state.restore(program.source()); !restored) {
            return make_unexpected(restored.error());
        }
        state.set_resume_block(kNoBlock);
    }
    if (step_index == kNoStep) {
        return invalid("this program has no entry block");
    }

    NativeFrame frame;
    frame.program = &program.source();
    frame.state = &state;
    frame.host = &host;
    frame.registers = state.registers();

    u32 executed = 0;
    const Span<const NativeStep> steps = program.steps();
    while (step_index < steps.size()) {
        if (++executed > instruction_budget) {
            return RunOutcome::BudgetExhausted;
        }
        const NativeStep& step = steps[step_index];
        frame.next = step_index + 1;
        step.run(step, frame);
        if (frame.failed) {
            return make_unexpected(frame.error);
        }
        if (frame.stop) {
            return frame.outcome;
        }
        step_index = frame.next;
    }
    return RunOutcome::Finished;
}

}  // namespace cy::graph::script

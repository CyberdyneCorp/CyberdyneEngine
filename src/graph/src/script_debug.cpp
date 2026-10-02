// SPDX-License-Identifier: MIT
// The graph debugger's engine services: instrumentation and watches. See cy/graph/script_debug.h.

#include <cy/graph/script_debug.h>

#include <utility>

#include "script_build.h"

namespace cy::graph::script {
namespace {

/// Whether an instruction is the visible effect of a node on an execution chain. A `Move` into a
/// variable's register is a `script.set_var`, the one execution node that lowers to a move.
[[nodiscard]] bool executes(const ScriptProgram& program, const Instruction& instruction) noexcept {
    switch (instruction.op) {
        case ScriptOp::Call:
        case ScriptOp::EmitEvent:
        case ScriptOp::EmitCommand:
        case ScriptOp::SetField:
        case ScriptOp::BranchIf:
        case ScriptOp::Jump:
        case ScriptOp::Suspend:
        case ScriptOp::Return:
            return true;
        case ScriptOp::Move:
            for (const Variable& variable : program.variables()) {
                if (variable.reg == instruction.dst) {
                    return true;
                }
            }
            return false;
        default:
            return false;
    }
}

[[nodiscard]] NodeKey origin_of(const ScriptProgram& program, u32 location) noexcept {
    const DebugMap::Site* site = program.debug().find(location);
    return site != nullptr ? site->node : kInvalidNodeKey;
}

/// A node's anchor in one block: its last instruction other than a `Return`, else its last one.
[[nodiscard]] u32 anchor_of(const ScriptProgram& program, const BasicBlock& block,
                            NodeKey node) noexcept {
    u32 last_effect = block.count;
    u32 last_any = block.count;
    for (u32 index = 0; index < block.count; ++index) {
        if (origin_of(program, block.first + index) != node) {
            continue;
        }
        last_any = index;
        if (program.code()[block.first + index].op != ScriptOp::Return) {
            last_effect = index;
        }
    }
    return last_effect != block.count ? last_effect : last_any;
}

/// Where in one block each node's probe goes, as a per-instruction mark (`kInvalidNodeKey` for
/// none).
///
/// A node's instructions are not always contiguous: an unwired argument's zero is loaded before
/// the operands wired from other nodes are evaluated, and a chain that runs out ends in a `Return`
/// recorded against the chain's FIRST node. So a node is probed before the run of its instructions
/// that holds its last instruction other than a `Return` — its own effect — and a node whose only
/// instruction is a `Return` (a `script.return`, an empty handler) before that.
[[nodiscard]] Status probe_marks(const ScriptProgram& program, const BasicBlock& block,
                                 Array<NodeKey>& marks) noexcept {
    marks.clear();
    if (Status sized = marks.resize(block.count); !sized) {
        return sized;
    }
    for (NodeKey& mark : marks) {
        mark = kInvalidNodeKey;
    }
    for (u32 index = 0; index < block.count; ++index) {
        const NodeKey node = origin_of(program, block.first + index);
        if (node == kInvalidNodeKey || anchor_of(program, block, node) != index) {
            continue;
        }
        u32 run = index;
        while (run > 0 && origin_of(program, block.first + run - 1) == node) {
            --run;
        }
        marks[run] = node;
    }
    return ok();
}

/// Whether the run of instructions starting at `index` (one node's) contains an execution effect.
[[nodiscard]] bool run_executes(const ScriptProgram& program, const BasicBlock& block,
                                u32 index) noexcept {
    const NodeKey node = origin_of(program, block.first + index);
    for (u32 at = index; at < block.count; ++at) {
        if (origin_of(program, block.first + at) != node) {
            break;
        }
        if (executes(program, program.code()[block.first + at])) {
            return true;
        }
    }
    return false;
}

class Instrumenter {
public:
    Instrumenter(const ScriptProgram& source, ScriptProgram& target,
                 Span<const EventHandler> handlers) noexcept
        : source_(&source), target_(&target), handlers_(handlers) {}

    [[nodiscard]] Status run() noexcept {
        if (Status copied = copy_tables(); !copied) {
            return copied;
        }
        Array<NodeKey> marks(source_->allocator());
        for (usize index = 0; index < source_->blocks().size(); ++index) {
            const auto block = static_cast<BlockId>(index);
            if (Status built = instrument_block(block, marks); !built) {
                return built;
            }
        }
        finish_program_digest(*target_);
        return ok();
    }

private:
    [[nodiscard]] Status copy_tables() noexcept {
        ProgramBuilder::set_name(*target_, source_->name());
        ProgramBuilder::set_registers(*target_, source_->register_count());
        ProgramBuilder::set_entry(*target_, source_->entry());
        for (const Value& constant : source_->constants()) {
            if (Status pushed = ProgramBuilder::constants(*target_).push_back(constant); !pushed) {
                return pushed;
            }
        }
        for (const AccessDecl& access : source_->accesses()) {
            if (Status pushed = ProgramBuilder::accesses(*target_).push_back(access); !pushed) {
                return pushed;
            }
        }
        for (const StateSlot& slot : source_->state_slots()) {
            if (Status pushed = ProgramBuilder::state_slots(*target_).push_back(slot); !pushed) {
                return pushed;
            }
        }
        for (const SuspendPoint& point : source_->suspends()) {
            if (Status pushed = ProgramBuilder::suspends(*target_).push_back(point); !pushed) {
                return pushed;
            }
        }
        for (const ExternalRef& external : source_->externals()) {
            if (Status pushed = ProgramBuilder::externals(*target_).push_back(external); !pushed) {
                return pushed;
            }
        }
        for (const Variable& variable : source_->variables()) {
            if (Status pushed = ProgramBuilder::variables(*target_).push_back(variable); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    /// The event node of the one handler that begins at `block`, or none: a block two handlers
    /// share is not given an entry probe, because it could name only one of them.
    [[nodiscard]] NodeKey entry_node(BlockId block) const noexcept {
        NodeKey found = kInvalidNodeKey;
        u32 count = 0;
        for (const EventHandler& handler : handlers_) {
            if (handler.block == block) {
                found = handler.node;
                ++count;
            }
        }
        return count == 1 ? found : kInvalidNodeKey;
    }

    [[nodiscard]] Status probe(BlockId block, NodeKey node, bool executes_node,
                               u32 first) noexcept {
        const auto index = static_cast<u32>(ProgramBuilder::probes(*target_).size());
        ProbeSite site;
        site.node = node;
        site.block = block;
        site.offset = static_cast<u32>(ProgramBuilder::code(*target_).size()) - first;
        site.executes = executes_node;
        if (Status pushed = ProgramBuilder::probes(*target_).push_back(site); !pushed) {
            return pushed;
        }
        Instruction instruction;
        instruction.op = ScriptOp::Probe;
        instruction.immediate = index;
        return append(instruction, node, Name{});
    }

    [[nodiscard]] Status append(const Instruction& instruction, NodeKey node, Name pin) noexcept {
        const auto location = static_cast<u32>(ProgramBuilder::code(*target_).size());
        if (Status pushed = ProgramBuilder::code(*target_).push_back(instruction); !pushed) {
            return pushed;
        }
        return ProgramBuilder::debug(*target_).record(location, node, pin);
    }

    [[nodiscard]] Status instrument_block(BlockId block, Array<NodeKey>& marks) noexcept {
        const BasicBlock& original = source_->blocks()[block];
        if (Status marked = probe_marks(*source_, original, marks); !marked) {
            return marked;
        }
        const auto first = static_cast<u32>(ProgramBuilder::code(*target_).size());
        const NodeKey entry = entry_node(block);
        const NodeKey first_node =
            original.count > 0 ? origin_of(*source_, original.first) : kInvalidNodeKey;
        if (entry != kInvalidNodeKey && entry != first_node) {
            if (Status probed = probe(block, entry, true, first); !probed) {
                return probed;
            }
        }
        for (u32 index = 0; index < original.count; ++index) {
            const u32 location = original.first + index;
            if (marks[index] != kInvalidNodeKey) {
                if (Status probed =
                        probe(block, marks[index], run_executes(*source_, original, index), first);
                    !probed) {
                    return probed;
                }
            }
            const DebugMap::Site* site = source_->debug().find(location);
            if (Status appended = append(source_->code()[location],
                                         site != nullptr ? site->node : kInvalidNodeKey,
                                         site != nullptr ? site->pin : Name{});
                !appended) {
                return appended;
            }
        }
        BasicBlock copied = original;
        copied.first = first;
        copied.count = static_cast<u32>(ProgramBuilder::code(*target_).size()) - first;
        return ProgramBuilder::blocks(*target_).push_back(copied);
    }

    const ScriptProgram* source_;
    ScriptProgram* target_;
    Span<const EventHandler> handlers_;
};

[[nodiscard]] Expected<ScriptProgram, Error> instrument(
    const ScriptProgram& program, Span<const EventHandler> handlers) noexcept {
    if constexpr (!kGraphDebuggerEnabled) {
        (void)program;
        (void)handlers;
        return fail(ErrorCode::Unsupported,
                    "the graph debugger is compiled out of this build (Profile and Shipping)");
    } else {
        ScriptProgram copy(program.allocator());
        Instrumenter instrumenter(program, copy, handlers);
        if (Status built = instrumenter.run(); !built) {
            return make_unexpected(built.error());
        }
        return copy;
    }
}

/// The block holding code location `location`, or `kNoBlock`.
[[nodiscard]] BlockId block_of(const ScriptProgram& program, u32 location) noexcept {
    for (usize index = 0; index < program.blocks().size(); ++index) {
        const BasicBlock& block = program.blocks()[index];
        if (location >= block.first && location < block.first + block.count) {
            return static_cast<BlockId>(index);
        }
    }
    return kNoBlock;
}

}  // namespace

Expected<ScriptProgram, Error> instrument_for_debug(const ScriptProgram& program) noexcept {
    return instrument(program, {});
}

Expected<EventProgram, Error> instrument_for_debug(const EventProgram& program) noexcept {
    auto copy = instrument(program.program(), program.handlers());
    if (!copy) {
        return make_unexpected(copy.error());
    }
    EventProgram instrumented(std::move(*copy));
    for (const EventHandler& handler : program.handlers()) {
        if (Status added = instrumented.add_handler(handler); !added) {
            return make_unexpected(added.error());
        }
    }
    return instrumented;
}

PinReading read_pin(const ScriptProgram& program, const ScriptState& state, NodeKey node, Name pin,
                    BlockId prefer) noexcept {
    PinReading reading;
    for (const DebugMap::Site& site : program.debug().sites()) {
        if (site.node != node || site.pin != pin || site.location >= program.code().size()) {
            continue;
        }
        const Instruction& instruction = program.code()[site.location];
        if (instruction.dst == kNoRegister || instruction.dst >= state.registers().size()) {
            continue;
        }
        const bool preferred = prefer != kNoBlock && block_of(program, site.location) == prefer;
        reading.found = true;
        reading.reg = instruction.dst;
        reading.kind = instruction.kind;
        reading.value = state.registers()[instruction.dst];
        if (preferred) {
            break;
        }
    }
    return reading;
}

const Variable* find_variable(const ScriptProgram& program, NodeKey id) noexcept {
    for (const Variable& variable : program.variables()) {
        if (variable.id == id) {
            return &variable;
        }
    }
    return nullptr;
}

const Variable* find_variable_named(const ScriptProgram& program, Name name) noexcept {
    for (const Variable& variable : program.variables()) {
        if (variable.name == name) {
            return &variable;
        }
    }
    return nullptr;
}

Value read_variable(const Variable& variable, const ScriptState& state) noexcept {
    return variable.reg < state.registers().size() ? state.registers()[variable.reg] : Value{};
}

}  // namespace cy::graph::script

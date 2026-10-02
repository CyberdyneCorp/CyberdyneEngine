// SPDX-License-Identifier: MIT
#pragma once
// The one door into `ProgramBuilder` for the files of this module that make or remake a program.
//
// `ScriptProgram` has no public mutator, by design: a compiled program is shared by every instance
// and is immutable at run time. `compile_script` and `compile_ability` live beside the builder;
// `compile_event_graph` (event_script.cpp) validates an event graph against its host's declared
// externals and then hands the handler roots here, so all three lower through one builder.
// `instrument_for_debug` (script_debug.cpp) is the one other writer: it copies a compiled program
// with a probe at every node boundary, and it is internal to this module for the same reason.

#include <cy/graph/lower_script.h>

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
    [[nodiscard]] static Array<Variable>& variables(ScriptProgram& program) noexcept {
        return program.variables_;
    }
    [[nodiscard]] static Array<ProbeSite>& probes(ScriptProgram& program) noexcept {
        return program.probes_;
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

/// Recompute `program`'s digest over its code, constants and externals, as the compiler does.
void finish_program_digest(ScriptProgram& program) noexcept;

/// Lower the execution chains that begin at `roots` into one program; `blocks` receives each root's
/// first block, in order.
[[nodiscard]] Expected<ScriptProgram, Error> build_script_program(const Graph& graph,
                                                                  const NodeRegistry& registry,
                                                                  Span<const NodeKey> roots,
                                                                  Array<BlockId>& blocks,
                                                                  DiagnosticSink& sink) noexcept;

}  // namespace cy::graph::script

// SPDX-License-Identifier: MIT
#pragma once
// The one door into `ProgramBuilder` for a compiler outside lower_script.cpp.
//
// `ScriptProgram` has no public mutator, by design: a compiled program is shared by every instance
// and is immutable at run time. `compile_script` and `compile_ability` live beside the builder;
// `compile_event_graph` (event_script.cpp) validates an event graph against its host's declared
// externals and then hands the handler roots here, so all three lower through one builder.

#include <cy/graph/lower_script.h>

namespace cy::graph::script {

/// Lower the execution chains that begin at `roots` into one program; `blocks` receives each root's
/// first block, in order.
[[nodiscard]] Expected<ScriptProgram, Error> build_script_program(const Graph& graph,
                                                                  const NodeRegistry& registry,
                                                                  Span<const NodeKey> roots,
                                                                  Array<BlockId>& blocks,
                                                                  DiagnosticSink& sink) noexcept;

}  // namespace cy::graph::script

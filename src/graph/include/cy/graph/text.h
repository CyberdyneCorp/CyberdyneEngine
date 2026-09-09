#pragma once
// CyberGraph's deterministic textual source. M8.b task 2.1.
//
// ================================================================================================
// WHY A GRAPH HAS A TEXT FORM AT ALL, AND WHY IT IS DETERMINISTIC
// ================================================================================================
//
// `visual-scripting` requires the infrastructure to own serialization, diffing and merging. A graph
// stored as a binary blob has neither: two authors who touch different nodes produce a conflict
// nothing can resolve, and a reviewer sees "graph.bin changed". So the on-disk form is text, and it
// is CANONICAL — one graph has exactly one spelling:
//
//   * nodes are written in key order, not in the order the author added them;
//   * a node's properties are written in name order, by TEXT;
//   * links are written in (target, target pin, source, source pin) order;
//   * a float is written with `%.9g`, which round-trips a `float` exactly.
//
// That is what makes a textual diff a semantic diff for the easy cases and what makes the
// three-way merge in merge.h possible for the rest. It is also what makes the round trip checkable
// by identity: `parse(write(g))` has the same semantic digest as `g`, and `write(parse(t)) == t`.
//
// ================================================================================================
// OPAQUE PRESERVATION
// ================================================================================================
//
// A node whose type the reader does not know — its plugin is not loaded, or is a version behind —
// keeps its authored body VERBATIM and is written back byte for byte. Nothing about it is guessed,
// nothing is dropped, and its wires survive. An editor that silently discarded what it could not
// understand would destroy an author's work the first time a plugin failed to load, and the author
// would find out at the next cook.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/graph/cybergraph.h>

#include <string_view>

namespace cy::graph {

/// The text format's own version, written on the first line. A reader refuses a version it does not
/// know rather than half-reading it.
inline constexpr u32 kTextFormatVersion = 1;

/// Write a graph in canonical form. Appends to `out`.
///
/// `include_layout` off writes the semantics alone, which is what a merge compares and what a cook
/// key is taken over — the two questions "did the meaning change?" and "did the canvas change?" are
/// different, and answering the first must not depend on the second.
[[nodiscard]] Status write_graph(const Graph& graph, Array<char>& out,
                                 bool include_layout = true) noexcept;

/// Read a graph. `registry` may be null; a node whose type it does not carry is preserved verbatim
/// and reported through `sink` rather than dropped.
[[nodiscard]] Expected<Graph, Error> parse_graph(std::string_view text,
                                                 const NodeRegistry* registry, Allocator& allocator,
                                                 DiagnosticSink& sink) noexcept;

}  // namespace cy::graph

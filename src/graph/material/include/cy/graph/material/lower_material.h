#pragma once
// The material lowering: an authored CyberGraph in, the material compiler's own graph front end out.
// M11.c task 6.1a.
//
// ================================================================================================
// WHY THIS FILE EXISTS, MEASURED RATHER THAN ASSUMED
// ================================================================================================
//
// M11.c's spike ran the rung's whole path and junction 1 — AUTHOR — came back REFUSED:
//
//     SpecialisedEditors::open(Domain::Materials) returns "this build declares no authoring
//     vocabulary for materials — `material-compiler` owes it"
//
// and the reason it owed it is here: `src/graph/src/` had `lower_script`, `lower_behaviour`,
// `lower_camera` and `lower_pose`, and **no material lowering at all**. The editor's palette is
// required by `tools/editor/play_contract.py specialised-editors` to be *the engine's own* — it
// reads the `prefix.node` literals out of the engine's lowering sources and requires the editor's
// list to equal them — so a material editor could not be opened until the engine declared what a
// material node IS.
//
// ================================================================================================
// THE VOCABULARY IS DERIVED FROM `GraphOp` AND THE DERIVATION IS TESTED
// ================================================================================================
//
// `rendering::material::GraphOp` is already "the editor's palette, not the IR's opcodes" in
// `graph.h`'s own words — `OneMinus` is a node an author drags in. So the node type names below are
// `"material." + graph_op_name(op)`, written out as string literals because the contract gate reads
// literals (a gate that evaluated calls would be a second compiler), and
// `unit.graph_material`'s *the palette is the engine's own vocabulary* asserts the table equals that
// derivation for every enumerator. An op added to `GraphOp` and not to this table is RED, in the
// engine's own suite, before the editor is involved at all.
//
// One name is NOT an op: `material.output`. `MaterialGraph` has no output node — it has
// `set_surface_output` and `set_opacity_output` — and an author needs something to wire the final
// closure into. It is the graph's root, and its two input pins are the two setters.
//
// ================================================================================================
// A SEPARATE MODULE, AND NOT A FIFTH FILE IN `cy_graph`
// ================================================================================================
//
// `src/graph/CMakeLists.txt` states the invariant this would have broken: "Nothing here depends on
// anything above core: a graph compiler that needed a scene, a device or a server would be a
// compiler that cannot run in a cook." `cy::rendering-material` depends on `cy::servers-render`, so
// putting this lowering inside `cy_graph` would have made every consumer of the authoring layer —
// `visual-scripting`, `ai-system`, `animation-and-skinning` — link the render server.
//
// So it is its own target, `cy::graph-material`, at the `rendering` layer, and the dependency points
// the way round the layering allows: this knows about both, and neither knows about this.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/graph/cybergraph.h>
#include <cy/rendering/material/graph.h>

namespace cy::graph::material {

/// Every `material.*` node type this engine can lower, in the order the palette offers them.
///
/// The editor's own catalogue is compared against this list by the contract gate. It is a function
/// rather than a constant so that a caller cannot hold a pointer into a table and outlive it.
[[nodiscard]] Span<const std::string_view> material_node_types() noexcept;

/// The most pins any material node type has: three inputs and one output.
inline constexpr usize kMaxPins = 5;

/// The input pins of one material node type, in port order, written into the caller's `storage`.
///
/// PORT ORDER IS THE POINT. `MaterialGraph::connect` takes a port index and `graph.h` fixes what
/// each one means — "input 0 is the colour ... and THE LAST INPUT IS ALWAYS THE WEIGHT" — so the
/// editor's pin names and the compiler's port numbers are one table here rather than two
/// conventions that agree by luck.
[[nodiscard]] Span<const PinDesc> material_node_pins(std::string_view type,
                                                    PinDesc storage[kMaxPins]) noexcept;

/// Register the `material.*` node types into an authoring registry.
///
/// The same shape `register_script_nodes` and `register_behaviour_nodes` have, and for the same
/// reason: an editor session builds one registry and every domain contributes to it.
[[nodiscard]] Status register_material_nodes(NodeRegistry& registry) noexcept;

/// Lower an authored graph into the material compiler's graph front end.
///
/// `out` is filled; it is an out-parameter rather than a return value because `MaterialGraph` is
/// deliberately non-copyable and holds its allocator.
///
/// WHAT IS PRESERVED AND WHY. A muted node stays muted, a disconnected node is still lowered, and a
/// port wired twice keeps the last wire — because `graph.h` requires the front end to emit exactly
/// the warts an editor produces, and the compiler is what removes them. A lowering that tidied up on
/// the way through would make `material-compiler`'s "both front-ends produce the same IR" true by
/// construction and therefore worth nothing.
[[nodiscard]] Status lower_material(const Graph& graph,
                                    rendering::material::MaterialGraph& out) noexcept;

}  // namespace cy::graph::material

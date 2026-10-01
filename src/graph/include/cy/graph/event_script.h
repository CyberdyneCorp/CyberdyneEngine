// SPDX-License-Identifier: MIT
#pragma once
// Event graphs: a gameplay graph whose entry points are events, compiled into ONE shared program
// with a handler table. Issue #29, visual scripting.
//
// ================================================================================================
// WHY EVENTS AND NOT A TICK
// ================================================================================================
//
// `visual-scripting`: "The default execution model SHALL be event-driven [...] A per-frame or
// per-tick update SHALL be available and SHALL be explicit." So an event graph has no tick node at
// all: its entry points are `script.on_event` nodes, each naming the event it answers, and a graph
// that wants to keep doing something across frames waits (`script.wait`) on a reason its host
// satisfies. A unit ordered to move does not ask "have I arrived?" sixty times a second; it waits
// on `unit.arrived` and the host resumes it once.
//
// ================================================================================================
// ONE PROGRAM, A HANDLER TABLE, AND THE SAME TWO BACK ENDS
// ================================================================================================
//
// `compile_event_graph` lowers every handler's chain through the builder `compile_script` and
// `compile_ability` use, into one `ScriptProgram`; the `EventProgram` beside it records which block
// each event begins at. An instance is still a `ScriptState`, run with `execute_from` or
// `execute_native_from` — so the bytecode and native back ends, the compact suspension state and
// the debug map are the ones `lower_script.h` already verifies. Nothing here interprets a graph.
//
// ================================================================================================
// FUNCTION METADATA: A NAME THE HOST DID NOT DECLARE IS A COMPILE ERROR
// ================================================================================================
//
// `visual-scripting`: "A function without metadata SHALL NOT be callable from a graph", and every
// call SHALL be checked against the graph's capability set. The host passes its declared externals
// (`ExternalDecl`); a call, query, emission or wait naming anything else, naming it as the wrong
// kind, or needing a capability the graph was not granted, is a diagnostic ON THE NODE that names
// it — and the program is not built.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/lower_script.h>

#include <string_view>

namespace cy::graph::script {

/// The node type an event graph's entry points are. Its `event` property names the event.
inline constexpr std::string_view kOnEventType = "script.on_event";

/// What a declared external is. A name used as another kind is refused.
enum class ExternalKind : u8 {
    /// `script.call`'s `function`.
    Call = 0,
    /// `script.query`'s `query`.
    Query,
    /// `script.emit_event`'s `event`.
    Event,
    /// `script.emit_command`'s `command`.
    Command,
    /// `script.get_field` and `script.set_field`'s `field`.
    Field,
    /// `script.wait`'s `reason`.
    Wait,
    Count,
};

[[nodiscard]] const char* external_kind_name(ExternalKind kind) noexcept;

/// Function metadata, as a host declares it. `visual-scripting`'s "Function and node metadata".
struct ExternalDecl {
    /// The exact name, or — when `family` — a prefix every member begins with (`cue.` declares
    /// `cue.unit.arrived`). A family is still resolved once, when a program is bound, never per
    /// run.
    std::string_view name;
    ExternalKind kind = ExternalKind::Call;
    /// The arguments the host reads, in pin order (`arg0`, `arg1`).
    u32 arity = 0;
    bool family = false;
    /// What the graph must have been granted to name it.
    Capability capability = Capability::None;
    /// One sentence for the palette and for an agent.
    std::string_view summary;
};

/// The declaration `name` resolves to as `kind`, or null. An exact name wins over a family.
[[nodiscard]] const ExternalDecl* find_external(Span<const ExternalDecl> table,
                                                std::string_view name, ExternalKind kind) noexcept;

/// One event the program answers, and where its handler begins.
struct EventHandler {
    Name event;
    BlockId block = kNoBlock;
    /// The `script.on_event` node, for the debug map and an editor.
    NodeKey node = kInvalidNodeKey;
};

/// A compiled event graph: one shared program and its handler table. Immutable once compiled.
class EventProgram {
public:
    explicit EventProgram(ScriptProgram&& program) noexcept;

    EventProgram(const EventProgram&) = delete;
    EventProgram& operator=(const EventProgram&) = delete;
    EventProgram(EventProgram&&) noexcept = default;
    EventProgram& operator=(EventProgram&&) noexcept = default;

    [[nodiscard]] const ScriptProgram& program() const noexcept { return program_; }
    [[nodiscard]] Span<const EventHandler> handlers() const noexcept { return handlers_.span(); }
    /// The handler for `event`, or null when the graph does not answer it.
    [[nodiscard]] const EventHandler* handler(Name event) const noexcept;
    [[nodiscard]] Status add_handler(const EventHandler& handler) noexcept;

private:
    ScriptProgram program_;
    Array<EventHandler> handlers_;
};

/// Compile an event graph. Every node is checked, not only the reachable ones, and every problem is
/// reported before the program is refused:
///
///   script.event.none          the graph answers no event
///   script.event.unnamed       an `on_event` names no event
///   script.event.duplicate     two `on_event` nodes answer one event (on the second; related:
///   first) script.node.unknown        a node whose type the registry does not declare
///   script.pin.unknown         a wire to or from a pin its node's type does not declare
///   script.pin.type            a wire between pins of different types with no declared conversion
///   script.external.unnamed    a call, query, emission, field or wait that names nothing
///   script.external.unknown    ... that names something the host did not declare
///   script.external.kind       ... that names a declared external of another kind
///   script.capability.missing  ... that needs a capability the graph was not granted
///   script.node.unreachable    a WARNING: a node no handler reaches, which compiles to nothing
[[nodiscard]] Expected<EventProgram, Error> compile_event_graph(const Graph& graph,
                                                                const NodeRegistry& registry,
                                                                Span<const ExternalDecl> externals,
                                                                DiagnosticSink& sink) noexcept;

/// The suspension an instance is waiting at, found by its resume block; null when it is not
/// suspended. The scheduler polls this point's reason before resuming the instance.
[[nodiscard]] const SuspendPoint* waiting_at(const ScriptProgram& program,
                                             const ScriptState& state) noexcept;

/// What a graph became, block by block — `visual-scripting`'s "the intermediate representation and
/// the resulting program SHALL be viewable". One line per instruction:
/// `b<block> <op> dst=<r> a=<r> b=<r> imm=<n> target=<n> ; node <key>`. Appends to `out`.
[[nodiscard]] Status disassemble(const ScriptProgram& program, Array<char>& out) noexcept;

}  // namespace cy::graph::script

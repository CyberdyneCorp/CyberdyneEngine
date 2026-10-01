// SPDX-License-Identifier: MIT
// Event graphs: validation against the host's declared externals, the handler table, and the
// listing of what a graph became. See event_script.h.

#include <cy/graph/event_script.h>

#include <cstdio>
#include <utility>

#include "script_build.h"

namespace cy::graph::script {
namespace {

/// Which property of which node type names an external, and as what kind.
struct ExternalUse {
    std::string_view type;
    std::string_view property;
    ExternalKind kind;
};

constexpr ExternalUse kExternalUses[] = {
    {"script.call", "function", ExternalKind::Call},
    {"script.query", "query", ExternalKind::Query},
    {"script.emit_event", "event", ExternalKind::Event},
    {"script.emit_command", "command", ExternalKind::Command},
    {"script.get_field", "field", ExternalKind::Field},
    {"script.set_field", "field", ExternalKind::Field},
    {"script.wait", "reason", ExternalKind::Wait},
};

[[nodiscard]] const ExternalUse* external_use(Name type) noexcept {
    for (const ExternalUse& use : kExternalUses) {
        if (type.text() == use.type) {
            return &use;
        }
    }
    return nullptr;
}

/// The diagnostic's fields, so a report reads as one statement at its call site.
struct Report {
    Severity severity = Severity::Error;
    const char* code = "graph.unspecified";
    NodeKey node = kInvalidNodeKey;
    Name pin;
    const char* message = "";
    Name detail;
    NodeKey related = kInvalidNodeKey;
};

void report(DiagnosticSink& sink, const Report& what) noexcept {
    Diagnostic diagnostic;
    diagnostic.severity = what.severity;
    diagnostic.code = what.code;
    diagnostic.node = what.node;
    diagnostic.pin = what.pin;
    diagnostic.message = what.message;
    diagnostic.detail = what.detail;
    diagnostic.related_node = what.related;
    sink.report(diagnostic);
}

[[nodiscard]] Name text_property(const Graph& graph, NodeKey node,
                                 std::string_view property) noexcept {
    const Literal* literal = graph.property(node, Name::intern(property));
    return literal != nullptr ? literal->text : Name{};
}

[[nodiscard]] NodeKey exec_successor(const Graph& graph, NodeKey node, Name pin) noexcept {
    for (const Link& link : graph.links()) {
        if (link.from == node && link.from_pin == pin) {
            return link.to;
        }
    }
    return kInvalidNodeKey;
}

/// Every unmuted `on_event`, with its event name. Unnamed and duplicate events are reported.
[[nodiscard]] Status collect_handlers(const Graph& graph, DiagnosticSink& sink,
                                      Array<EventHandler>& out) noexcept {
    const Name on_event = Name::intern(kOnEventType);
    bool any = false;
    for (const GraphNode& node : graph.nodes()) {
        if (node.type != on_event || node.muted) {
            continue;
        }
        any = true;
        const Name event = text_property(graph, node.key, "event");
        if (event.is_empty()) {
            report(sink, {.code = "script.event.unnamed",
                          .node = node.key,
                          .message = "this event node names no event, so nothing can start it"});
            continue;
        }
        const EventHandler* earlier = nullptr;
        for (const EventHandler& existing : out) {
            earlier = existing.event == event ? &existing : earlier;
        }
        if (earlier != nullptr) {
            report(sink, {.code = "script.event.duplicate",
                          .node = node.key,
                          .message = "another event node already answers this event",
                          .detail = event,
                          .related = earlier->node});
            continue;
        }
        if (Status pushed = out.push_back(EventHandler{event, kNoBlock, node.key}); !pushed) {
            return pushed;
        }
    }
    if (!any) {
        report(sink, {.code = "script.event.none",
                      .message = "this graph answers no event; add an event node to start it"});
    }
    return ok();
}

void check_node_types(const Graph& graph, const NodeRegistry& registry,
                      DiagnosticSink& sink) noexcept {
    for (const GraphNode& node : graph.nodes()) {
        if (registry.find(node.type) == nullptr) {
            report(sink, {.code = "script.node.unknown",
                          .node = node.key,
                          .message = "this node's type is not a gameplay graph node; the graph "
                                     "keeps it and cannot compile it",
                          .detail = node.type});
        }
    }
}

[[nodiscard]] const PinDesc* pin_of(const Graph& graph, const NodeRegistry& registry, NodeKey node,
                                    Name pin, PinDirection direction) noexcept {
    const GraphNode* authored = graph.find_node(node);
    const NodeType* type = authored != nullptr ? registry.find(authored->type) : nullptr;
    return type != nullptr ? type->find_pin(pin, direction) : nullptr;
}

/// A wire whose ends exist must join pins of one type, or of a declared conversion.
void check_link(const Graph& graph, const NodeRegistry& registry, const Link& link,
                DiagnosticSink& sink) noexcept {
    const GraphNode* from = graph.find_node(link.from);
    const GraphNode* to = graph.find_node(link.to);
    if (from == nullptr || to == nullptr || registry.find(from->type) == nullptr ||
        registry.find(to->type) == nullptr) {
        return;  // The missing node or type is reported once, by its own check.
    }
    const PinDesc* source = pin_of(graph, registry, link.from, link.from_pin, PinDirection::Output);
    const PinDesc* target = pin_of(graph, registry, link.to, link.to_pin, PinDirection::Input);
    if (source == nullptr || target == nullptr) {
        report(sink, {.code = "script.pin.unknown",
                      .node = source == nullptr ? link.from : link.to,
                      .pin = source == nullptr ? link.from_pin : link.to_pin,
                      .message = "a wire names a pin this node's type does not declare",
                      .related = source == nullptr ? link.to : link.from});
        return;
    }
    if (source->type == target->type || registry.converts(source->type, target->type)) {
        return;
    }
    char buffer[160] = {};
    (void)std::snprintf(buffer, sizeof(buffer), "expected %s, received %s",
                        target->type.c_str(), source->type.c_str());
    report(sink, {.code = "script.pin.type",
                  .node = link.to,
                  .pin = link.to_pin,
                  .message = "a wire joins pins of different types and no conversion is declared",
                  .detail = Name::intern(buffer),
                  .related = link.from});
}

/// One node's external: named, declared, of the right kind, and inside the granted capabilities.
void check_external(const Graph& graph, const GraphNode& node, const ExternalUse& use,
                    Span<const ExternalDecl> externals, DiagnosticSink& sink) noexcept {
    const Name name = text_property(graph, node.key, use.property);
    if (name.is_empty()) {
        report(sink, {.code = "script.external.unnamed",
                      .node = node.key,
                      .message = "this node names nothing to call, read, emit or wait for",
                      .detail = Name::intern(use.property)});
        return;
    }
    const ExternalDecl* declared = find_external(externals, name.text(), use.kind);
    if (declared == nullptr) {
        bool other_kind = false;
        for (u32 kind = 0; kind < static_cast<u32>(ExternalKind::Count); ++kind) {
            other_kind = other_kind ||
                         find_external(externals, name.text(), static_cast<ExternalKind>(kind));
        }
        report(sink, {.code = other_kind ? "script.external.kind" : "script.external.unknown",
                      .node = node.key,
                      .message = other_kind
                                     ? "this name is declared, but not as what this node uses it as"
                                     : "this name is not one the graph's host declares",
                      .detail = name});
        return;
    }
    if (declared->capability != Capability::None &&
        !has_capability(graph.granted(), declared->capability)) {
        report(sink, {.code = "script.capability.missing",
                      .node = node.key,
                      .message = "this needs a capability the graph was not granted",
                      .detail = Name::intern(capability_name(declared->capability))});
    }
}

[[nodiscard]] bool contains(const Array<NodeKey>& keys, NodeKey key) noexcept {
    for (const NodeKey existing : keys) {
        if (existing == key) {
            return true;
        }
    }
    return false;
}

/// The nodes a handler reaches: down every execution wire, and up every data wire into a reached
/// node. A node outside this set compiles to nothing.
[[nodiscard]] Status reach(const Graph& graph, const NodeRegistry& registry,
                           Span<const EventHandler> handlers, Array<NodeKey>& reached) noexcept {
    Array<NodeKey> pending(graph.allocator());
    for (const EventHandler& handler : handlers) {
        if (Status pushed = pending.push_back(handler.node); !pushed) {
            return pushed;
        }
    }
    while (!pending.empty()) {
        const NodeKey node = pending.back();
        pending.pop_back();
        if (contains(reached, node)) {
            continue;
        }
        if (Status pushed = reached.push_back(node); !pushed) {
            return pushed;
        }
        for (const Link& link : graph.links()) {
            const PinDesc* out =
                link.from == node
                    ? pin_of(graph, registry, node, link.from_pin, PinDirection::Output)
                    : nullptr;
            const PinDesc* in = link.to == node ? pin_of(graph, registry, node, link.to_pin,
                                                         PinDirection::Input)
                                                : nullptr;
            const bool downstream = out != nullptr && out->execution;
            const bool upstream = in != nullptr && !in->execution;
            if (downstream || upstream) {
                if (Status pushed = pending.push_back(downstream ? link.to : link.from);
                    !pushed) {
                    return pushed;
                }
            }
        }
    }
    return ok();
}

[[nodiscard]] Status warn_unreachable(const Graph& graph, const NodeRegistry& registry,
                                      Span<const EventHandler> handlers,
                                      DiagnosticSink& sink) noexcept {
    Array<NodeKey> reached(graph.allocator());
    if (Status walked = reach(graph, registry, handlers, reached); !walked) {
        return walked;
    }
    for (const GraphNode& node : graph.nodes()) {
        if (node.muted || contains(reached, node.key) || registry.find(node.type) == nullptr ||
            node.type.text() == kOnEventType) {
            continue;
        }
        report(sink, {.severity = Severity::Warning,
                      .code = "script.node.unreachable",
                      .node = node.key,
                      .message = "no event reaches this node, so it compiles to nothing"});
    }
    return ok();
}

[[nodiscard]] Error refused(u32 errors) noexcept {
    (void)errors;
    return Error{ErrorCode::InvalidArgument,
                 "the graph has errors; each diagnostic names the node responsible", 0};
}

void append(Array<char>& out, std::string_view text, Status& status) noexcept {
    if (status) {
        status = out.append(Span<const char>(text.data(), text.size()));
    }
}

void register_text(char* buffer, usize size, const char* label, Reg reg) noexcept {
    if (reg == kNoRegister) {
        (void)std::snprintf(buffer, size, " %s=-", label);
    } else {
        (void)std::snprintf(buffer, size, " %s=r%u", label, static_cast<unsigned>(reg));
    }
}

}  // namespace

const char* external_kind_name(ExternalKind kind) noexcept {
    switch (kind) {
        case ExternalKind::Call:
            return "call";
        case ExternalKind::Query:
            return "query";
        case ExternalKind::Event:
            return "event";
        case ExternalKind::Command:
            return "command";
        case ExternalKind::Field:
            return "field";
        case ExternalKind::Wait:
            return "wait";
        case ExternalKind::Count:
            break;
    }
    return "?";
}

const ExternalDecl* find_external(Span<const ExternalDecl> table, std::string_view name,
                                  ExternalKind kind) noexcept {
    const ExternalDecl* family = nullptr;
    for (const ExternalDecl& declared : table) {
        if (declared.kind != kind) {
            continue;
        }
        if (!declared.family && declared.name == name) {
            return &declared;
        }
        const bool member = declared.family && name.size() > declared.name.size() &&
                            name.starts_with(declared.name);
        family = member && family == nullptr ? &declared : family;
    }
    return family;
}

EventProgram::EventProgram(ScriptProgram&& program) noexcept
    : program_(std::move(program)), handlers_(program_.allocator()) {}

const EventHandler* EventProgram::handler(Name event) const noexcept {
    for (const EventHandler& handler : handlers_) {
        if (handler.event == event) {
            return &handler;
        }
    }
    return nullptr;
}

Status EventProgram::add_handler(const EventHandler& handler) noexcept {
    return handlers_.push_back(handler);
}

Expected<EventProgram, Error> compile_event_graph(const Graph& graph, const NodeRegistry& registry,
                                                  Span<const ExternalDecl> externals,
                                                  DiagnosticSink& sink) noexcept {
    const u32 errors_before = sink.errors();
    Array<EventHandler> handlers(graph.allocator());
    if (Status collected = collect_handlers(graph, sink, handlers); !collected) {
        return make_unexpected(collected.error());
    }
    check_node_types(graph, registry, sink);
    for (const Link& link : graph.links()) {
        check_link(graph, registry, link, sink);
    }
    for (const GraphNode& node : graph.nodes()) {
        if (const ExternalUse* use = external_use(node.type); use != nullptr && !node.muted) {
            check_external(graph, node, *use, externals, sink);
        }
    }
    if (Status warned = warn_unreachable(graph, registry, handlers.span(), sink); !warned) {
        return make_unexpected(warned.error());
    }
    if (sink.errors() != errors_before) {
        return make_unexpected(refused(sink.errors() - errors_before));
    }

    // Each handler's chain begins after its event node; an event node with nothing wired after it
    // is its own (empty) chain, which compiles to a return.
    Array<NodeKey> roots(graph.allocator());
    for (const EventHandler& handler : handlers) {
        const NodeKey next = exec_successor(graph, handler.node, Name::intern("then"));
        if (Status pushed = roots.push_back(next == kInvalidNodeKey ? handler.node : next);
            !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    Array<BlockId> blocks(graph.allocator());
    auto built = build_script_program(graph, registry, roots.span(), blocks, sink);
    if (!built) {
        return make_unexpected(built.error());
    }
    if (sink.errors() != errors_before) {
        return make_unexpected(refused(sink.errors() - errors_before));
    }
    EventProgram program(std::move(built.value()));
    for (usize index = 0; index < handlers.size() && index < blocks.size(); ++index) {
        EventHandler handler = handlers[index];
        handler.block = blocks[index];
        if (Status added = program.add_handler(handler); !added) {
            return make_unexpected(added.error());
        }
    }
    return program;
}

const SuspendPoint* waiting_at(const ScriptProgram& program, const ScriptState& state) noexcept {
    if (!state.suspended()) {
        return nullptr;
    }
    for (const SuspendPoint& point : program.suspends()) {
        if (point.resume == state.resume_block()) {
            return &point;
        }
    }
    return nullptr;
}

Status disassemble(const ScriptProgram& program, Array<char>& out) noexcept {
    Status status = ok();
    char line[256] = {};
    char operand[3][32] = {};
    for (usize block = 0; block < program.blocks().size(); ++block) {
        const BasicBlock& current = program.blocks()[block];
        for (u32 index = 0; index < current.count; ++index) {
            const u32 location = current.first + index;
            const Instruction& instruction = program.code()[location];
            register_text(operand[0], sizeof(operand[0]), "dst", instruction.dst);
            register_text(operand[1], sizeof(operand[1]), "a", instruction.a);
            register_text(operand[2], sizeof(operand[2]), "b", instruction.b);
            const DebugMap::Site* site = program.debug().find(location);
            (void)std::snprintf(line, sizeof(line), "b%u %s%s%s%s imm=%u target=%u ; node %llu\n",
                                static_cast<unsigned>(block), script_op_name(instruction.op),
                                operand[0], operand[1], operand[2],
                                static_cast<unsigned>(instruction.immediate),
                                static_cast<unsigned>(instruction.target),
                                static_cast<unsigned long long>(site != nullptr ? site->node : 0));
            append(out, line, status);
        }
    }
    return status;
}

}  // namespace cy::graph::script

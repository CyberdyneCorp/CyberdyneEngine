// SPDX-License-Identifier: MIT
// The `script.*` operations. See cy/editor/script_service.h.

#include <cy/editor/script_service.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/event_script.h>
#include <cy/graph/text.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "service_wire.h"

namespace cy::editor {
namespace {

namespace script = graph::script;
using script::ExternalKind;

/// The catalogue's property control kinds, numbered as the material catalogue numbers them.
enum class ControlKind : u8 { Text = 0, Bool, Scalar, Vector, Enumeration, Asset };

/// The `ExternalKind` whose declared names a property chooses from, or none.
inline constexpr u8 kFreeText = 0xFF;
/// A variable's type: one of `kVariableTypes`.
inline constexpr u8 kVariableTypeChoices = 0xFE;

constexpr std::string_view kVariableTypes[] = {"float", "int", "bool"};

struct PropertySpec {
    std::string_view node;
    std::string_view name;
    ControlKind kind;
    std::string_view fallback;
    /// The literal type the value is written at in the graph's text.
    std::string_view literal;
    u8 choices;
    std::string_view tooltip;
};

constexpr PropertySpec kProperties[] = {
    {"script.on_event", "event", ControlKind::Text, "unit.command", "name", kFreeText,
     "The event this handler answers. Play raises unit.command with a target's x, y and z."},
    {"script.const_float", "value", ControlKind::Scalar, "0", "float", kFreeText,
     "A constant number."},
    {"script.const_int", "value", ControlKind::Scalar, "0", "int", kFreeText,
     "A constant whole number, zero or more."},
    {"script.const_bool", "value", ControlKind::Bool, "false", "bool", kFreeText,
     "A constant true or false."},
    {"script.call", "function", ControlKind::Enumeration, "unit.move_to", "name",
     static_cast<u8>(ExternalKind::Call), "The engine function to call."},
    {"script.query", "query", ControlKind::Enumeration, "event.x", "name",
     static_cast<u8>(ExternalKind::Query), "The value to read."},
    {"script.emit_event", "event", ControlKind::Text, "cue.unit.arrived", "name", kFreeText,
     "The event to emit. cue.<name> plays the project cue <name> at the unit."},
    {"script.emit_command", "command", ControlKind::Text, "", "name", kFreeText,
     "The command to emit. This engine declares none yet."},
    {"script.get_field", "field", ControlKind::Text, "", "name", kFreeText,
     "The field to read. This engine declares none yet."},
    {"script.set_field", "field", ControlKind::Text, "", "name", kFreeText,
     "The field to write. This engine declares none yet."},
    {"script.wait", "reason", ControlKind::Enumeration, "unit.arrived", "name",
     static_cast<u8>(ExternalKind::Wait), "What to wait for before the next node runs."},
    {"script.variable", "name", ControlKind::Text, "count", "name", kFreeText,
     "The variable's name. Each entity running the graph has its own value, kept across events "
     "and across a reload while Play runs."},
    {"script.variable", "type", ControlKind::Enumeration, "int", "name", kVariableTypeChoices,
     "What the variable holds. Changing it while Play runs is refused: stop Play first."},
    {"script.variable", "default", ControlKind::Scalar, "0", "float", kFreeText,
     "The value an entity's variable starts at; an int truncates it, a bool is true unless 0."},
    {"script.get_var", "variable", ControlKind::Text, "count", "name", kFreeText,
     "The variable to read."},
    {"script.set_var", "variable", ControlKind::Text, "count", "name", kFreeText,
     "The variable to write."},
};

/// An event graph starts at events, so the single-entry node is not in its palette.
constexpr std::string_view kExcludedTypes[] = {"script.entry"};

constexpr u32 kCatalogueSchema = 3;
constexpr u32 kCatalogueVersion = 1;

/// A stable node-type identity: FNV-1a over the name, never zero.
[[nodiscard]] u32 type_identity(std::string_view name) noexcept {
    u32 hash = 2166136261U;
    for (const char character : name) {
        hash ^= static_cast<u8>(character);
        hash *= 16777619U;
    }
    return hash == 0 ? 1U : hash;
}

[[nodiscard]] bool excluded(std::string_view type) noexcept {
    return std::ranges::any_of(
        kExcludedTypes, [type](std::string_view excluded_type) { return excluded_type == type; });
}

/// Writes a payload and keeps the first failure, so an encoder checks once.
class Out {
public:
    explicit Out(Array<u8>& bytes) noexcept : bytes_(&bytes) {}

    Out& u8v(u8 value) noexcept { return keep(wire::put_u8(*bytes_, value)); }
    Out& u32v(u32 value) noexcept { return keep(wire::put_u32(*bytes_, value)); }
    Out& u64v(u64 value) noexcept { return keep(wire::put_u64(*bytes_, value)); }
    Out& f32v(f32 value) noexcept { return keep(wire::put_f32(*bytes_, value)); }
    Out& f64v(f64 value) noexcept {
        u64 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return u64v(bits);
    }
    Out& vec3(Vec3 value) noexcept { return keep(wire::put_vec3(*bytes_, value)); }
    Out& text(std::string_view value) noexcept { return keep(wire::put_text(*bytes_, value)); }

    [[nodiscard]] Status status() const noexcept { return status_; }

private:
    Out& keep(const Status& result) noexcept {
        if (status_ && !result) {
            status_ = result;
        }
        return *this;
    }

    Array<u8>* bytes_;
    Status status_ = ok();
};

void encode_choices(Out& out, const PropertySpec& property) noexcept {
    if (property.choices == kFreeText) {
        out.u32v(0);
        return;
    }
    if (property.choices == kVariableTypeChoices) {
        out.u32v(static_cast<u32>(std::size(kVariableTypes)));
        for (const std::string_view type : kVariableTypes) {
            out.text(type);
        }
        return;
    }
    const auto kind = static_cast<ExternalKind>(property.choices);
    u32 count = 0;
    for (const script::ExternalDecl& declared : game_backend::gameplay_graph_externals()) {
        count += declared.kind == kind && !declared.family ? 1U : 0U;
    }
    out.u32v(count);
    for (const script::ExternalDecl& declared : game_backend::gameplay_graph_externals()) {
        if (declared.kind == kind && !declared.family) {
            out.text(declared.name);
        }
    }
}

void encode_property(Out& out, const PropertySpec& property, u32 identity) noexcept {
    char semantic[32] = {};
    (void)std::snprintf(semantic, sizeof(semantic), "literal:%.*s",
                        static_cast<int>(property.literal.size()), property.literal.data());
    const bool integral = property.literal == "int";
    out.u32v(identity).u8v(static_cast<u8>(property.kind)).text(property.name);
    out.text(property.fallback).text(property.tooltip).text(semantic).text("");
    encode_choices(out, property);
    out.text("").text("visual-scripting").u64v(0).u8v(0);
    // A whole number is zero or more, in steps of one: `script.const_int` stores it unsigned.
    out.u8v(integral ? 0x5U : 0x0U).f64v(0.0).f64v(0.0).f64v(integral ? 1.0 : 0.0);
}

void encode_node(Out& out, const graph::NodeType& type) noexcept {
    out.u32v(type_identity(type.name().text())).u32v(type.version()).text(type.name().text());
    out.u8v(0);  // no stage restriction
    out.u32v(static_cast<u32>(type.pins().size()));
    u32 pin_identity = 0;
    for (const graph::PinDesc& pin : type.pins()) {
        out.u32v(++pin_identity).u8v(static_cast<u8>(pin.direction));
        out.text(pin.name.text()).text(pin.type.text());
    }
    u32 count = 0;
    for (const PropertySpec& property : kProperties) {
        count += property.node == type.name().text() ? 1U : 0U;
    }
    out.u32v(count);
    u32 property_identity = 0;
    for (const PropertySpec& property : kProperties) {
        if (property.node == type.name().text()) {
            encode_property(out, property, ++property_identity);
        }
    }
}

[[nodiscard]] Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Editor);
}

[[nodiscard]] u8 severity_of(graph::Severity severity) noexcept {
    switch (severity) {
        case graph::Severity::Info:
            return 0;
        case graph::Severity::Warning:
            return 1;
        case graph::Severity::Error:
            return 2;
    }
    return 2;
}

void encode_diagnostics(Out& out, const graph::DiagnosticSink& sink) noexcept {
    out.u32v(static_cast<u32>(sink.entries().size()));
    for (const graph::Diagnostic& diagnostic : sink.entries()) {
        out.u8v(severity_of(diagnostic.severity)).text(diagnostic.code).u64v(diagnostic.node);
        out.text(diagnostic.pin.text()).text(diagnostic.message).text(diagnostic.detail.text());
        out.u64v(diagnostic.related_node);
    }
}

void encode_program(Out& out, const script::EventProgram& compiled) noexcept {
    const script::ScriptProgram& program = compiled.program();
    out.u32v(static_cast<u32>(program.code().size()));
    out.u32v(static_cast<u32>(program.blocks().size()));
    out.u32v(program.register_count());
    out.u32v(static_cast<u32>(program.state_slots().size()));
    out.u32v(static_cast<u32>(compiled.handlers().size()));
    for (const script::EventHandler& handler : compiled.handlers()) {
        out.text(handler.event.text()).u64v(handler.node).u32v(handler.block);
    }
    out.u32v(static_cast<u32>(program.externals().size()));
    for (const script::ExternalRef& external : program.externals()) {
        // Every external the program names was admitted by the compiler as exactly one kind.
        u8 kind = 0;
        for (u32 candidate = 0; candidate < static_cast<u32>(ExternalKind::Count); ++candidate) {
            if (script::find_external(game_backend::gameplay_graph_externals(),
                                      external.name.text(),
                                      static_cast<ExternalKind>(candidate)) != nullptr) {
                kind = static_cast<u8>(candidate);
                break;
            }
        }
        out.text(external.name.text()).u8v(kind);
    }
    out.u32v(static_cast<u32>(program.accesses().size()));
    for (const script::AccessDecl& access : program.accesses()) {
        out.text(access.resource.text()).u8v(static_cast<u8>(access.mode));
    }
}

/// The parts of a compile reply before the diagnostics, for a graph that did not compile.
void encode_not_compiled(Out& out, u64 semantic) noexcept {
    out.u8v(0).u64v(semantic).u64v(0);
    out.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
}

[[nodiscard]] ScriptRefusal refused(const char* code, const char* detail) noexcept {
    return ScriptRefusal{code, detail};
}

[[nodiscard]] ScriptRefusal answered(const Status& status) noexcept {
    return status ? ScriptRefusal{} : refused("script.reply", "the reply could not be encoded");
}

[[nodiscard]] ScriptRefusal raise(ScriptPlayRuntime* play, Span<const u8> payload,
                                  Array<u8>& reply) noexcept {
    wire::Reader reader(payload);
    const u32 format = reader.read_u32();
    const u64 node = reader.read_u64();
    const std::string_view event = reader.read_text();
    const u32 count = reader.read_u32();
    f32 arguments[3] = {};
    for (u32 index = 0; index < count && index < 3U; ++index) {
        arguments[index] = reader.read_f32();
    }
    if (!reader.complete() || count > 3U || event.empty()) {
        return refused("script.request.malformed",
                       "a raise is format, node, event and at most three arguments");
    }
    if (format != kScriptWireFormat) {
        return refused("script.schema.unsupported", "this engine reads script format 1");
    }
    for (const f32 argument : arguments) {
        if (!std::isfinite(argument)) {
            return refused("script.request.malformed", "an event argument is not finite");
        }
    }
    if (play == nullptr || play->behaviours() == nullptr) {
        return refused("script.play.unavailable",
                       "graphs run only during Play; enter Play and raise the event again");
    }
    const ecs::Entity entity = play->entity_for(node);
    if (!entity.valid()) {
        return refused("script.node.unknown", "Play has no entity for this node");
    }
    const Expected<u32, Error> started =
        play->raise(entity, Name::intern(event), Span<const f32>(arguments, count));
    if (!started) {
        return refused("script.raise.failed", started.error().message);
    }
    reply.clear();
    if (Status put = wire::put_u32(reply, *started); !put) {
        return answered(put);
    }
    Array<u8> state(allocator());
    if (Status encoded = encode_script_state(play, state); !encoded) {
        return answered(encoded);
    }
    return answered(reply.append(state.span()));
}

/// The request prefix every debug and reload operation starts with.
[[nodiscard]] ScriptRefusal check_format(u32 format, bool complete, const char* shape) noexcept {
    if (!complete) {
        return refused("script.request.malformed", shape);
    }
    if (format != kScriptWireFormat) {
        return refused("script.schema.unsupported", "this engine reads script format 1");
    }
    return {};
}

[[nodiscard]] bool playing(const ScriptPlayRuntime* play) noexcept {
    return play != nullptr && play->behaviours() != nullptr;
}

[[nodiscard]] ScriptRefusal answered_debug(ScriptPlayRuntime* play, Array<u8>& reply) noexcept {
    return answered(encode_script_debug(play, 0, Name{}, {}, reply));
}

[[nodiscard]] ScriptRefusal debug_get(ScriptPlayRuntime* play, Span<const u8> payload,
                                      Array<u8>& reply) noexcept {
    wire::Reader reader(payload);
    const u32 format = reader.read_u32();
    const u64 inspect = reader.read_u64();
    const std::string_view graph_name = reader.read_text();
    const u32 count = reader.read_u32();
    constexpr u32 kMostWatches = 64;
    ScriptWatch watches[kMostWatches] = {};
    for (u32 index = 0; index < count && index < kMostWatches; ++index) {
        watches[index].node = reader.read_u64();
        watches[index].pin = Name::intern(reader.read_text());
    }
    if (const ScriptRefusal bad =
            check_format(format, reader.complete() && count <= kMostWatches,
                         "a debug read is format, node, graph and at most 64 (node, pin) watches");
        bad.refused()) {
        return bad;
    }
    return answered(encode_script_debug(play, inspect, Name::intern(graph_name),
                                        Span<const ScriptWatch>(watches, count), reply));
}

[[nodiscard]] ScriptRefusal debug_breakpoint(ScriptPlayRuntime* play, Span<const u8> payload,
                                             Array<u8>& reply) noexcept {
    wire::Reader reader(payload);
    const u32 format = reader.read_u32();
    const std::string_view graph_name = reader.read_text();
    const u64 node = reader.read_u64();
    const u64 entity = reader.read_u64();
    const u8 enabled = reader.read_u8();
    if (const ScriptRefusal bad =
            check_format(format, reader.complete() && !graph_name.empty() && node != 0,
                         "a breakpoint is format, graph, node, entity and enabled");
        bad.refused()) {
        return bad;
    }
    if (!playing(play)) {
        return refused("script.play.unavailable",
                       "breakpoints are set on Play's graphs; enter Play and set it again");
    }
    ecs::Entity target;
    if (entity != 0) {
        target = play->entity_for(entity);
        if (!target.valid()) {
            return refused("script.node.unknown", "Play has no entity for this node");
        }
    }
    if (Status set = play->set_breakpoint(Name::intern(graph_name), node, target, enabled != 0);
        !set) {
        return refused(set.error().code == ErrorCode::Unsupported ? "script.debug.unavailable"
                                                                  : "script.debug.refused",
                       set.error().message);
    }
    return answered_debug(play, reply);
}

[[nodiscard]] ScriptRefusal debug_control(ScriptPlayRuntime* play, Span<const u8> payload,
                                          Array<u8>& reply) noexcept {
    wire::Reader reader(payload);
    const u32 format = reader.read_u32();
    const u8 action = reader.read_u8();
    if (const ScriptRefusal bad =
            check_format(format, reader.complete() && action <= 3U,
                         "a control is format and an action: 0 pause, 1 continue, 2 step into, "
                         "3 step over");
        bad.refused()) {
        return bad;
    }
    if (!playing(play)) {
        return refused("script.play.unavailable", "the debugger controls Play; enter Play first");
    }
    if (Status done = play->debug(static_cast<ScriptDebugAction>(action)); !done) {
        return refused(done.error().code == ErrorCode::Unsupported ? "script.debug.unavailable"
                                                                   : "script.debug.refused",
                       done.error().message);
    }
    return answered_debug(play, reply);
}

[[nodiscard]] ScriptRefusal reload(ScriptPlayRuntime* play, Span<const u8> payload,
                                   Array<u8>& reply) noexcept {
    wire::Reader reader(payload);
    const u32 format = reader.read_u32();
    const std::string_view reference = reader.read_text();
    const std::string_view source = reader.read_text();
    if (const ScriptRefusal bad = check_format(format, reader.complete() && !reference.empty(),
                                               "a reload is format, reference and source");
        bad.refused()) {
        return bad;
    }
    if (!playing(play)) {
        return refused("script.play.unavailable",
                       "a reload swaps Play's running program; nothing is running");
    }
    graph::DiagnosticSink sink(allocator());
    const Expected<u32, Error> staged = play->reload(reference, source, sink);
    if (!staged && sink.entries().empty()) {
        // Nothing about the graph was wrong: the runtime could not reload it at all.
        return refused("script.reload.unavailable", staged.error().message);
    }
    reply.clear();
    Out writer(reply);
    writer.u32v(kScriptWireFormat).u8v(staged ? 1 : 0).u32v(staged ? *staged : 0U);
    encode_diagnostics(writer, sink);
    return answered(writer.status());
}

void encode_value(Out& out, graph::script::ValueKind kind, const graph::script::Value& value) {
    out.u8v(static_cast<u8>(kind)).f32v(value.x).u64v(static_cast<u64>(value.integer));
}

/// The instance `inspect` names (any graph when `graph_name` is empty), else the paused one when
/// `inspect` is zero; `instance_count()` when there is none.
[[nodiscard]] u32 inspected(const ScriptPlayRuntime& play,
                            const game_backend::GraphBehaviours& graphs, u64 inspect,
                            Name graph_name) noexcept {
    if (inspect == 0) {
        return graphs.paused() ? graphs.pause_view().instance : graphs.instance_count();
    }
    for (u32 index = 0; index < graphs.instance_count(); ++index) {
        const game_backend::GraphInstanceView view = graphs.instance(index);
        if (play.identity_of(view.entity) == inspect &&
            (graph_name.is_empty() || view.graph == graph_name)) {
            return index;
        }
    }
    return graphs.instance_count();
}

}  // namespace

Status encode_script_debug(const ScriptPlayRuntime* play, u64 inspect, Name graph_name,
                           Span<const ScriptWatch> watches, Array<u8>& out) noexcept {
    out.clear();
    Out writer(out);
    const game_backend::GraphBehaviours* graphs = play != nullptr ? play->behaviours() : nullptr;
    writer.u32v(kScriptWireFormat).u8v(graphs != nullptr ? 1 : 0);
    if (graphs == nullptr) {
        writer.u8v(0).u8v(0).u8v(0).u64v(0).text("").u64v(0).u64v(0).u64v(0);
        writer.u32v(0).u32v(0).u64v(0).text("").u32v(0).u32v(0);
        writer.text("").u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
        return writer.status();
    }
    const game_backend::GraphPauseView& pause = graphs->pause_view();
    writer.u8v(graphs->debugging() ? 1 : 0).u8v(pause.paused ? 1 : 0);
    writer.u8v(static_cast<u8>(pause.reason))
        .u64v(pause.paused ? play->identity_of(pause.entity) : 0);
    writer.text(pause.graph.text()).u64v(pause.node).u64v(pause.tick);
    writer.u64v(graphs->tick());
    writer.u32v(static_cast<u32>(graphs->breakpoints().size()));
    for (const game_backend::GraphBreakpoint& breakpoint : graphs->breakpoints()) {
        writer.text(breakpoint.graph.text()).u64v(breakpoint.node);
        writer.u64v(breakpoint.entity.valid() ? play->identity_of(breakpoint.entity) : 0);
    }
    writer.u32v(graphs->trace_count());
    for (u32 index = 0; index < graphs->trace_count(); ++index) {
        const game_backend::GraphTraceEntry entry = graphs->trace_entry(index);
        writer.u64v(entry.sequence).u64v(entry.tick).u64v(play->identity_of(entry.entity));
        writer.text(entry.graph.text()).u64v(entry.node);
    }
    const u32 instance = inspected(*play, *graphs, inspect, graph_name);
    if (instance < graphs->instance_count()) {
        const game_backend::GraphInstanceView view = graphs->instance(instance);
        writer.u64v(play->identity_of(view.entity)).text(view.graph.text());
        writer.u32v(graphs->variable_count(instance));
        for (u32 index = 0; index < graphs->variable_count(instance); ++index) {
            const game_backend::GraphVariableView variable = graphs->variable(instance, index);
            writer.u64v(variable.id).text(variable.name.text());
            encode_value(writer, variable.kind, variable.value);
        }
        writer.u32v(static_cast<u32>(watches.size()));
        for (const ScriptWatch& watch : watches) {
            const graph::script::PinReading reading =
                graphs->watch_pin(instance, watch.node, watch.pin);
            writer.u64v(watch.node).text(watch.pin.text()).u8v(reading.found ? 1 : 0);
            encode_value(writer, reading.kind, reading.value);
        }
    } else {
        writer.u64v(0).text("").u32v(0).u32v(0);
    }
    const game_backend::GraphReloadReport& reloaded = graphs->last_reload();
    writer.text(reloaded.graph.text()).u32v(reloaded.generation).u32v(reloaded.instances);
    writer.u32v(reloaded.kept).u32v(reloaded.added).u32v(reloaded.dropped);
    writer.u32v(reloaded.waits_kept).u32v(reloaded.waits_dropped);
    return writer.status();
}

Status encode_script_catalogue(Array<u8>& out) noexcept {
    graph::NodeRegistry registry(allocator());
    if (Status registered = game_backend::register_gameplay_graph_nodes(registry); !registered) {
        return registered;
    }
    out.clear();
    Out writer(out);
    u32 count = 0;
    for (const graph::NodeType& type : registry.types()) {
        count += excluded(type.name().text()) ? 0U : 1U;
    }
    writer.u32v(kCatalogueSchema).u32v(kCatalogueVersion).u32v(count);
    for (const graph::NodeType& type : registry.types()) {
        if (!excluded(type.name().text())) {
            encode_node(writer, type);
        }
    }
    return writer.status();
}

Status encode_script_compile(std::string_view source, Array<u8>& out) noexcept {
    graph::NodeRegistry registry(allocator());
    if (Status registered = game_backend::register_gameplay_graph_nodes(registry); !registered) {
        return registered;
    }
    graph::DiagnosticSink sink(allocator());
    out.clear();
    Out writer(out);
    writer.u32v(kScriptWireFormat);
    Expected<graph::Graph, Error> parsed = graph::parse_graph(source, &registry, allocator(), sink);
    if (!parsed) {
        graph::Diagnostic diagnostic;
        diagnostic.code = "script.source.invalid";
        diagnostic.message = parsed.error().message;
        sink.report(diagnostic);
        encode_not_compiled(writer, 0);
        encode_diagnostics(writer, sink);
        writer.text("");
        return writer.status();
    }
    Expected<script::EventProgram, Error> compiled = script::compile_event_graph(
        *parsed, registry, game_backend::gameplay_graph_externals(), sink);
    if (!compiled) {
        encode_not_compiled(writer, parsed->semantic_digest());
        encode_diagnostics(writer, sink);
        writer.text("");
        return writer.status();
    }
    writer.u8v(1).u64v(parsed->semantic_digest()).u64v(compiled->program().digest());
    encode_program(writer, *compiled);
    encode_diagnostics(writer, sink);
    Array<char> listing(allocator());
    if (Status listed = script::disassemble(compiled->program(), listing); !listed) {
        return listed;
    }
    writer.text(std::string_view(listing.data(), listing.size()));
    return writer.status();
}

Status encode_script_state(const ScriptPlayRuntime* play, Array<u8>& out) noexcept {
    out.clear();
    Out writer(out);
    const game_backend::GraphBehaviours* graphs = play != nullptr ? play->behaviours() : nullptr;
    writer.u32v(kScriptWireFormat).u8v(graphs != nullptr ? 1 : 0);
    if (graphs == nullptr) {
        writer.u64v(0).u32v(0).u32v(0);
        return writer.status();
    }
    writer.u64v(graphs->tick()).u32v(graphs->instance_count());
    for (u32 index = 0; index < graphs->instance_count(); ++index) {
        const game_backend::GraphInstanceView instance = graphs->instance(index);
        writer.u64v(play->identity_of(instance.entity)).text(instance.graph.text());
        writer.u8v(static_cast<u8>(instance.status)).text(instance.waiting.text());
        writer.vec3(instance.position).u8v(instance.moving ? 1 : 0).u32v(instance.runs);
        writer.text(instance.problem);
    }
    writer.u32v(static_cast<u32>(graphs->cues().size()));
    for (const game_backend::GraphCuePlayed& cue : graphs->cues()) {
        writer.u64v(play->identity_of(cue.entity)).text(cue.cue.text()).u64v(cue.tick);
        writer.vec3(cue.position);
    }
    return writer.status();
}

ScriptRefusal answer_script(ScriptPlayRuntime* play, std::string_view operation,
                            Span<const u8> payload, Array<u8>& reply) noexcept {
    if (operation == "script.catalogue.get") {
        return answered(encode_script_catalogue(reply));
    }
    if (operation == "script.compile") {
        wire::Reader reader(payload);
        const u32 format = reader.read_u32();
        const std::string_view source = reader.read_text();
        if (!reader.complete()) {
            return refused("script.request.malformed", "a compile is format and source text");
        }
        if (format != kScriptWireFormat) {
            return refused("script.schema.unsupported", "this engine reads script format 1");
        }
        return answered(encode_script_compile(source, reply));
    }
    if (operation == "script.event.raise") {
        return raise(play, payload, reply);
    }
    if (operation == "script.state.get") {
        return answered(encode_script_state(play, reply));
    }
    if (operation == "script.debug.get") {
        return debug_get(play, payload, reply);
    }
    if (operation == "script.debug.breakpoint") {
        return debug_breakpoint(play, payload, reply);
    }
    if (operation == "script.debug.control") {
        return debug_control(play, payload, reply);
    }
    if (operation == "script.reload") {
        return reload(play, payload, reply);
    }
    return refused("script.operation.unsupported", "this engine does not serve that operation");
}

}  // namespace cy::editor

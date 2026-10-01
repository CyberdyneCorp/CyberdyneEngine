// SPDX-License-Identifier: MIT
// Event graphs: handlers in one shared program, diagnostics on the node, and the two back ends.
// Issue #29, visual scripting.

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/event_script.h>
#include <cy/graph/lower_script.h>
#include <cy/test/test.h>

#include <string>
#include <string_view>
#include <vector>

using namespace cy;
using namespace cy::graph;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Scripting);
}

using script::ExternalDecl;
using script::ExternalKind;

constexpr ExternalDecl kExternals[] = {
    {"unit.move_to", ExternalKind::Call, 2, false, Capability::WriteWorld, "move"},
    {"event.x", ExternalKind::Query, 0, false, Capability::ReadWorld, "x"},
    {"event.z", ExternalKind::Query, 0, false, Capability::ReadWorld, "z"},
    {"unit.arrived", ExternalKind::Wait, 0, false, Capability::ReadWorld, "arrived"},
    {"cue.", ExternalKind::Event, 0, true, Capability::Audio, "cue"},
};

[[nodiscard]] Span<const ExternalDecl> externals() noexcept {
    return {kExternals, sizeof(kExternals) / sizeof(kExternals[0])};
}

[[nodiscard]] Literal text(std::string_view value) noexcept {
    Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(value);
    return literal;
}

/// A graph built line by line, with every step required to succeed.
struct Builder {
    Graph graph{allocator(), Name::intern("probe")};
    NodeRegistry registry{allocator()};

    Builder() {
        CY_REQUIRE(script::register_script_nodes(registry).has_value());
        graph.grant(Capability::ReadWorld | Capability::WriteWorld | Capability::Audio);
    }

    Builder& node(NodeKey key, std::string_view type) {
        CY_REQUIRE(graph.add_node(key, Name::intern(type)).has_value());
        return *this;
    }
    Builder& prop(NodeKey key, std::string_view name, std::string_view value) {
        CY_REQUIRE(graph.set_property(key, Name::intern(name), text(value)).has_value());
        return *this;
    }
    Builder& wire(NodeKey from, std::string_view from_pin, NodeKey to, std::string_view to_pin) {
        CY_REQUIRE(graph
                       .connect(from, Name::intern(from_pin), to, Name::intern(to_pin))
                       .has_value());
        return *this;
    }

    /// "on unit.command: move to (event.x, event.z), wait until arrived, play cue.done".
    Builder& order() {
        node(1, "script.on_event").prop(1, "event", "unit.command");
        node(2, "script.query").prop(2, "query", "event.x");
        node(3, "script.query").prop(3, "query", "event.z");
        node(4, "script.call").prop(4, "function", "unit.move_to");
        node(5, "script.wait").prop(5, "reason", "unit.arrived");
        node(6, "script.emit_event").prop(6, "event", "cue.done");
        wire(1, "then", 4, "in").wire(2, "value", 4, "arg0").wire(3, "value", 4, "arg1");
        return wire(4, "then", 5, "in").wire(5, "then", 6, "in");
    }
};

/// Every host call, in order, as text — what two back ends must agree on.
class RecordingHost final : public script::ScriptHost {
public:
    script::Value call(const script::ExternalRef& callee,
                       Span<const script::Value> arguments) override {
        log("call", callee, arguments);
        return script::Value::from_float(5.0F);
    }
    script::Value query(const script::ExternalRef& query,
                        Span<const script::Value> arguments) override {
        log("query", query, arguments);
        return script::Value::from_float(query.name == Name::intern("event.x") ? 3.0F : 4.0F);
    }
    void emit_event(const script::ExternalRef& event,
                    Span<const script::Value> arguments) override {
        log("event", event, arguments);
    }
    void emit_command(const script::ExternalRef& command,
                      Span<const script::Value> arguments) override {
        log("command", command, arguments);
    }
    script::Value get_field(const script::ExternalRef& field,
                            const script::Value& /*subject*/) override {
        log("get", field, {});
        return {};
    }
    void set_field(const script::ExternalRef& field, const script::Value& /*subject*/,
                   const script::Value& /*value*/) override {
        log("set", field, {});
    }
    [[nodiscard]] bool wait_satisfied(const script::SuspendPoint& point) override {
        calls.push_back("wait " + std::string(point.reason.text()));
        return arrived;
    }

    std::vector<std::string> calls;
    bool arrived = false;

private:
    void log(const char* what, const script::ExternalRef& external,
             Span<const script::Value> arguments) {
        std::string line = std::string(what) + " " + std::string(external.name.text());
        for (const script::Value& argument : arguments) {
            line += " " + std::to_string(argument.x);
        }
        calls.push_back(line);
    }
};

[[nodiscard]] script::EventProgram compiled(Builder& builder) {
    DiagnosticSink sink(allocator());
    auto program = script::compile_event_graph(builder.graph, builder.registry, externals(), sink);
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(sink.errors(), 0U);
    return std::move(*program);
}

/// The diagnostics a graph compiles to, which must refuse it.
[[nodiscard]] std::vector<Diagnostic> refused(Builder& builder) {
    DiagnosticSink sink(allocator());
    auto program = script::compile_event_graph(builder.graph, builder.registry, externals(), sink);
    CY_CHECK(!program.has_value());
    return {sink.entries().begin(), sink.entries().end()};
}

[[nodiscard]] const Diagnostic* coded(const std::vector<Diagnostic>& found, std::string_view code) {
    for (const Diagnostic& diagnostic : found) {
        if (code == diagnostic.code) {
            return &diagnostic;
        }
    }
    return nullptr;
}

}  // namespace

CY_TEST_CASE("event graph: each event starts its own handler in one shared program") {
    Builder builder;
    builder.order();
    builder.node(7, "script.on_event").prop(7, "event", "unit.selected");
    builder.node(8, "script.emit_event").prop(8, "event", "cue.selected").wire(7, "then", 8, "in");
    const script::EventProgram program = compiled(builder);
    CY_REQUIRE_EQ(program.handlers().size(), 2U);
    const script::EventHandler* order = program.handler(Name::intern("unit.command"));
    const script::EventHandler* select = program.handler(Name::intern("unit.selected"));
    CY_REQUIRE(order != nullptr);
    CY_REQUIRE(select != nullptr);
    CY_CHECK_NE(order->block, select->block);
    CY_CHECK_EQ(order->node, 1U);
    CY_CHECK(program.handler(Name::intern("unit.tick")) == nullptr);

    RecordingHost host;
    script::ScriptState state(allocator(), program.program());
    const auto ran = script::execute_from(program.program(), state, host, select->block);
    CY_REQUIRE(ran.has_value());
    CY_CHECK(*ran == script::RunOutcome::Finished);
    CY_REQUIRE_EQ(host.calls.size(), 1U);
    CY_CHECK_EQ(host.calls[0], "event cue.selected 0.000000 0.000000");
}

CY_TEST_CASE("event graph: a wait suspends, and a newer event discards the suspension") {
    Builder builder;
    builder.order();
    const script::EventProgram program = compiled(builder);
    const script::EventHandler* order = program.handler(Name::intern("unit.command"));
    CY_REQUIRE(order != nullptr);

    RecordingHost host;
    script::ScriptState state(allocator(), program.program());
    auto ran = script::execute_from(program.program(), state, host, order->block);
    CY_REQUIRE(ran.has_value());
    CY_CHECK(*ran == script::RunOutcome::Suspended);
    const script::SuspendPoint* waiting = script::waiting_at(program.program(), state);
    CY_REQUIRE(waiting != nullptr);
    CY_CHECK_EQ(waiting->reason, Name::intern("unit.arrived"));
    CY_CHECK_EQ(waiting->origin, 5U);

    // The same event again: the handler starts over rather than resuming after the wait.
    host.calls.clear();
    ran = script::execute_from(program.program(), state, host, order->block);
    CY_REQUIRE(ran.has_value());
    CY_CHECK(*ran == script::RunOutcome::Suspended);
    CY_CHECK_EQ(host.calls.front(), "query event.x 0.000000 0.000000");

    // Satisfied: the scheduler resumes it, and the cue is the one thing left to do.
    host.calls.clear();
    host.arrived = true;
    ran = script::execute(program.program(), state, host);
    CY_REQUIRE(ran.has_value());
    CY_CHECK(*ran == script::RunOutcome::Finished);
    CY_REQUIRE_EQ(host.calls.size(), 1U);
    CY_CHECK_EQ(host.calls[0], "event cue.done 0.000000 0.000000");
    CY_CHECK(script::waiting_at(program.program(), state) == nullptr);
}

CY_TEST_CASE("event graph: both back ends make the same host calls and leave the same state") {
    Builder builder;
    builder.order();
    const script::EventProgram program = compiled(builder);
    const script::EventHandler* order = program.handler(Name::intern("unit.command"));
    CY_REQUIRE(order != nullptr);
    auto native = script::compile_native(program.program(), allocator());
    CY_REQUIRE(native.has_value());

    RecordingHost bytecode_host;
    RecordingHost native_host;
    script::ScriptState bytecode_state(allocator(), program.program());
    script::ScriptState native_state(allocator(), program.program());
    auto first = script::execute_from(program.program(), bytecode_state, bytecode_host,
                                      order->block);
    auto second = script::execute_native_from(*native, native_state, native_host, order->block);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK(*first == *second);
    CY_CHECK_EQ(bytecode_state.resume_block(), native_state.resume_block());

    bytecode_host.arrived = true;
    native_host.arrived = true;
    first = script::execute(program.program(), bytecode_state, bytecode_host);
    second = script::execute_native(*native, native_state, native_host);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK(*first == *second);
    CY_CHECK(bytecode_host.calls == native_host.calls);
    CY_CHECK(!bytecode_host.calls.empty());
    for (usize reg = 0; reg < bytecode_state.registers().size(); ++reg) {
        CY_CHECK_EQ(bytecode_state.registers()[reg].x, native_state.registers()[reg].x);
        CY_CHECK_EQ(bytecode_state.registers()[reg].integer,
                    native_state.registers()[reg].integer);
    }
}

CY_TEST_CASE("event graph: a graph with no event, an unnamed event, or two of one is refused") {
    Builder none;
    none.node(1, "script.query").prop(1, "query", "event.x");
    const auto no_event = refused(none);
    CY_CHECK(coded(no_event, "script.event.none") != nullptr);

    Builder unnamed;
    unnamed.node(1, "script.on_event");
    const auto unnamed_found = refused(unnamed);
    const Diagnostic* nameless = coded(unnamed_found, "script.event.unnamed");
    CY_REQUIRE(nameless != nullptr);
    CY_CHECK_EQ(nameless->node, 1U);

    Builder twice;
    twice.order();
    twice.node(9, "script.on_event").prop(9, "event", "unit.command");
    const auto twice_found = refused(twice);
    const Diagnostic* duplicate = coded(twice_found, "script.event.duplicate");
    CY_REQUIRE(duplicate != nullptr);
    CY_CHECK_EQ(duplicate->node, 9U);
    CY_CHECK_EQ(duplicate->related_node, 1U);
}

CY_TEST_CASE("event graph: an undeclared, misused or unnamed external is an error on its node") {
    Builder unknown;
    unknown.order().prop(4, "function", "unit.mvoe_to");
    const auto unknown_found = refused(unknown);
    const Diagnostic* undeclared = coded(unknown_found, "script.external.unknown");
    CY_REQUIRE(undeclared != nullptr);
    CY_CHECK_EQ(undeclared->node, 4U);
    CY_CHECK_EQ(undeclared->detail, Name::intern("unit.mvoe_to"));
    CY_CHECK_EQ(undeclared->severity, Severity::Error);

    Builder misused;
    misused.order().prop(4, "function", "event.x");
    const Diagnostic* kind = coded(refused(misused), "script.external.kind");
    CY_REQUIRE(kind != nullptr);
    CY_CHECK_EQ(kind->node, 4U);

    Builder nothing;
    nothing.order().prop(5, "reason", "");
    const Diagnostic* nameless = coded(refused(nothing), "script.external.unnamed");
    CY_REQUIRE(nameless != nullptr);
    CY_CHECK_EQ(nameless->node, 5U);
}

CY_TEST_CASE("event graph: a call outside the graph's capabilities is an error on its node") {
    Builder builder;
    builder.order();
    builder.graph.revoke(Capability::Audio);
    const Diagnostic* missing = coded(refused(builder), "script.capability.missing");
    CY_REQUIRE(missing != nullptr);
    CY_CHECK_EQ(missing->node, 6U);
    CY_CHECK_EQ(missing->detail, Name::intern("audio"));
}

CY_TEST_CASE("event graph: a wire between different pin types names both ends and both types") {
    Builder builder;
    builder.order();
    // An execution output into a float input: the canvas refuses it, a hand-edited source can not.
    builder.wire(1, "then", 4, "arg1");
    const Diagnostic* mismatch = coded(refused(builder), "script.pin.type");
    CY_REQUIRE(mismatch != nullptr);
    CY_CHECK_EQ(mismatch->node, 4U);
    CY_CHECK_EQ(mismatch->pin, Name::intern("arg1"));
    CY_CHECK_EQ(mismatch->related_node, 1U);
    CY_CHECK_EQ(mismatch->detail, Name::intern("expected float, received exec"));
}

CY_TEST_CASE("event graph: a node type the registry lacks is reported, not silently dropped") {
    Builder builder;
    builder.order();
    builder.node(9, "ai.selector");
    const Diagnostic* unknown = coded(refused(builder), "script.node.unknown");
    CY_REQUIRE(unknown != nullptr);
    CY_CHECK_EQ(unknown->node, 9U);
}

CY_TEST_CASE("event graph: a node no event reaches is a warning and the graph still compiles") {
    Builder builder;
    builder.order();
    builder.node(9, "script.emit_event").prop(9, "event", "cue.orphan");
    DiagnosticSink sink(allocator());
    auto program = script::compile_event_graph(builder.graph, builder.registry, externals(), sink);
    CY_CHECK(program.has_value());
    CY_CHECK_EQ(sink.errors(), 0U);
    CY_REQUIRE_EQ(sink.warnings(), 1U);
    CY_CHECK_EQ(std::string(sink.entries()[0].code), "script.node.unreachable");
    CY_CHECK_EQ(sink.entries()[0].node, 9U);
}

CY_TEST_CASE("event graph: the listing shows each instruction and the node it came from") {
    Builder builder;
    builder.order();
    const script::EventProgram program = compiled(builder);
    Array<char> listing(allocator());
    CY_REQUIRE(script::disassemble(program.program(), listing).has_value());
    const std::string text(listing.data(), listing.size());
    CY_CHECK(text.find("call") != std::string::npos);
    CY_CHECK(text.find("suspend") != std::string::npos);
    CY_CHECK(text.find("emit_event") != std::string::npos);
    CY_CHECK(text.find("; node 4") != std::string::npos);
    CY_CHECK(text.find("; node 6") != std::string::npos);
}

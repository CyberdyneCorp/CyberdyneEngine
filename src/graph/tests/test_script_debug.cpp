// SPDX-License-Identifier: MIT
// The graph debugger's engine services, graph variables, and hot reload's state migration, over
// the compiled register machine on both back ends. Issues #84 and #29.

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/event_script.h>
#include <cy/graph/lower_script.h>
#include <cy/graph/script_debug.h>
#include <cy/graph/script_reload.h>
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

[[nodiscard]] Literal number(f32 value) noexcept {
    Literal literal;
    literal.type = Name::intern("float");
    literal.value.x = value;
    return literal;
}

[[nodiscard]] Literal whole(u32 value) noexcept {
    Literal literal;
    literal.type = Name::intern("int");
    literal.value.mask = value;
    return literal;
}

struct GraphBuilder {
    Graph graph{allocator(), Name::intern("debugged")};
    NodeRegistry registry{allocator()};

    GraphBuilder() {
        CY_REQUIRE(script::register_script_nodes(registry).has_value());
        graph.grant(Capability::ReadWorld | Capability::WriteWorld | Capability::Audio);
    }

    GraphBuilder& node(NodeKey key, std::string_view type) {
        CY_REQUIRE(graph.add_node(key, Name::intern(type)).has_value());
        return *this;
    }
    GraphBuilder& prop(NodeKey key, std::string_view name, std::string_view value) {
        CY_REQUIRE(graph.set_property(key, Name::intern(name), text(value)).has_value());
        return *this;
    }
    GraphBuilder& literal(NodeKey key, std::string_view name, const Literal& value) {
        CY_REQUIRE(graph.set_property(key, Name::intern(name), value).has_value());
        return *this;
    }
    GraphBuilder& wire(NodeKey from, std::string_view from_pin, NodeKey to,
                       std::string_view to_pin) {
        CY_REQUIRE(
            graph.connect(from, Name::intern(from_pin), to, Name::intern(to_pin)).has_value());
        return *this;
    }

    /// "on unit.command: move to (event.x, event.z), wait until arrived, play cue.done".
    GraphBuilder& order() {
        node(1, "script.on_event").prop(1, "event", "unit.command");
        node(2, "script.query").prop(2, "query", "event.x");
        node(3, "script.query").prop(3, "query", "event.z");
        node(4, "script.call").prop(4, "function", "unit.move_to");
        node(5, "script.wait").prop(5, "reason", "unit.arrived");
        node(6, "script.emit_event").prop(6, "event", "cue.done");
        wire(1, "then", 4, "in").wire(2, "value", 4, "arg0").wire(3, "value", 4, "arg1");
        return wire(4, "then", 5, "in").wire(5, "then", 6, "in");
    }

    /// "on count.up: count = count + step" with `count` an int declared by node 20.
    GraphBuilder& counter(std::string_view type = "int") {
        node(10, "script.on_event").prop(10, "event", "count.up");
        node(20, "script.variable").prop(20, "name", "count").prop(20, "type", type);
        literal(20, "default", number(0.0F));
        node(11, "script.get_var").prop(11, "variable", "count");
        node(12, "script.const_int").literal(12, "value", whole(1));
        node(13, "script.add_int");
        node(14, "script.set_var").prop(14, "variable", "count");
        wire(11, "value", 13, "a").wire(12, "value", 13, "b").wire(13, "value", 14, "value");
        return wire(10, "then", 14, "in");
    }
};

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

/// A hook that records every node boundary and breaks where it is told to.
class Breaker final : public script::ScriptDebugHook {
public:
    script::DebugVerdict on_probe(const script::ProbeSite& site,
                                  const script::ScriptState& /*state*/) noexcept override {
        seen.push_back(site.node);
        executes.push_back(site.executes);
        const bool hit = every || site.node == at;
        return hit ? script::DebugVerdict::Break : script::DebugVerdict::Continue;
    }

    NodeKey at = kInvalidNodeKey;
    bool every = false;
    std::vector<NodeKey> seen;
    std::vector<bool> executes;
};

[[nodiscard]] script::EventProgram compiled(GraphBuilder& builder) {
    DiagnosticSink sink(allocator());
    auto program = script::compile_event_graph(builder.graph, builder.registry, externals(), sink);
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(sink.errors(), 0U);
    return std::move(*program);
}

[[nodiscard]] script::EventProgram instrumented(const script::EventProgram& program) {
    auto debug = script::instrument_for_debug(program);
    CY_REQUIRE(debug.has_value());
    return std::move(*debug);
}

[[nodiscard]] std::vector<Diagnostic> refused(GraphBuilder& builder) {
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

[[nodiscard]] usize probe_count(const script::ScriptProgram& program) noexcept {
    usize count = 0;
    for (const script::Instruction& instruction : program.code()) {
        count += instruction.op == script::ScriptOp::Probe ? 1U : 0U;
    }
    return count;
}

/// Run `event`'s handler and then keep continuing it until it stops for something other than the
/// debugger, recording which node each break was at.
[[nodiscard]] script::RunOutcome run_breaking(const script::EventProgram& program,
                                              script::ScriptState& state, RecordingHost& host,
                                              Breaker& breaker, std::vector<NodeKey>& breaks,
                                              const script::NativeProgram* native = nullptr) {
    const script::EventHandler* handler = program.handler(Name::intern("unit.command"));
    CY_REQUIRE(handler != nullptr);
    auto outcome =
        native != nullptr
            ? script::execute_native_from(*native, state, host, handler->block, 4096, &breaker)
            : script::execute_from(program.program(), state, host, handler->block, 4096, &breaker);
    CY_REQUIRE(outcome.has_value());
    while (*outcome == script::RunOutcome::Paused) {
        breaks.push_back(breaker.seen.back());
        outcome = native != nullptr
                      ? script::execute_native(*native, state, host, 4096, &breaker)
                      : script::execute(program.program(), state, host, 4096, &breaker);
        CY_REQUIRE(outcome.has_value());
    }
    return *outcome;
}

}  // namespace

CY_TEST_CASE("graph debugger: a breakpoint stops before its node, on both back ends alike") {
    if constexpr (!script::kGraphDebuggerEnabled) {
        return;  // "the graph debugger is compiled out" below is this build's case.
    }
    GraphBuilder builder;
    builder.order();
    const script::EventProgram source = compiled(builder);
    const script::EventProgram program = instrumented(source);
    const script::EventHandler* handler = program.handler(Name::intern("unit.command"));
    CY_REQUIRE(handler != nullptr);
    auto native = script::compile_native(program.program(), allocator());
    CY_REQUIRE(native.has_value());

    RecordingHost bytecode_host;
    RecordingHost native_host;
    Breaker bytecode_breaker;
    Breaker native_breaker;
    bytecode_breaker.at = 4;
    native_breaker.at = 4;
    script::ScriptState bytecode_state(allocator(), program.program());
    script::ScriptState native_state(allocator(), program.program());
    auto first = script::execute_from(program.program(), bytecode_state, bytecode_host,
                                      handler->block, 4096, &bytecode_breaker);
    auto second = script::execute_native_from(*native, native_state, native_host, handler->block,
                                              4096, &native_breaker);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK(*first == script::RunOutcome::Paused);
    CY_CHECK(*second == script::RunOutcome::Paused);
    // Stopped BEFORE the call: both arguments were read, nothing was moved.
    const std::vector<std::string> read{"query event.x 0.000000 0.000000",
                                        "query event.z 0.000000 0.000000"};
    CY_CHECK(bytecode_host.calls == read);
    CY_CHECK(native_host.calls == read);
    // The same pause, in the same terms, on both back ends.
    CY_CHECK(bytecode_state.paused());
    CY_CHECK_EQ(bytecode_state.paused_block(), native_state.paused_block());
    CY_CHECK_EQ(bytecode_state.paused_offset(), native_state.paused_offset());

    // Continued, each runs the call and reaches the wait.
    bytecode_breaker.at = kInvalidNodeKey;
    native_breaker.at = kInvalidNodeKey;
    first =
        script::execute(program.program(), bytecode_state, bytecode_host, 4096, &bytecode_breaker);
    second = script::execute_native(*native, native_state, native_host, 4096, &native_breaker);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK(*first == script::RunOutcome::Suspended);
    CY_CHECK(*second == script::RunOutcome::Suspended);
    CY_CHECK(!bytecode_state.paused());
    CY_CHECK(bytecode_host.calls == native_host.calls);
    CY_CHECK_EQ(bytecode_host.calls.back(), "wait unit.arrived");
}

CY_TEST_CASE("graph debugger: stepping visits nodes in the order the program runs them") {
    if constexpr (!script::kGraphDebuggerEnabled) {
        return;
    }
    GraphBuilder builder;
    builder.order();
    const script::EventProgram program = instrumented(compiled(builder));
    auto native = script::compile_native(program.program(), allocator());
    CY_REQUIRE(native.has_value());

    for (const bool on_native : {false, true}) {
        RecordingHost host;
        Breaker step_into;
        step_into.every = true;
        script::ScriptState state(allocator(), program.program());
        std::vector<NodeKey> breaks;
        std::vector<usize> effects_at_break;
        const script::EventHandler* handler = program.handler(Name::intern("unit.command"));
        CY_REQUIRE(handler != nullptr);
        auto outcome = on_native ? script::execute_native_from(*native, state, host, handler->block,
                                                               4096, &step_into)
                                 : script::execute_from(program.program(), state, host,
                                                        handler->block, 4096, &step_into);
        CY_REQUIRE(outcome.has_value());
        while (*outcome == script::RunOutcome::Paused) {
            breaks.push_back(step_into.seen.back());
            effects_at_break.push_back(host.calls.size());
            outcome = on_native ? script::execute_native(*native, state, host, 4096, &step_into)
                                : script::execute(program.program(), state, host, 4096, &step_into);
            CY_REQUIRE(outcome.has_value());
        }
        CY_CHECK(*outcome == script::RunOutcome::Suspended);
        // The event, the two arguments, the call, the wait: the order the effects happen in. Each
        // break comes before its node's own effect and after every earlier node's.
        CY_CHECK(breaks == (std::vector<NodeKey>{1, 2, 3, 4, 5}));
        CY_CHECK(effects_at_break == (std::vector<usize>{0, 0, 1, 2, 3}));
        CY_CHECK(step_into.executes == (std::vector<bool>{true, false, false, true, true}));

        // The wait is satisfied: the handler resumes at the cue, and stops there first.
        host.arrived = true;
        step_into.seen.clear();
        outcome = on_native ? script::execute_native(*native, state, host, 4096, &step_into)
                            : script::execute(program.program(), state, host, 4096, &step_into);
        CY_REQUIRE(outcome.has_value());
        CY_CHECK(*outcome == script::RunOutcome::Paused);
        CY_CHECK(step_into.seen == (std::vector<NodeKey>{6}));
    }
}

CY_TEST_CASE("graph debugger: a breakpoint on the event stops before the handler's first node") {
    if constexpr (!script::kGraphDebuggerEnabled) {
        return;
    }
    GraphBuilder builder;
    builder.order();
    const script::EventProgram program = instrumented(compiled(builder));
    RecordingHost host;
    Breaker breaker;
    breaker.at = 1;
    script::ScriptState state(allocator(), program.program());
    std::vector<NodeKey> breaks;
    CY_CHECK(run_breaking(program, state, host, breaker, breaks) == script::RunOutcome::Suspended);
    CY_CHECK(breaks == (std::vector<NodeKey>{1}));
}

CY_TEST_CASE("graph debugger: pins and variables are read through the debug map") {
    if constexpr (!script::kGraphDebuggerEnabled) {
        return;
    }
    GraphBuilder builder;
    builder.order();
    const script::EventProgram program = instrumented(compiled(builder));
    auto native = script::compile_native(program.program(), allocator());
    CY_REQUIRE(native.has_value());
    const script::EventHandler* handler = program.handler(Name::intern("unit.command"));
    CY_REQUIRE(handler != nullptr);

    for (const bool on_native : {false, true}) {
        RecordingHost host;
        Breaker breaker;
        breaker.at = 5;
        script::ScriptState state(allocator(), program.program());
        auto outcome = on_native ? script::execute_native_from(*native, state, host, handler->block,
                                                               4096, &breaker)
                                 : script::execute_from(program.program(), state, host,
                                                        handler->block, 4096, &breaker);
        CY_REQUIRE(outcome.has_value());
        CY_REQUIRE(*outcome == script::RunOutcome::Paused);
        const script::BlockId here = state.paused_block();
        const script::PinReading x =
            script::read_pin(program.program(), state, 2, Name::intern("value"), here);
        const script::PinReading arg1 =
            script::read_pin(program.program(), state, 4, Name::intern("arg1"), here);
        const script::PinReading moved =
            script::read_pin(program.program(), state, 4, Name::intern("value"), here);
        CY_REQUIRE(x.found);
        CY_REQUIRE(arg1.found);
        CY_REQUIRE(moved.found);
        CY_CHECK_EQ(x.value.x, 3.0F);
        CY_CHECK(x.kind == script::ValueKind::Float);
        CY_CHECK_EQ(arg1.value.x, 4.0F);
        CY_CHECK_EQ(moved.value.x, 5.0F);  // what the host's call returned
        CY_CHECK(!script::read_pin(program.program(), state, 4, Name::intern("nonsense")).found);
    }

    // A variable, in one instance and not another.
    GraphBuilder counting;
    counting.counter();
    const script::EventProgram counter = instrumented(compiled(counting));
    const script::Variable* count = script::find_variable(counter.program(), 20);
    CY_REQUIRE(count != nullptr);
    CY_CHECK(script::find_variable_named(counter.program(), Name::intern("count")) == count);
    const script::EventHandler* up = counter.handler(Name::intern("count.up"));
    CY_REQUIRE(up != nullptr);
    RecordingHost host;
    script::ScriptState busy(allocator(), counter.program());
    script::ScriptState quiet(allocator(), counter.program());
    for (int run = 0; run < 3; ++run) {
        auto ran = script::execute_from(counter.program(), busy, host, up->block);
        CY_REQUIRE(ran.has_value());
    }
    CY_CHECK_EQ(script::read_variable(*count, busy).integer, 3);
    CY_CHECK_EQ(script::read_variable(*count, quiet).integer, 0);
}

CY_TEST_CASE("graph debugger: a program nobody debugs carries no probe and never calls the hook") {
    GraphBuilder builder;
    builder.order();
    const script::EventProgram program = compiled(builder);
    CY_CHECK_EQ(probe_count(program.program()), 0U);
    CY_CHECK(program.program().probes().empty());

    // Given a hook anyway, an uninstrumented program never consults it, on either back end.
    const script::EventHandler* handler = program.handler(Name::intern("unit.command"));
    CY_REQUIRE(handler != nullptr);
    auto native = script::compile_native(program.program(), allocator());
    CY_REQUIRE(native.has_value());
    RecordingHost host;
    Breaker breaker;
    breaker.every = true;
    script::ScriptState state(allocator(), program.program());
    auto ran = script::execute_from(program.program(), state, host, handler->block, 4096, &breaker);
    CY_REQUIRE(ran.has_value());
    CY_CHECK(*ran == script::RunOutcome::Suspended);
    script::ScriptState native_state(allocator(), program.program());
    ran = script::execute_native_from(*native, native_state, host, handler->block, 4096, &breaker);
    CY_REQUIRE(ran.has_value());
    CY_CHECK(*ran == script::RunOutcome::Suspended);
    CY_CHECK(breaker.seen.empty());

    if constexpr (script::kGraphDebuggerEnabled) {
        // The instrumented copy is the compiler's program with probes added and NOTHING else: the
        // same instructions in the same order, the same blocks, registers and suspend points.
        const script::EventProgram debug = instrumented(program);
        CY_CHECK_EQ(probe_count(debug.program()), debug.program().probes().size());
        CY_CHECK_EQ(debug.program().code().size(),
                    program.program().code().size() + debug.program().probes().size());
        CY_CHECK_EQ(debug.program().blocks().size(), program.program().blocks().size());
        CY_CHECK_EQ(debug.program().register_count(), program.program().register_count());
        CY_CHECK_EQ(debug.program().suspends().size(), program.program().suspends().size());
        CY_CHECK_NE(debug.program().digest(), program.program().digest());
        std::vector<script::ScriptOp> without;
        for (const script::Instruction& instruction : debug.program().code()) {
            if (instruction.op != script::ScriptOp::Probe) {
                without.push_back(instruction.op);
            }
        }
        std::vector<script::ScriptOp> original;
        for (const script::Instruction& instruction : program.program().code()) {
            original.push_back(instruction.op);
        }
        CY_CHECK(without == original);

        // And with no breakpoint it computes exactly what the original does.
        RecordingHost plain;
        RecordingHost probed;
        Breaker idle;
        script::ScriptState plain_state(allocator(), program.program());
        script::ScriptState probed_state(allocator(), debug.program());
        auto a = script::execute_from(program.program(), plain_state, plain, handler->block);
        auto b = script::execute_from(debug.program(), probed_state, probed, handler->block, 4096,
                                      &idle);
        CY_REQUIRE(a.has_value());
        CY_REQUIRE(b.has_value());
        CY_CHECK(*a == *b);
        CY_CHECK(plain.calls == probed.calls);
        CY_CHECK_EQ(plain_state.resume_block(), probed_state.resume_block());
        for (usize reg = 0; reg < plain_state.registers().size(); ++reg) {
            CY_CHECK_EQ(plain_state.registers()[reg].x, probed_state.registers()[reg].x);
            CY_CHECK_EQ(plain_state.registers()[reg].integer,
                        probed_state.registers()[reg].integer);
        }
    }
}

CY_TEST_CASE("graph debugger: compiled out of Profile and Shipping") {
    GraphBuilder builder;
    builder.order();
    const script::EventProgram program = compiled(builder);
    auto debug = script::instrument_for_debug(program);
    // `kGraphDebuggerEnabled` follows CY_DEVELOPMENT, so this is the shipping assertion in a
    // Profile or Shipping build and the development one elsewhere; CI's `profiles` job runs both.
#if defined(CY_DEVELOPMENT)
    CY_CHECK(script::kGraphDebuggerEnabled);
    CY_CHECK(debug.has_value());
#else
    CY_CHECK(!script::kGraphDebuggerEnabled);
    CY_REQUIRE(!debug.has_value());
    CY_CHECK(debug.error().code == ErrorCode::Unsupported);
#endif
}

CY_TEST_CASE("graph variables: a counter counts across handlers, per instance") {
    GraphBuilder builder;
    builder.counter();
    const script::EventProgram program = compiled(builder);
    CY_REQUIRE_EQ(program.program().variables().size(), 1U);
    const script::Variable& count = program.program().variables()[0];
    CY_CHECK_EQ(count.id, 20U);
    CY_CHECK(count.kind == script::ValueKind::Int);
    const script::EventHandler* up = program.handler(Name::intern("count.up"));
    CY_REQUIRE(up != nullptr);
    auto native = script::compile_native(program.program(), allocator());
    CY_REQUIRE(native.has_value());

    RecordingHost host;
    script::ScriptState bytecode(allocator(), program.program());
    script::ScriptState natively(allocator(), program.program());
    for (int run = 0; run < 4; ++run) {
        CY_REQUIRE(script::execute_from(program.program(), bytecode, host, up->block).has_value());
        CY_REQUIRE(script::execute_native_from(*native, natively, host, up->block).has_value());
    }
    CY_CHECK_EQ(script::read_variable(count, bytecode).integer, 4);
    CY_CHECK_EQ(script::read_variable(count, natively).integer, 4);
}

CY_TEST_CASE("graph variables: every misuse is an error on its node") {
    GraphBuilder builder;
    builder.counter();
    builder.node(30, "script.variable").prop(30, "name", "count").prop(30, "type", "int");
    builder.node(31, "script.variable").prop(31, "type", "int");
    builder.node(32, "script.variable").prop(32, "name", "label").prop(32, "type", "string");
    builder.node(33, "script.get_var").prop(33, "variable", "missing");
    builder.node(34, "script.set_var").prop(34, "variable", "count");
    builder.wire(14, "then", 34, "in").wire(33, "value", 34, "value");
    const std::vector<Diagnostic> found = refused(builder);
    const Diagnostic* duplicate = coded(found, "script.variable.duplicate");
    CY_REQUIRE(duplicate != nullptr);
    CY_CHECK_EQ(duplicate->node, 30U);
    CY_CHECK_EQ(duplicate->related_node, 20U);
    const Diagnostic* unnamed = coded(found, "script.variable.unnamed");
    CY_REQUIRE(unnamed != nullptr);
    CY_CHECK_EQ(unnamed->node, 31U);
    const Diagnostic* type = coded(found, "script.variable.type");
    CY_REQUIRE(type != nullptr);
    CY_CHECK_EQ(type->node, 32U);
    const Diagnostic* unknown = coded(found, "script.variable.unknown");
    CY_REQUIRE(unknown != nullptr);
    CY_CHECK_EQ(unknown->node, 33U);
    CY_CHECK_EQ(unknown->detail, Name::intern("missing"));
    // A declaration is state, not a step: it is never "unreachable".
    for (const Diagnostic& diagnostic : found) {
        CY_CHECK(!(std::string_view(diagnostic.code) == "script.node.unreachable" &&
                   diagnostic.node == 20U));
    }
}

CY_TEST_CASE("graph reload: a running counter keeps its count, by the variable's identity") {
    GraphBuilder before;
    before.counter();
    const script::EventProgram old_program = compiled(before);
    const script::EventHandler* up = old_program.handler(Name::intern("count.up"));
    CY_REQUIRE(up != nullptr);
    RecordingHost host;
    script::ScriptState running(allocator(), old_program.program());
    for (int run = 0; run < 7; ++run) {
        CY_REQUIRE(
            script::execute_from(old_program.program(), running, host, up->block).has_value());
    }

    // The edit: the variable is renamed, a second one is added with a default, and the step is now
    // two. Identity is the declaring node, so the renamed count keeps its value.
    GraphBuilder after;
    after.counter();
    after.prop(20, "name", "tally").prop(11, "variable", "tally").prop(14, "variable", "tally");
    after.literal(12, "value", whole(2));
    after.node(21, "script.variable").prop(21, "name", "bonus").prop(21, "type", "float");
    after.literal(21, "default", number(2.5F));
    const script::EventProgram new_program = compiled(after);

    DiagnosticSink sink(allocator());
    CY_CHECK_EQ(script::check_migration(old_program.program(), new_program.program(), sink), 0U);
    script::ScriptState moved(allocator(), new_program.program());
    auto migration =
        script::migrate_state(old_program.program(), running, new_program.program(), moved);
    CY_REQUIRE(migration.has_value());
    CY_CHECK_EQ(migration->kept, 1U);
    CY_CHECK_EQ(migration->added, 1U);
    CY_CHECK_EQ(migration->dropped, 0U);
    const script::Variable* tally = script::find_variable(new_program.program(), 20);
    const script::Variable* bonus = script::find_variable(new_program.program(), 21);
    CY_REQUIRE(tally != nullptr);
    CY_REQUIRE(bonus != nullptr);
    CY_CHECK_EQ(script::read_variable(*tally, moved).integer, 7);
    CY_CHECK_EQ(script::read_variable(*bonus, moved).x, 2.5F);

    // The new program runs on from there: seven, then nine.
    const script::EventHandler* again = new_program.handler(Name::intern("count.up"));
    CY_REQUIRE(again != nullptr);
    CY_REQUIRE(script::execute_from(new_program.program(), moved, host, again->block).has_value());
    CY_CHECK_EQ(script::read_variable(*tally, moved).integer, 9);

    // A variable the new program no longer declares is dropped.
    GraphBuilder emptied;
    emptied.node(10, "script.on_event").prop(10, "event", "count.up");
    const script::EventProgram bare = compiled(emptied);
    script::ScriptState stripped(allocator(), bare.program());
    auto dropped = script::migrate_state(old_program.program(), running, bare.program(), stripped);
    CY_REQUIRE(dropped.has_value());
    CY_CHECK_EQ(dropped->dropped, 1U);
    CY_CHECK_EQ(dropped->kept, 0U);
}

CY_TEST_CASE("graph reload: a variable that changed type is refused on its node") {
    GraphBuilder before;
    before.counter("int");
    GraphBuilder after;
    after.counter("float");
    const script::EventProgram old_program = compiled(before);
    const script::EventProgram new_program = compiled(after);
    DiagnosticSink sink(allocator());
    CY_CHECK_EQ(script::check_migration(old_program.program(), new_program.program(), sink), 1U);
    CY_REQUIRE_EQ(sink.entries().size(), 1U);
    CY_CHECK_EQ(std::string_view(sink.entries()[0].code), "script.reload.type");
    CY_CHECK_EQ(sink.entries()[0].node, 20U);
    CY_CHECK_EQ(sink.entries()[0].detail, Name::intern("int -> float"));
    script::ScriptState running(allocator(), old_program.program());
    script::ScriptState moved(allocator(), new_program.program());
    CY_CHECK(!script::migrate_state(old_program.program(), running, new_program.program(), moved));
}

CY_TEST_CASE("graph reload: a wait in progress survives when its wait node does") {
    GraphBuilder before;
    before.order();
    const script::EventProgram old_program = compiled(before);
    const script::EventHandler* handler = old_program.handler(Name::intern("unit.command"));
    CY_REQUIRE(handler != nullptr);
    RecordingHost host;
    script::ScriptState waiting(allocator(), old_program.program());
    CY_REQUIRE(
        script::execute_from(old_program.program(), waiting, host, handler->block).has_value());
    CY_REQUIRE(script::waiting_at(old_program.program(), waiting) != nullptr);

    // Edited: the arrival plays a different cue. The wait node is the same, so the unit keeps
    // waiting and, once it arrives, plays the NEW cue.
    GraphBuilder after;
    after.order().prop(6, "event", "cue.edited");
    const script::EventProgram new_program = compiled(after);
    script::ScriptState moved(allocator(), new_program.program());
    auto migration =
        script::migrate_state(old_program.program(), waiting, new_program.program(), moved);
    CY_REQUIRE(migration.has_value());
    CY_CHECK(migration->wait_kept);
    CY_CHECK(!migration->wait_dropped);
    // Still waiting, at the new program's own block for the same wait node.
    const script::SuspendPoint* still = script::waiting_at(new_program.program(), moved);
    CY_REQUIRE(still != nullptr);
    CY_CHECK_EQ(still->origin, 5U);
    host.calls.clear();
    host.arrived = true;
    auto resumed = script::execute(new_program.program(), moved, host);
    CY_REQUIRE(resumed.has_value());
    CY_CHECK(*resumed == script::RunOutcome::Finished);
    // Resumed after the wait, not started over: the cue is the one thing left to do.
    CY_CHECK(host.calls == std::vector<std::string>{"event cue.edited 0.000000 0.000000"});

    // Edited so the wait node is gone: the wait is dropped, said so, and nothing resumes.
    GraphBuilder unwaited;
    unwaited.node(1, "script.on_event").prop(1, "event", "unit.command");
    unwaited.node(6, "script.emit_event").prop(6, "event", "cue.done").wire(1, "then", 6, "in");
    const script::EventProgram immediate = compiled(unwaited);
    script::ScriptState idle(allocator(), immediate.program());
    auto dropped = script::migrate_state(old_program.program(), waiting, immediate.program(), idle);
    CY_REQUIRE(dropped.has_value());
    CY_CHECK(dropped->wait_dropped);
    CY_CHECK(!idle.suspended());
}

CY_TEST_CASE("graph reload: an instance paused mid-handler is not migrated") {
    if constexpr (!script::kGraphDebuggerEnabled) {
        return;
    }
    GraphBuilder builder;
    builder.order();
    const script::EventProgram program = instrumented(compiled(builder));
    RecordingHost host;
    Breaker breaker;
    breaker.at = 4;
    script::ScriptState state(allocator(), program.program());
    const script::EventHandler* handler = program.handler(Name::intern("unit.command"));
    CY_REQUIRE(handler != nullptr);
    auto ran = script::execute_from(program.program(), state, host, handler->block, 4096, &breaker);
    CY_REQUIRE(ran.has_value());
    CY_REQUIRE(*ran == script::RunOutcome::Paused);
    script::ScriptState moved(allocator(), program.program());
    auto migration = script::migrate_state(program.program(), state, program.program(), moved);
    CY_REQUIRE(!migration.has_value());
    CY_CHECK(migration.error().code == ErrorCode::Unavailable);
}

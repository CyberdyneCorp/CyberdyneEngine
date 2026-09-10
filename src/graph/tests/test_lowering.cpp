// The four consumer lowerings, and the one property each of them exists for. M8.b tasks 2.4, 2.5.
//
// design.md §1.7: "Build one front end, one expression core, and six back ends. Not one IR." These
// cases check the semantic that makes each back end necessary — the thing the shared expression
// core cannot do — rather than checking that the compilers run:
//
//   scripting   a suspension whose state is COMPACT, and one shared program over many instances
//   abilities   an ORDERED pipeline whose check stages run WITHOUT activating
//   animation   a LAZY branch: an unselected state's clip is never sampled, and neither is a
//               masked-out joint
//   artificial  RESUMPTION at the running node: a resumed tick does not re-test the conditions the
//   intelligence  previous tick already passed
//   cameras     a DECLARED PHASE BOUNDARY: every collision query in one batch, whatever the node
//               count, and frame-rate independent smoothing in half-life terms

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/lower_behaviour.h>
#include <cy/graph/lower_camera.h>
#include <cy/graph/lower_pose.h>
#include <cy/graph/lower_script.h>
#include <cy/test/test.h>

#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <utility>

using namespace cy;
using namespace cy::graph;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Scripting);
}

[[nodiscard]] Literal number(f32 value) noexcept {
    Literal literal;
    literal.type = Name::intern("float");
    literal.value = Immediate::scalar(value);
    return literal;
}

[[nodiscard]] Literal integer(u32 value) noexcept {
    Literal literal;
    literal.type = Name::intern("int");
    literal.value.mask = value;
    return literal;
}

[[nodiscard]] Literal text(const char* value) noexcept {
    Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(value);
    return literal;
}

// --- Scripting ----------------------------------------------------------------------------------

class RecordingHost final : public script::ScriptHost {
public:
    script::Value call(const script::ExternalRef& callee,
                       Span<const script::Value> arguments) override {
        calls += 1;
        last_call = callee.name;
        return arguments.empty() ? script::Value{} : arguments[0];
    }
    script::Value query(const script::ExternalRef& /*query*/,
                        Span<const script::Value> /*arguments*/) override {
        return script::Value::from_float(1.0F);
    }
    void emit_event(const script::ExternalRef& event,
                    Span<const script::Value> arguments) override {
        events += 1;
        last_event = event.name;
        event_argument = arguments.empty() ? 0.0F : arguments[0].x;
    }
    void emit_command(const script::ExternalRef& /*command*/,
                      Span<const script::Value> /*arguments*/) override {
        commands += 1;
    }
    script::Value get_field(const script::ExternalRef& /*field*/,
                            const script::Value& /*subject*/) override {
        return script::Value::from_float(0.0F);
    }
    void set_field(const script::ExternalRef& field, const script::Value& /*subject*/,
                   const script::Value& value) override {
        writes += 1;
        last_field = field.name;
        written = value.x;
    }
    [[nodiscard]] bool wait_satisfied(const script::SuspendPoint& /*point*/) override {
        return satisfied;
    }

    u32 calls = 0;
    u32 events = 0;
    u32 commands = 0;
    u32 writes = 0;
    f32 written = 0.0F;
    f32 event_argument = 0.0F;
    Name last_call;
    Name last_event;
    Name last_field;
    bool satisfied = false;
};

/// `entry -> set_field(health, 2 + 3) -> wait("landed") -> emit_event("done", 2 + 3)`, written into
/// a graph the caller owns — so a case that needs the graph on an allocator of its own can have
/// one.
[[nodiscard]] bool fill_script_graph(Graph& graph) noexcept {
    const auto add = [&graph](NodeKey key, const char* type) noexcept {
        return graph.add_node(key, Name::intern(type)).has_value();
    };
    const auto wire = [&graph](NodeKey from, const char* from_pin, NodeKey to,
                               const char* to_pin) noexcept {
        return graph.connect(from, Name::intern(from_pin), to, Name::intern(to_pin)).has_value();
    };
    bool good = add(1, "script.entry") && add(2, "script.const_float") &&
                add(3, "script.const_float") && add(4, "script.add_float") &&
                add(5, "script.set_field") && add(6, "script.wait") && add(7, "script.emit_event");
    good = good && graph.set_property(2, Name::intern("value"), number(2.0F)).has_value();
    good = good && graph.set_property(3, Name::intern("value"), number(3.0F)).has_value();
    good = good && graph.set_property(5, Name::intern("field"), text("health")).has_value();
    good = good && graph.set_property(6, Name::intern("reason"), text("landed")).has_value();
    good = good && graph.set_property(7, Name::intern("event"), text("done")).has_value();
    good = good && wire(2, "value", 4, "a") && wire(3, "value", 4, "b");
    good = good && wire(1, "then", 5, "in") && wire(4, "value", 5, "value");
    good = good && wire(5, "then", 6, "in") && wire(6, "then", 7, "in");
    good = good && wire(4, "value", 7, "arg0");
    return good;
}

[[nodiscard]] Expected<Graph, Error> script_graph() noexcept {
    Graph graph(allocator(), Name::intern("shout"));
    if (!fill_script_graph(graph)) {
        return make_unexpected(Error{ErrorCode::Internal, "the script fixture refused", 0});
    }
    return graph;
}

// --- Animation ----------------------------------------------------------------------------------

class CountingSampler final : public pose::PoseSampler {
public:
    void sample(const pose::ClipRef& clip, f32 /*time*/, const pose::JointMask& mask,
                Span<f32> out) override {
        ++samples;
        joints += mask.count();
        if (clip.name == Name::intern("upper")) {
            ++upper_samples;
        }
        if (clip.name == Name::intern("lower")) {
            ++lower_samples;
        }
        for (u32 joint = 0; joint < 8U; ++joint) {
            if (!mask.test(joint)) {
                continue;
            }
            out[static_cast<usize>(joint) * pose::kChannelsPerJoint] =
                static_cast<f32>(clip.name.text().size());
        }
    }
    void reference(const pose::JointMask& /*mask*/, Span<f32> /*out*/) override { ++references; }
    void solve_ik(u16 /*chain*/, const pose::JointMask& /*mask*/, Span<f32> /*pose*/) override {
        ++solves;
    }

    u32 samples = 0;
    u32 joints = 0;
    u32 upper_samples = 0;
    u32 lower_samples = 0;
    u32 references = 0;
    u32 solves = 0;
};

// --- Artificial intelligence ----------------------------------------------------------------

class ScriptedAgentHost final : public behaviour::BehaviourHost {
public:
    behaviour::BtStatus run_task(Name task, f32 /*dt*/) override {
        ++tasks;
        last_task = task;
        return task_result;
    }
    [[nodiscard]] bool test_condition(Name /*condition*/) override {
        ++conditions;
        return condition_result;
    }
    [[nodiscard]] f32 score(Name task) override {
        ++scores;
        return task == Name::intern("flee") ? flee_score : fight_score;
    }

    u32 tasks = 0;
    u32 conditions = 0;
    u32 scores = 0;
    Name last_task;
    behaviour::BtStatus task_result = behaviour::BtStatus::Success;
    bool condition_result = true;
    f32 fight_score = 0.4F;
    f32 flee_score = 0.6F;
};

// --- Cameras --------------------------------------------------------------------------------

class CountingBatch final : public camera::RigQueryBatch {
public:
    void resolve(Span<const camera::RigQuery> queries,
                 Span<camera::RigQueryResult> results) override {
        ++calls;
        resolved += static_cast<u32>(queries.size());
        for (usize index = 0; index < results.size(); ++index) {
            for (u32 channel = 0; channel < 3U; ++channel) {
                results[index].hit[channel] = queries[index].target[channel] * 0.5F;
            }
        }
    }

    u32 calls = 0;
    u32 resolved = 0;
};

// --- Two execution backends, and the requirement that they agree --------------------------------

/// `entry -> loop(while true) { emit_command("spin") }`. A back edge, a real branch and a
/// terminator that is not a return: the shapes the native back end resolves ahead of time.
[[nodiscard]] Expected<Graph, Error> loop_graph() noexcept {
    Graph graph(allocator(), Name::intern("spin"));
    const auto ok = [](const auto& result) noexcept { return result.has_value(); };
    bool good = ok(graph.add_node(1, Name::intern("script.entry"))) &&
                ok(graph.add_node(2, Name::intern("script.const_bool"))) &&
                ok(graph.add_node(3, Name::intern("script.loop"))) &&
                ok(graph.add_node(4, Name::intern("script.emit_command")));
    good = good && ok(graph.set_property(2, Name::intern("value"), integer(1)));
    good = good && ok(graph.set_property(4, Name::intern("command"), text("spin")));
    good = good && ok(graph.connect(1, Name::intern("then"), 3, Name::intern("in")));
    good = good && ok(graph.connect(2, Name::intern("value"), 3, Name::intern("condition")));
    good = good && ok(graph.connect(3, Name::intern("body"), 4, Name::intern("in")));
    good = good && ok(graph.connect(4, Name::intern("then"), 3, Name::intern("in")));
    if (!good) {
        return fail(ErrorCode::Internal, "the loop fixture could not be built");
    }
    return graph;
}

/// Every externally visible result one run had. Comparing outcomes alone would pass a back end
/// that emitted the right events in the wrong order, or wrote the wrong value into the world.
struct RunRecord {
    script::RunOutcome outcome = script::RunOutcome::Finished;
    u32 calls = 0;
    u32 events = 0;
    u32 commands = 0;
    u32 writes = 0;
    f32 written = 0.0F;
    f32 event_argument = 0.0F;
    Name last_event;
    Name last_field;
    bool suspended = false;
    u32 persisted = 0;
    u64 registers = 0;
};

[[nodiscard]] u64 mix(u64 seed, u64 value) noexcept {
    return (seed ^ value) * 1099511628211ULL;
}

[[nodiscard]] u64 register_digest(const script::ScriptState& state) noexcept {
    u64 digest = 14695981039346656037ULL;
    for (const script::Value& value : state.registers()) {
        u32 bits = 0;
        const f32 component = value.x;
        std::memcpy(&bits, &component, sizeof(bits));
        digest = mix(mix(mix(digest, static_cast<u64>(value.integer)), bits), value.handle);
    }
    return digest;
}

[[nodiscard]] RunRecord record_of(script::RunOutcome outcome, const RecordingHost& host,
                                  const script::ScriptState& state) noexcept {
    RunRecord record;
    record.outcome = outcome;
    record.calls = host.calls;
    record.events = host.events;
    record.commands = host.commands;
    record.writes = host.writes;
    record.written = host.written;
    record.event_argument = host.event_argument;
    record.last_event = host.last_event;
    record.last_field = host.last_field;
    record.suspended = state.suspended();
    record.persisted = state.persisted().size();
    record.registers = register_digest(state);
    return record;
}

void check_records_agree(const RunRecord& bytecode, const RunRecord& native) {
    CY_CHECK_EQ(bytecode.outcome, native.outcome);
    CY_CHECK_EQ(bytecode.calls, native.calls);
    CY_CHECK_EQ(bytecode.events, native.events);
    CY_CHECK_EQ(bytecode.commands, native.commands);
    CY_CHECK_EQ(bytecode.writes, native.writes);
    CY_CHECK_EQ(bytecode.written, native.written);
    CY_CHECK_EQ(bytecode.event_argument, native.event_argument);
    CY_CHECK_EQ(bytecode.last_event, native.last_event);
    CY_CHECK_EQ(bytecode.last_field, native.last_field);
    CY_CHECK_EQ(bytecode.suspended, native.suspended);
    CY_CHECK_EQ(bytecode.persisted, native.persisted);
    CY_CHECK_EQ(bytecode.registers, native.registers);
}

}  // namespace

CY_TEST_CASE("graph_script: one shared program, separate state, and a COMPACT suspension") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(script::register_script_nodes(registry).has_value());
    auto graph = script_graph();
    CY_REQUIRE(graph.has_value());
    graph.value().resolve(registry);

    DiagnosticSink sink(allocator());
    const script::ScriptCompileOptions options;
    auto program = script::compile_script(graph.value(), registry, options, sink);
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(sink.errors(), 0U);
    CY_CHECK_GT(program.value().blocks().size(), 1U);

    // THE DATA ACCESS DECLARATION the scheduler needs, emitted by compilation rather than written
    // beside the graph by hand.
    bool declares_write = false;
    for (const script::AccessDecl& access : program.value().accesses()) {
        declares_write = declares_write || (access.resource == Name::intern("health") &&
                                            access.mode == script::AccessMode::Write);
    }
    CY_CHECK(declares_write);

    // THE COMPACT STATE: fewer slots than registers, because only what is live across the
    // suspension is persisted.
    CY_CHECK_LT(program.value().state_slots().size(), program.value().register_count());
    CY_REQUIRE_EQ(program.value().suspends().size(), 1U);
    CY_CHECK_EQ(program.value().suspends()[0].reason, Name::intern("landed"));

    // TWO INSTANCES OVER ONE PROGRAM. There is no machine per entity: `execute` takes the program
    // and the state, and the program is `const`.
    script::ScriptState first(allocator(), program.value());
    script::ScriptState second(allocator(), program.value());
    RecordingHost host;
    auto ran = script::execute(program.value(), first, host, 256);
    CY_REQUIRE(ran.has_value());
    CY_CHECK_EQ(ran.value(), script::RunOutcome::Suspended);
    CY_CHECK_EQ(host.writes, 1U);
    CY_CHECK_EQ(host.written, 5.0F);
    CY_CHECK_EQ(host.events, 0U);
    CY_CHECK(first.suspended());
    CY_CHECK_LE(first.persisted().size(), program.value().state_slots().size());

    auto other = script::execute(program.value(), second, host, 256);
    CY_REQUIRE(other.has_value());
    CY_CHECK_EQ(other.value(), script::RunOutcome::Suspended);
    CY_CHECK_EQ(host.writes, 2U);

    // Resumed, the first instance carries on where it left off — and the second is untouched.
    host.satisfied = true;
    auto resumed = script::execute(program.value(), first, host, 256);
    CY_REQUIRE(resumed.has_value());
    CY_CHECK_EQ(resumed.value(), script::RunOutcome::Finished);
    CY_CHECK_EQ(host.events, 1U);
    CY_CHECK_EQ(host.last_event, Name::intern("done"));
    CY_CHECK_EQ(host.event_argument, 5.0F);
    CY_CHECK(second.suspended());
}

CY_TEST_CASE("graph_script: a runaway program is stopped by its budget rather than the frame") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(script::register_script_nodes(registry).has_value());
    Graph graph(allocator(), Name::intern("spin"));
    CY_REQUIRE(graph.add_node(1, Name::intern("script.entry")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("script.const_bool")).has_value());
    CY_REQUIRE(graph.add_node(3, Name::intern("script.loop")).has_value());
    CY_REQUIRE(graph.add_node(4, Name::intern("script.emit_command")).has_value());
    CY_REQUIRE(graph.set_property(2, Name::intern("value"), integer(1)).has_value());
    CY_REQUIRE(graph.set_property(4, Name::intern("command"), text("spin")).has_value());
    CY_REQUIRE(graph.connect(1, Name::intern("then"), 3, Name::intern("in")).has_value());
    CY_REQUIRE(graph.connect(2, Name::intern("value"), 3, Name::intern("condition")).has_value());
    CY_REQUIRE(graph.connect(3, Name::intern("body"), 4, Name::intern("in")).has_value());
    // The body's tail wires back to the loop: a BACK EDGE, which is the whole reason this consumer
    // does not lower through the shared expression core.
    CY_REQUIRE(graph.connect(4, Name::intern("then"), 3, Name::intern("in")).has_value());
    graph.resolve(registry);

    DiagnosticSink sink(allocator());
    const script::ScriptCompileOptions options;
    auto program = script::compile_script(graph, registry, options, sink);
    CY_REQUIRE(program.has_value());

    script::ScriptState state(allocator(), program.value());
    RecordingHost host;
    auto ran = script::execute(program.value(), state, host, 64);
    CY_REQUIRE(ran.has_value());
    CY_CHECK_EQ(ran.value(), script::RunOutcome::BudgetExhausted);
    CY_CHECK_GT(host.commands, 0U);
}

CY_TEST_CASE(
    "graph_ability: the check stages run in the specification's order, without activating") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(script::register_script_nodes(registry).has_value());
    CY_REQUIRE(script::register_ability_nodes(registry).has_value());

    Graph graph(allocator(), Name::intern("fireball"));
    // The stages are authored OUT OF ORDER on purpose: commit first, cost last. The compiler
    // collects them in the pipeline's order, not the author's.
    CY_REQUIRE(graph.add_node(1, Name::intern("ability.stage")).has_value());
    CY_REQUIRE(graph.set_property(1, Name::intern("stage"), text("commit")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("script.emit_command")).has_value());
    CY_REQUIRE(graph.set_property(2, Name::intern("command"), text("spend_mana")).has_value());
    CY_REQUIRE(graph.connect(1, Name::intern("then"), 2, Name::intern("in")).has_value());

    CY_REQUIRE(graph.add_node(3, Name::intern("ability.stage")).has_value());
    CY_REQUIRE(graph.set_property(3, Name::intern("stage"), text("check_cost")).has_value());
    CY_REQUIRE(graph.add_node(4, Name::intern("ability.refuse")).has_value());
    CY_REQUIRE(graph.set_property(4, Name::intern("reason"), text("not_enough_mana")).has_value());
    CY_REQUIRE(graph.connect(3, Name::intern("then"), 4, Name::intern("in")).has_value());
    graph.resolve(registry);

    DiagnosticSink sink(allocator());
    auto ability = script::compile_ability(graph, registry, sink);
    CY_REQUIRE(ability.has_value());
    CY_CHECK_NE(ability.value().stage(script::AbilityStage::CheckCost), script::kNoBlock);
    CY_CHECK_NE(ability.value().stage(script::AbilityStage::Commit), script::kNoBlock);
    // The cost check was authored second and is compiled first, because the ORDER IS THE
    // SPECIFICATION'S.
    CY_CHECK_LT(ability.value().stage(script::AbilityStage::CheckCost),
                ability.value().stage(script::AbilityStage::Commit));

    script::ScriptState state(allocator(), ability.value().program());
    RecordingHost host;
    auto result = script::validate_activation(ability.value(), state, host);
    CY_REQUIRE(result.has_value());
    CY_CHECK_FALSE(result.value().allowed);
    CY_CHECK_EQ(result.value().failed, script::AbilityStage::CheckCost);
    CY_CHECK_EQ(result.value().reason, Name::intern("not_enough_mana"));
    CY_CHECK_EQ(result.value().node, 4U);
    // NOTHING WAS ACTIVATED. The commit stage emits a command and no command was emitted.
    CY_CHECK_EQ(host.commands, 0U);
}

CY_TEST_CASE("graph_pose: a state nobody is in samples nothing") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(pose::register_pose_nodes(registry).has_value());
    Graph graph(allocator(), Name::intern("locomotion"));
    CY_REQUIRE(graph.add_node(1, Name::intern("pose.clip")).has_value());
    CY_REQUIRE(graph.set_property(1, Name::intern("clip"), text("lower")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("pose.clip")).has_value());
    CY_REQUIRE(graph.set_property(2, Name::intern("clip"), text("upper")).has_value());
    CY_REQUIRE(graph.add_node(3, Name::intern("pose.state")).has_value());
    CY_REQUIRE(graph.add_node(4, Name::intern("pose.state")).has_value());
    CY_REQUIRE(graph.connect(1, Name::intern("pose"), 3, Name::intern("pose")).has_value());
    CY_REQUIRE(graph.connect(2, Name::intern("pose"), 4, Name::intern("pose")).has_value());
    graph.resolve(registry);

    DiagnosticSink sink(allocator());
    auto program = pose::compile_pose(graph, registry, 8, sink);
    CY_REQUIRE(program.has_value());
    CY_REQUIRE_EQ(program.value().states().size(), 2U);

    Array<f32> out(allocator());
    CY_REQUIRE(out.resize(static_cast<usize>(8) * pose::kChannelsPerJoint).has_value());
    Array<f32> parameters(allocator());
    CY_REQUIRE(parameters.resize(4).has_value());

    pose::PoseInstance instance;
    CountingSampler sampler;
    pose::EvaluationReport report;
    CY_REQUIRE(
        pose::evaluate(program.value(), instance, parameters.span(), sampler, out.span(), report)
            .has_value());
    // `animation-and-skinning`: an unselected branch SHALL NOT be sampled. One state is active, so
    // one clip is sampled and the other is not touched.
    CY_CHECK_EQ(report.clips_sampled, 1U);
    CY_CHECK_EQ(sampler.lower_samples, 1U);
    CY_CHECK_EQ(sampler.upper_samples, 0U);
}

CY_TEST_CASE(
    "graph_pose: pose dependency analysis narrows a masked layer to the joints it writes") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(pose::register_pose_nodes(registry).has_value());
    Graph graph(allocator(), Name::intern("layers"));
    CY_REQUIRE(graph.add_node(1, Name::intern("pose.clip")).has_value());
    CY_REQUIRE(graph.set_property(1, Name::intern("clip"), text("lower")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("pose.clip")).has_value());
    CY_REQUIRE(graph.set_property(2, Name::intern("clip"), text("upper")).has_value());
    CY_REQUIRE(graph.add_node(3, Name::intern("pose.layer")).has_value());
    // The upper-body layer covers joints 4 to 7 only.
    CY_REQUIRE(graph.set_property(3, Name::intern("mask_first"), integer(4)).has_value());
    CY_REQUIRE(graph.set_property(3, Name::intern("mask_count"), integer(4)).has_value());
    CY_REQUIRE(graph.add_node(4, Name::intern("pose.state")).has_value());
    CY_REQUIRE(graph.connect(1, Name::intern("pose"), 3, Name::intern("a")).has_value());
    CY_REQUIRE(graph.connect(2, Name::intern("pose"), 3, Name::intern("b")).has_value());
    CY_REQUIRE(graph.connect(3, Name::intern("pose"), 4, Name::intern("pose")).has_value());
    graph.resolve(registry);

    DiagnosticSink sink(allocator());
    auto program = pose::compile_pose(graph, registry, 8, sink);
    CY_REQUIRE(program.has_value());

    Array<f32> out(allocator());
    CY_REQUIRE(out.resize(static_cast<usize>(8) * pose::kChannelsPerJoint).has_value());
    Array<f32> parameters(allocator());
    CY_REQUIRE(parameters.resize(4).has_value());
    pose::PoseInstance instance;
    CountingSampler sampler;
    pose::EvaluationReport report;
    CY_REQUIRE(
        pose::evaluate(program.value(), instance, parameters.span(), sampler, out.span(), report)
            .has_value());

    // "The lower-body joints of that layer's clips are never read, and they SHALL NOT be sampled":
    // the base clip is asked for all eight joints and the layer's clip for four.
    CY_CHECK_EQ(report.clips_sampled, 2U);
    CY_CHECK_EQ(report.joints_sampled, 12U);
}

CY_TEST_CASE("graph_pose: the state machine transitions, blends, and honours interruption") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(pose::register_pose_nodes(registry).has_value());
    Graph graph(allocator(), Name::intern("machine"));
    CY_REQUIRE(graph.add_node(1, Name::intern("pose.clip")).has_value());
    CY_REQUIRE(graph.set_property(1, Name::intern("clip"), text("lower")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("pose.clip")).has_value());
    CY_REQUIRE(graph.set_property(2, Name::intern("clip"), text("upper")).has_value());
    CY_REQUIRE(graph.add_node(3, Name::intern("pose.state")).has_value());
    CY_REQUIRE(graph.add_node(4, Name::intern("pose.state")).has_value());
    CY_REQUIRE(graph.connect(1, Name::intern("pose"), 3, Name::intern("pose")).has_value());
    CY_REQUIRE(graph.connect(2, Name::intern("pose"), 4, Name::intern("pose")).has_value());
    CY_REQUIRE(graph.add_node(5, Name::intern("pose.transition")).has_value());
    CY_REQUIRE(graph.set_property(5, Name::intern("condition"), text("jump")).has_value());
    CY_REQUIRE(graph.set_property(5, Name::intern("duration"), number(0.2F)).has_value());
    CY_REQUIRE(graph.set_property(5, Name::intern("interruption"), text("none")).has_value());
    CY_REQUIRE(graph.connect(3, Name::intern("pose"), 5, Name::intern("from")).has_value());
    CY_REQUIRE(graph.connect(4, Name::intern("pose"), 5, Name::intern("to")).has_value());
    graph.resolve(registry);

    DiagnosticSink sink(allocator());
    auto program = pose::compile_pose(graph, registry, 8, sink);
    CY_REQUIRE(program.has_value());
    CY_REQUIRE_EQ(program.value().transitions().size(), 1U);

    Array<f32> parameters(allocator());
    CY_REQUIRE(parameters.resize(program.value().parameters().size() + 1).has_value());
    for (f32& value : parameters) {
        value = 0.0F;
    }
    pose::PoseInstance instance;
    // The condition is false: nothing transitions, and only the current state is looked at.
    pose::advance(program.value(), instance, parameters.span(), 0.1F);
    CY_CHECK_EQ(instance.state, 0U);
    CY_CHECK_EQ(instance.transition, 0xFFFFU);

    parameters[program.value().transitions()[0].condition_param] = 1.0F;
    pose::advance(program.value(), instance, parameters.span(), 0.1F);
    CY_CHECK_EQ(instance.transition, 0U);
    CY_CHECK_EQ(instance.target, 1U);

    // Mid-blend, BOTH trees are evaluated and nothing else is.
    Array<f32> out(allocator());
    CY_REQUIRE(out.resize(static_cast<usize>(8) * pose::kChannelsPerJoint).has_value());
    CountingSampler sampler;
    pose::EvaluationReport report;
    CY_REQUIRE(
        pose::evaluate(program.value(), instance, parameters.span(), sampler, out.span(), report)
            .has_value());
    CY_CHECK_EQ(report.clips_sampled, 2U);

    // The blend completes and the machine settles in the target.
    pose::advance(program.value(), instance, parameters.span(), 0.3F);
    CY_CHECK_EQ(instance.state, 1U);
    CY_CHECK_EQ(instance.transition, 0xFFFFU);
}

CY_TEST_CASE("graph_behaviour: a resumed tick does not re-test what the last one already passed") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(behaviour::register_behaviour_nodes(registry).has_value());
    Graph graph(allocator(), Name::intern("guard"));
    CY_REQUIRE(graph.add_node(1, Name::intern("ai.root")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("ai.sequence")).has_value());
    CY_REQUIRE(graph.add_node(3, Name::intern("ai.condition")).has_value());
    CY_REQUIRE(graph.set_property(3, Name::intern("task"), text("sees_player")).has_value());
    CY_REQUIRE(graph.add_node(4, Name::intern("ai.wait")).has_value());
    CY_REQUIRE(graph.set_property(4, Name::intern("seconds"), number(1.0F)).has_value());
    CY_REQUIRE(graph.connect(2, Name::intern("node"), 1, Name::intern("child")).has_value());
    CY_REQUIRE(graph.connect(3, Name::intern("node"), 2, Name::intern("children")).has_value());
    CY_REQUIRE(graph.connect(4, Name::intern("node"), 2, Name::intern("children")).has_value());
    graph.resolve(registry);

    DiagnosticSink sink(allocator());
    auto program = behaviour::compile_behaviour(graph, registry, sink);
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(sink.errors(), 0U);

    behaviour::AgentState agent(allocator(), program.value());
    ScriptedAgentHost host;
    behaviour::TickReport first;
    auto status = behaviour::tick(program.value(), agent, host, 0.1F, first);
    CY_REQUIRE(status.has_value());
    CY_CHECK_EQ(status.value(), behaviour::BtStatus::Running);
    CY_CHECK_EQ(first.conditions_tested, 1U);
    CY_CHECK_FALSE(first.resumed);
    CY_CHECK_GT(agent.stack().size(), 0U);

    // RESUMPTION. `ai-system`: "execution resum[es] at the running node rather than re-descending
    // from the root each tick." The condition is not tested again, and fewer instructions run.
    behaviour::TickReport second;
    auto again = behaviour::tick(program.value(), agent, host, 0.1F, second);
    CY_REQUIRE(again.has_value());
    CY_CHECK_EQ(again.value(), behaviour::BtStatus::Running);
    CY_CHECK(second.resumed);
    CY_CHECK_EQ(second.conditions_tested, 0U);
    CY_CHECK_LT(second.instructions_evaluated, first.instructions_evaluated);

    // The wait finishes, the sequence succeeds, and the agent's stack empties.
    behaviour::TickReport third;
    auto finished = behaviour::tick(program.value(), agent, host, 1.0F, third);
    CY_REQUIRE(finished.has_value());
    CY_CHECK_EQ(finished.value(), behaviour::BtStatus::Success);
    CY_CHECK_EQ(agent.stack().size(), 0U);
}

CY_TEST_CASE("graph_behaviour: utility scoring picks one child, and hysteresis keeps it") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(behaviour::register_behaviour_nodes(registry).has_value());
    Graph graph(allocator(), Name::intern("mind"));
    CY_REQUIRE(graph.add_node(1, Name::intern("ai.root")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("ai.utility")).has_value());
    CY_REQUIRE(graph.add_node(3, Name::intern("ai.task")).has_value());
    CY_REQUIRE(graph.set_property(3, Name::intern("task"), text("fight")).has_value());
    CY_REQUIRE(graph.set_property(3, Name::intern("curve"), text("linear")).has_value());
    CY_REQUIRE(graph.set_property(3, Name::intern("hysteresis"), number(0.25F)).has_value());
    CY_REQUIRE(graph.add_node(4, Name::intern("ai.task")).has_value());
    CY_REQUIRE(graph.set_property(4, Name::intern("task"), text("flee")).has_value());
    CY_REQUIRE(graph.set_property(4, Name::intern("curve"), text("linear")).has_value());
    CY_REQUIRE(graph.connect(2, Name::intern("node"), 1, Name::intern("child")).has_value());
    CY_REQUIRE(graph.connect(3, Name::intern("node"), 2, Name::intern("children")).has_value());
    CY_REQUIRE(graph.connect(4, Name::intern("node"), 2, Name::intern("children")).has_value());
    graph.resolve(registry);

    DiagnosticSink sink(allocator());
    auto program = behaviour::compile_behaviour(graph, registry, sink);
    CY_REQUIRE(program.has_value());

    behaviour::AgentState agent(allocator(), program.value());
    ScriptedAgentHost host;
    host.fight_score = 0.4F;
    host.flee_score = 0.6F;
    behaviour::TickReport report;
    CY_REQUIRE(behaviour::tick(program.value(), agent, host, 0.1F, report).has_value());
    // Both children are SCORED and only the winner RUNS.
    CY_CHECK_EQ(host.scores, 2U);
    CY_CHECK_EQ(host.tasks, 1U);
    CY_CHECK_EQ(host.last_task, Name::intern("flee"));

    // Fight edges ahead by less than the incumbent's hysteresis... and the incumbent is `flee`,
    // which carries none, so the challenger takes it.
    host.fight_score = 0.65F;
    behaviour::TickReport second;
    CY_REQUIRE(behaviour::tick(program.value(), agent, host, 0.1F, second).has_value());
    CY_CHECK_EQ(host.last_task, Name::intern("fight"));

    // Now `fight` is the incumbent and DOES carry hysteresis: flee retaking the lead by 0.1 is not
    // enough, and the agent does not oscillate.
    host.flee_score = 0.75F;
    behaviour::TickReport third;
    CY_REQUIRE(behaviour::tick(program.value(), agent, host, 0.1F, third).has_value());
    CY_CHECK_EQ(host.last_task, Name::intern("fight"));
}

CY_TEST_CASE("graph_behaviour: GOAP is a budgeted search over a compiled operator table") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(behaviour::register_behaviour_nodes(registry).has_value());
    Graph graph(allocator(), Name::intern("forager"));
    CY_REQUIRE(graph.add_node(1, Name::intern("ai.root")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("ai.plan")).has_value());
    CY_REQUIRE(graph.set_property(2, Name::intern("goal"), integer(0x4U)).has_value());
    CY_REQUIRE(graph.connect(2, Name::intern("node"), 1, Name::intern("child")).has_value());
    // Two operators: gather wood, then build. Neither is reachable in one step.
    CY_REQUIRE(graph.add_node(3, Name::intern("ai.operator")).has_value());
    CY_REQUIRE(graph.set_property(3, Name::intern("task"), text("gather")).has_value());
    CY_REQUIRE(graph.set_property(3, Name::intern("sets"), integer(0x1U)).has_value());
    CY_REQUIRE(graph.add_node(4, Name::intern("ai.operator")).has_value());
    CY_REQUIRE(graph.set_property(4, Name::intern("task"), text("build")).has_value());
    CY_REQUIRE(graph.set_property(4, Name::intern("requires"), integer(0x1U)).has_value());
    CY_REQUIRE(graph.set_property(4, Name::intern("sets"), integer(0x4U)).has_value());
    graph.resolve(registry);

    DiagnosticSink sink(allocator());
    auto program = behaviour::compile_behaviour(graph, registry, sink);
    CY_REQUIRE(program.has_value());
    CY_REQUIRE_EQ(program.value().operators().size(), 2U);

    behaviour::AgentState agent(allocator(), program.value());
    ScriptedAgentHost host;
    behaviour::TickReport report;
    auto status = behaviour::tick(program.value(), agent, host, 0.1F, report);
    CY_REQUIRE(status.has_value());
    CY_CHECK_EQ(status.value(), behaviour::BtStatus::Running);
    CY_REQUIRE_EQ(agent.plan().size(), 2U);
    CY_CHECK_EQ(host.last_task, Name::intern("gather"));
    CY_CHECK_GT(report.plan_search_nodes, 0U);

    behaviour::TickReport second;
    CY_REQUIRE(behaviour::tick(program.value(), agent, host, 0.1F, second).has_value());
    CY_CHECK_EQ(host.last_task, Name::intern("build"));

    // A BUDGET THAT IS EXHAUSTED LEAVES NO PLAN, and says so, rather than committing to half of
    // one.
    behaviour::AgentState fresh(allocator(), program.value());
    const u32 expanded = behaviour::plan_towards(program.value(), fresh, 0x4U, 1);
    CY_CHECK_EQ(expanded, 1U);
    CY_CHECK_EQ(fresh.plan().size(), 0U);
}

CY_TEST_CASE("graph_camera: a declared phase boundary batches every query into ONE call") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(camera::register_camera_nodes(registry).has_value());
    Graph graph(allocator(), Name::intern("follow"));
    const auto add = [&graph](NodeKey key, const char* type) noexcept {
        return graph.add_node(key, Name::intern(type)).has_value();
    };
    const auto wire = [&graph](NodeKey from, NodeKey to, const char* to_pin) noexcept {
        return graph.connect(from, Name::intern("value"), to, Name::intern(to_pin)).has_value();
    };
    bool good = add(1, "camera.input") && add(2, "camera.input") && add(3, "camera.input") &&
                add(4, "camera.collide") && add(5, "camera.parameter") && add(6, "camera.input") &&
                add(7, "camera.smooth_half_life") && add(8, "camera.parameter") &&
                add(9, "camera.lens") && add(10, "camera.collide") && add(11, "camera.add") &&
                add(12, "camera.output");
    good = good && graph.set_property(1, Name::intern("name"), text("eye")).has_value();
    good = good && graph.set_property(1, Name::intern("type"), text("vector")).has_value();
    good = good && graph.set_property(2, Name::intern("name"), text("target")).has_value();
    good = good && graph.set_property(2, Name::intern("type"), text("vector")).has_value();
    good = good && graph.set_property(3, Name::intern("name"), text("state")).has_value();
    good = good && graph.set_property(3, Name::intern("type"), text("vector")).has_value();
    good = good && graph.set_property(5, Name::intern("name"), text("half_life")).has_value();
    good = good && graph.set_property(6, Name::intern("name"), text("dt")).has_value();
    good = good && graph.set_property(8, Name::intern("name"), text("focal")).has_value();
    // Two separate collision nodes, so "one call" is a statement about batching and not about there
    // being one query.
    good = good && graph.set_property(10, Name::intern("name"), text("")).has_value();
    good = good && wire(1, 4, "origin") && wire(2, 4, "target");
    good = good && wire(2, 10, "origin") && wire(1, 10, "target");
    good = good && wire(4, 11, "a") && wire(10, 11, "b");
    good = good && wire(3, 7, "previous") && wire(11, 7, "desired") && wire(5, 7, "half_life") &&
           wire(6, 7, "dt");
    good = good && wire(8, 9, "focal_length");
    good = good && graph.connect(7, Name::intern("value"), 12, Name::intern("pose")).has_value();
    good = good && graph.connect(9, Name::intern("value"), 12, Name::intern("lens")).has_value();
    good = good && graph.connect(7, Name::intern("value"), 12, Name::intern("state")).has_value();
    CY_REQUIRE(good);
    graph.resolve(registry);

    DiagnosticSink sink(allocator());
    auto program = camera::compile_rig(graph, registry, sink);
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(sink.errors(), 0U);
    // E4: the collide nodes end phase 0 and everything above them is phase 1.
    CY_CHECK_EQ(program.value().phase_count(), 2U);
    CY_CHECK_NE(program.value().ir_digest(), 0ULL);

    const Name input_names[] = {Name::intern("eye"), Name::intern("target"), Name::intern("state"),
                                Name::intern("dt")};
    const f32 input_values[] = {1.0F, 5.0F, 0.0F, 1.0F / 60.0F};
    const Name parameter_names[] = {Name::intern("half_life"), Name::intern("focal")};
    const f32 parameter_values[] = {0.25F, 35.0F};
    camera::RigInputs inputs;
    inputs.input_names = Span<const Name>(input_names, 4);
    inputs.input_values = Span<const f32>(input_values, 4);
    inputs.parameter_names = Span<const Name>(parameter_names, 2);
    inputs.parameter_values = Span<const f32>(parameter_values, 2);
    inputs.dt = 1.0F / 60.0F;

    camera::RigInstance instance;
    CountingBatch batch;
    camera::RigOutput out;
    CY_REQUIRE(camera::evaluate_rig(program.value(), inputs, instance, batch, out).has_value());
    // ONE CALL, TWO QUERIES. `camera-system` forbids "scattered synchronous casts from individual
    // rig nodes"; the phase boundary is how a pure expression DAG says so.
    CY_CHECK_EQ(batch.calls, 1U);
    CY_CHECK_EQ(batch.resolved, 2U);
    CY_CHECK_EQ(out.focal_length, 35.0F);
    // The smoothing state came out and was carried into the instance (E3).
    CY_CHECK(instance.primed);
}

CY_TEST_CASE("graph_camera: half-life smoothing is frame-rate independent") {
    // `camera-system`: smoothing "SHALL be expressed in physically meaningful terms — half-life, or
    // frequency and damping ratio". The check is the one that matters: the same elapsed time in two
    // steps and in one lands in the same place, to within the discretisation.
    const f32 half_life = 0.25F;
    const f32 desired = 10.0F;
    const auto step = [half_life, desired](f32 value, f32 dt) noexcept {
        const f32 alpha = 1.0F - std::pow(0.5F, dt / half_life);
        return value + ((desired - value) * alpha);
    };
    const f32 one_big = step(0.0F, 1.0F / 30.0F);
    const f32 two_small = step(step(0.0F, 1.0F / 60.0F), 1.0F / 60.0F);
    CY_CHECK_NEAR(one_big, two_small, 1e-5F);

    // A per-frame lerp factor, which is what an author writes when the spelling does not force the
    // question, is NOT frame-rate independent — and this is the failure the requirement exists for.
    const auto naive = [desired](f32 value) noexcept { return value + ((desired - value) * 0.1F); };
    const f32 naive_one = naive(0.0F);
    const f32 naive_two = naive(naive(0.0F));
    CY_CHECK_GT(naive_two - naive_one, 0.5F);
}

CY_TEST_CASE("graph_lowering: eight thousand instances over three shared programs, and teardown") {
    // ONE PROGRAM, MANY INSTANCES — and the destruction of the instances is the half that is easy
    // to get wrong and hard to see. This case builds the population `ROADMAP.md` budgets for, runs
    // it, and lets it all go out of scope in one step; under a sanitized build it is where a
    // use-after-free in a state's teardown would show up, and under any build it is where an
    // instance that quietly holds a pointer into its program would.
    NodeRegistry registry(allocator());
    CY_REQUIRE(script::register_script_nodes(registry).has_value());
    CY_REQUIRE(behaviour::register_behaviour_nodes(registry).has_value());
    CY_REQUIRE(pose::register_pose_nodes(registry).has_value());

    auto script_source = script_graph();
    CY_REQUIRE(script_source.has_value());
    script_source.value().resolve(registry);
    DiagnosticSink sink(allocator());
    const script::ScriptCompileOptions options;
    auto program = script::compile_script(script_source.value(), registry, options, sink);
    CY_REQUIRE(program.has_value());

    Graph behaviour_source(allocator(), Name::intern("crowd"));
    CY_REQUIRE(behaviour_source.add_node(1, Name::intern("ai.root")).has_value());
    CY_REQUIRE(behaviour_source.add_node(2, Name::intern("ai.sequence")).has_value());
    CY_REQUIRE(behaviour_source.add_node(3, Name::intern("ai.condition")).has_value());
    CY_REQUIRE(behaviour_source.set_property(3, Name::intern("task"), text("awake")).has_value());
    CY_REQUIRE(
        behaviour_source.connect(2, Name::intern("node"), 1, Name::intern("child")).has_value());
    CY_REQUIRE(
        behaviour_source.connect(3, Name::intern("node"), 2, Name::intern("children")).has_value());
    behaviour_source.resolve(registry);
    auto agents = behaviour::compile_behaviour(behaviour_source, registry, sink);
    CY_REQUIRE(agents.has_value());

    const u64 script_digest = program.value().digest();
    const u64 agent_digest = agents.value().digest();

    {
        Array<script::ScriptState> scripts(allocator());
        Array<behaviour::AgentState> minds(allocator());
        CY_REQUIRE(scripts.reserve(8000).has_value());
        CY_REQUIRE(minds.reserve(8000).has_value());
        RecordingHost host;
        ScriptedAgentHost agent_host;
        for (u32 index = 0; index < 8000U; ++index) {
            script::ScriptState state(allocator(), program.value());
            auto ran = script::execute(program.value(), state, host, 64);
            CY_REQUIRE(ran.has_value());
            CY_REQUIRE(scripts.push_back(std::move(state)).has_value());

            behaviour::AgentState agent(allocator(), agents.value());
            behaviour::TickReport report;
            CY_REQUIRE(
                behaviour::tick(agents.value(), agent, agent_host, 0.016F, report).has_value());
            CY_REQUIRE(minds.push_back(std::move(agent)).has_value());
        }
        CY_CHECK_EQ(host.writes, 8000U);
        CY_CHECK_EQ(agent_host.conditions, 8000U);
        CY_CHECK_EQ(scripts.size(), 8000U);
        // Every one of them is suspended on the same program, and the program did not move.
        CY_CHECK(scripts[0].suspended());
        CY_CHECK(scripts[7999].suspended());
    }

    // The shared programs outlive every instance and are byte-for-byte what they were.
    CY_CHECK_EQ(program.value().digest(), script_digest);
    CY_CHECK_EQ(agents.value().digest(), agent_digest);
}

CY_TEST_CASE("graph_script: the bytecode and native back ends agree, effect for effect") {
    // `visual-scripting`: "a graph SHALL produce identical results on either backend, which SHALL
    // be verified" and "a divergence SHALL be a defect". This is that verification, and it compares
    // the whole observable result — the outcome, every external effect the host saw, the
    // suspension state, the persisted slot count and a digest over the register file — because two
    // back ends that agree only on the outcome have not been checked.
    NodeRegistry registry(allocator());
    CY_REQUIRE(script::register_script_nodes(registry).has_value());
    auto graph = script_graph();
    CY_REQUIRE(graph.has_value());
    graph.value().resolve(registry);
    DiagnosticSink sink(allocator());
    const script::ScriptCompileOptions options;
    auto program = script::compile_script(graph.value(), registry, options, sink);
    CY_REQUIRE(program.has_value());

    auto native = script::compile_native(program.value(), allocator());
    CY_REQUIRE(native.has_value());
    // ONE INTERMEDIATE REPRESENTATION, TWO BACK ENDS: the cook key names the IR, not the back end.
    CY_CHECK_EQ(native.value().digest(), program.value().digest());
    CY_CHECK_EQ(native.value().steps().size(), program.value().code().size());
    CY_CHECK_EQ(native.value().block_starts().size(), program.value().blocks().size());

    // Run one: to the suspension.
    script::ScriptState interpreted(allocator(), program.value());
    script::ScriptState compiled(allocator(), program.value());
    RecordingHost interpreted_host;
    RecordingHost compiled_host;
    auto first = script::execute(program.value(), interpreted, interpreted_host, 256);
    auto second = script::execute_native(native.value(), compiled, compiled_host, 256);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(first.value(), script::RunOutcome::Suspended);
    check_records_agree(record_of(first.value(), interpreted_host, interpreted),
                        record_of(second.value(), compiled_host, compiled));

    // Run two: resumed, through the emission and to the end.
    interpreted_host.satisfied = true;
    compiled_host.satisfied = true;
    auto third = script::execute(program.value(), interpreted, interpreted_host, 256);
    auto fourth = script::execute_native(native.value(), compiled, compiled_host, 256);
    CY_REQUIRE(third.has_value());
    CY_REQUIRE(fourth.has_value());
    CY_CHECK_EQ(third.value(), script::RunOutcome::Finished);
    check_records_agree(record_of(third.value(), interpreted_host, interpreted),
                        record_of(fourth.value(), compiled_host, compiled));
}

CY_TEST_CASE("graph_script: an instance suspended on one back end resumes on the other") {
    // What makes the choice a BUILD CONFIGURATION rather than a fork: `ScriptState` is the same
    // object on both paths, so an instance saved by a development build running bytecode is
    // resumable by a shipping build running native. That is why a `Suspend` step carries its resume
    // point in the bytecode back end's terms as well as its own.
    NodeRegistry registry(allocator());
    CY_REQUIRE(script::register_script_nodes(registry).has_value());
    auto graph = script_graph();
    CY_REQUIRE(graph.has_value());
    graph.value().resolve(registry);
    DiagnosticSink sink(allocator());
    const script::ScriptCompileOptions options;
    auto program = script::compile_script(graph.value(), registry, options, sink);
    CY_REQUIRE(program.has_value());
    auto native = script::compile_native(program.value(), allocator());
    CY_REQUIRE(native.has_value());

    // Suspended by the bytecode back end, resumed by the native one.
    script::ScriptState crossing(allocator(), program.value());
    RecordingHost host;
    auto suspended = script::execute(program.value(), crossing, host, 256);
    CY_REQUIRE(suspended.has_value());
    CY_CHECK_EQ(suspended.value(), script::RunOutcome::Suspended);
    host.satisfied = true;
    auto resumed = script::execute_native(native.value(), crossing, host, 256);
    CY_REQUIRE(resumed.has_value());
    CY_CHECK_EQ(resumed.value(), script::RunOutcome::Finished);
    CY_CHECK_EQ(host.events, 1U);
    CY_CHECK_EQ(host.event_argument, 5.0F);

    // And the other way round: suspended by the native back end, resumed by the bytecode one.
    script::ScriptState back(allocator(), program.value());
    RecordingHost other;
    auto native_suspend = script::execute_native(native.value(), back, other, 256);
    CY_REQUIRE(native_suspend.has_value());
    CY_CHECK_EQ(native_suspend.value(), script::RunOutcome::Suspended);
    other.satisfied = true;
    auto bytecode_resume = script::execute(program.value(), back, other, 256);
    CY_REQUIRE(bytecode_resume.has_value());
    CY_CHECK_EQ(bytecode_resume.value(), script::RunOutcome::Finished);
    CY_CHECK_EQ(other.events, 1U);
}

CY_TEST_CASE("graph_script: the native back end stops a runaway at the same instruction count") {
    // A back edge on both paths. The budget is counted in the same unit on either back end, which
    // is what lets a scheduler's budget mean one thing across a build configuration change.
    NodeRegistry registry(allocator());
    CY_REQUIRE(script::register_script_nodes(registry).has_value());
    auto graph = loop_graph();
    CY_REQUIRE(graph.has_value());
    graph.value().resolve(registry);
    DiagnosticSink sink(allocator());
    const script::ScriptCompileOptions options;
    auto program = script::compile_script(graph.value(), registry, options, sink);
    CY_REQUIRE(program.has_value());
    auto native = script::compile_native(program.value(), allocator());
    CY_REQUIRE(native.has_value());

    script::ScriptState interpreted(allocator(), program.value());
    script::ScriptState compiled(allocator(), program.value());
    RecordingHost interpreted_host;
    RecordingHost compiled_host;
    auto first = script::execute(program.value(), interpreted, interpreted_host, 64);
    auto second = script::execute_native(native.value(), compiled, compiled_host, 64);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(first.value(), script::RunOutcome::BudgetExhausted);
    CY_CHECK_GT(interpreted_host.commands, 0U);
    check_records_agree(record_of(first.value(), interpreted_host, interpreted),
                        record_of(second.value(), compiled_host, compiled));
}

// --- Regression: a program digest may not close over a constant's padding
// -------------------------
//
// FOUND BY samples/08-vertical-slice, WHICH COMPILED ONE AUTHORED GRAPH THREE TIMES AND GOT THREE
// DIGESTS. `finish_digest` hashed each constant as raw bytes, and `script::Value` is thirty-two
// bytes of which four — between `z` and `handle` — are written by no member initialiser. So the
// digest closed over whatever memory the constant happened to land on: it moved between processes,
// and it did not reproduce under `--profile release` at all, which is what an indeterminate value
// looks like rather than what a logic error looks like.
//
// IT IS NOT COSMETIC. A `ScriptProgram`'s digest is a cook key and the back-end selection key
// `visual-scripting` requires to be stable, so an unstable one invalidates cooked data on every
// build and can select a different back end for the same program.

namespace {

/// The program digest, recomputed from the program's own published contents FIELD BY FIELD.
///
/// This deliberately restates `finish_digest`'s algorithm rather than calling it. That is what
/// makes the case a regression test instead of a tautology: the defect was one term of that sum
/// reading an object where it should read fields, and only a second, independent computation of
/// the same number can see the difference. It also makes the digest FORMAT a two-file edit, which
/// is proportionate for a number that is a cook key.
[[nodiscard]] u64 digest_from_the_fields(const script::ScriptProgram& program) noexcept {
    u64 digest = hash_u64(kHashSeed, program.code().size());
    for (const script::Instruction& instruction : program.code()) {
        digest = hash_u64(digest, static_cast<u64>(instruction.op));
        digest = hash_u64(digest, static_cast<u64>(instruction.kind));
        digest = hash_u64(digest, (static_cast<u64>(instruction.dst) << 32U) |
                                      (static_cast<u64>(instruction.a) << 16U) | instruction.b);
        digest =
            hash_u64(digest, (static_cast<u64>(instruction.immediate) << 32U) | instruction.target);
    }
    for (const script::Value& constant : program.constants()) {
        digest = script::hash_constant(digest, constant);
    }
    for (const script::ExternalRef& external : program.externals()) {
        digest = hash_text(digest, external.name.text());
    }
    return digest;
}

/// Whether any constant in the program has a non-zero byte where `Value`'s padding is — which is
/// what makes the comparison above discriminating rather than lucky. Measured on this tree it is
/// always true: the four bytes hold a fragment of a spilled stack address, so they vary with
/// address-space layout and therefore between processes and not within one.
[[nodiscard]] bool any_constant_has_dirty_padding(const script::ScriptProgram& program) noexcept {
    constexpr usize kPaddingOffset = 20;
    constexpr usize kPaddingBytes = 4;
    for (const script::Value& constant : program.constants()) {
        std::array<unsigned char, sizeof(script::Value)> raw{};
        std::memcpy(raw.data(), &constant, raw.size());
        for (usize index = kPaddingOffset; index < kPaddingOffset + kPaddingBytes; ++index) {
            if (raw[index] != 0) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

CY_TEST_CASE("graph_script: a program digest does not close over a constant's padding") {
    // FOUND BY samples/08-vertical-slice: one authored graph compiled three times gave three
    // digests, and the artefact's determinism act recorded it as a gap. `script::Value` is
    // thirty-two bytes of which four — between `z` and `handle` — are written by no member
    // initialiser, and `finish_digest` hashed the object. A `ScriptProgram`'s digest is a cook key
    // and the back-end selection key `visual-scripting` requires to be stable, so this is a defect
    // rather than a cosmetic difference.

    // The unit of it. Two values whose five fields agree and whose padding does not: a hash that
    // can tell them apart is a hash reading memory nobody wrote.
    alignas(script::Value) std::array<unsigned char, sizeof(script::Value)> zeroed{};
    alignas(script::Value) std::array<unsigned char, sizeof(script::Value)> poisoned{};
    zeroed.fill(0x00);
    poisoned.fill(0xAB);
    auto* quiet = new (static_cast<void*>(zeroed.data())) script::Value{};
    auto* loud = new (static_cast<void*>(poisoned.data())) script::Value{};
    quiet->x = 5.0F;
    loud->x = 5.0F;
    quiet->handle = 7;
    loud->handle = 7;
    CY_CHECK_EQ(script::hash_constant(kHashSeed, *quiet), script::hash_constant(kHashSeed, *loud));
    // And it is reading the fields rather than answering the seed.
    loud->z = 1.0F;
    CY_CHECK_NE(script::hash_constant(kHashSeed, *quiet), script::hash_constant(kHashSeed, *loud));

    // And the whole of it, over a real compilation: the program's own digest against the same sum
    // computed field by field. A `finish_digest` that went back to hashing the object would differ
    // here on this tree every time, because the padding is never zero.
    NodeRegistry registry(allocator());
    CY_REQUIRE(script::register_script_nodes(registry).has_value());
    auto graph = script_graph();
    CY_REQUIRE(graph.has_value());
    graph.value().resolve(registry);
    DiagnosticSink sink(allocator());
    const script::ScriptCompileOptions options;
    auto program = script::compile_script(graph.value(), registry, options, sink);
    CY_REQUIRE(program.has_value());
    CY_CHECK(any_constant_has_dirty_padding(program.value()));
    CY_CHECK_EQ(program.value().digest(), digest_from_the_fields(program.value()));
}

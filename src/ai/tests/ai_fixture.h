#pragma once
// The one place that knows how to build a behaviour graph, a host and a world for an AI test.
// M8.b section 6.
//
// Every suite here needs the same three things, and writing them inline four times would make four
// subtly different agents whose differences would be the first suspect in every failure.

#include <cy/ai/agent.h>
#include <cy/core/memory/allocator.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/lower_behaviour.h>
#include <cy/test/test.h>

#include <utility>

namespace cy::ai::testing {

using graph::DiagnosticSink;
using graph::Graph;
using graph::Literal;
using graph::NodeRegistry;

[[nodiscard]] inline Literal number(f32 value) noexcept {
    Literal literal;
    literal.type = Name::intern("float");
    literal.value = graph::Immediate::scalar(value);
    return literal;
}

/// A bit set, as the behaviour compiler reads a GOAP predicate mask.
[[nodiscard]] inline Literal bits(u32 value) noexcept {
    Literal literal;
    literal.type = Name::intern("int");
    literal.value.mask = value;
    return literal;
}

[[nodiscard]] inline Literal text(const char* value) noexcept {
    Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(value);
    return literal;
}

/// `root -> sequence { condition("sees_player"), task("chase") }`. The smallest graph that has a
/// condition to guard, a task to run and a resumable middle.
[[nodiscard]] inline graph::behaviour::BehaviourProgram patrol_program(Allocator& allocator,
                                                                       NodeRegistry& registry,
                                                                       DiagnosticSink& sink) {
    Graph tree(allocator, Name::intern("patrol"));
    CY_REQUIRE(tree.add_node(1, Name::intern("ai.root")).has_value());
    CY_REQUIRE(tree.add_node(2, Name::intern("ai.sequence")).has_value());
    CY_REQUIRE(tree.add_node(3, Name::intern("ai.condition")).has_value());
    CY_REQUIRE(tree.set_property(3, Name::intern("task"), text("sees_player")).has_value());
    CY_REQUIRE(tree.add_node(4, Name::intern("ai.task")).has_value());
    CY_REQUIRE(tree.set_property(4, Name::intern("task"), text("chase")).has_value());
    CY_REQUIRE(tree.connect(2, Name::intern("node"), 1, Name::intern("child")).has_value());
    CY_REQUIRE(tree.connect(3, Name::intern("node"), 2, Name::intern("children")).has_value());
    CY_REQUIRE(tree.connect(4, Name::intern("node"), 2, Name::intern("children")).has_value());
    tree.resolve(registry);

    Expected<graph::behaviour::BehaviourProgram, Error> program =
        graph::behaviour::compile_behaviour(tree, registry, sink);
    CY_REQUIRE(program.has_value());
    return std::move(*program);
}

/// A host that answers whatever a case tells it to, and counts what it was asked.
class CountingHost final : public graph::behaviour::BehaviourHost {
public:
    graph::behaviour::BtStatus run_task(Name task, f32 /*dt*/) override {
        ++tasks;
        last_task = task;
        return task_result;
    }
    [[nodiscard]] bool test_condition(Name /*condition*/) override {
        ++conditions;
        return condition_result;
    }
    [[nodiscard]] f32 score(Name /*task*/) override {
        ++scores;
        return 0.5F;
    }

    u32 tasks = 0;
    u32 conditions = 0;
    u32 scores = 0;
    Name last_task;
    graph::behaviour::BtStatus task_result = graph::behaviour::BtStatus::Success;
    bool condition_result = true;
};

}  // namespace cy::ai::testing

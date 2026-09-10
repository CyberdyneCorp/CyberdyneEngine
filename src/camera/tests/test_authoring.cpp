// A camera rig authored in CyberGraph, compiled, and evaluated. M8.b task 7.3.
//
// INTEGRATION, not unit, per hard rule 7: these cases run two compilers — CyberGraph's validator
// and the camera server's rig compiler — and drive a `CameraServer`. A compiler inside a suite with
// a one-millisecond budget is a suite that goes red on a loaded machine.
//
// WHAT THIS FILE IS ACTUALLY ASSERTING. M8.b's design says one authoring layer serves every
// consumer: "CyberGraph, the shared authoring layer ... Every one of the seven adopts it." For a
// camera that means a rig graph is edited, diffed, merged and validated by the same machinery as an
// ability or a behaviour tree, and then becomes the compact rig program `camera-system` requires —
// "compiled at cook time", "no per-node allocation or virtual dispatch". These cases walk that
// whole path and then check the camera moved.

#include <cy/camera/authoring.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/graph/text.h>
#include <cy/servers/camera/server.h>
#include <cy/test/test.h>

#include <cmath>
#include <utility>

using namespace cy;
using namespace cy::camera;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

[[nodiscard]] graph::Literal text(const char* value) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(value);
    return literal;
}

[[nodiscard]] graph::Literal number(f32 value) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("float");
    literal.value = graph::Immediate::scalar(value);
    return literal;
}

[[nodiscard]] graph::Literal vector(f32 x, f32 y, f32 z) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("vec3");
    literal.value = graph::Immediate{x, y, z, 0.0F, 0};
    return literal;
}

/// A third-person rig, authored as a graph: target → follow → look-at → lens → output.
///
/// `camera-system`'s "Composition, not inheritance" scenario, as an actual composition rather than
/// as a comment: five nodes wired in a chain, and no type anywhere inherits from anything.
[[nodiscard]] bool author_follow_rig(graph::Graph& rig, bool with_look_at = true) noexcept {
    const auto add = [&rig](graph::NodeKey key, const char* type, const char* id) noexcept {
        return rig.add_node(key, Name::intern(type)).has_value() &&
               rig.set_property(key, Name::intern("id"), text(id)).has_value();
    };
    const auto wire = [&rig](graph::NodeKey from, graph::NodeKey to) noexcept {
        return rig.connect(from, Name::intern("out"), to, Name::intern("in")).has_value();
    };

    bool good = add(1, "rig.target", "target") && add(2, "rig.follow", "follow") &&
                add(4, "rig.lens", "lens") && add(5, "rig.output", "out");
    good =
        good && rig.set_property(2, Name::intern("offset"), vector(0.0F, 2.0F, 6.0F)).has_value();
    good = good && rig.set_property(2, Name::intern("half_life"), number(0.0F)).has_value();
    good = good && rig.set_property(4, Name::intern("near_value"), number(1.0F)).has_value();
    good = good && rig.set_property(4, Name::intern("far_value"), number(1.0F)).has_value();
    good = good && rig.set_property(4, Name::intern("half_life"), number(0.0F)).has_value();

    if (with_look_at) {
        good = good && add(3, "rig.look_at", "look") &&
               rig.set_property(3, Name::intern("half_life"), number(0.0F)).has_value();
        good = good && wire(1, 2) && wire(2, 3) && wire(3, 4) && wire(4, 5);
    } else {
        good = good && wire(1, 2) && wire(2, 4) && wire(4, 5);
    }
    return good;
}

}  // namespace

CY_TEST_CASE("camera_authoring: a graph-authored rig compiles and moves a camera") {
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(register_rig_nodes(registry).has_value());

    graph::Graph rig(allocator(), Name::intern("third-person"));
    CY_REQUIRE(author_follow_rig(rig));
    rig.resolve(registry);

    // THE SHARED VALIDATOR. A rig graph is checked by the same node- and pin-precise validation as
    // every other CyberGraph consumer's, which is the promise the authoring layer actually makes.
    graph::DiagnosticSink validation(allocator());
    CY_REQUIRE(graph::validate(rig, registry, nullptr, validation).has_value());
    CY_CHECK_EQ(validation.errors(), 0U);

    graph::DiagnosticSink sink(allocator());
    auto definition = rig_definition_from_graph(rig, registry, allocator(), sink);
    CY_REQUIRE(definition.has_value());
    CY_CHECK_EQ(sink.errors(), 0U);
    CY_CHECK_EQ(definition.value().nodes.size(), 5U);
    CY_CHECK_EQ(definition.value().name, Name::intern("third-person"));

    CameraServer server(allocator());
    CY_REQUIRE(server.initialize().has_value());
    auto handle = server.create_definition(definition.value());
    CY_REQUIRE(handle.has_value());
    RigConfig config;
    config.name = Name::intern("player");
    auto camera = server.create_rig(handle.value(), config);
    CY_REQUIRE(camera.has_value());

    TargetBinding binding;
    binding.kind = TargetKind::Entity;
    binding.stable_id = 42;
    CY_REQUIRE(server.set_target(camera.value(), binding).has_value());

    TargetSample sample;
    sample.stable_id = 42;
    sample.transform = Transform::from_translation(Vec3{10.0F, 0.0F, 0.0F});
    sample.valid = true;

    EvaluationContext context;
    context.delta_seconds = 1.0F / 60.0F;
    context.targets = Span<const TargetSample>(&sample, 1);
    auto evaluated = server.evaluate(camera.value(), context);
    CY_REQUIRE(evaluated.has_value());

    // The rig placed the camera behind and above the target, and pointed it back at it: the
    // authored offset, evaluated by a compiled program.
    const EvaluatedCamera* result = evaluated.value();
    CY_CHECK_NEAR(result->pose.translation.x, 10.0F, 1e-3F);
    CY_CHECK_NEAR(result->pose.translation.y, 2.0F, 1e-3F);
    CY_CHECK_NEAR(result->pose.translation.z, 6.0F, 1e-3F);
    const Vec3 forward = result->pose.rotation * Vec3{0.0F, 0.0F, -1.0F};
    CY_CHECK_LT(forward.z, 0.0F);
    CY_CHECK_NEAR(result->lens.vertical_fov_radians(), 1.0F, 1e-3F);
    server.shutdown();
}

CY_TEST_CASE("camera_authoring: a muted node lowers to nothing rather than being deleted") {
    // `visual-scripting`'s authoring layer keeps an author's mute in the graph and lowers it to
    // whatever "nothing" means in the target domain. For a rig that means the node does not reach
    // the definition — and the graph still has it, so unmuting is not a re-author.
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(register_rig_nodes(registry).has_value());
    graph::Graph rig(allocator(), Name::intern("muted"));
    CY_REQUIRE(author_follow_rig(rig));
    CY_REQUIRE(rig.mute(3, true).has_value());  // the look-at node
    rig.resolve(registry);

    graph::DiagnosticSink sink(allocator());
    auto definition = rig_definition_from_graph(rig, registry, allocator(), sink);
    CY_REQUIRE(definition.has_value());
    CY_CHECK_EQ(definition.value().nodes.size(), 4U);
    CY_CHECK_EQ(rig.nodes().size(), 5U);
}

CY_TEST_CASE("camera_authoring: a rig graph survives a round trip through the text form") {
    // The authoring layer's canonical text is what a diff diffs and a merge merges. A rig graph is
    // an ordinary CyberGraph document, so it round-trips byte-identically like any other — and that
    // is the property that makes "one editor, one diff format" true for cameras.
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(register_rig_nodes(registry).has_value());
    graph::Graph rig(allocator(), Name::intern("round-trip"));
    CY_REQUIRE(author_follow_rig(rig));
    rig.resolve(registry);

    Array<char> written(allocator());
    CY_REQUIRE(graph::write_graph(rig, written).has_value());
    CY_CHECK_GT(written.size(), 0U);

    graph::DiagnosticSink parse_sink(allocator());
    auto parsed = graph::parse_graph(std::string_view(written.data(), written.size()), &registry,
                                     allocator(), parse_sink);
    CY_REQUIRE(parsed.has_value());
    CY_CHECK_EQ(parse_sink.errors(), 0U);

    Array<char> again(allocator());
    CY_REQUIRE(graph::write_graph(parsed.value(), again).has_value());
    CY_REQUIRE_EQ(again.size(), written.size());
    CY_CHECK_EQ(std::string_view(again.data(), again.size()),
                std::string_view(written.data(), written.size()));

    // And the definition built from the re-read graph is the one built from the original.
    graph::DiagnosticSink sink(allocator());
    auto first = rig_definition_from_graph(rig, registry, allocator(), sink);
    auto second = rig_definition_from_graph(parsed.value(), registry, allocator(), sink);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_REQUIRE_EQ(first.value().nodes.size(), second.value().nodes.size());
    // MATCHED BY ID, NOT BY INDEX. The canonical text writes nodes in key order while a live graph
    // holds them in the order they were added, so the two definitions carry the same nodes in a
    // different order — and that is the text form doing its job: a diff of two graphs that were
    // edited in different orders should be empty. What must survive the round trip is each node's
    // identity and what it is wired to, which is what a rig program is compiled from.
    for (const RigNodeDesc& node : first.value().nodes.span()) {
        const RigNodeDesc* twin = nullptr;
        for (const RigNodeDesc& candidate : second.value().nodes.span()) {
            if (candidate.id == node.id) {
                twin = &candidate;
                break;
            }
        }
        CY_REQUIRE(twin != nullptr);
        CY_CHECK_EQ(twin->input, node.input);
        CY_CHECK_EQ(static_cast<u32>(twin->kind), static_cast<u32>(node.kind));
    }
}

CY_TEST_CASE("camera_authoring: a node that is not a rig node is refused, by node") {
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(register_rig_nodes(registry).has_value());
    graph::Graph rig(allocator(), Name::intern("wrong"));
    CY_REQUIRE(author_follow_rig(rig));
    CY_REQUIRE(rig.add_node(9, Name::intern("script.branch")).has_value());
    rig.resolve(registry);

    graph::DiagnosticSink sink(allocator());
    const auto definition = rig_definition_from_graph(rig, registry, allocator(), sink);
    CY_REQUIRE_FALSE(definition.has_value());
    CY_CHECK_EQ(definition.error().code, ErrorCode::InvalidArgument);
    // NODE-PRECISE: the diagnostic names the node and the type, not the graph. A diagnostic that
    // named only the graph would send an author hunting through a canvas.
    CY_REQUIRE(sink.entries().size() > 0U);
    CY_CHECK_EQ(sink.entries()[0].node, 9U);
    CY_CHECK_EQ(sink.entries()[0].detail, Name::intern("script.branch"));
}

CY_TEST_CASE("camera_authoring: a rig with no output, and one with a floating node, are refused") {
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(register_rig_nodes(registry).has_value());

    graph::Graph headless(allocator(), Name::intern("headless"));
    const auto add = [&headless](graph::NodeKey key, const char* type, const char* id) noexcept {
        return headless.add_node(key, Name::intern(type)).has_value() &&
               headless.set_property(key, Name::intern("id"), text(id)).has_value();
    };
    CY_REQUIRE(add(1, "rig.target", "target"));
    CY_REQUIRE(add(2, "rig.follow", "follow"));
    CY_REQUIRE(headless.connect(1, Name::intern("out"), 2, Name::intern("in")).has_value());
    headless.resolve(registry);

    graph::DiagnosticSink sink(allocator());
    const auto no_output = rig_definition_from_graph(headless, registry, allocator(), sink);
    CY_REQUIRE_FALSE(no_output.has_value());
    CY_CHECK_EQ(sink.errors(), 1U);

    // A node with no input, which is not a target: a rig is a chain from a target, and a floating
    // node is an authoring mistake the compiler downstream would report less clearly.
    graph::Graph floating(allocator(), Name::intern("floating"));
    const auto add_floating = [&floating](graph::NodeKey key, const char* type,
                                          const char* id) noexcept {
        return floating.add_node(key, Name::intern(type)).has_value() &&
               floating.set_property(key, Name::intern("id"), text(id)).has_value();
    };
    CY_REQUIRE(add_floating(1, "rig.target", "target"));
    CY_REQUIRE(add_floating(2, "rig.follow", "follow"));
    CY_REQUIRE(add_floating(3, "rig.output", "out"));
    CY_REQUIRE(floating.connect(1, Name::intern("out"), 3, Name::intern("in")).has_value());
    floating.resolve(registry);

    graph::DiagnosticSink floating_sink(allocator());
    const auto orphan = rig_definition_from_graph(floating, registry, allocator(), floating_sink);
    CY_REQUIRE_FALSE(orphan.has_value());
    CY_REQUIRE(floating_sink.entries().size() > 0U);
    CY_CHECK_EQ(floating_sink.entries()[0].node, 2U);
}

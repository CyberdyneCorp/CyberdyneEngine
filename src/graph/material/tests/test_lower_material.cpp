// The material lowering: the palette, the ports, and the third front end. M11.c task 6.1a.
//
// WHAT THESE CASES ARE WRITTEN TO BE ABLE TO SAY NO TO.
//
// `material-compiler` requires that "a material SHALL be authorable as a node graph or as a text
// material definition, and both SHALL be front-ends producing the same material IR". M7 built both
// and tested them against each other — but the node graph it tested is a `MaterialGraph` assembled
// in C++, and **an editor cannot produce one**: what an editor holds is a `cy::graph::Graph`, the
// shared authoring layer `visual-scripting` requires every graph editor to be built on.
//
// M11.c's spike measured the consequence: the material editor refuses to open, because
// `Domain::Materials::node_types()` is empty, because the engine declared no material vocabulary.
//
// So the case below is a THIRD front end compared against the other two: an authored CyberGraph —
// keys, node types, properties and wires, exactly what the canvas edits — lowered through
// `lower_material` and required to produce the SAME IR DIGEST as M7's hand-built `MaterialGraph`
// and the SAME COOK KEY as the text definition. A lowering that dropped the muted node, tidied the
// orphan away, or numbered one port differently would change the digest and fail.

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/material/lower_material.h>
#include <cy/rendering/material/compiler.h>
#include <cy/rendering/material/text.h>
#include <cy/test/test.h>

#include <algorithm>
#include <string>
#include <vector>

#include "fixtures.h"

using cy::Name;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;
using cy::graph::Graph;
using cy::graph::Literal;
using cy::graph::NodeKey;
using cy::graph::material::lower_material;
using cy::graph::material::material_node_types;
using cy::rendering::material::CompileOptions;
using cy::rendering::material::GraphOp;
using cy::rendering::material::MaterialGraph;
using cy::rendering::material::ValueType;

namespace {

cy::Allocator& allocator() {
    return cy::system_allocator(cy::MemoryDomain::Assets);
}

/// A small builder for an authored graph, so the fixture below reads like a canvas session.
class Canvas {
public:
    explicit Canvas(const char* name) : graph_(allocator(), Name::intern(name)) {}

    NodeKey add(const char* type) {
        const NodeKey key = graph_.allocate_key();
        good_ = good_ && graph_.add_node(key, Name::intern(type)).has_value();
        return key;
    }

    void symbol(NodeKey key, const char* text) {
        Literal literal;
        literal.type = Name::intern("name");
        literal.text = Name::intern(text);
        good_ = good_ && graph_.set_property(key, Name::intern("symbol"), literal).has_value();
    }

    void type_of(NodeKey key, ValueType type) {
        Literal literal;
        literal.type = Name::intern("name");
        literal.text = Name::intern(cy::rendering::material::value_type_name(type));
        good_ = good_ && graph_.set_property(key, Name::intern("type"), literal).has_value();
    }

    void value(NodeKey key, const char* property, cy::f32 x, cy::f32 y, cy::f32 z, cy::f32 w,
               u32 mask) {
        Literal literal;
        literal.type = Name::intern("vec4");
        literal.value = cy::graph::Immediate{x, y, z, w, mask};
        good_ = good_ && graph_.set_property(key, Name::intern(property), literal).has_value();
    }

    void flag(NodeKey key, const char* property) {
        Literal literal;
        literal.type = Name::intern("bool");
        literal.value.mask = 1;
        good_ = good_ && graph_.set_property(key, Name::intern(property), literal).has_value();
    }

    void wire(NodeKey from, NodeKey to, const char* pin) {
        good_ =
            good_ && graph_.connect(from, Name::intern("out"), to, Name::intern(pin)).has_value();
    }

    void mute(NodeKey key) { good_ = good_ && graph_.mute(key, true).has_value(); }

    [[nodiscard]] bool good() const { return good_; }
    [[nodiscard]] const Graph& graph() const { return graph_; }

private:
    Graph graph_;
    bool good_ = true;
};

/// The reference material, authored on a canvas.
///
/// Node for node and wart for wart the same material as
/// `cy::rendering::material::testing::build_reference_graph` — the untouched weight ports, the
/// pairwise closure sums, the texture dragged in twice, the reversed operand order, the tint left
/// at one, the muted emission and the orphan. The declarations come out of the nodes themselves,
/// which is what an editor has: a texture is declared because somebody dropped a sample node on the
/// canvas, not because a separate list was edited.
[[nodiscard]] bool author_reference(Canvas& canvas) {
    const u8 components[] = {0, 1, 2};
    const u32 swizzle_xyz =
        cy::rendering::material::Builder::swizzle_mask(cy::Span<const u8>(components, 3));
    const u8 first[] = {0};
    const u32 swizzle_x =
        cy::rendering::material::Builder::swizzle_mask(cy::Span<const u8>(first, 1));

    const NodeKey uv = canvas.add("material.attribute");
    canvas.symbol(uv, "uv0");
    canvas.type_of(uv, ValueType::Vec2);

    const NodeKey albedo_sample = canvas.add("material.texture_sample");
    canvas.symbol(albedo_sample, "base_color_map");
    canvas.type_of(albedo_sample, ValueType::Vec4);
    canvas.value(albedo_sample, "average", 0.5F, 0.5F, 0.5F, 1.0F, 0);
    canvas.wire(uv, albedo_sample, "uv");

    const NodeKey albedo_swizzle = canvas.add("material.swizzle");
    canvas.type_of(albedo_swizzle, ValueType::Count);
    canvas.value(albedo_swizzle, "value", 0.0F, 0.0F, 0.0F, 0.0F, swizzle_xyz);
    canvas.wire(albedo_sample, albedo_swizzle, "value");

    const NodeKey base_color = canvas.add("material.parameter");
    canvas.symbol(base_color, "base_color");
    canvas.type_of(base_color, ValueType::Vec3);
    canvas.value(base_color, "default", 0.82F, 0.78F, 0.74F, 0.0F, 0);

    const NodeKey albedo = canvas.add("material.multiply");
    canvas.wire(base_color, albedo, "a");
    canvas.wire(albedo_swizzle, albedo, "b");

    const NodeKey metallic = canvas.add("material.parameter");
    canvas.symbol(metallic, "metallic");
    canvas.type_of(metallic, ValueType::Float);
    canvas.value(metallic, "default", 1.0F, 0.0F, 0.0F, 0.0F, 0);

    const NodeKey one_minus = canvas.add("material.one_minus");
    canvas.wire(metallic, one_minus, "value");

    const NodeKey grime_sample = canvas.add("material.texture_sample");
    canvas.symbol(grime_sample, "grime_map");
    canvas.type_of(grime_sample, ValueType::Vec4);
    canvas.value(grime_sample, "average", 0.35F, 0.35F, 0.35F, 1.0F, 0);
    canvas.wire(uv, grime_sample, "uv");
    canvas.flag(grime_sample, "microdetail");

    const NodeKey grime = canvas.add("material.swizzle");
    canvas.type_of(grime, ValueType::Count);
    canvas.value(grime, "value", 0.0F, 0.0F, 0.0F, 0.0F, swizzle_x);
    canvas.wire(grime_sample, grime, "value");

    const NodeKey worn = canvas.add("material.multiply");
    canvas.wire(albedo, worn, "a");
    canvas.wire(one_minus, worn, "b");

    const NodeKey worn_scaled = canvas.add("material.multiply");
    canvas.wire(worn, worn_scaled, "a");
    canvas.wire(grime, worn_scaled, "b");

    const NodeKey tint = canvas.add("material.constant");
    canvas.type_of(tint, ValueType::Float);
    canvas.value(tint, "value", 1.0F, 0.0F, 0.0F, 0.0F, 0);

    const NodeKey tinted = canvas.add("material.multiply");
    canvas.wire(worn_scaled, tinted, "a");
    canvas.wire(tint, tinted, "b");

    const NodeKey diffuse = canvas.add("material.diffuse");
    canvas.wire(tinted, diffuse, "colour");

    const NodeKey second_sample = canvas.add("material.texture_sample");
    canvas.symbol(second_sample, "base_color_map");
    canvas.type_of(second_sample, ValueType::Vec4);
    canvas.value(second_sample, "average", 0.5F, 0.5F, 0.5F, 1.0F, 0);
    canvas.wire(uv, second_sample, "uv");

    const NodeKey second_swizzle = canvas.add("material.swizzle");
    canvas.type_of(second_swizzle, ValueType::Count);
    canvas.value(second_swizzle, "value", 0.0F, 0.0F, 0.0F, 0.0F, swizzle_xyz);
    canvas.wire(second_sample, second_swizzle, "value");

    const NodeKey second_albedo = canvas.add("material.multiply");
    canvas.wire(base_color, second_albedo, "a");
    canvas.wire(second_swizzle, second_albedo, "b");

    const NodeKey roughness = canvas.add("material.parameter");
    canvas.symbol(roughness, "roughness");
    canvas.type_of(roughness, ValueType::Float);
    canvas.value(roughness, "default", 0.35F, 0.0F, 0.0F, 0.0F, 0);

    const NodeKey specular = canvas.add("material.specular");
    canvas.wire(second_albedo, specular, "colour");
    canvas.wire(roughness, specular, "roughness");

    const NodeKey sum = canvas.add("material.add_closures");
    canvas.wire(diffuse, sum, "a");
    canvas.wire(specular, sum, "b");

    const NodeKey emissive = canvas.add("material.parameter");
    canvas.symbol(emissive, "emissive");
    canvas.type_of(emissive, ValueType::Vec3);
    canvas.value(emissive, "default", 1.0F, 0.4F, 0.1F, 0.0F, 0);

    const NodeKey emission = canvas.add("material.emission");
    canvas.wire(emissive, emission, "colour");
    canvas.mute(emission);

    const NodeKey outer_sum = canvas.add("material.add_closures");
    canvas.wire(sum, outer_sum, "a");
    canvas.wire(emission, outer_sum, "b");

    const NodeKey opacity = canvas.add("material.constant");
    canvas.type_of(opacity, ValueType::Float);
    canvas.value(opacity, "value", 1.0F, 0.0F, 0.0F, 0.0F, 0);

    const NodeKey orphan_constant = canvas.add("material.constant");
    canvas.type_of(orphan_constant, ValueType::Float);
    canvas.value(orphan_constant, "value", 2.0F, 0.0F, 0.0F, 0.0F, 0);
    const NodeKey orphan = canvas.add("material.multiply");
    canvas.wire(roughness, orphan, "a");
    canvas.wire(orphan_constant, orphan, "b");

    const NodeKey output = canvas.add("material.output");
    canvas.wire(outer_sum, output, "surface");
    canvas.wire(opacity, output, "opacity");
    return canvas.good();
}

}  // namespace

CY_TEST_CASE("graph_material: the palette is the engine's own vocabulary, op for op") {
    // THE HALF THE CONTRACT GATE CANNOT CHECK. `play_contract.py` reads the string literals out of
    // this module and compares them with the editor's; it cannot know whether they are the
    // compiler's own ops. This does: every `GraphOp` must appear as `"material." +
    // graph_op_name(op)`, so an op added to the compiler and not to the palette is red here, in the
    // engine's own suite.
    const auto types = material_node_types();
    std::vector<std::string> offered;
    for (const auto& type : types) {
        offered.emplace_back(type);
    }
    CY_CHECK(offered.size() == static_cast<usize>(GraphOp::Count) + 1U);
    for (u32 index = 0; index < static_cast<u32>(GraphOp::Count); ++index) {
        const std::string expected =
            std::string("material.") +
            cy::rendering::material::graph_op_name(static_cast<GraphOp>(index));
        CY_CHECK(std::ranges::find(offered, expected) != offered.end());
    }
    CY_CHECK(std::ranges::find(offered, std::string("material.output")) != offered.end());
}

CY_TEST_CASE(
    "graph_material: an authored canvas lowers to the compiler's own graph, digest for digest") {
    Canvas canvas("worn_metal");
    CY_REQUIRE(author_reference(canvas));

    MaterialGraph lowered(allocator(), Name::intern("worn_metal"));
    CY_REQUIRE(lower_material(canvas.graph(), lowered));

    MaterialGraph reference(allocator(), Name::intern("worn_metal"));
    cy::rendering::material::testing::GraphIds ids;
    CY_REQUIRE(cy::rendering::material::testing::build_reference_graph(reference, ids));

    // Same node count and same declaration order first, because a digest that matched with a
    // different node count would mean the optimiser removed the difference and the front ends still
    // disagree about what an author wrote.
    CY_CHECK(lowered.nodes().size() == reference.nodes().size());
    CY_CHECK(lowered.parameters().size() == reference.parameters().size());
    CY_CHECK(lowered.textures().size() == reference.textures().size());
    for (usize index = 0; index < lowered.parameters().size(); ++index) {
        CY_CHECK(lowered.parameters()[index].name == reference.parameters()[index].name);
    }
    for (usize index = 0; index < lowered.textures().size(); ++index) {
        CY_CHECK(lowered.textures()[index].name == reference.textures()[index].name);
    }

    auto from_canvas = cy::rendering::material::lower_graph(lowered, allocator());
    auto from_cpp = cy::rendering::material::lower_graph(reference, allocator());
    CY_REQUIRE(from_canvas.has_value());
    CY_REQUIRE(from_cpp.has_value());
    CY_CHECK(from_canvas.value().digest() == from_cpp.value().digest());
}

CY_TEST_CASE("graph_material: the canvas, the compiler's graph and the text are one material") {
    Canvas canvas("worn_metal");
    CY_REQUIRE(author_reference(canvas));
    MaterialGraph lowered(allocator(), Name::intern("worn_metal"));
    CY_REQUIRE(lower_material(canvas.graph(), lowered));
    auto from_canvas = cy::rendering::material::lower_graph(lowered, allocator());
    CY_REQUIRE(from_canvas.has_value());

    cy::rendering::material::ParseDiagnostic diagnostic(allocator());
    auto from_text = cy::rendering::material::parse_material(
        cy::rendering::material::testing::reference_text(), allocator(), diagnostic);
    CY_REQUIRE(from_text.has_value());

    CompileOptions options;
    auto canvas_material =
        cy::rendering::material::compile_material(from_canvas.value(), options, allocator());
    auto text_material =
        cy::rendering::material::compile_material(from_text.value(), options, allocator());
    CY_REQUIRE(canvas_material.has_value());
    CY_REQUIRE(text_material.has_value());
    // THE REQUIREMENT, RUN. Two authoring paths, one cook key — which is what a cache hit is made
    // of, so this is also the statement that a material authored in the editor and the same
    // material written by hand do not cook twice.
    CY_CHECK(canvas_material.value().cook_key() == text_material.value().cook_key());
}

CY_TEST_CASE("graph_material: a node type this build cannot lower is refused by name") {
    Canvas canvas("broken");
    const NodeKey node = canvas.add("material.nonesuch");
    (void)node;
    CY_REQUIRE(canvas.good());
    MaterialGraph lowered(allocator(), Name::intern("broken"));
    // A silent fallback here would put a value into the IR the author never wrote, and the material
    // would still compile. The refusal is what makes a stale palette visible.
    CY_CHECK(!lower_material(canvas.graph(), lowered));
}

CY_TEST_CASE("graph_material: the pin an author wires is the port the compiler reads") {
    // The one join that a rename cannot survive and a comment cannot satisfy: for each palette
    // entry, wire a constant into every input pin in turn and require it to land on the port index
    // `MaterialGraph::input` reports — which is the number `graph.h` fixes the meaning of.
    for (const auto& type : material_node_types()) {
        if (type == "material.output") {
            continue;
        }
        cy::graph::PinDesc storage[cy::graph::material::kMaxPins];
        const auto pins = cy::graph::material::material_node_pins(type, storage);
        u8 port = 0;
        for (const auto& pin : pins) {
            if (pin.direction != cy::graph::PinDirection::Input) {
                continue;
            }
            Canvas canvas("ports");
            const NodeKey source = canvas.add("material.constant");
            canvas.type_of(source, ValueType::Float);
            canvas.value(source, "value", 0.5F, 0.0F, 0.0F, 0.0F, 0);
            const NodeKey target = canvas.add(std::string(type).c_str());
            canvas.wire(source, target, std::string(pin.name.text()).c_str());
            CY_REQUIRE(canvas.good());

            MaterialGraph lowered(allocator(), Name::intern("ports"));
            CY_REQUIRE(lower_material(canvas.graph(), lowered));
            CY_CHECK(lowered.input(1, port) == 0);
            ++port;
        }
    }
}

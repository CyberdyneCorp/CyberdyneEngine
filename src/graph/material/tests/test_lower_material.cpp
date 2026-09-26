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
// `lower_material` and required to produce the SAME IR DIGEST as M7's hand-built `MaterialGraph`.
// A lowering that dropped the muted node, tidied the orphan away, or numbered one port differently
// would change the digest and fail.
//
// THE COOK-KEY HALF OF THAT CLAIM IS NOT HERE. It compiles the same material twice — once from the
// canvas and once from the text — and `tests/harness/src/budget.cpp` measured it at 0.806 ms of CPU
// against the unit tier's 1.000 ms budget, where no other case in this file costs more than
// 0.107 ms. The instrument's own message names the remedy for a case that expensive ("the taxonomy
// in `testing-and-quality` places a test this expensive in the next suite up — move it, or make it
// cheaper"), so it moved: see test_material_front_ends.cpp, declared as
// `integration.graph_material`. That is the same reason src/graph/tests/ and src/vfx/tests/ put
// repeated compilation at the integration tier rather than this one, and it is a taxonomy decision
// rather than a budget raised until a case fitted under it.

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/material/lower_material.h>
#include <cy/rendering/material/compiler.h>
#include <cy/test/test.h>

#include <algorithm>
#include <string>
#include <vector>

#include "fixtures.h"
#include "reference_canvas.h"

using cy::Name;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;
using cy::graph::NodeKey;
using cy::graph::material::lower_material;
using cy::graph::material::material_node_pin_id;
using cy::graph::material::material_node_type_id;
using cy::graph::material::material_node_types;
using cy::graph_material_test::allocator;
using cy::graph_material_test::author_reference;
using cy::graph_material_test::Canvas;
using cy::rendering::material::GraphOp;
using cy::rendering::material::MaterialGraph;
using cy::rendering::material::ValueType;

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
    CY_CHECK(offered.size() == static_cast<usize>(GraphOp::Count) + 2U);
    for (u32 index = 0; index < static_cast<u32>(GraphOp::Count); ++index) {
        const std::string expected =
            std::string("material.") +
            cy::rendering::material::graph_op_name(static_cast<GraphOp>(index));
        CY_CHECK(std::ranges::find(offered, expected) != offered.end());
    }
    CY_CHECK(std::ranges::find(offered, std::string("material.output")) != offered.end());
    CY_CHECK(std::ranges::find(offered, std::string("material.vertex_output")) != offered.end());
}

CY_TEST_CASE("graph_material: every catalogue node and pin has a stable nonzero identity") {
    std::vector<cy::graph::NodeTypeId> identities;
    for (const auto& type : material_node_types()) {
        const auto identity = material_node_type_id(type);
        CY_REQUIRE(identity != cy::graph::kInvalidNodeTypeId);
        CY_CHECK(std::ranges::find(identities, identity) == identities.end());
        identities.push_back(identity);

        cy::graph::PinDesc storage[cy::graph::material::kMaxPins];
        const auto pins = cy::graph::material::material_node_pins(type, storage);
        std::vector<cy::graph::PinId> pin_ids;
        for (const auto& pin : pins) {
            CY_REQUIRE(pin.identity != cy::graph::kInvalidPinId);
            CY_CHECK_EQ(pin.identity, material_node_pin_id(type, pin.name.text(), pin.direction));
            CY_CHECK(std::ranges::find(pin_ids, pin.identity) == pin_ids.end());
            pin_ids.push_back(pin.identity);
        }
    }
    CY_CHECK_EQ(identities.size(), material_node_types().size());
}

CY_TEST_CASE("graph_material: stage compatibility comes from the engine palette") {
    using cy::graph::material::material_node_stage_mask;
    CY_CHECK_EQ(material_node_stage_mask("material.output"), 1U);
    CY_CHECK_EQ(material_node_stage_mask("material.vertex_output"), 2U);
    CY_CHECK_EQ(material_node_stage_mask("material.diffuse"), 1U);
    CY_CHECK_EQ(material_node_stage_mask("material.texture_sample"), 1U);
    CY_CHECK_EQ(material_node_stage_mask("material.custom"), 1U);
    CY_CHECK_EQ(material_node_stage_mask("material.sin"), 3U);
    CY_CHECK_EQ(material_node_stage_mask("material.attribute"), 3U);
    CY_CHECK_EQ(material_node_stage_mask("material.unknown"), 0U);
}

CY_TEST_CASE("graph_material: the service catalogue is deterministic and versioned") {
    cy::Array<u8> first(allocator());
    cy::Array<u8> second(allocator());
    CY_REQUIRE(cy::graph::material::encode_material_catalogue(first));
    CY_REQUIRE(cy::graph::material::encode_material_catalogue(second));
    CY_REQUIRE_EQ(first.size(), second.size());
    CY_REQUIRE(std::equal(first.begin(), first.end(), second.begin()));
    CY_REQUIRE(first.size() >= 12U);
    const auto read_u32 = [&](usize offset) {
        return static_cast<u32>(first[offset]) | (static_cast<u32>(first[offset + 1]) << 8U) |
               (static_cast<u32>(first[offset + 2]) << 16U) |
               (static_cast<u32>(first[offset + 3]) << 24U);
    };
    CY_CHECK_EQ(read_u32(0), 3U);
    CY_CHECK_EQ(read_u32(4), 6U);
    CY_CHECK_EQ(read_u32(8), material_node_types().size());
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
        if (type == "material.output" || type == "material.vertex_output") {
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

CY_TEST_CASE("graph_material: authored vector sine reaches the material IR") {
    Canvas canvas("sway");
    const NodeKey phase = canvas.add("material.parameter");
    canvas.symbol(phase, "phase");
    canvas.type_of(phase, ValueType::Vec3);
    canvas.value(phase, "default", 0.5F, 0.25F, 0.0F, 0.0F, 0);
    const NodeKey sine = canvas.add("material.sin");
    canvas.wire(phase, sine, "value");
    const NodeKey emission = canvas.add("material.emission");
    canvas.wire(sine, emission, "colour");
    const NodeKey output = canvas.add("material.output");
    canvas.wire(emission, output, "surface");
    CY_REQUIRE(canvas.good());

    MaterialGraph lowered(allocator(), Name::intern("sway"));
    CY_REQUIRE(lower_material(canvas.graph(), lowered));
    auto ir = cy::rendering::material::lower_graph(lowered, allocator());
    CY_REQUIRE(ir.has_value());
    bool found_sine = false;
    for (cy::rendering::material::NodeId id = 0; id < ir.value().size(); ++id) {
        const auto& node = ir.value().node(id);
        found_sine = found_sine ||
                     (node.op == cy::rendering::material::Op::Sin && node.type == ValueType::Vec3);
    }
    CY_CHECK(found_sine);
}

CY_TEST_CASE("graph_material: the vertex output reaches the same typed IR root") {
    Canvas canvas("wind_sway");
    const NodeKey offset = canvas.add("material.constant");
    canvas.type_of(offset, ValueType::Vec3);
    canvas.value(offset, "value", 0.0F, 0.25F, 0.0F, 0.0F, 0);
    const NodeKey output = canvas.add("material.vertex_output");
    canvas.wire(offset, output, "offset");
    CY_REQUIRE(canvas.good());

    MaterialGraph lowered(allocator(), Name::intern("wind_sway"));
    CY_REQUIRE(lower_material(canvas.graph(), lowered));
    auto ir = cy::rendering::material::lower_graph(lowered, allocator());
    CY_REQUIRE(ir.has_value());
    CY_CHECK_NE(ir.value().vertex_offset(), cy::rendering::material::kInvalidNode);
    CY_CHECK_EQ(ir.value().node(ir.value().vertex_offset()).type, ValueType::Vec3);

    Canvas invalid("bad_sway");
    const NodeKey scalar = invalid.add("material.constant");
    invalid.type_of(scalar, ValueType::Float);
    invalid.value(scalar, "value", 0.25F, 0.0F, 0.0F, 0.0F, 0);
    const NodeKey invalid_output = invalid.add("material.vertex_output");
    invalid.wire(scalar, invalid_output, "offset");
    CY_REQUIRE(invalid.good());
    MaterialGraph rejected(allocator(), Name::intern("bad_sway"));
    CY_REQUIRE(lower_material(invalid.graph(), rejected));
    CY_CHECK_FALSE(cy::rendering::material::lower_graph(rejected, allocator()).has_value());
}

CY_TEST_CASE("graph_material: named geometry nodes lower to fixed typed attributes") {
    Canvas canvas("geometry_inputs");
    const NodeKey position = canvas.add("material.object_position");
    const NodeKey world = canvas.add("material.world_position");
    (void)canvas.add("material.normal");
    (void)canvas.add("material.uv0");
    (void)canvas.add("material.time");
    const NodeKey noise = canvas.add("material.noise");
    canvas.wire(world, noise, "position");
    const NodeKey output = canvas.add("material.vertex_output");
    canvas.wire(position, output, "offset");
    CY_REQUIRE(canvas.good());

    MaterialGraph lowered(allocator(), Name::intern("geometry_inputs"));
    CY_REQUIRE(lower_material(canvas.graph(), lowered));
    auto ir = cy::rendering::material::lower_graph(lowered, allocator());
    CY_REQUIRE(ir.has_value());
    const auto root = ir.value().vertex_offset();
    CY_REQUIRE_NE(root, cy::rendering::material::kInvalidNode);
    CY_CHECK_EQ(ir.value().node(root).op, cy::rendering::material::Op::Attribute);
    CY_CHECK_EQ(ir.value().node(root).symbol, Name::intern("object_position"));
    CY_CHECK_EQ(ir.value().node(root).type, ValueType::Vec3);
    bool normal = false;
    bool uv0 = false;
    bool world_position = false;
    bool time = false;
    bool coherent_noise = false;
    for (cy::rendering::material::NodeId id = 0; id < ir.value().size(); ++id) {
        const auto& node = ir.value().node(id);
        world_position = world_position ||
                         (node.symbol == Name::intern("position") && node.type == ValueType::Vec3);
        normal = normal || (node.symbol == Name::intern("normal") && node.type == ValueType::Vec3);
        uv0 = uv0 || (node.symbol == Name::intern("uv0") && node.type == ValueType::Vec2);
        time =
            time || (node.symbol == Name::intern("time_seconds") && node.type == ValueType::Float);
        coherent_noise = coherent_noise ||
                         (node.op == cy::rendering::material::Op::Noise &&
                          node.type == ValueType::Float && ir.value().operands(id).size() == 1);
    }
    CY_CHECK(world_position);
    CY_CHECK(normal);
    CY_CHECK(uv0);
    CY_CHECK(time);
    CY_CHECK(coherent_noise);
}

CY_TEST_CASE("graph_material: spatial noise refuses a scalar coordinate") {
    Canvas canvas("invalid_noise");
    const NodeKey scalar = canvas.add("material.constant");
    canvas.type_of(scalar, ValueType::Float);
    const NodeKey noise = canvas.add("material.noise");
    canvas.wire(scalar, noise, "position");
    CY_REQUIRE(canvas.good());

    MaterialGraph lowered(allocator(), Name::intern("invalid_noise"));
    CY_REQUIRE(lower_material(canvas.graph(), lowered));
    CY_CHECK_FALSE(cy::rendering::material::lower_graph(lowered, allocator()).has_value());
}

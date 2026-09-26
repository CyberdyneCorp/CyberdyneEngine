// The cook key the two front ends share. M11.c task 6.1a.
//
// ONE CASE, AND IT IS HERE RATHER THAN IN `unit.graph_material` BECAUSE OF WHAT IT COSTS. It runs
// `compile_material` TWICE — once over the graph lowered from the authored canvas and once over the
// graph parsed from the text definition — and `tests/harness/src/budget.cpp` measured it at 0.806
// ms of CPU against the unit tier's 1.000 ms budget. That is 81% of the tier on one case, and it is
// what made `unit.graph_material` fail the budget twice inside M11.c's closing ledger while passing
// every standalone run: a host whose governor idles at 800 MHz is this instrument's worst case, and
// the instrument says so in its own failure text.
//
// THE REMEDY IS THE ONE THE INSTRUMENT NAMES, and it is the taxonomy's rather than the budget's:
// "the taxonomy in `testing-and-quality` places a test this expensive in the next suite up — move
// it, or make it cheaper". Repeated compilation to a fixed point is exactly the shape
// src/graph/tests/ already records as not fitting the unit tier, and src/vfx/tests/ declares
// `vfx_compiler` at integration for the same reason in as many words. Nothing about the assertion
// changed in the move: it is the same two front ends, the same material and the same cook key.
//
// THE FIXTURE IS INCLUDED AND NOT COPIED — reference_canvas.h, for the reason that header gives.

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/material/lower_material.h>
#include <cy/rendering/material/compiler.h>
#include <cy/rendering/material/text.h>
#include <cy/test/test.h>

#include "fixtures.h"
#include "reference_canvas.h"

using cy::Name;
using cy::graph::material::lower_material;
using cy::graph_material_test::allocator;
using cy::graph_material_test::author_reference;
using cy::graph_material_test::Canvas;
using cy::rendering::material::CompileOptions;
using cy::rendering::material::MaterialGraph;
using cy::rendering::material::ValueType;

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

CY_TEST_CASE("graph_material: spatial vertex noise has one graph and text cook identity") {
    Canvas canvas("noisy");
    const auto position = canvas.add("material.world_position");
    const auto noise = canvas.add("material.noise");
    canvas.wire(position, noise, "position");
    const auto scale = canvas.add("material.constant");
    canvas.type_of(scale, ValueType::Vec3);
    canvas.value(scale, "value", 0.0F, 0.1F, 0.0F, 0.0F, 0);
    const auto product = canvas.add("material.multiply");
    canvas.wire(noise, product, "a");
    canvas.wire(scale, product, "b");
    const auto output = canvas.add("material.vertex_output");
    canvas.wire(product, output, "offset");
    CY_REQUIRE(canvas.good());

    MaterialGraph lowered(allocator(), Name::intern("noisy"));
    CY_REQUIRE(lower_material(canvas.graph(), lowered));
    auto from_canvas = cy::rendering::material::lower_graph(lowered, allocator());
    CY_REQUIRE(from_canvas.has_value());

    cy::rendering::material::ParseDiagnostic diagnostic(allocator());
    auto from_text = cy::rendering::material::parse_material(
        "material noisy { attribute position : float3; "
        "vertex_offset = noise(position) * (0.0, 0.1, 0.0); }",
        allocator(), diagnostic);
    CY_REQUIRE(from_text.has_value());

    CompileOptions options;
    auto graph_program =
        cy::rendering::material::compile_material(*from_canvas, options, allocator());
    auto text_program = cy::rendering::material::compile_material(*from_text, options, allocator());
    CY_REQUIRE(graph_program.has_value());
    CY_REQUIRE(text_program.has_value());
    CY_CHECK_EQ(graph_program->cook_key(), text_program->cook_key());
}

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

CY_TEST_CASE("graph_material: normal displacement reaches visible and shadow vertex programs") {
    Canvas canvas("raised");
    const auto amount = canvas.add("material.constant");
    canvas.type_of(amount, ValueType::Float);
    canvas.value(amount, "value", 0.25F, 0.0F, 0.0F, 0.0F, 0);
    const auto offset = canvas.add("material.constant");
    canvas.type_of(offset, ValueType::Vec3);
    canvas.value(offset, "value", 0.0F, 0.1F, 0.0F, 0.0F, 0);
    const auto output = canvas.add("material.vertex_output");
    canvas.wire(amount, output, "displacement");
    canvas.wire(offset, output, "offset");
    CY_REQUIRE(canvas.good());

    MaterialGraph lowered(allocator(), Name::intern("raised"));
    CY_REQUIRE(lower_material(canvas.graph(), lowered));
    auto ir = cy::rendering::material::lower_graph(lowered, allocator());
    CY_REQUIRE(ir.has_value());
    CompileOptions options;
    auto compiled = cy::rendering::material::compile_material(*ir, options, allocator());
    CY_REQUIRE(compiled.has_value());
    cy::rendering::material::ParseDiagnostic diagnostic(allocator());
    auto from_text = cy::rendering::material::parse_material(
        "material raised { vertex_displacement = 0.25; "
        "vertex_offset = (0.0, 0.1, 0.0); }",
        allocator(), diagnostic);
    CY_REQUIRE(from_text.has_value());
    auto text_program = cy::rendering::material::compile_material(*from_text, options, allocator());
    CY_REQUIRE(text_program.has_value());
    CY_CHECK_EQ(compiled->cook_key(), text_program->cook_key());
    const cy::rendering::material::CompiledProgram* visible = nullptr;
    const cy::rendering::material::CompiledProgram* shadow = nullptr;
    for (const auto& program : compiled->programs()) {
        if (program.tier != cy::rendering::material::QualityTier::High) {
            continue;
        }
        if (program.kind == cy::rendering::material::ProgramKind::Primary) {
            visible = &program;
        } else if (program.kind == cy::rendering::material::ProgramKind::Shadow) {
            shadow = &program;
        }
    }
    CY_REQUIRE(visible != nullptr);
    CY_REQUIRE(shadow != nullptr);
    const std::string_view visible_source(visible->vertex_source.text.data(),
                                          visible->vertex_source.text.size());
    const std::string_view shadow_source(shadow->vertex_source.text.data(),
                                         shadow->vertex_source.text.size());
    CY_CHECK(visible_source.find("ctx.attributes.normal") != std::string_view::npos);
    CY_CHECK(shadow_source.find("ctx.attributes.normal") != std::string_view::npos);
    CY_CHECK(visible_source.find("0.25") != std::string_view::npos);
    CY_CHECK(shadow_source.find("0.25") != std::string_view::npos);

    auto invalid = cy::rendering::material::parse_material(
        "material raised { vertex_displacement = (0.0, 0.25, 0.0); }", allocator(), diagnostic);
    CY_CHECK_FALSE(invalid.has_value());
    CY_CHECK(std::string_view(diagnostic.text()).find("scalar distance") != std::string_view::npos);
}

CY_TEST_CASE("graph_material: animated procedural wind has one graph and text cook identity") {
    Canvas canvas("wind");
    const auto position = canvas.add("material.world_position");
    const auto time = canvas.add("material.time");
    const auto wind = canvas.add("material.procedural_wind");
    canvas.wire(position, wind, "position");
    canvas.wire(time, wind, "time");
    const auto output = canvas.add("material.vertex_output");
    canvas.wire(wind, output, "offset");
    CY_REQUIRE(canvas.good());

    MaterialGraph lowered(allocator(), Name::intern("wind"));
    CY_REQUIRE(lower_material(canvas.graph(), lowered));
    auto from_canvas = cy::rendering::material::lower_graph(lowered, allocator());
    CY_REQUIRE(from_canvas.has_value());

    cy::rendering::material::ParseDiagnostic diagnostic(allocator());
    auto from_text = cy::rendering::material::parse_material(
        "material wind { attribute position : float3; attribute time_seconds : float; "
        "vertex_offset = procedural_wind(position, time_seconds); }",
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

// Node previews use the real compiler. M7 task 6.4.
//
// `material-compiler` — "Node previews use the real compiler": "Every graph node SHALL be
// previewable, and previews SHALL be generated through the same compiler, lowering, and shader
// pipeline as runtime. There SHALL be no separate editor-only shading path. WHEN a node preview
// shows a surface THEN it SHALL be produced by the same generated program the renderer would use."
//
// ================================================================================================
// WHAT "THE SAME PROGRAM" IS CHECKED TO MEAN, AND WHY IT IS CHECKED THIS WAY
// ================================================================================================
//
// The claim a screenshot comparison would make is that two images look alike, which is the weakest
// available form of it and the one `editor-viewport-and-gizmos` refuses for the viewport for the
// same reason. What is checked here is stronger and needs no device:
//
//   1. The preview of the SURFACE ROOT is the primary program's body with the opacity assignment
//      removed — a prefix, byte for byte. Not "equivalent": identical text.
//   2. The preview of any INTERIOR node computes each of its values with the same statement the
//      final program computes it with, once the SSA numbers are read back to the IR nodes they
//      name. `GeneratedSource::value_nodes` is what makes that comparison possible, and it exists
//      for this and for the editor's "which node is this line?".
//
// Both hold because there is one emitter and a preview is that emitter with a root. If somebody
// adds a second path, case 1 fails on the first character that differs.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/material/compiler.h>
#include <cy/test/test.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fixtures.h"

using namespace cy;
using namespace cy::rendering::material;
using cy::rendering::material::testing::build_reference_graph;
using cy::rendering::material::testing::GraphIds;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// One statement, with every `vN` replaced by the IR node it names.
///
/// Without this a preview and a final program could never be compared: the same value is `v2` in
/// one and `v5` in the other, because a preview numbers only what it needs.
[[nodiscard]] std::string normalised(std::string_view line, const GeneratedSource& source) {
    std::string out;
    for (usize index = 0; index < line.size();) {
        const bool starts =
            line[index] == 'v' && index + 1 < line.size() && line[index + 1] >= '0' &&
            line[index + 1] <= '9' &&
            (index == 0 || !(std::isalnum(static_cast<unsigned char>(line[index - 1])) != 0));
        if (!starts) {
            out.push_back(line[index]);
            ++index;
            continue;
        }
        usize digits = index + 1;
        u32 value = 0;
        while (digits < line.size() && line[digits] >= '0' && line[digits] <= '9') {
            value = (value * 10U) + static_cast<u32>(line[digits] - '0');
            ++digits;
        }
        out += "n";
        out += std::to_string(value < source.value_nodes.size() ? source.value_nodes[value]
                                                                : kInvalidNode);
        index = digits;
    }
    return out;
}

[[nodiscard]] std::vector<std::string> statements(const GeneratedSource& source) {
    std::vector<std::string> lines;
    const std::string_view body = source.body();
    usize begin = 0;
    while (begin < body.size()) {
        const usize end = body.find('\n', begin);
        const std::string_view line =
            body.substr(begin, end == std::string_view::npos ? body.size() - begin : end - begin);
        // The root assignments are not statements: a preview writes `surface.preview` where the
        // final program writes `surface.closures` and `surface.opacity`, and comparing those would
        // be comparing what a preview is FOR rather than how it was generated.
        if (line.find(" = ") != std::string_view::npos &&
            line.find("    surface.") == std::string_view::npos) {
            lines.push_back(normalised(line, source));
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return lines;
}

struct Fixture {
    explicit Fixture(Allocator& memory) noexcept : compiled(memory) {}

    CompiledMaterial compiled;
    GraphIds ids;

    [[nodiscard]] bool build() noexcept {
        MaterialGraph graph(allocator(), Name::intern("worn_metal"));
        if (!build_reference_graph(graph, ids)) {
            return false;
        }
        auto authored = lower_graph(graph, allocator());
        if (!authored) {
            return false;
        }
        CompileOptions options;
        options.derive_family = false;
        options.derive_tiers = false;
        auto material = compile_material(authored.value(), options, allocator());
        if (!material) {
            return false;
        }
        compiled = std::move(material.value());
        return true;
    }
};

}  // namespace

CY_TEST_CASE("material_preview: the surface root's preview is the final program's body") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build());
    const CompiledProgram* primary = fixture.compiled.find(ProgramKind::Primary, QualityTier::High);
    CY_REQUIRE(primary != nullptr);

    auto preview = preview_node(*primary, primary->module.surface());
    CY_REQUIRE(preview.has_value());

    const std::string_view final_body = primary->source.body();
    const std::string_view preview_body = preview.value().body();
    CY_REQUIRE_FALSE(preview_body.empty());
    // A prefix, byte for byte. The only difference is the opacity assignment, which a preview of
    // the surface root does not compute.
    CY_CHECK(final_body.substr(0, preview_body.size()) == preview_body);
    CY_CHECK(final_body.substr(preview_body.size()) == "    surface.opacity = 1.0;\n");
    CY_CHECK_EQ(preview.value().statements, primary->source.statements);
}

CY_TEST_CASE("material_preview: an interior node's preview is the final program's own statements") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build());
    const CompiledProgram* primary = fixture.compiled.find(ProgramKind::Primary, QualityTier::High);
    CY_REQUIRE(primary != nullptr);
    const std::vector<std::string> final_statements = statements(primary->source);
    CY_REQUIRE_FALSE(final_statements.empty());

    // Every value in the compiled material, previewed. Not one node: a preview path that is right
    // for the root and wrong for an interior node is exactly the defect this case exists to catch.
    u32 previewed = 0;
    for (NodeId id = 0; id < primary->module.size(); ++id) {
        if (primary->module.node(id).type == ValueType::Closure) {
            continue;
        }
        auto preview = preview_node(*primary, id);
        CY_REQUIRE(preview.has_value());
        ++previewed;
        for (const std::string& line : statements(preview.value())) {
            bool found = false;
            for (const std::string& candidate : final_statements) {
                found = found || candidate == line;
            }
            CY_CHECK(found);
        }
    }
    CY_CHECK_GT(previewed, 4U);
}

CY_TEST_CASE("material_preview: a preview reaches a graph node through its provenance") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build());
    const CompiledProgram* primary = fixture.compiled.find(ProgramKind::Primary, QualityTier::High);
    CY_REQUIRE(primary != nullptr);

    // The editor holds a graph node id; the compiler holds IR values. The two graph nodes that
    // sampled one texture reach the ONE value they were merged into, which is what an editor
    // previewing either of them should show.
    const NodeId first = value_of_origin(primary->module, fixture.ids.albedo_sample);
    const NodeId second = value_of_origin(primary->module, fixture.ids.second_sample);
    CY_CHECK_NE(first, kInvalidNode);
    CY_CHECK_EQ(first, second);

    auto preview = preview_node(*primary, first);
    CY_REQUIRE(preview.has_value());
    CY_CHECK_EQ(preview.value().texture_samples, 1U);

    // A node the optimiser removed has no value to preview, and says so rather than inventing one.
    CY_CHECK_EQ(value_of_origin(primary->module, fixture.ids.orphan), kInvalidNode);
    CY_CHECK_FALSE(preview_node(*primary, primary->module.size()).has_value());
}

CY_TEST_CASE("material_preview: a preview of a derived program is that program's own") {
    // A preview is per PROGRAM, not per material: previewing a node in the far-field program has to
    // show what the far field will actually do, which is sample nothing.
    MaterialGraph graph(allocator(), Name::intern("worn_metal"));
    GraphIds ids;
    CY_REQUIRE(build_reference_graph(graph, ids));
    auto authored = lower_graph(graph, allocator());
    CY_REQUIRE(authored.has_value());
    CompileOptions options;
    options.derive_tiers = false;
    auto compiled = compile_material(authored.value(), options, allocator());
    CY_REQUIRE(compiled.has_value());

    const CompiledProgram* far = compiled.value().find(ProgramKind::FarField, QualityTier::High);
    CY_REQUIRE(far != nullptr);
    auto preview = preview_node(*far, far->module.surface());
    CY_REQUIRE(preview.has_value());
    CY_CHECK_EQ(preview.value().texture_samples, 0U);
    CY_CHECK(preview.value().view().find("far_field") != std::string_view::npos);
}

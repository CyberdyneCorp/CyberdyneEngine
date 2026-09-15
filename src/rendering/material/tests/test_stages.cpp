// EVERY STAGE OF A MATERIAL'S LOWERING, AND WHICH OF ITS INPUTS ARE TEXTURES. M11.c tasks 1.2, 1.3.
//
// Two requirements, one file, because they are the two halves of the same claim: that a compiled
// material can be READ. `shader-system` — "Visual material editor": "The editor SHALL be able to
// show, for any material, each stage of its lowering: the graph, the material IR before and after
// optimisation, the generated Slang, and the compiled backend output." `material-compiler` — "IR is
// inspectable" and "The compile report says which inputs are textures and which are constants".
//
// ================================================================================================
// WHAT EACH CASE WOULD CATCH, BECAUSE A CASE THAT NAMES NO DEFECT IS A CASE NOBODY CAN JUDGE
// ================================================================================================
//
//   * a stage list that silently drops the fifth stage and calls four "every stage" — the exact
//     shape of a panel that shows what it has and names it what it was asked for;
//   * two stages that are the same text, which is what a dump written once and shown twice looks
//     like: the IR before and after optimisation MUST differ for the reference material, because
//     the reference graph carries a muted node, a disconnected node and a duplicated sample and the
//     pipeline removes all three;
//   * a dump ordered by node id, which would make the graph front end's module and the text front
//     end's — one material, identical programs — produce different text;
//   * a report that says "textured" about the far-field program, which samples nothing by
//     derivation and is the single most likely way for a beauty shot's caption to be wrong.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/material/inputs.h>
#include <cy/rendering/material/stages.h>
#include <cy/rendering/material/text.h>
#include <cy/test/test.h>

#include <string_view>

#include "fixtures.h"

using namespace cy;
using namespace cy::rendering::material;
using cy::rendering::material::testing::build_reference_graph;
using cy::rendering::material::testing::GraphIds;
using cy::rendering::material::testing::reference_text;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// One program, not twelve. The family and the tiers are `integration.material_lowering`'s subject
/// and its cost — this suite's budget is a millisecond and the stage list is the same list whether
/// the family was derived or not.
[[nodiscard]] CompileOptions options() noexcept {
    CompileOptions compile;
    compile.derive_family = false;
    compile.derive_tiers = false;
    return compile;
}

/// The `digest 0x...` a module dump prints on its first line: the module's identity, which two
/// front ends producing one material must agree about.
[[nodiscard]] std::string_view digest_line(std::string_view dump) noexcept {
    const usize at = dump.find("digest 0x");
    if (at == std::string_view::npos) {
        return {};
    }
    return dump.substr(at, dump.find('\n', at) - at);
}

[[nodiscard]] u32 count(std::string_view haystack, std::string_view needle) noexcept {
    u32 found = 0;
    usize at = haystack.find(needle);
    while (at != std::string_view::npos) {
        ++found;
        at = haystack.find(needle, at + 1);
    }
    return found;
}

}  // namespace

CY_TEST_CASE("every lowering stage is inspectable, and the fifth names who owes it") {
    MaterialGraph graph(allocator(), Name::intern("worn_metal"));
    GraphIds ids;
    CY_REQUIRE(build_reference_graph(graph, ids));

    auto inspected =
        inspect_lowering(graph, options(), ProgramKind::Primary, QualityTier::High, allocator());
    CY_REQUIRE(inspected.has_value());
    const LoweringInspection& stages = inspected.value();

    // FIVE, IN THE SPECIFICATION'S ORDER. A list with four entries is the defect.
    CY_REQUIRE_EQ(stages.stages().size(), usize{5});
    CY_CHECK_EQ(std::string_view(lowering_stage_name(LoweringStage::Graph)), "graph");
    CY_CHECK_EQ(std::string_view(lowering_stage_name(LoweringStage::AuthoredIr)), "ir");
    CY_CHECK_EQ(std::string_view(lowering_stage_name(LoweringStage::OptimisedIr)), "optimised-ir");
    CY_CHECK_EQ(std::string_view(lowering_stage_name(LoweringStage::GeneratedSlang)), "slang");
    CY_CHECK_EQ(std::string_view(lowering_stage_name(LoweringStage::CompiledProgram)), "compiled");
    for (u32 index = 0; index < kLoweringStageCount; ++index) {
        CY_CHECK_EQ(static_cast<u32>(stages.stages()[index].stage), index);
    }

    // The four this module owns are produced and each has content.
    CY_CHECK_EQ(stages.available(), 4U);
    CY_CHECK_FALSE(stages.complete());
    for (const LoweringStage which : {LoweringStage::Graph, LoweringStage::AuthoredIr,
                                      LoweringStage::OptimisedIr, LoweringStage::GeneratedSlang}) {
        CY_CHECK(stages.stage(which).available);
        CY_CHECK_GT(stages.stage(which).text.size(), usize{32});
        CY_CHECK_NE(stages.stage(which).digest, 0ULL);
    }

    // AND THE FIFTH SAYS WHO OWES IT rather than coming back empty. An empty dump and an absent
    // stage read the same to a panel, which is how four stages get shown as five.
    const StageDump& backend = stages.stage(LoweringStage::CompiledProgram);
    CY_CHECK_FALSE(backend.available);
    CY_CHECK(backend.text.empty());
    CY_CHECK(std::string_view(backend.reason).find("shader-system") != std::string_view::npos);

    // NO TWO STAGES ARE THE SAME TEXT. The reference graph carries a muted closure, a disconnected
    // multiply and one texture sampled twice; the pipeline removes all three, so the IR before and
    // after optimisation cannot be the same dump unless one of them is a copy of the other.
    for (u32 a = 0; a + 1 < 4U; ++a) {
        for (u32 b = a + 1; b < 4U; ++b) {
            CY_CHECK_NE(stages.stages()[a].digest, stages.stages()[b].digest);
        }
    }

    // The authored IR HAS the dropped nodes and the optimised one does not, which is what makes
    // "the editor SHALL be able to show which nodes were dropped" answerable from these two dumps.
    const std::string_view authored = stages.stage(LoweringStage::AuthoredIr).view();
    const std::string_view optimised = stages.stage(LoweringStage::OptimisedIr).view();
    // THE AUTHORED DUMP HAS WORK IN IT THAT THE OPTIMISED ONE DOES NOT. The graph fixture carries
    // a muted emission closure and a disconnected multiply the author left behind; the pipeline
    // removes both. Counted off the dumps rather than asserted: an `emission` in the authored IR
    // and none in the optimised one, and strictly fewer values overall.
    CY_CHECK_GT(count(authored, " emission :"), count(optimised, " emission :"));
    CY_CHECK_EQ(count(optimised, " emission :"), 0U);
    CY_CHECK_GT(count(authored, "\n  %"), count(optimised, "\n  %"));
    CY_CHECK(authored.find("unreachable ") != std::string_view::npos);

    // The fourth stage is the emitter's own text and carries the emitter's own digest — not a
    // second hash of a copy, which would be a second identity for one artefact.
    CY_CHECK(stages.stage(LoweringStage::GeneratedSlang).view().find("cy_material_worn_metal") !=
             std::string_view::npos);
    CY_CHECK_NE(stages.cook_key, 0ULL);
}

CY_TEST_CASE("the two front ends' stages are the same three, and differ only in the first") {
    // ONE MATERIAL, TWO AUTHORING FORMS. `material-compiler`: "a graph and a text definition ...
    // SHALL produce identical IR and an identical compiled program". So stages two, three and four
    // must be IDENTICAL and stage one must NOT — a dump that compared equal for the graph and the
    // text would be a dump that is not showing the authored form at all.
    MaterialGraph graph(allocator(), Name::intern("worn_metal"));
    GraphIds ids;
    CY_REQUIRE(build_reference_graph(graph, ids));
    auto from_graph =
        inspect_lowering(graph, options(), ProgramKind::Primary, QualityTier::High, allocator());
    CY_REQUIRE(from_graph.has_value());

    ParseDiagnostic sink(allocator());
    auto parsed = parse_material(reference_text(), allocator(), sink);
    CY_REQUIRE(parsed.has_value());
    auto from_text = inspect_lowering(reference_text(), parsed.value(), options(),
                                      ProgramKind::Primary, QualityTier::High, allocator());
    CY_REQUIRE(from_text.has_value());

    CY_CHECK_NE(from_graph.value().stage(LoweringStage::Graph).digest,
                from_text.value().stage(LoweringStage::Graph).digest);

    // THE IR DUMPS ARE NOT COMPARED WHOLE, AND THE REASON IS THE POINT. A dump carries PROVENANCE —
    // `from 12 19`, the authoring nodes a value came from — and the two front ends have different
    // authoring nodes by construction: that is what provenance is for. What must agree is the
    // module's IDENTITY, which the dump prints on its first line and which is exactly what
    // `material-compiler`'s "identical IR" means.
    CY_CHECK_EQ(digest_line(from_graph.value().stage(LoweringStage::OptimisedIr).view()),
                digest_line(from_text.value().stage(LoweringStage::OptimisedIr).view()));
    CY_CHECK_NE(digest_line(from_graph.value().stage(LoweringStage::OptimisedIr).view()),
                std::string_view{});
    CY_CHECK_EQ(from_graph.value().stage(LoweringStage::GeneratedSlang).digest,
                from_text.value().stage(LoweringStage::GeneratedSlang).digest);
    CY_CHECK_EQ(from_graph.value().cook_key, from_text.value().cook_key);
}

CY_TEST_CASE("a material whose every input is a constant is visible as one") {
    // THE SHAPE EVERY PICTURE THIS PROJECT HAS PUBLISHED IS MADE OF, and the case that makes the
    // claim checkable rather than rhetorical. It renders, it shades, it photographs, and nothing in
    // the compiled artefact distinguished it from a textured material before this report existed.
    constexpr std::string_view kFlat = R"(
material flat_grey {
    surface = diffuse((0.5, 0.5, 0.5)) + specular((0.04, 0.04, 0.04), 0.5);
    opacity = 1.0;
}
)";
    ParseDiagnostic sink(allocator());
    auto parsed = parse_material(kFlat, allocator(), sink);
    CY_REQUIRE(parsed.has_value());
    CompileOptions compile;
    compile.derive_family = false;
    compile.derive_tiers = false;
    auto compiled = compile_material(parsed.value(), compile, allocator());
    CY_REQUIRE(compiled.has_value());

    const CompiledProgram* primary = compiled.value().find(ProgramKind::Primary, QualityTier::High);
    CY_REQUIRE(primary != nullptr);
    CY_CHECK(primary->inputs.constants_only());
    CY_CHECK_EQ(primary->inputs.textures, 0U);
    CY_CHECK_EQ(primary->inputs.constants, primary->inputs.inputs.size());
    CY_CHECK_EQ(std::string_view(input_binding_name(InputBinding::Constant)), "constant");
    CY_CHECK_EQ(std::string_view(input_binding_name(InputBinding::Texture)), "texture");
}

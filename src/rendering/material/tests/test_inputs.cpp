// WHICH OF A MATERIAL'S INPUTS ARE TEXTURES AND WHICH ARE CONSTANTS. M11.c task 1.2.
//
// `material-compiler` — "The compile report says which inputs are textures and which are
// constants": "The compile report SHALL state, per material, which inputs are bound to textures and
// which are constants, and the count of each SHALL be readable by a tool without opening the shader
// source."
//
// INTEGRATION AND NOT UNIT, for the reason the two cases beside it in `material_lowering` are: they
// compile the WHOLE PROGRAM FAMILY and compare programs with each other. The far-field program's
// answer is the point of the second case — it samples nothing by derivation — and there is no way
// to have it without deriving the family.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/material/compiler.h>
#include <cy/rendering/material/inputs.h>
#include <cy/test/test.h>

#include <string_view>

#include "fixtures.h"

using namespace cy;
using namespace cy::rendering::material;
using cy::rendering::material::testing::build_reference_graph;
using cy::rendering::material::testing::GraphIds;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

}  // namespace

CY_TEST_CASE("material_inputs: the compile report says which inputs are textures and which are constants") {
    MaterialGraph graph(allocator(), Name::intern("worn_metal"));
    GraphIds ids;
    CY_REQUIRE(build_reference_graph(graph, ids));
    auto authored = lower_graph(graph, allocator());
    CY_REQUIRE(authored.has_value());

    CompileOptions compile;
    compile.derive_tiers = false;
    auto compiled = compile_material(authored.value(), compile, allocator());
    CY_REQUIRE(compiled.has_value());

    const CompiledProgram* primary = compiled.value().find(ProgramKind::Primary, QualityTier::High);
    CY_REQUIRE(primary != nullptr);
    const InputReport& inputs = primary->inputs;

    // THE REFERENCE MATERIAL, READ OFF THE FIXTURE. `surface = diffuse(albedo * (1 - metallic) *
    // grime.x) + specular(albedo, roughness)` with `albedo = sample(base_color_map, uv0).xyz *
    // base_color`, and `opacity = 1.0`. So four inputs: diffuse.colour and specular.colour reach a
    // texture, specular.roughness reaches the `roughness` parameter and nothing else, and opacity
    // is the literal one — WHICH IS COUNTED. A report that silently dropped the constant would be a
    // report whose denominator moved with its numerator.
    CY_REQUIRE_EQ(inputs.inputs.size(), usize{4});
    CY_CHECK_FALSE(inputs.constants_only());
    CY_CHECK_EQ(inputs.textures, 2U);
    CY_CHECK_EQ(inputs.parameters, 1U);
    CY_CHECK_EQ(inputs.constants, 1U);
    CY_CHECK_EQ(inputs.varying, 0U);

    u32 named = 0;
    for (const MaterialInput& input : inputs.inputs) {
        const std::string_view name(input.name);
        if (name == "diffuse.colour" || name == "specular.colour") {
            CY_CHECK_EQ(input.binding, InputBinding::Texture);
            // WHICH MAP, which is the question an author asks next. `base_color_map` sorts before
            // `grime_map`, and the order is by name so two runs answer the same.
            CY_CHECK_EQ(input.texture, Name::intern("base_color_map"));
            ++named;
        }
        if (name == "specular.roughness") {
            CY_CHECK_EQ(input.binding, InputBinding::Parameter);
            CY_CHECK(input.texture.is_empty());
            ++named;
        }
        if (name == "opacity") {
            CY_CHECK_EQ(input.binding, InputBinding::Constant);
            ++named;
        }
        CY_CHECK_NE(input.value, kInvalidNode);
    }
    CY_CHECK_EQ(named, 4U);

    // THE FAR-FIELD PROGRAM SAMPLES NOTHING, BY DERIVATION, and the report says so. A report
    // answered per MATERIAL rather than per program would call this one textured — which is the
    // single most efficient way for a published picture's caption to be wrong.
    const CompiledProgram* far = compiled.value().find(ProgramKind::FarField, QualityTier::High);
    CY_REQUIRE(far != nullptr);
    CY_CHECK(far->inputs.constants_only());
    CY_CHECK_EQ(far->inputs.textures, 0U);
    CY_CHECK_GT(far->inputs.inputs.size(), usize{0});
}


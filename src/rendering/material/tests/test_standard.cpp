// The standard material and the fallbacks. Tasks 4.2.4 and 4.2.5.
//
// `rendering-materials-and-shading` — "Standard material" lists the slots the engine ships without
// custom shader authoring, and "Fallback materials" requires an error material for each of three
// states, "each visually distinctive (magenta checkerboard) so problems are immediately obvious
// rather than silently black".
//
// The slot list is checked BY NAME. That is the point of the case: the shader is compiled against
// these identifiers, so a slot renamed in one place and not the other is a parameter the shader
// reads as zero — a black albedo, or a roughness of nought, neither of which looks like a renaming.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/material/standard.h>
#include <cy/test/test.h>

using cy::f32;
using cy::u32;
using cy::Vec4;
using namespace cy::rendering;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

}  // namespace

CY_TEST_CASE("every slot the specification lists is declared, by the name it lists") {
    MaterialProgram program(allocator());
    CY_REQUIRE(describe_standard_material(program, cy::Name::intern("standard"), ShadingModel::Lit,
                                          BlendMode::Opaque)
                   .has_value());

    const StandardParameters ids;
    struct Slot {
        ParameterId id;
        ParameterKind kind;
    };
    const Slot required[] = {
        {ids.base_color_factor, ParameterKind::Color},
        {ids.base_color_texture, ParameterKind::Texture},
        {ids.metallic_factor, ParameterKind::Float},
        {ids.roughness_factor, ParameterKind::Float},
        {ids.normal_texture, ParameterKind::Texture},
        {ids.normal_scale, ParameterKind::Float},
        {ids.occlusion_strength, ParameterKind::Float},
        {ids.emission_color, ParameterKind::Color},
        {ids.emission_texture, ParameterKind::Texture},
        {ids.opacity, ParameterKind::Float},
        {ids.alpha_cutoff, ParameterKind::Float},
        {ids.height_texture, ParameterKind::Texture},
        {ids.height_scale, ParameterKind::Float},
        {ids.clearcoat, ParameterKind::Float},
        {ids.clearcoat_roughness, ParameterKind::Float},
        {ids.anisotropy, ParameterKind::Float},
        {ids.anisotropy_direction, ParameterKind::Vec2},
        {ids.subsurface_color, ParameterKind::Color},
        {ids.thickness_texture, ParameterKind::Texture},
        {ids.detail_texture, ParameterKind::Texture},
        {ids.detail_uv_scale, ParameterKind::Vec2},
        {ids.uv_tiling_offset, ParameterKind::Vec4},
        {ids.uv_channel_mask, ParameterKind::Int},
        {ids.vertex_color_modulation, ParameterKind::Float},
        {ids.distance_fade, ParameterKind::Vec2},
        {ids.proximity_fade, ParameterKind::Vec2},
        {ids.use_triplanar, ParameterKind::Bool},
        {ids.use_world_space_uv, ParameterKind::Bool},
        {ids.use_detail_layer, ParameterKind::Bool},
        {ids.two_sided, ParameterKind::Bool},
    };
    for (const Slot& slot : required) {
        const MaterialParameter* parameter = program.find(slot.id);
        CY_REQUIRE(parameter != nullptr);
        CY_CHECK(parameter->kind == slot.kind);
    }

    // The packed ORM slot is ONE slot, not three: "WHEN occlusion, roughness, and metallic share
    // one texture THEN the material SHALL sample it once and route channels, rather than three
    // samples." Three slots pointing at one image is a thing a shader compiler cannot detect.
    CY_REQUIRE(program.find(ids.orm_texture) != nullptr);
    CY_CHECK(program.find(parameter_id("occlusion_texture")) == nullptr);
    CY_CHECK(program.find(parameter_id("roughness_texture")) == nullptr);
    CY_CHECK(program.find(parameter_id("metallic_texture")) == nullptr);
}

CY_TEST_CASE("the standard material's four static parameters are the whole permutation budget") {
    MaterialProgram program(allocator());
    CY_REQUIRE(describe_standard_material(program, cy::Name::intern("standard"), ShadingModel::Lit,
                                          BlendMode::Opaque)
                   .has_value());
    // Sixteen, from four static booleans. The number is asserted so that a fifth convenience
    // boolean is a conversation about doubling it rather than a silent change.
    CY_CHECK_EQ(program.static_bool_count(), 4U);
    CY_CHECK_EQ(program.permutation_count(), 16ULL);
    CY_CHECK_LE(program.block_bytes(), kMaterialBlockBytes);
}

CY_TEST_CASE("two descriptions of the standard material produce identical offsets") {
    // The offsets are what the shader is compiled against, so they must not depend on anything but
    // the declaration order — which is a fixed table.
    MaterialProgram first(allocator());
    MaterialProgram second(allocator());
    CY_REQUIRE(describe_standard_material(first, cy::Name::intern("a"), ShadingModel::Lit,
                                          BlendMode::Opaque)
                   .has_value());
    CY_REQUIRE(describe_standard_material(second, cy::Name::intern("b"), ShadingModel::Cloth,
                                          BlendMode::Translucent)
                   .has_value());
    CY_REQUIRE_EQ(first.parameters().size(), second.parameters().size());
    for (cy::usize index = 0; index < first.parameters().size(); ++index) {
        CY_CHECK_EQ(first.parameters()[index].id, second.parameters()[index].id);
        CY_CHECK_EQ(first.parameters()[index].offset, second.parameters()[index].offset);
    }
    // The model and the blend mode are the program's, not the layout's.
    CY_CHECK(second.model() == ShadingModel::Cloth);
    CY_CHECK(second.blend() == BlendMode::Translucent);
}

CY_TEST_CASE("the defaults are a plausible surface rather than a black one") {
    MaterialProgram program(allocator());
    MaterialTable table(allocator());
    CY_REQUIRE(describe_standard_material(program, cy::Name::intern("standard"), ShadingModel::Lit,
                                          BlendMode::Opaque)
                   .has_value());
    CY_REQUIRE(table.initialize(4).has_value());
    const auto index = table.allocate();
    CY_REQUIRE(index.has_value());

    const StandardParameters ids;
    CY_REQUIRE(apply_standard_defaults(program, table, *index, ids).has_value());

    CY_CHECK_NEAR(*table.get_float(program, *index, ids.metallic_factor), 0.0F, 1e-6F);
    // Not zero: perfectly smooth is a delta lobe, and a default that showed a single-pixel
    // highlight would look like an aliasing bug rather than like a default.
    CY_CHECK_NEAR(*table.get_float(program, *index, ids.roughness_factor), 0.5F, 1e-6F);
    CY_CHECK_NEAR(*table.get_float(program, *index, ids.opacity), 1.0F, 1e-6F);
    CY_CHECK_NEAR(*table.get_float(program, *index, ids.specular_factor), 1.0F, 1e-6F);
    // The identity tiling, spelled rather than assumed: a zeroed block would mean a tiling of zero
    // and a material that samples one texel across the whole surface.
    const Vec4 tiling = *table.get_vec4(program, *index, ids.uv_tiling_offset);
    CY_CHECK_NEAR(tiling.x, 1.0F, 1e-6F);
    CY_CHECK_NEAR(tiling.y, 1.0F, 1e-6F);
    CY_CHECK_NEAR(tiling.z, 0.0F, 1e-6F);
}

CY_TEST_CASE("each fallback is magenta, and the three are told apart by their second colour") {
    Vec4 primary;
    Vec4 secondary;
    Vec4 previous_secondary{-1.0F, -1.0F, -1.0F, -1.0F};
    for (u32 index = 0; index < static_cast<u32>(FallbackKind::Count); ++index) {
        const auto kind = static_cast<FallbackKind>(index);
        fallback_colors(kind, primary, secondary);
        // Magenta: the specification's word, and the one hue that appears in almost no authored
        // content, so a magenta surface is never ambiguous.
        CY_CHECK_NEAR(primary.x, 1.0F, 1e-6F);
        CY_CHECK_NEAR(primary.y, 0.0F, 1e-6F);
        CY_CHECK_NEAR(primary.z, 1.0F, 1e-6F);
        // Three different bugs, distinguishable without opening the log.
        const bool differs_from_previous = (secondary.x != previous_secondary.x) ||
                                           (secondary.y != previous_secondary.y) ||
                                           (secondary.z != previous_secondary.z);
        CY_CHECK(differs_from_previous);
        previous_secondary = secondary;
        CY_CHECK(fallback_kind_name(kind)[0] != '\0');
    }
}

CY_TEST_CASE("the fallback checkerboard alternates and is generated rather than loaded") {
    // Generated in code because "a fallback that lives in an asset is a fallback that is missing
    // exactly when the asset system is what failed".
    cy::Array<cy::u8> texels(allocator());
    CY_REQUIRE(generate_fallback_texture(FallbackKind::MissingMaterial, 8, 2, texels).has_value());
    CY_REQUIRE_EQ(texels.size(), 8U * 8U * 4U);

    const auto texel = [&texels](u32 x, u32 y) noexcept {
        return &texels[((static_cast<cy::usize>(y) * 8U) + x) * 4U];
    };
    // (0,0) is the primary and (2,0) is the secondary — a two-texel square.
    CY_CHECK_EQ(texel(0, 0)[0], 255);
    CY_CHECK_EQ(texel(0, 0)[1], 0);
    CY_CHECK_EQ(texel(1, 0)[0], 255);
    CY_CHECK_EQ(texel(2, 0)[0], 0);
    // And down a square: the checker alternates in both axes.
    CY_CHECK_EQ(texel(0, 2)[0], 0);
    CY_CHECK_EQ(texel(2, 2)[0], 255);
    // Opaque throughout: a fallback that could be blended away would be a fallback nobody sees.
    CY_CHECK_EQ(texel(3, 3)[3], 255);

    CY_CHECK_FALSE(
        generate_fallback_texture(FallbackKind::MissingTexture, 0, 2, texels).has_value());
    CY_CHECK_FALSE(
        generate_fallback_texture(FallbackKind::MissingTexture, 8, 0, texels).has_value());
}

CY_TEST_CASE("a fallback material is unlit and opaque, so it is visible when lighting is broken") {
    MaterialProgram program(allocator());
    CY_REQUIRE(
        describe_fallback_material(program, FallbackKind::ShaderCompilationFailed).has_value());
    CY_CHECK(program.model() == ShadingModel::Unlit);
    CY_CHECK(program.blend() == BlendMode::Opaque);
    CY_REQUIRE(program.find(parameter_id("emission_color")) != nullptr);
    // No permutations: an error material that had to be compiled in sixteen variants is an error
    // material that might fail to compile.
    CY_CHECK_EQ(program.permutation_count(), 1ULL);
}

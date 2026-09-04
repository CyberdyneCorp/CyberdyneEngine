// Material validation. Task 4.2.5.
//
// `rendering-materials-and-shading` — "Material validation": the engine reports "parameters
// declared but unused, textures bound to non-existent slots, shading model and blend mode
// combinations that are unsupported, and materials whose permutation count exceeds a budget", and
// the scenario is
//
//   "WHEN a material requests subsurface scattering with an additive blend mode THEN cooking SHALL
//    fail with an explanation, rather than producing undefined shading."
//
// The first case below is that scenario. The rest are the other three classes, plus the property
// that holds them together: `validate_material` returning `ok()` means VALIDATION RAN, and
// `report.fatal()` is what says the material must not cook. A cooker that could not tell those
// apart would treat a full disk and a broken material the same way.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/material/standard.h>
#include <cy/rendering/material/validation.h>
#include <cy/test/test.h>

using cy::u32;
using namespace cy::rendering;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

/// A minimal program with one texture slot, which is what most of the binding cases need.
[[nodiscard]] bool describe(MaterialProgram& program, ShadingModel model,
                            BlendMode blend) noexcept {
    if (!program.initialize(cy::Name::intern("subject"), model, blend).has_value()) {
        return false;
    }
    return program.add_parameter("base_color_texture", ParameterKind::Texture).has_value() &&
           program.add_parameter("alpha_cutoff", ParameterKind::Float).has_value();
}

[[nodiscard]] bool has_kind(const MaterialReport& report, MaterialIssueKind kind) noexcept {
    return report.count(kind) > 0;
}

}  // namespace

CY_TEST_CASE("subsurface scattering with an additive blend mode does not cook") {
    // The specification's scenario, and the reason validation exists at all: the combination has no
    // defined shading, so producing something would be worse than refusing.
    MaterialProgram program(allocator());
    CY_REQUIRE(describe(program, ShadingModel::SubsurfaceScattering, BlendMode::Additive));

    MaterialReport report(allocator());
    CY_REQUIRE(validate_material(program, cy::Span<const TextureBinding>(),
                                 MaterialValidationOptions{}, report)
                   .has_value());

    CY_CHECK(report.fatal());
    CY_REQUIRE(has_kind(report, MaterialIssueKind::UnsupportedCombination));
    // "fail with an explanation": the finding carries a sentence naming what to do instead, not a
    // code the author has to look up.
    for (const MaterialIssue& issue : report.issues()) {
        if (issue.kind == MaterialIssueKind::UnsupportedCombination) {
            CY_CHECK(issue.detail[0] != '\0');
            CY_CHECK(issue.fatal);
        }
    }

    // And the same model over an opaque surface is fine, which is what makes the rule about the
    // combination rather than about the model.
    MaterialProgram opaque(allocator());
    CY_REQUIRE(describe(opaque, ShadingModel::SubsurfaceScattering, BlendMode::Opaque));
    MaterialReport clean(allocator());
    CY_REQUIRE(validate_material(opaque, cy::Span<const TextureBinding>(),
                                 MaterialValidationOptions{}, clean)
                   .has_value());
    CY_CHECK_FALSE(clean.fatal());
}

CY_TEST_CASE("the combination table answers every model, and says why when it refuses") {
    for (u32 model_index = 0; model_index < cy::render::kShadingModelCount; ++model_index) {
        const auto model = static_cast<ShadingModel>(model_index);
        for (u32 blend_index = 0; blend_index <= static_cast<u32>(BlendMode::PremultipliedAlpha);
             ++blend_index) {
            const auto blend = static_cast<BlendMode>(blend_index);
            const bool supported = shading_model_supports_blend(model, blend);
            const char* conflict = shading_model_blend_conflict(model, blend);
            // The two are one answer: a pair is supported exactly when there is no explanation.
            CY_CHECK_EQ(supported, conflict == nullptr);
            if (!supported) {
                CY_CHECK(conflict[0] != '\0');
            }
        }
        // Opaque is legal for every model. A model that could not be drawn opaquely would be a
        // model no scene could use.
        CY_CHECK(shading_model_supports_blend(model, BlendMode::Opaque));
    }
    // Water does its own refraction, so a framebuffer operation that overrides it is refused.
    CY_CHECK_FALSE(shading_model_supports_blend(ShadingModel::Water, BlendMode::Additive));
    CY_CHECK(shading_model_supports_blend(ShadingModel::Water, BlendMode::Translucent));
    // Transparent hair is expensive and correct; the engine does not decide that for the author.
    CY_CHECK(shading_model_supports_blend(ShadingModel::Hair, BlendMode::Translucent));
}

CY_TEST_CASE("a masked material without an alpha cutoff does not cook") {
    // "WHEN a Masked material is rendered THEN it SHALL participate in the depth prepass with its
    // alpha test applied, so the opaque pass can use Equal depth testing." With no threshold the
    // prepass writes depth for the whole quad and the opaque pass shades it.
    MaterialProgram program(allocator());
    CY_REQUIRE(program.initialize(cy::Name::intern("leaf"), ShadingModel::Lit, BlendMode::Masked)
                   .has_value());
    CY_REQUIRE(program.add_parameter("base_color_texture", ParameterKind::Texture).has_value());

    MaterialReport report(allocator());
    CY_REQUIRE(validate_material(program, cy::Span<const TextureBinding>(),
                                 MaterialValidationOptions{}, report)
                   .has_value());
    CY_CHECK(report.fatal());
    CY_CHECK(has_kind(report, MaterialIssueKind::MissingRequiredParameter));

    // With the threshold declared, it cooks.
    CY_REQUIRE(program.add_parameter("alpha_cutoff", ParameterKind::Float).has_value());
    MaterialReport second(allocator());
    CY_REQUIRE(validate_material(program, cy::Span<const TextureBinding>(),
                                 MaterialValidationOptions{}, second)
                   .has_value());
    CY_CHECK_FALSE(second.fatal());
}

CY_TEST_CASE("a permutation count over the budget is reported against the budget it exceeded") {
    MaterialProgram program(allocator());
    CY_REQUIRE(program.initialize(cy::Name::intern("busy"), ShadingModel::Lit, BlendMode::Opaque)
                   .has_value());
    char name[16] = {};
    for (u32 index = 0; index < 8; ++index) {
        name[0] = 's';
        name[1] = static_cast<char>('a' + index);
        name[2] = '\0';
        CY_REQUIRE(program.add_parameter(name, ParameterKind::Bool, true).has_value());
    }
    CY_CHECK_EQ(program.permutation_count(), 256ULL);

    MaterialValidationOptions options;
    options.permutation_budget = 64;
    MaterialReport report(allocator());
    CY_REQUIRE(
        validate_material(program, cy::Span<const TextureBinding>(), options, report).has_value());
    CY_CHECK(report.fatal());
    CY_CHECK(has_kind(report, MaterialIssueKind::PermutationBudgetExceeded));

    // Raising the budget is the wrong fix and is therefore possible: the number is a project's to
    // set, and the report names it so the choice is visible in a diff.
    options.permutation_budget = 1024;
    MaterialReport generous(allocator());
    CY_REQUIRE(validate_material(program, cy::Span<const TextureBinding>(), options, generous)
                   .has_value());
    CY_CHECK_FALSE(generous.fatal());
}

CY_TEST_CASE("a texture bound to a slot that does not exist does not cook") {
    MaterialProgram program(allocator());
    CY_REQUIRE(describe(program, ShadingModel::Lit, BlendMode::Opaque));

    TextureBinding bindings[2];
    bindings[0].name = cy::Name::intern("rock_albedo");
    bindings[0].slot = parameter_id("no_such_slot");
    bindings[0].format = TextureFormat::Bc7Srgb;
    bindings[0].expects = TextureUsageClass::Color;
    // Bound to a parameter that exists but is not a texture: writing a bindless index into it would
    // overwrite the float that is actually there.
    bindings[1].name = cy::Name::intern("rock_mask");
    bindings[1].slot = parameter_id("alpha_cutoff");
    bindings[1].format = TextureFormat::Bc4Unorm;
    bindings[1].expects = TextureUsageClass::Data;

    MaterialReport report(allocator());
    CY_REQUIRE(validate_material(program, cy::Span<const TextureBinding>(bindings, 2),
                                 MaterialValidationOptions{}, report)
                   .has_value());
    CY_CHECK(report.fatal());
    CY_CHECK_EQ(report.count(MaterialIssueKind::UnknownTextureSlot), 2U);
    // The finding names the BINDING, so an author is told which texture rather than which slot
    // identifier.
    CY_CHECK(report.issues()[0].subject == cy::Name::intern("rock_albedo"));
}

CY_TEST_CASE("an sRGB texture in a data slot does not cook") {
    // "WHEN a texture assigned to a data slot is marked sRGB THEN the import or material validation
    // SHALL warn, since it would be decoded incorrectly." Fatal here rather than a warning: the
    // hardware decode makes every value the shader reads wrong by a gamma, and the symptom looks
    // like a lighting bug for as long as nobody suspects the texture.
    MaterialProgram program(allocator());
    CY_REQUIRE(describe(program, ShadingModel::Lit, BlendMode::Opaque));

    TextureBinding binding;
    binding.name = cy::Name::intern("orm");
    binding.slot = parameter_id("base_color_texture");
    binding.format = TextureFormat::Bc7Srgb;
    binding.expects = TextureUsageClass::Data;

    MaterialReport report(allocator());
    CY_REQUIRE(validate_material(program, cy::Span<const TextureBinding>(&binding, 1),
                                 MaterialValidationOptions{}, report)
                   .has_value());
    CY_CHECK(report.fatal());
    CY_CHECK(has_kind(report, MaterialIssueKind::ColorSpaceMismatch));

    // The other direction is a note and not a refusal: a linear colour texture is correct for HDR
    // content and for a ramp read as data.
    binding.format = TextureFormat::Rgba16Sfloat;
    binding.expects = TextureUsageClass::Color;
    MaterialReport note(allocator());
    CY_REQUIRE(validate_material(program, cy::Span<const TextureBinding>(&binding, 1),
                                 MaterialValidationOptions{}, note)
                   .has_value());
    CY_CHECK_FALSE(note.fatal());
    CY_CHECK(has_kind(note, MaterialIssueKind::ColorSpaceMismatch));

    // And a correctly cooked pair reports nothing at all.
    binding.format = TextureFormat::Bc7Srgb;
    binding.expects = TextureUsageClass::Color;
    MaterialReport clean(allocator());
    CY_REQUIRE(validate_material(program, cy::Span<const TextureBinding>(&binding, 1),
                                 MaterialValidationOptions{}, clean)
                   .has_value());
    CY_CHECK(clean.empty());
}

CY_TEST_CASE("a parameter nothing reads is reported without stopping the cook") {
    MaterialProgram program(allocator());
    CY_REQUIRE(describe(program, ShadingModel::Lit, BlendMode::Opaque));
    // What the shader system's reflection would say: nothing reads the cutoff.
    program.mark_referenced(parameter_id("alpha_cutoff"), false);

    MaterialReport report(allocator());
    CY_REQUIRE(validate_material(program, cy::Span<const TextureBinding>(),
                                 MaterialValidationOptions{}, report)
                   .has_value());
    CY_CHECK_FALSE(report.fatal());
    CY_REQUIRE_EQ(report.count(MaterialIssueKind::UnusedParameter), 1U);
    CY_CHECK(report.issues()[0].subject == cy::Name::intern("alpha_cutoff"));

    // Off while a program is under construction: reflection has not run, and every parameter would
    // otherwise be reported.
    MaterialValidationOptions options;
    options.report_unused = false;
    MaterialReport quiet(allocator());
    CY_REQUIRE(
        validate_material(program, cy::Span<const TextureBinding>(), options, quiet).has_value());
    CY_CHECK(quiet.empty());
}

CY_TEST_CASE("the standard material validates clean, which is the claim the engine ships") {
    MaterialProgram program(allocator());
    CY_REQUIRE(describe_standard_material(program, cy::Name::intern("standard"), ShadingModel::Lit,
                                          BlendMode::Opaque)
                   .has_value());
    MaterialReport report(allocator());
    CY_REQUIRE(validate_material(program, cy::Span<const TextureBinding>(),
                                 MaterialValidationOptions{}, report)
                   .has_value());
    CY_CHECK_FALSE(report.fatal());
    CY_CHECK(report.empty());

    // Including as a masked material, which is the standard material's other common blend mode and
    // the one with a required parameter.
    MaterialProgram masked(allocator());
    CY_REQUIRE(describe_standard_material(masked, cy::Name::intern("foliage"),
                                          ShadingModel::Foliage, BlendMode::Masked)
                   .has_value());
    MaterialReport masked_report(allocator());
    CY_REQUIRE(validate_material(masked, cy::Span<const TextureBinding>(),
                                 MaterialValidationOptions{}, masked_report)
                   .has_value());
    CY_CHECK_FALSE(masked_report.fatal());
}

CY_TEST_CASE("a report accumulates, is clearable, and every issue kind has a name") {
    MaterialReport report(allocator());
    CY_CHECK(report.empty());
    CY_CHECK_FALSE(report.fatal());

    CY_REQUIRE(report
                   .add(MaterialIssue{MaterialIssueKind::UnusedParameter, cy::Name::intern("a"),
                                      "note", false})
                   .has_value());
    CY_CHECK_FALSE(report.fatal());
    CY_REQUIRE(report
                   .add(MaterialIssue{MaterialIssueKind::UnsupportedCombination,
                                      cy::Name::intern("b"), "error", true})
                   .has_value());
    CY_CHECK(report.fatal());
    CY_CHECK_EQ(report.issues().size(), 2U);

    report.clear();
    CY_CHECK(report.empty());
    CY_CHECK_FALSE(report.fatal());

    for (u32 index = 0; index < static_cast<u32>(MaterialIssueKind::Count); ++index) {
        CY_CHECK(material_issue_kind_name(static_cast<MaterialIssueKind>(index))[0] != '\0');
    }
}

// Material validation: what a cook reports, and what it refuses. See
// cy/rendering/material/validation.h.

#include <cy/rendering/material/validation.h>

namespace cy::rendering {
namespace {

constexpr const char* kIssueKindNames[] = {
    "UnusedParameter",        "UnknownTextureSlot",        "ColorSpaceMismatch",
    "UnsupportedCombination", "PermutationBudgetExceeded", "MissingRequiredParameter",
};
static_assert(sizeof(kIssueKindNames) / sizeof(kIssueKindNames[0]) ==
                  static_cast<usize>(MaterialIssueKind::Count),
              "add a name for the new MaterialIssueKind, in enumerator order");

/// The `Masked` blend mode's contract with the depth prepass.
///
/// "WHEN a `Masked` material is rendered THEN it SHALL participate in the depth prepass with its
/// alpha test applied, so the opaque pass can use `Equal` depth testing." A program with no
/// threshold to test cannot do that, and the failure mode is not subtle — the prepass writes depth
/// for the whole quad and the opaque pass then shades it.
constexpr ParameterId kAlphaCutoffId = parameter_id("alpha_cutoff");

}  // namespace

const char* material_issue_kind_name(MaterialIssueKind kind) noexcept {
    const auto index = static_cast<usize>(kind);
    return (index < static_cast<usize>(MaterialIssueKind::Count)) ? kIssueKindNames[index]
                                                                  : "<invalid>";
}

// --- The report -----------------------------------------------------------------------------

Status MaterialReport::add(const MaterialIssue& issue) noexcept {
    if (Status pushed = issues_.push_back(issue); !pushed) {
        return pushed;
    }
    fatal_ = fatal_ || issue.fatal;
    return ok();
}

void MaterialReport::clear() noexcept {
    issues_.clear();
    fatal_ = false;
}

u32 MaterialReport::count(MaterialIssueKind kind) const noexcept {
    u32 total = 0;
    for (const MaterialIssue& issue : issues_) {
        if (issue.kind == kind) {
            ++total;
        }
    }
    return total;
}

// --- The combination table ------------------------------------------------------------------
//
// STATED AS RULES RATHER THAN AS A 9x6 TABLE, because a table of fifty-four booleans is a table
// nobody can review: the reason a cell is false is the interesting part, and a rule carries it.
// Each rule below is a consequence of how the model is EVALUATED, not a policy:
//
//   SubsurfaceScattering diffuses light in screen space, in a pass that reads the depth buffer and
//   the scene colour written by the opaque pass. A surface that never wrote depth has nothing for
//   that pass to gather from, so every transparent blend mode is undefined for it rather than
//   merely expensive.
//
//   Water refracts what is behind it by sampling the scene colour and absorbing along the view ray
//   through the water column. `Additive` and `Modulate` discard the surface's own transmission
//   entirely — the framebuffer operation replaces the model's — so the two cannot both be in
//   effect. `Opaque` and the alpha blends are fine: the model does its own refraction.
//
// Everything else is legal. Transparent hair or cloth is expensive and correct, and the engine does
// not decide for the author which costs are worth paying.

const char* shading_model_blend_conflict(ShadingModel model, BlendMode blend) noexcept {
    switch (model) {
        case ShadingModel::SubsurfaceScattering:
            if (blend != BlendMode::Opaque && blend != BlendMode::Masked) {
                return "subsurface scattering diffuses light in screen space from the depth and "
                       "colour the opaque pass wrote; a surface that does not write depth gives "
                       "that pass nothing to gather, so the combination has no defined shading. "
                       "Use Opaque or Masked, or a translucent model such as Foliage.";
            }
            return nullptr;
        case ShadingModel::Water:
            if (blend == BlendMode::Additive || blend == BlendMode::Modulate) {
                return "the water model refracts and absorbs along the view ray through the water "
                       "column, which an additive or modulate framebuffer operation overrides. Use "
                       "Opaque or Translucent.";
            }
            return nullptr;
        case ShadingModel::Lit:
        case ShadingModel::Unlit:
        case ShadingModel::ClearCoat:
        case ShadingModel::Anisotropic:
        case ShadingModel::Cloth:
        case ShadingModel::Hair:
        case ShadingModel::Foliage:
        case ShadingModel::Count:
            break;
    }
    return nullptr;
}

bool shading_model_supports_blend(ShadingModel model, BlendMode blend) noexcept {
    return shading_model_blend_conflict(model, blend) == nullptr;
}

// --- Validation -----------------------------------------------------------------------------

namespace {

/// One check per function, and `validate_material` is the list of them.
///
/// WRITTEN THIS WAY ON PURPOSE. As one function this was forty-two points of cognitive complexity —
/// five independent rules interleaved with the same `if (Status added = report.add(...); !added)`
/// four times over, which is the shape where a sixth rule gets added in the wrong place. Each check
/// below states one rule and nothing else, and the reason each rule exists lives beside it.

[[nodiscard]] Status check_combination(const MaterialProgram& program,
                                       MaterialReport& report) noexcept {
    const char* conflict = shading_model_blend_conflict(program.model(), program.blend());
    if (conflict == nullptr) {
        return ok();
    }
    return report.add(
        MaterialIssue{MaterialIssueKind::UnsupportedCombination, program.name(), conflict, true});
}

[[nodiscard]] Status check_masked_threshold(const MaterialProgram& program,
                                            MaterialReport& report) noexcept {
    if (program.blend() != BlendMode::Masked || program.find(kAlphaCutoffId) != nullptr) {
        return ok();
    }
    return report.add(MaterialIssue{
        MaterialIssueKind::MissingRequiredParameter, program.name(),
        "a Masked material needs an `alpha_cutoff` parameter: the depth prepass applies the alpha "
        "test and the opaque pass then depth-tests Equal against what it wrote",
        true});
}

[[nodiscard]] Status check_permutation_budget(const MaterialProgram& program,
                                              const MaterialValidationOptions& options,
                                              MaterialReport& report) noexcept {
    if (program.permutation_count() <= options.permutation_budget) {
        return ok();
    }
    return report.add(MaterialIssue{
        MaterialIssueKind::PermutationBudgetExceeded, program.name(),
        "the program's static parameters imply more permutations than the budget allows; each "
        "static boolean doubles the count, so the fix is to make one of them runtime data rather "
        "than to raise the budget",
        true});
}

/// The slot a binding names must exist and must be a texture slot.
[[nodiscard]] Status check_binding_slot(const MaterialParameter* slot,
                                        const TextureBinding& binding,
                                        MaterialReport& report) noexcept {
    if (slot == nullptr) {
        return report.add(MaterialIssue{
            MaterialIssueKind::UnknownTextureSlot, binding.name,
            "no parameter with this identifier exists in the program; the texture would not be "
            "bound and the material would render as if the texture were missing",
            true});
    }
    if (slot->kind != ParameterKind::Texture) {
        return report.add(MaterialIssue{
            MaterialIssueKind::UnknownTextureSlot, binding.name,
            "the parameter this texture is bound to is not a texture slot; writing a bindless "
            "index into it would overwrite whatever the slot actually holds",
            true});
    }
    return ok();
}

/// The colour space the slot wants against the one the texture was cooked in.
[[nodiscard]] Status check_binding_color_space(const TextureBinding& binding,
                                               MaterialReport& report) noexcept {
    const render::TextureFormatInfo& info = render::texture_format_info(binding.format);
    if (info.is_srgb && binding.expects != TextureUsageClass::Color) {
        // Fatal: hardware decodes it, so every value the shader reads is wrong by a gamma — which
        // looks like a lighting bug for as long as nobody suspects the texture.
        return report.add(MaterialIssue{
            MaterialIssueKind::ColorSpaceMismatch, binding.name,
            "an sRGB texture is bound to a slot whose contents are a measurement rather than a "
            "colour; hardware decodes it, so roughness, metallic, normals or masks read back wrong "
            "by a gamma",
            true});
    }
    if (!info.is_srgb && info.is_color && binding.expects == TextureUsageClass::Color &&
        binding.format != TextureFormat::Undefined) {
        // The other direction, and NOT fatal: a linear colour texture is a deliberate choice for
        // HDR content and for a colour ramp read as data.
        return report.add(MaterialIssue{
            MaterialIssueKind::ColorSpaceMismatch, binding.name,
            "a colour slot is bound to a linear texture; correct for HDR content, and a mistake "
            "for an authored albedo, which would render washed out",
            false});
    }
    return ok();
}

[[nodiscard]] Status check_unused_parameters(const MaterialProgram& program,
                                             MaterialReport& report) noexcept {
    for (const MaterialParameter& parameter : program.parameters()) {
        if (parameter.referenced) {
            continue;
        }
        if (Status added = report.add(MaterialIssue{
                MaterialIssueKind::UnusedParameter, parameter.name,
                "declared but read by nothing the shader reflection found; it occupies block space "
                "and is uploaded every time the material changes",
                false});
            !added) {
            return added;
        }
    }
    return ok();
}

}  // namespace

Status validate_material(const MaterialProgram& program, Span<const TextureBinding> bindings,
                         const MaterialValidationOptions& options,
                         MaterialReport& report) noexcept {
    // In the order a report is read from the top: what stops the cook first, then the bindings in
    // the order the author gave them, then the notes.
    if (Status checked = check_combination(program, report); !checked) {
        return checked;
    }
    if (Status checked = check_masked_threshold(program, report); !checked) {
        return checked;
    }
    if (Status checked = check_permutation_budget(program, options, report); !checked) {
        return checked;
    }
    for (const TextureBinding& binding : bindings) {
        const MaterialParameter* slot = program.find(binding.slot);
        if (Status checked = check_binding_slot(slot, binding, report); !checked) {
            return checked;
        }
        if (slot == nullptr || slot->kind != ParameterKind::Texture) {
            continue;
        }
        if (Status checked = check_binding_color_space(binding, report); !checked) {
            return checked;
        }
    }
    if (!options.report_unused) {
        return ok();
    }
    return check_unused_parameters(program, report);
}

}  // namespace cy::rendering

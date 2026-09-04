#pragma once
// Material validation. Task 4.2.5.
//
// `rendering-materials-and-shading` — "Material validation": "The engine SHALL validate materials
// at cook time and report: parameters declared but unused, textures bound to non-existent slots,
// shading model and blend mode combinations that are unsupported, and materials whose permutation
// count exceeds a budget", and the scenario is:
//
//   "WHEN a material requests subsurface scattering with an additive blend mode THEN cooking SHALL
//    fail with an explanation, rather than producing undefined shading."
//
// ================================================================================================
// WHY A REPORT AND NOT A BOOLEAN
// ================================================================================================
//
// The four things the specification asks about are not the same severity, and collapsing them into
// one answer would force the wrong behaviour on two of them. A parameter nobody reads is a waste of
// sixteen bytes and a note to the author; an additive subsurface material has no defined meaning
// and must not cook. So every finding is an `MaterialIssue` carrying its own `fatal` flag, and
// `MaterialReport::fatal()` is what a cooker branches on. A warning that stopped a cook would be
// disabled within a week; an error that only warned would ship undefined shading.
//
// The report is also the shape an editor wants: a list it can show beside the material, each entry
// naming the parameter or the slot it is about. A boolean would have to be re-derived into that.
//
// ================================================================================================
// WHAT VALIDATION CAN SEE, AND WHAT IT DELIBERATELY CANNOT
// ================================================================================================
//
// It sees the PROGRAM — the parameter layout, the shading model, the blend mode, the permutation
// count — and the BINDINGS an author made against it. It does not see a device, a compiled shader
// or a texture's pixels, and it must not: this runs at cook time, on a machine that may have no
// GPU, and a validation that needed one would be a validation nobody runs in continuous
// integration.
//
// "Parameters declared but unused" therefore rests on `MaterialParameter::referenced`, which the
// shader system writes from reflection (`MaterialProgram::mark_referenced`). Validation reports
// what reflection found; it does not parse shader source to find out for itself, because there
// would then be two implementations of "does this shader read this parameter" and they would
// disagree.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/rendering/material/material.h>
#include <cy/servers/render/types.h>

namespace cy::rendering {

using render::TextureFormat;
using render::TextureUsageClass;

/// What a finding is about. One enumerator per thing the specification asks to be reported, plus
/// the colour-space check `rendering-materials-and-shading` states under "Texture sampling
/// conventions" — it belongs here because material validation is the second place it can be caught
/// and the first (import) is not always the same person's work.
enum class MaterialIssueKind : u8 {
    /// A parameter the program declares that nothing reads. Not fatal: it costs block space and an
    /// author's attention, and a program under construction legitimately has them.
    UnusedParameter = 0,
    /// A binding naming a parameter that does not exist, or one that is not a texture slot. Fatal:
    /// the texture would silently not be bound, which reads as a missing texture at run time and
    /// sends the reader looking at the asset.
    UnknownTextureSlot,
    /// An sRGB texture in a slot whose data is a measurement rather than a colour. Fatal, because
    /// hardware decodes it and every value the shader reads is then wrong by a gamma — a defect
    /// that looks like a lighting bug for as long as nobody suspects the texture.
    ColorSpaceMismatch,
    /// A shading model and blend mode combination with no defined meaning.
    UnsupportedCombination,
    /// More permutations than the budget allows.
    PermutationBudgetExceeded,
    /// A `Masked` material with no `alpha_cutoff` parameter. Fatal: the depth prepass has no
    /// threshold to apply, and the opaque pass's `Equal` depth test then rejects the whole surface.
    MissingRequiredParameter,
    Count,
};

[[nodiscard]] const char* material_issue_kind_name(MaterialIssueKind kind) noexcept;

/// One finding.
struct MaterialIssue {
    MaterialIssueKind kind = MaterialIssueKind::UnusedParameter;
    /// The parameter or slot the finding is about, or an empty name when it is about the material
    /// as a whole.
    Name subject;
    /// A sentence for the author. A string literal owned by this module, never allocated — a report
    /// is produced in a cook loop and a per-issue allocation would be paid by every material.
    const char* detail = "";
    bool fatal = false;
};

/// What validation is measured against.
struct MaterialValidationOptions {
    /// "materials whose permutation count exceeds a budget". Sixty-four is four static booleans and
    /// two more — enough for the standard material with room, and small enough that a fifth
    /// convenience boolean is a conversation rather than a silent doubling.
    u64 permutation_budget = 64;
    /// Report unused parameters. Off while a program is under construction — the shader system has
    /// not run reflection over it yet and every parameter would be reported.
    bool report_unused = true;
};

/// A texture an author bound to a slot.
///
/// The format travels with the binding because the colour-space check needs it and because a cooker
/// has it: it chose the format from the texture's declared usage. `expects` is what the SLOT wants,
/// which the material author states — the pair is what makes "a data slot fed an sRGB texture"
/// expressible at all.
struct TextureBinding {
    Name name;
    ParameterId slot = 0;
    /// The bindless index the shader will read. Not validated here — it is assigned by the
    /// descriptor manager and a cooker does not know it yet — but carried so a report names the
    /// binding rather than the slot alone.
    u32 bindless_index = 0;
    TextureFormat format = TextureFormat::Undefined;
    TextureUsageClass expects = TextureUsageClass::Color;
};

/// The findings, in the order validation produced them — which is a function of the program's
/// declaration order and the binding array, and so is the same for two runs over the same inputs.
class MaterialReport {
public:
    explicit MaterialReport(Allocator& allocator) noexcept : issues_(allocator) {}

    MaterialReport(const MaterialReport&) = delete;
    MaterialReport& operator=(const MaterialReport&) = delete;

    [[nodiscard]] Status add(const MaterialIssue& issue) noexcept;
    void clear() noexcept;

    [[nodiscard]] Span<const MaterialIssue> issues() const noexcept { return issues_.span(); }
    [[nodiscard]] u32 count(MaterialIssueKind kind) const noexcept;
    /// True when any finding is fatal. What a cooker branches on.
    [[nodiscard]] bool fatal() const noexcept { return fatal_; }
    [[nodiscard]] bool empty() const noexcept { return issues_.empty(); }

private:
    Array<MaterialIssue> issues_;
    bool fatal_ = false;
};

/// Whether a shading model can be drawn under a blend mode, and why not when it cannot.
///
/// Public because the answer is needed in two places — validation, and the pipeline that would
/// otherwise have to rediscover it — and because a table with one caller is a table that grows a
/// second, differently worded copy.
[[nodiscard]] bool shading_model_supports_blend(ShadingModel model, BlendMode blend) noexcept;
/// The explanation for the pair, or `nullptr` when the pair is supported.
[[nodiscard]] const char* shading_model_blend_conflict(ShadingModel model,
                                                       BlendMode blend) noexcept;

/// Validate a program and its bindings, appending every finding to `report`.
///
/// Returns `ok()` when validation RAN, whatever it found — a report with fatal issues is a
/// successful validation of a broken material, and conflating the two would make a cooker unable to
/// tell "could not validate" from "validated and it is wrong". The only failure is an allocation
/// one.
[[nodiscard]] Status validate_material(const MaterialProgram& program,
                                       Span<const TextureBinding> bindings,
                                       const MaterialValidationOptions& options,
                                       MaterialReport& report) noexcept;

}  // namespace cy::rendering

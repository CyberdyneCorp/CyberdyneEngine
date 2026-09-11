#pragma once
// THE COOK-TIME HALF OF THE DETERMINISM FIREWALL. M8.c task 1.3.
//
// ================================================================================================
// WHY THIS IS A SEPARATE FILE FROM `<cy/ecs/firewall.h>`, AND WHY IT IS COOK TIME
// ================================================================================================
//
// One firewall, two halves, because the two rules bite at two different moments.
//
//   `<cy/ecs/firewall.h>`   RUNTIME. A VFX-driven or non-pinned inference code path attempting an
//                           authoritative write is refused at the ECS write path.
//   this file               COOK TIME. A non-pinned model wired to an AI node that declares its
//                           output authoritative is refused **when the content is cooked**.
//
// `ml-inference` is explicit that the second is not the first:
//
//   "Where a model is used inside an AI graph, the graph node SHALL declare whether its output is
//    authoritative. A non-pinned model feeding an authoritative node SHALL be **rejected at cook
//    time**."
//
//   "#### Scenario: Multiplayer desync is prevented at cook time — WHEN a non-pinned model's output
//    feeds an authoritative AI decision THEN cooking SHALL fail with a diagnostic, rather than the
//    mismatch surfacing later as a multiplayer desync."
//
// **A runtime diagnostic does not satisfy that requirement and this file exists to say so once.**
// The runtime half refuses the write on the machine that runs it; the desync it is meant to prevent
// is between two machines whose *content* disagrees, and content is decided when it is cooked. A
// build that ships a non-pinned model behind an authoritative node has already shipped the defect
// even if every one of its writes is refused: the two peers then take different non-authoritative
// paths and disagree about the outcome months later, which is exactly the failure `ml-inference`
// describes and exactly what a runtime refusal is too late to prevent.
//
// ================================================================================================
// WHY THIS LIVES IN `src/gameplay/` AND NOT IN `src/inference/`
// ================================================================================================
//
// Because the thing being protected is gameplay's. "Authoritative" is a gameplay word in this
// engine — `NetworkAuthority`, `CommandDeclaration::authoritative`, `gameplay-framework`'s
// participants and their authority — and a gate that lived inside the module it gates would be a
// producer checking itself. It also has to be callable by a cook that was built with `CY_ML` OFF:
// a project that disabled inference must still refuse a package whose AI graphs bind models,
// rather than silently cooking one that a build with `CY_ML` ON would refuse.
//
// So the declarations below are plain values with no dependency on any inference type, exactly as
// `ml-inference` requires of everything outside a backend module — "Game and engine code SHALL
// depend only on the CyberML interface, never on a specific runtime's types" — and here not even on
// that. `src/inference/` fills them in from the model assets it reads; `src/ai/` fills them in from
// the graph nodes it compiles; the cook driver calls `check_inference_bindings` and fails its run
// on a non-ok status.
//
// **WHAT IS NOT DONE HERE, STATED RATHER THAN IMPLIED.** No cook driver calls this yet, because on
// the tree this was written against there is no `src/inference/`, no model asset and no AI graph
// node that binds one — those are M8.c section 4's, and section 1 is ordered before it precisely so
// that the gate exists before the first model does. The call site is `tools/cook/`'s `run()`, which
// already fails a run on a returned error; wiring it is one call and it belongs in the change that
// introduces the first `ModelPinning` to pass to it.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

namespace cy::gameplay {

/// What a model asset declares about its own reproducibility.
///
/// `ml-inference`: a session is **pinned** when it has "a fixed backend, fixed precision, and a
/// configuration the model asset declares as verified reproducible". All three are needed, so all
/// three are here and `pinned()` is the conjunction rather than a separate boolean somebody could
/// set without the other fields agreeing.
struct ModelPinning {
    /// The model asset's name, as a binding names it.
    Name model;
    /// The backend the asset declares verified — empty when it declares none.
    Name backend;
    /// The precision the asset declares verified — empty when it declares none.
    Name precision;
    /// A digest of the configuration the asset declares reproducible. Zero means "declares none",
    /// which is the honest encoding of an asset that was never verified: a configuration whose
    /// digest happens to be zero is not a thing a hash produces, and treating zero as a value would
    /// make "unverified" and "verified against configuration zero" the same state.
    u64 verified_configuration = 0;

    /// True when the asset declares a pinned configuration at all.
    [[nodiscard]] bool pinned() const noexcept {
        return verified_configuration != 0 && !backend.is_empty() && !precision.is_empty();
    }
};

/// One AI-graph node that consumes a model, as the graph compiler records it.
struct InferenceBinding {
    Name graph;
    Name node;
    Name model;
    /// `ml-inference`: "the graph node SHALL declare whether its output is authoritative". The
    /// declaration is the node's, and this is where the cook reads it.
    bool authoritative = false;
    /// The configuration this cook targets — backend, precision and device placement, digested.
    /// Compared against `ModelPinning::verified_configuration`.
    u64 cook_configuration = 0;
};

/// Why a binding was refused. Structured for the same reason `ValidationReason` is: "cooking
/// failed" is unactionable, and the four cases below each have a different fix.
enum class CookRefusalTag : u8 {
    None = 0,
    /// The binding names a model no `ModelPinning` describes. Refused rather than assumed pinned:
    /// an unknown model is the case where guessing is most expensive.
    UnknownModel = 1,
    /// The model declares no verified-reproducible configuration and the node is authoritative.
    ModelNotPinned = 2,
    /// The model is pinned, but to a configuration this cook is not producing.
    PinnedConfigurationMismatch = 3,
    Count = 4,
};

const char* cook_refusal_tag_name(CookRefusalTag tag) noexcept;

/// One refused binding, with the numbers behind it.
struct CookRefusal {
    CookRefusalTag tag = CookRefusalTag::None;
    Name graph;
    Name node;
    Name model;
    /// What the asset declares verified, and what this cook is producing. Both reported, because
    /// "they differ" without the two values is the diagnostic this project has spent milestones
    /// removing.
    u64 verified_configuration = 0;
    u64 cook_configuration = 0;
};

/// What one gate run examined. Reported whether or not anything was refused, because a gate that
/// looked at nothing and a gate that found nothing read identically otherwise — which is the defect
/// class this project has paid for more than once.
struct CookFirewallReport {
    u32 bindings_examined = 0;
    u32 authoritative_bindings = 0;
    u32 pinned_bindings = 0;
    /// Bindings that are not authoritative. Permitted without pinning — `ml-inference`'s
    /// "Presentation use is unrestricted" — and counted so the gate can say how much it waved past.
    u32 presentation_bindings = 0;
    u32 refusals = 0;
};

/// **The gate.** Returns a non-ok status when any binding is refused, so a cook driver that already
/// fails its run on an error fails on this without a second convention.
///
/// `refusals` is appended to, not cleared, so a driver may accumulate across several graphs.
[[nodiscard]] Status check_inference_bindings(Span<const ModelPinning> models,
                                              Span<const InferenceBinding> bindings,
                                              Array<CookRefusal>& refusals,
                                              CookFirewallReport& report) noexcept;

/// One binding's verdict, for a caller that has a single node to check.
[[nodiscard]] CookRefusalTag check_inference_binding(Span<const ModelPinning> models,
                                                     const InferenceBinding& binding,
                                                     const ModelPinning** matched) noexcept;

}  // namespace cy::gameplay

// The cook-time half of the determinism firewall. M8.c task 1.3.
//
// Nothing here allocates except the caller's `refusals` array, and nothing here reads a file: the
// gate is a pure function of the declarations it is handed, so a cook driver can call it, a test
// can call it, and both get the same answer. That is deliberate — `ml-inference` asks for a cook
// that FAILS, and a check that needed a session, a backend or a device to run would be a check that
// could not run inside a cook on a build machine with no GPU.

#include <cy/gameplay/cook_firewall.h>

namespace cy::gameplay {

const char* cook_refusal_tag_name(CookRefusalTag tag) noexcept {
    switch (tag) {
        case CookRefusalTag::None:
            return "none";
        case CookRefusalTag::UnknownModel:
            return "unknown-model";
        case CookRefusalTag::ModelNotPinned:
            return "model-not-pinned";
        case CookRefusalTag::PinnedConfigurationMismatch:
            return "pinned-configuration-mismatch";
        case CookRefusalTag::Count:
            break;
    }
    return "unknown";
}

CookRefusalTag check_inference_binding(Span<const ModelPinning> models,
                                       const InferenceBinding& binding,
                                       const ModelPinning** matched) noexcept {
    const ModelPinning* model = nullptr;
    for (const ModelPinning& candidate : models) {
        if (candidate.model == binding.model) {
            model = &candidate;
            break;
        }
    }
    if (matched != nullptr) {
        *matched = model;
    }
    if (!binding.authoritative) {
        // `ml-inference` — "Presentation use is unrestricted": a model driving an animation blend
        // weight or a non-authoritative visual choice needs no pinning, and an unknown model behind
        // one is a content error for somebody else's check rather than a determinism defect.
        return CookRefusalTag::None;
    }
    if (model == nullptr) {
        return CookRefusalTag::UnknownModel;
    }
    if (!model->pinned()) {
        return CookRefusalTag::ModelNotPinned;
    }
    if (model->verified_configuration != binding.cook_configuration) {
        // Pinned, but to a configuration this cook is not producing. The same desync with an extra
        // step: the asset's claim is true of a build nobody is making.
        return CookRefusalTag::PinnedConfigurationMismatch;
    }
    return CookRefusalTag::None;
}

Status check_inference_bindings(Span<const ModelPinning> models,
                                Span<const InferenceBinding> bindings, Array<CookRefusal>& refusals,
                                CookFirewallReport& report) noexcept {
    report = CookFirewallReport{};
    for (const InferenceBinding& binding : bindings) {
        ++report.bindings_examined;
        if (!binding.authoritative) {
            ++report.presentation_bindings;
            continue;
        }
        ++report.authoritative_bindings;

        const ModelPinning* model = nullptr;
        const CookRefusalTag tag = check_inference_binding(models, binding, &model);
        if (tag == CookRefusalTag::None) {
            ++report.pinned_bindings;
            continue;
        }

        CookRefusal refusal;
        refusal.tag = tag;
        refusal.graph = binding.graph;
        refusal.node = binding.node;
        refusal.model = binding.model;
        refusal.verified_configuration = (model == nullptr) ? 0 : model->verified_configuration;
        refusal.cook_configuration = binding.cook_configuration;
        if (Status pushed = refusals.push_back(refusal); !pushed) {
            return pushed;
        }
        ++report.refusals;
    }

    if (report.refusals != 0) {
        // The cook fails. `ml-inference`: "cooking SHALL fail with a diagnostic, rather than the
        // mismatch surfacing later as a multiplayer desync". The diagnostic is the `refusals`
        // array — one row per binding, with the model, the node and both configuration digests —
        // and the status is what makes a driver stop.
        return fail(ErrorCode::PermissionDenied,
                    "a non-pinned model feeds an authoritative AI node; see the cook firewall's "
                    "refusals for the graph, the node, the model and the two configurations");
    }
    return ok();
}

}  // namespace cy::gameplay

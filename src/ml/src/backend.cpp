// The registry and the selection rule. M8.c task 4.1.

#include <cy/ml/backend.h>

namespace cy::ml {

const char* selection_outcome_name(SelectionOutcome outcome) noexcept {
    switch (outcome) {
        case SelectionOutcome::Explicit:
            return "explicit";
        case SelectionOutcome::Automatic:
            return "automatic";
        case SelectionOutcome::RequestedBackendAbsent:
            return "requested-backend-absent";
        case SelectionOutcome::NotValidatedForModel:
            return "not-validated-for-model";
        case SelectionOutcome::NoBackendRegistered:
            return "no-backend-registered";
        case SelectionOutcome::Count:
            break;
    }
    return "unknown";
}

Status BackendRegistry::add(InferenceBackend* backend) noexcept {
    if (backend == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a null backend is not a backend");
    }
    if (find(backend->kind()) != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "this backend kind is already registered; one implementation per kind");
    }
    if (count_ >= kMaxBackends) {
        return fail(ErrorCode::OutOfRange, "the registry holds one backend per BackendKind");
    }
    backends_[count_++] = backend;
    return ok();
}

InferenceBackend* BackendRegistry::find(BackendKind kind) const noexcept {
    for (u32 index = 0; index < count_; ++index) {
        if (backends_[index] != nullptr && backends_[index]->kind() == kind) {
            return backends_[index];
        }
    }
    return nullptr;
}

InferenceBackend* select_backend(const BackendRegistry& registry, const ModelAsset& asset,
                                 const SessionDesc& desc, SelectionReport& report) noexcept {
    report = SelectionReport{};

    // EXPLICIT. `ml-inference` allows a caller to name a backend, and a named backend that is not
    // present is an error rather than a silent fallback: a caller that asked for TensorRT because
    // its model is pinned to TensorRT must not quietly get ONNX Runtime.
    if (desc.backend != BackendKind::Unknown) {
        InferenceBackend* backend = registry.find(desc.backend);
        if (backend == nullptr) {
            report.outcome = SelectionOutcome::RequestedBackendAbsent;
            return nullptr;
        }
        report.outcome = SelectionOutcome::Explicit;
        report.chosen = desc.backend;
        return backend;
    }

    if (registry.size() == 0) {
        report.outcome = SelectionOutcome::NoBackendRegistered;
        return nullptr;
    }

    // AUTOMATIC: "the highest-preference available backend that the model declares as validated
    // SHALL be chosen, and the choice SHALL be reported".
    for (BackendKind kind : kPreferenceOrder) {
        InferenceBackend* backend = registry.find(kind);
        if (backend == nullptr) {
            continue;
        }
        if (!asset.validated_for(kind)) {
            ++report.rejected_unvalidated;
            continue;
        }
        report.outcome = SelectionOutcome::Automatic;
        report.chosen = kind;
        return backend;
    }

    report.outcome = SelectionOutcome::NotValidatedForModel;
    return nullptr;
}

}  // namespace cy::ml

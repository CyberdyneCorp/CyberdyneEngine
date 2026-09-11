// The bridge from a model asset to the cook-time firewall's declarations. M8.c task 4.2.

#include <cy/ml/cook.h>

namespace cy::ml {

gameplay::ModelPinning pinning_of(const ModelAsset& asset) noexcept {
    const ModelDeterminism& determinism = asset.determinism();
    gameplay::ModelPinning pinning;
    pinning.model = asset.name();
    // An UNKNOWN backend or precision interns to the EMPTY name rather than to the text "unknown",
    // because `ModelPinning::pinned()` reads emptiness as "declares none". Interning "unknown" here
    // would make an undeclared model look pinned to a configuration called unknown, which is the
    // exact failure the gate exists to prevent, introduced by the bridge that feeds it.
    if (determinism.backend != BackendKind::Unknown) {
        pinning.backend = Name::intern(backend_kind_name(determinism.backend));
    }
    if (determinism.precision != Precision::Unknown) {
        pinning.precision = Name::intern(precision_name(determinism.precision));
    }
    pinning.verified_configuration = determinism.verified_configuration;
    return pinning;
}

u64 cook_configuration(BackendKind backend, Precision precision, std::string_view device) noexcept {
    return configuration_digest(backend, precision, device);
}

}  // namespace cy::ml

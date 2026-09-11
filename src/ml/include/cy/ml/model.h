#pragma once
// The model asset. M8.c tasks 4.1 and 4.2.
//
// `ml-inference`: "A trained model SHALL be imported as a **model asset** carrying: the model
// payload, its input and output tensor specifications ..., the backends it has been validated
// against, a declared precision, and a **determinism classification**."
//
// Six things, and every one of them is a field below rather than a convention. The determinism
// classification is the one that earns the asset its own type: without it "pinned" is a claim
// somebody makes at a call site, and `ml-inference` requires it to be a property of the ASSET that
// a cook can read —
//
//   "Model output SHALL NOT drive authoritative gameplay state ... unless the session is
//    **pinned**: a fixed backend, fixed precision, and a configuration the model asset declares as
//    verified reproducible."
//
// THE CONTAINER. `write_model_asset`/`read_model_asset` are the engine-owned wrapper the cook
// produces and the runtime loads: a header, the declared specifications, and the payload, so that
// what a build ships is the asset rather than a bare `.onnx` beside a promise about it. The payload
// is opaque here — CyberML never parses a model, which is `ml-inference`'s "The engine SHALL NOT
// implement a neural network runtime" applied to the loader as well as to the executor.
//
// The three determinism fields are deliberately field-identical to `cy::gameplay::ModelPinning`
// minus its `model` name, so the cook-time bridge is a copy rather than a translation. See
// `<cy/ml/cook.h>` and `<cy/gameplay/cook_firewall.h>`; the firewall's declarations carry no
// dependency on any inference type on purpose, and this file does not undo that by making them
// depend on one.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/ml/tensor.h>

#include <string_view>

namespace cy::ml {

/// The backends `ml-inference` names: "**ONNX Runtime** (portable default), **Core ML** (Apple
/// platforms), **DirectML** (Windows), and **TensorRT** (NVIDIA)".
///
/// All four are enumerated even though only one is implemented, because a model asset declares the
/// backends it was VALIDATED against and that list is authored on a machine that has none of them.
/// An enumerator with no backend behind it makes a cook refuse rather than a cook guess.
enum class BackendKind : u8 {
    Unknown = 0,
    OnnxRuntime = 1,
    CoreML = 2,
    DirectML = 3,
    TensorRT = 4,
    Count = 5,
};

[[nodiscard]] const char* backend_kind_name(BackendKind kind) noexcept;
[[nodiscard]] BackendKind backend_kind_from_name(std::string_view text) noexcept;

/// The precision a model declares. `Mixed` is a real answer, not a missing one: a quantised network
/// with float normalisation layers is mixed, and calling it `F32` would make the pinning claim
/// false.
enum class Precision : u8 {
    Unknown = 0,
    F32 = 1,
    F16 = 2,
    I8 = 3,
    Mixed = 4,
    Count = 5,
};

[[nodiscard]] const char* precision_name(Precision precision) noexcept;
[[nodiscard]] Precision precision_from_name(std::string_view text) noexcept;

/// What a model asset declares about its own reproducibility.
///
/// `verified_configuration` is zero when the asset declares none. Zero is the honest encoding of
/// "never verified" for the reason `cy::gameplay::ModelPinning` gives at its own field: a digest a
/// hash produced is not zero, so "unverified" and "verified against configuration zero" cannot be
/// confused.
struct ModelDeterminism {
    BackendKind backend = BackendKind::Unknown;
    Precision precision = Precision::Unknown;
    u64 verified_configuration = 0;

    /// All three, because `ml-inference` requires all three. A backend without a precision is a
    /// half-made claim and this is where it is refused rather than at the call site.
    [[nodiscard]] bool pinned() const noexcept {
        return verified_configuration != 0 && backend != BackendKind::Unknown &&
               precision != Precision::Unknown;
    }
};

/// The digest both sides of the cook-time gate compare: what a model declares verified, and what a
/// cook is producing. A pure function of the three things that make an inference result
/// reproducible — `ml-inference`'s "a fixed backend, fixed precision, and a configuration".
///
/// `device` is the placement token — "cpu", "cuda:0", "ane" — because two devices running the same
/// backend at the same precision are not the same configuration, which is exactly the case the
/// requirement's "devices, or driver versions" clause is about.
[[nodiscard]] u64 configuration_digest(BackendKind backend, Precision precision,
                                       std::string_view device) noexcept;

/// The largest number of declared inputs, outputs, and validated backends an asset carries.
/// Fixed and small: these are authored counts, and a model with more than sixteen named inputs is
/// a research artefact rather than something a frame budget runs.
inline constexpr u32 kMaxModelTensors = 16;

/// A cooked model.
///
/// Owns its payload. `ml-inference` requires models to be "cooked per target platform, converted to
/// the backend-native form where the backend requires it, and content-addressed like any other
/// asset" — the conversion belongs to the cook, and what this type owes is that the bytes it holds
/// are the ones the target's backend loads, and that `content_hash()` identifies them.
class ModelAsset {
public:
    explicit ModelAsset(Allocator& allocator) noexcept;

    ModelAsset(const ModelAsset&) = delete;
    ModelAsset& operator=(const ModelAsset&) = delete;
    ModelAsset(ModelAsset&&) noexcept = default;
    ModelAsset& operator=(ModelAsset&&) noexcept = default;

    /// Take a copy of `payload`. The asset owns its bytes because a session outlives whatever
    /// buffer a loader read the file into.
    [[nodiscard]] static Expected<ModelAsset, Error> create(Allocator& allocator, Name name,
                                                            Span<const u8> payload) noexcept;

    [[nodiscard]] Status declare_input(const TensorSpec& spec) noexcept;
    [[nodiscard]] Status declare_output(const TensorSpec& spec) noexcept;
    [[nodiscard]] Status declare_validated_backend(BackendKind kind) noexcept;

    void set_precision(Precision precision) noexcept { precision_ = precision; }
    void set_determinism(const ModelDeterminism& determinism) noexcept {
        determinism_ = determinism;
    }

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] Span<const u8> payload() const noexcept { return payload_.span(); }
    [[nodiscard]] Span<const TensorSpec> inputs() const noexcept { return inputs_.span(); }
    [[nodiscard]] Span<const TensorSpec> outputs() const noexcept { return outputs_.span(); }
    [[nodiscard]] Span<const BackendKind> validated_backends() const noexcept {
        return validated_.span();
    }
    [[nodiscard]] Precision precision() const noexcept { return precision_; }
    [[nodiscard]] const ModelDeterminism& determinism() const noexcept { return determinism_; }

    /// Content addressing, over the payload and everything declared about it. Two assets with the
    /// same bytes and different declared precisions are different assets, because they cook and run
    /// differently.
    [[nodiscard]] u64 content_hash() const noexcept;

    /// `ml-inference`'s "filtered ... by the model's validated-backend list". An asset that
    /// declares no validated backend accepts none: an empty list is a model nobody checked, and
    /// treating it as "any" would make the automatic selection a guess.
    [[nodiscard]] bool validated_for(BackendKind kind) const noexcept;

    /// The input specification with this name, or null.
    [[nodiscard]] const TensorSpec* input(Name name) const noexcept;
    [[nodiscard]] const TensorSpec* output(Name name) const noexcept;

private:
    /// The container's reader fills `name_` and `payload_` after it has read the declarations, so
    /// it is a friend rather than a caller of `create` — which would have to be handed the payload
    /// first and would then read the file twice.
    friend Expected<ModelAsset, Error> read_model_asset(Allocator& allocator,
                                                        Span<const u8> bytes) noexcept;

    Name name_;
    Array<u8> payload_;
    Array<TensorSpec> inputs_;
    Array<TensorSpec> outputs_;
    Array<BackendKind> validated_;
    Precision precision_ = Precision::Unknown;
    ModelDeterminism determinism_;
};

/// The container's magic and version. `CYML` and 1: a loader that reads a different version says so
/// rather than reading a struct that has moved.
inline constexpr u32 kModelAssetMagic = 0x4C4D5943;  // 'CYML' little-endian
inline constexpr u32 kModelAssetVersion = 1;

/// Serialise an asset into `out`, appending. The inverse of `read_model_asset`.
[[nodiscard]] Status write_model_asset(const ModelAsset& asset, Array<u8>& out) noexcept;

/// Read an asset. Fails with a named reason on a bad magic, an unknown version, or a truncation —
/// never by reading past the end of `bytes`.
[[nodiscard]] Expected<ModelAsset, Error> read_model_asset(Allocator& allocator,
                                                           Span<const u8> bytes) noexcept;

}  // namespace cy::ml

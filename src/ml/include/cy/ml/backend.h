#pragma once
// The backend abstraction, and the one place a backend is chosen. M8.c tasks 4.1 and 4.1b.
//
// `ml-inference`: "CyberML SHALL define an `InferenceBackend` interface, with the engine shipping
// backends for: **ONNX Runtime** (portable default), **Core ML**, **DirectML**, and **TensorRT**,
// each optional and capability-gated", and "Backend selection SHALL be: explicit, or automatic by a
// declared preference order filtered by availability and by the model's validated-backend list."
//
// ================================================================================================
// WHERE A RUNTIME'S TYPES MAY APPEAR, AND HOW THAT IS CHECKED
// ================================================================================================
//
// "WHEN engine or game code is compiled THEN no inference runtime type SHALL appear outside its
// backend module."
//
// The backend module here is ONE TRANSLATION UNIT — src/ml/src/onnxruntime_backend.cpp — which is
// the same arrangement `cy::audio`'s Steam Audio backend and `cy::physics`'s Jolt backend have, and
// for the same reason: an interface that exists only in some configurations has a suite that runs
// only in some configurations. Nothing in this header names an ONNX Runtime type, the dependency is
// linked PRIVATE so its include directories are not inherited, and tools/layercheck/layercheck.py
// carries the rule as a gate — an `onnxruntime_cxx_api.h` include anywhere else in the tree is a
// build failure rather than a review comment.
//
// `create_onnxruntime_backend` is declared here and NOT behind `#if defined(CY_ML_ONNXRUNTIME)`,
// deliberately. With the option off it returns `Unavailable` with the configure line in the
// message, so a caller takes its declared fallback — `ml-inference`'s "the caller SHALL take its
// declared fallback" — rather than failing to compile. A header that changes shape with an option
// is a header two configurations disagree about.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/ml/model.h>
#include <cy/ml/tensor.h>

namespace cy::ml {

/// What a backend can do. `ml-inference`: "Backends SHALL report their capabilities — supported
/// operators, precisions, device placement, and whether execution is deterministic — and the engine
/// SHALL surface these."
///
/// Operators are not a list here: the set is in the hundreds and changes per version, so the
/// question is asked per model through `InferenceBackend::validate`, which is the form the
/// requirement's own scenario needs ("cooking for that platform SHALL fail with the operator
/// named").
struct BackendCapabilities {
    BackendKind kind = BackendKind::Unknown;
    /// A bit per `Precision` enumerator.
    u32 precisions = 0;
    /// Can a session be placed on a named device other than the CPU?
    bool device_placement = false;
    /// Can a session read a caller's tensor memory without copying it?
    bool zero_copy = false;
    /// **Whether execution is deterministic**, as the backend claims it for its own default
    /// configuration. Reported and never trusted on its own: `ml-inference` requires inference to
    /// be "treated as non-deterministic across backends, devices, and driver versions unless
    /// explicitly proven otherwise", and the proof is the model asset's pinned configuration, not
    /// this bit.
    bool deterministic_execution = false;

    [[nodiscard]] bool supports(Precision precision) const noexcept {
        return (precisions & (1u << static_cast<u32>(precision))) != 0;
    }
};

[[nodiscard]] constexpr u32 precision_bit(Precision precision) noexcept {
    return 1u << static_cast<u32>(precision);
}

/// An operator a backend cannot run, named. Fixed-size text because this crosses no allocator and
/// is read in a diagnostic.
struct UnsupportedOperator {
    static constexpr usize kMaxName = 48;
    char name[kMaxName] = {};
};

/// How a session is to be created.
struct SessionDesc {
    /// Explicit backend selection. `Unknown` means automatic — see `select_backend`.
    BackendKind backend = BackendKind::Unknown;
    /// The device placement token: "cpu", "cuda:0", "ane". Part of the configuration digest, so it
    /// is part of what "pinned" means.
    const char* device = "cpu";
    /// The largest batch the session will be asked for. A session that reuses its allocations has
    /// to know the biggest shape it will see; `ml-inference`'s "reuse across invocations without
    /// reallocation" is otherwise a hope.
    i64 max_batch = 1;
    /// How many threads the backend may use inside one call. One by default: a frame's inference
    /// runs on a job worker, and a backend that spawns its own pool underneath the engine's is the
    /// oversubscription `core-jobs-and-concurrency` exists to prevent.
    u32 intra_op_threads = 1;
};

/// A backend's live session over one model. Created and destroyed by its `InferenceBackend`.
class BackendSession {
public:
    virtual ~BackendSession() = default;

    /// Run once. `inputs` and `outputs` are in the model asset's declared order. Outputs are
    /// written in place — the caller owns them, which is what makes reuse across invocations the
    /// default rather than an optimisation.
    [[nodiscard]] virtual Status run(Span<Tensor* const> inputs,
                                     Span<Tensor* const> outputs) noexcept = 0;

    /// How many bytes this session holds. `ml-inference`'s diagnostics require "memory consumed"
    /// per model, and a backend that cannot answer reports zero rather than a guess.
    [[nodiscard]] virtual u64 memory_bytes() const noexcept { return 0; }
};

/// A runtime CyberML can execute a model through.
class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;

    [[nodiscard]] virtual const char* backend_name() const noexcept = 0;
    [[nodiscard]] virtual BackendKind kind() const noexcept = 0;
    [[nodiscard]] virtual BackendCapabilities capabilities() const noexcept = 0;

    /// COOK TIME. Load the model far enough to answer "can this backend run it?", appending every
    /// operator it cannot to `unsupported`. `ml-inference`: "Import SHALL validate the model and
    /// report unsupported operators for each target backend at cook time, rather than at first
    /// inference."
    [[nodiscard]] virtual Status validate(const ModelAsset& asset,
                                          Array<UnsupportedOperator>& unsupported) noexcept = 0;

    [[nodiscard]] virtual Expected<BackendSession*, Error> create_session(
        Allocator& allocator, const ModelAsset& asset, const SessionDesc& desc) noexcept = 0;

    virtual void destroy_session(BackendSession* session, Allocator& allocator) noexcept = 0;
};

/// The backends this process has. Fixed capacity: there are four in the specification and there
/// will not be a fifth without a change to it.
class BackendRegistry {
public:
    static constexpr u32 kMaxBackends = static_cast<u32>(BackendKind::Count);

    [[nodiscard]] Status add(InferenceBackend* backend) noexcept;
    void clear() noexcept { count_ = 0; }

    [[nodiscard]] u32 size() const noexcept { return count_; }
    [[nodiscard]] InferenceBackend* at(u32 index) const noexcept {
        return (index < count_) ? backends_[index] : nullptr;
    }
    [[nodiscard]] InferenceBackend* find(BackendKind kind) const noexcept;

private:
    InferenceBackend* backends_[kMaxBackends] = {};
    u32 count_ = 0;
};

/// Why a selection failed, or how it succeeded. Reported rather than logged, because
/// `ml-inference` requires that "the choice SHALL be reported" and that a failure "SHALL fail with
/// a diagnostic naming the reason".
enum class SelectionOutcome : u8 {
    Explicit = 0,
    Automatic = 1,
    /// The explicitly named backend is not in the registry.
    RequestedBackendAbsent = 2,
    /// A backend is present but the model does not declare it validated.
    NotValidatedForModel = 3,
    /// No backend at all is registered — a build with every ML option off.
    NoBackendRegistered = 4,
    Count = 5,
};

[[nodiscard]] const char* selection_outcome_name(SelectionOutcome outcome) noexcept;

struct SelectionReport {
    SelectionOutcome outcome = SelectionOutcome::NoBackendRegistered;
    BackendKind chosen = BackendKind::Unknown;
    /// How many registered backends were rejected because the model does not list them. Counted so
    /// that "no backend available" can distinguish an empty registry from a model nobody validated.
    u32 rejected_unvalidated = 0;
};

/// `ml-inference`'s declared preference order, highest first. The specification calls ONNX Runtime
/// the "portable default" and the other three platform-specific, so a platform backend that is
/// present and validated wins — it is the one tuned for the device — and ONNX Runtime is the one
/// that is always there.
inline constexpr BackendKind kPreferenceOrder[] = {
    BackendKind::TensorRT,
    BackendKind::CoreML,
    BackendKind::DirectML,
    BackendKind::OnnxRuntime,
};

/// Choose a backend for `asset`. Explicit when `desc.backend` names one; otherwise the
/// highest-preference registered backend the asset declares validated.
[[nodiscard]] InferenceBackend* select_backend(const BackendRegistry& registry,
                                               const ModelAsset& asset, const SessionDesc& desc,
                                               SelectionReport& report) noexcept;

// --- The reference backend ----------------------------------------------------------------------

/// Is ONNX Runtime compiled into this build? The build question; `BackendCapabilities` answers the
/// runtime one.
[[nodiscard]] bool onnxruntime_compiled_in() noexcept;

/// The ONNX Runtime backend, or `Unavailable` when this build has none.
///
/// Not an error a caller has to handle specially: it takes its declared fallback and the game
/// behaves identically without a model, which is what makes CyberML optional.
[[nodiscard]] Expected<InferenceBackend*, Error> create_onnxruntime_backend(
    Allocator& allocator) noexcept;

void destroy_onnxruntime_backend(InferenceBackend* backend, Allocator& allocator) noexcept;

}  // namespace cy::ml

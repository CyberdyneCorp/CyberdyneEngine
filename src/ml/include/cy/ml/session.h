#pragma once
// The inference session, and the runtime half of the determinism boundary. M8.c tasks 4.1 and 4.2.
//
// `ml-inference`: "an `InferenceSession` created from a model asset and a backend selection.
// Sessions SHALL support: synchronous execution, asynchronous execution with deterministic
// completion semantics as declared by the caller, batched execution of multiple inputs in one call,
// and reuse across invocations without reallocation."
//
// ================================================================================================
// WHY A SESSION KNOWS WHAT `WriteOrigin` IT IS, AND WHY THAT IS THE POINT OF THE TYPE
// ================================================================================================
//
// M8.c's firewall is enforced at the ECS write path and reads a THREAD-LOCAL origin
// (`<cy/ecs/firewall.h>`). A session is where "pinned" stops being a claim in an asset and becomes
// a runtime fact, so the session owns the origin:
//
//     WriteOrigin::PinnedInference   the model declares a verified-reproducible configuration AND
//                                    this session is running that exact configuration
//     WriteOrigin::Inference         anything else
//
// and `ResultScope` is how a caller opens it around the code that consumes a result. The two enum
// values differ in exactly one way — `origin_may_write_authoritative` is true for the first — so a
// non-pinned model's output reaching a replicated or physics-owned component is refused at the
// write rather than diagnosed after it.
//
// **This is the runtime half and it is NOT the requirement.** `ml-inference` requires the mismatch
// to be caught at COOK time ("Multiplayer desync is prevented at cook time"), which is
// `<cy/ml/cook.h>` over `<cy/gameplay/cook_firewall.h>`. The runtime half exists because a cook
// gate cannot see a model a developer wired up in a live-edit session, and because the requirement
// asks for both: "Development builds SHALL report attempts to use non-pinned inference output in a
// replicated or physics-owned write."

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/firewall.h>
#include <cy/ml/backend.h>
#include <cy/ml/model.h>
#include <cy/ml/tensor.h>

namespace cy::ml {

/// What one run cost and what it did. `ml-inference`'s diagnostics requirement asks for
/// "invocation count, wall-clock and device time per invocation, batch sizes achieved, backend in
/// use, memory consumed, staleness of asynchronous results, and budget utilisation"; the per-model
/// totals are here and the budget half is in `<cy/ml/schedule.h>`.
struct SessionStats {
    u64 invocations = 0;
    u64 total_nanoseconds = 0;
    u64 last_nanoseconds = 0;
    u64 largest_batch = 0;
    u64 total_batch_rows = 0;
    /// How many times a run allocated. A session that reuses its allocations reports the number it
    /// reached on its first run and never moves it again.
    u32 allocations = 0;

    [[nodiscard]] u64 mean_nanoseconds() const noexcept {
        return (invocations == 0) ? 0 : total_nanoseconds / invocations;
    }
    [[nodiscard]] f64 mean_batch() const noexcept {
        return (invocations == 0)
                   ? 0.0
                   : static_cast<f64>(total_batch_rows) / static_cast<f64>(invocations);
    }
};

/// A live model. Owns its input and output tensors so that "reuse across invocations without
/// reallocation" is the default path rather than something a caller has to arrange.
class InferenceSession {
public:
    InferenceSession() noexcept = default;
    ~InferenceSession();

    InferenceSession(const InferenceSession&) = delete;
    InferenceSession& operator=(const InferenceSession&) = delete;
    InferenceSession(InferenceSession&& other) noexcept;
    InferenceSession& operator=(InferenceSession&& other) noexcept;

    /// Create over `asset`, choosing a backend from `registry` per `desc`.
    ///
    /// The asset must outlive the session: it owns the payload the backend reads and, in a backend
    /// that maps rather than copies it, the memory the graph points at.
    ///
    /// Fails with `Unavailable` and a diagnostic naming the reason when no backend can run the
    /// model — `ml-inference`'s "session creation SHALL fail with a diagnostic naming the reason,
    /// and the caller SHALL take its declared fallback".
    [[nodiscard]] static Expected<InferenceSession, Error> create(Allocator& allocator,
                                                                  const BackendRegistry& registry,
                                                                  const ModelAsset& asset,
                                                                  const SessionDesc& desc,
                                                                  SelectionReport& report) noexcept;

    /// Run over the tensors the session owns. The caller fills `input(i)` and reads `output(i)`.
    [[nodiscard]] Status run() noexcept;

    /// Run with a batch of `rows`. Reshapes the owned tensors' leading dimension, which is the
    /// dimension the model declared dynamic, and fails without running when the model declared it
    /// fixed — a batch a model cannot take is a caller error, not a slower path.
    ///
    /// `ml-inference`: "WHEN many agents require the same model's inference in one tick THEN their
    /// inputs SHALL be batchable into one session call rather than one call per agent."
    [[nodiscard]] Status run_batch(i64 rows) noexcept;

    [[nodiscard]] Tensor* input(usize index) noexcept;
    [[nodiscard]] Tensor* output(usize index) noexcept;
    [[nodiscard]] Tensor* input(Name name) noexcept;
    [[nodiscard]] Tensor* output(Name name) noexcept;
    [[nodiscard]] usize input_count() const noexcept { return inputs_.size(); }
    [[nodiscard]] usize output_count() const noexcept { return outputs_.size(); }

    /// **Is this session pinned?** True when the model declares a verified-reproducible
    /// configuration and this session is running it: same backend, same precision, same device.
    /// Nothing else. A session over a pinned asset running on another device is NOT pinned, which
    /// is the case the requirement's "devices, or driver versions" clause exists for.
    [[nodiscard]] bool is_pinned() const noexcept { return pinned_; }

    /// The origin an ECS write from this session's result carries.
    [[nodiscard]] ecs::WriteOrigin write_origin() const noexcept {
        return pinned_ ? ecs::WriteOrigin::PinnedInference : ecs::WriteOrigin::Inference;
    }

    [[nodiscard]] BackendKind backend() const noexcept { return backend_kind_; }
    [[nodiscard]] const char* backend_name() const noexcept { return backend_name_; }
    [[nodiscard]] u64 configuration() const noexcept { return configuration_; }
    [[nodiscard]] const SessionStats& stats() const noexcept { return stats_; }
    [[nodiscard]] u64 memory_bytes() const noexcept;
    [[nodiscard]] Name model_name() const noexcept { return model_; }

private:
    void release() noexcept;
    /// The batch this session is currently shaped for.
    [[nodiscard]] u64 leading_rows() const noexcept;

    Allocator* allocator_ = nullptr;
    InferenceBackend* backend_ = nullptr;
    BackendSession* session_ = nullptr;
    Array<Tensor> inputs_;
    Array<Tensor> outputs_;
    Array<Name> input_names_;
    Array<Name> output_names_;
    Array<Tensor*> input_pointers_;
    Array<Tensor*> output_pointers_;
    SessionStats stats_;
    Name model_;
    const char* backend_name_ = "none";
    BackendKind backend_kind_ = BackendKind::Unknown;
    u64 configuration_ = 0;
    bool pinned_ = false;
};

/// Open the session's write origin around the code that consumes its result.
///
///     {
///         const cy::ml::ResultScope scope(session, "ai.threat-classifier");
///         world.add(entity, ThreatLevel{...});   // refused unless the session is pinned
///     }
///
/// A scope rather than a check, because `<cy/ecs/firewall.h>`'s enforcement point is the ECS write
/// path and the whole design of it is that a producer never asks for permission and then writes
/// anyway.
class ResultScope {
public:
    ResultScope(const InferenceSession& session, const char* writer) noexcept
        : scope_(session.write_origin(), writer) {}

    ResultScope(const ResultScope&) = delete;
    ResultScope& operator=(const ResultScope&) = delete;
    ResultScope(ResultScope&&) = delete;
    ResultScope& operator=(ResultScope&&) = delete;

private:
    ecs::WriteScope scope_;
};

}  // namespace cy::ml

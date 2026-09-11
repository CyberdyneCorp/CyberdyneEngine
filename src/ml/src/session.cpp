// The session: creation, the pinning decision, and the two run paths. M8.c tasks 4.1 and 4.2.

#include <cy/ml/session.h>

#include <cy/core/jobs/types.h>

#include <algorithm>

namespace cy::ml {
namespace {

/// ONE CLOCK. `cy::jobs::monotonic_now_ns` is the reading the scheduler, the watchdog and the
/// critical-path report already share, and its own header says why: three subsystems that each took
/// their own reading cannot agree about what "now" was.
[[nodiscard]] u64 monotonic_nanoseconds() noexcept {
    return static_cast<u64>(jobs::monotonic_now_ns());
}

/// Resolve a declared specification into the concrete shape a session allocates at.
///
/// The LEADING dimension is the batch and may be dynamic; every other dynamic dimension is refused.
/// That is a real restriction and it is stated rather than worked around: a model whose *width* is
/// dynamic cannot be given a fixed allocation, and `ml-inference`'s reuse requirement — "it SHALL
/// reuse its allocations rather than allocating per invocation" — is exactly the thing such a model
/// makes impossible.
[[nodiscard]] Expected<TensorShape, Error> concrete_shape(const TensorSpec& spec,
                                                          i64 batch) noexcept {
    TensorShape shape = spec.shape;
    if (shape.rank == 0) {
        return fail(ErrorCode::InvalidArgument, "a declared tensor has at least one dimension");
    }
    for (u32 index = 0; index < shape.rank; ++index) {
        if (shape.dimensions[index] != TensorShape::kDynamic) {
            continue;
        }
        if (index != 0) {
            return fail(ErrorCode::Unsupported,
                        "only the leading dimension may be dynamic: a session allocates once and a "
                        "model whose width changes per call cannot be given a fixed allocation",
                        static_cast<i64>(index));
        }
        shape.dimensions[0] = batch;
    }
    return shape;
}

/// Set a tensor's leading dimension. A rank-zero tensor has none and is left alone rather than
/// refused: a model may declare a scalar beside a batched input.
[[nodiscard]] Status reshape_leading(Tensor& tensor, i64 rows) noexcept {
    TensorShape shape = tensor.shape();
    if (shape.rank == 0) {
        return ok();
    }
    shape.dimensions[0] = rows;
    return tensor.reshape(shape);
}

}  // namespace

InferenceSession::~InferenceSession() {
    release();
}

InferenceSession::InferenceSession(InferenceSession&& other) noexcept
    : allocator_(other.allocator_),
      backend_(other.backend_),
      session_(other.session_),
      inputs_(std::move(other.inputs_)),
      outputs_(std::move(other.outputs_)),
      input_names_(std::move(other.input_names_)),
      output_names_(std::move(other.output_names_)),
      input_pointers_(std::move(other.input_pointers_)),
      output_pointers_(std::move(other.output_pointers_)),
      stats_(other.stats_),
      model_(other.model_),
      backend_name_(other.backend_name_),
      backend_kind_(other.backend_kind_),
      configuration_(other.configuration_),
      pinned_(other.pinned_) {
    other.allocator_ = nullptr;
    other.backend_ = nullptr;
    other.session_ = nullptr;
}

InferenceSession& InferenceSession::operator=(InferenceSession&& other) noexcept {
    if (this != &other) {
        release();
        allocator_ = other.allocator_;
        backend_ = other.backend_;
        session_ = other.session_;
        inputs_ = std::move(other.inputs_);
        outputs_ = std::move(other.outputs_);
        input_names_ = std::move(other.input_names_);
        output_names_ = std::move(other.output_names_);
        input_pointers_ = std::move(other.input_pointers_);
        output_pointers_ = std::move(other.output_pointers_);
        stats_ = other.stats_;
        model_ = other.model_;
        backend_name_ = other.backend_name_;
        backend_kind_ = other.backend_kind_;
        configuration_ = other.configuration_;
        pinned_ = other.pinned_;
        other.allocator_ = nullptr;
        other.backend_ = nullptr;
        other.session_ = nullptr;
    }
    return *this;
}

void InferenceSession::release() noexcept {
    if (backend_ != nullptr && session_ != nullptr && allocator_ != nullptr) {
        backend_->destroy_session(session_, *allocator_);
    }
    session_ = nullptr;
    backend_ = nullptr;
}

Expected<InferenceSession, Error> InferenceSession::create(Allocator& allocator,
                                                           const BackendRegistry& registry,
                                                           const ModelAsset& asset,
                                                           const SessionDesc& desc,
                                                           SelectionReport& report) noexcept {
    InferenceBackend* backend = select_backend(registry, asset, desc, report);
    if (backend == nullptr) {
        // "session creation SHALL fail with a diagnostic naming the reason, and the caller SHALL
        // take its declared fallback". The reason is in `report.outcome`, and the message names it
        // so a caller that logs the error alone still learns which of the three it was.
        switch (report.outcome) {
            case SelectionOutcome::RequestedBackendAbsent:
                return fail(ErrorCode::Unavailable,
                            "the backend this session asked for is not in this build");
            case SelectionOutcome::NotValidatedForModel:
                return fail(ErrorCode::Unavailable,
                            "no registered backend is one this model declares validated",
                            static_cast<i64>(report.rejected_unvalidated));
            default:
                return fail(ErrorCode::Unavailable,
                            "this build registered no inference backend: configure with "
                            "-DCY_ML_ONNXRUNTIME=ON, or take the declared fallback");
        }
    }
    if (desc.max_batch < 1) {
        return fail(ErrorCode::InvalidArgument, "a session runs at least one row per call");
    }

    InferenceSession session;
    session.allocator_ = &allocator;
    session.backend_ = backend;
    session.model_ = asset.name();
    session.backend_kind_ = backend->kind();
    session.backend_name_ = backend->backend_name();

    // THE PINNING DECISION, and it is a comparison of two digests rather than a flag.
    //
    // `configuration_digest` covers the backend, the precision and the device placement — the three
    // things `ml-inference` says make a configuration — so a session over a pinned asset that is
    // running on a different device or through a different backend produces a different digest and
    // is NOT pinned. That is the "across backends, devices, and driver versions" clause enforced by
    // arithmetic instead of by trust.
    session.configuration_ = configuration_digest(backend->kind(), asset.precision(), desc.device);
    const ModelDeterminism& determinism = asset.determinism();
    session.pinned_ = determinism.pinned() && determinism.backend == backend->kind() &&
                      determinism.verified_configuration == session.configuration_;

    if (Status reserved = session.inputs_.reserve(asset.inputs().size()); !reserved) {
        return make_unexpected(reserved.error());
    }
    if (Status reserved = session.outputs_.reserve(asset.outputs().size()); !reserved) {
        return make_unexpected(reserved.error());
    }

    for (const TensorSpec& spec : asset.inputs()) {
        Expected<TensorShape, Error> shape = concrete_shape(spec, desc.max_batch);
        if (!shape) {
            return make_unexpected(shape.error());
        }
        Expected<Tensor, Error> tensor = Tensor::allocate(allocator, spec.type, shape.value());
        if (!tensor) {
            return make_unexpected(tensor.error());
        }
        if (Status pushed = session.inputs_.push_back(std::move(tensor).value()); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = session.input_names_.push_back(spec.name); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    for (const TensorSpec& spec : asset.outputs()) {
        Expected<TensorShape, Error> shape = concrete_shape(spec, desc.max_batch);
        if (!shape) {
            return make_unexpected(shape.error());
        }
        Expected<Tensor, Error> tensor = Tensor::allocate(allocator, spec.type, shape.value());
        if (!tensor) {
            return make_unexpected(tensor.error());
        }
        if (Status pushed = session.outputs_.push_back(std::move(tensor).value()); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = session.output_names_.push_back(spec.name); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    // The pointer views the backend is handed. Built after every tensor is in place, because an
    // Array that grows moves its elements and a pointer taken before the last push_back would name
    // a tensor that has moved.
    for (usize index = 0; index < session.inputs_.size(); ++index) {
        if (Status pushed = session.input_pointers_.push_back(&session.inputs_[index]); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    for (usize index = 0; index < session.outputs_.size(); ++index) {
        if (Status pushed = session.output_pointers_.push_back(&session.outputs_[index]); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    Expected<BackendSession*, Error> created = backend->create_session(allocator, asset, desc);
    if (!created) {
        return make_unexpected(created.error());
    }
    session.session_ = created.value();

    for (const Tensor& tensor : session.inputs_.span()) {
        session.stats_.allocations += tensor.allocations();
    }
    for (const Tensor& tensor : session.outputs_.span()) {
        session.stats_.allocations += tensor.allocations();
    }
    return session;
}

Status InferenceSession::run() noexcept {
    if (session_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "this session was moved from or never created");
    }
    const u64 started = monotonic_nanoseconds();
    Status status = session_->run(input_pointers_.span(), output_pointers_.span());
    const u64 elapsed = monotonic_nanoseconds() - started;

    ++stats_.invocations;
    stats_.last_nanoseconds = elapsed;
    stats_.total_nanoseconds += elapsed;
    stats_.total_batch_rows += leading_rows();
    stats_.largest_batch = std::max(leading_rows(), stats_.largest_batch);
    return status;
}

/// The batch this call is running: the leading dimension of the first input, or one when the model
/// takes none. Reported rather than assumed, because `SessionStats::largest_batch` is a
/// measurement.
u64 InferenceSession::leading_rows() const noexcept {
    if (inputs_.empty() || inputs_[0].shape().rank == 0) {
        return 1;
    }
    return static_cast<u64>(inputs_[0].shape().dimensions[0]);
}

Status InferenceSession::run_batch(i64 rows) noexcept {
    if (rows < 1) {
        return fail(ErrorCode::InvalidArgument, "a batch has at least one row");
    }
    for (Tensor& tensor : inputs_.span()) {
        if (Status reshaped = reshape_leading(tensor, rows); !reshaped) {
            return reshaped;
        }
    }
    for (Tensor& tensor : outputs_.span()) {
        if (Status reshaped = reshape_leading(tensor, rows); !reshaped) {
            return reshaped;
        }
    }
    return run();
}

Tensor* InferenceSession::input(usize index) noexcept {
    return (index < inputs_.size()) ? &inputs_[index] : nullptr;
}

Tensor* InferenceSession::output(usize index) noexcept {
    return (index < outputs_.size()) ? &outputs_[index] : nullptr;
}

Tensor* InferenceSession::input(Name name) noexcept {
    for (usize index = 0; index < input_names_.size(); ++index) {
        if (input_names_[index] == name) {
            return &inputs_[index];
        }
    }
    return nullptr;
}

Tensor* InferenceSession::output(Name name) noexcept {
    for (usize index = 0; index < output_names_.size(); ++index) {
        if (output_names_[index] == name) {
            return &outputs_[index];
        }
    }
    return nullptr;
}

u64 InferenceSession::memory_bytes() const noexcept {
    u64 total = (session_ != nullptr) ? session_->memory_bytes() : 0;
    for (const Tensor& tensor : inputs_.span()) {
        total += tensor.capacity_bytes();
    }
    for (const Tensor& tensor : outputs_.span()) {
        total += tensor.capacity_bytes();
    }
    return total;
}

}  // namespace cy::ml

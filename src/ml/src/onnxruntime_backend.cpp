// THE ONE TRANSLATION UNIT THAT MAY NAME AN ONNX RUNTIME TYPE. M8.c tasks 4.1b, 4.1c and 4.1d.
//
// `ml-inference`: "no inference runtime type SHALL appear outside its backend module", and
// "CyberML SHALL be removable at build time via `CY_ML`" with "no inference runtime ... fetched,
// built, or linked" when it is off. Both are structural here rather than promised:
//
//   * `onnxruntime_cxx_api.h` is included inside `#if defined(CY_ML_ONNXRUNTIME)` and nowhere else
//     in the repository. tools/layercheck/layercheck.py enforces that as a build gate — the same
//     rule Jolt and miniaudio have — so an include somewhere else fails the check rather than a
//     review.
//   * `cy::dep::onnxruntime` is a PRIVATE dependency of `cy_ml`, so its include directories are not
//     inherited and the include would not resolve above this file even without the gate.
//
// WHAT THE OPTION IS OFF MEANS. `create_onnxruntime_backend` returns `Unavailable` naming the
// configure line. It is not a compile error and not a stub that pretends to run: a caller takes its
// declared fallback and the game behaves identically without a model, which is the whole reason
// CyberML is optional.
//
// ================================================================================================
// THE C++ API, EXCEPTIONS, AND WHY EVERY CALL IS WRAPPED
// ================================================================================================
//
// The engine is compiled with `-fno-exceptions`. ONNX Runtime's C++ header (`Ort::`) reports
// failures by throwing `Ort::Exception`, which under `-fno-exceptions` would terminate — so this
// file uses the **C API** (`OrtApi`), which reports through `OrtStatus*`, and converts each one
// into a `cy::Error`. That is a deliberate cost: the C API is more verbose and it is the only form
// of the library that can be called from a translation unit that cannot catch.

#include <cy/ml/backend.h>
#include <cy/ml/model.h>
#include <cy/ml/tensor.h>

// THE OPTION REACHES THIS FILE THROUGH THIS HEADER AND NOWHERE ELSE, and that is worth stating
// because getting it wrong is silent. cmake/features.cmake does NOT turn a CY_* option into a
// compile definition: it writes `#define CY_ML_ONNXRUNTIME 1` into the generated <cy_features.h>,
// and a translation unit that does not include it sees every `#if defined(CY_...)` as false
// whatever the option says. A `#if` guarding a backend in a file that never includes this header is
// a backend that is never compiled — which is exactly what M8.b's `CY_UI` finding was, one layer
// down.
#include <cy_features.h>

#include <cstring>
#include <new>

#if defined(CY_ML_ONNXRUNTIME)
#    include <onnxruntime_c_api.h>
#endif

namespace cy::ml {

bool onnxruntime_compiled_in() noexcept {
#if defined(CY_ML_ONNXRUNTIME)
    return true;
#else
    return false;
#endif
}

#if defined(CY_ML_ONNXRUNTIME)

namespace {

/// The ORT API table, fetched once. `OrtGetApiBase()` is the only symbol the library exports that
/// is not reached through it.
[[nodiscard]] const OrtApi* ort_api() noexcept {
    static const OrtApi* api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    return api;
}

/// Turn an `OrtStatus*` into a `cy::Error` and release it. Null is success.
///
/// The message is copied into a fixed buffer because `cy::Error::message` is a `const char*` with
/// no ownership and ORT frees its own status: a pointer into a released status is a use-after-free
/// that only shows up in the diagnostic, which is the worst place for one.
[[nodiscard]] Status from_ort(OrtStatus* status, char* buffer, usize capacity) noexcept {
    if (status == nullptr) {
        return ok();
    }
    const OrtApi* api = ort_api();
    const char* message = api->GetErrorMessage(status);
    const auto code = static_cast<i64>(api->GetErrorCode(status));
    if (message != nullptr && capacity > 1) {
        const usize length = std::strlen(message);
        const usize copied = (length < capacity - 1) ? length : capacity - 1;
        std::memcpy(buffer, message, copied);
        buffer[copied] = '\0';
    } else if (capacity > 0) {
        buffer[0] = '\0';
    }
    api->ReleaseStatus(status);
    return make_unexpected(Error{ErrorCode::Internal, buffer, code});
}

/// Discard an `OrtStatus*` a caller genuinely cannot act on — a free that failed inside a
/// destructor. Releases it, so the discard is not also a leak.
void release_status(OrtStatus* status) noexcept {
    if (status != nullptr) {
        ort_api()->ReleaseStatus(status);
    }
}

[[nodiscard]] ONNXTensorElementDataType to_ort_type(ElementType type) noexcept {
    switch (type) {
        case ElementType::F32:
            return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
        case ElementType::F16:
            return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
        case ElementType::I8:
            return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8;
        case ElementType::U8:
            return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;
        case ElementType::I32:
            return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;
        case ElementType::I64:
            return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
        case ElementType::Bool:
            return ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL;
        case ElementType::Unknown:
        case ElementType::Count:
            break;
    }
    return ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
}

/// The largest number of characters an ORT diagnostic is carried in.
constexpr usize kMessageCapacity = 256;

/// One live ORT session over one model.
class OnnxSession final : public BackendSession {
public:
    OnnxSession() noexcept = default;

    ~OnnxSession() override {
        const OrtApi* api = ort_api();
        // `AllocatorFree` returns an `OrtStatus*` and is declared warn_unused_result. There is
        // nothing a destructor can do with a failure to free, and dropping it silently is what the
        // attribute exists to prevent — so the discard is explicit and says why.
        for (usize index = 0; index < name_count_; ++index) {
            if (input_names_[index] != nullptr) {
                release_status(
                    api->AllocatorFree(allocator_, const_cast<char*>(input_names_[index])));
            }
        }
        for (usize index = 0; index < output_name_count_; ++index) {
            if (output_names_[index] != nullptr) {
                release_status(
                    api->AllocatorFree(allocator_, const_cast<char*>(output_names_[index])));
            }
        }
        if (memory_info_ != nullptr) {
            api->ReleaseMemoryInfo(memory_info_);
        }
        if (session_ != nullptr) {
            api->ReleaseSession(session_);
        }
        if (options_ != nullptr) {
            api->ReleaseSessionOptions(options_);
        }
    }

    OnnxSession(const OnnxSession&) = delete;
    OnnxSession& operator=(const OnnxSession&) = delete;

    [[nodiscard]] Status initialize(OrtEnv* environment, const ModelAsset& asset,
                                    const SessionDesc& desc) noexcept {
        const OrtApi* api = ort_api();

        if (Status created =
                from_ort(api->CreateSessionOptions(&options_), message_, kMessageCapacity);
            !created) {
            return created;
        }
        // ONE THREAD INSIDE ONE CALL BY DEFAULT. `SessionDesc::intra_op_threads` says why: a
        // frame's inference runs on a job worker, and a backend that spawns its own pool underneath
        // the engine's is the oversubscription `core-jobs-and-concurrency` exists to prevent.
        if (Status set = from_ort(
                api->SetIntraOpNumThreads(options_, static_cast<int>(desc.intra_op_threads)),
                message_, kMessageCapacity);
            !set) {
            return set;
        }
        if (Status set = from_ort(api->SetSessionGraphOptimizationLevel(options_, ORT_ENABLE_BASIC),
                                  message_, kMessageCapacity);
            !set) {
            return set;
        }

        // FROM THE ASSET'S OWN BYTES, never from a path. The model asset owns the payload and the
        // cook decided what it is; a backend that reloaded the file would be reading something the
        // asset's content hash does not cover.
        if (Status created =
                from_ort(api->CreateSessionFromArray(environment, asset.payload().data(),
                                                     asset.payload().size(), options_, &session_),
                         message_, kMessageCapacity);
            !created) {
            return created;
        }

        if (Status created = from_ort(api->GetAllocatorWithDefaultOptions(&allocator_), message_,
                                      kMessageCapacity);
            !created) {
            return created;
        }
        if (Status created = from_ort(
                api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info_),
                message_, kMessageCapacity);
            !created) {
            return created;
        }

        // THE NAMES ORT ITSELF REPORTS, not the ones the asset declares. The two must agree, and
        // this is where a disagreement is caught: a cooked asset whose declared input names have
        // drifted from the model's is refused at session setup with both counts named, rather than
        // running against a tensor ORT quietly bound to something else.
        size_t input_count = 0;
        size_t output_count = 0;
        if (Status counted = from_ort(api->SessionGetInputCount(session_, &input_count), message_,
                                      kMessageCapacity);
            !counted) {
            return counted;
        }
        if (Status counted = from_ort(api->SessionGetOutputCount(session_, &output_count), message_,
                                      kMessageCapacity);
            !counted) {
            return counted;
        }
        if (input_count != asset.inputs().size() || output_count != asset.outputs().size()) {
            return fail(ErrorCode::InvalidArgument,
                        "the model asset declares a different number of inputs or outputs than the "
                        "model itself has",
                        static_cast<i64>(input_count));
        }
        if (input_count > kMaxModelTensors || output_count > kMaxModelTensors) {
            return fail(ErrorCode::OutOfRange, "this model has more tensors than CyberML carries",
                        static_cast<i64>(input_count));
        }

        for (size_t index = 0; index < input_count; ++index) {
            char* name = nullptr;
            if (Status named =
                    from_ort(api->SessionGetInputName(session_, index, allocator_, &name), message_,
                             kMessageCapacity);
                !named) {
                return named;
            }
            input_names_[index] = name;
            ++name_count_;
        }
        for (size_t index = 0; index < output_count; ++index) {
            char* name = nullptr;
            if (Status named =
                    from_ort(api->SessionGetOutputName(session_, index, allocator_, &name),
                             message_, kMessageCapacity);
                !named) {
                return named;
            }
            output_names_[index] = name;
            ++output_name_count_;
        }
        return ok();
    }

    [[nodiscard]] Status run(Span<Tensor* const> inputs,
                             Span<Tensor* const> outputs) noexcept override {
        const OrtApi* api = ort_api();
        if (inputs.size() != name_count_ || outputs.size() != output_name_count_) {
            return fail(ErrorCode::InvalidArgument,
                        "this session was created for a different number of tensors",
                        static_cast<i64>(inputs.size()));
        }

        // ZERO COPY, both ways. `CreateTensorWithDataAsOrtValue` wraps the caller's memory rather
        // than copying it, and the output values are pre-bound to the caller's tensors too — so an
        // engine tensor that lives in an engine allocator is what ORT reads and writes, which is
        // the "SHALL support zero-copy where the backend permits it" half of the requirement.
        OrtValue* input_values[kMaxModelTensors] = {};
        OrtValue* output_values[kMaxModelTensors] = {};
        Status status = ok();

        for (usize index = 0; index < inputs.size() && status; ++index) {
            status = wrap(inputs[index], memory_info_, &input_values[index]);
        }
        for (usize index = 0; index < outputs.size() && status; ++index) {
            status = wrap(outputs[index], memory_info_, &output_values[index]);
        }
        if (status) {
            status = from_ort(api->Run(session_, nullptr, input_names_, input_values, inputs.size(),
                                       output_names_, outputs.size(), output_values),
                              message_, kMessageCapacity);
        }

        for (OrtValue* value : input_values) {
            if (value != nullptr) {
                api->ReleaseValue(value);
            }
        }
        for (OrtValue* value : output_values) {
            if (value != nullptr) {
                api->ReleaseValue(value);
            }
        }
        return status;
    }

    [[nodiscard]] u64 memory_bytes() const noexcept override {
        // ORT reports arena statistics only through a profiling build; what this session can say
        // honestly is what it holds itself, which is nothing beyond the caller's tensors. Zero is
        // the documented answer for a backend that cannot measure — see `BackendSession`.
        return 0;
    }

private:
    [[nodiscard]] Status wrap(Tensor* tensor, OrtMemoryInfo* memory_info, OrtValue** out) noexcept {
        if (tensor == nullptr || tensor->data() == nullptr) {
            return fail(ErrorCode::InvalidArgument, "a session was handed an empty tensor");
        }
        const ONNXTensorElementDataType type = to_ort_type(tensor->type());
        if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED) {
            return fail(ErrorCode::Unsupported, "ONNX Runtime has no element type for this tensor");
        }
        int64_t shape[kMaxTensorRank] = {};
        for (u32 index = 0; index < tensor->shape().rank; ++index) {
            shape[index] = static_cast<int64_t>(tensor->shape().dimensions[index]);
        }
        return from_ort(ort_api()->CreateTensorWithDataAsOrtValue(memory_info, tensor->data(),
                                                                  tensor->byte_size(), shape,
                                                                  tensor->shape().rank, type, out),
                        message_, kMessageCapacity);
    }

    OrtSessionOptions* options_ = nullptr;
    OrtSession* session_ = nullptr;
    OrtAllocator* allocator_ = nullptr;
    OrtMemoryInfo* memory_info_ = nullptr;
    const char* input_names_[kMaxModelTensors] = {};
    const char* output_names_[kMaxModelTensors] = {};
    usize name_count_ = 0;
    usize output_name_count_ = 0;
    char message_[kMessageCapacity] = {};
};

/// The backend. One `OrtEnv` per backend instance, which is what ORT wants: the environment owns
/// the logging sink and the thread pools, and two of them in one process is a supported but
/// pointless arrangement.
class OnnxRuntimeBackend final : public InferenceBackend {
public:
    OnnxRuntimeBackend() noexcept = default;

    ~OnnxRuntimeBackend() override {
        if (environment_ != nullptr) {
            ort_api()->ReleaseEnv(environment_);
        }
    }

    OnnxRuntimeBackend(const OnnxRuntimeBackend&) = delete;
    OnnxRuntimeBackend& operator=(const OnnxRuntimeBackend&) = delete;

    [[nodiscard]] Status initialize() noexcept {
        return from_ort(
            ort_api()->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "cyberdyne.ml", &environment_),
            message_, kMessageCapacity);
    }

    [[nodiscard]] const char* backend_name() const noexcept override { return "onnxruntime"; }
    [[nodiscard]] BackendKind kind() const noexcept override { return BackendKind::OnnxRuntime; }

    [[nodiscard]] BackendCapabilities capabilities() const noexcept override {
        BackendCapabilities capabilities;
        capabilities.kind = BackendKind::OnnxRuntime;
        capabilities.precisions = precision_bit(Precision::F32) | precision_bit(Precision::F16) |
                                  precision_bit(Precision::I8) | precision_bit(Precision::Mixed);
        // The CPU execution provider only. A device-placed provider — CUDA, DirectML, Core ML — is
        // a separate build of the library and a separate manifest decision; claiming placement this
        // build cannot do would make the capability query useless.
        capabilities.device_placement = false;
        capabilities.zero_copy = true;
        // NOT A CLAIM OF REPRODUCIBILITY ACROSS MACHINES. ORT's CPU provider is deterministic for a
        // given build, instruction set and thread count — and this build is compiled with AVX2 and
        // dispatches on what the CPU reports at run time, so two machines are not guaranteed to
        // agree. `false` is the honest answer and it is what forces a model to declare a pinned
        // configuration before its output may reach authoritative state.
        capabilities.deterministic_execution = false;
        return capabilities;
    }

    [[nodiscard]] Status validate(const ModelAsset& asset,
                                  Array<UnsupportedOperator>& unsupported) noexcept override {
        // COOK TIME. ORT answers "can I run this?" by building the session: an unsupported operator
        // is reported by `CreateSessionFromArray` with the operator named in its message, which is
        // the only place ORT exposes the answer without linking its graph library directly.
        //
        // The message is carried into `unsupported` rather than parsed into an operator name: ORT's
        // wording is "Fatal error: <domain>:<op>(<version>) is not a registered function/op", and a
        // parser over somebody else's diagnostic text is a parser that breaks at their next
        // release.
        OnnxSession probe;
        SessionDesc desc;
        Status initialised = probe.initialize(environment_, asset, desc);
        if (initialised) {
            return ok();
        }
        // THE MESSAGE IS COPIED OUT OF THE PROBE BEFORE THE PROBE DIES. `cy::Error::message` is a
        // non-owning `const char*`, and the probe's own buffer is a member of a local that is about
        // to be destroyed — returning `initialised` unchanged would hand the caller a pointer into
        // a dead stack frame, and it would read correctly right up until something reused it.
        UnsupportedOperator entry;
        adopt_message(initialised.error().message);
        const usize length = std::strlen(message_);
        const usize copied = (length < UnsupportedOperator::kMaxName - 1)
                                 ? length
                                 : UnsupportedOperator::kMaxName - 1;
        if (copied > 0) {
            std::memcpy(entry.name, message_, copied);
        }
        entry.name[copied] = '\0';
        if (Status pushed = unsupported.push_back(entry); !pushed) {
            return pushed;
        }
        return make_unexpected(
            Error{initialised.error().code, message_, initialised.error().system_code});
    }

    [[nodiscard]] Expected<BackendSession*, Error> create_session(
        Allocator& allocator, const ModelAsset& asset, const SessionDesc& desc) noexcept override {
        void* memory = allocator.allocate(sizeof(OnnxSession), alignof(OnnxSession));
        if (memory == nullptr) {
            return fail(ErrorCode::OutOfMemory, "no room for an ONNX Runtime session");
        }
        auto* session = new (memory) OnnxSession();
        if (Status initialised = session->initialize(environment_, asset, desc); !initialised) {
            // Same lifetime rule as `validate`: copy the diagnostic into the BACKEND's buffer
            // before the session that owns the original is destroyed.
            const Error reported{initialised.error().code, nullptr,
                                 initialised.error().system_code};
            adopt_message(initialised.error().message);
            session->~OnnxSession();
            allocator.deallocate(memory, sizeof(OnnxSession), alignof(OnnxSession));
            return make_unexpected(Error{reported.code, message_, reported.system_code});
        }
        return static_cast<BackendSession*>(session);
    }

    void destroy_session(BackendSession* session, Allocator& allocator) noexcept override {
        if (session == nullptr) {
            return;
        }
        // Handed back exactly what create_session made, and -fno-rtti means dynamic_cast does not
        // exist in this tree. Every backend in this engine destroys its own sessions this way.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        auto* onnx = static_cast<OnnxSession*>(session);
        onnx->~OnnxSession();
        allocator.deallocate(static_cast<void*>(onnx), sizeof(OnnxSession), alignof(OnnxSession));
    }

private:
    /// Copy a diagnostic into this backend's own buffer, so the `cy::Error` handed back outlives
    /// whatever produced it. Not thread-safe, and deliberately so: `create_session` and `validate`
    /// are cook-time and load-time calls, and a per-call buffer would put 256 bytes on a path that
    /// runs once.
    void adopt_message(const char* text) noexcept {
        if (text == nullptr) {
            message_[0] = '\0';
            return;
        }
        const usize length = std::strlen(text);
        const usize copied = (length < kMessageCapacity - 1) ? length : kMessageCapacity - 1;
        std::memcpy(message_, text, copied);
        message_[copied] = '\0';
    }

    OrtEnv* environment_ = nullptr;
    char message_[kMessageCapacity] = {};
};

}  // namespace

Expected<InferenceBackend*, Error> create_onnxruntime_backend(Allocator& allocator) noexcept {
    void* memory = allocator.allocate(sizeof(OnnxRuntimeBackend), alignof(OnnxRuntimeBackend));
    if (memory == nullptr) {
        return fail(ErrorCode::OutOfMemory, "no room for the ONNX Runtime backend");
    }
    auto* backend = new (memory) OnnxRuntimeBackend();
    if (Status initialised = backend->initialize(); !initialised) {
        backend->~OnnxRuntimeBackend();
        allocator.deallocate(memory, sizeof(OnnxRuntimeBackend), alignof(OnnxRuntimeBackend));
        return make_unexpected(initialised.error());
    }
    return static_cast<InferenceBackend*>(backend);
}

void destroy_onnxruntime_backend(InferenceBackend* backend, Allocator& allocator) noexcept {
    if (backend == nullptr) {
        return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto* onnx = static_cast<OnnxRuntimeBackend*>(backend);
    onnx->~OnnxRuntimeBackend();
    allocator.deallocate(static_cast<void*>(onnx), sizeof(OnnxRuntimeBackend),
                         alignof(OnnxRuntimeBackend));
}

#else  // CY_ML_ONNXRUNTIME

Expected<InferenceBackend*, Error> create_onnxruntime_backend(Allocator& allocator) noexcept {
    (void)allocator;
    // NOT AN ERROR A CALLER HAS TO HANDLE SPECIALLY, exactly as `cy::audio::create_steam_audio` is
    // not: the caller takes its declared fallback, and the game runs without a model.
    return fail(ErrorCode::Unavailable,
                "this build has no ONNX Runtime: configure with -DCY_ML_ONNXRUNTIME=ON");
}

void destroy_onnxruntime_backend(InferenceBackend* backend, Allocator& allocator) noexcept {
    (void)backend;
    (void)allocator;
}

#endif  // CY_ML_ONNXRUNTIME

}  // namespace cy::ml

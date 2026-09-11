#pragma once
// A backend the suites drive without an inference runtime. M8.c section 4.
//
// NOT A SECOND IMPLEMENTATION OF INFERENCE, and the distinction matters. This backend computes a
// declared arithmetic function of its input — it does not interpret a model, and it reads the
// payload only far enough to prove it was handed one. What it exists to test is everything
// `ml-inference` puts ABOVE a runtime: session creation and its failure modes, the selection rule,
// tensor reuse across invocations, batching, the budget and its deferral, and the pinning decision.
//
// The ONNX Runtime suite (`integration.ml_onnxruntime`) is what tests a real runtime, and it is a
// different suite for a different reason: it exists only when `CY_ML_ONNXRUNTIME` is on, and a
// build without the option must still run everything here.

#include <cy/core/memory/array.h>
#include <cy/ml/backend.h>

namespace cy::ml::test {

/// A session that writes `input * 2 + 1` into its output, element by element, over as many elements
/// as both tensors have. Deterministic, allocation-free, and it counts its own calls.
class FakeSession final : public BackendSession {
public:
    [[nodiscard]] Status run(Span<Tensor* const> inputs,
                             Span<Tensor* const> outputs) noexcept override {
        if (inputs.empty() || outputs.empty()) {
            return fail(ErrorCode::InvalidArgument, "the fake backend takes one tensor each way");
        }
        const Span<const f32> source = inputs[0]->as<f32>();
        const Span<f32> destination = outputs[0]->as<f32>();
        const usize count =
            (source.size() < destination.size()) ? source.size() : destination.size();
        for (usize index = 0; index < count; ++index) {
            destination[index] = (source[index] * 2.0F) + 1.0F;
        }
        ++runs;
        return ok();
    }

    [[nodiscard]] u64 memory_bytes() const noexcept override { return 4096; }

    u32 runs = 0;
};

/// A backend of a declared kind. The kind is a constructor argument so that one test can register
/// several and drive the preference order.
class FakeBackend final : public InferenceBackend {
public:
    explicit FakeBackend(BackendKind kind, bool deterministic = true) noexcept
        : kind_(kind), deterministic_(deterministic) {}

    [[nodiscard]] const char* backend_name() const noexcept override {
        return backend_kind_name(kind_);
    }
    [[nodiscard]] BackendKind kind() const noexcept override { return kind_; }

    [[nodiscard]] BackendCapabilities capabilities() const noexcept override {
        BackendCapabilities capabilities;
        capabilities.kind = kind_;
        capabilities.precisions = precision_bit(Precision::F32);
        capabilities.zero_copy = true;
        capabilities.deterministic_execution = deterministic_;
        return capabilities;
    }

    [[nodiscard]] Status validate(const ModelAsset& asset,
                                  Array<UnsupportedOperator>& unsupported) noexcept override {
        if (!refuse_operator.empty()) {
            UnsupportedOperator entry;
            const usize copied = (refuse_operator.size() < UnsupportedOperator::kMaxName - 1)
                                     ? refuse_operator.size()
                                     : UnsupportedOperator::kMaxName - 1;
            for (usize index = 0; index < copied; ++index) {
                entry.name[index] = refuse_operator[index];
            }
            if (Status pushed = unsupported.push_back(entry); !pushed) {
                return pushed;
            }
            return fail(ErrorCode::Unsupported, "the fake backend was told to refuse an operator");
        }
        return asset.payload().empty()
                   ? fail(ErrorCode::InvalidArgument, "a model asset carries a payload")
                   : ok();
    }

    [[nodiscard]] Expected<BackendSession*, Error> create_session(
        Allocator& allocator, const ModelAsset& asset, const SessionDesc& desc) noexcept override {
        (void)desc;
        if (asset.payload().empty()) {
            return fail(ErrorCode::InvalidArgument, "a model asset carries a payload");
        }
        void* memory = allocator.allocate(sizeof(FakeSession), alignof(FakeSession));
        if (memory == nullptr) {
            return fail(ErrorCode::OutOfMemory, "no room for a fake session");
        }
        ++sessions_created;
        return static_cast<BackendSession*>(new (memory) FakeSession());
    }

    void destroy_session(BackendSession* session, Allocator& allocator) noexcept override {
        if (session == nullptr) {
            return;
        }
        // The interface's own destroy_session, handed back exactly what create_session made, and
        // -fno-rtti is the engine's language contract, so dynamic_cast does not exist here.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        auto* fake = static_cast<FakeSession*>(session);
        fake->~FakeSession();
        allocator.deallocate(static_cast<void*>(fake), sizeof(FakeSession), alignof(FakeSession));
        ++sessions_destroyed;
    }

    /// Set to make `validate` refuse, naming this operator.
    std::string_view refuse_operator;
    u32 sessions_created = 0;
    u32 sessions_destroyed = 0;

private:
    BackendKind kind_;
    bool deterministic_;
};

/// A model asset over a payload that is not a model: four bytes, enough for every check above the
/// backend. Declares one dynamic-batch input of four floats and one output of three.
[[nodiscard]] inline Expected<ModelAsset, Error> make_fake_asset(
    Allocator& allocator, BackendKind validated = BackendKind::OnnxRuntime) noexcept {
    static constexpr u8 kPayload[] = {'f', 'a', 'k', 'e'};
    Expected<ModelAsset, Error> asset =
        ModelAsset::create(allocator, Name::intern("test.fake"), Span<const u8>(kPayload, 4));
    if (!asset) {
        return asset;
    }
    const i64 input_dimensions[] = {TensorShape::kDynamic, 4};
    const i64 output_dimensions[] = {TensorShape::kDynamic, 3};

    TensorSpec input;
    input.name = Name::intern("features");
    input.type = ElementType::F32;
    input.shape = TensorShape::of(Span<const i64>(input_dimensions, 2)).value();

    TensorSpec output;
    output.name = Name::intern("scores");
    output.type = ElementType::F32;
    output.shape = TensorShape::of(Span<const i64>(output_dimensions, 2)).value();

    if (Status declared = asset.value().declare_input(input); !declared) {
        return make_unexpected(declared.error());
    }
    if (Status declared = asset.value().declare_output(output); !declared) {
        return make_unexpected(declared.error());
    }
    if (Status declared = asset.value().declare_validated_backend(validated); !declared) {
        return make_unexpected(declared.error());
    }
    asset.value().set_precision(Precision::F32);
    return asset;
}

}  // namespace cy::ml::test

// The registry, the selection rule, and a session over a backend that is not a runtime.
// M8.c tasks 4.1 and 4.2.

#include <cy/core/memory/system_allocator.h>
#include <cy/ml/session.h>
#include <cy/test/test.h>

#include "fake_backend.h"

namespace {

using namespace cy;
using namespace cy::ml;
using cy::ml::test::FakeBackend;
using cy::ml::test::make_fake_asset;

CY_TEST_CASE("ml.backend: a registry holds one backend per kind") {
    FakeBackend first(BackendKind::OnnxRuntime);
    FakeBackend duplicate(BackendKind::OnnxRuntime);
    FakeBackend other(BackendKind::TensorRT);

    BackendRegistry registry;
    CY_CHECK(registry.add(&first).has_value());
    Status again = registry.add(&duplicate);
    CY_REQUIRE_FALSE(again.has_value());
    CY_CHECK_EQ(again.error().code, ErrorCode::AlreadyExists);
    CY_CHECK(registry.add(&other).has_value());
    CY_CHECK_EQ(registry.size(), 2U);
    CY_CHECK_EQ(registry.find(BackendKind::TensorRT), &other);
    CY_CHECK_EQ(registry.find(BackendKind::CoreML), nullptr);
    CY_CHECK_FALSE(registry.add(nullptr).has_value());
}

CY_TEST_CASE("ml.backend: automatic selection takes the highest preference the model validated") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    FakeBackend portable(BackendKind::OnnxRuntime);
    FakeBackend platform(BackendKind::TensorRT);
    BackendRegistry registry;
    CY_REQUIRE(registry.add(&portable).has_value());
    CY_REQUIRE(registry.add(&platform).has_value());

    // Validated for ONNX Runtime alone: the higher-preference TensorRT is present and is rejected,
    // which is the "filtered ... by the model's validated-backend list" half of the rule.
    Expected<ModelAsset, Error> portable_only =
        make_fake_asset(allocator, BackendKind::OnnxRuntime);
    CY_REQUIRE(portable_only.has_value());
    SelectionReport report;
    SessionDesc desc;
    CY_CHECK_EQ(select_backend(registry, portable_only.value(), desc, report), &portable);
    CY_CHECK_EQ(report.outcome, SelectionOutcome::Automatic);
    CY_CHECK_EQ(report.chosen, BackendKind::OnnxRuntime);
    CY_CHECK_EQ(report.rejected_unvalidated, 1U);

    // Validated for both: the platform backend wins, because it is the one tuned for the device.
    CY_REQUIRE(portable_only.value().declare_validated_backend(BackendKind::TensorRT).has_value());
    CY_CHECK_EQ(select_backend(registry, portable_only.value(), desc, report), &platform);
    CY_CHECK_EQ(report.chosen, BackendKind::TensorRT);
    CY_CHECK_EQ(report.rejected_unvalidated, 0U);
}

CY_TEST_CASE("ml.backend: an explicit request is never quietly substituted") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    FakeBackend portable(BackendKind::OnnxRuntime);
    BackendRegistry registry;
    CY_REQUIRE(registry.add(&portable).has_value());
    Expected<ModelAsset, Error> asset = make_fake_asset(allocator, BackendKind::OnnxRuntime);
    CY_REQUIRE(asset.has_value());

    SessionDesc desc;
    desc.backend = BackendKind::TensorRT;  // asked for, and not in this build
    SelectionReport report;
    CY_CHECK_EQ(select_backend(registry, asset.value(), desc, report), nullptr);
    CY_CHECK_EQ(report.outcome, SelectionOutcome::RequestedBackendAbsent);

    // A caller that asked for TensorRT because its model is pinned to TensorRT must not silently
    // get ONNX Runtime, so session creation fails rather than falling back.
    Expected<InferenceSession, Error> session =
        InferenceSession::create(allocator, registry, asset.value(), desc, report);
    CY_REQUIRE_FALSE(session.has_value());
    CY_CHECK_EQ(session.error().code, ErrorCode::Unavailable);
}

CY_TEST_CASE("ml.backend: with no backend registered a session fails with the configure line") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<ModelAsset, Error> asset = make_fake_asset(allocator);
    CY_REQUIRE(asset.has_value());
    const BackendRegistry empty;
    SelectionReport report;
    SessionDesc desc;
    Expected<InferenceSession, Error> session =
        InferenceSession::create(allocator, empty, asset.value(), desc, report);
    CY_REQUIRE_FALSE(session.has_value());
    CY_CHECK_EQ(session.error().code, ErrorCode::Unavailable);
    CY_CHECK_EQ(report.outcome, SelectionOutcome::NoBackendRegistered);
}

CY_TEST_CASE("ml.backend: a model nobody validated is refused, not run anyway") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    FakeBackend portable(BackendKind::OnnxRuntime);
    BackendRegistry registry;
    CY_REQUIRE(registry.add(&portable).has_value());

    // Validated for Core ML alone, which this build does not have.
    Expected<ModelAsset, Error> asset = make_fake_asset(allocator, BackendKind::CoreML);
    CY_REQUIRE(asset.has_value());
    SelectionReport report;
    SessionDesc desc;
    CY_CHECK_EQ(select_backend(registry, asset.value(), desc, report), nullptr);
    CY_CHECK_EQ(report.outcome, SelectionOutcome::NotValidatedForModel);
}

CY_TEST_CASE("ml.backend: a session allocates once and reuses its tensors across invocations") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    FakeBackend backend(BackendKind::OnnxRuntime);
    BackendRegistry registry;
    CY_REQUIRE(registry.add(&backend).has_value());
    Expected<ModelAsset, Error> asset = make_fake_asset(allocator);
    CY_REQUIRE(asset.has_value());

    SessionDesc desc;
    desc.max_batch = 8;
    SelectionReport report;
    Expected<InferenceSession, Error> created =
        InferenceSession::create(allocator, registry, asset.value(), desc, report);
    CY_REQUIRE(created.has_value());
    InferenceSession& session = created.value();

    CY_CHECK_EQ(session.input_count(), 1U);
    CY_CHECK_EQ(session.output_count(), 1U);
    CY_CHECK_EQ(session.stats().allocations, 2U);  // one input tensor, one output tensor
    CY_CHECK_NE(session.input(Name::intern("features")), nullptr);
    CY_CHECK_EQ(session.input(Name::intern("nothing")), nullptr);

    const void* input_address = session.input(0)->data();
    Span<f32> features = session.input(0)->as<f32>();
    CY_REQUIRE_EQ(features.size(), 32U);  // the batch was allocated at max_batch
    for (usize index = 0; index < features.size(); ++index) {
        features[index] = static_cast<f32>(index);
    }
    for (u32 tick = 0; tick < 4; ++tick) {
        CY_CHECK(session.run().has_value());
    }
    CY_CHECK_EQ(session.stats().invocations, 4U);
    CY_CHECK_EQ(session.stats().allocations, 2U);  // still two: nothing reallocated
    CY_CHECK_EQ(session.input(0)->data(), input_address);
    CY_CHECK_EQ(session.output(0)->as<f32>()[3], (3.0F * 2.0F) + 1.0F);
    CY_CHECK_GT(session.memory_bytes(), 0U);
}

CY_TEST_CASE("ml.backend: a batch reshapes without reallocating and is refused above its ceiling") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    FakeBackend backend(BackendKind::OnnxRuntime);
    BackendRegistry registry;
    CY_REQUIRE(registry.add(&backend).has_value());
    Expected<ModelAsset, Error> asset = make_fake_asset(allocator);
    CY_REQUIRE(asset.has_value());

    SessionDesc desc;
    desc.max_batch = 16;
    SelectionReport report;
    Expected<InferenceSession, Error> created =
        InferenceSession::create(allocator, registry, asset.value(), desc, report);
    CY_REQUIRE(created.has_value());
    InferenceSession& session = created.value();

    CY_CHECK(session.run_batch(5).has_value());
    CY_CHECK_EQ(session.input(0)->shape().dimensions[0], 5);
    CY_CHECK_EQ(session.input(0)->as<f32>().size(), 20U);
    CY_CHECK_EQ(session.output(0)->as<f32>().size(), 15U);
    CY_CHECK_EQ(session.stats().largest_batch, 5U);
    CY_CHECK_EQ(session.stats().allocations, 2U);

    CY_CHECK(session.run_batch(16).has_value());
    CY_CHECK_EQ(session.stats().largest_batch, 16U);
    CY_CHECK_EQ(session.stats().allocations, 2U);

    // Above the ceiling the session was built for: refused, because growing would be the
    // reallocation-in-a-frame the reuse requirement forbids.
    Status too_big = session.run_batch(17);
    CY_REQUIRE_FALSE(too_big.has_value());
    CY_CHECK_EQ(too_big.error().code, ErrorCode::BufferTooSmall);
    CY_CHECK_FALSE(session.run_batch(0).has_value());
}

CY_TEST_CASE("ml.backend: a session destroys exactly the backend session it created") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    FakeBackend backend(BackendKind::OnnxRuntime);
    BackendRegistry registry;
    CY_REQUIRE(registry.add(&backend).has_value());
    Expected<ModelAsset, Error> asset = make_fake_asset(allocator);
    CY_REQUIRE(asset.has_value());
    SessionDesc desc;
    SelectionReport report;
    {
        Expected<InferenceSession, Error> created =
            InferenceSession::create(allocator, registry, asset.value(), desc, report);
        CY_REQUIRE(created.has_value());
        CY_CHECK_EQ(backend.sessions_created, 1U);
        CY_CHECK_EQ(backend.sessions_destroyed, 0U);

        // A move must not double-destroy: the moved-from session owns nothing.
        InferenceSession moved(std::move(created).value());
        CY_CHECK_EQ(moved.backend(), BackendKind::OnnxRuntime);
        CY_CHECK(moved.run().has_value());
    }
    CY_CHECK_EQ(backend.sessions_destroyed, 1U);
}

CY_TEST_CASE("ml.backend: cook-time validation names the operator it cannot run") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    FakeBackend backend(BackendKind::OnnxRuntime);
    backend.refuse_operator = "com.example:Gelu(1)";
    Expected<ModelAsset, Error> asset = make_fake_asset(allocator);
    CY_REQUIRE(asset.has_value());

    Array<UnsupportedOperator> unsupported(allocator);
    Status validated = backend.validate(asset.value(), unsupported);
    CY_REQUIRE_FALSE(validated.has_value());
    CY_REQUIRE_EQ(unsupported.size(), 1U);
    CY_CHECK_EQ(std::string_view(unsupported[0].name), std::string_view("com.example:Gelu(1)"));
}

}  // namespace

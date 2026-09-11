// THE COOK-TIME DETERMINISM BOUNDARY, driven end to end over real model assets.
// M8.c tasks 4.2 and 1.3.
//
// `ml-inference`: "A non-pinned model feeding an authoritative node SHALL be rejected at cook
// time", and "WHEN a non-pinned model's output feeds an authoritative AI decision THEN cooking
// SHALL fail with a diagnostic, rather than the mismatch surfacing later as a multiplayer desync."
//
// The gate is `cy::gameplay::check_inference_bindings`. What this suite adds to the gate's own
// tests in src/gameplay/ is the half that is CyberML's: that a `ModelAsset`'s declared determinism
// becomes the `ModelPinning` the gate reads, and that the two digests being compared are produced
// by the same function on both sides. A gate fed a wrong pinning is a gate that passes.

#include <cy/core/memory/system_allocator.h>
#include <cy/ml/cook.h>
#include <cy/test/test.h>

namespace {

using namespace cy;
using namespace cy::ml;

constexpr const char* kDevice = "cpu";

[[nodiscard]] Expected<ModelAsset, Error> make_asset(Allocator& allocator, const char* name,
                                                     bool pinned) {
    static constexpr u8 kPayload[] = {'m', 'o', 'd', 'e', 'l'};
    Expected<ModelAsset, Error> asset =
        ModelAsset::create(allocator, Name::intern(name), Span<const u8>(kPayload, 5));
    if (!asset) {
        return asset;
    }
    asset.value().set_precision(Precision::F32);
    if (pinned) {
        ModelDeterminism determinism;
        determinism.backend = BackendKind::OnnxRuntime;
        determinism.precision = Precision::F32;
        determinism.verified_configuration =
            cook_configuration(BackendKind::OnnxRuntime, Precision::F32, kDevice);
        asset.value().set_determinism(determinism);
    }
    return asset;
}

[[nodiscard]] gameplay::InferenceBinding binding_for(const char* model, bool authoritative) {
    gameplay::InferenceBinding binding;
    binding.graph = Name::intern("ai.guard");
    binding.node = Name::intern("classify_target");
    binding.model = Name::intern(model);
    binding.authoritative = authoritative;
    binding.cook_configuration =
        cook_configuration(BackendKind::OnnxRuntime, Precision::F32, kDevice);
    return binding;
}

CY_TEST_CASE("ml.cook: the bridge reports an undeclared model as declaring nothing") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<ModelAsset, Error> unpinned = make_asset(allocator, "model.unpinned", false);
    CY_REQUIRE(unpinned.has_value());

    const gameplay::ModelPinning pinning = pinning_of(unpinned.value());
    CY_CHECK_EQ(pinning.model, Name::intern("model.unpinned"));
    // EMPTY, not the text "unknown". `ModelPinning::pinned()` reads emptiness as "declares none",
    // so interning "unknown" here would make an undeclared model look pinned to a configuration
    // called unknown — the exact failure the gate exists to prevent, introduced by its own bridge.
    CY_CHECK(pinning.backend.is_empty());
    CY_CHECK(pinning.precision.is_empty());
    CY_CHECK_EQ(pinning.verified_configuration, 0U);
    CY_CHECK_FALSE(pinning.pinned());
}

CY_TEST_CASE("ml.cook: the bridge carries a pinned asset's whole claim") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<ModelAsset, Error> pinned = make_asset(allocator, "model.pinned", true);
    CY_REQUIRE(pinned.has_value());

    const gameplay::ModelPinning pinning = pinning_of(pinned.value());
    CY_CHECK_EQ(pinning.backend, Name::intern("onnxruntime"));
    CY_CHECK_EQ(pinning.precision, Name::intern("f32"));
    CY_CHECK_EQ(pinning.verified_configuration,
                cook_configuration(BackendKind::OnnxRuntime, Precision::F32, kDevice));
    CY_CHECK(pinning.pinned());
}

CY_TEST_CASE("ml.cook: a non-pinned model behind an authoritative node fails the cook") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<ModelAsset, Error> unpinned = make_asset(allocator, "model.unpinned", false);
    CY_REQUIRE(unpinned.has_value());

    const gameplay::ModelPinning models[] = {pinning_of(unpinned.value())};
    const gameplay::InferenceBinding bindings[] = {binding_for("model.unpinned", true)};

    Array<gameplay::CookRefusal> refusals(allocator);
    gameplay::CookFirewallReport report;
    Status cooked = gameplay::check_inference_bindings(
        Span<const gameplay::ModelPinning>(models, 1),
        Span<const gameplay::InferenceBinding>(bindings, 1), refusals, report);
    // THE COOK FAILS. Not a warning, not a runtime diagnostic: a non-ok status, which a cook driver
    // that already fails its run on an error fails on without a second convention.
    CY_REQUIRE_FALSE(cooked.has_value());
    CY_REQUIRE_EQ(refusals.size(), 1U);
    CY_CHECK_EQ(refusals[0].tag, gameplay::CookRefusalTag::ModelNotPinned);
    CY_CHECK_EQ(refusals[0].model, Name::intern("model.unpinned"));
    CY_CHECK_EQ(report.refusals, 1U);
    CY_CHECK_EQ(report.authoritative_bindings, 1U);
}

CY_TEST_CASE("ml.cook: the same model behind a presentation node cooks") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<ModelAsset, Error> unpinned = make_asset(allocator, "model.unpinned", false);
    CY_REQUIRE(unpinned.has_value());

    const gameplay::ModelPinning models[] = {pinning_of(unpinned.value())};
    const gameplay::InferenceBinding bindings[] = {binding_for("model.unpinned", false)};

    Array<gameplay::CookRefusal> refusals(allocator);
    gameplay::CookFirewallReport report;
    // "WHEN a model drives an animation blend weight or a non-authoritative visual choice THEN it
    // SHALL be permitted without pinning."
    CY_CHECK(gameplay::check_inference_bindings(Span<const gameplay::ModelPinning>(models, 1),
                                                Span<const gameplay::InferenceBinding>(bindings, 1),
                                                refusals, report)
                 .has_value());
    CY_CHECK_EQ(refusals.size(), 0U);
    CY_CHECK_EQ(report.presentation_bindings, 1U);
    // The gate says what it waved past as well as what it refused, which is how a reader can tell a
    // gate that found nothing from a gate that looked at nothing.
    CY_CHECK_EQ(report.bindings_examined, 1U);
}

CY_TEST_CASE("ml.cook: a pinned model cooks, and cooking it for another device does not") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<ModelAsset, Error> pinned = make_asset(allocator, "model.pinned", true);
    CY_REQUIRE(pinned.has_value());
    const gameplay::ModelPinning models[] = {pinning_of(pinned.value())};

    Array<gameplay::CookRefusal> refusals(allocator);
    gameplay::CookFirewallReport report;
    const gameplay::InferenceBinding matching[] = {binding_for("model.pinned", true)};
    CY_CHECK(gameplay::check_inference_bindings(Span<const gameplay::ModelPinning>(models, 1),
                                                Span<const gameplay::InferenceBinding>(matching, 1),
                                                refusals, report)
                 .has_value());
    CY_CHECK_EQ(report.pinned_bindings, 1U);

    // The asset is verified on the CPU and this cook targets a GPU. Same model, same backend, same
    // precision — and `ml-inference` treats inference as non-deterministic "across backends,
    // DEVICES, and driver versions", so it is refused with both numbers named.
    gameplay::InferenceBinding elsewhere = binding_for("model.pinned", true);
    elsewhere.cook_configuration =
        cook_configuration(BackendKind::OnnxRuntime, Precision::F32, "cuda:0");
    refusals.clear();
    report = gameplay::CookFirewallReport{};
    Status cooked = gameplay::check_inference_bindings(
        Span<const gameplay::ModelPinning>(models, 1),
        Span<const gameplay::InferenceBinding>(&elsewhere, 1), refusals, report);
    CY_REQUIRE_FALSE(cooked.has_value());
    CY_REQUIRE_EQ(refusals.size(), 1U);
    CY_CHECK_EQ(refusals[0].tag, gameplay::CookRefusalTag::PinnedConfigurationMismatch);
    CY_CHECK_NE(refusals[0].verified_configuration, refusals[0].cook_configuration);
}

CY_TEST_CASE("ml.cook: a binding whose model the cook never saw is refused, not assumed") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    const gameplay::InferenceBinding bindings[] = {binding_for("model.absent", true)};
    Array<gameplay::CookRefusal> refusals(allocator);
    gameplay::CookFirewallReport report;
    Status cooked = gameplay::check_inference_bindings(
        Span<const gameplay::ModelPinning>(), Span<const gameplay::InferenceBinding>(bindings, 1),
        refusals, report);
    CY_REQUIRE_FALSE(cooked.has_value());
    CY_REQUIRE_EQ(refusals.size(), 1U);
    CY_CHECK_EQ(refusals[0].tag, gameplay::CookRefusalTag::UnknownModel);
}

CY_TEST_CASE("ml.cook: both sides of the comparison come from one function") {
    // The property that makes the gate mean anything: the number an asset stores and the number a
    // cook computes are produced by the same code. Two functions that agree today are two functions
    // that can drift, and a gate comparing them would then refuse everything or nothing.
    CY_CHECK_EQ(cook_configuration(BackendKind::OnnxRuntime, Precision::F16, "ane"),
                configuration_digest(BackendKind::OnnxRuntime, Precision::F16, "ane"));
}

}  // namespace

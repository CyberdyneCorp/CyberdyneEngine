// The model asset, its container, its content hash and the configuration digest. M8.c tasks 4.1
// and 4.2.

#include <cy/core/memory/system_allocator.h>
#include <cy/ml/model.h>
#include <cy/test/test.h>

namespace {

using namespace cy;
using namespace cy::ml;

[[nodiscard]] TensorShape shape_of(std::initializer_list<i64> dimensions) {
    return TensorShape::of(Span<const i64>(dimensions.begin(), dimensions.size())).value();
}

[[nodiscard]] Expected<ModelAsset, Error> make_asset(Allocator& allocator) {
    static constexpr u8 kPayload[] = {0x08, 0x07, 'm', 'o', 'd', 'e', 'l'};
    Expected<ModelAsset, Error> asset = ModelAsset::create(
        allocator, Name::intern("perception.threat"), Span<const u8>(kPayload, sizeof(kPayload)));
    if (!asset) {
        return asset;
    }
    TensorSpec input;
    input.name = Name::intern("features");
    input.type = ElementType::F32;
    input.shape = shape_of({TensorShape::kDynamic, 4});

    TensorSpec output;
    output.name = Name::intern("scores");
    output.type = ElementType::F32;
    output.shape = shape_of({TensorShape::kDynamic, 3});

    if (Status declared = asset.value().declare_input(input); !declared) {
        return make_unexpected(declared.error());
    }
    if (Status declared = asset.value().declare_output(output); !declared) {
        return make_unexpected(declared.error());
    }
    if (Status declared = asset.value().declare_validated_backend(BackendKind::OnnxRuntime);
        !declared) {
        return make_unexpected(declared.error());
    }
    asset.value().set_precision(Precision::F32);
    return asset;
}

CY_TEST_CASE("ml.model: an asset carries the six things the specification names") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<ModelAsset, Error> asset = make_asset(allocator);
    CY_REQUIRE(asset.has_value());

    CY_CHECK_EQ(asset.value().name(), Name::intern("perception.threat"));
    CY_CHECK_EQ(asset.value().payload().size(), 7U);
    CY_CHECK_EQ(asset.value().inputs().size(), 1U);
    CY_CHECK_EQ(asset.value().outputs().size(), 1U);
    CY_CHECK_EQ(asset.value().precision(), Precision::F32);
    CY_CHECK(asset.value().validated_for(BackendKind::OnnxRuntime));
    CY_CHECK_FALSE(asset.value().validated_for(BackendKind::TensorRT));
    CY_CHECK_NE(asset.value().input(Name::intern("features")), nullptr);
    CY_CHECK_EQ(asset.value().input(Name::intern("scores")), nullptr);
}

CY_TEST_CASE("ml.model: an asset with no declared determinism is not pinned") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<ModelAsset, Error> asset = make_asset(allocator);
    CY_REQUIRE(asset.has_value());
    CY_CHECK_FALSE(asset.value().determinism().pinned());

    // HALF A CLAIM IS NOT A CLAIM. All three of backend, precision and a verified configuration are
    // required, because `ml-inference` requires all three, and a partial declaration is exactly the
    // shape a mistake takes.
    ModelDeterminism partial;
    partial.backend = BackendKind::OnnxRuntime;
    asset.value().set_determinism(partial);
    CY_CHECK_FALSE(asset.value().determinism().pinned());

    partial.precision = Precision::F32;
    asset.value().set_determinism(partial);
    CY_CHECK_FALSE(asset.value().determinism().pinned());

    partial.verified_configuration =
        configuration_digest(BackendKind::OnnxRuntime, Precision::F32, "cpu");
    asset.value().set_determinism(partial);
    CY_CHECK(asset.value().determinism().pinned());
}

CY_TEST_CASE("ml.model: the configuration digest separates backend, precision and device") {
    const u64 base = configuration_digest(BackendKind::OnnxRuntime, Precision::F32, "cpu");
    CY_CHECK_NE(base, 0U);
    CY_CHECK_EQ(base, configuration_digest(BackendKind::OnnxRuntime, Precision::F32, "cpu"));
    CY_CHECK_NE(base, configuration_digest(BackendKind::TensorRT, Precision::F32, "cpu"));
    CY_CHECK_NE(base, configuration_digest(BackendKind::OnnxRuntime, Precision::F16, "cpu"));
    // The clause the requirement spends a sentence on: "non-deterministic across backends,
    // DEVICES, and driver versions".
    CY_CHECK_NE(base, configuration_digest(BackendKind::OnnxRuntime, Precision::F32, "cuda:0"));

    // A digest is never zero, so "declares none" and "verified against configuration zero" cannot
    // be confused. This is the one property `ModelDeterminism::pinned()` depends on.
    for (u32 index = 0; index < static_cast<u32>(BackendKind::Count); ++index) {
        CY_CHECK_NE(configuration_digest(static_cast<BackendKind>(index), Precision::F32, ""), 0U);
    }
}

CY_TEST_CASE("ml.model: the container round-trips everything the asset declares") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<ModelAsset, Error> asset = make_asset(allocator);
    CY_REQUIRE(asset.has_value());
    ModelDeterminism determinism;
    determinism.backend = BackendKind::OnnxRuntime;
    determinism.precision = Precision::F32;
    determinism.verified_configuration =
        configuration_digest(BackendKind::OnnxRuntime, Precision::F32, "cpu");
    asset.value().set_determinism(determinism);

    Array<u8> bytes(allocator);
    CY_REQUIRE(write_model_asset(asset.value(), bytes).has_value());
    CY_CHECK_GT(bytes.size(), 7U);

    Expected<ModelAsset, Error> read = read_model_asset(allocator, bytes.span());
    CY_REQUIRE(read.has_value());
    CY_CHECK_EQ(read.value().name(), asset.value().name());
    CY_CHECK_EQ(read.value().payload().size(), asset.value().payload().size());
    CY_CHECK_EQ(read.value().inputs().size(), 1U);
    CY_CHECK_EQ(read.value().inputs()[0].name, Name::intern("features"));
    CY_CHECK_EQ(read.value().inputs()[0].shape.dimensions[0], TensorShape::kDynamic);
    CY_CHECK_EQ(read.value().outputs()[0].shape.dimensions[1], 3);
    CY_CHECK(read.value().validated_for(BackendKind::OnnxRuntime));
    CY_CHECK_EQ(read.value().determinism().verified_configuration,
                determinism.verified_configuration);

    // Content addressing: the same declarations and the same bytes give the same identity.
    CY_CHECK_EQ(read.value().content_hash(), asset.value().content_hash());
}

CY_TEST_CASE("ml.model: the content hash covers the declarations, not only the payload") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<ModelAsset, Error> first = make_asset(allocator);
    Expected<ModelAsset, Error> second = make_asset(allocator);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(first.value().content_hash(), second.value().content_hash());

    // The same bytes at a different declared precision cook and run differently, so they are a
    // different asset.
    second.value().set_precision(Precision::F16);
    CY_CHECK_NE(first.value().content_hash(), second.value().content_hash());
}

CY_TEST_CASE("ml.model: a truncated or foreign container is refused rather than read") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<ModelAsset, Error> asset = make_asset(allocator);
    CY_REQUIRE(asset.has_value());
    Array<u8> bytes(allocator);
    CY_REQUIRE(write_model_asset(asset.value(), bytes).has_value());

    // Not a CyberML container at all.
    const u8 foreign[] = {'n', 'o', 'p', 'e', 0, 0, 0, 0};
    CY_CHECK_FALSE(
        read_model_asset(allocator, Span<const u8>(foreign, sizeof(foreign))).has_value());

    // Every prefix of a valid container is refused, and none of them reads past its end. That is
    // the property a bounds-checked cursor exists for, and it is checked over every length rather
    // than at one arbitrary cut.
    for (usize length = 0; length + 1 < bytes.size(); ++length) {
        CY_CHECK_FALSE(read_model_asset(allocator, bytes.span().subspan(0, length)).has_value());
    }
    CY_CHECK(read_model_asset(allocator, bytes.span()).has_value());
}

}  // namespace

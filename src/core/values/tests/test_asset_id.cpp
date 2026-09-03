// `AssetId` — the 128-bit content identity, and its separation from runtime handles. Task 1.3.2.
//
// `core-type-system` — "Asset ids are distinct from handles". The scenario is "Serialized reference
// survives a reload": a saved scene resolves its mesh through the `AssetId`, and the runtime handle
// it ends up with may differ. What can be tested here without an asset system is the half that
// makes that possible — the id is a value with a canonical text form that round-trips, and it is
// structurally not a handle. The compile-fail suite proves the second half.

#include <cy/core/values/asset_id.h>
#include <cy/core/values/handle.h>

#include <cy/test/test.h>

#include <string>
#include <type_traits>

CY_TEST_CASE("AssetId: the canonical form round-trips") {
    const cy::AssetId id(0x0123456789abcdefULL, 0xfedcba9876543210ULL);

    char text[cy::AssetId::kTextLength + 1] = {};
    CY_CHECK_EQ(id.format(text), cy::AssetId::kTextLength);
    CY_CHECK_EQ(std::string(text), std::string("0123456789abcdeffedcba9876543210"));

    const cy::Expected<cy::AssetId, cy::Error> parsed = cy::AssetId::parse(text);
    CY_REQUIRE(parsed.has_value());
    CY_CHECK(*parsed == id);
}

CY_TEST_CASE("AssetId: uppercase parses, and anything else is rejected") {
    CY_CHECK(cy::AssetId::parse("0123456789ABCDEFFEDCBA9876543210").has_value());

    const cy::Expected<cy::AssetId, cy::Error> short_form = cy::AssetId::parse("0123");
    CY_REQUIRE_FALSE(short_form.has_value());
    CY_CHECK(short_form.error().code == cy::ErrorCode::InvalidArgument);

    const cy::Expected<cy::AssetId, cy::Error> bad =
        cy::AssetId::parse("0123456789abcdeffedcba987654321z");
    CY_REQUIRE_FALSE(bad.has_value());

    // 32 characters is the whole form: a trailing separator is not tolerated.
    CY_CHECK_FALSE(cy::AssetId::parse("0123456789abcdef-fedcba9876543210").has_value());
}

CY_TEST_CASE("AssetId: the nil id names nothing") {
    const cy::AssetId nil;
    CY_CHECK(nil.is_nil());
    CY_CHECK_FALSE(static_cast<bool>(nil));
    CY_CHECK(cy::AssetId(0, 1).is_nil() == false);
}

CY_TEST_CASE("AssetId: ordering matches the text form") {
    const cy::AssetId low(0, 1);
    const cy::AssetId mid(0, 2);
    const cy::AssetId high(1, 0);
    CY_CHECK(low < mid);
    CY_CHECK(mid < high);
    CY_CHECK_FALSE(high < low);
}

CY_TEST_CASE("AssetId is structurally not a handle") {
    CY_CHECK_EQ(sizeof(cy::AssetId), 16u);
    CY_CHECK_EQ(sizeof(cy::Handle<struct AnyTag>), 8u);

    static_assert(!std::is_convertible_v<cy::AssetId, cy::Handle<struct AnyTag>>);
    static_assert(!std::is_convertible_v<cy::Handle<struct AnyTag>, cy::AssetId>);
    static_assert(!std::is_constructible_v<cy::AssetId, cy::Handle<struct AnyTag>>);
    static_assert(!std::is_constructible_v<cy::Handle<struct AnyTag>, cy::AssetId>);
}

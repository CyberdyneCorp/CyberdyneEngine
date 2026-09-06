// The derivation key: what makes a piece of derived data *the same* piece of derived data. M5 task
// 5.1.
//
// The cases here are `asset-import-pipeline`'s own — an importer version bump re-cooks, an option
// change re-cooks — plus the one that is not in any specification and would otherwise be discovered
// in production: two different field sequences producing one key by concatenation.
//
// Unit rather than integration: nothing here touches a filesystem. The cache that files artefacts
// under these keys is exercised in test_derived_cache.cpp, which does.

#include <cy/core/assets/derivation.h>
#include <cy/test/test.h>

#include <cstring>

using namespace cy::assets;
using cy::u32;

namespace {

DerivationKey key_of(const char* source_text, u32 version, const char* option) {
    DerivationKeyBuilder builder;
    builder.producer(DerivedKind::Import, "texture", version)
        .source("source", content_hash(source_text, std::strlen(source_text)))
        .text("option", option)
        .text("platform", "desktop")
        .text("profile", "client");
    auto key = builder.finish();
    CY_REQUIRE(key.has_value());
    return key.value();
}

}  // namespace

CY_TEST_CASE("DerivationKey: the same computation produces the same key") {
    CY_CHECK(key_of("pixels", 1, "bc7") == key_of("pixels", 1, "bc7"));
}

CY_TEST_CASE("DerivationKey: an importer version bump re-cooks everything it handles") {
    // `asset-import-pipeline`: "WHEN an importer's version increases THEN all assets it handles
    // SHALL be re-cooked, since the version is part of the derivation key."
    CY_CHECK(key_of("pixels", 1, "bc7") != key_of("pixels", 2, "bc7"));
}

CY_TEST_CASE("DerivationKey: an option change changes the key") {
    // "WHEN an import option changes THEN the cache key SHALL change and the asset SHALL be
    // re-cooked."
    CY_CHECK(key_of("pixels", 1, "bc7") != key_of("pixels", 1, "astc"));
    CY_CHECK(key_of("pixels", 1, "bc7") != key_of("other pixels", 1, "bc7"));
}

CY_TEST_CASE("DerivationKey: concatenation cannot make two different inputs one key") {
    // The failure the framing exists to prevent. Without a length in front of each contribution,
    // ("ab", "c") and ("a", "bc") hash identically — and a cook cache that collapses two inputs
    // serves the wrong artefact with no diagnostic at all.
    DerivationKeyBuilder first;
    first.producer(DerivedKind::Import, "p", 1).text("ab", "c");
    DerivationKeyBuilder second;
    second.producer(DerivedKind::Import, "p", 1).text("a", "bc");
    CY_CHECK(first.finish().value() != second.finish().value());
}

CY_TEST_CASE("DerivationKey: a number, a flag and a text of the same shape are three keys") {
    DerivationKeyBuilder number;
    number.producer(DerivedKind::Shader, "p", 1).number("n", 1);
    DerivationKeyBuilder flag;
    flag.producer(DerivedKind::Shader, "p", 1).flag("n", true);
    DerivationKeyBuilder text;
    text.producer(DerivedKind::Shader, "p", 1).text("n", "1");

    const DerivationKey a = number.finish().value();
    const DerivationKey b = flag.finish().value();
    const DerivationKey c = text.finish().value();
    CY_CHECK(a != b);
    CY_CHECK(b != c);
    CY_CHECK(a != c);
}

CY_TEST_CASE("DerivationKey: two producers over the same source do not collide") {
    DerivationKeyBuilder importer;
    importer.producer(DerivedKind::Import, "gltf", 1).source("s", content_hash("x", 1));
    DerivationKeyBuilder shader;
    shader.producer(DerivedKind::Shader, "gltf", 1).source("s", content_hash("x", 1));
    CY_CHECK(importer.finish().value() != shader.finish().value());
}

CY_TEST_CASE("DerivationKey: a key with no producer is refused") {
    DerivationKeyBuilder builder;
    builder.text("platform", "desktop");
    CY_CHECK(!builder.finish().has_value());
}

CY_TEST_CASE("DerivationKey: the text form round-trips") {
    const DerivationKey key = key_of("pixels", 1, "bc7");
    char text[DerivationKey::kTextLength + 1] = {};
    key.format(text);
    const auto parsed = DerivationKey::parse(text);
    CY_REQUIRE(parsed.has_value());
    CY_CHECK(parsed.value() == key);
}

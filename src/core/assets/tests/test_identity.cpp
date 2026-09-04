// Asset identity: minting, the sidecar, placeholders and the collision refusal. Task 3.3.1.
//
// Three of `core-assets-and-io`'s scenarios live here, each as a case named after it.

#include <cy/core/assets/identity.h>
#include <cy/test/test.h>

#include <cstring>
#include <string_view>

using namespace cy::assets;

namespace {

VirtualPath path_of(const char* raw) {
    auto path = VirtualPath::normalise(raw);
    CY_REQUIRE(path.has_value());
    return path.value();
}

AssetMeta meta_for(cy::AssetId id, const char* source, AssetKind kind = AssetKind::Texture) {
    AssetMeta meta;
    meta.id = id;
    meta.kind = kind;
    meta.source = path_of(source);
    meta.source_hash = content_hash(source, std::strlen(source));
    return meta;
}

}  // namespace

CY_TEST_CASE("An asset id is minted once and derived from nothing") {
    const cy::AssetId first = mint_asset_id();
    const cy::AssetId second = mint_asset_id();
    CY_CHECK(first != second);
    CY_CHECK_FALSE(first.is_nil());
    CY_CHECK_FALSE(is_placeholder(first));
}

CY_TEST_CASE("Scenario: Asset moved on disk") {
    // WHEN a texture is moved to a different folder along with its .meta
    // THEN every reference SHALL continue to resolve, with no scene or material edits.
    AssetDatabase database;
    const cy::AssetId id = mint_asset_id();
    CY_REQUIRE(database.register_asset(meta_for(id, "art/textures/stone.png")).has_value());

    // The reference a scene holds is the id, and it is what resolves.
    CY_REQUIRE(database.find(id) != nullptr);
    CY_CHECK(database.find(id)->source == path_of("art/textures/stone.png"));

    CY_REQUIRE(database.rebind_source(id, path_of("art/environment/rock/stone.png")).has_value());

    // The same reference, unchanged, still resolves — to the new location.
    const AssetMeta* found = database.find(id);
    CY_REQUIRE(found != nullptr);
    CY_CHECK(found->source == path_of("art/environment/rock/stone.png"));
    CY_CHECK(found->id == id);
}

CY_TEST_CASE("Scenario: Duplicated meta file") {
    // WHEN two source files claim the same AssetId
    // THEN the importer SHALL report a collision and refuse to register the second, rather than
    //      silently shadowing the first.
    AssetDatabase database;
    const cy::AssetId id = mint_asset_id();
    CY_REQUIRE(database.register_asset(meta_for(id, "art/stone.png")).has_value());

    const auto refused = database.register_asset(meta_for(id, "art/copy_of_stone.png"));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::AlreadyExists);
    CY_CHECK_EQ(database.collisions(), 1u);

    // Both sides are named, which is what makes the diagnostic actionable.
    CY_CHECK(database.last_collision().id == id);
    CY_CHECK(database.last_collision().first == path_of("art/stone.png"));
    CY_CHECK(database.last_collision().second == path_of("art/copy_of_stone.png"));

    // The FIRST is still the one registered: it was not shadowed.
    CY_REQUIRE(database.find(id) != nullptr);
    CY_CHECK(database.find(id)->source == path_of("art/stone.png"));
    CY_CHECK_EQ(database.size(), 1u);
}

CY_TEST_CASE("Re-importing the same file is not a collision") {
    AssetDatabase database;
    const cy::AssetId id = mint_asset_id();
    AssetMeta meta = meta_for(id, "art/stone.png");
    CY_REQUIRE(database.register_asset(meta).has_value());
    meta.source_hash = content_hash("edited", 6);
    CY_REQUIRE(database.register_asset(meta).has_value());
    CY_CHECK_EQ(database.collisions(), 0u);
    CY_CHECK_EQ(database.size(), 1u);
}

CY_TEST_CASE("Scenario: Source deleted") {
    // WHEN a referenced asset no longer exists
    // THEN loading SHALL yield a typed placeholder — the identity half of it: a placeholder is a
    //      reserved id per kind, so it is a thing a package can actually contain and a diagnostic
    //      can actually name. The loading half is in test_asset_system.cpp.
    const cy::AssetId missing_texture = placeholder_for(AssetKind::Texture);
    const cy::AssetId missing_mesh = placeholder_for(AssetKind::Mesh);
    CY_CHECK(missing_texture != missing_mesh);
    CY_CHECK(is_placeholder(missing_texture));
    CY_CHECK(placeholder_kind(missing_texture) == AssetKind::Texture);
    CY_CHECK(placeholder_kind(missing_mesh) == AssetKind::Mesh);
    CY_CHECK(placeholder_kind(mint_asset_id()) == AssetKind::Unknown);
}

CY_TEST_CASE("A placeholder id cannot be claimed by content") {
    AssetDatabase database;
    const auto refused =
        database.register_asset(meta_for(placeholder_for(AssetKind::Texture), "art/stone.png"));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("The nil id names no asset") {
    AssetDatabase database;
    CY_CHECK_FALSE(database.register_asset(meta_for(cy::AssetId{}, "art/stone.png")).has_value());
}

CY_TEST_CASE("A sidecar round-trips through its text form") {
    AssetMeta meta = meta_for(mint_asset_id(), "art/textures/stone.png", AssetKind::Texture);
    meta.cooked_hash = content_hash("cooked", 6);

    char text[1024] = {};
    const auto written = write_meta(meta, text, sizeof(text));
    CY_REQUIRE(written.has_value());

    const auto parsed = parse_meta(std::string_view(text, written.value()));
    CY_REQUIRE(parsed.has_value());
    CY_CHECK(parsed.value().id == meta.id);
    CY_CHECK(parsed.value().kind == meta.kind);
    CY_CHECK(parsed.value().source == meta.source);
    CY_CHECK(parsed.value().source_hash == meta.source_hash);
    CY_CHECK(parsed.value().cooked_hash == meta.cooked_hash);
}

CY_TEST_CASE("A sidecar buffer that is too small is refused rather than truncated") {
    const AssetMeta meta = meta_for(mint_asset_id(), "art/stone.png");
    char text[32] = {};
    const auto written = write_meta(meta, text, sizeof(text));
    CY_REQUIRE_FALSE(written.has_value());
    CY_CHECK(written.error().code == cy::ErrorCode::BufferTooSmall);
}

CY_TEST_CASE("A sidecar with an unknown key is refused") {
    // Skipping it would silently drop meaning on the next write.
    const auto parsed = parse_meta("meta_version = 1\nimporter_settings = \"x\"\n");
    CY_REQUIRE_FALSE(parsed.has_value());
}

CY_TEST_CASE("A sidecar missing a required key is refused") {
    CY_CHECK_FALSE(parse_meta("meta_version = 1\n").has_value());
}

CY_TEST_CASE("The sidecar path is the source path plus .meta") {
    const auto sidecar = meta_path_for(path_of("art/stone.png"));
    CY_REQUIRE(sidecar.has_value());
    CY_CHECK(sidecar.value().view() == "art/stone.png.meta");
}

CY_TEST_CASE("Scenario: Platform variants") {
    // WHEN a texture is cooked for desktop (BC7) and mobile (ASTC)
    // THEN both variants SHALL be addressable by the same AssetId plus a variant key.
    // The identity half: a variant key is a value, distinct, ordered, and NOT a hash — a collision
    // here would serve the wrong variant silently. The addressing half is in test_package.cpp.
    const auto desktop = VariantKey::parse("desktop-bc7");
    const auto mobile = VariantKey::parse("mobile-astc");
    CY_REQUIRE(desktop.has_value());
    CY_REQUIRE(mobile.has_value());
    CY_CHECK(desktop.value() != mobile.value());
    CY_CHECK(desktop.value().view() == "desktop-bc7");
    CY_CHECK(VariantKey::any().is_any());
    CY_CHECK_FALSE(desktop.value().is_any());

    CY_CHECK_FALSE(VariantKey::parse("Desktop").has_value());  // lowercase only
    CY_CHECK_FALSE(VariantKey::parse("a-very-long-variant-key-indeed").has_value());
}

CY_TEST_CASE("parsing an empty key yields the any-key without touching a null source") {
    // REGRESSION. `parse` used to `memcpy(destination, text.data(), text.size())` unconditionally.
    // A default-constructed `std::string_view` has a null `data()` with a zero `size()`, and
    // `memcpy`'s source parameter is declared never-null, so this call was undefined behaviour that
    // copied nothing — invisible in an ordinary build and reported by UndefinedBehaviorSanitizer as
    // "null pointer passed as argument 2, which is declared to never be null". Under
    // `CY_SANITIZE=undefined` the build traps, so this case fails there if the guard is removed.
    //
    // It is not a corner: the any-key is what a package with one variant is addressed by, so this
    // path runs on every load. Both spellings of empty are exercised, because only the first has a
    // null pointer behind it.
    const auto from_default = VariantKey::parse(std::string_view{});
    CY_REQUIRE(from_default.has_value());
    CY_CHECK(from_default.value().is_any());
    CY_CHECK(from_default.value() == VariantKey::any());

    const auto from_literal = VariantKey::parse("");
    CY_REQUIRE(from_literal.has_value());
    CY_CHECK(from_literal.value().is_any());
    CY_CHECK(from_literal.value() == VariantKey::any());
}

CY_TEST_CASE("Asset kinds round-trip through their names") {
    CY_CHECK(asset_kind_from_name("texture").value() == AssetKind::Texture);
    CY_CHECK(asset_kind_from_name("binary").value() == AssetKind::Binary);
    CY_CHECK_FALSE(asset_kind_from_name("sprite").has_value());
}

CY_TEST_CASE("Scenario: Content hash") {
    // WHEN a cooked asset is written THEN its BLAKE3 content hash SHALL be recorded.
    const char payload[] = "the cooked bytes";
    const ContentHash first = content_hash(payload, sizeof(payload) - 1);
    const ContentHash again = content_hash(payload, sizeof(payload) - 1);
    CY_CHECK(first == again);
    CY_CHECK_FALSE(first.is_zero());

    const ContentHash different = content_hash("the cooked byteS", sizeof(payload) - 1);
    CY_CHECK(first != different);

    // A streaming digest of the same bytes in pieces is the same digest.
    ContentHasher hasher;
    hasher.update(payload, 4);
    hasher.update(payload + 4, sizeof(payload) - 5);
    CY_CHECK(hasher.finish() == first);

    char text[ContentHash::kTextLength + 1] = {};
    first.format(text);
    const auto parsed = ContentHash::parse(text);
    CY_REQUIRE(parsed.has_value());
    CY_CHECK(parsed.value() == first);
    CY_CHECK_FALSE(ContentHash::parse("not a hash").has_value());
}

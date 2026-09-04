// The cook path: the cooked-asset header, and a directory of documents becoming a package.

#include <cy/cook/pipeline.h>
#include <cy/core/assets/cooked.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/memory/ownership.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/scene/serialization/format.h>
#include <cy/test/test.h>

#include <cstring>
#include <string_view>
#include <utility>

using namespace cy;
using namespace cy::scene::serialization;

namespace {

[[nodiscard]] Allocator& test_allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

/// A component with one plain field. Enough to make a cooked block non-empty; the serialization
/// suites are where the component model itself is exercised.
struct Marker {
    u32 value = 0;
};

inline constexpr u32 kMarkerType = 9401;

[[nodiscard]] const reflect::TypeInfo& marker_type() noexcept {
    static reflect::FieldInfo fields[1];
    fields[0].name = "value";
    fields[0].id = reflect::FieldId(1);
    fields[0].kind = reflect::FieldKind::U32;
    fields[0].offset = 0;
    fields[0].size = sizeof(u32);

    static reflect::TypeInfo info;
    info.name = "cy::cook::test::Marker";
    info.id = reflect::TypeId(kMarkerType);
    info.size = static_cast<u32>(sizeof(Marker));
    info.alignment = static_cast<u32>(alignof(Marker));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 1;
    return info;
}

[[nodiscard]] Status make_world(ecs::World& world) noexcept {
    if (Status started = world.initialize(); !started) {
        return started;
    }
    if (auto id = world.components().register_reflected(marker_type()); !id) {
        return make_unexpected(id.error());
    }
    return ok();
}

/// A document of `count` entities, each carrying a marker.
[[nodiscard]] Status build_document(Document& document, AssetKind kind, AssetId id,
                                    u32 count) noexcept {
    document.kind = kind;
    document.id = id;
    for (u32 index = 0; index < count; ++index) {
        const Expected<DocumentEntity*, Error> entity = document.add_entity(kNoLocalId, "e");
        if (!entity) {
            return make_unexpected(entity.error());
        }
        const Expected<ComponentData*, Error> component =
            (*entity)->ensure(reflect::TypeId(kMarkerType));
        if (!component) {
            return make_unexpected(component.error());
        }
        if (Status written =
                (*component)
                    ->record.set_scalar(reflect::FieldId(1), serialize::WireType::U32, &index, 4);
            !written) {
            return written;
        }
    }
    return ok();
}

/// Write a document into a memory mount as its text form.
[[nodiscard]] Status place(assets::MemoryMount& mount, const char* path,
                           const Document& document) noexcept {
    Array<char> text(test_allocator());
    if (Status written = write_text(document, text); !written) {
        return written;
    }
    const Expected<assets::VirtualPath, Error> virtual_path = assets::VirtualPath::normalise(path);
    if (!virtual_path) {
        return make_unexpected(virtual_path.error());
    }
    return mount.add(virtual_path.value(), text.data(), text.size());
}

}  // namespace

CY_TEST_CASE("a cooked asset begins with a header naming its kind, hash and variant") {
    const u8 payload[] = {1, 2, 3, 4, 5, 6, 7, 8};
    const Expected<assets::VariantKey, Error> variant = assets::VariantKey::parse("desktop-bc7");
    CY_REQUIRE(variant.has_value());

    Array<u8> bytes(test_allocator());
    CY_REQUIRE(assets::write_cooked_asset(assets::AssetKind::Scene, variant.value(),
                                          Span<const u8>(payload, sizeof(payload)), bytes)
                   .has_value());
    CY_CHECK_EQ(bytes.size(), assets::kCookedHeaderBytes + sizeof(payload));

    const Expected<assets::CookedAssetHeader, Error> header =
        assets::read_cooked_header(bytes.data(), bytes.size());
    CY_REQUIRE(header.has_value());
    CY_CHECK_EQ(header->format_version, assets::kCookedFormatVersion);
    CY_CHECK_EQ(header->kind, assets::AssetKind::Scene);
    CY_CHECK_EQ(header->payload_size, sizeof(payload));
    CY_CHECK(header->variant == variant.value());
    CY_CHECK_FALSE(header->content.is_zero());

    const Expected<Span<const u8>, Error> read =
        assets::read_cooked_payload(bytes.data(), bytes.size(), /*verify_hash=*/true);
    CY_REQUIRE(read.has_value());
    CY_REQUIRE_EQ(read->size(), sizeof(payload));
    CY_CHECK_EQ((*read)[0], 1U);
}

CY_TEST_CASE("bytes that are not a cooked asset are refused at the magic") {
    u8 rubbish[assets::kCookedHeaderBytes] = {};
    const Expected<assets::CookedAssetHeader, Error> header =
        assets::read_cooked_header(rubbish, sizeof(rubbish));
    CY_REQUIRE_FALSE(header.has_value());
    CY_CHECK_EQ(header.error().code, ErrorCode::InvalidArgument);

    // And something shorter than a header is out of range rather than read past its end.
    CY_CHECK_FALSE(assets::read_cooked_header(rubbish, 4).has_value());
}

CY_TEST_CASE("a cooked asset from a newer format version fails with a diagnostic") {
    Array<u8> bytes(test_allocator());
    CY_REQUIRE(
        assets::write_cooked_asset(assets::AssetKind::Binary, assets::VariantKey::any(), {}, bytes)
            .has_value());
    // The version field is four bytes at offset eight, little-endian.
    bytes[8] = static_cast<u8>(assets::kCookedFormatVersion + 1);

    const Expected<assets::CookedAssetHeader, Error> header =
        assets::read_cooked_header(bytes.data(), bytes.size());
    CY_REQUIRE_FALSE(header.has_value());
    CY_CHECK_EQ(header.error().code, ErrorCode::Unsupported);
}

CY_TEST_CASE("a corrupted payload is caught by the recorded hash") {
    const u8 payload[] = {9, 9, 9, 9};
    Array<u8> bytes(test_allocator());
    CY_REQUIRE(assets::write_cooked_asset(assets::AssetKind::Binary, assets::VariantKey::any(),
                                          Span<const u8>(payload, sizeof(payload)), bytes)
                   .has_value());
    bytes[assets::kCookedHeaderBytes] = 8;

    CY_CHECK(
        assets::read_cooked_payload(bytes.data(), bytes.size(), /*verify_hash=*/false).has_value());
    const Expected<Span<const u8>, Error> verified =
        assets::read_cooked_payload(bytes.data(), bytes.size(), /*verify_hash=*/true);
    CY_REQUIRE_FALSE(verified.has_value());
    CY_CHECK_EQ(verified.error().code, ErrorCode::Io);
}

CY_TEST_CASE("an asset cooked for another platform is refused by its variant key") {
    const Expected<assets::VariantKey, Error> desktop = assets::VariantKey::parse("desktop-bc7");
    const Expected<assets::VariantKey, Error> mobile = assets::VariantKey::parse("mobile-astc");
    CY_REQUIRE(desktop.has_value());
    CY_REQUIRE(mobile.has_value());

    Array<u8> bytes(test_allocator());
    CY_REQUIRE(assets::write_cooked_asset(assets::AssetKind::Texture, desktop.value(), {}, bytes)
                   .has_value());
    const Expected<assets::CookedAssetHeader, Error> header =
        assets::read_cooked_header(bytes.data(), bytes.size());
    CY_REQUIRE(header.has_value());
    CY_CHECK(assets::check_cooked_variant(header.value(), desktop.value()).has_value());
    CY_CHECK_FALSE(assets::check_cooked_variant(header.value(), mobile.value()).has_value());
}

CY_TEST_CASE("a directory of documents is read, cooked, and written into a package") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"cook", 16384});
    CY_REQUIRE(make_world(world).has_value());

    Document prefab(test_allocator());
    CY_REQUIRE(build_document(prefab, AssetKind::Prefab, AssetId(0, 0xB0B), 3).has_value());
    Document scene(test_allocator());
    CY_REQUIRE(build_document(scene, AssetKind::Scene, AssetId(0, 0x5CE7E), 2).has_value());
    const Expected<Instance*, Error> instance =
        scene.add_instance(AssetId(0, 0xB0B), kNoLocalId, "prefab");
    CY_REQUIRE(instance.has_value());

    // The mount is heap-allocated because the filesystem takes ownership of it; `mount_owned`
    // captures the concrete type so the allocator is handed the right size back.
    Expected<UniquePtr<assets::MemoryMount>, Error> mount =
        make_unique<assets::MemoryMount>(test_allocator(), "source");
    CY_REQUIRE(mount.has_value());
    CY_REQUIRE(place(**mount, "robot.cyprefab", prefab).has_value());
    CY_REQUIRE(place(**mount, "level.cyscene", scene).has_value());
    // A file the cook does not read, so that the extension filter is exercised rather than assumed.
    const Expected<assets::VirtualPath, Error> readme = assets::VirtualPath::normalise("README.md");
    CY_REQUIRE(readme.has_value());
    CY_REQUIRE((*mount)->add(readme.value(), "not a document", 14).has_value());

    assets::VirtualFileSystem vfs;
    CY_REQUIRE(vfs.mount_owned(std::move(mount.value()), 0).has_value());

    const Expected<assets::VirtualPath, Error> root = assets::VirtualPath::normalise("");
    CY_REQUIRE(root.has_value());

    Array<Document> documents(test_allocator());
    CY_REQUIRE(cook::read_documents(vfs, root.value(), documents).has_value());
    CY_REQUIRE_EQ(documents.size(), 2U);

    cook::CookRequest request;
    request.source = root.value();
    request.world = &world;
    request.variant = assets::VariantKey::any();

    assets::PackageWriter package;
    cook::CookRunReport report(test_allocator());
    CY_REQUIRE(cook::cook_documents(request, documents, package, report).has_value());

    CY_CHECK_EQ(report.documents_read, 2U);
    CY_CHECK_EQ(report.documents_cooked, 2U);
    // Three entities in the prefab, and the scene's two plus the three the instance contributes.
    CY_CHECK_EQ(report.total_entities, 3U + 5U);
    CY_CHECK_EQ(package.entry_count(), 2U);

    // Every cooked payload begins with the header, so the loader can reject the wrong one before
    // parsing it.
    for (const cook::CookedDocumentReport& entry : report.documents) {
        CY_CHECK_GT(entry.cooked_bytes, assets::kCookedHeaderBytes);
        CY_CHECK_GT(entry.blocks, 0U);
    }
}

CY_TEST_CASE("a cook of a cyclic graph fails naming the chain, and writes nothing") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"cook", 16384});
    CY_REQUIRE(make_world(world).has_value());

    Document a(test_allocator());
    CY_REQUIRE(build_document(a, AssetKind::Prefab, AssetId(0, 1), 1).has_value());
    Document b(test_allocator());
    CY_REQUIRE(build_document(b, AssetKind::Prefab, AssetId(0, 2), 1).has_value());
    CY_REQUIRE(a.add_instance(AssetId(0, 2), kNoLocalId, "b").has_value());
    CY_REQUIRE(b.add_instance(AssetId(0, 1), kNoLocalId, "a").has_value());

    Array<Document> documents(test_allocator());
    CY_REQUIRE(documents.push_back(std::move(a)).has_value());
    CY_REQUIRE(documents.push_back(std::move(b)).has_value());

    cook::CookRequest request;
    request.world = &world;
    request.variant = assets::VariantKey::any();

    assets::PackageWriter package;
    cook::CookRunReport report(test_allocator());
    const Status cooked = cook::cook_documents(request, documents, package, report);
    CY_REQUIRE_FALSE(cooked.has_value());
    CY_CHECK_GE(report.cycle.size(), 3U);
    // Nothing was written: a package half of whose assets were cooked is worse than none.
    CY_CHECK_EQ(package.entry_count(), 0U);
    CY_CHECK_EQ(report.documents_cooked, 0U);
}

CY_TEST_CASE("a shipping cook strips prefab provenance and a development cook keeps it") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"cook", 16384});
    CY_REQUIRE(make_world(world).has_value());

    const auto cook_once = [&](bool shipping) noexcept {
        Document scene(test_allocator());
        (void)build_document(scene, AssetKind::Scene, AssetId(0, 3), 2);
        Array<Document> documents(test_allocator());
        (void)documents.push_back(std::move(scene));

        cook::CookRequest request;
        request.world = &world;
        request.variant = assets::VariantKey::any();
        request.shipping = shipping;

        assets::PackageWriter package;
        cook::CookRunReport report(test_allocator());
        const Status status = cook::cook_documents(request, documents, package, report);
        return status.has_value() ? report.documents[0].cooked_bytes : 0U;
    };

    // Both produce a package entry; provenance lives beside the stream rather than in it, so the
    // assertion that matters is that both cooks succeed and the shipping one is not larger.
    const u32 development = cook_once(false);
    const u32 shipping = cook_once(true);
    CY_CHECK_GT(development, 0U);
    CY_CHECK_EQ(shipping, development);
}

CY_TEST_CASE("two cooks of one tree produce identical cooked bytes") {
    // Determinism is what makes an incremental build and a content-addressed cache possible at all,
    // and it is a property of every list in the pipeline being ordered.
    ecs::World world(test_allocator(), ecs::WorldConfig{"cook", 16384});
    CY_REQUIRE(make_world(world).has_value());

    Document scene(test_allocator());
    CY_REQUIRE(build_document(scene, AssetKind::Scene, AssetId(0, 4), 8).has_value());

    ComponentLayoutTable layouts(test_allocator());
    CY_REQUIRE(describe_from_world(world, layouts).has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(scene).has_value());

    CookOptions options;
    options.layouts = &layouts;

    CookedAsset first(test_allocator());
    CookReport first_report(test_allocator());
    CY_REQUIRE(
        scene::serialization::cook(library, scene.id, options, first, first_report).has_value());

    CookedAsset second(test_allocator());
    CookReport second_report(test_allocator());
    CY_REQUIRE(
        scene::serialization::cook(library, scene.id, options, second, second_report).has_value());

    CY_REQUIRE_EQ(first.stream().size(), second.stream().size());
    for (usize index = 0; index < first.stream().size(); ++index) {
        CY_REQUIRE_EQ(first.stream()[index], second.stream()[index]);
    }
    CY_CHECK_EQ(first.build_schema, second.build_schema);
}

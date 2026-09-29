// SPDX-License-Identifier: MIT
// The `.cynavmesh` codec: a round trip keeps every tile digest, and a corrupt, unknown-version or
// truncated blob is refused with a named reason. Issue #28, task 1.6.

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/bake_codec.h>
#include <cy/navigation/tile_identity.h>
#include <cy/test/test.h>

#include <string_view>

#include "nav_fixture.h"

using namespace cy;
using namespace cy::navigation;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

constexpr f32 kTile = 8.0F;
/// magic, version, settings (7 f32, 2 u64, 1 u8), fingerprint, identity, tile count.
constexpr usize kHeaderBytes = 8 + 4 + 28 + 16 + 1 + 8 + 8 + 4;
constexpr usize kIdentityOffset = 8 + 4 + 28 + 16 + 1 + 8;
constexpr u64 kFingerprint = 0x1234'5678'9abc'def0ULL;

[[nodiscard]] NavBakeSettings settings() noexcept {
    NavBakeSettings out;
    out.tile_size = kTile;
    out.cell_size = 0.5F;
    out.agent_radius = 0.3F;
    out.layers = 0x0F;
    out.backend = NavBuildBackend::Engine;
    return out;
}

/// Two fixture tiles side by side, the second with a hole and a non-ground area.
[[nodiscard]] NavMesh two_tiles() noexcept {
    NavMesh mesh(allocator(), Name::intern("test.codec"), kTile);
    CY_REQUIRE(mesh.add_tile(testing::grid_tile(allocator(), TileCoord{1, 0, 0}, kTile, 4, 0.5F, 3))
                   .has_value());
    CY_REQUIRE(mesh.add_tile(testing::grid_tile_where(
                                 allocator(), TileCoord{0, 0, 0}, kTile, 4,
                                 [](u32 row, u32 column) noexcept { return row != column; }))
                   .has_value());
    return mesh;
}

[[nodiscard]] Array<u8> encoded(const NavMesh& mesh) noexcept {
    Array<u8> bytes(allocator());
    CY_REQUIRE(encode_nav_bake(settings(), kFingerprint, mesh, bytes).has_value());
    return bytes;
}

[[nodiscard]] bool mentions(const Error& error, std::string_view word) noexcept {
    return std::string_view(error.message).find(word) != std::string_view::npos;
}

/// The decode failed, and its reason names `word`.
[[nodiscard]] bool refused_for(const Expected<NavBakeAsset, Error>& decoded,
                               std::string_view word) noexcept {
    return !decoded.has_value() && mentions(decoded.error(), word);
}

}  // namespace

CY_TEST_CASE("a cynavmesh round trip keeps every tile digest and the bake identity") {
    const NavMesh mesh = two_tiles();
    const Array<u8> bytes = encoded(mesh);
    CY_REQUIRE(bytes.size() > kHeaderBytes);

    Expected<NavBakeAsset, Error> asset = decode_nav_bake(allocator(), bytes.span());
    CY_REQUIRE(asset.has_value());
    if (!asset.has_value()) {
        return;
    }
    CY_CHECK_EQ(asset->source_fingerprint, kFingerprint);
    CY_CHECK_EQ(asset->settings.tile_size, kTile);
    CY_CHECK_EQ(asset->settings.cell_size, 0.5F);
    CY_CHECK_EQ(asset->settings.agent_radius, 0.3F);
    CY_CHECK_EQ(asset->settings.layers, u64{0x0F});
    CY_CHECK_EQ(asset->settings.backend, NavBuildBackend::Engine);
    const Expected<u64, Error> identity = mesh_bake_identity(kFingerprint, mesh);
    CY_REQUIRE(identity.has_value());
    CY_CHECK_EQ(asset->bake_identity, *identity);

    // Stored in coordinate order: (0,0) first although it was published second.
    Array<u32> slots(allocator());
    CY_REQUIRE(ordered_tile_slots(mesh, slots).has_value());
    CY_REQUIRE_EQ(asset->tiles.size(), slots.size());
    CY_CHECK(asset->tiles[0].coord == (TileCoord{0, 0, 0}));
    for (usize index = 0; index < slots.size() && index < asset->tiles.size(); ++index) {
        CY_CHECK_EQ(tile_digest(asset->tiles[index]), mesh_tile_digest(mesh, slots[index]));
        CY_CHECK_EQ(asset->digests[index], mesh_tile_digest(mesh, slots[index]));
    }

    NavMesh loaded(allocator(), Name::intern("test.codec"), kTile);
    CY_REQUIRE(install_nav_bake(std::move(*asset), loaded).has_value());
    CY_CHECK_EQ(loaded.tile_count(), 2U);
    for (const u32 slot : slots.span()) {
        const u32 twin = loaded.tile_slot(mesh.tile_coord(slot));
        CY_REQUIRE_NE(twin, 0xFFFFFFFFU);
        CY_CHECK_EQ(mesh_tile_digest(loaded, twin), mesh_tile_digest(mesh, slot));
    }
}

CY_TEST_CASE("a cynavmesh with a flipped tile byte is refused as a digest mismatch") {
    const Array<u8> bytes = encoded(two_tiles());
    Array<u8> corrupt(allocator());
    CY_REQUIRE(corrupt.append(bytes.span()).has_value());
    // Inside the first tile's first vertex: past its coordinate, bounds and vertex count.
    constexpr usize kFlipped = kHeaderBytes + 12 + 24 + 4 + 1;
    corrupt[kFlipped] = static_cast<u8>(corrupt[kFlipped] ^ 0x40U);
    const Expected<NavBakeAsset, Error> refused = decode_nav_bake(allocator(), corrupt.span());
    CY_REQUIRE_FALSE(refused.has_value());
    if (!refused.has_value()) {
        CY_CHECK(mentions(refused.error(), "digest"));
    }
}

CY_TEST_CASE("a cynavmesh with a changed identity, magic or trailing byte is refused") {
    const Array<u8> bytes = encoded(two_tiles());
    Array<u8> copy(allocator());

    CY_REQUIRE(copy.append(bytes.span()).has_value());
    copy[kIdentityOffset] = static_cast<u8>(copy[kIdentityOffset] ^ 0x01U);
    const Expected<NavBakeAsset, Error> identity = decode_nav_bake(allocator(), copy.span());
    CY_REQUIRE_FALSE(identity.has_value());
    CY_CHECK(refused_for(identity, "identity"));

    copy.clear();
    CY_REQUIRE(copy.append(bytes.span()).has_value());
    copy[0] = static_cast<u8>('X');
    const Expected<NavBakeAsset, Error> magic = decode_nav_bake(allocator(), copy.span());
    CY_CHECK(refused_for(magic, "magic"));

    copy.clear();
    CY_REQUIRE(copy.append(bytes.span()).has_value());
    CY_REQUIRE(copy.push_back(u8{0}).has_value());
    const Expected<NavBakeAsset, Error> trailing = decode_nav_bake(allocator(), copy.span());
    CY_CHECK(refused_for(trailing, "trailing"));
}

CY_TEST_CASE("a cynavmesh of an unknown version is refused") {
    Array<u8> bytes = encoded(two_tiles());
    bytes[8] = static_cast<u8>(kNavBakeFormatVersion + 1U);
    const Expected<NavBakeAsset, Error> refused = decode_nav_bake(allocator(), bytes.span());
    CY_REQUIRE_FALSE(refused.has_value());
    if (!refused.has_value()) {
        CY_CHECK_EQ(refused.error().code, ErrorCode::Unsupported);
        CY_CHECK(mentions(refused.error(), "version"));
    }
}

CY_TEST_CASE("a truncated cynavmesh is refused at every cut") {
    const Array<u8> bytes = encoded(two_tiles());
    const usize cuts[] = {0,
                          7,
                          11,
                          40,
                          kHeaderBytes - 1,
                          kHeaderBytes,
                          kHeaderBytes + 20,
                          bytes.size() / 2,
                          bytes.size() - 1};
    for (const usize cut : cuts) {
        const Expected<NavBakeAsset, Error> refused =
            decode_nav_bake(allocator(), Span<const u8>(bytes.data(), cut));
        CY_CHECK_FALSE(refused.has_value());
        CY_CHECK(refused_for(refused, "truncated"));
    }
}

CY_TEST_CASE("installing a cynavmesh into a mesh of another tile size is refused") {
    Expected<NavBakeAsset, Error> asset = decode_nav_bake(allocator(), encoded(two_tiles()).span());
    CY_REQUIRE(asset.has_value());
    if (!asset.has_value()) {
        return;
    }
    NavMesh other(allocator(), Name::intern("test.codec"), kTile * 2.0F);
    CY_CHECK_FALSE(install_nav_bake(std::move(*asset), other).has_value());
}

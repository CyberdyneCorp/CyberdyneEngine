// SPDX-License-Identifier: MIT
#pragma once
// The `.cynavmesh` asset: a saved bake. Issue #28, task 1.6 of
// `implement-issue-28-navigation-authoring`.
//
// Layout, little-endian throughout:
//
//   header   "CYNAVMSH" (8 bytes), u32 version
//            settings: 7 x f32 (agent radius, agent height, max slope, step height, cell size,
//            cell height, tile size), u64 layers, u64 tags, u8 backend
//            u64 source fingerprint, u64 bake identity, u32 tile count
//   tile     i32 x, i32 z, i32 layer, 6 x f32 bounds (min, max)
//            u32 vertex count, 3 x f32 per vertex
//            u32 poly count, per poly: u32 first corner, u8 corner count, u8 area, f32 cost,
//            3 x f32 centre
//            u32 corner count, u32 per corner
//            u64 tile digest
//
// Tiles are stored in `ordered_tile_slots` order. Decoding re-derives every tile digest and the
// bake identity and refuses the blob on any mismatch, an unknown version, a bad magic, a truncated
// or oversized payload, or a polygon that names a corner or vertex the tile does not have.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/navigation/bake.h>
#include <cy/navigation/navmesh.h>

namespace cy::navigation {

/// The only version this build writes and reads.
inline constexpr u32 kNavBakeFormatVersion = 1;

/// A decoded `.cynavmesh`.
struct NavBakeAsset {
    NavBakeSettings settings;
    u64 source_fingerprint = 0;
    u64 bake_identity = 0;
    Array<NavTileData> tiles;
    /// `tile_digest` of each tile, verified against the stored one.
    Array<u64> digests;

    explicit NavBakeAsset(Allocator& allocator) noexcept : tiles(allocator), digests(allocator) {}
};

/// Encodes every resident tile of `mesh` with `settings` and `fingerprint`. The stored identity is
/// `mesh_bake_identity(fingerprint, mesh)`. `out` is cleared first.
[[nodiscard]] Status encode_nav_bake(const NavBakeSettings& settings, u64 fingerprint,
                                     const NavMesh& mesh, Array<u8>& out) noexcept;

/// Decodes and verifies a `.cynavmesh`. The error message names the reason.
[[nodiscard]] Expected<NavBakeAsset, Error> decode_nav_bake(Allocator& allocator,
                                                            Span<const u8> bytes) noexcept;

/// Publishes every tile of a decoded asset into `mesh`, whose tile size must match the asset's.
[[nodiscard]] Status install_nav_bake(NavBakeAsset&& asset, NavMesh& mesh) noexcept;

}  // namespace cy::navigation

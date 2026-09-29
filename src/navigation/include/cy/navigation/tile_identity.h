// SPDX-License-Identifier: MIT
#pragma once
// Tile digests, the source fingerprint and the bake identity. Issue #28, tasks 1.2 and 1.5 of
// `implement-issue-28-navigation-authoring`.
//
// Every value here is a 64-bit FNV-1a over a fixed little-endian encoding of its inputs, so it is
// the same on every platform and in every process (the engine's `hash_bytes` is seeded per process
// and cannot be stored). Floats are hashed by their bits, with -0 folded into +0.
//
// * `tile_digest` names a tile's content: coordinate, bounds, vertices, polygons and their corners.
//   `NavBuildReport::duration_ns` is wall time and is never part of it.
// * `source_fingerprint` names a bake's inputs. The editor compares it with the saved one to show a
//   stale bake; it never hashes geometry itself.
// * `bake_identity` names a bake: the fingerprint and the ordered tile digests.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/navigation/bake.h>
#include <cy/navigation/navmesh.h>

namespace cy::navigation {

/// The digest of a built tile, as `build_tile` returns it or as a decoded asset holds it.
[[nodiscard]] u64 tile_digest(const NavTileData& tile) noexcept;

/// The same digest computed from the tile resident in `slot`, without copying it out. Zero when the
/// slot is not resident.
[[nodiscard]] u64 mesh_tile_digest(const NavMesh& mesh, u32 slot) noexcept;

/// Hashes the vertices, the indices, each triangle's layer, tag and area (defaults filled in), the
/// surface and area volumes, the settings, the back end and `producer_version`. The source's
/// obstacles and links are excluded: they are applied without a rebuild.
[[nodiscard]] u64 source_fingerprint(const NavBakeSettings& settings, const NavBakeSource& source,
                                     u32 producer_version) noexcept;

/// `hash(fingerprint, digests...)`, over the digests in the order given. A bake lists its tiles by
/// layer, z and x, which is the order `ordered_tile_slots` returns.
[[nodiscard]] u64 bake_identity(u64 fingerprint, Span<const u64> ordered_digests) noexcept;

/// The resident slots of `mesh`, ordered by layer, z and x.
[[nodiscard]] Status ordered_tile_slots(const NavMesh& mesh, Array<u32>& out) noexcept;

/// `bake_identity` over every resident tile of `mesh`, in `ordered_tile_slots` order.
[[nodiscard]] Expected<u64, Error> mesh_bake_identity(u64 fingerprint,
                                                      const NavMesh& mesh) noexcept;

}  // namespace cy::navigation

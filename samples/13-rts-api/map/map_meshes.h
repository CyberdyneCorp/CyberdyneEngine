// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_MAP_MESHES_H
#define CY_SAMPLE_RTS_MAP_MESHES_H
// map_meshes.h — the map's meshes, built in code at cook time: every building, resource, tree and
// prop, and the skinned worker.
//
// NO MESH IS A FILE. Each is a few primitives — boxes, cylinders, cones, gable roofs and noisy
// spheres — faceted and in parts, one material a part, the way a low-polygon strategy game's
// kit is. The cook writes the result as a cooked mesh library (map_format.h), so what the runtime
// draws is what was cooked and not this code. Licensed as the repository is; there is nothing
// third-party in it (deps/shipped-content.md).
//
// The worker is skinned to the rig samples/13-rts-api/host/worker_rig.h cooks — eight joints at
// the same bind positions — one rigid influence a vertex, so the arms and legs swing whole as the
// walk clip turns their joints.

#include <cy/core/base/expected.h>

#include "map_format.h"

namespace sample::rts::map {

/// The names `build_mesh_library` makes, which a source's placements and species name.
inline constexpr const char* kLibraryMeshes[] = {
    "barracks",  "keep",      "house",     "well",        "watchtower", "beacon",
    "crate",     "banner",    "tent",      "fence",       "crops",      "dock",
    "gold_mine", "gold_rock", "rock_pile", "log_pile",    "outcrop",    "boulder",
    "tree_pine", "tree_oak",  "bush",      "water_plane",
};

/// The worker's joints, in the rig's order, and their bind positions in model metres.
inline constexpr u32 kWorkerJointCount = 8;
inline constexpr Vec3 kWorkerBind[kWorkerJointCount] = {
    Vec3{0.0F, 0.0F, 0.0F},   Vec3{0.0F, 0.9F, 0.0F},   Vec3{0.0F, 1.2F, 0.0F},
    Vec3{0.0F, 1.7F, 0.0F},   Vec3{-0.25F, 1.6F, 0.0F}, Vec3{0.25F, 1.6F, 0.0F},
    Vec3{-0.12F, 0.9F, 0.0F}, Vec3{0.12F, 0.9F, 0.0F},
};

/// Build every mesh of `kLibraryMeshes` into `out`, with materials resolved against `layout`.
/// `terrain` shapes the water: the lake's surface covers the cells below the water line.
[[nodiscard]] cy::Status build_mesh_library(const MapLayout& layout, const TerrainData& terrain,
                                            const Vec3& water_origin, MeshLibrary& out);

/// Build the skinned worker. Its parts use the layout's "team", "skin", "trousers", "straw" and
/// "wood" materials.
[[nodiscard]] cy::Status build_worker(const MapLayout& layout, SkinnedMesh& out);

}  // namespace sample::rts::map

#endif  // CY_SAMPLE_RTS_MAP_MESHES_H

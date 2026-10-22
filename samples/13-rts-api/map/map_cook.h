// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_MAP_COOK_H
#define CY_SAMPLE_RTS_MAP_COOK_H
// map_cook.h — the RTS test map's cook: a `.cymap` in, a `.cypak` out.
//
//   parse       content/rts/frontier.cymap (map_source.h)
//   terrain     the generator and the modifier stack — mounds, the carved lake, flattened ground,
//               roads, brush strokes — evaluated, stitched, meshed and collided by
//               `terrain::evaluate_region`, the call the editor's terrain tools preview with
//   bake        the texels' painted layers, the slope's rock and the shore's sand into one sRGB
//               image, its detail from content/beauty's stone and gravel and from value noise
//   foliage     `foliage::generate_region` over the map's clusters, on a `TerrainQuery` over the
//               cooked tiles, with the source's species, rules and exclusions
//   kit         every building, resource and plant mesh, and the skinned worker (map_meshes.h)
//   package     each asset in the cooked-asset envelope, into one `.cypak` the loader mounts
//
// The cook is a PURE FUNCTION of the source and the content files: two cooks of one source write
// byte-identical packages, and `integration.rts_map` holds that.

#include <cy/core/base/expected.h>

#include <string>
#include <vector>

#include "map_format.h"

namespace sample::rts::map {

/// What a cook produced and how long each step took.
struct CookReport {
    u32 modifiers = 0;
    u32 terrain_edge = 0;
    u32 ground_triangles = 0;
    u32 painted_texels = 0;
    u32 albedo_edge = 0;
    u32 meshes = 0;
    u32 placements = 0;
    u32 foliage = 0;
    u32 foliage_clusters = 0;
    u64 foliage_bytes = 0;
    /// What placement considered and why it refused: the engine's `PlacementDiagnostic`, summed
    /// over the clusters, and the instances a road's declared decoration removed.
    u32 foliage_candidates = 0;
    u32 foliage_rejected_weight = 0;
    u32 foliage_rejected_spacing = 0;
    u32 foliage_rejected_exclusion = 0;
    u32 foliage_on_roads = 0;
    u32 worker_vertices = 0;
    u64 package_bytes = 0;
    f64 terrain_ms = 0.0;
    f64 bake_ms = 0.0;
    f64 foliage_ms = 0.0;
    f64 total_ms = 0.0;
};

/// Where the cook reads and writes.
struct CookRequest {
    /// The `.cymap`.
    std::string source;
    /// What a source's texture paths are relative to: the repository root.
    std::string content_root;
    /// The `.cypak` to write. Empty cooks without writing, for a test that wants the content.
    std::string package;
    /// The baked albedo's edge, in texels. A power of two.
    u32 albedo_edge = 2048;
};

/// Cook `request.source`. `out` is the content the package holds, as the loader will read it back.
[[nodiscard]] cy::Status cook_map(const CookRequest& request, MapContent& out, CookReport& report,
                                  std::string& detail);

}  // namespace sample::rts::map

#endif  // CY_SAMPLE_RTS_MAP_COOK_H

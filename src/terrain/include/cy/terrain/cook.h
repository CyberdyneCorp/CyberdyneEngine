#pragma once
// The cook side of terrain: a meshed tile becoming virtual geometry clusters, and the terrain
// surface's own MATERIAL GRAPH compiled by the material compiler like any other material.
// M10 task 2.1.
//
// ================================================================================================
// WHY THIS IS A SECOND TARGET
// ================================================================================================
//
// Everything here names the renderer — `rendering::vg` and `rendering::material` — and nothing in
// the runtime half of terrain does. A dedicated server that meshes terrain for collision, and a
// cooker that never opens a device, should not link a material compiler and a cluster builder
// between them; `cy::ml-cook` is split from `cy::ml` for the same reason and says so in its own
// CMakeLists.txt.
//
// ================================================================================================
// THE TWO CLAIMS THIS FILE EXISTS TO MAKE CHECKABLE
// ================================================================================================
//
// 1. "TERRAIN IS A GEOMETRY SOURCE. There SHALL NOT be a terrain-specific renderer, a
//    terrain-specific level-of-detail morphing scheme, or a terrain-specific shadow path."
//
//    `build_tile_geometry()` fills a `rendering::vg::SourceMesh` and calls
//    `rendering::vg::build_geometry()`. That call is the whole of terrain's rendering path, and it
//    is the same call a static mesh makes with the same options — so the negative claim is a
//    property of this file's length rather than of a promise: there is nowhere else for a terrain
//    renderer to be.
//
// 2. "Terrain shading SHALL go through the material compiler like any other material; A MONOLITHIC
//    TERRAIN SHADER CARRYING EVERY LAYER AND EVERY FEATURE SHALL NOT BE PRODUCED", and "each texel
//    SHALL store only its dominant few, and SHADING COST SHALL NOT SCALE WITH THE LAYER COUNT of
//    the region."
//
//    `build_terrain_material()` authors a `rendering::material::MaterialGraph` whose closure count
//    is the PER-TEXEL BOUND and not the world's layer count, and whose environment inputs are
//    `GraphOp::Field` nodes — the compiler's own first-class field sampling, which
//    `CompileOptions::check_fields` then verifies against the project's declared fields. A terrain
//    with four layers and a terrain with forty produce the same graph, and the suite compiles both
//    and compares the program counts.

#include <cy/core/base/expected.h>
#include <cy/core/values/name.h>
#include <cy/rendering/material/compiler.h>
#include <cy/rendering/material/graph.h>
#include <cy/rendering/virtual_geometry/build.h>
#include <cy/terrain/material.h>
#include <cy/terrain/meshing.h>

namespace cy::terrain {

/// The source mesh a terrain tile hands the engine's geometry builder. Non-owning: every span
/// points into the `TerrainMesh` and must outlive the call.
[[nodiscard]] rendering::vg::SourceMesh source_mesh_of(const TerrainMesh& mesh) noexcept;

/// Build one tile's virtual geometry. Nothing here is terrain-specific but the input.
[[nodiscard]] Expected<rendering::vg::GeometryBuild, Error> build_tile_geometry(
    Allocator& allocator, const TerrainMesh& mesh,
    const rendering::vg::BuildOptions& options) noexcept;

/// What a terrain surface material blends and reads.
struct TerrainMaterialDescription {
    /// The per-texel bound. THE NUMBER OF CLOSURES THE GRAPH CARRIES, and the only thing about the
    /// graph that depends on how many layers exist anywhere.
    u32 layers_per_texel = 2;
    /// The environment fields the surface reads, by name, as `GraphOp::Field` nodes. `wetness`
    /// darkening the albedo and `snow-depth` whitening it are the two the specification's own
    /// examples need.
    Span<const Name> fields;
    /// How many material layers the terrain has in total. Recorded so a diagnostic can report it,
    /// and DELIBERATELY not used to size anything.
    u32 layers_in_world = 0;
};

/// Author the terrain surface material into an ordinary material graph.
///
/// The graph is an OUT PARAMETER rather than a return value because
/// `rendering::material::MaterialGraph` declares a deleted copy constructor and therefore has no
/// implicit move one — it is built in place, by its owner, exactly as a graph front end builds one.
/// The caller constructs it with its own allocator and the material's name, which is also what lets
/// a project author its own terrain material and pass it here to be extended.
[[nodiscard]] Status build_terrain_material(rendering::material::MaterialGraph& graph,
                                            const TerrainMaterialDescription& description) noexcept;

}  // namespace cy::terrain

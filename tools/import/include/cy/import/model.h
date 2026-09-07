#ifndef CY_IMPORT_MODEL_H
#define CY_IMPORT_MODEL_H
// The half of a model import that does not depend on the source FORMAT. M6 task 8.1.
//
// `asset-import-pipeline` — "Model import" fixes ten steps in a defined order, and only the first
// of them is about the file on disk. Steps 2 to 6 and 10 — build meshes split by material, generate
// what is missing, optimise, generate the level-of-detail chain, generate collision by the naming
// convention, and produce the hierarchy — are the same work whether the bytes arrived as glTF, as
// FBX or, later, as USD.
//
// ================================================================================================
// WHY THIS FILE EXISTS, AND WHAT WOULD HAVE HAPPENED WITHOUT IT
// ================================================================================================
//
// At M5 there was one model importer, so its post-processing lived inside it. M6 adds a second, and
// the alternative to this file is six hundred lines of welding, tangent generation, chain
// simplification and collision derivation copied into `fbx.cpp` — where it would drift. The failure
// that follows is specific and expensive: the same mesh imported from a glTF and from an FBX would
// weld to different vertex counts, and a project that re-exported one model in the other format
// would find every downstream `AssetId` rebound and every cooked artefact different.
//
// So the format-specific half of an importer is: parse, convert to the engine's conventions, and
// hand `MeshData` and a node list to the functions below. `gltf.cpp` and `fbx.cpp` both do exactly
// that, and the ORDER of the steps is fixed here rather than in either of them.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/import/gltf.h>
#include <cy/import/importer.h>
#include <cy/import/mesh.h>

#include <string>
#include <string_view>
#include <vector>

namespace cy::import {

/// What a model importer's shared steps are configured by.
///
/// Every field is the value of a declared option — the two importers' schemas both carry them under
/// the same names, deliberately, so that a project that switches a model from FBX to glTF keeps its
/// import settings. A field here that no schema declares would be a setting that changes the output
/// and cannot reach the derivation key, which `options.h` calls the one defect a cook cache cannot
/// survive.
struct ModelBuildOptions {
    f32 weld_tolerance = 1.0e-5f;
    f32 smoothing_angle = 60.0f;
    bool generate_tangent_basis = true;
    bool optimise = true;
    /// The overdraw budget, as the share of the vertex-cache miss ratio the reorder may give up.
    /// 1.0 forbids any regression, which in practice disables the step.
    f32 overdraw_threshold = 1.05f;
    /// Whether to generate the lightmap coordinate set. Off by default because it costs an unwrap
    /// per mesh, and a project that bakes no lightmaps should not pay for one.
    bool generate_lightmap_uvs = false;
    Uv2Options uv2;
    i64 lod_count = 0;
    f32 lod_ratio = 0.5f;
    f32 lod_error_bound = 0.0f;
    std::string_view collision_suffix = "_collision";
    /// One of "none", "convex", "decompose" or "triangle".
    std::string_view collision_mode = "convex";
};

/// Names that are unique within one import.
///
/// A sub-asset's name is what its `AssetId` is bound to across re-imports, so two nodes called
/// "Cube" must not produce one name — the second would take the first's id and the first would be
/// re-minted on the next import.
class SubAssetNames {
public:
    /// A name of the form `<prefix><name>`, suffixed with `.1`, `.2`, … until it is unused. An
    /// empty `name` becomes `unnamed-<fallback_index>`, which is stable because the index is the
    /// source's own ordering of the thing that has no name.
    [[nodiscard]] std::string unique(std::string_view prefix, std::string_view name,
                                     usize fallback_index);

private:
    std::vector<std::string> taken_;
};

/// True when `text` ends with `suffix`. The collision naming convention's whole test.
[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) noexcept;

/// Steps 3, 2 and 4, in the order `asset-import-pipeline` fixes: generate the normals the source
/// did not supply, weld within the tolerances, generate the tangent basis, then optimise for the
/// post-transform vertex cache, for overdraw and for vertex fetch — and, when it is asked for,
/// unwrap the lightmap coordinate set.
///
/// THE UNWRAP RUNS BEFORE THE OPTIMISERS AND AFTER THE WELD, which is the only order that works:
/// unwrapping splits vertices, so a fetch order computed before it would be discarded; and welding
/// after it would merge the split the unwrap just made.
///
/// Diagnostics about the mesh go to `out` under `subject`; a returned error is about the machine.
[[nodiscard]] Status finish_mesh(MeshData& mesh, const ModelBuildOptions& options,
                                 ImportResult& out, std::string_view subject) noexcept;

/// Step 5: emit the mesh as a cooked sub-asset, followed by its level-of-detail chain.
///
/// Each level is simplified from the level above it rather than from the full-detail mesh, so a
/// chain of 0.5 ratios halves, quarters and eighths. `name` is the mesh's stable sub-asset name;
/// the levels take `<name>/lod<n>`.
[[nodiscard]] Status emit_mesh_with_lods(const MeshData& mesh, std::string_view name,
                                         const ModelBuildOptions& options,
                                         ImportResult& out) noexcept;

/// Step 6: emit the collision representation of one node, derived from the SOURCE mesh.
///
/// "WHEN a 50-million-triangle asset is cooked THEN its collision representation SHALL be generated
/// from the source mesh to its own budget, and SHALL NOT derive from the render clusters." So this
/// takes the mesh as it was built, never a level of detail, and it is the caller's job to pass the
/// former.
///
/// `collision-mode` decides the shape: `convex` is one hull, `decompose` is a bounded set of hulls
/// emitted as `<name>/part<n>`, and `triangle` keeps the topology with every render attribute
/// stripped. Returns how many sub-assets were emitted, which is zero when the mode is `none`.
[[nodiscard]] Expected<usize, Error> emit_collision(const MeshData& source, std::string_view name,
                                                    const ModelBuildOptions& options,
                                                    ImportResult& out) noexcept;

// --- The standard material record ---------------------------------------------------------------

/// The cooked material payload's format version. Moved when the layout changes.
inline constexpr u32 kCookedMaterialVersion = 1;

/// The standard material's parameters, as every model importer writes them.
///
/// `asset-import-pipeline` — "Import materials, mapping source parameters to the standard
/// material". WHICH standard material is the point: glTF's metallic-roughness and FBX's PBR maps
/// are two spellings of one model, and if each importer wrote its own record the same asset
/// re-exported from one format to the other would cook to different bytes and rebind every
/// downstream reference. So the record is here, both importers fill it, and one function writes it.
///
/// Texture references are NOT in the record. They are resolved by `AssetId` through the asset
/// database, which is what makes "a texture referenced by an imported material moves and no
/// re-import is needed" true.
struct StandardMaterial {
    f32 base_colour[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    f32 metallic = 1.0f;
    f32 roughness = 1.0f;
    f32 emissive[3] = {0.0f, 0.0f, 0.0f};
    /// 0 opaque, 1 masked against `alpha_cutoff`, 2 blended.
    u32 alpha_mode = 0;
    f32 alpha_cutoff = 0.5f;
    bool double_sided = false;
};

/// Write the record, little-endian, as a pure function of the parameters.
[[nodiscard]] Status write_cooked_material(const StandardMaterial& material,
                                           Array<u8>& out) noexcept;

/// Read one back. For the tests, and for a tool that inspects a package.
[[nodiscard]] Status read_cooked_material(Span<const u8> payload, StandardMaterial& out) noexcept;

/// Step 10: write the node table as the cooked scene graph, and add it as the import's PRIMARY
/// sub-asset. Every model import ends with this call.
[[nodiscard]] Status emit_prefab(Span<const ImportedNode> nodes, ImportResult& out) noexcept;

}  // namespace cy::import

#endif  // CY_IMPORT_MODEL_H

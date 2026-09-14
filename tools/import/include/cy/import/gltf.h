#ifndef CY_IMPORT_GLTF_H
#define CY_IMPORT_GLTF_H
// glTF 2.0 import, and the cooked payloads a model import produces. M5 task 5.1.
//
// `asset-import-pipeline` — "Model import": "The model importer SHALL support **glTF 2.0**
// (`.gltf`, `.glb`) as the primary interchange format ... producing meshes, materials, textures,
// skeletons, animations, and a scene hierarchy as a prefab", in a defined order of ten steps.
//
// --- WHAT IS IMPLEMENTED, AND WHAT IS DECLARED AND NOT ------------------------------------------
//
// Steps 1-6 and 10 of that list are here: parse and convert to the engine's conventions, build
// meshes split by material, generate missing normals and tangents, weld, optimise for the vertex
// cache and for fetch, generate an LOD chain to configured targets, generate collision from the
// naming convention, and produce the hierarchy.
//
// STEPS 7 AND 8 ARRIVED AT M11.b, and what they replaced is worth stating because the sentence that
// stood here for six milestones was a refusal: "there is nothing to import a skeleton INTO before
// M8". There has been since M8.b, and M8.d built both steps for FBX. This importer now reads
// `skins` into the same `ImportedSkeleton` record (`cy/import/fbx_skeleton.h`), `JOINTS_0` and
// `WEIGHTS_0` into `MeshData::skin`, and `animations` into `cy::animation::Clip` through the same
// quantised codec and the same cooked record (`cy/import/clip_record.h`) — so one character
// exported as glTF and as FBX cooks to one skeleton and one clip rather than two of each.
//
// TWO NUMBERINGS, AND THE MAP BETWEEN THEM, is the part of step 7 that is glTF's alone. A skin owns
// a `joints` array and a mesh's `JOINTS_0` addresses a SLOT in it; `Skeleton::add_joint` refuses a
// parent index that is not smaller than the child's and glTF requires no such ordering, nor does it
// require a skin to list the ancestors its bind poses depend on. So the cooked record is built by a
// hierarchy walk and every influence is remapped through `GltfRig::slot_to_joint`. Getting it wrong
// is not a crash: it is a character whose left arm moves when its right leg does.
//
// Step 9 — material extraction as separately editable assets — remains absent, and so do morph
// targets, whose channels are counted and named rather than dropped in silence.
//
// USD as a tool-time-only importer is absent, for the same reason as meshoptimizer: the dependency
// is not integrated. The interface it slots into is `Importer`, and nothing about adding it touches
// this file.
//
// --- THE TWO COOKED PAYLOADS ---------------------------------------------------------------------
//
// A model import produces cooked MESHES and a cooked SCENE GRAPH, and both formats are defined here
// rather than borrowed:
//
//   * The cooked mesh is the importer's own compact form. It is deliberately not
//     `rendering-geometry-and-resources`' GPU layout — that is interleaved, quantised and split
//     into streams for a particular renderer, and an importer that wrote it would have to be
//     rewritten the first time the renderer's vertex format changed.
//   * The cooked scene graph is a flat node table. It is NOT a `cy::scene` prefab: a prefab is a
//     layer-4 concept over an ECS world, and a layer-7 tool that constructed one would have to link
//     the scene module and instantiate a world to write a file. The cook step (tools/cook/) is
//     where a scene graph becomes a prefab, and this is the record it reads.
//
// Both are little-endian, versioned, and read back by the tests that write them.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/import/importer.h>
#include <cy/import/mesh.h>

#include <string_view>

namespace cy::import {

/// The cooked mesh payload's format version. Moved when the layout changes.
///
/// 2 at M11.b: a cooked mesh may now carry a skin binding per vertex (`MeshAttributes::Skin`). The
/// version moves rather than the bit being added silently, because a reader of version 1 computes
/// its payload length from the attribute set and would accept a version-2 skinned mesh as a
/// truncated one — an error with a misleading message instead of a refusal.
inline constexpr u32 kCookedMeshVersion = 2;

/// Which attribute arrays a cooked mesh carries, after its positions.
enum class MeshAttributes : u32 {
    None = 0,
    Normals = 1U << 0U,
    TexCoords = 1U << 1U,
    TexCoords2 = 1U << 2U,
    Tangents = 1U << 3U,
    /// Four joint indices and four weights per vertex: `MeshData::skin`. M11.b.
    Skin = 1U << 4U,
};

[[nodiscard]] constexpr MeshAttributes operator|(MeshAttributes a, MeshAttributes b) noexcept {
    return static_cast<MeshAttributes>(static_cast<u32>(a) | static_cast<u32>(b));
}

[[nodiscard]] constexpr bool has(MeshAttributes set, MeshAttributes wanted) noexcept {
    return (static_cast<u32>(set) & static_cast<u32>(wanted)) != 0;
}

/// Write a mesh as a cooked payload: a header, the attribute arrays, the indices, the sections.
///
/// The bytes are a pure function of the mesh, so two cooks of identical input produce identical
/// output — which is `asset-import-pipeline`'s determinism requirement, and is the property the
/// cook cache's content addressing rests on.
[[nodiscard]] Status write_cooked_mesh(const MeshData& mesh, Array<u8>& out) noexcept;

/// Read a cooked mesh back. For the tests, for a tool that inspects a package, and for the cook
/// step that turns these into GPU-ready buffers.
[[nodiscard]] Status read_cooked_mesh(Span<const u8> payload, MeshData& out) noexcept;

/// The cooked scene graph's format version.
inline constexpr u32 kCookedSceneGraphVersion = 1;

/// One node of an imported hierarchy.
struct ImportedNode {
    /// The node's own name, from the source. Owned by the caller's storage.
    std::string_view name;
    /// The index of the parent node, or -1 for a root. Always less than this node's own index, so a
    /// reader can build the hierarchy in one forward pass.
    i32 parent = -1;
    Vec3 translation{0.0f, 0.0f, 0.0f};
    Quat rotation;
    Vec3 scale{1.0f, 1.0f, 1.0f};
    /// The index of the sub-asset this node draws, or -1 when it draws nothing.
    i32 mesh = -1;
    /// The index of the collision sub-asset this node contributes, or -1.
    i32 collision = -1;
    /// True when the node is a collision proxy and must not be rendered. Set by the naming
    /// convention; see the `collision-suffix` option.
    bool collision_only = false;
};

/// Write a hierarchy as a cooked payload.
[[nodiscard]] Status write_cooked_scene_graph(Span<const ImportedNode> nodes,
                                              Array<u8>& out) noexcept;

/// Read one back, appending to `out_nodes` and the names into `out_names`.
///
/// The names arrive in one blob and each node's `name` points into it, so the caller keeps
/// `out_names` alive for as long as it reads the nodes.
[[nodiscard]] Status read_cooked_scene_graph(Span<const u8> payload, Array<ImportedNode>& out_nodes,
                                             Array<char>& out_names) noexcept;

/// The glTF importer's option schema.
[[nodiscard]] OptionsSchema gltf_options() noexcept;

/// The built-in glTF importer, for `.gltf` and `.glb`.
///
/// Holds no state, so one instance serves every worker.
class GltfImporter final : public Importer {
public:
    [[nodiscard]] ImporterInfo info() const noexcept override;
    [[nodiscard]] OptionsSchema schema() const noexcept override;
    [[nodiscard]] Status import(const ImportRequest& request, ImportResult& out) noexcept override;
};

}  // namespace cy::import

#endif  // CY_IMPORT_GLTF_H

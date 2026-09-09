#ifndef CY_IMPORT_OBJ_H
#define CY_IMPORT_OBJ_H
// Wavefront OBJ import, with its companion `.mtl`. M8.a tasks 3.2 and 3.3.
//
// `asset-import-pipeline` — "Model import":
//
// > **OBJ** (`.obj`, with its companion `.mtl`) SHALL be supported. It carries no rig, no animation
// > and no scene graph, so it exercises only steps 1 to 6 and 9 of the sequence below and SHALL
// > report the steps it did not reach rather than appearing to have performed them. It is supported
// > because it is the format a mesh arrives in when it came from anywhere at all — a sculpt, a
// > scan, a generator, a thirty-year-old archive — and an engine that cannot open one cannot be
// > handed a model by a stranger.
//
// OBJ was in NO specification in this project until `split-m8-authorable-and-systems` added that
// paragraph, which is how it came to be missing rather than declined. glTF and FBX were specified
// at M5 and implemented at M5 and M6.
//
// ================================================================================================
// WHAT THIS IMPORTER OWNS, AND WHAT IT SHARES
// ================================================================================================
//
// It owns exactly one thing: turning text into `MeshData` and `StandardMaterial`. Everything after
// that — welding, normal and tangent generation, the optimisers, the level-of-detail chain,
// collision from the naming convention — is `model.h`'s and is the SAME CODE the glTF and FBX
// importers run. `model.h` states why that sharing is load-bearing rather than tidy: without it,
// one mesh exported to two formats welds to two vertex counts, and a project that re-exported a
// model would find every downstream `AssetId` rebound.
//
// There is no third-party parser here and `deps/manifest.toml` gains nothing. OBJ is a line-based
// ASCII format with eight statements that matter, and a dependency for it would be larger than the
// parser. That is a different judgement from ufbx's and from xatlas's, and it is a judgement about
// this format rather than a policy.
//
// ================================================================================================
// THE THREE STEPS IT DOES NOT REACH, AND WHY NONE OF THEM IS A WARNING
// ================================================================================================
//
// Steps 7 (skeletons), 8 (animations) and 10 (a prefab of the hierarchy) are not reached, and the
// reasons are different in kind:
//
//   * 7 and 8 are absent from every model importer in this build, because `animation-and-skinning`
//     reaches Working at M8 and there is nothing to import a rig INTO before it. `gltf.h` gives the
//     whole argument.
//   * 10 is absent because **OBJ has no hierarchy to produce one from**. There are no nodes, no
//     transforms and no parents in the format: an `o` statement names a group of faces and says
//     nothing about where it is. An importer that emitted a flat prefab of identity transforms
//     would be inventing a scene graph the file does not contain, and the next system to read it
//     could not tell the invention from an author's intent.
//
// None of the three is reported as a warning. `asset-import-pipeline` now states the rule
// generally — "A step skipped for that reason is not a warning about the file and SHALL NOT be
// reported as one" — and `ImporterInfo::steps` is the mechanism: the set is declared, and
// `ImportReport::format` names what is missing from it under its own heading. See `ModelImportStep`
// in `importer.h` for why the declaration lives on the importer rather than in a per-import
// diagnostic (a cache hit must report the same thing a miss does).
//
// ================================================================================================
// ONE DERIVATION KEY AND ONE CACHE, WHICH IS TASK 3.4 AND WAS M6'S DEFECT
// ================================================================================================
//
// This importer builds no key of its own. `import_derivation_key` is the one function every import
// in this tree keys through, it contributes `assets::current_toolchain()`, and it fails on an
// incomplete fingerprint rather than defaulting. M6's gate measured two importer binaries pointed
// at one cache reporting 1 hit and 0 miss, and M7 spent its first section repairing it; adding a
// third format is exactly the moment a second key gets introduced by accident, so
// `test_obj.cpp` and `test_pipeline.cpp` assert the property from both ends — the key differs by
// importer, and one cache serves all three formats without either colliding or cross-serving.

#include <cy/core/base/expected.h>
#include <cy/import/importer.h>
#include <cy/import/model.h>
#include <cy/import/options.h>

#include <string_view>
#include <vector>

namespace cy::import {

/// The OBJ importer's option schema.
///
/// Every option the glTF and FBX schemas declare appears here under the SAME NAME and with the same
/// meaning, so a project that re-exports a model from one format to another keeps its import
/// settings — `test_obj.cpp` asserts that against both of the other schemas rather than leaving it
/// to a comment, because the three lists are edited in three files.
///
/// It declares no option the other two lack. An OBJ says nothing about its own units or axes, so
/// `scale` and `source-up-axis` mean exactly what they mean for glTF, and `source-up-axis` defaults
/// to `y-up` for the same reason glTF's does: the format has no declaration to trust, unlike FBX's
/// `auto`.
[[nodiscard]] OptionsSchema obj_options() noexcept;

/// The built-in Wavefront OBJ importer, for `.obj`.
///
/// Holds no state, so one instance serves every worker.
class ObjImporter final : public Importer {
public:
    [[nodiscard]] ImporterInfo info() const noexcept override;
    [[nodiscard]] OptionsSchema schema() const noexcept override;
    [[nodiscard]] Status import(const ImportRequest& request, ImportResult& out) noexcept override;
};

// --- The parser, exposed so it can be tested without an import ----------------------------------

/// One `usemtl` run within an object: the faces drawn with one material.
struct ObjGroup {
    /// The material's name as the file spells it. Empty when the faces precede any `usemtl`.
    std::string name;
    /// Indices into the object's own de-indexed corner list, three per triangle.
    std::vector<u32> indices;
};

/// One `o` (or, failing that, `g`) object: a named set of faces.
struct ObjObject {
    std::string name;
    /// The de-indexed corners, in the order the faces named them.
    ///
    /// OBJ indexes position, texture coordinate and normal SEPARATELY, so there is no vertex in the
    /// file to copy — the corners are what a face names and one corner per index is the only
    /// faithful reading. `finish_mesh`'s weld is what turns them back into a vertex buffer, and it
    /// is the same welder the glTF and FBX importers run.
    std::vector<Vec3> positions;
    /// One per position, or EMPTY. An attribute the source supplied for only some corners is
    /// dropped by the parser rather than left partial: the arrays are parallel, so a missing entry
    /// would shift every later one.
    std::vector<Vec3> normals;
    /// One per position, or empty. See `normals`.
    std::vector<Vec2> uvs;
    /// The `usemtl` runs, in file order and never empty ones.
    std::vector<ObjGroup> groups;
};

/// Everything an `.obj` says.
struct ObjDocument {
    std::vector<ObjObject> objects;
    /// The `mtllib` references, in the order they appear and without duplicates.
    std::vector<std::string> material_libraries;
    /// A line the parser did not understand, kept so the importer can report the count once rather
    /// than a diagnostic per line. `s`, `l`, `p` and comments are understood and not counted.
    usize unsupported_lines = 0;
    /// A face this parser dropped because it named fewer than three corners or an index outside the
    /// file's own vertex arrays.
    usize malformed_faces = 0;
};

/// Parse an OBJ.
///
/// Never fails for a reason that is about the FILE: an unreadable statement is counted, not fatal,
/// which is the only workable policy for a format whose thirty-year history includes several
/// dialects. A returned error is about memory.
///
/// **Faces are triangulated by a fan**, which is correct for the convex polygons OBJ exporters
/// write and is what every OBJ reader does. A concave n-gon is the one case a fan gets wrong, and
/// the format carries no winding or normal information that would let a reader detect one.
///
/// **Negative indices are relative to the end** of the arrays as they stand at that line, which is
/// the specification's rule and the one dialect difference that silently corrupts a mesh when it is
/// got wrong.
[[nodiscard]] Expected<ObjDocument, Error> parse_obj(std::string_view text) noexcept;

/// One material out of an `.mtl`.
struct ObjMaterial {
    std::string name;
    StandardMaterial standard;
    /// The texture maps this material names, project-relative as the file spells them. Recorded as
    /// dependencies and imported by the TEXTURE importer; a model importer that cooked them would
    /// produce a second cooked texture and a second id for one image.
    std::vector<std::string> textures;
};

/// Parse an `.mtl` into the standard material every model importer writes.
///
/// The mapping is documented at the implementation. The one judgement worth stating here: an OBJ's
/// `Ns` specular exponent is converted to a roughness by the Blinn-Phong relation
/// `roughness = sqrt(2 / (Ns + 2))`, and a file carrying the `Pr`/`Pm` PBR extension has those used
/// verbatim instead. A material with neither gets the standard material's own defaults.
[[nodiscard]] Expected<std::vector<ObjMaterial>, Error> parse_mtl(std::string_view text) noexcept;

}  // namespace cy::import

#endif  // CY_IMPORT_OBJ_H

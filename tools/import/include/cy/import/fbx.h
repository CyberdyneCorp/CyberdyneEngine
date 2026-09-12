#ifndef CY_IMPORT_FBX_H
#define CY_IMPORT_FBX_H
// FBX import, over ufbx. M6 task 8.1.
//
// `asset-import-pipeline` — "Model import": "The model importer SHALL support **glTF 2.0**
// (`.gltf`, `.glb`) as the primary interchange format and **FBX** via ufbx, producing meshes,
// materials, textures, skeletons, animations, and a scene hierarchy as a prefab."
//
// FBX was specified at M5 and unimplemented until now: `tools/import` handled glTF alone, and
// `gltf.h` said so — "FBX via ufbx and USD as a tool-time-only importer are likewise absent, for
// the same reason as meshoptimizer: the dependency is not integrated at M5. The interface they slot
// into is `Importer`, and nothing about adding them touches this file." That prediction held. This
// importer implements the same interface, declares its options under the same names, and shares
// every post-parse step with the glTF importer through `model.h`.
//
// ================================================================================================
// WHAT ufbx IS DOING THAT THE glTF IMPORTER HAS TO DO BY HAND
// ================================================================================================
//
// An FBX file DECLARES its own coordinate system and its own unit, and every exporter writes a
// different pair — Maya centimetres Y-up, 3ds Max centimetres Z-up, Blender metres Z-up. glTF fixes
// both by specification, so the glTF importer's `source-up-axis` option exists only for files whose
// exporter lied. Here the file's declaration is normally right, so `source-up-axis` defaults to
// `auto` and ufbx performs the conversion from the file's own metadata: `target_axes` is the
// engine's right-handed Y-up and `target_unit_meters` is 1. The scenario "WHEN a Z-up model is
// imported THEN it SHALL be converted at import so no runtime code accounts for source handedness"
// is therefore satisfied by ufbx for the ordinary case and by this importer's own rotation for the
// override — and `test_fbx.cpp` asserts the ordinary case rather than trusting the option.
//
// ================================================================================================
// WHAT IS NOT IMPLEMENTED, AND WHAT STOPPED BEING ON THAT LIST AT M8.d
// ================================================================================================
//
// STEP 7 — skeletons, with bone levels of detail derived and the humanoid profile mapped — IS HERE,
// in `cy/import/fbx_skeleton.h`. It was absent through M8.a for the reason `gltf.h` gives:
// `animation-and-skinning` reaches Working at M8 and there was nothing to import a skeleton INTO
// before it. M8.b landed `cy/animation/skeleton.h`, so the blocker is gone and this importer's
// declared step set says so — the glTF importer's is unchanged, and the shared constant in
// `importer.h` exists precisely so one importer's claim does not grow with the other's.
//
// Step 8 landed beside step 7: animation curves are sampled into `cy::animation::Clip` through the
// quantised codec, so an FBX carrying a take now produces it rather than dropping it silently. Step
// 9 — material extraction as separately editable assets — remains absent.
//
// SKINNED MESH VERTICES ARE THE HOLE THAT REMAINS, and it is worth naming here rather than in a
// roadmap file: a skeleton and its clips import, but `MeshData` still carries no joint-index or
// joint-weight arrays and `write_cooked_mesh` has no attribute bit for them. So the rig arrives and
// the vertices are not bound to it. A project skinning an imported character has to supply weights
// from somewhere else, which is exactly what `samples/09b-animated-character` does and says it
// does. An FBX carrying one still reports a diagnostic naming what was skipped, so this is learned
// at import rather than discovered in the editor.
//
// USD remains absent. `asset-import-pipeline` makes it "an optional, tool-time-only importer" and
// `thirdparty-dependencies` calls OpenUSD "a large dependency, so editor and cooker only"; it is
// not in `deps/manifest.toml` and this milestone does not add it.

#include <cy/core/base/expected.h>
#include <cy/import/importer.h>
#include <cy/import/options.h>

namespace cy::import {

/// The FBX importer's option schema.
///
/// Every option the glTF schema declares appears here under the SAME NAME and with the same
/// meaning, so a project that re-exports a model from one format to the other keeps its import
/// settings. The two schemas are not identical: `source-up-axis` gains an `auto` choice and
/// defaults to it, because an FBX declares its own axis system and a glTF does not.
[[nodiscard]] OptionsSchema fbx_options() noexcept;

/// The built-in FBX importer, for `.fbx`.
///
/// Holds no state, so one instance serves every worker — which matters here because ufbx's own
/// threading is switched off and the pipeline's parallelism is one asset per job.
class FbxImporter final : public Importer {
public:
    [[nodiscard]] ImporterInfo info() const noexcept override;
    [[nodiscard]] OptionsSchema schema() const noexcept override;
    [[nodiscard]] Status import(const ImportRequest& request, ImportResult& out) noexcept override;
};

}  // namespace cy::import

#endif  // CY_IMPORT_FBX_H

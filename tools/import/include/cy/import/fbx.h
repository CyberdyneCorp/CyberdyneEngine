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
// WHAT IS NOT IMPLEMENTED, AND WHY IT IS THE SAME LIST AS glTF's
// ================================================================================================
//
// Steps 7, 8 and 9 of the ten — skeletons with bone LOD and profile remapping, animations with
// error-bounded compression and retargeting, and material extraction as separately editable assets
// — are absent here for exactly the reason `gltf.h` gives: `animation-and-skinning` reaches Working
// at M8 and there is nothing to import a skeleton INTO before it. An FBX carrying a rig produces
// its meshes and materials and a diagnostic naming what was skipped, so a project importing a
// character learns that its rig did not come through rather than discovering it in the editor.
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

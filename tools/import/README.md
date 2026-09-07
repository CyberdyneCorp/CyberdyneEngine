# `tools/import/` — layer 7

Layer 7, targets `cy::import` and `cy_import_cli`, headers `<cy/import/*.h>`, namespace `cy::import`.
M5 tasks 5.1 and 5.2, governed by `asset-import-pipeline` and (for live reload) `live-editing`.

The importer framework and the two importers M5 names: a source file in, cooked sub-assets and two
sidecars out, with a content-addressed cache in the middle so that the second run of a project costs
a file open per asset rather than a re-cook.

```
digest    the source, and build the derivation key from it, the TOOLCHAIN THAT COMPILED THIS
          BINARY (M7 task 1.1), the options, the importer's version, the variant and the profile
ask       the one derived-data cache, whose answer is a hit, a miss, or an invalidation naming the
          dependency that changed
import    on a miss, through a resolver that RECORDS every input the importer reads
bind      each produced sub-asset to the id it already had, or mint one
publish   the cooked assets, the `.meta` identity record, and the `.import` record beside it
report    what happened, with the reason, per asset and for the run
```

## What is here

| Header | What it owns |
|---|---|
| `options.h` | An importer's declared option schema, the values set for one import, and the canonical form the cache key is built from |
| `importer.h` | `Importer`, `ImportRequest`, `ImportResult`, `ImporterRegistry`, and `ImportResolver` — the seam that makes an import a pure function |
| `sidecar.h` | The `.import` record: which importer, which options, and which id every sub-asset holds |
| `mesh.h` | `MeshData` and the processing steps: welding, normals, tangents, vertex cache, overdraw and fetch ordering, quadric-error simplification, convex hulls, convex decomposition, and the xatlas-backed lightmap unwrap |
| `model.h` | The half of a model import that does not depend on the source format: `finish_mesh`, `emit_mesh_with_lods`, `emit_collision`, `emit_prefab`, and the `StandardMaterial` record both model importers write |
| `texture.h` | Image decoding, format selection from declared usage, mip generation in the correct colour space, alpha coverage, and the mistake detector |
| `gltf.h` | glTF 2.0 and GLB, and the two cooked payload formats a model import produces |
| `fbx.h` | FBX via ufbx: the same interface, the same option names, and every post-parse step shared with `gltf.h` through `model.h` |
| `json.h` | A strict JSON reader, written to be deleted when a glTF dependency is integrated |
| `pipeline.h` | The driver: the cache, the sidecars, the parallel phase, cancellation and the report |
| `report.h` | Per-asset rows and the project-level summary `asset-import-pipeline` asks for |
| `live_import.h` | M5 task 5.2's asset half: watch, re-cook, and reload behind the stable handle |

The **cook cache is not here**. It is `src/core/assets/derived_cache.h` at layer 0, because
`asset-import-pipeline` requires one cache over all derived data and a cache that lived with the
importer would be a second mechanism the day the shader toolchain wanted one. This module is a client
of it, like every other producer will be.

## Two targets, and why the split is what makes the pipeline testable

`cy_import` is a library with no `main` and no argument parsing; `cy_import_cli` is a thin front end
over it. The assertions worth making — which sub-assets a glTF produced, what the cache said and why,
which diagnostic a mis-tagged normal map raised — are on `ImportResult` and `ImportReport`, not on an
exit code. This is the same split `tools/cook/` made for the same reason.

## What M6 added

**FBX imports** (task 8.1). `tools/import` handled glTF alone through M5; FBX had been specified and
unimplemented since then. `src/fbx.cpp` is the only translation unit in the tree that names a `ufbx_`
symbol, and everything after the parse — welding, tangent generation, the optimisers, the level-of-
detail chain, collision from the naming convention, the node table — is `model.h`'s and is shared
with the glTF importer. That sharing is the point rather than tidiness: without it, one mesh exported
in two formats would weld to two vertex counts and cook to two sets of bytes.

**The lightmap unwrap, the overdraw reorder and convex decomposition** (task 8.2). Three of the six
steps `asset-import-pipeline` names under "Mesh processing" were missing at M5. `src/unwrap.cpp` is
the only translation unit that names an `xatlas` symbol.

**Cook profiles select content, and report what they removed** (task 8.3). `profile_retains()` is the
whole policy, in one function: a `DedicatedServer` cook keeps prefabs and collision meshes and drops
textures, materials and render meshes, and the report says how many bytes that saved. Exclusion
happens at PUBLICATION and not at import, so an id does not move between a client cook and a server
cook — a prefab in the server package references a mesh by the id the client package uses.

**A cook can refuse to invent an identity** (`--no-mint`, design.md §1.7). `assets::mint_asset_id()`
draws 128 random bits, and M6 is the milestone at which an `AssetId` reaches the inside of a payload —
from which point two cold builds of one project stop producing the same bytes. A shipping or CI cook
passes `--no-mint` and fails naming the asset; the remedy is to commit the `.import` sidecar, which
should have happened anyway.

## What M7 fixed: the key was blind to its own compiler

`import_derivation_key` contributed the producer, the source hash, the variant, the profile and the
options — and no compiler, no flags and no library versions. It is the function `cy_import_cli`
uses. M6's spike built one importer at `-O2` and at `-O0`, ran both over the same glTF and got the
identical key `04a6fff1…`; M6's closing gate re-measured it on two importer binaries pointed at one
cache and recorded **1 hit, 0 miss**. `asset-import-pipeline` requires "one cache covering all
derived data", and one cache with two keys — one of which cannot see its own compiler — serves the
wrong artefact and reports success.

The remedy shipped at M6 but inside `tools/build/`, which this module cannot link. M7 moved
`ToolchainFingerprint` to `cy/core/assets/toolchain.h` at layer 0 and this function now contributes
it through the same `contribute()` the build graph and the shader cache call, and FAILS on an
incomplete fingerprint rather than defaulting. `tests/test_importer_key.cpp` is the regression: it
reconstructs the M6 spelling of the key and requires the current one to differ from it.

## The importer is a node in the build graph (M7 task 1.4)

`cy_import_cli` is still the front end a person runs. What M7 added is that the same importers run
as a `cy::build` node: `tools/build/src/content_producers.cpp` registers an `import` producer that
reads its source through `NodeContext::read`, hands the importer an `ImportResolver` whose `read`
goes to `NodeContext::discover`, and writes the result as a bundle — `encode_import_bundle`, which
was file-local to `pipeline.cpp` and is public for exactly this reason, so there is one framing
rather than two.

Composing the two resolvers is what earns it. `ImportResolver` exists so an importer cannot read a
file without recording it; `NodeContext::discover` exists so a producer cannot read a name without
the BUILD recording it. Put together, a glTF's external `.bin` is a declared dependency of the node,
so editing it invalidates the node — which is the whole point of a derivation graph and is what
running the importer beside the graph could not give.

## What it deliberately does not do

* **It links no simplification library or block encoder.** meshoptimizer and the BC7/ASTC encoders
  are named by `thirdparty-dependencies` and neither is integrated. ufbx and xatlas ARE, from M6, and
  `deps/manifest.toml` carries both. That file's header says what each remaining omission costs and
  names the file the library lands behind. Every gap is reported by the code that has it — a
  diagnostic naming the missing decoder, a `CookedTexture::encoded = false` written into the payload
  — rather than approximated.
* **It reads Targa and not PNG.** PNG needs a DEFLATE decoder and JPEG a DCT one. `decode_image`
  fails with a message naming which dependency would read the file.
* **It does not import skeletons or animations.** `animation-and-skinning` reaches Working at M8 and
  there is nothing to import a rig *into* before it; a glTF or FBX carrying skins is imported for its
  meshes and materials with a diagnostic naming what was skipped.
* **It does not import USD.** `asset-import-pipeline` makes USD "an optional, tool-time-only
  importer" and `thirdparty-dependencies` calls OpenUSD "a large dependency, so editor and cooker
  only". It is not in `deps/manifest.toml` and M6 does not add it.
* **It does not produce a `cy::scene` prefab.** A prefab is a layer-4 concept over an ECS world, and
  a layer-7 tool that constructed one would have to instantiate a world to write a file. The importer
  produces a documented flat node table; turning it into a prefab is the cook step's.

## Two sidecars, and why

`<source>.meta` is the ENGINE's identity record — `cy::assets::AssetMeta`, written by
`cy::assets::write_meta`, read by the asset database. `<source>.import` is this module's: the
importer, its version, the option values and the sub-asset id table.

`asset-import-pipeline` describes one file. The split is structural rather than stylistic and
`sidecar.h` gives the whole argument: `AssetMeta` is a fixed-size, trivially copyable struct that
`AssetDatabase` stores by value, so a variable-length sub-asset table inside it makes the type
move-only and turns a layer-0 registration into a redesign. It also earns something — the identity
record never changes once written, and the import record changes whenever an option is edited, so a
review can see at a glance that no id moved.

## Registration

`add_subdirectory(import)` was added to `tools/CMakeLists.txt`, beside `cook`. That file is not part
of this change's ownership grant; the line is additive and nothing else in it changed.

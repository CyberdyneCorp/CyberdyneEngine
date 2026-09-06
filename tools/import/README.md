# `tools/import/` — layer 7

Layer 7, targets `cy::import` and `cy_import_cli`, headers `<cy/import/*.h>`, namespace `cy::import`.
M5 tasks 5.1 and 5.2, governed by `asset-import-pipeline` and (for live reload) `live-editing`.

The importer framework and the two importers M5 names: a source file in, cooked sub-assets and two
sidecars out, with a content-addressed cache in the middle so that the second run of a project costs
a file open per asset rather than a re-cook.

```
digest    the source, and build the derivation key from it, the options, the importer's version,
          the variant and the cook profile
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
| `mesh.h` | `MeshData` and the processing steps: welding, normals, tangents, vertex cache and fetch ordering, quadric-error simplification, convex hulls |
| `texture.h` | Image decoding, format selection from declared usage, mip generation in the correct colour space, alpha coverage, and the mistake detector |
| `gltf.h` | glTF 2.0 and GLB, and the two cooked payload formats a model import produces |
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

## What it deliberately does not do

* **It links no third-party parser or encoder.** meshoptimizer, cgltf and the block encoders are all
  named by `thirdparty-dependencies` and none is integrated at M5. `deps/manifest.toml`'s header says
  why, states what each omission costs, and names the file each library lands behind. Every gap is
  reported by the code that has it — a diagnostic naming the missing decoder, a
  `CookedTexture::encoded = false` written into the payload — rather than approximated.
* **It reads Targa and not PNG.** PNG needs a DEFLATE decoder and JPEG a DCT one. `decode_image`
  fails with a message naming which dependency would read the file.
* **It does not import skeletons or animations.** `animation-and-skinning` reaches Working at M8 and
  there is nothing to import a rig *into* before it; a glTF carrying skins is imported for its meshes
  and materials with a diagnostic naming what was skipped.
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

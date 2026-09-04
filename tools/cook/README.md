# `tools/cook/` — layer 7

Layer 7, targets `cy::cook-pipeline` and `cy_cook`, headers `<cy/cook/*.h>`, namespace `cy::cook`.
M2 task 3.2.13, governed by `core-assets-and-io`.

The cook path: a directory of authoring documents in, a `.cypak` of cooked assets out.
`core-assets-and-io` requires that "the runtime SHALL load only cooked assets". M1 built the package
format's write path and left `PackageWriter::add` taking bytes that were already cooked;
`src/scene/serialization/` produces those bytes. This is what joins them.

```
read      every *.cyscene and *.cyprefab under a source root, through the virtual filesystem
register  each into a Library, keyed by the asset id its own header declares
validate  the dependency graph — cycles rejected as a chain, before anything is cooked
cook      resolve, bind parameters, validate references, flatten, assign identity, emit blocks
wrap      each cooked stream in the cooked-asset header (<cy/core/assets/cooked.h>)
write     into a .cypak, recording each asset's dependencies so the loader can preload them
```

## Two targets, and why the split is what makes the pipeline testable

`cy_cook_pipeline` is a library with no `main` and no argument parsing; `cy_cook` is a thin front end
over it. A cook that existed only as an executable would be testable only by running a process and
reading its exit code, and the interesting assertions — how many relationships flattened, which
references were nulled, that a cyclic graph writes *nothing* — are on the report.

## What it deliberately does not do

* **It does not import source formats.** A `.png` or a `.gltf` needs an importer per format, and the
  list of them is a milestone of its own. What it cooks is the engine's own authoring form.
* **It does not decide the component set.** A cook needs each component's size, alignment and
  entity-reference offsets, and the only authority on that is a world that has registered them — so
  the caller supplies one. A tool that guessed would produce a package the runtime rejects at the
  build-schema check, which is the check working as intended and a poor way to find out.
* **It is not incremental.** Every document is cooked every run. The content hash the cooked header
  records is what an incremental build would compare, and it is written; the comparison is not.

## The front end's own limit, stated here rather than discovered

`cy_cook` registers the types `reflect::default_registry()` holds, with **no entity-reference
offsets**, because reflection does not record them. A project whose components hold entity
references should drive `cy::cook::run` from its own tool until the project system can load a game's
module and call its registration (M4's ABI work) — the pipeline takes the world, so that is a
five-line program.

## Registration

`add_subdirectory(cook)` was added to `tools/CMakeLists.txt`, which was a placeholder naming no
targets. That file is not part of this agent's ownership grant and the line is flagged in the M2
handoff notes; it is additive and nothing else in that file changed.

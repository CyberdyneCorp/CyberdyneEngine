# `tools/material/` — layer 7

Material cooking, as a node in the derivation graph, and the front end that runs it.

**Governed by**: `material-compiler`. Arrived at M7, task 6.3.

## What is here and what is not

The **compiler** is engine code and lives in [`src/rendering/material/`](../../src/rendering/material/).
`material-compiler` requires that — "The material compiler SHALL be engine code" — and the reason is
practical rather than architectural: a runtime creates material instances and an editor previews
graph nodes with no tool present, and both go through the same compiler.

What is here is the **cook**: the `material` producer, the cooked bundle format, and `cy_material`.

| | |
|---|---|
| `cy_material_cook` | the `material` producer, `cook_material`, and the bundle's encoding |
| `cy_material` | `compile` one material, or `cook` a project's materials through the build graph |

## A graph node, and not a second cache

M6's closing gate recorded that "the real cooks are not nodes in the build graph"; M7 task 1.4 put
`import` and `cook` into it, and `design.md` §5 lists "every new cook is a graph node with a two-run
determinism gate" among the things that must not be retrofitted, with the reason:

> M6's spike measured what a non-deterministic step plus a cache costs: the artefact that ships is
> decided by a race.

So this cook is a `ProducerBody` handed a `NodeContext`. It reads its source through `context.read`
and writes its output through `context.write`, and **it caches nothing**: the derivation key, the
content-addressed artefact store, precise invalidation and the shared cache tiers are all
`cy::build`'s. `tools/material/tests/` measures the consequences — two runs from empty produce
identical bytes, a second build is a cache hit, editing one material leaves the other alone, and a
material with a cook-time error produces no artefact at all.

For a material assigned to geometry, set the build node's `geometry` option to renderer source
names separated by commas (for example, `StaticMesh,VirtualGeometry`). The producer passes these
paths to the material compiler. A vertex offset assigned to `VirtualGeometry` fails with
`vertex-geometry-unsupported` and emits no bundle; an unknown source fails with
`material-geometry-source-invalid`. Omitting the option preserves an unassigned material cook.

**The producer's version is the compiler's version.** `kMaterialProducerVersion` is *defined as*
`cy::rendering::material::kCompilerVersion`, so "WHEN the material compiler version increases THEN
compiled programs SHALL be recooked and the authored material assets SHALL be untouched" is a fact
about the key rather than a note somebody has to remember to act on.

## Running it

Compile one material and see what it costs:

```
$ cy_material compile worn_metal.cymat --slang out/
material worn_metal  cook key 0x02f90b50d5427262
  primary/high: 2 samples, 6 alu, 3 closures, 3 permutations, 0.016 ms full screen
  primary/medium: 1 samples, 5 alu, 3 closures, 3 permutations, 0.010 ms full screen
  secondary/high: 1 samples, 5 alu, 3 closures, 3 permutations, 0.010 ms full screen
  far_field/high: 0 samples, 4 alu, 1 closures, 3 permutations, 0.003 ms full screen
  shadow/high: no program (opacity is constant, so there is no fragment work)
  warning derivation-changes-albedo (grime_map): a texture that reaches this material's base
    colour in the primary program does not reach it in this derived one
bundle: 8769 bytes
```

Cook a project's materials through the graph, twice:

```
$ cy_material cook project/ artefacts/ --cache cache/ materials/worn_metal.cymat materials/copy.cymat
ran          material:materials/worn_metal.cymat
ran          material:materials/copy.cymat
ran 2, cached 0, rebuilt 0, failed 0

$ cy_material cook project/ artefacts/ --cache cache/ materials/worn_metal.cymat materials/copy.cymat
cached       material:materials/worn_metal.cymat
cached       material:materials/copy.cymat
ran 0, cached 2, rebuilt 0, failed 0
```

Both transcripts above are real output from this tool, not an illustration.

## The bundle

One cooked material is one artefact carrying the serialised **IR**, the **generated Slang** for every
program of the family and every tier, each program's **cost report**, and the **cook key**. That is
`material-compiler`'s list — "the IR, the generated Slang, the compiled programs per target and tier,
reflection data, the cost report, and pipeline state metadata" — with one thing missing and said
plainly: **nothing here invokes the Slang compiler.** The bundle carries source, and `shader-system`
owns turning it into SPIR-V through the one shader pipeline the specification permits. Wiring that
join is not this task's, and a bundle that claimed to hold compiled programs would be lying about it.
Bundle version 2 also stores each program's vertex-offset Slang and digest. An opaque shadow
program can have no fragment source while still carrying the vertex source needed to displace its
silhouette. The reader accepts version 1 bundles with an empty vertex source; the compiler version
increment makes the build graph recook current materials before they are shipped.

## The failure a reader should know about

A material's cook fails, and produces **no artefact**, when the compiler reports an error: a static
annotation on a numeric parameter, a closure set the target profile cannot evaluate, an environment
field the project does not declare, or a material the validator refuses. `build-and-packaging`
requires a failed node to produce nothing, and the reason is sharper here than usual — a cached
failure is a failure you cannot clear by fixing the source.

## `--stages`

`cy_material compile <file.cymat> --stages` prints every stage of the material's lowering — the
graph or the authored text, the IR before optimisation, the IR after it, the generated Slang, and
the compiled backend output — each with its byte count and a content digest:

```
stages primary/high  4 of 5 available
  graph: 413 bytes  digest 0x6909647cab0d4c52
  ir: 692 bytes  digest 0x2c712c8f1381921f
  optimised-ir: 723 bytes  digest 0x5acb4f4694ce2adb
  slang: 811 bytes  digest 0x8a5ed0a6183f8d10
  compiled: ABSENT — the compiled backend output is `shader-system`'s: this target does not link
            the shader toolchain, and `attach_backend_stage` in cy::rendering-material-slang fills
            it in
```

**The list is the library's and this is one caller of it.** `shader-system`'s "Visual material
editor" requires the editor to show the same five stages, and two lists cannot be compared if one of
them is assembled inside a `main` — the command line would then be the only caller, and "they agree"
would be a statement about one function. So `rendering::material::inspect_lowering` produces the
list, `material::write_stage_report` renders it, and `integration.material_cook`'s
*the command line and the library agree about the lowering stages* RUNS this binary and compares what
it printed against what a second caller obtains.

The cook report also states, per program, **which of the material's inputs are textures and which are
constants**, with a `[constants only: this program samples no texture]` marker — which is what makes
a claim about a published picture's materials checkable from the artefact rather than from memory.

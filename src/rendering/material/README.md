# `src/rendering/material/` — layer 4

The BRDF, the material model and its parameter storage, the standard material, the fallbacks, and
material validation.

Since M7, also **the material compiler**: the IR, its two front-ends, the optimisation pipeline,
closure lowering, the program family, quality tiers, Slang emission, cost analysis and material
binning.

**Governed by**: `rendering-materials-and-shading` (M3, tasks 4.2.3–4.2.5) and `material-compiler`
(M7, tasks 6.1–6.4).

## Why the BRDF exists as C++ and not only as Slang

The specification is unusually concrete here, and says why:

> The BRDF is specified concretely — exact terms and their sources — because "PBR" alone is not a
> specification and mismatched terms produce subtly wrong lighting that is very hard to debug later.

A shading term that can only be evaluated on a GPU is a term whose energy conservation is checked by
looking at a screenshot. Every term in `brdf.h` is a free function over cosines, so `material`'s
suite checks the properties that actually matter — that GGX is normalised, that Fresnel is `f0`
head-on and `f90` at grazing, that a metal has no diffuse, that rough metals get their energy back —
in arithmetic, with no device.

That paid for itself immediately. **The stable GGX form was transcribed without its `NoH`**
(`k = α / (1 − NoH² + α²)` rather than `α / (1 − NoH² + (NoH·α)²)`), which integrates to 0.80 at
α = 0.5 and 0.60 at α = 0.81: every rough highlight was quietly losing energy that multi-scatter
compensation could not distinguish from the loss it exists to correct. `material_ibl` now integrates
`∫D·NoH·dω` over the roughness range and asserts it is one.

## The three parts

| | |
|---|---|
| `brdf.h` | the core BRDF (`D`, `V`, `F`, three diffuse models), multi-scatter compensation, the DFG table, octahedral environment maps, L2 irradiance |
| `material.h` | parameters and their compile-time identifiers, `MaterialProgram`, and the **GPU material table** |
| `standard.h` | the standard material's slot set, its defaults, and the three fallback materials |
| `validation.h` | what cooking reports and what it refuses |

## Parameter storage: a table, not a descriptor set per material

`rendering-materials-and-shading` requires that a GPU-generated draw reach its parameters with no
per-object binding:

> **WHEN** a GPU-generated draw shades a pixel **THEN** it SHALL index the material table using the
> instance's material identifier, with no per-object descriptor binding

So a material's parameters are **bytes at an index**: `index * kMaterialBlockBytes`, a
multiplication with nothing to look up first. Every block is the same size, a parameter is addressed
by a compile-time hash of its name, and a change marks one interval that the frame uploads in one
transfer. A material *instance* is a slot plus a program — allocating one copies 256 bytes and
compiles nothing.

## Validation is a report, not a boolean

The four things the specification asks to be reported are not the same severity, and one answer
would force the wrong behaviour on two of them. A parameter nobody reads is a note; a subsurface
material with an additive blend mode has no defined shading and must not cook. So every finding
carries its own `fatal` flag and `MaterialReport::fatal()` is what a cooker branches on.

`validate_material()` returning `ok()` means validation **ran**. A cooker that could not tell "could
not validate" from "validated and it is wrong" would treat a full disk and a broken material the
same way.

## The material compiler (M7)

M3's README said the compiler was "deliberately not here" and that "what is here is the *shape* a
compiled program has, so an authored graph lowers into it at M7 and nothing downstream changes".
That is what happened: `compile_material` fills in the same `MaterialProgram`, and `MaterialTable`
is untouched.

| | |
|---|---|
| `ir.h` | the typed SSA DAG, its **content hash**, the builder, the serialiser and the canonical order |
| `graph.h` | the node-graph front-end — shaped like an editor, warts included |
| `text.h` | the text front-end — shaped like a person writing |
| `passes.h` | the optimisation pipeline: a **rebuild** through the same builder, iterated to a fixed point |
| `lowering.h` | closure sets to shading models, the program family, derivation, quality tiers |
| `emit.h` | Slang emission, and the node preview, which is the same function with a root |
| `cost.h` | the cost report, attributed back to authoring nodes |
| `binning.h` | material classification and binning, as the CPU reference a GPU pass is checked against |
| `compiler.h` | the join, parameter classification, and the cook key |

The material IR also carries an optional, typed `float3` world-space vertex offset root. Its
content enters the material digest and versioned module encoding; optimisation and family
derivation retain it for visible and shadow programs. Graph lowering and the text front end
(`vertex_offset = ...;`) require a float3 expression. Displaced frame rendering remains under
issue #15.
The engine material catalogue assigns `material.vertex_output` a stable identity and marks it as a
vertex-only node. Wiring its `offset` pin lowers to the same IR root as the text assignment.
`material.world_position` lowers to the engine's `position` attribute, in camera-relative world
coordinates; `material.object_position` retains mesh-local coordinates. Both are float3 inputs.
`material.time` is a scalar engine input. The first-light hosted renderer writes elapsed seconds into the
frame uniform before drawing each preview frame, and the material shader binds it for vertex and
fragment expressions. Its value changes without a material recompile.
Each compiled variant now carries separate Slang for that offset; a shadow variant retains it even when
its fragment program is absent. The vertex source compiles as an actual Slang vertex entry point in
the smoke suite. Material bundle version 2 retains each variant's vertex source and digest, including
opaque shadow variants without fragment work. The hosted editor viewport evaluates the same
offset for its visible and shadow passes; the main frame's motion pass still needs integration. A
vertex expression that reaches `TextureSample` or handwritten `Custom` Slang reports
`vertex-stage-unsupported` in the compiler, editor validation, and cook; those nodes have no
supported authored vertex binding. Texture samples used only by the surface stage remain valid.
`material.object_position`, `material.normal`, and `material.uv0` are fixed typed geometry inputs
in the engine catalogue. The hosted viewport passes mesh object coordinates to the generated
vertex function before projection; these nodes lower to the same IR attributes as text-authored
geometry reads.
`CompileOptions::geometry_paths` records named geometry sources for the variant report and cook
identity. A vertex graph targeting `VirtualGeometry` reports `vertex-geometry-unsupported` with
the source name: its visibility and shadow paths cannot evaluate the offset. Callers that do not
know their geometry assignments may continue using the older anonymous `geometry_sources` count;
the editor and project cooker still need assignment-aware requests to enforce the named refusal.

### The one decision everything else follows from

**A material's identity is a content hash over a canonicalised DAG.** It closes over the op, the
result type, the symbol's *text*, the immediates and the operands' hashes — and over nothing else.
Not the node id, not the order a front-end called the builder, not which front-end it was.

That is what makes M7's exit criterion true rather than lucky: a node graph and a hand-written text
definition of one material produce the same IR digest and byte-identical Slang. The suite builds
that material both ways, deliberately as unalike as two real authoring paths are — the graph has a
weight port on every closure, samples one texture from two nodes, wires a multiply in the opposite
order, carries a muted emission closure and a disconnected node, and drags in a tint node left at
one — and checks that the two are different *before* the compiler runs and identical after.

`cy::Name` makes the trap concrete, and it is worth knowing about before touching this code: a
`Name` is an index into a process-wide intern table, and its own header says the ordering "is
interning order ... NOT lexicographic and NOT stable across runs". **Every hash and every sort in
the compiler goes through `Name::text()`.** A hash over `Name::index()` — or a commutative operand
list sorted by it — would make two identical materials differ, in a way that reproduces on one
machine and not on another.

### Node previews are the same emitter with a root

`material-compiler` forbids "a separate editor-only shading path", which is the rule
`editor-viewport-and-gizmos` states for the viewport for the same reason: a second path is a second
answer. `preview_node` is `emit_program` with `preview_root` set, and the suite proves the
relationship rather than asserting it — the preview of the surface root is the primary program's
body **byte for byte** with the opacity assignment removed, and the preview of every interior value
computes it with the primary program's own statement, once the SSA numbers are read back to the IR
nodes they name.

### What a bisection build actually is

Every pass is individually disableable, as the specification requires. **Eight of the nine switches
change the compiled program**, so a development build with one turned off is not the same material
compiled more slowly, and `compile_material` says so in a diagnostic. The exception here is
dead-node elimination, and the reason is in `passes.h`: emission walks from the roots, so an orphan
cannot reach the source. `design.md` §1.4's table has a different exception for a different emitter,
and the difference is recorded rather than smoothed over.

Note also that the switches reach the **front-ends'** builders. Interning happens in
`Builder::make`, so a front-end that ignored them would merge the values a bisection is trying to
keep apart before the pipeline ever ran.

## What is deliberately not here

* **Anything that touches a device.** The table is bytes; whoever owns a device uploads them. That
  is what lets the DFG bake — the one piece of the image-based lighting chain that would otherwise
  need a GPU — run in continuous integration.
* **A shader toolchain.** The compiler emits Slang **text**; `shader-system` compiles, reflects,
  caches and hot-reloads it, and `material-compiler` forbids a second toolchain. Nothing in this
  module or its suite invokes the Slang compiler, which is why the exit criterion is checkable with
  no GPU: it is a property of the text.
* **The cook.** `tools/material/` owns it, as a node in the derivation graph.
* **The GPU classification dispatch.** `binning.h` is the CPU reference the dispatch is checked
  against, the way `cpu_reference_cull` is for culling. The dispatch itself belongs to the frame.

## M11.c section 1 — what a compiled material can now be asked

Three things were added, and each closes a sentence in a requirement that had no implementation
behind it rather than a feature somebody wanted.

### Which inputs are textures and which are constants — `inputs.h`

**Every material in every picture this project has published is made of constants**, and nothing in
the compiled artefact distinguished one from a fully textured material. A constants-only material
renders, shades and photographs; the M11.c spike's `out/shot-average-post.png` is a lit, tone-mapped,
entirely plausible picture of one. So a sentence about what a published picture demonstrates could
not be checked, only believed.

`classify_inputs` answers it off the IR: one `MaterialInput` per operand of every leaf closure the
surface reaches, plus opacity, each classified `Constant`, `Varying`, `Parameter` or `Texture` — the
**strongest** thing reachable from that input, which is why `sample(map, uv).xyz * base_color` is a
texture input. It is **per program and not per material**, because the far-field program of a fully
textured material substitutes averages and samples nothing: a report answered per material would
call it textured, which is the single most efficient way for a caption to be wrong.
`cy_material compile` prints the counts on one line a tool can read, and says
`[constants only: this program samples no texture]` in those words.

### Every stage of the lowering, readable — `stages.h`

`shader-system` requires the editor to be able to show "the graph, the material IR before and after
optimisation, the generated Slang, and the compiled backend output", and `material-compiler`
requires the IR to be "dumpable in a readable form, before and after optimisation". **Four of those
five existed as data and none of them could be read**: no function in this tree turned a `Module`
into anything a person can look at.

`dump_graph` and `dump_module` are those readable forms — canonical order, not node order, so two
front-ends producing one material produce comparable text — and `inspect_lowering` collects the five
stages **in the specification's order from one compilation**. The fifth is `shader-system`'s and
this target does not link the shader toolchain, so it comes back unavailable **with a reason naming
who owes it**: an inspection with four stages says so, because a panel that showed four and called
it "every stage" is the same defect as a preview drawn by a second renderer.

### A preview that is a compiled program — `preview.h`, in `cy::rendering-material-slang`

`unit.material_compiler` proves a preview's **text** is the shipping program's own, byte for byte.
That is half the requirement. The other half — "the same compiler, lowering, **and shader pipeline**
as runtime" — **had no caller anywhere in this tree**: nothing ever took a preview's text to a
shader compiler, and a preview is a picture.

`compile_preview` is that path: `preview_node`, the generated prelude, a preview probe entry point,
and `cy::shader`. It **refuses first** when the front end cannot compile source, which is the state a
shipping build is in, and there is no second branch in the file for a cached thumbnail or a stand-in
renderer to be returned from. `smoke.material_preview` compiles a preview to SPIR-V against the
engine's standard library — 592 words for the reference material's surface root, the first time this
path has run — and then asks the same call with the compiler gone and requires it to fail.

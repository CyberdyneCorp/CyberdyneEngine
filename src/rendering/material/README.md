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

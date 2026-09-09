# Design: M8.b — Systems

## 1. The spike, and the failure budget it carries

**The spike is one question asked seven times: can a single graph IR express what each consumer
needs, and what does it cost the consumer that fits worst?** The seven are `visual-scripting`,
`gameplay-abilities-and-effects`, `ai-system`, `animation-and-skinning`, `sequencing-and-cinematics`,
`vfx-system` and `camera-system`. The starting position is not a blank page: `src/rendering/material/`
already compiles a graph to an IR to closures to a program, it is the only compiled-graph
implementation in the tree, and the roadmap ordered the material compiler before every other graph
consumer for exactly this reason.

**The failure budget, stated before the work rather than after it.** The split that created this
milestone put the spike's risk inside it rather than in front of the authoring work, and a risk
inside a milestone needs a stated price:

| Spend | What it buys | What it costs |
|---|---|---|
| The IR carries a consumer's semantics as written | that consumer lowers through it | nothing |
| The IR is **extended** to carry them | the extension is one change to one module | every consumer already built on it is re-tested; the extension is specified before it is written |
| The IR **cannot** carry them and the consumer needs an escape hatch | that consumer keeps its own evaluator behind the same authoring surface | the "no graph is interpreted at runtime" exit criterion is narrowed **by name**, in `ROADMAP.md`, to the consumers it still holds for |
| Two or more consumers need escape hatches | the shared-IR premise is wrong | the milestone is re-planned rather than delivered on a premise its own spike refuted |

**Two escape hatches is the budget.** It is a number so that exhausting it is an observation rather
than an argument, and it is two rather than one because a single ill-fitting consumer is a normal
outcome of designing an abstraction against six.

**The spike runs before the implementing work and its report is committed**, the way M7's two were.
What it must produce: for each of the seven, the semantics it needs stated as a list, whether the IR
expresses each one, and — for anything it does not — which row of the table above that spends.

## 2. Order, and why the interface is not last

The dependency graph in `docs/roadmap/dependencies.md` puts `visual-scripting` above abilities, AI,
sequences, animation and VFX, and `navigation` above AI. That fixes most of the order. The one
choice not forced by dependencies is where `ui-system` and `text-and-fonts` go, and they go **early
rather than last**, because the vertical slice is the artefact and an artefact with no heads-up
interface cannot show an ability's cost, an effect's duration or a team's score. An interface built
last is an interface tested once.

## 3. What M8.a leaves for this milestone to finish, and what it warns about

**Two inherited items, both recorded at M8.a's closing gate rather than discovered here:**

- `serialization-and-prefabs`' "Apply and extract" has no implementation and has not had one since
  M2. The data model supports it; the operations are not written.
- **The mesh a created primitive references reaches no renderer.** `cy::render::MeshRenderer` is a
  name in `src/scene/src/node_template.cpp`'s catalogue with no reflected type behind it, so a world
  authored in the editor round-trips through `.cyworld` and is drawn by nothing. M8.a's own artefact
  photograph shows an authored sphere and box as two identical unit boxes through M3's fixed slots.
  That is `editor-viewport-and-gizmos` and the renderer's, both of which complete here.

**And the warning M7's gate wrote for M8 and M8.a did not have to face.** Of the modules under
`src/rendering/`, `cy_rendering_forward`, `cy_rendering_material`, `cy_rendering_post`,
`cy_rendering_shadows`, `cy_rendering_sky`, `cy_rendering_temporal`, `cy_rendering_gpu_culling` and
`cy_rendering_virtual_texturing` are linked by **nothing but their own test binaries**. Nothing in
the tree assembles a frame out of the renderer's own parts. A vertical slice is the first artefact
that cannot avoid it, and `vfx-system`, `rendering-2d` and `ui-system` all render into that frame.
**Assembling the renderer is a prerequisite of this milestone's artefact and it has no owner on the
ladder.** It is stated here so that it is planned rather than discovered in section 12.

## 4. What must not be retrofitted

| Invariant | Why it cannot wait |
|---|---|
| One IR, extended deliberately | Six consumers each patching the IR for themselves is six IRs with one name |
| No graph interpreted at runtime | A per-entity virtual tick in one consumer becomes the pattern the next five copy |
| The determinism firewall | VFX and inference must not write gameplay state; a firewall added after the fact is a firewall with holes |
| Cost bounded by configuration | 8,000 agents and 100 concurrent effects are budgets, and a budget discovered in a frame is not one |

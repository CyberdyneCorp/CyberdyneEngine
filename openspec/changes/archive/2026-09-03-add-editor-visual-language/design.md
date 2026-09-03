# Design: the editor's visual language

## Why this is a specification and not a style guide

A style guide is advice. It is read once, at the start, by whoever is drawing the first panel, and
consulted thereafter by nobody. What actually decides a tool's appearance is a long series of small
local decisions — this header needs emphasis, this state needs a colour, this plugin needs an icon —
each of which is reasonable on its own.

The failure is cumulative and it is not recoverable by taste. By the time an editor looks
inconsistent, the inconsistency is distributed across a hundred files and a dozen contributors, and
fixing it is a rewrite of the interface rather than a change to it.

So the visual language goes where every other compounding decision in this repository goes: into a
capability with `SHALL` requirements, scenarios that can be checked, and a forbidden-patterns list a
reviewer can point at.

## Why a separate capability

Three capabilities now divide the editor's front end, and the boundary is clean:

| Capability | Owns | Example |
|---|---|---|
| `editor-ui-ux` | **Interaction** — what the user does and how the editor responds | Command palette, keyboard-first operation, docking, never losing work |
| `editor-viewport-and-gizmos` | **Behaviour** — what happens in the viewport | Engine-side picking, numerically stable manipulation, one transaction per drag |
| `editor-visual-language` | **Appearance** — what it looks like and what things are called | Gold means selection, arrows translate, X is red, a node is not an Actor |

Folding appearance into `editor-ui-ux` was the alternative. It was rejected because that capability
is already sixteen requirements about behaviour, and appending twenty about colour and typography
would bury both. The split also gives reviewers a single place to check a visual proposal against.

The boundary is deliberately drawn at *legibility versus mechanism*. `editor-viewport-and-gizmos`
owns that a drag produces exactly one transaction and returns exact values; `editor-visual-language`
owns that the user can tell a rotate gizmo from a scale gizmo without looking at the toolbar. Both
constrain the same widget and neither restates the other.

## The tension the references exposed

`editor-ui-ux` requires the editor to be *deliberately familiar* to Unity and Unreal users, with a
Unity-compatible keymap, on the argument that an expert's first hour should not be spent relearning
where things are. The design direction requires the opposite-sounding thing: that the editor be
immediately recognisable as Cyberdyne rather than a derivative.

Both are correct, and they resolve cleanly once the axis is named:

- **Familiar**: where panels are, how navigation works, what the transform tools do, what the play
  bar means, which key does what. This is muscle memory, and copying it is a gift to the user.
- **Distinct**: what it looks like, what the icons are, what the colours mean, and — most of all —
  what things are called. This is identity, and copying it makes the product read as a
  reimplementation.

That split is folded back into `editor-ui-ux`'s own requirement rather than asserted only here, so a
reader of either capability finds it.

## Vocabulary is the part that will be argued about

The reference images use Unreal's product vocabulary throughout: *Actors*, *Static Mesh*, *Blueprint*,
*Blueprint Log*, *Content Drawer*, *Level*. That is unsurprising — they are concept art, and the
fastest way to draw a convincing game editor is to draw the one everybody has seen.

Shipping it would be a mistake, and a subtle one. Nothing about "2,341 actors" is wrong on its own.
But an editor that calls entities Actors, graphs Blueprints, and the content browser a Content
Drawer has told the user what it is a copy of, before they have evaluated a single feature. The
engine already has its own words for all of these, chosen deliberately in the specifications, and
`ecs-core` and `scene-graph-and-nodes` in particular would be actively confusing if the interface
used a different object model's terms for entities and nodes.

The mitigation for the real cost — that users know the other words — is search aliases, not
adoption. Type "blueprint", find the script graph editor, see it labelled with the engine's term.
The user is served and the identity is intact.

## What the references get right, and what they get wrong

**Right, and worth stating precisely because it is easy to lose:**

- The scene carries the entire colour range of the screen. Every panel is charcoal. This is the
  single most important property of the direction and the easiest to erode one accent at a time.
- Selection is a thin gold outline that does not glow. It reads instantly against sunlit rock and
  against shadow, and it does not prevent judging the material underneath.
- Viewport chrome is overlaid — `Perspective`, `Lit`, `Show` float in the corner rather than
  occupying a toolbar. In the RTS reference the viewport is roughly two thirds of the window.
- The orientation widget is three axes and nothing else, and is visually much quieter than the
  transform gizmo a few hundred pixels away. Those two things must not be confusable, and here they
  are not.
- Density sits in the right band: the outliner shows around thirty rows without crowding, the
  inspector shows transform, unit, AI, abilities and rendering at once.
- Status is ambient. `● Live` and `All Saved` sit in the chrome and interrupt nothing.

**Wrong, or at least not to be copied literally:**

- The vocabulary, as above.
- In the adventure reference, the panel labelled **World Partition** contains CPU, GPU, memory and
  VRAM graphs. That is a profiler. World partition is a streaming capability with its own concerns,
  and mislabelling a panel in the reference would propagate into the implementation if nobody said
  so. The performance graphs belong to the profiler; the requirement here splits the ambient
  overlay from the profiler panel deliberately.
- The two references brand the header differently — one uses the Cyberdyne mark, the other a
  different symbol beside the words "CYBERDYNE EDITOR". The mark in
  `docs/design/images/cyberdyne-mark.png` is the normative one.
- The adventure reference shows a universal gizmo — translation arrows, rotation rings and scale
  boxes at once — on a slender column, and it is close to the density at which such a gizmo stops
  being individually acquirable. The requirement permits the mode and constrains it: readable, or it
  degrades. This is the specific case the constraint was written for.

## What a reference image is and is not

Both scenes depict volumetric clouds, procedurally generated forests, physically based skies with
aerial perspective, water with shoreline foam, and thousands of instanced units. None of that exists
before M7 to M10.

If a reference image is read as a target for *when the editor is finished*, it becomes an implicit
dependency on the last third of the roadmap and stops guiding anything at M5, which is when the
editor is actually built.

So the requirement states the distinction: a reference is a statement of **visual language** —
hierarchy, density, colour, chrome, composition — not of feature completeness. Every constraint in
this capability is implementable at M5 against whatever the renderer can produce, including a grey
box on a flat ground plane. The chrome, the colour, the density and the vocabulary do not depend on
what is in the scene.

## Where this lands on the roadmap

No re-sequencing. `editor-visual-language` reaches Working at **M5** with the rest of the editor and
Complete at **M11**.

Three of its requirements bind earlier than the capability itself, in the way the roadmap's
invariant table describes — cheap now, expensive to retrofit:

| Rule | Binds from | Why it cannot wait |
|---|---|---|
| Engine vocabulary | The first label drawn | Terminology spreads into every panel, every doc, every tutorial, and every user's habits |
| Semantic colour and the axis mapping | The first coloured element | A second meaning for a hue is discovered only when both appear on one screen |
| Disclosure decisions accompany new properties | The first inspector | Default views accrete; nothing is ever removed from one |

They are not added to the roadmap's invariant table, which is reserved for engine-wide invariants,
but they are stated here so the M5 change knows to carry them.

## Alternatives considered

**Style guide in `docs/` only, no specification.** Rejected for the reason at the top: documentation
that constrains nothing gets overtaken by locally reasonable decisions. Keeping the language
normative is also what makes the reference images a *view* rather than a second source of truth.

**Design tokens as the specification.** Naming exact hex values and pixel sizes now would be false
precision — no interface exists to tune them against, and the toolkit is deliberately an
implementation detail per `editor-rust-application`. The specification constrains *meaning and
relationship*; the palette that satisfies it is an implementation decision the M5 change makes and
records.

**Fold the gizmo rules into `editor-viewport-and-gizmos`.** Tempting, since that capability owns
gizmos. Rejected because it owns their *mechanism*, and legibility rules there would be read as
implementation guidance rather than as part of one visual system that also governs the inspector's
vector fields and the debug visualisations.

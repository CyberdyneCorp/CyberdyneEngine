# Design: UI system rework

## Context

UI is unusual among engine subsystems: it is simultaneously a layout engine, a styling engine, an
input router, a renderer, an animation system, and an authoring surface — and it must serve three
consumers with different demands. Game UI needs performance and controller navigation. World-space
UI needs renderer integration. The **editor** needs virtualised lists of tens of thousands of rows,
docking, and property grids, and is the most demanding consumer of all.

The decision that shapes everything else is where UI elements live.

## Decisions

### 1. UI elements are not ECS entities — reversing an earlier decision

The baseline specification made UI elements entities with UI components. This change reverses
that.

**Why the original decision was made.** Consistency: one world, one hierarchy, one serialization
path, one inspector. That reasoning is real and this reversal gives some of it up.

**Why it is wrong here.** Three reasons, in order of weight:

1. **Volume asymmetry.** UI element counts routinely exceed gameplay entity counts by an order of
   magnitude. A strategy game with 5,000 units has 20,000-plus labels, icons, spans and containers.
   Those entities sit in the same archetype storage that gameplay queries scan.
2. **Access pattern mismatch.** ECS is optimised for scanning many entities sharing a component
   set. UI is a *hierarchical* workload: measure walks bottom-up, arrange walks top-down, and each
   element is touched by exactly one system. Archetype iteration, queries, change detection and
   parallel scheduling all cost something and return almost nothing here.
3. **Invalidation granularity.** UI needs three independent dirty states per element — measure,
   arrange, paint — propagating different distances up and down the tree. Expressing that as ECS
   change detection means chunk-granular dirtiness on data that needs element-granular precision.

**What is kept.** ECS integration where it earns its place: a scene node may *host* a UI document,
world-space UI attaches to entities, and data binding reads component data through change
detection. UI is adjacent to ECS, not inside it.

**Cost accepted.** UI is no longer inspectable as entities in the scene hierarchy, and needs its
own serialization, its own inspector integration, and its own undo support in the editor. That is
real work, and it is worth it.

### 2. Retained mode is the foundation; immediate mode is a tool

The retained tree persists across frames. Changing a health value marks one label's paint state
dirty; it does not rebuild a tree.

```
Health 92 → 91          Text content changes width
      │                             │
   paint dirty                measure dirty
      │                             │
   GPU buffer patch           arrange dirty
                                    │
                               paint dirty
```

An immediate-mode API also exists, for debug overlays and editor tooling, compiling into the same
primitive stream. It is explicitly **not** the production UI architecture: immediate mode rebuilds
per frame, which is the cost retained mode exists to avoid.

**Alternative rejected — immediate mode as the foundation.** Excellent for tools, wrong for
production UI at this scale: per-frame rebuild of 20,000 elements wastes the invalidation
structure that makes UI cheap, and complicates animation, focus, and accessibility, all of which
want persistent identity.

### 3. Declarative authoring diffs into the retained tree

The Swift DSL and the C++ API produce a **description**, which is diffed against the current
retained tree; only differences are applied.

```
Swift DSL / C++ DSL / Designer
              │
         UI description
              │
            diff
              │
      retained UI tree  ──► dirty flags ──► layout ──► flatten ──► GPU
```

**Rationale.** This is how modern declarative UI gets ergonomics without cost. Rebuilding widgets
every frame — the naive reading of a SwiftUI-like API — would forfeit the entire invalidation
design.

**Trade-off accepted.** Diffing needs stable identity across rebuilds. Elements therefore carry
identity from structural position plus an optional explicit key, with the same caveats as any
diffing UI framework: reordering without keys causes state to follow position rather than data.

### 4. One document representation, three authoring surfaces

The Swift DSL, the C++ API, and the visual designer all produce and consume the same **UI
document** asset.

**Rationale.** This is the specific thing to avoid about layered UI stacks, where a designer-built
asset and code-built UI are different technologies that interoperate awkwardly. One representation
means a designer can open what a programmer wrote, and the runtime has one loader.

**Trade-off accepted.** The document format constrains all three surfaces to a common expressive
core; anything one surface can express, the others must be able to represent.

### 5. Stay close to CSS

Styling uses `.cyss`: a **CSS-compatible subset** — selectors by type, class and id, pseudo-states
(`:hover`, `:focus`, `:active`, `:disabled`), descendant and child combinators, custom properties
(`--accent`), and the cascade with specificity.

**Rationale.** CSS is widely understood, well-documented, and has decades of tooling and
convention behind it. Inventing a styling language means teaching it, tooling it, and defending
its differences forever. Staying close means a new hire already knows most of it.

**Explicitly not adopted:** the full CSS box model's historical quirks, floats, `position: static`
semantics, the full selector grammar, and cascade layers. The subset is documented as a subset,
with divergences listed, rather than pretending to be a browser.

### 6. Flex, grid, absolute — nothing else

Three layout models, each with well-understood semantics. No bespoke anchor-plus-pivot-plus-offset
system with its own mental model.

**Rationale.** Flexbox and grid are solved problems with enormous shared understanding. Absolute
positioning with anchors covers HUD placement. A fourth model would be a fourth thing to learn.

### 7. GPU-driven rendering with a flattened primitive stream

Layout output flattens into a primitive buffer — bounds, UVs, material index, clip index,
transform index — batched and drawn with few indirect draws.

```
   20,000 elements  →  visibility + clip cull  →  ~5,000 primitives  →  a few batches
```

Text glyphs, rounded rectangles, borders, gradients, shadows and images share one primitive
representation and, where possible, one shader with material-indexed behaviour, rather than each
widget type owning a draw path.

**Rationale.** Per-element draw submission is what makes conventional UI expensive at editor
scale. Flattening also makes UI cost measurable and budgetable in the same terms as everything
else.

### 8. Layers and navigation are system-level, not game-level

A layer stack (game, HUD, overlay, modal, system) with per-layer input routing, focus scoping,
back-navigation, and modal blocking.

**Rationale.** Every game re-implements this, badly, if the engine does not provide it. Opening a
settings screen should automatically capture focus, block input beneath, register a back action,
and restore focus on close. This is the single most valuable idea to borrow from CommonUI.

### 9. Semantic input actions

UI consumes `UI.Accept`, `UI.Cancel`, `UI.NavigateUp/Down/Left/Right`, `UI.NextTab`,
`UI.PreviousTab` — never raw keys or buttons — with platform-appropriate default bindings.

**Rationale.** Device-specific input in UI code is how a project ends up unable to add controller
support. It also gives correct glyph display for free.

### 10. Animation is internal

UI animation is part of the UI system: style state transitions, declarative modifiers, and a
timeline in the designer — not the general tween system.

**Rationale.** UI animation is high-frequency, high-count, and tightly coupled to style state.
Running it through a general entity-bound tween system means crossing the scripting boundary per
element per frame and coupling UI to entity lifetime. The general tween system remains for
gameplay.

## Risks

- **This is a large surface.** Layout, styling, input, rendering, animation, and authoring are
  each substantial. Mitigation: the layered structure means layout and rendering can be built and
  tested before the declarative surface exists.
- **CSS subset boundary disputes.** Users will expect CSS features the subset omits. Mitigation:
  the subset and its divergences are documented as a requirement, not discovered.
- **Diffing identity bugs** are notoriously hard to diagnose. Mitigation: explicit keys, and a
  diagnostic that reports elements whose identity changed unexpectedly between frames.
- **Editor as first consumer** means the system must be good before the editor is usable, which
  front-loads risk. Mitigation is also the benefit: defects surface immediately in daily use.

## Open questions

- Whether the UI document format should be text-first (readable, diffable, hand-editable) or
  binary-first with a text projection. Leaning text-first for the same reasons scenes are.
- Whether style resolution should be fully dynamic (recomputed on state change) or partially
  compiled at cook time for static style sheets. Deferred pending measurement.
- Whether the immediate-mode API should be able to host retained sub-trees for hybrid tooling.
  Deferred; not needed for the first milestone.

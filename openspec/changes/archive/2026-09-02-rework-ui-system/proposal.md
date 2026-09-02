# Rework the UI system as a dedicated, GPU-driven, declaratively-authored framework

## Why

The `ui-system` specification currently makes UI elements ECS entities. That decision is wrong for
this engine and this change reverses it.

The reasoning was consistency — one world, one hierarchy, one serialization path. In practice UI
element counts are an order of magnitude above gameplay entity counts (a strategy game with 5,000
units easily has 20,000 labels, icons, spans and layout containers), their access pattern is a
tree walk rather than an archetype scan, and almost none of the ECS machinery — archetypes,
queries, change detection, parallel systems — pays for itself on data that is traversed
hierarchically and touched by exactly one system. Putting UI in the gameplay ECS makes gameplay
queries slower to serve a subsystem that gains nothing from being there.

Beyond that, the current specification describes a competent but conventional UI system. UI is one
of the subsystems this engine should own outright, alongside the ECS, renderer and VFX, and the
target is worth stating concretely: Unreal-class capability, Unity UI Toolkit-like styling,
SwiftUI-like ergonomics, CommonUI-class navigation, on a more aggressively data-oriented and
GPU-driven runtime — and **one** system serving games, world-space UI, and the editor, rather than
layers stacked on each other.

## What Changes

- **BREAKING: UI elements are no longer ECS entities.** They live in dedicated, data-oriented UI
  storage addressed by a lightweight `UIElementID`. ECS integration remains where it earns its
  place: a node may *host* a UI document, world-space UI attaches to entities, and data binding
  reads component data.
- **Retained tree with fine-grained invalidation** as the foundation: separate measure, arrange,
  and paint dirty state propagating only as far as each actually requires.
- **A UI document asset** — the single representation that the Swift DSL, the C++ API, and the
  visual designer all produce and consume, so there is one system rather than three.
- **Declarative authoring** in Swift and C++, diffed into the retained tree rather than rebuilding
  it. Declarative ergonomics without immediate-mode cost.
- **An immediate-mode API** for tooling and debug overlays, feeding the same renderer.
- **CSS-compatible stylesheets** (`.cyss`): selectors, classes, pseudo-states, custom properties,
  and cascade — deliberately close to CSS rather than a new language.
- **Flex, grid, and absolute/anchored** layout as the three models, replacing the
  container-type-centric framing.
- **GPU-driven rendering**: the tree flattens to a primitive buffer, batched and drawn with few
  indirect draws rather than per-element submission.
- **A UI layer stack** with modal semantics, focus scoping, back navigation, and input routing
  handled by the system rather than coordinated by hand in game code.
- **Semantic input actions** (`UI.Accept`, `UI.Cancel`, directional navigation) with
  platform-appropriate bindings, so game code is device-agnostic.
- **UI materials and effects** — blur, glass, SDF shapes, gradients, distortion, render targets,
  custom shaders — as first-class, not an escape hatch.
- **World-space and surface-space UI** publishing into the GPU scene.
- **Animation built into the system** rather than delegated to the general tween system, so
  transitions run without crossing into script per frame.
- **A UI frame budget**, consistent with the audio and VFX budget controllers.

Non-goals: a browser engine, full CSS (only a defined subset), HTML/DOM compatibility, and
retained-mode immediate APIs pretending to be production UI.

## Capabilities

### New Capabilities

None. This reworks an existing capability.

### Modified Capabilities

- `ui-system` — substantially reworked: element storage, retained tree, document asset,
  declarative and immediate APIs, designer, stylesheets, layout, GPU-driven rendering, layer
  stack, semantic input, materials, world-space UI, animation, budget, and diagnostics.
- `scene-graph-and-nodes` — the node template list referenced "UI root and UI elements"; only a
  UI **host** node remains, since elements are no longer nodes.
- `animation-and-skinning` — the tween requirement's UI example is replaced, since UI animation is
  now internal to the UI system.
- `rendering-architecture` — world-space UI joins the GPU scene's list of instance producers.

## Impact

- **BREAKING** for the existing specification: any implementation assuming UI-as-entities is
  invalidated. No code exists yet, so the cost is limited to the specification.
- **ECS**: removes a large class of entities from the gameplay world; gameplay queries no longer
  scan UI archetypes.
- **Renderer**: UI becomes a GPU scene producer and a consumer of render targets for blur-behind
  and surface-space UI.
- **Editor**: the editor is built on this system, so its virtualised lists, docking, and property
  grids become the system's most demanding consumer.
- **Dependencies**: none added. Text continues through `TextServer` (HarfBuzz + ICU + FreeType).

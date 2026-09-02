# Tasks: UI system rework

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change was archived on that basis.

Sections 3 to 8 record the implementation the decision implies and are **deliberately deferred to
implementation changes**. They are listed so the scope is not lost. The layout and style engines
(section 3) and the diffing algorithm (4.1) are where the risk concentrates; see `design.md`.

## 1. Specification

- [x] 1.1 Record the ECS reversal, its rationale, and the cost accepted, in `design.md`
- [x] 1.2 Rework `ui-system`: remove UI-as-ECS; add dedicated element storage, retained tree with
      three-way invalidation, UI document asset, declarative authoring with diffing, immediate
      mode, `.cyss` style sheets, GPU-driven flattening, layer stack, semantic input, UI
      materials, world/surface-space UI, and a UI frame budget
- [x] 1.3 Modify layout (flex/grid/absolute), input routing, styling, data binding, rendering,
      animation (internal, not tweens), editor sharing, and diagnostics
- [x] 1.4 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `scene-graph-and-nodes` — node templates now list a **UI host** node; individual UI
      elements are explicitly not node templates
- [x] 2.2 `animation-and-skinning` — tweens scoped to gameplay and scene properties; the UI
      transition scenario now states that UI animation is internal to the UI system
- [x] 2.3 `rendering-architecture` — world-space and surface-space UI added to the GPU scene's
      instance producers
- [x] 2.4 `text-and-fonts` — reviewed; no change needed. UI consumes `TextServer` through the
      existing interface, and inline objects already cover UI elements in text layout.
- [x] 2.5 `core-platform-abstraction` — reviewed; no change needed. The accessibility tree
      interface is unchanged by where UI elements are stored.
- [x] 2.6 `ecs-core` — reviewed; no requirement change needed. Removing UI from the ECS narrows
      what the world contains but changes no ECS requirement.

## 3. Core runtime (deferred to implementation)

- [ ] 3.1 UI element store: SoA arrays, generational `UIElementID`, dense pooling
- [ ] 3.2 Retained tree with measure/arrange/paint dirty propagation
- [ ] 3.3 Layout: flex, grid, absolute, following the documented CSS subset semantics
- [ ] 3.4 Style engine: `.cyss` parser, selector matching, cascade, specificity, custom properties
- [ ] 3.5 Document the CSS subset and every divergence, as a shipped reference
- [ ] 3.6 UI document asset: text form, binary form, composition with parameters

## 4. Authoring surfaces (deferred to implementation)

- [ ] 4.1 Description model and the diffing algorithm, including identity and keys
- [ ] 4.2 Swift declarative DSL and reactive state
- [ ] 4.3 C++ declarative API over the same description model
- [ ] 4.4 Immediate-mode API emitting into the same primitive stream
- [ ] 4.5 Visual designer producing and consuming the UI document

## 5. Rendering (deferred to implementation)

- [ ] 5.1 Flattening to the primitive stream, with incremental re-emission
- [ ] 5.2 Batching, clip culling, indirect draws
- [ ] 5.3 Text integration via `TextServer` and glyph atlases
- [ ] 5.4 UI materials: SDF shapes, gradients, shadows, masks, blur-behind, custom shaders
- [ ] 5.5 World-space and surface-space presentation via the GPU scene

## 6. Interaction (deferred to implementation)

- [ ] 6.1 Hit testing, pointer capture, multi-touch
- [ ] 6.2 Layer stack with modal semantics, focus scoping, back navigation
- [ ] 6.3 Semantic input actions and platform bindings, with active-device glyph reporting
- [ ] 6.4 Animation: state transitions, declarative modifiers, keyframes, springs, staggering
- [ ] 6.5 Accessibility publication and keyboard-only operation

## 7. Widgets and editor (deferred to implementation)

- [ ] 7.1 Core widget set
- [ ] 7.2 Virtualised list and tree
- [ ] 7.3 Text input with selection, undo, IME, RTL
- [ ] 7.4 Editor UI module: docking, property grids from reflection, graph canvas
- [ ] 7.5 Verify no editor panel requires a private API

## 8. Validation (deferred to implementation)

- [ ] 8.1 Layout conformance tests against the documented flex and grid semantics
- [ ] 8.2 Style cascade and specificity tests
- [ ] 8.3 Diffing tests: identity preservation, key behaviour, focus and scroll retention
- [ ] 8.4 Golden-image tests for widgets and effects across backends
- [ ] 8.5 Invalidation tests: assert that a paint-only change performs no layout work
- [ ] 8.6 Benchmark: 20,000-element document, and a 100,000-row virtualised list, as regression
      guards

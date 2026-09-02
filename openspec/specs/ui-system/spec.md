# ui-system Specification

## Purpose

Defines **CyberUI**: one user interface system serving game UI, world-space UI, and the editor.

Four decisions shape it. UI elements live in **dedicated data-oriented storage**, not in the
gameplay ECS — element counts run an order of magnitude above entity counts, and the workload is
hierarchical rather than an archetype scan. The tree is **retained** with three independent dirty
states (measure, arrange, paint), so a changing number repaints one label rather than rebuilding
anything. Authoring is **declarative in Swift and C++, diffed** into that retained tree, giving
SwiftUI-like ergonomics without immediate-mode cost — and the visual designer produces the same
document asset, so there is one system rather than layers stacked on each other. Rendering
**flattens to a GPU primitive stream** batched into few indirect draws, rather than submitting per
element.

Styling is a deliberately CSS-compatible subset (`.cyss`), layout is flex, grid, and absolute —
nothing bespoke. Navigation borrows the layer stack and semantic input actions that make
controller support and modal flows work without every game re-implementing them. Text is delegated
to `TextServer` (HarfBuzz, ICU, FreeType), because correct international text is not a place to
build your own.

The editor is built on this system, which makes it the most demanding consumer and the reason
virtualisation and docking are core rather than optional.

## Requirements

### Requirement: Engine-owned UI system
The UI system SHALL be engine code: element storage, layout, styling, input routing, animation,
rendering, and authoring surfaces.

One system SHALL serve **game UI**, **world-space UI**, and the **editor**, rather than separate
technologies layered on each other.

The system SHALL be removable at build time via `CY_UI`.

#### Scenario: One system, three consumers
- **WHEN** the editor, a game HUD, and a world-space health bar are all rendered
- **THEN** they SHALL use the same element storage, layout, styling, and rendering code

#### Scenario: No UI toolkit dependency
- **WHEN** the dependency manifest is audited
- **THEN** it SHALL contain no third-party UI toolkit; text remains delegated to `TextServer`

### Requirement: Dedicated UI element storage
UI elements SHALL be stored in a dedicated, data-oriented UI store addressed by a lightweight
`UIElementID`, **not** as ECS entities.

Storage SHALL be structure-of-arrays over parallel arrays for hierarchy, layout input, computed
layout output, style reference, flags, and paint data, so each pass touches only what it needs.

Elements SHALL be allocated from a dense pool with generational ids, so a stale `UIElementID` is
detected rather than aliasing a recycled element.

ECS integration SHALL be retained only where it is warranted:

- a scene node MAY **host** a UI document, giving it a place in the scene and a lifetime
- **world-space UI** attaches to an entity's transform
- **data binding** reads component data through ECS change detection

#### Scenario: UI does not burden gameplay queries
- **WHEN** a game has 5,000 gameplay entities and 20,000 UI elements
- **THEN** gameplay queries SHALL scan only gameplay archetypes, and UI elements SHALL not appear
  in the ECS world at all

#### Scenario: Pass touches only its data
- **WHEN** the arrange pass runs
- **THEN** it SHALL read layout input and write layout output without touching paint or style
  payloads

#### Scenario: Stale element id
- **WHEN** an element is destroyed and its slot reused
- **THEN** the previous `UIElementID` SHALL fail validation rather than resolving to the new
  element

### Requirement: Retained tree with fine-grained invalidation
The UI tree SHALL persist across frames. Changes SHALL mark elements dirty rather than triggering
a rebuild.

Three independent dirty states SHALL be maintained per element, each propagating only as far as it
must:

| State | Meaning | Propagation |
|---|---|---|
| `Measure` | Desired size may have changed | Upward while ancestors' desired size depends on it |
| `Arrange` | Final rect may have changed | Downward through affected subtree |
| `Paint` | Visual output changed, geometry unchanged | The element only |

Each frame SHALL process only dirty work: re-measure dirty subtrees, re-arrange affected regions,
and re-emit paint data for repainted elements.

#### Scenario: Value change repaints only
- **WHEN** a label's numeric text changes without changing its measured size
- **THEN** only its paint state SHALL be dirtied, and no layout work SHALL occur

#### Scenario: Text growth relayouts locally
- **WHEN** a label's text grows and changes its desired size
- **THEN** measure SHALL propagate upward only while ancestors' desired sizes depend on it, and
  arrange SHALL re-run only for affected subtrees

#### Scenario: Idle UI costs nothing
- **WHEN** no UI state changes in a frame
- **THEN** no layout or paint work SHALL be performed, and the previous frame's GPU buffers SHALL
  be reused

### Requirement: UI document asset
A **UI document** SHALL be the single asset representation of a UI tree, its styles, its bindings,
and its animations.

The Swift DSL, the C++ API, and the visual designer SHALL all produce and consume this same
representation, so no authoring surface has capabilities the others cannot express or read.

Documents SHALL be composable: a document may instantiate another as a sub-tree with parameters,
which is how reusable components are built.

The document SHALL have a text form for version control, following the conventions in
`serialization-and-prefabs`.

#### Scenario: Designer opens what code wrote
- **WHEN** a UI tree authored in Swift is opened in the visual designer
- **THEN** it SHALL be displayed and editable, because both operate on the same document

#### Scenario: Reusable component
- **WHEN** a document instantiates another with parameters
- **THEN** the instance SHALL reflect the source document's changes, with parameter overrides
  preserved

#### Scenario: Diffable text form
- **WHEN** one property of one element changes
- **THEN** the text diff SHALL show that single change

### Requirement: Declarative authoring
The engine SHALL provide declarative UI construction in **Swift** and in **C++**, producing a
description that is **diffed** into the retained tree.

The description SHALL NOT be rebuilt into new elements each frame: only differences SHALL be
applied, preserving element identity, animation state, focus, and scroll position across rebuilds.

Element identity SHALL derive from structural position plus an optional explicit key. The
documentation SHALL state the consequence: reordering children without keys causes state to follow
position rather than data.

Reactive state SHALL be supported, so a change to bound state schedules a description rebuild for
the affected subtree only, not the whole document.

#### Scenario: Declarative without per-frame rebuild
- **WHEN** a declarative view's bound state changes
- **THEN** only the affected subtree's description SHALL be rebuilt and diffed, and unaffected
  elements SHALL retain identity and state

#### Scenario: Focus survives a rebuild
- **WHEN** a list rebuilds while one of its text fields has focus and a selection
- **THEN** focus, caret, selection, and scroll position SHALL be preserved for elements whose
  identity is unchanged

#### Scenario: Keys preserve identity across reorder
- **WHEN** list items are reordered and declare explicit keys
- **THEN** each item's element state SHALL follow its key, not its position

### Requirement: Immediate-mode API
The engine SHALL provide an immediate-mode UI API for debug overlays, development tooling, and
editor utilities, emitting into the same primitive stream and renderer as the retained system.

The immediate-mode API SHALL be documented as **not** the production UI architecture, and SHALL be
excludable from shipping builds.

#### Scenario: Debug overlay
- **WHEN** a developer writes an immediate-mode stats window
- **THEN** it SHALL render through the same batching and text pipeline as retained UI, with no
  separate renderer

#### Scenario: Excluded from shipping
- **WHEN** a shipping build is produced
- **THEN** immediate-mode UI code SHALL be excludable, and its absence SHALL not affect retained UI

### Requirement: Layout model
Layout SHALL be a two-pass process: **measure** (compute each element's desired size bottom-up)
then **arrange** (assign final rects top-down), driven by the dirty states in the retained tree.

Three layout models SHALL be supported, and no others:

| Model | Semantics |
|---|---|
| **Flex** | Row and column with direction, wrap, gap, justify and align, grow, shrink, and basis |
| **Grid** | Explicit and implicit tracks, spans, gaps, and area placement |
| **Absolute** | Anchors (fractions of the parent rect) plus offsets, for HUD placement |

Each element SHALL declare: preferred, minimum, and maximum size; margins and padding; alignment
within its allocated space; flex grow, shrink, and basis; and an aspect-ratio constraint.

Flex and grid semantics SHALL follow their CSS definitions within the documented subset, so
existing understanding transfers.

Scrolling, wrapping, and splitting SHALL be behaviours of container elements built on these three
models, not additional layout models.

#### Scenario: Flexible row
- **WHEN** a flex row has more width than its children's minimum
- **THEN** surplus SHALL be distributed by flex grow factors, respecting each child's maximum

#### Scenario: Text drives layout
- **WHEN** a label's text changes and its desired size grows
- **THEN** the measure pass SHALL propagate the change upward and the arrange pass SHALL
  re-layout affected ancestors only

#### Scenario: Layout is incremental
- **WHEN** one element is invalidated
- **THEN** only the dirty subtree and its size-affecting ancestors SHALL be re-measured and
  re-arranged

#### Scenario: Anchored HUD element
- **WHEN** a minimap is positioned absolutely against the bottom-right corner with a fixed size
- **THEN** it SHALL remain anchored there across window resizes and aspect changes

### Requirement: Resolution independence
UI SHALL be authored against a **reference resolution** and scaled to the actual output, with
selectable strategies: scale with width, scale with height, scale with the smaller or larger
dimension, match a blend, or fixed pixel size.

UI SHALL respect the platform **DPI scale**, and SHALL support a user-configurable UI scale
factor on top.

#### Scenario: Different aspect ratio
- **WHEN** the window aspect differs from the reference
- **THEN** the chosen strategy SHALL determine scaling, and anchored elements SHALL remain
  attached to their intended edges

#### Scenario: High-DPI display
- **WHEN** the display reports a 2× scale
- **THEN** UI SHALL render at twice the pixel density with text rasterised at the effective size,
  not upscaled

### Requirement: Style sheets
Styling SHALL use **`.cyss`**, a deliberately **CSS-compatible subset**, supporting: type, class
and id selectors; descendant and child combinators; pseudo-states `:hover`, `:focus`, `:active`,
`:disabled`, `:checked`, and `:first-child`/`:last-child`; custom properties (`--name`) with
`var()` resolution; and the cascade with specificity and inline-override precedence.

The supported subset and its **divergences from CSS** SHALL be documented explicitly, rather than
approximating CSS and leaving differences to be discovered.

Style resolution order SHALL be: inline element overrides, matched rules by specificity, inherited
inheritable properties from ancestors, the active theme, then engine defaults.

Themes SHALL be swappable at runtime, re-resolving affected styles.

#### Scenario: Familiar authoring
- **WHEN** a developer who knows CSS writes a `.cyss` rule with a class selector and a hover state
- **THEN** it SHALL behave as CSS would within the documented subset

#### Scenario: Unsupported property is diagnosed
- **WHEN** a style sheet uses a CSS property outside the subset
- **THEN** cooking SHALL report it by name and location, rather than silently ignoring it

#### Scenario: Runtime theme swap
- **WHEN** the theme changes at runtime
- **THEN** affected elements SHALL re-resolve styles and re-layout only where sizes changed

### Requirement: Styling and theming
Visual appearance SHALL be defined by style sheets (see the style sheets requirement) resolved per
element, with a **theme** supplying the base rule set and custom property values.

A style SHALL be able to define: colours, fonts and font sizes, spacing and sizing, icons,
backgrounds (fills, borders, corner radii, shadows, nine-slice textures), transforms, opacity,
and transition timings.

Styles SHALL support **variants** (a named style derived from a base) and **states** (normal,
hovered, pressed, disabled, focused, selected, checked) with per-state overrides and transition
durations.

#### Scenario: Theme change at runtime
- **WHEN** the active theme changes
- **THEN** all elements SHALL re-resolve their styles and re-layout where sizes changed

#### Scenario: State transition
- **WHEN** a button is hovered with a 0.1 s transition
- **THEN** its background colour SHALL interpolate rather than switching

#### Scenario: Variant
- **WHEN** a button declares the variant "danger"
- **THEN** properties SHALL resolve from the danger variant first, falling back to the base button
  style

### Requirement: Data binding
The engine SHALL provide binding between UI element properties and data sources: reflected
component fields, resources, script-provided observable values, and reactive state in declarative
views.

Bindings SHALL support one-way and two-way modes with optional value converters, and SHALL update
only when their source changes — using ECS change detection where the source is component data.

A binding update SHALL mark only the affected element dirty, at the finest applicable granularity.

#### Scenario: Health bar
- **WHEN** a progress bar is bound to a `Health` component field
- **THEN** it SHALL update when that component changes, and not otherwise

#### Scenario: Two-way binding
- **WHEN** a text field is two-way bound to a value
- **THEN** editing the field SHALL write back, and external changes SHALL update the field unless
  it is being edited

#### Scenario: Binding granularity
- **WHEN** a bound value changes without affecting layout
- **THEN** only the bound element's paint state SHALL be dirtied

### Requirement: Input routing and focus
UI input SHALL be routed: hit-test from the topmost **layer** downward, deliver to the element
under the pointer, then bubble to ancestors unless handled.

Elements SHALL declare a **hit-test mode**: `Block` (consume), `Pass` (handle and continue), or
`Ignore` (transparent).

The system SHALL maintain: pointer-over state with enter and exit events, pressed state and
capture (an element that captured the pointer receives events until release), keyboard focus with
a visible focus indicator, and per-touch tracking for multi-touch.

**Focus navigation** SHALL use the semantic navigation actions, support explicit neighbours and
geometric fallback, and be scoped to the active layer.

Input consumption SHALL be resolved by the layer stack rather than by ad-hoc flags in game code.

#### Scenario: Pointer capture
- **WHEN** a slider is pressed and the pointer moves outside it
- **THEN** the slider SHALL continue receiving move events until release

#### Scenario: Gamepad navigation
- **WHEN** `UI.NavigateRight` is triggered
- **THEN** focus SHALL move to the explicitly declared right neighbour, or to the nearest
  focusable element in that direction within the active layer

#### Scenario: UI consumes gameplay input
- **WHEN** a modal layer is open
- **THEN** it SHALL block input from reaching gameplay, by the layer's declared behaviour rather
  than by game code checking a flag

### Requirement: Semantic input actions
UI SHALL consume **semantic actions** from `input-and-actions`, never raw device input:
`UI.Accept`, `UI.Cancel`, `UI.NavigateUp`, `UI.NavigateDown`, `UI.NavigateLeft`,
`UI.NavigateRight`, `UI.NextTab`, `UI.PreviousTab`, `UI.Context`, `UI.ScrollUp`, `UI.ScrollDown`.

Interface actions SHALL live in **mapping contexts** pushed onto the input user's context stack, so
that a modal consumes navigation while a background context does not observe it, and so that
gameplay actions bound to the same controls are suppressed while the interface holds focus.

The engine SHALL provide platform-appropriate default bindings for keyboard and mouse, gamepad,
and touch, and SHALL expose the **active control scheme** so UI can display correct button glyphs,
with the hysteresis defined in `input-and-actions` so glyphs do not flicker.

Text entry SHALL use the platform text input path, not interface actions or key interpretation.

Directional navigation SHALL support explicit neighbours and geometric fallback, constrained to
the focused layer's scope.

#### Scenario: Device-agnostic game code
- **WHEN** a button handles `UI.Accept`
- **THEN** it SHALL respond to Enter, left click, gamepad south button, or tap, with no
  device-specific code

#### Scenario: Glyph display follows the device
- **WHEN** the player switches from keyboard to gamepad
- **THEN** prompts SHALL update to the gamepad's glyphs

#### Scenario: Navigation stays in the layer
- **WHEN** directional navigation reaches the edge of a modal
- **THEN** focus SHALL NOT escape to elements beneath it

#### Scenario: A modal suppresses gameplay
- **WHEN** a modal interface layer is open
- **THEN** its context SHALL consume the actions it uses, and gameplay bound to the same controls
  SHALL NOT observe them

### Requirement: UI layer stack and navigation
The system SHALL provide a **layer stack** with defined semantics, ordered from back to front:
`Game`, `HUD`, `Overlay`, `Modal`, and `System`.

Pushing a layer SHALL, according to its declared behaviour: capture or scope focus, block or pass
input to layers beneath, register a back-navigation action, dim or blur content beneath, and play
enter and exit transitions.

Back navigation (`UI.Cancel`, a platform back gesture) SHALL be routed to the topmost layer that
declares a handler, popping layers in order.

Focus SHALL be **scoped** to the active layer, and restored to its previous target when a layer
pops.

#### Scenario: Opening a settings screen
- **WHEN** a settings screen is pushed as a `Modal`
- **THEN** focus SHALL move into it, input to layers beneath SHALL be blocked, a back action SHALL
  be registered, and the enter transition SHALL play — without game code coordinating any of it

#### Scenario: Focus restoration
- **WHEN** a modal is dismissed
- **THEN** focus SHALL return to the element that had it before the modal opened

#### Scenario: HUD remains visible under an overlay
- **WHEN** an inventory `Overlay` is open
- **THEN** the HUD SHALL remain visible but non-interactive, per the overlay's declared behaviour

### Requirement: Widget set
The engine SHALL provide: `Panel`, `Label`, `RichText` (markup, inline images, per-character
effects), `Image`, `Button`, `ToggleButton`, `Checkbox`, `RadioGroup`, `Slider`, `ProgressBar`,
`TextField` (single-line), `TextArea` (multi-line with selection, undo, and IME support),
`Dropdown`, `ListView` (virtualised), `TreeView` (virtualised), `TabView`, `ScrollView`,
`SplitView`, `Tooltip`, `Popup`, `Modal`, `MenuBar`, `ContextMenu`, `ColorPicker`,
`NumericField`, and `Separator`.

List and tree views SHALL be **virtualised**: only visible items are realised, so large data sets
scroll without cost proportional to their size.

Text input SHALL support: selection, clipboard, undo and redo, IME composition, RTL editing,
input masks and validation, and platform text-editing keyboard conventions.

#### Scenario: Large list
- **WHEN** a list view displays 100 000 items
- **THEN** only visible rows SHALL be realised, and scrolling SHALL cost the same as for 100 items

#### Scenario: IME input
- **WHEN** a user composes text with an IME
- **THEN** the composition string SHALL be displayed inline with the candidate window positioned
  at the caret

### Requirement: Animation and transitions
UI animation SHALL be **internal to the UI system**, not delegated to the general tween system, so
transitions run without crossing the scripting boundary per element per frame and without coupling
UI lifetime to entity lifetime.

The system SHALL support: style state transitions with duration, delay, and easing; declarative
animation modifiers on elements; keyframe animations declared in style sheets; spring dynamics;
and staggered sequences.

Animations affecting only paint properties SHALL NOT trigger layout.

The visual designer SHALL expose a timeline and curve editor over the same animation model.

#### Scenario: Menu entrance
- **WHEN** a menu opens with a staggered slide-and-fade
- **THEN** each item SHALL animate with an incremental delay, evaluated inside the UI system

#### Scenario: Paint-only animation
- **WHEN** an element's opacity or colour animates
- **THEN** no layout work SHALL occur for any frame of the animation

#### Scenario: Reduced motion honoured
- **WHEN** the platform reports a reduced-motion preference
- **THEN** decorative animations SHALL be shortened or disabled while state feedback remains

### Requirement: GPU-driven rendering
Laid-out UI SHALL be **flattened** into a primitive stream — per primitive: bounds, UV rect,
material index, clip index, transform index, and colour — rather than submitted per element.

Primitives SHALL be culled against the viewport and their clip rects, batched by material and
atlas, and drawn with a small number of indirect draws.

Text glyphs, rounded rectangles, borders, gradients, shadows, and images SHALL share the primitive
representation and, where practical, one shader with material-indexed behaviour.

Flattening SHALL be incremental: unchanged regions SHALL reuse their previous primitive data
rather than being re-emitted.

#### Scenario: Many elements, few draws
- **WHEN** 20,000 elements produce 5,000 visible primitives
- **THEN** they SHALL be drawn in a small, reportable number of batches

#### Scenario: Incremental flattening
- **WHEN** one panel repaints in an otherwise static document
- **THEN** only its primitives SHALL be re-emitted and its GPU buffer range updated

#### Scenario: Clip culling
- **WHEN** elements lie outside their scroll container's clip rect
- **THEN** their primitives SHALL be culled before batching

### Requirement: Rendering
UI SHALL be rendered from the flattened primitive stream, batched by material and atlas, with
draw order derived from layer, tree order, and explicit z-overrides.

UI SHALL support: clipping to element rects (scissor, or stencil for rotated and rounded cases),
opacity groups (composited offscreen so overlapping children do not double-blend), and per-element
materials.

By default UI SHALL be drawn **after tonemapping** in display-referred colour; a per-document
option SHALL allow HDR UI drawn before tonemapping.

#### Scenario: Nested clipping
- **WHEN** a scroll view contains another scroll view
- **THEN** clipping SHALL be the intersection of both rects

#### Scenario: Opacity group
- **WHEN** a panel with overlapping children is faded to 50 %
- **THEN** it SHALL be composited once at 50 %, without the overlaps darkening

#### Scenario: HDR UI
- **WHEN** a document opts into HDR
- **THEN** it SHALL be composited before tonemapping so emissive UI participates in bloom and
  exposure

### Requirement: UI materials and effects
Elements SHALL support **custom materials** authored through the material system, and the engine
SHALL provide built-in effects: gradients, SDF-based shapes and borders, drop and inner shadows,
masking by shape or texture, blur-behind (frosted glass), distortion, colour adjustment, and
render-target sampling.

Effects requiring a source of the content beneath (blur-behind, distortion) SHALL declare it, and
the render graph SHALL schedule the necessary capture.

Video and 3D render targets SHALL be usable as element content.

#### Scenario: Frosted panel
- **WHEN** a panel requests blur-behind
- **THEN** the content beneath SHALL be captured and blurred, with the graph inserting the capture
  and its synchronisation

#### Scenario: Custom material
- **WHEN** an element is assigned a custom UI material
- **THEN** it SHALL be batched with other elements sharing that material and drawn without a
  bespoke code path

#### Scenario: SDF shapes stay sharp
- **WHEN** a rounded rectangle or icon is scaled or animated
- **THEN** SDF-based rendering SHALL keep its edges sharp without re-rasterisation

### Requirement: World-space and surface-space UI
A UI document SHALL be presentable in three spaces:

| Space | Behaviour |
|---|---|
| `Screen` | Overlaid on the viewport in screen coordinates |
| `World` | Positioned in the 3D scene with depth testing and optional lighting and occlusion |
| `Surface` | Rendered to a texture consumed by a material, for in-world screens |

World-space UI SHALL publish into the **GPU scene** as an instance producer, participating in
culling and LOD like other instances, rather than being a special-case scene traversal.

World-space UI SHALL support billboarding, fixed screen size, distance-based fade, and a maximum
interaction distance.

#### Scenario: Health bar above a unit
- **WHEN** a world-space document is attached to an entity
- **THEN** it SHALL follow that entity, be culled with the scene, and be occluded by geometry
  according to its depth settings

#### Scenario: In-world monitor
- **WHEN** a document is presented in `Surface` space
- **THEN** it SHALL render to a texture usable by any material, with input mapped from a ray
  intersection against that surface

#### Scenario: Distant world UI is cheap
- **WHEN** world-space UI is far away
- **THEN** it SHALL be reduced in detail or culled by the GPU scene's LOD, without UI-specific
  logic

### Requirement: UI frame budget
The UI system SHALL report and bound its cost, consistent with the audio and VFX budget
controllers.

Budgets SHALL cover: layout time, primitive count, batch count, and blur-behind or render-target
capture passes.

When budgets are exceeded, the system SHALL degrade in a defined order — reducing effect quality,
disabling blur-behind, and reducing world-space UI detail — before reducing anything affecting
interaction or legibility, and SHALL report the degradation.

#### Scenario: Complex screen exceeds the budget
- **WHEN** a screen with many blurred panels exceeds the effects budget
- **THEN** blur quality SHALL be reduced before any element is dropped, and the degradation SHALL
  be reported

#### Scenario: Interaction is never sacrificed
- **WHEN** budgets are under pressure
- **THEN** hit-testing, focus, and text legibility SHALL be preserved

### Requirement: Accessibility
UI elements SHALL publish accessibility information — role, label, description, value, and state —
to the platform accessibility layer where available.

The system SHALL support: keyboard-only operation of every interactive element, focus indication
that meets contrast requirements, respect for the platform's reduced-motion and increased-contrast
settings, and configurable text scaling.

#### Scenario: Screen reader
- **WHEN** focus moves to a slider with accessibility available
- **THEN** its role, label, and current value SHALL be announced

#### Scenario: Reduced motion
- **WHEN** the platform reports a reduced-motion preference
- **THEN** decorative transitions SHALL be shortened or disabled while functional feedback
  remains

### Requirement: Editor UI shares the runtime UI
The editor SHALL be built with this UI system rather than a separate toolkit, so improvements
benefit both and the system is exercised by its most demanding consumer.

The editor SHALL specifically exercise: virtualised lists and trees of tens of thousands of rows,
docking layouts, property grids generated from reflection, graph canvases, and text editing.

Editor-only widgets SHALL live in an editor UI module layered on the runtime widgets, using only
the public API.

#### Scenario: Editor exercises the UI system
- **WHEN** the editor is used daily
- **THEN** it SHALL exercise virtualisation, docking, and text editing, so defects surface during
  development rather than in games

#### Scenario: No private editor API
- **WHEN** an editor panel needs a capability the public UI API lacks
- **THEN** the capability SHALL be added to the public API rather than accessed privately

### Requirement: UI diagnostics
The engine SHALL provide: an element inspector showing the tree with computed layout rects,
resolved style values and their source rule, and dirty state; counts of measure, arrange, and
paint invalidations per frame; primitive and batch counts with batch-break reasons; per-frame UI
CPU and GPU time against the budget; overdraw visualisation; and hit-test debugging showing which
element would receive a pointer event and why.

The system SHALL report **elements whose diffing identity changed unexpectedly** between rebuilds,
since that is the most common and least obvious source of declarative UI bugs.

#### Scenario: Layout thrash
- **WHEN** an element invalidates layout every frame
- **THEN** the diagnostic SHALL identify it and the property causing the invalidation

#### Scenario: Style source is traceable
- **WHEN** an element has an unexpected colour
- **THEN** the inspector SHALL show which rule in which style sheet supplied it, and what it
  overrode

#### Scenario: Identity churn is reported
- **WHEN** a declarative list rebuilds and elements lose state unexpectedly
- **THEN** the diagnostic SHALL report the identity changes, pointing at the missing keys

# ui-system Specification

## Purpose

Defines the user interface system used for both game UI and the editor: the layout model, input
routing and focus, styling and theming, the widget set, and how UI integrates with ECS and Swift
scripting.

The layout model takes **flexbox-style constraint layout** as its primary mechanism (predictable,
well-understood, and familiar) with an **anchor/offset** mode available for absolute positioning,
following Godot's proven combination.

## Requirements

### Requirement: UI is built on ECS
UI elements SHALL be entities with UI components, participating in the same world, hierarchy, and
serialization as everything else.

A UI hierarchy SHALL be rooted at a **UI canvas** entity declaring its coordinate space
(screen space, camera space, or world space) and its reference resolution.

#### Scenario: UI in the scene hierarchy
- **WHEN** a designer inspects the scene
- **THEN** UI elements SHALL appear as ordinary entities with components, editable like any other

#### Scenario: World-space UI
- **WHEN** a canvas is set to world space
- **THEN** its elements SHALL be rendered into the 3D scene with depth testing and optional
  lighting

### Requirement: Layout model
Layout SHALL be a two-pass process per canvas: **measure** (compute each element's desired size
bottom-up) then **arrange** (assign final rects top-down).

Primary layout SHALL be constraint-based with these container types: `Row`, `Column`, `Stack`
(overlay), `Grid`, `Wrap`, `Scroll`, `Split`, and `Absolute`.

Each element SHALL declare: preferred, minimum, and maximum size; margins and padding; alignment
within its allocated space; a flex grow and shrink factor; and an aspect-ratio constraint.

**Anchor mode** SHALL additionally be available: four anchors (fractions of the parent rect) plus
four offsets (pixels), for elements positioned absolutely relative to a parent edge or centre.

#### Scenario: Flexible row
- **WHEN** a `Row` has more width than its children's minimum
- **THEN** surplus SHALL be distributed by flex grow factors, respecting each child's maximum

#### Scenario: Text drives layout
- **WHEN** a label's text changes and its desired size grows
- **THEN** the measure pass SHALL propagate the change upward and the arrange pass SHALL
  re-layout affected ancestors only

#### Scenario: Layout is incremental
- **WHEN** one element is invalidated
- **THEN** only the dirty subtree and its size-affecting ancestors SHALL be re-measured and
  re-arranged

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

### Requirement: Input routing and focus
UI input SHALL be routed: hit-test from the topmost canvas downward, deliver to the element under
the pointer, then bubble to ancestors unless handled.

Elements SHALL declare a **hit-test mode**: `Block` (consume), `Pass` (handle and continue), or
`Ignore` (transparent).

The system SHALL maintain: pointer-over state with enter and exit events, pressed state and
capture (an element that captured the pointer receives events until release), keyboard focus with
a focus ring, and per-touch tracking for multi-touch.

**Focus navigation** SHALL support explicit neighbours and automatic geometric navigation for
gamepad and keyboard.

#### Scenario: Pointer capture
- **WHEN** a slider is pressed and the pointer moves outside it
- **THEN** the slider SHALL continue receiving move events until release

#### Scenario: Gamepad navigation
- **WHEN** the user presses right on a gamepad
- **THEN** focus SHALL move to the explicitly declared right neighbour, or to the nearest
  focusable element in that direction

#### Scenario: UI consumes gameplay input
- **WHEN** a modal dialog is open
- **THEN** it SHALL consume input so gameplay does not also receive it, via an input-layer
  priority rather than ad-hoc flags

### Requirement: Styling and theming
Visual appearance SHALL be defined by **styles** resolved per element from, in priority order:
the element's inline overrides, its assigned style, styles on ancestors, the active **theme**,
and finally the engine default theme.

A style SHALL define typed properties: colours, fonts and font sizes, spacing and sizing
constants, icons, and **panels** (backgrounds with fills, borders, corner radii, shadows, and
nine-slice textures).

Styles SHALL support **variants** (a named style derived from a base type's style) and **states**
(normal, hovered, pressed, disabled, focused, selected) with per-state property overrides and
transition durations.

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

### Requirement: Data binding
The engine SHALL provide a binding mechanism connecting UI element properties to data sources —
reflected component fields, resources, or script-provided observable values — with one-way and
two-way modes and optional value converters.

Bindings SHALL update only when their source changes, using the ECS change detection where the
source is component data.

#### Scenario: Health bar
- **WHEN** a progress bar is bound to a `Health` component field
- **THEN** it SHALL update when that component changes, and not otherwise

#### Scenario: Two-way binding
- **WHEN** a text field is two-way bound to a value
- **THEN** editing the field SHALL write back, and external changes SHALL update the field unless
  it is being edited

### Requirement: Rendering
UI SHALL be rendered through the 2D pipeline, batched by material and atlas, with a draw order
derived from tree order and explicit z-overrides.

UI SHALL support: clipping to element rects (scissor or stencil for rotated cases), opacity
groups (composited offscreen so overlapping children do not double-blend), blur-behind for
frosted panels, and custom materials per element.

By default UI SHALL be drawn **after tonemapping** in display-referred colour; a per-canvas option
SHALL allow HDR UI drawn before tonemapping.

#### Scenario: Nested clipping
- **WHEN** a scroll view contains another scroll view
- **THEN** clipping SHALL be the intersection of both rects

#### Scenario: Opacity group
- **WHEN** a panel with overlapping children is faded to 50 %
- **THEN** it SHALL be composited once at 50 %, without the overlaps darkening

### Requirement: Animation and transitions
UI elements SHALL support property animation through the tween system and through style state
transitions, with common presets (fade, slide, scale, and spring) and support for staggered
sequences.

#### Scenario: Menu entrance
- **WHEN** a menu opens with a staggered slide-and-fade
- **THEN** each item SHALL animate with an incremental delay

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
The editor SHALL be built with this same UI system rather than a separate toolkit, so improvements
benefit both and the system is exercised by its most demanding consumer.

Editor-only widgets (docking, property grids, graph canvases) SHALL live in an editor UI module
layered on the runtime widgets.

#### Scenario: Editor exercises the UI system
- **WHEN** the editor is used
- **THEN** it SHALL exercise virtualised lists, docking layouts, and text editing, so defects
  surface during development rather than in games

### Requirement: UI diagnostics
The engine SHALL provide: a UI inspector showing the element tree with computed layout rects and
resolved style values, layout invalidation counts per frame, batch counts and break reasons, and
overdraw visualisation for UI.

#### Scenario: Layout thrash
- **WHEN** an element invalidates layout every frame
- **THEN** the diagnostic SHALL identify it and the property causing the invalidation

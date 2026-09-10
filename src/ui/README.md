# `src/ui/` — layer 4

**CyberUI**: the element store, the retained tree, layout, `.cyss`, input routing, the layer stack,
flattening, the frame budget and the accessibility audit.

**Governed by**: `ui-system`, which reaches **Working** at M8.b — tasks 9.1, 9.2 and 9.3.

**Behind `CY_UI`, which defaults ON.** `ui-system` requires "The system SHALL be removable at build
time via `CY_UI`", and until M8.b's closing gate that option gated nothing: there was no `if(CY_UI)`
and no `#if defined(CY_UI)` anywhere in the tree, and it defaulted OFF while this module was being
delivered — so `-D CY_UI=OFF` and the default build produced byte-identical output and neither had
ever been compared. It now removes this directory and its two suites, `samples/08-vertical-slice`
declines to declare itself without it, and `m8b:feature-options-off` builds that configuration.

## The three properties this module is built around

**UI elements are not entities, and the link graph is what says so.** `ui-system`: "UI elements SHALL
be stored in a dedicated, data-oriented UI store addressed by a lightweight `UIElementID`, **not** as
ECS entities", with the scenario "UI elements SHALL not appear in the ECS world at all". This module
does not link `cy::ecs` — not "does not create entities", *cannot name one* — so the requirement is
kept by the build rather than by a reviewer.

**Three dirty states with three different propagations.** Measure goes UP, and stops at the first
ancestor whose desired size does not depend on its children. Arrange goes DOWN through the affected
subtree. Paint goes NOWHERE. A paint dirty that propagated would relayout a document because a
label's number changed, and `tests/test_store.cpp` holds each of the three separately.

**Nothing dirty costs nothing.** `layout()` walks the store's dirty lists rather than the tree, and
`LayoutReport::measured` is zero on an idle frame. `flatten()` re-emits only what is paint-dirty and
counts the rest as reused. Both are numbers a test reads.

## What is here

| file | what it holds |
|---|---|
| `store.h` | the six parallel arrays, generational ids, the tree, and the three dirty states |
| `layout.h` | measure and arrange over Flex, Grid and Absolute; resolution independence |
| `style.h` | `.cyss` — the parser, the matcher, the cascade, and **the divergences from CSS** |
| `interaction.h` | hit testing, hover, capture, focus, directional navigation, the layer stack |
| `paint.h` | flattening to primitives, batching with break reasons, the budget ladder, accessibility |
| `widgets.h` | declarative descriptions and the reconciler, virtualisation, data binding |

## The `.cyss` divergences, in one place

`ui-system` requires them documented "rather than approximating CSS and leaving differences to be
discovered". `style.h`'s header carries the full list; the short version:

1. **The property set is closed.** A property outside the subset is an error with a line and a
   column, not a silently ignored declaration.
2. **Lengths are unitless or `px`, and both mean reference units.** `em`, `rem`, `%`, `vh` and `vw`
   are not in the subset.
3. **The cascade order is `ui-system`'s**: inline overrides, matched rules by specificity, inherited
   properties, the theme, engine defaults.
4. **No `!important`.** An inline override always wins.
5. **`var()` resolves at match time**, and an undefined reference is a diagnostic rather than a zero.
6. **No `@media`, no `@supports`, no keyframes in the sheet.**

## What `ui-system` asks for that this module does not yet have

Recorded rather than left for a reader to find:

* **The widget set is not built.** `widgets.h` has the reconciler, virtualisation and data binding —
  the machinery every widget is made of — and the twenty-six named widgets are not written. What is
  here is what they would be built from.
* **Animation and transitions** are declared in the style subset (`transition-duration`) and not
  evaluated: there is no keyframe player, no spring and no stagger.
* **The immediate-mode API** is absent.
* **World-space and surface-space UI**, the UI materials and effects, and the render-target capture
  the budget's blur-behind rung talks about are not implemented — this module produces a primitive
  stream and does not submit it.
* **The forcing functions** — the developer console, the debuggers, the profiler overlays, the
  settings interface and the conformance suite — are not built. `ui-system` is explicit that a UI
  system with no demanding first-party consumer decays, and this module has no consumer in the tree
  yet.

## Testing

`unit.ui` — 45 cases: the store's identity and dirty propagation, the reconciler's identity and
churn reporting, virtualisation at a hundred thousand rows, flex/grid/absolute layout, the scale
strategies, `.cyss` parsing and cascade, hit testing and capture, focus scoping and navigation, the
layer stack's semantics, flattening and batching, the budget ladder, data binding granularity, and
the accessibility audit with each of its four findings.

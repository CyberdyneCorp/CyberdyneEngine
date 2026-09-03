# Add the editor's visual language as a specification

## Why

`editor-ui-ux` says the editor should be dense rather than decorative, familiar rather than novel,
and dark or light by theme. That is a statement about *interaction*. It says almost nothing about
*appearance*, and appearance is where a tool's identity actually lives.

Three reference images now exist, and they are far more specific than the prose that preceded them.
They establish a viewport-first composition where the scene carries the screen's colour and the
chrome recedes to charcoal; a semantic colour vocabulary in which gold means selection and green
means live; gizmos sized for confident acquisition rather than CAD precision; an orientation widget
deliberately unlike a manipulator; overlaid viewport chrome instead of a second toolbar; and a
density band between consumer software and legacy engineering tools.

Left as images, that decays. Someone adds a coloured panel header for emphasis, someone else picks
green for selection because gold looked washed out on their monitor, a plugin ships an axis widget
with rotation rings, and eighteen months later the editor looks like four people's taste rather than
one product. Every one of those decisions is locally reasonable and collectively fatal, which is
precisely the class of decision this repository writes down.

There is also a tension in the existing specification worth resolving explicitly. `editor-ui-ux`
requires the editor to be *deliberately familiar* to users of Unity and Unreal — down to a
Unity-compatible keymap. The design direction requires it to be *immediately recognisable as
Cyberdyne rather than a derivative*. Both are right, and they are about different things: familiar
in **structure and interaction**, distinct in **identity and vocabulary**. Stated that way the
tension disappears; left unstated, it gets resolved differently by every contributor.

## What Changes

- **New `editor-visual-language` capability** — the appearance contract, alongside the interaction
  contract in `editor-ui-ux` and the behaviour contract in `editor-viewport-and-gizmos`.
- **The viewport is the subject.** A stated reading order for the screen — scene, selection, active
  controls, state, panels, metadata — and the rule that saturated colour is reserved for meaning.
- **Semantic colour, and axis colour as one language.** Gold is selection, green is live, orange
  warns, red errors. X red, Y green, Z blue holds in gizmos, in vector fields, in the orientation
  widget, in debug visualisation, and is not user-remappable. Subordinate throughout to the existing
  accessibility rule: colour is never the sole encoding.
- **Gizmo legibility as a contract**: arrows translate, arcs rotate, boxes scale — distinguishable
  by shape and not only by colour, with the universal mode required to stay readable or degrade.
- **The orientation widget is not a manipulator**: three axes, no rings, no boxes.
- **Chrome is overlay, not toolbar.** Viewport controls cost no viewport height.
- **Typography, iconography, surfaces and density** stated concretely — tabular figures, subtle
  headings, one icon system that borrows no other engine's recognisable symbols, luminance and
  spacing instead of borders and cards.
- **Spatial stability under context.** Panel *content* adapts to selection; panel *position* never
  does. Contextual behaviour that relocates panels destroys the muscle memory that makes a
  professional tool fast.
- **Progressive disclosure as the default state**, with the forcing function that matters: adding
  capability to the engine does not automatically justify adding permanently visible interface. A
  change introducing new properties must say where they sit in the disclosure hierarchy.
- **Engine vocabulary.** Node not Actor, graph not Blueprint, world not Level — with familiar terms
  kept as search aliases so a user who types what they know still finds the feature.
- **Reference imagery is normative and committed**, with the explicit rule that a reference states
  visual language rather than feature completeness — so a mockup showing volumetric clouds does not
  become an implicit dependency on M10.
- **Forbidden visual patterns**, each checkable.

## Capabilities

### New Capabilities

- `editor-visual-language` — visual hierarchy, identity, surfaces, semantic and axis colour,
  selection, gizmo legibility, the orientation widget, viewport chrome, the performance overlay,
  typography, iconography, default composition, spatial stability, disclosure, ambient status,
  thumbnails, graph surfaces, vocabulary, normative references, and forbidden patterns.

### Modified Capabilities

- `editor-ui-ux` — "Familiarity is a feature" resolves the tension explicitly: familiar in structure
  and interaction, distinct in identity and vocabulary.
- `editor-viewport-and-gizmos` — "Overlays and in-viewport interfaces" adds that viewport controls
  are overlays rather than a second toolbar, and that the orientation widget is an overlay and not a
  manipulator.

## Impact

- **Documentation**: adds `docs/design/` with the illustrated reference, the three committed
  reference images, the colour and vocabulary tables, and an explicit note on what the mockups get
  wrong.
- **Roadmap**: no re-sequencing. `editor-visual-language` reaches Working at **M5** alongside the
  rest of the editor and Complete at **M11**; the vocabulary, colour and disclosure rules apply from
  the first panel drawn.
- **Every future editor change** acquires two obligations: state where new properties sit in the
  disclosure hierarchy, and use the engine's vocabulary.
- **Risk**: the reference images depict content the renderer cannot produce until M7–M10. The
  requirement that a reference states visual language rather than feature completeness is what stops
  that becoming an accidental dependency — it is the single most likely misreading of this change.

# Tasks: editor visual language

Specification-stage change. Sections 1 to 3 are the work of this change and are complete.

Section 4 records the implementation the decision implies. It is **deferred to M5**, where the
editor is built — none of it can be done before there is an editor to look at. It is listed so the
scope is not lost.

## 1. Specification

- [x] 1.1 Record in `design.md`: why this is a specification rather than a style guide, the boundary
      between the three editor capabilities, the familiarity-versus-identity tension and its
      resolution, the vocabulary argument, what the references get right and wrong, what a reference
      image is and is not, where it lands on the roadmap, and the rejected alternatives
- [x] 1.2 New `editor-visual-language` capability: viewport-first hierarchy, Cyberdyne identity,
      surface system, semantic colour, axis colour as one language, selection appearance,
      multi-selection, gizmo legibility, the orientation widget, viewport chrome as overlay, the
      ambient performance overlay, typography, iconography, default composition, spatial stability,
      progressive disclosure, ambient status, asset thumbnails, graph surfaces, engine vocabulary,
      normative references, and forbidden visual patterns
- [x] 1.3 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `editor-ui-ux` — "Familiarity is a feature" states the split explicitly: familiar in
      structure and interaction, distinct in identity and vocabulary
- [x] 2.2 `editor-viewport-and-gizmos` — "Overlays and in-viewport interfaces" adds viewport
      controls as overlays rather than a second toolbar, and the orientation widget as an overlay
      that is not a manipulator
- [x] 2.3 `editor-architecture` — reviewed; no change needed. It owns the editor's structure as an
      application, not its appearance
- [x] 2.4 `editor-rust-application` — reviewed; no change needed. "Interface toolkit is an
      implementation detail" is why this capability constrains meaning and relationship rather than
      naming tokens or widgets
- [x] 2.5 `visual-scripting` — reviewed; no change needed. The graph surface requirement here
      constrains appearance; the shared graph infrastructure remains that capability's
- [x] 2.6 `ui-system` — reviewed; no change needed. `ui-system` is explicitly not the editor's
      toolkit, so the runtime UI system is not bound by the editor's visual language
- [x] 2.7 `delivery-roadmap` — reviewed; no re-sequencing. The capability reaches Working at M5 and
      Complete at M11, alongside the rest of the editor

## 3. Documentation

- [x] 3.1 `docs/design/images/` — commit the three reference images
- [x] 3.2 `docs/design/editor-visual-language.md` — the illustrated reference: the images with
      annotated readings, the ten principles, the colour and vocabulary tables, the layout map, the
      gizmo language, and an explicit account of what the mockups get wrong
- [x] 3.3 `docs/README.md` — index the design reference
- [ ] 3.4 `docs/roadmap/capability-matrix.md` and `docs/roadmap/status.yaml` — add
      `editor-visual-language` (M5 Working, M11 Complete). **Deferred until the M0 workflow
      completes**: adding a capability while `roadmap-status` is being built and run would trip its
      own drift check mid-flight

## 4. Implementation (deferred to M5)

- [ ] 4.1 Derive the concrete palette satisfying the semantic colour requirement, in both themes and
      in a colour-blind-safe variant; record the tokens and the contrast measurements
- [ ] 4.2 The Cyberdyne icon set: geometric, monochromatic, one stroke weight, legible at compact
      density, with an audit against other engines' recognisable symbols
- [ ] 4.3 Type scale and tabular figures; verify numeric column alignment in the generated inspector
- [ ] 4.4 Gizmo geometry for translate, rotate, scale and universal, with the readability degradation
      rule implemented and tested at small screen sizes
- [ ] 4.5 The view-orientation widget, visually distinct from the transform gizmo
- [ ] 4.6 Selection outline: legible on bright and dark content, no bloom, material still judgeable;
      editor selection visually distinct from gameplay selection
- [ ] 4.7 Viewport chrome as overlays; confirm no viewport height is consumed
- [ ] 4.8 The ambient performance overlay, movable and disableable, excluded from clean captures
- [ ] 4.9 Default workspace composition as the shipped default layout
- [ ] 4.10 Vocabulary audit of every interface string, with search aliases for the familiar terms
- [ ] 4.11 A review checklist derived from the forbidden-patterns requirement
- [ ] 4.12 Re-shoot the reference imagery from the real editor once it renders, and replace the
      concept art — a reference that no longer reflects the product is replaced, not left to decay

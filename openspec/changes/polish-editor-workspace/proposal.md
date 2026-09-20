# Proposal

## Why

The editor already has a coherent visual language, but its shell does not yet carry the clarity of
`docs/design/images/editor-rts-desertfrontier.png`. Boxed resting controls, indistinguishable
transform modes, and implementation counters dilute the scene-first composition.

## What Changes

- Flatten resting application menus and toolbar controls; preserve clear hover and keyboard focus.
- Show the current transform mode with a restrained blue active treatment and a non-colour cue.
- Give selected dock tabs a thin underline and hierarchy selection a clearer gold treatment.
- Replace hierarchy rebuild and thumbnail-request counters with useful content summaries; retain
  instrumentation in diagnostic surfaces or tooltips.
- Refine search fields and empty-state typography using the existing density and semantic tokens.
- Capture the actual editor before and after at matching dimensions for visual review.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `editor-visual-language`: specify persistent active-tool and selected-tab feedback, and task-oriented
  panel summaries.

## Impact

Primarily `editor/crates/cy-editor-shell`, with presentation data derived from existing services and
view models. No dependency, document-format, engine-renderer, or command-protocol changes are planned.
The existing workspace arrangement, themes, density modes, and keyboard interaction remain supported.

Scope approved by the user on 2026-09-19. The UI pass is separate from the Linux roadmap work; native Metal and macOS viewport transport are the next owned workstream.

# Design

## Context

The strategy reference fixes the default arrangement. The adventure and scene references reinforce
compact typography, quiet surfaces, and readable selection. The existing `theme.rs` adapter already
centralizes colour and spacing; build on it rather than introducing another palette.

Source inspection found that `chrome::toolbar` creates identical buttons for every transform mode,
`hierarchy::show` displays rebuild counts, and `browser::show` displays preview-request counts.
The saved `editor-window-m5b.png` capture is historical evidence, not a capture of the current build.
A fresh baseline is required before evaluating the result.

## Goals

- Make active tools and panel selection readable at a glance.
- Bring resting chrome closer to the references while keeping pointer and keyboard affordances.
- Make default panel text describe the user's work.
- Keep all styling in existing semantic and density abstractions.

## Non-goals

No new renderer, thumbnail transport, project asset index, inspector schema, graph editor, decorative
animation, or workspace rearrangement. Real asset previews are valuable follow-up work: the current
browser displays text even for a render handle, so cosmetic changes cannot deliver that capability.

## Decisions

1. Use scoped menu/toolbar styling, so making the header flat does not erase inspector input affordances.
2. Derive active transform feedback from the same state as the viewport. Do not retain a toolbar-local
   selection that can drift after shortcuts or commands. Use blue plus an underline/pressed cue.
3. Use gold for selected tabs and entities, preserving error markers and keyboard-focus treatment.
   Verify the dock library's actual painting path rather than assuming its outline field is an underline.
4. Show visible entity count and selected count in the hierarchy, explicitly distinguishing filtered
   results if necessary. Show item counts in the browser. Put implementation counters in diagnostic
   tooltips or the existing profiler rather than removing instrumentation.
5. Keep search and empty states small: a primary message and a quieter actionable next step. Derive
   shortcut labels from bindings where shown; avoid adding controls with no registered action.

## Verification

Capture a fresh running baseline and the finished shell at the same size and content. Inspect dark
and light themes, compact and comfortable density, a narrow window, search with no results, keyboard
focus, and transform switching by both toolbar and shortcuts. Include regression tests with each bug
fix and run relevant shell/interface tests, formatting, and lint checks. Record environmental blockers
explicitly rather than presenting concept imagery as a working editor screenshot.

The cognitive-complexity skill currently has no Rust analyzer; do not claim a numeric measurement
for this Rust-only pass. Keep presentation helpers small and review branching manually.

## Scope review

The user approved overall shell polish and separately authorized native Metal and macOS viewport
transport next. The unchanged main branch failed to build on macOS because the non-Linux transport
adapter lacked `texture()`. This pass adds its explicit no-image response and regression coverage.

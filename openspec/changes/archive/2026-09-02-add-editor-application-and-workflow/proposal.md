# The editor application and the developer workflow

## Why

Three capabilities already describe what the editor *does* — documents and transactions, live
editing, and the editor's panels and tooling. None describe what the editor **is**: what language it
is written in, what process it runs in, how its presentation layer is organised, where the boundary
with the engine sits, or how a developer builds and runs any of it.

Left unspecified, those answers get decided by whoever writes the first panel, and they are close to
irreversible. This change decides them.

**It also reverses an earlier decision, and that is the most important thing in this proposal.**
`ui-system` currently requires that "the editor SHALL be built with this UI system rather than a
separate toolkit, so improvements benefit both and the system is exercised by its most demanding
consumer", and its purpose names the editor as "the reason virtualisation and docking are core rather
than optional". `editor-architecture` requires that the editor "be built on the engine runtime, using
the same ECS world, scene graph, UI system, and renderer, compiled only when `CY_EDITOR` is enabled".

A Rust editor in its own process contradicts both. That trade is worth making, and the cost is real
and is not hidden here: CyberUI loses its most demanding consumer, and a replacement forcing function
is specified rather than assumed.

What is gained is substantial: **crash isolation** — an engine fault no longer takes the editor and
unsaved work with it; a **language boundary that enforces the SDK boundary**, because Rust physically
cannot reach a C++ implementation type; and **remote and console parity by construction**, since an
editor that already talks to a runtime over a protocol treats a console the same as a local process.

The contracts:

> **CyberEditor is a Rust desktop application and a client of the engine through Cyberdyne-owned
> stable boundaries.** It never depends on C++ implementation types, standard-library containers, or
> direct ownership of runtime objects.

> **The editor owns viewport interaction and presentation; the engine owns all world-space
> rendering.** There is one renderer, and the Scene view is the game's renderer.

> **`just` is the canonical human-facing workflow.** It orchestrates dependency-aware tools; it does
> not replace them, and the editor, the command line, and continuous integration drive the same
> build.

## What changes

**`editor-rust-application`** — the Rust workspace and its dependency direction; the rules that keep
unsafe code confined to narrow interop modules and forbid raw pointers as identity; three engine
hosting modes with **hosted (separate process) as the production default**, so a runtime crash leaves
the editor and its journal intact; and the presentation architecture: **MVVM plus services plus
commands**, with models owning authoritative state, view models owning presentation state only, views
containing no domain logic, and — the rule that matters most in a fifteen-panel application —
**no view model may depend on another panel's view model**. Cross-panel coordination goes through
shared services, selection, documents, and commands.

**`editor-ui-ux`** — a dark, high-density professional shell using **semantic design tokens** rather
than fixed colours; the familiar default layout and panel set; docking, workspaces, and multi-monitor;
a **command registry** through which every user-invokable action passes, so menus, toolbars, hotkeys,
the command palette, automation, and future assistants share one model; a **Unity-compatible default
keymap** with selectable profiles, because familiarity beats novelty for a tool people use for eight
hours a day; the schema-driven inspector with property states, multi-editing, and **provenance** —
*why does this value have this value*; structured console, notifications, and error presentation;
accessibility that does not rely on colour alone; and interaction-rate and virtualisation
requirements.

**`editor-viewport-and-gizmos`** — the rendering responsibility split, stated as a requirement:
**the editor never implements a renderer**, never issues graphics commands, and never reimplements
gizmo geometry in the interface toolkit. The editor decides *what should be shown*; the renderer
decides *how it is drawn*. Viewports are engine render views hosted by the editor, over a **transport
abstraction** — local surface, shared texture, or encoded stream — so that a console viewport is the
same panel. Also: consistent tools, transform gizmos, snapping, picking that returns stable identity
rather than pointers, debug visualisation as typed renderer modes, and a shared **debug draw**
producer interface.

**`developer-workflow-and-just`** — recipes as the documented entry point for setup, build, run,
test, benchmark, lint, format, cook, package, specification validation, and diagnosis; **`just
doctor`** as a required first-class recipe; consistent build profiles across four toolchains, so a
release C++ build never pairs with a debug Rust one by accident; benchmarks producing machine-readable
results and a comparison workflow; continuous integration invoking the same recipes rather than
duplicating build logic in workflow files; and the rule that recipes orchestrate rather than
reimplement dependency tracking.

## Impact

- **New**: `editor-rust-application`, `editor-ui-ux`, `editor-viewport-and-gizmos`,
  `developer-workflow-and-just`
- **Reversed**: `ui-system`'s requirement that the editor be built on CyberUI is **removed with a
  supersession note**, and replaced by an explicit alternative forcing function — engine-shipped
  in-game tooling and a conformance suite — which is honestly weaker than dogfooding an editor and is
  recorded as such
- **Modified**: `editor-architecture` (the editor is a Rust client, not an engine application),
  `live-editing` (hosting modes), `native-abi` (the Rust SDK layer),
  `project-and-plugins` (the editor SDK boundary and the prohibition on exposing Rust's ABI),
  `build-system-and-platforms` (Rust toolchain integration), `thirdparty-dependencies`

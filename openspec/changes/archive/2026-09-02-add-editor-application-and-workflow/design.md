# Design: the editor application and the developer workflow

## 1. The reversal, stated plainly

Two existing requirements say the editor is a C++ application built on the engine's own UI system:

- `editor-architecture`: "The editor SHALL be built on the engine runtime, using the same ECS world,
  scene graph, UI system, and renderer, compiled only when `CY_EDITOR` is enabled."
- `ui-system`: "The editor SHALL be built with this UI system rather than a separate toolkit, so
  improvements benefit both and the system is exercised by its most demanding consumer."

A Rust editor in its own process contradicts both, and both were good arguments. What the change buys:

- **Crash isolation.** A device loss, a null dereference in a system, or a plugin fault currently
  takes the editor and its unsaved journal with it. With a hosted runtime, the editor survives, shows
  the crash artefact, and offers to restart or open the reproduction.
- **A boundary the compiler enforces.** The rule "the editor talks to the engine only through stable
  interfaces" is a convention in one language and a physical fact across two. Rust cannot
  accidentally include a C++ header.
- **Remote and console parity by construction.** An editor that already speaks to a runtime over a
  protocol treats a console like a local process. The alternative retrofits that later, badly.

What it costs, and this is the honest part: **CyberUI loses its most demanding consumer.** The
requirement being removed existed because an editor used daily is the best possible test of
virtualisation, docking, property grids, and text editing. Nothing replaces that entirely.

So a **replacement forcing function** is specified rather than assumed: the engine ships in-game
tooling built on CyberUI — a developer console, gameplay and network debuggers, profiler overlays, a
settings interface — plus a conformance suite covering the same demanding cases, plus a sample project
that is a real interface rather than a demo. That is weaker than dogfooding an editor, and saying so
is better than pretending otherwise.

## 2. Three hosting modes, and which is the default

| Mode | Engine | Used for |
|---|---|---|
| **No runtime** | None | Project browsing, source assets, build configuration, source control, specification tools |
| **Embedded** | In the editor process | Schema and reflection, asset metadata, small previews |
| **Hosted** | Separate process or device | Rendering, play mode, physics, streaming, profiling — the production default |

**Hosted is the default**, and the requirement that keeps it honest is that every feature must remain
capable of operating against a hosted runtime unless it has a documented reason not to. Without that
rule, in-process convenience accumulates until the remote case quietly stops working — which is
exactly how console debugging becomes a separate, worse experience in other engines.

## 3. MVVM, plus services, plus commands

Textbook MVVM applied literally to a fifteen-panel editor produces a view model per widget and a great
deal of ceremony. What the editor actually needs is the *separation*, at a larger grain:

- **Models and services** own authoritative state: documents, project, workspace, runtime sessions,
  selection, transactions, assets, builds.
- **View models** own presentation state: expansion, filtering, sort order, scroll position, formatted
  values, in-progress edits, and the intents a panel can raise.
- **Views** render a view model and emit intents. No domain logic.

Two rules do most of the work:

**A view model is never a second source of truth.** It may cache a formatted string; it may not hold
the value the document holds. When that rule slips, two panels disagree and nobody can say which is
right.

**No view model depends on another panel's view model.** Selecting an entity must update the
hierarchy, the inspector, the viewport, the properties, and the status bar — and if those five talk
to each other, the editor acquires twenty edges and no order of initialisation that works. They all
observe `SelectionService`. This is the single most valuable constraint in the whole presentation
architecture, and it is stated as a prohibition because it is violated by accident, always for a
locally reasonable reason.

Commands complete it: every user-invokable action is an entry in a registry, so the same action serves
a menu, a toolbar, a hotkey, the command palette, a script, and a future assistant. A feature reachable
only by clicking a specific button is a feature no automation can use.

## 4. Rendering: the editor never has a renderer

The failure this rules out is a second renderer. If the editor draws the Scene view with its interface
toolkit, the editor and the game diverge visually, the editor cannot show virtual geometry clusters or
shadow pages, and every renderer feature needs an editor implementation too.

So the split is by question rather than by feature:

> The editor decides **what should be shown**. The renderer decides **how it is drawn**.

The editor owns tools, selection state, gizmo *semantics*, snapping policy, and overlay *intent*. The
engine owns the render view, gizmo geometry, selection outlines, the grid, debug visualisation, and
every pixel of world-space content — because all of those need correct depth, occlusion, world
transforms, and the viewport camera, none of which an interface toolkit drawing over a texture has.

Picking follows the same rule: the editor asks a viewport service to pick at a coordinate and receives
**stable editor identity**, never a pointer into runtime memory.

The natural boundary is that **two-dimensional editor content is interface rendering — graphs,
timelines, property grids — and world-space content is engine rendering.** A graph canvas is interface;
a material preview sphere is the engine.

## 5. Viewport transport, so the console case is not special

A hosted runtime has to get pixels into the editor's window, and how it does that differs by platform
and by whether the runtime is on this machine at all.

That is a **transport abstraction** — local surface, shared texture, or encoded stream — behind one
viewport interface. The panel does not know which is in use. Without that abstraction the remote case
becomes a different panel with different behaviour, which is how remote debugging ends up as a poor
relation of local.

## 6. Familiarity is a feature

The default keymap matches Unity's where the semantics match. This is not a lack of ambition: a person
who has spent years building muscle memory should not spend their first week fighting a tool, and
there is no user benefit in a different key for "frame selection".

Every binding lives in the command registry with selectable profiles, so a project or a person can
change all of it — and a widget with a hard-coded key is a defect, because it cannot be rebound,
listed, or invoked from the palette.

The same reasoning applies to layout and panel names: Hierarchy, Scene, Game, Inspector, Project,
Console. Internally they may be an outliner and a property editor; the label a new user reads should
be the one they already know.

## 7. Density over decoration

A tool used for eight hours a day is not a landing page. Large rounded cards, generous padding, and
decorative gradients cost information density, and density is what a professional tool trades in.

So: a dark, low-noise shell around a bright viewport, semantic tokens rather than fixed colours,
compact spacing with a comfortable option, tabular numerals in inspectors and profilers, and status
never conveyed by colour alone — which is an accessibility requirement and also simply more legible.

## 8. `just` orchestrates; it does not build

The temptation with a task runner is to let it accumulate logic until it becomes a build system with
no dependency tracking. The rule that prevents it: **recipes invoke dependency-aware tools and never
implement dependency checking themselves.**

The layering is explicit — `just` is the human surface, `cybuild` is engine-aware orchestration, and
CMake, Ninja, Cargo, the shader compiler, and the cooker do the work. Before `cybuild` exists, recipes
call the tools directly and become thin wrappers later without the developer-facing commands changing.

Two requirements are worth more than the recipe list. **`just doctor`** turns "it doesn't build on my
machine" into a diagnosis with a remedy, which is the single highest-value hour in any repository's
tooling. And **profiles are consistent across four toolchains**, so a release C++ build never
accidentally pairs with a debug Rust one — a class of confusion that costs days and is invisible while
it happens.

Continuous integration invokes the same recipes. The workflow file chooses machines and caches; it
does not define what a build is. Otherwise the two drift, and the failure that only happens in
continuous integration is unreproducible locally.

## 9. Build order

| Phase | Contents |
|---|---|
| 1 | Rust workspace, crate layout, `just` recipes, `doctor` |
| 2 | Editor SDK over the C ABI; protocol client; runtime host process |
| 3 | Shell: windows, docking, panels, workspaces, layout persistence |
| 4 | Command registry, keymap profiles, command palette |
| 5 | Models and services: documents, selection, transactions, assets |
| 6 | View model layer and the panels that follow from it |
| 7 | Viewport hosting, transport, tools, gizmos, picking |
| 8 | Inspector with reflection, property states, provenance |
| 9 | Console, notifications, build panel, profiler views |
| 10 | Domain editors on shared graph and timeline infrastructure |
| 11 | Plugin surface; source control; accessibility |
| 12 | Remote and console viewports; benchmark comparison workflow |

**Phase 2 is the milestone that matters.** Once the editor talks to a hosted runtime over the SDK and
the protocol, every later feature is built the right way by default. Building panels first against an
in-process engine produces an editor that must be re-architected to reach a console.

## 10. Non-goals

- **A second renderer.** Under any circumstances.
- **Committing to a specific Rust interface toolkit in the specification.** The choice is an
  implementation decision behind editor abstractions, so it can be measured and replaced.
- **Rust's native ABI as a plugin boundary.** It is not stable enough; editor plugins cross the
  engine's C ABI or a process protocol.
- **`just` as a build system.** It runs recipes; the tools track dependencies.

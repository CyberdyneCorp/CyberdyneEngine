# `editor/` — CyberEditor, a Rust client of the engine

**Layer 7 of the repository, and layer 0–5 of its own.** A native Rust desktop application that is a
*client* of the engine rather than a part of it, reaching it only through the stable C ABI and the
live bridge protocol. Cargo owns its compilation; the engine build does not reimplement it.

**Governed by**: `editor-rust-application`, `editor-documents-and-transactions`,
`editor-architecture`, `editor-ui-ux`, `editor-viewport-and-gizmos`, `editor-agent-interface`.
Arrives at M5.

---

## The three decisions everything else follows from

**The editor reaches the engine only through the SDK, over the C ABI.** Never by linking an engine
target. That is what makes a runtime crash cost a restart rather than a session, what makes editing
a remote or embedded target the same code path as editing locally, and what makes the boundary
enforced by the language instead of by discipline. `crates/cy-editor-app/tests/safety.rs` checks it:
every crate but three carries `#![forbid(unsafe_code)]`, and nothing outside the SDK names a C type.
The three are the SDK, the test fixture that implements the C ABI, and `cy-editor-viewport-transport`
— the platform module that imports the runtime's rendered image, which is dma-buf, `memfd` and
Vulkan and has no safe standard-library path. That file also asserts that the list is exactly three
long, so widening it is a decision rather than an edit.

**Transactions are the only path for persistent mutation.** `DocumentContent` has no public mutating
method that does not take a `WriteToken`, and a `WriteToken` has a private field — so no code outside
`cy-editor-documents` can construct one, and there is no direct write path to write around. Undo,
autosave, crash recovery, semantic diff, merge and live editing are one mechanism read six ways, and
that is only true if nothing writes around it.

**Commands are the single action surface, with metadata sufficient for machine invocation.** The
registry *refuses* a command whose description, typed parameters or effect class would not satisfy a
caller that has never seen the interface. That binds from the first command registered, because
retrofitting it across an established registry is an entry-by-entry migration.

## The crates

Dependencies flow downward, and `crates/cy-editor-app/tests/layering.rs` fails a manifest that
depends upward or sideways. Each crate declares its layer in `[package.metadata.cy]`.

| Layer | Crate | Owns |
|---|---|---|
| 0 | `cy-editor-core` | Stable identity, the value vocabulary, structured problems, revisions, progress, actors, the byte codec |
| 1 | `cy-editor-sdk` | The generated C ABI bindings and the safe layer over them. **The only crate that may say `unsafe`** |
| 1 | `cy-editor-protocol` | The live bridge: framing, messages, and a session that never blocks a UI frame |
| 1 | `cy-editor-documents` | Documents, transactions, history, the journal, semantic diff and merge, the three kinds of world |
| 2 | `cy-editor-commands` | The command registry, its machine-readable metadata, and connection scopes |
| 2 | `cy-editor-testhost` | A test fixture that implements `cy_get_interface` in Rust, plus `cy-runtime-stub` |
| 3 | `cy-editor-services` | Authoritative state: documents, selection, workspace, notifications, operations, runtime sessions |
| 3 | `cy-editor-viewport-transport` | The engine's image across the process boundary: dma-buf/Vulkan on Linux, IOSurface/Metal on macOS, and the bounded ownership ring. **The only crate besides the render crate that may name a graphics API** |
| 1 | `cy-editor-visual` | The visual language as data: semantic colour, the axis triad, density, gizmo and chrome rules, vocabulary |
| 2 | `cy-editor-viewport` | The viewport model: camera, picking, gizmo geometry, snapping, view modes — headless |
| 3 | `cy-editor-reflection` | One description of a type, whatever described it, and the input the generated inspector reads |
| 4 | `cy-editor-interface` | Docking, workspaces, the palette, the keymap, the generated inspector, problems, notifications — as models, with no toolkit |
| 4 | `cy-editor-viewmodels` | Presentation state derived from services. Never a second source of truth |
| 4 | `cy-editor-agent` | The projection of the command registry, the read surface, the session and its budget. No transport |
| 5 | `cy-editor-shell` | **The window.** The one crate that may see an interface toolkit: egui 0.36.1 + egui_dock 0.21.1 over wgpu 30.0.1 |
| 5 | `cy-editor-mcp` | **The wire.** The Model Context Protocol over JSON-RPC on stdio, and the only crate that knows what JSON is. Optional at build time |
| 6 | `cy-editor-app` | The `cyberdyne-editor` binary, and the workspace's own structural checks |

`cy-editor-app` moved from layer 5 to 6 at M5.5, when the render crate arrived: the binary hands a
built `Editor` to the window, so it has to sit above it.

The SDK's embedded-runtime loader uses `dlopen`/`dlsym` on Linux and macOS and
`LoadLibraryW`/`GetProcAddress` on Windows. The same generated ABI fixture and inspector integration
tests run on all three platforms; Windows paths are passed to the loader as UTF-16 rather than being
re-encoded through a narrow string.

**The agent interface is a projection and a wire, and they are separate crates on purpose.**
`cy-editor-agent` holds what an agent can see and do — the tools, which are the registry; the read
surface, which is the services; the session, its scope, its budget and its claims. It names no
protocol and has no socket. `cy-editor-mcp` holds the protocol and nothing else, behind
`cy_editor_agent::AgentTransport`, which is what `editor-agent-interface` means by "no MCP type SHALL
appear in the editor's command, document, or view-model layers": the dependency direction makes it
impossible rather than discouraged, and `crates/cy-editor-app/tests/gating.rs` checks that the
transport stays optional, stays on by default, and stays named by nothing but the binary.

`--mcp` now hosts that transport alongside the desktop window; `--mcp --headless` retains the
stdio-only mode for automation. The Agent Sessions panel shows the connected identity, intent,
scope, budget, current operation, pause/revoke controls, pending irreversible or external effects,
and privacy-labelled activity. Reversible edits run without a prompt. A destructive/external
request waits for Allow once, a five-minute grant, or Refuse; revocation drops queued work and rolls
back any uncommitted transaction attributed to that session.
Use `--agent-scope operator` only when the desktop human should be able to confirm irreversible or
external work; `read` remains the default and `author` remains limited to undoable edits and
`game/` source.

For a hosted material scene, an MCP client can call `material.graph.read` with a project-relative
`.cygraph` reference, edit the returned `source` (`cymatcanvas 1` text), and call
`material.graph.preview` with that reference and source. Poll `material.graph.status`, then read
`viewport:` to receive a PNG copied from the engine frame shown by the desktop viewport. Call
`material.graph.save` to ask the engine to author the graph; poll status until it says `saved` or
`failed`. A successful save writes both the canonical `.cygraph` and editable `.cymatcanvas` and
records their prior contents in the active scene's undo history.
`play.enter` and `play.leave` control the hosted simulation; `play:` reports `state`, `mode`, and
connection status. These commands work over `--mcp` with the desktop open, without computer-use
automation. The default `viewport:` read captures the current editor camera and excludes UI
overlays. Restated camera/projection and independent debug-view requests still need a pumped
agent viewport and may report that no frame has arrived; use the registered viewport view-mode
commands to change the focused renderer view before reading `viewport:`.

`editor:window` returns what the person at the desktop sees: a PNG of the editor window as it was
presented, including panels, an open palette, and the engine image inside the viewport. It works
the same on Linux and macOS, because egui renders every frame to a capture texture before
presenting it. `editor:window?panel=<kind>` crops to one dock panel (`viewport`, `hierarchy`,
`inspector`, …) using the rectangle the dock drew in that frame. The read arrives one or more frames
after the request and costs one render from the connection's budget. The reply's text entry states
the window size and the rectangle, and says that the image is the editor's composition, not the
shipping frame. A headless session, an unknown or hidden panel, and a window that presents nothing
for 2 s are refused with a reason. No input is sent, so the machine stays usable while an agent
looks. On Linux, `viewport:` still carries no bytes, because the engine frame arrives as a dma-buf.
`editor:window?panel=viewport` shows the same frame as the editor displayed it.

The hosted MCP verification captured the [copper cube](../docs/design/images/editor-mcp-material-before-metal.png)
and the [green preview](../docs/design/images/editor-mcp-material-preview-metal.png) from the
same engine scene. The plane, shadow, and camera framing stayed fixed across the two reads.

The domain editors named by `editor-rust-application` arrive later. Their layer positions are
already decided by the rule above.

**The interface toolkit is contained by a test, now that there is one.**
`crates/cy-editor-app/tests/containment.rs` fails if more than one crate names egui, eframe,
egui_dock or winit, if any crate but that one and the viewport transport names a graphics API, and
if anything at layer 2 or below names either. Without it `egui::Color32` reaches `cy-editor-visual`
inside a milestone, because importing it is locally reasonable every single time.

## The workflow

```
just build-editor                  build (profile-mapped, into build/editor/)
just build-editor --generate       regenerate the SDK bindings from the C ABI first
just build-editor-check            the selftest, rustfmt, clippy and the tests
just build-editor-format           format the hand-written Rust in place
just run-editor --project <directory> --open <asset>
just run-editor --project <directory> --open <asset> --script <path>
just run-editor-runtime [<socket>] the hosted-runtime stub `--host` connects to
just content-new-project <directory>           a project from the `empty` template
just run-engine --project <directory>          the engine host, until interrupted
just run-editor-live --project <directory>     the engine and an editor attached to it
```

`cyberdyne-editor --new-project <directory> [--template empty|swift-gameplay]` is what
`content-new-project` runs. Every template writes the engine's `types.cytypes`, so an entity created
in the new world has a `Transform`. A world file that declares no types, which is what a template
writes, is opened with that manifest, the same as a world that does not exist yet.

Command-line scripts wait for an attached runtime to confirm each `play.enter`, `play.pause`, and
`play.leave` request before continuing. Their summaries include the runtime's authoritative state
and detail, so a script cannot exit after merely queueing a play request that the engine never saw.

`--project` is resolved before workspace restoration, service discovery, and document opening. The
directory must contain `project.json`; the editor refuses an arbitrary directory instead of making
it writable project content. Without the option, the working directory remains the implicit root.

Playing and Paused are engine states. If no runtime is attached, requesting either leaves the
viewport in Editing and posts a remedy instead of showing a state no engine accepted. Losing a
runtime likewise returns every viewport to Editing while the documents remain open. A hosted
runtime connection remembers its local endpoint and retries once per second after a loss. When the
runtime returns, the existing editor process reconnects and replays the open document's unsaved
transaction history before incremental live editing resumes.

The Hierarchy Create menu and Scene menu can add a Plane, directional light, point light, spot
light, or Camera to the open world. Camera creation from the desktop uses the current editor view
position and rotation. The same commands are available to scripts and MCP clients as
`scene.create-primitive shape=plane`, `scene.create-light kind=directional|point|spot`, and
`scene.create-camera`. Created actors use normal scene transactions, can be edited in the Inspector,
and survive Save and reopen.

The viewport has Editor and Game controls. Editor uses its navigation camera and shows transform,
light, and Camera handles. Click a light or Camera icon to select its actor and edit its Transform
and component fields in the Inspector. Directional and spot lights and Cameras show their forward
direction; point lights show their position because they emit in every direction. Game uses the
first enabled scene Camera and hides editor handles; if no scene
Camera exists, the view explains that one is needed. Switching views alone does not start the
simulation. Play selects Game and advances hosted physics; Pause holds it; Stop restores the
authored world and Editor view. The hosted runtime loads a built project Swift module when a node
has a `ScriptBehaviour` component with a text `class` field naming a registered `@Behaviour`.
It calls `onFixedUpdate` after physics during Play, pauses it with the simulation, and restores
authored transforms on Stop. Run `project.build` and wait for completion before Play; a missing
module or unknown behaviour is reported as a Play refusal. Play also sounds the world: the runtime
applies the project's mixer, starts every autoplaying `cy::audio::AudioSource`, and binds its audio
server to Swift's `Audio`; the Play detail names the sources and the backend (see "The audio
tools" below). The Editor's studio fill is omitted from
Game rendering, so authored lights determine its illumination.
Disabling the last authored light removes its illumination in both views; its Editor handle remains
selectable so it can be enabled again. Rotating a directional or spot light changes where it shines.
Point lights emit in every direction, so moving one changes the image but rotating one does not.
An enabled directional light with `casts_shadow` draws a shadow from MeshRenderers whose
`casts_shadow` is enabled onto MeshRenderers whose `receives_shadow` is enabled. The shadow follows
light rotation and mesh transforms in both views. The Editor adds a modest fill so unlit sides stay
visible while the shadow remains readable; Game uses the authored lights. The hosted Metal capture
uses an sRGB target, matching the frame's tone mapper.

For the imported tree example, the Plane is centered at `(0.5, -1.17, 0)` and scaled to
`(8, 1, 8)`, below the tree's lowest root. A directional light points downward across it, and a
point light near the viewing side reveals the dark bark texture. The live captures show the
[tree shadow](../docs/design/images/editor-tree-plane-shadow-metal.png), the
[light disabled](../docs/design/images/editor-tree-plane-light-off-metal.png), and the
[light rotated](../docs/design/images/editor-tree-plane-light-rotated-metal.png).

Live Metal captures: [Editor view with selected light](../docs/design/images/editor-scene-light-editor-metal.png)
and [Game view during Play](../docs/design/images/editor-scene-light-game-play-metal.png).
The [selected Camera](../docs/design/images/editor-scene-camera-gizmo-metal.png) and
[selected directional light](../docs/design/images/editor-scene-directional-gizmo-metal.png)
captures show their direction handles and Inspector properties. The
[Game capture](../docs/design/images/editor-scene-game-no-gizmos-metal.png) shows the same scene
without editor marks.

Every recipe takes `--profile <name>`. The four profiles mean the same thing in Cargo that they mean
in CMake — M0's spike wrote that column and reserved it unused for five milestones, and
`crates/cy-editor-app/tests/profiles.rs` reads `justfile`'s table and checks it rather than trusting
it:

| `just` profile | CMake configuration | assertions | Cargo profile |
|---|---|---|---|
| `debug` | Debug | on | `dev` |
| `dev` | Development | on | `development` |
| `profile` | Profile | off | `profiling` |
| `release` | Shipping | off | `shipping` |

## Current authoring surfaces and command help

The desktop now carries the local parts of the M11.b editor completion pass. Document tabs preserve
open order, active document and per-document view state across restart; dirty tabs close only through
Save, Discard or Cancel. The Hierarchy searches and selects by stable identity, supports additive,
subtractive and visible-range selection, and routes rename, reparent and template creation through
transactions. Settings separates canonical project/platform overrides from per-user preferences.
Source Control presents status and history through Git, Perforce or the null provider and names a
provider when checkout, revert, submit or locking is unavailable. Undo History attributes entries to
the human or agent intent that produced them.

The Swift Workspace discovers project Swift files, keeps edits in buffers until Save, and tracks
cursor, selection, diagnostics and external-change conflicts. Save includes the fingerprint and base
text the edit began from, so a human, agent or external tool cannot silently overwrite another edit;
Reload, Keep and Merge remain explicit choices. SourceKit-LSP supplies diagnostics, navigation,
completion, hover, symbols and rename only when the server advertises them. Editing, saving and the
existing Swift build/reload loop remain usable when SourceKit-LSP is absent or terminates.
For a selected scripted node, the small arrow beside `ScriptBehaviour.class` resolves its
`@Behaviour(name:)` declaration, opens that project source, and activates the Swift Workspace tab.
Generated Swift files under the project's `build/` directory are omitted from the source tree;
file labels stay compact and reveal their full paths on hover.

Semantic Diff compares document operations by stable identity. Merge classifies independent changes
and typed conflicts, accepts local, incoming or a validated replacement for each conflict, and
commits the resolved merge as one attributed, undoable transaction. The Content Browser adds folder
navigation, combined name/type filtering, identity-safe move/rename with sidecars, scene and Inspector
drop intents, and importer-declared settings for supported formats.

`cyberdyne-editor --list-commands` is the canonical command help: it prints every registered command
with its typed parameters and effect class. The desktop palette, scripts and MCP tool listing are
projections of that same registry, so these are not separate APIs:

Two bounded discovery paths support release and roadmap checks without replacing the desktop:

- `just run-editor --smoke` opens the native editor window, draws three frames through the shipped
  shell and dock layout, and closes successfully.
- `cyberdyne-editor --list-importers` prints the importer names, extensions and typed settings the
  editor discovered from `cy_import_cli`. `tools/editor/import_contract.py` compares that projection
  with the tool's own catalogue so a format cannot disappear from either side unnoticed.

| Area | Registered commands added or completed by this pass |
|---|---|
| Hierarchy and history | `scene.rename-entity`, `scene.reparent-entity`, `scene.create-entity`, `edit.undo`, `edit.redo` |
| Settings | `settings.set-flag`, `settings.set-whole`, `settings.set-real`, `settings.set-text`, `settings.set-list`, `settings.reset` |
| Source control | `source-control.refresh`, `source-control.history`, `source-control.checkout`, `source-control.revert`, `source-control.submit`, `source-control.lock`, `source-control.unlock` |
| Swift Workspace | `source.write`, `source.delete`, `project.build`, `project.reload` |
| Semantic merge | `document.merge-start`, `document.merge-resolve` |
| Content Browser | `asset.import`, `asset.move`, `asset.rename`, `asset.place`, `asset.assign`, `asset.import-setting.set` |
| Lighting & lightmaps | `lighting.bake-lightmaps`, `lighting.cancel-lightmap-bake`, `viewport.view-mode.lightmap-density` |
| Physics (#29) | `physics.joint.add`, `physics.joint.set`, `physics.joint.remove`, `viewport.physics.<layer>`, `viewport.physics.hide-all` |
| Navigation | `navigation.world.create`, `navigation.settings.set`, `navigation.bake`, `navigation.bake.status`, `navigation.{surface,obstacle,area,link}.add`, `navigation.path.query`, `navigation.point.pick` and the rest of the eighteen `navigation.*` commands (issue #28) |

**Lighting & lightmaps.** The lighting and lightmap baking specialised editor opens onto a form: a
level's `.cylightmap` description, an output, Bake, Cancel and the bake's progress, and the density
view. `lighting.bake-lightmaps` runs the engine's bake as `cy_build lightmap` in an operation, so
the progress surface shows the texels traced and its row's Cancel — or
`lighting.cancel-lightmap-bake` — sends the tool its `cancel`; a cancelled bake writes nothing.
`CY_BUILD` names the tool, or it is found as `build/<profile>/tools/build/cy_build` walking up from
the project. `viewport.view-mode.lightmap-density` requests the engine's `LightmapDensity` debug view,
which `cy/frame.slang` draws (`src/rendering/lightmaps/README.md`); like every debug view, the
editor-hosted runtime does not draw it yet, and it loads no cooked lightmap.

Conflict-sensitive commands deliberately require observed state. `source.write` requires
`expected_fingerprint` and the exact `base` text; a conflict returns base, buffer and disk text.
`asset.move` and `asset.rename` require the catalogue's `expected_fingerprint` and move the source,
`.meta` and `.import` sidecars together. Traversal, collision, changed-on-disk and unsupported
format/setting cases are named refusals rather than best-effort mutations.

This pass does not manufacture data that an engine producer does not expose. Rendered Content
Browser thumbnails and previews, renderer/debugger/profiler captures, remote-device encoded
streaming, general cook/package/deploy and device installation, and specialised domain editors
without canonical authoring vocabularies remain open. The viewport continues to show an engine frame
or an explicit reason that no frame is available.

## Adding a specialised editor

`cy_editor_interface::specialised` declares the sixteen specialised-editor domains and the shared
surfaces they edit on: one graph canvas, one timeline surface, one painting surface. The shell has
one piece of window code for each of those, and a new tool is built from them rather than beside
them.

| Piece | File | What it gives a tool |
|---|---|---|
| The panel scaffold | `crates/cy-editor-shell/src/panels/specialised.rs` | `SpecialisedTool`: the header (title, Undo, Redo over the active document's history), the domain opened through `SpecialisedEditors::open`, a diagnostics area, and `register_tool`, the MCP and undo parity check |
| The node-graph canvas | `crates/cy-editor-shell/src/panels/graph_canvas.rs` | `draw_canvas`, the node property controls, and `node_palette`/`catalogue_palette`, which host any engine-declared vocabulary (`Domain::node_types` or a backend catalogue). The material and VFX graphs draw through it |
| The timeline | `crates/cy-editor-shell/src/panels/timeline.rs` | `show` over a `TimelineSurface`: ruler and playhead scrub, tracks, keys and clips, zoom about the pointer, selection, Escape to cancel a drag, and `TimelineEdit`s that each answer their own inverse |

To add one, for example the animation editor:

1. Put the engine's vocabulary behind the domain: node types in `Domain::node_types` or a backend
   catalogue, track kinds in `Domain::track_kinds`. A domain with neither refuses to open by name,
   and that is correct until the engine owns the vocabulary.
2. Register the authoring commands. Each mutation is one command that runs inside one document
   transaction, with `EffectClass::ReversibleMutation`, so `edit.undo` covers it and the MCP tool
   list carries it without further work (tools are a projection of the registry).
3. Write one module in `crates/cy-editor-shell/src/panels/` with a type that implements
   `SpecialisedTool`: `DOMAIN`, `TITLE`, `COMMANDS` (every command the panel invokes; a long
   operation that edits no document goes in `OPERATIONS` instead), `target`
   (resolve what is edited, or draw the empty state), `diagnostics` and `body`. The body draws on
   the session's shared surface and pushes `Intent::Invoke` for registered commands; it never
   mutates a document.
4. Route the kind in the `match` in `panels/mod.rs`
   (`"editor-<spec term>" => specialised::show::<my_tool::MyTool>(self, ui)`) and add the type to
   `register_specialised_tools`. `Application::new` calls that last, and startup is refused, naming
   the command, if a panel command is unregistered, excluded from agents, or not undoable.
5. Test it: a Rust case per acceptance point, an MCP case in
   `crates/cy-editor-mcp/tests/a_session_over_the_wire.rs` that drives the same commands and
   undoes them, and a frame in `crates/cy-editor-shell/tests/new_panels_are_accessible.rs`.
   Prove each case red with a recorded mutation.

`panels/terrain.rs` is the worked example: `TerrainTool` is the whole panel, and its refusals appear
in the scaffold's diagnostics area. See [Terrain tools](#terrain-tools) for how its strokes reach
the engine. `panels/navigation_baking.rs` (`NavigationTool`, issue #28) is a form tool on the same
frame: it opens no shared surface, reads the document and the engine's answers in `target`, and arms
the viewport so the next click asks the engine for a navmesh point (`navigation.point.pick`) instead
of selecting.

`panels/lighting.rs` is the scaffold's one tool with an `OPERATIONS` list: a lightmap bake writes a
cooked file through `cy_build lightmap` (`EffectClass::ExternalEffect`), not a document transaction,
so there is nothing for undo to restore. `register_tool` holds each listed operation to being an
external effect and an MCP tool with no exclusion — a document mutation listed there is refused,
naming it, as is a bake listed in `COMMANDS`.

`crates/cy-editor-shell/tests/panel_snapshots.rs` renders a panel offscreen through the same
`Panels::ui` and egui-wgpu renderer the window uses, on any wgpu adapter, with no window and no
input. `editor:window?panel=<kind>` can only capture a front tab. Run it with
`CY_PANEL_SNAPSHOTS=<directory> cargo test -p cy-editor-shell --test panel_snapshots -- --ignored`.
The terrain panel [before](../docs/design/images/editor-terrain-before.png) and
[after](../docs/design/images/editor-terrain-after.png) the port shows the brush field, which was
squeezed into a strip at the right edge, now filling the space beside the controls. The
[diagnostics area](../docs/design/images/editor-terrain-diagnostics-after.png) replaces the
[refusal painted over the field](../docs/design/images/editor-terrain-diagnostics-before.png).
The [material](../docs/design/images/editor-materials-canvas.png) and
[VFX](../docs/design/images/editor-vfx-canvas.png) graph panels render pixel for pixel as they did
before the canvas moved. Graph gestures and timeline gestures come back to the host as
one value per completed gesture (`CanvasFeedback::on_connect`/`on_move`, `TimelineEdit`), so the host
turns one drag into one command and one undo entry.

## Terrain tools

The terrain panel sculpts (Raise, Lower, Smooth, Flatten), paints material layers and cuts holes,
each with a radius, strength and falloff. The editor computes no terrain. It sends the stack to the
engine's `cy::terrain` module and shows what the engine answers.

- **One gesture is one transaction.** A drag across the brush field is one `terrain.stroke.commit`,
  which adds one modifier child under the terrain root. Undo removes it. Modifiers can be disabled
  and reordered, and every one is kept.
- **The engine evaluates the stack.** `Editor::pump` sends the edited terrain's whole ordered stack
  to the engine's `terrain.evaluate` (`crate::terrain_engine`) after every change, undo and redo
  included. Only one evaluation is in flight at a time, and a stack is not sent twice. The engine
  answers with heights, material texels, holes, triangle counts and the regions whose navigation
  it marked stale. The payloads are specified in `src/editor_backend/include/cy/editor/
  terrain_service.h`. Undo needs no inverse on the wire: the document restores the stack, and the
  same stack evaluates to the same bytes.
- **The panel draws the engine's answer.** The brush field shows the engine's surface: shaded
  heights, tinted painted layers, holes see-through, and a thin warning outline around each stale
  region. The status line gives the triangle and hole counts. Without a runtime it says that strokes
  are recorded and will be evaluated when the engine connects. The hosted runtime draws the same
  meshed surface in the viewport at the terrain root.
- **Navigation is flagged, not rebaked.** The engine marks the reach of every modifier that is
  added, removed or changed. The region stays stale until navigation is rebaked, which is #28's
  job.
- **Agents get the same tools.** `terrain.brush.apply` takes the stroke as numbers (`points` as
  `"x y [pressure]; ..."` from 0 to 1, plus `radius`, `strength`, `falloff`, and `layer` for paint)
  and records the same modifier a panel gesture would. `terrain.status` reports the engine's last
  answer, with digests of its heights and weights.

The [sculpted, painted and holed terrain](../docs/design/images/editor-terrain-tools.png) and the
[paint tool](../docs/design/images/editor-terrain-paint.png) show a reply the engine produced.
`crates/cy-editor-shell/tests/fixtures/` holds the engine's reply to the requests the editor sends
for the scripted strokes. `panel_snapshots.rs` checks the requests on every run, and
`integration.editor_backend_terrain` checks the reply. To regenerate them, run
`CY_TERRAIN_FIXTURE=write` first on `cargo test -p cy-editor-shell --test panel_snapshots` and then
on `ctest -R editor_backend_terrain`.

The [viewport](../docs/design/images/editor-terrain-viewport.png) shows a raise with a hole cut
through it, as the hosted runtime's Vulkan frame draws what the engine meshed. It is photographed by
`smoke.editor_authored_frame_vulkan` when `CY_TERRAIN_VIEWPORT_SHOT` names a PNG path:
`CY_TERRAIN_VIEWPORT_SHOT=<file>.png build/dev/cy_test_smoke_editor_authored_frame_vulkan
--test-case='authored native frame draws the terrain*'`.

What is not built yet:
- Painted layers show in the panel but not in the viewport, because the engine's terrain surface
  material is not yet bound on a device (`src/terrain/README.md`, "No shader").
- A hole can be undone but cannot be filled with an eraser stroke.
- The terrain's extent is fixed at 128 m by two engine tiles (`terrain_engine::TERRAIN_EXTENT_METRES`).

## The audio tools

The Audio Mixer (`editor-audio-buses-and-mixing`, `panels/audio_mixer.rs`) is the second tool on the
specialised scaffold. It edits two project assets and one scene component, and the engine plays all
three through one `cy::audio::AudioServer` in the hosted runtime:

| What | Where | Authored by |
|---|---|---|
| The bus graph | `audio/mixer.cymixer` (`cymixer 1`) | `audio.mixer.create`, `audio.bus.add`, `.remove`, `.volume`, `.flag`, `.route`, `.send`, `.effect.add`, `.effect.set`, `.effect.remove` |
| A playable sound | `*.cycue` (`cycue 1`) | `audio.cue.save` |
| A sound in the world | `cy::audio::AudioSource` on an entity | `audio.source.create`, `audio.source.range`, or the Inspector |

Every edit is one undoable transaction in the open world's history and an MCP tool of the same
name. A mixer edit validates the whole graph first, so a route or send that would close a cycle is
refused before it is saved. A saved mixer is sent to the engine (`audio.mixer.apply`), and an undo
sends the text the file returned to. The engine keeps every bus that keeps its name, so a gain
change does not interrupt what is playing.

The panel shows what the engine answered, not what it sent: each bus's gain, routing and effect
chain as the engine's graph holds them, and each bus's last-block peak as a meter with its value in
dB. The line above the table names the backend, so a mix on the null backend is never mistaken for
a silent device. While the panel is on screen the editor asks for the state four times a second.
The panel refuses to open until the engine has answered `audio.capabilities.get`, because the
effects it offers are the engine's.

`audio.cue.preview` plays a saved cue through the mixer. `audio.source.preview` plays a source's cue
at its position, heard from the focused viewport's camera. `audio.status` reports the engine's last
answer: `backend`, `active_voices`, `playing`, `play_voices`, `bus.<name>.volume`,
`bus.<name>.peak`, `bus.<name>.route`, and `preview.gain`, `preview.left` and `preview.right` for a
spatial preview. `audio.refresh` asks for a new one. With no output device, `seconds` advances the
engine's mix first, which is how the tests measure a level.

In the Editor view the runtime draws each enabled source as a teal speaker, with a solid ring where
its attenuation starts (`min_distance`) and a dashed ring where it falls silent (`max_distance`).
Both rings are projected with the frame's own view. Clicking the speaker selects the source, and the
panel then shows its range and **Preview from the camera**. Game view draws no rings.

The runtime opens the miniaudio output device under `CY_AUDIO`. Where no device opens, it mixes on
the null backend and says so on stderr, and the panel's backend line reads `null`. A clip is
`tone:<hertz>:<seconds>`, or a project-relative 48 kHz WAV (16-bit PCM or 32-bit float, mono or
stereo). Decoding other formats is still the asset system's job (`src/servers/audio/README.md`).

The contract between the two sides is one set of files, `src/editor_backend/tests/data/audio_*`. The
mixer, cue and preview request are what this workspace encodes, and its tests compare them byte for
byte. The engine's suite (`integration.editor_backend_audio`) submits those same bytes, reads the
result out of the `AudioServer`, and writes the state and vocabulary replies. The MCP tests replay
those replies as the runtime's answers. The [mixer](../docs/design/images/editor-audio-mixer.png)
and the [selected source](../docs/design/images/editor-audio-source-range.png) are rendered offscreen
by `tests/panel_snapshots.rs` from those same engine replies.

## The dependencies, and the rule they arrived under

Through M5 the workspace depended on nothing but the Rust standard library, and
`[workspace.dependencies]` was empty rather than absent so that adding the first one would be a
visible edit in a reviewed file. M5.5 is that edit, and it was a decision about *when* rather than
about *whether*: the interface toolkit is specified to be "an implementation choice behind editor
abstractions, **selected on measurement**", the measurement was taken, and the choice is recorded in
`openspec/changes/implement-m5b-operable/design.md` with the numbers that decided it.

| Crate | Version | Named by |
|---|---|---|
| `eframe`, `egui`, `egui_dock`, `egui-wgpu` | `=0.36.1` / `=0.21.1` | `cy-editor-shell`, and nothing else |
| `wgpu`, `wgpu-hal`, `wgpu-types` | `=30.0.1` | `cy-editor-shell` and `cy-editor-viewport-transport` |
| `ash`, `libc`, `pollster` | pinned | `cy-editor-viewport-transport` |
| `image` | `=0.25.9` | `cy-editor-shell`, for the identity PNGs |

`cy-editor-mcp` has none, deliberately: what it needs is a JSON value, a parser and a writer for the
subset JSON-RPC carries, which is four hundred lines with its own round-trip tests. A serialisation
library would bring a derive macro and a proc-macro toolchain into a workspace that has kept
`cargo build --offline` working for five milestones, in exchange for code the crate would still have
to review.

Every version is exact rather than caret-ranged, because the spike measured *those*. The consequence
is that `cargo build --offline` no longer works in a bare checkout, so the continuous-integration
editor job carries a registry cache keyed on `Cargo.lock`.

Everything below layer 5 still builds and tests with no window, no graphics device and no interface
toolkit, and `crates/cy-editor-app/tests/containment.rs` is what keeps that true.

## The SDK is generated, and cannot drift

`crates/cy-editor-sdk/src/generated/` is produced by `tools/gen/rust/sdk_gen.py` from the ABI
description that `tools/abi/abi_describe.py` computes — the same single parse that produces the
committed ABI baseline and the Swift overlay, so no two of the three can disagree about the header.

`cargo test -p cy-editor-sdk` fails when the committed bindings are not what regeneration produces,
and the layout the description computed is asserted against `rustc` by `const` assertions that fail
the *compile*. `src/abi/tests/test_layout.cpp` asserts the same numbers against the C compiler, so
the model is checked from both sides of the boundary it describes.

## Hosting modes

| Mode | Engine location | State today |
|---|---|---|
| `NoRuntime` | None | Complete. Project browsing and document editing work with no engine at all |
| `Embedded` | In the editor process | Implemented and exercised against `cy-editor-testhost`. **No engine build exports the entry point it needs** — see `crates/cy-editor-sdk/src/host.rs` |
| `Hosted` | A separate process or a remote device | **The default.** The engine host is `cy_editor_window_runtime` (`just run-engine`, `just run-editor-live`); `cy-runtime-stub` holds no world and serves the scripted artefacts |

`crates/cy-editor-app/tests/survives_a_runtime_crash.rs` starts a runtime as a separate process,
edits a document, kills the process with a signal, and asserts that the editor is still running with
its documents and its history intact. A test that dropped a socket would prove the protocol handles
an end of stream; only killing a process proves the editor's fate is not tied to the runtime's.

## What the live-bridge spike measured, and what was built on it

The spike at `build/spike/latency-spike/` measured a gizmo drag's round trip. Four findings are
structural in `cy-editor-protocol` rather than advisory:

* The boundary costs about **60 microseconds** at p50; the runtime's frame costs **8.6 ms**, and
  that is paid identically with no boundary at all. Out of process is +0.3 to +1.3 ms at p50.
* **Never block a UI frame on the round trip.** `Session` has no blocking send. Blocking p50 is
  9.9 ms locally, p99 is 20.2 ms, and past a LAN the p50 alone is 27 to 170 ms.
* **Apply on arrival when nothing is simulating**: p50 0.059 ms against 9.938 ms, a 168-fold
  improvement from a scheduling decision. `Message::Apply` carries `ApplyWhen` so the runtime is
  told rather than guessing.
* `TCP_NODELAY` and an application-level retransmit on any future TCP path: 1% loss at
  `TCP_RTO_MIN` gave p99 210 ms against 30 ms with a 20 ms application timeout.

**Not measured: the viewport transport.** The spike covered the control path only. If the image a
user drags against is two or three frames stale, the drag feels laggy however fast the transform
applies. M5.5's synchronisation spike measured exactly that and `cy-editor-viewport-transport` was
built on the result; see its README for the numbers.

## The window

`just run-editor` opens it. The window is the **default** — `--headless` is what asks for the
scripted driver, and `--script` implies it — because a capability that has to be asked for is a
capability nothing exercises, which is the defect M5.5 exists to correct.

What it draws and what it refuses to draw:

* **The viewport shows the engine's own frame, or a sentence saying why it cannot.** There is no
  toolkit-drawn approximation, no grid and no placeholder cube. `editor-viewport-and-gizmos` forbids
  a second renderer, and an approximation is one arriving a frame at a time.
  Short frame hitches remain in the pacing counters; the stale-image warning starts after 250 ms
  without a fresh focused frame (500 ms when unfocused). Its age refers to the displayed frame,
  so it does not claim the runtime stopped when delivery or the UI was delayed.
* **The inspector is generated from reflection**, from the open world's schema or from the engine's
  registered component types. There is no type name anywhere in `panels/inspector.rs`; when nothing
  has been described it says so rather than showing a hand-written form.
* **Panels whose capability has not arrived say which milestone brings it**, rather than drawing an
  empty canvas that looks like a working one.
* **The default workspace is the arrangement of `docs/design/images/editor-rts-desertfrontier.png`.**
  Three references exist and they differ; `crates/cy-editor-shell/src/lib.rs` says which is shipped
  and why.

## Worlds, and what M6 changed about opening one

At M5.5 the editor was real and had nothing to edit. Its own gate wrote it down: *"`DocumentService::open`
calls `Document::new` — a name and an EMPTY SCHEMA — because there is no world loader."* Nothing was
selectable, no `Transform` bound, the inspector correctly reported that nothing was described, and a
gizmo drag committed nothing.

M6 task 2.4 closed it from the **engine's** end. Two files, one grammar, and
`crates/cy-editor-services/src/worldfile.rs` reads both:

| File | First line | Written by |
|---|---|---|
| `types.cytypes` | `cyschema 1` | The engine, from its own `cy::reflect::TypeRegistry` (`cy::scene::serialization::write_authoring_schema`). Committed per project and regenerated-and-compared by `cy_test_unit_scene_serialization`. |
| `<world>.cyworld` | `cyworld 1` | The editor, by `file.save`. Carries the schema it was written against, then its nodes. |

Three consequences, each of which is a test rather than an intention:

* **`TransformBinding::of_schema` finds a `Transform`**, because the manifest names one — and it is
  the engine's `LocalTransform`, grouped into `translation`, `rotation` and `scale` and aliased once.
* **`scene.create-entity` gives a new entity that transform**, so a created object is somewhere
  rather than nowhere and a gizmo has something to move.
* **`file.save` writes the world**, which M5's own source said was "the serialisation layer's, at a
  later task". A second editor opens what the first wrote and finds the same values and no recovery
  to offer.

`crates/cy-editor-services/tests/a_person_opens_a_world_and_saves_it.rs` is that sequence end to end
with no window, and `samples/05b-editor-window` is the same sequence through synthesised X11 input.

**A load is not an edit.** Opening a world applies its content through a transaction — that is the
only write path there is — and then forks the document, which is what leaves it clean with an empty
history. Undo may not take a world away.

## Creating a primitive, and the one thing the editor does *not* do (M8.a task 2.1)

`scene.create-primitive` makes a box, sphere, cylinder, plane or capsule in the open world. What it
writes is a **source asset**: `assets/primitives/<Name>.cyprim`, six lines of text naming the shape
and its parameters. The geometry is `tools/import/`'s — a `.cyprim` is imported like a `.gltf`, by
the same registry, under the same derivation key, into the same cache — so there is no mesh
generator in this workspace and nowhere here to put one. `design.md` §2 of
`implement-m8a-authorable` is the argument: a created box is "a mesh instance whose mesh the engine
generated rather than imported", and every later system must be unable to tell the difference.

Three consequences worth knowing:

* **One transaction per primitive.** Undo removes the entity; redo restores it with the same
  identity. The `.cyprim` stays on disk, exactly as an imported `.fbx` does when its placement is
  undone.
* **`create_mesh_instance` is the only constructor.** An import that lands a mesh in the world calls
  the same function with a different path, which is how "nothing downstream can tell them apart" is
  guaranteed rather than checked. `crates/cy-editor-services/tests/`
  `primitives_are_ordinary_mesh_instances.rs` asserts it over components, the gizmo's binding, the
  mesh reference and the saved file.
* **Parameters are edited by editing the asset.** `asset.write-primitive` rewrites a `.cyprim`; the
  engine re-generates its mesh under a new key and every entity drawing it follows. A parameter the
  shape does not take is refused rather than ignored, on both sides of the boundary.

The mesh reference lives on a `MeshRenderer` component with a `mesh` field, found by name in the
document's schema and declared there when the world does not already carry it — the same
by-name relationship `TransformBinding` has, and the engine's own name for the component
(`src/scene/src/node_template.cpp` declares `cy::render::MeshRenderer` and no build registers it
yet). The editor runtime resolves this authoring reference by name and draws its mesh through
`FrameAssembly`; it does not require the component to be reflected to render saved worlds.

## Catalogue-driven material properties

The Material Graph does not identify node names to decide which widgets to draw. Catalogue schema
3 describes each property by stable identity, kind, typed default, numeric constraints, enum
choices, required asset kind, semantic role, compiler/runtime stage, graph domain and target
capabilities. It also carries each node's engine-owned surface/vertex stage mask; the surface
palette hides vertex-only nodes. The Material Graph's Stage selector switches the palette between
surface and vertex-compatible engine nodes. Both stages use the same saved canvas and its engine
owned output roots. The shared graph canvas validates authored literals before
mutation and retains values by node and property identity across compatible catalogue refreshes.
Texture controls query the project asset catalogue for stable identities whose kind is `texture`;
the compiled dependency list therefore contains asset identities rather than display paths.

Schema-1 and schema-2 catalogues remain readable. Schema 1's combined textual constraint is
migrated into the typed property shape, and older catalogues leave stage compatibility unrestricted,
so reconnecting an older runtime does not discard the graph being authored.
The engine catalogue includes `material.sin`, a typed scalar/vector sine node shared with the
text material front end. It is available for authored material arithmetic. The vertex palette offers
`material.vertex_output` for an offset expression or scalar normal displacement, and typed `material.object_position`,
`material.world_position`, `material.normal`, and `material.uv0` geometry inputs. World position
uses the renderer's camera-relative world coordinates; object position uses the mesh's local
coordinates. The hosted viewport binds both positions for visible and shadow vertex evaluation.
`material.time` reads elapsed engine seconds from the first-light hosted preview frame in both stages, so a
`material.sin` chain can animate an offset without a parameter edit or recompile. The shared
`material.noise` node samples smooth scalar noise from a float3 position, so a world-position
input can vary vertex motion spatially. `material.procedural_wind` samples a smooth animated float3
vector from world position and engine time; multiply it by a scalar or vector amplitude before connecting
it to the vertex offset. `material.vertex_color` reads the mesh vertex's linear RGB colour;
the hosted compiled-material preview passes that attribute through to vertex and fragment graphs.
Its generated mesh assigns a different colour to each face axis for a visible preview.
The `displacement` pin accepts a scalar distance in metres; the engine combines it with any
connected `offset` as `offset + normal * displacement` for visible and shadow vertex programs.
The typed `wind` field node samples the Engine weather field in the authored scene and hosted
material mesh previews. The material mesh renderer refreshes its weather image when the camera
moves outside the preview region; other field names receive a named refusal. The sample project
includes `materials/issue15_sway.cymatcanvas` and its Engine-authored `.cygraph` beside
`worlds/issue15-sway.cyworld` for opening the time and sine vertex graph in a scene. Native pixel
proof for displacement, shadow, and motion remains tracked by issue #15.
Run `just test-issue15-acceptance` on a Mac with a working Metal device to verify the full issue
#15 ledger locally, including its reference image and CPU-displaced pixel comparisons.
When an opened graph is assigned to a mesh in the active scene, including an imported material
slot, Validate, Compile, Save, and live Preview send its geometry source to the Engine material
compiler. An ordinary mesh requests `StaticMesh`; a `.cyvg` mesh asset requests
`VirtualGeometry`. A material assigned to a terrain layer sends `Terrain`; a material used by
multiple sources requests each variant. A `.cyvg` asset can be assigned through the same mesh
asset command used by the Inspector and MCP. The editor
backend also accepts named geometry-source requests and reports the compiler's
`vertex-geometry-unsupported` diagnostic for a vertex graph assigned to `VirtualGeometry`. A
successful Compile result lists the named geometry sources whose variants were produced. A vertex
graph assigned to a `.cyvg` mesh is refused before Save or live Preview replaces the scene graph.
The desktop Save button and `material.graph.save` over MCP invoke the same registered command.
Both send the active scene's mesh and terrain assignments to the engine, which refuses unsupported
vertex paths before authoring the canonical graph. Saving a graph and syncing its generated
Inspector fields form one undo step; undo and redo restore both graph files and scene fields.
When that save comes from the open Material Graph, undo and redo also reload its canvas from the
project source. Undoing a newly created graph clears the canvas; an unrelated history step leaves
unsaved canvas edits alone.
On an unchanged saved canvas, the desktop palette routes node add, connect, drag release, typed
property edit, disconnect, and remove through `material.node.*` commands. The same commands are
available over MCP, validate against the engine's material catalogue, and ask the engine to author
each edit as one undoable save. Before a canonical graph exists, canvas gestures record the
editable `.cymatcanvas` through `material.canvas.draft.save` as individual undo steps. This keeps
incomplete graphs editable without asking the engine to author them. The canonical Save still
validates the complete graph through the engine.
The authored scene frame now compiles vertex offset and interpolant graphs for visible, depth,
and shadow passes. A graph requiring an unbound environment field is refused before replacing the
last valid preview. The Engine weather wind field is bound for scene and material-mesh previews.
The scene viewport conservatively keeps vertex-graph meshes in view and shadow draw lists because
their displaced bounds can extend beyond the source mesh. Large scenes with many such graphs may
draw more meshes until authored displacement bounds are available.
With no authored world open on the Metal host, live graph Preview applies to the first-light
material mesh. With a world open, Preview applies to every scene mesh using that graph reference.

## Importing an asset from inside the editor (M8.a tasks 3.1 and 3.5)

`asset.import` cooks a source file the project already holds — a glTF, an FBX, an **OBJ with its
`.mtl`**, or a texture — and places what it produced in the open world as an entity. Before it, the
importer was reachable only from a command line, so content was cooked outside the editor and, more
to the point, **an agent could not import at all**: the agent interface is a projection of the
command registry, so a capability that is not a command is not a tool. `--list-commands` and the MCP
tool listing both carry it now, with its parameters and its effect class.

* **The importer runs as a subprocess.** `tools/import/` is layer 7 and nothing links layer 7 —
  `thirdparty-dependencies` requires that a shipped runtime carry no glTF or FBX parser, and the
  layer is what makes that true rather than intended. So the editor runs `cy_import_cli`, exactly as
  `ProjectService` runs `cy_swift_module.py`. It is found through `CY_IMPORT_CLI` first, then by
  walking up from the project for `build/<profile>/tools/import/cy_import_cli`.
* **`--json`, not prose.** The command reads a machine-readable report: which importer ran, the
  identity the source holds, what the cache did, and every sub-asset with its stable kind, source,
  dependencies and identity. Schema 3 also projects complete prefab transforms, mesh identities and
  material slots. Imported slots persist in `ImportedMaterialSlots`; slot zero is mirrored to the
  engine-owned `MeshRenderer.material` field.
  An agent that had to parse a paragraph to find the mesh is an agent that will get it wrong.
* **The entity is built by `create_mesh_instance`**, the same function `scene.create-primitive`
  calls. Undo removes the entity; the cooked assets and their sidecars stay, because they belong to
  the file rather than to the world.
* **A format's absent steps are stated, not warned about.** An OBJ carries no rig, no animation and
  no scene graph, so it reaches steps 1-6 and 9 of the import sequence. The result names 7, 8 and 10
  and counts none of them as a warning: `asset-import-pipeline` is explicit that a step a format
  cannot express "is not a warning about the file and SHALL NOT be reported as one".
* **A refusal names what this build can import**, read from the importer itself, so a project's own
  importer appears in that list the day it is registered.
* **External companion files are staged with their source.** OBJ `mtllib` files and their declared
  texture maps retain relative paths; FBX relative texture references and the conventional `.fbm`
  folder are copied before the ordinary importer command runs. Reimport replaces the project-owned
  staged bytes while stable sub-asset identities continue to come from the sidecar.

`crates/cy-editor-services/tests/importing_from_inside_the_editor.rs` drives all of it through the
registry against a recording double; `assets.rs`' own unit tests parse the exact bytes a real
`cy_import_cli --json` run produced, which is what pins the two halves of the boundary together.

## Adding a body, and pressing play (M8.a tasks 4.3, 5.1 and 5.2)

`scene.add-body` puts a physics body and its collider on an entity, as **one** transaction. The two
go together on purpose: `cy::physics::validate` refuses a dynamic body with no collider — it has no
volume, therefore no derived mass — so writing them separately would leave a world that cannot
simulate for exactly one undo step. `shape=none` is the escape hatch and `scene.add-collider` is the
other half of it; `scene.remove-body` takes the body away and records the values it had, so undo
restores the body that was there rather than a default one.

The component names are the engine's own with the namespace dropped — `RigidBody`, `StaticBody`,
`KinematicBody`, `Collider` — and **they are a contract across the process boundary**, because
physics' components are registered in the ECS by name with no reflected type behind them:
`cy::scene::serialization::resolve_against` carries them rather than resolving them, and
`src/gameplay/play/src/session.cpp` reads them back out of the saved `.cyworld`'s own type section by
name. The spellings are held in both languages' tests, the way `.cyprim`'s golden string is.

**`play.enter` now reaches the runtime.** Until M8.a it set `PlayState` on every viewport and told
the engine nothing: the badge said PLAYING and the world did not move, which is design.md §4's
"today it reports `hosting: NoRuntime`". `Message::Play` carries the state as a **word** —
`editing`, `playing`, `paused` — so a fourth state added on one side is refused by name rather than
falling through a match to the closest number, and `Message::Playing` answers with the state
actually in force plus a line a person reads.

The badge is still switched first and unconditionally. An editor with no engine attached is a
first-class mode, so pressing play there is not an error — but it must not claim to be simulating
either, and the summary and a notification both say which of the two happened.

`crates/cy-editor-services/tests/pressing_play_reaches_the_runtime.rs` drives the whole path over a
real socket; `crates/cy-editor-services/tests/a_body_is_a_transaction.rs` holds the transaction and
the golden names. What is on the far end is `cy::gameplay::PlaySession`, and what it guarantees —
**stop restores the authored document byte for byte, verified rather than asserted** — is
`src/gameplay/play/README.md`.

## Physics tools: debug layers and joints (#29)

The **Physics** panel (panel kind `physics`, beside the specialised editors) draws in the scaffold's
frame — its header with Undo and Redo, its diagnostics area — and `register_specialised_tools`
checks its commands with the same parity rule as a specialised tool (`command_parity`). It is not a
seventeenth `Domain`: `editor-architecture` names no physics editor, and what it edits is entities
and the viewport. The design is `openspec/changes/add-editor-physics-tools/design.md`.

![The Physics panel with a hinge selected: the viewport layer toggles, the joint's kind and target,
and only the fields a hinge reads](../docs/design/images/editor-physics-joint.png)

**Debug layers.** `cy_editor_viewport::physics_view::PhysicsLayer` is `cy::physics::DebugDrawFlags`,
bit for bit (a test reads `debug.h`). Each is a read command, `viewport.physics.<layer>` with
`state=on|off|toggle`, plus `viewport.physics.hide-all`; the panel's checkboxes are callers of them.
A viewport's layers go out in the gizmo request (`physics_overlays`, after the camera choice), and
`cy_editor_window_runtime` draws them from the play session's physics world with
`PhysicsServer::debug_draw`, projected through the frame's own view into the pixels it publishes.
The editor draws nothing. The layers show the simulated world, so they appear while a world plays or
is paused.

**Joints.** A joint is a `Joint` component on the entity carrying body A, naming body B's entity
(or none, for the world), with the anchor and axis in body A's unscaled frame. `physics.joint.add`,
`physics.joint.set` (`field` and `value`, one field per transaction) and `physics.joint.remove` are
reversible, so each is an MCP tool and `edit.undo` covers it; a change the engine would refuse at play
is refused when it is made. The panel shows only the fields the selected kind reads and commits a
drag or a typed edit once, on release. Selecting the entity makes the runtime draw the joint through
`cy::physics::debug_draw_constraint` — the drawing Jolt uses for a simulated constraint — with its
axis. At play `cy::gameplay::PlaySession` resolves both bodies and derives frame B so the anchors
meet where the bodies were authored.

![The engine drawing the selected door's authored hinge while editing: its anchor on the post, the
vertical axis and the limit arms](../docs/design/images/editor-physics-joint-gizmo.png)

![Play paused with the collider, contact, joint and sleep layers on: every collider, awake bodies in
green, sleeping and static ones in grey, drawn by the engine from its physics
world](../docs/design/images/editor-physics-layers.png)

Both are taken through the editor's own MCP interface by
`python3 samples/05b-editor-window/mcp_physics.py --shots docs/design/images`, which also fails
unless the runtime reports frames carrying the joint gizmo and a paused frame the layers changed.

Entity references in a `.cyworld` are written as the referenced node's **position** and resolved to
an identity on load, on both sides, because a save that drops a node renumbers the file.

**Ragdolls are not here yet.** `physics::ragdoll::Profile::generate` needs a skeleton and the editor
cannot import one (model import stops before step 7), so the panel says so in its diagnostics area
rather than opening an empty profile editor.

Tests: `crates/cy-editor-services/tests/a_joint_is_a_transaction.rs`,
`physics_authoring_is_an_undoable_mcp_peer_of_the_physics_panel` in
`crates/cy-editor-mcp/tests/a_session_over_the_wire.rs`, the physics frames in
`crates/cy-editor-shell/tests/new_panels_are_accessible.rs`, and on the engine side
`integration.gameplay_joints` and `unit.editor_window_physics_overlay`.

## VFX graph authoring status

The VFX Graph tab loads the engine's `vfx.catalogue.get` result into the same node canvas as the
Material Graph. Its palette includes compiler-registered nodes and typed sample nodes generated
from data-interface fields. The panel creates a saved system at the Document path through
`vfx.document.create`, then adds emitters and selects each emitter's
Spawn, Initialise, Update, Event, Render, or Compute stage. Stage selection snapshots the shared
canvas and restores the selected stage; graph-tab switches preserve the active draft. The
renderer and CPU/GPU selectors come from `vfx.authoring-capabilities.get`; missing Decal, Light,
and Volume compositors appear with engine-provided reasons. GPU authoring is available while
runtime readiness waits for an attached preview device. The
**Save VFX draft** action writes a versioned `.cyvfxdoc` through the `vfx.document.save` command,
so it participates in scene-document undo/redo and can be reopened through
`vfx.document.read`. A scene document must be active for save history. This source is editable
authoring data; the engine reads and cooks it for the runtime preview.
After creation gives a system its project path, each desktop frame that changes its VFX
canvas or metadata saves one document transaction automatically. The same applies to a saved
module. Undo and redo reload the open graph from the project source; an unchanged frame does not
add history. Edits saved through MCP refresh the desktop graph before its next frame is drawn, so
the next desktop transaction starts from the current project source. Creation requires an active
scene document and refuses to replace an existing system at the chosen path.
`vfx.document.create` creates an empty named system at a project path through the same undoable
transaction. It refuses to overwrite an existing system; agents can then add CPU and GPU emitters
and stage nodes without seeding a source file outside the editor.
The command palette, scripts, and MCP also expose `vfx.emitter.add`, `vfx.emitter.remove`,
`vfx.emitter.configure`, `vfx.interface.bind`, `vfx.interface.unbind`, `vfx.node.add`,
`vfx.node.move`, `vfx.node.connect`, `vfx.node.disconnect`, `vfx.node.remove`,
`vfx.node.property.set`, and
`vfx.parameter.set`. Each reads the saved system,
applies one edit, and saves through the same undoable document transaction. Node placement,
connections, and property changes require the live engine VFX catalogue; an unavailable catalogue
or unknown node, pin, or property is refused by name.
Clicking a palette node in a saved, unchanged system stage or module uses its matching
`vfx.node.add` or `vfx.module.node.add` command, creating one undo entry and the same result as MCP.
Editing a node property in that state uses `vfx.node.property.set` or
`vfx.module.node.property.set` with the engine catalogue's property name.
Connecting compatible pins uses `vfx.node.connect` or `vfx.module.node.connect`; the canvas
validates the typed connection before the command is queued.
**Remove selected node** uses `vfx.node.remove` or `vfx.module.node.remove` for a clean saved
asset, removing its attached wires in one undoable edit.
The selected node's wires appear as **Disconnect** actions and use `vfx.node.disconnect` or
`vfx.module.node.disconnect` for clean saved assets.
Dragging a node on a clean saved system stage or module previews its position locally, then sends
one `vfx.node.move` or `vfx.module.node.move` command when the drag ends. That release adds one
undo entry. A new draft without a saved project path keeps movement local until its first Save.
If the open canvas has unsaved edits, palette insertion stays in that draft until Save so those
edits are preserved; property, connection, removal, and disconnection edits follow the same rule.
The panel's **Remove emitter** control retains the other emitters' stage graphs and selects
the next available emitter; the edit is recorded in document history for a saved draft.
**Add emitter** sends `vfx.emitter.add` for a saved system, selects the new Spawn stage after the
command succeeds, and adds one undo entry. A new system without a saved path adds locally.
`vfx.emitter.capacity.set`, `vfx.attribute.set` / `vfx.attribute.remove`, and
`vfx.channel.set` / `vfx.channel.remove` provide the panel's particle storage and bounded event
declarations through MCP with the same save and undo history. Invalid bounds or attribute types
leave the saved document intact.
`vfx.parameter.remove` removes a saved system parameter by name.
`vfx.emitter.parameter.set` and `vfx.emitter.parameter.remove` edit typed defaults for one named
emitter through the same undoable transaction. Two emitters may use the same local parameter name;
the VFX compiler and runtime resolve it by emitter. Removing an emitter removes its local
declarations too. The preview parameter command addresses a local declaration as
`emitter:name`; a bare name addresses a system declaration.
`scene.vfx-effect.create` adds a scene entity with a `.cyvfxdoc` asset reference and generated
Inspector fields for every exposed system and emitter parameter. `scene.vfx-effect.parameter.set`
changes one entity's typed override by its hexadecimal identity; both commands use the scene's
undo history. Saving a `.cyworld` stores the asset path and each instance's values.
Integer defaults and scene overrides must be whole numbers in the signed 32-bit range; invalid
values are refused without changing the saved document or instance override.
Interface names and renderer choices in saved commands are checked against the engine when the
draft is compiled; the desktop pickers only offer entries reported by the attached engine.
Reusable modules can be created and edited with `vfx.module.create`, `vfx.module.input.add`, and
`vfx.module.dependency.add`, then linked to an emitter with `vfx.module.attach`. Each command saves
one undoable change; creating a module refuses to replace an existing source at that path.
`vfx.module.stage.set` changes a saved module's compatible stage through the same undo history;
an unknown stage is refused without changing the file.
`vfx.module.input.remove` and `vfx.module.dependency.remove` remove named declarations through
the same history.
The module graph also supports `vfx.module.node.add`, `vfx.module.node.move`, `vfx.module.node.connect`,
`vfx.module.node.disconnect`, `vfx.module.node.remove`, and
`vfx.module.node.property.set`. These commands use the live engine catalogue and save each
canvas edit as an undoable module transaction. MCP wire tests save and reopen connected stage
and module nodes, then exercise move, disconnect, remove, undo, and redo through this registry.
Use `vfx.document.read` to inspect the saved source and `edit.undo` / `edit.redo` to reverse or
reapply an edit. An open scene document is required for these transactions.
The current draft payload records emitter capacity, typed particle attributes with range,
tolerance, and precision, plus bounded system event channels. Existing version 1 draft payloads
open with engine defaults (capacity 1024 and no attribute or channel declarations) and save as
version 4 payloads. Version 2 drafts also reopen. Version 3 maps module names to explicit
project-relative `.cyvfxmodule` paths. Version 4 adds emitter-local typed parameters while retaining
the older fields. The `.cyvfxdoc` text envelope remains version 1.
The VFX panel exposes those declarations in collapsible sections: typed system parameters with
runtime exposure, emitter-local typed parameters, per-emitter capacity and particle attributes with precision controls, and event
channels with event/depth limits and optional CPU readback. Invalid metadata edits leave the open
draft intact. **Save VFX draft** records the resulting document through the project transaction
command. Undo and redo of that command now refresh the open VFX document and stage canvas as well
as the project file; undoing its creation closes the open draft until redo restores it. Direct
history for a pathless draft begins with its first Save; the desktop New action creates a saved
asset immediately, so subsequent edits are undoable. A separately saved `.cyvfxmodule`
records one compatible stage, named typed inputs, dependency names, and a shared-canvas graph.
`vfx.module.save` and `vfx.module.read` use the command registry shared with MCP; save requires an
active scene document and supports undo/redo. The sample project includes
`effects/shared_drag.cyvfxmodule`. The VFX panel creates a saved module at the Asset path through
`vfx.module.create`, or opens an existing module on the shared canvas. It can edit the compatible
stage, typed host inputs and dependencies, and save changes through `vfx.module.save`. It can
attach a saved module to an emitter through an undoable document save;
the attachment records its explicit project path. Creation, module saves, and attachments refresh when the
scene history is undone or redone. A pathless local graph draft is saved as one transaction before
its node edits can receive individual history entries.
Opening or creating another module keeps unsaved changes in place; **Discard module edits**
reopens its last saved source or closes a new module that has never been saved.
Each referenced module needs an explicit path
in the system document; the engine does not guess a file from its name. Compile and preview load
the mapped project sources, validate typed inputs and dependency stages, reject missing sources
and cycles, and compose the module graphs into the engine's emitter stages before cooking.
Saved module content participates in automatic compile signatures; moving nodes on its
canvas does not request a new cook.
**Compile VFX** submits the current stage snapshots to the engine's `vfx.compile` service. The
engine reads the document into `VfxSystemAsset`, resolves its registered nodes, and runs
`compile_system`; the panel shows the last cook identity, per-emitter kernel and memory counts,
derived layout, generated Slang, and node and pin diagnostics. Compilation does not install an
effect in the preview world. A module reference without an asset mapping or source produces a
named refusal; module assets must be saved to the project before a dependent draft can compile.
The engine tags each compiler diagnostic with its emitter and stage. The panel lists that location;
clicking it opens the stage and selects the offending node. The active canvas outlines that node
in red and shows the compiler message on hover, even when another stage reuses the same node key.

## Navigation authoring commands (issue #28)

`cy-editor-services/src/navmesh.rs` registers eighteen `navigation.*` commands through
`builtin::register`, so the desktop, scripts and MCP share them. Navigation state is ordinary
scene-document data: a `NavigationWorld` component holds the agent profile (radius, height, max
slope, step height), the build settings (cell size, cell height, tile size, layer and tag masks,
`engine` or `recast` back end), the overlay flags and the accepted bake (`bake_identity`,
`source_fingerprint`, `tile_count`, `sidecar`). `NavMeshSurface`, `NavObstacle`, `NavArea` and
`NavLink` use the engine's component names without the `cy.navigation.` namespace, with the field
names `samples/05b-editor-window/runtime/nav_runtime.cpp` reads.
`the_schema_matches_the_runtime_test_map` checks that contract against the runtime's own test map.

| Class | Commands |
|---|---|
| Reversible (one transaction per call) | `navigation.world.create`, `navigation.settings.set`, `navigation.overlay.set`, `navigation.bake`, `navigation.{surface,obstacle,area,link}.add` and `.set`, `navigation.component.remove` |
| Read | `navigation.settings.get`, `navigation.bake.status`, `navigation.path.query`, `navigation.flowfield.query`, `navigation.point.pick` |

The `add`, `set` and `settings.set` commands take `values` as `field=value` pairs separated by `;`,
for example `shape.offset=4, 0, 4; shape.radius=1`. Each value is parsed as its field's kind, so
one gesture that changes several fields is still one undo entry. Settings the engine would refuse
are refused before anything is recorded.

The editor does not bake, hash geometry or cast rays. `navigation.bake` checks the settings, sends
the engine's `navigation.bake` request and returns its request id. `NavmeshService`
(`navmesh_service.rs`) matches the service events by request id and keeps the per-tile progress.
On COMPLETED, `Editor::pump` records one transaction on the world that sets the bake identity,
fingerprint, tile count and sidecar. Undoing that transaction restores the previous identity, and
the runtime reloads that bake's sidecar. A FAILED bake records nothing. `navigation.bake.status`
reports the pending request and its progress, the last report or failure diagnostics, and the
last path, flow-field and pick answers. With `refresh=true` it asks the engine for the stale flag.
The path, flow-field and pick queries return a request id, and their answers appear in
`navigation.bake.status`. The engine serves one navigation request at a time, so a second request
is refused locally while one is pending. A runtime disconnect fails the pending request.

### The Navigation panel

The Navigation tab (`editor-navigation-baking`, `panels/navigation_baking.rs`) is the desktop face
of these commands. It reads the document and the engine's answers and only pushes
`Intent::Invoke`, so every gesture is the same command an agent sends over MCP and records the same
history:

- **Navigation world.** Pick the world to edit, or create one.
- **Agent and build settings.** Edit the profile and the build settings, then **Apply settings**.
  Only the changed fields are sent, as one `navigation.settings.set`.
- **Bake.** **Bake** sends `navigation.bake` and shows the per-tile progress, then the report or
  the failure. **Check for changes** sends `navigation.bake.status refresh=true`. The badge reads
  *Not baked*, *Up to date* or *Stale*.
- **Overlays.** One checkbox per overlay flag (`navigation.overlay.set`). The engine draws the
  overlay into the viewport frame.
- **Components.** Add a surface, obstacle, area or link. **Place link in viewport** arms two
  viewport clicks that become one `navigation.link.add`.
- **Test path and Flow field.** **Pick start** and **Pick end** arm the viewport. The next click is
  sent to the engine as `navigation.point.pick` instead of selecting, and the two points feed
  `navigation.path.query`.

The Nav* components also appear in the Inspector, where edits are undoable but desktop-only.
[`docs/guides/navigation.md`](../docs/guides/navigation.md) walks through a session.
`python3 tools/issue28_acceptance.py` runs the acceptance ledger for issue #28.

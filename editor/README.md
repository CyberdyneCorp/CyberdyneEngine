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
module or unknown behaviour is reported as a Play refusal. Audio is still unavailable in this
host and is reported separately. The Editor's studio fill is omitted from
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
`material.vertex_output` for an offset expression and typed `material.object_position`,
`material.world_position`, `material.normal`, and `material.uv0` geometry inputs. World position
uses the renderer's camera-relative world coordinates; object position uses the mesh's local
coordinates. The hosted viewport binds both positions for visible and shadow vertex evaluation.
Time-driven motion and the remaining outputs are tracked by issue #15.

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

## VFX graph authoring status

The VFX Graph tab loads the engine's `vfx.catalogue.get` result into the same node canvas as the
Material Graph. Its palette includes compiler-registered nodes and typed sample nodes generated
from data-interface fields. The panel can create a system and emitters, then select each emitter's
Spawn, Initialise, Update, Event, Render, or Compute stage. Stage selection snapshots the shared
canvas and restores the selected stage; graph-tab switches preserve the active draft. The
renderer and CPU/GPU selectors come from `vfx.authoring-capabilities.get`; missing Decal, Light,
and Volume compositors appear with engine-provided reasons. GPU authoring is available while
runtime readiness waits for an attached preview device. The
**Save VFX draft** action writes a versioned `.cyvfxdoc` through the `vfx.document.save` command,
so it participates in scene-document undo/redo and can be reopened through
`vfx.document.read`. A scene document must be active for save history. This source is editable
authoring data; engine canonicalisation, runtime cooking, and runtime preview are tracked by
`openspec/changes/implement-issue-15-graph-authoring/`.
The command palette, scripts, and MCP also expose `vfx.emitter.add`, `vfx.emitter.remove`,
`vfx.emitter.configure`, `vfx.interface.bind`, `vfx.interface.unbind`, `vfx.node.add`,
`vfx.node.move`, `vfx.node.connect`, `vfx.node.disconnect`, `vfx.node.remove`,
`vfx.node.property.set`, and
`vfx.parameter.set`. Each reads the saved system,
applies one edit, and saves through the same undoable document transaction. Node placement,
connections, and property changes require the live engine VFX catalogue; an unavailable catalogue
or unknown node, pin, or property is refused by name.
The panel's **Remove emitter** control retains the other emitters' unsaved stage graphs and selects
the next available emitter; saving then records that removal in document history.
`vfx.emitter.capacity.set`, `vfx.attribute.set` / `vfx.attribute.remove`, and
`vfx.channel.set` / `vfx.channel.remove` provide the panel's particle storage and bounded event
declarations through MCP with the same save and undo history. Invalid bounds or attribute types
leave the saved document intact.
`vfx.parameter.remove` removes a saved system parameter by name.
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
version 3 payloads. Version 2 drafts also reopen. Version 3 maps module names to explicit
project-relative `.cyvfxmodule` paths. The `.cyvfxdoc` text envelope remains version 1.
The VFX panel exposes those declarations in collapsible sections: typed system parameters with
runtime exposure, per-emitter capacity and particle attributes with precision controls, and event
channels with event/depth limits and optional CPU readback. Invalid metadata edits leave the open
draft intact. **Save VFX draft** records the resulting document through the project transaction
command. Undo and redo of that command now refresh the open VFX document and stage canvas as well
as the project file; undoing its creation closes the open draft until redo restores it. Direct
history for every unsaved edit remains an OpenSpec task. A separately saved `.cyvfxmodule`
records one compatible stage, named typed inputs, dependency names, and a shared-canvas graph.
`vfx.module.save` and `vfx.module.read` use the command registry shared with MCP; save requires an
active scene document and supports undo/redo. The sample project includes
`effects/shared_drag.cyvfxmodule`. The VFX panel can create or open a module on the shared canvas,
edit its compatible stage, typed host inputs and dependencies, and save it through
`vfx.module.save`. It can attach a saved module to an emitter through an undoable document save;
the attachment records its explicit project path. Module saves and attachments refresh when the
scene history is undone or redone. Unsaved graph edits do not yet have individual history entries.
Opening or creating another module keeps an unsaved draft in place; **Discard module edits**
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

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
| 3 | `cy-editor-viewport-transport` | The engine's image across the process boundary: dma-buf import, timeline semaphores, the ring. **Linux only, and the only crate besides the render crate that may name a graphics API** |
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

**The agent interface is a projection and a wire, and they are separate crates on purpose.**
`cy-editor-agent` holds what an agent can see and do — the tools, which are the registry; the read
surface, which is the services; the session, its scope, its budget and its claims. It names no
protocol and has no socket. `cy-editor-mcp` holds the protocol and nothing else, behind
`cy_editor_agent::AgentTransport`, which is what `editor-agent-interface` means by "no MCP type SHALL
appear in the editor's command, document, or view-model layers": the dependency direction makes it
impossible rather than discouraged, and `crates/cy-editor-app/tests/gating.rs` checks that the
transport stays optional, stays on by default, and stays named by nothing but the binary.

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
just run-editor --open <asset> --script <path>
just run-editor-runtime [<socket>] the hosted-runtime stub `--host` connects to
```

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
| `Hosted` | A separate process or a remote device | **The default.** Complete against `cy-runtime-stub`; the engine's own hosted runtime arrives with `live-editing` at task 5.2 |

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
yet). Until a renderer does, the reference is authoring data that round-trips through `.cyworld`.

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
  identity the source holds, what the cache did, and every sub-asset with the identity bound to it.
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

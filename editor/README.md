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

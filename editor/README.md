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
every crate but the SDK carries `#![forbid(unsafe_code)]`, and nothing outside the SDK names a C
type.

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
| 4 | `cy-editor-viewmodels` | Presentation state derived from services. Never a second source of truth |
| 5 | `cy-editor-app` | The `cyberdyne-editor` binary, and the workspace's own structural checks |

`ui`, `inspector`, `assets`, `viewport` and the domain editors named by `editor-rust-application`
arrive with tasks 4.x and 5.x. Their layer positions are already decided by the rule above.

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

## No third-party dependencies

The workspace depends on nothing but the Rust standard library, and `[workspace.dependencies]` is
empty rather than absent so that adding the first one is a visible edit in a reviewed file.
`cargo build --offline` works in a fresh checkout.

That is a decision about *when*, not about *whether*. The interface toolkit is specified to be "an
implementation choice behind editor abstractions, selected on measurement", and the crates delivered
here are precisely the ones that must be testable with no window, no graphics device and no toolkit.
Choosing one now would put its types in the layer that is specified never to see them.

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
applies. That belongs to task 4.1 and should be measured before panels are built on it.

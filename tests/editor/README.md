# tests/editor/ — the editor's headless tests, as CTest entries. Task 6.3.

The editor is a Rust workspace, so **most** of its tests are Cargo's: `cargo test` over
`editor/crates/*`, which is what `just build-editor-check` runs alongside rustfmt and clippy, and
what the `editor` gate in `tools/roadmap/gates.toml` and the `authorable` job in
`.github/workflows/ci.yml` run on every pull request. Those tests need no window, no graphics device
and no toolkit, which is a property of the crates rather than of the runner — `editor-rust-application`
requires it and `cy-editor-app`'s own suites hold it.

**What lives here is the part Cargo cannot express**: the tests whose subject is the editor as a
*process*, beside a runtime that is another process and an importer that is a third. `cargo test`
can start a child process — `cy-editor-app`'s `survives_a_runtime_crash.rs` does exactly that, and
it is the library-level version of the same claim — but it cannot reach the engine's own binaries,
because the editor workspace does not and must not know they exist. The artefact does, and this
directory is where it is registered.

## The entries

| CTest name | what it runs |
|------------|--------------|
| `unit.editor_documents` | **a node has a name**, on the ENGINE's side of the format and the operation stream. M11.b task 4.1 |
| `smoke.editor_session` | the whole M5 artefact: `samples/05-editor-session/session.py` |
| `integration.editor_session_selftest` | the driver's own negative cases, which require its two load-bearing assertions to fail when they should |

`unit.editor_documents` is declared from the `CMakeLists.txt` beside this file, which
`tests/CMakeLists.txt` now adds. **The other two are still declared from
`samples/05-editor-session/CMakeLists.txt`** and were deliberately left there: the change that
landed them is archived, moving a test declaration risks a suite that silently stops being
registered, and M11.b's business was the suite above. `samples/04-character` uses the same
arrangement for `smoke.character_sample`, for a different reason — ordering — and its comment argues
the shape.

### Why a C++ suite about the EDITOR's document model

Most of the editor's tests are Cargo's, and the Rust half of "a node has a name" is there —
`cy_editor_documents::{content,document,transaction}`, `cy_editor_services::worldfile`,
`cy_editor_viewmodels::hierarchy`. What cannot be written there is the half this suite is about: the
**engine** reads the same `.cyworld` and applies the same operation stream, so a name that existed
only in the editor would be a name the runtime drops on the first save. That is the shape of the
defect `worldfile.h`'s header was written to close for *identity*, and a name would have repeated
it. Two readers of one format need a test on each side, or they agree until one of them changes.

### Where the binary lands, because it looks like a typo

`build/<dir>/tests/editor/cy_test_unit_editor_documents`, not `build/<dir>/`. A suite declared from
under `tests/` is created in that subdirectory's scope and its binary lands beside its
`CMakeLists.txt`; a suite declared **deferred** from `src/**/tests/` is created in the top-level
scope and lands at the root. Both conventions are in this tree, `m11a.toml` already spells three
criteria the first way, and `m11b.toml`'s `a-node-has-a-name` spells this one.

## When they are not registered, and why that is not a skip

They are declared only where the artefact can actually run:

* **on a Unix host**, because the live bridge is a Unix domain socket and `cy-runtime-stub` refuses
  to pretend otherwise;
* **when `cargo` and `just` are both on the path**, because the session runs the real editor binary
  and builds it if it is stale.

Where one of those is missing the entries do not exist, the configure says so in one line, and the
test list is visibly shorter — which is this tree's position throughout (`samples/04-character`
without a Swift toolchain, `tests/render/` without a device). A registered test that skipped would
report green on exactly the machines least able to judge the claim.

## The cost, measured

`smoke.editor_session` runs `just build-editor` first, which is a Cargo build of a workspace with no
third-party dependencies: **3.4 s cold and about 0.2 s warm** on the development profile. It does
not build the engine — the importer arrives as a CMake target dependency instead, because running
`just build-tools` from inside a CTest run would reconfigure the tree ctest is reading its own test
list out of.

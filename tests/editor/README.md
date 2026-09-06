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
| `smoke.editor_session` | the whole M5 artefact: `samples/05-editor-session/session.py` |
| `integration.editor_session_selftest` | the driver's own negative cases, which require its two load-bearing assertions to fail when they should |

Both are declared from `samples/05-editor-session/CMakeLists.txt` rather than from a `CMakeLists.txt`
of their own added by `tests/CMakeLists.txt`, and the reason is ownership rather than taste: the
change that landed this directory does not own `tests/CMakeLists.txt`. `samples/04-character` uses
the same arrangement for `smoke.character_sample`, for a different reason — ordering — and its
comment argues the shape. Whoever next edits `tests/CMakeLists.txt` should add
`add_subdirectory(editor)` and move the two declarations into a `CMakeLists.txt` here; it is a
lateral move and nothing else changes.

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

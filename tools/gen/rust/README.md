# `tools/gen/rust/` — the editor's Rust SDK generator

Produces `editor/crates/cy-editor-sdk/src/generated/` from the ABI description that
`tools/abi/abi_describe.py` computes and `tools/abi/abi_gate.py` diffs against
`src/abi/abi_baseline.json`.

```
just build-editor --generate                   write
python3 tools/gen/rust/sdk_gen.py --check      fail if the committed bindings are stale
python3 tools/gen/rust/tests/run_tests.py      the selftest
```

`cargo test -p cy-editor-sdk` runs the `--check` above, so the currency of the bindings is a test
failure rather than something a developer has to remember. `just build-editor-check` runs all three.

There is exactly one reader of `cy_abi.h` in this repository and it is `abi_describe.py`. This
package consumes its output and never opens the header itself — so a change to the ABI reaches the
gate, the committed baseline, the Swift overlay and the Rust SDK through one parse rather than four.

## The pieces

| File | What it owns |
|---|---|
| `sdk/rusttypes.py` | C types to Rust. Total: it produces a spelling or raises. |
| `sdk/entries.py` | The one hand-written table, and the check that makes it safe. |
| `sdk/emit.py` | The Rust each generated file contains. |
| `sdk/cli.py` | Write and `--check`, through one file set built once. |

## The one hand-written table, and why it cannot drift

`abi_describe.py` strips parameter names, deliberately: a name is not part of a C ABI, and a baseline
that diffed on one would fail a review that renamed `entity` to `target`. But a generated Rust API
whose parameters read `a0`, `a1`, `a2` is one nobody will call, and one whose doc comment cannot name
what it takes is one no agent can call either.

So the names live in `entries.py`, hand-written, beside the one other thing a C declaration does not
carry: whether an entry returning `CyResult` reports a **failure** or answers a **question**
(`get_last_error_code` returns a status and never fails).

`entries.validate()` refuses a description with an entry that has no record, refuses a record naming
an entry that is not in the description, refuses a record whose parameter count differs from the
signature's arity, and refuses one with no documentation line. Appending an entry to `CyInterface`
therefore *stops generation* until somebody names its parameters — the same shape as the ABI gate:
the change is legal, and it is not silent.

This is a second copy of `tools/gen/swift/overlay/entries.py`. The duplication is deliberate and it
is checked: both tables are validated against the same description, so neither can drift away from
the ABI. Sharing them would couple two generators through a module that belongs to neither, and
would put Swift's argument labels — part of Swift's API and not of Rust's — into this generator's
vocabulary.

## What is generated, and what is deliberately not

Generated: the version constants, the enums, the `#[repr(C)]` mirrors of every ABI struct and union,
the interface table, one typed raw call per table entry, and the layout assertions.

**Not generated: the safe API.** That a null `CyWorld` means "no world is bound", that a
`const char*` the engine returned is UTF-8 borrowed for the engine's lifetime, that a `CyBorrow` must
be re-validated against the world's epoch before every read — these are semantic decisions the C
declarations do not contain. They are hand-written in `cy-editor-sdk`, in one audited module, and
each says which sentence of `cy_abi.h` it implements. A generator that invented them would be a
second, unchecked copy of the ABI, which is the failure the generated half exists to prevent.

**Not bound by bindgen**, for three reasons: it would need libclang on every machine that builds the
editor, it would produce a different file for each host it ran on, and it would put a second reader
of `cy_abi.h` in the repository.

## Why the layout assertions are `const` and not tests

`editor-rust-application` requires that when the ABI changes, "the SDK SHALL be regenerated or
updated and the mismatch SHALL be a **build failure**". A test would report the mismatch *after*
producing a binary that had already read the struct wrongly. `const _: () = assert!(...)` fails the
compile. `src/abi/tests/test_layout.cpp` asserts the same numbers against the C compiler, so the
description is checked from both sides of the boundary it describes.

## The generator runs `rustfmt` over its own output, and why

`rustfmt` formats a crate by walking its module tree from the crate root, and a stable toolchain has
no way to tell it to skip a module — `--skip-children` is not a recognised option. So a repository
that both formats the editor and checks that the generated bindings are current has two gates that
disagree by construction: the formatter rewrites the generated files, and the currency check then
reports them as stale. Both gates are worth having, so the generator writes what the formatter would
produce and the two agree because there is one pipeline.

The cost, stated: generation depends on `rustfmt` and on its version. A different `rustfmt` produces
different bytes, the currency check reports the bindings as stale, and the diff says so — the same
shape as the clang-format pin `justfile` documents at length, and the same remedy. **This repository
does not yet pin a Rust toolchain version**; it pins LLVM 22.1.8 for exactly this reason and should
do the same for Rust.

## The selftest has no fixture directory

Every case edits the **live** header in memory and runs the real generator over the result, exactly
as `tools/abi/selftest.py` does and for the same reason: a committed "broken header" fixture goes
stale, and if the parser ever stopped recognising the table, a hand-written broken fixture and a
hand-written correct one would both describe nothing — and comparing nothing to nothing succeeds.

Case 0 is the control: the unedited header generates exactly what is committed. It is what makes the
other sixteen mean anything.

**Governed by**: `editor-rust-application` (Editor SDK boundary), `native-abi`,
`build-system-and-platforms` (code generation).

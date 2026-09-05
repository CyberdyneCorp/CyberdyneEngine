# `tools/gen/swift/` — the Swift overlay generator

Produces `bindings/swift/Sources/CyberdyneCore/Generated/`, the C header copy in
`Sources/CyberdyneABI/`, and the generated layout suite, from the ABI description that
`tools/abi/abi_describe.py` computes and `tools/abi/abi_gate.py` diffs against
`src/abi/abi_baseline.json`.

```
just generate-swift              write
just generate-swift --check      fail if the committed overlay is stale, printing the diff
python3 tools/gen/swift/tests/run_tests.py     the selftest
```

There is exactly one reader of `cy_abi.h` in this repository and it is `abi_describe.py`. This
package consumes its output and never opens the header itself, except to copy it verbatim into the
package's C target — so a change to the ABI reaches the gate, the baseline and the overlay through
one parse rather than three.

## The pieces

| File | What it owns |
|---|---|
| `cnames.py` | C spellings to Swift spellings. Total: it produces a name or raises. |
| `swifttypes.py` | C types to the Swift the importer presents. Raises on a type it has no rule for. |
| `entries.py` | The one hand-written table, and the check that makes it safe. |
| `emit.py` | The Swift each generated file contains. |
| `cli.py` | Write and `--check`, through one file set built once. |

## The one hand-written table, and why it cannot drift

`abi_describe.py` strips parameter names, deliberately: a name is not part of a C ABI, and a baseline
that diffed on one would fail a review that renamed `entity` to `target`. Swift has argument labels,
though, and a generated API whose labels read `_ a0:` is one nobody will call. So the labels are in
`entries.py`, hand-written, beside the one other thing a C declaration does not carry — whether an
entry returning `CyResult` is reporting a *failure* (the wrapper throws) or answering a *question*
(`get_last_error_code` returns a status and never fails).

`entries.validate()` refuses a description with an entry that has no record here, refuses a record
naming an entry that is not in the description, and refuses a record whose label count differs from
the signature's arity. Appending an entry to `CyInterface` therefore *stops generation* until
somebody names its parameters — the same shape as the ABI gate itself: the change is legal, and it is
not silent.

Nullability is **not** in that table. A returned pointer is Optional in Swift, always, because C
cannot say otherwise and a generator that trusted a header comment would trap on the day the comment
was wrong.

## What is generated, and what is deliberately not

Generated: the enums, the vector types, `Interface` (one method per table entry), the handle
wrappers, the C header copy, the module map, and the layout suite.

**Not generated: Swift mirrors of the C structs.** The C importer already produces them, and its
`CyVar` is the same thirty-two bytes the engine writes *by construction*. A generated `struct Var`
beside it would be a hand-modelled copy of an exact layout — one more declaration that can drift,
which is the failure this generator exists to prevent. The types that genuinely have no C counterpart
are the vectors, which the ABI carries as four floats inside a union; those *are* generated, and
their layout is asserted.

## Which entries land on which wrapper is derived, not listed

An entry whose first parameter is a `CyWorld` becomes a method on `World`; one whose first parameter
is not a handle stays on `Interface`. So appending a `world_*` entry puts a method on `World` with no
edit here. The rule strips the receiver's word from the method name — `world_create_entity` reads
`world.createEntity()` — and a collision produced by that stripping is an error naming both entries,
because a generated API with two methods of one name is either a compile failure or a silent
replacement.

## The selftest has no fixture directory

Every case edits the **live** header in memory and runs the real generator over the result, exactly
as `tools/abi/selftest.py` does and for the same reason: a committed "broken header" fixture goes
stale, and if the parser ever stopped recognising the table, a hand-written broken fixture and a
hand-written correct one would both describe nothing — and comparing nothing to nothing succeeds.

Case 0 is the control: the unedited header generates exactly what is committed. It is what makes the
other twelve mean anything.

**Governed by**: `swift-scripting` (generated overlay), `native-abi`, `build-system-and-platforms`
(code generation).

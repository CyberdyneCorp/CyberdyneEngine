# `src/abi/` — layer 6

The stable flat C ABI exported to scripting languages and extensions. Symbols are `cy_*`; the ABI is
the boundary at which C++ types stop.

**Governed by**: `native-abi`. Landed at M4.

## The one exported symbol

```c
const CyInterface* cy_get_interface(uint32_t requested_major, uint32_t requested_minor);
```

Everything else is reached through the returned table. A module is a shared library exporting
`cy_module_entry`, described by a `module.toml`; the loader is `cy/abi/module.h`.

## What is here

| | |
|---|---|
| `include/cy/abi/cy_abi.h` | **The ABI.** Pure C. Opaque handles, POD structs, function-pointer table, `CyResult` codes. The only file a Swift, Rust or C consumer needs. |
| `include/cy/abi/errors.h` | `cy::Expected<T, Error>` → `(CyResult, out-parameter)`, and the thread-local last error |
| `include/cy/abi/host.h` | What is behind `CyEngine`, `CyWorld` and `CyBehaviourType` |
| `include/cy/abi/var.h` | `CyVar` marshalling: the reference-counted heap payload and the inline constructors |
| `include/cy/abi/module.h` | Manifests, images, **generations**, and the reload sequence |
| `abi_baseline.json` | The committed machine-readable description. The gate diffs against it; the Swift overlay and the Rust SDK are generated from it. |
| `abi_approvals.toml` | Reviewed, recorded exceptions to the gate. Empty, and meant to stay that way. |

## The compatibility gate

```
just quality-abi              # the header against abi_baseline.json
just quality-abi --selftest   # prove the gate still refuses a reorder, a removal and a break
just quality-abi --update     # accept the current header as the new baseline
```

`native-abi` requires the description to be diffed in CI and any non-append to fail. design.md §1
fixes *when*: **with the first exported symbol, not the first consumer** — the obligation starts the
moment anything links the table, and by M5 that is the editor.

Registered as `integration.abi_baseline` (the header matches the baseline) and `integration.abi_gate`
(the gate still fails when it should), so `just test-all` runs both.

### Adding an entry

1. Append it below the marker comment at the end of `CyInterface`. **Never above it, never
   between.**
2. Increment `CY_ABI_MINOR`.
3. Add the thunk at the corresponding position in `src/interface.cpp`.
4. `just quality-abi --update`, and commit the baseline with the change.

Anything else — a reorder, a removal, a changed signature, an inserted struct member, a changed enum
value — is refused, naming the entry and printing the approval stanza that would record the break.
The whole three-way demonstration (reorder stops, removal stops, append passes) is
`tools/abi/selftest.py`, run over the live header rather than over a copied fixture.

## Layout is computed, and then checked

`tools/abi/abi_describe.py` derives every struct's size and offsets from the declarations, under the
layout model the C ABI fixes. It does not compile anything: a description produced by compiling
would be a description of one toolchain on one machine, and the baseline is diffed across the whole
matrix.

That is a claim, so `tests/test_layout.cpp` asserts the compiler's `sizeof` and `offsetof` against
exactly those numbers. If the model is ever wrong on a platform, that test fails there rather than
the overlays being generated against a struct that does not exist.

## Hot reload: the image is retired, not unloaded

The reload sequence is in `include/cy/abi/module.h`, and it does everything that can fail before
anything that cannot be undone:

```
open the new image → serialize every instance through its own vtable → open the next generation and
run the new entry point → check every live type and schema → (point of no return) destroy and shut
down → recreate and restore by name
```

**There is no `dlclose`.** M4's spike measured that unloading a Swift image is unsafe whenever the
Swift runtime outlives the module — the foreign-type-metadata cache and the protocol-conformance
section list both keep pointers into the unloaded image, and the next image is mapped over the same
addresses. The amendment to `native-abi`'s "Hot reload" requirement carries the measurements.

The cost is stated rather than hidden: **58-85 kB of address space per reload, never reclaimed**, and
0.1-0.6 ms per reload, flat across 40 generations. A thousand reloads is under 90 MB. The mitigation,
if it is ever needed, is a process restart.

Every instance carries the generation that created it and is called through **that** generation's
vtable. Version-2 code run against a version-1 object reported `health = 17` and `mana = 3.5e18` with
no trap and no diagnostic; that is the failure this rule exists for.

## Not behind an option

`src/abi/` is compiled in every build and every profile. M3's gate found the shape of the mistake to
avoid: `CY_RENDERER_VULKAN` defaulted off, so the real backend was the one nothing tested. A table
that exists only in some configurations is a table whose gate runs only in some configurations.

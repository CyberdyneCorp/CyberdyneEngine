# `src/core/values/` — the vocabulary types

Layer 0, target `cy::core-values`. Section 1.3 of M1. Governed by `core-type-system`.

These are the types the scripting boundary, the serializer, the editor inspector and the C ABI all
speak. Their shape outlives this milestone: M4 generates an ABI against them and M5 generates an
editor against them, so a change here is a change to every artefact produced from here on.

| Header | What it is |
|---|---|
| `name.h` | `Name` — the interned, immutable string. O(1) comparison, one 32-bit word, thread-safe interning. `CY_NAME("literal")` interns once at its declaration site. |
| `handle.h` | `Handle<Tag>` — a 32-bit slot and a 32-bit generation in 64 bits; `AnyHandle` when the tag has to be data; `EntityId`; and `GenerationTable`, which owns the counters that make a stale handle **detectable**. |
| `asset_id.h` | `AssetId` — 128 bits of content identity, structurally not a handle. |
| `payload.h` | The math shapes a `Var` carries, and `var_payload_cast` — the seam an engine math type crosses through. |
| `var.h` | `Var` — the tagged dynamic value. 24 bytes; small kinds inline, the rest in a reference-counted block with copy-on-write. Coercion is explicit and reports narrowing. |
| `callable.h` | `Callable` — a free function, a method bound to a handle, a script function, or a bound-argument wrapper, all one type. Invocation returns `Expected<Var, CallError>`. |
| `event.h` | `EventChannel<T>` — the per-frame double-buffered queue systems talk through. |
| `signal.h` | `Signal` and `SignalQueue` — the named emitters the authoring and scripting layer connects to. |
| `diagnostics.h` | What the module reports about itself, onto the M0 trace. |

## The three decisions worth knowing

**Identity is detectable, not merely defined.** A handle's generation is odd while its slot is live
and even while it is free, and it only ever increases. A slot freed and reallocated therefore hands
out a generation no previous handle carried, so a handle held across the free is rejected rather
than aliasing the new occupant. `GenerationTable` counts those rejections;
`values_diagnostics().stale_handle_rejections` is where an ownership bug upstream shows up.

**Asset ids are not handles, and the compiler says so.** Different widths, no conversion in either
direction, no shared base, no mixed comparison. `tests/compile_fail/` compiles five programs that
try the confusion and requires every one of them to fail.

**Reflection is control plane; this module is not.** A handle validation, a `Name` comparison and a
`Var` copy are all on paths that run at frame rate, so none of them emits a trace record. They bump
a relaxed atomic, and `values_trace_report()` — called at a frame boundary or a shutdown — is what
puts the figures on the trace.

## What is deliberately not here

- **A handle pool.** Slots that hold *objects* are `core-memory-and-containers` task 2.5. This
  module owns the identity half: indices, counters and the validity test.
- **Math types.** `core-math` owns `Vec3` and the rest, with their operators and SIMD paths.
  `payload.h` declares only the boundary shapes and the cast between them.
- **The reflection registry.** `core-reflect` owns `TypeId`, `FieldId` and the identity manifest.
  A `Name` is metadata; it is never a persistent identifier.
- **A pooled allocator for `Var`'s blocks.** They are `new`ed today; `core-memory-and-containers`
  is where the pool comes from, and `var.cpp`'s allocation helpers are the one place it plugs in.

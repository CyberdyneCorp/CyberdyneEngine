# Design: M6 — Scale

## 1. The spike, and its one criterion — the derivation key model

The roadmap names this spike and states the failure: *"If keys are not precise, the cache is either
wrong or useless, and every later milestone builds on top of it."*

A derivation key identifies the output of a build step by everything that can change it. Two failure
modes, and they are not symmetric:

- **A key too coarse is a correctness bug** — two different inputs collide on one key, and the cache
  serves the wrong artefact. This is the one that must be impossible, because it is discovered as a
  mysterious wrong result in a downstream milestone rather than as a build error.
- **A key too fine is a performance bug** — the cache misses on changes that could not have affected
  the output. Recoverable, but it makes the cache worthless, which makes the graph pointless.

**The spike's criterion**: a one-asset change invalidates exactly the derivations that depend on it,
and a cold build and a cache-warm build produce **byte-identical** artefacts. Both are M6 exit
criteria, so the spike is testing the milestone's own gate rather than a proxy for it.

What the spike must settle before the graph is built:
- what belongs in a key — tool versions, cook profile, options, input content hashes, and the
  transitive closure of those, and what deliberately does not;
- whether the compiler and its flags are inputs (they are: M0 through M5 pinned LLVM 22.1.8 and the
  Rust toolchain for exactly this reason, and a build that does not treat them as inputs is lying);
- how a key survives a path move, since absolute paths in a key make a cache machine-local;
- and whether non-determinism in any existing cook step defeats byte-identity before the graph is
  written on top of it.

Run the spike outside the repository, as M3's and M5.5's were — `~/cyberdyne-spikes/m6-keys-spike/`.
A prototype in `docs/` fails `quality-layers`, correctly, and the two previous spikes were more
useful for being disposable.

## 2. Worlds load, and the editor is the first consumer

The order matters. `serialization-and-prefabs` reaching Complete is what gives a world a schema and
nodes; `world-partition-and-streaming` is what makes that world larger than memory. The editor is
downstream of both and should be wired **as soon as opening a world produces content**, not at the
end of the milestone — because the editor is the fastest way to see that streaming, activation and
the persistence overlay are wrong.

Concretely: `DocumentService::open` must produce a document whose schema declares the engine's
registered component types, so `TransformBinding::of_schema` finds a `Transform`. Everything M5.5
built downstream of that — selection, the gizmo's four modes, per-axis entry, one transaction per
manipulation — is already tested against documents that declare one. This is a connection, not new
interaction work.

## 3. Residency and activation are separate, and a test must prove it

`residency` is "shared policy with separate storage". The requirement that keeps it honest is the
M6 exit criterion: **a test holds bytes resident with simulation off.** If residency and activation
cannot be separated, streaming becomes an all-or-nothing operation and the frame budget goes with it.
Design the policy so that being resident and being active are two independent facts about a cell from
the first commit; retrofitting that separation after HLOD and the persistence overlay depend on it is
the kind of change that is a migration rather than an edit.

## 4. The save is the overlay

`save-and-persistence` does not add a serialisation path beside the streaming one. The persistence
overlay that world partition already maintains **is** the save: scopes and traits decide what
persists, persistent identity survives a cell unloading, and atomic generations make a save that is
interrupted by `kill -9` either fully applied or absent. The exit criterion is exactly that — a
save/load round-trip of an *unloaded* region's state, and atomic generations under `kill -9`.

## 5. What must not be retrofitted

Pinned to this milestone because they are cheap now and are migrations later:

| Invariant | Why it cannot wait |
|---|---|
| Derivation keys include the toolchain | A cache that ignores the compiler serves artefacts built by a different one |
| Cells are cooked in ECS-native form | Cooking to an intermediate and converting at load makes streaming cost proportional to content |
| Residency separate from activation | Every later system that streams assumes it |
| Persistent identity is stable across unload | A save written before this is a save that cannot be migrated |
| Immutable artefacts | A mutable artefact makes every downstream key a lie |

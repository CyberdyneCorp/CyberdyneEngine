# Implement M1 — Substrate: the services everything else is written against

## Why

M0 made the tree buildable. M1 makes it possible to write engine code at all: every capability from
M2 onward is expressed in terms of reflected types, pooled memory, engine math, and jobs the
scheduler can parallelise. Nothing above this layer can be written honestly until these exist, which
is why the roadmap treats M0 through M2 as one unbroken sequence rather than three deliverables.

Two things make M1 the highest-leverage milestone on the ladder, and both are about what comes
after it rather than what it contains.

**Stable identity is encoded into every artefact produced from here on.** Once a scene, a prefab, a
save, an animation binding or a network schema is written, it refers to types and fields by
identifier. Assign those identifiers late and everything written in the meantime is invalid.
`core-type-system` therefore lands its manifest, its tombstones and its CI gate at Working in this
milestone, not at Complete in a later one — this is the first entry in the roadmap's
retrofit-hostile table and the one with the widest blast radius.

**Systems written without access declarations cannot be parallelised afterwards.** The scheduler
parallelises from what a system declares it reads, writes and excludes. A system written before that
contract exists does not merely lack the declaration — it is written in a shape that may not admit
one. Every system in M2 through M11 is written against the job system this milestone builds, so the
declaration model has to be right before the first system exists rather than after the fiftieth.

The third reason is quieter and it is why the spike goes first: **every contributor pays the
reflection generator's cost on every build, forever.** If regeneration is slow or non-reproducible,
that tax compounds silently across ten more milestones and is never attributed to the decision that
caused it.

## What Changes

Five workstreams, one milestone gate. `tasks.md` has the ordered plan; `design.md` records what the
specifications leave open.

- **Reflection and identity.** The opt-in registry, the generator over an established compiler
  frontend, field attributes, the **committed identity manifest with tombstones and its CI gate**,
  and the rule that reflection is control plane and never hot path.
- **Values.** `Var`, generational handles, asset ids as distinct from handles, events and signals,
  `Callable`, string interning — the vocabulary types the ABI and the editor will both need.
- **Memory.** The allocator interface, memory domains and the budget tree, pressure levels,
  allocator propagation, sequence and associative containers, handle pools, **chunked component
  storage**, scratch and frame memory, retirement and frame epochs.
- **Math.** Types and SIMD, and — as executable tests rather than prose — the coordinate, depth and
  unit conventions: right-handed, Y-up, −Z forward, reversed-Z with a `[0,1]` range, column-major,
  metres and radians. Plus BVH, frustum primitives, curves and seeded RNG.
- **Jobs.** One job system owning all worker threads, thread roles, **access declarations and the
  safety-by-construction property that follows from them**, coroutines as the asynchronous model,
  cancellation, priorities and deadlines, chunked long-running work, and workers that never block on
  I/O or the GPU.
- **Assets and I/O at Seed.** Asset identity, the virtual filesystem, the package format, async
  loading, compression — enough for the closing artefact to load a package, with cooking and
  streaming left to M2 and M6.
- **The project graph.** M0 seeded `project-and-plugins` on its module half only; M1 adds the
  project manifest that "the project graph is authoritative" actually refers to, plus layered typed
  configuration.

**Closing artefact**: `samples/01-headless-host` — loads a package from the virtual filesystem, runs
a parallel job graph over reflected data, reports its memory budget tree, and shuts down
deterministically. Exit criteria are executable as `just roadmap-milestone m1`.

## Capabilities

### Advanced Capabilities

`core-type-system`, `core-memory-and-containers`, `core-math`, `core-jobs-and-concurrency` and
`project-and-plugins` to **Working**; `core-assets-and-io` and `engine-architecture` to **Seed**.

### Modified Capabilities

Both corrections come from what M0's gate actually found, rather than from anticipation.

- `diagnostics-profiling-and-crash` — **a source location is classified data, not a name.** M0
  shipped log sites built from `__FILE__` and registered as event *names*, which put them
  structurally beyond the writer's redaction; a build-path prefix reached committed artefacts and
  was only caught by an adversarial read. The compiler flag that fixed it has no MSVC equivalent, so
  the flag is a mitigation and not the mechanism. The requirement now states that any field carrying
  a filesystem path — including one the compiler injects — is a classified field.
- `testing-and-quality` — **the sanitizer gate names its suites, and intentional lifetime
  allocations are declared.** M0 wired ASan, UBSan and TSan and proved each catches its class of
  bug, but no job runs a suite under them, and LeakSanitizer reports the trace's process-lifetime
  thread rings, which are pooled by design. A subsystem that intentionally holds an allocation for
  process lifetime must declare it where the sanitizer can see the declaration.

## Impact

- **New code**: `src/core/reflect/`, `src/core/values/`, `src/core/memory/`, `src/core/math/`,
  `src/core/jobs/`, `src/core/assets/`, `src/core/config/`, `tools/gen/`, `tools/project/`, and
  `samples/01-headless-host/`. All at layer `core` except the tools.
- **New dependencies**: an established compiler frontend for the reflection generator — the
  specification requires parsing be integrated rather than written — and a general-purpose allocator
  chosen by measurement, or the platform's, if measurement does not justify one. Both pinned in
  `deps/manifest.toml` behind engine-owned interfaces.
- **New permanent gates**: the identity manifest check, reflection round-trip goldens,
  generated-code reproducibility, the job system throughput benchmark, and TSan over the job suite.
- **Carried forward from M0**: the sanitizer CI job and the LeakSanitizer decision, which M1 must
  settle because the job system makes ASan and TSan non-optional.
- **Risk**: concentrated almost entirely in the generator's incrementality and in whether the access
  declaration model can express what M2's systems will need. Both are spiked before the milestone
  commits.

# Tasks: M1 — Substrate

Ordered. The spike first, then reflection and identity because everything else registers against it,
then the three independent workstreams, then assets and the project graph, then the artefact.

Sections 2 and 3 depend on section 1. Sections 3.1, 3.2 and 3.3 are independent of one another.

## 0. Spike — reflection generator incrementality

M1's named risk. Every capability after this one pays the generator's cost on every build.
Its only deliverable is a decision.

- [x] 0.1 Prototype the libclang-based generator over a synthetic annotated type set an order of
      magnitude larger than M1's own
- [x] 0.2 Measure cold and incremental regeneration; state both numbers
- [x] 0.3 Prove output is byte-reproducible across runs and across build directories
- [x] 0.4 If incremental regeneration is not fast enough to disappear into a normal build, propose
      the approach change **before** section 1 proceeds — do not accept the cost

## 1. Reflection and identity — `core-type-system` → Working

### 1.1 Registry and generator

- [x] 1.1.1 Opt-in reflection registry: types register explicitly, nothing is reflected by accident
- [x] 1.1.2 The generator over libclang, emitting engine-owned C++; the identity model, metadata
      format and output stay ours
- [x] 1.1.3 Field attributes as the specification lists them
- [x] 1.1.4 **Reflection is control plane, not hot path** — prove it: no reflected lookup on any
      per-entity or per-frame path, checked rather than asserted
- [x] 1.1.5 `just generate-headers` regenerates; `just generate-check` fails on stale output

### 1.2 Stable identity — **invariant, M1**

- [x] 1.2.1 Identifiers are opaque numbers assigned once, never derived from names, hashes or
      indices (`design.md` §1)
- [x] 1.2.2 The committed manifest; the generator only ever appends
- [x] 1.2.3 Tombstones on removal; a number is never reused
- [x] 1.2.4 **The CI gate**: a renamed field with no tombstone fails the build, naming the field,
      its identifier and the fix. Prove it by renaming a field.
- [x] 1.2.5 Round-trip golden tests over reflected data
- [x] 1.2.6 `just quality-identity` and its CI job

### 1.3 Values

- [x] 1.3.1 `Var` — the dynamic value type
- [x] 1.3.2 Generational handles, and asset ids as a distinct type from handles
- [x] 1.3.3 Events and signals
- [x] 1.3.4 `Callable`
- [x] 1.3.5 String interning
- [x] 1.3.6 Type-system diagnostics on the M0 trace

## 2. Memory — `core-memory-and-containers` → Working

- [x] 2.1 Allocator interface and allocator propagation
- [x] 2.2 Memory domains and the **budget tree**
- [x] 2.3 Pressure levels, and the response a subsystem must implement
- [x] 2.4 Sequence and associative containers over the allocator interface
- [x] 2.5 Handle pools
- [x] 2.6 **Chunked storage** — allocator, layout and iteration, with no knowledge of components or
      archetypes (`design.md` §7). M2's ECS is the first consumer.
- [x] 2.7 Scratch and frame memory
- [x] 2.8 Retirement and frame epochs
- [x] 2.9 Virtual address reservation; reference-counted shared data; ownership conventions
- [x] 2.10 The general heap decided **by measurement** on the engine's own allocation pattern, with
      the numbers recorded (`design.md` §6)
- [x] 2.11 Memory diagnostics: the budget tree reports, and an over-budget domain raises pressure
- [x] 2.12 Declare the trace's process-lifetime rings to the leak detector — carried from M0

## 3. The three independent workstreams

### 3.1 Math — `core-math` → Working

- [x] 3.1.1 Math types
- [x] 3.1.2 **Conventions as executable tests** (`design.md` §4): right-handed, Y-up, −Z forward;
      reversed-Z with `[0,1]`, cleared to 0, compared GreaterEqual; column-major with column
      vectors; metres, seconds, radians. Each asserts its numeric consequence.
- [x] 3.1.3 SIMD with an always-compiled scalar reference, and every path tested against it
      (`design.md` §5)
- [x] 3.1.4 Spatial acceleration structures; frustum culling primitives
- [x] 3.1.5 Geometry utilities; curves and easing
- [x] 3.1.6 Seeded random number generation — reproducible, and inspectable, because
      `simulation-and-determinism` will require both

### 3.2 Jobs — `core-jobs-and-concurrency` → Working

- [x] 3.2.1 One job system owning all worker threads; thread roles
- [x] 3.2.2 **Access declarations and the conflict checker** (`design.md` §3) — Read/Write/Exclude,
      rejected at registration, exercised by synthetic systems
- [x] 3.2.3 Task context
- [x] 3.2.4 Coroutines as the asynchronous model
- [x] 3.2.5 **Workers never block on I/O or the GPU** — a test that tries to, and fails
- [x] 3.2.6 Cancellation
- [x] 3.2.7 Priority classes, fairness and deadlines
- [x] 3.2.8 Long-running work is chunked
- [x] 3.2.9 Synchronisation primitives; double buffering across thread boundaries
- [x] 3.2.10 Deterministic scheduling mode and deterministic parallel primitives — seeded here
      because M9 can only validate what M1 made possible
- [x] 3.2.11 Concurrency diagnostics, including the critical path
- [x] 3.2.12 Throughput benchmark with a regression threshold; **TSan over the job suite in CI**

### 3.3 Assets and I/O — `core-assets-and-io` → Seed

- [x] 3.3.1 Asset identity
- [x] 3.3.2 Virtual filesystem with mount points
- [x] 3.3.3 Package format — read path only; cooking is M2, streaming is M6
- [x] 3.3.4 Asynchronous loading over the job system, never blocking a worker
- [x] 3.3.5 File and directory access; serialization formats
- [x] 3.3.6 Compression through the engine-owned interface over the pinned codec

## 4. The project graph — `project-and-plugins` → Working

M0 seeded this on its module half only. The manifest is what "the project graph is authoritative"
refers to.

- [x] 4.1 The project manifest, and the project graph built from it
- [x] 4.2 Layering enforcement extended from targets to the project graph
- [x] 4.3 Layered typed configuration
- [x] 4.4 Undeclared dependencies and cycles are errors, with a test for each
- [x] 4.5 `engine-architecture` → Seed: deterministic startup and shutdown ordering, recorded and
      asserted, over the module registration levels

## 5. The artefact

- [x] 5.1 `samples/01-headless-host` — loads a package from the virtual filesystem, runs a parallel
      job graph over reflected data, reports its memory budget tree, shuts down deterministically
- [x] 5.2 `just run-sample headless-host`
- [x] 5.3 Smoke test over the sample, asserting a clean exit and a plausible budget report
- [x] 5.4 Startup and shutdown order identical across 100 runs

## 6. Closing the milestone

- [x] 6.1 The identity manifest gate fails a renamed field with no tombstone, and passes when
      tombstoned
- [x] 6.2 Reflection round-trip goldens pass; generated code is byte-reproducible
- [x] 6.3 The job system throughput benchmark meets its threshold; TSan is clean over the job suite
- [x] 6.4 Startup and shutdown order is identical across 100 runs
- [x] 6.5 Memory budgets report; an over-budget domain raises pressure
- [x] 6.6 A layering violation between core modules fails the build
- [x] 6.7 All four profiles build clean and `just test-all` is green in each — M0's gate found this
      class of failure only because it looked outside `dev`
- [x] 6.8 The sanitizer CI job exists and is green, with intentional lifetime allocations declared
- [x] 6.9 `just roadmap-milestone m1` exits zero
- [x] 6.10 Update `docs/roadmap/status.yaml` and `capability-matrix.md`; record what is thinner than
      the tasks claim
- [x] 6.11 `openspec validate --specs --strict` passes; archive this change
- [x] 6.12 Open the M2 change — cook-time flattening is its named spike

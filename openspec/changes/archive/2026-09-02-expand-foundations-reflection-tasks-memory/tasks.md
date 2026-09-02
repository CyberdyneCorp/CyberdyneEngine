# Tasks: foundations

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis. Sections 3 onward are the implementation backlog, sequenced by
the build-order table in `design.md`.

## 1. Specification

- [x] 1.1 Record in `design.md`: why name-derived identity is a defect, the field identity
      inconsistency this closes, the tagged/cooked split, value-level migration and its classes,
      reflection as control plane, coroutines over a fiber runtime, the deadline-versus-determinism
      resolution, the task context as the tasks–memory junction, memory pressure as the missing
      half of the budget model, generalised retirement epochs, the two orderings that must not be
      reversed, and the non-goals
- [x] 1.2 `core-type-system` — stable field identity, the identity manifest with tombstones and a
      CI gate, the reflection generator, and the control-plane rule; `TypeId` corrected from a
      name hash to an assigned recorded identifier; attributes made strongly typed
- [x] 1.3 `serialization-and-prefabs` — the two serialization modes, value-level migration with
      declared classes applying to overrides and saves, unknown-data preservation, and
      identity-addressed generated serializers
- [x] 1.4 `core-jobs-and-concurrency` — coroutines, the never-block rule for I/O and GPU,
      cooperative cancellation, priority classes with fairness and deadline hints, the task
      context, deterministic parallel primitives, chunked long-running work, deterministic mode
      extended, and critical-path diagnostics
- [x] 1.5 `core-memory-and-containers` — memory domains, the budget tree, pressure levels,
      retirement and epochs, virtual reservation, ownership conventions, the general heap as a
      benchmarked integration, and telemetry before optimisation
- [x] 1.6 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 **A defect corrected.** `TypeId` was specified as a hash of the fully qualified type
      name, which makes serialized identity a function of C++ source organisation: moving a type
      into a namespace would silently invalidate every asset, save, and network schema referencing
      it. Identity is now assigned once and recorded.
- [x] 2.2 **An inconsistency closed.** `serialization-and-prefabs` requires overrides to address a
      "field identifier" that `core-type-system` never defined. `FieldId` now exists with the same
      guarantees, and the byte offset is confined to native access.
- [x] 2.3 `animation-and-skinning` — property tracks bind by identity rather than by name path, are
      resolved once to a direct accessor, and report unresolved bindings instead of failing a clip
- [x] 2.4 `networking-and-replication` — schemas key on `FieldId`, so a rename is no longer
      "schema drift"; drift now means a field genuinely removed or changed
- [x] 2.5 `rhi-and-render-graph` — GPU memory reports into the shared domain and budget tree,
      raises the shared pressure level, and uses the shared retirement mechanism instead of a
      GPU-specific deferral scheme
- [x] 2.6 `build-system-and-platforms` — the reflection generator as a build step with a real C++
      frontend, the identity manifest committed rather than generated into the build directory,
      deterministic generation, and the CI identity gate
- [x] 2.7 `thirdparty-dependencies` — the foundations recorded as engine-built, with the compiler
      frontend, general heap, and codecs integrated beneath them
- [x] 2.8 `ecs-core`, `world-partition-and-streaming`, `core-assets-and-io`, `editor-architecture`
      — reviewed; no change needed. Chunk storage, cell cooking, and asset residency already
      describe the behaviour these foundations now name, and the inspector already builds from
      reflected metadata.
- [x] 2.9 **Non-goals recorded**: a fiber runtime (coroutines instead), NUMA-aware allocation and
      worker placement (not precluded, not required), and an engine-written general-purpose malloc

## 3. Identity and reflection (deferred)

- [ ] 3.1 Identity manifest format, assignment, tombstones, and the CI gate
- [ ] 3.2 Reflection generator over a C++ compiler frontend; deterministic incremental output
- [ ] 3.3 Type registry with assigned identity; strongly typed attributes
- [ ] 3.4 Generated serializers, component registration, editor metadata, ABI and binding
      descriptors from one declaration
- [ ] 3.5 Verify no `typeid`, `dynamic_cast`, or mangled-name dependency remains

## 4. Serialization and migration (deferred)

- [ ] 4.1 Tagged format: chunked, versioned, bounds-checked, skip-unknown, streamable
- [ ] 4.2 Unknown-data preservation with reporting and deliberate purge
- [ ] 4.3 Value record and the migration pipeline; automatic, generated, and custom classes
- [ ] 4.4 Migration of prefab overrides and saves, not only assets
- [ ] 4.5 Cooked format: packed archetype blocks with build schema identity

## 5. Memory (deferred, before allocator tuning)

- [ ] 5.1 Domains, attribution in all builds, allocator scope carrying a domain
- [ ] 5.2 Telemetry and reporting by domain, type, thread, cell, and asset
- [ ] 5.3 Budget tree with hard and soft limits and startup validation
- [ ] 5.4 Pressure levels with hysteresis and declared subsystem responses
- [ ] 5.5 Frame, scratch, pool, slab, and ECS chunk allocators
- [ ] 5.6 Retirement queues and frame epochs, adopted by the RHI, assets, world, and tasks
- [ ] 5.7 General heap selection by benchmark on target platforms
- [ ] 5.8 Virtual reservation where measurement justifies it

## 6. Tasks (deferred)

- [ ] 6.1 Worker pool, handles, work stealing, per-worker slabs
- [ ] 6.2 Task profiling with critical-path reporting — **before** work-stealing tuning
- [ ] 6.3 Dependency scheduling from declared system access
- [ ] 6.4 Task context: worker index, scratch, cancellation
- [ ] 6.5 Coroutine integration; asynchronous I/O and GPU fence continuations
- [ ] 6.6 Cooperative cancellation with propagation
- [ ] 6.7 Priority classes, anti-starvation fairness, deadline hints
- [ ] 6.8 Deterministic parallel primitives and ordered command and event commit
- [ ] 6.9 Chunked long-running work and its reporting

## 7. Validation (deferred)

- [ ] 7.1 Identity tests: rename a type, move it between namespaces, rename and reorder fields —
      every asset, save, override, animation binding, and replication schema still resolves
- [ ] 7.2 Manifest gate test: an accidental identity change fails CI
- [ ] 7.3 Migration tests including a field split, with overrides and saves migrated
- [ ] 7.4 Round-trip test: a file containing a disabled plugin's data is saved unchanged
- [ ] 7.5 Cooked load test: bulk copy activation with no per-field work, benchmarked against the
      tagged path as a regression guard
- [ ] 7.6 Determinism tests: parallel and single-threaded deterministic modes agree; reductions are
      bit-identical; ordering randomisation exposes order-dependent systems
- [ ] 7.7 Blocked-worker detection test: an I/O wait on a worker is reported
- [ ] 7.8 Cancellation tests: propagation, resource release, and unresponsive-task reporting
- [ ] 7.9 Pressure tests: subsystems trim at `Elevated`, no oscillation at a threshold, and
      `Critical` precedes any allocation failure
- [ ] 7.10 Epoch tests: a resource retired while in flight is reclaimed only after its epoch passes
- [ ] 7.11 Allocation benchmark suite covering the general heap choice

---

**Archived 2026-09-02.** Sections 1 and 2 are complete: four foundational capabilities were
substantially extended and five more updated to match. Two things were corrected rather than added
— name-derived type identity, which would have invalidated content the first time a class moved
namespace, and a field identifier that prefab overrides already required and nothing defined. The
unchecked items from section 3 onward are the implementation backlog; **step 1, stable identity, is
first for a reason** — every later step encodes identity into data, and changing the model
afterwards invalidates everything already written with it.

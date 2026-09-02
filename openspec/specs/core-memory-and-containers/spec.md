# core-memory-and-containers Specification

## Purpose

Defines memory management and the container library. Allocation strategy follows **lifetime and
access pattern**: ECS chunks, frame arenas, per-task scratch, per-worker slabs, fixed-size pools,
streaming caches, and GPU heaps are each budgeted and observable, and the general heap is the
fallback rather than the default hot-path strategy.

Every allocation is attributed to a **memory domain**, and domains are apportioned by a **budget
tree** with hard and soft limits per platform profile. When utilisation rises, a **pressure level**
is broadcast so that caches trim, prefetching backs off, and optional data is dropped *before* an
allocation fails — memory is the resource that fails hardest, and pressure is to memory what the
renderer's budget arbiter is to GPU time.

Resources that may still be referenced are **retired** rather than freed, and reclaimed once no
in-flight frame or task can reach them. One epoch mechanism serves the renderer, the asset system,
the world, and the task system rather than four subtly different deferral schemes.

One ordering is a requirement rather than advice: **telemetry exists before allocator
optimisation**, because choosing an allocator without per-domain attribution is guesswork — and the
general-purpose heap itself is an integration chosen by benchmark, not something the engine writes.

## Requirements
### Requirement: Allocator interface
All engine allocation SHALL pass through an `Allocator` interface providing `allocate(size,
align)`, `reallocate`, `deallocate`, an owning tag, and a **memory domain**. Global `new`/`delete`
SHALL NOT be used in engine code.

The engine SHALL provide these allocators:

| Allocator | Use |
|---|---|
| `SystemAllocator` | Backing allocator over the platform heap; an integration chosen by benchmark |
| `ArenaAllocator` | Bump allocation, freed as a whole; per-frame and per-load scratch |
| `PoolAllocator<T>` | Fixed-size blocks for uniform objects |
| `ChunkAllocator` | Fixed-size chunks with stable addresses, backs ECS storage and handle pools |
| `StackAllocator` | LIFO scoped allocation within a function or job |
| `SlabAllocator` | Per-worker slabs for task records, coroutine frames, and events |
| `TrackingAllocator` | Debug wrapper recording tag, size, and call site |

Hot paths SHALL be able to use a concrete allocator type directly, without virtual dispatch, where
the indirection would be measurable.

#### Scenario: Per-frame scratch is freed in O(1)
- **WHEN** a frame ends
- **THEN** the frame arena SHALL be reset by resetting its offset, without running destructors
  for trivially destructible data

#### Scenario: Allocation is attributable
- **WHEN** the engine allocates
- **THEN** the allocation SHALL carry a tag and a domain, and per-domain totals and peaks SHALL be
  queryable in all builds

#### Scenario: Out of memory
- **WHEN** an allocation fails
- **THEN** the allocator SHALL return null and the caller SHALL surface an error; the engine
  SHALL NOT throw

#### Scenario: No virtual dispatch where it matters
- **WHEN** a hot path allocates from a known allocator
- **THEN** it SHALL be able to use the concrete type directly rather than through the interface

### Requirement: Memory domains
Every allocation SHALL be attributed to a **memory domain**, declared rather than inferred:
engine, ECS, frame, renderer, GPU, physics, animation, audio, assets, streaming, world, network,
scripting, and editor.

Domains SHALL be hierarchical, so a domain can be reported in aggregate or broken down.

Domain attribution SHALL be available in **all builds**, not only development ones, since a
shipping build that cannot say where its memory went cannot be diagnosed on the platform where it
failed. Detailed per-call-site tracking MAY remain development-only.

The allocator scope defined for containers SHALL carry a domain, so allocations made within a
subsystem's scope are attributed without per-call-site annotation.

#### Scenario: Shipping build can attribute memory
- **WHEN** a shipping build approaches its memory limit
- **THEN** per-domain live bytes SHALL be queryable, without a development build

#### Scenario: Scope attributes automatically
- **WHEN** the renderer pushes its allocator scope
- **THEN** containers constructed within it SHALL be attributed to the renderer domain with no
  per-allocation annotation

### Requirement: Memory budget tree
The engine SHALL define a **memory budget tree**: a total budget for the process, apportioned to
domains and sub-domains, configurable per platform profile.

Each budget SHALL be declared **hard** or **soft**. A soft budget being exceeded raises pressure; a
hard budget SHALL NOT be exceeded — the owning system SHALL evict, refuse, or degrade instead.

Budgets SHALL be reportable as target against actual, and the sum of child budgets exceeding the
parent SHALL be a configuration error detected at startup rather than at the moment of failure.

Streaming and caching systems SHALL hold memory budgets in the same way that rendering subsystems
hold GPU-time allocations, so that content cannot opportunistically consume all available memory.

#### Scenario: Caches cannot take everything
- **WHEN** an asset cache would grow beyond its budget
- **THEN** it SHALL evict rather than allocate, and the eviction SHALL be reported

#### Scenario: Over-subscription is caught at startup
- **WHEN** configured sub-budgets sum to more than their parent
- **THEN** the engine SHALL report the misconfiguration at startup

#### Scenario: Platform profiles differ
- **WHEN** the same project runs on a platform with less memory
- **THEN** its profile SHALL supply different budgets, with no code change

### Requirement: Memory pressure levels
The engine SHALL maintain a **pressure level** — `Normal`, `Elevated`, or `Critical` — derived from
budget utilisation and from platform-reported memory conditions, and SHALL broadcast changes to
subsystems.

Subsystems SHALL declare their response:

| Level | Expected response |
|---|---|
| `Normal` | Prefetch and cache freely within budget |
| `Elevated` | Trim caches, reduce prefetch distance, evict unreferenced data |
| `Critical` | Drop optional caches, force streaming quality down, defer non-essential work |

Pressure SHALL be **the coordination mechanism** for memory across streaming, geometry, texture,
shadow, world, audio, and asset residency, in the same way the renderer budget arbiter coordinates
GPU time. Subsystems SHALL respond to the declared level rather than each polling platform memory.

For **paged subsystems** — virtual geometry, virtual textures, virtual shadows, and illumination
caches — the response SHALL be coordinated by the residency layer (see `residency`), which weighs
their reductions against each other by importance and visible impact rather than letting each evict
independently.

GPU memory SHALL participate in the same pressure model as CPU memory.

Pressure transitions SHALL be hysteretic, so systems do not oscillate between trimming and
refilling.

An allocation failure SHALL remain a defined outcome — the allocator returns null and the caller
surfaces an error — but pressure SHALL be the mechanism that prevents it, since by the time an
allocation fails the system has already failed.

#### Scenario: Everything trims together
- **WHEN** pressure reaches `Elevated`
- **THEN** every cache-holding subsystem SHALL trim according to its declared response, rather than
  one subsystem freeing memory that another immediately consumes

#### Scenario: No oscillation
- **WHEN** utilisation hovers at a threshold
- **THEN** hysteresis SHALL prevent repeated trim-and-refill cycles

#### Scenario: Pressure precedes failure
- **WHEN** memory is exhausted
- **THEN** `Critical` pressure SHALL have been signalled beforehand and recorded, so the failure is
  diagnosable

#### Scenario: Paged subsystems reduce together
- **WHEN** pressure forces a reduction across geometry, texture, and shadow caches
- **THEN** the residency layer SHALL decide the split by importance, rather than three independent
  evictions competing

### Requirement: Allocator propagation
Containers SHALL accept an allocator at construction and SHALL default to the current
**allocator scope**, a thread-local stack pushed and popped by RAII guards.

#### Scenario: Subsystem allocations are grouped
- **WHEN** the renderer pushes its allocator scope during initialisation
- **THEN** containers constructed within that scope SHALL allocate from the renderer's arena and
  be attributed to it

### Requirement: Sequence containers
The engine SHALL provide:

| Container | Characteristics |
|---|---|
| `Array<T>` | Growable, contiguous, allocator-aware; the default dynamic array |
| `FixedArray<T, N>` | Inline capacity, no heap allocation |
| `SmallArray<T, N>` | Inline for N elements, spills to the heap beyond |
| `Span<T>` | Non-owning (pointer, length) view; `std::span` is used where sufficient |
| `RingBuffer<T>` | Bounded FIFO for audio and networking |
| `SparseSet<T>` | Dense storage with a sparse index, used for entity-keyed lookups |
| `IntrusiveList<T>` | Embedded links, O(1) removal from the element itself |

`Array<T>` SHALL grow geometrically, SHALL support `reserve`, and SHALL provide
`remove_unordered` as an O(1) removal.

#### Scenario: Trivially relocatable types move cheaply
- **WHEN** an `Array<T>` grows and `T` is trivially relocatable
- **THEN** elements SHALL be moved with a single `memcpy` rather than per-element move
  construction

#### Scenario: No copy-on-write in runtime containers
- **WHEN** an `Array<T>` is copied
- **THEN** the copy SHALL be deep and explicit; the engine SHALL NOT use implicit copy-on-write
  in runtime containers, so ownership and cost are visible at the call site

### Requirement: Associative containers
The engine SHALL provide `HashMap<K, V>` and `HashSet<K>` using open addressing with robin-hood
probing, `FlatMap<K, V>` (sorted array, cache-friendly for small maps), and `OrderedMap<K, V>`
where deterministic iteration order is required.

Hashing SHALL be defined in one place (`core/hash`), be seeded per process in development builds
to catch iteration-order dependencies, and be deterministic in shipping builds.

#### Scenario: Serialization order is deterministic
- **WHEN** reflected data backed by a map is serialized
- **THEN** iteration SHALL use a container with a defined order, so output is byte-stable across
  runs

#### Scenario: Accidental order dependence is caught
- **WHEN** development builds randomise the hash seed
- **THEN** code that depends on `HashMap` iteration order SHALL produce varying results and be
  caught in testing

### Requirement: Handle pools
`HandlePool<T>` SHALL back every server's object storage: chunked, address-stable slots with
generation counters, a free list, and optional thread-safe allocation.

Chunk size SHALL be configurable per pool, and chunk pointer arrays SHALL be pre-reserved in
thread-safe pools so growth never reallocates the pointer array under a concurrent reader.

#### Scenario: Pointer stability
- **WHEN** a pool grows while a caller holds a `T*` obtained from a handle
- **THEN** the pointer SHALL remain valid

#### Scenario: Concurrent resolve during growth
- **WHEN** one thread allocates while another resolves an existing handle
- **THEN** the resolve SHALL be safe, with capacity published by an atomic store

### Requirement: Chunked component storage
ECS component data SHALL be stored in fixed-size **chunks** (default 16 KiB) holding
structure-of-arrays component data for one archetype, with the entity id array first and each
component array aligned to its type's alignment.

Chunks SHALL carry a per-component change version so change detection is O(1) per chunk.

#### Scenario: Chunk capacity derives from the archetype
- **WHEN** an archetype's component set is finalised
- **THEN** the chunk capacity SHALL be computed as the largest entity count whose SoA arrays plus
  header fit the chunk size, and SHALL be recorded on the archetype

#### Scenario: Iteration is linear
- **WHEN** a query iterates a chunk
- **THEN** each component array SHALL be traversed contiguously, with no per-entity indirection

### Requirement: Scratch and frame memory
The engine SHALL expose a per-thread frame arena and a per-job scratch arena. Memory obtained
from them SHALL NOT outlive the frame or job respectively, and development builds SHALL poison
the memory on reset to catch use-after-reset.

#### Scenario: Temporary buffer in a system
- **WHEN** a system needs a temporary array sized by entity count
- **THEN** it SHALL allocate from the frame arena and SHALL NOT free it individually

#### Scenario: Use after reset is caught
- **WHEN** development builds reset a frame arena
- **THEN** the region SHALL be filled with a poison pattern so stale reads are obvious

### Requirement: Retirement and frame epochs
Resources that may still be referenced by in-flight frames or running tasks SHALL be **retired**
rather than freed immediately: logically released, then reclaimed once no in-flight consumer can
still reference them.

The engine SHALL define **frame epochs** for this purpose, and one retirement mechanism SHALL serve
every consumer — GPU resources, asset pages, published snapshots, command buffers, task records —
rather than each subsystem implementing its own deferral.

Retirement queues SHALL be bounded and reportable; a queue growing without draining SHALL be
detectable, since it indicates a consumer that never completes.

#### Scenario: Destroyed while in use
- **WHEN** a resource is destroyed during a frame that may still reference it
- **THEN** it SHALL be retired and reclaimed only after that frame's epoch has passed

#### Scenario: One mechanism, many consumers
- **WHEN** the asset system and the renderer both defer reclamation
- **THEN** they SHALL use the same epoch mechanism rather than separate deferral schemes

#### Scenario: Stuck retirement is visible
- **WHEN** a retirement queue grows without draining
- **THEN** it SHALL be reported with the epoch that has not advanced

### Requirement: Virtual address reservation
Where the platform supports it, large caches and stable arenas MAY **reserve** a virtual address
range and **commit** pages on demand, so an arena can grow without relocating and without
committing its maximum size.

Reservation SHALL be a declared property of an allocator, not a global default, and its use SHALL be
justified by measurement rather than applied uniformly.

Reserved-but-uncommitted address space SHALL be excluded from memory budgets and reported
separately, since it is not memory in use.

#### Scenario: A cache grows without moving
- **WHEN** a streaming cache grows
- **THEN** it SHALL commit further pages within its reservation, and existing pointers SHALL remain
  valid

#### Scenario: Reservation is not consumption
- **WHEN** memory is reported
- **THEN** reserved address space SHALL be reported separately from committed memory and SHALL NOT
  count against budgets

### Requirement: Reference-counted shared data
`Ref<T>` SHALL provide intrusive atomic reference counting for shared, immutable-after-load data
such as loaded assets, shader programs, and font faces.

`Ref<T>` SHALL NOT be used for per-entity gameplay state, which is component data owned by ECS
storage.

#### Scenario: Asset unloaded when unreferenced
- **WHEN** the last `Ref` to a loaded mesh is released
- **THEN** the asset SHALL become eligible for unloading by the asset system, subject to its
  retention policy

#### Scenario: Weak observation
- **WHEN** a subsystem must observe an asset without keeping it alive
- **THEN** it SHALL hold an `AssetId` and resolve on demand, not a `Ref`

### Requirement: Ownership conventions
The engine SHALL define and document its ownership vocabulary, and code SHALL follow it:

| Form | Meaning |
|---|---|
| `UniquePtr<T>` | Sole heap ownership |
| `Ref<T>` | Shared ownership of immutable-after-load data; deliberately rare |
| `Handle<Tag>` | Generational reference to server- or pool-owned objects |
| `Span<T>`, `StringView` | Non-owning views with a caller-guaranteed lifetime |
| Arena and scratch pointers | Lifetime bounded by an arena or task scope |

Shared ownership SHALL NOT be the default object model. Reference counting SHALL be reserved for
data that is genuinely shared and immutable after load.

Cooked asset data SHALL be **immutable after load**, so it can be shared, memory-mapped, and read
concurrently without synchronisation. Mutable runtime state derived from an asset SHALL live
separately from the asset.

#### Scenario: Assets are read without locks
- **WHEN** several workers read one loaded mesh asset
- **THEN** they SHALL read shared immutable data with no synchronisation

#### Scenario: Mutable state is separate
- **WHEN** an instance of an asset carries mutable state
- **THEN** that state SHALL be stored separately from the immutable asset data

### Requirement: General heap is an integration decided by measurement
The general-purpose heap backing `SystemAllocator` SHALL be a proven third-party allocator selected
by **benchmark on target platforms**, not an engine-written implementation and not a preference.

The choice SHALL be replaceable behind the allocator interface, and the benchmark SHALL be part of
the performance suite so a regression in the choice is visible.

The general heap SHALL be the **fallback**, not the hot path: ECS chunks, frame arenas, task
scratch, and pools SHALL cover the allocation patterns that occur per frame.

#### Scenario: The choice is evidence-based
- **WHEN** a general allocator is selected
- **THEN** the decision SHALL be supported by benchmarks on target platforms, recorded with the
  dependency

#### Scenario: Hot paths do not reach the general heap
- **WHEN** a frame executes
- **THEN** per-frame and per-task allocation SHALL come from arenas, scratch, and pools, and general
  heap allocation SHALL be rare and attributable

### Requirement: Memory diagnostics
Development builds SHALL provide: per-tag and per-domain live bytes, peak bytes and allocation
counts; leak reporting at shutdown with the allocating call site; optional guard pages or red zones
around allocations; poisoning of freed and reset memory; double-free and generation validation; and
integration hooks for ASan, UBSan, and TSan.

All builds SHALL provide per-domain live and peak bytes, budget utilisation, pressure level and its
history, retirement queue depths, and pool and arena utilisation.

Reporting SHALL be attributable along the axes that answer real questions: by domain, by type, by
thread, by world cell, and by asset — so that "why is this region consuming this much" is
answerable.

**Telemetry SHALL exist before allocator optimisation.** Choosing or tuning allocators without
per-domain attribution is guesswork, and the ordering is a requirement rather than advice.

Allocations that are intentionally held for the process lifetime SHALL be taggable as such, so leak
reports distinguish them from defects.

#### Scenario: Leak report at shutdown
- **WHEN** the process exits with outstanding allocations in a tracked allocator
- **THEN** a report SHALL list them by tag with counts, sizes, and call sites, excluding
  allocations tagged as process-lifetime

#### Scenario: Sanitiser build
- **WHEN** the engine is built with `CY_SANITIZE=address`
- **THEN** custom allocators SHALL route through the sanitiser's interface so overflows and
  use-after-free are reported accurately

#### Scenario: Attribution answers a question
- **WHEN** a world region consumes unexpected memory
- **THEN** the report SHALL attribute it by domain, asset, and cell

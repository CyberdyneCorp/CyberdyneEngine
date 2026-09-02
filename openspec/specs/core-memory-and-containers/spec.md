# core-memory-and-containers Specification

## Purpose

Defines memory management and the container library. CyberdyneEngine controls allocation
explicitly: every allocation is attributable to an arena or tag, per-frame data comes from
scratch allocators that are freed in O(1), and containers are chosen for known access patterns
rather than defaulting to `std::vector` everywhere.

## Requirements

### Requirement: Allocator interface
All engine allocation SHALL pass through an `Allocator` interface providing `allocate(size,
align)`, `reallocate`, `deallocate`, and an owning tag. Global `new`/`delete` SHALL NOT be used
in engine code.

The engine SHALL provide these allocators:

| Allocator | Use |
|---|---|
| `SystemAllocator` | Backing allocator over the platform heap |
| `ArenaAllocator` | Bump allocation, freed as a whole; per-frame and per-load scratch |
| `PoolAllocator<T>` | Fixed-size blocks for uniform objects |
| `ChunkAllocator` | Fixed-size chunks with stable addresses, backs ECS storage and handle pools |
| `StackAllocator` | LIFO scoped allocation within a function or job |
| `TrackingAllocator` | Debug wrapper recording tag, size, and call site |

#### Scenario: Per-frame scratch is freed in O(1)
- **WHEN** a frame ends
- **THEN** the frame arena SHALL be reset by resetting its offset, without running destructors
  for trivially destructible data

#### Scenario: Allocation is attributable
- **WHEN** the engine is built with `CY_DEVELOPMENT`
- **THEN** every allocation SHALL carry a tag, and per-tag totals and peaks SHALL be queryable
  and shown in the profiler

#### Scenario: Out of memory
- **WHEN** an allocation fails
- **THEN** the allocator SHALL return null and the caller SHALL surface an error; the engine
  SHALL NOT throw

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

### Requirement: Memory diagnostics
Development builds SHALL provide: per-tag live bytes, peak bytes and allocation counts; leak
reporting at shutdown with the allocating call site; optional guard pages or red zones around
allocations; and integration hooks for ASan, UBSan, and TSan.

#### Scenario: Leak report at shutdown
- **WHEN** the process exits with outstanding allocations in a tracked allocator
- **THEN** a report SHALL list them by tag with counts, sizes, and call sites

#### Scenario: Sanitiser build
- **WHEN** the engine is built with `CY_SANITIZE=address`
- **THEN** custom allocators SHALL route through the sanitiser's interface so overflows and
  use-after-free are reported accurately

# `src/world/` — CyberWorld

The spatial and persistence layer above the ECS: what exists, where it belongs, whether it should
exist now, and where its persisted state lives. `world-partition-and-streaming`, reaching **Working**
at M6 (tasks 3.1–3.8).

## The name is the first requirement

`world-partition-and-streaming` opens by saying the engine "SHALL NOT name both *world* in its API".
`cy::ecs::World` is the ECS runtime container and nothing else is ever called one. Everything here is
`cy::world::` and no type in it is `World`:

| Concept | Type |
|---|---|
| Partition configuration and cell identity | `PartitionConfig`, `CellId`, `CellCoord`, `WorldPosition` |
| The replaceable partitioner | `Partitioner`, `HierarchicalGrid` |
| A cooked cell in ECS-native form | `CookedCell`, `CookedBlock`, `CellBuilder` |
| Sources, shapes and prediction | `StreamingSource`, `SourceRegistry`, `CellRequirement` |
| Staged, atomic activation | `CellActivation`, `CellEventQueue` |
| Layers, HLOD, representation tiers | `LayerTable`, `HlodRegistry`, `RepresentationTable` |
| The persistence overlay | `PersistenceOverlay`, `DynamicIndex` |
| The tick that ties them together | `WorldStreaming` |

One direction only: the persistent world **publishes** entities into the ECS world, and no ECS system
is required to know this layer exists.

## The four axes

The distinction that does the most work in the specification, and the one this module is shaped by:

> cell residency, cell activation, asset residency and simulation detail are **four separate axes**.

`CellState` is the first two, and they are independently controllable — a cell is `Resident` for as
long as it likes without being `Activated`. Asset residency belongs to `residency`; simulation detail
belongs to the AI, animation and physics LOD systems. Collapsing them is what makes crossing a
boundary mean *load everything now*, and it is the thing every later system that streams assumes has
not happened.

`unit.world` and `integration.world_streaming` hold that separation: a prefetch source buys residency
and asks for no activation, and the ECS world stays empty while the bytes are in memory.

## What must not be retrofitted

design.md §5 pins these to M6 because they are cheap now and migrations later. Each has a test.

| Invariant | Where it lives | What holds it |
|---|---|---|
| Cells are cooked in **ECS-native form** | `cell.h`'s `CookedBlock` | `unit.world` — a cook produces archetype blocks, and `CookedBlock`'s spans **are** `ecs::World::instantiate()`'s arguments, with no conversion step anywhere |
| Residency is separate from activation | `CellState`, `StreamingSource::activates` | `integration.world_streaming` — bytes resident with simulation off |
| Persistent identity is stable across unload | `PersistentId`, assigned at authoring time | `unit.world` — identity is never derived from position, file, index or path |
| Cooked cells are immutable | `PersistenceOverlay` | `integration.world_activation` — the overlay is applied to private staging, and the authored cell is unchanged afterwards |

## The tick

`WorldStreaming::tick()` is bounded in every loop. In order:

1. **Deferred requests** made during the last tick's event consumption are taken up. There is no
   callback for a request to be re-entrant from.
2. **Sources state what they require** — location, shape, radius, prediction, channels, priority,
   deadline. A cell required by any source is required.
3. **Hard dependencies close.** A required cell's `RequireLoaded` targets are required too.
4. **Work is ordered**: critical, gameplay, visible, predicted, background; then priority, then
   deadline, then cell identifier — the last so that two machines agree.
5. **The I/O budget buys residency**, one channel delta at a time. A resident cell that gained a
   channel pays for that channel and is not reloaded.
6. **The activation budget buys preparation and publication.** Preparation spans frames in private
   staging; each publication is atomic or does not happen.
7. **Cells no longer required are withdrawn**, then evicted under memory pressure, lowest priority
   first, with the shortfall reported rather than silently exceeded.
8. **HLOD visibility is recomputed** — in this tick, so a proxy is replaced in the same tick its
   cells are published.

### The budget is spent against the cost model, not against a clock

A budget enforced by sampling wall time makes activation non-deterministic: the same route on two
machines prepares different cells on the same frame, and no test can reproduce it. So preparation
spends `estimate_activation_time()` — the same function the cooker used to write the cell's cost —
and the *measured* time is compared against that estimate afterwards. That comparison is
`world-partition-and-streaming`'s "estimates SHALL be validated against measured runtime cost, and
significant divergence SHALL be reported", doing double duty as the thing that keeps the budget
honest. `TickReport::worst_cost_divergence` is where it comes out.

## Task 3.8 — the exit criterion

> Continuous traversal holds the frame budget with no hitch above threshold, measured over a fixed
> route.

`integration.world_traversal` is that measurement: a two-kilometre square of 400 cells, 20 000 props,
traversed for 240 ticks at 5 m a tick while cells stream in, activate, deactivate and are evicted
behind the traveller. It reports the per-tick distribution rather than asserting a shape, and checks
two different things:

* the **modelled** work per tick never exceeds the activation budget plus the one publication that
  budget admitted — deterministic, and identical on an idle laptop and a loaded CI runner;
* the **wall clock** per tick against a threshold with two orders of magnitude of headroom, because
  the failure it exists to catch is a tick that does work proportional to the size of the world.

A second case runs the same route twice in two freshly created worlds and compares the counters: the
route is deterministic, which is what makes the first case a measurement of the code rather than of
the machine.

## Suites

| Suite | Kind | What it holds |
|---|---|---|
| `world` | unit | Coordinates and cell identity, the partitioner and spatial binding, cooked cells and validation, sources and shapes, layers, HLOD, representation tiers, the overlay |
| `world_activation` | integration | Staged preparation, atomic publication, layer switching over 20 000 rows, the overlay applied during activation, teardown mid-preparation |
| `world_streaming` | integration | The whole tick, and a world destroyed mid-activation twelve times over |
| `world_traversal` | integration | Task 3.8's route |

Two of them exist because of this milestone's rule that teardown is tested **under load**:
`world_activation` destroys a cell at every phase of preparation, and `world_streaming` destroys a
whole `WorldStreaming` after every tick count from zero to eleven — a mixture of cells published,
mid-preparation and untouched — and then checks the ECS world it published into is clean.

## What is not here

* **No file I/O and no save format.** `save-and-persistence` owns the overlay's encoding,
  journalling, atomicity, incremental writing, migration and storage backends. This module owns the
  *model* the world maintains, which is the division the specification itself draws.
* **No renderer, physics backend or navigation.** Cells carry payloads for those subsystems and
  publish structured events they consume at their own defined points. The world never calls into
  them — a dependency here is what would make a re-entrant callback during streaming possible.
* **No cooker and no asset loading yet.** `CellBuilder` is the in-memory cook; the pipeline that
  writes cells to content-addressed chunks and reads them back is `build-and-packaging`'s and
  `asset-import-pipeline`'s, and the interface they meet is `CellPayload::chunk`.
* **No allocator.** Every container takes the allocator its owner was handed, so a streaming world's
  memory is attributable to `MemoryDomain::World` and visible to the pressure system that evicts it.

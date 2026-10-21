# `src/rendering/gi_streaming/` — layer 4

The GI's scene content streamed with world cells. Issue #35, the world half;
`rendering-global-illumination` ("GI content streams with world cells") and
`world-partition-and-streaming` ("Cell lifecycle events").

| File | What it holds |
|---|---|
| `include/cy/rendering/gi_streaming/gi_streaming.h` | `GiCellStreaming`: a `world::CellEventQueue` consumer; `GiCellContent`, `GiCellLoader`, `GiStreamingBudget`, `GiStreamingReport` |
| `tests/test_gi_streaming.cpp` | `smoke.render_gi_streaming`: a real `world::WorldStreaming` route, the budget, and the streamed GI against a system that never streamed |

## What it does

```cpp
gi_streaming::GiCellStreaming gi_cells(allocator, system, partition);
gi_cells.attach(world.events(), /*order=*/50);
gi_cells.set_loader(&load_gi, &cooked);        // a cell's cards, fields and volumes, as cooked
// every frame, after the world's tick and before the GI's update:
gi_cells.tick(frame, {/*cards_per_tick=*/2048, /*fields_per_tick=*/8});
system.update(context);
```

A cell that activates with `world::Channel::Illumination` is asked of the loader once; its cards go
into the GI as CHUNKS — each its own GI scene cell through `IlluminationSystem::ingest_cell`, so its
emitters are classified and its small bright lamps promoted, cut only between instances so no
emitter is split — and its meshes' distance fields — and, where the device traces the hardware tier, its instances of
geometry the application declared once (`acceleration().add_instance`) — are placed a few a tick. When
the last of it is in, its GI volumes join the streamed cells' own set (`set_volumes` under
`GiVolumeOwner::Streaming`, which leaves the author's and the game's alone). A cell that deactivates
or is evicted leaves at once — its chunks evicted, its fields and instances removed, its volumes withdrawn — and
what it released is charged to that tick's budget first. A cell that leaves before it has finished
arriving takes out what part of it arrived.

The device copies follow by what changed: cards upload by revision, bricks by what a placement
touched, and the card lookup grid lists again only the cards that arrived or left
(`gi::CardNeighbourGrid::update`). `smoke.gi_gpu_streaming` holds that on the device.

## Measured

- `smoke.render_gi_streaming`: an 8 x 8 grid of 16 m cells, a traveller across it for 160 ticks —
  32 cells in and 25 out, their cards, fields, promoted lamps and volumes with them. The streaming
  tick's own processor time 0.02 ms median and 0.86 ms worst (a 256-card budget); the GI, converged
  at the route's end, is the GI of the same cells ingested at once to 1.2e-7 mean relative over 49
  points; the GI's update is never above the worst frame the same route costs a world that never
  streamed.
- `smoke.gi_gpu_streaming` (RTX 5060, Vulkan): six cells along a strip with the caches on the
  device — the host's whole GI frame 0.45 ms median and 2.5 ms worst, the device's card shading and
  gather 0.18 ms; every frame's upload no more than what arrived, left or was invalidated.

## What is not here

- **`samples/10-world`.** The world sample shades per vertex with no depth and normal prepass and
  its terrain is not carded; streaming its cells into a GI it does not draw with proves nothing the
  test world does not.

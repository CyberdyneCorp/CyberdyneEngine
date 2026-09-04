# `src/rendering/` — layer 4

The renderer's engine-side code: everything that consumes the RHI and is not a backend.

**Governed by**: `rhi-and-render-graph`, `rendering-architecture`, `rendering-geometry-and-resources`,
`rendering-materials-and-shading`, `rendering-forward-clustered`, `rendering-lighting-and-shadows`
and `rendering-culling-and-lod`.

| Subdirectory | Target | What it is |
|---|---|---|
| `graph/` | `cy::rendering-graph` | the render graph: barriers, aliasing, scheduling and semaphores, derived from declared reads and writes |

## Why layer 4 and not layer 2

`rendering-architecture`'s render server is a server, and servers are layer 2. The render graph is
not a server: it calls the device, creates transient resources, records command buffers and emits the
barriers it derived. A graph at layer 2 would need the RHI injected as an abstract interface it is
not allowed to name — which is the shape you get when a layer number is chosen before the dependency
is looked at. The graph sits above the backends because it uses one.

## The invariant this directory carries

**A pass declares what it reads and what it writes, and has no API to emit a barrier.** That is M3's
whole point (design.md §2): it is a property of the thirtieth pass, and the thirtieth pass obeys it
because the first one did. `graph/include/cy/rendering/graph/graph.h` is the entire vocabulary a pass
author has, and `graph/src/compile.cpp` is where everything else is derived.

`cy::rendering::GraphExecutor` is the one type in the engine that can reach
`cy::rhi::Device::barrier_recorder()`. `tools/layercheck/layercheck.py --check barriers` fails the
build if a barrier symbol appears anywhere else, and
`tools/layercheck/fixtures/barrier-outside-graph/` is the deliberate violation that proves the check
still fires.

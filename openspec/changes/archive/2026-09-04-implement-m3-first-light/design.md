# Design: M3 — First light

## The M2 handoff

| | |
|---|---|
| ECS | `src/ecs/` — archetypes over M1 chunks, queries, systems with access declarations, structural deferral at flush points |
| Nodes | `src/scene/` — a `Node` is a handle onto an entity and caches nothing |
| Serialization | `src/scene/serialization/`, `src/core/serialize/`, `tools/cook/` — cooked scenes load as archetype blocks plus a key pass and a reference-site pass |
| Loop | `src/runtime/` — N fixed ticks then one variable render, interpolation alpha, one commit point per tick. The render step is an empty seam waiting for this milestone. |
| Determinism | `src/core/determinism/` — clock, epochs, commit boundary, seeded streams, `Classified<>`, hierarchical hashing |
| Servers | `servers.h` — registry, fallback chain, `NullServer`. All seven servers resolve to null because no backend has ever registered. M3 registers the first. |

## 1 — The null backend is written first, not last

`rhi-and-render-graph` requires a null backend for continuous integration without a GPU.

**Decision.** The null backend is implemented **before** Vulkan, not after it.

Written first, it forces the RHI to be an interface rather than a thin wrapper over whichever
Vulkan calls were convenient — because there is no Vulkan to lean on. Written afterwards, it becomes
a set of empty functions shaped by decisions Vulkan already made, and it stops being a reference for
what the RHI requires.

It is also what makes every later milestone's rendering work testable in CI on a machine with no
GPU, which is most CI machines.

## 2 — Barriers are computed — the invariant, and this milestone's whole point

`rhi-and-render-graph`: the graph owns synchronisation, transient aliasing and pass scheduling, and
no renderer code writes a barrier.

**Decision.** A pass declares what it reads and what it writes. It has **no API to emit a barrier**,
and the check is structural rather than a review convention: the barrier-emitting calls live behind
an interface only the graph implementation can reach, and a grep-level gate fails the build if a
barrier call appears outside it.

That gate lands in this milestone, with the first pass, and joins the permanent set. The reason is
the same as every other invariant on the roadmap: this is a property of the thirtieth pass, and the
thirtieth pass obeys it because the first one did.

**The scheduling model is this milestone's named risk** (`tasks.md` §0). Deriving correct barriers
for a linear pass sequence is tractable; deriving them across async compute queues, where a resource
written on one queue is read on another and the graph must insert semaphores rather than pipeline
barriers, is where the model either holds or does not. Spike it against a hard case — a compute pass
writing a resource that a graphics pass samples while a second compute pass writes a different
subresource of the same image — before thirty passes depend on the answer.

## 3 — Reversed-Z is a number, not a convention

M1 made the conventions executable tests in arithmetic. M3 is where they meet a depth buffer.

**Decision.** Every convention gets a GPU-side assertion, not only a CPU-side one: the depth buffer
is `[0,1]`, cleared to **0**, compared **GreaterEqual**, and a perspective matrix built by the
engine maps near to 1 and far to 0 *as sampled back from the device*. A golden image is not
sufficient evidence — a scene can look right with an inverted comparison until something intersects.

**Camera-relative rendering lands with the first draw, not when precision breaks.** Positions reach
the GPU relative to the camera, and the test is a scene one million units from the origin rendering
without visible jitter. Retrofitting this at M6, when world partition puts real content at real
distances, means revisiting every shader and every transform path that assumed world space.

## 4 — The GPU scene is an interface, published to, not walked

`rendering-architecture` names the GPU scene as the shared GPU-side instance representation.

**Decision.** The GPU scene is defined as a **publication interface** in this milestone, even though
there is exactly one producer. From M7 onward VFX publishes mesh particles into it, animation
publishes skinned instances, and virtual geometry publishes clusters — none of them through the ECS
and none through a CPU round trip.

Designing it as "whatever the mesh renderer needs" and generalising later is the failure mode: the
second producer arrives with a requirement the interface cannot express, and the interface becomes
two interfaces. One producer is the cheapest moment to get the shape right, provided the shape is
designed for the producers that are coming.

Nothing walks the node tree at render time. The renderer's input is the GPU scene; the snapshot that
fills it is taken at M2's commit boundary.

## 5 — Slang is integrated, the material compiler is not built

`thirdparty-dependencies` is explicit: shader toolchains are integrated, and the engine does not
author a shading language or write a shader optimiser.

**Decision.** M3 integrates Slang → SPIR-V and builds the caching, permutation and reflection-driven
binding around it. It does **not** build the material compiler — that is M7's, and it is the part
the engine owns because it is where material cost is decided.

The seam matters: engine-*generated* shader source, when it arrives at M7, passes through this same
pipeline. No second toolchain, no separate cache, no backend-specific source. Build the pipeline
now so that generated source has somewhere to go.

## 6 — Deterministic submission, because M9 will need it

`rendering-architecture` requires deterministic submission order.

**Decision.** Draw ordering derives from a sort key computed from stable inputs — material, mesh,
depth — and never from iteration order over a hash map, pointer values, or the order instances
happened to be published.

At M3 this looks like pedantry. At M9 it is the difference between a golden-image test that
reproduces and one that is flaky for reasons nobody can find, and between a replay that renders the
same frame twice and one that does not. It costs nothing now.

## 7 — What M3 deliberately does not do

- **No virtual geometry, no virtual shadows, no GI, no temporal.** M7. The frame is conventional:
  cluster the lights, prepass the depth, shade forward.
- **No material compiler, no material graphs.** M7. A standard material with parameters, authored as
  data, is enough to light a scene.
- **No GPU-driven culling.** M6. CPU frustum culling and LOD selection at Seed.
- **No Metal, no D3D12.** M11, with a Metal seed at M7 — the point of which is to expose
  Vulkan-specific assumptions while removing them is still cheap.
- **No editor viewport.** M5. The sample is a standalone window.

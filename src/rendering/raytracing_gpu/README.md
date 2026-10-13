# `src/rendering/raytracing_gpu/` — layer 4

The acceleration service's structures, built on a device. `cy::rendering-raytracing-gpu`. Issue #35,
stage 3.

**Governed by**: `ray-tracing-infrastructure` — "Ray tracing infrastructure is a renderer service",
"Structure lifecycle and budget", "Ray query interface". Change:
`openspec/changes/add-gpu-gi-radiance-tracing/`.

## The service decides; this builds what it decided

`rt::AccelerationService` (`src/rendering/raytracing/`) keeps every decision the requirement gives
it: the build budget and its priority order, refit against rebuild by the adapter's quality floor,
eviction, and which instances the top level traces this frame. It names no device. `DeviceStructures`
is an `rt::StructureMirror` — `AccelerationService::set_mirror(&structures)` — and records the same
work through the RHI's acceleration structures (`cy/backends/rhi/acceleration.h`):

| The service reports | The device records |
|---|---|
| `bottom_built` | the geometry's triangles copied into the shared vertex and index pools, and a BUILD |
| `bottom_refit` | the moved positions written over the same range, and an in-place UPDATE |
| `bottom_evicted` | the structure destroyed and its pool ranges returned |
| `top_level` (every active frame) | an UPDATE when only transforms or refits moved, NOTHING when nothing did, a BUILD otherwise |

`declare(graph)` adds the bottom-level pass and then the top-level pass; `import_top(graph)` is the
top level's storage as a resource of the same graph, for a consumer's
`ComputeAccelerationStructureRead`. Both go through `RenderGraph::import_buffer_once`, so the build and
the trace name one resource and the graph orders them.

## What a shader reads

A ray query returns an instance, a primitive and a distance. A GI hit needs a normal to resolve
through the surface cache, so `binding()` hands a consumer, beside the top level, what
`AccelerationService::trace_instance` reads on the host: the vertex and index pools, and per traced
instance its offsets into them, its normal matrix's columns (multiplied in the host's order) and its
geometry's declared error. `cy::rendering-gi-gpu`'s `gi_hardware.slang` is the consumer.

## Measured

`render.gi_gpu_tracing`'s "the device builds, refits and updates exactly what the acceleration
service decided", RTX 5060, driver 580.95.05: after every step — the first build, a still frame, a
moved instance, a pose, an added instance, an eviction — 1 024 hardware ray queries agree with
`AccelerationService::trace` with 0 hit/miss disagreements and a worst |dt| of 1.9e-6 m, and the
device recorded exactly 2 builds / nothing / 1 top-level update / 1 bottom-level update and 1
top-level update / 1 top-level build / 1 eviction.

## A device without ray queries

`create` refuses with `ErrorCode::Unsupported` before allocating anything — the RHI's own refusal,
because the device has no `Capability::RayTracing` — and the service on such a device is
`Unsupported` too (`rt::service_config_for`), so it never calls a mirror. Procedural geometry has no
triangles and this module has no intersection shader: its instances are left out of the device top
level, as an instance whose structure is not built is left out of the service's.

## What is not here

- **Frames in flight.** The pools and the instance records are host-visible and written in place,
  right for a renderer that drains each frame; stage 4 adds per-frame staging.
- **DXR and Metal.** The vocabulary is theirs too; the backends refuse until they implement it.
- **Compaction.** Structures are built at their maximum size and never compacted.

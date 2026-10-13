# Design

## The host stays the authority, again

Every class here follows stage 2's rule: the host keeps the decisions and the device does the
arithmetic of the work the host chose. The radiance cache places probes adaptively, scrolls its
clipmaps, invalidates, and selects with the progress-guaranteeing scheduler; `GpuRadianceCache`
gathers exactly the selection `RadianceCache::select` returns. The acceleration service keeps its
budget, refit-against-rebuild rule, eviction and the instances a top level traces;
`DeviceStructures` builds exactly what it is told through `rt::StructureMirror`. So the host path is
the oracle by construction, and "consumers SHALL NOT build, refit, or own acceleration structures"
holds on a device because the mirror has no decision to make.

## Acceleration structures: owned storage, owned scratch, owned instance records

A structure owns the buffer it lives in (registered in the device's buffer pool, so a render graph
pass can name it), a scratch sized for the larger of a build and an update, and — for a top level —
host-visible instance records the backend writes in its own format at record time. A build declares
`AccelerationStructureBuildWrite` on the storage, a top-level build `AccelerationStructureBuildRead`
on each bottom level it instances, a trace `ComputeAccelerationStructureRead` on the top level. The
graph turns a whole-buffer hazard into a global memory barrier, which also orders the scratch.

Two modules then share a buffer in one graph, so `RenderGraph::import_buffer_once` imports a handle
once: two imports of one handle are two resources and NO barrier between them.

## The device's top level: build, update, or nothing

Each frame the service hands the mirror its top level. Against the last one the device built: the
same instances over the same bottom levels with nothing rebuilt below is an UPDATE when a transform
moved or a bottom level was refit and NOTHING when neither did; anything else is a BUILD. A bottom
level built this frame forces a build because its contents changed under every instance of it.

A hit needs a normal for the surface-cache lookup, so the device keeps what `trace_instance` reads:
the vertex and index pools the builds read, and per traced instance its offsets into them and its
normal matrix's columns, multiplied in the order the host multiplies them.

## The gather: one workgroup per probe, summed in ray order

A probe's rays are independent and the host only SUMS them in order. One lane per ray traces; lane 0
then encodes them in ray order, so the payload's float sums are the host's. One lane per probe was
1.5 ms for 64 probes of 32 rays on the RTX 5060 and is 0.14 ms this way. The frame's ray table is
`gi::probe_ray_direction` evaluated once on the host, so both sides trace bit-identical directions.

## The query: each level's window, toroidally

The host finds a query's probes in a dynamic BVH; the device walks, per clipmap level, the cells
within one and a half spacings of the query in a toroidal table of the level's window — the same
probes, summed in a different order. The table is rewritten when a probe is created or retired or
the window's centre moves.

## The screen tier: the oracle's march, minus what a pyramid proves empty

`ScreenTracer::trace` steps at fixed parameters. The accelerated march visits the same parameters
and skips a run of them only when a NEAREST-depth pyramid proves every step in it `continue`s: both
ends on screen with a pixel's margin and in front of the eye, and the run's furthest ray depth nearer
than the nearest buffer depth over every pixel it can touch. The engine's HZB keeps the furthest
depth, which proves occlusion and cannot prove this, so the screen tier builds the dual. The run
length adapts — doubled after a proven run, halved after a failed one.

## One predicate chooses the world tier

Hardware when the hardware pipelines exist (the device reports `Capability::RayTracing`), the
service is active, and the device top level is built — `HardwareTracer::available()` with a device
behind it. A device without ray queries and a profile that switched them off take one path; the
suite holds the two to the same image bit for bit.

## Floating point, and the remainder that was not one

Bounds are stage 2's: on the fraction of answers beyond a small tolerance and on the typical
difference. One finding did not fit them: `((value % modulus) + modulus) % modulus` came back from
the RTX 5060 as the unsigned remainder of a negative dividend, which equals the modulo only for a
power-of-two modulus — every stage-1 field had one. `giWrap` is unsigned throughout now.

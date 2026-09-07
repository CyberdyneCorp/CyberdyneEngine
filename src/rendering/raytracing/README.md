# `src/rendering/raytracing/` — layer 4

The renderer service that owns acceleration structures: bottom-level lifecycle and refit policy, the
top-level structure, a geometry adapter per geometry source, the ray query interface, the build
budget, and the capability gate.

**Governed by**: `ray-tracing-infrastructure`, at **Working** for M7. Task 9.4.

## The files

| File | What it holds |
|---|---|
| `geometry.h` | `GeometrySource`, `MaintenancePolicy`, `ProxyPolicy`, `BuildInput`, and the six adapters — the specification's adapter table as data |
| `query.h` | `Availability`, `QueryKind`, `Consumer`, `RayHit`, `RayQuery`. The interface every consumer sees |
| `acceleration.h` | `AccelerationService`: declare, instance, budget, `update()`, `trace()`, and the diagnostics |

## The one thing to read before anything else: where the rays actually go

**This service executes its queries on the CPU**, over the `cy::Bvh` structures it builds. The
interface is the deliverable and it is what the GI tiers consume as their hardware tier — but the
device half does not exist at M7, and two checkable facts decide that:

* `cy::rhi::Capability::RayTracing` is an enumerator that **nothing sets**. Grep
  `src/backends/rhi/vulkan/src/vulkan_instance.cpp` for `capabilities_.set(` — every capability the
  Vulkan backend reports is in that block and `RayTracing` is not among them. So every device this
  engine can open reports no ray tracing, including the RTX 5060 in the machine this was written on.
* `tools/layercheck/layercheck.py`'s `gpuapi` rule allows a Vulkan header only under
  `src/backends/`. Layer 4 cannot name `VK_KHR_acceleration_structure` even if the RHI grew the API.

The consequence is not hidden, it is the default: **on this tree the service is `Unsupported`, and
every consumer runs its software fallback.** That is the same state the specification requires of a
device without the extension, which is why `rendering-global-illumination`'s software tier is the
path that actually runs here and the one `src/rendering/gi/tests/` measures.

A device backend arrives as two edits and no interface change: the RHI reports the capability, and
`ServiceConfig::device_supports_ray_tracing` is set from it.

## Four things worth knowing before changing anything here

**A geometry does not own a material; the instance does.** A hit resolves to the geometry's
per-triangle material where the mesh declares one and to `InstanceDescriptor::material_id`
otherwise. There is no third source, deliberately: a mesh shared between two instances with
different materials is the ordinary case, and a geometry-level default beside the instance's is a
second identity that disagrees with the GPU material table exactly there.

**An instance without a structure is excluded, never waited for.** The build budget is what turns a
residency spike into two coarse frames instead of a hitch, and `FrameReport::excluded_instances` is
how many instances that cost this frame. A service that blocked would be the defect the budget
exists to prevent.

**Disabled and unsupported must be indistinguishable, and it is a test rather than an intention.**
`tests/test_capability.cpp` runs one script through three services and compares an `Observation`
struct field for field. It also asserts that the *available* service differs — without that control
the case would pass on a service that never traced anything at all.

**The refit is a rebuild of the same tree at refit cost.** `cy::Bvh` is built, not refit, so a pose
change re-runs the build over moved positions. That is what a driver does underneath the update
flag, it keeps the traced result exact rather than approximate, and the *policy* — refit until
bounds growth passes the adapter's quality floor, then rebuild — is what the specification asked
for and is what is implemented.

## What it does not depend on

No device, no render graph, no shader, and no Vulkan header — see above; that is the layer rule
rather than a simplification. `cy::core-jobs` is linked for one function, the monotonic clock that
stamps the top-level rebuild time the diagnostics requirement asks for.

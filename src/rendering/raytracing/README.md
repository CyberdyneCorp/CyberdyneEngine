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
| `capability.h` | `service_config_for()` and `ray_tracing_refusal()` — the one place an `rhi::DeviceCapabilities` becomes this service's configuration. M11.c task 2.6 |

## The one thing to read before anything else: where the rays actually go

**This service executes its queries on the CPU**, over the `cy::Bvh` structures it builds. That was
true at M7 for two reasons and **M11.c closed the first of them**:

* ~~`cy::rhi::Capability::RayTracing` is an enumerator that nothing sets~~ — **it is set now.** The
  Vulkan backend observes `VK_KHR_acceleration_structure`, `VK_KHR_ray_query` and
  `VK_KHR_deferred_host_operations`, asks the device for the `accelerationStructure` and `rayQuery`
  features, enables them, and records all six answers in `rhi::RayTracingObservation`;
  `DeviceCapabilities::set_ray_tracing_observation` derives the capability from them. The RTX 5060
  this was written on reports `RayTracing=1` — `render.ray_tracing_capability` prints the six answers
  and requires the bit to agree with them, and `unit.rhi` drives the derivation with no device at
  all. **This is a VULKAN-ONLY claim**: Metal and D3D12 are `rhi-and-render-graph`'s, which is
  M11.d's row, and the ledger's criterion says so in its own words.
* `tools/layercheck/layercheck.py`'s `gpuapi` rule allows a Vulkan header only under
  `src/backends/`. Layer 4 cannot name `VK_KHR_acceleration_structure` even if the RHI grew the API —
  **and this one still stands**, which is why `capability.h` takes a `rhi::DeviceCapabilities` and
  not a device.

`capability.h` is the second of the "two edits and no interface change" this README has promised
since M7: `service_config_for()` is the one expression that turns the capability into
`ServiceConfig::device_supports_ray_tracing`, and `ray_tracing_refusal()` says which of the device's
six answers stopped it when the capability is false.

**WHAT HAS NOT CHANGED, AND IT IS THE HALF THAT MATTERS FOR AN IMAGE.** The queries still execute on
the processor. What the capability decides is whether the service reports `Available` on a device
that can trace, instead of reporting `Unsupported` on hardware that has the extension; acceleration
structures built by the driver and ray queries issued from a shader are not in this tree. A default
`ServiceConfig` still reports `Unsupported`, so every suite that does not ask a device still measures
the software tier — which is what `src/rendering/gi/tests/test_fallback.cpp` does and why its numbers
did not move.

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
rather than a simplification. It does depend on `cy::rhi` since M11.c, which is the INTERFACE and not
a device: `capability.h` reads a capability record a backend filled in and cannot open, create or
submit anything. `cy::core-jobs` is linked for one function, the monotonic clock that
stamps the top-level rebuild time the diagnostics requirement asks for.

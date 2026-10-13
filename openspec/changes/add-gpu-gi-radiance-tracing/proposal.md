# Put the radiance cache and the tracing tiers on the device

## Why

Issue #35 runs CyberGI on the GPU in five stages, each replacing a host subsystem behind its seam
and keeping the host version as the oracle. Stages 1 and 2 put the distance field, the surface
cards and the card lighting on the device. The radiance cache — the probes every resolve reads — and
every tracing tier were still host arithmetic, and the hardware tier was not hardware at all:
`cy::rendering-raytracing` traced `cy::Bvh` on the processor because no backend built an
acceleration structure. This change is stage 3.

## What changes

- **Acceleration structures in the RHI.** `cy/backends/rhi/acceleration.h`: bottom and top levels,
  a build or an in-place update recorded into a command buffer, a descriptor kind a ray query reads,
  three access intents the render graph derives barriers from, and `DeviceDescription::
  request_ray_tracing`. The Vulkan backend implements it over `VK_KHR_acceleration_structure`; every
  other backend inherits a refusal (`ErrorCode::Unsupported`) from `rhi::Device`, so a device without
  ray queries refuses by name. D3D12 and Metal refuse a layout that binds a structure.
- **The service's structures on the device.** `rt::StructureMirror` in `acceleration.h`: the service
  tells a mirror every build, refit, eviction and top level it decided. A new module
  `cy::rendering-raytracing-gpu` (`DeviceStructures`) builds what it is told — a refit is an in-place
  update of the bottom level, a moved instance an update of the top level, a still frame nothing.
- **The radiance cache on the device.** `gi::ProbeGatherBackend`, `RadianceCache::select` (the
  scheduler lifted out of `update` unchanged), `RadianceCache::apply` and a probe `revision`, as stage
  2 did for the surface cache. `GpuRadianceCache` gathers the host's selection on the device, one
  workgroup per probe, through the software tier (the uploaded field) or the hardware tier (ray
  queries), every hit resolved through the device surface cache; and answers
  `RadianceCache::sample` on the device over a toroidal layout of each clipmap level.
- **The tiers on the device.** `GpuTierTracer`: the screen tier against a nearest-depth pyramid and
  last frame's colour (bit-identical to the unaccelerated march), the software and hardware world
  tiers, and `TieredTracer`'s escalation with its budget.
- **Two defects found by running it.** The page table's toroidal wrap was a signed remainder the
  RTX 5060's driver answers as unsigned for a negative dividend — harmless for a 16-brick window,
  wrong for any other — and the device scene's host-written buffers lived in system memory, which a
  march read across the bus per step. Both fixed, the first with a regression case.

## Scope

Stage 3 only. NOTHING IS COMPOSITED INTO THE FRAME: no frame the tree draws reads anything the new
code writes, and `cy/frame.slang` is untouched. Not in this change: the per-pixel resolve, the
denoiser transcription and the composite (stage 4); the device cost in `GiBudget` (stage 5); DXR
and Metal acceleration structures; per-frame staging for frames in flight.

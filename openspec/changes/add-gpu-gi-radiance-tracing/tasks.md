# Tasks

- [x] Add acceleration structures to the RHI (`acceleration.h`): descriptions, builds and updates, a descriptor kind, three access intents, the inherited refusal, `request_ray_tracing`.
- [x] Implement them in the Vulkan backend (`vulkan_acceleration.cpp`); make D3D12 and Metal refuse a layout that binds one; reflect `OpTypeAccelerationStructureKHR`.
- [x] Add `RenderGraph::imported` and `import_buffer_once`, and import the device GI buffers once per graph.
- [x] Add `rt::StructureMirror` to the acceleration service and `cy::rendering-raytracing-gpu` (`DeviceStructures`): builds, in-place refits, top-level update or build or nothing, eviction.
- [x] Add `gi::ProbeGatherBackend`, `RadianceCache::select`, `apply`, `collect`, a probe `revision`, `probe_ray_direction`, and `IlluminationSystem::set_probe_gather`.
- [x] Add `GpuRadianceCache` (the gather, one workgroup per probe; the query) and `GpuTierTracer` (screen with the nearest-depth pyramid, software, hardware, escalation with its budget), and the shaders `gi_probe_common.slang`, `gi_radiance.slang`, `gi_hardware.slang`.
- [x] Regenerate `gi_gpu_spirv.h` and `gi_gpu_msl.h` through `regenerate.py`; `just build-shaders --strict src samples` reports `target_refusals=0`.
- [x] Write `render.gi_gpu_tracing`, and the headless halves in `unit.rhi`, `unit.render_graph`, `unit.render_raytracing` and `integration.render_gi_pipeline`.
- [x] Fix the toroidal wrap (unsigned arithmetic) with `render.gi_gpu`'s non-power-of-two window as the regression, and put the device scene's host-written buffers in device-local memory.
- [x] Check the host GI with no backend is byte-identical to main (`evidence/off-reference.txt`).
- [x] Prove every device case red by a mutation (`evidence/falsification.txt`).
- [x] Update the READMEs, `docs/guides/global-illumination.md`, the requirements map, and validate this change with `--strict`.
- [ ] Stage 4: the per-pixel resolve, the denoiser transcription, per-frame staging and the composite (issue #35).
- [ ] Stage 5: the device cost in `GiBudget` and the arbiter's ladders (issue #35).
- [ ] DXR and Metal acceleration structures behind the same RHI vocabulary.

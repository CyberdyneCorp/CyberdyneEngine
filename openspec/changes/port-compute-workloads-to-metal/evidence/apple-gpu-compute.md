# Apple GPU compute validation

Validated on 2026-09-20 on a physical MacBook Pro with an Apple M3 Pro GPU (18 GPU cores), macOS
27.0 build 26A428, Metal 4. Both suites used the native Metal RHI, MSL produced from the same Slang
sources as the Vulkan SPIR-V, Metal argument buffers, and the render graph's compute recording.

| Workload | Result | Measured comparison |
|---|---:|---|
| GPU skinning | 6 cases, 454 assertions | Matrix worst position delta 1.19209e-7; dual quaternion plus blend shapes 8.34465e-7; packed-frame directional delta 8.63133e-5 |
| GPU VFX | 7 cases, 1,300 assertions | 502 live particles compared; worst relative attribute delta 8.345e-7; colour delta 0/255; 368 sorted particles with 0 inversions |

The VFX run also exercised reset, compaction, GPU-authored indirect dispatch, spawn/update, count-cap
degradation, kill accounting, sorting, event bounds, and the graphics-queue fallback for a device
with no separate asynchronous compute queue. Both suites exited successfully with no skipped cases.

A second build disabled Vulkan entirely (`CY_RENDERER_VULKAN=OFF`, `CY_RENDERER_METAL=ON`). The two
Metal suites remained registered and passed, proving their registration and link closure do not
depend on the Vulkan backend.

```sh
ctest --test-dir build/dev -R '^(render\.skinning_metal|render\.vfx_gpu_metal)$' \
  --output-on-failure
```

This proves the shared engine passes on a physical Apple GPU. It does not yet claim that the final
iPhone presentation scene submits both workloads; that integration has its own device measurement.

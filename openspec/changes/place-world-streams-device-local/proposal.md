# Place the world's streams in device-local memory, and report that memory on Vulkan

## Why

`m11a:world-budget-on-a-device` failed about one run in three (#77). #109 traced the frame's growth
from about 10.8 to 13 ms to water shading (#25): its refraction and reflection pictures redraw the
scene, and `--no-water-shading` takes `stage_submit_ms` from 7.0 back to 4.6 ms. It also found that
the added cost does not change with resolution, so the cost is geometry and not fill.

The device's own timestamps say which geometry cost. `samples/10-world` keeps its vertex, colour
and index streams in `MemoryUse::Upload`. On a discrete GPU that is system memory. The streams are
about sixteen megabytes: the terrain's positions and colours, and eleven megabytes of plant
proxies, sky, stars and sea written every frame. The opaque frame, the refraction picture and the
reflection picture each pulled them across the bus. On the RTX 5060 the frame spent 4 to 5 ms on
the device, 2.0 to 2.7 ms of it in the two water pictures. Leaving the reflection's plants out
saved about 1 ms and leaving the refraction out about 0.4 ms. Putting the same streams in
device-local memory took the frame's device time to 0.35 ms after submission, with no pixel
changed.

The engine already has the memory for this: `MemoryUse::HostVisibleDeviceLocal`, and
`Capability::HostVisibleDeviceLocalMemory` to ask whether a device offers it. The Vulkan backend
never set the capability, so every Vulkan device reported it absent. That includes the RTX 5060,
which lists a device-local, host-visible, host-coherent memory type. This is the same shape of
defect `Capability::RayTracing` had before M11.c.

## What Changes

- `rhi::MemoryObservation`, `device_offers_host_visible_device_local()` and
  `DeviceCapabilities::set_memory_observation()`. The Vulkan backend counts its device's memory
  types: all of them, the device-local, host-visible and host-coherent ones, and the host-visible
  ones that are not device-local. The capability is derived from those counts.
- `samples/10-world/geometry_memory.h`: `geometry_memory()` answers `HostVisibleDeviceLocal` where
  the device reports the capability, and `Upload` everywhere else. The stage creates its six
  geometry streams with it. Asking first is not optional: D3D12 places `HostVisibleDeviceLocal` in
  a default heap the processor cannot map, and every stream here is written through a mapping.
- The stage times the frame on the device. It writes four timestamps: when the first pass begins,
  around the water's two pictures, and after the read-back. It reports `gpu_ms` and `water_gpu_ms`
  per frame in the budget CSV, and the take prints a `device time` line. A device without timestamp
  queries reports nothing rather than zero.
- No shader, pass, draw or picture changes. The budget, the frame count and the criterion stay as
  they were.

## Impact

- `src/backends/rhi/`: capabilities header, `types.cpp`, the Vulkan backend's capability fill. No
  ABI change: the C ABI exposes no RHI capability.
- `samples/10-world/`: `stage.{h,cpp}`, `main.cpp`, `geometry_memory.h`.
- Tests: `unit.rhi` (`test_memory_capability.cpp`) and the new `render.geometry_memory`.
- Docs: `samples/10-world/README.md`, `tests/render/README.md`, `src/backends/rhi/README.md`,
  `tools/roadmap/requirements-coverage.toml`.

# Tasks

## 1. Find the cost

- [x] 1.1 Remove each water pass, and the reflection's plants and terrain, one at a time.
  Reflection about 2 ms of `stage_submit_ms`, its plants about 1 ms, refraction about 0.4 ms.
- [x] 1.2 Split `stage_submit_ms` into recording and waiting. The wait for the device was 4.2 ms,
  and recording and submission took 0.06 ms.
- [x] 1.3 Move the stream buffers to `HostVisibleDeviceLocal`: the wait fell to 0.35 ms, and all
  64 frames stayed byte-identical.
- [x] 1.4 Find why nothing asked for that memory: the Vulkan backend never set
  `Capability::HostVisibleDeviceLocalMemory`, and D3D12 answers the request with an unmappable heap.

## 2. Fix

- [x] 2.1 `MemoryObservation`, `device_offers_host_visible_device_local()`,
  `set_memory_observation()`; the Vulkan backend counts its memory types.
- [x] 2.2 `geometry_memory()`; the stage creates its six streams with it.
- [x] 2.3 Device timestamps: `gpu_ms` and `water_gpu_ms` in the CSV, and the `device time` line.

## 3. Tests that fail without the fix

- [x] 3.1 `unit.rhi`: the derivation, with each count removed in turn.
- [x] 3.2 `render.geometry_memory`: the capability agrees with the driver's memory types; the
  stage's choice reads a world-sized stream three times in at most half the time `Upload` takes.
- [x] 3.3 Mutations, each seen red and restored md5-verified (`evidence/falsification.txt`).

## 4. The picture and the budget

- [x] 4.1 All 64 frames byte-identical to the build before, with water shading on and off
  (`evidence/frame-identity.txt`).
- [x] 4.2 The criterion's command line, alternated with the build before on the same quiet host
  (`evidence/budget.txt`).

## 5. Records

- [x] 5.1 READMEs: `samples/10-world`, `tests/render`, `src/backends/rhi`.
- [x] 5.2 `tools/roadmap/requirements-coverage.toml`.
- [x] 5.3 `openspec validate place-world-streams-device-local --strict`.

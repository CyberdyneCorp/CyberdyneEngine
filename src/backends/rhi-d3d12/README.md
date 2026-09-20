# `src/backends/rhi-d3d12/` — native Direct3D 12 backend

This module implements `cy::rhi::Device` directly over Direct3D 12 and DXGI. It is built by
default on Windows through `CY_RENDERER_D3D12=WINDOWS`; other platforms neither compile nor link
Windows SDK types.

## Adapter selection and evidence

Adapter selection uses the reported adapter name and PCI vendor ID. The DXGI software flag is
retained in the report but never decides whether an adapter is hardware. This is deliberate:
GitHub's `windows-2022` image exposes `Microsoft Basic Render Driver` as adapter zero without
`DXGI_ADAPTER_FLAG_SOFTWARE`. The backend classifies that identity as software and falls back to
the adapter returned by `EnumWarpAdapter` when no known hardware vendor is available. Unknown
vendors remain `unknown` rather than being promoted to hardware.

`integration.rhi_d3d12` prints the identity, vendor, derived class, DXGI flag and resource-heap tier
of the device that answered. `unit.rhi_d3d12` keeps the device-free Tier 1/Tier 2 heap policy under
the unit budget. A WARP run establishes that the API path compiles, creates a device, records under
the debug layer, draws and reads back. It is not hardware evidence.

## Memory

The backend uses engine-owned allocation code and adopts no additional dependency. Persistent
resources begin as committed allocations. Render-graph transients use placed resources in an
`ID3D12Heap`, which is the path where aliasing matters.

`MemoryPoolClass` carries the Resource Heap Tier rule without exposing D3D12 above the backend:

- Tier 2 reports one class for buffers and textures, allowing the graph to alias them when their
  lifetimes do not overlap.
- Tier 1 reports separate classes for buffers, non-render-target textures and render/depth
  textures. Their meet is empty, so the graph refuses an illegal mixed heap before execution.

The Tier 1 classification is covered without a device. Executing that path remains a named
hardware deferral because hosted Windows currently reports Tier 2.

## Descriptors and shaders

Classic sets map to shader-visible CBV/SRV/UAV and sampler heaps. Pipeline layouts become root
signatures with one descriptor table per populated heap and set. Resource binding Tier 3 enables
the engine's 16,384-slot global bindless texture table; lower tiers report the compatibility model.

The native shader format is DXIL. `samples/03-first-light/shaders/first_light_dxil.h` is generated
from the same Slang source as the SPIR-V and MSL payloads, using Slang 2026.9.2 and its pinned DXC
v1.9.2602 downstream compiler. The Windows workflow regenerates the bytes before compiling the
sample, which makes a stale or invalid payload fail before the golden image is judged.

## Synchronisation and command recording

The render graph remains the only owner of barriers. Its engine-side `ImageUse` and `AccessFlags`
are translated to D3D12 resource states in `command_buffer.cpp`. Queue timelines, binary
semaphores and fences use `ID3D12Fence`; render, compute and copy work use native command lists.

`Capability::ParallelPassRecording` is false. D3D12 can record direct command lists concurrently,
but the RHI's secondary form spans complete passes while graph barriers are recorded between them.
Claiming the capability without changing that ordering contract would be incorrect.

## Build and test

On a Windows developer prompt:

```powershell
cmake --preset dev -D CY_RENDERER_D3D12=ON
cmake --build --preset dev --target cy_rhi_d3d12 cy_test_unit_rhi_d3d12 cy_test_integration_rhi_d3d12 cy_test_render_golden_backends
ctest --test-dir build/dev -C Development -R "^(unit.rhi_d3d12|integration.rhi_d3d12|render.golden_backends)$" --output-on-failure
```

Set `CY_GOLDEN_CAPTURE_DIR` and `CY_GOLDEN_LEDGER` when running `render.golden_backends` to retain
the rendered PNG and the device-labelled result. A missing reference is an error; the suite never
regenerates it.

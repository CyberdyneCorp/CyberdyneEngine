# implement-m11d5-backends

M11.d.5 of the roadmap, **inserted between M11.d and M11.e by M11.d's own spike**: Metal native
rather than a translation layer, D3D12 from nothing, and the one claim no single leg of the matrix
can make — the M3 golden images matching across Vulkan, Metal and D3D12 within tolerance, with the
device that answered each named in the result.

It exists because the machine does not match the plan: this project works on a Linux host with one
GPU vendor and no Apple toolchain, so neither backend can be compiled where M11.d is worked, let
alone judged. The sections moved rather than being deleted or descoped, and
`golden-images-across-three-backends` moved with them so this rung cannot close on "it compiles
somewhere".

## Windows handoff — 2026-09-20

Continue from branch `feat/close-m11d5` and draft PR #2. The native D3D12 backend now compiles on
`windows-2022`; its device-free heap-tier suite and its real-device integration suite both pass
with the D3D12 debug layer enabled. Run
[`35532341415`](https://github.com/CyberdyneCorp/CyberdyneEngine/actions/runs/35532341415)
identified the hosted adapter as `Microsoft Basic Render Driver` (`0x1414`, software) from its
identity rather than the unreliable `DXGI_ADAPTER_FLAG_SOFTWARE` bit.

The remaining Windows blocker is narrow and observable. The first-light golden test reaches
`CreateGraphicsPipelineState`, where the hosted WARP device rejects the generated vertex shader:

```text
ID3D12Device::CreateVertexShader: Shader must be vs_6_2 ... Shader version provided: vs_6_6.
```

The first-light workflow currently compiles its three shaders as `sm_6_6` in
`.github/workflows/m11d5-dxil.yml`. Do not lower the engine-wide Shader Model 6.6 floor: virtual
geometry needs it. Compile this compatibility scene at the highest shader model supported by the
selected adapter, or provide a 6.2 first-light payload for the WARP evidence path. Keep the hardware
path at 6.6 and record the actual adapter name, vendor, shader model and device class.

Use these checks on the Windows GPU machine:

```powershell
cmake --preset dev -DCY_RENDERER_D3D12=ON -DCY_RENDERER_VULKAN=OFF -DCY_RENDERER_METAL=OFF
cmake --build --preset dev --config Development --target cy_rhi_d3d12 cy_test_unit_rhi_d3d12 cy_test_integration_rhi_d3d12 cy_test_render_golden_backends
ctest --test-dir build/dev -C Development -R "^(unit.rhi_d3d12|integration.rhi_d3d12|render.golden_backends)$" --output-on-failure
```

Set `CY_GOLDEN_CAPTURE_DIR` and `CY_GOLDEN_LEDGER` before the last command. A valid result contains
`row backend=d3d12 ... outcome=matched`, writes the D3D12 PNG, and has no debug-layer error. Copy the
PNG and manifest to `docs/design/images/m11d5-three-backends-d3d12.*`, then run:

```text
just test-render --compare-backends vulkan metal d3d12
```

The committed Vulkan image came from an NVIDIA GeForce RTX 5060. The committed Metal image came
from a physical Apple M3 Pro and matches the reference with maximum raw channel delta 1. Finish by
proving the golden criterion can fail through a visible scene mutation, restoring it to green, and
recording both runs. D3D12 hardware evidence must name the physical adapter; WARP evidence remains
software evidence and cannot close the hardware deferral.

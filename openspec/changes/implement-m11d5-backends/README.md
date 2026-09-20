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

## Physical Windows evidence — 2026-09-20

The hardware deferral is now closed. `render.golden_backends` was run on an AMD Radeon RX 6900 XT
(RDNA 2, vendor `0x1002`, class `hardware`) with `CY_RENDERER_D3D12=ON` and Vulkan and Metal off.
The D3D12 debug layer stayed silent through `unit.rhi_d3d12`, `integration.rhi_d3d12` and the
golden capture, and the ledger row is
`row backend=d3d12 ... class=hardware outcome=matched ... max_delta=1`. The first-light shaders
were compiled at the engine-wide `sm_6_6` floor rather than the hosted-WARP `sm_6_2` compatibility
payload, using `slangc 2026.9.2` with `dxcompiler.dll` / `dxil.dll` from `dxc 1.9.2602`, and
embedded into `first_light_dxil.h` the same way the CI workflow does.

`tools/ci/compare_backend_goldens.py vulkan metal d3d12` is green across three vendors:

```text
vulkan: matched on "NVIDIA GeForce RTX 5060" (hardware, NVIDIA); max channel delta 0
metal:  matched on "Apple M3 Pro"           (hardware, Apple);  max channel delta 1
d3d12:  matched on "AMD Radeon RX 6900 XT"  (hardware, AMD);    max channel delta 1
vulkan versus metal: within tolerance;  max channel delta 1
vulkan versus d3d12: within tolerance;  max channel delta 1
```

The adversarial pass required by §7.2 was performed on the D3D12 leg: scaling
`sun_.color` from `* 1.05F` to `* 1.25F` in `samples/03-first-light/scene.cpp` turned the ledger
row from `matched` to `differed` (`10745 texels, 3242 off_edge, max_delta=17`); restoring the
scalar returned it to `matched` and produced a byte-identical PNG to the committed evidence.

**Committed `first_light_dxil.h` was regenerated as part of this closure.** The previous package
was written in `d6f6104 "Package validated first-light DXIL"`, before
`c657b8f "Narrow the first-light shadow input signature"` modified the source. The header shipped
in the repo therefore compiled from a shadow-vertex input layout the runtime no longer emits.
The refreshed payload is compiled from the current `first_light.slang` at `sm_6_6`, matches the
runtime signature, and is what produced the matching D3D12 golden image above. Hosted WARP CI is
unaffected: `.github/workflows/m11d5-dxil.yml` regenerates the header at `sm_6_2` in its own
`Compile and embed DXIL` step before the build.

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

The first-light workflow now compiles this compatibility scene as `sm_6_2` in
`.github/workflows/m11d5-dxil.yml`, which is the highest model the hosted device accepts. This does
not lower the engine-wide Shader Model 6.6 floor: virtual geometry needs it, and the checked-in
physical-hardware payload remains 6.6. The physical Windows run must record the actual adapter name,
vendor, shader model and device class.

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

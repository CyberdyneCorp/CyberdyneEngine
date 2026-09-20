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

## Handoff to M11.e

M11.d.5 now has native Vulkan, Metal and D3D12 implementations rendering the same committed
scene on NVIDIA, Apple and AMD hardware. The following hardware questions were declared before
implementation and are now resolved: Apple-family memoryless/tile behavior, Tier 2 argument
buffers, and hardware golden parity. Their device-labelled evidence remains in the committed
manifests and backend suites.

One backend risk remains and is carried forward explicitly: **D3D12 Resource Heap Tier 1 execution
has not run on a Tier 1 device**. Hosted WARP and the physical Radeon RX 6900 XT both report Tier 2.
The allocator policy is implemented and its device-free regression test requires Tier 1 to separate
buffers, ordinary textures and render/depth textures. Re-enter when a Tier 1 adapter is available,
or when a D3D12 validation forcing mode can exercise the native placed-resource path. Do not infer
Tier 1 execution from the policy test.

The rung's gate remains open. A clean macOS build with Metal and D3D12 disabled succeeds, but its
required `test-all` run currently reports 12 integration failures and 4 smoke failures in inherited,
non-backend suites. The failures
include Python 3.9 selecting code that requires `tomllib`, Vulkan device tests registering on a Mac
without a Vulkan device, macOS crash-report module discovery, Swift/XCTest discovery, and unrelated
math, denoise, networking, asset and editor-play cases. Task 4.3 and the full §7 gate stay unchecked
until the renderer-off configuration passes its whole suite; the capability status therefore stays
Working and `milestone-m11d5` stays `joins-on-close`.

M11.e receives no missing Metal or D3D12 implementation task from this rung. It receives
the Tier 1 hardware exercise above, plus its own mobile, distribution, dependency and final-record
work already listed in `implement-m11e-ship/tasks.md`.

The other team's four review questions have concrete answers:

- Metal evidence came from a physical Apple M3 Pro. Hosted macOS evidence is not used for the
  Apple-family, memoryless, Tier 2 argument-buffer or hardware-parity claims.
- `cy::pcg::ExecutionDomain` still has no `Gpu` enumerator. The existing GPU conformance path does
  not satisfy the execution-domain requirement; M11.e still owes that API and its two-vendor proof.
- The PCG agreement gate needs a joint self-hosted Apple/NVIDIA artifact exchange. Hosted runners
  cannot supply either physical-device leg.
- `cross-compilation-works` and `porting-surface-against-a-real-non-desktop` in `m11e.toml` both run
  `just build-ios simulator`. They state different claims but currently collect identical evidence,
  so the latter needs its own porting-surface assertion before the M11.e gate can close.

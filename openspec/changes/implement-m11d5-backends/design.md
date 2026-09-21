# Design: M11.d.5 — Backends

## 1. The evidence this rung is written against, inherited from M11.d's spike

This rung runs no spike. M11.d's ran, on 2026-09-19, and its second question produced this rung.
`openspec/changes/implement-m11d-desktop/design.md` §1.4 carries it in full; what follows is the part
that shapes what this rung may claim.

**Method, stated because it decides how strong the evidence is.** A throwaway GitHub Actions workflow
on `spike/m11d-device-probe`, whose tree was *only* the workflow and two probe programs, so pushing it
ran this and none of `ci.yml`. Each probe **creates a device, clears a 64×64 target to a known colour,
reads the pixel back, and presents.** A frame that happened, not a manifest saying an SDK is
installed. Reproduced identically across two runs (`35438833469`, `35439163646`); the branch was
deleted and the logs kept.

| Leg | Device | Drew | Presented | What kind |
|---|---|---|---|---|
| `macos-14` (arm64) | `MTLDevice` **yes** | pixel exact | `CAMetalLayer` headless, `presentDrawable` clean | **"Apple Paravirtual device"** |
| `macos-13` (x86_64) | — | — | — | **no runner was ever allocated**, on either run |
| `windows-2022` (x64) | `ID3D12Device` **yes** | pixel exact | swapchain on a hidden HWND, `hr=0` | **"Microsoft Basic Render Driver"** — software |
| `windows-11-arm` | `ID3D12Device` **yes** | pixel exact | `hr=0` | same, software |

Four readings, each of which changes what a criterion in this rung is allowed to say:

- **The macOS device reports no Apple GPU family at all** — `Apple7=0`, `Apple8=0`, `Metal3=0`,
  `Mac2=1`. **Tile memory and memoryless attachments are Apple-family features**, and they are
  `rhi-and-render-graph`'s entire stated reason for refusing MoltenVK as the long-term Apple strategy.
  *The thing a native Metal backend is for cannot be exercised on any hosted runner.*
- **Argument buffers are Tier 1** there. The engine's descriptor model is bindless and the bindless
  path needs Tier 2, so `integration.rhi_metal` can pass on that leg **without the descriptor model having
  been exercised once**.
- **There is no hardware GPU on any hosted Windows image**, and the trap is sharper than "it is
  WARP": the probe found **two** adapters, both `Microsoft Basic Render Driver`, and **adapter 0 does
  not set `DXGI_ADAPTER_FLAG_SOFTWARE`**. The ordinary "pick the first adapter without the software
  flag" selects a software rasteriser. A backend that trusts that flag reports hardware it does not
  have, and so will its golden images.
- `isDepth24Stencil8PixelFormatSupported = 0` on the Metal device, confirming gap 7's Metal claim
  first-hand; `ResourceHeapTier = 2` on the Windows device, which is a *hosted* answer to a question
  only *hardware* can answer, since Tier 1 hardware still ships.

### 1.1 Apple-hardware evidence gathered during implementation

The implementation machine is an Apple M3 Pro. Its native device reports Apple GPU family support
and argument buffers Tier 2, so the two Metal deferrals above can be answered here even though a
hosted runner cannot answer them. The backend suite reports that identity and tier, exercises a
memoryless attachment, aliases a texture and buffer at offset zero of one placement heap, and reads
a sampled colour through the device-owned 16,384-slot argument buffer.

The shader toolchain required one measured correction. Slang 2026.9.2 emitted the existing
runtime-sized global texture array as a direct MSL parameter; Apple's `metal` compiler rejected it
because the flexible texture array was not the final struct member and carried an invalid address
space. A fixed-capacity `ParameterBlock<T>` produced one argument-buffer parameter, compiled to both
AIR and a metallib, and retained a descriptor set for SPIR-V. The permanent Slang fixture and the
native runtime test now cover the compile-time and shader-readable halves separately.

The M3 first-light scene now runs through the same renderer and committed reference as Vulkan. Its
native MSL groups each descriptor set into a Slang `ParameterBlock`, matching Metal argument-buffer
slots; vertex streams occupy a disjoint native buffer range and push constants follow the set
buffers. On the M3 Pro the captured Metal image has zero differing texels against
`tests/render/references/first_light.png` (maximum raw channel delta 1), and the generated ledger
names `Apple M3 Pro`. The capture and ledger are committed under `docs/design/images/`.

## 2. Hardware claims and their re-entry points

Four hardware questions were declared before implementation rather than expressed as criteria a
hosted leg could satisfy vacuously. Three were later retired by physical Apple, NVIDIA and AMD
evidence; one remains deferred. `delivery-roadmap` requires deferred scope to name what is unmet,
why, and the condition that brings it back.

| Hardware question | Current status | Evidence or re-entry point |
|---|---|---|
| **Apple-family tile memory and memoryless attachments** | **Closed** | The Apple M3 Pro suite exercised memoryless attachments and explicit-placement heap aliasing on an Apple-family device. |
| **Argument buffers at Tier 2** | **Closed** | The same M3 Pro reported Tier 2 and sampled through the 16,384-entry global texture table in a native compute dispatch. |
| **Golden-image parity against hardware references** | **Closed** | Vulkan on NVIDIA RTX 5060, Metal on Apple M3 Pro and D3D12 on AMD Radeon RX 6900 XT all matched the same reference and each other with maximum channel delta 1. |
| **D3D12 Resource Heap Tier 1** | **Deferred** | Hosted WARP and the physical AMD device report Tier 2. Re-enter on a Tier 1 device, or through a validation forcing mode if one becomes available. The device-free policy test already requires Tier 1 to split buffers, non-render-target textures and render/depth textures into incompatible pool classes. |

The remaining Tier 1 item is a hardware exercise, not missing allocator policy. Compile, create a
device, draw, present and readback are covered on hosted WARP and physical AMD hardware.

### 2.1 D3D12 memory decision

The backend uses an engine-owned placed-resource allocator over `ID3D12Heap`; it adopts no new
dependency. Persistent resources begin as committed allocations, while the render graph's
transients use the placed heap where aliasing changes peak memory. This keeps the implementation
behind the existing RHI memory report and pressure path and avoids introducing a second allocator
policy beside `MemoryPoolClass`.

Resource Heap Tier 2 reports one pool class for buffers and textures. Tier 1 reports distinct
classes for buffers, non-render-target textures and render/depth textures. The graph meets those
classes: compatible resources can share a heap, and an empty meet rejects an illegal mixed heap
before execution. The classification and refusal are device-free; executing placed resources on
Tier 1 remains the hardware deferral above.

## 3. What counts as a delivered backend, and what is reported instead

Carried across from M11.d's design §3, because the decisions did not change when the rung did.

- **Vulkan's references stay the references.** A second committed reference per backend would make
  every backend its own truth and no comparison would ever fail. Metal and D3D12 are compared against
  the images already in `tests/render/references/` with a perceptual tolerance, and the per-backend
  delta is **reported** rather than thresholded away. `Backend divergence` is already a scenario of
  `testing-and-quality`: the test fails identifying both backends, because parity is the requirement.
- **A device is named in the result. Always.** Hardware, paravirtual, or software. A green tick that
  does not say which answered is a claim about hardware nobody ran.
- **NOT EVALUATED is never a pass, and never a failure to be hidden.** A leg with no device produces
  a compile-and-validate claim and an image criterion that reports itself unevaluated with its reason.
  That is a worse outcome than a photograph and a much better one than a tick.

### 3.1 The labelling rule, which is the one thing a Linux host can judge in full

**A device's kind comes from its identity, never from a flag it sets about itself.** The spike found
adapter 0 on every hosted Windows image identifying as `Microsoft Basic Render Driver` while *not*
setting `DXGI_ADAPTER_FLAG_SOFTWARE`. Task 3.5 of M11.d's old section 3 said *"a WARP adapter is
labelled WARP in the result"*, and that is not enough on its own.

The rule this rung adopts, and the reason it is a criterion rather than a convention: the device
report carries the adapter's **reported identity string and vendor**, classifies from a table this
engine owns, and a device it cannot classify is reported **unknown** rather than assumed hardware.
The arithmetic is engine-side and needs no Metal and no D3D12, so `m11d5:device-identity-is-named`
runs, fails and is iterated on this host — which is what keeps the rung developable rather than a
place work goes to wait for a runner.

## 4. Why an insertion, and why the terminus was not renamed

`delivery-roadmap` permits a milestone to be inserted between two existing ones without renumbering
those that follow, and requires the insertion to take a **rung between its neighbours** in every
mechanism that depends on milestone order. M5.5 set the precedent and paid the cost of getting it
wrong: it was inserted without being added to the ordered list, so `criteria.rung` answered with the
length of the list and its ledger sorted to the end of the ladder.

**M11.e was not renamed, and the arithmetic is why.** Renaming the terminus would move 27
`known_gap_closes` across five ledgers, 30 criterion identifiers and 30 falsifiability entries — all
counted rather than estimated — and `falsify check` had just been driven to zero disagreements. That churn is not worth a label. `M11.d.5` is instead
the first identifier on the ladder carrying both forms at once — a letter rung from M11's split and a
`.5` from M5.5's insertion — which is exactly what it is, and which the three readers that parse a
milestone heading now admit by pattern and by name.

**What the insertion touched, listed so nothing is discovered later**: `record.MILESTONES`;
`plan.milestone_id` and the matrix-header, section-heading and load-table patterns; the matrix column
set, the `rhi-and-render-graph` row and the load table; `docs/ROADMAP.md`'s ladder, mermaid graph,
rung table and sections; `docs/roadmap/dependencies.md`'s graph; `docs/roadmap/implementing.md`;
`docs/roadmap/risks.md` entry 12; `selftest.MINIMUM_CRITERIA` and the three insertion checks;
`gates.toml`; and M11.d's own handover criterion, which named `m11e` and would otherwise have skipped
a rung.

## 5. What this rung deliberately does not do

- **No MoltenVK.** `rhi-and-render-graph` refuses it as the long-term Apple strategy by name. A
  translation layer would produce a green golden image and satisfy nothing — and §2's first deferral
  is the honest version of that refusal: the features are implemented and unexercised, rather than
  substituted.
- **No interface changes.** They are M11.d's, settled on Vulkan and null before this rung opens. If
  this rung needs one, that is a finding against M11.d's work and it is recorded as one rather than
  patched inside a backend, because a gap closed inside a backend is a gap the other two rediscover.
- **No second reference-image set per backend**, for the reason in §3.
- **No new render graph and no new pass set.** The backends execute the graph the renderer already
  produces.
- **No platform work.** The native `Platform` and `DisplayServer` are M11.d's row. This rung consumes
  `Feature::MetalSurface` and `Feature::D3D12Surface` across `DisplayServer` and puts **no platform
  `#ifdef` inside a backend**, which is `core-platform-abstraction`'s own scenario.
- **No mobile, no console, no XR.** Those are M11.e's, out of scope by specification, and deferred
  respectively.

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
  path needs Tier 2, so `unit.rhi_metal` can pass on that leg **without the descriptor model having
  been exercised once**.
- **There is no hardware GPU on any hosted Windows image**, and the trap is sharper than "it is
  WARP": the probe found **two** adapters, both `Microsoft Basic Render Driver`, and **adapter 0 does
  not set `DXGI_ADAPTER_FLAG_SOFTWARE`**. The ordinary "pick the first adapter without the software
  flag" selects a software rasteriser. A backend that trusts that flag reports hardware it does not
  have, and so will its golden images.
- `isDepth24Stencil8PixelFormatSupported = 0` on the Metal device, confirming gap 7's Metal claim
  first-hand; `ResourceHeapTier = 2` on the Windows device, which is a *hosted* answer to a question
  only *hardware* can answer, since Tier 1 hardware still ships.

## 2. What this rung may not claim, with re-entry points

Four deferrals, each recorded here rather than expressed as a criterion a hosted leg could satisfy
vacuously. `delivery-roadmap` requires deferred scope to name what is unmet, why, and the condition
that brings it back.

| Deferral | Why it cannot be claimed | Re-entry point |
|---|---|---|
| **Apple-family tile memory and memoryless attachments** | The only Metal device any hosted runner presents reports no Apple GPU family. The features do not exist on it | An Apple-silicon machine in the matrix, self-hosted or otherwise. Until then the Metal backend implements them and the claim is *compiled and exercised nowhere* |
| **Argument buffers at Tier 2** | The hosted device is Tier 1; the bindless descriptor model needs Tier 2, so a passing `unit.rhi_metal` proves nothing about it | The same machine. The backend's conformance suite SHALL report the tier it ran at, so a Tier 1 pass cannot be read as a Tier 2 one |
| **Golden-image parity against hardware references** | `tests/render/references/` are photographs taken on this project's own hardware; every hosted leg is paravirtual or software | A hardware leg per backend. Until then the comparison is run and the **delta is reported** with the device named, rather than thresholded into a tick |
| **D3D12 Resource Heap Tier 1** | Every hosted image reports Tier 2; Tier 1 hardware still ships and the allocator's partition behaviour differs on it | A Tier 1 device, or a validation-layer forcing mode if one proves to exist. The allocator SHALL be written for both tiers regardless, which is the half that does not need the device |

**None of these is a reason to skip the backend.** Compile, create a device, draw, present and read a
pixel back are all real on a hosted leg, and a backend that does those four correctly is a backend
whose remaining risk is a device away rather than a rewrite away.

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

# M11.d.5 — Backends: one scene, three backends, the same picture

## Why

**A rung cannot be judged on work that no machine it runs on can build.** M11.d was written carrying
Metal and D3D12. Its spike ran, and the second thing it established was not about graphics at all:
**this project works on a Linux host with one GPU vendor and no Apple toolchain.** Metal cannot be
compiled here. D3D12 cannot be compiled here. Neither backend could be written or judged where that
rung is being worked, and half-building them there would have produced the one defect this project
has paid for nine times — a check that cannot fail.

So the two sections **moved**. They are not deleted and they are not descoped. This is the rung they
moved to, inserted between M11.d and M11.e in the shape M5.5 was inserted between M5 and M6, and
`delivery-roadmap`'s *"A spike may resize a milestone as well as redirect it"* is the rule that
permits it: re-scoping **SHALL move whole capabilities, with their exit criteria**, and **SHALL NOT**
narrow a capability in place.

**And `golden-images-across-three-backends` moved with them**, which is the half that matters. It is
M11's first exit criterion as `docs/ROADMAP.md` has stated it since the plan was drawn, it is the one
claim no single leg of the matrix can make, and a rung that received two backends without it could
close on *"it compiles somewhere"*. The same requirement says so in as many words: *a criterion whose
subject has been deferred SHALL move with its subject; leaving it behind produces a check with
nothing to check.*

**What is verified absent on the tree this rung starts from**, rather than assumed:

- **There is no Direct3D 12 backend.** `src/backends/` holds `rhi` (with `null` and `vulkan`),
  `rhi-metal`, `audio-miniaudio`, `physics-jolt`, `shader` and `viewport`. Every `d3d12` string in
  `src/` is an enumerator in a capability, format or display-server table describing an API the
  engine does not have.
- **The Metal backend is a seed that has never been compiled.** `src/backends/rhi-metal/README.md`
  states it in its own first sentence. What the seed carries instead of a backend is a finding:
  eight gaps, three where Metal has no equivalent at all.
- **Nothing in this tree has ever compared two backends' output.** `tests/render/references/` holds
  photographs of one implementation, and `just test-render --compare-backends` does not exist.

## What this rung inherits rather than re-derives

**M11.d's spike is this rung's evidence and it is already spent.** A throwaway GitHub Actions
workflow on a branch whose tree was *only* the workflow and two probe programs — so none of `ci.yml`
ran — created a device on each leg, cleared a 64×64 target to a known colour, **read the pixel back**
and presented it. That is a frame that happened, not a runner-image manifest saying an SDK is
installed. Reproduced identically across two runs (`35438833469`, `35439163646`).

| Leg | Device | Drew | Presented | What kind |
|---|---|---|---|---|
| `macos-14` (arm64) | `MTLDevice` **yes** | pixel exact | `CAMetalLayer` headless, `presentDrawable` clean | **"Apple Paravirtual device"** |
| `macos-13` (x86_64) | — | — | — | **no runner was ever allocated**, on either run |
| `windows-2022` (x64) | `ID3D12Device` **yes** | pixel exact | swapchain on a hidden HWND, `hr=0` | **"Microsoft Basic Render Driver"** — software |
| `windows-11-arm` | `ID3D12Device` **yes** | pixel exact | `hr=0` | same, software |

**Every allocated leg has a device that draws and presents. Not one of them is a GPU.** This rung is
therefore *partly* verifiable — compile, create a device, draw, present, read a pixel back, all real
and all reportable — and the rest is declared deferrals rather than criteria a hosted leg could fake.
`design.md` §2 carries them with re-entry points; §1 carries the measurement in full.

**This rung runs no spike of its own.** Inheriting an answer produced at the head of the rung above
is what a spike at the head of a rung is *for*, and the one question left is narrower and cannot be
spiked ahead of the work: does the interface M11.d settled survive contact with two backends that
were not consulted about it?

## What Changes

- **`rhi-and-render-graph` to Complete** — the row's whole remaining gap, and its Complete cell moved
  here from M11.d with its subject. **Metal, native and not a translation layer**; **D3D12 from
  nothing**, against the interface M11.d settled on Vulkan and the null backend before either backend
  existed.
- **The device that answered is named in every result, and a software device is labelled from its
  identity rather than its flag.** The spike's sharpest finding turned into a check: every hosted
  Windows image presents **two** adapters, both `Microsoft Basic Render Driver`, and **adapter 0 does
  not set `DXGI_ADAPTER_FLAG_SOFTWARE`**. The ordinary "pick the first adapter without the software
  flag" selects a software rasteriser and reports it as hardware — and so do its golden images.
- **The three-backend comparison** — the M3 golden images across Vulkan, Metal and D3D12 within
  tolerance, between legs of the matrix, in the same shape as the cross-leg digest job M11.a built.

## Capabilities

**One row to Complete, carrying 12 requirements**: `rhi-and-render-graph`, Working since M3 over a
single backend. It is the oldest unfinished row on the ladder, and the only one whose entire gap to
Complete is a second and third implementation of an interface that is otherwise settled.

## What is contingent, and what this rung predicts about itself

- **The interface is M11.d's and it is below this rung on the ladder.** `m11d:rhi-interface-gaps-settled`
  and `m11d:null-backend-refuses-what-it-cannot-do` are inherited here rather than restated. If the
  interface turns out to be wrong, that is a finding against M11.d's work discovered by this rung,
  and it is worth more than the Complete cell — but it is a migration across every pass, which is
  precisely what settling the interface first was meant to avoid.
- **MSL and DXIL are `shader-system`'s, which is M11.c's row.** `m11d:shader-targets-emitted` is the
  prerequisite check and it stays in M11.d, inherited here. This rung cannot start without it and
  does not own it.
- **The most likely demotion is not a row; it is a claim.** No hosted leg can exercise Apple-family
  tile memory or memoryless attachments, argument buffers at Tier 2, hardware of any vendor, or D3D12
  Resource Heap Tier 1. If the golden-image comparison can only be run against paravirtual and
  software devices, this rung's artefact is a **labelled** comparison and the hardware-parity claim
  is a deferral with a re-entry point — which is the honest outcome, not a failure.
- **`build-and-packaging`, `testing-and-quality` and the core rows are M11.d's and stay there.** This
  rung absorbs no other rung's row. A rung that quietly does another rung's work is a rung whose gate
  cannot say what it is looking at, which is the reason M11 was split at all.

## Impact

- **New code**: a D3D12 backend; a Metal backend behind the seed's mapping layer; device identity and
  labelling in the RHI's device report; a cross-backend golden-image comparison.
- **Existing code**: none of the interface — that is M11.d's, deliberately settled before this rung
  opens.
- **Plan documents**: this rung is an **insertion**, so `record.MILESTONES`, the matrix column set,
  the load table, `docs/ROADMAP.md`'s ladder and the three readers that parse a milestone heading all
  gained one entry, and `roadmap-test` asserts the rung sits between its neighbours rather than at
  one end.
- **Machinery**: `milestone-m11d5` in `gates.toml` at `joins-on-close`; a floor in
  `selftest.MINIMUM_CRITERIA`; CI legs that build and run with a device where a runner has one and
  report NOT EVALUATED where it has not.
- **Closing artefact**: the M3 golden images through three backends, compared within tolerance, with
  one committed screenshot per backend under `docs/design/images/` **each labelled with the device
  that produced it**.
- **Risk**: not the backends. It is that a hosted leg's green tick is mistaken for hardware. Every
  criterion in this rung's ledger that a hosted leg answers carries the device in its result, and the
  one check a Linux host can judge in full is the labelling rule itself.

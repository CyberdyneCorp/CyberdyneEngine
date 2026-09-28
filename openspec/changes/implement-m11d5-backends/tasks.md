# Tasks: M11.d.5 — Backends

Ordered. **Section 0 is not a spike**, and that is the difference between this rung and the five it
was inserted among: its spike was run at the head of M11.d, it produced this rung, and re-running it
would be re-deriving an answer that is already written down. Section 0 is therefore the decisions
that answer *what this rung may claim*, recorded once so every section below reads them rather than
rediscovering them at a gate.

Section 1 is the D3D12 memory decision, because it is a dependency adoption and those go through the
change flow rather than through a backend. Everything else follows the interface M11.d settled.

## 0. What is inherited, and what this rung may not claim

- [x] 0.1 **The runner answer is inherited, not re-measured.** M11.d's spike created a device on every
      allocated leg, drew, read the pixel back and presented — `macos-14` an "Apple Paravirtual
      device", `windows-2022` and `windows-11-arm` a "Microsoft Basic Render Driver", `macos-13` never
      allocated at all. Recorded in `design.md` §1 with the run identifiers, so the rows that depend
      on it read it rather than re-derive it
- [x] 0.2 **The four deferrals are declared with re-entry points before any backend is written** —
      Apple-family tile memory and memoryless attachments, argument buffers at Tier 2, golden-image
      parity against hardware references, and D3D12 Resource Heap Tier 1. `design.md` §2 carries each
      with what is unmet, why, and the condition that brings it back. **A deferral declared after a
      gate has looked at it is an excuse; declared before, it is scope**
- [x] 0.3 **The one claim a Linux host can judge in full is identified and kept here**: the labelling
      rule — a device's kind comes from its identity, never from a flag it sets about itself.
      `design.md` §3.1 states it, and `m11d5:device-identity-is-named` runs it on this host so the
      rung is developable rather than a queue for a runner
- [x] 0.4 **State what this rung does NOT take from the rung above**: the interface, the shader
      targets, the platform backend and the quality gates are M11.d's and are inherited through the
      flat ledger rather than restated. `design.md` §5 lists them

## 1. The D3D12 memory decision, before the backend is half written

- [x] 1.1 **Memory is the first decision, not the last.** Vulkan's allocator is VMA, fetched through
      `deps/manifest.toml`; there is no equivalent in this tree for D3D12. Either a suballocator of
      the engine's own over `ID3D12Heap`, or an adopted dependency — and an adoption **"SHALL go
      through the OpenSpec change flow recording the evaluation against these criteria"**, so it is a
      change of its own against `thirdparty-dependencies`, decided before the backend exists
- [x] 1.2 **Whichever is chosen, it is written for Resource Heap Tier 1 as well as Tier 2.** Every
      hosted image reports Tier 2 and Tier 1 hardware still ships; on Tier 1 a heap holds buffers *or*
      textures and never a mix, which is the same partition Vulkan spells as a bitmask and which the
      memory-pool class M11.d settled already expresses. The tier-1 path is the half that does not
      need a device, and `design.md` §2 records the tier-1 *exercise* as a deferral, not the code

## 2. Metal, native — not a translation layer

- [x] 2.1 **`src/device.mm` is compiled for the first time.** Its per-row `static_assert`s against the
      real `MTLPixelFormat` enumerators either fire or prove the transcription in `mapping.cpp`; on
      every machine this project owns that table is unverified and the README says so. **Treat every
      line of that file as a proposal**, which is what its own README calls it
- [x] 2.2 The rest of `cy::rhi::Device` — eighty-odd pure virtual members — behind the seed's mapping
      layer, with **tile memory and memoryless attachments implemented**. Hosted runners still
      exercise neither because their Metal device reports no Apple family; the local M3 Pro suite
      exercises both the memoryless path and placement-heap aliasing and records the device
- [x] 2.3 **The bindless descriptor model against argument buffers, and the tier reported.** The
      hosted device is Tier 1 and the engine's model needs Tier 2, so `integration.rhi_metal`
      **SHALL report the argument-buffer tier it ran at** — a Tier 1 pass that reads as a Tier 2 one
      is the defect this rung is most likely to ship
- [x] 2.4 The three things that map cleanly, spent as the seed says rather than re-derived:
      `MTLSharedEvent` for timeline semaphores, `MTLFunctionConstantValues` for specialization
      constants, and **nothing at all** for reversed-Z, because the projection inverts and the
      viewport stays [0, 1]
- [x] 2.5 The surface comes from `DisplayServer`, not from the backend: `Feature::MetalSurface` and a
      `CAMetalLayer` handed across, **with no platform `#ifdef` inside the backend**, which is
      `core-platform-abstraction`'s own scenario
- [x] 2.6 **The golden images on Metal**, against the committed references, failing by **naming the
      backend and the device** — or reported NOT EVALUATED with §1's runner reason. A committed
      reference is a photograph of one implementation on this project's own hardware; the delta
      against a paravirtual device is **reported**, never thresholded into a tick

## 3. D3D12 — from nothing

- [x] 3.1 **There is no D3D12 backend and there is no D3D12 file.** The only three D3D12 things in the
      tree are `BackendKind::D3D12`, `Feature::D3D12Surface` and `CY_RENDERER_D3D12|OFF` — three
      enumerators describing an API the engine does not have. The module is written from nothing,
      against the interface M11.d settled
- [x] 3.2 Descriptor heaps and bindless against the engine's descriptor model; a root signature derived
      from the engine's pipeline layout; **barriers derived from the access masks**, which is M11.d's
      interface change paying for itself a second time
- [x] 3.3 DXIL out of the shader pipeline: `SLANG_ENABLE_DXIL` on, DXC declared in the manifest with a
      licence identifier and a justification like every other integrated toolchain
- [x] 3.4 **Adapter selection, and the trap the spike found.** Every hosted Windows image presents two
      adapters, both `Microsoft Basic Render Driver`, and **adapter 0 does not set
      `DXGI_ADAPTER_FLAG_SOFTWARE`**. Selection SHALL NOT trust that flag. It classifies from the
      adapter's reported identity and vendor against a table this engine owns, and an adapter it
      cannot classify is reported **unknown** rather than assumed hardware
- [x] 3.5 **The golden images on D3D12**, same rule as 2.6, with the adapter's identity in the result.
      Physical evidence: AMD Radeon RX 6900 XT (RDNA 2, vendor 0x1002, class hardware) at
      `docs/design/images/m11d5-three-backends-d3d12.png` and its manifest. `unit.rhi_d3d12` and
      `integration.rhi_d3d12` pass with the debug layer enabled; `render.golden_backends` writes
      the row `backend=d3d12 ... outcome=matched ... max_delta=1`. First-light shaders compiled at
      `sm_6_6` (the engine-wide floor) rather than the hosted-WARP `sm_6_2` compatibility payload

## 4. The device report, and the claim a Linux host can check

- [x] 4.1 **Every device report names the device that answered** — across all three backends, Vulkan
      included, because the rule is the engine's and not a Windows workaround. Identity string,
      vendor, and the classification the engine derived: hardware, paravirtual, software, or unknown
- [x] 4.2 **The classification is tested where it can be tested**, which is here: `unit.rhi` carries
      *"a software device is labelled from its identity"* and *"a device report names the device that
      answered"*, both device-free, both red on this host until they are written. This is the rung's
      only fully judgeable claim and it is deliberately not pushed onto a runner
- [x] 4.3 **The engine builds and passes its suites with `CY_RENDERER_METAL` and `CY_RENDERER_D3D12`
      OFF as well as ON** — M8.c's rule, applied to the two options this rung delivers. Off is what
      every machine that is not a Mac or a Windows box builds, and a backend that has quietly become
      mandatory shows up on a Linux host first

## 5. The artefact — one scene, three backends, the same picture

- [x] 5.1 `just test-render --compare-backends vulkan metal d3d12`: the M3 golden images compared
      across legs of the matrix within tolerance, in the same shape as the cross-leg digest job M11.a
      built. **It is the one claim no single leg can make**, which is why it moved here with its
      subject rather than staying in M11.d. Now green across three vendors:
      `vulkan: NVIDIA GeForce RTX 5060 (delta 0)`,
      `metal: Apple M3 Pro (delta 1)`,
      `d3d12: AMD Radeon RX 6900 XT (delta 1)`;
      `vulkan versus metal: within tolerance`, `vulkan versus d3d12: within tolerance`.
      Adversarial pass performed (§7.2): scaling `sun_.color` from `1.05` to `1.25` in
      `samples/03-first-light/scene.cpp` turned the D3D12 row red with 10,745 texels over
      tolerance, 3,242 of them off any high-contrast edge, worst channel delta 17; restoring the
      scalar returned the ledger to `matched`
- [x] 5.2 **One committed screenshot per backend** under `docs/design/images/`, named
      `m11d5-three-backends-<backend>.png`, **each labelled with the backend and the device that
      produced it**. A diagram is allowed and **SHALL be labelled one**. All three backends now
      committed: `m11d5-three-backends-vulkan.png` (NVIDIA GeForce RTX 5060),
      `m11d5-three-backends-metal.png` (Apple M3 Pro),
      `m11d5-three-backends-d3d12.png` (AMD Radeon RX 6900 XT)
- [x] 5.3 **The artefact is honest about its own coverage on its own face**, the way M10's was about
      its 122 ms: which legs ran, which reported NOT EVALUATED and why, which device answered on each,
      and the four deferrals of `design.md` §2 named rather than omitted. Each committed manifest
      carries the row for its backend plus the `null` NOT-EVALUATED-no-image row; `vendor_id`,
      `class` and `device` are populated from the RHI classifier and are non-empty on every
      hardware row
- [x] 5.4 `rhi-and-render-graph` read **requirement by requirement at Complete grade** — satisfied,
      partial or unmet per requirement with the evidence in the module's README — the way M10 read
      `save-and-persistence`. The row has been Working since M3 over one backend and has never been
      read at Complete grade

## 6. Records and gates

- [x] 6.1 `tools/roadmap/milestones/m11d5.toml` — this rung's own criteria only, the ledger flat — the
      `milestone-m11d5` gate in `gates.toml`, and the floor in `selftest.MINIMUM_CRITERIA`
- [x] 6.2 The insertion itself, and every reader of a milestone identifier: `record.MILESTONES`,
      `plan.milestone_id` and the three heading patterns, the matrix column set and load table,
      `ROADMAP.md`, `dependencies.md`, `implementing.md`, `risks.md` entry 12, and the three insertion
      checks in `selftest.py`. `design.md` §4 lists them so none is discovered later
- [x] 6.3 An `m11e-open` criterion using the double-star glob form, and **M11.d's own handover
      criterion re-pointed at this rung** — it named `m11e`, and a handover that skips a rung is the
      one thing a handover check exists to make impossible
- [ ] 6.4 Update `status.yaml`, `capability-matrix.md`, `ROADMAP.md` and `dependencies.md`, and run
      the plan-consistency checks over them
- [ ] 6.5 Move `ci.yml`'s milestone job to `m11d5` in the same commit that flips the gate green
- [x] 6.6 **Hand M11.e its entry**: a written statement of **what M11.d.5 did not close**, in the shape
      M8.a, M8.c and M10 used — unchecked tasks named, with the defect rather than the intention, and
      the four deferrals of §2 carried forward with their re-entry points
- [x] 6.7 **Re-point, do not delete.** Any gap this rung closes has its declaration deleted in the same
      change that closes it; any it does not close keeps `known_gap_closes` pointed at the rung that
      will

## 7. The gate

- [ ] 7.1 Clean build of every profile from empty; `test-all` in each; every gate by hand; the M11.d.5
      ledger run once
- [ ] 7.2 **Every criterion executes something and can fail** — break what it checks and prove it goes
      red. M6 shipped four that did not, and M9's gate found one that passed 44 of 44 with its
      enforcement point deleted
- [ ] 7.3 **Adversarial pass on this rung's own invariants**: hand the adapter selector a software
      adapter that does not set the software flag and confirm it is still labelled software; ask for a
      backend that was not built and confirm the refusal names the option rather than failing at the
      first call; run the three-backend comparison with one leg missing and confirm it reports NOT
      EVALUATED rather than passing on two; claim a Tier 2 argument-buffer pass from a Tier 1 device
      and confirm the suite refuses
- [x] 7.4 **The evidence rule applied to this rung's own claims.** No golden-image tick over an
      unphotographed frame; no "parity" over a backend that merely compiled; no hardware claim over a
      paravirtual or software device; NOT EVALUATED is never a pass, and a reported gap is the outcome
      this gate prefers to a green one it cannot defend

### The gate's record for 7.2, 7.3 and 7.4 — 2026-09-27, on `453e2605` plus the working changes below

Build tree `build/m11d5-gate` (dev, from empty). Changed C++ compiled at `-O2 -DCY_DEVELOPMENT` and
at `-O0 -DCY_DEBUG`; `unit.rhi` 52 of 52. Nothing here was run on macOS or Windows.

**Ticked: 7.4. Left open: 7.2**, because `lint`, `metal-backend-is-native`, `d3d12-backend-exists` and
`golden-images-across-three-backends` are still not proven in the prover's record, **and 7.3**,
because its Tier 1 item was checked by reading the code, not by running it.

**7.2 — every criterion can fail.** Before this pass, eight of the ledger's eighteen had no proof in
`tools/roadmap/falsifiability.toml`. Where each stands now:

| criterion | before | now |
|---|---|---|
| `device-identity-is-named` | not provable here, no mutation | **proven against `build/m11d5-gate`**: rename `Microsoft Basic Render Driver` in `device_identity.cpp` → *the named case(s) FAILED*, green once restored |
| `generated-code` | not provable here, no mutation | **proven against `build/m11d5-gate`**: truncate `cy_reflect_generated.cpp` (m11b's mutation, same digest) → `generate-check` red, green once restored |
| `rhi-row-at-complete-grade` | no mutation | **proven**: rename the `Backend roadmap` case in `test_golden_backends.cpp` → the row is no longer mapped |
| `three-backend-images-committed` | no mutation | **strengthened, then proven**: it accepted any PNG, so it now also needs the manifest beside it to carry a row naming the device and a `hardware`/`software`/`paravirtual` class (7.4). Delete `row backend=metal` → red |
| `lint` | observed red on an old tree, no mutation | **not declared in the ledger, on purpose.** Its check is `m0:lint`'s and `m11d:lint`'s byte for byte, and a declared mutation moves the digest off the recorded entry, so the change owes a whole-tree lint proof (three runs of 1,432 files) and the list does not accept an unproven one. A targeted run shows the mutation works: with the three `NOLINTNEXTLINE(bugprone-casting-through-void)` lines taken out of `src/core/memory/src/tracking_allocator.cpp`, `just quality-lint src/core/memory` exits 123 with three `bugprone-casting-through-void` errors; with the file restored (md5 checked) it passes over the same 30 files. `m11d:lint` is getting a declared mutation of its own in parallel work. **Once that is proven, give `m11d5:lint` the same declaration word for word** and the prover carries the proof across, because the check is identical |
| `metal-backend-is-native`, `d3d12-backend-exists` | not provable here (`where = "ci"`) | **unchanged, and not provable from this host.** A declared mutation changes the digest, and the list does not take a new unproven entry, so the only proof that would count is a `[criterion.ci_proof]`. That needs `ci.yml`, which only the Close may touch. Mutations for the legs to use: rename `rhi_metal_surface` in `src/backends/rhi-metal/tests/CMakeLists.txt` (3 of 4 suites registered → red before ctest), and rename `rhi_d3d12` in `src/backends/rhi-d3d12/tests/CMakeLists.txt` (0 of 2 → red) |
| `golden-images-across-three-backends` | not provable here (`where = "ci"`) | **proven by hand; not in the prover's record.** The body only reads committed files, so it runs here: deleting `row backend=d3d12 ` from the D3D12 manifest → exit 1, *no row for backend=d3d12*; restored (md5 `aa575db3…`) → exit 0. Same reason as above for leaving the ledger alone |
| `roadmap-tiers` | red in the tree (as the ledger says it should be until the Close) | unchanged; its `lower-tiers` mutation is derived |

**7.3 — the adversarial pass.**

1. *A software adapter that does not set the software flag.* `classify_adapter`, the D3D12 selector's
   own classifier, was compiled on this host from `src/backends/rhi-d3d12/src/backend.cpp` with the
   Windows factory stubbed out and given the spike's adapters: `Microsoft Basic Render Driver` with
   vendor `0x1414`, with vendor `0`, and with a false NVIDIA vendor all come out **software**. An empty
   name and `NVIDIA GeForce RTX 5060` with vendor `0` come out **unknown**, never hardware. The
   classifier never sees `DXGI_ADAPTER_FLAG_SOFTWARE`. `unit.rhi_d3d12` had no case for this (only the
   Windows integration suite printed the class), so one was added: *the D3D12 adapter selector labels
   a software adapter from its identity, not its flag*. Linked on this host against the real
   classifier it passes 2 of 2; with the name marker renamed in a scratch copy it goes red at the
   vendor-0 and wrong-vendor checks. On Windows it will run in `unit.rhi_d3d12`.
   **Defect found in the shared classifier** (`cy::rhi::classify_device_identity`, which labels every
   golden manifest row): Mesa's Venus reports `Virtio-GPU Venus (<host GPU>)` and passes through the
   **host's** vendor ID, so `Virtio-GPU Venus (NVIDIA GeForce RTX 5060)` / `0x10DE` came out
   **hardware**. VMware `SVGA3D` and `Parallels Display Adapter` came out unknown. Fixed: paravirtual
   names are checked before the vendor table. Regression case *a paravirtual device that forwards its
   host's vendor is not labelled hardware*: **red with the fix reverted (3 failed checks)**, green with
   it.
2. *Ask for a backend that was not built.* `create_device(..., "metal")` on this host fell back to
   null with *"the requested backend is not registered in this build"*, which **did not name the
   option**. Fixed: a request for `vulkan`, `metal` or `d3d12` that the registry does not hold now
   falls back with a reason naming `CY_RENDERER_VULKAN`/`_METAL`/`_D3D12`. It still falls back rather
   than failing at the first call. Regression case *a backend this build left out is refused naming
   the option that builds it*: **red with the fix reverted (3 failed checks)**, green with it.
3. *The three-backend comparison with one leg missing.* It already refused to pass on two
   (*"d3d12: image and manifest must both be committed"*, exit 1), but called that a plain failure. It
   now finds every missing leg before comparing and reports *"NOT EVALUATED: d3d12 has no committed
   capture and manifest, so 2 of 3 backends cannot be called a match"*, still exit 1. The recipe's
   `--selftest` has a missing-leg case, which is red against the previous code.
4. *A Tier 2 argument-buffer pass from a Tier 1 device.* **Read, not run: this needs Metal.**
   `device.mm` sets `Capability::Bindless` only when `argumentBuffersSupport ==
   MTLArgumentBuffersTier2`. *Metal Tier 2 argument buffers are shader-readable* opens with
   `CY_REQUIRE(...has(Capability::Bindless))`, and *the Metal device names the hardware and
   argument-buffer tier that answered* checks `argument_buffer_tier == 2` and `apple_gpu_family`. So a
   Tier 1 device fails both suites; it cannot pass as Tier 2. That is also why hosted `macos-14`
   (Tier 1, paravirtual) cannot be the leg that turns `metal-backend-is-native` green.

**7.4 — the evidence rule.** `compare_backend_goldens.py` refused a row only when its class was
`unknown` **and** its device name was empty. The selftest broke both rules in one row, so deleting
either check left it green. It now refuses each case on its own: no device, a class outside
`hardware`/`software`/`paravirtual` (including `null-backend`), and a `hardware` claim over a
software or paravirtual name or Microsoft's `0x1414`. The selftest has one row per rule plus a
labelled software row that must be accepted, and it is **red against the previous function**.
`three-backend-images-committed` now reads the manifest as well as the PNG (see 7.2). The committed
evidence as it stands:

- `vulkan`: **re-photographed at this gate.** `render.golden_backends` run with
  `CY_GOLDEN_CAPTURE_DIR` on this host's `NVIDIA GeForce RTX 5060` (`nvidia-smi` names the same
  device) wrote a PNG **byte-identical** to the committed one (md5 `a57b477e…`, which is also the M3
  reference) and a row naming the same device, vendor and class. Only the committed row's `reason`
  field differs: it was written by hand, where the suite writes an empty one.
- `metal` (`Apple M3 Pro`, `0x106b`, hardware, max delta 1) and `d3d12` (`AMD Radeon RX 6900 XT`,
  `0x1002`, hardware, max delta 1): each manifest names its device, and the strengthened checks
  accept both. Neither could be re-photographed from here.
- One gap this gate cannot close: **a manifest is not bound to its PNG by content.** The suite writes
  both in the same run, but nothing committed records the image's digest, so a PNG swapped after
  capture would still pass. Closing it means an `image_md5` field written by
  `render.golden_backends` and recapturing on each backend's own machine. That is M11.e's (below),
  not something to hand-edit into the manifests.

### Carried to M11.e: picture parity of the renderer features merged after M11.d

Bloom, ambient occlusion, soft and contact shadows, light probes and the irradiance volume, and water
were merged to `main` after M11.d's verdict commit `1fe6446`. Grading and outlines are in flight. They
are renderer code above the RHI and are reported to compile for all three targets, but **nobody has
rendered their picture on Metal or D3D12.** This gate did not re-check the macOS and Windows builds:
CI run 36309640694 for `453e2605` was cancelled. `golden-images-across-three-backends` covers only
the M3 `first_light` scene, which uses none of them. So **the three-backend claim is parity of
`first_light`, not of the current renderer.** M11.e owes a golden per feature, or one scene using all
of them, captured on each backend's own device with a manifest naming that device, before any claim
wider than `first_light`.

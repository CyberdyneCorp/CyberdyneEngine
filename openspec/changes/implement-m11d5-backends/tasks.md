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

- [ ] 1.1 **Memory is the first decision, not the last.** Vulkan's allocator is VMA, fetched through
      `deps/manifest.toml`; there is no equivalent in this tree for D3D12. Either a suballocator of
      the engine's own over `ID3D12Heap`, or an adopted dependency — and an adoption **"SHALL go
      through the OpenSpec change flow recording the evaluation against these criteria"**, so it is a
      change of its own against `thirdparty-dependencies`, decided before the backend exists
- [ ] 1.2 **Whichever is chosen, it is written for Resource Heap Tier 1 as well as Tier 2.** Every
      hosted image reports Tier 2 and Tier 1 hardware still ships; on Tier 1 a heap holds buffers *or*
      textures and never a mix, which is the same partition Vulkan spells as a bitmask and which the
      memory-pool class M11.d settled already expresses. The tier-1 path is the half that does not
      need a device, and `design.md` §2 records the tier-1 *exercise* as a deferral, not the code

## 2. Metal, native — not a translation layer

- [ ] 2.1 **`src/device.mm` is compiled for the first time.** Its per-row `static_assert`s against the
      real `MTLPixelFormat` enumerators either fire or prove the transcription in `mapping.cpp`; on
      every machine this project owns that table is unverified and the README says so. **Treat every
      line of that file as a proposal**, which is what its own README calls it
- [ ] 2.2 The rest of `cy::rhi::Device` — eighty-odd pure virtual members — behind the seed's mapping
      layer, with **tile memory and memoryless attachments implemented** and **recorded as exercised
      nowhere**: they are Apple-family features, the only hosted Metal device reports no Apple family
      at all, and they are `rhi-and-render-graph`'s entire stated reason for refusing MoltenVK
- [ ] 2.3 **The bindless descriptor model against argument buffers, and the tier reported.** The
      hosted device is Tier 1 and the engine's model needs Tier 2, so `unit.rhi_metal` **SHALL report
      the argument-buffer tier it ran at** — a Tier 1 pass that reads as a Tier 2 one is the defect
      this rung is most likely to ship
- [ ] 2.4 The three things that map cleanly, spent as the seed says rather than re-derived:
      `MTLSharedEvent` for timeline semaphores, `MTLFunctionConstantValues` for specialization
      constants, and **nothing at all** for reversed-Z, because the projection inverts and the
      viewport stays [0, 1]
- [ ] 2.5 The surface comes from `DisplayServer`, not from the backend: `Feature::MetalSurface` and a
      `CAMetalLayer` handed across, **with no platform `#ifdef` inside the backend**, which is
      `core-platform-abstraction`'s own scenario
- [ ] 2.6 **The golden images on Metal**, against the committed references, failing by **naming the
      backend and the device** — or reported NOT EVALUATED with §1's runner reason. A committed
      reference is a photograph of one implementation on this project's own hardware; the delta
      against a paravirtual device is **reported**, never thresholded into a tick

## 3. D3D12 — from nothing

- [ ] 3.1 **There is no D3D12 backend and there is no D3D12 file.** The only three D3D12 things in the
      tree are `BackendKind::D3D12`, `Feature::D3D12Surface` and `CY_RENDERER_D3D12|OFF` — three
      enumerators describing an API the engine does not have. The module is written from nothing,
      against the interface M11.d settled
- [ ] 3.2 Descriptor heaps and bindless against the engine's descriptor model; a root signature derived
      from the engine's pipeline layout; **barriers derived from the access masks**, which is M11.d's
      interface change paying for itself a second time
- [ ] 3.3 DXIL out of the shader pipeline: `SLANG_ENABLE_DXIL` on, DXC declared in the manifest with a
      licence identifier and a justification like every other integrated toolchain
- [ ] 3.4 **Adapter selection, and the trap the spike found.** Every hosted Windows image presents two
      adapters, both `Microsoft Basic Render Driver`, and **adapter 0 does not set
      `DXGI_ADAPTER_FLAG_SOFTWARE`**. Selection SHALL NOT trust that flag. It classifies from the
      adapter's reported identity and vendor against a table this engine owns, and an adapter it
      cannot classify is reported **unknown** rather than assumed hardware
- [ ] 3.5 **The golden images on D3D12**, same rule as 2.6, with the adapter's identity in the result

## 4. The device report, and the claim a Linux host can check

- [ ] 4.1 **Every device report names the device that answered** — across all three backends, Vulkan
      included, because the rule is the engine's and not a Windows workaround. Identity string,
      vendor, and the classification the engine derived: hardware, paravirtual, software, or unknown
- [ ] 4.2 **The classification is tested where it can be tested**, which is here: `unit.rhi` carries
      *"a software device is labelled from its identity"* and *"a device report names the device that
      answered"*, both device-free, both red on this host until they are written. This is the rung's
      only fully judgeable claim and it is deliberately not pushed onto a runner
- [ ] 4.3 **The engine builds and passes its suites with `CY_RENDERER_METAL` and `CY_RENDERER_D3D12`
      OFF as well as ON** — M8.c's rule, applied to the two options this rung delivers. Off is what
      every machine that is not a Mac or a Windows box builds, and a backend that has quietly become
      mandatory shows up on a Linux host first

## 5. The artefact — one scene, three backends, the same picture

- [ ] 5.1 `just test-render --compare-backends vulkan metal d3d12`: the M3 golden images compared
      across legs of the matrix within tolerance, in the same shape as the cross-leg digest job M11.a
      built. **It is the one claim no single leg can make**, which is why it moved here with its
      subject rather than staying in M11.d
- [ ] 5.2 **One committed screenshot per backend** under `docs/design/images/`, named
      `m11d5-three-backends-<backend>.png`, **each labelled with the backend and the device that
      produced it**. A diagram is allowed and **SHALL be labelled one**
- [ ] 5.3 **The artefact is honest about its own coverage on its own face**, the way M10's was about
      its 122 ms: which legs ran, which reported NOT EVALUATED and why, which device answered on each,
      and the four deferrals of `design.md` §2 named rather than omitted
- [ ] 5.4 `rhi-and-render-graph` read **requirement by requirement at Complete grade** — satisfied,
      partial or unmet per requirement with the evidence in the module's README — the way M10 read
      `save-and-persistence`. The row has been Working since M3 over one backend and has never been
      read at Complete grade

## 6. Records and gates

- [ ] 6.1 `tools/roadmap/milestones/m11d5.toml` — this rung's own criteria only, the ledger flat — the
      `milestone-m11d5` gate in `gates.toml`, and the floor in `selftest.MINIMUM_CRITERIA`
- [ ] 6.2 The insertion itself, and every reader of a milestone identifier: `record.MILESTONES`,
      `plan.milestone_id` and the three heading patterns, the matrix column set and load table,
      `ROADMAP.md`, `dependencies.md`, `implementing.md`, `risks.md` entry 12, and the three insertion
      checks in `selftest.py`. `design.md` §4 lists them so none is discovered later
- [ ] 6.3 An `m11e-open` criterion using the double-star glob form, and **M11.d's own handover
      criterion re-pointed at this rung** — it named `m11e`, and a handover that skips a rung is the
      one thing a handover check exists to make impossible
- [ ] 6.4 Update `status.yaml`, `capability-matrix.md`, `ROADMAP.md` and `dependencies.md`, and run
      the plan-consistency checks over them
- [ ] 6.5 Move `ci.yml`'s milestone job to `m11d5` in the same commit that flips the gate green
- [ ] 6.6 **Hand M11.e its entry**: a written statement of **what M11.d.5 did not close**, in the shape
      M8.a, M8.c and M10 used — unchecked tasks named, with the defect rather than the intention, and
      the four deferrals of §2 carried forward with their re-entry points
- [ ] 6.7 **Re-point, do not delete.** Any gap this rung closes has its declaration deleted in the same
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
- [ ] 7.4 **The evidence rule applied to this rung's own claims.** No golden-image tick over an
      unphotographed frame; no "parity" over a backend that merely compiled; no hardware claim over a
      paravirtual or software device; NOT EVALUATED is never a pass, and a reported gap is the outcome
      this gate prefers to a green one it cannot defend

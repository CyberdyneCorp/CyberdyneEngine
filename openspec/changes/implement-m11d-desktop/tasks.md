# Tasks: M11.d — Desktop

Ordered. Section 0 is the spike and it runs first because **the eight gaps the Metal seed recorded
are interface changes before they are backends**: changing `reserve_transient_memory`'s contract
after two backends are written is a migration across every pass in the engine, and changing it before
is an afternoon. Section 1 is that interface, on Vulkan and null, before a line of either new backend
exists.

The spike carries a second question the rung cannot start without, and it is not a code question:
**does a hosted runner present a graphics device at all?** Every Metal and D3D12 image claim in this
rung is judged on a machine nobody here owns, and `ci.yml` has six legs that build and test and not
one that has ever created a device.

## 0. The spike — the eight RHI gaps, and whether a hosted runner has a device

- [ ] 0.1 **Measure, do not assume: each of the eight proposed interface changes applied to the two
      backends that exist.** `src/backends/rhi-metal/README.md` proposes a fix for each gap and
      `metal_gaps()` returns them as data; what none of them has is a cost. Apply each against
      `VulkanDevice`, `NullDevice` and `src/rendering/graph/` and count what moves — the call sites
      are known (`executor.cpp:192` for the transient reservation, `executor.cpp:417` for secondary
      execution, `device.h:254/305/336` and `device.h:182` for the interface itself). **A change that
      cannot be made cleanly on the two backends that exist will not be made cleanly on four**, and
      that is the finding
- [ ] 0.2 **The runner question, measured before anything depends on it.** Does a hosted
      `macos-14` runner create a `MTLDevice` and present a `CAMetalLayer`, and does a hosted
      `windows-11-arm` / `windows-2022` runner create a D3D12 device? **Name what answers**: a WARP
      adapter is a D3D12 device and is not a GPU, and a paravirtualised Metal device is not the
      hardware the golden images were photographed on. The answer decides how every image claim in
      sections 2, 3 and 8 is *reported*, and the honest outcomes are three — a device, a
      software/paravirtual device that is labelled as one, or no device and a compile-and-validate
      claim with the image criterion **reported NOT EVALUATED**, exactly as `m0:three-platforms` is
      reported today
- [ ] 0.3 State plainly what the spike cannot answer on this host, through the ledger's own
      `requires`/`where` mechanism rather than as a sentence. This host has one operating system and
      one GPU vendor; `where = "ci"` is the mechanism's way of saying "another machine", and NOT
      EVALUATED is never a pass
- [ ] 0.4 Commit the spike outside the repository — as M3's, M5.5's, M6's, M7's, M8.b's, M9's and
      M10's were, because a prototype under `docs/` fails `just quality-layers` — and record its
      answer in `design.md` §1, so the rows that depend on it read it rather than re-derive it

## 1. The interface, settled before either backend — `rhi-and-render-graph`

- [ ] 1.1 **Gap 2, the one with no workaround and no equivalent.** `reserve_transient_memory(bytes,
      memory_type_bits)` intersects a Vulkan bitmask across every transient in a frame to prove one
      pool is legal for all of them; Metal can only answer `~0u`. Replace it with an **opaque memory
      pool class the graph only compares for equality**, so the proof survives on a backend that has
      no bitmask. `RenderGraph`'s plan and `executor.cpp:192` are the only producers and consumers
- [ ] 1.2 **Gap 5, the other one with no workaround.** `execute_secondary` today permits a secondary
      recorded before its pass instance exists; `MTLParallelRenderCommandEncoder`'s sub-encoders
      exist only inside a live encoder. State **"the pass is begun before its secondaries are
      recorded"** as a precondition of the interface, refuse the violation in the null backend where
      every test can see it, and fix any caller the refusal finds
- [ ] 1.3 **Gaps 1, 3, 4, 6 and 7 — each a capability or a vocabulary change, none an `#ifdef`.**
      A native shader form (`Span<const u8>` plus `native_shader_format()`) beside SPIR-V; image
      layouts derived **inside the Vulkan backend** from the access masks `access.h` already carries,
      so `ImageLayout` stops being a Vulkan object in an engine-owned interface; a
      `needs_queue_ownership_transfer()` capability instead of `queue_family()`/`kQueueFamilyIgnored`
      at call sites; a pipeline cache that takes an opaque backend-defined token or a path; and a
      **per-format support query** so the *engine* picks the substitute for `D24UnormS8Uint` rather
      than each backend inventing one. `Backend capability model` already requires that the renderer
      branch on capabilities and never on backend identity — every one of these must land that way
- [ ] 1.4 **Gap 8 stays as it is**, and that is recorded rather than silently skipped: a multi-stage
      `PushConstantRange` is genuinely fine on Metal, the seed says so, and the next reader should not
      spend an afternoon re-deriving it
- [ ] 1.5 **Every change lands on Vulkan AND null first, and `metal_gaps()` shrinks as data.**
      `unit.rhi_metal_seed` reads the same table the README prints, so the gap table, the diagnostic
      and the test cannot drift apart; a gap closed in prose and not in `mapping.cpp` is a gap that
      will be re-found at the first Metal compile
- [ ] 1.6 **The shader targets both backends need do not exist.** `cache.h` already names
      `"metal-msl"` and `"d3d12-dxil"` as interchange forms, `compiler.h` describes the translation
      in a comment, **nothing in the tree emits either**, and `cmake/dependencies.cmake` sets
      `SLANG_ENABLE_DXIL OFF` with "D3D12 turns this on" written beside it. `shader-system`'s
      Complete cell is **M11.c's**, not this rung's — `design.md` §6 carries it as a dependency — but
      it is a prerequisite of sections 2 and 3 either way, and this rung states which it got

## 2. Metal, native — not a translation layer

- [ ] 2.1 **`src/device.mm` is compiled for the first time.** Its per-row `static_assert`s against
      the real `MTLPixelFormat` enumerators either fire or prove the transcription in `mapping.cpp`;
      on this machine that table is unverified and the README says so. **Treat every line of that
      file as a proposal**, which is what its own README calls it
- [ ] 2.2 The rest of `cy::rhi::Device` — eighty-odd pure virtual members — behind the seed's mapping
      layer, with **tile memory and memoryless attachments usable directly**, which is the reason
      `rhi-and-render-graph` refuses MoltenVK as the long-term Apple strategy in as many words
- [ ] 2.3 The three things that map cleanly, spent as the seed says rather than re-derived:
      `MTLSharedEvent` for timeline semaphores, `MTLFunctionConstantValues` for specialization
      constants, and **nothing at all** for reversed-Z, because the projection inverts and the
      viewport stays [0, 1]
- [ ] 2.4 The surface comes from `DisplayServer`, not from the backend: `Feature::MetalSurface` and a
      `CAMetalLayer` handed across, **with no platform `#ifdef` inside the backend**, which is
      `core-platform-abstraction`'s own scenario
- [ ] 2.5 **The golden images on Metal**, against the committed references, failing by **naming the
      backend** — or reported NOT EVALUATED with 0.2's runner reason. `ci.yml` already states the
      rule for M3's images and it is not weakened here: a committed reference is a photograph of one
      implementation, and a criterion that passes on a machine that cannot judge it is the defect
      `requires` exists to prevent

## 3. D3D12 — from nothing

- [ ] 3.1 **There is no D3D12 backend and there is no D3D12 file.** The only three D3D12 things in
      the tree are `BackendKind::D3D12`, `Feature::D3D12Surface` and `CY_RENDERER_D3D12|OFF` — three
      enumerators describing an API the engine does not have. The module is written from nothing,
      against the interface section 1 settled
- [ ] 3.2 **Memory is the first decision, not the last.** Vulkan's allocator is VMA, fetched through
      `deps/manifest.toml`; there is no equivalent in this tree for D3D12. Either a suballocator of
      the engine's own over `ID3D12Heap`, or an adopted dependency — and an adoption **"SHALL go
      through the OpenSpec change flow recording the evaluation against these criteria"**, so it is a
      change of its own against `thirdparty-dependencies`, decided before the backend is half written
- [ ] 3.3 Descriptor heaps and bindless against the engine's descriptor model; a root signature
      derived from the engine's pipeline layout; **barriers derived from the access masks**, which is
      1.3's change paying for itself a second time
- [ ] 3.4 DXIL out of the shader pipeline: `SLANG_ENABLE_DXIL` on, DXC declared in the manifest with
      a licence identifier and a justification like every other integrated toolchain, and the engine
      **building and passing its suites with `CY_RENDERER_D3D12` OFF as well as ON** — M8.c's rule,
      applied to every option this rung adds
- [ ] 3.5 **The golden images on D3D12**, same rule as 2.5, and one addition this rung will not let
      itself blur: **a WARP adapter is labelled WARP in the result.** It is a real D3D12 device and it
      is not a GPU, and a green tick that does not say which answered is a claim about hardware
      nobody ran

## 4. The native platform backend and the porting surface — `core-platform-abstraction`

- [ ] 4.1 `platform/<native>/` implementing `Platform`, `DisplayServer`, the input event source and
      the surface provider for one desktop platform, **replacing SDL3 there**. `design.md` §4 names
      which desktop and the trade that choice makes
- [ ] 4.2 **The exit criterion is a diff, and it is checked mechanically.** *"requiring no change in
      `src/core/`, `src/ecs/`, `src/servers/` or `src/scene/`"* is a claim about a changeset, so the
      change that adds the backend is the evidence, and a script that reads it is the criterion. **If
      the port does change one of those four, that change IS the finding** and it is worth more than
      the Complete cell — `platform/README.md` already says "if it does, the abstraction is wrong"
- [ ] 4.3 `samples/00-empty` and the M3 golden images run on the native backend, which is the M11 exit
      criterion stated in the ROADMAP word for word
- [ ] 4.4 **The stub platform**, which is the porting surface's own proof: no mouse, no resizable
      window, no filesystem writable outside the user mount, and **no ownership of the main loop** —
      it drives frames through `runtime.tick()`, the entry point `platform/host/` already calls
      rather than owns. It builds, it links, and it runs a headless frame; a porting surface that
      only ever compiles against desktop backends has never been tested
- [ ] 4.5 SDL3 stops being the only way this engine opens a window, and **stays** — it is not deleted
      anywhere, because the second implementation is the proof and not a replacement. `headless/`
      stays too; the specification requires it. `just quality-layers` already refuses an SDL type
      above `platform/` and that rule is unchanged

## 5. `rendering-forward-clustered` — the desktop half only

- [ ] 5.1 MSAA through the render graph's attachment model, resolved where the pass declares it
- [ ] 5.2 Multi-view, selected by **capability query** rather than backend identity, with the
      baseline path still correct where the capability is absent
- [ ] 5.3 **The row does NOT reach Complete here, and the ledger says so rather than the gate
      discovering it.** The mobile pipeline differences are M11.e's half of the same row, and a row
      is not Complete on the half of its scope one rung can reach. This rung records a floor

## 6. The core rows the port audits

These five are here because **the exit criterion for the native backend is that four directories do
not change**, which is a first-hand reading of them whether or not anybody calls it one.

- [ ] 6.1 **`core-assets-and-io`** — "Development file serving" has no transport: `RemoteFileProvider`
      is an interface whose only implementation in the tree is `FakeHost` in
      `src/core/assets/tests/test_vfs.cpp`, and a remote mount is exactly what a second machine
      needs. And `AssetSystem::reload` returns `NotImplemented` for an asset served from a cooked
      package (`asset_system.cpp:1190`), which the message itself explains and scopes
- [ ] 6.2 **`core-memory-and-containers`** — the attribution axes have **no producer**: the only four
      files naming `MemoryAttributionScope` are its own header, source, test and README, so
      "attribution by domain, type, thread, world cell and asset" is a mechanism nobody pushes. And
      `MemoryDomain::Gpu` is budgeted while nothing reports device memory into it — the backend that
      allocates is the module that owes it, which makes this section 2 and 3's debt as much as this
      row's
- [ ] 6.3 **`core-jobs-and-concurrency`, `ecs-core` and `engine-architecture` read requirement by
      requirement at Complete grade**, the way M10 read `save-and-persistence` — satisfied, partial
      or unmet per requirement with the evidence in each module's README. **No named blocker means
      nothing has refused them, not that nothing is missing**, and a table is the only way to tell
      those two apart
- [ ] 6.4 **`engine-architecture`'s claim is what the port cost above layer 3.** Two new graphics
      backends and one new platform backend is the largest architectural stress this engine has had;
      record what it cost in `src/` above `platform/` and `src/backends/`, including zero if that is
      the honest number

## 7. Build, packaging and the gates

- [ ] 7.1 **`testing-and-quality`'s four absent gates**, each a recipe, each in CI, and **each shown
      red once**: `swift-format`, the licence-header check, the spelling check, and the
      undocumented-symbol gate — which is the "documentation gate" the M11 exit criteria name.
      `just/quality.just` today has `format`, `lint`, `layers`, `identity`, `abi` and `specs` and
      none of these four
- [ ] 7.2 The three acceptance scenarios the matrix records as unwritten — **strategy stress**,
      **control handover**, **headless server** — written where the taxonomy can run them, with the
      kind and budget they belong to stated rather than assumed
- [ ] 7.3 Golden images **run against every enabled RHI backend and record which backend produced a
      failure**, which the requirement has said since M3 and one backend has never been able to test
- [ ] 7.4 **`build-and-packaging` — content audit**: *why is this in the build* (the reference chain
      from a declared root) and *what references this*; size by category, asset, plugin, world region
      and install bundle; cook and compile time by stage with cache hit rates
- [ ] 7.5 **`build-and-packaging` — provenance and symbols**: a build identity, engine and project
      revisions, lockfile hash, build and cook configuration, toolchain versions, manifest hash;
      shipping binaries stripped with symbols archived separately and retrievable by build identity;
      a reproducibility bundle archived by CI
- [ ] 7.6 **Downloadable content and distributed execution — contingent, and `design.md` §5 says
      why.** `docs/roadmap/risks.md` already lists distributed build execution as "M11 or later". If
      the distribution surface only becomes real at M11.e, this row's Complete cell moves there
      **with its reason recorded**, which is what a demotion is for
- [ ] 7.7 **`developer-workflow-and-just`** — target selection on the build, test, package and deploy
      recipes, and **an impossible target explained**: the requirement says the workflow "SHALL say so
      and state what is required" rather than failing obscurely, and a second desktop is the first
      time that sentence has a second answer
- [ ] 7.8 **What this rung must NOT claim over.** `just/release.just`'s four recipes —
      `release-version`, `release-changelog`, `release-artefacts`, `release-publish` — all refuse and
      name *"M12 — build-and-packaging"*, **a milestone that does not exist on a ladder whose
      `record.MILESTONES` ends at m11**. Implementing them is M11.e's; naming them here is how this
      rung's gate is stopped from writing a Complete cell for `developer-workflow-and-just` over four
      recipes that refuse

## 8. The artefact — `samples/11-ship`, the desktop half

- [ ] 8.1 One project — **built, cooked, packaged and launched from a single recipe** on each desktop
      target. `samples/` holds fifteen entries today and not one of them is a packaged project
- [ ] 8.2 It **draws**: through the native platform backend on the platform that has one, and through
      each graphics backend on a leg that has a device
- [ ] 8.3 **The artefact is honest about its own coverage on its own face**, the way M10's was about
      its 122 ms: which legs ran, which reported NOT EVALUATED and why, and **which device answered**
      on each — hardware, paravirtual or WARP
- [ ] 8.4 The package carries its provenance (7.5) and the launch reproduces from it
- [ ] 8.5 **Capture it.** One screenshot per backend under `docs/design/images/`, each labelled with
      the backend and the device that produced it. A diagram is allowed and **SHALL be labelled one**

## 9. Records and gates

- [ ] 9.1 Write `tools/roadmap/milestones/m11d.toml` — this rung's own criteria only, the ledger flat
      — declare `milestone-m11d` in `gates.toml` and raise `selftest.MINIMUM_CRITERIA`
- [ ] 9.2 An `m11e-open` criterion using the double-star glob form
- [ ] 9.3 Update `status.yaml`, `capability-matrix.md`, `ROADMAP.md` and `dependencies.md`, and run
      the plan-consistency checks over them
- [ ] 9.4 Move `ci.yml`'s milestone job to `m11d` in the same commit that flips the gate green — the
      job runs on every push to `main`, so this commit must not land before the gate is green
- [ ] 9.5 **Hand M11.e its entry.** The M11.e change directory already exists with its README and
      proposal; what this rung owes it is a written statement of **what M11.d did not close**, in the
      shape M8.a, M8.c and M10 used — unchecked tasks named, with the defect rather than the intention
- [ ] 9.6 **Re-point, do not delete.** Any gap this rung closes has its declaration deleted in the
      same change that closes it, because a declared gap that starts passing fails the ledger; any it
      does not close keeps `known_gap_closes` pointed at the rung that will

## 10. The gate

- [ ] 10.1 Clean build of every profile from empty; `test-all` in each; every gate by hand; the
      M11.d ledger run once
- [ ] 10.2 **Every criterion executes something and can fail** — break what it checks and prove it
      goes red. M6 shipped four that did not, and M9's gate found one that passed 44 of 44 with its
      enforcement point deleted
- [ ] 10.3 **Adversarial pass on this rung's own invariants**: make a deliberate edit under
      `src/core/` during the port and confirm 4.2's check refuses it; ask for a backend that was not
      built and confirm the refusal names the option rather than failing at the first call; remove a
      format from the per-format query and confirm the engine picks the substitute rather than
      drawing wrong; record a secondary before its pass is begun and confirm 1.2's precondition
      refuses; delete a gate's input and confirm the gate goes red
- [ ] 10.4 Records verified against what the code supports, not what the plan claimed — **including
      this rung's own Complete cells against the status record**, which is what `m9:record-matches-plan`
      is for and it is not milestone-specific in shape
- [ ] 10.5 **The evidence rule applied to this rung's own claims.** No golden-image tick over an
      unphotographed frame; no "parity" over a backend that compiled; NOT EVALUATED is never a pass,
      and a reported gap is the outcome this gate prefers to a green one it cannot defend

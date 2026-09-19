# Tasks: M11.d — Desktop

Ordered. Section 0 is the spike and it runs first because **the eight gaps the Metal seed recorded
are interface changes before they are backends**: changing `reserve_transient_memory`'s contract
after two backends are written is a migration across every pass in the engine, and changing it before
is an afternoon. Section 1 is that interface, on Vulkan and null, before a line of either new backend
exists.

The spike carried a second question the rung could not start without, and it was not a code
question: **does a hosted runner present a graphics device at all?** It was answered, and the answer
**resized this rung**: every allocated leg presents a device that draws and presents, and not one of
them is a GPU — and, more simply, neither Metal nor D3D12 compiles on the Linux host this work
happens on. Sections 2 and 3 therefore moved to **M11.d.5 · Backends**, inserted between this rung
and M11.e, with `rhi-and-render-graph`'s Complete cell and the three-backend golden-image comparison.
The marker where those sections were carries the reasoning; `design.md` §1.4 carries the
measurement.

## 0. The spike — the eight RHI gaps, and whether a hosted runner has a device

- [x] 0.1 **Measure, do not assume: each of the eight proposed interface changes applied to the two
      backends that exist.** `src/backends/rhi-metal/README.md` proposes a fix for each gap and
      `metal_gaps()` returns them as data; what none of them has is a cost. Apply each against
      `VulkanDevice`, `NullDevice` and `src/rendering/graph/` and count what moves — the call sites
      are known (`executor.cpp:192` for the transient reservation, `executor.cpp:417` for secondary
      execution, `device.h:254/305/336` and `device.h:182` for the interface itself). **A change that
      cannot be made cleanly on the two backends that exist will not be made cleanly on four**, and
      that is the finding
- [x] 0.2 **The runner question, measured before anything depends on it.** Does a hosted
      `macos-14` runner create a `MTLDevice` and present a `CAMetalLayer`, and does a hosted
      `windows-11-arm` / `windows-2022` runner create a D3D12 device? **Name what answers**: a WARP
      adapter is a D3D12 device and is not a GPU, and a paravirtualised Metal device is not the
      hardware the golden images were photographed on. The answer decides how every image claim in
      sections 2, 3 and 8 is *reported*, and the honest outcomes are three — a device, a
      software/paravirtual device that is labelled as one, or no device and a compile-and-validate
      claim with the image criterion **reported NOT EVALUATED**, exactly as `m0:three-platforms` is
      reported today
- [x] 0.3 State plainly what the spike cannot answer on this host, through the ledger's own
      `requires`/`where` mechanism rather than as a sentence. This host has one operating system and
      one GPU vendor; `where = "ci"` is the mechanism's way of saying "another machine", and NOT
      EVALUATED is never a pass
- [x] 0.4 Commit the spike outside the repository — as M3's, M5.5's, M6's, M7's, M8.b's, M9's and
      M10's were, because a prototype under `docs/` fails `just quality-layers` — and record its
      answer in `design.md` §1, so the rows that depend on it read it rather than re-derive it

## 1. The interface, settled before either backend — `rhi-and-render-graph`

- [x] 1.1 **Gap 2, the one with no workaround and no equivalent.** `reserve_transient_memory(bytes,
      memory_type_bits)` intersects a Vulkan bitmask across every transient in a frame to prove one
      pool is legal for all of them; Metal can only answer `~0u`. Replace it with an **opaque memory
      pool class the graph MEETS and tests for empty** — *not* one it compares for equality, which
      is what this task said before the spike measured it. `design.md` §1.4.2 carries the number:
      this project's NVIDIA RTX 5060 answers `0x03` for transient images and `0x1F` for transient
      buffers, so an equality would refuse to put them in one pool and **split the transient heap in
      two**, losing exactly the aliasing `heap_bytes` against `naive_bytes` exists to report. A meet
      keeps the proof, loses the Vulkan spelling, and needs no device — so `compile()`'s "the
      derivation touches no device" invariant and `plan_hash`'s determinism both survive.
      `RenderGraph`'s plan and `executor.cpp:192` are the only producers and consumers
- [x] 1.2 **Gap 5, the other one with no workaround — and the seed's fix does not address the
      mismatch, which the spike measured and this task is rewritten to.** The original wording was:
      state *"the pass is begun before its secondaries are recorded"* as a precondition and refuse
      the violation in the null backend. Read against the tree that is not the shape of the problem.
      `executor.cpp:337-382` records **every** secondary for a submit, one per pass, on job workers,
      **before the primary loop reaches any of them**; `frame_recorder.cpp`'s pass callback itself
      calls `begin_rendering`/`end_rendering`, so **each secondary contains a whole render pass**;
      and the barriers go into the primary between passes. `MTLParallelRenderCommandEncoder` is
      parallelism **within** one render pass and this engine's is parallelism **across** passes —
      different axes, and no ordering precondition converts one into the other. So: a **capability**,
      `Capability::ParallelPassRecording`, **one term at `executor.cpp:334`**. A device that answers
      false records sequentially and produces the identical command stream, and that is proved
      against a null device told to answer false, because no machine here can create a Metal one
- [x] 1.3 **Gaps 1, 3, 4, 6 and 7 — each a capability or a vocabulary change, none an `#ifdef`.**
      Landed, and **not one platform conditional was written in the interface**. Gap 1: additive —
      `ShaderModuleDescription::native` (`Span<const u8>`) with a `ShaderFormat native_format`, plus
      `DeviceCapabilities::native_shader_format()`, and `validate_shader_module()` in the interface
      module enforcing "exactly one of the two" for every backend at once; **zero existing call
      sites moved**, because SPIR-V stays the interchange form. Gap 4: `Device::queue_family()` and
      `kQueueFamilyIgnored` are **deleted**, replaced by `needs_queue_ownership_transfer()` and an
      opaque `queue_ownership_domain(QueueKind)` the graph only compares; a barrier carries
      `QueueKind`s and an `ownership_transfer` flag, and the family index never leaves
      `vulkan_command_buffer.cpp`. Gap 6: both cache calls take a path, and an absent file is a cold
      start. Gap 7: the query **already existed and had no consumer** —
      `select_depth_stencil_format()` is the engine picking, `FrameAssembly::attach_device` calls it,
      and `validate_texture` now refuses an unsupported depth target instead of letting a backend
      substitute quietly.
      **Gap 3 is where the cost was and where this task's own text was wrong**: "image layouts derived
      inside the Vulkan backend from the access masks `access.h` already carries" is **not
      implementable**, and `compile.cpp` is why — a barrier's `src_access` deliberately carries only
      the WRITE access, because a write-after-read needs an execution dependency and not a memory
      one, so it is not the resource's current state and a backend deriving from it would transition
      from the wrong one; and `Access::Present` carries no access bits and no stage at all. What
      landed instead is the half that is true: `ImageLayout` became **`ImageUse`**, engine
      vocabulary, and inside `cy::rhi` `VkImageLayout` now exists only in
      `vulkan_translate.{h,cpp}` — checked at M11.d resume, and the one live use outside it is
      `src/backends/viewport/src/publisher.cpp`, a native Vulkan client (volk, dma-buf export)
      that never goes through this interface, so the neutrality gap 3 is about is intact
- [x] 1.4 **Gap 8 stays as it is**, and that is recorded rather than silently skipped: a multi-stage
      `PushConstantRange` is genuinely fine on Metal, the seed says so, and the next reader should not
      spend an afternoon re-deriving it. Recorded as **data** rather than prose:
      `MetalGapStatus::NoChangeNeeded` on its row — a third answer beside Open and Closed — and
      `unit.rhi_metal_seed` asserts that row is exactly that, so deleting it to shorten the table
      fails a test
- [x] 1.5 **Every change lands on Vulkan AND null first, and `metal_gaps()` shrinks as data.**
      All eight are implemented on both existing backends, and `smoke.vulkan_frame` runs the changed
      interface on this project's own RTX 5060 — 4 of 4, including the transient-aliasing case the
      pool-class change could have broken. The table shrank AS DATA: `MetalGapRecord` gained a
      `status` and a `closed_by`, `metal_open_gap_count()` is the number that moves, and
      `unit.rhi_metal_seed` prints **"gaps: 8 total, 0 still open, 3 where Metal has no equivalent at
      all, 0 open with no workaround"** against eight open and two blocking at M7. Rows are never
      deleted when they close — the finding, the remedy that was argued for and what was actually
      done are one row a reviewer reads together — and a row cannot be marked closed without an
      account of how, which the test checks.
      **VERIFIED AT RESUME, BY MUTATION RATHER THAN BY READING.** The previous session ticked 1.1-1.6
      and was killed before reporting, so every claim above was re-checked against a build in
      `build/m11d-rhi`: 9 of 9 suites green (`unit.rhi`, `unit.rhi_metal_seed`,
      `integration.rhi_pipeline_cache`, `unit.render_graph`, `integration.render_graph_scale`,
      `smoke.vulkan_frame`, `unit.shader`, `integration.shader_pipeline`, `smoke.shader_slang`), with
      `smoke.vulkan_frame` on this project's own RTX 5060 (51 of 51 assertions, driver 580.380.320)
      and `unit.rhi_metal_seed` printing the gap line verbatim. **And three mutations proved the
      checks can fail** — the one thing a passing suite cannot tell you: flipping gap 8's row from
      `NoChangeNeeded` to `Closed` failed `unit.rhi_metal_seed` (1.4 is really test-protected);
      changing `meet()` from `&` to `|` failed three assertions in `test_interface_gaps.cpp`
      including the 0x03/0x1F NVIDIA case (1.1); and replacing the
      `Capability::ParallelPassRecording` term at `executor.cpp:344` with `true` failed
      `integration.render_graph_scale` on `secondary_command_buffers == 0` (1.2). All three mutations
      were restored and md5-verified against the pre-mutation hashes
- [x] 1.6 **Checked rather than assumed, and the dependency landed: this rung got both targets.**
      `design.md` §6 named this as owed by M11.c, and M11.c delivered it — so the sentence this task
      was written from (*"nothing in the tree emits either"*, `SLANG_ENABLE_DXIL OFF`) is now false,
      which is recorded rather than repeated. `slang/src/slang_compiler.cpp` maps `Target::Msl` to
      `SLANG_METAL` — MSL **source**, deliberately not `SLANG_METAL_LIB`, because a `.metallib` needs
      Apple's `metal` driver and would make the target unavailable on every machine this project's
      CI runs on — and `Target::Dxil` to `SLANG_DXIL` at `sm_6_6`, a floor that was *measured* from
      `vgVisRaster`'s 64-bit atomics rather than chosen. `cmake/dependencies.cmake` no longer
      hard-codes the option off: `CY_SHADER_DXIL` is the switch, default **on in Debug and
      Development**, and MSL needs no option at all because it comes out of the same Slang session.
      **What this rung owed the shader system in return is gap 1**: `ShaderModuleDescription::native`
      and `native_shader_format()` are where those two artefacts can now be handed to a device, which
      before M11.d had nowhere to go but inside `create_shader_module`, on the frame path

## 2 and 3. Metal and D3D12 — MOVED TO M11.d.5, not deleted and not descoped

**Sections 2 and 3 of this task list are no longer here.** Section 0.2 measured the question this
rung was scoped against, and the answer that resized it is not about graphics at all: **this project
works on a Linux host with one GPU vendor and no Apple toolchain.** Metal cannot be compiled here.
D3D12 cannot be compiled here. Neither backend could be written or judged where this rung is being
worked, and half-building them here would have produced the one defect this project has paid for
nine times — a check that cannot fail.

They moved to **M11.d.5 · Backends**, a rung inserted between this one and M11.e:
[`implement-m11d5-backends`](../implement-m11d5-backends/tasks.md) sections 1 to 5 carry them,
`tools/roadmap/milestones/m11d5.toml` carries the criteria, and `design.md` §1.4.3 of this change is
the measurement that argued for it. The governing rule is `delivery-roadmap`'s **"A spike may resize
a milestone as well as redirect it"**, which requires re-scoping to move whole capabilities with
their exit criteria rather than narrowing one in place — so `rhi-and-render-graph`'s **Complete cell
moved with them**.

**And `m11d:golden-images-across-three-backends` moved with them**, which is the half that matters.
It is M11's first exit criterion as `docs/ROADMAP.md` has stated it since the plan was drawn, it is
the one claim no single leg of the matrix can make, and a rung that received two backends without it
could close on *"it compiles somewhere"*. The same requirement's second scenario says so: a criterion
whose subject has been deferred moves with its subject, because leaving it behind produces a check
with nothing to check.

**What this rung keeps of them**, and it is the expensive part rather than the leftovers:

- **Section 1 — the interface — stays, and is now ordered by the ladder rather than by a promise.**
  "No gap is fixed in a backend" was a rule in `design.md` §2 that a task list had to be trusted to
  honour; it is now a property of the rungs, because the backends are one rung above. A gap fixed
  inside Metal is a gap Vulkan never agreed to, and there is no Metal here to fix it in.
- **`m11d:shader-targets-emitted` stays**, because it is a *prerequisite* check rather than a backend
  claim — M11.c's row read from the rung that consumes it — and M11.d.5 inherits it from here through
  the flat ledger. The prerequisite is proved below the rung that needs it.
- **Task 6.2's GPU-memory debt stays here and is recorded as partly M11.d.5's**: `MemoryDomain::Gpu`
  is budgeted while nothing reports device memory into it, and the backend that allocates is the
  module that owes it. The Vulkan half is this rung's; the other two are M11.d.5's.
- **Task 8.2's "it draws through each graphics backend on a leg that has a device" is now
  one backend**, and section 8 says so rather than leaving a sentence that quietly waits for two
  backends that are not in this rung.

## 4. The native platform backend and the porting surface — `core-platform-abstraction`

- [x] 4.1 `platform/<native>/` implementing `Platform`, `DisplayServer`, the input event source and
      the surface provider for one desktop platform, **replacing SDL3 there**. `design.md` §4 names
      which desktop and the trade that choice makes
- [x] 4.2 **The exit criterion is a diff, and it is checked mechanically.** *"requiring no change in
      `src/core/`, `src/ecs/`, `src/servers/` or `src/scene/`"* is a claim about a changeset, so the
      change that adds the backend is the evidence, and a script that reads it is the criterion. **If
      the port does change one of those four, that change IS the finding** and it is worth more than
      the Complete cell — `platform/README.md` already says "if it does, the abstraction is wrong"
- [ ] 4.3 `samples/00-empty` and the M3 golden images run on the native backend, which is the M11 exit
      criterion stated in the ROADMAP word for word
      - **The sample half is done and measured**: `cy_sample_empty --platform native --frames 120`
        opens a real X11 window, runs 120 frames and 119 simulation ticks, exits 0, and writes its
        trace to the same XDG path the SDL3 backend uses. All four backends — `sdl3`, `native`,
        `headless`, `stub` — run it, which is task 4.5's evidence as well
      - **The golden-image half cannot be satisfied as written, and that is a FINDING about the
        criterion rather than about the backend.** `tests/render/` links NO platform target and uses
        no `DisplayServer`: the goldens render offscreen through Vulkan with no window, so no
        platform backend can change them and "on the native backend" names nothing. The claim a
        native backend CAN make about M3's images is that the sample which produces them runs on it,
        and that is the half above. Rewriting the criterion is the ledger owner's call, not this
        task's
      - Independently, `cy_test_render_golden` **does not compile on this tree** as of this writing,
        from section 1's in-flight `ImageLayout` removal (`samples/03-first-light/renderer.h:152`,
        `renderer.cpp:558`) — a peer's change, not this port's, and it is why no golden run could be
        attempted at all
- [x] 4.4 **The stub platform**, which is the porting surface's own proof: no mouse, no resizable
      window, no filesystem writable outside the user mount, and **no ownership of the main loop** —
      it drives frames through `runtime.tick()`, the entry point `platform/host/` already calls
      rather than owns. It builds, it links, and it runs a headless frame; a porting surface that
      only ever compiles against desktop backends has never been tested
- [x] 4.5 SDL3 stops being the only way this engine opens a window, and **stays** — it is not deleted
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

- [x] 6.1 **`core-assets-and-io`** — "Development file serving" has no transport: `RemoteFileProvider`
      is an interface whose only implementation in the tree is `FakeHost` in
      `src/core/assets/tests/test_vfs.cpp`, and a remote mount is exactly what a second machine
      needs. And `AssetSystem::reload` returns `NotImplemented` for an asset served from a cooked
      package (`asset_system.cpp:1190`), which the message itself explains and scopes
- [x] 6.2 **`core-memory-and-containers`** — the attribution axes have **no producer**: the only four
      files naming `MemoryAttributionScope` are its own header, source, test and README, so
      "attribution by domain, type, thread, world cell and asset" is a mechanism nobody pushes. And
      `MemoryDomain::Gpu` is budgeted while nothing reports device memory into it — the backend that
      allocates is the module that owes it, so **the Vulkan half is this rung's and the Metal and
      D3D12 halves are M11.d.5's**, recorded here rather than left for that rung to rediscover
- [x] 6.3 **`core-jobs-and-concurrency`, `ecs-core` and `engine-architecture` read requirement by
      requirement at Complete grade**, the way M10 read `save-and-persistence` — satisfied, partial
      or unmet per requirement with the evidence in each module's README. **No named blocker means
      nothing has refused them, not that nothing is missing**, and a table is the only way to tell
      those two apart
- [x] 6.4 **`engine-architecture`'s claim is what the port cost above layer 3.** Two new graphics
      backends and one new platform backend is the largest architectural stress this engine has had;
      record what it cost in `src/` above `platform/` and `src/backends/`, including zero if that is
      the honest number

## 7. Build, packaging and the gates

- [x] 7.1 **`testing-and-quality`'s four absent gates**, each a recipe, each in CI, and **each shown
      red once**: `swift-format`, the licence-header check, the spelling check, and the
      undocumented-symbol gate — which is the "documentation gate" the M11 exit criteria name.
      `just/quality.just` today has `format`, `lint`, `layers`, `identity`, `abi` and `specs` and
      none of these four.
      **DONE.** `tools/quality/` holds all four — `just quality-swift-format`, `quality-licence`,
      `quality-spelling`, `quality-docs` — each in `ci.yml`'s `quality` job, and
      `just quality-gates-selftest` breaks what each checks in a tree derived from the live one and
      requires a red. **All 15 cases were watched failing on this host** (re-run and re-watched
      after the resumed session: 15 of 15).
      **A SECOND CORRECTION, AND A REGRESSION THIS GATE CAUSED:** `ROOTS` named the whole of
      `bindings/swift/Tests` while `EXCLUDED` named only `Sources/CyberdyneCore/Generated`, so
      `--fix` reformatted `Tests/CyberdyneCoreTests/Generated/LayoutTests.swift` — first line
      GENERATED FILE — DO NOT EDIT — and turned `integration.swift_overlay` and
      `integration.swift_overlay_gen` RED. Both generated trees are excluded now, the file was
      restored by regenerating it (it is byte-identical to ebdf8d5 again), both tests are green, and
      the regression is held by a fifteenth selftest case, `swift-skips-generated`: a misformatted
      file inside a generated tree must leave the gate GREEN. That case was watched going red with
      the exclusion removed, and the gate file restored and md5-verified afterwards.
      **STILL RED, AND NOT MINE TO FIX:** `just quality-licence` names 23 files added by this rung
      that carry no SPDX line — `platform/linux-native/` (5), `platform/stub/` (4),
      `samples/11-ship/` (4), ~~`src/backends/rhi/` (4)~~ (**done** — the section 1 agent added the
      header to all four and re-ran the gate, which now names none of them),
      `src/core/assets/remote*` (3),
      `tests/integration/test_stub_frame.cpp`, `tools/ci/port_engine_layer_diff.py`,
      `tools/docs/collect_ship.py`. They belong to peers writing them now; a header is one line and
      the gate names every file. The close cannot go green over this.
      **A CORRECTION WORTH RECORDING:** the swift-format gate was first written as unrunnable here,
      because `command -v swift` finds nothing — and that was wrong. There is a **Swift 6.3.3**
      toolchain installed through `swiftly`, reachable only from a login shell, which
      `bindings/swift/tools/cy_swift_module.py` has resolved that way since M4. The gate now
      resolves identically; it found **all 34 Swift files unformatted**, they were reformatted, and
      the fourteenth case runs here. *A check that reports "not available" is as wrong as one that
      reports a false pass, if it looked in the wrong place.* Two gates carry a numbered, shrink-only backlog
      (2 582 of 2 584 files without an SPDX header, 540 of 2 472 public symbols undocumented) and a
      baseline entry that has since been FIXED also fails, which is what stops a backlog being an
      allowlist — `tools/quality/README.md` §"the baseline is a snapshot" names what the close owes
- [ ] 7.2 The three acceptance scenarios the matrix records as unwritten — **strategy stress**,
      **control handover**, **headless server** — written where the taxonomy can run them, with the
      kind and budget they belong to stated rather than assumed.
      **NOT DONE, and not started.** These are *benchmarks*, not tests — `testing-and-quality`'s
      "Performance benchmarks" requirement is where the table lives — and each needs systems this
      rung's other sections were writing at the same time: 100 000 units with world streaming,
      network authority and replay recording; four networked players with vehicle entry, AI
      takeover, turret control, prediction, a spectator and a replay; a dedicated server at a fixed
      rate. **The cheapest true half is the headless one's BUILD claim** — *"it SHALL execute with
      no rendering, audio, or interface code linked, and a dependency on any of them SHALL fail the
      build"* — a link-closure check in the shape `just quality-layers` already has. **But it has
      nothing to check against yet, and that is the finding**: there is NO dedicated-server build
      configuration in this tree. "Headless" today is a RUN-TIME display-server choice on the
      ordinary binary — `just run-headless` is `just run-sample --headless` — so the renderer, the
      audio backend and the interface are linked into it whatever it chooses at startup. The
      scenario's build claim needs a configuration before it needs a check, and writing the check
      first would produce one that passes because there is nothing for it to look at
- [x] 7.3 Golden images **run against every enabled RHI backend and record which backend produced a
      failure**, which the requirement has said since M3 and one backend has never been able to test.
      **DONE.** `render.golden_backends` (`tests/render/test_golden_backends.cpp`) enumerates the
      backend registry instead of naming one, judges every entry against the ONE committed
      reference, and writes a row per backend — backend, device name, device class, outcome, and the
      numbers when it differed — to `CY_GOLDEN_LEDGER`, which `ci.yml`'s render job uploads. The
      classifier follows the spike's own rule: **an unrecognised device name is `unattested`, never
      `hardware`**, because the hosted Windows image's adapter 0 is a software rasteriser that does
      not set `DXGI_ADAPTER_FLAG_SOFTWARE`. A backend with no device is a row saying so with its
      reason, never an absence
- [ ] 7.4 **`build-and-packaging` — content audit**: *why is this in the build* (the reference chain
      from a declared root) and *what references this*; size by category, asset, plugin, world region
      and install bundle; cook and compile time by stage with cache hit rates.
      **PARTLY DONE — three of the four questions answered, and the fourth NAMED rather than
      invented.** `just content-audit` is the recipe: `cy_build audit` and `cy_build explain` have
      existed since M6 and **no recipe reached either**, so the reference chain was a capability
      with no workflow. `stage_report()` answers cook and compile time by stage with hit rates —
      every number was already on `BuildReport` and nothing aggregated them — and `content_report()`
      answers size by install bundle, by category and by asset, printing the SUM against the package
      set's own size so a report that has lost bytes says so. **Size by plugin and by world region
      are printed as `NOT REPORTED` with the reason**: a node does not record the plugin that
      declared it and `cybuild 1` has no world-region concept, so both need a declaration the graph
      does not carry, and an invented attribution in a size report is worse than an absent one.
      The row's Complete cell must not be written over those two
- [ ] 7.5 **`build-and-packaging` — provenance and symbols**: a build identity, engine and project
      revisions, lockfile hash, build and cook configuration, toolchain versions, manifest hash;
      shipping binaries stripped with symbols archived separately and retrievable by build identity;
      a reproducibility bundle archived by CI.
      **PARTLY DONE — the provenance half; the symbols half is untouched.** `Provenance` carried
      four of the requirement's seven fields; it now carries all of them: the ENGINE revision beside
      the project's (one field could not say which tree a difference came from), the plugin lockfile
      hash, the cook configuration (which is not the build configuration), and readable toolchain
      VERSIONS beside the toolchain digest — the digest is what a cache key compares, the versions
      are what a bug report quotes, and neither answers the other's question. The build identity and
      the content manifest hash are deliberately ONE field, because two would be two things that can
      disagree. A round-trip case in `unit.build_graph` names each of the seven, so a field dropped
      from the writer or the reader fails there.
      **NOT DONE: shipping binaries stripped with symbols archived separately and retrievable by
      build identity, and a reproducibility bundle archived by CI.** Both are packaging and CI work
      rather than manifest work — `objcopy --only-keep-debug` / `dsymutil` / PDB handling per
      platform, an archive keyed by `build_id`, and an upload step — and neither was started
- [ ] 7.6 **Downloadable content and distributed execution — contingent, and `design.md` §5 says
      why.** `docs/roadmap/risks.md` already lists distributed build execution as "M11 or later". If
      the distribution surface only becomes real at M11.e, this row's Complete cell moves there
      **with its reason recorded**, which is what a demotion is for
- [x] 7.7 **`developer-workflow-and-just`** — target selection on the build, test, package and deploy
      recipes, and **an impossible target explained**: the requirement says the workflow "SHALL say so
      and state what is required" rather than failing obscurely, and a second desktop is the first
      time that sentence has a second answer.
      **DONE.** `just/targets.toml` is the table and `tools/workflow/targets.py` reads it;
      `_resolve-target`, `_ctest` (so every `test-*` recipe), `content-package` and the new
      `deploy-install` all resolve through it, so one table gives one answer. **Three refusals that
      used to be one sentence are now three messages and three exit statuses** — a typo (3), a
      target this host cannot produce (2), a target nobody has written (2, naming the rung) — and
      `just env-targets --selftest` requires them to stay distinct. The table's `owner` is checked
      against `record.MILESTONES` **at load**, so a refusal cannot tell a developer to wait for a
      milestone that is not on the ladder, which is 7.8's defect enforced mechanically
- [x] 7.8 **What this rung must NOT claim over.** `just/release.just`'s four recipes —
      `release-version`, `release-changelog`, `release-artefacts`, `release-publish` — all refuse and
      name *"M12 — build-and-packaging"*, **a milestone that does not exist on a ladder whose
      `record.MILESTONES` ends at m11**. Implementing them is M11.e's; naming them here is how this
      rung's gate is stopped from writing a Complete cell for `developer-workflow-and-just` over four
      recipes that refuse.
      **HONOURED AND RECORDED.** No release recipe was implemented and none was touched. The rule is
      now enforced in one place a criterion cannot route around: `tools/workflow/targets.py` refuses
      at load any target whose `owner` is not on `record.MILESTONES`, and `tools/workflow/README.md`
      names `release.just`'s four recipes as the defect that check exists to prevent —
      *"a developer who reads that refusal is told to wait for nothing"*. `just deploy-device`
      refuses in the same shape and names **M11.e**, a rung that does exist

## 8. The artefact — `samples/11-ship`, the desktop half

**Done, with one gap the artefact reports rather than hides.** `samples/11-ship/` is the first
packaged project in this tree and the first thing in this repository ever to present a frame to a
window: `Device::create_swapchain` and `GraphExecutor`'s `wait_acquire`/`signal_present` have existed
since M3 with no caller anywhere. `just run-ship` is the one recipe; `smoke.ship` is the headless
half; `samples/11-ship/README.md` argues the shape.

- [x] 8.1 One project — **built, cooked, packaged and launched from a single recipe** on each desktop
      target. `samples/` holds fifteen entries today and not one of them is a packaged project.
      **Done**: `just run-ship` runs `cy_build` over a four-node graph (two imports, a cook, a
      package), proves a cold and a cache-warm build produce byte-identical manifests, installs it,
      **verifies** every chunk against the manifest in force, and launches `cy_sample_ship` out of
      that installation. The card is read **by logical name**: act 3 changes one line of content,
      watches `import:palette` come from the cache while `import:card` re-runs, reinstalls, and the
      same binary draws a different card — 2277 bytes before, 2363 after, no recompile
- [x] 8.2 It **draws**: through the native platform backend section 4 built, and through SDL3.
      **Done, on hardware**: 90 frames presented on each leg through `linux-x11` and through
      `desktop-sdl3`, on an NVIDIA GeForce RTX 5060, at `Bgra8Srgb 1280x720`. `--platform both` runs
      the same binary through each in turn, and **both legs report the identical plan hash**
      `0x74d32a3e5faa067b` — one submit, two passes, two derived barriers — which is the sharpest
      form the "no SDL assumption" claim has taken: the frame is byte-identical and only the object
      that opened the surface differs. **One interface addition was needed and it is the finding**:
      a `VkSurfaceKHR` is created by the platform against a `VkInstance` and nothing in
      `cy::rhi::Device` exposed one (`native_handle()` is the VkDevice), so
      `vulkan_backend.h` grew `vulkan_instance_handle(Device&)` — on the *backend*, never on
      `Device`, so it cannot become an identity query the renderer branches on
- [x] 8.3 **The artefact is honest about its own coverage on its own face.** **Done**: the window
      carries a five-row coverage table — SDL3 **BUILT**, native X11 **BUILT**, Metal **NOT
      EVALUATED**, D3D12 **NOT EVALUATED**, GPU vendors **1** — and the device that answered is
      drawn onto the card by `present.cpp` once it is known. **BUILT is deliberately not RAN**: the
      card is composed before a frame exists, so a row saying RAN would be a claim made before its
      evidence. The device is classified from its **identity** and never from a flag, which is
      §1.4.1's finding applied: this engine has **no device-type query at all** — `DeviceCapabilities`
      carries `device_name()` and `driver_version()` and nothing that says discrete, integrated or
      software — and the report says that is what it read
- [x] 8.4 The package carries its provenance (7.5) and the launch reproduces from it. **Done**: the
      installed manifest carries build identity, project, revision, platform, profile, toolchain
      fingerprint and content version, and the LAUNCH reads all of them back out of the installation
      rather than out of the driver's memory of what it asked for; `ship.py` checks the revision
      against the one the build was given
- [x] 8.5 **Capture it.** **Done**: `docs/design/images/m11d-ship-sdl3.png` and
      `m11d-ship-native.png`, each with a `.manifest` beside it naming the display server, the RHI
      backend, the device, its class, the swapchain format, the frame plan and the validation count.
      Both are **read back off the device after presentation**, not composed on the host — the
      program says which in its own output, because a picture of what was going to be drawn is a
      different claim from a picture of what was drawn. One per **platform leg** rather than per
      graphics backend: there is one graphics backend in this rung and two ways of opening a window

### What section 8 found, and what it did not close

**The frame trips two synchronisation-validation hazards per frame, on both legs, with an identical
plan hash — so they are the render graph's and not the window system's.** The artefact reports them
as a GAP: `cy_sample_ship` exits **3** ("it drew and tripped validation" — neither a pass nor a
fallen-over run) and `ship.py` records a gap, which `artefact.Report.exit_code` cannot turn back into
a zero.

1. **`SYNC-HAZARD-PRESENT-AFTER-WRITE`**, one per frame, **measured to one token.**
   `access.cpp`'s `Present` row is `{Stage::None, AccessFlags::None, ImageUse::Presentable, …}` with
   a comment arguing the semaphore orders the transition instead of a destination stage. Syncval
   disagrees — a `dstStageMask` of `NONE` makes the layout-transition write available to nothing, and
   it reports `write_barriers: 0`. **Setting that row's stage to `Stage::AllCommands` removes every
   one of these** (measured: 10 of 10 gone on a 10-frame run, the other hazard untouched). **The edit
   was made, measured, and REVERTED**, md5-verified: that row is `rhi-and-render-graph`'s own
   vocabulary and its comment is a deliberate design statement, so section 8 owes it the measurement
   and not an edit. Section 1's, or M11.d.5's
2. **`SYNC-HAZARD-WRITE-AFTER-READ` against `PRESENT_ACQUIRE_READ`**, one per frame, **not fixed by
   anything this section can reach.** Two candidates were tried and measured and neither moved it:
   widening the acquire semaphore's wait stage (`SubmitInfo::wait_binary_stage` defaults to
   `ColorAttachmentOutput`, which is right only for a frame whose first touch of the swapchain image
   is a render pass and wrong for this one, whose first touch is a copy — syncval's `read_barriers`
   widened to every stage and the hazard stayed), and importing the image as `ImageUse::Presentable`
   rather than `Undefined`. Both experimental edits were reverted. The acquire boundary needs an
   expression the graph does not currently have; the wait-stage default is worth changing on its own
   merits and is named here so the next reader does not re-derive it

**A third finding, smaller and already fixed here**: `GraphExecutor` releases its transient pool in
its destructor, so an executor declared at function scope outlives the device it holds. It segfaulted
on the native X11 leg and did **not** on the SDL3 one — the same dangling pointer, one allocator's
luck apart. `present.cpp` scopes it, and says so where it does.

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

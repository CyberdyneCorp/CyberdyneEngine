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
- [x] 4.3 `samples/00-empty` and the M3 golden images run on the native backend, which is the M11 exit
      criterion stated in the ROADMAP word for word
      - **The sample half is done and measured**: `cy_sample_empty --platform native --frames 120`
        opens a real X11 window, runs 120 frames and 119 simulation ticks, exits 0, and writes its
        trace to the same XDG path the SDL3 backend uses. All four backends — `sdl3`, `native`,
        `headless`, `stub` — run it, which is task 4.5's evidence as well
      - **The golden half needed the criterion read twice, and the second reading is satisfiable.**
        The golden SUITE cannot carry it: `tests/render/CMakeLists.txt` links NO platform target and
        no case there constructs a `DisplayServer` — the frames are rendered offscreen through
        Vulkan with no window and no swapchain — so no platform backend can change one texel of
        `render.golden`, and forcing a window into it would make every machine that runs the render
        suites need a display, which is the opposite of the argument `design.md` §1 and
        `tests/render/README.md` make. **But the golden IMAGE is a different object from the golden
        suite.** `tests/render/references/first_light.png` is the picture `samples/03-first-light`
        draws, and `test_golden_frame.cpp` says so itself — *"`--frames 1` on the sample is the same
        frame"* — and the sample, unlike the suite, is HOSTED: it owns a `cy::Platform`, a
        `cy::DisplayServer`, a `cy::Runtime` and the host loop, and until now that pair was
        hardwired to SDL3
      - **Done, and measured on hardware.** `cy_sample_first-light` gained the same
        `--platform headless|sdl3|native|stub` selector `samples/00-empty` has;
        `samples/03-first-light/golden_legs.py` runs every leg at the reference's own 192x108 and
        `--frames 1`, and `smoke.first_light_legs` is the CTest entry that runs it (27.9 s, passed).
        **All four legs produced a capture that is EXACTLY the committed reference** — 62208 bytes
        of RGB, `md5 a23a1510b36bc8e9510245895fda59ab`, zero differing texels, no tolerance applied
        — and the native leg reports `platform=linux-native display=linux-x11`, the same display
        server `docs/design/images/m11d-ship-native.manifest` names. The legs are also compared
        against **each other**, which is the claim that survives a regenerated reference
      - **The check can go red**: the same binary with `--no-shadows` differs from the reference in
        129 texels, worst channel 132, which `golden_legs.py` reports as a GAP and a non-zero exit
      - **It does not claim a platform backend PRESENTED the frame**, because this sample renders
        offscreen and always has. That claim is `samples/11-ship`'s (task 8.2): two captures through
        `platform/linux-native` and `platform/desktop-sdl3`, byte-identical, manifests differing in
        one line. The two measurements are complementary — 11-ship proves a PRESENTED frame does not
        depend on the display server, this one proves the M3 REFERENCE IMAGE does not depend on the
        platform beneath it — and neither replaces the other
      - The earlier note that `cy_test_render_golden` would not compile from section 1's in-flight
        `ImageLayout` removal is **resolved**: the suite builds and is green on this tree in
        `build/m11d-golden` — 5 cases, 50 assertions, 0 failed
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
- [x] 7.2 The three acceptance scenarios the matrix records as unwritten — **strategy stress**,
      **control handover**, **headless server** — written where the taxonomy can run them, with the
      kind and budget they belong to stated rather than assumed.
      **DONE — all three written, registered and green, each asserting the property this rung's
      delta names for it; and the first one found a defect before it could pass.**
      `tests/acceptance/` holds them, its CMakeLists.txt carries the kind-and-budget table, and its
      README says what each asserts and what it does NOT exercise:
      * **`smoke.acceptance_strategy_stress`** (smoke, 30 s) — 8 participants, 4 teams, 100 000
        units (20 000 moving), 5 000 groups, 1 000 structures, 320 group orders → **6 400 validated
        member commands a tick**, every one reaching the replay-recording seam; the framework's share
        of the tick measured against the simulation's and compared with a committed ceiling.
      * **`integration.acceptance_control_handover`** (integration, 1 s) — four players, a vehicle,
        a gunner, an AI taking the wheel, a spectator: the vehicle's motion continues across the
        handover, the gunner is accepted on every tick including the handover's, the departed driver
        is refused `NotControlled` on the next tick, the spectator reads the true controllers every
        tick and is refused every order, and the recorded stream replayed into fresh state matches
        at every tick.
      * **`smoke.acceptance_headless_server`** (smoke, 30 s) — `cy_headless_server`, **the
        dedicated-server BUILD configuration the tree did not have**: 100 000 entities, tiered AI
        (~2 700 thinks a tick, none starved), ~270 orders a tick through one group binding per squad,
        a kinematic Jolt body per entity, stepped at a fixed 30 Hz. Its trace is read back by
        `tools/trace/trace_inspect.py`, which does not link the engine, and must carry every counter
        a server owes at every tick. **"A dependency on any of them SHALL fail the build" is held
        twice**: at configure time by `cy_require_headless` (gameplay's own closure check, reused so
        there is one forbidden list) over the DECLARED closure, and POST_BUILD by
        `headless_closure.py` over the LINKED binary's symbols and dynamic dependencies — which,
        pointed at `cy_sample_ship`, names SDL, Vulkan, the RHI, the render graph and libX11.
      **THE DEFECT (the regression is in `src/gameplay/tests/test_control.cpp`).** The strategy
      stress could not be BUILT: `ControlRegistry::kMaxGroups` was **64** against the scenario's
      5 000, and with the cap lifted the framework measured **94 % of a strategy tick** — 50.7 ms of
      validation against 3.2 ms of simulation — because `controls()`, asked once per member command,
      walked every binding and searched the group LIST for every group binding. It now answers from a
      binding index and a membership index (exact for an entity in more groups than the inline four,
      by falling back to the walk for that entity alone): **0.5–1.2 ms a tick, 22–34 % of this
      scenario's deliberately modest simulation** on a host at load 20+. Three registry cases — 5 000
      groups, every way a binding or membership goes away, an overflowed membership — were watched
      red with the cap restored to 64 (the first) and with `unindex_binding` deleted (the second),
      and the strategy stress red at 94 % before the index; each mutation restored and md5-verified.
      **THE CEILING IS 50 %, AND IT IS A REGRESSION GUARD, NOT THE CLAIM THAT A THIRD IS "SMALL".**
      A first draft set 15 % before anything was measured; the measurement replaced the guess and
      the test records both. `gameplay-framework`'s "small, reported fraction" is judged against a
      real strategy tick — pathfinding, combat, streaming — which this scenario does not have yet.
      **NOT EXERCISED, and said on each test's own output:** world streaming and network authority
      (strategy stress), networking and prediction (control handover), connections, replication and
      world streaming (headless server). **The headless server's physics finding**: as DYNAMIC
      bodies two metres apart, orders woke neighbours through contacts and the step went from 8 ms to
      140 ms in thirty ticks; kinematic units whose orders run out after half a second hold it at
      ~25 ms of the 33 ms tick — single-threaded, because the server hands Jolt no job system.
      Criterion: `m11d:acceptance-scenarios`.
      *Superseded record, kept:* These are *benchmarks*, not tests — `testing-and-quality`'s
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
- [x] 7.4 **`build-and-packaging` — content audit**: *why is this in the build* (the reference chain
      from a declared root) and *what references this*; size by category, asset, plugin, world region
      and install bundle; cook and compile time by stage with cache hit rates.
      **DONE — per FILE, from a DECLARED root, with what nothing asked for flagged, and the two
      sizes that were NOT REPORTED now reported from declarations.** Three pieces:
      * **Declared roots.** `cybuild 1` gains a top-level `root "<node>"` line
        (`BuildGraph::declare_root`, resolved at `finalize()`, a root naming no node refused
        `NotFound`, round-tripped by the writer). `roots()` was an INFERENCE — every node nothing
        consumes — so a stray node was its own root and nothing could ever be called unreferenced.
      * **`audit_content()` / `cy_build build --audit`.** Every file in the package with the chain
        from the nearest declared root to the node that produced it and the project files that node
        read; then every node no declared root reaches and every project file no node reads,
        DECLARED OR DISCOVERED — `NodeResult::discovered` now carries discovery on a cache hit as
        well as on a run, because a glTF's `.bin` is referenced and flagging it would be the false
        positive that teaches a team to ignore the audit (`integration.build_content` holds it on a
        warm build — watched red with the cache-hit half deleted). Exit **4** when anything is
        flagged. A
        description with no root is refused rather than inferred.
      * **Size by plugin and by world region** from `plugin` / `region` node fields — attribution
        only, never in the key, like `bundle`; undeclared content reported as `(undeclared)`, each
        section's sum printed against the package's own size.
      **The artefact**: `samples/11-ship/project/build/ship.cybuild` declares `root "package:card"`
      and `ship.py`'s act 1b requires every one of the package's four files to trace to it and
      **nothing unreferenced** — a copy of a card dropped into `project/card/` fails the run naming
      `UNREFERENCED source card/stray.cycard` (watched, then removed and the project md5-verified).
      Three `unit.build_graph` cases hold the library half. Criterion: `m11d:content-audit`.
      *The earlier record, kept:* **PARTLY DONE — three of the four questions answered, and the fourth NAMED rather than
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
      **SYMBOLS: DONE on ELF. REPRODUCIBILITY BUNDLE: produced and verified, NOT archived BY CI —
      which is why this box stays open.**
      * **`tools/build/symbols.py`** — `split` strips a binary (`objcopy --strip-all` plus a
        `.gnu_debuglink`) and archives its symbols in GDB's own `debug-file-directory` layout keyed
        by the GNU build-id, indexed under the PACKAGE build identity too; `locate` answers from
        either; `verify` proves the archive belongs to the binary with four checks, because another
        build's symbols symbolicate without complaint and name the wrong line — stripped; one
        build-id; the debuglink's CRC-32 over the archived bytes; and `main` symbolicated through the
        archive to its source line while the stripped binary alone places nothing.
        `unit.build_symbols` builds a probe for every refusal (another build's symbols under this
        build-id, symbols edited after the split, unstripped, no build-id, no `-g`, not ELF).
      * **THE DEFECT WRITING IT EXPOSED**: `cmake/profiles.cmake`'s **Shipping row compiled with no
        `-g`**, while its own comment said symbols were split at packaging time — a shipping build
        had nothing to archive and a crash in one could never be placed on a line. Shipping now
        compiles `-O3 -g -DNDEBUG` (`/Zi` + `/DEBUG /OPT:REF /OPT:ICF` on MSVC; debug information does
        not change generated code), and `unit.build_symbols` is handed the CONFIGURED Shipping flags
        and fails on a configuration without debug information — watched red on `-O3 -DNDEBUG`.
      * **The artefact launches the STRIPPED binary.** `ship.py` act 1c splits `cy_sample_ship`,
        verifies and locates the archive, then every launch runs the stripped copy — so what was
        verified is what ran (measured: the stripped binary presented 30 frames through
        `linux-x11` on the RTX 5060) — and writes `reproduce/<build id>/`: the package manifest, the
        description, the lockfile statement, the build tree's configuration, every artefact hash and
        the symbols, and requires that bundle's symbols to verify on their own. The launch now prints
        and the driver checks all seven provenance fields, the ENGINE revision separate from the
        PROJECT's (`git log -1 -- samples/11-ship/project`).
      * **Not done, and not this rung's to do:** the CI upload of that bundle is a step in `ci.yml`,
        which the close phase owns and M11.e's full matrix rewrites; and Mach-O (`dsymutil`) and
        PE/PDB are the same four checks with other spellings, which this Linux host cannot produce —
        a non-ELF input is refused by name, never treated as stripped. Both travel with the row to
        M11.e (7.6). Criterion: `m11d:provenance-and-symbols`
- [x] 7.6 **Downloadable content and distributed execution — contingent, and `design.md` §5 says
      why.** `docs/roadmap/risks.md` already lists distributed build execution as "M11 or later". If
      the distribution surface only becomes real at M11.e, this row's Complete cell moves there
      **with its reason recorded**, which is what a demotion is for.
      **DECIDED: the cell MOVES TO M11.e, and the prediction held.** `design.md` §5.1 records it
      under *"DECIDED — task 7.6"*: downloadable content is a SIGNED package set and nothing in
      `core/crypto` signs; distributed execution needs remote workers, a second machine; the
      reproducibility bundle exists (7.5) but archiving it is a `ci.yml` step. The move has two
      halves and both are made: `m11d.toml`'s `roadmap-tiers` no longer expects the row, and
      `m11e.toml`'s does, received by M11.e task 4.4 and `downloadable-content-and-distributed-execution`,
      which already existed for exactly this. `m11d:build-and-packaging-moves-to-m11e` fails if
      either half is undone — watched red with M11.e's line deleted. **What the close phase owes**:
      `capability-matrix.md` still shows the row's **C** under M11.d, and `status.yaml` /
      `ROADMAP.md` follow it; the close phase edits them
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
      milestone that is not on the ladder, which is 7.8's defect enforced mechanically.
      **AND THE DEFECT THAT WRITING IT EXPOSED, WHICH IS WORTH MORE THAN THE CRITERION.** Before
      `_ctest` took the flag, `just test-render --platform <anything>` *accepted* it: the recipe
      passed the argument through to ctest, which drops an unrecognised positional without a word.
      Measured — three different argument lists selected the same 20 tests. So every caller who has
      ever written `just test-<kind> ... --platform ...` read an unfiltered run as a filtered one.
      A flag that is accepted and ignored is worse than one that is rejected, and the fix is not
      allowed to be "silently continue": `--platform` with no name exits 2 naming the flag, an
      unknown name exits 3 through `_resolve-target`, and a PORT name (`stub`, `native`) — which
      deliberately selects the host build, since a port's closure has no fifty executables for ctest
      to run — now **says on stderr** that it did so and which suite carries the port's own claim.
      **The regression test is the invariant, not the instance**: `tools/ci/test_recipes.py` case
      *"a recipe never accepts a flag it then ignores"* reads every recipe in `just/` and fails on a
      flag arm with an empty body, or on a variable an argument loop writes that the recipe never
      reads. Proved red twice by reinstating each shape in `_ctest` (1 of 6 cases failed, exit 1),
      then restored and md5-verified. A sweep of all 13 `.just` files under that rule finds no other
      offender: `build-reap`'s `--apply` is read arithmetically as `((apply))`, and the recipes that
      hand `${rest}` on wholesale forward to argument parsers that *do* refuse — `ship.py` uses
      `parse_args`, not `parse_known_args`. `ctest` is the one consumer in the workflow that does not
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
- [x] 9.2 An `m11e-open` criterion using the double-star glob form — satisfied and RE-POINTED by
      the insertion; see the close phase's verdict at the end of this file
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
- [ ] 9.7 **Bind the quiet-host marker to the real wrapper.** Moved here from M11.c's tenth close by
      the owner's ruling. `tests/harness/src/quiet_host_marker.cpp` trusts `CY_QUIET_HOST` when the
      named pid is a live ancestor with the marker's start tick and the BASENAME of its
      `/proc/<pid>/exe` is `cy_quiet_host`, so any binary renamed `cy_quiet_host` is trusted. It did
      not block M11.c because the forgery only goes in the strict direction: it can make the harness
      enforce a stall ceiling on an unchecked host (a possible false red), never let a case escape a
      ceiling it should get (no false green). Check the executable's identity, not its name — e.g.
      compare the ancestor's `/proc/<pid>/exe` against the `cy_quiet_host` the build produced (same
      dev/inode or resolved path, passed at configure time), or hand the child a secret over an
      inherited file descriptor — and add a copied/renamed-binary forgery case to
      `smoke.quiet_host_marker`, proven red on the current check

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


## The close phase's verdict — M11.d DOES NOT CLOSE, AND NEITHER DOES M11.c TODAY

Written by the close phase, which owns `gates.toml`, `ci.yml`, `status.yaml`, `capability-matrix.md`
and `ROADMAP.md`. **No gate was flipped. No tier was written. `ci.yml`'s milestone job was not
moved.** Every row below was run at the close rather than read off a task list, and the runs are
named so the next reader can repeat them.

### The fact that frames everything else: THIS PHASE IS STILL RUNNING

The close phase started while **at least three peer agents were still writing code and building**
(`build/m11d-golden`, `build/m11d-just`, `build/m11d-crit` were all mid-configure at 12:00, and the
interval snapshotter committed at 11:39, 11:48 and 11:56). Two criteria in this very ledger —
`native-platform-backend` and `native-backend-runs-the-m0-sample` — were **rewritten by a peer while
the close was reading them**, in both cases more strictly than the close's own first attempt, and
`samples/03-first-light/golden_legs.py` landed at 11:52. A gate decision taken now is a decision over
a tree that is still moving, and that is the strongest single reason not to have taken one.

### What was proved GREEN, first-hand

- `specs` (76 of 76), `format` (2 258 files), `layering` (11 of 11 — **with two platform
  implementations in the tree**, which is this rung's whole claim seen from the gate's side),
  `workflows` (`just ci-check`, 19 of 19), `roadmap-record`, `generate-check`,
  `maintenance-deps-check` (37 of 37), `roadmap-debts --check`.
- `documentation-gate`: 1 932 of 2 472 public symbols documented, 540 in the declared backlog,
  **0 new**. `full-gate-set`: all three gates pass after the repair below. `just
  quality-gates-selftest` proves **15 of 15** cases that each of the four gates still refuses what it
  exists to refuse — so none of the four is a check that cannot fail.
- `ship-sample-exists`, and `ship-sample-drawn`: `docs/design/images/m11d-ship-native.png` was opened
  and read at the close. It is a real frame from the packaged application on the X11 backend, and it
  states in its own pixels that it is a packaging proof and not a game.
- **The M0 sample on the native backend, run at the close**: `cy_sample_empty --platform native
  --frames 60` → `display=linux-x11 window=1`, 60 frames, 62 ticks, exit 0, trace written to the
  XDG path. `sdl3` and `headless` likewise.
- **THE GOLDEN HALF OF 4.3 IS DONE, AND THE CLOSE RAN IT**: `samples/03-first-light/golden_legs.py
  --build-dir build/dev` renders one frame per platform backend and compares each against
  `tests/render/references/first_light.png`. All four legs — headless, sdl3, **native**, stub —
  produce **62 208 bytes EXACTLY equal to the committed reference**, and each leg is byte-identical
  to the headless one: *"the platform backend is not in the picture."* 7 of 7 steps satisfied. It is
  registered as `smoke.first_light_legs`, so it runs under the smoke suite rather than only by hand.
  **Task 4.3 is therefore satisfied in substance** — it is left unticked only because its author, not
  the close, should tick it.

### What is RED — and none of it is a declarable gap, because every row has been started

1. **`lint` — A PERMANENT GATE, RED ON THIS RUNG'S OWN NEW CODE.** A full sweep at the close
   (`just quality-lint`, 1 344 files against `build/dev/compile_commands.json`) exits **123 with 25
   clang-tidy errors across 9 files**, and **12 of the 25 are in `platform/linux-native/` — the
   rung's headline deliverable**:
   - `platform/linux-native/src/x11_display_server.cpp` — 4 × `readability-math-missing-parentheses`,
     1 × `modernize-use-integer-sign-comparison`, 1 ×
     `bugprone-implicit-widening-of-multiplication-result`, 1 × `bugprone-branch-clone`
   - `platform/linux-native/src/linux_platform.cpp` — 4 × `readability-implicit-bool-conversion`,
     2 × `readability-math-missing-parentheses`, 2 ×
     `readability-convert-member-functions-to-static`, 1 × `modernize-use-integer-sign-comparison`
   - `platform/stub/src/stub_display_server.cpp:20` — `readability-math-missing-parentheses`
   - `samples/11-ship/card.cpp:84`, `samples/11-ship/present.cpp` — math parentheses, nested
     conditional operator
   - `src/core/assets/src/remote.cpp:360` — `readability-container-size-empty`;
     `src/core/assets/tests/test_remote.cpp:312` — `modernize-use-auto`
   - `src/rendering/graph/tests/test_aliasing.cpp:257` —
     `bugprone-implicit-widening-of-multiplication-result` (`64 * 1024` into a `u64`)
   - `tests/render/test_golden_backends.cpp` — `misc-unused-using-decls`;
     `tools/build/src/package.cpp` — 2 × `modernize-use-auto`
   Every one is a one-line fix and none is a design question. **Deliberately not applied here**:
   every one of those files is in a directory peers were writing to during this phase, and a
   close-phase edit landing under an active author is the collision this project has already paid
   for. **This is also why M11.c cannot close today**: `lint` is a permanent gate on every ledger, so
   M11.d's in-flight code holds the rung below it shut.
2. `core-rows-at-complete-grade` — **0 of 71 requirements** across the six rows map to a test, a gate
   or a recorded exemption. The tool works; the map is empty. Largest single piece of unfinished work.
3. `release-recipes-stop-refusing` — all four `release-*` recipes still exit through
   `_not-implemented` (`just release-version` → *"not implemented (task M12 — build-and-packaging)"*).
   **The ledger's own instruction applies: `developer-workflow-and-just` is DEMOTED to M11.e with
   this reason rather than claimed over four refusals**, and M11.a's declared gap
   `developer-workflow-at-working` does not close here either.
4. `developer-workflow-recipes` — **8** recipe lines name a milestone the ladder does not carry:
   `deploy.just:110`, `quality.just:166`, `quality.just:285`, `release.just:12,16,20,24`,
   `run.just:408`. Two of the eight are this rung's own additions.
5. `content-audit-and-symbols` — `cy_test_integration_packaging` **does not exist anywhere in the
   tree**; task 7.5 says in its own words that the symbols half was never started.
6. `rhi-interface-gaps-settled` — **the work is real and green and the criterion is stale.**
   `cy_test_unit_rhi` passes **45 of 45**, including twelve `gap N:` interface cases; the criterion's
   filter names four cases that do not exist and selects **0 of 45** — run and confirmed at the close.
   The real names are `gap 2: the pool class is MET, not compared for equality`, `gap 4: ownership is
   a capability and a queue kind, never a family index`, `gap 7: the engine substitutes the depth
   format, and never drops the stencil`; gap 6's is `the pipeline cache round-trips through a path,
   and an absent one is a cold start`, **in a different binary**
   (`cy_test_integration_rhi_pipeline_cache`), which the criterion cannot span as written.
7. `null-backend-refuses-what-it-cannot-do` — same defect: `-tc=the null backend refuses*` selects
   **0 of 45**. **6 and 7 are deliberately NOT repaired here.** Aligning a filter to whatever happens
   to pass, at the gate, by the person deciding the gate, is how a check stops being one; the person
   who wrote those cases should name them.
8. `port-touches-no-engine-layer` — **red for the workflow's commit convention, not for the
   abstraction.** It looks for a commit whose subject names `native-platform-backend` in the last 40;
   every commit in this phase is `WIP snapshot: N file(s) in flight`, written by the interval
   snapshotter, so the port's commits are not separable and the criterion reports *"the native backend
   has not landed"* about a backend that demonstrably has. Run over the whole rung instead
   (`tools/ci/port_engine_layer_diff.py --since ebdf8d5`) it names 14 engine-layer files — all of them
   section 6's `core-assets` remote-file-serving, memory and ECS work, **none of them the port's**.
   The criterion needs a range or a marker this orchestration can actually produce.
9. `m11d5-open` — `openspec/changes/implement-m11d5-backends/tasks.md` has **no checked task outside
   section 0**, so the rung this rung's spike carved out has been scoped and not entered. The handover
   check working exactly as designed.
10. `roadmap-tiers` — all nine rows are below Complete, which is correct: the closing change writes
    them and this rung is not closing. **It must not be written while 1–9 stand.**
11. `plan-consistency` — `tools/roadmap/falsifiability.toml` still carries an `m11d:m11e-open` entry
    for a criterion re-pointed to `m11d5-open`, records `ship-sample-exists` and `ship-sample-drawn`
    as red when both now pass, and has **zero entries for any of `m11d5`'s 19 criteria**. It cannot
    pass until `just roadmap-falsify --record` is re-run — hours, and worth doing only on a quiet tree.

### The two repairs the close did make, both of which make a check stricter

- **`full-gate-set` called a recipe that does not exist.** It ran `just quality-licence-headers`; the
  recipe is `just quality-licence`. Behind the misname the gate was **genuinely red: 19 files added
  by this rung carried no SPDX header** — all of `platform/linux-native/`, `platform/stub/`,
  `samples/11-ship/`, `src/core/assets/remote.*`, `tests/integration/test_stub_frame.cpp`,
  `tools/ci/port_engine_layer_diff.py`, `tools/docs/collect_ship.py`. The name is corrected and the 19
  headers added, which is what the gate asks for and not a loosening — the baseline only shrinks, and
  the gates' own selftest still proves 15 of 15 refusals.
- **`native-platform-backend` could not fail.** Its body searched `src/` while every implementation of
  the interface lives under `platform/`, so it matched only the abstract interface header and
  **exited 0 against ebdf8d5, the commit this rung opened on**, when SDL3 genuinely was the only
  implementation. `just roadmap-falsify` had already classified it "no mutation". The close rewrote
  it; a peer's stricter version — requiring `Platform` as well as `DisplayServer`, excluding headless
  and the stub, and requiring `platform/CMakeLists.txt` to build it — superseded that rewrite and is
  what stands. Measured green on this tree, red at ebdf8d5.

### Two smaller findings, recorded rather than fixed

- **The M11.c ledger run started by the close FAILED ITS BUILD LEG ON A RACE, not on a defect.**
  `m0:build` died at 1 249 s with `samples/03-first-light/main.cpp:50: fatal error:
  cy/platform/stub_display_server.h: No such file or directory` — `build/dev` had been configured at
  11:35, before a peer added `cy::platform-stub` to that sample's link list at 11:48. Rebuilding the
  same target after CMake re-configured succeeds. The run was therefore discarded rather than
  reported, and **M11.c's gate was left at `joins-on-close` because no trustworthy full run exists.**
- **The stub platform's default user mount is a relative path that must already exist.**
  `StubPlatform::user_mount_` defaults to `"stub-user/"` and nothing calls `set_user_mount()`, so
  `cy_sample_empty --platform stub` fails startup with *"the trace file could not be created (Io)"*
  in any working directory without that folder, and succeeds in one with it. Task 4.4's "it runs a
  headless frame" is true; task 4.3's "all four backends run it" is true only with that undocumented
  precondition. Either default it to a directory the platform creates, or make the refusal name the
  path it wanted.

### What the next run owes, in order

The three lint errors (1) — they block **both** gates and are minutes of work. Then the requirements
map (2), the two stale filters (6, 7), a `port-touches-no-engine-layer` this orchestration can
satisfy (8), the packaging suite (5), the eight recipe lines (4), and finally a falsifiability
re-record (11) **on a tree with no other agent writing to it**, followed by one clean
`just roadmap-milestone m11c` and one clean `just roadmap-milestone m11d`.

**On task 9.2 — the only box the close phase ticked.** An `m11e-open` criterion in the double-star
glob form: **satisfied, and re-pointed.** The insertion of M11.d.5 moved the handover — `m11d.toml`
carries `m11d5-open` in that exact form and `m11d5.toml` carries `m11e-open`, so the chain
m11d → m11d5 → m11e is unbroken. A handover check still naming M11.e here would have skipped a rung,
which is the one thing a handover check exists to make impossible.

## The close phase's second verdict — THE LEDGER RAN TO A NUMBER, AND M11.d STILL DOES NOT CLOSE

Written by the close phase, which owns `gates.toml`, `ci.yml`, `status.yaml`, `capability-matrix.md`
and `ROADMAP.md`. **No gate was flipped. No tier was written. `ci.yml`'s milestone job was not
moved.** The verdict above stands and is not restated; what this section adds is the thing it did
not have — **both ledgers run end to end, to a count**.

### The two runs, and the conditions they were taken under

| | `m11c` | `m11d` |
|---|---|---|
| command | `CY_BUILD_DIR=build/m11c-final just roadmap-milestone m11c` | `CY_BUILD_DIR=build/m11c-final just roadmap-milestone m11d` |
| window | 16:31 → 19:15, 9 824 s | 19:15 → 21:07, 6 744 s |
| log | `/tmp/close-m11c.log` | `/tmp/close-m11d.log` |

**HEAD was `b27669a` at the first line of the first run and at the last line of the second**, `git
status` was clean at both ends, no `.tmp.<pid>.<hash>` file exists anywhere in the tree, and `ps` at
every poll showed **no peer build, `ctest`, prover or second ledger alive**. Unlike the run the
verdict above had to discard, nothing moved underneath these.

### `M11D is not closed: 19 of 423 evaluated criteria failed.`

| bucket | m11d | m11c, for comparison |
|---|---|---|
| declared | **428** (410 inherited, 18 new) | 440 |
| evaluated on this host | **423** | 435 |
| PASS | **377** | 397 |
| FAIL (not a declared gap) | **19** | 10 |
| declared gaps, still open | **27** | 28 |
| declared gaps that NOW PASS (these DO block) | **0** | 0 |
| NOT EVALUATED, legitimately | **5** | 5 |

**READ THE 410 FIRST.** M11.c's gate is still `joins-on-close`, so **this ledger does not evaluate a
single one of M11.c's criteria** — the flat ledger merges the criteria of every *green* gate below
the target. A green `m11d` run today would therefore say nothing whatever about the fifteen image
rows. That is a second reason, independent of its own 19, that this rung cannot close before the one
beneath it does.

**NINE OF THE 19 ARE THE INHERITED SET, AND EVERY ONE IS THIS RUNG'S OWN CODE**: `m0:lint`,
`m0:test`, `m1:four-profiles`, `m3:sanitizers-render`, `m4:sanitizers`, `m6:open-world-artefact`,
`m6:open-world-recipe`, `m8c:feature-options-off`, `m7:plan-consistency`. The three defects behind
seven of them are run down to a line in `implement-m11c-image/tasks.md` under *THE CLOSE, THIRD
ATTEMPT*; in one sentence each:

1. **`cy_build` writes a manifest it cannot read back.** `main.cpp:256` puts
   `describe_toolchain()`'s **five newline-terminated lines** into `provenance.toolchain_versions`;
   `text::quote` (`text.cpp:92`) escapes `"` and `\` and not `\n`; `text::read` (`text.cpp:62`)
   splits on `\n` before parsing — so every package manifest written since task 7.5 is unparseable.
   Reproduced at the close: `cy_build install --package …/base.cypackage` → *"the package manifest
   could not be parsed: unterminated quoted word"*, exit 1. **This is also
   `m11d:ship-sample-on-desktop`, and it has regressed M6's closing artefact**, which had been green
   since M6.
2. **`src/backends/rhi/tests/test_interface_gaps.cpp:190-191` compares `const char*` by pointer** —
   green in Development, red in Debug and under ASan/UBSan.
3. **`samples/11-ship/card.cpp:222` (`-Werror=null-dereference`) and `present.cpp` (six
   `-Werror=unused-function` with `CY_RENDERER_VULKAN=OFF`) do not compile** in the sanitizer and
   feature-off configurations.

### THE 18 NEW CRITERIA: 8 PASS, 10 FAIL

**PASS** — `documentation-gate`, `full-gate-set`, `native-platform-backend`,
`native-backend-runs-the-m0-sample`, `porting-surface-against-a-stub`, `shader-targets-emitted`,
`ship-sample-exists`, `ship-sample-drawn`. The rung's headline claim — *a native `Platform` and
`DisplayServer` that is neither SDL3's nor a stub, compiled, running the M0 sample, with a porting
surface that builds against a stub* — **is green on a ledger run, not on a report**.

**FAIL** — `rhi-interface-gaps-settled`, `null-backend-refuses-what-it-cannot-do`,
`port-touches-no-engine-layer`, `developer-workflow-recipes`, `release-recipes-stop-refusing`,
`content-audit-and-symbols`, `core-rows-at-complete-grade`, `ship-sample-on-desktop`,
`roadmap-tiers`, `m11d5-open`. Nine of these ten are diagnosed in the verdict above and the diagnoses
held on re-measurement; the tenth, `ship-sample-on-desktop`, now has defect 1 as its named cause
rather than a symptom.

### The sections that moved, verified rather than assumed

`git show e39ddd3 -- tools/roadmap/milestones/m11d.toml` removes exactly
`metal-backend-is-native`, `d3d12-backend-exists` and `golden-images-across-three-backends`, and
`m11d5.toml` carries all three plus `rhi-row-at-complete-grade` and
`three-backend-images-committed` — **moved, not deleted**, in one commit. `m11d`'s terminal criterion
changed from `m11e-open` to `m11d5-open` in the same diff, so the chain is unbroken. Nothing in this
close evaluated a Metal or a D3D12 claim, and nothing in `m11d.toml` asks it to.

### What the reader decides, and what is not the closer's to decide

Nothing was promoted and no gate moved, so there is nothing to walk back. **19 of M11.d's 46 tasks
are unchecked** — sections 5 (MSAA and multi-view), 7.2, 7.4, 7.5, 7.6, all of 9 but 9.2, and all of
10 — and `core-rows-at-complete-grade` at **0 of 71 requirements mapped** is the largest single
piece of it. The three compile-and-test defects are minutes of work in files this phase may not
touch; everything else on the list is a rung's work rather than a gate's.

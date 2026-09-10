# Tasks: M8.c — Spectacle

Ordered. Section 1 is first because it is the debt this milestone inherited rather than the work it
chose, and because it is the one thing here that a later milestone cannot add cheaply: a firewall
retrofitted after two systems already write through it is a rewrite.

## 1. The determinism firewall — the criterion that arrived with its subjects

- [ ] 1.1 State where the firewall is ENFORCED, not only where it is required. `vfx-system` and
      `ml-inference` each state it from their own side; neither names the enforcement point, and
      M8.b built the two candidates — `gameplay-framework`'s command origin and the ECS write path
- [ ] 1.2 A development-build diagnostic that names the writer, the component and the path, per
      `vfx-system` "Development builds SHALL detect and report attempts to write replicated or
      physics-owned components from VFX-driven code paths"
- [ ] 1.3 A non-pinned model feeding an authoritative AI node is refused **at cook time**, per
      `ml-inference`'s "Multiplayer desync is prevented at cook time" — a runtime diagnostic is not
      that requirement
- [ ] 1.4 **The test, in both directions and as a negative control**: a VFX readback and a
      non-pinned inference result each attempt a gameplay write and are each refused; and a
      re-simulation reaches the identical state digest regardless of what either did. A test that
      cannot fail when the firewall is removed is not this criterion — mutate the firewall and
      prove the test goes red

## 1b. The shader and pipeline layer — the wall M8.b's gate named and did not schedule

**M8.b assembled a frame out of the renderer's own modules and stopped one layer short.**
`FrameAssembly` hands each pass's record callback to its caller, and the vertical slice supplies
none — which is why the slice's picture is DRAWN rather than captured, and why the milestone's own
report says a person could build all of M8.b and still not *see* it without writing their own
renderer.

This is scheduled here rather than assumed because a particle system is the first consumer that
cannot proceed without it: task 5.1 puts particles inside the existing slice holding its frame
budget, and there is nothing for them to be recorded into. M8.b's gate called this its day-one wall.
A tier claimed with no task beneath it is the slip that cost the M5.5 insertion and was caught again
at M7 — this is the same shape, found before rather than after.

- [ ] 1b.1 A shader and pipeline layer above `FrameAssembly`: pipeline state objects, bindings, and
      the record callbacks each pass declares. The seams exist — `FrameSinks::passes` and
      `SceneIndex` — and the layer above them does not
- [ ] 1b.2 The vertical slice supplies real record callbacks, so its frame is CAPTURED rather than
      drawn. Its committed screenshot is then the engine's own output
- [ ] 1b.3 `material-compiler` emits Slang that something compiles, closing M7's recorded gap: the
      bundle carries the IR, the generated source, the cost report and the cook key, and nothing
      invokes the compiler. **Report rather than fake** if the shader standard library the generated
      source imports (`cy.material`, `CyMaterialContext`, `CySurface`) still does not exist
- [ ] 1b.4 A particle renderer is the first consumer, and it proves the layer by using it rather
      than by a test written beside it

## 2. VFX — `vfx-system` → W

CyberGraph is the authoring layer. The IR is its own: a particle kernel is not an expression DAG,
not a register machine and not a timeline, and M8.b's spike is why that is stated rather than
rediscovered.

- [ ] 2.1 Engine-owned runtime, the asset model, and graph compilation to VFX's own IR
- [ ] 2.2 Compiler-derived attribute layout — the layout is an output of compilation, not a fixed
      struct
- [ ] 2.3 GPU-first simulation, with the CPU path a declared fallback that reports itself
- [ ] 2.4 The unified simulation world and its global scheduler
- [ ] 2.5 Data interfaces; GPU scene integration for mesh particles; GPU-to-GPU events and the
      bounded CPU readback path
- [ ] 2.6 Importance classes and frame-budget scalability — cost bounded by configuration, measured
      the way M8.b measured agents and effects: a per-unit cost at four times the population

## 3. Sequences — `sequencing-and-cinematics` → W

- [ ] 3.1 Compiled timelines; cost scales with what is ACTIVE rather than with what exists
- [ ] 3.2 Exact time and clock domains; seek and scrub; skipping applies what it skips
- [ ] 3.3 Bindings; tracks, sections and channels; track authority classification
- [ ] 3.4 A sequence is a COMMAND PRODUCER: the simulation cannot distinguish a sequence-issued
      command from any other, which is `gameplay-framework`'s requirement and the reason a sequence
      does not write a camera transform
- [ ] 3.5 Batched subsystem dispatch; arbitration between sequences; capture and restore
- [ ] 3.6 Preload plans

## 4. Inference — `ml-inference` → S

- [ ] 4.1 Model assets, tensors and sessions, the backend abstraction
- [ ] 4.1b **ONNX Runtime as the reference backend, and it runs a real model.** The specification has
      named it the portable default since it was written; what it has never had is an
      implementation, and an abstraction with no backend is a capability nothing exercises. It is
      the portable default because it is the only one of the four that runs on every target the
      engine ships to, and because its format is the interchange Core ML, DirectML and TensorRT
      import from
- [ ] 4.1c Gate it on `CY_ML_ONNXRUNTIME`, declare it in `deps/manifest.toml` and `THIRD_PARTY.md`
      with a licence identifier and a justification, and **build and pass the suites with the option
      OFF as well as ON**. M8.b's gate found `CY_UI` defaulting off while gating nothing, and M8.b's
      spike found an option that could not be configured at all — this milestone checks both
      directions for every option it adds
- [ ] 4.1d A committed model asset small enough to live in the repository, loaded and run with a
      checked result. `ml-inference` at Seed means the backend runs, not that the interface compiles
- [ ] 4.2 The determinism boundary (section 1 is where it is proven)
- [ ] 4.3 Scheduling and a declared per-frame budget; an unavailable async result yields rather than
      stalls

## 4b. The audio debt — `audio` → C

- [ ] 4b.1 Make `-D CY_AUDIO_STEAM_AUDIO=ON` CONFIGURE. It does not today: upstream's CMake calls
      `find_package(PFFFT)`, `find_package(IPP)` and `find_package(FFTS)` and
      `deps/manifest.toml` provides none of them. Decide between vendoring those, using upstream's
      own dependency bootstrap, or a prebuilt `phonon` — and record the decision against
      `thirdparty-dependencies`
- [ ] 4b.2 Implement `SteamAudioBackend::simulate`, which returns `ErrorCode::NotImplemented` today
- [ ] 4b.3 The fallback keeps answering every query with the option off, and a test proves the two
      paths agree on everything `audio` says content may depend on

## 5. The artefact — `samples/08-vertical-slice`, EXTENDED

- [ ] 5.1 Particles and a cut inside the EXISTING slice, holding the frame budget it already
      declares. Not a second sample
- [ ] 5.1b The slice's sound goes through Steam Audio when the option is on, and through the
      fallback when it is off, with the same gameplay either way
- [ ] 5.2 The cut drives cameras through the camera stack and writes no camera transform
- [ ] 5.3 The slice's determinism act is re-run with both new systems live and reaches the same
      digest it reaches without them
- [ ] 5.4 **Refresh the committed screenshot, and make it the first one that is CAPTURED rather than
      drawn.** Section 1b gives the slice real record callbacks, so its frame becomes the engine's own
      output instead of a diagram of it. Commit it to `docs/design/images/`
- [ ] 5.5 **Capture the milestone's systems where they are visible, not only where they are
      counted.** This project went six milestones with one screenshot and has been paying for it in
      every conversation since: a subsystem verified in isolation photographs badly, and a reader who
      cannot see a capability cannot judge it. At minimum, and each committed under
      `docs/design/images/`:
      a **particle effect mid-simulation** with its budget on screen; the **cut running**, with the
      camera stack's blend visible rather than asserted; and a **before/after pair of the same frame**
      with the shader layer supplying callbacks and without, because that difference is the whole of
      section 1b and a number does not show it
- [ ] 5.6 Every committed image is the engine's own output. **A diagram is allowed and SHALL be
      labelled as one** — M8.b's artefact took its silhouettes from the sample's own table rather
      than the mesh handle the frame resolved, and would have drawn a defect correctly

## 6. Records and gates

- [ ] 6.1 Write `tools/roadmap/milestones/m8c.toml`; declare `milestone-m8c` in `gates.toml` and
      raise `selftest.MINIMUM_CRITERIA`
- [ ] 6.2 An `m9-open` criterion using the double-star glob form
- [ ] 6.3 Update `status.yaml`, `capability-matrix.md`, `ROADMAP.md` and `dependencies.md`, and run
      the plan-consistency checks over them
- [ ] 6.4 Move `ci.yml`'s milestone job to `m8c` in the same commit that flips the gate green
- [ ] 6.5 Open the M9 change

## 7. The gate

- [ ] 7.1 Full audit, per `delivery-roadmap`. **M8.b was the last milestone that specification
      requires the audit in full for**; run it anyway and record what the reduced form would have
      missed, because that is the evidence the reduction is safe
- [ ] 7.2 Clean build of every profile from empty; `test-all` in each
- [ ] 7.3 Every gate by hand; the M8.c ledger run once
- [ ] 7.4 Every criterion actually executes something
- [ ] 7.5 Adversarial pass: remove the firewall and confirm the test goes red; drive a gameplay
      write from a VFX readback and from a non-pinned model and confirm both are refused; exceed
      the effect budget and confirm the cost is bounded rather than the frame
- [ ] 7.6 Records verified against what the code supports, not what the plan claimed

# Tasks: M11.b — Authoring

Ordered. Section 0 is the spike and it runs first because the play-mode seam decides whether the
editor rows below it are built on one hosted world model or two, and discovering that after sections
2 to 5 are written is a migration rather than an edit. Section 1 is next and alone, because
**thirteen of this rung's requirements are not code in `editor/` at all** — they are third-party
integrations that `thirdparty-dependencies` requires to go through the OpenSpec change flow one at a
time, and a rung that starts them in section 6 starts them too late.

**Three rows here are at Seed and reach Complete, which is two tiers in one rung and which nothing on
this ladder has done.** `editor-architecture` and `live-editing` have been at Seed since M5;
`ml-inference` since M8.c. Each of the three has its own section, and each section's Working half is
separable from its Complete half on purpose, so a rung that runs short demotes a **tier** rather than
abandoning a row.

## 0. The spike — the play-mode seam

- [x] 0.1 **Is `editor-architecture` and `live-editing` at Seed a mis-record or a thin foundation?
      ANSWERED BEFORE THE RUNG OPENED, AND IT IS A THIN FOUNDATION.** M10 task 6.5 read all nineteen
      contested cells against the tree M10 closes on rather than against the gate that parked them,
      moved fifteen and claimed four, and **kept both of these rows at Seed with a grep as the
      evidence for each** — `SeparateProcess|RemoteDevice|InEditor` returns nothing across `src/`,
      `editor/` and `tools/`, and so does
      `LiveEditPolicy|ReinitializeComponent|RecreateEntity|RestartWorld`. Both greps were re-run on
      the tree this rung opens against and both still return nothing. The record is right; the
      foundation is thin. **This is recorded here so the rung does not spend a spike re-asking a
      question the previous milestone's audit already answered**, and so that M11.a is not asked for
      it either
- [x] 0.2 **MEASURED, AND THE ANSWER IS ONE WORLD MODEL AND THREE TRANSPORTS.** `PlayMode` is a
      table beside `PlaySession` (`src/gameplay/play/include/cy/gameplay/play/mode.h`), not a second
      session type: `PlayConfiguration::mode` is the only thing that changes between the three, and
      `tests/test_editor_play.cpp`'s `drive()` reads the mode nowhere else. The case *"a world plays
      in each of the three modes, and the same command stream drives them"* runs ONE authored world
      through ONE sequence of calls in each mode and compares the simulated result: 30 ticks and the
      sphere's height agree to 1e-6 across all three, and the height moved (4.0 → 2.66), so the
      comparison is over a simulation rather than over three copies of an initial placement. A second
      world model could not produce that agreement by accident. The original question follows:
      **The spike proper: can one hosted runtime carry `InEditor`, `SeparateProcess` and
      `RemoteDevice` without a second world model?** `live-editing` says *"Locality SHALL be an
      optimisation of transport, not a different architecture"* and *"no play mode runs in the editor
      process"*; `HostingMode::Hosted` is already the production default and the M5.5 live-bridge
      spike already measured the process boundary at about 60 µs at the median against an 8.6 ms
      runtime frame. So the architecture question is **not** whether out-of-process is affordable —
      that is measured — it is whether the three modes are three transports over one world model or
      three world models. Measure it; do not reason about it
- [x] 0.3 **All three verified.** (a) and (b) were confirmed at source level by the rung's spike;
      (c) is now confirmed from both ends and is *stronger* than stated — `EncodedStream` is declared
      on the Rust side (`cy_editor_viewport::transport::TransportKind`) AND on the C++ side
      (`ViewportTransportKind`), and implemented on neither, so the blocker is not that the remote
      transport is unnamed. `PlayModeSupport::frame_encoder` is that fact, made a query.
      **The three facts the spike has to start from, each verified rather than assumed.**
      (a) `cy_editor_documents::worlds` already carries the three *world kinds* the specification
      demands — Authoring, Preview, Runtime — with a `compile` step and an independent
      `authoring_leaks` check, so the editor side of the model exists.
      (b) `play.enter` already reaches a runtime over a real socket and `cy::gameplay::PlaySession`
      already restores the authored document byte for byte on stop, verified rather than asserted —
      so *play* is not the gap, the **mode** is.
      (c) `cy_editor_viewport::transport::TransportKind` declares `LocalSurface`, `SharedTexture` and
      `EncodedStream`, and the only one implemented above a local surface is a **same-machine**
      shared texture built on `VK_KHR_external_semaphore_fd`. **`RemoteDevice` cannot use it**, so
      `EncodedStream` is on this rung's critical path and nothing in the tree encodes a frame
- [x] 0.4 **RECONCILED: the middle row of the budget was spent and the refutation was not.** One
      world model, three transports, and a **declared per-mode capability set** —
      `PlayModeCapabilities`, queried by `PlaySession::step_frame`/`step_tick` and mirrored in Rust
      by `PlayMode::can_step_frame`. Exactly one capability differs between the modes and it differs
      by TRANSPORT: `RemoteDevice` cannot step a single frame because an encoded stream's frames are
      not individually addressable; a tick step is a message rather than a picture, so every mode
      keeps it. No second world model was needed and none was written.
      **The failure budget, stated before the work rather than after it.** One world model and
      three transports costs nothing. One world model, three transports and a declared per-mode
      capability set — *"this mode cannot single-step"* — costs a capability query on every feature
      that steps, and is still one architecture. **Two world models is the refutation**, and if the
      spike spends it the rung is re-planned the way M8.b was re-planned on its own spike's answer,
      not delivered on a premise the spike refuted
- [x] 0.5 **THE REFUSAL EXISTS, NAMES THE MODE, STARTS NOTHING, AND IS PROVEN LOAD-BEARING.**
      `PlaySession::enter` consults `availability_of()` before it snapshots the document; the case
      *"a play mode that is not available refuses by name and starts nothing"* checks the message
      names `remote-device` and `encoder`, that the state is still `Editing`, that `world()` is null
      and that the document's bytes are unchanged. The editor refuses at its own end too
      (`Editor::set_play` resolves the mode before the badge moves) and
      `the_mode_is_carried_on_every_play_message_and_an_unknown_one_is_refused` checks that NOTHING
      crossed the socket. **Both refusals were mutated and both went red**: replacing the C++ refusal
      with a fallback to `InEditor`, and replacing `PlayMode::from_name` with `unwrap_or_default()`.
      Restores md5-verified.
      **And the one outcome that is worse than a refutation: a silent fallback.** A mode that is
      selected and not implemented must refuse by name. A `RemoteDevice` request that quietly runs
      `InEditor` is a green test over a feature that does not exist, and it is the exact shape M9's
      gate found when a criterion passed 44 of 44 with its enforcement point deleted
- [x] 0.6 Recorded in `design.md` §1.5, beside the budget it reconciles. Not committed — the
      orchestrator commits between phases.
      Original: Commit the spike and record its answer in `design.md` §1, the way M8.b's IR spike, M9's
      determinism spike and M10's invalidation spike were committed and consumed without being
      re-derived

## 1. The thirteen dependency adoptions, each its own change

**This section is the rung's long pole and it is not editor work.** `thirdparty-dependencies` names
an intended set, `deps/manifest.toml` carries seventeen of it, and **thirteen of the entries this
rung's rows require by name are absent** — HarfBuzz, ICU, FreeType, msdfgen, libpng, libjpeg-turbo,
libwebp, tinyexr, a BC7 encoder, astc-encoder, cgltf, meshoptimizer and OpenUSD, with ACL beside them
marked *to evaluate*. That specification's "New dependency is a reviewed
decision" scenario is normative: adding one *"SHALL go through the OpenSpec change flow recording the
evaluation against these criteria"*. Thirteen changes, not one section.

The row that owes the `thirdparty-dependencies` **cell** is M11.e's. The *adoptions* are this rung's,
because this rung's rows are what cannot be Complete without them.

- [ ] 1.1 **Text: HarfBuzz, ICU, FreeType, msdfgen.** `text-and-fonts` names the first three in
      normative text — *"shaped through HarfBuzz"*, *"via ICU"* — and `src/text/README.md` states
      plainly that the module is the honest half: the algorithms over declared coverage, with
      `ShapingCapabilities`, `coverage_of()`, `BidiResult::approximated`, `BreakReport::used_dictionary`
      and `plural_rules_known()` reporting every gap in code. The engine can lay out Hebrew, Arabic
      and Thai and **cannot load a font to draw them with**: the only font it loads is
      `ImageGridFont`, a bitmap grid. `deps/manifest.toml` defers all three by name and states the
      cost, and ICU in particular is *"an autotools project whose CMake support is somebody else's
      build system wearing a hat"*
- [ ] 1.2 **Images: libpng, libjpeg-turbo, libwebp, tinyexr.** `decode_image` reads Targa and
      nothing else, and names what the other two would need — *"PNG needs a DEFLATE decoder and JPEG
      a DCT one"* (`tools/import/src/texture.cpp:433`)
- [ ] 1.3 **Texture compression: an ISPC-class BC7 encoder and astc-encoder.** There is no encoder in
      the tree. `cy::import::select_format` already names the format a cook *would* produce and
      records `CookedTexture::encoded = false` beside the uncompressed payload — see task 6.4, which
      is what stops a cache built without an encoder from being served to a build with one
- [ ] 1.4 **glTF: cgltf.** `tools/import/src/json.cpp` is a strict JSON reader *"written to be
      deleted when this lands"* and its header says so
- [ ] 1.5 **Meshes: meshoptimizer.** M5 delivered engine-owned simplification, vertex cache and fetch
      optimisation behind free functions over `cy::import::MeshData`, so this is a change to one
      `.cpp` and to nothing that calls it — `tools/import/include/cy/import/mesh.h` states what the
      engine-owned simplifier costs against a tuned one
- [ ] 1.6 **USD: OpenUSD, optional and tool-time only**, which the specification requires in so many
      words — *"SHALL NOT be linked into a shipped runtime"*
- [ ] 1.7 **Animation compression: ACL, or a recorded decision not to.** The specification marks it
      *to evaluate* and states the rule for that marking: *"the requirement is the capability, not the
      library"*. A recorded evaluation concluding the engine-owned codec is sufficient closes this
      item; silence does not
- [ ] 1.8 **Every one of the thirteen is judged against the dependency policy's own criteria,
      including the licence rule** — GPL is refused for runtime code by name — and each lands with
      its attribution row in `THIRD_PARTY.md`, its `cy__configure_<name>` block, and a pin. **A
      dependency adopted without the change record is the defect this section exists to prevent**,
      and the check is cheap: the manifest's names against the specification's table
- [ ] 1.9 **State the arithmetic at the head of the rung rather than at its gate.** Thirteen
      adoptions is thirteen libraries plus one marked *to evaluate*, against the **seventeen**
      entries `deps/manifest.toml` has accumulated across the whole ladder to date — so this one rung
      proposes to grow the dependency set by more than three quarters.
      `design.md` §3 carries the contingency; if it does not fit, the rows that move are named there
      and they move **with their reason recorded**

## 2. The plugin surface — `project-and-plugins` → C

**First among the editor sections, and not by preference.** `editor-architecture`'s "Specialised
editors" requirement is normative about it: *"Each SHALL be a plugin using the same panel and undo
infrastructure as user plugins, so the extension API is exercised by the engine's own tooling"*, with
a scenario — *"WHEN a built-in editor is implemented THEN it SHALL use only the public plugin API"*.
Section 3 cannot be built before this one without violating the requirement it is built to satisfy.

- [x] 2.1 Plugins, extension points and the plugin lifecycle. `find src tools -iname '*plugin*'`
      returns **one layercheck fixture** and has since M5; four of the row's eleven requirements have
      no implementation

      **DONE for 2.1 and 2.3, in `src/core/plugins/` (`cy::core-plugins`, layer 0).** `plugin.h`
      carries identity independent of name and path, semantic versions, the three constraint kinds
      the requirement names, the content-kind set, the trust tiers and the manifest reader;
      `resolve.h` carries resolution into a concrete set, a deterministic load order and the
      lockfile with the three fields the requirement asks for; `host.h` carries the eight phases,
      failure containment, the twenty-one extension points the requirement names at minimum with
      independent interface versioning, and type ownership with an unload refused by name and count.
      Suites `unit.project` (17 cases) and `integration.plugins` (8 cases) — the two the m11b
      ledger's `plugins` criterion names, which was red for want of them. **Nine mutations run and
      restored; eight went red on the first pass and the ninth exposed a real hole** — dropping the
      resolver's tie-break changed no order my fixtures could see, because their graph's topology
      already fixed it. Two cases were added over two INDEPENDENT plugins, and each half of the
      tie-break is now pinned separately. Cognitive complexity of the new functions tops out at 23
      (a manifest keyword dispatch), inside the parser/systems band. `README.md` records what is
      still owed: the loader, the first first-party registration at a point, and task 2.5's check.

- [ ] 2.2 The plugin binary boundary as the engine's C ABI, and type ownership with unload safety —
      against the engine's own reload model, which `cy-editor-sdk`'s `RuntimeLibrary` already
      states: serialize, migrate by name, recreate, **never `dlclose`**, because a retired image's
      string literals are still referenced by every component and behaviour registration

      **HALF DONE, AND DELIBERATELY NOT TICKED.** The *type ownership and unload safety*
      requirement is finished — `cy::plugins::TypeOwnership` records the owner of every type, an
      unload with outstanding instances is refused naming the types and their counts, and a
      successful unregister withdraws every extension binding and every ownership row
      (`integration.plugins`, three cases, two mutations verified red). The *binary boundary* half —
      the C ABI descriptor entry point taking the host's API version — is NOT done: `PluginRuntime`
      is filled in by hand today, and joining it to `src/abi/`'s loader is `native-abi` work this
      rung did not reach. A box ticked for half a task is what M10 needed three extra workflows to
      undo.
- [x] 2.3 Plugin resolution and the lockfile; trust tiers for extensions
- [ ] 2.4 Layered typed configuration, and the project graph as authoritative
- [ ] 2.5 **The check that makes 2.1 load-bearing**: a built-in specialised editor that reaches past
      the public plugin API fails the build. Without it, "the engine dogfoods its plugin API" is a
      convention, and a convention is what this rung is here to stop relying on

## 3. The two Seed rows — `editor-architecture` → C, `live-editing` → C

Section 0's answer is the input to 3.1 and 3.2. Everything else here is independent of it.

- [x] 3.1 **DONE.** `cy::gameplay::PlayMode` + `PlayModeCapabilities` + `PlayModeAvailability` +
      `PlayModeSupport` (`src/gameplay/play/include/cy/gameplay/play/mode.h`, `src/mode.cpp`);
      `PlayConfiguration::mode`/`support` and the refusal in `PlaySession::enter`;
      `PlaySession::step_tick()` and `step_frame()` with `ticks_per_frame()` computed from the two
      rational rates (60/1 ticks against 30/1 frames is two ticks, checked). Through the same live
      bridge: `Message::Play`/`Playing` gained a `mode` word beside the state word,
      `RuntimeSession::play(state, mode)` sends it, `play.enter|pause|leave` take an optional `mode`
      argument defaulting to `in-editor`, and `cy_editor_viewport::play::PlayMode` is the editor's
      mirror of the same three spellings. Suite: `cy_test_integration_editor_play`, 5 cases, 88
      assertions. Criteria `m11b:play-modes-exist` and `m11b:play-mode-round-trip` both green.
      **AND A REGRESSION THIS FOUND AND FIXED**: the first version of the protocol change added the
      `mode` word to the Rust side only, so the runtime's `Playing` answer was one field short of
      what the editor decodes — the editor's session dropped the connection and `smoke.authoring`
      failed two acts later with *"the engine's world is not empty after the undos"*.
      `src/runtime/editor_bridge/` now reads the mode on a `Play` (empty when an older editor sends
      none) and writes it on a `Playing`, `samples/05b-editor-window/runtime` refuses an unavailable
      mode by name and carries the chosen one into `PlayConfiguration`, and two cases in
      `unit.editor_bridge` check the **fields** of both messages rather than only their tags — which
      is why the suite's existing "two languages, one wire" case did not catch it. Both were mutated
      and both went red.
      **A mode that is not available refuses by name** (task 0.5)
      **CORRECTED AT M11's GATE, AND THE CORRECTION IS THE POINT.** "`m11b:play-modes-exist` green"
      was worth nothing: that criterion was `grep -rniIl SeparateProcess src/ editor/crates/ tools/`
      — M10's absence grep inverted — and the gate refused it in three words, *word-greps a dummy
      job satisfies*. Reproduced since: with `src/gameplay/play/`, `src/gameplay/live/` and
      `cy-editor-viewport/src/play.rs` deleted outright and one comment file holding the seven words
      added, the old criterion PASSES. It is now `python3 tools/editor/play_contract.py play-modes`,
      which derives the mode table from the declaration that fixes it on each side — the engine's
      `constexpr kNames`, its `capabilities_of` and `availability_of` switch arms, the editor's
      `const fn name()` and `matches!` predicates, and the markdown tables in `live-editing` and
      `editor-architecture` — and requires them to agree. Over the same deleted tree it goes RED.
      Proven by `just roadmap-falsify` (`rename-token 'separate-process' in
      src/gameplay/play/src/mode.cpp`), watched red by hand under five separate breaks, and run
      outside the ledger as `integration.editor_contract_play_modes` so it cannot stop firing
      **CORRECTED A SECOND TIME AT M11.b's GATE, AND THIS IS THE CORRECTION THAT COST CODE.** The
      gate refuted "the engine gained what the specification asks for" on `SeparateProcess`:
      *"NO process launch of any kind … mode.cpp:66-68 says it itself"*. It was right.
      `PlayModeSupport::runtime_launcher` was a literal `true` beside a comment reading "this build
      carries no launcher yet", `kSeparateProcessDue` named M11.d as the rung that would write one,
      and `cy_test_integration_editor_play` "drove three modes" by building THREE `PlaySession`s IN
      ONE PROCESS and comparing the three results — which agree whatever the mode argument said.
      Deleting separate-process play outright would have left every one of those green.
      **What is in the tree now:**
      `src/gameplay/play/include/cy/gameplay/play/launcher.h` + `src/launcher.cpp` — `RuntimeProcess`
      launches, supervises, drives and reaps a real second process over a versioned one-line-per-
      request protocol; `driver.h` + `src/driver.cpp` — `PlayDriver` is the one command vocabulary
      and `LocalPlayDriver`/`ProcessPlayDriver` are its two localities, which is *"locality SHALL be
      an optimisation of transport, not a different architecture"* as code rather than as a
      sentence; `host/main.cpp` → `cy_play_runtime_host`, the binary that IS separate-process play,
      built unconditionally rather than behind `CY_BUILD_TESTS` because it is a product;
      `Platform::spawn_process` (there since M0, never once called from `src/gameplay/`) gained
      `ProcessOptions::piped_standard_streams`, `write_process_input`, `close_process_input`,
      `read_process_output` and `process_id`, because a spawned child with inherited streams is
      started but not addressable and a play mode has to be DRIVEN.
      **What makes it unfakeable.** `launch` requires the identifier the CHILD reports and the one
      the OPERATING SYSTEM gives this launch to agree, and the case requires that identifier not to
      be this process's. `play_mode_support(platform)` MEASURES `runtime_launcher` by resolving the
      host binary on disk, so the no-argument overload now answers `false` and a tree without the
      binary reports the mode unavailable. `PlaySession::enter` REFUSES `SeparateProcess` unless
      `support.hosted_runtime_process` — set only by `cy_play_runtime_host` — so the exact shape
      this rung shipped is now an error naming `ProcessPlayDriver`.
      **And the comparison is bit for bit**: one world, one command stream, two processes, the
      sphere's height compared as its raw `u32` bits rather than "near", because two processes
      running the same compiled simulation over the same bytes produce the same float and anything
      weaker would let a real divergence through. 8 cases, 150 assertions, all green.
      `m11b:play-mode-round-trip` was rewritten around it and `m11b:separate-process-is-a-second-
      process` added beside it; both watched red under `rename-token 'cy_play_runtime_host' in
      launcher.h` (rebuilt, red, restored, rebuilt, green) and under the declared case rename, with
      md5 restores verified
- [x] 3.2 **DONE, AND IT IS A COMPILER RATHER THAN A FIELD.** `src/gameplay/live/` — `cy::gameplay-live`:
      `LiveEditPolicy` with all six of the specification's outcomes, `derived_policy_for` deriving
      only the three the classification can justify (`reflect::PersistenceKind` gets its first
      consumer), `LiveEditPolicyTable` for the per-field declarations, and `LiveEditCompiler`
      translating an `AuthoringChange` into a runtime delta and applying it. `announce()` answers
      before the change is made, from the same function `apply()` uses, so the announcement cannot
      disagree with the outcome. Runtime state is carried across a rebuild and what could not be
      carried is COUNTED. "Without a restart" is two tick numbers the session produced, not a
      boolean. `declare_engine_policies` declares eleven fields, and every one of them is stronger
      than the derived `Immediate` because the physics bridge consumes the value at body creation —
      a finding written into `src/gameplay/live/README.md`. **And the editor now actually sends a
      live edit**: `RuntimeMirror` no longer hard-codes `ApplyWhen::OnArrival`, so `AtTickBoundary`
      is constructed outside a unit test for the first time in this tree's history. Suite:
      `cy_test_integration_editor_live_edit`, 7 cases, 120 assertions, plus
      `an_edit_made_while_the_world_is_playing_is_scheduled_for_a_tick_boundary` reading the
      scheduling off a real socket. Criteria `m11b:live-edit-policy-exists` and
      `m11b:live-edit-applies-without-a-restart` both green
      **CORRECTED AT M11's GATE, for the same reason 3.1 is.** `m11b:live-edit-policy-exists` was
      four more inverted greps and a comment naming the four symbols closed it. It is now `python3
      tools/editor/play_contract.py live-edit-policy`, which compares the engine's table against the
      requirement's own table and against `reflect::PersistenceKind` — the classification the
      defaults are derived FROM — and whose load-bearing leg is the claim this task rests on:
      **`RecreateEntity` and `RestartWorld` are never DERIVED.** A build in which the classification
      could produce either would make the per-field declaration unnecessary and
      `live-edit-applies-without-a-restart` vacuous; inserting `LiveEditPolicy::RestartWorld` into
      one arm of `derived_policy_for` turns the criterion red, watched by hand. Proven by
      `just roadmap-falsify` (`rename-token 'recreate-entity' in src/gameplay/live/src/policy.cpp`)
      and run outside the ledger as `integration.editor_contract_live_edit_policy`
- [ ] 3.3 **THE HOST AND THE REGION ARE DONE; THE SIXTEEN EDITORS ARE NOT, AND THE BOX STAYS
      UNTICKED FOR THAT REASON.** `SpecialisedEditors` fills `Region::CentreLower` — the first thing
      in this workspace to do so since M5.5 reserved it — registers all sixteen editors the
      requirement enumerates, and opens three of them: gameplay and utility graphs (`script.*` and
      `ai.*`), abilities and effects (`ability.*`), and animation graphs and clips (`pose.*` plus
      the keyed track kinds). **The other thirteen refuse by name**, naming themselves and the
      capability row that owes the vocabulary, because an empty canvas opened for `terrain` would be
      a specialised editor that exists only in a screenshot — the silent fallback M11.b's own gate
      called the outcome worse than a refutation. Untouched: the environment tool set the
      requirement also asks for — sculpting over a non-destructive modifier stack, river spline
      authoring, foliage rule authoring — which is task 3.5's join and M11.c's rows.
      Original: **The specialised editors**, into the `CentreLower` region `chrome.rs` has reserved since
      M5.5 for *"the active specialised editor: script graph, animation, materials, sequencing"* and
      which nothing fills. **Read the requirement's whole list before scoping this task**: it names
      materials, animation graphs and clips, the VFX graph, **terrain, foliage, water and environment
      fields**, tilemaps, UI layout, audio buses, navigation baking, lighting and lightmap baking,
      abilities and effects, gameplay and utility graphs, sequences and cinematics, and localisation
      tables — plus an environment tool set with sculpting over a non-destructive modifier stack,
      river spline authoring and foliage rule authoring. That is a larger surface than the two Seed
      rows' twenty-four requirements suggest and `design.md` §4 says what this rung does about it
- [x] 3.4 **DONE, AND THE PROHIBITIONS ARE WHAT IS CHECKED.**
      `editor/crates/cy-editor-interface/src/specialised/` — `graph.rs` is THE node-graph canvas
      (`GraphCanvas`: the engine's `NodeKey` identity, links ordered `(to, to_pin, from, from_pin)`
      the way `cy::graph` fixes, the layout as a side table outside the semantic model, node- and
      pin-precise diagnostics, a type-checked connect that refuses a cycle by name, and a diff that
      reports meaning and ignores where the boxes sit); `timeline.rs` is THE curve surface
      (`TimelineSurface`: `TrackId`/`SectionId`/`KeyId` that survive retiming and trimming,
      `KeyingMode` with `value_edited` as the one function every viewport edit goes through, and
      scrubbing, whole-frame stepping, loop ranges, markers, snapping, filtering, locking, binding
      validation, retiming and trimming); `mod.rs` is the host, which holds **one** of each and
      hands them out through `Session`, so a domain has no way to bring its own.
      **What makes it unfakeable.** "Every graph editor shares one canvas" is `CanvasId` equality
      across the opens and "every keyed-time editor shares one surface" is `SurfaceId` equality —
      measured, not declared. And the requirement's own words decide which editors those are:
      `tools/editor/play_contract.py specialised-editors` parses the requirement's enumeration and
      requires every editor **it** describes as a graph to declare `Surface::Graph` and every one it
      describes with a timeline, a curve or a sequence to declare `Surface::Timeline`. Seventeen
      legs, no build needed.
      **What is NOT here, stated rather than discovered later.** The canvas does not write
      `cy::graph`'s canonical text form — `src/graph/include/cy/graph/text.h` owns it, and a second
      writer of a canonical format is a second format the day the two disagree about a float. The
      built-in catalogues declare node type NAMES without the engine's pin tables, so `connect`
      refuses on a built-in palette naming what would fix it. Both are M11.e's, beside the semantic
      diff `editor-documents-and-transactions` still owes
      Original: **One node-graph canvas and one timeline surface, not six of each.** The requirement
      forbids a sixth bespoke graph editor by name and requires all keyed-time editors to share one
      curve surface, one keying model and one identity model for tracks, sections and keys
- [ ] 3.5 **Rule-driven tools explain themselves** — *"WHEN a designer asks why no trees appear in a
      region THEN the tool SHALL name the rule input responsible"*. This is a join with M10's PCG and
      foliage provenance, and it is a real requirement rather than a nicety: a procedural result that
      cannot be explained cannot be corrected
- [ ] 3.6 **THE SETTINGS HALF IS DONE AND THE PACKAGE HALF IS NOT, SO THE BOX STAYS UNTICKED.**
      `editor/crates/cy-editor-services/src/settings.rs` — `Catalogue` (declared settings by
      category, with the categories the requirement names: layers, tags, input actions, quality,
      rendering — searched over summaries as well as keys, because a person looking for the frame
      budget types "frame rate" and not `target_frame_rate`), `ProjectSettings` with per-platform
      overrides, `UserPreferences`, and `Template` with two project templates and a `create` that
      refuses to write over an existing manifest.
      **The requirement's only scenario is the check.** *"WHEN a project setting changes THEN the
      diff SHALL show only that setting, and user preferences SHALL not appear in the project
      file."* That is false of every settings file serialised out of a hash map or re-indented by
      its writer, so the text form is canonical — one setting a line, in key order, overrides after
      the defaults, reals written with `{:?}` so a file read and written again is the same bytes —
      and `SettingsService::project_diff` makes the scenario a function the case calls. The second
      half is enforced by shape: a declaration says which file a setting belongs in, `set_project`
      on a preference is refused BY NAME, and the two files have different writers, so there is no
      path that would put a preference in the project file.
      Criterion `m11b:project-settings-in-version-control`, seven named cases with the count
      asserted, **proven against a built tree** — the case rename, and by hand two mechanism
      mutations: the writer emitting settings in reverse key order (red on the canonical-order
      assertion) and the project writer also writing the preferences (red on the separation case).
      Restores md5-verified.
      **NOT DONE, and why the requirement is not recorded as answered**: package and dependency
      management for engine modules and Swift packages. `ProjectService` carries the module list and
      `SwiftModuleBuilder` builds one; neither is joined to this, and a requirement with a clause
      nobody implemented recorded as answered is the green tick that outlives its subject
      Original: **Project creation from templates and project settings**, stored in text form suitable for
      version control with user preferences stored separately — neither has an implementation
- [ ] 3.7 **Build and deployment as a client of the build service**, which the requirement states in
      the negative: the editor *"SHALL NOT invoke shell scripts and parse their output"*. Select a
      target, request compile, cook, package and deploy, show structured progress, allow
      cancellation. **The service is `build-and-packaging`'s, which is M11.d's row** — see
      `design.md` §6
- [ ] 3.8 **The debugging and profiling surface, which is the largest single requirement on this
      rung and is routinely under-read.** It is not "a profiler panel": a `profiler` panel kind, its
      title and a docking slot exist, and what the shell reports into it is the *editor's* own frame
      cost. The requirement asks for a **remote and local debugger with breakpoints, stepping and
      variable inspection for Swift and native**; a live entity inspector for the running game; a
      frame profiler over CPU stages, jobs and GPU passes with a timeline; memory profiling by
      allocator tag and asset category; a **render debugger** showing the render graph as it was
      built for a frame with per-pass GPU time, queue, resources, transient memory and barrier wait,
      plus the budget arbiter's allocations and adjustments; and a **shader and material inspector**
      showing every stage of lowering — graph, material IR before and after optimisation, generated
      Slang, backend binary — with cost attributed to graph nodes. Profiler markers map into
      RenderDoc, PIX, Xcode, Nsight and Radeon GPU Profiler, and a capture is launchable from the
      editor. **The last two are joins with rows other rungs own** — see `design.md` §6 — and the
      editor is a *client* of `diagnostics-profiling-and-crash`, which is Complete since M10, not its
      owner
- [ ] 3.9 **Capture, crash and reproduction artefacts open without the game running**, including
      artefacts produced on another platform — which is a check this host can actually run, because
      M10 closed `m9:crash-artefact-paths` and a produced artefact exists to open
- [ ] 3.10 Live asset reload, shader and material live reload, module hot reload as a declared
      capability, runtime inspection, runtime tweaking distinct from authoring with the keep-changes
      flow, the versioned live bridge protocol, and live editing diagnostics
- [x] 3.11 **Make the window criterion execute the window it names.** Add a bounded `--smoke`
      lifecycle to `cyberdyne-editor` that opens the real desktop shell, draws several frames,
      verifies the shipped dock and specialised-editor region were exercised, and exits. Keep
      `just run-editor --smoke` as the criterion command so the check runs the same binary and
      startup path a person uses rather than a headless model substitute
      **Done:** the native shell draws exactly three frames and reports that count before closing;
      the parser and lifecycle have regression cases and the exact roadmap command passes on macOS
- [x] 3.12 **Make editor import parity an executable cross-language claim.** Compare the importer
      names, extensions and option schemas reported by the built `cy_import_cli` with what the
      editor discovers through `AssetImportService`; add the missing integration surface and make
      the criterion fail when either side drops or invents a format
      **Done:** the editor exposes its dynamically discovered catalogue through `--list-importers`;
      the independent comparator observes 5 importers, 9 extensions and 84 typed settings from both
      built programs
- [ ] 3.13 **Read all nine editor rows requirement by requirement.** Add only evidence that resolves
      to a named test case, a proven criterion, a gate, or a recorded exemption. A partial case does
      not answer a whole requirement. The target is `just quality-requirements` reporting 133 of
      133 with zero stale or missing evidence
- [x] 3.14 **Repair the view-mode criterion without taking M11.c rendering work.** The editor SHALL
      prove every engine-provided debug view is selectable, described and carried to the runtime.
      Modes whose renderer producer is absent remain explicit gaps owned by their rendering
      capability; this task does not fabricate pixels or treat `PLANNED_VIEWS` as drawn output
      **Done:** the criterion executes exact Rust cases proving all 19 engine modes mirror the C++
      table, carry useful descriptions and unique command identifiers, and survive the runtime-facing
      view-state encoding. The eight absent renderer producers remain named by owning capability

## 4. The document model, and a node with a name — `editor-documents-and-transactions` → C

- [x] 4.1 **A node has a name.** `cy_editor_documents` carries none: identity is a `NodeId` and
      nothing else, and `hierarchy.rs:28` says so at the field — *"a node has no name in the document
      model … so the label is the node's **kind**"*. The consequence is visible in the tree and is
      worse than a missing field: `samples/05b-editor-window/project/worlds/city.cyworld` reads
      `node 0 - "Pillar"`, `node 1 - "Crate"`, `node 2 - "Marker"` — and **the third field of a
      `node` line is the layer**, so the only authored world in the repository that looks like it
      names its nodes is putting three objects in three one-node layers because there is nowhere else
      to put a name. See `specs/editor-documents-and-transactions/spec.md`

      **DONE.** The name is a fifth word on a `node` line and a `name` field on `NodeState` /
      `WorldNode`, with `Operation::SetName` (tag 12) carrying a rename through the one operation
      stream the journal, live editing and the engine all read. The engine decodes it
      (`apply_set_name`, and `read_node_state` reads it between the layer and the prefab flag), the
      outliner labels a row with it and falls back to the kind only where there is none, and
      `layers_used_as_names` reports a document that put a name in the layer field rather than
      accepting it silently. `samples/05b-editor-window/project/worlds/city.cyworld` now reads
      `node 0 - "set" "Pillar"`, so `Pillar` and `Crate` share a layer and differ by name — which is
      the case the old file could not express at all. The name word is OMITTED where there is no
      name, so the format's byte-identical round trip survives the field being added.
      Suites: `unit.editor_documents` (new, `tests/editor/`), plus Rust cases in
      `cy_editor_documents::{content,document,transaction}`, `cy_editor_services::worldfile` and
      `cy_editor_viewmodels::hierarchy`. Seven mutations verified red and restored; the one that did
      NOT go red on the first pass — a symmetric drop of the name from `NodeState`'s codec — is why
      `a_node_states_name_survives_the_codec_in_the_position_the_engine_reads_it` pins the POSITION
      as well as the value.

- [x] 4.2 **Source control integration, which is unstarted rather than partial.**
      `grep -niE 'source.control' editor/crates/*/src/` returns seven hits and every one is a comment
      or a remedy string. The requirement asks for a provider interface — status, history, diff,
      check out, revert, submit, lock — with **Git, Perforce and a null provider** behind it. The
      null provider is not a placeholder: it is what makes the other two optional

      **DONE.** `cy_editor_services::source_control`: a `SourceControlProvider` trait — name,
      capabilities, status, history, content at a revision, check out, revert, submit, lock, unlock
      — with `NullSourceControl`, `GitSourceControl` and `PerforceSourceControl` behind it and a
      `SourceControlService` that swaps one for another without a caller naming either. `Capability`
      is a declared set rather than discovered behaviour, and the requirement's second scenario is a
      test: Git declares no exclusive locking and `lock()` refuses by name, where a silent `Ok(())`
      would be a lock a person believed they held. Git also declares no check out, for the same
      reason. Both real providers run the vendor's own client through a `CommandRunner` seam, so the
      argument list a provider builds is checked as well as the output it parses; one case runs
      against a REAL `git` in a temporary repository and reports nothing where `git` is absent, the
      way the render suites do without a device. `content_at` returns bytes rather than hunks
      deliberately — the semantic diff is what a caller shows for an authored document, and a hunk
      parser here would be a third diff representation.

- [ ] 4.3 The rest of the row's twelve: semantic diff and three-way merge over authored documents,
      which `src/sequencing/README.md` is also waiting on — *"there is no text form, no diff and no
      merge. That is an editor feature with an editor's test surface"*

## 5. The rest of the editor — `editor-rust-application`, `editor-viewport-and-gizmos`, `editor-ui-ux`, `editor-visual-language`, `editor-agent-interface` → C

- [ ] 5.1 **The eight missing view modes.** `ViewMode` carries nineteen discriminants mirroring
      `cy::render::DebugViewMode` exactly, and `PLANNED_VIEWS` is an eight-entry table naming what the
      requirement asks for and the engine cannot draw: lightmap density, GI probe placement, virtual
      texture feedback, virtual texture residency, virtual shadow pages, physics colliders,
      navigation data, audio emitters — plus streaming region state. **Five of the eight are drawn by
      subsystems M11.c owns** and `design.md` §6 records that as a dependency rather than absorbing
      it. The editor half — the mirror, the command palette entry, the per-viewport request and the
      composition with visibility filters and isolation — is this rung's, and
      `tests::the_mode_list_matches_the_engines` is the check that keeps the two lists honest
- [ ] 5.2 **`EncodedStream`**, per task 0.3(c): the transport kind that makes a remote device
      possible, which is declared and unimplemented
- [ ] 5.3 The enumeration `cy-editor-sdk`'s `HostingMode` reserves for features that require
      in-process execution is **empty today, and it must still be empty when this rung closes** —
      *"such reasons SHALL be enumerated rather than accumulated"*. A feature added here that needs
      `Embedded` adds its reason to that list or does not ship
- [ ] 5.4 The remaining requirements of the five rows, read first-hand rather than inherited: the
      application shell, the inspector's generation from reflection, the visual language's tokens and
      the agent interface's resources and tools
- [ ] 5.5 **A runtime failure in any mode leaves the editor running**, checked in all three play
      modes rather than in the one that exists today

## 6. The authoring formats — `asset-import-pipeline` → C, `input-and-actions` → C

Section 1's adoptions are the input to 6.1 through 6.4.

- [x] 6.1 **Skins and animations through glTF — DONE, and the model change beneath it reaches the
      FBX path too.** `MeshData::skin` is four joint indices and four weights per vertex;
      `MeshAttributes::Skin` and `kCookedMeshVersion = 2` carry it into the cooked record (the
      version moved with the bit, because a version-1 reader computes its payload length from the
      attribute set and would take a skinned mesh for a truncated one). All seven places that
      permute or duplicate a vertex carry it — the weld's gather AND its key (two vertices in one
      place with different bindings are a RIG SEAM and stay split), the normal split, the fetch
      reorder, the simplifier's emit and the unwrap's remap. glTF reads `skins` into the same
      `ImportedSkeleton` record M8.d defined, `JOINTS_0`/`WEIGHTS_0` through a slot-to-joint map
      (glTF fixes no ordering on a skin's `joints` array and `Skeleton::add_joint` refuses a parent
      that is not smaller than its child, so the two numberings differ and confusing them is a
      character whose left arm moves when its right leg does), and `animations` into
      `cy::animation::Clip` through the same codec. `write_cooked_clip` moved out of `fbx_clip.cpp`
      into `cy/import/clip_record.h` so the two formats cannot grow two clip records. **FBX'S SKIN
      CLUSTERS WERE PARSED AND DROPPED SINCE UFBX LANDED** — `Walking.fbx`'s 65 of them —
      and `resolve_mesh_skin` now resolves each to a joint by bone NAME and keeps the four heaviest
      influences, renormalised, reporting the reduction, an unresolved cluster and a second skin by
      name. Both importer versions moved (gltf 2→3, fbx 1→2) so everything re-cooks. Evidence:
      `integration.asset_import_gltf` (16 cases, new suite — the two the ledger names plus the slot
      map, the bind-pose disagreement, determinism, and one per mesh step),
      `integration.import_fbx_skeleton` (+4 cases over a new always-present ASCII-FBX skinned
      fixture: two polygons so a corner index differs from a vertex index, five clusters in an order
      the skeleton does not share, and a five-influence vertex). Fifteen mutations run and every one
      went red; `skins-and-animations-import` run verbatim, green, and proved red under one
- [ ] 6.2 PNG, JPEG, WebP and EXR decoding, over section 1.2
- [ ] 6.3 BC7 and ASTC encoding, over section 1.3
- [ ] 6.4 **The derivation key changes when an encoder lands**, so a cache built without one is not
      served to a build with one. `deps/manifest.toml` already asserts this as the intended behaviour
      — *"a build that links an encoder produces a different derivation key and re-cooks rather than
      serving uncompressed pixels from the cache"* — and nothing checks it. See
      `specs/asset-import-pipeline/spec.md`.
      **THE GENERAL HALF IS DONE AND LEFT UNTICKED DELIBERATELY, because the encoder half cannot be
      done until 1.3/6.3 land an encoder.** `import_derivation_key` now contributes
      `ImporterInfo::steps` — the set of model-import steps THIS BUILD reaches — which is the spec
      delta's second scenario ("the capability set is part of the key, not a note beside it") over
      the case that already exists: `-D CY_ANIMATION=OFF` removes the clip codec and therefore step
      8, and until now a cache populated by such a build served its artefacts to one that had a
      codec — the cache hit, the build was fast, and the character came back without its animation.
      `unit.import`'s new case *"import key: what the cooker could not do is in it"* asserts both
      directions and goes red when the contribution is removed. The texture half needs the same
      treatment on `CookedTexture::encoded`/`select_format`, and there is nothing to key off until
      an encoder exists
- [ ] 6.5 **Virtual-geometry cooking reachable from inside the editor** rather than from a command
      line only. The cooker is `virtual-geometry`'s and exists; the reachability is this rung's
- [ ] 6.6 USD import, tool-time only, over section 1.6
- [ ] 6.7 **Input assets authored and cooked.** `src/servers/input/README.md` records the gap in its
      own words: *"Input assets are not cooked … M4 builds the tables in code"*. Actions, contexts,
      bindings, processors and triggers authored as assets, cooked into the runtime tables,
      participating in the derived data cache and the identity manifest
- [ ] 6.8 `ActionStableId` allocated from `identity/manifest.toml` rather than being the right shape
      with nothing allocating it; the interface-routing half `ui-system` owes (section 7); and the
      eight-user, thousand-action benchmark the requirement's performance clause asks for and which
      `benchmarks/` does not contain

## 7. Text, the interface and 2D — `text-and-fonts` → C, `ui-system` → C, `rendering-2d` → C

- [ ] 7.1 **Fonts that load**, over section 1.1: TrueType, OpenType, `.ttc`, WOFF and WOFF2, bitmap;
      shaping through HarfBuzz with GSUB and GPOS; variable-font axes, OpenType feature selection and
      colour glyphs; FreeType rasterisation and hinting; SDF through msdfgen
- [ ] 7.2 The Unicode half ICU owns and `src/text/` currently approximates and **reports** it is
      approximating: X10's isolating run sequences, N0 paired brackets, mirrored glyphs, the Korean
      and emoji line-break classes, a shipped Thai/Khmer/Lao dictionary, and number, currency and
      date formatting — which are locale **data** and which a hand-written table would get wrong for
      most of the world
- [ ] 7.3 **The twenty-six named widgets.** `widgets.h` has the reconciler, virtualisation and data
      binding — *"the machinery every widget is made of"* — and not one widget
- [ ] 7.4 Animation and transitions actually evaluated (`transition-duration` is parsed and not
      played: no keyframe player, no spring, no stagger); the immediate-mode API; world-space and
      surface-space UI, UI materials and effects, and the render-target capture the budget's
      blur-behind rung needs — this module *"produces a primitive stream and does not submit it"*
- [ ] 7.5 **The forcing functions, which are the row's own requirement and not a nicety.**
      `ui-system` is explicit that an interface system with no demanding first-party consumer decays,
      and names them: the developer console, the debuggers, the profiler overlays, the settings
      interface and the conformance suite. **`cy::ui` has exactly one consumer in this tree —
      `samples/08-vertical-slice` — and the editor is not one**, because the editor is a Rust
      application with its own chrome. So the consumer that makes this row Complete is section 11's
      game, and the console and settings interface are built in it
- [ ] 7.6 `rendering-2d`'s remaining requirements, read first-hand

## 8. The graph consumers finished — `visual-scripting`, `gameplay-abilities-and-effects`, `ai-system`, `animation-and-skinning`, `camera-system`, `sequencing-and-cinematics` → C

**Four of M8.b's and M8.c's tasks in these rows are unchecked and they are this rung's opening
position**, not a discovery: M8.b 2.4 (a lowering per consumer), 2.5 (execution backends, async
graphs, semantic merge and debugging), 7.1 and 7.2 (compiled timelines, exact time, bindings, tracks
and authority; batched dispatch, arbitration, capture and restore, seek and skip, preload plans).

- [ ] 8.1 **The editor half these rows have been waiting on.** `src/graph/README.md`'s section
      heading is the whole argument — *"A graph the engine authors, because the editor cannot yet
      save one"* — and `locomotion.h` is a state machine written in C++ because there is no graph
      asset. Section 3.4's one canvas is what closes it
- [ ] 8.2 M8.b 2.4 and 2.5: the lowerings and the execution backends, per the verdict M8.b's spike
      wrote — one authoring layer, one shared expression core, a lowering each, **all compiling and
      none interpreting**
- [ ] 8.3 M8.b 7.1 and 7.2, which are `sequencing-and-cinematics`' and `camera-system`'s
- [ ] 8.4 **`sequencing-and-cinematics`' six recorded absences**, which `src/sequencing/README.md`
      lists because *"a specification cell that says Working over a gap nobody wrote down is how a
      capability rots"*: source form, semantic diff and three-way merge (section 4.3); replication and
      late join; persistence (**`PersistenceClass` is declared and carried and no save format writes
      it — see `design.md` §6, this is M11.a's re-scoped `save-and-persistence`**); replay and
      rollback reconciliation; reverse playback of channels; and track-kind extension by plugins
      (section 2)
- [ ] 8.5 **`animation-and-skinning`'s eleven recorded absences**, likewise listed in
      `src/animation/README.md` against the row's thirty requirements: clip streaming, cubic
      interpolation stored with tangents, motion matching and pose search, animation warping, the
      control rig as an asset, full-body IK and FABRIK and CCD and spline IK and spring bones and
      foot placement (**each refused by name today by `solve_unimplemented()`, with
      `test_ik.cpp` asserting the refusal — so each is a check that goes red the day it is
      answered**), physics animation, facial animation, tweens, the rigging workspace (section 3.3)
      and animation diagnostics as a view
- [ ] 8.6 `visual-scripting`, `gameplay-abilities-and-effects` and `ai-system` read requirement by
      requirement at Complete grade. **`design.md` §4 says why these three are the rows most likely
      to surprise this rung**, and it is not because anything has refused them

## 9. Physics, navigation and scripting — `physics` → C, `navigation` → C, `swift-scripting` → C

- [ ] 9.1 **Constraints and the character controller.** `Joint` and `CharacterBody` are registered
      and counted in `BridgeStatistics` and **nothing is created**, because neither backend maps them
      and both report `Capabilities::constraints == false`. A bridge that tried would fail at every
      world with a diagnostic about a component the author was entitled to add
- [ ] 9.2 `navigation`'s remaining requirements, read first-hand. The row carries no named blocker
- [ ] 9.3 **`swift-scripting`'s shipping configuration**: the static, whole-module,
      cross-module-optimised half of the two the specification requires, of which only the dynamic,
      hot-reloadable one is exercised
- [ ] 9.4 **The Swift toolchain pin**, which the requirement asks for *"per engine release and
      verified in CI"*, in `deps/host-tools.toml` — where `just env-doctor`'s Swift check already
      says it will read a `swift` entry when one lands. **This half of the row is a build-system
      requirement wearing a scripting row's name and it cannot be judged before M11.d pins the
      matrix**; `design.md` §4 names it as the likeliest demotion on this rung
- [ ] 9.5 `@Node(path)` resolving to something: ABI 1.0 has no node entry, and the wrapper is the
      seam. The tree callbacks — `onEnterTree`, `onReady`, `onEnable`, `onDisable`, `onUpdate`,
      `onExitTree` — declared in `behaviourCallbacks` and driven rather than declared. A chunk source
      for the generator. `CyStage` and `CySeverity` appended to `CyInterface` so the generator owns
      both enumerations rather than the binding restating them — which is the fix for a defect that
      already shipped once, six enumerators against the engine's three, every `Log.info` arriving as
      an error on a green run

## 10. Inference — `ml-inference`, and the deferral this rung predicts

**This section's honest outcome may be a recorded deferral, and that is stated before the work rather
than at the gate.** See `design.md` §4.

- [ ] 10.1 **Does the game want inference?** Section 11's game is the only thing on this rung that
      would exercise the row. If its AI does not want a model, a Complete cell over an unexercised
      runtime is a claim nothing supports
- [ ] 10.2 If it does: the AI graph node that binds a model — *"`ml-inference` requires CyberML to be
      **usable** from the AI graph, and `ai-system` SHALL NOT depend on it"*, so the node is the
      cook's resolution rather than a link, and `cy::ml::pinning_of` is the half that already exists.
      Plus the cook driver call that refuses a graph binding a non-pinned model
- [ ] 10.3 If it does: the Swift surface `ml-inference`'s gameplay-API requirement asks for, which is
      `native-abi`'s work over a C++ surface deliberately kept small enough to bind
- [ ] 10.4 **If it does not: an explicitly recorded deferral with its re-entry point at M11.e**, with
      what is unmet, why, and the condition that brings it back — the three things
      `implement-m11e-ship`'s proposal says a deferral needs to be honest. **Not a Complete cell**

## 11. The artefact — `samples/11b-game`, a real game

**Fifteen samples and not one of them is a game.** Each proves one slice — `03-first-light` a frame,
`04-character` a character, `06-open-world` streaming and saving, `08-vertical-slice` a playable
slice, `09-multiplayer` four peers, `10-world` an environment — and `samples/11-ship`, which M11.d and
M11.e own, is a packaging proof that would prove its recipe over an empty directory just as well.

**This is the only artefact that can judge twenty-four authoring and gameplay rows at once**, and it
is the forcing function `ui-system` requires by name (task 7.5).

- [ ] 11.1 **A small complete game**: a start, a loop, a way to win or lose, an end, and a way back to
      the start. Not a slice and not a scene
- [ ] 11.2 **Authored through the editor, not assembled in C++.** Its content lives in the project's
      own files — worlds, prefabs, graphs, sequences, input assets, interface documents, localisation
      tables — and the sample's C++ is an entry point and a rules module, not a scene builder
- [ ] 11.3 **And the artefact states which is which, checkably.** The sample reports its own split —
      the authored content files it loads against the lines of its own code that construct scene
      content — and the check fails when the sample constructs in code what it claims to have
      authored. See `specs/testing-and-quality/spec.md`. **This is the task that stops this rung
      telling itself the pleasant version of its own result**
- [ ] 11.4 Every authored kind this rung built is exercised by it: a visual script, an ability with
      effects, an animation graph authored as an asset rather than in `locomotion.h`, a camera rig, a
      sequence, a navmesh, an interface with the widget set, text in more than one script, a 2D
      element, an input asset, and a plugin
- [ ] 11.5 **The three play modes are demonstrated on it**, which is what makes section 3.1 a claim
      rather than an interface: the same game entered `InEditor`, run `SeparateProcess`, and — or, if
      no second machine is available, reported NOT EVALUATED with the mechanism the ledger already
      has — `RemoteDevice`
- [ ] 11.6 Runs from a single recipe; a recorded gap exits non-zero rather than printing a different
      number into a log
- [ ] 11.7 **Capture it.** A screenshot under `docs/design/images/`, and the picture is the game as a
      player sees it. **It is not the beauty shot**: M11.c owns that, this rung's renderer is
      whatever M11.c has not yet done, and a sample game that looks unfinished and says so is more
      honest than one that borrows a claim from the rung above it

## 12. Records and gates

- [ ] 12.1 Write `tools/roadmap/milestones/m11b.toml`; declare `milestone-m11b` in `gates.toml` and
      raise `selftest.MINIMUM_CRITERIA`. **Every criterion needs a `ci_job` — `criteria.py` rejects
      one without it — and must be able to go red.** Where this host cannot judge a claim, the
      criterion reports NOT EVALUATED through `requires`/`where` rather than passing on evidence that
      does not support it
- [ ] 12.2 **Re-point, do not delete, the inherited gaps this rung inherits from M11's proposal.**
      None of the seven `known_gap_closes = "m11"` entries is this rung's to close — they are M11.a's
      — but a gap re-pointed at the wrong rung is a gap nobody owns. Confirm each still names the
      rung that closes it
- [ ] 12.3 The successor criterion. **`m11c-open` in the double-star glob form the ladder has used
      since M8.c would pass the moment it is written**, because `openspec/changes/implement-m11c-image/`
      already carries a proposal — the five rung changes were opened together. A criterion that
      cannot fail is not a criterion, so this rung's handover check must be something else: that
      M11.c's `tasks.md` exists and that M11.b's own findings reached it. **The shape of a
      handover criterion between rungs opened together is a ladder-mechanics question and
      `implement-m11-reach`'s task 0.1 is where it is answered — M11.a owns that.** See `design.md` §6
- [ ] 12.4 Update `status.yaml`, `capability-matrix.md`, `ROADMAP.md` and `dependencies.md`, and run
      the plan-consistency checks over them. **Recording a tier is this rung's closing gate, not this
      phase**
- [ ] 12.5 Move `ci.yml`'s milestone job to `m11b` in the same commit that flips the gate green, and
      not before: the job runs on every push to `main`
- [ ] 12.6 **Thirteen dependency changes archived**, or the ones that did not land named with the row
      they block

## 13. The gate

- [ ] 13.1 Clean build of every profile from empty; `test-all` in each; every gate by hand
- [x] 13.2 **Every criterion executes something and can fail** — break what it checks and prove it
      goes red. This is the check M9's gate added after a criterion passed 44 of 44 with its
      enforcement point deleted, and M10's gate used it to refuse four claims.
      DONE for all 33 of `m11b.toml`'s criteria, recorded one entry each in
      `tools/roadmap/falsifiability.toml`: 8 `proven` (mutated in a sandbox, watched red),
      9 `proven against a built tree` (mutated in the WORKING tree, rebuilt over, watched red,
      restored, watched green again — `just roadmap-falsify prove --build-dir <dir>
      --mutate-the-tree`), and 16 red unmutated — 8 in the tree, 8 against a build — which is this
      rung being open. Nine of those criteria had no proof of any kind before this round: they PASSED
      against a build and the prover had nothing that could turn them red, which is the shape of all
      seven unfalsifiable criteria this mechanism exists to end.
      REPAIR ROUND 2 corrected two of those entries. `gameplay-at-complete-grade` and
      `editor-at-complete-grade` ran `just quality-requirements`, a recipe that existed nowhere:
      `just` stopped at argument parsing having run nothing, and the exit 1 that produced was
      recorded as `red in the tree` — a PROOF. `falsify`'s new `absent-recipe` rule refuses that
      shape before any run, and `tools/roadmap/requirements.py` is the recipe those criteria were
      written for. Both now execute: 0 of 183 requirements across eight gameplay rows and 0 of 133
      across nine editor rows map to a test, a gate or a recorded exemption, each named
      **REPAIR ROUND 3 CORRECTED `requirements.py` ITSELF, BEFORE THE MAP HAD AN ENTRY IN IT.** The
      recipe accepted `test:<kind>.<name>` and asked only whether a suite of that name was declared
      somewhere — so twenty-four requirements could have been answered by naming one suite
      twenty-four times, and every one of them would have resolved. That is the ninth instance of
      the defect, in the tool built to count the other eight. A `test:` entry now carries a **case**,
      searched for in the sources beside the `cy_add_test` that declares the suite, so it cannot be
      borrowed from another suite and a rename turns it red; a `criterion:` entry is accepted only
      where `falsifiability.toml` records a PROOF, so a requirement cannot be answered by a check
      nobody has shown can fail; and a `rust:` kind was added for the editor's own suites, resolved
      against the crate it names. `tools/roadmap/selftest.py::test_requirements_coverage` breaks
      each of those on purpose — five new checks beside the ten it had.
      The map now carries **nine entries across two rows, read requirement by requirement**:
      live-editing 5 of 11, editor-architecture 4 of 13. The fifteen that are not there are named in
      the map's own comments with the task that owes each, because several of them have a case in
      the tree that answers PART of the requirement — and a part recorded as an answer is exactly
      the tick that outlives its subject
      **THE TWO PLAY-MODE CRITERIA ARE NOW IN THE REGISTRY, PROVEN AGAINST A BUILT TREE.** The two
      repair round 2 left owed — `play-mode-round-trip`, rewritten so its digest moved and its entry
      went stale by construction, and `separate-process-is-a-second-process`, which had no entry of
      any kind and which `just roadmap-test` named verbatim — were re-earned by the tooling rather
      than argued: `prove --build-dir build/repair-3-3 --mutate-the-tree --record`, one criterion at
      a time. `separate-process-is-a-second-process` is `proven against a built tree` under
      `rename-token 'cy_play_runtime_host' in launcher.h` — the mode's ROOT, not a case name: with
      the constant pointing at a binary nothing builds, `runtime_launcher_available()` is false and
      the criterion dies at `FAILED: a world plays in the editor's process and in a second one*`.
      `play-mode-round-trip` is `proven against a built tree` under the rename of its own case,
      which takes it red on the SELECTED count — the assertion that exists because
      `-tc=<a name that is not there>` exits zero having run nothing. The same break was also
      watched by hand before the recorder ran: `kRuntimeHostBinary` renamed, rebuilt into
      `build/repair-3-3`, three of the four named cases red at `REQUIRE(runtime_launcher_available)`,
      `REQUIRE(launched.has_value())` and `REQUIRE(configuration.support.runtime_launcher)`,
      restored, rebuilt, green, `md5sum -c` on both files. Note the rebuild is what makes the
      recorded red mean anything: a stale binary still carries the old case name, so the count
      assertion would have stayed GREEN had the mutation not actually been compiled
      **REPAIR ROUND 3 — DONE FOR THE THREE CRITERIA IT TOUCHED, AND RECORDED.** `m11b.toml` now
      carries 36. `specialised-editors` was rewritten from `grep -l CentreLower` — a check whose own
      body called it a placeholder that "three comments satisfy" — into the static contract
      `tools/editor/play_contract.py specialised-editors`, and it is **`proven`** by
      `just roadmap-falsify` itself in the source-only sandbox (`rename-token 'Surface::Timeline'`).
      `specialised-editors-open` and `project-settings-in-version-control` are new, need a compiled
      editor, and are both **`proven against a built tree`** — mutation applied to the working tree,
      rebuilt over, watched red, restored, watched green, `git status` clean after each
      (`prove --build-dir build/repair-3-2 --mutate-the-tree --record`). Beyond the recorded
      mutations, five more breaks were watched red by hand and every restore md5-verified: the
      engine registering `pose.mirror` in place of `pose.transition` (the palette leg), `chrome.rs`
      reserving `CentreLower` for something else (the region leg), the settings writer emitting
      settings in reverse key order (the canonical-order leg), the project writer also writing the
      preferences (the separation leg), and a named case renamed (the count leg on each).
      `tools/editor/selftest.py` gained eleven cases, one per input the new contract reads, and
      stands at 29
- [ ] 13.3 **Adversarial pass on this rung's own invariants**: select a play mode that is not
      available and confirm it refuses by name rather than falling back; reach past the public plugin
      API from a built-in editor and confirm the build fails; give two authored nodes the same name
      and confirm the document model keeps them distinct; request a cooked texture from a cache built
      without an encoder and confirm it re-cooks; delete an authored content file the game claims and
      confirm the game fails rather than falling back to code
- [ ] 13.4 **Twenty-four rows read requirement by requirement against what the code supports**, not
      against what this task list claimed. Three of them arrive at this gate from Seed, which is two
      tiers in one rung, and a gate that reads a specification and feels better about it is the exact
      failure M10's gate refused twice
- [ ] 13.5 **The spike's failure budget reconciled against what it actually spent** (task 0.4), and
      `design.md` §1 updated with the result rather than the forecast
- [ ] 13.6 **Demotions, if any, recorded with their re-entry point** rather than absorbed. A row that
      does not fit moves to a named rung with what is unmet, why, and the condition that brings it
      back. `design.md` §4 predicts which rows those are; the gate's job is to compare the prediction
      with the outcome and record both

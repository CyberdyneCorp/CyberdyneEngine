# Design: M11.b — Authoring

## 1. The spike, and the failure budget it carries

**The spike is one question: can one hosted runtime carry `InEditor`, `SeparateProcess` and
`RemoteDevice` without a second world model?** It is section 0, it runs before sections 2 to 5, and
its budget is stated here before it is spent rather than rewritten after — a budget rewritten after
the fact measures nothing.

### 1.1 What is already known, so the spike does not re-derive it

Three things are measured or built and are the spike's starting position rather than its subject.

- **`Hosted` is already the production default and the boundary is already priced.** `HostingMode`
  names `NoRuntime`, `Embedded` and `Hosted`, and `Hosted` is `#[default]`. The M5.5 live-bridge
  spike measured the process boundary at about **60 µs at the median** against a runtime frame of
  **8.6 ms** — out-of-process buys crash isolation and remote editing for +0.3 to +1.3 ms at p50. So
  *"locality SHALL be an optimisation of transport, not a different architecture"* is not an
  aspiration this rung has to argue for; it is the shape the editor already has.
- **The three *world kinds* already exist editor-side.** `cy_editor_documents::worlds` carries
  Authoring, Preview and Runtime with a `compile` step producing the third from the first, and an
  independently written `authoring_leaks` that reads the *result* — two functions rather than one,
  because *"a compiler that also decided whether its own output was correct would agree with itself
  by construction"*. The check has a test that proves it can fail.
- **Play already reaches a runtime.** `play.enter` drives the whole path over a real socket,
  `Message::Play` carries the state as a **word** so a fourth state is refused by name rather than
  falling through a match to the closest number, and `cy::gameplay::PlaySession` restores the
  authored document byte for byte on stop, **verified rather than asserted**.

**So play is not the gap. The mode is.** `grep -rniI 'SeparateProcess\|RemoteDevice\|InEditor' src/
editor/ tools/` returns nothing, on the tree M10 audited and on the tree this rung opens against.

### 1.2 The one place the spike will actually bite

`cy_editor_viewport::transport::TransportKind` declares three kinds — `LocalSurface`,
`SharedTexture`, `EncodedStream` — and the only one implemented above a local surface is the
**shared texture**, which is built on `VK_KHR_external_semaphore_fd`, a timeline semaphore imported
into a wgpu device, and a 2 ms bounded host wait. Every one of those is same-machine.

**`RemoteDevice` cannot use any of it.** `EncodedStream` is declared and nothing encodes a frame. So
the mode that the specification says *"is not an afterthought"* is the one with no transport, and it
is on this rung's critical path rather than at the end of it.

### 1.3 The budget

| Spend | What it buys | What it costs |
|---|---|---|
| One world model, three transports | all three modes as written | nothing |
| One world model, three transports, and a **declared per-mode capability set** | a mode that cannot single-step says so | a capability query on every feature that steps; still one architecture, and the enumeration is bounded and reviewable |
| **Two world models** | — | **the premise is refuted.** The rung is re-planned on the spike's answer, the way M8.b was re-planned when five escape hatches came in against a budget of two |

**And one spend that is not on the table at any price: a silent fallback.** A `RemoteDevice` request
that quietly runs `InEditor` is a green test over a feature that does not exist. That is the shape
M9's gate found when a criterion passed 44 of 44 with its enforcement point deleted, and the shape
M10's gate refused four times. Task 0.5 makes the refusal the requirement; `specs/live-editing/`
makes it normative.

### 1.4 What the spike does NOT ask

**Whether `editor-architecture` and `live-editing` at Seed are a mis-record has already been
answered, and this rung does not spend a spike on it.** M10 task 6.5 audited nineteen contested cells
across four closed milestones — reading each against the tree M10 closes on rather than against the
gate that parked it — moved fifteen, claimed four, and **kept these two at Seed with a grep as the
evidence for each**. Both greps were re-run on this tree and both still return nothing. The record is
right and the foundation is thin, which is a much more expensive answer than a mis-record and is why
this rung exists.

That is recorded in `tasks.md` as 0.1, ticked, because the useful thing to hand the implementing
agents is the answer and its provenance, not the question again. It is also recorded so that **M11.a
is not asked for it either** — the two rungs were both pointed at it and only one of them needs to
look.

## 2. Three rows arrive here at Seed and are asked for Complete, which is two tiers in one rung

`editor-architecture` (13 requirements) and `live-editing` (11) have been at Seed since M5;
`ml-inference` (9) since M8.c. **Nothing on this ladder has moved a row two tiers in one rung.**

The reason it is being attempted rather than split is that Working and Complete are not two bodies of
work here, they are one body read twice: `editor-architecture`'s thirteen requirements include the
specialised editors, project settings, the build client and the debugger, and a row with those
absent is not Working either. Seeding the intermediate tier would buy a cell and no code.

**What it costs is that the gate has no intermediate reading to fall back on**, and the mitigation is
structural rather than hopeful: each of the three rows has its own task section, and each section's
mechanism tasks are separable from its completeness tasks, so a rung that runs short demotes a
**tier** with a named remainder rather than abandoning a row. §4 names which.

## 3. The rung's real cost is not code in `editor/`

`thirdparty-dependencies` names an intended dependency set; `deps/manifest.toml` carries seventeen
entries accumulated across the whole ladder to date. **Thirteen of the entries this rung's rows
require by name are absent**, and one more is marked *to evaluate*:

| Row | Absent, named in the specification's own table |
|---|---|
| `text-and-fonts` | **HarfBuzz**, **ICU**, **FreeType**, **msdfgen** |
| `asset-import-pipeline` | **libpng**, **libjpeg-turbo**, **libwebp**, **tinyexr**; a **BC7 encoder** (ISPC Texture Compressor or bc7enc) and **astc-encoder**; **cgltf**; **meshoptimizer**; **OpenUSD** (optional, tool-time) |
| `animation-and-skinning` | **ACL**, marked *to evaluate* — for which the specification's own rule is that *"the requirement is the capability, not the library"* |

Each one is a change of its own. That specification's "New dependency is a reviewed decision"
scenario is normative — adding a dependency *"SHALL go through the OpenSpec change flow recording the
evaluation against these criteria"* — and M10 already had this exact collision: `save-and-persistence`
did not complete because its AEAD was a dependency adoption and *"a change of its own, not one to open
inside a closing gate"*.

**This rung proposes to grow the engine's dependency set by more than three quarters, one reviewed
change at a time, while also finishing the editor and building a game.** That sentence is the whole
of §4's risk section and it is written here at proposal time rather than discovered at the gate.

Two of the thirteen are cheap and should be taken first as calibration, because they measure the
adoption process itself against work that is already staged for them:

- **meshoptimizer** — M5 delivered engine-owned simplification, vertex cache and fetch optimisation
  behind free functions over `cy::import::MeshData`, so the integration is *"a change to one `.cpp`
  and to nothing that calls it"*.
- **cgltf** — `tools/import/src/json.cpp` is a strict JSON reader *"written to be deleted when this
  lands"*, and its header says so.

And one is known-expensive and must not be left last: **ICU**, which `deps/manifest.toml` already
describes as *"an autotools project whose CMake support is somebody else's build system wearing a
hat"*, and which three of `text-and-fonts`' normative sentences name.

## 4. Which of these rows are actually contingent, stated rather than assumed

Twenty-four rows and 420 requirements is the largest load on the ladder, and the honest position at
proposal time is that they are not equally risky.

**The three rows this rung predicts it will not complete, named now:**

- **`ml-inference` — a recorded deferral to M11.e is the likely and correct outcome.** Nine
  requirements at Seed since M8.c, and the only thing on this rung that would exercise it is section
  11's game. `src/ml/README.md` states what is deliberately absent — no AI graph node, no Core ML or
  DirectML or TensorRT backend, no Swift surface — and each of those is real work whose *demand*
  comes from a game that wants inference. If the game does not want it, a Complete cell over an
  unexercised runtime is a claim nothing supports, and the honest artefact is a deferral carrying
  what is unmet, why, and the condition that brings it back.
- **`swift-scripting` — the likeliest demotion, and to M11.d rather than M11.e.** Three of its four
  open items are scripting and this rung can do them. The fourth is a **shipping configuration and a
  toolchain pin *verified in CI***, which is a build-system requirement wearing a scripting row's
  name: `deps/host-tools.toml` is where the pin belongs, `just env-doctor` already says it will read
  a `swift` entry when one lands, and **CI has no Swift toolchain**, so `integration.swift_package`
  and `integration.swift_reload` are not registered there today. None of that can be judged before
  M11.d pins the matrix.
- **`text-and-fonts` — contingent on §3 in a way no other row here is.** Four of the thirteen
  adoptions are its, one of them is ICU, and the row cannot reach Complete without all four. The
  engine can lay out Hebrew, Arabic and Thai today and cannot load a font to draw them with. If the
  dependency budget runs out, this is the row it runs out on.

**The rows that carry no named blocker, which is a risk and not a comfort:**

`sequencing-and-cinematics` (31 requirements), `animation-and-skinning` (30), `camera-system` (28),
`input-and-actions` (23), `visual-scripting` (22) and `editor-visual-language` (22) are six of the
eight largest rows on this rung. **Nothing has refused them — because nothing has read them end to
end at Complete grade.** That is precisely the shape M10's gate found behind twelve parked Complete
cells, and the mitigation is section 13.4: a first-hand requirement-by-requirement reading, at the
gate, against what the code supports rather than against what `tasks.md` claimed.

Two of the six have a partial defence already: `src/sequencing/README.md` and
`src/animation/README.md` each carry an explicit "what is NOT here" table written by the milestone
that built them, which is why tasks 8.4 and 8.5 can name seventeen absences between them rather than
discovering them. The other four have no such table, and producing one is the cheapest thing this
rung can do early.

**The rows that are genuinely well-understood**: `physics` (two named gaps — constraints and the
character controller, both refused today by a capability flag rather than approximated), `navigation`,
`rendering-2d`, `editor-agent-interface` and `editor-rust-application`.

**The demotion this design predicts, if one is needed, is `ml-inference`, and the demotion it fears
is `text-and-fonts`.** The difference matters: the first is a scope decision this rung is entitled to
take, and the second would be the dependency budget failing, which would take `asset-import-pipeline`
with it and would be a finding about §3 rather than about either row.

**And the prediction this design is most likely to be wrong about.** M10's design predicted
`procedural-content-generation` and was wrong **in the useful direction**, because a spike measured
it and the contingency did not fire. The equivalent here is section 0: this design's working
assumption is that one world model carries all three play modes, because the transport abstraction
and the three world kinds already exist and the specification says locality is a transport concern.
**If the spike refutes that, it is the plan that changes, not the spike's answer** — and the rung is
re-planned mid-flight, which is what M8.b did and what makes a spike worth running rather than
reasoning past.

## 5. Order, and why the plugin surface is first

The plugin surface is section 2 rather than somewhere convenient, because `editor-architecture`'s
"Specialised editors" requirement is normative about it: *"Each SHALL be a plugin using the same
panel and undo infrastructure as user plugins, so the extension API is exercised by the engine's own
tooling"*, with the scenario *"WHEN a built-in editor is implemented THEN it SHALL use only the
public plugin API, so any limitation is discovered internally first"*.

Building the specialised editors first and retrofitting the plugin boundary afterwards would satisfy
the cell and violate the requirement, and it would do so **invisibly**, because the retrofit's whole
purpose is to make the difference undetectable. Task 2.5 is the forcing function: a built-in editor
that reaches past the public API fails the build.

`find src tools -iname '*plugin*'` returns one layercheck fixture, and that has been true since M5.

**The other ordering decision is that the game is last and is not optional.** `ui-system`'s own
specification says an interface system with no demanding first-party consumer decays, and names the
consumers: the developer console, the debuggers, the profiler overlays, the settings interface and
the conformance suite. **`cy::ui` has exactly one consumer in this tree — `samples/08-vertical-slice`
— and the editor is not one of them**, because the editor is a Rust application with its own chrome
in `cy-editor-visual`. So the row's forcing function has to be the game, and a rung that runs out of
time before section 11 does not get to record `ui-system` at Complete on the grounds that the widgets
compile.

## 6. What this rung depends on from other rungs, and does not absorb

Stated as dependencies so that the boundary is visible, rather than quietly pulled in.

- **M11.a — the save format.** `sequencing-and-cinematics` declares and carries a `PersistenceClass`
  and **no save format writes it**. M11.a re-scopes `save-and-persistence` against M10's
  requirement-by-requirement audit (nine satisfied, three unmet, eight partial, eleven pieces of
  work). Task 8.4 names sequence persistence and does not build a save format for it.
- **M11.a — the ladder mechanics, including this rung's own handover criterion.** Every rung since
  M8.c has closed with an `<next>-open` criterion in the double-star glob form. **That form cannot
  fail here**: `openspec/changes/implement-m11c-image/` already carries a proposal, because the five
  rung changes were opened together rather than one at the close of the last. A criterion that cannot
  go red is not a criterion, and inventing a replacement unilaterally would leave five rungs with
  five different handover checks. `implement-m11-reach`'s task 0.1 is where the split's ladder
  mechanics are answered and M11.a owns it; task 12.3 records the problem and waits for the answer.
- **M11.c — five of the eight missing view modes.** `PLANNED_VIEWS` names eight with the capability
  that owns each, and lightmap density, GI probe placement, virtual texture feedback, virtual texture
  residency and virtual shadow pages are drawn by `rendering-global-illumination`, `virtual-texturing`,
  `residency` and `virtual-shadows` — all M11.c's rows. This rung builds the editor half: the mirror,
  the palette entry, the per-viewport request, the composition with filters and isolation, and
  `tests::the_mode_list_matches_the_engines` keeping the two lists from drifting.
- **M11.c and M11.d — two of the debugging surfaces `editor-architecture` requires.** The
  **render debugger** must show the render graph as it was built for a frame, per pass its GPU time,
  queue, resources, transient memory and barrier wait, and the budget arbiter's allocations for that
  frame — which is `rhi-and-render-graph`'s data and M11.d's row. The **shader and material
  inspector** must show every stage of lowering — graph, material IR before and after optimisation,
  generated Slang, backend binary — with cost attributed to graph nodes and the reason each
  permutation exists, which is `material-compiler`'s and `shader-system`'s data and M11.c's rows.
  **The specification's own sentence about them is the awkward one: *"These tools SHALL be built
  alongside the renderer rather than after it."*** This rung is before M11.c, so it cannot honour
  that ordering for these two. What it can do is build the *views* against the interfaces that
  already exist and leave the criteria that would exercise them reporting NOT EVALUATED, and say so
  — which is what task 3.8 does. If instead these two surfaces move to M11.c with their reason
  recorded, that is a demotion this design would accept rather than argue with.
- **M11.c — what the game looks like.** Task 11.7's screenshot is the game as a player sees it
  through whatever renderer exists when this rung closes. **It is not the beauty shot.** M11.c owns
  that, and owns it *after* this rung deliberately, so that the art-directed shot is authored through
  an editor rather than assembled in C++ — which is the argument for this rung's placement and is
  made in `proposal.md`. A sample game that looks unfinished and says so is more honest than one that
  borrows a claim from the rung above it.
- **M11.d — the build service.** `editor-architecture`'s "Build and deployment" requires the editor
  to be a *client of the build service defined in `build-and-packaging`* and forbids it from invoking
  shell scripts and parsing their output. `build-and-packaging` is M11.d's row. Task 3.7 builds the
  client against the service's interface; if the service is not ready, the client is written against
  the interface and the criterion that would exercise it reports NOT EVALUATED rather than passing.
- **M11.d — the matrix that judges `swift-scripting`'s fourth item**, per §4.
- **M11.e — `thirdparty-dependencies` at Complete.** §3's thirteen adoptions are this rung's work;
  the *row* is M11.e's, and M11.e's proposal already flags that Complete requires integrating *"about
  half the intended set"* and that turning that phrase into a list is its own job. Thirteen landed
  here would be most of that list, which is a fact M11.e should be handed rather than left to
  discover.
- **M11.e — the re-entry point for anything deferred here**, with what is unmet, why, and the
  condition that brings it back.

## 7. What must not be retrofitted

- **The plugin boundary** (§5). Retrofitting it is undetectable by construction.
- **A node's name.** Adding a name to `cy_editor_documents` after the specialised editors, the
  outliner, the diff format, the `.cyworld` writer and the game's content all exist is a migration
  across every one of them. It is section 4.1 for that reason, and the tree already shows the cost of
  not having it: `samples/05b-editor-window/project/worlds/city.cyworld` puts "Pillar", "Crate" and
  "Marker" in the **layer** field, because there is nowhere else to put a name, and
  `hierarchy.rs` labels rows by component kind because there is nothing else to label them with.
- **The cook's derivation key.** `CookedTexture::encoded = false` is recorded beside every
  uncompressed payload today precisely so that a build that links an encoder re-cooks. If the
  encoders land and the key does not change with them, every machine and every cache in the project
  serves uncompressed pixels to a build that asked for BC7, silently, and nothing goes red. Task 6.4
  and `specs/asset-import-pipeline/` make it a requirement rather than a note in a manifest.
- **`HostingMode`'s empty enumeration.** `editor-rust-application` requires reasons for in-process
  execution to be *"enumerated rather than accumulated"*, and `cy-editor-sdk` keeps the enumeration in
  one comment so it is impossible to grow without a reviewer seeing the whole of it. Twenty-four rows
  of new editor work is exactly the pressure that list was written to survive.

## 8. What this rung deliberately does not do

- **It does not tune the image.** Every renderer row is M11.c's. This rung's artefact is a game, and
  the honest measure of it is whether it plays and whether its content was authored — not whether it
  is beautiful.
- **It does not touch `status.yaml` tiers in this phase.** Recording a tier is the closing gate's act
  (task 12.4), and doing it here would be the defect M10's gate refused: a tier written into a record
  that nothing re-checks is not a tier.
- **It does not close any of M11's seven inherited gaps.** All seven are M11.a's, they stay declared,
  and task 12.2's job is only to confirm that each still names the rung that closes it.
- **It does not build a second world model.** If section 0 says it must, the rung is re-planned and
  this sentence is what gets rewritten — in `design.md` §1, with the measurement beside it.

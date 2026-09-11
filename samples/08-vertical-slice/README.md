# samples/08-vertical-slice — the M8.b artefact, EXTENDED at M8.c

*A playable game: a level, characters that animate and think, abilities with effects, a heads-up
interface, sound and a 2D menu — every one of them the engine's real module, driven in one tick.
**M8.c added the two things that make it look finished: particles thrown by its own activation cues,
and a two-shot cinematic cut over its own fight — and made its picture a photograph.***

```
just run-vertical-slice                     # build, play, measure, and draw the picture
just run-sample vertical-slice --agents 512 # the program alone
just run-vertical-slice --only capture --capture build/shots/slice   # M8.c: needs a Vulkan device
ctest --test-dir <build> -R smoke.vertical_slice
```

![the vertical slice, recorded on a device](../../docs/design/images/vertical-slice-m8c.png)

**That image came off a graphics device.** Every earlier picture in this repository of this sample
was the frame's own answer *drawn* by `slice.py`; this one is the frame, recorded through
`cy::rendering-pipeline`'s five callbacks with `cy::rendering-particles` attached to its transparent
stage, executed on Vulkan with validation on, and copied back as a PNG. The readout across the top
is drawn by the same particle renderer, one sprite per lit cell of a 3x5 glyph, so it is the
engine's output too rather than text painted over a photograph.

And this is the identical frame, assembled, compiled, barriered and executed with an **empty
`FrameSinks`** — which is what every caller in this tree supplied before M8.c, and the reason M8.b's
own report said a person could build all of it and still not *see* it:

![the same frame with no record callbacks](../../docs/design/images/vertical-slice-m8c-no-callbacks.png)

The cut, photographed mid-blend — both shots contributing to the camera stack at once, through a
lens strictly between the two rigs' own:

![the cut, mid-blend](../../docs/design/images/vertical-slice-m8c-cut.png)

## What is here

| | |
|---|---|
| `cy_sample_vertical-slice` | the game. `main.cpp` reads a command line and prints `key = value`; `slice.cpp` builds the level; `systems.cpp` authors and compiles the graphs and musters the characters; `tick.cpp` is the loop, the audit and the projection; `presentation.cpp` is the interface, the menu, the sound and the assembled frame; **`spectacle.cpp` is M8.c's particles and its cut, and `capture.cpp` is the frame recorded on a device**. |
| `slice.py` | the driver. **Five acts** — the fifth needs a graphics device and runs only with `--capture` — every claim the program printed checked, the picture drawn from the frame's own draw list and the photographs read back off the device. It reports through `samples/harness/artefact.py`, so a recorded gap cannot exit zero and an extreme value cannot be the headline. |

The target's dependency list is the point. Before this directory `cy::rendering-assembly`,
`cy::animation`, `cy::ai`, `cy::navigation`, `cy::ui`, `cy::audio-acoustics`, `cy::camera`,
`cy::rendering2d`, `cy::gameplay-abilities` and `cy::graph` were each reachable only from their own
test binaries. M7's closing gate said of eight rendering modules that they were "linked by nothing
but their own test binaries"; this is the line that removes that sentence.

## The tick, in the order it runs

```
think     AiRuntime::update_tiers → PerceptionScheduler::update → AiRuntime::think
navigate  find_path over the navmesh · Crowd::set_desired_velocity · Crowd::step · integrate
animate   AnimationBatch::advance_all (deterministic) → evaluate_range (skippable)
act       ActivationPipeline::activate ×N → EffectSystem::advance → the objective script
present   cy::ui layout/flatten/audit · rendering2d::build_batches · audio tiers and music
frame     SnapshotExtractor → SceneIndex::apply → FrameAssembly::assemble
```

Nothing in that list is interpreted. Five graphs are authored with `cy::graph::Graph` and compiled
once, before the loop: the objective script and the ability program by `cy::graph::script`, the pose
program by `cy::graph::pose`, the behaviour program by `cy::graph::behaviour`, and the camera rig by
`cy::graph::camera` on the shared expression core. `Slice::audit()` is what says so — see below.

## The five things this artefact is asked to prove, and how each is checked

**1. No graph is interpreted at runtime.** `InterpretationAudit` is three independent checks and
each can fail on its own: every authored graph audits clean **and complete** (`AuditReport::complete`
is a third answer beside pass and fail, and a gate that read `missing` alone would go green the day
a plugin failed to load); no compilation happens inside the loop, counted by the slice's own compile
calls and read before and after it; and no per-entity state type of any consumer is polymorphic,
which is `std::is_polymorphic_v` over the ten state types, reported per type rather than as one
boolean. `--interpret-control` is the negative control: it puts an object with a virtual `tick()` in
the loop, the audit finds it by name, and the program exits non-zero. Act 4 passes only when that
run **fails**.

**2. Cost is bounded by configuration.** The scale act runs 8,000 agents and 2,000 agents and
compares them. Every declared budget (`Budgets` in `slice.h`, printed by the program so the driver
compares a measurement against the source rather than against a number copied into a script) must
hold, **and** the cost per agent at four times the population must be within the declared linearity
factor. The second is the one that means something: a budget alone measures the host, and a
linearity factor catches a quadratic on any machine.

What does the bounding is the AI LOD policy, and the artefact prints the distribution so a reader
can see which dial did it. Two configuration decisions are load-bearing and are argued at their
lines: the tier distances are written for **this** arena rather than left at the default written for
a world tens of kilometres across, and the arena's size scales with the population so that the
density is constant — eight thousand characters in a 48 m square is 0.29 m² each, which is closer
than they can physically stand, and every neighbour query would then be measuring the overcrowding.

**3. Determinism.** Two slices in one process (`--replay`), and two processes. Both fold the same
digest: every character's quantised placement each tick, the AI's per-tick state hash, the effect
system's digest, the attribute store's digest and every activation identity.

**4. An authored mesh reaches a renderer.** M8.a's photograph showed an authored sphere as a box
because `MeshRenderer` had no reflected type and no reference ever became a handle. Here the chain
runs end to end: the component is reflected (task 11.3's first half), `bind_render_assets` resolves
its reference against the level's own mesh table (the second half), the extract stage publishes the
handle, and a surface query resolves each draw through `SceneIndex::surface_of`. The picture's
colours are the **material slot the frame resolved**, read back out of the scene index — which is
why the two teams are two colours over one mesh.

**5. The interface holds its budget and passes its accessibility audit.** `cy::ui`'s own
`audit_accessibility`, over the one interactive element, with no finding.

## M8.c: what was added, and where each piece attaches

**The slice was EXTENDED rather than replaced**, because a second sample would prove these systems
work beside a copy of the game rather than inside it. Both new halves attach at the seams the table
below already named at the end of M8.b, and neither of them added a third.

| | |
|---|---|
| `spectacle.{h,cpp}` | the particles and the cut. `on_cue()` plays a compiled VFX system at the caster for every committed activation; `step()` advances the effect world and, while the cinematic is live, hands back the pose `cy::camera::CameraServer::evaluate_stack()` produced. It names no ECS component, no entity and no camera transform. |
| `capture.{h,cpp}` | the frame RECORDED on a device: the level's mesh assets as geometry, the material table filled from the palette `Level::resolve_material` resolves into, the pipeline layer's callbacks, the particle renderer's extension, the readout, and the readback. |
| `--no-spectacle` | the control that makes task 5.3 mean something. |

**The effect is `src/vfx/tests/effects.cpp`, compiled in rather than copied.** That file authors the
spark plume with CyberGraph and cooks it through `compile_system`; `integration.vfx_compiler`
asserts what it compiles to, `integration.vfx` what it simulates to and `render.vfx` what it draws
as. A second plume written out here would be a fourth description of one effect, drifting from the
other three inside a milestone. What this artefact adds is the one thing those three suites cannot
say: that it survives being played from a gameplay cue, inside a running game, at the population a
fight produces.

**The cut writes no camera transform, and that is a measured zero.** A shot selects a RIG; the
bridge pushes it as a stack contribution; the camera server blends the poses its own rigs produce.
`cut_camera_property_writes` counts arbitrated writes addressed at `SubsystemId::Camera` and
`cut_pose_overrides` counts frames on which the server reported a pose forced past the rig. Both are
zero, and both were proved able to move: pointing the first counter at `SubsystemId::Light` instead
turns the check red at 90 writes, which is the light track doing exactly what a camera track must
not.

**The determinism act is the firewall's proof from this side.** `--no-spectacle` runs the same
options with neither system live, and the two runs must fold the identical digest. Folding
`vfx_live_particles` into `Slice::fold_tick` was run and turns it red immediately.

### What the two new systems cost, and how they are bounded

`Budgets::kSimulationUs` is M8.b's number and **M8.c did not raise it**. Task 5.1 asks that particles
and a cut fit *inside* the budget the slice already declares, so `slice.py` checks the sum. The
configuration that bounds the cost is three numbers in `spectacle.cpp`, and each was measured rather
than guessed: the first version cooked the plume at two emitters of 256 particles with a ceiling of
192 instances, reached 96,000 live particles at 120 ticks and cost **20.4 ms a tick** — five times
the whole spectacle allocation. One emitter of 48, at most twelve *spawning* effects at once with
the oldest stopped gracefully rather than evicted, and a ceiling of 160 instances gives **1.40 ms a
tick at 64 agents and 1.68 ms at 256**: the cue rate rises with the population and the cost does
not. What rises instead is `effects_refused`, 32 against 651 — the world declining to exceed its own
instance ceiling and saying so.

## Why one picture is a diagram and the others are photographs

**This section used to be called "Why the picture is drawn and not captured", and M8.c closed it.**
Its argument was correct at the time: `FrameAssembly` hands a pass's record callback to its
**caller** — "it does not own the shaders or the pipelines" — this sample supplied none, and writing
them would have meant a second renderer beside `samples/03-first-light`'s. M8.c's section 1b built
that layer once, in the engine (`src/rendering/pipeline/`), so what was left here was what a *game*
does with it: fill in the geometry, the materials and the exposure, attach the particle renderer,
and read the image back. `capture.cpp` is 700 lines and creates no shader and no pipeline.

`--shot` still writes the drawn picture and **its banner now labels it a DIAGRAM**, which is what
task 5.6 requires of an image that is not the engine's own output. It is still worth having: it
shows the heads-up interface's own flattened primitives and the menu's own batched instances, which
no recorded frame in this sample draws.

What that drawing is, and it has not changed: Every shape in the picture is one item
of the **sorted draw list** the assembly produced; its bounds are the spatial index's; its
silhouette is the mesh handle `surface_of(slot)` published — the one `bind_render_assets` resolved,
looked up in the level's mesh table, **not** in the sample's record of what it authored; its colour
is the material slot, read back the same way; and its corners were projected by `projection * view`
in C++, by `cy::Mat4`, before `slice.py` saw them.

That distinction was a finding of M8.b's closing gate rather than a design: `kind_of` searched the
level's own `props` array for the entity and answered the kind this sample had authored, which
draws the same picture whatever the renderer resolved — and would have drawn M8.a's
sphere-as-a-box correctly while the defect was still there. Breaking `resolve_mesh` to answer one
handle for every reference now collapses every silhouette in the picture to the ground mesh, and
`the frame names as many distinct meshes as the level interned` goes red; before the gate, both
stayed green. The heads-up interface is `cy::ui`'s own flattened primitives and the menu is
`cy::rendering2d`'s own batched instances. Nothing in the driver re-derives what is on screen; it
reads a file the program wrote and paints it.

## The seams M8.c attached to, and what each one turned into

**M8.c extended this slice rather than building a second one**, so the two things it adds had
somewhere to attach. Every row below is now USED rather than reserved.

| M8.c's | attaches at | what it consumes |
|---|---|---|
| `vfx-system` | `ActivationPipeline::cues()` | The activation pipeline already emits a cue per committed activation, carrying (activation identity, cue tag, simulation point). `tick.cpp`'s `act()` counts them; a particle system reads them instead. A cue is suppressed by (activation, cue, simulation point), so a re-simulation must pass the recorded identity in `ActivationRequest::identity` — that is what lets the ledger recognise a repeat, and it is the constraint a particle system inherits. |
| `vfx-system`, on the render side | `FrameSinks::passes[...]` and `SceneIndex` | The frame is assembled with a `FrameSinks` whose surface query is supplied and whose pass callbacks are not. A particle pass is one more callback in that array plus one more entry in the spatial index; nothing in `presentation.cpp` changes shape. |
| `sequencing-and-cinematics` | `ViewState` in `tick.cpp` and `brain.rig_output` | The camera is a compiled rig program evaluated every tick with its inputs named. A sequence drives cameras by supplying those inputs, which is that specification's own "a sequence does not write camera transforms". |
| `sequencing-and-cinematics`, for gameplay | `kit.phases` and the objective script | Authoritative change is ordered commands. `PhaseController::enter` and `cy::graph::script`'s host interface are the two places a timeline would issue them, and both already refuse what they are meant to refuse. |

## Findings this artefact measured, in files it does not own

Each was found by running and each is reproduced by a command. The first was CLOSED at the closing
gate, in the module that owns it, with a regression test; the other three are open, and none of them
is fixed here because the file belongs to another module.

**1. `ScriptProgram::digest()` was not reproducible across processes — FOUND HERE, FIXED AT THE
GATE.** `finish_digest` (`src/graph/src/lower_script.cpp`) hashed each constant as raw bytes —
`hash_bytes(digest, &constant, sizeof(constant))` — and `script::Value` is 32 bytes of which four
are padding between `z` and `handle` that no member initialiser touches. Three runs of the same
graph gave three digests. A probe run inside this sample settled the cause and a byte dump settled
it beyond argument: bytes 20 and 21 of every constant held a fragment of a spilled stack address,
so they moved with address-space layout — which is why the same run reproduced under
`--profile release` and not under `--profile dev`, the shape of an indeterminate value rather than
of a logic error. This is a cook key and the back-end selection key `visual-scripting` requires to
be stable, so it reached further than this artefact.

`src/graph/` now hashes the five fields through `script::hash_constant`, and
`src/graph/tests/test_lowering.cpp` carries the regression: it compares the program's own digest
against the same sum recomputed field by field, having first asserted that the padding really is
dirty, so a return to hashing the object fails on this tree every time. Restoring the old line was
run and the case went red. `sizeof(Immediate)` in `src/graph/include/cy/graph/expr.h` gained a
`static_assert` beside the three places that still hash a struct as bytes, because that layout is
flat today and nothing was stopping it from not being.

**2. `Crowd::rebuild_grid` is O(agents × occupied cells).** Its comment says the cell lookup is
"O(1) in the common one because agents arrive in position order", and the scan restarts from the
beginning of the cell table every time, so an agent in the last cell scans every cell before it.
Measured on this machine at 8,000 agents over a 179 m arena: **72 ms a tick with 2 m cells and 9 ms
with 11 m cells**, the whole difference being the scan. `crowd_cell_size()` in `internals.h`
chooses the cell that minimises the sum of the two costs and says why; a hash of the cell key would
make the build linear and make that function unnecessary.

**3. `PerceptionScheduler`'s broad phase has no spatial index.** Its header describes step 2 as
"broad-phase by declared filter — faction, range, layer — **over a spatial grid**"; `gather()` loops
every target for every due observer, and `build_batch` then scans the queries already built for each
request, so the cost is (due observers × targets × queries built). At 8,000 agents with the default
20 m sight range in a 48 m arena that was 140 ms a tick. It is bounded here by configuration — a
crowd member sees nine metres, and only the squad is registered as a perception target, which is
`PerceptionTarget`'s own "what is perceivable is a gameplay question" — but the shape is worth
closing.

**4. `ThinkReport::starved` counts an agent promoted out of a coarse tier.** Starvation is measured
as `tick > last_think_tick + tier_think_interval(current tier)`, and a `Statistical` agent is
skipped entirely so its `last_think_tick` goes stale. When the LOD policy promotes it, it is
reported starved for as long as the stale timestamp lasts, although it thought exactly when its tier
said it should. `ai-system` says "zero is the requirement; a gate reads this and nothing else", so
the definition matters: measuring against the interval the agent was AT when it last thought would
report what the requirement means.

## Findings M8.c measured, in files this sample does not own

**5. `SceneIndex::apply` leaves `SpatialEntry::gpu_slot` at zero and nothing can set it
afterwards.** Its own header says the first half — "a caller that does own a GPU scene writes
`gpu_slot` into the index itself" — and `SpatialIndex` exposes no way to do so after `insert`. Every
draw of a `SceneIndex` frame therefore carries `instance_slot == 0`, so every instance reads
instance row zero. **That is invisible on a frame which is only ASSEMBLED and puts the whole level
on top of its first prop on a frame which is RECORDED**, which is how it was found. `capture.cpp`
works around it by rebuilding the entries with `gpu_slot` set to the publisher's own spatial slot,
and says so at the site; the fix belongs in `src/rendering/culling/` or `src/rendering/assembly/`.

**6. The slice's sun had an identity rotation, so it pointed straight down the view axis.** An
identity `Transform`'s forward is `(0, 0, -1)`, which leaves every horizontal surface unlit — the
arena floor photographed black. The assembled frame reported five lights either way, which is
exactly the class of defect a picture finds and a counter cannot. Fixed here, in this sample's own
`presentation.cpp`, because the light is this sample's.

**7. At 8,000 agents almost every activation from the main rotation is REFUSED**, because the
victim is chosen half an arena away and the arena scales with the population — 21 committed against
519 refused, and the 21 all come from the deliberate repeat activation rather than from the
rotation. The fight, and therefore the particle load, is thin at the scale act's population: 21
effects and about a thousand particles against 309 effects and four thousand particles at 256
agents. It is this sample's own configuration rather than a defect in `gameplay-abilities-and-
effects`, and it is recorded because "particles inside the slice at the criterion's population" is a
weaker claim than it looks until you know it.

## What is deliberately not here

**Steam Audio.** Task 5.1b asks that the slice's sound go through it when `CY_AUDIO_STEAM_AUDIO` is
on and through the fallback when it is off, with the same gameplay either way. **Only the second
half is true**: the option still does not configure and `SteamAudioBackend::simulate` still returns
`ErrorCode::NotImplemented`, so there is no ON configuration for the slice to run in.
`deps/manifest.toml` now records exactly what it costs to change that, and
`tools/roadmap/milestones/m8c.toml` declares `steam-audio-configures` as a criterion that fails
today rather than dropping it. No physics
solver: the level is static geometry and the characters are steered by the crowd, which is what
`navigation`'s own "this is a stand-in for the character controller" note describes; `physics`
closed at M8.a and a body per character would be measuring Jolt rather than this milestone.

**And one sentence that used to end this section is gone, because M8.c made it false.** It read "No
recorded render pass: see why the picture is drawn and not captured", and it survived into this
milestone's own README while act 5 was recording one. The closing gate found it; it is noted rather
than quietly deleted, because a stale absence is exactly as misleading as a stale claim and this
sample's whole argument is that the difference is checkable.

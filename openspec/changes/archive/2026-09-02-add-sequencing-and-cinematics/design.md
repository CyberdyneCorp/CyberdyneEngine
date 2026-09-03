# Design: CyberSequence

## 1. An orchestrator, not an owner

The failure mode of cinematics systems is that they acquire their own versions of everything: their
own camera, their own animation playback, their own weather, their own particle lifetime management —
because each was easier to write than to integrate. The result is two cameras that disagree, weather
that snaps back when a cutscene ends, and effects that outlive the sequence that spawned them.

Six capabilities anticipated this and left hooks rather than letting a cinematic system route around
them. So a sequence **evaluates a timeline and produces commands**; camera rigs are evaluated by the
camera system, poses by animation, playback by audio, and weather transitions by weather.

The practical form of that rule is **batched dispatch**: evaluation fills per-subsystem command
buffers, and each subsystem consumes its batch, rather than tracks making virtual calls into
subsystems mid-traversal.

## 2. Compiled, like everything else that composes

Authoring is tracks, sections, channels and keys. Runtime is a program.

This is the seventh time the engine makes this decision — after materials, effects, animation,
behaviour, camera rigs, procedural generation, and gameplay graphs — and the argument is identical:
authoring composes, runtime executes, and no editor object is traversed at sixty hertz.

The specific payoff here is **cost proportional to what is active**. A five-minute cinematic with
fifty thousand keys should cost what its currently-active sections cost, which requires the compiler
to build interval and event indexes rather than the runtime scanning tracks. A million authored keys
must never imply a million evaluations.

## 3. Time is exact, and it is not the simulation tick

Accumulating a floating-point delta is the wrong clock for something that will be scrubbed, stepped,
seeked, replayed, and rendered offline. Sequence time is an **exact rational** — a frame and a
subframe at a declared rate — so that frame 1,432 is the same instant every time it is reached, by
any route.

**Sequence time is not the simulation tick.** A cinematic at twenty-four frames per second may play
over a sixty-hertz simulation rendered at a hundred and forty-four hertz, and conflating them
corrupts one of the three. So a sequence declares a **clock domain** — presentation, simulation,
cinematic, real time, or external — and only the simulation domain is tied to ticks and eligible to
carry authoritative gameplay.

## 4. The gameplay boundary, and the sixth producer

`gameplay-framework` specifies that human input, artificial intelligence, network peers, replay, and
automation all produce the same command stream, and that the simulation cannot tell them apart. A
cinematic that changes gameplay state is a sixth producer, and the honest thing is to say so rather
than to let property tracks write authoritative components directly.

So gameplay tracks emit **commands and events**, validated like any other, and every track carries an
**authority classification** that the compiler checks: a presentation-only sequence containing an
authoritative track is a build error, not a surprise in multiplayer.

This is what allows a cinematic to be safely non-deterministic while the mission event it triggers is
not.

## 5. Arbitration, because two sequences will drive one camera

A gameplay camera, a boss introduction, and a photo mode will eventually all want the camera in the
same frame, and last-writer-wins produces behaviour nobody can explain.

Sequences therefore declare **priority and blend groups**, and the camera system receives a resolved
result rather than a sequence of overwrites. The same applies to any property two sequences touch:
the resolution is declared, and the debugger can attribute a value to the contributions that produced
it.

## 6. Restoring what a sequence changed

A sequence that raises a light's intensity for a shot must be able to put it back, and the naive
implementations either snapshot everything or leak the change.

Adapters supply **capture and restore for the properties they support**, and a section declares
whether it restores, holds, or keeps its effect on completion. What is captured is the properties the
sequence touches, not an arbitrary object graph — which is affordable precisely because bindings and
properties are known at compile time.

## 7. Seeking is where cinematics systems usually break

Scrubbing a timeline is not playing it quickly. Firing every event from time zero would trigger
mission events during editing; ignoring them entirely would leave subsystems in the wrong state.

So seeking has **modes** — preview, runtime, replay, reconstruct — and each adapter declares its
**seek capability**: evaluate directly at a time, reconstruct state, simulate forward with a
pre-roll, or restart. Animation can usually evaluate a pose at a time; a particle system may need to
simulate; audio may need to seek a stream or skip a sound.

The contract that makes this testable: **playing to a time and seeking to that time must agree**
wherever an adapter claims seekability, and that equivalence is a test.

## 8. Skipping must apply what it skips

This is the sharpest requirement in the capability, and it is a defect that ships regularly: a player
skips a cutscene, the sequence stops, and the door it would have opened stays closed.

A skippable sequence declares its **required authoritative outcomes**, and skipping applies them —
events, commands, and final state — before jumping presentation to the end. A sequence that carries
gameplay consequence and does not declare them is not skippable, and saying so at author time is
better than discovering it in a bug report.

## 9. A sequence knows the future, which streaming systems never do

Every streaming and residency decision in this engine is either reactive — feedback from what was
sampled — or predictive from camera motion, which extrapolates a few hundred milliseconds.

A sequence knows exactly where the camera will be in six seconds, which entities will be bound, and
which assets each shot needs. Compiling a **preload plan** and publishing a **streaming source with
future bounds and deadlines** turns that knowledge into prefetching, and it is the one place the
engine can be genuinely ahead rather than merely quick.

This is the strongest argument for compiling sequences rather than interpreting them: the plan falls
out of the compilation.

## 10. Networking replicates intent, not interpolation

Replicating every animated track value would be absurd when every client holds the same program.

So the network policies are: local only, server triggered (identity, start time, bindings,
parameters — clients evaluate locally), synchronised (with periodic clock correction, for shared
music and coordinated events), and deterministic (simulation clock, for lockstep).

Late join **seeks**; it does not replay ten minutes of timeline in real time. And a speculatively
started sequence that a rollback invalidates reconciles through the **side-effect ledger** that
already exists, rather than inventing a second suppression mechanism.

## 11. Not a second scripting language

Timeline systems accrete logic: a condition here, a loop there, and eventually a worse version of the
scripting language the engine already has.

Sequences support **limited conditional selection** — branch on a parameter, jump to a marker, select
a section variant — and nothing more. Deciding *whether* a sequence plays, and under what conditions,
belongs to gameplay graphs and gameplay code. This is a recorded non-goal, because the pressure to
cross it is continuous.

## 12. Build order

| Phase | Contents |
|---|---|
| 1 | Identity, exact time, source schema, tracks, sections, channels |
| 2 | Compiler, intermediate representation, interval and event indexing |
| 3 | Runtime player, instances over shared programs, binding resolution |
| 4 | Camera adapter; animation adapter |
| 5 | Audio and effects adapters; nested sequences and parameters |
| 6 | Preload plans and the streaming source |
| 7 | Gameplay event and command tracks with authority classification |
| 8 | Environment, material, light and interface adapters |
| 9 | Seek, scrub, restore, and skip semantics |
| 10 | Networking, replay, rollback reconciliation |
| 11 | Editor timeline and curve editing on the shared graph infrastructure |
| 12 | Hot reload, semantic diff and merge, diagnostics, cinematic rendering |

**Phase 2 is the milestone that matters**: once the timeline is a compiled program with interval
indexing, cost stops scaling with authored content and the preload plan becomes available. A runtime
built on editor objects would have to be replaced to get either.

## 13. Non-goals

- **A general logic language.** Limited selection only.
- **Owning subsystem state.** Sequences produce commands; subsystems remain authoritative.
- **Backward simulation.** Reverse playback evaluates curves in reverse and follows declared event
  policies; it does not run subsystems backwards.
- **Animating a head-mounted display's pose.** A sequence may move a rig root; the user's head pose
  remains the user's.

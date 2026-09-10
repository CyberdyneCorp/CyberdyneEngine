# M8.c — Spectacle: what makes a game look finished, once it already plays

## Why

**M8.b made the engine playable and deliberately did not make it look finished.** Its own spike
found the milestone larger than its plan assumed — one graph IR could not serve seven consumers, so
what shipped was one authoring layer, one shared expression core and a lowering per consumer — and
`split-m8b-defer-spectacle` reduced the scope before it was built rather than after it failed to
close. Three capabilities moved here: `vfx-system`, `sequencing-and-cinematics` and `ml-inference`.

**Nothing deferred was a prerequisite of anything kept, and that is verifiable rather than asserted.**
`animation-and-skinning` names VFX only as a *consumer* of its curves and its pose world;
`camera-system` calls sequences "the principal producer of anticipated cuts" and works without one;
`gameplay-framework` requires that "the simulation SHALL NOT be able to distinguish" a
sequence-issued command from any other; `ml-inference` is recorded as "`ai-system`, optionally".
Every reference is a consumer relationship. M8.b closed with all three at `none` and no capability
it advanced waiting on them.

**What is verified absent on the tree M8.b closed on**, rather than assumed:

- There is no `src/vfx/`, no `src/sequencing/` and no `src/inference/`. Three of this milestone's
  three capabilities have no directory.
- `vfx-system`, `sequencing-and-cinematics` and `ml-inference` are all recorded at `none` in
  `docs/roadmap/status.yaml`.
- `samples/08-vertical-slice` runs four acts — the game, scale, determinism and a negative control —
  and none of them contains a particle or a cut. Its README says so.
- **The determinism firewall has never been tested, because it has never had a subject.** M8.b's
  exit criterion "VFX and inference cannot write gameplay state, proven by a test" moved here with
  the two systems it names, on the rule that *a criterion whose subject was deferred is a criterion
  its milestone satisfies vacuously*. That is the single most important thing this milestone owes.

**And one inherited certainty about how to build it.** M8.b's spike is committed and its answer
applies here unchanged: `vfx-system` requires "graph compilation to an intermediate representation"
and `sequencing-and-cinematics` requires "compiled programs", and **neither lowers through the other
consumers' IR**. Both adopt CyberGraph as their authoring layer and compile to the form their own
specification names — a particle kernel and a timeline are not the same language, which is what
`visual-scripting`'s "No universal representation" requirement says and what five escape hatches
against a budget of two measured.

*Reject on sight any proposal to reach for one IR again because several compilers looks like
duplication. It was measured, not argued.*

## What Changes

- **`vfx-system` to Working** — the graph compiler and its own IR, GPU-first simulation with the CPU
  path as a declared fallback rather than a silent one, compiler-derived attribute layout, the
  unified simulation world and its global scheduler, data interfaces, GPU scene integration for mesh
  particles, GPU-to-GPU events, and importance classes that make cost bounded by configuration.
- **`sequencing-and-cinematics` to Working** — compiled timelines, exact time and clock domains,
  bindings, tracks and their authority classification, batched subsystem dispatch, arbitration
  between sequences, capture and restore, seek and skip that applies what it skips, and preload
  plans.
- **`ml-inference` to Seed** — model assets, tensors and sessions, the backend abstraction, and
  **the determinism boundary**, which is the requirement that makes the tier worth reaching at all:
  a non-pinned model feeding an authoritative node is rejected at cook time, not diagnosed at
  runtime.
- **The determinism firewall proven by a test**, in both directions: a VFX readback and a
  non-pinned inference result each attempting a gameplay write, each refused, and a re-simulation
  that lands on the identical digest regardless of what either did.
- **`audio` to Complete**, inherited from M8.b's closing gate. That gate ran
  `-D CY_AUDIO_STEAM_AUDIO=ON` for the first time and it did not configure — upstream expects
  PFFFT, IPP and FFTS as pre-installed packages the manifest does not provide — and found
  `SteamAudioBackend::simulate` returning `ErrorCode::NotImplemented`. Spatial acoustics is the same
  kind of work as particles and cuts, so the cell moved here rather than to a milestone about
  something else.
- **The slice is extended, not replaced.** `samples/08-vertical-slice` gains particles and a cut. A
  second slice would prove these systems work beside a copy of the game rather than inside it.

## Capabilities

### Advanced Capabilities

`vfx-system` and `sequencing-and-cinematics` to **Working**; `ml-inference` to **Seed**; `audio` to
**Complete**, inherited.

The full column is `docs/roadmap/capability-matrix.md`; the M8.c row of `docs/ROADMAP.md` is the
scope statement this proposal implements.

### Modified Capabilities

- `vfx-system` — **the determinism firewall names where it is enforced.** `vfx-system` and
  `ml-inference` each state the rule from the producer's side and neither states where it is
  enforced; a rule enforced nowhere is a convention. The delta requires a single enforcement point
  every authoritative write passes through, and requires the firewall's own test to be a negative
  control — because M8.b carried this criterion with neither subject in the tree, which is a
  criterion its milestone satisfies vacuously.

## Impact

- **New code**: `src/vfx/`, `src/sequencing/` and `src/inference/` — three directories that do not
  exist today — plus the two tracks the slice grows.
- **Existing code**: `samples/08-vertical-slice` gains an act; the camera stack gains a sequence as
  a producer of anticipated cuts and **does not gain a sequence writing camera transforms**;
  `src/ai/` gains an optional inference node behind its authoritative-output declaration.
- **Closing artefact**: `samples/08-vertical-slice`, extended — the same playable game with
  particles and a cinematic, holding the frame budget it already declares.
- **Risk**: none new. Both graph consumers lower through the layer M8.b built, and the spike that
  sized that layer already accounted for them. The GPU-first half of `vfx-system` is the part with
  real unknowns, and it is bounded by an existing seam: the render graph, its barriers and its
  async-compute queue all exist and are gated.

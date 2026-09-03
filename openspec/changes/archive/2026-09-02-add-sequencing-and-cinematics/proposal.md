# Sequencing and cinematics

## Why

Six capabilities already reference cinematics as something that exists, and nothing produces it.

`residency` propagates deadlines when "a cinematic declares a camera cut". `camera-system` specifies
cinematic overrides in the camera stack, anticipated cuts announced in advance, and authored camera
tracks for directed replay. `temporal-rendering` invalidates history on a camera cut. `weather-and-wind`
states that "sequencing tools SHALL drive weather by manipulating this state; a separate
cinematic-only weather implementation SHALL NOT exist". `animation-and-skinning` has markers and clip
streaming built for exactly this. `audio` has timing and synchronisation.

Each of those is a hook waiting for an orchestrator, and each was specified that way deliberately —
the alternative, a cinematic system that owns its own camera, its own weather, and its own animation
playback, is the failure mode those requirements were written to prevent.

The contract:

> **CyberSequence coordinates subsystems; it does not own them.** Camera, animation, audio, effects,
> environment, interface, and gameplay remain authoritative within their own systems, and a sequence
> produces batched commands for them.

And the boundary that keeps it safe:

> **Presentation may be orchestrated freely. Authoritative gameplay changes cross the same command
> and determinism boundaries as every other source** — a sequence is the sixth producer of the
> gameplay command stream, alongside human input, artificial intelligence, network peers, replay, and
> automation.

## What changes

**`sequencing-and-cinematics`** — timelines authored as tracks and sections and **compiled into
programs**, so runtime never traverses editor objects and cost scales with what is active rather than
with how many keys were authored; **exact rational time** with declared clock domains, so a
twenty-four-frame cinematic, a sixty-hertz simulation, and a hundred-and-forty-four-hertz display
coexist without one corrupting another; **stable bindings** by identity so a sequence is reusable and
survives reload; per-track **authority classification** validated at compile time, so a
presentation-only sequence cannot quietly change authoritative state; batched dispatch into
subsystem command buffers rather than virtual calls per track; **arbitration** when two sequences
drive one thing, instead of last writer winning; **seek and scrub semantics** with per-adapter
capability, and the contract that playing to a time and seeking to it agree; **skip semantics** that
apply required authoritative state rather than merely stopping — the defect that ships in a
surprising number of games; **preload plans and a streaming source**, so a sequence tells the world
what a shot will need seconds before the camera arrives; network policies from local-only to
deterministic, with late join by seeking rather than by replaying ten minutes of timeline; rollback
reconciliation through the side-effect ledger; accessibility metadata; and semantic diff and merge.

**Six capabilities are updated** where the hook becomes a real connection: the gameplay command
stream gains its sixth producer; camera cuts gain their principal announcer; weather's sequencing
seam is filled; the residency layer's cinematic deadline gains a producer; and the editor gains a
sequence editor built on the shared graph and curve infrastructure rather than a seventh bespoke one.

## Impact

- **New**: `sequencing-and-cinematics`
- **Modified**: `gameplay-framework` (sequences are a command producer),
  `camera-system` (cinematic cuts and tracks), `weather-and-wind` (the sequencing seam closes),
  `residency` (the cinematic deadline has a producer), `editor-architecture` (the sequence editor),
  `thirdparty-dependencies`
- **Deliberately not in scope**: a general branching or logic language. Sequences support limited
  conditional selection; deciding *whether* a sequence plays belongs to `visual-scripting` and
  `gameplay-framework`, and a recorded non-goal keeps it that way.

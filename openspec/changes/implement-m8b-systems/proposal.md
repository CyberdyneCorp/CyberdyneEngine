# M8.b — Systems: everything that lowers through one graph

## Why

**M8.a made a scene something a person can build; nothing in it moves on its own.** A sphere falls
on a box because Jolt integrates it. There is no behaviour, no animation, no ability, no cinematic,
no interface and no sound in the loop. `samples/08a-authoring`'s own report says the two entities it
authored are drawn as unit boxes because the mesh a primitive references reaches no renderer, and
that is the smaller half of the gap: the larger half is that fourteen capabilities on the ladder are
at Seed or below, and **six of them are the same problem wearing six names.**

**That problem is the shared graph.** Abilities, AI, animation, sequences, VFX and camera rigs are
each specified as an authored graph that must be *compiled to a program* and evaluated in batch —
`gameplay-abilities-and-effects`' compiled ability programs, `ai-system`'s unified tree/utility/GOAP
graph, `animation-and-skinning`'s compiled animation programs, `sequencing-and-cinematics`' compiled
timelines, `vfx-system`'s graph compiler and IR, and `camera-system`'s rig graphs. Seven consumers
counting `visual-scripting` itself. **Discovering that one IR cannot express one consumer's
semantics is cheap with one consumer built on it and expensive with six.** That is the risk spike
this milestone exists to run first, and it is the reason M8 was split: the settled authoring half
closed as M8.a without waiting for it.

**What is verified absent rather than assumed**, on the tree M8.a closed on:

- `src/gameplay/` holds `play/` and nothing else — no rules, no participants, no teams, no
  ownership, no capabilities, no tags, no phases, no time domains, no interaction, no features and
  no indexes. `cy::gameplay::PlaySession` builds bodies from an authored world and ticks them.
- There is no `src/animation/`, no `src/ai/`, no `src/navigation/`, no `src/vfx/`, no `src/ui/`, no
  `src/sequencing/` and no `src/abilities/`. Nine of M8.b's fourteen capabilities have no directory.
- `visual-scripting`, `sequencing-and-cinematics`, `gameplay-abilities-and-effects`, `ai-system`,
  `navigation`, `vfx-system`, `ui-system`, `rendering-2d` and `animation-and-skinning` are all
  recorded at `none` in `docs/roadmap/status.yaml`. `ml-inference` likewise.
- **No graph IR exists anywhere.** `src/rendering/material/` compiles a material graph to closures
  and a program, and its IR is the only compiled-graph implementation in the tree. The roadmap's own
  dependency note says the material compiler precedes the other graph consumers precisely so that
  its IR is the thing they lower through; whether it can be is what the spike answers.

**And one inherited debt.** M8.a's closing gate moved `serialization-and-prefabs`' Complete cell
here: "Apply and extract" — pushing an instance's overrides back onto its prefab, and lifting a
subtree into a new prefab asset — has no implementation anywhere in the tree and has not had one
since M2, while the data model that would support it (`diff`, `Override::clone_into`, the placement
mapping) has been complete since M2. It belongs in this milestone beside `live-editing` and
`editor-viewport-and-gizmos`, which complete here for the same reason: they are all editor
operations over a data model that is already there.

## What Changes

- **One graph IR, proven against every consumer before six are built on it.** Typed pins, stable
  node identity, the IR itself, execution backends, async graphs, semantic merge and debugging —
  `visual-scripting` to Working — with the spike measuring the material compiler's IR against the
  semantics each of the other six needs and recording what it cannot express.
- **The gameplay framework at Working**: rules, session fragments, participants and teams,
  ownership/control/authority, capabilities, tags, phases, time domains, interaction, features and
  indexes, on top of M8.a's play session.
- **Abilities, AI, navigation, animation, sequences, VFX** — each a compiled program over the shared
  IR, each evaluated in batch rather than per entity per frame.
- **An interface and text**: `ui-system` to Working over dedicated element storage and a retained
  tree, `text-and-fonts` to Complete, `rendering-2d` to Working.
- **Audio to Complete** and `camera-system` to Working; `ml-inference` to Seed with its determinism
  boundary declared and enforced.
- **`serialization-and-prefabs` to Complete**: apply and extract, inherited from M8.a's gate.

## Capabilities

### Advanced Capabilities

`gameplay-framework`, `gameplay-abilities-and-effects`, `visual-scripting`,
`sequencing-and-cinematics`, `animation-and-skinning`, `ai-system`, `navigation`, `vfx-system`,
`ui-system`, `rendering-2d` and `camera-system` to **Working**; `text-and-fonts`, `audio` and
`serialization-and-prefabs` to **Complete**; `ml-inference` to **Seed**.

The full column is `docs/roadmap/capability-matrix.md`; the M8.b row of `docs/ROADMAP.md` is the
scope statement this proposal implements.

### Modified Capabilities

None yet. The spike is expected to produce at least one: a semantic the shared IR must carry that no
current specification states, found by the consumer that needs it.

## Impact

- **New code**: `src/gameplay/` grown from one module to a framework, and new modules for the graph
  IR, abilities, AI, navigation, animation, sequencing, VFX, the interface and 2D rendering. Nine
  directories that do not exist today.
- **Closing artefact**: `samples/08-vertical-slice` — a playable game: a level, characters that
  animate and think, abilities with effects, a cinematic, a heads-up interface, effects, sound, and a
  2D menu.
- **Risk**: the shared graph IR across seven consumers, and it now carries **its own failure
  budget** rather than blocking a milestone that could close without it. `design.md` §1 states the
  budget, what spends it, and what happens when it is exhausted.

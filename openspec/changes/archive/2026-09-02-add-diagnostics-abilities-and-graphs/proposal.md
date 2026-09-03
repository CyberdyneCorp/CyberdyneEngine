# Diagnostics, abilities, and visual scripting

## Why

Three capabilities, each of which the engine has been building toward and two of which it explicitly
promised.

**Diagnostics has thirty-nine implementations.** Almost every capability specified so far ends with a
diagnostics requirement: the job system reports steal counts and critical paths, memory reports
per-domain attribution, the renderer reports pass timings and breadcrumbs, residency answers *why is
this not resident*, the world answers *why is this cell loaded*, illumination answers *why is this
area dark*, procedural generation answers *why is this tree here*. Every one of those is right, and
none of them share a transport, a buffer, a capture artefact, or a viewer. Thirty-nine subsystems
about to invent thirty-nine tracing formats is the definition of a missing foundation.

**Abilities were promised.** `gameplay-framework` states that abilities, attributes, inventories,
quests, and objectives are "optional modules built on this one, not parts of it, so that a game does
not pay for the ability system whether or not it has abilities". That was the right boundary, and the
module was left unspecified.

**Visual scripting was deferred with a seam.** The same requirement says gameplay visual scripting is
deferred, and that commands, events, tags, rules, and validation are reflected schemas "so that a
future gameplay graph can compile to a system rather than being interpreted per entity, and that seam
SHALL be preserved". Five capabilities have since built their own graph compilers — materials, VFX,
animation, AI, camera rigs — each proving the pattern and each building its own editor, serialization,
diffing, and identity handling.

The three contracts:

> **Diagnostics are infrastructure, not an editor feature.** Every subsystem emits into one
> low-overhead trace, and the profiler, the crash artefact, and the reproduction all read it.

> **Abilities are compiled shared programs plus compact state.** Ten thousand units with ability sets
> are ten thousand small state records, not ten thousand ability objects.

> **Visual graphs are an authoring language.** Shipping execution runs compiled typed programs, and
> there is never one interpreted graph instance per entity.

## What changes

**`diagnostics-profiling-and-crash`** — one trace schema and transport with compiled event
identifiers and per-thread buffers, so no subsystem invents its own; declared channel priorities and
a loss policy, because telemetry must never destabilise a frame; the profiler views the engine has
already asked for — task graphs and critical paths, ECS archetype behaviour, memory by domain, GPU
passes and residency, input latency, simulation ticks and hashes; **structured logging** with typed
fields rather than formatted strings; a **rolling diagnostic buffer** frozen on a hitch or a fault, so
a profiler need not be running before the problem; **crash artefacts** carrying build identity,
breadcrumbs, GPU state on device loss, and module offsets that symbolicate without symbols on the
player's machine; **privacy classification** on every captured field; remote and dedicated-server
diagnostics that do not require the editor; and the link to the crash replay buffer that
`replay-and-rollback` already specified, turning a bug report into a reproduction.

**`gameplay-abilities-and-effects`** — the promised module. Ability definitions compiled to programs
shared by every owner; compact per-owner state with cooldowns as tick values rather than float timers;
attributes as ECS-native typed data with a **specified modifier order** and declared stacking policies;
effects as compact instances scheduled on simulation ticks; targeting as first-class serialisable
data distinct from target validation; validation returning structured reasons through the one path the
interface, AI, and authority share; a declared **prediction policy** per ability with activation
identity for reconciliation; deterministic execution with derived random streams; gameplay cues as
presentation signals on the correct side of the firewall; and bulk activation, because a strategy
game's abilities arrive ten thousand at a time.

**`visual-scripting`** — CyberGraph as **shared graph infrastructure with domain-specific lowering**.
The honest boundary: materials, VFX, animation, AI, and camera rigs keep their own intermediate
representations, because a material's algebra and a gameplay event's control flow are not one language;
what they share is nodes, pins, typed connections, stable identity, serialization, diffing, subgraphs,
and debugging. On top of that, gameplay and ability graph frontends compile to ECS systems and ability
programs. Typed pins rather than a universal variant; events rather than tick as the default;
compiled programs with per-entity state as generated data; a portable bytecode for iteration and a
native path for shipping; **compiler-enforced determinism auditing**, which a graph compiler can do
far more thoroughly than review of handwritten code; capability restriction for mods; and semantic
diff and merge, which is where visual scripting usually fails a team and where being better is
achievable.

## Impact

- **New**: `diagnostics-profiling-and-crash`, `gameplay-abilities-and-effects`, `visual-scripting`
- **Modified**: `gameplay-framework` (the deferred modules and the scripting seam are now filled),
  `editor-architecture` (the editor becomes a client of the diagnostics backend, and graph editors
  share the graph infrastructure), `core-jobs-and-concurrency` and `core-memory-and-containers`
  (their diagnostics emit into the shared trace), `rhi-and-render-graph` (GPU breadcrumbs feed the
  crash artefact), `thirdparty-dependencies`
- **Remaining production gaps**, unchanged by this: sequencing, destruction, vehicles and movement,
  hydrology, localisation and accessibility, online services, and modding

# Design: CyberGameplay

## 1. Take Unreal's concepts, refuse Unreal's shape

The conceptual separation Unreal established is the most valuable thing in any of the three
reference engines: a session that outlives a level, rules distinct from replicated state, player
identity distinct from the thing being driven, and possession as an explicit relationship. Those
distinctions are hard-won and this specification keeps all of them.

What it does not keep is the delivery. Every one of those concepts arrives as an Actor, which for
this engine would mean a second object hierarchy running alongside the ECS, with its own lifetime,
its own replication path, and its own update model. The concepts are sound; the container is wrong.

So each becomes data or a service: rules are a composable asset, session state is a set of reflected
fragments, participants and teams are registry entries, and control is a binding between a source
and a set of entities. Nothing gains a base class.

## 2. Six things that are routinely conflated

Most gameplay framework bugs come from treating these as one:

| Concept | Question it answers |
|---|---|
| **Participant** | Who is taking part — human, bot, spectator, server agent |
| **Player** | A participant's session-level identity, score, and progression |
| **Local player** | A human at *this* machine, with a viewport, input device, and listener |
| **Controlled entity** | What is being driven right now |
| **Owner** | Whose it is |
| **Network authority** | Who is allowed to change its authoritative state |

A captured turret makes the point: an enemy faction **owns** the installation, a player
**controls** the turret, the server has **authority**, and the player's identity survives all of it.
Collapsing any pair of these is how a game acquires a class of bug that only appears in multiplayer.

## 3. One command stream, five producers

This is the decision with the widest consequences and the clearest differentiation.

A human presses a key; an input action becomes a **gameplay command** — a semantic intent, not a key
event. An AI agent decides to attack and emits the same command type. A network peer's command
arrives over the wire. A replay plays commands back. An automated test injects them.

Downstream, the simulation cannot tell them apart, and that single property gives, for free: replay
that records intent rather than state, network prediction over a well-defined channel, AI that is
exercised by the same validation as players, deterministic tests without a renderer, and input
rebinding that cannot break gameplay logic.

The alternative — each source calling gameplay functions directly — is what makes replay a
retrofit and AI a special case, and it is the default outcome if the command layer is not specified
first.

**The boundary with RPCs has to be drawn explicitly**, or teams will use both for the same thing:
gameplay commands are the client-to-server *gameplay intent* channel, validated and predictable;
remote procedure calls carry everything that is not gameplay intent — chat, session control,
notifications. Intent that arrives by RPC loses prediction, replay, and validation, so the line is
stated as a requirement rather than left to taste.

## 4. Control is many-to-many and channelled

One controller possessing one pawn describes a first-person game and nothing else. A strategy player
controls one hero, two hundred selected units, and a construction interface simultaneously. A tank
has a driver and a gunner. An AI assists a human's aim while the human steers.

So a **control binding** connects a control source to a set of entities on a named **channel** —
movement, weapons, camera, turret, command. Multiple bindings coexist; the set is addressed as a
group rather than by copying thousands of identifiers into every command.

This is the place where the object-oriented frameworks are structurally, not incidentally, unsuited,
and it is worth being explicit that the improvement is architectural rather than a matter of
convenience.

## 5. Behaviours must compile, or the Unity comparison is empty

Attaching a script to an object is the most intuitive authoring model there is, and the engine
should support it. The cost in the reference implementations is one heap object and one virtual call
per entity per frame, which is exactly what the ECS exists to avoid.

The resolution is a compiler, not a concession: a behaviour whose callbacks read and write declared
component data lowers into a **generated system** iterating chunks, and a hundred thousand trees
with a burnable behaviour cost one system rather than a hundred thousand callbacks.

The honest part is the limit. A behaviour that calls arbitrary script per entity per tick, captures
unbounded per-instance state, or reaches outside its declared access **cannot** be batched. Rather
than implying otherwise, the compiler **reports which behaviours batched and which did not, and
why** — so a developer discovers the cost at build time instead of at ten thousand instances.

This replaces the current guidance in `scene-graph-and-nodes`, which concedes that behaviours do not
scale and directs developers to write systems by hand. The concession becomes a compilation
strategy with a stated boundary.

## 6. Validation returns reasons

`bool canBuild()` forces every consumer to reimplement the reasoning: the UI greys out a button and
invents its own explanation, the AI re-derives the conditions to decide what to try, the server
rejects with a generic failure, and the three drift.

So validation returns a structured result — allowed or not, with tagged reasons and the data behind
them. One implementation then serves the UI's explanation, the AI's planning, the server's
rejection, and the test's assertion, and they cannot disagree because there is one of them.

## 7. Tags are compiled integers, and they are not ECS tags

Gameplay tags are hierarchical and semantic: `Unit.Robot.Harvester`, `State.Burning`,
`Damage.Fire`. They are authored as text, cooked to identifiers, and compared as integers, with
hierarchy queries resolved through compact metadata. Strings at runtime are forbidden.

They are **not** a replacement for ECS tag components, and the distinction matters for performance:
ECS tags are structural — they change an entity's archetype and make queries cheap — while gameplay
tags are dynamic state carried in a set. Using gameplay tags where an archetype query belongs turns
an O(1) query into a scan, and the specification says so because the mistake is easy and the
diagnosis is not.

## 8. Time is domains, and the clock is a tick

There is no single delta time. Gameplay, user interface, cinematics, and real time advance
independently, so pausing gameplay while menus animate is configuration rather than a special case,
and slow motion does not slow the interface.

Scheduled gameplay uses **simulation ticks**, not wall-clock timestamps. "At tick 8842" is
reproducible across machines and replays; "in 3.0 seconds" is not. This is the same rule the AI
budget and the task scheduler already follow — simulation must not depend on measured time — applied
where designers will most often be tempted to break it.

## 9. Death is not destruction

A unit reaching zero health enters a state; it may animate, leave a wreck, be revivable, and be
removed much later or never. An entity leaving the world because its cell streamed out is not dead
at all.

Conflating gameplay death with entity destruction is a mistake that surfaces as units vanishing
mid-animation and as save files that resurrect the dead. So the specification separates them and
requires a structured **despawn reason** — destroyed, streamed out, session ended, replaced,
scripted — because systems genuinely need to distinguish these.

## 10. Game modes compose, they do not inherit

`TerraformSkirmishMode` deriving from `RTSMode` deriving from `BaseMode` is where gameplay code goes
to become unmaintainable, and every engine that offers the base class gets the hierarchy.

Rules are therefore composable pieces — spawn, victory, teams, economy, respawn, time — assembled in
a **rules asset**, and **gameplay features** package the systems, components, tags, input contexts,
assets, and interface that a mode needs. A skirmish mode is a composition, not a subclass, and a
designer can produce one without writing C++.

## 11. Headless is a requirement, not a configuration

Gameplay must run with no renderer, no audio, no interface, and no GPU — because the dedicated
server needs it, the test harness needs it, and continuous integration needs to run thousands of
scenarios in parallel.

Stating this as a requirement rather than an aspiration has a specific effect: it forbids the
convenience that would break it. A gameplay system that reaches for a camera, a viewport, or a
material fails in the one configuration that matters most for testing, and the boundary is easier to
hold from the first day than to recover later.

## 12. Performance contracts, with numbers

Architectural claims that cannot fail are not claims. So the specification carries figures — a
hundred thousand active gameplay entities, batch spawning ten thousand without ten thousand
allocations, indexed ownership and team queries, integer tag tests, no virtual dispatch in primary
loops — and an acceptance scenario that is a strategy game frame rather than a third-person
character, because the character case is where object frameworks look fine.

## 13. Explicitly not in this capability

Abilities, attributes, inventories, quests, and objectives are **optional modules built on this
one**, not parts of it. Putting them in the core would mean every game pays for the ability system
whether or not it has abilities, and would fix in engine code decisions that are genuinely
game-specific.

Gameplay visual scripting is deferred. The seam is that commands, events, tags, and validation are
all reflected schemas, so a future graph can compile to a system rather than being interpreted per
entity — which is the only version of it worth building.

## 14. Build order

| Phase | Contents |
|---|---|
| 1 | Application, game instance, scoped services, session, world session |
| 2 | Participants, players, local players, teams, affiliations, ownership |
| 3 | Control sources, bindings, gameplay commands, routing and validation |
| 4 | Rules, session state, phases |
| 5 | Spawning with batch and reservation; ownership and team indexes |
| 6 | Gameplay tags, typed events, messages |
| 7 | Simulation clock, time domains, timers, random streams |
| 8 | Interaction; gameplay features |
| 9 | Network integration: prediction hooks, authority validation |
| 10 | Swift APIs; behaviour batching and its compiler report |
| 11 | Editor gameplay debugger, command timeline, rule debugger |
| 12 | Stress and headless benchmarks; replay and determinism validation |

**Phase 3 is the milestone that matters.** Once the command stream exists and human, AI, network,
and replay all flow through it, everything after is addition. If it is retrofitted, replay and
prediction become special cases and never fully recover.

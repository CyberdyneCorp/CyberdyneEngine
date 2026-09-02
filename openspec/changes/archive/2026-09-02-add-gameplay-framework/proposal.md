# Gameplay framework

## Why

The engine can render, simulate, stream, author, and ship. It has no answer to the question a team
asks on day one: *how is a game structured?* Where does a match live, what is a player as distinct
from the thing they are driving, who owns a unit, who is allowed to give it an order, and what
happens to all of that when the world streams or the level changes.

Every engine answers this, and the three obvious references answer it in ways that would each cost
this engine something specific:

**Unreal's conceptual model is the best of the three and its implementation is the wrong shape for
us.** The separation of session lifetime, rules from replicated state, player identity from the
pawn, and controller possession is genuinely well-factored — and it is delivered as Actors. Adopting
it literally would mean a parallel object hierarchy alongside the ECS, and the possession model is
one controller to one pawn, which does not describe a player commanding two hundred units.

**Unity's model is ergonomic and costs one object and one virtual update per behaviour.** The
authoring convenience is real and worth keeping; the execution model is precisely what this engine's
archetype storage, declared-access scheduling, and chunk iteration exist to avoid.

**Godot's scene tree is excellent for authoring and is also its simulation architecture.** This
engine already separated those: the node tree is a façade over ECS storage. The gameplay framework
must not quietly reintroduce tree traversal as the update mechanism.

So the contract:

> **Gameplay concepts are roles and services layered over ECS data, not a base class every game
> object inherits from.**

And the two that follow from it:

> **Human players, AI agents, network peers, replay playback, and automated tests all produce the
> same semantic command stream, and the simulation cannot tell them apart.**

> **Authoring convenience must never require one gameplay object, one script object, one virtual
> update call, or one controller per simulated entity.**

## What changes

**`gameplay-framework`** — a new capability covering: the lifetime model (application, game
instance, session, world session) with **scoped services** instead of global managers; game rules as
composable data separate from replicated session state; participants, players, local players, teams,
and **affiliation relationships** that are not derived from team inequality; **ownership, control,
and network authority as three separate concepts**; control bindings that are many-to-many and
channelled, so one player commands an army and two players share a tank; **gameplay commands** as
the one semantic intent stream, with validation that returns reasons rather than booleans; compiled
hierarchical **gameplay tags**; typed events and messages with phase-buffered delivery; **time
domains** and a simulation tick as the gameplay clock; deterministic random streams; spawning with
batch and reservation; **gameplay features** that compose game modes instead of inheritance;
interaction; a headless requirement; performance contracts with numbers; and an explicit
**forbidden patterns** requirement naming the architectures this specification exists to prevent.

**`scene-graph-and-nodes`** gains the part that makes the Unity comparison a real claim rather than
an aspiration: **behaviours compile into generated systems** where their callbacks are batchable,
with the compiler reporting which behaviours batched and which did not — because a behaviour that
calls arbitrary script per entity per tick genuinely cannot be batched, and saying so is more useful
than implying it can.

**`networking-and-replication`** gains a boundary it needs: gameplay commands are the client-to-
server **gameplay intent** channel, and remote procedure calls are for everything that is not
gameplay intent. Without that line, teams use both for the same thing and lose prediction, replay,
and validation on whichever half went through RPCs.

**`core-platform-abstraction`**, **`ai-system`**, and **`testing-and-quality`** are updated so that
input actions become intents rather than reaching gameplay raw, AI emits commands through the same
path as a human, and the acceptance scenarios — a hundred thousand unit RTS frame, a four-player
vehicle handover, a headless server — are benchmarks rather than ambitions.

## Impact

- **New**: `gameplay-framework`
- **Modified**: `scene-graph-and-nodes`, `networking-and-replication`, `core-platform-abstraction`,
  `ai-system`, `testing-and-quality`, `thirdparty-dependencies`
- **Deliberately out of scope**: abilities, attributes, inventory, quests, and objectives, which are
  optional modules built on this rather than parts of it; and gameplay visual scripting, whose seams
  are reserved

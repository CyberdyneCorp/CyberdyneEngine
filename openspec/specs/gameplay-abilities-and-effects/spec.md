# gameplay-abilities-and-effects Specification

## Purpose

Defines **CyberAbilities**: the optional module `gameplay-framework` promised when it excluded
abilities so that a game without them pays nothing.

The runtime shape is the engine's usual answer, applied again: a **compiled program shared by every
owner** plus compact per-owner state. Ten thousand units with ability sets are ten thousand small
records — an identity, a cooldown tick, a charge count — processed by systems over archetypes, not
ten thousand ability objects with virtual activation.

Two details decide whether an ability system is usable rather than merely present. **Modifier order
is specified by the engine, not conventional** — additive, multiplicative, override, clamp, with
stable tie-breaks — because every game with attributes eventually has two modifiers whose result
depends on order, and a convention produces two answers on two machines. And **stacking is a declared
policy** rather than something each effect implements: poison stacking to five and refreshing its
duration is the same problem in every game that has poison.

Cooldowns and effect periods are **ticks**, not float timers, so they are exact, reproducible and
rollback-safe. Activations carry identity, which is what makes prediction reconciliation exact rather
than heuristic. And cues are presentation: they carry their simulation point so the side-effect ledger
suppresses them on re-simulation, and they never influence authoritative state.

## Requirements

### Requirement: Abilities are an optional module
Abilities, effects, and attributes SHALL be an **optional gameplay module** built on
`gameplay-framework`, activatable as a gameplay feature.

A project that does not use abilities SHALL link none of this capability's code and SHALL carry none
of its data.

This capability SHALL build on the framework's existing mechanisms — commands, validation, tags,
time domains and ticks, ownership and control, determinism, and the persistence and replay contracts
— rather than defining parallel ones.

#### Scenario: A game without abilities pays nothing
- **WHEN** a project does not enable the ability module
- **THEN** no ability code, data, or per-entity state SHALL be present in its build

#### Scenario: No parallel mechanisms
- **WHEN** an ability is activated
- **THEN** it SHALL arrive as a gameplay command and be validated through the framework's validation
  path

### Requirement: Compiled ability programs
An ability SHALL be authored as a **definition asset** and compiled into an **ability program**
shared by every owner that has it.

Compilation SHALL resolve: tag requirements into compact queries, attribute references into bindings,
constant expressions, cost and cooldown descriptions, targeting configuration, and the effects
applied — producing a program executed without graph traversal or reflection at activation time.

There SHALL NOT be one polymorphic runtime object per ability per owner.

#### Scenario: Ten thousand owners, one program
- **WHEN** ten thousand units share an ability
- **THEN** one compiled program SHALL exist and per-owner data SHALL be compact state

#### Scenario: Activation performs no lookup by name
- **WHEN** an ability activates
- **THEN** its requirements, attributes, and effects SHALL be reached through compiled bindings

### Requirement: Ability state and sets
Per-owner ability state SHALL be **compact and ECS-native**: an ability identity, cooldown state,
charges, and any small declared state — stored so that many owners are processed by systems over
archetypes.

Abilities SHALL be organised into **ability sets** shared by templates, so that a unit type's
abilities are referenced rather than duplicated per instance.

Abilities SHALL be **grantable and revocable** at runtime — by equipment, upgrades, effects,
features, or scripts — through grant records, so that the source of a granted ability is known and
removal is exact.

#### Scenario: Sets are shared
- **WHEN** a thousand units of one type exist
- **THEN** they SHALL reference one ability set, not carry a copy each

#### Scenario: A granted ability is removed exactly
- **WHEN** the equipment that granted an ability is removed
- **THEN** that grant SHALL be withdrawn without affecting an ability granted by another source

### Requirement: Attributes
Attributes SHALL be **typed ECS data** with declared metadata — base value, clamps, replication,
persistence, prediction behaviour, and presentation information — not entries in a string-keyed
dictionary.

Attribute access in hot paths SHALL be through compiled bindings; string lookup SHALL NOT occur per
access.

Attribute sets SHALL be composable per entity type, so that a unit carries the attributes it has and
nothing more.

Attributes SHALL participate in the state classification defined in `simulation-and-determinism`, so
that hashing, snapshotting, saving, and replication follow from one declaration.

#### Scenario: Attributes are components
- **WHEN** a system iterates entities with health
- **THEN** it SHALL iterate component data, with no attribute dictionary lookup

#### Scenario: One declaration, several behaviours
- **WHEN** an attribute is declared replicated and persistent
- **THEN** replication, saving, and hashing SHALL follow from that declaration

### Requirement: Modifiers and evaluation order
Attribute modification SHALL support at minimum: add, multiply, override, clamp minimum, clamp
maximum, and a declared custom operation.

**The evaluation order SHALL be specified by the engine, not conventional**: modifiers apply in a
defined sequence — additive, then multiplicative, then override, then clamping — with equal-priority
modifiers ordered by a stable tie-break.

Order SHALL NOT depend on insertion order, container iteration order, or the order effects happened
to be applied, since those differ between machines and between runs.

The final value and every contributing modifier SHALL be inspectable.

#### Scenario: The same result everywhere
- **WHEN** two modifiers of different kinds affect one attribute
- **THEN** the result SHALL follow the specified order and be identical on every peer

#### Scenario: Contributions are visible
- **WHEN** an attribute's value is inspected
- **THEN** its base and each modifier's contribution SHALL be listed

### Requirement: Effects
Effects SHALL be authored as definitions and compiled, with runtime instances stored as **compact
records** — effect identity, source, start and end tick, and stack count — not as heap-allocated
objects.

Effect kinds SHALL include instant, duration, infinite, and periodic.

Periodic effects SHALL be scheduled on **simulation ticks**, not on accumulated floating-point time,
so that periods are exact, reproducible, and rollback-safe.

Effect application SHALL follow a defined pipeline: immunity and requirement checks, stacking
resolution, modifier application, attribute change, and event emission.

#### Scenario: A burn ticks exactly
- **WHEN** a periodic effect applies every thirty ticks for three hundred
- **THEN** it SHALL apply exactly ten times, at exact ticks, and survive rollback correctly

#### Scenario: Effects are not objects
- **WHEN** fifty thousand effects are active across a battle
- **THEN** they SHALL be compact records processed in bulk

### Requirement: Stacking policy
Every effect SHALL declare a **stacking policy**: stack, refresh duration, replace, keep highest,
keep lowest, unique by source, or limited stacks with a declared maximum.

Stacking SHALL be resolved by the engine according to that declaration. Games SHALL NOT be required
to implement stacking behaviour per effect.

Where a limit is reached, the declared behaviour on overflow SHALL apply, and the outcome SHALL be
reportable.

#### Scenario: Poison stacks and refreshes
- **WHEN** a poison effect declares five maximum stacks with duration refresh and is reapplied
- **THEN** the stack count SHALL rise to its limit and duration SHALL refresh, with no per-effect
  implementation

### Requirement: Costs, cooldowns, and charges
Costs SHALL be declared and MAY consume attributes, charges, or a project resource service.

Cost handling SHALL be **transactional**: validate, reserve, and commit — so that two activations
resolved in the same tick cannot both spend the same resource.

**Cooldowns SHALL be expressed as ticks**, as a ready-tick value rather than a counting float timer,
and MAY be shared across abilities through a cooldown group identified by a gameplay tag.

Charges SHALL be supported with independent recharge, and their state SHALL participate in
snapshots and saves like other ability state.

#### Scenario: Resources cannot be double-spent
- **WHEN** two activations in one tick would each consume the last of a resource
- **THEN** the transactional path SHALL permit one and reject the other with a structured reason

#### Scenario: Cooldowns are exact
- **WHEN** a cooldown of ninety ticks is applied
- **THEN** readiness SHALL be a tick value, exact under rollback and identical across peers

### Requirement: Targeting
Targeting SHALL be first-class, with **target data** that is typed, serialisable, and networkable,
covering: self, an entity, an entity set, a point, a direction, an area, a cone, a line, a volume,
and a world region.

**Target acquisition and target validation SHALL be distinct.** Acquisition selects candidates —
explicitly, from a cursor, from an aim ray, by proximity, by area query, by chaining, or by an agent's
choice; validation checks range, line of sight, relationship, required and forbidden tags,
reachability, and resource availability.

Validation SHALL be compiled where the rules are static, and SHALL run identically for the interface,
artificial intelligence, and the authority.

#### Scenario: Aim is data, not a camera
- **WHEN** a targeted ability is activated
- **THEN** its target data SHALL travel in the command and be validated by the authority, and the
  camera SHALL not be authoritative

#### Scenario: Acquisition and validation are separable
- **WHEN** an agent selects a target by proximity
- **THEN** it SHALL then validate it through the same rules a player's target would face

### Requirement: Activation pipeline and structured validation
Activation SHALL proceed through a defined pipeline: resolve owner and context, check state and tag
requirements, check cost, check cooldown, resolve and validate the target, apply the prediction and
authority policy, commit the activation, apply effects, and emit cues and events.

Validation SHALL return a **structured result** — permitted or not, with tagged reasons and their
data — through the same path the interface, artificial intelligence, and the authority use, as
`gameplay-framework` requires.

Validation SHALL be callable **without activating**, so the interface can explain why an ability is
unavailable and an agent can decide what to attempt.

#### Scenario: The interface explains itself
- **WHEN** an ability is unavailable
- **THEN** the reason and its data SHALL come from the same validation the server would apply

#### Scenario: Rejection is diagnosable
- **WHEN** an activation is rejected
- **THEN** the reason tag and the values that produced it SHALL be reportable

### Requirement: Activation identity and prediction
Every activation SHALL carry a **stable activation identity**, used for reconciliation with the
authority, for suppressing duplicated cues through the side-effect ledger, for rollback, and for
network debugging.

Each ability SHALL declare a **prediction policy**: none, client-predicted, authority-only, or
deterministic lockstep.

A client-predicted activation SHALL: validate locally, apply predicted state, send the command, and
reconcile against the authority's response — correcting predicted effects that were not confirmed.

**Prediction SHALL NOT bypass authoritative validation.** A predicted activation the authority
rejects SHALL be rolled back and the rejection reported.

#### Scenario: A rejected prediction is undone
- **WHEN** a predicted activation is rejected by the authority
- **THEN** its predicted effects SHALL be reverted and the rejection SHALL be reportable

#### Scenario: Two similar activations are distinguishable
- **WHEN** the same ability is activated twice in quick succession
- **THEN** activation identity SHALL determine which the authority's response refers to

### Requirement: Deterministic execution
Ability and effect execution SHALL be deterministic to the level the session's determinism profile
requires.

Randomness SHALL come from a **stream derived from activation identity, ability identity, and the
session seed**, so that a critical hit or a random duration is reproducible in replay.

Target ordering, modifier ordering, effect stack ordering, and effect application ordering SHALL all
have declared stable tie-breaks, and SHALL NOT depend on container iteration order.

All authoritative ability and effect state SHALL participate in snapshotting, hashing, and rollback.

#### Scenario: A critical hit replays
- **WHEN** a session is replayed
- **THEN** the same activations SHALL produce the same random outcomes

#### Scenario: Ordering is stable
- **WHEN** several effects resolve in one tick
- **THEN** their order SHALL follow declared tie-breaks and be identical on every peer

### Requirement: Async ability behaviour
Abilities SHALL support waiting: for a number of ticks, for an animation marker, for a gameplay
event, for target confirmation, or for an authority response.

Waiting SHALL be compiled into an explicit **state machine with compact state** — a program position
and small data — rather than allocating a coroutine or closure per activation where that can be
avoided.

Abilities SHALL support **cancellation** with declared causes: by the owner, by a tag, by damage, by
movement, or not at all; and cancellation SHALL emit a structured reason.

Waiting state SHALL participate in snapshots so that a rollback restores an in-progress activation
correctly.

#### Scenario: A channelled ability rolls back correctly
- **WHEN** a rollback restores a tick during a channelled ability
- **THEN** its wait state SHALL be restored and resume correctly

#### Scenario: Interruption is explained
- **WHEN** an ability is cancelled by damage
- **THEN** the cancellation reason SHALL be structured and reportable

### Requirement: Gameplay cues
Presentation responses to abilities and effects SHALL be **cues**: tagged signals consumed by
effects, audio, animation, camera, and interface.

Ability logic SHALL emit cues; it SHALL NOT call presentation systems directly.

Cues are **presentation** and sit on the presentation side of the determinism firewall. A cue SHALL
NOT influence authoritative state.

Cues SHALL carry their simulation point so the side-effect ledger can suppress duplicates during
re-simulation, and SHALL declare whether they may be realised speculatively or only when confirmed.

#### Scenario: A rolled-back cast does not play twice
- **WHEN** an activation is re-simulated after a rollback
- **THEN** its cues SHALL be suppressed by the ledger rather than replayed

#### Scenario: Presentation cannot change the outcome
- **WHEN** a cue's effect is disabled for performance
- **THEN** ability outcomes SHALL be unchanged

### Requirement: Bulk activation and evaluation
Activation, effect application, and attribute evaluation SHALL be **batchable**: many owners
activating the same ability, and an area effect applying to many targets, SHALL be processed in bulk
rather than one call at a time.

There SHALL be no heap allocation per activation or per effect application on the normal path.

Systems SHALL process owners grouped by program and archetype, so that a strategy game's ten
thousand simultaneous orders are a loop rather than ten thousand dispatches.

#### Scenario: An area effect hits a thousand targets
- **WHEN** an effect applies to a thousand entities
- **THEN** the target set SHALL be resolved once and the effect applied in bulk

#### Scenario: Mass orders are a loop
- **WHEN** ten thousand units activate the same ability in one tick
- **THEN** they SHALL be processed as a batch over their shared program

### Requirement: Artificial intelligence integration
Agents SHALL use the **same abilities, the same validation, and the same commands** as players. A
separate agent-only ability path SHALL NOT exist.

Abilities MAY declare optional **planning metadata** — range, expected effect magnitude, resource
cost, and role tags — so that an agent can reason about them without hard-coded knowledge.

Agents SHALL be able to enumerate usable abilities and validate targets without activating, using the
validation path described above.

#### Scenario: An agent and a player are indistinguishable
- **WHEN** an agent uses an ability
- **THEN** it SHALL emit the same command and face the same validation as a player

#### Scenario: Planning without hard-coding
- **WHEN** an agent evaluates its options
- **THEN** it SHALL read declared metadata rather than embedding knowledge of specific abilities

### Requirement: Persistence and replay
Ability and effect state SHALL declare persistence classification: cooldowns, charges, active
effects, and attribute values SHALL each be classified as session transient, save-game persistent, or
derived.

A replay SHALL reconstruct abilities from recorded commands and the deterministic execution above,
without recording per-activation results where determinism suffices.

Where an activation's outcome depends on something not reproducible, it SHALL be recorded as an
external result.

#### Scenario: Saving mid-fight
- **WHEN** a game is saved with cooldowns running and effects active
- **THEN** those declared persistent SHALL be restored exactly

#### Scenario: Replay does not record every hit
- **WHEN** a session replays
- **THEN** activations SHALL be reconstructed from commands and streams, not from recorded results

### Requirement: Ability diagnostics
The engine SHALL provide inspection showing, per entity: granted abilities with their sources,
cooldown and charge state, active effects with stacks and remaining duration, attribute values with
base and each modifier's contribution, and applicable tag state.

An **activation timeline** SHALL show activations by tick with their validation results, costs
committed, effects applied, and cues emitted.

Rejections SHALL be shown with their structured reason and the values that produced it, and
prediction mismatches SHALL be shown with the predicted and authoritative outcome.

#### Scenario: Why did this fail
- **WHEN** an ability is rejected
- **THEN** the debugger SHALL show the reason tag and the values compared

#### Scenario: Prediction divergence is visible
- **WHEN** a prediction is corrected
- **THEN** the debugger SHALL show what was predicted and what the authority returned

### Requirement: Ability performance
The module SHALL support at architectural scale: a hundred thousand entities carrying ability sets,
large numbers of concurrent effect instances, and ten thousand activations per second, without
per-activation allocation and without a central lock.

Attribute evaluation SHALL be structured so that an entity with many modifiers is not evaluated by
walking a linked structure per access.

Costs SHALL be reported per system so that ability overhead is attributable.

#### Scenario: Scale is not a special case
- **WHEN** a strategy battle activates abilities at scale
- **THEN** ability processing SHALL remain a reported, bounded fraction of simulation time

### Requirement: Forbidden ability patterns
The following SHALL NOT appear, and each SHALL be checkable:

- One polymorphic runtime object per ability per owner
- Heap-allocated effect instances on the normal path
- Attribute access by string lookup in a hot path
- Modifier or effect ordering determined by container iteration order
- Cooldowns and effect durations as accumulated floating-point timers
- Gameplay cues influencing authoritative state
- Client prediction bypassing authoritative validation
- A separate agent-only ability implementation
- Ability state excluded from snapshotting while affecting authoritative outcomes

#### Scenario: A proposal is checked
- **WHEN** a change would allocate an effect object per application
- **THEN** it SHALL be flagged against this requirement

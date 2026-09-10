# `src/gameplay/abilities/` — CyberAbilities

Layer 4. Attributes and modifiers, effects and stacking, ability sets and grants, costs and
cooldowns, targeting, and the activation pipeline. M8.b section 4; the governing specification is
`gameplay-abilities-and-effects`.

## A separate target, because "pays nothing" is structural or it is nothing

The specification's first requirement:

> A project that does not use abilities SHALL link **none** of this capability's code and SHALL
> carry **none** of its data.

`cy::gameplay` does not depend on this module. A dedicated server, a headless test, or a game with
no abilities links no attribute store, no effect array and no activation pipeline — not because
somebody remembered to exclude them, but because nothing names the target. The same argument
`src/gameplay/CMakeLists.txt` makes about `cy::servers-input`, from the other side.

## Where the compiled program comes from

M8.b's spike found that abilities and visual scripting are **one language serving two consumers** —
`visual-scripting` says so itself: it "SHALL additionally provide gameplay and **ability** graph
languages, lowering to ECS systems and **ability programs** respectively". So the compiler is
`cy::graph::script::compile_ability`, which produces a `ScriptProgram` plus the pipeline's stage
table, and this module holds what that program operates on.

`AbilityDefinition::program` is a pointer to a program somebody else compiled, and it may be null:
an ability whose whole content is "cost, cooldown, effects" needs no graph, and making one mandatory
would put a compiler in the path of the simplest thing the module does. `AbilityScriptHost` is the
only place a program's external names are resolved — `ability.attribute`, `ability.has_tag`,
`ability.ready`, `ability.charges`, `ability.tick` — which is what makes the graph's capability
audit an enforceable statement rather than documentation.

## The four decisions worth knowing

**The modifier order is stated once, in full, in `attributes.h`.** Base, then every Add, then every
Multiply, then every Custom, then the highest-priority Override, then the clamps, then the
attribute's own declared range. The tie-break within a class is `(priority descending, source
ordinal ascending, modifier identity ascending)` — three declared values, none of them a container
position. `add_modifier` inserts into that order rather than appending, and
`test_attributes.cpp`'s "insertion order does not decide the answer" applies the same four modifiers
in two orders on two stores and asserts the two answers are equal. Without that case the requirement
is a comment.

**Ticks, never floats.** A cooldown is a ready-tick value; an effect's period is
`next_period_tick += period_ticks`. `test_effects.cpp` asserts that a periodic effect applying every
thirty ticks for three hundred applies exactly ten times, at exactly those ticks, and that a
rollback re-runs the same periods — which it does because the comparison is an integer one.

**An effect instance is a trivially copyable record.** The live set is one array of them, so a
snapshot is `Span<const EffectInstance>` and a rollback is `restore()`. Fifty thousand active
effects are one allocation. The forbidden-patterns list names "Heap-allocated effect instances on
the normal path"; this shape makes it structurally impossible rather than merely avoided.

**A cue carries its simulation point, and the ledger suppresses the repeat.** A re-simulation
replays a recorded command *with its recorded activation identity* — `ActivationRequest::identity` —
and the ledger recognises the `(activation, cue, simulation point)` triple it already emitted. That
is the requirement's "a rolled-back cast does not play twice", and it is why the identity is a
parameter rather than always freshly derived.

## What is measured

`ability_scale` is `integration` and holds the milestone's exit criterion for this module.

| Property | Declared | Measured on the reference machine |
|---|---|---|
| A hundred concurrent gameplay effects, per simulation tick | under 10 µs | ~0.2–0.5 µs |
| Four times the effects for at most eight times the cost | — | ~3–6x |
| Ten thousand activations over one shared ability | no allocation per activation | 0 allocations, ~2.6 ms |

The linearity case is what actually catches a quadratic; a microsecond bound alone would only catch
one on a slow machine. And "no allocation per activation" is `BatchReport::allocations`, a number the
batch computes from its own capacities, rather than a claim.

**These are gameplay effects** — damage over time, stacking modifiers, expiring buffs. Particles are
`vfx-system`'s and were deferred to M8.c.

## What is absent

* **The interface is not wired.** `ValidationResult` is the framework's own, so an interface that
  greys out a button reads exactly what the authority would reject with — but nothing draws it.
* **Cost kinds beyond attributes and charges are declared and not implemented.** `CostKind::Resource`
  names a project resource service; the ledger reports the requirement and the host answers it,
  because a resource service is game code.
* **`Custom` modifier operations are resolved per store.** `register_custom` is a per-`AttributeStore`
  table, so two stores in one process may disagree about what `halve` means. A project-wide registry
  is the right shape and nothing needed it yet.
* **`validate()` allocates for a graph-backed ability.** A compiled program's register file is per
  instance and a validation has no instance, so it builds a `ScriptState`. Every other validation
  path allocates nothing. An interface polling a hundred graph-backed abilities per frame would want
  a pooled state; nothing needs that yet.
* **Async ability behaviour is the graph's, and only the committed side has it.** `ScriptProgram`
  already compiles a wait into a state machine with compact state, and `ScriptState` persists only
  the registers live across it — but `AbilityScriptHost::wait_satisfied` returns false, because a
  requirement check may not block. A host that runs the committed stages supplies its own
  `ScriptHost` that can answer.
* **Nothing is replicated.** Attributes and fragments declare their replication; no transport reads
  those declarations, because `networking-and-replication` is M9's.

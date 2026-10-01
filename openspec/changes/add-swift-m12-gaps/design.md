# Design

## Context

ABI 1.4 already carries what the bulk path needs underneath: `world_chunks` hands a module one
component's column per archetype chunk, `CyStage` is generated, and `@System` derives an
`AccessSet` from its `Query<...>` type. The ECS scheduler (`ecs::Schedule`) orders systems of a stage
by `jobs::AccessSet::conflicts_with`. The scene tree (`scene::SceneTree`) queues tree-shape
transitions and dispatches them at `pump()` to the behaviours its `BehaviourRegistry` holds. The
physics server has `add_force`, `add_impulse(_at)`, `add_torque`, `set_body_velocity`, and the
engine's `CharacterController` runs over its queries. What is missing is the boundary, plus the join
between script instances (`abi::BehaviourRuntime`) and the scene's behaviours.

## Decisions

### D1. One appended block, ABI 1.5

Eleven entries after `vfx_effect_parameter_get` and five vtable members after `frame_update`. Every
growable struct has `struct_size`; arrays (`CySystemAccess`) are fixed forever. The gate accepts all
of it as appends, and the Swift overlay and Rust SDK are regenerated from the one description.

### D2. Systems: register by declaration, schedule by name, resolve per run

`register_system` (`[N]`) copies the declaration into a host registry, one record per name per
generation, like behaviour records. `cy::abi::ScriptSystems::install(schedule)` adds one schedule
entry per name not yet installed, whose access is the record's terms (`READ`/`WRITE`/`EXCLUDE` to
`jobs::AccessSet::read/write/exclude`), and rebuilds the schedule. The entry's body resolves the
current generation's record by name on every run, so a hot reload changes what runs without
touching the schedule — which has no removal, and must not be re-ordered under a running frame.

A reload may therefore not change a system's stage or access: `register_system` refuses it in a
later generation (`UNSUPPORTED`), counts the refusal on the host, and `BehaviourRuntime::reload`
refuses the whole reload at its last reversible step (`ReloadFailure::SystemChanged`), abandoning
the new generation's registrations. *Rejected:* rebuilding the schedule on reload — it would move
native systems' batches under a running game and needs a removal the ECS deliberately lacks.

Phase: `ScriptSystems::run(schedule, stage, jobs)` sets the clock's phase once for the stage
(`phase_of_stage`: fixed stages F, frame stages U, render N) on the game thread before any job is
dispatched; never per system, since bodies of one batch run on several workers. While a body runs
the world is held iterating (`ecs::World::IterationGuard`), so structural entries answer
UNAVAILABLE instead of moving a chunk another system is reading.

Swift: `EngineChunkSource` calls `world_chunks` once per component the query reads or writes and
joins them by archetype (the k-th chunk of an archetype is the same chunk for every component), skips
archetypes holding an excluded component, and invalidates every view when the body returns.

### D3. Tree callbacks: the scene tree drives, a bridge forwards

`ScriptSceneBridge::sync_types()` registers one scene behaviour per script type, named the same, with
a `user` pointer (new on `BehaviourDesc`/`BehaviourContext`) naming the type. Its `on_create` creates
the script instance through the runtime, `on_destroy` destroys it, and the five tree callbacks call
`BehaviourRuntime::tree_callback`, which dispatches through the vtable of the generation that created
the instance. The types have no per-tick callback, so registering them adds no dispatch system;
`onFixedUpdate` and `onUpdate` stay the runtime's. A prefab naming a script behaviour spawns with
one, and a destroyed node destroys its script. Tree callbacks run in the pump caller's phase, which
the frame loop calls at the boundary (N), in the tree's order.

`@Node(path)`: the macro lists the wrappers (`nodePaths`, `nodeReferences()`), and the bridge's
`ready` thunk resolves each through `node_find` before calling `onReady`; an unresolved path stays
nil and is logged.

The scene fix this exposed: `pump()` dispatched each queued subtree event independently, so a node
created under a parent attached in the same frame received `onEnterTree` twice. An `entered` flag
per instance, cleared on exit, makes it once per attachment.

### D4. Bodies and characters

`PhysicsBodyAdapter` resolves an entity through the embedder's `EntityBodies`, refuses a write on a
body that cannot move (the server silently ignores it), wakes on force and torque, and answers
UNAVAILABLE during the step in every build. `CharacterAdapter` owns one `CharacterController` per
entity, sorted by entity, applies `CyCharacterDesc`'s zero-means-default rule, sets the entity as
the body's user data (so ray casts name the character) and is itself an `EntityBodies`.
`character_move` is `[F]` only and uses the clock's fixed delta, so stair behaviour cannot depend on
the frame rate.

## Phases and determinism, per entry

| Entry | Phases | Determinism |
|---|---|---|
| `register_system` | N | Schedule built from declarations; ties by registration order. |
| `node_find` | N F U | Names are unique among siblings: one node or none. |
| `physics_apply_force`, `_impulse`, `_torque`, `physics_set_velocity` | N F | Applied in call order (script order); forces cleared by the step. |
| `physics_get_velocity` | N F U | Last step plus writes since. |
| `character_create`, `character_destroy` | N F | — |
| `character_move` | F | One fixed step, applied at the call, in call order. |
| `character_state` | N F U | What the last move produced. |
| vtable `enter_tree` … `exit_tree` | the pump's (N) | The tree's order. |

## Risks

- A system body runs on a job worker when the embedder passes a job system; a Swift body must not
  touch behaviour state. The sample runs stages serially.
- An entity destroyed through a command buffer has its scene behaviour reclaimed without
  `onDestroy`, so its script instance lives until the runtime is destroyed.

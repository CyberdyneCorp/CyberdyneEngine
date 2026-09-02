# Design: CyberNet

## Context

Networking is the subsystem where architectural mistakes are least recoverable. Replication
topology, authority, and determinism assumptions propagate into gameplay code, and changing them
later means rewriting the game. It is also the subsystem most dependent on decisions made
elsewhere: ECS storage and change detection, the determinism contracts in physics, AI and
animation, world streaming, and asset cooking.

## Decisions

### 1. Three network modes, chosen per world, with honest requirements

| Mode | Replicates | Requires | Suits |
|---|---|---|---|
| `SnapshotAuthoritative` | Component state, server → clients | Server authority | Most games |
| `Rollback` | Inputs plus authoritative state, with client-side replay | Deterministic simulation on the client, same binary | Fighting, action, small-player-count |
| `Lockstep` | Commands only, all peers simulate | Deterministic simulation across **all participating peers** | RTS, simulation |

**Rationale.** These are genuinely different architectures, not quality settings. Forcing one
model means either an RTS sending ten thousand unit states, or an action game paying rollback cost
it does not need.

**The lockstep limitation, stated plainly.** Lockstep requires bit-identical simulation on every
participating machine. The engine's physics backend explicitly does *not* guarantee cross-platform
determinism — floating-point behaviour differs across architectures and compilers — and the engine
has no fixed-point or soft-float strategy. Therefore:

> Cross-platform lockstep is **not supported**. A lockstep session SHALL declare its platform and
> binary scope, and the engine SHALL verify participants match before allowing the session to start.

This is a real constraint, not a temporary gap. Supporting cross-platform lockstep would mean
either a fixed-point simulation path (a different physics backend, and a different maths layer) or
a soft-float implementation — both large, invasive projects. Stating the limitation now is better
than discovering it when a console port desyncs.

**Alternative rejected — making lockstep the default for RTS.** Tempting given the bandwidth
advantage, but it makes every subsystem's determinism a shipping requirement rather than a
testable property, and one non-deterministic system anywhere desyncs the match. Snapshot mode
remains the safe default; lockstep is an opt-in with declared prerequisites.

### 2. Replicate component state, not objects

Replication operates on ECS component data through **schemas**: per-component declarations of what
crosses the wire and how it is encoded.

```
NET_COMPONENT(Transform,
    NET_FIELD(position, Quantize<Vec3, 16>),
    NET_FIELD(rotation, QuantizeQuat<12>))
```

Schemas compile to serialisation code rather than being interpreted per field per entity.

**Rationale.** Actor-centric replication walks a heterogeneous object graph, dispatching per
property. Component replication processes homogeneous packed arrays — the same shape as every
other ECS system — so a thousand transforms serialise as a loop over a contiguous array with a
known encoder.

**Trade-off accepted.** Schemas are a second description of component layout that can drift from
the component definition. Mitigation: schemas are generated from or validated against the reflected
type, and a mismatch is a cook-time error.

### 3. Interest management is a scheduler, not a predicate

```
   100,000 entities
          │
   relevance filter          spatial, faction, ownership, explicit
          │
   ~3,500 candidates
          │
   priority scoring          distance, visibility, ownership, importance, staleness
          │
   budget scheduler          within this peer's bandwidth budget
          │
   ~800 sent this tick       at frequency and precision set by priority
```

Priority does not merely order a queue: it selects **update frequency** and **encoding precision**.
A distant unit is not dropped, it is sent less often and more coarsely.

**Rationale.** At scale, everything relevant does not fit. A boolean relevance test leaves the
hard question — what to do when relevant exceeds budget — to the game. Making it a scheduler with
explicit degradation levers is the same pattern used for VFX, audio, AI, and animation, and it is
what makes bandwidth a configured budget rather than an emergent property of content.

**Starvation is the failure mode to design against.** A low-priority entity must still update
eventually; the scheduler guarantees a maximum staleness per priority band, and staleness feeds
back into the score.

### 4. Network LOD mirrors the other LOD systems

| Band | Frequency | Precision |
|---|---|---|
| Owned / selected | 30–60 Hz | Full |
| Near, active | 20 Hz | Full |
| Near, idle | 10 Hz | Reduced |
| Far | 2 Hz | Coarse |
| Very far | 0.2 Hz | Minimal, or dormant |

Clients interpolate at frame rate regardless.

**Rationale.** This is the fourth appearance of the same pattern. Consistency matters: a developer
who understands VFX importance classes already understands network priority bands.

### 5. Rollback is engine-provided, not per-game

The engine records inputs with tick numbers, snapshots simulation state per tick within a bounded
window, and provides the reconciliation loop: on receiving authoritative state for tick N, compare,
and if diverged, restore and replay buffered inputs.

**Rationale.** Rollback is subtle, and every team that reimplements it reimplements the same bugs.
The ECS snapshot mechanism specified in `ecs-core` already provides the hard part — cheap
whole-world state capture and restore.

**Constraint this imposes.** Any system participating in rollback must be snapshot-restorable and
deterministic. Systems that are not — VFX, audio, non-pinned ML inference — must be excluded from
the rollback set, which the existing determinism firewalls already establish.

### 6. Lag compensation needs historical state, and that is expensive

Server-side rewind requires retaining recent world state to reconstruct what a client saw. The
engine retains a bounded history of **collision proxies** rather than full physics state: simplified
shapes and transforms per tick, sufficient for hit resolution and far cheaper than snapshotting the
physics world.

**Rationale.** Full physics history is prohibitive at scale. Hit resolution needs positions and
shapes, not contact manifolds and solver state.

**Trade-off accepted.** Rewound hit tests use simplified geometry, so results can differ marginally
from a live query. This is documented, and the compensation window is bounded so a high-latency or
malicious client cannot claim an unbounded rewind.

### 7. Replication cells, and the world partition gap

Interest management wants a spatial partition. **The engine currently has no world partition
capability** — asset streaming and navigation streaming each have their own notion of regions, and
nothing unifies them.

This change specifies **replication cells** as a networking-owned spatial partition, plus the
contract by which a future world partition system would supply them instead:

```
   client viewpoint → interest cells → { replication, asset streaming, world streaming }
```

**This is recorded as a known gap.** A world partition capability should be specified as its own
change; until then, replication cells are networking-local and the integration points are named
rather than wired.

**Rationale for not designing it here.** World partition is a substantial subsystem touching
streaming, level authoring, and the editor. Inventing it as a side effect of the networking spec
would produce a worse design than giving it its own change.

### 8. Dedicated server is a build and cook profile, not a runtime flag

A dedicated server build strips the renderer, UI, VFX, client audio, and high-resolution assets,
retaining ECS, physics, AI, navigation, gameplay, and networking.

**Rationale.** A runtime flag still links and ships everything, which wastes memory, binary size,
and attack surface on a machine that renders nothing. The engine already has feature slicing and
asset cook profiles; the dedicated server is their most demanding consumer.

### 9. Observability answers causal questions

The profiler answers "why", not just "how much": why an entity was replicated to a peer, what its
priority score was and which factors contributed, why a client received a given byte count broken
down by component and entity, and what caused a specific correction.

**Rationale.** Bandwidth totals tell you there is a problem. Causal attribution tells you which
component schema, which entity, and which relevance rule produced it. Networking bugs are
otherwise nearly undebuggable in production.

## Risks

- **Three modes is three implementations.** Mitigation: they share transport, schemas, and interest
  management; only the simulation contract differs. Snapshot mode ships first.
- **Lockstep determinism is load-bearing across the whole engine.** One non-deterministic system
  desyncs a match. Mitigation: the per-tick state hashing already specified in physics, AI, and
  animation, run in CI, plus a session-start verification of binary and platform match.
- **Schema drift** from component definitions. Mitigation: cook-time validation against reflection.
- **Replication cells without world partition** may need reworking when world partition lands.
  Mitigation: the integration contract is specified now so the seam is known.

## Open questions

- **World partition is the significant one.** It is needed by networking, asset streaming,
  navigation, and level authoring, and currently each improvises. It deserves its own change.
- Whether the interest scheduler and the AI LOD controller should share scoring infrastructure.
  They score similar things from different viewpoints (peer versus observer). Deferred.
- Which transport backend to adopt. QUIC brings encryption, congestion control, and stream
  multiplexing but adds latency characteristics worth measuring against a lean reliable-UDP layer.
  Specified as an evaluation.

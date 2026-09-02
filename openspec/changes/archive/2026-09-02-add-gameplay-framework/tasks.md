# Tasks: CyberGameplay

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis. Sections 3 onward are the implementation backlog, sequenced by the
phase table in `design.md`.

## 1. Specification

- [x] 1.1 Record in `design.md`: which of Unreal's concepts are kept and why its container is not,
      the six routinely conflated concepts, one command stream from five producers and the boundary
      with RPCs, many-to-many channelled control, behaviours compiling to systems with the honest
      limit, validation returning reasons, tags as compiled integers and why they are not ECS tags,
      time domains with ticks as the clock, death versus destruction, composition over mode
      inheritance, headless as a requirement, numeric performance contracts, what is deliberately
      out of scope, and the phase table
- [x] 1.2 New `gameplay-framework` (33 requirements, 68 scenarios): data-and-services principle,
      lifetime model, scoped services, gameplay context, composable rules, session state fragments,
      participants and players and local players, teams and affiliations, ownership/control/
      authority, control sources and bindings, one command stream, validation with reasons,
      capabilities, gameplay tags, events and messages, phases, spawning, time domains, the
      simulation clock, random streams, interaction, features, lifecycle and death, references,
      indexes, network integration, save and replay contracts, headless operation, behaviour
      compilation, performance contracts, **forbidden patterns**, diagnostics, and scope
- [x] 1.3 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `scene-graph-and-nodes` — behaviours now **compile into generated systems** where their
      callbacks are batchable, with the build reporting which did not batch and why. This replaces
      the previous concession that behaviours do not scale and that developers should hand-write
      systems instead; the concession becomes a compilation strategy with a stated boundary.
- [x] 2.2 `networking-and-replication` — the boundary between gameplay commands and RPCs is drawn
      explicitly: intent travels as commands so prediction, validation and replay apply, and RPCs
      carry what is not intent. Without this line both would be used for the same thing.
- [x] 2.3 `core-platform-abstraction` — input actions become the boundary with gameplay: actions
      produce commands, raw input never reaches gameplay systems, and binding contexts are added.
      This is what makes rebinding incapable of changing gameplay behaviour.
- [x] 2.4 `ai-system` — agents act through the same command stream as players and can validate
      without issuing, so planning reads the same structured reasons the interface and the authority
      use
- [x] 2.5 `testing-and-quality` — the three acceptance scenarios become benchmarks, with the
      strategy stress scenario named as the primary architectural test because a single-character
      scenario does not distinguish a data-oriented framework from an object-oriented one
- [x] 2.6 `thirdparty-dependencies` — the gameplay framework recorded as engine-built, with an
      external framework evaluated against the coupling it cannot provide
- [x] 2.7 `ecs-core`, `world-partition-and-streaming`, `swift-scripting`, `ui-system`,
      `serialization-and-prefabs` — reviewed; no change needed. Command buffers and deferred
      structural change, persistent identity and representation tiers, the behaviour and system
      programming models, semantic input actions in the interface, and entity templates already
      provide what this capability consumes.
- [x] 2.8 **Deliberately out of scope**: abilities, attributes, inventory, quests and objectives as
      optional modules; gameplay visual scripting, with the compile-to-system seam preserved

## 3. Phase 1–2 — structure and participants (deferred)

- [ ] 3.1 Application, game instance, scoped service registry, gameplay context
- [ ] 3.2 Game session and world session with multi-world support
- [ ] 3.3 Participants, players, local players with viewport, input user and listener
- [ ] 3.4 Teams, affiliations, and the relationship service
- [ ] 3.5 Ownership with hierarchy inheritance, kept separate from control and authority

## 4. Phase 3 — the milestone that matters (deferred)

- [ ] 4.1 Control sources, channelled bindings, entity groups
- [ ] 4.2 Gameplay command schemas with reflection and versioning
- [ ] 4.3 Command routing with per-worker accumulation and deterministic commit
- [ ] 4.4 Structural validation before game logic; structured rejection reasons
- [ ] 4.5 Capabilities and command filtering with reported exclusions
- [ ] 4.6 Verify all five sources — human, AI, network, replay, automation — share the path

## 5. Phase 4–5 — rules and spawning (deferred)

- [ ] 5.1 Composable rules and the rules asset
- [ ] 5.2 Session and player state fragments with authority, visibility and persistence
- [ ] 5.3 Phases as tags with validated transitions recorded by tick
- [ ] 5.4 Spawn service, policies, reservation, and batch spawning from entity templates
- [ ] 5.5 Ownership, team, affiliation and tag indexes maintained incrementally

## 6. Phase 6–8 — tags, time, features (deferred)

- [ ] 6.1 Gameplay tag registry, cooking to identifiers, hierarchy queries
- [ ] 6.2 Typed events, targeting, message channels, declared routing relevance
- [ ] 6.3 Time domains, simulation clock, bucketed timers
- [ ] 6.4 Named deterministic random streams derived from the session seed
- [ ] 6.5 Interaction queries, options, and batched spatial evaluation
- [ ] 6.6 Gameplay features with activation states and dependencies

## 7. Phase 9–11 — integration and tooling (deferred)

- [ ] 7.1 Network integration: prediction hooks, authority validation, local-only commands
- [ ] 7.2 Save and replay contracts; replay as a control source
- [ ] 7.3 Swift gameplay API and declarative rules
- [ ] 7.4 Behaviour compiler with the batching report
- [ ] 7.5 Gameplay debugger, command timeline, rule debugger

## 8. Phase 12 — validation (deferred)

- [ ] 8.1 **Strategy stress scenario** as the primary architectural benchmark, with framework
      overhead reported as a fraction of simulation time
- [ ] 8.2 **Control handover scenario**: vehicle entry and exit, AI takeover, turret operation,
      prediction transfer, spectator, replay reconstruction
- [ ] 8.3 **Headless scenario** verified to link no rendering, audio or interface code
- [ ] 8.4 Command determinism: identical command streams produce identical state
- [ ] 8.5 Replay reconstruction from seed and commands matches the original session
- [ ] 8.6 Batch spawn allocation counts against the declared target
- [ ] 8.7 Command submission throughput without a central lock
- [ ] 8.8 Index correctness: rebuilt indexes match incrementally maintained ones
- [ ] 8.9 Behaviour batching: a batchable behaviour at 100 000 instances costs one system, and a
      non-batchable one is reported at build time
- [ ] 8.10 Forbidden-pattern checks: no per-entity gameplay object, no per-entity virtual tick, no
      string tag comparison in a runtime path
- [ ] 8.11 Conflation tests: ownership, control and authority can be set independently and a
      captured-turret case behaves correctly

---

**Archived 2026-09-02.** Sections 1 and 2 are complete: `gameplay-framework` is in
`openspec/specs/` with 33 requirements and 68 scenarios, and six capabilities were updated —
including `scene-graph-and-nodes`, where the previous concession that behaviours do not scale
becomes a compilation strategy with a stated boundary. The unchecked items from section 3 onward
are the implementation backlog; **phase 3, the command stream, is the milestone that matters** —
retrofitted, replay and prediction become special cases and never fully recover.

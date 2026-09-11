# `src/gameplay/` — CyberGameplay

Layer 4. Gameplay lifetimes, scoped services, control sources and bindings, and **one validated
command stream**. Section 4.4 of the M4 tasks; the governing specification is `gameplay-framework`.

## The invariant this module exists to keep

`design.md` §3:

> Input reaches simulation **only** as commands. There is no "read the input state in a system"
> path, and no tool that pokes simulation state directly.
>
> Replay, rollback and lockstep are not three mechanisms — they are one command log read three ways.
> That is only true if the log is complete. A single system that reads a device directly does not
> merely bypass the stream; it makes the M9 guarantees **unachievable** until someone finds and
> removes it, and nothing will point at it, because everything will appear to work until a desync
> months later.

It is kept in three ways, at three depths, and `tests/test_bypass.cpp` asserts all three.

**1 — The build.** This module declares **no dependency on `cy::servers-input`**. An input header is
therefore not on a gameplay translation unit's include path, and a gameplay system cannot include one
even by accident. `test_bypass.cpp` checks it with `__has_include` and fails if the dependency is
ever added.

**2 — The shape.** `GameplayContext` — the only thing a gameplay system is handed — carries the
world, the world session, the session, the services, the command stream and the moment. No device,
no action, no input server. Concepts in `test_bypass.cpp` assert that no such member exists, so
adding one is a compile failure rather than a convenience nobody questions.

**3 — The consequence.** `test_bypass.cpp` runs two simulations on the same signal. The conforming
one turns the signal into commands in a *producer* and reads only committed commands in the
simulation; replaying its log into a fresh world reproduces it bit for bit. The bypassing one is
identical except that one axis is read directly by the simulation — one convenient line, of the kind
somebody adds because the value was right there. It works perfectly while it runs. Its replay loses
exactly the motion that never became a command, and the shape of the divergence is the diagnosis:
the axis that went through the stream matches, the one that went around it does not.

**So where does input actually reach gameplay?** In a bridge that lives **above** both modules — in
the runtime or in the sample — where it can see the input server and the command stream at once.
That is the whole architectural content of the invariant: the translation is a thing somebody wrote,
in one place, rather than a capability every system quietly has.

## What is here

| Header | Task | What it owns |
|---|---|---|
| `context.h` | M4 4.4.1 | The four lifetimes, scoped services, participants, and `GameplayContext`. |
| `control.h` | M4 4.4.2 | Control sources, channels, many-to-many bindings, entity groups. |
| `command.h` | M4 4.4.3 | Command declarations, per-producer buffers, the deterministic commit, the log. |
| `validation.h` | M4 4.4.4 | `ValidationResult` and its tagged reasons with the data behind them. |
| `random.h` | M4 4.4.5 | Named gameplay streams over `core-determinism`'s seeded streams. |
| `tags.h` | M8.b 3.2 | Hierarchical gameplay tags, tag sets, and the tooling that reports a tag doing an archetype's job. |
| `teams.h` | M8.b 3.1 | Teams, the relationship matrix, and affiliations beyond teams. |
| `ownership.h` | M8.b 3.2 | Ownership, declared inheritance, and network authority — the two of the three that are one value per entity. |
| `fragments.h` | M8.b 3.1 | Session and player state fragments, and the four answers derived from one declaration. |
| `rules.h` | M8.b 3.1, 3.2 | Composable rule pieces, the rules asset, and phases as gameplay tags. |
| `time.h` | M8.b 3.3 | Time domains with their own scale and pause, and per-domain tick-exact timers. |
| `events.h` | M8.b 3.3 | Typed events, declared delivery, tagged message channels, and the three relevance flags. |
| `interaction.h` | M8.b 3.3 | Interactables, their options, the batched spatial query, and `select()` producing a command. |
| `features.h` | M8.b 3.3 | Gameplay features, their contributions, their states, and dependency-ordered activation. |
| `indexes.h` | M8.b 3.3 | The derived indexes — by owner, team, affiliation and tag — and the digest that proves they are a cache. |
| `diagnostics.h` | M8.b 3.2 | The entity report, the command timeline, and the rule debugger. |
| `cook_firewall.h` | M8.c 1.3 | The **cook-time** half of the determinism firewall: a non-pinned model behind an authoritative AI node fails the cook. |
| `firewall_arming.h` | M9 1.4b | `arm_write_firewall()` — the startup call that tells the **runtime** firewall which components are authoritative, and the report that says how much of the world it ends up guarding. |

`abilities/` is `gameplay-abilities-and-effects`, a **separate target**: a project that does not use
abilities links none of it. See `abilities/README.md`.

## M9: the log seam, and arming the firewall

**`CommandStream` grew a seam and nothing else changed.** `RecordSink` is a function pointer called
once per *committed* command, from inside the merge loop, immediately after the in-memory
`CommandLog` takes it — so the two cannot disagree about what was recorded or in what order. It is
not consulted, it cannot reject, and it is not part of the merge key. `cy::replay::LogRecord` is
what the other side makes of it, and `src/replay/README.md` explains why there is exactly one such
record type for five readers.

Three numbers rather than three readings, in `tests/test_log_seam.cpp`: `records_emitted()`,
`committed_count()` and the sink's own count must agree; the sink's sequence is compared element by
element against `committed(i)`; and two runs differing only in provenance produce the same order with
different provenance intact.

**Proved by mutation**, because a check that cannot fail is not a check. A second `sink_.fn(...)`
added beside the first in `CommandStream::commit()`:

```
$ ctest --test-dir build/command-log -R '^unit\.gameplay_core$' --output-on-failure
unit.gameplay_core ...............***Failed
TEST CASE:  gameplay: the log seam sees every committed command exactly once, in merge order
  test_log_seam.cpp:122: ERROR: CHECK_EQ( recorder.count, 6U )   values: CHECK_EQ( 12, 6 )
  test_log_seam.cpp:130: ERROR: CHECK_EQ( recorded.sequence, committed.sequence )
    values: CHECK_EQ( 0, 1 )
  test_log_seam.cpp:137: ERROR: CHECK_EQ( from_seam.dx, from_stream.dx )
    values: CHECK_EQ( 0, 1 )
```

**`arm_write_firewall()` closes M8.c's own finding.** Its gate recorded that the runtime firewall in
`<cy/ecs/firewall.h>` is armed and guards nothing — `guarded_count()` is zero until something calls
`declare()` or `declare_from_reflection()`, and the only callers in the tree were test files. The
gap was never in the firewall; it was that nobody called it at startup, because until M9 nothing in
the engine knew which components are authoritative in the sense that matters.

What this phase owes is the mechanism and a report whose numbers cannot be read as better than they
are: `guarded` is printed **beside** `underived`, because "armed: yes / guarded: 0" is precisely the
state the gate found and it has to be visible at a glance. What it does not owe — and does not claim
— is the check itself: task 1.4b asks for a startup report "asserted by an artefact, not by a unit
test with three components in it", and that artefact is `samples/09-multiplayer`.
`tests/test_firewall_arming.cpp` says so in its own header.

## The determinism firewall, and why the command origin is NOT where it is enforced

M8.c section 1 required one enforcement point to be named for `vfx-system`'s and `ml-inference`'s
shared rule, and this module's command origin was one of the two candidates. **It was not chosen,
and the reason is a requirement this module already carries**: `gameplay-framework` says
"Provenance SHALL NOT affect validation, ordering, or execution" and
`sequencing-and-cinematics` requires that the simulation cannot distinguish a sequence-issued
command from any other. A check that rejected a VFX-originated *command* would be exactly a
provenance that affects validation. Beyond that, a command is the simulation's input rather than its
write: a VFX readback that reaches into the world and pokes a health value never submits one, so a
firewall here would catch only the producers that were already well behaved.

**The enforcement point is the ECS write path** — `<cy/ecs/firewall.h>`, `World::admit_write`, six
doors. `CommandStream` is untouched by it, which is the point: a sequence-issued command is still
indistinguishable, because the firewall never looks at a command.

**What this module does own is the cook-time half.** `ml-inference` requires that "a non-pinned
model feeding an authoritative node SHALL be rejected at cook time", and is explicit that this is a
different obligation from the runtime one: the desync it prevents is between two machines whose
*content* disagrees, and content is decided when it is cooked. `cook_firewall.h` is that gate — a
pure function over model-pinning and node-binding declarations, with no dependency on any inference
type, so a cook built with `CY_ML` off still refuses a package a build with it on would refuse. It
lives here because "authoritative" is a gameplay word in this engine and a gate inside the module it
gates would be a producer checking itself.

**What is not done, stated rather than implied**: no cook driver calls it yet, because there is no
`src/inference/`, no model asset and no AI graph node to feed it — those are M8.c section 4's, and
section 1 runs first so the gate exists before the first model does. The call site is
`tools/cook/`'s `run()`, which already fails a run on a returned error.

The suite for both halves is `tests/test_firewall.cpp`.

## What M8.b measured rather than asserted

Four of the framework's requirements are about cost, and each is now a number a test reads rather
than a claim a comment makes:

* **"Advancing a tick SHALL cost work proportional to the timers actually due."** `TimerWheel`
  buckets by due tick and keeps the occupied ticks in a min-heap; `last_examined()` reports what an
  advance touched. `gameplay_scale` schedules fifty thousand and asserts that a tick with nothing
  due examines nothing.
* **"Answering 'what does this participant own' SHALL NOT require scanning every entity."**
  `GameplayIndexes::entities_scanned()` counts what a lookup touched, and the hundred-thousand-entity
  case asserts a hundred.
* **"Queries SHALL be batched against spatial structures."** `InteractionQueryReport` reports cells
  visited and candidates tested; a query over a thousand interactables tests fewer than twenty.
* **"Rebuilding them from the world SHALL produce the same result."** `GameplayIndexes::digest()` is
  a canonical value identity, so the requirement is one equality rather than a comparison nobody
  makes.

Every per-entity table here — tags, teams, affiliations, ownership, the indexes — is hashed by
entity rather than scanned. That is not a micro-optimisation: `gameplay-framework`'s performance
contract is a hundred thousand active gameplay entities, and a scan per assignment makes populating
a world quadratic in it. The teardown case in `gameplay_scale` builds twenty thousand and would take
minutes rather than a second if any of them regressed to a walk.

## Three decisions worth knowing

**The session outlives the world.** `GameSession` owns the participants and the seed; `WorldSession`
owns nothing but a world and a role. Moving from a lobby to a play world adds and removes
`WorldSession`s and touches nothing else — `remove_world()` has literally nothing else to do, which
is what makes *"changing world SHALL NOT end the session"* true rather than remembered.

**The merge key is `(producer order, sequence)` and nothing else.** Never a thread identity, which
`simulation-and-determinism` forbids in an ordering key; never provenance, which
`gameplay-framework` requires to be diagnostic-only. `CommandLog::hash()` excludes provenance too,
so a replay's log hashes the same as the run it reproduces — otherwise the first "fix" anyone would
reach for is making the replay lie about where its commands came from.

**Validation returns reasons because a bool forces four consumers to disagree.** The interface
greying out a button, an AI choosing what to attempt, the authority rejecting a command and a test
asserting behaviour all call `CommandStream::validate()`. With a bool, three of the four grow their
own copy of the rule, and the day they disagree is the day a client shows an action the server
refuses. `tests/test_validation.cpp` ends by asserting that the interface's answer and the
authority's are the same object.

## Headless is a requirement, not a configuration

`gameplay-framework`: *"fully functional with no renderer, no audio, no interface, and no GPU … a
gameplay system that requires a camera, viewport, material, or audio device SHALL be a defect."*
None of those is reachable from this module's dependency list, so the dedicated server links no
rendering code because there is none to link, and every case in `tests/` runs with no world, no
device and no display.

## What is still thinner than the specification

M8.b takes `gameplay-framework` to **Working**. What remains genuinely absent, stated so that the
next milestone does not have to rediscover it:

* **Capabilities are a derived index, not components.** `CommandStream::set_capabilities()` keeps an
  entity-to-mask table so that this module is testable with no world. Rebuilding it from ECS
  components is a later change with no call-site consequences.
* **`GameSession::phase()` is still a `Name`.** `PhaseController` is the hierarchical-tag phase the
  requirement asks for and it is what a session should drive, but the `Name` field M4 added to
  `GameSession` is still there and nothing has migrated to the controller yet. Removing it is a
  change to M4's own type and its callers, and it is not this section's.
* **Behaviours do not compile to systems yet.** "Behaviour ergonomics compile to systems" is
  `visual-scripting`'s lowering plus a generator over it; the seam is preserved — commands, events,
  tags, rules and validation are all reflected schemas — but nothing here generates a system.
* **The indexes are maintained by their caller.** `GameplayIndexes` is incremental and correct, but
  a host calls `on_owner_changed` where it changes the owner; nothing observes ECS structural
  changes and maintains it automatically. That observation is `ecs-core`'s to provide.
* **The command stream is single-threaded in practice.** The *structure* is the one the requirement
  asks for — per-producer buffers, no central lock, a deterministic merge — but nothing yet records
  from several threads, so the claim is architectural rather than measured.
* **No benchmark.** The performance contracts are asserted by the `gameplay_scale` suite at the
  scales stated above; they are not in `benchmarks/` and so are not tracked against a baseline.

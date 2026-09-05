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
| `context.h` | 4.4.1 | The four lifetimes, scoped services, participants, and `GameplayContext`. |
| `control.h` | 4.4.2 | Control sources, channels, many-to-many bindings, entity groups. |
| `command.h` | 4.4.3 | Command declarations, per-producer buffers, the deterministic commit, the log. |
| `validation.h` | 4.4.4 | `ValidationResult` and its tagged reasons with the data behind them. |
| `random.h` | 4.4.5 | Named gameplay streams over `core-determinism`'s seeded streams. |

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

## What is thinner than the tasks claim

M4 delivers `gameplay-framework` at **Seed**, and section 4.4 names six of its requirements. What
the other requirements would add, and what is genuinely absent:

* **Teams and affiliations.** `Participant::team` is an integer and there is no relationship matrix,
  so "ally", "neutral" and "hostile" are not expressible yet. The requirement is explicit that
  relationships must not be inferred from identifier inequality; nothing here infers them, because
  nothing here reads them.
* **Gameplay tags.** The session's phase is a `Name`, standing in for the hierarchical tag the full
  requirement asks for. The shape is the one a tag registry slots into without a change at the call
  sites, but `Unit.Robot` matching `Unit.Robot.Harvester` does not work yet.
* **Capabilities are a derived index, not components.** `CommandStream::set_capabilities()` keeps an
  entity-to-mask table so that this module is testable with no world. Rebuilding it from ECS
  components is a later change with no call-site consequences.
* **Events, spawning, time domains, indexes, features, rules assets, session-state fragments** — all
  unstarted. They are `gameplay-framework` requirements outside section 4.4.
* **The command stream is single-threaded in practice.** The *structure* is the one the requirement
  asks for — per-producer buffers, no central lock, a deterministic merge — but nothing yet records
  from several threads, so the claim is architectural rather than measured.
* **No benchmark.** The performance contracts (100 000 entities, 100 000 commands per second) are
  unmeasured.

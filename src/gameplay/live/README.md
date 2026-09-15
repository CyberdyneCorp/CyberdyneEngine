# `src/gameplay/live/` — layer 4: the live edit policy, and the live edit compiler

`LiveEditPolicy`, `LiveEditPolicyTable` and `LiveEditCompiler`: what a change to one authored field
means for a world that is already running, decided before the change is made and then performed.

**Governed by**: `live-editing` (→ Seed at M5). Section 3.2 of the M11.b tasks.

## What was here before M11.b, and it was nothing

`grep -rniI 'LiveEditPolicy\|ReinitializeComponent\|RecreateEntity\|RestartWorld' src/ editor/
tools/` returned **no files** on the tree this module was written against, across five milestones
that built features on top of the row. That absence is one of the two greps M10's audit used as its
evidence for keeping `live-editing` at Seed, and the other is the play modes.

The classification the policy derives from did exist and had no consumer — the specification says so
itself, of `reflect::PersistenceKind`: it *"until now had no consumer"*. This module is that
consumer.

And the path a live edit would have travelled did not exist either.
`cy-editor-services/src/mirror.rs` hard-coded `ApplyWhen::OnArrival` with a comment beside it saying
*"a playing world would want `AtTickBoundary`, and the editor is what knows which"* —
`AtTickBoundary` was constructed in exactly one place in the whole tree, a protocol round-trip unit
test. **No edit had ever reached a playing world.** M11.b fixed that too, in the mirror, and
`an_edit_made_while_the_world_is_playing_is_scheduled_for_a_tick_boundary` reads the scheduling off
the socket rather than out of the mirror's own counters.

## What is here

| | |
|---|---|
| `policy.h` | the six policies, the classification defaults, and the per-field declaration table |
| `compiler.h` | `AuthoringChange` in, a validated runtime delta out, and then performed |

## Only three of the six policies are ever derived

`live-editing` requires that policy be *"derivable by default from the field's classification and the
component's nature, and overridable per field"*. The derivation is deliberately narrow:

| Classification | Derived policy | Why |
|---|---|---|
| `Authoring`, a plain value | `Immediate` | the asset defines it and the running instance follows |
| `Authoring`, an asset reference | `ReloadAsset` | changing which asset is referenced is a rebind |
| `Derived` | `ReinitializeComponent` | derived fields are recomputed, and rebuilding is what that costs |
| `RuntimeState`, `PersistentState` | `Unsupported` | the simulation owns the value; the change applies on the next run |

**`RecreateEntity` and `RestartWorld` are never derived.** Nothing about a field's classification can
say that changing it invalidates the entity or the world — that is knowledge about what the engine
*does* with the value, and it has to be declared. `LiveEditDecision::declared` carries which
happened, and the suite checks it field by field.

## Every field a play session creates is declared stronger than `Immediate`, and that is a finding

All eleven fields `declare_engine_policies` names are classified `Authoring`, so the derived default
for every one of them is `Immediate` — and `Immediate` would be **wrong** for every one of them.
`cy::physics::PhysicsBridge` creates a solver body from the node's `WorldTransform` and from the body
and collider components *at creation*, and thereafter **writes** `LocalTransform` back every step
(`bridge.cpp`'s own access declaration lists `local_transform` under writes and `world_transform`
under reads). An immediate write into any of them is overwritten on the next tick or never read at
all.

So: `Transform.translation`/`rotation`/`scale` → `RecreateEntity`; `RigidBody.mass` and
`gravity_scale` and the collider's four fields → `ReinitializeComponent`; the world's gravity →
`RestartWorld`. A field whose authored value the engine cooks into something else is bound
`Structural`, and an `Immediate` policy over one is **refused** rather than writing bytes that mean
nothing.

## How "without a restart" is measured rather than claimed

`LiveEditOutcome` carries the session's tick before and after the change. Every policy but
`RestartWorld` leaves them equal — the simulation continued across the edit, which is the whole claim
of live editing — and `RestartWorld` leaves the world at tick zero. The session produces those
numbers; the compiler only reports them and the suite only compares them.

Runtime state is preserved the same way: a `ReinitializeComponent` or `RecreateEntity` copies every
bound field classified `RuntimeState` or `PersistentState` out before the rebuild and back after, and
**counts both what it carried and what it could not**. The health-at-53 case rebuilds the component
through a host that resets health to full, so preservation is the only way the value can still be 53
afterwards.

## An authoring edit made during play is not discarded when play ends

`PlaySession::stop()` restores the document to the bytes it had at `enter()`. An authoring change
applied during play therefore has to reach the **restore target** as well as the live world, or a
designer's edit would vanish the moment they left play. `record_authoring` writes it into both, and
`PlayReport::restored_exactly` keeps its exact meaning: the bytes at `stop()` equal the bytes the
authoring state implies.

## What `live-editing` asks for that is not here

**The row is larger than this module and the rest is named rather than implied.**

* **Shader and material live reload.** `grep -ril reload src/rendering/` returns one unrelated file.
  There is no async recompile and no keep-last-good-pipeline. This is renderer work, not editor work,
  and it is easy to miss inside an "editor" rung.
* **Runtime inspection, and runtime tweaking distinct from authoring.** No inspection message, no
  read-only default, no keep-changes flow.
* **The live bridge protocol's full message set.** The requirement names nine things at minimum; the
  protocol covers about four of them now that the play mode rides on `Message::Play`. Absent: asset
  changed, prefab recompiled, layer state change, console command, profiler request and response.
* **Live editing diagnostics.** What was applied, what was refused and why, as a surface rather than
  as a return value.
* **A `LiveEditHost` for anything but a play session's own components.** The default rebuilds the
  physics components and refuses any other type **by name**; a project component needs a host that
  knows how to build it, and an asset rebind needs one wired to an asset system. Both refusals are
  deliberate: a policy that quietly did nothing would report applied over a rebind that did not
  happen.

## What checks the policy table, and the one leg that carries the claim

`m11b:live-edit-policy-exists` was four inverted greps — `LiveEditPolicy`, `ReinitializeComponent`,
`RecreateEntity`, `RestartWorld` — which a comment naming them satisfies, and M11's gate refused it
as such. It is now `tools/editor/play_contract.py live-edit-policy`, which compares the table above
against `live-editing`'s own markdown table and against `reflect::PersistenceKind`, the
classification the defaults are derived FROM.

Its load-bearing leg is the one `policy.h` argues at length: **`RecreateEntity` and `RestartWorld`
are never derived.** Nothing about a field's classification can tell you that changing it invalidates
the entity or the world; that has to be DECLARED, and `m11b:live-edit-applies-without-a-restart` is
written against exactly that distinction. A build in which `derived_policy_for` could return either
would make the per-field declaration unnecessary and that criterion vacuous — so inserting
`LiveEditPolicy::RestartWorld` into one of its arms turns the gate red, which is one of the eighteen
cases `tools/editor/selftest.py` checks. Run by `just quality-editor-contract` and by
`integration.editor_contract_live_edit_policy`.

# M13 — After 1.0: the three capabilities 1.0 ships without

## Why

**A deferral needs somewhere to point.** `delivery-roadmap` treats a re-entry point named in
the record as a decision and one named nowhere as an oversight wearing the same clothes — and
until this rung existed, three capabilities had no destination:

- **`xr-support`** is the one row of seventy-six that reaches 1.0 without a Complete cell. Its
  re-entry was recorded only as *"after 1.0"*, which is a date rather than a rung.
- **`ml-inference`** has been at **Seed since M8.c**. `m11b:ml-inference-or-a-deferral` asks for
  the row to be exercised by the game's AI **or** recorded as a deferral with its re-entry at
  M11.e — and M11.b produced neither.
- **Android** is the half of mobile that M11.e does not ship. iOS landed at M11.e with
  `platform/ios/`; Android has no platform directory, no toolchain and no cross-compilation leg.

These are deferred **deliberately**, not discovered late. Each is a capability the engine can
reach 1.0 without, and each costs more than the rung that would otherwise carry it can afford.

## What changes

- M13 joins the ladder with all four attachments the end-of-ladder check requires: a ledger
  (`tools/roadmap/milestones/m13.toml`), a gate (`milestone-m13`), a floor, and this change
  directory. `m11e:the-ladder-ends-consistently` is satisfied by a rung that carries all four
  rather than by M11.e being last everywhere.
- The three deferrals name M13 as their re-entry point, replacing *"after 1.0"* and the
  M11.e destination that could not close them.
- **1.0 still ships from M11.e, and M12 proves it with a game.** M13 is after both, and nothing in M11.e waits on this rung.

## What this is not

This is **not** a place to move work that a current rung finds inconvenient. Three capabilities
enter here by name and by decision. A fourth arrives only through a change that argues for it —
the ladder already carries the cost of a rung that became a collector, and it will not carry two.

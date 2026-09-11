# M10 — Worlds: environment as one substrate with one producer per field

## Why

**M9 made the engine's state defensible; nothing yet makes its world large.** The engine can record a
session, replay it bit-exactly, replicate it to four peers under packet loss and narrow a divergence
to one field on one entity. What it cannot do is fill a world: there is no terrain, no foliage, no
water, no weather, no sky, and no procedural generation to place any of it.

**What is verified absent on the tree M9 closes on**, rather than assumed:

- `src/environment/`, `src/terrain/`, `src/foliage/`, `src/water/`, `src/weather/`, `src/sky/` and
  `src/pcg/` do not exist. Seven of M10's eight rows have no directory.
- `environment-fields`, `terrain`, `foliage`, `water`, `weather-and-wind`,
  `atmosphere-sky-and-clouds` and `procedural-content-generation` are all recorded at `none` in
  `docs/roadmap/status.yaml`. Every one of them reaches **Working** here, from nothing, which is the
  largest from-nothing load any milestone in this plan carries and is the reason §4 of `design.md`
  says which rows are contingent before the gate says it.
- `vfx-system` is at **Seed**. M8.c built the asset model, the IR, the compiler, the shared
  simulation world, the scheduler, the budget controller and the determinism firewall's producer
  side, and its closing gate moved the Working cell here rather than claim it over an absent GPU
  compute path. **That cell is work, not a formality.**

**And five things M9's closing gate handed forward**, each a running, failing, rung-bearing criterion
in `tools/roadmap/milestones/m9.toml` rather than a sentence in a document:

- `m9:breadcrumbs-adopted` — the breadcrumb ring is armed and has no callers outside its own module,
  so a crash artefact reports `0 of 64`.
- `m9:crash-artefact-paths` — a *produced* crash artefact still carries the build machine's absolute
  paths, because `backtrace_symbols_fd()` writes each frame's module path as the loader resolved it.
- `m9:gameplay-benchmarks` — `gameplay-framework`'s performance table is specified to be benchmarked
  and is benchmarked nowhere.
- `m9:record-matches-plan-history` — four closed milestones' matrix columns claim nineteen cells the
  status record does not support, thirteen of them Complete. M9's own column was repaired at its
  gate; the earlier four were not audited by it and are not this milestone's to claim silently
  either.
- **`tests/determinism/` is still empty.** `tests/CMakeLists.txt` says in as many words that the
  determinism kind "joins them at M9", and `testing-and-quality` gives it a location and a 10 s
  budget. M9 put its determinism cases in `unit` and `integration` instead. Golden replays, replay
  and save fuzzing and the transactional save tests all live in that suite and none of them exist.

## What Changes

- **`environment-fields` to Working** — the shared substrate, field declaration, **one producer per
  field** enforced by a refusal rather than a convention, sparse tiled storage and streaming, CPU and
  GPU access, residency levels, and determinism for the fields gameplay can see.
- **`terrain`, `foliage`, `water`, `weather-and-wind`, `atmosphere-sky-and-clouds` and
  `procedural-content-generation` to Working** — the scope each carries in the M10 row of
  `docs/ROADMAP.md`.
- **`vfx-system` to Working** — the GPU compute dispatch M8.c did not build: particle state resident
  in GPU buffers, indirect dispatch driven by GPU-maintained counts, async compute where the device
  exposes a queue, the GPU sort behind `BudgetLevers::sorted`, and the renderer kinds beyond `Sprite`
  and `Mesh`.
- **`diagnostics-profiling-and-crash`, `save-and-persistence` and `gameplay-framework` to Complete** —
  the three rows M9 planned to complete and demoted, each with its blocker named above. **They are
  Complete here only if the blocker is closed**; a second demotion of the same row is a signal that
  the row is mis-scoped rather than late.
- **`tests/determinism/` stops being empty**, and the three gaps above close with the criteria that
  declare them.

## Capabilities

### World and environment

`environment-fields`, `terrain`, `foliage`, `water`, `weather-and-wind`,
`atmosphere-sky-and-clouds`, `procedural-content-generation` and `vfx-system` to **Working**;
`diagnostics-profiling-and-crash`, `save-and-persistence` and `gameplay-framework` to **Complete**.

The full column is `docs/roadmap/capability-matrix.md`; the M10 row of `docs/ROADMAP.md` is the scope
statement this proposal implements.

## Impact

- **New code**: seven directories that do not exist today, plus the GPU half of `src/vfx/`.
- **Existing code**: `world-partition-and-streaming` gains the environment substrate as a streamed
  producer; `save-and-persistence`'s overlay carries terrain deformation; `src/core/diagnostics/`
  gains the breadcrumb call sites its ring has been waiting for.
- **Closing artefact**: `samples/10-world` — an open world: procedurally populated terrain with
  rivers and an ocean, foliage responding to a wind field driven by weather, wetness and snow
  accumulating, a full day/night cycle with volumetric clouds, all streamed and all persistent.
- **Risk**: region invalidation in PCG, and it is the milestone's named spike. Getting
  dependency-driven partial regeneration wrong means either stale content or full-world
  regeneration, and both are project-defining.

# `tests/acceptance/`

`testing-and-quality` — "Performance benchmarks": the suite SHALL include **acceptance scenarios**
"exercising the architecture rather than a favourable case". Three are named, and from M0 to M11.d
none of them existed. M11.d task 7.2 wrote them, each in the existing kind whose budget fits, and
each asserting the property this rung's delta names for it rather than merely running.

| Scenario | CTest | Kind, budget | Asserts |
|---|---|---|---|
| **Strategy stress** | `smoke.acceptance_strategy_stress` | smoke, 30 s | 8 participants, 4 teams, 100 000 units (20 000 moving), 5 000 groups, 1 000 structures, 320 group orders → 6 400 validated member commands a tick, every one reaching the replay-recording seam. The framework's share of a tick — routing, structural validation, commit, recording, ownership queries — against the SIMULATION's — orders applied, movers integrated, 101 000 entities binned, target acquisition — compared with a committed ceiling |
| **Control handover** | `integration.acceptance_control_handover` | integration, 1 s | four players, a vehicle, a gunner, an AI taking the wheel and a spectator: the vehicle is one entity whose motion continues across the handover; the gunner is accepted on every tick including the handover's; the departed driver is refused `NotControlled` on the very next tick; the spectator reads the true controllers every tick and every order it tries is refused; the recorded stream replayed into a fresh state matches at every tick |
| **Headless server** | `smoke.acceptance_headless_server` | smoke, 30 s | `cy_headless_server`, a dedicated-server BUILD configuration: 100 000 entities, tiered AI (~2 700 thinks a tick, none starved), ~270 orders a tick through one group binding per squad, a kinematic Jolt body per entity, stepped at a fixed 30 Hz. Its trace is read back by `tools/trace/trace_inspect.py`, which does not link the engine, and must carry every counter a server owes at every tick — including where the tick went — with values that mean it ran, and ticks that are one fixed step each. The achieved wall rate and overruns are REPORTED, not asserted: measured 29 Hz paced, 0 overruns, worst tick 25 ms, the physics step dominating |

## The headless build claim fails the BUILD, twice

"A dependency on any of them SHALL fail the build." Until M11.d there was nothing to check it
against: "headless" was a run-time display-server choice on binaries that link the renderer anyway.
`cy_headless_server` is the configuration, and two checks hold it:

1. **Configure time** — `cy_require_headless` (`src/gameplay/cmake/headless.cmake`, the check
   `gameplay-framework`'s headless requirement already runs over every gameplay module) walks the
   server's DECLARED transitive link closure and refuses the configure naming the path to a
   rendering, audio, interface or desktop-display target.
2. **Every link** — `headless_closure.py` runs POST_BUILD over the LINKED binary: no symbol in
   `cy::rendering::`, `cy::rhi::`, `cy::audio::`, `cy::ui::`, `cy::text::` or a desktop display
   server, no Vulkan, SDL or miniaudio symbol, and no dynamic dependency on a window, GPU or sound
   library. Run over `cy_sample_ship` it names SDL, Vulkan, the RHI, the render graph and libX11.

## What these do NOT exercise, said here as well as on each test's own output

- **World streaming** and **network authority** in the strategy stress: one resident world, every
  participant local. The merge is the authority's merge, but nothing is serialised or sent.
- **Networking and prediction** in the control handover: one process, no latency, no rollback.
  `samples/09-multiplayer` owns rollback under loss; composing it with a handover is the next step.
- **Connections, replication and world streaming** in the headless server: no transport and one
  world, so those counters would be zeros that looked like measurements.

## The finding the strategy stress produced before it passed

It could not be BUILT: `ControlRegistry` refused a 65th group (`kMaxGroups` was 64). With the cap
lifted it measured the framework at **94 %** of a strategy tick — 50.7 ms of validation against
3.2 ms of simulation — because `controls()`, asked once per member command, walked every binding
and searched the group list for each group binding. The registry now answers from a binding index
and a membership index (`src/gameplay/include/cy/gameplay/control.h`); the framework measures
0.5–1.2 ms a tick, 22–34 % of this scenario's deliberately modest simulation on a loaded host.

**The ceiling is 50 %, and it is a regression guard, not the claim that a third is "small".** The
first draft set 15 % before anything was measured; the measurement replaced the guess, and both are
recorded in the test so a reader does not mistake 50 % for a loosened check. `gameplay-framework`'s
"small, reported fraction" is judged against a real strategy tick — pathfinding, combat, streaming
— which this scenario does not yet have.

## What the headless server measured about physics

Units are KINEMATIC bodies that collide with the ground and not with each other. Both choices were
measured rather than assumed: as dynamic bodies packed two metres apart, every order woke its
neighbours through contacts and the step went from 8 ms to 140 ms within thirty ticks; as dynamic
bodies with unit–unit collision filtered out, the ground contacts of the growing woken set still
took it past 170 ms. Kinematic units whose orders run out after half a second hold the active set to
~5 000 and the step to ~25 ms of the 33 ms tick on a loaded host — single-threaded, because the
server hands Jolt no job system. That is the number a real server would attack first.

## Benchmarks run nightly; these run on every `ctest`

The strategy stress defends a RATIO of two measurements taken side by side on one thread, which a
loaded machine moves far less than either measurement — that is what lets it live in CTest. It is
not a replacement for `benchmarks/`' nanosecond baselines recorded on quiet hardware.

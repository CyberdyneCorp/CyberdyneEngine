# M7 is not closeable — one blocker, and the measurement it was missing

Written overnight 2026-09-08 ~02:00. **M8.a was NOT launched**, per the runbook's stop condition:
a gate that says a milestone is not closeable ends the sequence rather than being worked around.

## THE BLOCKER

`just roadmap-milestone m7` exits 1 after two full runs. Everything M7 built passes its own
criterion. What fails is the claim the milestone was named for — *"the arbiter's allocations
converge without oscillation under a step load"*. `samples/07-fidelity` reports the gap in **6 of 20
runs on an idle machine**, and the split is one `if`:

`spike.cpp:286` substitutes the device's measured geometry frame for the modelled one, but only
while the resulting nominal state keeps `kNominalHeadroom = 0.92` ms of room. The device frame here
is **bimodal — 3.3 ms or ~5.2 ms depending on the GPU's power state** — so the substitution is taken
on some runs and refused on others. Every run that took it oscillated; every run that refused it did
not.

`integration.render_arbiter_sweep` cannot see this because **it sweeps the step LOAD over a fixed
cost table.** The design note says "one step magnitude can make any law look stable" and sweeps
magnitudes for exactly that reason. One nominal cost can make any law look settled, and nothing
swept that.

## THE MEASUREMENT THE GATE ASKED FOR — DONE, AND IT IS BETTER NEWS THAN THE VERDICT

The gate offered two remedies: *sweep the operating point and find where the law stops settling*, or
*narrow the claim in `rendering-architecture`*. It did neither, correctly — raising
`kNominalHeadroom` until the substitution is refused would turn the artefact green by dodging the
operating point.

I built a throwaway probe that sweeps the operating point (geometry cost 2.0 → 5.4 ms, 71 step
magnitudes at each) and deleted it afterwards; `samples/07-fidelity/CMakeLists.txt` is
byte-identical. Budget is 13.90 ms.

| geometry | nominal | headroom | oscillating /71 | frames over budget |
|---:|---:|---:|---:|---:|
| 2.00 – 3.70 | 9.80 – 11.50 | **≥ 2.40** | **0** | **0** |
| 3.80 | 11.60 | 2.30 | 0 | 12 |
| 3.90 – 4.10 | 11.70 – 11.90 | 2.20 – 2.00 | 0 | 44 |
| 4.20 – 4.70 | 12.00 – 12.50 | 1.90 – 1.40 | 0 | 79 → 225 |
| 4.80 | 12.60 | 1.30 | **2** | 244 |
| 4.90 – 5.20 | 12.70 – 13.00 | 1.20 – 0.90 | **3, 4, 6, 7** | 276 → 474 |
| 5.30 – 5.40 | 13.10 – 13.20 | 0.80 – 0.70 | 0 (nothing restored) | 484 → 504 |

**Three regimes, and the boundaries are sharp:**

1. **Headroom ≥ 2.40 ms — clean.** Zero oscillation, zero frames over budget, every magnitude
   restored. This is where the shipped artefact sits when the device is in its 3.3 ms mode.
2. **2.30 down to ~1.40 ms — stable but over budget.** The law still never oscillates, but it
   overshoots: 12 frames at the top of the band, 225 at the bottom. This regime is invisible in
   today's criterion and is arguably a worse finding than the oscillation, because a loop that holds
   its lever while missing budget looks converged.
3. **≤ 1.30 ms — oscillation begins**, rising monotonically 2 → 7 of 71. The artefact's 5.2 ms
   device mode lands at the very bottom of this band.

Below 0.80 ms the nominal state no longer fits at all and nothing is restored — that is a content
defect, not a control-law one, and the design note already calls it that.

## WHAT THIS MAKES THE DECISION

The law does not fail at a point. It **degrades monotonically as headroom shrinks**, and the
oscillation the gate saw is the tail of that, not a distinct bug. So:

- **Option A — fix the law.** The target is holding budget at ~1 ms of headroom. That is real
  control work, and the data above gives it a pass/fail curve to aim at rather than one failing run.
- **Option B — state the envelope.** `rendering-architecture` says the arbiter converges; it does
  not say over what operating range. Adding "with at least N ms of nominal headroom" makes the claim
  true and checkable. **N = 2.4 ms** is where the measurement puts the clean boundary. This is
  honest rather than a dodge *only if* the criterion then sweeps the operating point and asserts the
  envelope, so a future change that narrows the margin fails.
- **Either way**, `integration.render_arbiter_sweep` must sweep the operating point as well as the
  step magnitude. That is not optional in either option, and it is the actual hole.

My recommendation is **B plus the swept criterion, then A as M8-or-later work**: the envelope is
wide (2.4 ms of 13.9 is 17 %), every shipped profile sits inside it, and the alternative is holding
a closed milestone open on control-loop tuning while the rest of the ladder waits. But narrowing a
claim is the user's call, not mine, which is why this is written down instead of applied.

## THE OTHER FOUR FINDINGS — NOT BLOCKERS, BUT READ THE FIRST ONE

1. **The unit budget measures the CPU's clock rate as much as the test, and an IDLE machine is its
   worst case.** `CLOCK_THREAD_CPUTIME_ID` counts seconds; this governor idles at 800 MHz. The same
   M2 suite measures 0.72–1.05 ms alone and 0.20 ms beside four spinners. *This is the mechanism
   behind "one unit suite failed once" in every milestone report since M4* — and behind ten of 56
   Debug suites going red four hours after none of them did. The gate fixed the harness comment to
   state the measurement; normalising the budget against a reference workload in the same process is
   the contained fix and nothing short of it makes `four-profiles` reliable.
2. **A closed milestone's criterion selects suites by substring.** `--tests text` was written at M5
   for `unit.text`; M7 added `render.virtual_texturing_gpu`, and `virtual_texturing` contains
   `text`. A device test entered a leak-checked sanitizer run for the first time and a driver-owned
   libdbus leak took the criterion red. Fixed, but a substring is not a name.
3. **Two artefacts still report a GAP and exit zero.** `just run-agent-authoring` prints four
   unsatisfied steps and returns 0, and the ledger reports it `ok`. M6 made the rule structural;
   these two predate it and were not migrated. **It must not be left a third time.**
4. **`ci-check` was green with `milestone-m7` flipped green before its criteria passed** — the rule
   is one-directional. The gate put the gate back to `joins-on-close` so the failure is visible on
   main rather than at the next gate.

## THE HEADLINE THE USER ASKED FOR, WHICH IS UNAFFECTED

**A person can now see the engine's own 3D world in the editor's viewport, with a gizmo on it.** The
gate ran it and looked: five shaded, checker-textured boxes on a checkerboard ground with cast
shadows, drawn by `cy_editor_window_runtime` (linking `cy::sample-first-light`, `cy::servers-render`
and `cy::rhi-vulkan`), delivered over an imported dma-buf. The magenta fixture is out of the loop.
The engine publishes the gizmo layout, a pointer drag moves the object **in the engine's world**
(the engine re-centres its own gizmo from (365,471) to (433,530)), and Ctrl+Z returns it to within a
pixel in one transaction.

Three qualifications the gate was careful to state: the scene is the runtime's, not the editor's
document, so entities created in the editor do not appear in it; selection is by outliner row
because engine-side picking is unexercised; and the frame is read back to host memory and re-uploaded
(106 µs at 1280×720) because the renderer and the publisher own different Vulkan devices — so it is
not yet zero-copy end to end.

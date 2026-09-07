# `src/rendering/arbiter/` — layer 4

The renderer's one budget arbiter, the subsystem controllers it allocates to, the four named
renderer profiles, and the rendering configuration asset.

**Governed by**: `rendering-architecture`, which reaches **Complete** here. M7 tasks 10.1 and 10.2.

## The files

| File | What it holds |
|---|---|
| `arbiter.h` | `BudgetArbiter` — the only thing in the renderer that is given a frame time — its config, its report, and every adjustment with its cause |
| `subsystem.h` | what a subsystem declares (a priced ladder, a reduction order, a reserved minimum) and `SubsystemController`, which holds one allocation and cannot see the frame |
| `profiles.h` | `Mobile`, `Standard`, `HighEnd`, `Cinematic`; the pipelines and their strengths; the capability fallback that names what is missing |
| `configuration.h` | the rendering configuration asset: layered overrides, the three validations, and the structural diff a review reads |

## Why this module depends on none of the six systems it arbitrates

Six paged or stochastic systems arrived in M7 and each of them can spend an unbounded amount of a
frame. The arbiter is what makes them one budget rather than six competing ones — and it does that
without naming a single one of them. A subsystem declares a **priced ladder** and reports its own
cost; the arbiter answers with milliseconds.

That is not tidiness. It is the criterion. `design.md` §2.1 is emphatic that a control law over a
discrete ladder must be certified over a **sweep** of loads rather than one, because where the
equilibrium lands relative to a ladder boundary is what decides whether it oscillates — and the
spike's own first draft passed at one load and oscillated on 27 of 71. A sweep is 71 loads ×
1,400 frames × 7 subsystems. Nothing that renders can be run a hundred thousand times inside a test
budget. An arbiter that named `cy::rendering-shadows` would be a control law you could only test by
rendering.

The measured result, and the criterion of task 10.1:

    just test-integration -R integration.render_arbiter_sweep

    sweep: 71 loads, 0 oscillating (worst tail 0 changes), 0 settled over budget,
    mean settle frame 91, worst 237

## The four mechanisms, and the one that finishes the job

`design.md` §2.4 is a factorial over them — an EMA filter, a relax dwell, an arbiter period of eight
frames, and a deadband. Each alone leaves 56 to 64 of 71 loads oscillating; all four together leave
zero.

The **deadband** is the one that finishes it, and it is sized against the coarsest single-lever cost
quantum **reachable from the current state**, not against a fixed number of milliseconds. What makes
a discrete ladder oscillate is a correction smaller than the step it would have to take.

It has a price and this module pays it deliberately: a deadband centred on the budget is by
construction a refusal to correct an error smaller than one lever quantum, so the loop settles
happily *over* the budget. The setpoint is one deadband **below** the budget, which puts the band's
upper edge on it. That costs about 0.6 ms of a 12.7 ms allocatable budget and it is why zero of 71
loads settle over budget rather than two.

## The actuator is the declared reduction order

`design.md` §2.3, and the single most important finding of the spike. When the frame is over budget
by `e`, the arbiter walks the subsystems in ascending `reduction_order` and forces **one** step down
from each until `gain × e` of the deficit is covered. Nothing is scaled.

A uniform scale moves whichever subsystems happen to sit nearest a ladder boundary, so one tick
drops four of them at once and the next gives them all back. Replacing it with the declared order
took the spike's sweep from 27 of 71 oscillating (worst 411 lever changes) to 2 of 71.

## Tightening is local; relaxing is arbitrated

`rendering-architecture` forbids a subsystem controller to measure total frame time, and there is no
setter in `subsystem.h` through which one could arrive. `design.md` §2.6 found the half nobody
writes down: **a controller may tighten on its own authority and may never relax on it**, because
the time a step back up costs comes out of the frame — the one quantity it is forbidden to see.

The forbidden arrangement is modelled and measured rather than described. `test_sweep.cpp` gives
every subsystem its **own private arbiter** that sees the whole frame — which is what "a controller
measuring total frame time" amounts to once it is written down — and counts what happens:

    tail lever changes over 450 frames at load 1.6: one arbiter 0, seven private arbiters 252

## One number in `ArbiterConfig` is this implementation's own finding, not the spike's

`relax_allocation_margin = 1.20`. The two halves of the loop have to agree and at ×1.08 they do not:
a controller relaxes only when `predicted(next better) <= allocation × 0.88`, so an allocation that
buys a step up must be at least `predicted / 0.88` = ×1.136. An arbiter granting ×1.08 spends its
surplus on an allocation the controller then refuses to use — which is `design.md` §2.5 defect 3
arriving through a different door. **Measured: 1 of 15 loads returned to authored quality at ×1.08,
and 15 of 15 at ×1.20.**

## Profiles are configuration, and the enforcement is that they cannot be anything else

`RendererProfile` is a plain aggregate and every shipped profile is a function returning one. There
is no virtual call and nothing a profile can select that a project cannot: `named_profile()` and a
project's own literal produce the same type and go through the same three functions.

Two properties the suite asserts that are easy to lose:

* **Every profile carries every subsystem that draws the scene.** "Content SHALL render under every
  profile it targets, differing in fidelity and performance, not in whether it appears." `Mobile`
  sits at the coarse end of every ladder and turns off the features that are additive; it does not
  unregister geometry.
* **Every profile's nominal state fits its own budget with headroom.** `design.md` §2.10 records
  this as a modelling trap rather than a design one: the spike's first model had a 17.1 ms baseline
  against a 13.9 ms budget and produced a 54-frame limit cycle that looked like a control-law defect
  and was a content defect. A profile whose authored state does not fit its own budget is a renderer
  that begins every session degrading, and the arbiter gets blamed for the profile.

## How a frame uses it

```cpp
BudgetArbiter arbiter;
apply_profile(named_profile(ProfileName::Standard), arbiter);

// per frame
arbiter.report_frame_ms(gpu_frame_time);            // only the arbiter gets this
for (each subsystem) {
    arbiter.report_subsystem(id, own_cost_ms, controller.position(), controller.at_minimum());
}
const ArbiterReport report = arbiter.update();
for (each subsystem) {
    controller.set_allocation_ms(report.allocation_ms[i]);
    if (report.relax_granted[i]) controller.grant_relax_step();
    controller.update();
}
```

`virtual-shadows`' `ShadowBudget` and `rendering-global-illumination`'s `GiBudget` are two
hand-written instances of exactly the `SubsystemController` loop over their own named levers. They
were written before this class existed and they stay, because each maps a composite ladder position
onto levers only it understands; what they take from the arbiter is `set_allocation_ms` and
`grant_relax_step`, which are the same two calls.

## What is not here

* **No adapter to `residency`.** That layer's levers are arbitrated in *bytes* by the memory
  pressure monitor and these in *milliseconds* by the renderer. `subsystem.h` says where the two
  meet and why folding them into one declaration is a change to `residency`'s enumeration rather
  than to this module.
* **No `samples/07-fidelity`.** Section 11 is the artefact that drives this in a shipped path. The
  arbiter has a profile, a controller and a sweep; it does not yet have a scene.

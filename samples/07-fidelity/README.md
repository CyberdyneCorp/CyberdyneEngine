# samples/07-fidelity — M7's closing artefact

*A film-detail interior and exterior, millions of source triangles, dynamic lighting, indirect
illumination and reflections, holding a frame budget while the arbiter reallocates under a scripted
load spike.*

```
just run-fidelity                       # the artefact, four runs, the median of them
just run-fidelity --shot docs/design/images/fidelity-m7.png
just test-smoke -R smoke.fidelity       # the same thing at half detail, one run
```

Two files do the work. `cy_sample_fidelity` is the program — it generates and cooks the set, puts it
on the device, lights it, and drives the arbiter — and `fidelity.py` is the driver that checks every
claim the program printed and reports through `samples/harness/artefact.py`. The harness is why a
recorded gap cannot exit zero and why the figure this run leads with cannot be a maximum.

## The four acts

| Act | What it does | What it is evidence for |
|---|---|---|
| `detail` | five closed shells generated, cooked through `vg::build_geometry`, and checked with `check_watertight` | task 11.1 |
| `frame` | the cluster traversal and the visibility buffer on the device, 1280×720, timed | task 11.1 |
| `light` | `gi::IlluminationSystem` converged over the scene; indirect diffuse and specular resolved | task 11.2 |
| `spike` | `BudgetArbiter` and seven `SubsystemController`s under a scripted load, swept over nine magnitudes, plus the starvation case | task 11.3 |

## What is measured and what is modelled

The distinction is stated everywhere it matters, because an artefact that blurs it is an artefact
nobody can act on.

**Measured on this machine.** The cook's wall clock. Every count: source triangles, distinct
triangles, clusters, pages, resident bytes, watertightness findings, covered pixels, visible
clusters, material bins, validation errors. The device frame — recorded as *submit to idle* on a
serialised frame, which is the dispatches, the graph's derived barriers and the submission, and is
not a presented swapchain because this artefact has no window. `ExecutionResult` carries no
timestamps, so nothing finer is available through the graph today; timing a frame with the device's
own query pool is a change to `cy::rendering::GraphExecutor` rather than to this sample.

**Modelled.** The six subsystem costs other than geometry, and what the spike does to them. The
geometry subsystem's authored cost is the frame the device just measured — that substitution happens
only when the nominal state that results still keeps its headroom inside the budget, and *both*
numbers are printed either way so the substitution is visible rather than implied.

## Three things this artefact found

**1. `check_watertight`'s monotonicity test is sensitive to the scale a mesh is cooked at, and the
sensitivity is in single precision.** Cooked at world size — a terrain slab 44 m across — the check
reported **47 monotonicity violations** on a hierarchy the builder had just constructed to satisfy
that invariant. The same shells cooked into a radius of about two report **zero**. The test asks
whether a group's enclosing sphere contains each member's, and `ErrorSphere::contains` allows an
absolute `1.0e-6` of slack, which is far below the rounding of `sqrt(dot(delta, delta)) + radius`
at tens of metres. The measured table, at detail 1.0:

| shells cooked into a bounding radius of | hall | column | statue | facade | terrain |
|---|---|---|---|---|---|
| world size — 12 m to 31 m | 28 | 1 | 0 | 30 | 48 |
| 2.6 m to 2.8 m | 0 | 1 | 0 | 1 | 1 |
| 1.0 m to 1.4 m | 0 | 0 | 0 | 0 | 0 |

The statue is the control: it was already about a metre across at every one of the three settings
and reported zero every time.

So the shells are cooked in their own space and placed by `GeometryInstance::scale`, which is what
an asset pipeline does anyway — `scene.cpp`'s `cook_scale` carries the argument. **The finding
belongs to `src/rendering/virtual_geometry/`**: a containment test with an absolute epsilon is a
test whose meaning changes with the units of the content, and a relative one would not have needed
this paragraph.

**2. The crack-free hierarchy holds to about ten levels on this content and starts failing above
it.** At `--detail 1.4` — 53,868 triangles per shell, eleven to fourteen levels — two of the five
shells report boundary mismatches and open cuts (hall: 3 mismatches, 24 open cuts of 120 thresholds;
column: 4 and 107). At `--detail 1.0` and `--detail 0.5`, all five are clean at every threshold. The
artefact records a **gap** when any asset fails, so running it deeper is how someone reproduces
this, and the default is the depth this content is clean at rather than the depth that hides it.

**3. "The budget is held" has to mean the FILTERED frame time, and the two weaker things it could
have meant both flip on the weather.** A first draft asserted that no single frame exceeded the
budget more than 48 frames after the load changed; a second widened the window to 128. Both passed
on this machine and failed on the same machine when the device's own measured frame came out 0.19 ms
slower. What the arbiter controls is the filtered frame time, driven to `budget − deadband`, and an
individual frame is that plus this scene's ±2 % noise — so a claim about individual frames is a
claim the control law does not make. The artefact now asserts on where the filtered frame *settled*
under the spike, and prints the count of individual frames over budget beside it as a figure.

## What the artefact does not claim

* **It does not shade the visibility buffer.** `gi::IlluminationSystem` is a host system in this
  tree and the ray-query capability its hardware tier needs is not among the fourteen the Vulkan
  backend sets, so the software tier is the default path here. The light act queries the shipped
  system at points on the scene's own surfaces. Coupling it to the resolved pixels needs a deferred
  pass that has nowhere to run.
* **It is not the arbiter's certification.** `design.md` §2.1: a control law over a discrete ladder
  must be certified over a sweep, and that sweep is `integration.render_arbiter_sweep` — 71 step
  magnitudes. This artefact sweeps nine, on a real scene with a measured baseline, which is the
  other question: does the loop hold a budget on content rather than on a model.
* **It does not stream.** Every geometry page is resident. `virtual-geometry`'s residency is its own
  suite's and the geometry cache's; a page table that reported half the scene missing would measure
  the streamer instead of the traversal.
* **Linux only.** Nothing here is verified for Windows or macOS.

## Why the set is generated rather than imported

Every shell comes out of one function, `generate`, over a cube-topology grid whose vertex is a
radius function of the direction. That is closed by construction whatever the radius does, which is
what makes `check_watertight`'s cut sweep meaningful — the check reports "not applicable" for an
open mesh rather than a false pass, so a set of planes and cards would have made the whole first act
vacuous. The five shapes differ only in that radius function: a panelled room, a fluted column, a
displaced boulder, a facade with window recesses, and a ground slab carrying a heightfield.

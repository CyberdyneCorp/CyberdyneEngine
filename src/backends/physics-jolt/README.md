# `src/backends/physics-jolt/` — layer 3

Jolt Physics behind `cy::physics::PhysicsServer`. Task 4.2.2, governed by `physics` — "Jolt as the
3D backend".

Gated by `CY_PHYSICS`, which is **ON by default**: a delivered backend that is off by default is a
backend nothing tests, which is what M3 learned with `CY_RENDERER_VULKAN`. The option also gates the
Jolt fetch in `deps/manifest.toml`, so a build with it off downloads nothing and still simulates
through `src/servers/physics/reference/`.

## The boundary

`include/cy/backends/physics/jolt/server.h` declares two functions and names no `JPH` type. Jolt is
a **private** dependency of the target, so its include directories are not inherited — a file
outside this directory that wrote `#include <Jolt/Jolt.h>` would not compile. `src/servers/physics/`
is layer 2 and cannot include a layer-3 header at all.

What is *missing* is a `physicsapi` check in `tools/layercheck/layercheck.py`, beside `gpuapi` and
`sdl`. The two mechanisms above make the mistake a build failure today; the third would make it a
named diagnostic. It is recommended in this milestone's report.

## The four places Jolt's model and the engine's do not line up

Each is marked in `src/jolt_server.cpp` where it bites, and none of them is papered over.

1. **Filtering is per body, not per collider.** The matrix half maps to an object-layer pair filter
   exactly. The *mask* half is a property of a body, not of a layer, so it is applied in
   `OnContactValidate` using the body's first collider's filter. The reference backend filters per
   collider; a body whose colliders carry different masks behaves differently on the two.
2. **A sensor is a body, not a shape.** A body whose colliders are all triggers becomes a sensor; a
   mixed body is rejected with a diagnostic rather than silently becoming solid.
3. **Contact impulses are not exposed.** Jolt's contact callbacks run before the solver. What the
   events carry is an estimate — relative normal velocity times the reduced mass — which is the
   quantity an impact sound wants and what `contact_impulse_threshold` is compared against.
4. **Contact callbacks arrive on worker threads.** They are collected under a lock and the event
   buffer is sorted by pair and phase at the end of the step, so the event order is a function of the
   simulation rather than of the scheduler.

Two smaller ones, recorded in the code beside the line that drops them: `Tuning`'s
`sleep_angular_velocity` has no Jolt analogue (Jolt folds rotation into one linear threshold), and
the step-time breakdown across broad phase, narrow phase and solve is not published without Jolt's
own profiler, which is deliberately not built — so the total is real and the three parts are zero
rather than fabricated.

## What is not implemented

Constraints, soft bodies and vehicles. Jolt has all three; the mapping is not in M4's task list.
`Capabilities` reports them false and creation fails with `NotImplemented` naming why — which is the
honest form, because this is a gap in the engine's mapping and not a limit of the backend.

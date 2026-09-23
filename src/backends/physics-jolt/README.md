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

`Tuning`'s `sleep_angular_velocity` has no Jolt analogue (Jolt folds rotation into one linear
threshold). The backend now times named Jolt jobs by broad-phase, narrow-phase, and solve/other
work. These phase counters are cumulative CPU nanoseconds; concurrent jobs can make their sum
exceed the wall-clock `total_ns`. They are diagnostic costs, not a partition of wall time.

`debug_draw` reads the simulated body transforms and emits collider primitives (including
world-space triangles for convex, mesh and compound shapes), world-space
broad-phase bounds, contacts, sleep state, linear/angular velocities, centres of mass, joint
anchors, and hinge, slider, distance, cone, swing-twist and six-degree limits. Query overlays use
the stateless `debug_draw_query` helpers in the physics interface: callers pass their input and
optional result, leaving parallel queries read-only.
The island count is derived from active bodies joined by solver contacts or enabled constraints;
static and sleeping bodies are excluded.

## What is not implemented

Constraints are available through `PhysicsServer` on this backend. Fixed, point, hinge,
slider, distance, cone, swing-twist, six-degree, rack-and-pinion, and gear joints map to Jolt.
Joint handles are invalidated when either body or the world is destroyed. Joined bodies do not
collide unless `collide_connected` is set; breaking a force- or torque-limited joint disables it
and emits a `ConstraintBroken` event for that step. Hinge and slider motors can be updated at
runtime; six-degree motors are configured per axis at creation. A zero-frequency position drive
uses a stiff 30 Hz spring in Jolt, not an exact rigid target.

Cloth soft bodies are available from an engine-owned triangle mesh with per-vertex inverse mass;
zero inverse mass pins a corner. Jolt generates stretch and bend constraints and returns world-space
deformed vertices after each step. Cloth uses a normal body handle and contributes its vertex
positions and velocities to the deterministic state hash. The reference backend reports cloth
unsupported. Vehicles and ragdoll animation integration are not yet implemented; vehicle
capability remains false.

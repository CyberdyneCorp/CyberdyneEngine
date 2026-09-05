# `src/servers/physics/` — layer 2

`PhysicsServer`, the engine-owned physics interface; the **reference backend** that was written
before Jolt; the character controller; the fixed-step integration on M2's simulation clock; the
determinism probe; and the 2D API.

**Governed by**: `physics`. Section 4.2 of the M4 tasks.

## The order this was written in is the design

`design.md` §4, and `thirdparty-dependencies`: the interface and a trivial implementation come
**before** the library. M0 did it with `DisplayServer` before SDL3, M3 with the RHI before Vulkan.
`PhysicsServer` was defined and made green over `reference/` before a line of Jolt existed, and the
reference backend is retained — it is what proves at every build that the interface does not leak
the library, and it is why `-D CY_PHYSICS=OFF` still simulates.

## What is here

| | |
|---|---|
| `handles.h` | The five object families, as generational handles, and `UserData` |
| `types.h` | Motion types, materials, collision filtering and the project matrix, backend tuning, world description, capabilities, statistics |
| `shapes.h` | Nine shape kinds, the shared cache key, bounds and mass properties |
| `body.h` | Bodies and colliders, and the degree-of-freedom locks the 2D strategy is made of |
| `events.h` | Contact and trigger events, and the buffer a step fills |
| `queries.h` | Ray, shape cast, overlap and closest point, their one filter, and the mid-step guard |
| `constraints.h` | Ten joint kinds, limits, motors and break thresholds |
| `server.h` | The interface itself |
| `character.h` | The capsule controller, written over the interface's queries |
| `stepper.h` | One step per simulation tick, and the interpolation pair |
| `determinism.h` | The policy, the session validation, and the per-tick divergence probe |
| `components.h` | The ECS component layouts, as plain data |
| `physics2d.h` | The genuinely-2D API over the constrained 3D solver |
| `debug.h` | Debug visualisation, as a sink the caller implements |
| `reference/` | The backend with no library behind it |

## Three decisions worth knowing

**The character controller is engine code, not a backend call.** Every solver has one, and every one
of them has different stair behaviour and a different answer at exactly the maximum slope. `physics`
requires that swapping the backend changes no gameplay — and a character that walks differently
after a swap has changed gameplay whether or not any code moved. So it is written once, over
`shape_cast` and `overlap`, and its suite is a conformance test for both backends.

**Nothing here knows about the ECS or the scene.** `PhysicsStepper` publishes into a `TransformSink`
the caller implements, because `cy::scene::LocalTransform` is layer 4 and this is layer 2. The
component *layouts* are here because they are written in physics' vocabulary; the registration and
the transform publication are a layer-4 bridge, and `components.h` says where it belongs.

**The module is not behind `CY_PHYSICS`.** That option gates the Jolt backend and the Jolt fetch.
An interface that exists in some configurations has a test suite that runs in some configurations —
which is M3's `CY_RENDERER_VULKAN` mistake with a longer tail.

## What is not here yet

* **The ECS bridge.** `src/physics/` at layer 4, mirroring `src/rendering/scene/`: it would register
  the components in a world, create bodies from them, drive `PhysicsStepper` in the `Physics` stage
  and write `cy::scene::LocalTransform` back through the sink. Everything it needs exists.
* **Constraints in a backend.** The vocabulary is complete and validated; neither backend maps it
  yet, and both report `Capabilities::constraints == false` and fail creation with a diagnostic
  naming why — which is `physics`' "Unsupported feature" scenario rather than a silent gap.
* **Soft bodies, vehicles, ragdolls and buoyancy.** Declared in the capability model, unimplemented,
  and reported as unsupported. `physics` scopes ragdolls to `animation-and-skinning`, which is M6.
* **Cross-platform determinism.** Not claimed and not tested; `determinism.h` says so at length and
  `validate_session()` rejects a configuration that assumes otherwise.

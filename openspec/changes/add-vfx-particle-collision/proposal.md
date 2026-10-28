# Add particle collision to CyberVFX

## Why

`vfx-system` requires particles to collide with analytic primitives, the scene signed distance
field and the depth buffer, and to bounce, slide, die and raise events when they do, at a quality
the budget controller can reduce. The requirements map recorded it as `exempt:m11e`: the READ side
existed — `scene_sdf`, `scene_depth` and `physics_query` were declared data interfaces — and nothing
bounced or died on contact. Sparks fell through floors.

## What changes

- **Six collider nodes**, `vfx.collide.plane`, `.sphere`, `.box`, `.capsule`, `.sdf` and `.depth`,
  each with the particle's position and velocity, an optional radius and response (restitution,
  friction, stick, rest speed, the contact flag of the step before), its shape's inputs, and six
  outputs: the resolved position, the responded velocity, `contact`, `began`, `impact` and `normal`.
  In the editor's palette through `vfx.catalogue.get`, with their defaults as inspector properties,
  and composable in reusable `.cyvfxmodule` assets.
- **The IR** gains `Contact`, `Bounce`, `Resolve` and `Impact` (appended — every existing op and cook
  key keeps its value) and `Swizzle` gains its two spellings. The CPU executor evaluates them through
  `collision.h`; the generated Slang carries a collision prelude transcribing the same functions,
  written only for an emitter that collides.
- **The scene field is the GI field.** `GpuGiScene::field_binding()` hands out the clipmaps' page
  table and brick pool; `VfxGpuPass::set_collision` binds them, the frame's depth texture and a
  constants block as descriptor set 1, which only a colliding emitter's pipelines declare. The CPU
  path collides with a `CollisionField` and a `DepthCollisionView` given to
  `SimulationWorld::set_collision`.
- **The budget's `collision_quality` lever** reaches both paths: 3 authored, 2 cheaper normals, 1
  analytic only, 0 none.
- **Events consumed on the CPU path**: `vfx.event_count` (Spawn) and `vfx.event_payload`
  (Initialise), with a per-emitter cursor so two sub-steps do not spawn one event twice, and an
  optional `payload` input on `vfx.emit_event`. `VfxGpuPass::create` refuses an emitter that reads
  events, by name.
- **`VfxGpuPass` can time itself** (`GpuPassDescription::timed`, `last_device_ms`, `last_update_ms`).
- **Two defects** found on the way, each with a regression case: `lower_stage` returned at the first
  event raise and dropped every node after it; the CPU executor did not broadcast a scalar attribute
  or parameter across a vector, so `velocity * scale` disagreed with the device.
- **Tests**: `integration.vfx` and `integration.vfx_compiler` cases, `render.vfx_collision_gpu` (the
  device against the CPU executor for every collider) and `render.vfx_collision` (the pictures).
  `just capture-vfx-collision` photographs, films and times it.

## Scope

A device emitter cannot yet consume another's events: the GPU event chain is one level deep, as
`GPU-to-GPU events` records, so spawn-on-contact runs on the CPU path. Boxes are axis-aligned and
analytic colliders are authored in the effect's space; a host writes a physics body's shape into a
collider's inputs, and nothing does so automatically for every body.

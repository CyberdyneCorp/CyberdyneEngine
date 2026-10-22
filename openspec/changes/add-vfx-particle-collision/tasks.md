# Tasks

- [x] Append `Contact`, `Bounce`, `Resolve`, `Impact` and `EventRead` to the VFX IR; give `Swizzle` its CPU and Slang spellings.
- [x] Add `collision.h` / `collision.cpp`: the contact for every collider and the three responses, the `CollisionField` and `DepthCollisionView` the CPU path reads, and `SimulationWorld::set_collision`.
- [x] Register the six collider nodes and the two event reads; lower a collider's six outputs over one memoised contact; give `vfx.emit_event` an optional `payload`; refuse event reads outside their stage and a fifth consumed channel.
- [x] Emit the collision prelude, descriptor set 1 and the widened push block only for an emitter that collides; hold the plume's generated units and cook key to `main`'s bytes.
- [x] Add `GpuGiScene::field_binding`; bind the field, the depth texture and the constants in `VfxGpuPass`; push the budget's collision quality; time a sub-step and its update on the device.
- [x] Fix `lower_stage`'s early return at an event raise and the CPU executor's scalar broadcast, each with a regression case proven red on the old code.
- [x] Add the CPU, compiler and module cases, `render.vfx_collision_gpu` and `render.vfx_collision`, and `just capture-vfx-collision`.
- [x] Prove every new case red by a mutation, and record it in `evidence/`.
- [x] Update src/vfx/README.md, the GI GPU README, `docs/guides/vfx-collision.md`, the requirements map and the living specification, and validate this change.

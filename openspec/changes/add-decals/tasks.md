# Tasks

- [x] Make `decal_application_order` a free function and have `DecalBudget::application_order` use it.
- [x] Bound a cluster element by an oriented box (`ClusterElement::oriented_box`, `oriented_box_touches`), and test it against the sphere it refines.
- [x] Hand a view's decals to `FrameAssembly`, rank them, and assign them beside the lights as `ClusterElementType::Decal`, filtered by their channels; report `decals` and `decal_assignments`.
- [x] Carry an instance's layer mask to `GpuDrawInstance::layer_mask`, and a survivor's flags and layer mask through `apply_gpu_cull`, each with a regression test.
- [x] Add `cy::rendering-decals`: `pack_decal_table`, `DecalTableTexture` (outside a frame and declared into one), `write_decal_frame`, and `unit.rendering_decals`.
- [ ] Add `cy/decal.slang` and apply it in `cy/frame.slang`'s forward fragment before the light loop; append `decalControl`; regenerate the committed SPIR-V and MSL.
- [ ] Add `render.decals` and prove each case red by a mutation.
- [ ] Pin the frame without a table to a committed reference rendered by the pre-change frame shader.
- [ ] Apply decals in `samples/12-beauty` (scorch and moss) and `samples/10-world` (a ground marker), and publish before/after images.
- [x] Update the READMEs and the requirements map, and validate this change.

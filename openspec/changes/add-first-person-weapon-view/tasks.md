# Tasks

- [x] `cy::rendering-viewmodel`: the squeezed projection, `guaranteed_clear_distance`, `settings_from_views`, `assembly_view`, `ViewmodelFrame` and `write_viewmodel`; `unit.render_viewmodel`.
- [x] `AssemblyView::viewmodel`, the weapon view's own cull, the dropped layers, the mark and the shadow policy; the device-cull refusal; `integration.viewmodel_assembly`.
- [x] Append the viewmodel rows and policy word to `CyFrameData` / `FrameViewData`; the four vertex entries; `frameClusterPixel` and the contact policy in the forward pass; regenerate the frame, particle and strip headers.
- [x] `PipelineSetup::viewmodel`, `FramePipelines::viewmodel_pipeline`, the recorder's weapon draws and `RecorderReport::viewmodel_draws`.
- [x] `previous_camera_relative_view` split out of `previous_camera_relative_to_clip`.
- [x] `render.weapon_view`: off against the frame before, the wall, the near plane, the composition golden, the lighting against world geometry and under two world lenses, the motion vectors, the shadow policy and the 1080p timing.
- [x] `cy::camera::FirstPersonCamera`, `step_spring`, `produce_weapon_view`, `ViewPurpose::Viewmodel`; cases in `unit.camera_system`.
- [x] ABI 1.13, `abi::game::FirstPersonBackend`, the thunks, `unit.abi`'s cases; `cy::game-backend-first-person` and `integration.game_backend_first_person`; regenerate the Swift and Rust overlays and the baseline; CyberdyneKit's `FirstPersonCamera`, `WeaponView` and `FirstPersonTests`.
- [x] Prove the new cases red by mutation, and record it in `evidence/`.
- [x] Update the READMEs, `docs/guides`, the requirements map and the living specifications, and validate this change.

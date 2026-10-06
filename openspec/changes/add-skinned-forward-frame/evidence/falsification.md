# Mutation proofs — `add-skinned-forward-frame`

Each mutation was applied to the working tree, the named targets rebuilt with the dev profile, the
named cases run, and the file restored and its md5 checked against the original before the next.
Every one went red; the restored tree is green.

| Mutation | Where | Red in |
|---|---|---|
| The prepass reads the current window as the previous one (`previous = current`) | `src/rendering/pipeline/src/frame_recorder.cpp`, `bind_skinned_streams` | `render.skinned_frame` (d): `moving.moving > 200` and `moving.largest > 5` fail — the forearm has no motion |
| A skinned draw binds the rigid pipeline (`pipeline(kind)` for `skinned_pipeline(kind)`) | `frame_recorder.cpp`, `record_skinned_draw` | `render.skinned_frame` (b): the golden image differs off every edge; `integration.render_pipeline` "a skinned draw binds the skinned pipelines…": no skinned pipeline bound |
| The pose upload copies the whole world (loop from 0) | `src/rendering/skinning/src/skinned_scene.cpp`, `upload_poses` | `render.skinned_frame` (g): the sentinel staged outside the dirty range reaches the device; `integration.skinned_scene` "an upload writes the dirty range…": matrices outside the range change |
| The selection mask draws a skinned draw from the rigid stream | `src/rendering/selection/src/outline_pass.cpp`, `draw_marked` | `render.skinned_frame` (e): `changed > 100` fails — the mask is the same for both poses |
| The skinning pass records one dispatch for the whole table (`break` after the first) | `skinned_scene.cpp`, `record_skin` | `integration.skinned_scene` "one pass records one dispatch per posed instance": 1 dispatch recorded, 2 expected |
| Creating the skinned variants changes the rigid pipelines' normal format (`skinned || setup.skinned`) | `src/rendering/pipeline/src/frame_pipelines.cpp`, `make_geometry_pipeline` | `render.skinned_frame` (a): the frame with skinned pipelines and no skinned draw differs from `frame_scene_before_bloom.png` |

## A defect found and fixed before review

`cySkinnedDepthVertex` and `cySkinnedForwardVertex` first called `cyDepthVertex` and
`cyForwardVertex`. `slangc` compiles that, and `cy_shaderc build` — Slang through its API, what
`just build-shaders` and `smoke.shader_targets` run — crashed with a segmentation fault in
`translateEntryPointInParamToBorrow` on `src/rendering/shaders`, where main's shaders pass (`cy_shaderc
build --strict` over main's `src/rendering/shaders` reports `failures=0`). The entries now carry the
rigid bodies line for line; `smoke.shader_targets` is the existing regression gate, and the frame's
golden images were byte-identical across the change.

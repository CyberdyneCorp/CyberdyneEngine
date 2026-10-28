# Tasks

- [x] Write the Swift game (`game/`): the hero's movement, jump, animation states and footsteps; the camera director; the ambience; the contract.
- [x] Write the host (`host/`): servers, the level's colliders on Jolt, the adapters, the hero's rig with its blend space and foot placement, the hero's mesh, the sounds, the camera rig and its resolver, the scripted player, the report and the digest.
- [x] Write the level (`content/third-person/hillside.cymap`) and cook it in the build with the RTS map's cook.
- [x] Draw the run (`host/render.*`): the level, the skinned hero, a sky, shadows, AO, the device GI, the temporal upscaler.
- [x] Add `CameraCollisionResolver` and `integration.game_backend_camera_collision`.
- [x] Fix the camera collision node's question and its recovery; fix `CharacterController`'s platform measure; make the device's adaptive probe slots deterministic. A regression test for each, proved red on the old code (`evidence/`).
- [x] Add `integration.third_person_sample` and `smoke.third_person_render` (golden, budget); prove them red by mutation.
- [x] Add `just capture-third-person`; publish the images and the video.
- [x] Update the READMEs, `content/third-person/`, `deps/shipped-content.md`, `THIRD_PARTY.md`, `docs/guides/third-person-demo.md`, the requirements map and this change, and validate it.

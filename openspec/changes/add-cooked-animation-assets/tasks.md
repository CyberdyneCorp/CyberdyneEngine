## 1. Records and the compiler split

- [x] 1.1 `pose_program.cpp` holds the program's runtime half and `assemble_pose_program`; `lower_pose.cpp` the compiler.
- [x] 1.2 `cy/animation/cooked.h`: skeleton, clip (versions 1 and 2) and program records; `Clip::adopt_compressed`.
- [x] 1.3 `integration.animation_assets` round-trips each record byte for byte; `unit.animation_runtime_only` links without the compiler.

## 2. The library

- [x] 2.1 `AnimationLibrary`: load by id, bind by name, refuse a missing clip by name, a foreign clip by joint, a missing asset by id; hot reload of clips.
- [x] 2.2 `integration.animation_assets` cases for each.

## 3. The cook

- [x] 3.1 `cook_locomotion_set`; the importer's clip writer calls the runtime encoder.
- [x] 3.2 The `animation` producer and `integration.build_animation`; `integration.animation_cook` for the seam.

## 4. The sample and the documents

- [x] 4.1 `samples/09b-animated-character` cooks, loads by asset id and animates through the system.
- [x] 4.2 `docs/guides/animation.md`, `src/animation/README.md`, `tools/import/README.md`, `tools/build/README.md`, `requirements-coverage.toml`, `falsifiability.toml`.

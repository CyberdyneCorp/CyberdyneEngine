# Tasks: audio backend architecture

Specification-stage change. Sections 1 and 2 — the specification work and cross-spec
consistency — are complete, and this change was archived on that basis: the decision is recorded
and the specs are updated.

Sections 3 to 5 are **deliberately deferred to implementation changes**. They are listed here so
the work the decision implies is not lost, but none of it can be done before there is an audio
subsystem to do it in. Each will be picked up by the change that first builds the corresponding
piece.

## 1. Specification

- [x] 1.1 Record rationale and rejected alternatives in `design.md`
- [x] 1.2 Delta spec for `audio`: backend abstraction, miniaudio, Steam Audio, asynchronous
      acoustic simulation, acoustic geometry and materials, importance tiers, virtualisation,
      optional middleware, and updated diagnostics
- [x] 1.3 Delta spec for `thirdparty-dependencies`: dependency set, engine-built list, and the
      optional proprietary middleware policy
- [x] 1.4 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `physics` — reviewed; no requirement change needed. Audio consumes geometry through the
      extraction interface specified in `audio`, not by calling the physics server, so physics
      requirements are unaffected.
- [x] 2.2 `build-system-and-platforms` — `CY_AUDIO_STEAM_AUDIO` added to the feature option list,
      with the optional-backend fallback rule made explicit
- [x] 2.3 `core-jobs-and-concurrency` — acoustic simulation added to the job worker role, the
      realtime audio role tightened, and the no-blocking rule between them stated
- [x] 2.4 `swift-scripting` — reviewed; no requirement change needed. The audio components are
      plain reflected value types, already covered by the `@Component` macro contract.

## 3. Dependency scaffolding

- [ ] 3.1 Add miniaudio to the dependency manifest, pinned, required when `CY_AUDIO` is enabled
- [ ] 3.2 Add Steam Audio to the manifest, pinned, optional behind `CY_AUDIO_STEAM_AUDIO`
- [ ] 3.3 Verify licence texts are captured for the attribution report
- [ ] 3.4 Confirm platform coverage of both against the supported and planned platform list,
      recording any gap (Steam Audio platform support is narrower than miniaudio's)

## 4. Interface definition

- [ ] 4.1 Define `AudioBackend` — device lifecycle, callback, conversion, decoding, streaming
- [ ] 4.2 Define `AcousticsBackend` — HRTF, occlusion, transmission, reflections, propagation
- [ ] 4.3 Define the acoustic geometry extraction interface consumed by audio and produced from
      the ECS world, with no direct dependency on the physics server
- [ ] 4.4 Define `AcousticMaterial` and its derivation from physics materials
- [ ] 4.5 Define the importance scoring inputs and tier budget configuration surface

## 5. Validation plan

- [ ] 5.1 Null-backend tests: deterministic playback position advance without a device
- [ ] 5.2 Tier system tests: budget enforcement, hysteresis, pinning, deterministic demotion order
- [ ] 5.3 Virtualisation tests: position continuity across virtualise and restore
- [ ] 5.4 Benchmark: per-frame audio cost at 1 000 / 8 000 / 50 000 sources, with tier budgets held
      constant, as a regression guard
- [ ] 5.5 Fallback test: identical source set with and without Steam Audio produces no missing or
      silent sounds

# Tasks — M12, After 1.0

## 0. The rung exists on the ladder, in all four places

- [x] 0.1 `tools/roadmap/record.py`'s `MILESTONES` carries `m12` after `m11e`
- [x] 0.2 `tools/roadmap/gates.toml` carries `milestone-m12` at `joins-on-close`
- [x] 0.3 `tools/roadmap/milestones/m12.toml` exists and is read by `just roadmap-milestone m12`
- [x] 0.4 This change directory exists and validates
- [ ] 0.5 `m11e:the-ladder-ends-consistently` passes with M12 present — the end of the ladder is
      **consistent**, which is what that check asks, rather than M11.e being last

## 1. Android — the half of mobile 1.0 does not ship

- [ ] 1.1 `platform/android/` implementing `Platform` and `DisplayServer`, beside `platform/ios/`
- [ ] 1.2 Cross-compilation for `arm64-v8a`, and the toolchain recorded the way the iOS one is
- [ ] 1.3 Vulkan on Android — the backend exists; what is absent is the surface and the loader path
- [ ] 1.4 The artefact: `samples/11-ship` packaged and launched on a physical Android device, with
      its coverage manifest saying which device answered
- [ ] 1.5 The criterion is a captured frame compared against a committed reference, not a build that
      succeeded

## 2. `ml-inference` — off Seed, with a consumer

- [ ] 2.1 The row has been at Seed since M8.c and `integration.ai` does not exist
- [ ] 2.2 A consumer that exercises it — `m11b:ml-inference-or-a-deferral`'s first half, which asks
      for the row to be driven by the game's AI rather than by a test that calls the API
- [ ] 2.3 The ONNX Runtime backend registers more than the CPU execution provider, or the
      restriction is recorded with its reason
- [ ] 2.4 Requirements mapped to a test, a gate or a recorded exemption

## 3. `xr-support` — entered through the seam M3 left half open

- [ ] 3.1 **The half-open seam is the entry point, and it is already named.** `m3.toml`:
      *"the half that is NOT open is `cy::Runtime::tick()`, which takes no predicted display time,
      so a host cannot yet tell the engine when a frame will be displayed"*
- [ ] 3.2 `tick()` accepts a predicted display time, or the partial closure is recorded as a
      decision with its cost
- [ ] 3.3 The three M3 prerequisites still pass — two views are **one** submission, the frame's
      inputs are **arguments** rather than a clock, and the view reaches submission **through
      memory** rather than a re-recorded command
- [ ] 3.4 Requirements mapped, or the row is deferred again — deliberately, and with a re-entry

## 4. Records and the gate

- [ ] 4.1 The three rows reach Complete, or each is recorded deferred again with its reason
- [ ] 4.2 `docs/roadmap/capability-matrix.md` carries an M12 column
- [ ] 4.3 `docs/roadmap/ROADMAP.md` describes M12 as after 1.0, not as part of it
- [ ] 4.4 Every criterion this rung adds has been shown able to fail by `just roadmap-falsify`
- [ ] 4.5 The gate

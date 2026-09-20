# Tasks

## 1. Establish the measured path

- [x] 1.1 Make `just run-sample` portable to macOS Bash 3.2 and verify the recipe regression suite passes
- [x] 1.2 Make the world sample register and select native Metal on Apple and Vulkan elsewhere, and verify a missing backend fails explicitly rather than silently producing zero frames
- [x] 1.3 Generate SPIR-V and MSL world shader artifacts from the same Slang sources and verify native pipeline creation and parameter-block binding
- [x] 1.4 Capture the unchanged 64-frame headless and device baselines with adapter, OS, build, resolution, and per-band timings recorded

## 2. Dispatch terrain substrate shading

- [x] 2.1 Add a render-graph compute pass that uploads the four field images, dispatches terrain substrate shading, and exposes a device-resident colour buffer to the terrain draw
- [x] 2.2 Add a CPU/device terrain agreement regression with a mutation that proves the check can fail
- [x] 2.3 Remove synchronous terrain shading from rendered `World::advance()` while retaining the CPU reference for device agreement tests, and verify the terrain band leaves the device frame

## 3. Dispatch visual cloud composition

- [x] 3.1 Add a backend-neutral cloud compute module and render-graph pass whose output feeds the rendered sky
- [x] 3.2 Add CPU/device cloud agreement coverage and prove its tolerance check fails under a known shader mutation
- [x] 3.3 Remove synchronous visual sky evaluation from rendered `World::advance()` and verify the sky band leaves the device frame

## 4. Dispatch visual water foam

- [x] 4.1 Audit foam consumers and add a digest regression proving visual foam does not affect save, replay, lockstep, PCG, or gameplay queries
- [x] 4.2 Add a ping-pong foam compute module and render-graph pass and bind its result to the water draw
- [x] 4.3 Omit visual foam evolution in headless execution and verify rendered and headless authoritative digests agree

## 5. Close the budget

- [x] 5.1 Add dispatch counters and workload-audit failures for missing terrain, cloud, foam, or rendered output, and prove each check can fail
- [x] 5.2 Run the exact Shipping 64-frame take at 960x540 on real Apple hardware and commit the raw report showing a worst frame at or below 16.7 ms
- [x] 5.3 Run the restated headless take and commit its raw report and authoritative digest comparison
- [x] 5.4 Update the M10/M11.a roadmap ledgers and world sample documentation, then verify `openspec validate --all --strict` and the affected test suites pass

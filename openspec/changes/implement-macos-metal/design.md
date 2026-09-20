# Design

## Ownership and inherited decisions

This workstream owns native Metal and macOS editor frame transport. It consumes the RHI decisions in
`implement-m11d5-backends` without rewriting its roadmap or claiming its D3D12 and cross-backend gates.
No MoltenVK path and no toolkit-drawn substitute for engine frames.

## Local evidence (2026-09-19)

- `MTLCreateSystemDefaultDevice()` reports **Apple M3 Pro**, unified memory, argument buffers **Tier 2**,
  and `supportsFamily(MTLGPUFamilyApple1) == YES`.
- `src/backends/rhi-metal/src/device.mm` passes `xcrun clang++ -std=c++20 -fobjc-arc -fsyntax-only`
  against the installed SDK, including its Metal enumerator assertions. Include roots are
  `src/backends/rhi-metal/include`, `src/backends/rhi/include`, and `src/core/{base,memory,values}/include`.
- This is compilation and device-discovery evidence only. No engine frame has rendered on Metal.
- The factory functions declared in `backend.cpp` are not implemented by the seed. Device discovery
  must not be mistaken for a functioning RHI.
- A minimal top-level configure with Vulkan, Slang, DXIL, Jolt, audio and navigation disabled reaches
  `samples/00-empty/CMakeLists.txt`, then fails because `PRIVATE_DEFINITIONS` has no value on macOS.
- `src/runtime/editor_bridge/CMakeLists.txt` explicitly excludes Apple. The C++ viewport publisher and
  Rust viewport transport also gate their implementation to Linux.

The hardware satisfies the re-entry prerequisite for Apple-family and Tier 2 verification in the
existing backend plan. It does not, by itself, close either deferral.

## Implementation sequence

1. Restore a native macOS engine configure/build and link a real Metal factory. Add regression
   coverage for every build fix; do not weaken global module validation to permit malformed targets.
2. Implement resource/handle lifetime, queues and timelines, shader modules, render/compute pipelines,
   descriptors, command recording, presentation, and transient heap aliasing through the existing RHI.
   Report capabilities from supported and exercised paths rather than from device presence.
3. Establish offscreen pixel and compute-readback tests, then swapchain presentation and render-graph
   integration. Exercise Tier 2 argument buffers and memoryless attachments on this hardware.
4. Share engine frames through IOSurface-backed Metal textures and explicit completion/reuse signals.
   Keep platform APIs in the backend/transport crates. Reuse protocol frame IDs, generations, view
   state, and runtime-disconnect semantics rather than creating a second viewport model.
5. Validate resize, runtime crash/restart, and buffer-ring reuse before measuring frame pacing and
   comparing the rendered editor with the design references.

## Decisions to resolve through measured implementation

- Exact IOSurface handoff and Metal shared-event import with the pinned wgpu 30 API.
- The smallest portable runtime-control bridge changes needed on Darwin.
- Shader binding conventions and function-constant reflection needed by the native MSL path.

These are implementation investigations, not reasons to ask the user for the same scope approval.

## Evidence boundary

Record device identity, command, SDK/toolchain, actual image/readback, and validation outcome with each
conformance run. Keep hardware evidence separate from compilation. Missing or untested paths remain
explicitly outstanding; the UI pass does not certify native rendering.

## macOS editor transport evidence (2026-09-20)

The editor transport ran on an Apple M3 Pro (`arm64`), macOS 27.0 build 26A428, using the Command
Line Tools SDK and the engine's native Metal backend. The runtime exports a four-image IOSurface ring
at 960x540. Its announcement page travels over `SCM_RIGHTS`; each plane's existing handle field
carries the IOSurface ID. wgpu-hal imports each surface into the editor's existing Metal device, so
there is no process-boundary pixel copy.

The synchronization selected by measured implementation is publish-after-`MTLCommandBuffer`
completion plus the shared page's held/writing ownership protocol. The consumer performs a bounded
two-millisecond wgpu device wait before retiring its held slot and repeats the prior frame on timeout.
This keeps reuse explicit and bounded, but serializes the producer upload rather than importing a
cross-process `MTLSharedEvent`.

The headless consumer observed 240 distinct nonzero frames in four seconds with zero skipped frames
and zero bounded-wait timeouts. In the real editor, the debug runtime published 2,087 frames over a
40-second session, reported a 16.666 ms mean cadence, zero full-ring drops, zero ownership vetoes,
and 1,119,812 us total host staging/upload time (0.537 ms per published frame). The overall 52.2 fps
includes process startup and connection time.

Restart was exercised in both halves. Two independent Metal consumer processes imported the same
live publisher in sequence and received 180 and 181 nonzero frames; a stale texture registration is
keyed by session epoch as well as ring slot. Two independent editor control sessions then connected
to one live C++ runtime. They sent resize-aware intents for 721x413 and 1183x677 viewports; the runtime
answered both, answered 17 frame-addressed picks, and returned stable identity
`6025500999727375647` for a scene hit. The runtime reported two connections and no unknown messages.

The remaining security boundary is explicit: wgpu 30 imports an IOSurface ID without an XPC object,
so the publisher uses deprecated global IOSurface lookup behind a mode-0600 same-user socket. An
XPC/Mach-port broker is required before claiming hostile multi-user isolation.

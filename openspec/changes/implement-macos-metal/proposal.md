# Proposal

## Why

The editor now opens on macOS, but the engine has only a Metal seed and the hosted viewport path
is Linux-only. The local Apple M3 Pro makes native hardware verification possible while the other
team continues the Linux roadmap.

## What Changes

- Implement the native Metal RHI against the existing device interface and shared validation.
- Exercise Tier 2 argument buffers, memoryless attachments, rendering, compute, readback, and resource
  lifetime on the local Apple GPU; report unsupported features explicitly.
- Add macOS hosted viewport transport using shared native surfaces and explicit producer/consumer
  synchronization, preserving frame identity and stale-frame behavior.
- Resolve the macOS build prerequisites and add platform-specific regression/conformance tests.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `rhi-and-render-graph`: native Metal conformance evidence on Apple hardware.
- `editor-viewport-and-gizmos`: macOS hosted transport lifecycle and frame identity.

## Impact

Primary ownership: `src/backends/rhi-metal`, a macOS viewport publisher, and the macOS side of
`editor/crates/cy-editor-viewport-transport`. Shared build, runtime bridge, and shell integration changes
must remain narrowly scoped. The Linux roadmap team retains milestone ledgers, status files, D3D12,
and Vulkan changes. Do not mark M11.d.5 complete from this workstream.

The user authorized this as the next workstream after UI polish. Implementation is pending; the
initial hardware and compile findings are recorded in design.md.

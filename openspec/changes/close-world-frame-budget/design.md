# Design

## Context

See `proposal.md` for motivation. `World::advance()` currently performs all three expensive visual
producers synchronously. The terrain and field Slang libraries already agree with CPU references,
but no integrated frame dispatches them. The sample embeds only Vulkan SPIR-V and explicitly asks
for Vulkan, although the native Metal RHI and Slang-to-MSL pipeline now exist.

The budget has two distinct claims. A device run measures the complete rendered frame. A headless
run has no visual device work and must measure only the authoritative simulation that a server or
determinism check performs.

## Goals / Non-Goals

**Goals:**

- Make all three named workloads observable RHI dispatches in the integrated frame.
- Preserve one Slang source of truth and commit reproducible backend artifacts for Shipping builds.
- Keep authoritative digests identical between headless and rendered execution.
- Record reproducible real-hardware Metal timing evidence and make each criterion falsifiable.

**Non-Goals:**

- General renderer replacement, D3D12 support, iOS support, or unrelated visual upgrades.
- Moving authoritative weather, water height, terrain generation, or PCG state onto a nondeterministic
  device path.
- Satisfying the budget by reducing the sample's resolution, frame count, world density, or visible
  features.

## Decisions

### Generate SPIR-V and MSL from the same Slang modules

Offline shader regeneration emits both artifacts and the sample selects the artifact matching
`DeviceCapabilities::native_shader_format()`. This keeps backend choice out of shader semantics and
avoids handwritten MSL drifting from Vulkan. Runtime translation was rejected because Metal's RHI
contract deliberately consumes offline-produced MSL and release packages must not depend on the
Slang compiler.

### Model the work as render-graph compute passes

Each producer declares its input and output buffers to the graph, records one compute dispatch, and
feeds a later draw. This lets the graph derive barriers on Vulkan and Metal and gives the criterion
an auditable command-stream shape. Native command-buffer escape hatches and shared-RHI platform
conditionals are excluded.

### Keep CPU references for agreement tests, not for the shipping frame

Tests run compact fixed inputs through both implementations and compare results. The normal device
frame dispatches only the GPU implementation. This proves the work is real while preventing a
reference readback from becoming the measured path.

### Separate authoritative and visual foam

Water height, flow, breaking inputs, and gameplay queries remain CPU authoritative. The foam cache
sampled by the water material is visual state: it is advanced in ping-pong device buffers and is
absent headless. This is the only separation that removes the measured 12.7 ms band without making
save, replay, or networking depend on floating-point GPU behavior.

### Measure CPU wall time around the complete submitted frame

The existing 64-frame harness remains the user-facing gate, including upload, graph execution,
submission, and required output. Per-pass timestamps diagnose regressions but do not replace the
16.7 ms wall-time threshold. Warm-up occurs before the recorded take so shader compilation and
pipeline creation are not confused with steady-state frame cost.

## Risks / Trade-offs

- **[Metal and Vulkan resource layouts diverge]** → Compile the same parameter-block shader sources
  to both artifacts and exercise native pipeline creation and binding on each backend.
- **[Readback hides the GPU win]** → Agreement tests may read back; the measured frame keeps visual
  outputs device-resident and connects them directly to draws.
- **[Removing CPU foam changes authoritative state]** → Add a digest test that compares rendered and
  headless runs and audit every consumer before classifying foam as visual.
- **[One fast machine masks missing work]** → The gate records dispatch counters and fails when any
  workload or rendered output is absent.
- **[Driver scheduling makes wall time noisy]** → Use a fixed warm-up, fixed take, Shipping build,
  declared power state, and preserve the raw per-frame report.

## Migration Plan

Land the portable run-recipe fix and multi-backend world pipeline first. Add the three passes one at
a time with CPU/device agreement checks, then remove each corresponding visual CPU call. Update the
roadmap gaps only after the fixed Metal take passes. A rollback restores the CPU producers and keeps
the generated artifacts unused; no serialized data changes.

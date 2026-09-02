## MODIFIED Requirements

### Requirement: Memory management
The RHI SHALL manage GPU memory through a suballocating allocator with pools per memory type,
dedicated allocations for large resources, defragmentation for transient pools, and budget
tracking against device-reported limits.

GPU memory SHALL be reported into the engine's **memory domain and budget tree** (see
`core-memory-and-containers`) as the `GPU` domain with sub-domains for persistent, streaming,
upload and readback, and transient graph memory — so that GPU and CPU memory are visible in one
model rather than two unrelated reports.

GPU memory pressure SHALL raise the engine's **pressure level** so that streaming and residency
systems respond through the same mechanism they use for CPU memory, rather than each polling the
device budget.

Resource destruction SHALL use the engine's **retirement and epoch** mechanism rather than a
GPU-specific deferral scheme.

Uploads SHALL go through a ring staging buffer; devices with host-visible device-local memory
(unified memory, resizable BAR) SHALL be able to write directly.

#### Scenario: Budget exceeded
- **WHEN** GPU memory allocation approaches the device budget
- **THEN** the engine SHALL raise pressure, trigger streaming eviction through the shared
  mechanism, and fail the allocation gracefully rather than crashing

#### Scenario: Unified memory
- **WHEN** the device exposes host-visible device-local memory
- **THEN** per-frame instance data SHALL be written directly, skipping the staging copy

#### Scenario: One memory report
- **WHEN** memory is reported
- **THEN** GPU memory SHALL appear in the same domain and budget model as CPU memory

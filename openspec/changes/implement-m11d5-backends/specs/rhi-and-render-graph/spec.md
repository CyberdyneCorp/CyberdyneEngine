## MODIFIED Requirements

### Requirement: Shader modules and pipelines
Shaders SHALL be cooked once from Slang into the form each backend consumes natively: SPIR-V for
Vulkan, MSL for Metal, and DXIL for D3D12. A shader bundle SHALL carry the portable SPIR-V form and
an optional native form tagged with its format; the device SHALL select exactly the form reported by
`native_shader_format()` and SHALL refuse a mismatched or missing form before pipeline creation.

Pipelines SHALL be cached by a hash of their full state, and the engine SHALL persist a pipeline
cache across runs. Specialization constants SHALL be preferred over separate preprocessor
permutations where the backend supports them.

#### Scenario: Pipeline cache warm start
- **WHEN** the game starts with a valid on-disk pipeline cache
- **THEN** pipeline creation SHALL be near-instant and no first-use compilation hitch SHALL occur

#### Scenario: Cache invalidated by driver update
- **WHEN** the GPU driver version changes
- **THEN** the cache key SHALL differ and pipelines SHALL be recompiled and re-cached

#### Scenario: Specialization over permutation
- **WHEN** a shader feature can be expressed as a specialization constant
- **THEN** it SHALL be, rather than compiling a separate preprocessor permutation

#### Scenario: A backend receives the wrong native payload
- **WHEN** a shader bundle carries MSL for a device whose native shader format is DXIL
- **THEN** shader-module creation SHALL fail naming the format mismatch rather than forwarding the
  bytes to the driver

### Requirement: Backend roadmap
| Backend | Status | Platforms |
|---|---|---|
| Vulkan 1.3 | Primary | Linux, Windows, Android |
| Metal 3 | Native backend | macOS, iOS, visionOS |
| D3D12 | Native backend | Windows |

MoltenVK SHALL NOT be the long-term Apple strategy. The Metal backend SHALL use Metal directly so
tile memory, memoryless attachments, argument buffers, and Metal GPU capture remain available.

Vulkan 1.3 SHALL remain the minimum Vulkan version, permitting dynamic rendering,
synchronisation2, and timeline semaphores rather than maintaining fallbacks for older versions.

Every backend conformance and golden-image result SHALL name the device that answered and classify
it as hardware, software, paravirtual, null, or unknown from its identity. A backend run on software
or a paravirtual device SHALL be useful API evidence but SHALL NOT be reported as hardware evidence.

#### Scenario: Feature requires a newer version
- **WHEN** a capability requires an extension beyond the baseline
- **THEN** it SHALL be optional and capability-gated, with the baseline path still correct

#### Scenario: A software adapter omits its software flag
- **WHEN** an adapter identifies itself as Microsoft Basic Render Driver but does not set
  `DXGI_ADAPTER_FLAG_SOFTWARE`
- **THEN** it SHALL still be classified and reported as software, because identity rather than the
  flag determines the evidence class

#### Scenario: Three backend images are compared
- **WHEN** Vulkan, Metal, and D3D12 render the committed first-light scene on separate machines
- **THEN** each capture SHALL be compared with the same committed reference and with the other
  captures, and each manifest SHALL name the backend, device, vendor, and evidence class

# Design: portable compute workloads on Metal

## Native shader bundle

The RHI already declares which shader form a device consumes and accepts SPIR-V or one native byte
payload. A small `ShaderModuleBundle` groups the checked-in forms of one entry point and selects
exactly one into `ShaderModuleDescription`. It owns no bytes and performs no runtime translation.
Missing payloads fail with `Unsupported` before a backend or driver is called.

The bundle complements the cook-time `shader::TargetArtefact`: the latter owns one compiler result;
the bundle is the shipping-side view over the checked-in results for all supported targets.

## Metal resource layout

Metal descriptor sets are argument buffers bound at the set index. Slang compute sources therefore
group each set in a `ParameterBlock`; push constants occupy the buffer index immediately after the
last set, matching the Metal command encoder. The SPIR-V interface remains set 0 with the same
bindings, so Vulkan descriptor layouts and tests do not change.

## Skinning

The skinning source is compiled to SPIR-V and MSL from one Slang entry point. `SkinPass` chooses the
payload from `DeviceCapabilities::native_shader_format()`. Existing buffer comparisons remain the
oracle; a Metal device run must compare matrix, dual-quaternion, blend-shape, offsets, and double
buffering cases against `cpu_reference_skin`.

## VFX

The reset, compact, and sort programs receive checked-in MSL siblings. Generated emitter kernels
are represented by a target-neutral cooked bundle rather than a SPIR-V-only field. Development
builds compile the generated Slang source for the active target; shipping builds provide the cooked
form. GPU state remains device-local and dispatch sizes remain GPU-authored.

## Non-goals

This change does not add a new animation evaluator, alter particle behavior, introduce CPU fallback
as Metal evidence, or build the final combined iOS presentation scene. That scene follows after the
two engine paths are independently correct on Metal.

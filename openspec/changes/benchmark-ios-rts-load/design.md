# Design: iOS RTS load

## Skinning batch

The sample repeats the 36-vertex humanoid mesh 500 times in one `SkinPass`. Every repeated vertex
is processed by the Metal skinning kernel, producing 18,000 device-local output vertices. The
models share the five-bone walk pose, as units sharing an animation clip do, while the vertex
shader places each model in a 25 by 20 formation. This measures the requested vertex skinning load
without creating 500 duplicate pipelines and descriptor sets.

## Independent VFX residency

The sample owns 100 `VfxGpuPass` instances. Each instance has its own particle, liveness, free-list,
counter, event, and indirect-argument buffers and declares four simulation dispatches per frame.
The presentation pass reads all 100 particle/liveness pairs and draws every emitter at a distinct
position. At 512 slots per emitter, the fixed-capacity path submits 51,200 particle instances and
400 VFX dispatches without reading a live count on the CPU.

## Measurement

The existing physical-device runner accepts a zero FPS floor for a capacity measurement but still
requires continuous presentation and enough samples. The workload marker is exact: 500 models,
18,000 skinned vertices, 100 emitters, 51,200 resident particle slots, 400 VFX dispatches, and zero
CPU particle readback. Evidence records the measured median and range without claiming the normal
55 FPS presentation gate applies to this stress tier.

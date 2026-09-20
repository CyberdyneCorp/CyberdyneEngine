# Design: combined iPhone compute presentation

## One frame graph

Each displayed frame imports the swapchain image, declares `SkinPass`, advances and declares
`VfxGpuPass`, and then declares one graphics pass that reads the skin output as vertex data and VFX
storage as vertex-stage storage. The graph derives both compute-to-graphics barriers. Presentation
remains the final side effect.

The mobile scene schedules its short skinning and VFX dispatches on the graphics queue. They are
consumed immediately by the presentation pass, so a separate compute queue adds a cross-queue
rendezvous without exposing useful overlap on the Apple tile GPU.

Skinning owns one pass and one pose buffer per Metal frame slot. `begin_frame()` selects the pass
only after that slot's completion value has retired, so the CPU can upload the next pose without
racing an in-flight dispatch. VFX state remains persistent and ordered on the same Metal command
queue; its immutable parameter buffer is uploaded once. This permits normal frames in flight
without draining the device after every presentation.

## Character

A small authored-in-code humanoid uses rigid four-weight records over five bones. A procedural walk
pose rotates arms and legs around their joints. `SkinPass` writes the positions consumed by the draw;
the CPU uploads only the pose matrices and never writes the displayed output stream.

## Particles

The sample cooks the established spark-plume graph at startup and supplies its checked-in MSL
kernel to the shipping VFX pass. A vertex shader reads position and liveness directly from the
pass's device-local buffers and expands each live slot into a camera-facing quad. Fixed-capacity
instancing avoids reading the live count on the CPU.

## Evidence

The device runner requires a `CY_IOS_COMPUTE` marker that reports non-zero skin vertices, particle
capacity, VFX dispatches, and GPU particle instances. FPS is sampled from successfully presented
frames on the physical device. The combined screenshot and evidence use distinct paths from the
terrain-only baseline.

The combined mobile tier uses a 42% linear render scale, three terrain-noise octaves, and 32 ray
march steps. Those settings keep the terrain, day/night cycle, skinning, and VFX visible while
holding the 55 FPS device gate; the higher terrain-only tier saturated drawable production once
the compute presentation was added.

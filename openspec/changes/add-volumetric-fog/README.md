# add-volumetric-fog

Volumetric fog fills a froxel volume with a height fog and authored fog volumes, lights it with the
frame's own sun through the directional shadow map and with its ambient term, integrates it front to
back, and every surface is seen through it. Where an occluder stands between the sun and the air, the
froxels behind it scatter no sunlight: the light shafts between the Colonnade's columns.


In `samples/10-world` the same march writes the atmosphere's own froxel table with the fog
composited in, so a morning mist lies in the valleys under the air that was already there.

**Status: written, not yet built.** The fog shaders' embedded SPIR-V and MSL (`fog_spirv.h`,
`fog_msl.h`) and the frame shader's regenerated headers are not committed, so `cy_rendering_fog`
does not compile yet; the off reference, the device cases' mutation proofs and the pictures are
open tasks (3.1, 4.2, 5.2, 5.4, 6.1). The requirement stays exempt until they are done.

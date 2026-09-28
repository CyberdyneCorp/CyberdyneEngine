# add-volumetric-fog

Volumetric fog fills a froxel volume with a height fog and authored fog volumes, lights it with the
frame's own sun through the directional shadow map and with its ambient term, integrates it front to
back, and every surface is seen through it. Where an occluder stands between the sun and the air, the
froxels behind it scatter no sunlight: the light shafts between the Colonnade's columns.


In `samples/10-world` the same march writes the atmosphere's own froxel table with the fog
composited in, so a morning mist lies in the valleys under the air that was already there.

![the Colonnade through volumetric fog](../../../docs/design/images/volumetric-fog-beauty-on.png)
![the same shot with fog off](../../../docs/design/images/volumetric-fog-beauty-off.png)

**Status: built and run on Vulkan.** `render.volumetric_fog`, `unit.rendering_fog`,
`integration.rendering_fog_march` and `integration.rendering_fog_air` pass in the debug and dev
profiles, as do the frame suites the regenerated frame shader feeds. With fog off the frame is
byte-identical to a reference drawn by the frame shader from before the change. Each device case was
proved red by a shader mutation (`evidence/falsification.txt`). The Metal MSL is generated and not
run. The pictures are `just capture-volumetric-fog` and `just capture-world-fog`.

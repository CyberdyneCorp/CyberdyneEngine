# Design: the RTS test map

## The map is a source, a cook and a package

The source is text because a reviewer should read a map change as a diff. It names engine objects
— modifiers, layers, species, rules — rather than pixels, so the engine computes the terrain and
the forest and the source stays a few kilobytes. The cook is a pure function of the source, the
cook's code and three images already in the tree, and writes fixed asset ids: a minted id would buy
nothing and would make two cooks differ.

Each asset is its own payload (layout, terrain, ground mesh, albedo, kit, skinned worker, foliage,
three textures), each with a magic and a version after the engine's cooked-asset header, so a
loader that reads a wrong kind, a newer format or a truncated payload says which. The header's
content hash is checked on load; a damaged payload is refused before a decoder reads it.

## The terrain is the editor's

The map's ground is evaluated by `terrain::evaluate_region`, the call the editor's terrain tools
preview with, over a `ModifierStack` the source's statements build in order. A `brush` statement is
the modifier the editor records for a stroke, its listed points the stroke's corners and its dabs
spaced a third of a radius along it. The region's metres start at its corner; the cook converts the
source's world metres once.

## The bake

One albedo for the whole map rather than a splat shader: the engine's standard material samples
one base colour texture, and a map drawn with the engine's own frame should not need a material of
its own. Each texel blends the painted layers of the four quads round it (bilinearly, its lookup
warped by noise so a painted edge is ragged), then the slope's rock and the shore's sand over them,
each layer's detail from a content image or from noise. 2048 texels over 128 m is 6 cm a texel.

## The forest

`foliage::generate_region` over the cooked tiles through a `TerrainQuery`, with the source's rules
and zones. A road declares its layer as suppressing foliage (`ModifierStack::declare_decoration`);
the placement rules read no road field here, so the cook asks the surface each instance stands on
and drops those on such a layer. The clusters keep the engine's sixteen-byte records verbatim.

The renderer merges the plants into one mesh a 32 m square, species and tint bucket: the frame
records a draw per item, and a draw a plant would be thousands.

## Drawing it under the session

`MapRenderer` owns the frame the way `pipeline_test::FrameScene` does, generalised: any number of
meshes and parts, a material per part from the map's table (plus the team colour's variants), the
shadow map imported per frame, the AO, GI, fog and UI stages, and the skinning pass declared before
the frame with every geometry pass reading its output. The game's camera drives the view; the
overview is a scripted orbit for the video. Units are skinned instances of one mesh: the host's
animation system's matrices for the workers, the rig's clips sampled here for figures the game
does not animate.

The level's GI keeps its own boxes and adds the map's buildings as boxes, before it is built, so
the GI and the frame see the same buildings. That changes the light the Swift game reads, and only
that: the smoke test holds every other line of the report to the headless run.

## The sky irradiance

Measured while budgeting the map's frame: `FrameAssembly::update_sky` re-integrated the sky's
irradiance every frame (32 directions, serially), 4.6 ms of a 5.5 ms assembly, while the sky's own
tables were already reused. The irradiance depends on the atmosphere, the eye's planet-centred
position and the sun; when all three are exactly what they were it is reused. Exact comparison,
so the reused value is the integrated one bit for bit and a moving sun integrates every frame as
before.

## The budget

At 1920x1080 on the RTX 5060 a frame is held to 16.7 ms on average, measured as the wall time from
recording to the readback with the validation layer off — the processor's and the device's halves in
series, so the bound is conservative for a host that overlaps them.

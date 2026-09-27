# Design

## Per-object motion: the rows the bindings uploaded last frame

Every caller of the pipeline layer already hands `FrameBindings::upload` this frame's instance rows.
Last frame's are the rows it handed the frame before, so the bindings keep them
(`pipeline::InstanceHistory`) and nobody else has to: that is the requirement's "without per-system
effort" as a structure rather than a request. The previous rows are written into the SAME instance
buffer after the current ones — the ring is sized twice as deep — so the view set's layout, and every
shader compiled against it, is unchanged; `motionControl.x` in the frame block says where they begin.

Rows are camera-relative (design.md section 3), so a kept row is rebased by the camera's
displacement before it is written: `relative_now = relative_then - (camera_now - camera_then)`.
`upload_for` takes the displacement and the cut flag from the temporal framework, which already holds
both. A still camera rebases by exactly zero, so a still frame's previous rows are its current rows
bit for bit.

A row is keyed by its GPU scene slot and confirmed by an optional stable identity
(`FrameUpload::instance_ids`): a slot handed to a new instance is a new instance, whose previous row
is its current one. A history cut makes every row new. Neither case invents motion; both carry the
camera's alone, which is what the requirement's "rather than smearing" asks of a surface with no
history.

## The prepass: previous placement, previous vertices

`cyDepthVertex` pushes each vertex through its previous placement and through its PREVIOUS POSITION,
a third vertex attribute. For a rigid mesh the recorder binds the position stream there a second
time, so the value is the current one bit for bit. For a mesh deformed on the device —
`skinning::SkinnedBuffers` keeps both frames in one buffer by parity — the lookup names the other
half through `DrawGeometry::previous_vertex_offset`. Vertex-buffer offsets cannot be negative and
the previous half lies below the current one on every other frame, so the recorder draws from the
smaller of the two starts and offsets each binding to its own half.

With no previous rows (`kCyNoPreviousInstances`) the previous relative position is
`transformToRelative(instance, modelPosition)`, the expression the prepass always pushed through the
previous projection: off is the frame from before, and `render.motion_blur` pins it to references
drawn with the pre-change SPIR-V.

## Motion blur: three dispatches at step 8

Tile max, neighbour max and gather, after the temporal resolve (which it reads and whose history it
never touches) and before bloom (so a streak of a bright light blooms as that light). The gather is
McGuire et al.'s reconstruction filter: a nearer tap covers the pixel as far as its own streak
reaches, a farther one only as far as the pixel's own streak uncovers it, and two about equally near
are blended where both streaks cover the tap. A still background's radius is half a pixel, so it
reaches nothing and is never smeared; a still foreground is never covered by a farther moving
background.

The blur vector is half the shutter-open motion — the shutter is centred on the frame's instant.
The open fraction is `360 * shutter_seconds / frame_seconds`, taken from the same shutter time the
exposure uses (`settings_for_camera`), and `motion_blur_length` in the post module is the arithmetic.
The camera's share of a pixel's motion is where last frame's camera saw the pixel's depth; the
remainder is the object's, which is how the two scales separate. A texel the prepass left cleared is
the sky, and moves with the camera alone.

A pixel whose neighbourhood blurs less than half a pixel is copied, not reconstructed, which makes a
closed shutter byte-identical to no stage.

## Free parameters

`max_radius_pixels` (24) is the tile edge and the longest radius: a cost bound, because the 3x3
neighbourhood reaches one tile. `samples` (15) is the line integral's quadrature. `soft_depth_metres`
(0.05) is the depth within which two taps are one surface, a precision and thickness tolerance. None
of the three is a physical quantity and each is named as such.

## Backends

Vulkan is built and tested. The three dispatches' MSL is generated and committed with the depth bound
as `DepthTexture2D`, and the frame's MSL is regenerated; both are compiled by Slang only on this host.
D3D12: every entry compiles to DXIL through `just build-shaders --strict`, and no DXIL is embedded.

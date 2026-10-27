# Design: the first-person weapon view

## One frame, not two

A weapon view composited from a second frame has no shadows, no fog, no GI and motion vectors the
temporal resolve does not agree with. Drawing it into the world's frame gives all of those for
nothing; what has to differ is only the projection.

## The depth slice

Reversed-Z puts the world's near plane at depth 1 and every world surface at `near/d` or less. The
viewmodel's own projection is squeezed by `z' = (1 - s)·w + s·z` in clip space: its x, y and w rows
are untouched, so the weapon lands on the texels its own lens puts it on, and its depth is in
`[1 - s, 1]`. A world surface beyond `near / (1 - s)` has depth below `1 - s` and is behind every
weapon fragment — with the default `s = 1/16` and a 10 cm near plane, anything past 10.7 cm. The
weapon keeps 2^20 distinct depths in its slice, so it orders itself.

Screen-space effects read the squeezed depth as a weapon flattened against the near plane. That is
why contact shadows are off for it by default; ambient occlusion and the screen-space GI resolve see
it as a thin shell at the eye and sample there, which is the world's light at the player's head.

## Where it is lit

The vertex stage hands the fragment stage the position the instance row put the weapon at — the
camera-attached world position — not the position the squeezed depth reconstructs. The light loop,
the shadow map lookup, the fog and the irradiance volume all use it. The cluster grid tiles the
world's projection, so a weapon surface's cluster is found by projecting that position through the
world's rows (`frameClusterPixel`); a position outside the world's frustum or nearer than its near
plane walks every light, the frame's declared fallback.

## Culling and marking

The assembly culls the world view without the weapon layers and drops any survivor on one of them,
then culls the weapon view against its own frustum (uncompressed projection, finite far plane). Its
draws are marked `kInstanceViewmodel`; the shadow policy clears `kInstanceCastsShadow` and
`kInstanceReceivesShadow`, which the shadow pass and `directionalContribution` already read.

## Motion

The previous rows are last frame's viewmodel projection over last frame's view rebased by the
camera's displacement — the world's rule — so per-object motion of the camera-attached rows cancels
the camera's turn exactly. `ViewmodelFrame::reset` forgets the last lens when a weapon is swapped.

## The behaviours

Every behaviour is frame-rate independent by construction: the bob's phase advances by distance,
the sights and the bob's amplitude are half-lives, and the recoil spring is stepped by the exact
solution of `x'' + 2ζω x' + ω² x = 0`. The look's sensitivity is applied in the controller — the
requirement's "rig parameter" form of the one place — so the look action carries no sensitivity
processor.

## What is not built

A material variant's vertex stage projects through the world's rows, so a weapon draw ignores a
`DrawPipelineFn`. The selection mask, virtual geometry and the device cull do not know the weapon
view. Depth of field reads the squeezed depth as the near plane. The host's first-person rig — the
eye at the character's head, the weapon's rows every frame — is the embedder's; no sample drives it
yet.

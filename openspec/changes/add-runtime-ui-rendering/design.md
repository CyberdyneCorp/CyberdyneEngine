# Design: CyberUI on the device

## One pass at the interface stage

The frame already declared `UiAndDebug` after the post-process and the selection outlines, as a
single pass with an empty callback. The renderer needs to own its declaration — stage 2's opacity
groups and blur-behind add passes — so the stage gains the `FrameStageDeclaration` seam the outlines
use, and a frame with no producer is unchanged. The pass draws over the colour the chain ended in,
so an interface colour is the output's bytes: `ui-system`'s "after tonemapping in display-referred
colour".

## One shader, one row, one draw per batch

`flatten()` batches break on material, atlas, clip and transform, so a batch is exactly what one
draw can cover. Each batch is one `draw_indexed_indirect` of an instanced quad, its clip a scissor,
its atlas page a descriptor set. The rows (64 bytes: bounds, uv, radius and border, two colours,
material, page) and the indirect arguments are host-visible buffers per frame in flight; they are
not graph resources because nothing on the device writes them.

The fragment is a signed distance to a rounded box in pixels: a pixel centre half a pixel inside is
covered fully, half outside not at all. A whole-pixel rectangle therefore lands byte for byte, and a
border is the box minus the box inset by its width. The quad is grown by a pixel so a fractional edge
is anti-aliased at all, and a pixel nothing covers is discarded, which is what makes "nothing to
draw" byte-identical.

## Opacity

Opacity multiplies down the tree into the premultiplied colours at flatten time. That is not the
offscreen opacity group `ui-system` asks for — overlapping children of a faded panel darken their
overlap — and the group is stage 2.

## Text without a font importer

`TextPainter` is `cy::ui`'s two content interfaces implemented over `TextServer`, so `cy::ui` still
links no font server. The font is a public-domain bitmap grid compiled in as data, drawn at
whole-number scales with point sampling; it is replaced by a cooked font when #86 lands. The atlas is
rasterised whole at start because a glyph's uv is normalised against the atlas extent when it is
painted, and growth between two glyphs of one frame would move the first.

## Tests

`render.ui` pins the frame with no interface to a reference drawn with main's frame code before the
seam existed, compares the device with `draw_reference` to one 8-bit step, checks order, nested
clipping and opacity byte for byte, counts one draw per batch, and holds the HUD and console to a
golden image. Each case was proven red by a recorded mutation (the change's PR description lists
them).

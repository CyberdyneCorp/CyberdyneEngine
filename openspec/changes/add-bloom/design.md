# Design

**Every level is a graph texture.** A mip chain in one texture would make every pass declare a
subresource range; separate transients make every pass read and write whole resources, so the graph
derives each barrier from the simplest declaration it has and the levels alias with the rest of the
frame.

**One stage kind, many graph passes.** `FramePassKind::Bloom` carries one callback; every pass of
the chain records through it, and `BloomChain::step_of(context.pass)` hands the recorder the step:
its kind, its inputs, its target and both extents. The frame keeps the structure and the caller
keeps the drawing, which is the division `ForwardFrame` already has.

**Pass resources at set 2.** Bloom's pipeline layout names the frame's sets 0 and 1 and its own pass
set at 2, so it has the engine's set convention and Metal binds it at the index the RHI uses.
`embed_bloom.py` moves Slang's compacted Metal buffer indices to those slots and fails if Slang
compacts them differently.

**Coordinates from the pixel.** Each fragment derives its texture coordinate from `SV_Position`, and
the composite reads the scene with `Load`. An intensity of zero is then the scene bit for bit, which
is what lets the frame with bloom at zero be compared texel for texel with the frame without it.

**The Karis weight in thresholds.** `1 / (1 + luma / threshold)`, so its suppression does not depend
on the scene's physical scale.

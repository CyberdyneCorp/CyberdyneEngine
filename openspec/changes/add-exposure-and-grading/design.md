# Design

**Grading is at step 12, on display-referred colour, in a log encoding made for that range.** The
specification puts the LUT after tonemapping and asks for it in log space "so the LUT has adequate
precision in shadows". `grading.h`'s `log_encode` spans eighteen stops of scene light; after the
curve every value is in [0, 1] and eight of those stops would index nothing. `display_log_encode` is
`log2(1 + x / 2^-12) / log2(4097)`: exact at 0 and 1, linear below the toe so black is a lattice
point, and logarithmic above it — thirty-two intervals over twelve stops at 33³.

**The table stores encoded outputs.** Interpolating the encoded value and decoding once is exact for
any grade linear in the encoding — the identity, a channel permutation, a gain above the toe —
where interpolating display-linear outputs across a 0.375-stop cell is wrong by up to 0.8 %, two
8-bit steps at white. The texture is `Rgba16Sfloat`, whose rounding of an encoding near 1 is 0.2 %
of a display value: half a step.

**A `.cube` is read in its own encoding.** An exported creative LUT is indexed by sRGB-encoded
values; the bake decodes the engine's lattice to display-linear, applies the parametric grade,
re-encodes into the file's encoding, samples the file and decodes. Any file size becomes one engine
table, and the runtime still does one lookup.

**An identity is not applied.** `set_lut` recognises a table that bakes to the identity and leaves
the lookup off. With no table and a pushed exposure the graded resolve is `fullscreenResolve` — the
same `applyExposure` and the same curve, in the same order — so a neutral grade draws the ungraded
frame byte for byte, which the device suite asserts rather than bounds.

**Metering is one frame behind.** The histogram reads the frame's scene-referred colour before bloom
(step 6: the temporal history when there is one, else the shading target), and the adapted EV is
written to a one-`float4` state the NEXT frame's resolve reads. A frame never waits on its own
metering; the graph orders the resolve's read of the state before the adapt pass's write through
their declarations. On a restart (the first frame, a cut) the starting EV is pushed to the resolve
and the adapt pass starts from it.

**One thread adapts.** Two walks over 256 bins are microseconds; a parallel prefix would be the same
answer with a summation order the host twin could not follow. The device's target and adapted EV
are compared with `metered_ev100` and `adapt_ev100` of the same histogram to 1e-3 EV.

**Exposure units.** A manual or metered EV100 becomes stops by `exposure_stops_for_ev100`:
`-EV100 - log2(1.2)`, the saturation-based multiplier `1 / (1.2 · 2^EV100)` that `exposure.h`
already defined. The frame's globals still carry stops, so an EV reaches the frame's own resolve
without it changing.

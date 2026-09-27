# `src/rendering/grading/` — layer 4

Exposure and colour grading on the device: steps 6, 10, 11 and 12 of `rendering-post-processing`'s
chain. The arithmetic is `src/rendering/post/`'s; this module is the three compute dispatches and
the resolve that run it on a frame.

**Governed by**: `rendering-post-processing` — "Exposure", "Colour grading", "Chain order and colour
space". OpenSpec change `add-exposure-and-grading`.

## The files

| File | What it holds |
|---|---|
| `grading_renderer.h` | `GradingRenderer`: the pipelines, the grading table, the histogram and exposure state, the post-process callback and the metering declarations; `exposure_constants`, the push block as a free function |
| `look_file.h` | `load_look` and `load_cube`: a `.cygrade` look and the `.cube` it names, from disk, baked into the table |
| `shaders/exposure_*.slang` | clear, histogram, adapt — each expression twinned in `post/src/exposure.cpp` |
| `shaders/graded_resolve.slang` | exposure, the tone curve, one lookup; `fullscreenResolve` plus two things |
| `shaders/regenerate.py` | recompiles the four and the library's `fullscreenVertex` into `src/grading_{spirv,msl}.h` |

## How a frame uses it

```cpp
grading.initialize(device, allocator, {output_format});      // once, outside a device frame
grading.set_lut(table, look.lut_size, applied);               // once per look; identity -> not applied
grading.set_automatic(settings, starting_ev100);              // or set_manual_ev100 / set_manual_stops

// every frame
grading.import_state(graph);                                  // before assemble
sinks.passes[PostProcess] = grading.post_process();           // in place of the frame's resolve
assembly.assemble(...);
grading.declare_metering(graph, assembly.resources(), w, h);  // after assemble
```

`PostChainConfig::auto_exposure` and `::colour_grading` are the caller's to set, so the frame's
manifest names the stages it ran.

## Four things worth knowing before changing anything here

**Metering is one frame behind.** The histogram reads the frame's scene-referred colour before bloom
(the temporal history when there is one, else the shading target) and the adapted EV lands in a
state the NEXT frame's resolve reads. On a restart — the first frame, a cut — the starting EV is
pushed to the resolve and the adapt pass starts from it. The graph orders the resolve's read of the
state before the adapt pass's write from their declarations; nothing here names a barrier.

**The graded resolve with nothing to do IS the frame's resolve.** Same `applyExposure`, same curve,
same order, same sampler state. That is why an identity grade is not applied (`set_lut` checks
`display_lut_is_identity`) and why render.grading can assert the neutral frame byte for byte rather
than bound it. Change the resolve's arithmetic and that case goes red.

**The table is display-referred and stored encoded.** Indexed by `display_log_encode` — a base-two log
with a linear toe over [0, 1], because at step 12 nothing is above 1 — and each entry is the graded
colour in the same encoding, so the one trilinear fetch is exact for anything linear in the encoding.
`Rgba16Sfloat` rounds an encoding near white by 0.2 % of the display value: half an 8-bit step.

**A `.cube` is read in its own encoding.** sRGB by default, as an exported creative LUT is; `linear`
for a table generated for the engine. The bake resamples it into the engine's table together with the
parametric grade, so any file size costs one lookup.

## What is not here

A metering mask; exposure published to material shaders for emissive units (the globals still carry
stops, and `exposure_multiplier` is the number a material would read); the Custom tonemap curve;
HDR output — the graded resolve writes SDR display-referred colour, and a PQ or scRGB target is the
frame resolve's gap as much as this one's. D3D12: no DXIL is embedded, as for every device pass in
the tree.

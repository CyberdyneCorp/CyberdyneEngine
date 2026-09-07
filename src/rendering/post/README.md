# `src/rendering/post/` — layer 4

The post-process chain, in its defined order, with the colour space each stage operates in.

**Governed by**: `rendering-post-processing`, at **Working** for M7. Task 8.4.

## The files

| File | What it holds |
|---|---|
| `chain.h` | `PostStage` — the order, as data — the colour space of each stage, and `build_post_chain()` |
| `exposure.h` | manual, camera and automatic exposure; the histogram percentile; adaptation with two speeds |
| `tonemap.h` | the six operators, AgX by default, and the output transfer functions |
| `grading.h` | every grading control, the log encoding, and the bake into one 3D LUT |
| `effects.h` | the derivations: AO, froxel fog, circle of confusion, motion blur, bloom |
| `volumes.h` | the post-process volume stack, blended **per parameter** by normalised weight |
| `quality.h` | five levels per stage, each with a declared cost, and the fit against a frame budget |
| `temporal_binding.h` | the seam: the chain's temporal stage asks `TemporalFramework` for its history and never allocates one |

## The order is data, not a sequence of `if` statements

`PostStage`'s enumerator values **are** the specification's order, and `build_post_chain()` walks the
enumeration once appending what is enabled. A chain therefore cannot come out in a different order
than the specification lists, and moving a stage is a diff a reviewer sees rather than a behaviour
somebody notices later.

The alternative is how a bloom that runs after tonemapping gets into a renderer and stays there: by
the time anyone notices, the threshold has been re-tuned around the wrongness and fixing it changes
every scene.

A disabled stage is **absent**, not skipped. With nothing switched on, the chain is exactly three
stages — exposure, tonemap, output encoding — because those are what a frame cannot do without.

### Reconciling M7's task list with the specification

Task 8.4 names the chain as "AO, fog, exposure, DOF, bloom, tonemap, grading, AA, temporal
upscaling". The specification's fifteen steps are the normative order and are what is implemented,
and the two agree once two things are said:

* **"exposure" is the metering**, which is step 6 and therefore before depth of field. The exposure
  *multiply* is step 10, immediately before tonemapping. `tests/test_chain.cpp` asserts both.
* **Temporal upscaling is step 5, not last.** It and TAA are alternatives at the same step, before
  exposure, because they reconstruct scene-referred colour — a neighbourhood clamp over
  display-referred values clamps against a curve rather than against radiance. The AA that comes
  after grading is FXAA/SMAA, step 14. Both stages exist here and both are asserted in their places.

## Five things worth knowing before changing anything here

**The colour-space boundary is tonemapping, and it is asserted as a sweep.** Every stage before it is
scene-referred and every stage after it is display-referred, checked over the whole enumeration — so
a new stage cannot be added on the wrong side without failing the test. Bloom lands on the
scene-referred side, which is what makes its threshold meaningful in physical units; film grain lands
on the other, which is what makes its strength perceptually uniform.

**Auto-exposure meters a percentile, not a mean.** An average luminance is dragged by a window, a
bulb or a specular highlight, and the symptom is an interior that darkens as the camera turns toward
a window. `tests/test_exposure.cpp` measures both halves: a small bright window moves the exposure by
less than a third of a stop, and a window big enough to *be* the scene still moves it by two.

**The circle of confusion is signed.** Negative in front of the focus plane, positive behind it. The
near/far split reads that sign, and a magnitude-only CoC cannot express which side a sample is on —
which is exactly what near-field bleeding needs to know.

**The grading bake has to agree with the reference.** `apply_grading()` is what an artist sees and
`bake_grading_lut()` is what ships; a bake that drifts is a grade that looks different in the editor
and in the build. `tests/test_bake.cpp` compares them over the log-encoded range and holds the worst
relative difference under 6 %.

**Every stage declares a cost.** `rendering-post-processing` asks for it, and M7 `design.md` §2.10
says why it is load-bearing: an arbiter allocating milliseconds over a ladder it cannot price is
choosing blind. The declared numbers are the engine's defaults at a stated reference resolution of
1920 × 1080 — **they are declarations, not measurements of this machine**, because there is no device
in this module. A project that measures its own replaces the table.

## What is here and what is not

No device and no shader. What is here is the arithmetic that decides what each effect *does*, which
is the part that is wrong when an effect looks wrong: the lens equation, the froxel depth
distribution and its inverse, the metering percentile, the tone curves, the grade and its bake, the
soft bloom threshold, the volume blend, the declared costs. Every one of them is checkable without a
GPU.

The gathers, the blurs, the downsample and upsample chains, the histogram compute pass and the
froxel volume itself are shaders, and they belong with the frame's passes in
`src/rendering/forward/`.

Two stages of this chain are consumers of `src/rendering/temporal/` rather than implementations:
`rendering-post-processing` says TAA "SHALL consume the temporal framework… It SHALL NOT implement
its own". `temporal_binding.h` is where that is written down — the temporal stage gets its history by
asking, through a `ConsumerId` only the framework can mint — and **nothing in this module allocates a
history buffer or owns a jitter sequence**, which is the property that sentence is about.
`build_post_chain()` refuses — or asks for — motion vectors for the same reason rather than assuming
them.

The internal resolution an upscaler renders at arrives as a parameter and is never decided here:
`rendering-post-processing` says it "SHALL be a budget allocation held by the renderer budget
arbiter… not an independent controller measuring frame time".

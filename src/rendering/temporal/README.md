# `src/rendering/temporal/` — layer 4

CyberTemporal: the single framework that owns everything accumulating across frames.

**Governed by**: `temporal-rendering`, at **Working** for M7. Task 8.3 — and the word "one" in that
task is the requirement.

## The files

| File | What it holds |
|---|---|
| `jitter.h` | the Halton sequence, the current and previous offsets, the NDC conversion, and pinned mode |
| `motion.h` | `derive_surface_motion()` and the one motion-vector convention, written down once |
| `reprojection.h` | the four history states and the classification every consumer reads instead of deriving |
| `history.h` | history declarations, their provenance, and what each costs |
| `framework.h` | `TemporalFramework` — consumer registration, invalidation detection and broadcast, the single jitter application, and the diagnostics |

## Why "one" is a requirement and not an aspiration

Temporal antialiasing, temporal upscaling, screen-space reflections, screen-space GI, ambient
occlusion, volumetric integration and shadow caching all need the same six things. Specified per
effect they get implemented per effect, disagree about conventions, and each carry their own
history-invalidation bug. M7 `design.md` §5 lists it first among the things that must not be
retrofitted: **five reprojections that disagree about history invalidation is the defect that cannot
be found from a screenshot.**

Three properties of the interface make the single framework the path of least resistance rather than
a rule somebody has to remember:

1. **A consumer cannot get history without registering.** `declare_history()` takes a `ConsumerId`
   that only `register_consumer()` mints, and it refuses an invalid one — so the memory report and
   the invalidation broadcast cannot have a member the framework does not know about.
2. **A consumer cannot apply jitter.** `jitter()` is read-only, `jittered_view_projection()` is the
   one place the offset reaches a matrix, and `needs_jitter` is declared at registration — which is
   what makes "no consumer, no jitter" a computed answer rather than a flag to clear.
3. **Invalidation is broadcast, never polled.** `begin_frame()` invalidates every history in the
   same frame, and it reaches a consumer *as a classification* (`Unrepresentable`) rather than as a
   second flag beside the state. There is no per-consumer notification to miss.

Every test in `tests/test_framework.cpp` registers **two** consumers, because a suite with one would
pass just as well against five separate frameworks.

## The two conventions, and why they are asserted rather than commented

**A motion vector takes a pixel to where its surface was last frame, in normalised screen space,
with jitter removed.** So `history_uv = current_uv + motion`, and a static surface under a static
camera has a motion vector of exactly zero however the projection was jittered.

The direction points at the past because every consumer of a motion vector is reading history;
storing the forward vector means every consumer negates and the one that forgets produces a trail
that points the wrong way. The jitter is removed here and not per consumer because leaving it in
makes a static scene's motion vectors a sub-pixel dither, which reads as camera shake to a
neighbourhood clamp and is the classic source of "TAA is soft and I cannot find why".

Both are sign errors that are invisible in a still frame, so `tests/test_motion.cpp` asserts them
against a case whose two answers differ visibly.

## `view` and `projection` are separate fields on purpose

"The projection changed" and "the camera moved" are different invalidation causes, and a
pre-multiplied view-projection matrix cannot tell them apart. A field-of-view change mid-shot must
invalidate history; walking must not. `TemporalView::view_projection()` composes them where a
consumer needs the product.

## What is here and what is not

No device. The framework decides what the jitter **is**, what a motion vector **means**, **when**
history becomes invalid and what each pixel's history state **is** — and those are the parts that go
wrong. The textures themselves are the render graph's to allocate; `HistoryResource::bytes()` is the
accounting figure `temporal-rendering`'s diagnostics requirement asks for ("history memory in use per
consumer"), not an allocation.

The per-pixel motion vector **pass** is likewise not here: `derive_surface_motion()` is the
arithmetic one thread or one shader invocation performs, and the buffer it writes into belongs to
the frame's prepass (`src/rendering/forward/`, whose `PrepassMode::DepthNormalVelocity` already
exists for it).

# Design: iOS mobile rendering optimization

## Baseline

The signed reference workload rendered at 2556 × 1179 on an iPhone 16 with an Apple A18 GPU. Six
one-second samples ranged from 16.75 to 19.75 FPS, with a 17.30 FPS median. The fragment shader can
execute 92 terrain march iterations; every iteration evaluates six trigonometric noise octaves.
The workload is therefore dominated by fragment cost and native display pixel count.

## Bounded mobile workload

The sample uses a 0.60 linear render scale. `CAMetalLayer` upscales the rendered drawable to the
native UIKit view while the FPS overlay remains a native-resolution UIKit label. This reduces
shaded pixels by 64 percent without changing layout or touch coordinates. A 0.75 first pass reached
60 FPS on a cool device but fell to roughly 40 FPS after repeated profiling runs warmed the phone;
0.60 retains enough margin for the measured thermal state.

The terrain remains procedural and retains mountains, rock/snow classification, fog, stars, the
sun, orbiting camera, and the 28-second day/night cycle. Its noise sum uses four octaves of smooth
value noise rather than nested trigonometric functions, and its intersection loop uses at most 64
steps. These are explicit mobile quality choices rather than silent backend differences.

The scale is fixed for this reference workload so physical comparisons are reproducible. Runtime
dynamic-resolution integration belongs to the renderer budget arbiter and is outside this sample's
scope.

## Evidence

The app emits `CY_IOS_QUALITY` with native and drawable dimensions, linear scale, terrain octaves,
and maximum march steps. The device runner requires that marker and records those values beside the
FPS samples. A dedicated comparison preserves the 17.30 FPS baseline and the optimized run.

Success requires a median of at least 55 FPS across at least eight post-launch one-second samples
over a twelve-second run on the same iPhone 16. This leaves a small margin for a 60 Hz presentation
target while tolerating normal device variance. The screenshot must still visibly contain terrain
and the day/night sky; performance alone cannot satisfy the change.

## Non-goals

This change does not claim that the complete desktop open-world renderer runs on iOS, add temporal
upscaling, or alter desktop quality. It does not replace the renderer's existing budget arbiter.

# `samples/10-world` — M10's artefact

Seven modules, one world, one day.

    just capture-world

writes `docs/design/images/m10-world.png`, `docs/design/images/m10-world-budget.png` and
`docs/design/videos/m10-world.mp4`. It needs a graphics device and **no content at all** — there is
no asset, no mesh and no texture anywhere in this artefact. The world is generated from the seed on
the command line.

![the world](../../docs/design/images/m10-world.png)

## What this program claims

M10 landed `src/environment/`, `src/pcg/`, `src/terrain/`, `src/water/`, `src/foliage/`,
`src/weather/` and the atmosphere half of `src/rendering/sky/`, and every one of them was reached
only from its own suite. This is the first target in the repository that names all seven. What it
demonstrates is not any one of them; it is the ARROW between them, and the arrow is the substrate:

| step | module | what it does here |
|---|---|---|
| generate | `cy::pcg` | a nine-node graph over 576 regions of 64 m produces an elevation raster and a scattered point set |
| adapt | `cy::pcg-adapters` | the accepted points become **foliage clusters**, through `FoliageOutputAdapter`. No entities |
| cook | `cy::terrain` | the raster becomes 36 level-0 tiles plus two derived levels, and **claims** the `soil` field |
| float | `cy::water` | a spectral ocean, and **claims** `water-depth`, `water-distance`, `water-flow` and `water-shore-wetness` |
| blow | `cy::weather` | a climate over the terrain's own elevation, and **claims** `wind`, `temperature`, `wetness`, `snow-depth` and fourteen more |
| plant | `cy::foliage` | **reads** the substrate and terrain's surface back out, places tens of thousands of plants, and **reads** `wind` to move them |
| light | `cy::rendering::sky` | the atmosphere, the volumetric cloud deck weather drives, and the light everything is shaded by |

**None of those modules links another.** `cy::weather` does not link `cy::water`; `cy::water` does
not link `cy::terrain`; `cy::pcg` links neither `cy::foliage` nor `cy::ecs`; none of them links the
renderer. Those absences are requirements made checkable by the link graph, and the wiring has to
happen somewhere — an artefact is where.

Three things in this world are the substrate being load-bearing rather than decorative, and each
one disappears if a producer is removed:

* **The beaches** are `water-distance`, the two-pass chamfer transform `cy::water`'s shoreline
  publishes, read at every terrain vertex and mixed to sand inside 14 m of the water — so the hem
  follows the coast into every inlet instead of following a contour line. **Look for it on the open
  west coast in the video's early frames, not in the still**, and the reason it is faint is itself
  the substrate working: 14 m is three or four vertices of a 4 m lattice, and the same vertex then
  has `wetness` (1.00 all morning, darkening it by nearly half) and `vegetation` (pulling it back
  toward grass) applied over it, both of them `cy::weather`'s. Three producers compose in one colour
  and the wettest of them wins. Delete water's producer and the hem goes entirely, and not by
  accident: `water-distance` declares its default as the FAR end of its range rather than zero,
  precisely so that a world with no shoreline data reads as "no water near here" instead of as one
  continuous beach.
* **The snow** is `snow-depth`, which `cy::weather` accumulates. The run prints it at two probes —
  the world's centre and 600 m WEST of it — and they do not move together: the western probe climbs
  continuously from the start of the take (0.06 m by 05:21, and 0.36 m by 10:21, the hour the still
  is taken in) while the centre reads exactly 0.000 m through the whole morning. That is a westerly's
  windward side accumulating orographic precipitation, from the declared model rather than a painted
  patch, and it is why the left-hand hills in the still are white and the ones behind them are not.
  The centre only goes white when the front arrives at midday, and thaws once it has passed.
  (The number at frame 0 is the one exception and it is a transient: both probes are within a
  millimetre of zero before anything has had time to settle.)
* **The trees lean** because `foliage::evaluate_response()` reads a `ClusterWind` prepared from the
  `wind` field that `cy::weather` published — one field sample per cluster, not per plant. In the
  still they stand nearly upright in a morning whose hour lines read 4-8 m/s; by 15:21, with the
  front's 18 m/s over them, they are bent close to the horizontal. That is what the species'
  declared stiffness does with that wind and not an animation — the lean is the wind field made
  visible, and it is the one thing in this picture a reader can check against a printed number,
  because the hour lines report the wind the response was evaluated from.

## What the rendered path now does on the device

The authoritative world simulation remains on the processor. The visual work that dominated the M10
frame now runs as three render-graph compute passes before the opaque draw:

* terrain vertices sample the four packed environment field images through `cy/field.slang` and
  `cy/terrain_shade.slang`, writing the device-resident terrain colour stream;
* the sky dome is composed by `shadeClouds`, writing the dynamic colour stream;
* a 128x128 foam field evolves in ping-pong buffers and shades the water vertices.

The opaque callback declares those colour streams as vertex reads, so the graph derives the
compute-to-vertex dependency. The sample audits all three dispatches and the rendered manifest;
`--budget-ms` fails if any is absent. Apple builds select native Metal and other supported desktop
builds select Vulkan. SPIR-V and MSL are generated from the same Slang sources and embedded so a
Shipping build does not need a shader compiler.

The geometry path is still deliberately small. It uses this sample's terrain, dome, ocean, and
foliage proxy streams rather than the renderer's mesh/material tables. It has no temporal
anti-aliasing, virtual geometry, GPU grass expansion, underwater pass, caustics, or reflections.
The cloud producer is a seeded layered density march over dome vertices rather than the full
per-pixel spherical-shell march, and foliage wind response remains CPU work. The frame is assembled by
`rendering::assembly` and resolved by `rendering::pipeline` through exposure, tone mapping, and
output encoding; the emitted manifest records those stages.

There is also no STREAMING: the whole world is resident, every terrain tile is meshed at level 0,
and `MeshReport::stitched_vertices` is reported precisely so that a reader can see it is zero. The
streaming binder `src/terrain/`'s README records as its largest gap is still missing, and this
artefact does not stand in for it.

**And there are no RIVERS.** M10 tasks.md 7.1 asks for "terrain with rivers and an ocean"; this world
has an ocean, a fjord coastline and no river. `cy::water`'s `RiverNetwork` is built and tested —
Catmull-Rom spline networks, continuity-conserved discharge, junctions that add their tributaries,
bed profiles, turbulence from curvature and drop — and authoring one needs a centreline somebody
drew. The generator produces a `flow` raster that a river network could be traced from; tracing it is
a step this artefact does not take, and saying so is cheaper than a river that was really a blue
stripe. The other absences of the same shape: no `vfx-system` effect plays in this world (spray at
the shore and snow in the air are both `cy::vfx` work that wants the GPU path M10 section 5 built and
no sample drives), and no plant is ever PROMOTED to an entity, because promotion is a gameplay call
site and this artefact has no gameplay in it.

## The three tasks this artefact answers

**7.1 — the world.** Above. Run it and read the report it prints; every number in it is read back
off the module that produced it.

**7.2 — streamed and persistent.** `World::deform_and_round_trip()` stamps a crater through
`terrain::TerrainDeltaStore`, records it into `world::PersistenceOverlay` as a subsystem blob under
`terrain::kOverlayChannelTerrain`, **clears the delta store and verifies the crater is gone**,
restores from the overlay alone, and compares 289 probes over the crater's footprint against the
surface as it stood before the save — bit for bit, because a height delta is stored and restored as
the same `f32` and a tolerance would hide a lossy round trip. The clear-and-verify step is there
because a restore that MERGED would pass whether or not the overlay held anything at all.

What it does not do is save and resume the world's OTHER state. Weather's fields are declared
`persistent` and nothing writes them to a save; `src/environment/`, `src/terrain/`, `src/water/` and
`src/weather/` all record the same missing piece — `save-and-persistence`'s field-overlay encoding.
The half of 7.2 this artefact answers is the terrain half.

**The world is the seed, and nothing else.** Two runs of this program with the same seed write
byte-identical frames — `frame_0000.png` and the still both hash the same, and the whole report is
identical line for line apart from the output directory in it. `--seed 0x1234` is a different world
in the same breath: 2 183 generated points against 2 581, 37 190 plants against 37 381, and a
different picture. That is `cy::determinism::RandomStream` under every producer here, and it is
worth checking rather than assuming, because a capture that was accidentally constant would look
exactly like a capture that was correct.

**7.3 — the budget as a curve.** `--budget <csv>` writes every frame's cost, per producer, and the
collector plots it.

![the budget](../../docs/design/images/m10-world-budget.png)

**The 60 Hz target now holds on the real Apple M3 Pro Metal path.** The exact Shipping take is 64
frames at 960x540, seed `20260913`, with a 16.7 ms worst-frame gate. The committed evidence records
12.08 ms mean and 14.93 ms worst, with zero RHI validation errors and all three visual dispatches in
every rendered frame. Timing varies with host load, so the executable remains the authority and
fails the run when any frame exceeds the threshold.

The three former CPU bands are absent from rendered `FrameCosts`: `terrain_shade_ms` is zero,
`sky_ms` now covers only the small authoritative lighting integral, and `water_ms` advances the
clock without evolving visual foam. Device submission is therefore the largest rendered band,
followed by the camera-relative ocean patch and construction of the sample's vertex streams.

**On Linux, Vulkan and a Development build the gate was red at M11.c** — 21.8 ms mean and 25.8 ms
worst on an RTX 5060, with the Khronos validation layer and synchronisation validation on, as
`m11a:world-budget-on-a-device` runs it. A sampling profile of the take named the processor costs;
five of them were this program's own and are fixed, none of them by changing an output bit (all 64
frames of the take compare byte-identical, PNG for PNG, against the build before):

| cost | was | now |
|---|---|---|
| the ocean patch resolved every wave train — two hashed-stream draws, a `pow`, a `cos`/`sin` — per train per vertex, and evaluated the vertices the rings share twice | about 5.5 ms | about 0.6 ms: trains resolved once per build, shared vertices copied, rows spread over four workers (`water_ocean_parallel` proves the parallel patch is the serial one bit for bit) |
| the dynamic streams (eleven megabytes a frame) were built in arrays with nine hundred thousand checked pushes, then copied into the mapped buffers a byte at a time, which GCC did not turn into a block copy | about 6.5 ms of `stage_build` | about 0.9 ms: each plant proxy written straight into its own block of the mapped buffers across the workers, each rim angle's `cos`/`sin` taken once, the small sky/star/sea part copied with `memcpy` |
| every plant's wind response evaluated serially | about 1.3 ms | about 0.5 ms: listed in order, evaluated across the workers |
| the first frame of every take faulted in the proxy streams and the plant list, because the warm-up frame runs before the world has any plants | 2 to 3 ms extra on frame 0 | the dynamic buffers and plant storage are touched once while the stage and the world are built |

Measured back to back on the same loaded host (load average about 12), the build before takes
24.7 ms mean and 30.2 ms worst and this one 12.9 ms mean and 15.8 ms worst. Headless went from 10.4
to 4.8 ms mean.

**`stage_submit` WAS MOSTLY THE SKY, INTEGRATED ON ONE THREAD.** Timed phase by phase inside
`Stage::shoot()` (thread CPU time and context switches beside the wall clock), a daytime frame's
7.5 ms of `stage_submit` was 4.7 ms of `FrameAssembly::assemble()`, 2 ms of device wait and under
1 ms of recording and submission. The assembly's time was all `update_sky()`: a `SkyViewTable`
rebuild (1.5 ms) and `sky::sky_irradiance()` over 1 024 upward directions (3.0 ms), both redone
every frame because this take compresses a day into 64 frames and the sun moves about five degrees
a frame, so no cache could have saved them. The spikes were that same work on a thread the host had
slowed: no involuntary context switch in the spiking frames, but the thread's own CPU time doubled,
and so did every other processor band in the same frame. The stage now lends the world's four
workers to the assembly (`Stage::set_jobs` → `FrameAssembly::set_jobs`), and both integrals are
spread over them with every direction written to its own slot and the irradiance summed in index
order, so the answer is the serial one bit for bit (`render_sky_light` and `render_assembly` assert
it, and all 64 frames of the take compare byte-identical, PNG for PNG, against the build before).
The assembly's share went from about 4.7 ms to 1.3–1.8 ms, and `stage_submit`'s mean from about 7
to about 4 ms.

| same host, same command line, alternated with the build before | before: mean / worst | after: mean / worst |
|---|---|---|
| no build of this session's own beside it (load average 7–10 from other work) | 11.4–11.6 / 13.4–14.8 ms | 9.0–9.1 / 10.6–11.6 ms |
| a `-j 8` clean rebuild beside it | 11.8–20.4 / 16.2–29.1 ms | 10.4–14.8 / 20.4–41.0 ms |

**THE MARGIN STILL DOES NOT SURVIVE A BUILD BESIDE IT, AND IT IS NO LONGER THE SUBMIT.** With a
build running, the worst frame is one of two things, and the take now prints the worst frame's own
bands so that a red run says which. Either it is a frame near the horizon, where
`compose_sky_lighting()`'s gradient fit and cloud probes make `sky_ms` about 3.4 ms on a quiet host
and 7 ms on a slowed one; or it is a frame in which EVERY processor band is four to five times its
median at once — weather 8.9 ms against 1.6, ocean 4.5 against 0.6, the stage's build and submit
likewise — which is the process losing its processor, not any one band stalling. Neither is in the
submit path.

**SO THE CRITERION NOW MEASURES ON A QUIET HOST, AND SAYS SO.** The owner's decision: a frame
budget on a loaded machine measures the machine, so `m11a:world-budget-on-a-device` passes
`--quiet-host` and states in its own text the host it assumes. `tools/quiet-host/host_load.h` is the
check, shared since M11.c's seventh close with the ledger criteria that run timing-sensitive test
suites (`just test-quiet-host`). Before the
run does any work — before the world is built, and so before the take — it looks at the host a second at a time, for up to `--quiet-wait-s` (ten minutes by default),
until one window shows every other process together using at most two cores and CPU pressure at or
under 10%. Across the take it looks again a second at a time, with this program's own CPU time
subtracted from the machine's so its own workers do not count against the host, and the take is
judged by its BUSIEST second: one busy second is enough to make the worst frame, and an average over
the whole take would hide it. A host that is not quiet at either point FAILS the run with a
`host too busy:` line that carries the numbers. It is never a pass and never a skip. The first check
comes before the world build and not just before the take because it idles for at least a second:
placed between the stage opening and the take, it let the workers park and the caches go cold, and
frame 0 cost 19-22 ms against 10-11 ms worst without it on the same quiet host. The budget, frame count, warm-up and worst-frame judgement are unchanged.

The bounded wait is there so that a ledger reaching this criterion as something else finishes still
measures. A host that stays busy for ten minutes is reported as busy. There is no retake after a busy
take: the take advances the world, and a second take would be a different measurement. `loadavg` is
printed and not judged, because it counts this program's own threads and takes a minute to decay.
Where `/proc` cannot be read (anything but Linux) the verdict is `host load unreadable:`, and that
fails too.

Headless has a narrower meaning now: it measures authoritative simulation and skips visual terrain,
cloud, and foam work because there is no device to consume it. The same 64-frame take is committed
separately as headless evidence. Save, replay, lockstep, PCG, and gameplay state do not depend on the
visual buffers.

## Reading order

`world.h` first — its header comment is the map. Then `world.cpp`, which is one function per module
in the order the dependencies force. `stage.h`/`stage.cpp` are the renderer and name no module above
`cy::rhi`; `shaders/world.slang` is forty lines and says what it is not.

## Running it

    # the whole thing, as the recipe runs it
    just capture-world

    # the world and its report with no device and no pictures — a second and a half
    build/<profile>/samples/10-world/cy_sample_world --headless

    # a different world
    build/<profile>/samples/10-world/cy_sample_world --headless --seed 0x1234 --regions 16

| flag | what it does |
|---|---|
| `--frames <dir>` | write `frame_%04d.png` per frame |
| `--still <path>` | write one frame a second time, for the committed image |
| `--budget <path>` | write the per-frame, per-producer cost as a CSV, including field publication and device-stage bands |
| `--budget-ms <ms>` | fail when the worst frame exceeds the threshold, a required visual dispatch is absent, or a rendered frame has no manifest |
| `--headless` | generate, cook, claim, place and simulate; draw nothing |
| `--quiet-host` | measure only on a quiet host: wait for one before the take, judge it again across the take, and fail with `host too busy:` when it is not quiet (Linux) |
| `--quiet-wait-s <s>` | how long `--quiet-host` waits for a quiet host before failing. Default 600 |
| `--seconds <s>` | length of the take, which is always exactly one simulated day |
| `--fps <n>` | frames per second of the take. One frame is one simulated tick |
| `--width`, `--height` | refused unless even, for the video encoder's 4:2:0 |
| `--seed <n>` | the world seed. Accepts `0x` |
| `--regions <n>` | regions per side. 24 is the 24x24 grid M10's spike measured on |
| `--still-frame <n>` | which frame the still is taken from |

## The picture has no CTest entry here, and that is deliberate

The two entries this directory does declare are `smoke.world_quiet_host_before` and
`smoke.world_quiet_host_across` (`quiet_host_test.py --leg ...`). Each runs the binary headless on a
4x4 world under four niced spinners, which it starts on purpose and kills by PID, and requires
`--quiet-host` to fail the run with `host too busy:`: the first loads the host before the take, the
second once the take has started. Removing either check turns its entry red. The second needs the
pre-take check to pass first, so on a host that is already busy it reports NOT EVALUATED and exits
3, which CTest records as a skip; it is a separate entry so that skip never hides the first one's
verdict. Neither needs a device, and both run `RUN_SERIAL`.

The picture needs a graphics device. `just/run.just`'s own note about `capture-virtual-geometry`
states the rule this follows: "It needs a graphics device. On a machine without one the tool says so
and writes nothing, which is why this is a recipe a person runs and not a test." A suite that skipped
on every machine without one would be a suite whose failure nobody would notice.

What IS gated automatically is everything underneath it: `unit.environment`,
`integration.environment_gpu`, `unit.terrain`, `integration.terrain_*`, `unit.water`,
`integration.water_*`, `unit.foliage`, `integration.foliage_*`, `unit.weather`,
`integration.weather_*`, `unit.pcg`, `integration.pcg_*` and `unit.sky` each test one of the parts
this artefact composes.

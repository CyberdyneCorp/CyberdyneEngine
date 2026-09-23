# `src/rendering/sky/` — layer 4: CyberSky

A participating atmosphere in physical units and its precomputed tables, the celestial model on a
declared time domain, clouds that are **reconstructed rather than stored**, a cloud shadow field
published into CyberField, the sky as a **light**, environment profiles and their transitions, the
quality tiers, and the diagnostics that say what determined a pixel.

**Governed by**: `atmosphere-sky-and-clouds`. **Seed** at M7 (task 10.4); **Working** at M10
(task 3.3).

## THERE IS ONE SKY MODULE AND THIS IS IT

M10's brief allowed for `src/sky/`. It was not created, because M7 left a real Seed here — an
atmosphere in physical units with no colour constant in it, a celestial model a project can bypass,
a measured GI sky term — and a second module would have meant two atmospheres that could disagree
about what a sunset is. M10 EXTENDED this one: five new headers, five new translation units, four
new dependencies and four new suites, against three files and three dependencies that were already
here. `atmosphere.h`, `celestial.h` and `sky_light.h` are M7's and their public surface is unchanged.

## The files

| File | What it holds | Tier |
|---|---|---|
| `atmosphere.h` | Rayleigh, Mie, ozone, ground albedo and stellar illuminance; transmittance, single scattering, the sun's disc, the sunlight that reaches the ground, and aerial perspective from the same parameters | M7 |
| `celestial.h` | sun and moon from time, latitude and axial tilt, on a declared time domain — and a model a project bypasses by writing a direction | M7 |
| `sky_light.h` | spherical-harmonic irradiance, the three-colour gradient `rendering-global-illumination` consumes, and `SkyViewTable` — the full-rebuild sky view | M7 |
| `tables.h` | `TransmittanceTable`, `MultipleScatteringTable`, `IncrementalSkyView` and `AerialPerspectiveTable`: the four tables the requirement names, and the two halves M7 declared as gaps | M10 |
| `clouds.h` | the coarse weather map, the layers, `drive_cloud_layers()`, the procedural reconstruction, the ray march and its cost, and the cloud half of temporal reprojection | M10 |
| `cloud_shadows.h` | the coarse world-scale cloud shadow field, produced into `cy::environment` under one `ProducerToken` | M10 |
| `composition.h` | planetary scale, stars and background content, aurorae, the filtered radiance map, the composition itself, the sky as a light, and the medium weather publishes into fog | M10 |
| `profile.h` | environment profiles, per-group transitions, the four named worlds, the quality tiers and the priced ladder | M10 |
| `budget.h` | the sky's `SubsystemController`: what a frame's march and shadow field COUNTED, a nanoseconds-per-sample learned from a clock, and the cost it reports to `rendering::BudgetArbiter` — the engine's first MEASURED cost, against the hard-coded table in `samples/07-fidelity` | M11.c |
| `diagnostics.h` | the table and celestial diagnostics, the per-pixel "what determined this", the cost attribution and the debug views | M10 |

## The measurements, because none of these is a claim

    ctest --test-dir build/<label> -R "render_sky" --output-on-failure

**Multiple scattering is a table, and the table is 20 to 32 per cent of a clear sky.** M7's
`Atmosphere::multiple_scattering_factor` was an isotropic constant and its header said so.
`MultipleScatteringTable` is the second-order table plus the geometric series that stands for orders
three and up, and what it contributes is measured by an A/B a reader can reproduce: an
`AtmosphereTables` whose multiple-scattering half was never built samples zero there, so the SAME
function over the SAME transmittance evaluates single scattering alone. Zenith at noon 20.3%,
horizon away from a low sun 32.3%, a slope facing away from the sun 32.2%.

**Incremental regeneration is a measured trade-off rather than a design intention.** A moving sun
never forces a full rebuild; what it costs and what it buys is this curve, over a quarter-day at 0.92
degrees of sun movement per update — a day compressed to about six seconds, two hundred times faster
than a project's twenty-minute day:

| row budget of 16 | fraction of a rebuild | worst error, as a fraction of the brightest sky | worst row staleness |
|---|---|---|---|
| 1 | 7.2% | 35.6% | 40.0° |
| 3 | 19.6% | 19.0% | 16.6° |
| 8 | 50.5% | 7.3% | 6.7° |

Staleness **accumulates per row**, which is what stops a scheme that always refreshes the rows near
the sun from starving the ones far from it — a failure a single sunrise hides completely.

**Clouds are compact, and the number is asserted rather than described.** A hundred-kilometre world's
weather map is 50 000 bytes. The volume it replaces — the same world voxelised at the 32 m a ray
march actually resolves — is 2 128 906 250. That is the requirement's "a world-scale volumetric cloud
field SHALL NOT be stored, streamed, or replicated", as a ratio of forty thousand to one.

**Aerial perspective is the atmosphere's, in the engine's own froxel volume.** `AerialPerspectiveTable`
IS a `rendering::FroxelVolume` and calls `froxel_slice_depth()` and `froxel_slice_of()` rather than
re-deriving the slice distribution. Against M7's `aerial_perspective()` at eight kilometres its
transmittance agrees to within 0.8% and its in-scattering is 1.52 times as large — because the
analytic function integrates single scattering only and the table adds the tabulated multiple
scattering, which near the ground is a third of the light.

**Ground to orbit is one model.** A camera walked from two metres to four hundred kilometres,
geometrically, shows a worst step in zenith radiance of 6.3% of the ground's value and arrives at a
black sky because the model ran out of air. There is no altitude band anywhere in this module.

## Two defects M10 found, both with the regression test beside the fix

**The sky glowed at midnight.** M7's isotropic multiple-scattering stand-in was added wherever the
view ray passed, WITHOUT the sun's own transmittance — so it was added to air no sunlight reaches. A
sun twenty degrees below the horizon produced **378 nits at the zenith and 2 679 at the horizon**,
which is daylight brightness in a scene that is supposed to be dark and which no tone mapper can
recover from. The fix is one `cwise_mul` in `sky_radiance()`; the regression test is
`test_sky_tables.cpp`'s midnight case, which checks both the marched and the tabulated path. With the
factor applied, M7's 0.35 turns out to be a good guess in daylight — the constant and the table agree
to within 15% where the sun can reach. It was never the daylight value that was wrong.

**Every descending `remap01` returned zero, so there were no clouds anywhere.** `height_profile()`
fades a layer's top out with `remap01(h, 1.0F, 0.88F)`, a deliberately DESCENDING interval. The guard
was `max(high - low, 1e-5)`, which turns a span of −0.12 into +1e-5 and sends the result to zero — so
the height profile was zero for every layer, the density was zero everywhere, and the cloud shadow
field was a field of ones. `test_clouds.cpp`'s reconstruction case is the regression test, and it
searches for a position the reconstruction actually puts a cloud at rather than asserting on one it
assumes, because a determinism check on a sample that happens to be zero is a check that two zeroes
are equal.

## The decisions worth reading before changing anything

**1. What is STATE and what is DRAWING is drawn in the types.** The capability leads with "the same
environmental state SHALL drive every tier", so `CloudWeatherState`, `CloudWeatherMap` and
`drive_cloud_layers()` do not take a `CloudQuality` — a tier cannot reach them because it is not in
their signatures. `CloudDensitySample` carries both halves for the same reason: `coverage` and `type`
come from the map and are identical at every tier; `density` is the reconstruction and is not.

**2. The cloud shadow is an `environment` field, and the alternative is unreachable rather than
unused.** `src/rendering/sky/CMakeLists.txt` does not link `cy::rendering-shadows`, so there is no
expression here that can allocate a virtual shadow page. `cloud_shadow_declaration()` also refuses a
cell below 64 m, which is the other half of the same guarantee: a caller asking for two-metre cells
is asking for a shadow map by another name.

**3. A profile describes the world and a preset describes the day, and the enforcement is an
absence.** There is no `CloudWeatherState` in `EnvironmentProfile`. `configure_sky(profile, weather)`
is the only function in the module that reads both, so a project cannot end up with two definitions
of what a storm does to the low deck.

**4. Weather reaches fog through state, and the arrow only points one way.** `AtmosphericMedium` is
published by whoever owns the weather; `derive_fog_parameters()` is a pure function from it; and no
function here takes a `FogParameters` and writes it anywhere. The Koschmieder relation between
visibility and extinction is one constant used in both directions, and the round trip is a test.

**5. The noise is `cy/noise.slang`'s, transliterated.** `value_noise()` is that module's arithmetic
operation for operation, so the day a cloud shader is written the CPU and the GPU reconstruct the
same cloud rather than two clouds that look alike. The one addition is the seed, which offsets the
lattice.

**6. Temporal reconstruction calls the engine's classifier.** `reproject_cloud()` calls
`rendering::classify_history()`; what it adds is the cloud's own screen motion — a still camera and a
thirty-metre-a-second wind at three kilometres moves the fetch by 0.0087 of the screen, and a
camera-only motion vector would have moved it by nothing — and the weather rejection, at two
granularities because a cell that changed and a state that changed fail differently.

## What this tier does NOT do, said rather than implied

* **NO SHADER, AND NO DEVICE.** Everything here is CPU. The cloud march, the tables and the radiance
  map are the algorithms a shader will implement, measured against each other and against the model;
  no `.slang` module accompanies them and no agreement against a real device is claimed. `cy/field.slang`
  — which `src/environment/`'s README said was owed by "the renderer-facing row that first samples a
  field in a shader" — **was written at M11.a and is not this row's**, and it brought
  `cy/cloud_shadow.slang` with it: the one line every consumer of this field wants, `saturate`d and
  applied to the direct term only. What is still CPU here is the PRODUCER — `CloudShadowField::update`
  marches the reconstruction on the processor and writes tiles — and the cloud march itself, which is
  23.2 ms of `m10:world-frame-budget` and is still open.

* **THE ROUND TRIP WAS NOT WHERE ITS GAP SAID IT WAS.** `m10:sky-field-round-trip` recorded that
  `update` reported writing tiles darker than 0.5 while `sample` returned the declared 1.0 at all
  twenty-five points inside `radius_metres`, and concluded that the sky's WRITE PATH was broken.
  It was not. Reading every cell the producer writes back through `FieldStore::sample_at()` resolves
  1024 of 1024 regional cells and 256 of 256 macro cells, and the darkest reads 0.0039 against a
  producer report of 0.0041 — one `UNorm8` quantum apart. What was wrong was the TEST'S SAMPLING
  POSITIONS: a five-by-five grid 256 m about the origin, over ground that is genuinely in full sun
  under that weather, because the cloud map's cells are 1000 m across and the mean over everything
  written is 0.974. Twenty-five samples of lit ground read 1.0 whether the field was published or
  not, which is why suppressing `publish()` did not move them. `test_cloud_shadows.cpp` now reads
  back every cell at both levels and compares the extremes with the producer's own `stats()`; both
  halves go red when `publish()` is suppressed.

  **AND THAT REPAIR WAS REFUTED IN TURN, WHICH IS WHY THE GAP STAYED OPEN THROUGH M11.a AND CLOSED AT
  M11.c.** The rewrite reads through `FieldStore::sample_at(field, position, level)` with an explicit
  residency, which is a better test of the STORE and is no longer a test of the function the
  requirement's four consumers call. M11.a's own gate proved it: it replaced the body of
  `CloudShadowField::sample` with `return 1.0F` — the literal symptom the gap names — rebuilt, and
  the criterion stayed GREEN, because the only call to that function left in the suite was one probe
  nine million metres from anything. The case now walks the same cell centres a SECOND time through
  `CloudShadowField::sample` and compares the two answers: `through CloudShadowField::sample: 5120
  samples, lowest 0.00392157, highest 1, disagreeing 0`. Not one cell reads one number through the
  consumer path and another through the store.

* **THE FIELD HAS A CONSUMER OUTSIDE THIS DIRECTORY, AND UNTIL M11.c IT HAD NONE.**
  `CloudShadowField::declare_consumers()` has declared terrain, foliage, water and illumination to
  the registry since M10 and `FieldRegistry::validate()` has been happy with all four, because
  declaring a consumer is a configuration entry. `src/rendering/lighting/src/cloud_shadow.cpp` is
  illumination's half: it attenuates a DIRECTIONAL light's illuminance by
  `CloudShadowField::sample`, and `integration.render_illumination_clouds` searches the written
  ground for the darkest and the brightest cell and reports both — 10 980 lux under the cloud against
  100 000 beside it. Terrain, foliage and water are still declared and still do not sample.
* **AND NO FRAME CALLS EITHER HALF, WHICH IS WHY THE ROW IS NOT COMPLETE AT M11.c.** The illumination
  consumer above is a library whose only caller is its own test; `cy/cloud_shadow.slang` is imported
  by no shader; and `samples/10-world` dims its sun by one scalar `cloud_transmittance`, not by the
  field at a position. `aerial_perspective()` and the froxel table are built from the same
  `Atmosphere` as the sky — which is the requirement's prohibition, answered — and applied to no
  opaque surface anywhere in the tree. M11.c task 5.1 measured both, and `Cloud shadows` and `Aerial
  perspective` are `exempt:m11e` in `tools/roadmap/requirements-coverage.toml`, so
  `atmosphere-sky-and-clouds` waits for M11.e. The re-entry point is the forward frame's lit path
  sampling the field per surface position and the aerial-perspective table for opaque surfaces.
* **The lighting integral's cloud term is a hemispherical mean.** `compose_sky_lighting()` measures
  the clouds' effect over twelve probes and applies one attenuation plus one addition. It is right in
  magnitude — thicker cover gives less irradiance — and wrong in DIRECTION: a cloud bank on one
  horizon tilts the real irradiance and this does not.
* **The aurora is a band.** It is composed WITH the sky — occluded by cloud, attenuated by air,
  invisible in daylight — which is the structural half of the requirement. Curtains and rays are a
  project's own `aurora_radiance()`.
* **`StarSource::Imagery` names an asset and does not resolve one.** This module owns no asset
  system; the composition's caller binds the image. The other two sources compose through the same
  function, which is what makes the choice a content decision rather than a structural one.
* **No wetness, no precipitation and no wind field.** `CloudWeatherState` is DECLARED here and
  PRODUCED by `weather-and-wind`. That direction is what let the sky be built and measured before
  that row existed.

# `src/rendering/sky/` — layer 4

A participating atmosphere in physical units, the celestial model and its declared time domain, and
the sky as a **light**.

**Governed by**: `atmosphere-sky-and-clouds`, at **Seed** for M7. Task 10.4.

## The files

| File | What it holds |
|---|---|
| `atmosphere.h` | Rayleigh, Mie, ozone, ground albedo and stellar illuminance; transmittance, single scattering, the sun's disc, the sunlight that reaches the ground, and aerial perspective from the same parameters |
| `celestial.h` | sun and moon from time, latitude and axial tilt, on a declared time domain — and a model a project bypasses by writing a direction |
| `sky_light.h` | spherical-harmonic irradiance, the three-colour gradient `rendering-global-illumination` consumes, and the sky view table a frame samples instead of ray marching |

## The sky follows from the coefficients. There is no tinted preset in this module

`sky_radiance()` takes an `Atmosphere` and a direction and has no other input, and there is no colour
constant anywhere in the file. The Rayleigh coefficients go as 1/λ⁴, so blue scatters about six
times as strongly as red — that **is** why the sky is blue, and a project that changes them gets a
different colour because the arithmetic changed.

`thin_dusty_atmosphere()` exists because the requirement's own scenario is "a project sets a dusty
thin atmosphere", and a scenario with no example in the tree is a scenario nobody runs. The suite
asserts the consequence rather than the parameters: Earth's zenith has more than 2.5× as much blue
radiance as red, and the dusty planet's has less than 1.6× — it is not dimmer, it is a different
colour.

## "Sufficient for GI's sky term" is a measurement

`src/rendering/gi/` already declares what it wants — `gi::SkyTerm`, three colours and an intensity,
with a comment saying the full model "is the seam it will replace, not a second sky".
`fit_sky_gradient()` is that replacement, and the criterion is an **integral**: the irradiance the
three-colour gradient delivers against the irradiance the full atmosphere delivers, over four sun
elevations and three surface orientations. A gradient that agreed with the sky only at the zenith
would pass a spot check and be wrong for every surface that is not facing straight up.

    just test-integration -R integration.render_sky_light

    gradient vs atmosphere: mean relative irradiance difference 12.395% over 12 sun and
    surface combinations, worst 17.4125%
    mean irradiance over three orientations: solved horizon 14.042% off, sampled horizon 111.651% off
    SH-9 irradiance: worst relative difference 1.92087% over four orientations
    sky view table at high quality: worst relative difference from the model 0.169231%

**The horizon colour is solved for, not sampled**, and it is solved by least squares over five
surface orientations rather than one. Three numbers from the run above: sampling the horizon
direction is 112% off; solving for the upward hemisphere alone is exact there and 50% off on a wall
facing away from the sun; solving over the five is what ships. A GI sky term is read by every
surface in a scene and almost none of them faces straight up.

For a consumer that can hold nine colours rather than three, `project_sky_irradiance()` is 1.9%.

**This module names nothing in `cy::rendering::gi` and does not link it.** That is what lets the
measurement above run with no global illumination system present. The adapter at the composition
point is two lines and `sky_light.h` carries them.

## What this Seed does not do, said rather than implied

* **Multiple scattering is an isotropic approximation**, not the second-order table the requirement's
  list names. `Atmosphere::multiple_scattering_factor` is the knob. The visible consequence is a sky
  slightly too dark near the horizon at sunset and a shadowed slope that is too blue.
* **The sky view table is rebuilt in full**, not incrementally, when the sun moves past
  `kSunMovementThreshold`. The requirement asks for incremental regeneration as the sun moves.
  `SkyTableStats::full_rebuilds` is a counter a reader can see, which is the half of that
  requirement this tier does honour — along with "regenerated only when the parameters change" (the
  sun is compared by *angle*, because a sun driven from a clock is never twice the same float) and
  "their generation cost SHALL be reported".
* **No clouds, no aurorae, no stars.** All three are required by the capability and none is here;
  they are its Working tier. `CelestialState::star_visibility` is computed and drives nothing in this
  module, because the sky composition consumes it and the sky composition is not here either.

## Two defects the tests found, both recorded at the site

* **A ray starting exactly on the surface and pointing down was declared to miss the planet.** The
  first-root test was `root > 0` and that root is exactly zero there, so the integration ran straight
  through the planet and a sun 30° below the horizon delivered 5×10⁻³⁶ lux instead of none. The test
  is now on the closest approach. Not zero is not zero, and the thing that eventually notices is a
  tone mapper.
* **The spherical-harmonic irradiance was 68% low, which is exactly 1 − 1/π.** The cosine convolution
  had an extra 1/π folded in — the factor that turns irradiance into the "diffuse radiance" a shader
  wants *after* it has also multiplied by albedo. The shape of the error is what named it.

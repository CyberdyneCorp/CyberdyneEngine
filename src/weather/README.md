# `src/weather/` — CyberWeather

**Weather publishes state and touches nothing.** That sentence is `weather-and-wind`'s first
requirement and it is the shape of this whole module: nothing here iterates materials, foliage
instances, water bodies or particle systems, and there is nowhere in it for such a loop to be added.
The link line is the enforcement — `cy::foliage`, `cy::vfx`, `cy::water`, `cy::terrain` and every
`cy::rendering-*` target are absent from it, so the forbidden call is unspellable rather than
discouraged. See `CMakeLists.txt`, where each absence is named.

M10 tasks 3.1 and 3.2. Layer 4 (`scene`), read after `src/environment/`, whose fields this module
produces.

## The three layers, and which file each lives in

`weather-and-wind` separates climate from weather from presentation, and the separation is the
directory's structure rather than a note in it.

| File | Layer | What it holds |
|---|---|---|
| `climate.h` | **Climate** | Prevailing tendencies, the derived latitude/elevation/ocean model, and the **biome potential** procedural generation consumes. Counts its own evaluations, so "climate is not recomputed to answer a question about the current state" is a number a suite requires not to move |
| `cells.h` | **Weather** | Weather cells and the global/regional/local hierarchy, the relaxation, semi-Lagrangian advection, and the declared orographic model that makes a windward slope wet and a lee dry |
| `storm.h` | **Weather** | Storms as spatial phenomena with identity, and lightning as an **event** carrying a position, an intensity and a seed — no sound, no effect, no thunder delay |
| `preset.h` | **Weather** | Presets, per-property transition clocks, and the schedule. **One entry point**, which is how "a cinematic-only weather implementation SHALL NOT exist" is met |
| `sample.h` | **Weather** | The environment sample, its three qualities, and the batch that allocates nothing |
| `wind.h` | **Weather + presentation** | The wind field: the composition, the volumes, the transient budget, and the line between the authoritative half and the presentation residual |
| `precipitation.h` | **Presentation** | The occlusion representation that answers "am I sheltered" with one lookup, the tiered plan, and the interaction rates. Read by nothing else in the module |
| `accumulation.h` | **Weather** | Wetness and snow: what precipitation leaves behind and what takes it away |
| `ecosystem.h` | **Weather** | Macro ecosystem state, the knock-backs, and the biome thresholds |
| `fields.h` | **Output** | The eighteen fields weather declares, claims and writes. **The only output side of the module** |
| `diagnostics.h` | — | The inspector, which explains a composition rather than reporting a number |
| `system.h` | — | The composition root and the tick |

## Five decisions worth reading before changing anything here

### 1. One wind field, two determinism classes, and the line is in the value

A gameplay-visible wind and a presentation-only gust are not the same thing under
`simulation-and-determinism`. Putting everything in one authoritative field makes a blade of grass's
high-frequency detail into state that must be hashed and replicated; putting everything in one
presentation field makes a projectile reading the wind a firewall violation.

So the composition produces three components and publishes two fields: `wind` carries `base + gust`
and is **authoritative** (the gust included — it is drawn from `determinism::RandomStream` and is a
pure function of seed, tick and position); `wind-turbulence` carries the residual and is
**presentation**. `WindSample::authoritative()` is the same number for every reader, and
`WindComposer::sample()` fills `turbulence` only for a reader `determinism::may_read()` allows it to.

That is why "trees, smoke, water and cloth move consistently" and "visual detail does not influence
authoritative state" are one mechanism rather than two that compete. `test_wind.cpp` measures the
equality bit for bit, and `test_determinism.cpp` measures it again across a thousand ticks with the
presentation levers at their extremes.

### 2. Contributions go into the TARGET, never onto the state

A storm's wind added to a cell after every relaxation step is **amplified by the reciprocal of the
relaxation fraction** — the state relaxes away from it and it is added again next step, so a storm
worth three metres per second settles at thirty and can flip the sign of the prevailing wind. The
first version of `WeatherCells::step_regional()` did exactly that, and the symptom was an orographic
uplift with the wrong sign on the windward slope.

Everything now composes one target — climate informed by the global tendency, the storms over it,
the terrain under it — and the state takes a single exponential step toward it. `apply_storms()` and
`apply_terrain()` take a `WeatherState&` target for that reason, and their comments say so.

### 3. A quantised field that is read back and nudged does not accumulate slowly — it does not accumulate

Every ecosystem field is read, changed by a small amount and written again once per macro step. If
the change is below the field's own storage quantum, **every write rounds back to the value it
started from**: a burned forest stays burned for ever and nothing reports anything. An hour of
vegetation recovery is 3.6e-4 of the gap and a `UNorm8` quantum is 3.9e-3 — a factor of ten the
wrong way, and entirely silent.

`vegetation-density`, `moisture`, `soil-health`, `burn-state` and `wetness` are therefore `UNorm16`,
and `forest-age` — the one field that accumulates rather than approaching a target — is `f32`. And
`Ecosystem::check_resolution()` refuses a configuration where a step would round away, naming the
field and both numbers. It is the only defence against a class of bug whose entire evidence is a
number that never moves.

### 4. The regrowth is the substrate's, and only the knock-back is weather's

`environment-fields` already has the mechanism: a field declares a `potential` and a
`recovery_per_second`, and `FieldStore::advance_recovery()` closes the gap exponentially. So
`vegetation-density` declares `vegetation-potential`, `moisture` declares `moisture-potential`, and
"a burned forest regrows toward its potential" is one call each. What is left here is the half the
substrate deliberately does not do — fire, deforestation, drought, pollution, terraforming.

**Terraforming is the one event that also moves the potentials**, because irrigating a desert changes
what the land can support. Every other event knocks the current state away from a potential the
climate still holds; without that distinction a terraforming programme would evaporate.

The ecosystem writes the **base** layer and everything else in the module writes the **delta** layer.
That is not an inconsistency: the recovery walks base tiles, and ecosystem state has no cooked base
to preserve — it IS the state.

### 5. The fast-forward is the runtime, not a preview of it

`WeatherCells::advance()` and `Ecosystem::advance()` both run whole macro steps and carry the
remainder. Ninety `advance(1 day)` calls therefore run exactly the steps one `advance(90 days)` call
runs, in the same order, and reach the same state bit for bit — and a sequence of advances shorter
than one step still adds up rather than rounding away. `advance_days()` is a unit conversion and
nothing else, so "the editor uses the runtime's macro model, not a separate preview model" is a
property of the code rather than a claim about it.

## The seams, and what is on the far side of each

| Seam | Shape | Why not a dependency |
|---|---|---|
| Terrain elevation | `TerrainProfile` — one callback | The rain shadow, the uplift and the channelling need one number. `src/water/` draws the same seam and calls it `BedSource` |
| Scene geometry | `SkyOcclusion::add_cover()` — rectangles | An occlusion representation that needed the geometry would be a second scene. The specification asks for a COARSE structure derived from one |
| The sky | `CloudDrive` — six numbers | `atmosphere-sky-and-clouds` declares `sky::CloudWeatherState` and says weather produces it. Linking the renderer from here to name one struct would put presentation underneath authority and make weather uncookable; the composition point assigns them field by field |
| Water's wetness | `water-shore-wetness`, composed by `Max` | `water` says the shoreline writes wetness and this capability says precipitation does. `environment-fields` allows one producer. `WetnessSource::ComposeShore` is the declared resolution, and the rule both rows chose independently is the same one |

## Suites

| Suite | Kind | What it measures |
|---|---|---|
| `weather` | unit | The climate map and its counter, the cell hierarchy and its rain shadow against a flat control, the wind composition and its budget, the sample's qualities, the occlusion and the tiers, storms and their wire form, presets and their per-property clocks, and the inspector's sum |
| `weather_fields` | integration | A real registry, a real store, real tokens and real tiles: the second-producer refusal naming both, the all-or-nothing claim, the firewall at `open()` and over the configuration, and a consumer reading weather without calling it |
| `weather_ecosystem` | integration | Ninety simulated days against the substrate's own recovery, compared step for step with ninety one-day advances; a burned forest regrowing; a terraformed desert crossing declared thresholds into savanna and on into forest |
| `weather_determinism` | integration | Two systems, one seed, a thousand ticks, presentation levers at both extremes — authoritative state compared bit for bit; and a snapshot that reconstructs the sender |

Run them with `ctest --test-dir <build> -R weather`.

# `src/water/` — CyberWater

Oceans, lakes, rivers, pools and waterfalls as **one water body abstraction over several simulation
backends**, rather than four systems that happen to be wet. M10 section 2.3, and `water` reaching
Working.

Water is a **producer into `environment-fields`**: it owns the shoreline, and it writes water depth,
water distance, water flow and the shore's wetness into the substrate. Terrain reads those fields
and never calls here — which is the requirement this module's dependency list exists to make
checkable.

## What is here

| file | what it carries |
|---|---|
| `include/cy/water/body.h` | `WaterBodyId`, `WaterBodyDesc`, the backend table and its Planned/Deferred refusals, `WaterOptics`, and `WaterRegistry` with the declared resolution order |
| `include/cy/water/displacement.h` | **The displacement contract**: authoritative and visual-only bands, one `evaluate_displacement()`, the amplitude split, and `validate_model()`'s refusals |
| `include/cy/water/ocean.h` | The fetch-limited Pierson-Moskowitz spectrum, the cascades it fills, and `OceanSurface` — the camera-relative patch |
| `include/cy/water/river.h` | Spline networks, junctions, discharge, continuity, obstacle deflection, bends and drops, and the foam sources they generate |
| `include/cy/water/query.h` | `WaterSample` — every member of the specification's own list — the resolution indicator, and the character water states |
| `include/cy/water/buoyancy.h` | Multi-sample buoyancy, drag and the current's carry |
| `include/cy/water/foam.h` | The persistent, advected, decaying, memory-bounded foam field |
| `include/cy/water/shading.h` | The surface closure's parameters, Beer-Lambert over the column, the reflection escalation, the caustic tiers and the explicit surface crossing |
| `include/cy/water/shoreline.h` | `BedSource` — the terrain seam — and `WaterFields`, the producer that declares, claims and publishes the four fields |
| `include/cy/water/streaming.h` | Segments bound to world cells through `world::CellEventQueue`, and the profile that decides what a segment carries |
| `include/cy/water/system.h` | `WaterSystem`: the composition, the one query, the weather seam, the navigation contribution and the diagnostics |

## The six decisions a reader should know before changing anything

**1. There is one displacement definition and two selections of it.** `evaluate_displacement(model,
selection, …)` is the only function in the engine that says where the water surface is. Rendering
passes `BandSelection::All`; every physics, buoyancy and gameplay query passes
`BandSelection::Authoritative` — and `WaterSystem::query()` passes it for *everyone*, so there is no
overload a caller could float a boat on the rendered surface with. The difference between the two
answers is exactly the declared visual-only bands and nothing else, which is the whole content of
`water`'s displacement contract. A second entry point is the bug this shape exists to prevent.

**2. A visual band large enough to be felt is refused, and the split is always reportable.**
`validate_model()` refuses a model whose largest visual-only band exceeds the declared
`felt_threshold_metres`, and `WaterSystem::set_ocean()` runs it. `amplitude_split()` reports the two
totals whether or not anything was refused, because the question a developer asks is "do physics and
rendering disagree here, and by how much" and the answer to it is a number in metres.
`build_ocean_model()` **promotes** a cascade that would be felt rather than emitting a violation —
`OceanReport::promoted_cascades` says when it did.

**3. A river's flow is continuity, not an authored number.** Q = width x depth x speed is conserved
along a section, so a narrows accelerates without anyone authoring the number, and a junction ADDS
two discharges rather than overlaying two surfaces. A junction also pulls the tributary's mouth to
the trunk's level, blended back up its last stretch, and **reports the correction in metres** — a
silent adjustment would hide an authoring error the size of a metre.

**4. The terrain seam is one callback, and that is the acyclicity.** `BedSource` is a function
pointer the application installs. `cy::terrain` is not in this module's dependency list and could
not be: what terrain needs to know about water it learns by sampling fields. `water`'s "The
dependency stays acyclic" scenario is therefore a property of the link graph rather than of a review.

**5. Wetness has two possible producers and the substrate allows one, so it is DECLARED.** `water`
says the shoreline writes wetness; `weather-and-wind` says precipitation accumulates into it;
`environment-fields` allows one producer per field. `WaterFieldOptions::wetness_owner` picks: with
`Water`, this module claims `wetness`; with `External`, it claims `water-shore-wetness` and the
wetness producer composes it. Both are honest. Claiming `wetness` and letting whichever row
registered second take the refusal as a surprise is not.

**6. A body is global and its runtime data is segmented.** A `WaterBodyId` exists whether or not
anything is resident; a `WaterSegmentKey` exists only while a bound cell overlaps it. There is no
type in this module that could hold a per-cell river, which is `water`'s "A continental river"
scenario made structural. Segments are bound through `world::CellEventQueue` and carry no residency
budget of their own — see `streaming.h` for why a *derived* payload has nothing to admit.

## What is NOT here, and who owes it

- **The surface's publication into the GPU scene.** `OceanSurface` produces the geometry —
  positions, normals, the breaking indicator and a triangle list, camera-relative and snapped — and
  stops there. `water` requires it to be "published into the GPU scene, so it is culled and shaded
  like other geometry", and that call belongs to a renderer-facing row with a device. This module
  links no renderer, deliberately.
- **The water surface closure's evaluation.** `build_closure()` fills the closure's inputs;
  `material-compiler` owns the closure itself and `rendering-materials-and-shading` owns evaluating
  it. A water module that emitted shader code would be the second material system `water` forbids.
- **The underwater passes.** `underwater_state()` derives the parameters — including the submerged
  fraction that makes the crossing explicit rather than a switch — and the volumetric fog, the
  caustics and the audio filtering are the systems that already own those.
- **Navigation's rebuild.** `directional_cost()` and `drain_navigation_dirty()` are the contribution
  and the invalidation; wiring them into a navmesh build is `navigation`'s.
- **Persistence of a changed level.** `set_mean_level()` moves a body and emits its dirty region;
  writing that into the save overlay is `save-and-persistence`'s, alongside terrain deformation.

## Suites

| suite | kind | what it measures |
|---|---|---|
| `water` | unit | the registry and its refusals, the displacement contract, the spectrum and the camera-relative patch, rivers and their flow, the query, buoyancy, the navigation cost and the shading parameters — 53 cases |
| `water_foam` | integration | persistence measured over time: a wake deposited, advected for a couple of hundred steps and watched to decay, with the memory bound held across it |
| `water_shoreline` | integration | a real field registry and store: the four fields declared, claimed, published, sampled, firewalled — and the refusal when a second producer wants `wetness` |
| `water_streaming` | integration | a real cell event queue: segments bound, straddled, evicted, and the server profile's payload mask against the client's |

Every behavioural claim above was **watched to fail**: eleven mutations, one per claim, each applied to
the module, built, run red and restored. They are listed in this milestone's report, and the suites
that carry them say at the top of the file which mutation reddens them.

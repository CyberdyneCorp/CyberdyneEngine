# Design: CyberPCG and CyberEnvironment

## 1. The substrate already exists, and it was waiting for producers

`environment-fields` was specified before anything wrote to it, deliberately: putting moisture inside
terrain would have made foliage depend on terrain, and putting wind inside foliage would have made
water depend on foliage. The consumers were built first, and weather and hydrology were recorded as
"field producers against systems that do not exist".

This change supplies two of those producers. Nothing that consumes a field changes, which is the
evidence that specifying the substrate first was correct.

## 2. Procedural generation was already happening, in four places

Foliage has deterministic rule-based placement with seeds and an exception store. Terrain has a
non-destructive modifier stack with noise, erosion, splines, and craters. Water generates surfaces
and flow from spline networks. Each is a procedural system with its own determinism story,
invalidation, and caching, built inside the capability that needed it.

That is four partial implementations of one thing, and a fifth was inevitable. CyberPCG is the
generalisation, and the existing systems become **consumers of it** rather than being replaced:
foliage placement rules are PCG programs producing foliage populations, terrain modifiers are PCG
programs producing terrain layers.

## 3. Typed datasets, not an object spawner

The failure mode of procedural systems is that they become "spawn things in a pattern", at which
point every new content type needs a new tool.

So PCG operates on **typed spatial datasets** — point sets, volumes, surfaces, splines, fields,
geometry, attribute tables, entity sets, regions, rasters — and a generator transforms datasets. The
same filter-and-scatter infrastructure then produces forests, buildings, roads, rocks, resource
nodes, spawn regions, and gameplay influence maps, because none of those are special to it.

Points carry typed attributes with compiled identifiers, using the same identity mechanism as
everything else, so an attribute is not a string lookup at runtime.

## 4. Output is the representation the consumer wants

The most consequential rule in the capability: **a procedural result is not an entity by default.**

| Output | Goes to |
|---|---|
| Foliage population | Compact GPU instances |
| Terrain stamp or layer | The non-destructive modifier stack |
| Field tiles | The field substrate |
| Entity template instances | Cooked cells, or batch instantiation at runtime |
| Spline networks | Roads, rivers, rails |
| Geometry | The asset pipeline |

Ten million trees are a foliage population. Producing ten million spawn commands instead would defeat
the foliage capability entirely, and it is the obvious implementation, which is why it is stated as a
requirement rather than left to judgement.

## 5. Stable identity is what makes the workflow survivable

A generated tree needs to be the *same logical tree* across regeneration, or a designer's deletion
lands on a different tree, a player's felled tree returns, and burn state attaches to the wrong
trunk.

Identity is derived from generator, region, and a stable per-output key — never from iteration order,
array index, or worker scheduling. That single decision is what allows: author overrides that
survive regeneration, persistence deltas that outlive a rule change, and the orphan reporting that
follows when a rule change genuinely removes the thing an override referred to.

This is the same decision the foliage exception store already made locally, generalised — and the
same decision the type system made for fields, for the same reason.

## 6. Region-based, dependency-driven, incremental

World-scale generation as one operation is not a system, it is a batch job. Generation is partitioned
into **regions** — not necessarily the same size as world cells, because a road network's domain is
not a grass patch's — and generators declare inputs, outputs, and the **radius** over which they
sample.

Editing a road then invalidates: the terrain stamps it cuts, the foliage within the clearance radius
it samples, and the settlement placement that reads road access. Nothing else. The dependency and
radius declarations are what make that computable rather than guessed.

Region results are cached by a derivation key in the derived data cache, which means procedural
generation distributes across build workers for free — it is already a graph of derivations with
declared inputs, which is exactly what the build system was specified to execute.

## 7. Explainability is a requirement, not a debugging feature

Procedural content is the hardest kind of content to reason about, because the artefact and the
cause are far apart.

So provenance is mandatory in development builds: for any generated instance, which generator,
which region, which seed, which rule, and which attribute values selected it. And the harder
question, which no tool usually answers: **why is nothing here** — which filter rejected this
location, and by how much.

This is the same "answer the causal question" discipline applied to residency, shadows, illumination,
and builds, and procedural generation is where it pays most.

## 8. Weather publishes state; it touches nothing

The failure mode of weather systems is a `WeatherManager` that iterates materials setting `wet =
true`, walks foliage setting sway, and pokes particle systems. That does not scale, it cannot be
saved, and it cannot be made deterministic.

Weather writes **fields**. Materials sample wetness. Foliage samples wind. Water samples
precipitation. Audio samples wind and rain. AI samples visibility. Nothing is pushed to anyone.

The corollary is that lowering cloud quality cannot change how wet the ground is, because the ground's
wetness came from a field, not from the renderer.

## 9. Climate, weather, presentation — three layers

**Climate** is slow: prevailing wind, mean temperature, rainfall potential, ocean influence. It is
mostly authored or generated once and feeds biome potential.

**Weather** is the current state: temperature, humidity, pressure, wind, precipitation, cloud
coverage, visibility — evolving over weather cells and storm phenomena.

**Presentation** renders it: atmosphere, clouds, volumetrics, precipitation effects, wet surfaces,
audio.

Keeping them apart is what lets a strategy game make rain slow units deterministically while the
cloud renderer runs a non-deterministic GPU simulation at whatever quality the budget allows.

## 10. Wind is the field with the most consumers and no producer

Foliage, VFX, cloth, water, clouds, and audio were all specified to sample the wind field.
`environment-fields` defined it with layered contributions. Nothing writes it.

Weather now does: prevailing wind from climate, regional wind from weather cells and storms, terrain
influence, local volumes, and transient sources. One field, one set of contributions, and trees,
smoke, and water finally agree about which way the wind is blowing because they always were sampling
the same thing.

## 11. Clouds are reconstructed, not stored

A volumetric cloud field at useful resolution over a large world is not storable and not
transmittable.

So clouds are **reconstructed procedurally** from a low-resolution weather map plus noise and a height
profile, evaluated by the GPU during ray marching. What streams and replicates is coverage and type
at kilometres of resolution; what is expensive exists only where it is being looked at.

Cloud shadows follow the same logic: a coarse world-scale shadow field, not cloud geometry pushed
through the virtual shadow page system, whose whole design assumes shadow detail correlates with
screen pixels.

## 12. Precipitation does not simulate drops

Millions of raindrops with collision is the naive implementation and it is not affordable at any
quality that matters.

Rain near the camera is an effect. Rain everywhere else is a field value. Whether you are sheltered
is answered by a **precipitation occlusion representation** — a coarse sky-visibility structure —
not by a ray per drop. Wetness accumulates in a field and evaporates by temperature, sun, and wind;
snow accumulates and melts the same way; both are consumed by materials and by the runtime virtual
texture layer that already exists for exactly this kind of world-state composition.

## 13. Biome potential is not biome state

The distinction that makes an ecosystem rather than a painted map:

**Potential** is what the climate, soil, altitude, and water could support. **State** is what fire,
deforestation, pollution, and terraforming have actually left.

Vegetation grows toward potential and is knocked back by events. A burned forest has forest potential
and grassland state, and regrows over time. A terraformed desert has rising potential and slowly
rising state.

That is a small modelling decision that unlocks the world-restoration mechanic the project wants,
and it costs one extra field rather than an ecology simulation.

## 14. Macro state, materialised on demand

A distant forest does not need ten million trees to exist. It needs vegetation density, forest age,
burn fraction, and soil health — a few field values per region — evolving slowly whether or not
anyone is looking.

When the region becomes relevant, PCG **deterministically materialises** detail consistent with that
state. This is the same pattern as the world's representation tiers and AI's statistical population
model, applied to content, and it is what makes an evolving planet affordable.

## 15. Determinism, per generator

A GPU-scattered decorative debris field does not need to be reproducible. Resource node placement in
a competitive strategy game absolutely does.

So each generator declares a determinism level consistent with the session's profile, and only those
that need it pay for deterministic math and ordering. Weather does the same: authoritative weather
state can be deterministic while the cloud renderer is not, separated by the firewall that already
exists.

## 16. Build order, and the one ordering that matters

| Phase | Contents |
|---|---|
| 1 | Field extensions: residency levels, versioning, potential versus state |
| 2 | PCG datasets, deterministic identity, region execution, graph compilation |
| 3 | Output adapters: terrain, foliage, fields, entities |
| 4 | Dependency invalidation, derived data caching, distribution |
| 5 | Overrides, provenance, editor tooling |
| 6 | Atmosphere tables, celestial model, sky |
| 7 | Weather cells and state, the wind field, precipitation |
| 8 | Wetness, snow, occlusion |
| 9 | Volumetric clouds, cloud shadows, temporal reconstruction |
| 10 | Storms, transitions, determinism and replay integration |
| 11 | Macro ecosystem state and on-demand materialisation |
| 12 | Time-simulation tooling and scale benchmarks |

**Phase 1 comes first for the reason the substrate existed at all.** If PCG and weather are built
before the field extensions they need, each will grow its own grid, and the engine will acquire the
duplication the substrate was specified to prevent.

## 17. Gaps after this change

**Hydrology and erosion** remain, and are now the last environmental gap. Every input and output they
need — rainfall, moisture, flow, water distance, terrain height, soil — is already a field, and the
terrain modifier stack already has an erosion slot. That is the shape of a well-prepared gap rather
than an unexplored one.

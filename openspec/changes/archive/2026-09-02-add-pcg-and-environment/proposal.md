# Procedural generation, atmosphere, and weather

## Why

Two capabilities are missing, and they are missing from the same place: the field substrate has
consumers and almost no producers.

`environment-fields` was specified as the shared world-scale data layer — biome, moisture,
temperature, wind, wetness, snow, flow, burn state — precisely so that terrain, foliage, water,
VFX, audio, and AI would sample one thing rather than each inventing masks. It works, and the
producers were deferred: **weather** was recorded as "specified as a field producer against a system
that does not exist", and **hydrology** the same. Foliage placement reads moisture that nothing
writes.

**Procedural generation** is absent entirely, and its absence is already shaping other capabilities.
Foliage has deterministic rule-based placement with an exception store. Terrain has a
non-destructive modifier stack with noise, erosion, road, and river modifiers. Water has spline
networks generating flow. Each of those is a small procedural system, built inside its own
capability, with its own determinism story, its own invalidation, and its own caching. A fourth and
fifth will follow.

The organising decisions:

> **Weather publishes environmental state; it does not touch a single tree, material, or particle.**
> Consumers derive behaviour from fields at whatever fidelity they need.

> **Procedural generation produces the representation the target subsystem wants** — foliage
> populations, terrain layers, field tiles, entity templates, spline networks — **not entities by
> default.** Ten million trees are a foliage population, not ten million spawn commands.

> **Generated content carries stable identity.** Regenerating a region must not move a designer's
> override onto a different tree, nor resurrect one a player felled.

## What changes

**`procedural-content-generation`** — CyberPCG. Typed spatial datasets rather than an object
spawner; graphs compiled to programs, the sixth time this engine uses that pattern; execution
domains so an editor-scale city generator cannot accidentally run at runtime; **region-based
generation with a dependency graph and spatial invalidation**, so editing five hundred metres of
road regenerates what that road touched and nothing else; deterministic derivation from world seed,
generator, region, and version, so changing one region does not scramble another; **stable generated
identities** and the override model that depends on them; provenance answering *why is this tree
here* and *why is nothing here*; output adapters producing the right representation per subsystem;
runtime generation under task and memory budgets; macro state for unloaded regions that materialises
into detail on demand; and caching through the derived data cache so generation distributes across
build workers.

**`atmosphere-sky-and-clouds`** — a physically based atmosphere with lookup tables rather than
per-pixel ray marching, parameterised for planets that are not Earth; celestial bodies and
time-of-day driving the sky rather than a fixed skybox; volumetric clouds reconstructed
procedurally from low-resolution weather maps, with cloud shadows as a coarse world-scale field
rather than through the shadow page system; temporal reconstruction with cloud-specific history
semantics; aerial perspective as the distance model, replacing tuned distance fog; and quality tiers
from a mobile atmosphere table to cinematic multiple scattering — all driven by the same state.

**`weather-and-wind`** — the three-layer separation of climate, weather, and presentation; weather
cells and hierarchical state written into fields; the **wind field** with layered contributions,
which foliage, VFX, cloth, water, clouds, and audio already sample and nothing produces;
precipitation with occlusion rather than per-drop collision; wetness and snow as accumulating,
decaying fields consumed by materials and virtual textures; storm cells as spatial phenomena;
weather transitions with per-property durations; determinism profiles and the firewall between
authoritative weather state and visual detail; and the replay and network model that records a storm
rather than a cloud volume.

**`environment-fields` is extended** with what these producers need: residency levels so far regions
stay coarse, field versioning and change events at region granularity, and the distinction between
**biome potential** — what the climate and soil could support — and **current biome state** — what
fire, deforestation, and terraforming have left. That distinction is what makes an ecosystem that
changes rather than a map that is painted once.

## Impact

- **New**: `procedural-content-generation`, `atmosphere-sky-and-clouds`, `weather-and-wind`
- **Modified**: `environment-fields` (residency, versioning, potential versus state),
  `terrain` (the modifier stack is a PCG consumer), `foliage` (placement rules are PCG programs),
  `rendering-global-illumination` (sky and atmosphere move; GI keeps its consumption),
  `water` (the weather seam closes; hydrology stays open), `thirdparty-dependencies`
- **Two recorded gaps close**: weather, and procedural generation beyond foliage rules. **Hydrology
  and erosion remain open**, and are now the last environmental gap — with every input and output
  they need already expressed as fields.

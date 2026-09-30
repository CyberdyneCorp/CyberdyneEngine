# Design: Lighting authoring on the lightmap bake pipeline

## Context

#68 put the lightmap bake behind one command, `lighting.bake-lightmaps`, which runs `cy_build
lightmap` over a `.cylightmap` description as an operation. The first version of this change baked
in process instead, from the runtime's live world, with its own panel, observer, density view and
bake commands. Merging both would have left two bakes. The owner chose #68's pipeline (option A):
the editor writes the description, and the one command bakes it.

## Decisions

### The editor writes the description; the tool keys it

The description is generated from the document on every bake request and never edited by hand.
It lives beside the world (`worlds/x.cyworld` → `worlds/x.cylightmap`), a project path the build
graph can declare as a `lightmap` node's source. Two layers make an unchanged world free:

1. the writer is a pure function of the world and the settings — lines in engine-identity order,
   numbers in Rust's shortest round-trip form — and `write_if_changed` leaves identical bytes alone;
2. `cy_build lightmap` computes `lightmap_level_key`, the graph's own `derivation_key` over the
   producer and its version, the toolchain, the description and every file the description made it
   read, and keeps it in `<out>.cykey` with the `baked` line. A matching key with the outputs in
   place bakes nothing and prints `cached=1`.

The key is the graph's, not a second hash scheme, so the command line and a graph node agree on
what "unchanged" means. The alternative — the editor diffing its own world — would miss an edited
mesh or material file the world only names.

### Stable identity is the engine identity

`light` and `instance` lines carry `id <engine_identity(node)>`, the number the runtime world knows
the object by, and `volume` lines lead with it. The cooked lightmap's `shadow_lights` and
`direct_lights`, and each captured volume, therefore name scene objects. Without `id` the old
numbering (lights from one, instances from zero) is kept, so an old description bakes the same
bytes.

### `cylightmap 1`, not 2

Every addition is optional and a description without them reads as before, so the parser does not
need a version to tell them apart. A tool that predates them refuses a `volume` line by name ("a
lightmap description line nothing reads") rather than misreading it.

### Materials: what the object draws

The frame draws an object's first section with its imported slot 0, else the renderer's material,
times the renderer's tint. The writer resolves the same reference and writes
`material "mN" cooked ".cy/cooked/<ref>.cyasset" [tint r g b]`; the tool reads the cooked
material's base colour and emissive colour. A material graph (`.cygraph`) or no material bakes with
the frame's default grey (0.5).

### Primitives

A `.cyprim` instance is generated with `build_primitive_mesh` and unwrapped with the importer's
`generate_uv2_cached` defaults, exactly as the primitive importer does with
`generate-lightmap-uvs`, so a world made of primitives bakes.

### Volumes in the same run

`capture_irradiance_volumes` builds the bake's tracer once more over the same scene and captures
each `gi::IrradianceVolume` with `capture_all`, a probe ray's hit returning that surface's
path-traced radiance. It reports the `probes` stage (one unit per volume, `probes 0 0` for none, so
a progress reader sees the bake reach its end) and stops on the same cancel. The payload is the
probes exactly as the volume holds them; the test compares it bit for bit with a direct capture.

### Cancel latency

`lightmap_progress_interval(samples)` = min(1024, max(1, 16384 / samples)) atlas texels between
checks: 256 at the default 64 samples, 8 at 2048. The regression case raises the cancel at the start
of a block that holds real surface and requires the bake to stop within one interval of traced
texels and within a second.

## Risks

- A mesh without UV2 that is not an `occluder` is refused by the bake with its remedy (import with
  `generate-lightmap-uvs`, or set `receives` false), as #68 already refused a hand-written one.
- Spot lights bake as points: `gi::GiLight` has no cone.
- The world authors no sky for the bake; the description writes none.

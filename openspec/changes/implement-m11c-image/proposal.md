# M11.c — Image: what the engine actually looks like

## Why

**Every renderer row in this engine is at Working, every renderer mechanism is built and gated, and
nothing the project has photographed looks like a modern engine.** That is not a complaint about
polish; it is a statement about what the record can support. The mechanisms are real and each is
tested against a reference rather than against a screenshot — which was the deliberate choice — and
**the output has never been assembled, textured or tuned.**

**What is verified absent on the tree M11.b closes on**, read off the tree rather than assumed:

- **There is no texture in this repository.** Outside `docs/`, where the project's own published
  captures live, the tree holds exactly six image files: four editor identity marks under
  `editor/assets/identity/` and two golden references under `tests/render/references/`. There is no
  albedo, normal, roughness or mask texture anywhere — no `.png`, `.tga`, `.ktx2`, `.dds`, `.exr` or
  `.hdr` that is content — for anything. **The only texture any sample binds is a
  checkerboard generated on the first frame in `samples/03-first-light/renderer.cpp`.** Every
  material in every picture the project has produced is constants.
- **The virtual-geometry capture is a debug view, by its own caption.**
  `docs/design/virtual-geometry.md` publishes the frame as *"Resolved world normals under one
  light"*, and the three level-of-detail captures beside it are *"one colour per cluster"*. The
  numbers under them are excellent — 116,928 distinct triangles presenting 4,478,208, coverage held
  at 921,592 pixels from 1 px to 16 px of error — and **none of those pictures is of a material.**
- **The newest and largest artefact never passes through the post chain.**
  `samples/10-world/CMakeLists.txt` links `cy::environment`, `cy::foliage`, `cy::pcg`,
  `cy::rendering-graph`, `cy::rendering-sky`, `cy::rhi`, `cy::terrain`, `cy::water`, `cy::weather`
  and `cy::world`. It does **not** link `cy::rendering-assembly`, `cy::rendering-post`,
  `cy::rendering-temporal`, `cy::rendering-forward` or `cy::rendering-shadows`. The world picture is
  produced without tone mapping, without an anti-aliasing stage and without the assembled frame.
- **`cy::rendering-post`, `cy::rendering-temporal`, `cy::rendering-shadows` and
  `cy::rendering-denoise` are linked by nothing outside `src/rendering/`.** M8.b built
  `src/rendering/assembly/` precisely to answer M7's finding, and it is consumed by
  `src/rendering/pipeline/` and `samples/08-vertical-slice` — two consumers, neither of them the
  world.
- **Global illumination never got its sky.** [Dependency cycle 2](../../../docs/roadmap/dependencies.md)
  is the whole reason the GI Complete cell was at M10: *"the physical atmosphere, its precomputed
  tables and volumetric clouds land at M10, and GI reaches Complete there"*. **The atmosphere landed
  and the seam was never joined.** `src/rendering/gi/` links `cy::rendering-denoise` and
  `cy::rendering-raytracing` and not `cy::rendering-sky`; `sky_light.h` names `gi::SkyTerm` only in
  comments describing the shape it mirrors; **nothing in the tree constructs one.** One requirement
  of twenty-nine is open and it is the one the cycle was about.
- **Three of the denoiser's five signals have no producer.** `denoising` declares
  `IndirectDiffuse`, `IndirectSpecular`, `RayTracedShadow`, `AmbientOcclusion` and `StochasticDirect`;
  only the first two route through it, and `src/rendering/gi/` is the only module in the tree that
  names `Denoiser` at all.
- **Occlusion culling is a CPU model.** `GpuCullPass::upload` refuses `kGpuCullOcclusion` with
  `NotImplemented` (`cull_pass.cpp:380`), there is no hierarchical depth buffer on the device, and
  cluster-granular occlusion for virtual geometry has no implementation.
- **The GPU skin pass refuses dual quaternion skinning by name** (`skin_dispatch.cpp:58`) because
  `PoseWorld` publishes matrices, and blend shapes are not applied in it (`skin_pass.h:60`) though
  the specification requires them *"in the same compute pass as skinning"*.
- **No subsystem controller reports a measured cost.** `rendering-architecture`'s arbiter is built
  and certified over 71 step magnitudes; the seven `SubsystemController`s in the tree live in
  `samples/07-fidelity` over a hard-coded cost table, and no instance under `src/` reports a
  measurement to the arbiter.
- **The material compiler has no visual front end and the shader system has no editor panel.** Both
  rows' single open requirement is the same absence read from two sides: *"Node previews use the real
  compiler"* has no graph to preview, and *"Visual material editor"* has no panel. `cy_material
  compile` prints every lowering stage on a command line.

**And one thing that is true of the whole rung: nothing has been optimised, so nothing has been
tuned.** M11.a brings the world inside its frame budget; until a frame fits, exposure, tone mapping,
anti-aliasing and every temporal decision are being judged on a picture nobody could afford to draw.

## What Changes

- **`material-compiler` and `shader-system` to Complete first, and deliberately first.** Image
  quality is expressed through them, and global illumination cannot be Complete on a material
  compiler that is not. The visual material editor is the panel M11.b built; this rung is what stands
  behind it — node previews generated through the runtime compiler, every lowering stage visible.
- **The eight rows that are the image, to Complete** — `virtual-geometry`, `virtual-shadows`,
  `rendering-global-illumination` (the row the plan promised Complete at M10 and M10 did not
  deliver), `denoising`, `ray-tracing-infrastructure`, `rendering-post-processing`,
  `temporal-rendering` and `rendering-lighting-and-shadows` — including the GI/atmosphere seam, the
  three denoiser signals with no producer, and a post chain that is actually in the frame the
  artefact photographs.
- **`rendering-culling-and-lod` and `atmosphere-sky-and-clouds` to Complete** — a hierarchical depth
  buffer on the device, cluster-granular occlusion, and the sky judged as an image rather than as a
  table.
- **`rendering-architecture` and `rendering-geometry-and-resources` to Complete** — subsystem
  controllers under `src/` reporting measured costs to the arbiter, and a skin pass that does dual
  quaternions and blend shapes where the specification says they belong.
- **`vfx-system` to Complete**, because an art-directed shot with no particles in it is a shot that
  does not exercise the row.
- **Real materials in the tree**, authored through the editor M11.b finished, encoded by the encoder
  M11.b built, and reaching the renderer as textures rather than as constants.

## Capabilities

**Fifteen rows to Complete, all fifteen from Working**, carrying **232 requirements** — nine of them
last advanced at M7 (`material-compiler`, `virtual-geometry`, `virtual-shadows`,
`rendering-global-illumination`, `denoising`, `ray-tracing-infrastructure`,
`rendering-post-processing`, `temporal-rendering`, `rendering-lighting-and-shadows`), three at M3
(`shader-system`, `rendering-architecture`, `rendering-geometry-and-resources`), two at M10
(`atmosphere-sky-and-clouds`, `vfx-system`) and one at M6 (`rendering-culling-and-lod`).

**No row in this rung is at Seed and none is from nothing.** That is the point of the rung and also
its trap: fifteen Working rows with mechanisms already built is exactly the shape in which a gate is
tempted to read a specification and feel better about it.

## What is contingent, and what this rung predicts about itself

- **This rung cannot start without M11.b's texture path.** BC7 and ASTC encoding, PNG and JPEG
  decoding and the material graph panel are M11.b's work. If any of them demotes, **the beauty shot
  degrades to another debug view and this rung should say so rather than photograph one and call it
  art direction.**
- **It cannot be judged without M11.a's budget.** A tuned frame at 122 ms is not a tuned frame.
- **`rendering-global-illumination` is the row with the longest history of being promised.** Its
  Complete cell was at M7, then at M10 because of dependency cycle 2, and it is here because the
  cycle's own condition — the atmosphere — landed and was never joined. **One requirement of
  twenty-nine is the whole difference**, which makes it the rung's cheapest Complete on paper and the
  one most likely to reveal that the seam is a design question rather than an adapter.
- **`rendering-culling-and-lod` is the row this rung predicts it may demote.** A hierarchical depth
  buffer and cluster-granular occlusion are device work, and this host has one GPU vendor. A
  device-free criterion can check the model; only a device can check the pass, and a row whose last
  requirement can only be judged on hardware nobody else has is a row whose claim has to be narrow.
- **The artefact is the contingency.** An art-directed shot is the only artefact on this ladder whose
  quality is a judgement rather than a measurement, so it comes with the obligation to publish what
  is authored content and what the renderer produced — resolution, exposure, which passes ran, what
  was hand-placed, what was procedural, what the frame cost. **A beautiful picture with an unstated
  provenance is the most efficient way to make this whole record dishonest.**

## Impact

- **New code**: the GI/atmosphere seam; producers for `RayTracedShadow`, `AmbientOcclusion` and
  `StochasticDirect`; a device hierarchical depth buffer and cluster-granular occlusion; dual
  quaternion skinning and blend shapes in the skin pass; subsystem controllers under `src/`;
  node previews through the runtime compiler.
- **Existing code**: the world and the game are rendered **through** `src/rendering/assembly/` rather
  than beside it, which is what puts tone mapping, temporal accumulation and the post chain into the
  picture the project publishes.
- **New content**: the first textures in the repository's history, and the licensing and provenance
  record for every one of them — `thirdparty-dependencies`' governance applies to content the project
  ships as much as to code it links.
- **Closing artefact**: **an art-directed beauty shot** — real materials, tone mapping,
  anti-aliasing, tuned post — assembled through the editor rather than in C++, published beside a
  statement of what was authored and what was rendered.
- **Risk**, and the rung's named spike: **one authored material, end to end, before anything is
  scoped.** Author one textured material in the editor's graph, compile it through the runtime
  compiler, encode its textures, bind it in the assembled frame, and photograph it. Everything in
  this rung assumes that path exists; nothing in the tree has ever run it.

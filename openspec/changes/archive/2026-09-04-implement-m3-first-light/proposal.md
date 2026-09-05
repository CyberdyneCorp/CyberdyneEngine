# Implement M3 — First light: a frame, drawn by a graph nobody wrote a barrier for

## Why

Three milestones have produced nothing you can look at. M3 changes that, and it is the milestone
where the engine stops being a simulation and starts being a renderer.

It is also where the single most expensive-to-retrofit commitment in the whole specification set
lands.

**Barriers are computed, not written.** The render graph owns synchronisation, transient aliasing
and pass scheduling, and no renderer code writes a barrier by hand. This is not a feature of the
render graph — it is a property of the thirtieth pass, which obeys the rule because the first one
did. Establish it now and it costs nothing; establish it at M7, after virtual geometry, virtual
shadows, temporal accumulation and GI have each hand-written their synchronisation, and removing
those barriers is a renderer rewrite rather than a change. That is why this milestone's named risk
is the graph's scheduling model under async compute, spiked before anything consumes it.

**The conventions become numbers on a GPU.** M1 made right-handed, Y-up, −Z forward and reversed-Z
with a `[0,1]` range into executable tests. M3 is where those tests stop being arithmetic and start
determining whether a depth comparison is correct at ten kilometres. Camera-relative rendering lands
here for the same reason: precision assumptions propagate into every shader and every transform
path, and a scene one million units from the origin either renders or it does not.

The third reason is quieter. **The GPU scene is the renderer's input, not the scene tree.** Nothing
walks a node hierarchy at render time. M2 built the world; M3 builds the flat, GPU-resident
representation that VFX, animation, virtual geometry and culling will all publish into from M7
onward. Getting that publication interface right while there is exactly one producer is the whole
reason to do it now.

## What Changes

Five workstreams, one milestone gate. `tasks.md` has the ordered plan; `design.md` records what the
specifications leave open and what M2's handoff makes possible.

- **The RHI and the render graph.** An explicit, Vulkan-shaped RHI; a **null backend** that runs in
  continuous integration without a GPU and is the reference for what the RHI actually requires;
  Vulkan behind it; and the graph that computes **barriers, transient aliasing and pass scheduling**
  from declared reads and writes. Parallel command recording, resource lifetime and frames in
  flight, descriptor and memory management, the backend capability model, validation.
- **The shader system.** Slang as the authoring language, SPIR-V as the interchange form,
  permutations and specialization, **reflection-driven binding**, the shader library and its cache,
  hot reload, and the rule that engine-generated shader source passes through the same pipeline as
  hand-written source.
- **The render server and the GPU scene.** A handle-based server, the simulation-to-render snapshot
  taken at M2's commit boundary, frame structure, **the GPU scene as the shared instance
  representation**, render targets, debug visualisation, deterministic submission order.
- **Geometry, materials and the frame.** Mesh representation and vertex compression, instancing,
  texture formats; the BRDF in concrete terms, shading models, image-based lighting and the standard
  material; then clustered forward — the cluster grid, light assignment, depth prepass, draw
  sorting, pass order.
- **Lights, shadows and culling at Seed.** Light types with physical units, the shadow atlas,
  directional cascades, filtering; spatial indexing, frustum culling and LOD selection. Enough for a
  lit, shadowed frame; the virtualised and GPU-driven paths are M7.

**Closing artefact**: `samples/03-first-light` — a lit, textured, shadowed scene with a moving
camera, guarded by golden images, rendering the same frame through the null backend in CI.

## Capabilities

### Advanced Capabilities

`rhi-and-render-graph`, `shader-system`, `rendering-architecture`,
`rendering-geometry-and-resources`, `rendering-materials-and-shading` and
`rendering-forward-clustered` to **Working**; `rendering-lighting-and-shadows` and
`rendering-culling-and-lod` to **Seed**. `core-math` reaches **Complete** as the conventions are
proven on a GPU rather than only in arithmetic.

### Modified Capabilities

- `simulation-and-determinism` — **the state hash is a function of entity identity, and the
  specification must say so.** M2's implementation folds the entity index into every entity node, so
  two worlds with identical component values but different ids hash differently. That is defensible
  and probably correct — an id is state the moment anything holds a reference to it, and a
  divergence report has to name an entity — but it is currently an undocumented property with real
  consequences: a streaming cell activated after different world history hashes differently even
  when its content matches, and a replay must restore ids verbatim rather than merely restore
  values. M3 is the last comfortable moment to write it down, because M6's streaming and M9's replay
  are both priced on it.

## Impact

- **New code**: `src/servers/render/`, `src/backends/rhi/{null,vulkan}/`, `src/backends/shader/`,
  `src/rendering/`, and `samples/03-first-light/`. First code at the servers and backends layers,
  which have existed as empty slots since M0.
- **New dependencies**: Slang, SPIRV-Tools, SPIRV-Reflect, volk, Vulkan-Headers and VMA — all
  already named in `thirdparty-dependencies`' intended set, each pinned in `deps/manifest.toml`
  behind an engine-owned interface, none of them visible above `src/backends/`.
- **New permanent gates**: golden-image comparison, the null backend running the same frame, a check
  that no barrier call exists outside the render graph, and a large-coordinate precision test.
- **Carried forward from M2**: seven items the gate recorded, including a `four-profiles` flake that
  a pull request is now exposed to three times over, a determinism firewall that guards zero fields,
  and a state hash covering four of seventeen subjects. Each is section 1, because M3 is what makes
  them expensive.
- **Risk**: the graph's scheduling model. If barriers and aliasing cannot be derived correctly under
  async compute, the alternative is explicit synchronisation in every pass — which is the outcome
  this milestone exists to prevent, and a roadmap change rather than something to absorb quietly.

# Virtual texturing and virtual shadows

## Why

These are the last two named gaps in the specification set, and both have been getting more
load-bearing with every change.

**Virtual textures** were recorded as a seam by `virtual-geometry` (geometry and texture residency
should share a budget), and then made urgent by `terrain`, which specifies kilometre-scale surfaces
with unique albedo, normal, roughness, and layer data and states plainly that a conventional texture
set cannot carry them. Terrain material is currently specified against a capability that does not
exist.

**Virtual shadow maps** were deliberately excluded from the illumination change — a shadow system is
not an illumination one — with the note that they were the recommended companion. The conventional
shadow atlas that exists today allocates tiles per light and renders whole shadow maps, which does
not survive the geometric density virtual geometry makes possible: a scene where every visible
surface is source-quality geometry cannot re-rasterise entire shadow maps per light per frame.

They belong in one change because they are the same architecture applied to different data, and
because the interesting failure is at their boundary — shadow rendering needs opacity masks, opacity
masks may be virtualised, and virtual texture residency is driven by visibility that depends on
shadows. That cycle has to be broken deliberately.

The organising principle, which now spans the world, geometry, texture, shadow, and illumination
systems consistently:

> **World scale must not determine memory scale.** A project may contain terabytes of source
> content, billions of texels, and billions of potential shadow texels, while a frame requires only
> what the platform's budget permits.

And its corollary, which is what makes it work:

> **Visibility and prediction determine residency; residency does not determine existence.**

## What changes

**`residency`** — the shared policy layer the engine has been converging on without naming. Four
subsystems now maintain page caches with priorities, budgets, eviction, prediction, and telemetry:
geometry, texture, shadow, and the GI caches. This capability owns the **policy** — importance,
priority scoring, budgets, eviction rules, hysteresis, deadline propagation, pressure response, and
the diagnostics that answer *why is this not resident* — while each subsystem keeps its **own
storage**, because page lifetimes and update patterns genuinely differ. It also carries deadline
propagation: when the world predicts arrival at a region, one prediction becomes geometry, texture,
shadow, audio, and illumination deadlines rather than five independent discoveries.

**`virtual-texturing`** — CyberTexture. Four residency models behind one public texture handle;
virtual address spaces, page tables, and a shared physical tile cache grouped by format class;
borders sized for filtering; an always-resident mip tail so nothing is ever missing; GPU feedback
that deduplicates and compacts before the CPU sees a request; predictive prefetch from world and
camera hints; **runtime producers** so that terrain composition, decals, world state, and procedural
surfaces write pages through the same system as disk-streamed content; UDIM and virtual lightmaps;
semantic-aware mip generation; and the derivative reconstruction that virtual texture sampling
requires under a visibility buffer pipeline.

**`virtual-shadows`** — CyberShadow. Virtual shadow address spaces per light with clipmaps for
directional lights; **receiver-driven page marking** computed on the GPU from the visible depth
buffer, so pages exist because a visible pixel needs them rather than because a caster exists; a
persistent page cache with precise invalidation derived from GPU scene transform changes; shadow
geometry error decoupled from primary geometry error; a compiled **shadow material program**
carrying only opacity and deformation; staleness as a budget lever with update classes; and a
fallback chain that never stalls the frame.

**Cross-spec** — the conventional shadow atlas becomes the shipping path for profiles that need it
rather than the only path; texture streaming gains the residency models; virtual geometry's texture
seam is closed; terrain's virtual texture seam is closed; the material compiler gains the shadow
program and the derivative requirement; and **render importance becomes one shared value** consumed
by geometry, texture, shadow, animation, and illumination rather than five unrelated notions of what
matters.

## Impact

- **New**: `residency`, `virtual-texturing`, `virtual-shadows`
- **Modified**: `rendering-geometry-and-resources`, `rendering-lighting-and-shadows`,
  `virtual-geometry`, `terrain`, `material-compiler`, `rendering-architecture`,
  `core-memory-and-containers`, `thirdparty-dependencies`
- **Both remaining named gaps are closed.** The gaps that remain are ones no subsystem is currently
  working around: weather, hydrology, procedural generation beyond foliage rules, and the deferred
  items each capability records for itself.

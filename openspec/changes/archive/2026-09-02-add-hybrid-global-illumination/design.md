# Design: CyberGI

## 1. The GI scene is a named thing, and it is not the render scene

The single most useful decision here is to give the world's illumination representation a name and
an owner, because the alternative is what the current specification has: each technique inventing
its own view of the world.

The **GI scene** is derived from the GPU scene and holds what an illumination query needs —
geometry to intersect, surfaces to read radiance from, and coarse occlusion — at a *deliberately
different fidelity* from primary visibility.

That difference is the point. Primary visibility targets sub-pixel geometric error. Illumination
does not need it:

| Query | Acceptable error |
|---|---|
| Primary visibility | ~0.5 px geometric error |
| Near-field illumination | centimetres of world-space error |
| Far-field illumination | metres |

Virtual geometry already produces a hierarchy with a continuous error metric, so the GI scene asks
it for a coarser level rather than building a separate simplification. That is a direct dividend
of the geometry work: one hierarchy, two error targets.

## 2. Two representations, chosen by capability, one interface

The GI scene carries:

- **Surface cards** — parameterised patches of surface with position, normal, and material
  attributes, which is what the surface cache is keyed on
- **A sparse distance field**, in camera-centred clipmaps, for software sphere tracing, coarse
  occlusion, and sky visibility
- **Acceleration structures** from `ray-tracing-infrastructure` where the device supports them

These are not three GI systems. They are three ways of answering the same query, behind one
interface, so a consumer asks for radiance along a ray and does not branch on hardware.

The clipmap structure is deliberate: a dense uniform world representation does not survive
kilometre-scale scenes, and camera-centred clipmaps degrade gracefully with distance instead of
falling off a cliff.

## 3. The surface cache is what makes hybrid tracing affordable

Without it, a secondary ray hitting a surface means: decode geometry, evaluate a material graph,
sample its textures, evaluate lighting. At the ray counts GI needs, that is not a real option, and
it becomes *less* of an option as materials get richer — which the material compiler work makes
inevitable.

So a hit is a **lookup**: the surface cache stores position, normal, albedo, roughness, emissive,
direct lighting, and accumulated radiance for world surfaces, and a secondary ray reads it.

Two consequences follow, and both are specified rather than implied:

- **Multi-bounce falls out for free.** The cache holds radiance including previously gathered
  indirect light, so feeding it back gives an infinite-bounce approximation over time without
  tracing paths of any length. What looks like a caching optimisation is actually the bounce
  mechanism.
- **The cache is where colour bleeding lives.** It records material reflectance, so a red wall
  tints the floor because the cache says the wall is red — which means the material system must
  supply physically meaningful albedo, and materials that cheat visually will bleed the wrong
  colour.

## 4. Materials compile a secondary program

The surface cache still has to be filled, and filling it by running full material programs
recreates the cost it was meant to avoid.

So the material compiler emits three programs per material:

| Program | Used for | Typical cost |
|---|---|---|
| Primary | Camera-visible shading | Full graph |
| Secondary | Filling the surface cache, illumination queries | Reduced samples, no microdetail |
| Far field | Distant illumination | Constants and averages |

Derivation is automatic — drop detail normals, parallax, procedural microdetail, and secondary
closures whose contribution to *outgoing radiance* is small — and **overridable**, because
automatic derivation can be wrong. A material whose albedo comes from a procedural node the
compiler treats as microdetail will bleed the wrong colour, and that is exactly the case an author
must be able to correct.

The honest cost: this is another permutation axis, and it counts against the permutation budget
like every other.

## 5. Confidence is a computed number, not a heuristic ordering

"Try screen space, then fall back" is not enough, because a screen-space hit can be wrong — thin
geometry, a surface facing away, a stale history — and a silent wrong answer is worse than a
cheap approximation.

Every radiance sample carries a **confidence**: how much the resolve should trust it, derived from
the source, hit validity, screen-edge proximity, thickness agreement, cache age, and whether the
region has been invalidated since. Resolve blends by confidence rather than switching, and the
tracer uses low confidence as the signal that spending a more expensive ray is worthwhile.

This is what turns a fallback chain into a scheduler.

## 6. Diffuse and specular share everything except the ray distribution

Reflections are not a separate system. They use the same GI scene, the same caches, the same
tracer, and the same denoiser, differing in ray distribution and in how roughness collapses the
work:

| Roughness | Strategy |
|---|---|
| Mirror-like | Dedicated traced rays |
| Moderate | Sparse rays plus radiance cache |
| Rough | Radiance cache only |

The rough case matters most for performance: a very rough surface's reflection lobe is close
enough to its diffuse gather that tracing separate rays for it is largely wasted, and the
specification says so rather than leaving it to a tuning pass.

## 7. Emissive surfaces as light, and the firefly problem

Materials have an emission closure, and the surface cache records it, so a neon sign lights a room
with no light placed. That is desirable and it is also the classic source of fireflies: a tiny,
extremely bright polygon that a stochastic sampler occasionally hits.

The resolution is a classification rather than a filter: emissive surfaces are ranked by radiant
power and area, surfaces below a threshold contribute through the cache only and are excluded from
importance sampling, and surfaces above one are **promoted to actual lights** so they are sampled
deterministically by the direct lighting path. Clamping the result would hide energy; classifying
the source spends it where it matters.

## 8. Convergence versus reproducibility

A temporally converging GI system is, by construction, history-dependent — which collides directly
with the golden-image tests the project already requires. Left unaddressed, either the tests are
flaky or GI is excluded from them, and both are bad outcomes.

So a **converged mode** is specified: the scheduler is forced to full update rates and the frame is
advanced until a convergence metric falls below a threshold or a frame cap is hit, then captured.
Golden-image tests and cinematic capture use it, and it is the same mechanism, not two.

The convergence metric is also useful at runtime, because "why hasn't this lighting change
converged yet" is a question the profiler should be able to answer.

## 9. Direct lighting stays out of the GI solver

Thousands of shadowed area lights is a *direct* lighting problem. Solving it inside the GI system
would produce two lighting models that disagree at their boundary.

So the many-light path is specified where it belongs, in `rendering-lighting-and-shadows`, as an
architected growth path: candidate generation, reservoir sampling with temporal and spatial reuse,
and ray-traced visibility, with clustered lighting as the shipping default. It is sequenced after
the denoiser because stochastic direct lighting is a noisy signal and is not usable without one —
which is a real dependency, not a preference.

## 10. Baked lighting is not a legacy path

Dynamic GI is not mandatory, and treating baked lighting as a compatibility leftover would be a
mistake for two independent reasons: low-end hardware genuinely benefits, and baked data can
**seed** the dynamic caches so a level looks correct on the first frame instead of converging from
black.

So GI modes are `None`, `Baked`, `Probe`, `Dynamic`, and `Hybrid`, and the offline path tracer
serves three purposes at once — baking, seeding, and ground truth for validating the real-time
result. Building it once for all three is why it is worth building at all.

## 11. Budget, and where the levers are

GI holds an allocation from the renderer budget arbiter, exactly like geometry and VFX, and
distributes it internally: probe update count, ray counts per tier, tracing resolution, cache
density, reflection resolution, and denoiser quality.

The differentiating part is not the controller but its inputs. The engine already knows things a
renderer usually does not — per-object gameplay importance from the ECS, geometric error from the
virtual geometry hierarchy, residency from the streaming system. So GI quality can be distributed
by *what matters in the game*, not only by distance from the camera: a hero unit gets probe
density and traced reflections while the far battlefield gets a low-frequency cache, inside one
GPU-time target.

That is the claim worth making — not "better than Lumen", which would be unserious, but that
importance-aware distribution under a global budget is a structural advantage of having built the
GPU scene, the arbiter, and the geometry hierarchy first.

## 12. Build order

| Phase | Contents | Proves |
|---|---|---|
| 1 | Physical direct lighting, clustered lights, shadows, IBL, reflection probes | The lighting equations are right |
| 2 | Screen-space GI over the temporal framework, first denoiser | Temporal GI works at all |
| 3 | Radiance cache: adaptive probes, clipmaps, priority scheduler | Off-screen GI that scales |
| 4 | GI scene, surface cache, distance fields, software tracing | Dynamic GI without hardware RT |
| 5 | Hardware tracing as a tier of the same tracer | Hardware helps, is not required |
| 6 | Reflections unified into the tracer, roughness strategy | One system, two outputs |
| 7 | Stochastic many-light direct lighting | Thousands of shadowed lights |
| 8 | Path tracer, baking, ground-truth comparison | The result is verifiable |

**Phase 3 is the milestone that matters.** Screen-space GI is a demo; a scheduled world-space
radiance cache is the first thing that scales, and every later phase plugs into it.

## 13. Gaps

- **Virtual shadow maps** — deliberately excluded; a shadow system, not an illumination one. The
  recommended next change.
- **World partition** — the GI scene is specified as cell-scoped and evictable, against a
  capability that still does not exist. This is now the **fourth** subsystem to specify a seam
  into it, after networking, virtual geometry, and renderer profiles at open-world scale.
- **Virtual textures** — still unspecified, and now also relevant to surface cache residency.
- **Volumetric clouds** and **participating media beyond froxel fog** — interface reserved,
  behaviour unspecified.

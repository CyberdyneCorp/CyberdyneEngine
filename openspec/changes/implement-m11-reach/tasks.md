# Tasks: M11 — Reach

**This is a decision list, not an implementation plan.** The change was opened by M10's closing gate
(M10 task 8.5, checked by `m10:m11-open`) so the ladder continues as a deliberate act. Section 0 has
to be answered before any of the rest can be written honestly, and nothing below section 0 should be
expanded into a task list until it is.

## 0. The decisions that scope this milestone

- [ ] 0.1 **Is M11 one milestone or two?** The matrix has said since M6 that M11 is the one milestone
      that could reasonably be split, "through a change if the work turns out to be separable along a
      real seam rather than an arbitrary one". Its load is now **65 of 76 capabilities**, seventeen of
      which arrived at M10's gate alone. `proposal.md` names the seam it thinks is real — a
      *platform and reach* group, which is what `delivery-roadmap`'s M11 row is actually about,
      against an *editor and authoring* group and a *finishing* group, which are accumulated debt.
      Splitting the ladder is an OpenSpec change against `delivery-roadmap` by that specification's
      own rule, and `record.MILESTONES`, `gates.toml`, `selftest.MINIMUM_CRITERIA`, the matrix
      columns and the load table all move with it
- [ ] 0.2 **The milestone's named spike.** `docs/roadmap/risks.md` names the spike for each
      milestone; decide M11's before scoping the backends, and run it at the head of the milestone
      rather than inside it
- [ ] 0.3 **What `save-and-persistence` actually costs.** `proposal.md` carries M10's
      requirement-by-requirement audit — nine satisfied, three unmet, eight partial, eleven pieces of
      work — as a finding handed forward rather than a scope. Decide whether the row is completed
      here, split, or re-scoped through a change against its own specification. The AEAD half is a
      **dependency adoption** and `thirdparty-dependencies` requires it to go through the OpenSpec
      change flow with the evaluation recorded, so it is a change of its own either way

## 1. The three inherited gaps that are one piece of work

`m10:fields-sampled-on-a-device`, `m10:world-frame-budget` and — the only credible route to the
second — a field sampler on the device. Scoping these together is the point; scoping them apart is
how the artefact stays at seven times its budget.

- [ ] 1.1 `cy/field.slang` and a shader-side environment-field sampler, which `src/environment/`'s
      README says is owed by the first renderer-facing row to sample a field in a shader. Closes
      `m10:fields-sampled-on-a-device`, whose current measurement is **zero `.slang` modules**
- [ ] 1.2 The three bands `m10:world-frame-budget` names, in that order: the substrate re-sampled at
      every terrain vertex (63.0 ms), the cloud march (23.2 ms) and water's foam field (12.0 ms). All
      three are work a shipping engine does in a shader and all three are recorded as GPU debts in
      M10's own module READMEs
- [ ] 1.3 `m10:sky-field-round-trip` — the sky's write path into `FieldStore`, and the two assertions
      parked in `src/rendering/sky/tests/test_cloud_shadows.cpp` restored as the check. **And the
      consumers**: `Cloud shadows` requires the field to be "consumed by terrain, foliage, water, and
      illumination" and nothing outside `src/rendering/sky/` reads it
- [ ] 1.4 `m10:fields-one-vegetation-potential` — a modelling decision across two rows before it is a
      code change. `weather-and-wind` owns macro ecosystem state including vegetation density;
      `FoliageSystem::register_producer()` declares and claims a `vegetation` field of its own.
      Whether those are one quantity with two names is the question, and `integration.standard_fields`
      goes red on purpose the day it is answered, because it asserts the known state

## 2. The one CI job that answers three criteria

- [ ] 2.1 A continuous-integration job that publishes one leg's digest and compares it with
      another's. `m9:lockstep-cross-platform`, `m10:pcg-regeneration-cross-platform` and
      `m10:pcg-gpu-domain-agreement` each look for exactly this shape and each fails or reports NOT
      EVALUATED for want of it. The legs already exist — `ci.yml`'s matrices include `linux-arm64`,
      `macos-arm64` and `windows-arm64` — and they run independently

## 3. The milestone as planned

Not expanded until 0.1 is answered.

- [ ] 3.1 `rhi-and-render-graph` → Complete: **Metal** (native, not a translation layer) and
      **D3D12** to parity with Vulkan
- [ ] 3.2 `core-platform-abstraction` → Complete: a native `Platform` and `DisplayServer` backend for
      one desktop platform, replacing SDL3 there and requiring no change in `src/core/`, `src/ecs/`,
      `src/servers/` or `src/scene/`
- [ ] 3.3 `build-system-and-platforms` → Complete: cross-compilation, the porting surface, mobile
      targets, distribution artefacts, the full CI matrix
- [ ] 3.4 `build-and-packaging` → Complete: content audit, provenance and symbols, downloadable
      content, distributed execution
- [ ] 3.5 `rendering-forward-clustered` → Complete: mobile pipeline differences, MSAA, multi-view
- [ ] 3.6 `m8c:steam-audio-configures` — the cost is already measured in full in `deps/manifest.toml`
- [ ] 3.7 `testing-and-quality` → Complete: the full gate set and the documentation gate
- [ ] 3.8 Every remaining capability to Complete, or an explicitly recorded deferral with its
      re-entry point. `xr-support` stays deferred with its prerequisites checked
- [ ] 3.9 `samples/11-ship` — one project built, cooked, packaged and launched on every supported
      target from a single recipe

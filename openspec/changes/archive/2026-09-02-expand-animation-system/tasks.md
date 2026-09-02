# Tasks: CyberAnimation

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis.

Sections 3 to 9 record the implementation the decision implies and are **deliberately deferred to
implementation changes**. They are listed so the scope is not lost. The graph and rig compilers (sections 3 and 5) and the pose search index
(section 6) are where the risk concentrates; see `design.md`.

## 1. Specification

- [x] 1.1 Record rationale, the root-motion determinism resolution, and rejected alternatives in
      `design.md`
- [x] 1.2 Expand `animation-and-skinning`: asset model with skeleton/rig separation, compiled
      programs, pose/skinning separation, GPU pose world, animation and bone LOD, pose sharing,
      motion matching and pose search, control rig, constraint framework with full-body IK,
      semantic retargeting, warping, layers/masks/sync groups, curves, physics animation, facial,
      determinism, batching, streaming, and authoring
- [x] 1.3 Modify skeleton, evaluation, root motion, events, compression, and diagnostics to match
- [x] 1.5 Consolidate: the old "Inverse kinematics and modifiers" and "Retargeting" requirements
      are removed rather than modified, since the new constraint framework and semantic-chain
      retargeting supersede them; every solver and modifier they listed is preserved
- [x] 1.4 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `rendering-geometry-and-resources` — skinning now reads the GPU pose world and respects
      animation and bone LOD tiers
- [x] 2.2 `physics` — ragdolls gain powered and partial modes with per-body continuous blending,
      derived from a ragdoll profile asset
- [x] 2.3 `asset-import-pipeline` — model import gains bone LOD derivation, error-bounded
      animation compression, retarget profiles, and optional tool-time USD
- [x] 2.4 `thirdparty-dependencies` — animation runtime recorded as engine-built; ACL added as an
      evaluation and OpenUSD as optional tool-time only, with the "capability, not library"
      framing made explicit
- [x] 2.5 `build-system-and-platforms` — add `CY_ANIMATION` and require tool-time dependencies to
      be excluded from shipped runtimes
- [x] 2.6 `vfx-system` — reviewed; no change needed. Its skeletal-mesh data interface is the
      mechanism by which VFX reads the GPU pose world, and that contract is unchanged.
- [x] 2.7 `ai-system` — reviewed; no change needed. AI requests movement and animation states; the
      root-motion determinism rule mirrors the AI scheduling rule already specified there.

## 3. Core runtime (deferred to implementation)

- [ ] 3.1 Skeleton asset: flat arrays, bone LOD levels, per-joint bounds
- [ ] 3.2 Pose representation, local-to-global resolution, bone matrix production
- [ ] 3.3 Animation graph IR and compiler, including pose dependency analysis
- [ ] 3.4 Batched evaluation by shared program, SIMD inner loops
- [ ] 3.5 Layers, masks, additive blending, sync groups and marker alignment
- [ ] 3.6 Deterministic CPU root motion extraction and integration across reduced rates

## 4. GPU pose and skinning (deferred to implementation)

- [ ] 4.1 GPU pose world: allocation, current and previous matrices, add/remove without rebuild
- [ ] 4.2 CPU pose upload and GPU pose evaluation paths
- [ ] 4.3 Skinning consuming the pose world; verify VFX attachment reads it without readback
- [ ] 4.4 Pose textures and vertex animation for the `Baked` tier

## 5. Rigging and constraints (deferred to implementation)

- [ ] 5.1 Rig asset and rig graph IR and compiler
- [ ] 5.2 Constraint framework: ordering, conflict detection, blend weights
- [ ] 5.3 Solvers: two-bone, FABRIK, CCD, spline, look-at, aim, foot placement, spring bones
- [ ] 5.4 Full-body IK with per-effector weights and priorities
- [ ] 5.5 Retarget profile, semantic chain mapping, offline baking and runtime retargeting

## 6. Motion matching (deferred to implementation)

- [ ] 6.1 Feature extraction and pose database build
- [ ] 6.2 Deterministic search index and batched runtime query
- [ ] 6.3 Continuity bias, exclusion masks, per-feature weight tuning
- [ ] 6.4 Warping: motion, stride, orientation, and distance matching

## 7. Scale (deferred to implementation)

- [ ] 7.1 Animation LOD tiers with hysteresis and blended promotion
- [ ] 7.2 Bone LOD evaluation and mesh LOD influence correspondence
- [ ] 7.3 Pose cache with phase bucketing and per-instance variation
- [ ] 7.4 Animation budget reporting and tier distribution
- [ ] 7.5 Event emission policy at reduced tiers

## 8. Physics and facial (deferred to implementation)

- [ ] 8.1 Ragdoll profile generation from a skeleton, and refinement tooling
- [ ] 8.2 Powered and partial ragdoll with per-body blending
- [ ] 8.3 Hit reactions blending back to animation
- [ ] 8.4 Facial: blendshapes, bone rigs, curve parameters, viseme model
- [ ] 8.5 Audio-driven and ML-driven facial paths

## 9. Tooling and validation (deferred to implementation)

- [ ] 9.1 Rigging workspace: skeleton tree, viewport, rig graph, constraint setup, pose debugging
- [ ] 9.2 Skinning tools: automatic weights, painting, normalise, mirror, prune, influence limits
- [ ] 9.3 Retarget authoring with side-by-side preview
- [ ] 9.4 Animation diagnostics and motion matching inspection
- [ ] 9.5 Compression evaluation: ACL versus an engine implementation, against error bounds,
      ratios, decode cost, and integration cost
- [ ] 9.6 Determinism tests: root motion identical under re-simulation, across tiers, and with GPU
      pose evaluation active
- [ ] 9.7 Golden-image tests for skinning, IK, and retargeting across backends
- [ ] 9.8 Benchmarks: 1k / 10k / 50k animated instances across tier mixes, as regression guards

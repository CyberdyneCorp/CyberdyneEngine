# Expand animation into CyberAnimation: compiled, GPU-aware, and built for crowds

## Why

The `animation-and-skinning` specification describes a competent conventional animation system:
clips, a blend graph, IK modifiers, retargeting, and events. It is roughly Unity-class. It has two
structural gaps that matter for this engine.

**Scale.** Everything in it assumes per-character CPU evaluation. The engine targets scenes with
tens of thousands of animated agents; the AI system already tiers 100,000 agents by reasoning
fidelity, and animation is the subsystem that will make that pointless if every visible unit
evaluates 150 bones on the CPU every frame. Animation needs the same treatment the renderer, VFX,
AI, and audio have: LOD tiers, pose sharing, budget awareness, and GPU-side work.

**Authoring depth.** It conflates the runtime deformation hierarchy with the control system artists
manipulate, has no procedural rigging, and treats retargeting as a bone-name mapping. Those are the
things that separate an engine you can ship a character-driven game on from one you cannot.

Animation also couples to more subsystems than almost anything else — renderer, physics, AI, VFX,
audio, asset streaming — which is the argument for specifying it properly now rather than
retrofitting it around decisions made elsewhere.

## What Changes

- **Skeleton and rig become separate assets.** A skeleton is the runtime deformation hierarchy; a
  rig is the artist-facing control system. Conflating them makes runtime data heavy and rigging
  impossible.
- **Animation graphs and rigs are compiled** to programs shared across all instances using them,
  following the same pattern as VFX and AI graphs.
- **Pose evaluation is separated from skinning**, with a **GPU pose world** holding bone matrices
  and previous-frame poses, consumable by skinning, VFX attachments, and rendering without CPU
  readback.
- **Animation LOD tiers** reduce evaluation rate *and* fidelity — full graph, simplified graph,
  cached pose, and pose-texture or impostor — alongside **bone LOD** that drops facial, finger, and
  twist joints with distance.
- **Pose sharing**: many characters in the same cycle sample a shared pose cache rather than each
  evaluating the same clip.
- **Motion matching and pose search** are built in, with an offline-built pose database, extracted
  features, and an indexed runtime search.
- **A control rig system** — procedural rigging authored visually, compiled, running in editor and
  at runtime.
- **A unified constraint framework** over the IK solvers, including **full-body IK**.
- **Retargeting by semantic chains** through a retarget profile asset, offline or at runtime.
- **Animation warping**: motion, stride, and orientation warping, and distance matching.
- **Physics animation**: powered ragdoll, partial ragdoll, and hit reactions through Jolt, rather
  than a hard switch from animation to ragdoll.
- **Facial animation** as a specified layer, with blendshapes, visemes, and an ML-driven path.
- **Sync groups and markers**, layers and masks, and animation curves consumable by other systems.
- **Compression and streaming**: aggressive clip compression with error bounds, and streamed
  playback for long clips.
- **A determinism contract.** Root motion drives the character controller, so it is gameplay: root
  motion SHALL be computed on a deterministic path even when the rest of the pose is evaluated on
  the GPU or at a reduced tier.

Non-goals: becoming a DCC application. The engine will fix and tune rigs and weights; authoring
characters remains external. Also deferred: cloth simulation as a first-class system, and crowd
animation authoring tools.

## Capabilities

### Modified Capabilities

- `animation-and-skinning` — substantially expanded: asset model, compiled programs, GPU pose
  world, LOD and pose sharing, motion matching, control rig, constraint framework, warping,
  physics animation, facial, determinism, authoring, and diagnostics.
- `rendering-geometry-and-resources` — skinning consumes the GPU pose world rather than a
  per-skeleton bone buffer supplied ad hoc.
- `physics` — ragdolls gain powered and partial modes driven from an animated pose.
- `asset-import-pipeline` — animation import gains compression settings, retarget profiles, and
  optional USD.
- `thirdparty-dependencies` — record CyberAnimation as engine-built; add ACL and optional OpenUSD.
- `build-system-and-platforms` — add `CY_ANIMATION`.

## Impact

- **Dependencies**: adds ACL for clip compression (MIT), and OpenUSD as an **optional, tool-time
  only** importer — flagged because OpenUSD is a large dependency, unlike ufbx.
- **Renderer**: the GPU pose world becomes a shared resource between animation, skinning, and VFX.
- **Physics**: powered ragdolls require joint motors driven from a target pose each tick.
- **Determinism**: animation joins AI as a subsystem whose LOD scheduling is constrained by
  gameplay determinism.
- **Risk**: the graph and rig compilers, and the pose search index, are where risk concentrates.

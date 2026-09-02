# Input, actions, and the camera system

## Why

The gameplay framework established that gameplay consumes **commands**, never key events, and that
human, AI, network, replay, and automation all produce the same stream. It did not specify the layer
that turns a thumbstick into an intent, and the engine currently has only a platform-level action
map: named actions bound to inputs, with no users, no contexts, no processors, no triggers, and no
notion that two people might be playing on one machine.

The camera is missing entirely, and it is missing from more places than it looks. Nine capabilities
already assume something the camera has to provide: the renderer needs views, the world needs
streaming sources with velocity and prediction, the temporal framework needs cut events and history
identity, illumination and residency need view importance, audio needs a listener, gameplay needs a
screen ray for selection and a reference frame for movement, and the strategy scenario in the
benchmarks needs a camera that frames an army rather than following a character.

They belong in one change because the seam between them is where both are usually got wrong:
camera-relative movement, look sensitivity, aim, edge scrolling, and selection are all input
producing camera intent producing world queries, and specifying either half alone leaves that seam
to be improvised.

Two contracts:

> **Devices produce actions; actions produce intent. Gameplay never sees a key, and rebinding
> therefore cannot change gameplay behaviour.**

> **A camera is a composable rig producing an evaluated state, from which a render view, an audio
> listener, and a streaming source are derived. It is not a scene object, not a base class to
> inherit from, and not the same thing as the entity being controlled.**

## What changes

**`input-and-actions`** — the platform boundary and timestamped events; **input users** with
explicit device ownership, so local multiplayer and hot-plugging are structural rather than
retrofitted; typed action values; a **layered context stack** with consumption and priority, so a
modal, an inventory, a vehicle, and default gameplay coexist without globally toggling maps;
bindings and composites compiled to identifiers rather than parsed paths; **processors** for
numerical shaping and **modifiers** for contextual alteration; **triggers** with a full action
lifecycle; control schemes with hysteretic device detection; **fixed-tick sampling** that preserves
timestamps rather than reading whatever value happened to be current; command frames for continuous
control; input buffering; rebinding with defaults kept immutable and overrides stored per profile;
accessibility as a first-class part of the model; text entry kept separate from key actions;
synthetic and remote input for tests and device testing; diagnostics that answer *why did this not
trigger*; and forbidden patterns.

**`camera-system`** — the four separated concepts (definition, rig, evaluated camera, render view);
**rig graphs compiled to programs**, following the same pattern as materials, VFX, animation, and AI;
targets that may be entities, groups, bounds, or splines; camera **intent** distinct from gameplay
control, so the camera need not follow what you are driving; a per-local-player **camera stack** with
priorities and well-defined blending; a lens model covering both gameplay and physical camera
parameters; framing and composition constraints in screen space; stable spring smoothing expressed
in half-lives rather than frame-rate-dependent interpolation; collision and **occlusion as separate
queries**; aim as a rig layer with the view-aim, control-aim, and weapon-aim distinction that
shooters need; shake and recoil as an **additive impulse bus**, so gameplay never writes a camera
transform; camera volumes; a first-class **strategy camera**; large-world camera positions and
camera-relative rendering; **camera cuts as typed events** that notify the temporal, illumination,
shadow, texture, and world systems; the screen-ray and projection API that selection and placement
need; the audio listener anchor; and the **streaming source with prediction** that the world has been
expecting.

**`core-platform-abstraction`** keeps devices, normalisation, and timestamps, and its action-mapping
requirement is superseded: the action model now belongs to a capability that can carry users,
contexts, processors, and triggers.

## Impact

- **New**: `input-and-actions`, `camera-system`
- **Modified**: `core-platform-abstraction` (the platform boundary; the action layer moves),
  `ui-system` (interface actions use the same model and focus routing),
  `rendering-architecture` (render views are produced from evaluated cameras),
  `thirdparty-dependencies`
- **Recommended next**: `save-replay-and-determinism`. With gameplay, input, and camera specified,
  the engine finally has every concept needed to define what a simulation tick contains, what is
  recorded, and how a session is reconstructed exactly.

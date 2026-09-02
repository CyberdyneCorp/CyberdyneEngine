# Tasks: CyberInput and CyberCamera

Specification-stage change. Sections 1 and 2 are the work of this change and are complete; the
change is archived on that basis. Sections 3 onward are the implementation backlog, sequenced by the
build order in `design.md`.

## 1. Specification

- [x] 1.1 Record in `design.md`: the three layers and their boundaries, input users making local
      multiplayer structural, the context stack rather than toggles, processors versus modifiers,
      mouse-as-delta versus stick-as-rate, fixed-tick sampling preserving time, the camera's four
      separated concepts, rigs compiling to programs, gameplay never writing a camera transform, the
      target not being the controlled entity, collision versus occlusion, the three meanings of aim,
      cuts as events with consequences, one evaluated camera producing three products, evaluation
      timing modes, strategy cameras as first-class, and the build order
- [x] 1.2 New `input-and-actions` (23 requirements): the platform boundary, users and device
      ownership, device lifecycle, actions and value types, mapping contexts, bindings and
      composites, processors, modifiers, triggers and the action lifecycle, action state and edge
      queries, control schemes and detection, fixed-tick sampling, buffering, rebinding and
      profiles, accessibility, interface routing and focus, text entry, synthetic and remote input,
      assets and cooking, headless operation, diagnostics, performance, and forbidden patterns
- [x] 1.3 New `camera-system` (28 requirements): the four concepts, cameras without scene objects,
      compiled rig graphs, targets, intent, the stack and blending, the lens model, follow and orbit
      and constraints, stable smoothing, collision and occlusion, framing and composition, the three
      aims, aim assistance, shake and recoil and the impulse bus, volumes, the strategy camera,
      projection utilities, large-world positions, render view production, cuts, listener and
      streaming source, evaluation timing, networking and replay, spectator and photo and director
      cameras, settings and accessibility, diagnostics, performance, and forbidden patterns
- [x] 1.4 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `core-platform-abstraction` — "Input actions" removed with a supersession note: an action
      model needs users, contexts and triggers, none of which belong in a platform abstraction. Both
      original scenarios are preserved in the successor. "Input" now ends at normalised timestamped
      events, and "Fixed-step input handling" states the guarantee the platform must make possible
      by not coalescing transitions away.
- [x] 2.2 `ui-system` — interface actions live in mapping contexts on the input user's stack, so a
      modal consumes what it uses and gameplay bound to the same controls is suppressed; glyph
      switching uses the specified hysteresis
- [x] 2.3 `rendering-architecture` — views are produced from evaluated cameras, which supply pose,
      projection semantics, viewport, importance and history identity, and jitter remains the
      temporal framework's concern
- [x] 2.4 `thirdparty-dependencies` — the action model and camera system recorded as engine-built,
      with device backends integrated beneath them
- [x] 2.5 **Producers found for existing consumers.** Nine capabilities already assumed something the
      camera had to provide and had no producer: render views, streaming sources with velocity and
      prediction, temporal cut events and history identity, view importance for residency and
      budgets, the audio listener, screen rays for selection, and reference frames for
      camera-relative movement. All are now produced from one evaluated camera.
- [x] 2.6 `gameplay-framework`, `world-partition-and-streaming`, `temporal-rendering`, `residency`,
      `audio`, `terrain`, `physics` — reviewed; no change needed. The command stream, streaming
      sources with shapes and prediction, invalidation events including camera cuts, deadline
      propagation, the listener, terrain height queries, and batched shape queries already accept
      what these capabilities publish.

## 3. Input (deferred)

- [ ] 3.1 Timestamped device events and the platform boundary
- [ ] 3.2 Input users, device ownership policies, device lifecycle and reassignment
- [ ] 3.3 Action definitions, value types, cooked identifiers
- [ ] 3.4 Bindings, composites, interpretation metadata, the context stack with handles
- [ ] 3.5 Processor chains without per-frame allocation; modifiers with reference frames
- [ ] 3.6 Triggers and the full action lifecycle
- [ ] 3.7 Control schemes with hysteretic detection
- [ ] 3.8 Rebinding, conflict policies, profile overrides, accessibility transformations
- [ ] 3.9 Fixed-tick resolution and command frames — the input milestone
- [ ] 3.10 Buffering; synthetic and remote input; input inspector, event trace and latency view

## 4. Camera (deferred)

- [ ] 4.1 Camera definition, evaluated camera, render view separation — the camera milestone
- [ ] 4.2 Follow and orbit rigs; camera stack with priorities and blending
- [ ] 4.3 Frame-rate independent smoothing with resettable state
- [ ] 4.4 Lens model covering gameplay and physical parameters; orthographic and infinite far
- [ ] 4.5 Batched collision and occlusion with declared responses
- [ ] 4.6 Framing and screen-space composition constraints
- [ ] 4.7 Aim layer with view, control and weapon aim separated; aim assistance as a modifier
- [ ] 4.8 Impulse bus: shake, recoil, spatial attenuation, player scaling
- [ ] 4.9 Camera volumes through the spatial index
- [ ] 4.10 Strategy camera: pan, zoom curve, edge scroll, terrain following, map bounds
- [ ] 4.11 Large-world positions and the rendering origin
- [ ] 4.12 Render view production with importance and history identity
- [ ] 4.13 Cut events and anticipated cuts as deadlines
- [ ] 4.14 Listener anchor and streaming source with prediction
- [ ] 4.15 Evaluation modes with hybrid as the default
- [ ] 4.16 Spectator, photo mode and director cameras
- [ ] 4.17 Rig graph compiler
- [ ] 4.18 Camera inspector, rig graph debugger, collision debugger, streaming attribution

## 5. Validation (deferred)

- [ ] 5.1 Fast press and release within one frame is observed by the tick
- [ ] 5.2 Command frames reproduce identically from the same event stream — a replay precondition
- [ ] 5.3 Look sensitivity is frame-rate independent for mouse and correct for stick
- [ ] 5.4 Context consumption: a modal suppresses gameplay actions, and unwinding out of order is
      correct
- [ ] 5.5 Rebinding does not modify shipped assets; overrides survive a content update
- [ ] 5.6 Accessibility transformations cannot be bypassed by any supported gameplay path
- [ ] 5.7 No allocation in input evaluation for a frame; eight users with full binding sets remain
      negligible
- [ ] 5.8 Camera smoothing settles over the same wall-clock time at 60 and 144 frames per second
- [ ] 5.9 Cut handling: temporal history invalidated, illumination and shadow notified, no smear
- [ ] 5.10 Camera is not authoritative: a manipulated client camera cannot affect server-validated
      hits
- [ ] 5.11 Listener and streaming source derive from the evaluated camera and cannot disagree with it
- [ ] 5.12 Strategy camera: terrain following uses terrain queries, not per-frame physics casts
- [ ] 5.13 Camera evaluation allocates nothing per frame and stays within its declared budget
- [ ] 5.14 Forbidden-pattern checks for both capabilities

---

**Archived 2026-09-02.** Sections 1 and 2 are complete: `input-and-actions` and `camera-system` are
in `openspec/specs/`, and four capabilities were updated — including `core-platform-abstraction`,
whose action-mapping requirement is superseded because an action model needs users, contexts and
triggers that do not belong in a platform abstraction. The change also found producers for nine
existing consumers that had been specified against a camera that did not exist. The unchecked items
from section 3 onward are the implementation backlog; **fixed-tick command frames and the
camera/render-view separation are the two milestones**, because the first is replay's substrate and
the second keeps renderer concepts out of gameplay for the life of the project.

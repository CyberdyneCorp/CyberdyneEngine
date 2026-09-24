# Design

## Context

See proposal.md. `scene.create-primitive` already writes `.cyprim` and adds a mesh entity. The editor currently rejects nonempty entity templates even though the engine declares camera and light templates. The hosted renderer builds mesh instances from the serialized world but supplies one fixed light. The shell and runtime exchange one viewport image; Play currently steps physics while that image continues to use the editor camera and gizmo.

## Goals / Non-Goals

**Goals:** Reuse the normal scene transaction and component schema for created entities; render authored light values and transforms; keep editor and game view state distinct; make unsupported runtime services visible.

**Non-Goals:** A second rendering implementation, a new scene file format, or implementing the entire Swift and audio subsystems inside editor UI code.

## Decisions

1. Expose direct Plane creation by invoking the existing primitive command with `shape=plane`. Add camera and light creation as typed scene commands that look up component and field identities in the document schema, then add Transform and the relevant component in one transaction. This keeps GUI and MCP behavior identical. Avoid a widget-only mutation path.
2. Treat authored lights as world components, not editor preferences. Extract values and composed transforms every frame, including live edits. Preserve a separate editor-only lighting fallback while migrating empty projects so Game view can truthfully render no authored lights. Avoid copying editor light settings into the scene.
3. Represent Editor and Game as separate view intents backed by the existing engine rendering path. Editor uses the navigation camera and editor gizmos; Game uses a primary enabled scene camera and suppresses all editor adornment. View selection does not mutate the scene. A missing camera produces an explicit placeholder. Prefer one active transport image at a time initially, since the hidden view should consume no GPU time; the model can later expose simultaneous docked views through two transport subscriptions.
4. Play changes simulation state in the hosted runtime and selects Game view in the shell. Stop restores the document snapshot and previous Editor camera. Report physics, script, and audio service readiness separately. Script and audio wiring belongs in the runtime, never in the Rust shell.

## Risks / Trade-offs

- [World field names differ between authoring schema and C++ reflection] → resolve by schema names and add round-trip tests using real `.cyworld` bytes.
- [A single active image is insufficient for simultaneous Editor and Game tabs] → keep separate view state now, then extend transport to two consumers only when simultaneous display is requested; inactive views cost nothing.
- [Older worlds relied on the fixed editor sun] → keep an editor preview light only where needed and show zero authored lights in Game view.
- [Swift or audio services are absent from this sample runtime] → surface a clear unsupported status and track runtime integration as an explicit task rather than silently claiming full Play support.

## Migration Plan

Existing worlds load without conversion. New component types use fields already recognized by the scene schema. Rolling back the editor and host restores the previous UI; worlds containing standard camera and light components remain valid.

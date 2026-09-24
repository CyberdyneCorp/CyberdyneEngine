# Proposal

## Why

The editor can place imported meshes but cannot author lights through its scene controls, and its single viewport mixes editing controls with the image shown during Play. A scene needs authorable lighting and a distinct game presentation to judge what will run.

## What Changes

- Expose Plane, directional light, point light, spot light, and camera creation in the scene UI and command interface.
- Save and reload authored lights and cameras, and use authored lights in the engine renderer.
- Provide Editor and Game viewport modes. Editor shows navigation and editing gizmos; Game shows the active scene camera and excludes editor overlays and selection handles.
- Make Play enter Game view, run the project simulation, and restore Editor view on Stop. Physics, gameplay scripts, and audio must report their actual availability rather than silently appearing to run.
- Cover scene creation, persistence, rendering, and view transitions with regression tests and user documentation.

## Capabilities

### Modified Capabilities

- `editor-viewport-and-gizmos`: Distinguish the editor camera and gizmos from a clean scene-camera Game view, including Play transitions.
- `editor-rust-application`: Make scene creation actions discoverable in the editor and command interface.
- `rendering-lighting-and-shadows`: Use authored light components in hosted scene rendering.
- `live-editing`: Coordinate Play view and simulation status with the hosted runtime.

## Impact

Editor scene commands and hierarchy UI, world component serialization, hosted renderer and bridge, Play controls, OpenSpec, tests, and editor documentation change. No saved-world format migration is intended.

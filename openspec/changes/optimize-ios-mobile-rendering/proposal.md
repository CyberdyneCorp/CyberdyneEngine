# Proposal

## Why

The first native iPhone run proves the Metal path but renders the reference open-world workload at
only 17.30 FPS on an iPhone 16. The mobile sample needs an explicit pixel and shader-cost policy so
it remains interactive while preserving the visible terrain and day/night presentation.

## What Changes

- Render the 3D workload at a mobile internal resolution while UIKit keeps native-resolution UI.
- Reduce redundant terrain work in the reference fragment shader without removing its scene
  features.
- Report render scale, drawable size, and physical-device FPS as machine-checkable evidence.
- Require a sustained physical-device target and retain the before/after measurement.

## Capabilities

### New Capabilities

- `ios-mobile-rendering`: Defines the iOS reference workload's resolution, scene-fidelity,
  diagnostics, and physical-device performance contract.

### Modified Capabilities

None.

## Impact

The iOS ship sample, device measurement tool, iOS documentation, and physical-device evidence are
affected. Shared rendering interfaces and desktop renderer paths are unchanged.

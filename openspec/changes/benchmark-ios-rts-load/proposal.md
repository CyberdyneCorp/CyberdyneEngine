# Proposal

## Why

The combined iPhone presentation proves one skinned model and one GPU emitter, but that is not a
useful capacity test for an RTS frame. We need a reproducible device workload at the requested
unit and effects counts before deciding where mobile batching work belongs.

## What Changes

- Expand the iOS presentation to skin and draw 500 model meshes in one batched skin dispatch.
- Simulate and draw 100 independently resident GPU VFX emitters with no CPU particle readback.
- Extend the device marker and evidence report with model, emitter, vertex, dispatch, and particle
  totals, then measure the result on the connected iPhone.

## Capabilities

### New Capabilities

- `ios-rts-load`: Defines the repeatable 500-model, 100-emitter physical-device workload.

### Modified Capabilities

- `ios-compute-presentation`: Reports explicit model and emitter counts.

## Impact

The iOS shipping sample, device evidence runner, build/run documentation, screenshot, and physical
device evidence are affected.

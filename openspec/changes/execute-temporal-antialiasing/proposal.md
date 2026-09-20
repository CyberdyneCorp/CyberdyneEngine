# Execute temporal anti-aliasing in the visible frame

## Why

M11.c enables jitter, velocity, a temporal graph pass, and a history declaration, but the visible renderer records no commands for that pass. The post process therefore reads the unfiltered scene color and the published image contains no temporal anti-aliasing.

## What changes

- Allocate and retain two full-resolution RGBA16F history images per assembled view.
- Import the previous and current histories into each temporal frame and swap them only after successful execution.
- Record a fullscreen temporal resolve that reprojects with motion vectors, rejects invalid history, clamps history to the current neighborhood, and writes the current history.
- Feed the resolved history into tone mapping and expose execution in the recorder report.
- Add structural and real-device regression tests, plus documentation for reset and fallback behavior.

## Scope

This change owns temporal anti-aliasing execution only. Material texture sampling, virtual geometry, VFX authoring, temporal upscaling, and M11.c roadmap closure remain outside it.

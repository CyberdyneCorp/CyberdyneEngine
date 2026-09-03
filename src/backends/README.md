# `src/backends/` — layer 3

Concrete implementations of the layer-2 server interfaces: Vulkan, Metal and D3D12 render backends,
Jolt physics, the audio device backend, and the null backends that keep handle bookkeeping valid
when a real one is unavailable.

**What belongs here**: everything that names a third-party API. A backend is the only place a
third-party type may appear, and it may not appear in a header consumed above layer 3.

**What does not belong here**: windowing and process code — those are `platform/`, which is the same
layer but a different porting surface.

**Governed by**: `engine-architecture`, `rhi-and-render-graph`, `physics`, `audio`,
`thirdparty-dependencies`.

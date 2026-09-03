# `src/servers/` — layer 2

Servers are singleton facades that own all of their state, address every object through opaque
generational handles, and have no knowledge of the ECS world, the scene graph, or scripting.

`RenderServer`, `PhysicsServer`, `AudioServer`, `NavigationServer`, `TextServer`, `DisplayServer`,
`InputServer`.

**What belongs here**: the server interfaces, their handle tables, their command queues, and the
factory registry that selects a backend.

**What does not belong here**: a concrete backend (that is `src/backends/` or `platform/`), and any
dereference of an entity or a node — a server that reaches into ECS storage is a layering defect.

**Governed by**: `engine-architecture` (server architecture), and the per-server capabilities:
`rhi-and-render-graph`, `physics`, `audio`, `navigation`, `text-and-fonts`,
`core-platform-abstraction`, `input-and-actions`.

# `src/ecs/` — layer 1

The authoritative runtime storage: entities are generational ids, component data lives in packed
per-archetype chunks, and behaviour runs as scheduled systems over queries.

**What belongs here**: the world, archetypes and chunks, component storage, queries, the system
scheduler and its stages, deferred structural change.

**What does not belong here**: the `Node` façade (that is `src/scene/`), any server, any knowledge
of scripting. Not every entity has a node, and the ECS must not assume one does.

**Governed by**: `ecs-core`. Arrives at M2.

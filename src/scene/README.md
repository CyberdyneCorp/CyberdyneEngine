# `src/scene/` — layer 4

The authoring and scripting façade. A `Node` is a named, hierarchical handle onto an entity, giving
designers and script code an object-oriented view of the same world the ECS stores.

**What belongs here**: the node tree, transforms and their propagation, prefabs and instancing,
built-in component and node types.

**What does not belong here**: duplicated state. `node.transform` reads the entity's `Transform`
component; a node that caches component data has broken the coherence rule.

**Governed by**: `scene-graph-and-nodes`, `serialization-and-prefabs`. Arrives at M2.

# scene-graph-and-nodes Specification

## Purpose

Defines the scene-graph façade: the authoring and scripting view layered over the ECS world. A
`Node` is a named, hierarchical handle onto an entity. Designers and Swift developers work with
nodes; the runtime works with entities and components.

This layer exists because the object-oriented tree is the right interface for authoring and
scripting (Godot, Unity, Unreal all converge on it), while packed component storage is the right
representation for execution. The specification's job is keeping the two provably coherent.

## Requirements

### Requirement: Node is a view onto an entity
A `Node` SHALL be a lightweight handle carrying an `Entity` plus tree bookkeeping (name, parent,
child order). It SHALL NOT duplicate component data.

Reading or writing a node property SHALL read or write the underlying component.

Every node SHALL map to exactly one entity. An entity MAY exist without a node.

#### Scenario: Property access goes to components
- **WHEN** script reads `node.transform`
- **THEN** the value SHALL be read from the entity's `LocalTransform` component

#### Scenario: Node-less entities are first class
- **WHEN** a system spawns bulk entities with no nodes
- **THEN** they SHALL be fully simulated and rendered, and SHALL NOT appear in the scene tree

#### Scenario: Node destroyed with entity
- **WHEN** an entity with a node is destroyed by a system
- **THEN** the node SHALL be detached and invalidated at the flush point, before scripting
  callbacks observe the frame

### Requirement: Hierarchy and naming
Nodes SHALL form a tree with ordered children. A node's name SHALL be unique among its siblings;
a collision SHALL be resolved by suffixing.

Nodes SHALL be addressable by path (`/Root/Level/Player`), relative path, and by a project-unique
alias for stable references from script.

The tree SHALL be backed by the ECS `Parent`/`Children` relation, so hierarchy is visible to
systems.

#### Scenario: Path lookup
- **WHEN** script resolves `"../Camera"` from a node
- **THEN** the sibling named `Camera` SHALL be returned, or a null handle if absent

#### Scenario: Reparenting preserves world transform
- **WHEN** a node is reparented with `keep_world_transform`
- **THEN** its local transform SHALL be recomputed so its world transform is unchanged

### Requirement: Transform model
Transforms SHALL be split into two components:

- `LocalTransform` — authored TRS relative to the parent
- `WorldTransform` — derived, computed by the transform propagation system

Propagation SHALL run as a system in `PostSimulation` and again before rendering, processing the
hierarchy in depth order and skipping subtrees whose `LocalTransform` version has not changed.

A root node's `LocalTransform` SHALL equal its `WorldTransform`.

#### Scenario: Dirty subtree only
- **WHEN** one node in a large hierarchy moves
- **THEN** only that node's subtree SHALL be recomputed

#### Scenario: Writing world transform
- **WHEN** script assigns a world transform directly
- **THEN** the corresponding `LocalTransform` SHALL be derived from the parent's inverse and
  written, keeping the local value authoritative

#### Scenario: Interpolated rendering
- **WHEN** a node is marked interpolatable and rendering occurs between simulation ticks
- **THEN** the renderer SHALL use the interpolation of the previous and current `WorldTransform`,
  and a teleport flag SHALL suppress interpolation for that frame

### Requirement: Node types and components
A node "type" SHALL be defined by a **component archetype template** plus an optional behaviour,
not by C++ inheritance. Creating a `MeshRenderer` node means creating an entity with
`LocalTransform`, `WorldTransform`, `MeshRenderer`, and `Visibility`.

The engine SHALL ship node templates for: spatial grouping, mesh rendering, skinned mesh,
instanced mesh, camera, lights (directional, point, spot, area), reflection probe, decal,
particle emitter, audio listener, audio emitter, rigid body, static body, character controller,
collider shapes, navigation region and agent, UI root and UI elements, and 2D sprite and tilemap.

Users SHALL be able to define their own templates without engine changes.

#### Scenario: Composition over inheritance
- **WHEN** a designer wants a light that also emits sound
- **THEN** they SHALL add both components to one entity, with no new class

#### Scenario: Template is data
- **WHEN** a project defines a custom node template
- **THEN** it SHALL be a data declaration listing components and defaults, registered at startup

### Requirement: Visibility and enablement
Nodes SHALL support two orthogonal flags:

- **Visible** — affects rendering only, and is inherited down the tree
- **Enabled** — affects simulation participation (systems skip disabled entities), also inherited

Effective state SHALL be computed by a propagation system alongside transforms.

#### Scenario: Hidden parent hides children
- **WHEN** a parent is set invisible
- **THEN** its entire subtree SHALL stop rendering while continuing to simulate if still enabled

#### Scenario: Disabled subtree stops simulating
- **WHEN** a subtree is disabled
- **THEN** queries used by gameplay systems SHALL exclude it, while its data remains intact for
  re-enabling

### Requirement: Node lifecycle callbacks
Nodes with an attached behaviour SHALL receive lifecycle callbacks in a defined order:

| Callback | When |
|---|---|
| `onCreate` | Entity and components exist; parent may not be set |
| `onEnterTree` | Node has been attached to the active tree; parent-first |
| `onReady` | After all children are ready; child-first; once per attachment unless re-requested |
| `onEnable` / `onDisable` | Effective enabled state changed |
| `onFixedUpdate(dt)` | Each simulation tick, if enabled |
| `onUpdate(dt)` | Each frame, if enabled |
| `onExitTree` | Detached from the tree; child-first |
| `onDestroy` | Before components are released |

#### Scenario: Ready runs bottom-up once
- **WHEN** a subtree is added to the tree
- **THEN** every descendant SHALL receive `onReady` before its ancestors, exactly once

#### Scenario: Callbacks are opt-in
- **WHEN** a behaviour does not implement `onUpdate`
- **THEN** it SHALL NOT be added to the per-frame dispatch list, so unimplemented callbacks cost
  nothing

### Requirement: Behaviours bridge nodes and systems
A **behaviour** SHALL be a script-side object (Swift, or native) attached to a node, receiving
lifecycle callbacks and holding script state.

Behaviour dispatch SHALL be driven by a system that iterates entities with a `BehaviourRef`
component, so script execution is scheduled like any other system and participates in stage
ordering.

Behaviours SHALL be documented as the ergonomic path, and systems as the performant path;
per-entity behaviours are not intended for very high entity counts.

#### Scenario: Behaviour dispatch is a system
- **WHEN** the `Simulation` stage runs
- **THEN** the behaviour dispatch system SHALL invoke `onFixedUpdate` on entities with behaviours,
  ordered and scheduled like other systems

#### Scenario: Guidance on scale
- **WHEN** a project needs per-entity logic on 100 000 entities
- **THEN** the documented guidance SHALL be to write a system over a query rather than 100 000
  behaviours

### Requirement: Groups and tags
Nodes SHALL be assignable to named **groups**, and the engine SHALL provide efficient iteration
over a group and broadcast invocation across it.

Groups SHALL be implemented as tag components so group membership is queryable from systems.

#### Scenario: Broadcast to a group
- **WHEN** script calls a method on the group `"enemies"`
- **THEN** every node in that group SHALL receive the call in a deterministic order

### Requirement: Scenes and worlds
A **scene** SHALL be a serialized tree of nodes and their components, loadable into a world
additively or as a replacement.

Multiple scenes MAY be loaded into one world simultaneously, each tracked so it can be unloaded
independently, taking its entities with it.

#### Scenario: Additive load
- **WHEN** a second scene is loaded additively
- **THEN** its nodes SHALL be added under a scene root, and unloading it SHALL destroy exactly
  those entities

#### Scenario: Async scene load
- **WHEN** a scene is loaded asynchronously
- **THEN** entity creation SHALL be spread across frames under a budget, and the scene SHALL
  become active only once fully instantiated

### Requirement: Coherence invariants
The engine SHALL maintain these invariants, checked by assertions in development builds:

1. Every node's entity is alive.
2. Every entity with a node has exactly one node.
3. A node's ECS `Parent` matches its tree parent.
4. `WorldTransform` is consistent with `LocalTransform` and the parent chain after propagation.
5. Effective visibility and enablement are consistent with ancestors after propagation.

#### Scenario: Invariant violation is caught early
- **WHEN** a system mutates `Parent` directly without going through the node API
- **THEN** the coherence check SHALL fail in development builds, identifying the entity

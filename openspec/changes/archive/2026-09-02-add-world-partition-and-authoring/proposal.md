# World partition, streaming, and the authoring model

## Why

World partition is the most-referenced missing capability in this specification set. Four
subsystems already specify seams into it and then work around its absence:

- `networking-and-replication` owns its own replication cells, with the contract written so a
  future partition can supply them instead
- `virtual-geometry` distinguishes content streaming from geometry streaming and states that the
  first has no owner
- `rendering-global-illumination` specifies the GI scene as cell-scoped "against a capability that
  does not yet exist"
- renderer profiles at open-world scale assume content arrives and leaves, with nothing saying how

Every one of those is a placeholder for the same missing thing. Specifying it removes four
workarounds rather than adding one system.

The authoring half is the other side of the same coin. `serialization-and-prefabs` already covers
prefabs, nesting, variants, overrides, migration, and cooking to archetype blobs — but it stops at
the scene. It has no concept of a **world** composed from reusable scenes, no notion that a prefab
might expose a deliberate parameter surface rather than inviting arbitrary overrides, and no answer
for what a prefab instance means once the cooker has flattened it into a streaming cell.

The contract, in two halves:

> **CyberWorld presents designers with a continuous world composed from reusable prefabs and
> scenes, while the cooker resolves those authoring structures into stable-identity ECS entity
> data, partitions them into independently streamable runtime cells, extracts subsystem payloads,
> and stores them through the asset system. Runtime streaming is predictive, asynchronous, and
> budget-driven. Spatial residency, gameplay activation, asset residency, and simulation detail are
> four distinct concepts.**

> **Prefabs are an authoring and compilation system, not a runtime object hierarchy. They compile
> into immutable ECS-native entity templates that are batch-instantiated or flattened into world
> cells. Provenance exists for editing and live development; shipping hot paths carry none of it.**

## What changes

**New capability `world-partition-and-streaming`** — the persistent world layer: the world asset as
metadata rather than content, cell-relative world coordinates, a pluggable partitioner with a
hierarchical grid as the default, stable cell identity, per-entity streaming policy, runtime cells
carrying subsystem payloads, the residency-versus-activation state machine, streaming sources with
shapes and prediction, per-subsystem streaming channels, transactional-but-incremental activation,
persistent entity identity and cross-cell references, dependency explosion detection, world layers
and scenario switching, world HLOD, the persistence overlay, streaming budgets, server and client
profiles, macro representation tiers, and the diagnostics that answer *why is this cell loaded*.

**`serialization-and-prefabs` is extended** into the full authoring model: prefab, scene, and world
as distinct asset kinds; scene instances with embedded and packed cook modes; **exposed prefab
parameters** as a prefab's deliberate public interface; override addressing by stable identifiers
rather than paths; hierarchy flattening at cook time; entity templates and batch spawning; cycle
rejection; field classification and the live prefab update policy; and authoring file granularity —
one file per authoring unit, explicitly not one file per entity.

One earlier decision is **corrected**: `serialization-and-prefabs` currently says a stale override
is dropped on load with a development-build warning. That silently discards designer work in a
shipping build and reports nothing in the editor where it could be fixed. It becomes an explicit
**override conflict** state with declared resolutions.

## Impact

- **New**: `world-partition-and-streaming`
- **Modified**: `serialization-and-prefabs` (substantially), `core-math`,
  `networking-and-replication`, `navigation`, `virtual-geometry`,
  `rendering-global-illumination`, `ecs-core`, `editor-architecture`, `thirdparty-dependencies`
- **Four workarounds removed**: replication cells, content streaming ownership, GI scene scoping,
  and open-world profile assumptions now have a real system to point at
- **Not in scope**: virtual textures and virtual shadow maps, which remain the two open gaps

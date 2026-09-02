# foliage Specification

## Purpose

Defines **CyberFoliage**: vegetation as a GPU-instance ecosystem, with entities as the exception
rather than the rule.

A million trees cannot be a million entities. But "vegetation is never interactive" is not an
acceptable architecture either, so the bridge is **promotion**: an instance becomes an ECS entity
when gameplay touches it and demotes back when it stops mattering. Two properties make that safe
and both are requirements — identity survives the round trip, so a felled tree does not return
standing; and demotion is refused when state cannot be represented compactly, because losing that
state is worse than the memory it costs.

Placement is **deterministic from rules, a seed, and a region**, so a forest is regenerated rather
than serialised, and only exceptions are stored. The failure mode that follows is specified rather
than discovered: changing a rule graph regenerates the region, and an exception that can no longer
be resolved becomes an orphan that is reported, not silently dropped — the same decision as the
prefab override conflict, for the same reason.

Ground cover goes further still: blades are expanded on the GPU from patch descriptions, so tens of
millions of them cost a few kilobytes of stored data.

## Requirements

### Requirement: Foliage instances are not entities
Foliage SHALL be represented as **compact GPU instances** in spatial clusters, not as ECS entities.

An instance SHALL be a small record — position, rotation, scale, variation, and flags — sized so
that millions can exist in memory and be culled and drawn entirely on the GPU.

Foliage SHALL publish into the GPU scene as instances (see `rendering-architecture`), and SHALL be
culled, shaded, and shadowed by the same passes as other geometry.

Creating foliage SHALL NOT create entities, components, physics bodies, or nodes.

#### Scenario: A million trees
- **WHEN** a world contains a million trees
- **THEN** they SHALL exist as GPU instances with no entities, and their per-instance memory SHALL
  be tens of bytes rather than hundreds

#### Scenario: Culling is hierarchical
- **WHEN** foliage is culled
- **THEN** clusters SHALL be culled before instances, so most instances are rejected in bulk

### Requirement: Foliage clusters
Instances SHALL be stored in **clusters** carrying bounds, per-species instance blocks, detail
metadata, wind metadata, and streaming information.

Clusters SHALL stream with world cells as a cell channel, and SHALL be independently evictable.

Cluster size SHALL be a cooker policy balancing culling granularity against per-cluster overhead,
and SHALL be reported rather than fixed as a constant in the specification.

#### Scenario: Streaming is per cluster
- **WHEN** a region streams in
- **THEN** its foliage clusters SHALL be loaded and published, and evicted when the region unloads

### Requirement: Promotion to entities
A foliage instance SHALL be **promotable** to an ECS entity when gameplay requires interaction —
damage, felling, physics, attachment, or scripted behaviour — and **demotable** back to an instance
when it no longer does.

Promotion SHALL preserve **identity**: a promoted instance SHALL carry a persistent identity
derived from its cluster and index, so that damage, removal, and state survive the round trip and a
felled tree does not return.

Demotion SHALL be **state-preserving or refused**. An instance whose state cannot be represented in
the compact instance form — mid-fall, partially destructed, physically constrained, or carrying
gameplay state — SHALL remain an entity rather than losing that state.

Promotion and demotion SHALL be bounded per frame by a budget, and the number of promoted instances
SHALL be reportable.

#### Scenario: Chopping a tree
- **WHEN** a player damages a tree
- **THEN** that instance SHALL be promoted to an entity with physics, and the GPU instance SHALL be
  suppressed so it is not drawn twice

#### Scenario: A felled tree stays felled
- **WHEN** a felled tree's entity is demoted after the player leaves
- **THEN** its removal or its fallen state SHALL be recorded, and it SHALL NOT reappear standing

#### Scenario: Demotion is refused when state would be lost
- **WHEN** an instance is mid-fall
- **THEN** it SHALL remain an entity rather than being demoted to a static instance

### Requirement: Procedural placement
Foliage SHALL be placeable by **rules** evaluated against environment fields and terrain: biome,
slope, altitude, moisture, temperature, soil, sun exposure, water distance, noise, roads, and
exclusion zones.

Rules SHALL produce species selection, density, scale, orientation, variation, and age.

Rule evaluation SHALL be **deterministic**, derived from the world seed, a region identifier, and
the rule graph version, so that a region regenerates identically rather than being serialised
instance by instance.

Manual painting SHALL compose with rules: an author MAY place, move, or remove instances, and MAY
suppress rules within a region.

#### Scenario: A forest is generated, not placed
- **WHEN** a forest covers ten square kilometres
- **THEN** its instances SHALL be derived from rules and seed rather than stored individually

#### Scenario: Regeneration is identical
- **WHEN** a region is regenerated on another machine or in another session
- **THEN** it SHALL produce identical instances for the same seed and rule version

### Requirement: Exceptions are stored, instances are not
Only **exceptions** to procedural placement SHALL be stored: instances an author moved, added, or
deleted, and instances gameplay destroyed or modified.

Exceptions SHALL be recorded in the world persistence overlay where they are runtime changes, and
in authoring data where they are authored.

Exceptions SHALL be anchored by stable instance identity **and** spatially, so that after a rule
graph or seed change they can be re-resolved against the regenerated set.

An exception that cannot be re-resolved SHALL become an **orphaned exception**: retained, reported,
and resolvable, and SHALL NOT be silently discarded — regenerating a region must not quietly undo
deliberate work.

#### Scenario: Storage is proportional to change
- **WHEN** a player fells fifty trees in a forest of a hundred thousand
- **THEN** the save SHALL record fifty exceptions, not a hundred thousand instances

#### Scenario: Rule change orphans an exception
- **WHEN** a rule change removes the tree an exception referred to
- **THEN** the exception SHALL be reported as orphaned rather than dropped, and SHALL be resolvable
  by an author

### Requirement: Grass is generated on the GPU
Dense ground cover SHALL be generated on the GPU from **patch descriptions** — density, species,
height, orientation, and seed — rather than stored as individual instances.

Blades SHALL be expanded only where visible and within a configured distance, and the expansion
SHALL be bounded by a budget.

Ground cover SHALL respond to the wind field and the interaction field like other foliage.

Persistent storage for ground cover SHALL be proportional to patch descriptions, not to blade
count.

#### Scenario: Tens of millions of blades, tiny storage
- **WHEN** a meadow is rendered
- **THEN** blades SHALL be expanded on the GPU from patch descriptions, and the stored data SHALL
  describe patches rather than blades

#### Scenario: Expansion is budgeted
- **WHEN** the visible grass exceeds the budget
- **THEN** density or distance SHALL be reduced through the renderer budget allocation rather than
  the frame overrunning

### Requirement: Foliage geometry classes
Foliage assets SHALL declare their **surface class** (see `virtual-geometry`) so that the geometry
system applies appropriate simplification, culling, and rasterisation policy: foliage simplifies
poorly, occludes poorly, and is dominated by alpha-tested thin surfaces.

Detail SHALL progress from detailed geometry near the camera, through simplified virtual geometry,
to **aggregate representations** in which many plants become one object, to a canopy or forest
macro representation at long range.

Billboard impostors MAY be used as a fallback tier but SHALL NOT be the only distant
representation.

#### Scenario: A forest becomes one object
- **WHEN** a forest is viewed from far away
- **THEN** it SHALL render as an aggregate representation rather than as thousands of simplified
  trees

#### Scenario: Alpha-tested detail is respected
- **WHEN** foliage is simplified
- **THEN** the geometry system SHALL apply foliage policy rather than treating it as solid
  geometry

### Requirement: Wind response
Foliage SHALL deform in response to the **wind field** (see `environment-fields`), with response
detail scaled by distance and importance.

Near foliage SHALL support **hierarchical response** — trunk, branch, twig, and leaf motion at
different amplitudes and frequencies — and distant foliage SHALL receive a simple sway.

Wind response SHALL be evaluated on the GPU as part of geometry processing, and SHALL NOT require
per-instance CPU work.

Wind response SHALL be consistent with other consumers of the wind field, so trees, particles, and
water agree.

#### Scenario: A gust crosses a forest
- **WHEN** a gust moves across a treeline
- **THEN** the response SHALL travel with it, because all instances sample the same field

#### Scenario: Distant foliage costs less
- **WHEN** foliage is far away
- **THEN** it SHALL receive simplified wind response rather than full hierarchical deformation

### Requirement: Interaction field
Foliage SHALL respond to moving objects through a **GPU interaction field**: characters, vehicles,
and projectiles register interaction primitives that bend, flatten, or displace foliage nearby.

Interaction SHALL NOT require physics bodies per plant.

The field SHALL have a bounded extent around streaming sources and a bounded number of
contributors, with the lowest-priority contributors dropped deterministically.

Persistent flattening — a trail through grass — SHALL be supported as a decaying contribution with
a declared lifetime.

#### Scenario: Grass parts around a vehicle
- **WHEN** a vehicle drives through grass
- **THEN** the grass SHALL bend around it through the interaction field, with no per-blade physics

#### Scenario: A trail fades
- **WHEN** a character walks through tall grass
- **THEN** a flattened trail SHALL persist and decay over its declared lifetime

### Requirement: Regional environmental state
Foliage SHALL carry **regional state** derived from environment fields — normal, wet, dry, burning,
burned, snow-covered — modulating appearance and behaviour, with per-instance exceptions where
gameplay has affected an individual plant.

Regional state SHALL be read from fields rather than stored per instance, so that a burned forest
costs a field region rather than a million instance updates.

State changes SHALL be visible to materials, VFX, audio, and gameplay through the same fields.

#### Scenario: A forest burns
- **WHEN** fire spreads through a region
- **THEN** the burn-state field SHALL change and the foliage in that region SHALL render burned,
  without per-instance writes

#### Scenario: One tree is different
- **WHEN** a single plant is destroyed within an unburned region
- **THEN** it SHALL be recorded as an instance exception rather than changing the region's state

### Requirement: Foliage budget
Foliage SHALL hold an allocation from the renderer budget arbiter (see `rendering-architecture`)
and distribute it across: instance counts by species class, ground cover density and distance,
detail tier thresholds, wind response detail, and interaction field resolution.

Reduction SHALL proceed from least to most important, and species MAY declare a gameplay importance
that protects them — cover that matters tactically SHALL NOT vanish because the frame is busy.

Promotion count SHALL be budgeted separately, since promoted instances cost simulation rather than
rendering.

#### Scenario: Dense scene reduces ground cover first
- **WHEN** the foliage allocation is exceeded
- **THEN** ground cover distance and density SHALL be reduced before trees are removed

#### Scenario: Gameplay-relevant cover persists
- **WHEN** a species is marked gameplay-relevant
- **THEN** it SHALL not be culled by budget pressure while decorative species remain

### Requirement: Foliage authoring
The editor SHALL support: painting and erasing instances, defining species and their assets and
parameters, authoring placement rules against fields and terrain, previewing generated results
before committing, and defining exclusion zones.

Rule authoring SHALL show its inputs — the fields and terrain values driving a decision — so that
an unexpected result is diagnosable rather than mysterious.

#### Scenario: Why is nothing growing here
- **WHEN** a region generates no foliage
- **THEN** the editor SHALL show which rule input excluded it

#### Scenario: Preview before commit
- **WHEN** a rule is edited
- **THEN** the affected region SHALL regenerate for preview without cooking the world

### Requirement: Foliage diagnostics
The engine SHALL report: instance counts by species and cluster, memory in use, cluster residency,
detail tier distribution, promoted instance count and their causes, exception counts and orphaned
exceptions, ground cover expansion counts against budget, and interaction field contributors.

#### Scenario: Promotion is attributable
- **WHEN** promoted instance count is high
- **THEN** the diagnostics SHALL report what promoted them

## MODIFIED Requirements

### Requirement: Scene hierarchy and asset browser
The **hierarchy** panel SHALL show the edited scene's node tree with: search and filtering,
drag-and-drop reparenting, multi-selection, visibility and lock toggles, prefab instance and
override indication, and creation from templates.

For a **world**, the hierarchy SHALL be a virtualised outliner over a **world metadata index** —
persistent identity, name, type, bounds, layer, cell, prefab provenance, and asset references —
rather than a materialised tree of every entity. It SHALL support queries by name, component,
layer, cell, prefab, tag, and asset, and SHALL NOT require the world to be loaded to search it.

Selecting an unloaded entity SHALL load its **authoring record** for inspection, and SHALL NOT
require activating its region.

The editor SHALL provide a **streaming debugger** view (see `world-partition-and-streaming`)
showing cell states, streaming sources, priorities, costs, layers, and why a given cell is in its
current state.

The **asset browser** SHALL show the project's assets with: folder navigation, search and type
filtering, thumbnails generated asynchronously, drag-and-drop into scene and inspector, rename
and move with reference preservation, and import settings access.

Prefab and scene assets SHALL support a **structural diff** view showing added, removed, and
changed entities, components, and fields, and the inspector SHALL show each value's provenance —
base, variant, or instance override — with unresolved override conflicts surfaced rather than
hidden.

#### Scenario: Asset moved
- **WHEN** an asset is moved in the browser
- **THEN** its `.meta` SHALL move with it and all references SHALL continue to resolve by
  `AssetId`

#### Scenario: Thumbnail generation
- **WHEN** a folder of models is displayed
- **THEN** thumbnails SHALL be rendered asynchronously without blocking the UI, and cached

#### Scenario: Millions of entities are searchable
- **WHEN** a designer searches a world containing millions of entities
- **THEN** the query SHALL run against the metadata index and results SHALL be virtualised, without
  loading entity component data

#### Scenario: Inspecting an unloaded entity
- **WHEN** a designer selects an entity in an unloaded region
- **THEN** its authoring record SHALL be loaded for inspection without streaming its region

#### Scenario: Overrides are legible
- **WHEN** a prefab instance is inspected
- **THEN** each value SHALL show whether it is inherited or overridden, and any override conflict
  SHALL be surfaced with its resolutions

## MODIFIED Requirements

### Requirement: Model import
The model importer SHALL support **glTF 2.0** (`.gltf`, `.glb`) as the primary interchange
format and **FBX** via ufbx, producing meshes, materials, textures, skeletons, animations, and a
scene hierarchy as a prefab. **USD** SHALL be supported as an optional, tool-time-only importer.

Import SHALL perform, in a defined order:
1. Parse and convert to engine coordinate conventions (handedness, up axis, unit scale)
2. Build meshes: index and vertex buffers, split by material, weld vertices within a tolerance
3. Generate missing data: normals (with a smoothing angle), tangents, and UV2 for lightmapping
4. Optimise: vertex cache ordering, overdraw reduction, vertex fetch optimisation
5. Generate LOD chain to configured reduction targets
6. Generate collision: none, convex hull, convex decomposition, or triangle mesh, per options and
   node naming conventions
7. Import skeletons, derive bone LOD levels, and remap to a `SkeletonProfile` if configured
8. Import animations with error-bounded compression settings, optionally splitting into clips by
   time ranges, and optionally retargeting through a retarget profile
9. Import materials, mapping source parameters to the standard material
10. Produce a prefab representing the hierarchy

Node-level options SHALL be editable per node in an import settings dialog and stored in the
`.meta`, so an artist's naming convention or a designer's per-node choice both work.

#### Scenario: Coordinate conversion
- **WHEN** a Z-up model is imported
- **THEN** it SHALL be converted at import so no runtime code accounts for source handedness

#### Scenario: Collision from a naming convention
- **WHEN** a node is named with the configured collision suffix
- **THEN** a collider SHALL be generated from it and the node excluded from rendering

#### Scenario: Material extraction
- **WHEN** materials are set to be extracted
- **THEN** they SHALL be written as separate editable assets, and re-import SHALL preserve edits
  rather than overwriting them

#### Scenario: Animation-only re-import
- **WHEN** a source file is re-imported with meshes and materials disabled
- **THEN** only animations SHALL be produced, which is the fast path for animation iteration

#### Scenario: USD is tool-time only
- **WHEN** USD import is enabled
- **THEN** it SHALL be available in the editor and cooker only, and no USD code SHALL be linked
  into a shipped runtime

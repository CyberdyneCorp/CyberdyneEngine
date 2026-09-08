## MODIFIED Requirements

### Requirement: Model import
The model importer SHALL support **glTF 2.0** (`.gltf`, `.glb`) as the primary interchange
format and **FBX** via ufbx, producing meshes, materials, textures, skeletons, animations, and a
scene hierarchy as a prefab. **USD** SHALL be supported as an optional, tool-time-only importer.

**OBJ** (`.obj`, with its companion `.mtl`) SHALL be supported. It carries no rig, no animation and
no scene graph, so it exercises only steps 1 to 6 and 9 of the sequence below and SHALL report the
steps it did not reach rather than appearing to have performed them. It is supported because it is
the format a mesh arrives in when it came from anywhere at all — a sculpt, a scan, a generator, a
thirty-year-old archive — and an engine that cannot open one cannot be handed a model by a stranger.

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

A format that cannot express a step SHALL skip it and say so in the import report. A step skipped
for that reason is not a warning about the file and SHALL NOT be reported as one.

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

#### Scenario: An OBJ produces a mesh
- **WHEN** an `.obj` is imported
- **THEN** it SHALL produce meshes, its `.mtl` materials, generated normals and tangents, an LOD
  chain and collision, through the same steps and the same derivation key as every other format

#### Scenario: A format's absent steps are reported, not warned about
- **WHEN** a format carries no skeleton and no animation
- **THEN** the import report SHALL name the steps it did not reach, and SHALL NOT emit a warning
  implying the file is deficient

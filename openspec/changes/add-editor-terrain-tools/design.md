# Design: terrain tools (#29, Terrain)

## Context

The editor is a client of the engine. The scaffolded terrain panel already records every completed
gesture as one modifier child of the terrain root, inside one document transaction
(`terrain.stroke.commit`). The engine's `cy::terrain` already has a non-destructive modifier stack,
meshing with holes, collision with hole samples, and `TerrainNavigation::mark_dirty`. Until this
change nothing joined the two.

## Decisions

### The stack travels, not the delta

After every change to the edited terrain's stack, the editor sends the whole ordered stack as
`terrain.evaluate`. That covers a stroke, an undo, a redo, an enable and a reorder. Undo therefore
needs no engine-side inverse. The document's history restores the stack, the stack evaluates to the
same bytes, and each side can check its half separately. The Rust side shows that undo restores the
request byte for byte. The C++ side shows that the same request gives the same heights, weights and
holes. Sending the whole stack costs a few hundred bytes per stroke. `TerrainEngine` keeps one
request in flight, so a burst of edits costs one evaluation per round trip.

### A brush is a modifier kind, and its footprint is arithmetic

`ModifierKind::Brush` carries its dabs in a pool, as sculpt stamps and spline points already do, so
reordering stays a permutation. The weight is a smoothstep from the hard core to the radius, times
the dab's pressure. It is zero at and beyond the radius, so a brush writes nothing outside the union
of its discs. The tests measure that against the stroke's own geometry rather than by calling
`brush_weight`.

- Smooth uses Jacobi passes with two rolling rows. The erosion pass's in-place sweep has a
  dependency chain that runs along the scan, so Jacobi is what makes the declared halo meaningful.
- The flatten target is read by the service from the stack beneath the flatten, at the stroke's
  first dab. That is the usual "flatten to where you started" behaviour, it stays a pure function of
  the stack, and it moves if something beneath changes.
- Paint blends into the bounded texel through `paint_texel`, which reuses the compositor's merge,
  sort and renormalise.

### The engine's own builders, once

`evaluate_region` flattens the tiles, stitches them, and runs `mesh_tile` and `build_collision` on
each. So the hole counts the editor shows, the triangles the viewport draws and the collision
samples physics would take all come from one evaluation. The hosted runtime draws the joined mesh at
every node carrying `TerrainAuthoring`.

### Navigation is marked, not rebuilt

The service keeps, per session, the identity, content digest and reach of every modifier it last
evaluated for a terrain. The digest covers the modifier's position and its resolved flatten target.
Reaches that appear, disappear or change are clipped to the region and added to a
`TerrainNavigation` dirty list, which is capped at 64 regions and past that becomes their union.
They are reported with every reply. The first evaluation of a terrain marks nothing, because
nothing was baked against it. Clearing the list is #28's job.

### Agents get a stroke they can write

`terrain.stroke.commit` takes the surface's binary stroke payload, which MCP carries as raw text
bytes. `terrain.brush.apply` takes points and brush settings as numbers, builds the same `Stroke`,
and goes through the same commit function. `terrain.status` is a read, and is listed in the panel's
`COMMANDS` so the scaffold's parity check covers it.

### The layer bug

`keep_selected_layer` keeps a layer selected whenever the stack has one, and the drag handler
attached it to every stroke. `validate_layer` correctly refuses a layer on a sculpt, so after the
first layer existed every sculpt gesture was refused. The panel now names a layer only on paint.
`a_sculpt_stroke_with_a_layer_in_the_stack_names_no_layer_and_is_accepted` drags across the field
and invokes what the panel produced.

## Not built

- Painted layers do not show in the viewport: the terrain surface material is not bound on a device
  (`src/terrain/README.md`, "No shader"). The panel shows them.
- There is no eraser stroke that fills a hole. Undo removes one.
- The region is fixed at 128 m by two engine tiles, and is not stored on the terrain root.
- Stale navigation lives in the engine session. It is lost if the runtime restarts, which is
  harmless while nothing rebakes (#28).

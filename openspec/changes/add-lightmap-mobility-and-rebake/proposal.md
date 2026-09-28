# Light mobility with a baked shadow mask, incremental rebakes, and a chart-padding check

## Why

`rendering-global-illumination` — "Lightmap baking" — requires a **shadow mask** that lets
stationary lights keep dynamic direct light with baked shadows, and a bake that is **incremental**:
moving one object in a large level re-solves the affected region rather than the whole level.
"UV2 and chart packing" requires padding sufficient for bilinear filtering and mip generation at the
bake resolution. The first slice of #36 (`add-lightmap-baking`) left all three exempt by name: there
is no light mobility in the tree, a changed level re-bakes whole, and nothing checks that an unwrap's
chart padding survives the rectangle the atlas gives the object — a rectangle at a lower density than
the unwrap's shrinks it.

## What changes

- **Light mobility.** `gi::GiLight` gains `mobility`: `Stationary` (the default), `Static` or
  `Movable`. The bake honours it per light:
  - `Static`: the light's direct term at the receiver is baked with its indirect, as
    `LightmapContent::DirectAndIndirect` did for every light.
  - `Stationary`: its indirect is baked, its direct stays dynamic, and the bake writes a
    **shadow-mask channel** for it — the fraction of the light's extent visible from the texel.
  - `Movable`: nothing of it is baked, not even its bounce; the dynamic caches own it.

  The default keeps every existing bake's texels: a light with no mobility set is `Stationary`,
  whose baked content is exactly today's `Indirect`. `LightmapContent::DirectAndIndirect` stays and
  now means "bake the direct term of every light that is not `Movable`".
- **The shadow mask.** `BakedLightmap::shadow_mask` is one RGBA plane in the atlas layout, one
  channel per stationary light (at most four per level), with the channel's light id alongside it. It
  is dilated with the texels, so a bilinear tap at a chart border reads the chart's own shadow. The
  cooked asset carries it (format version 2; version 1 still decodes, with no mask).
- **Incremental rebake.** `rebake_lightmaps` takes the previous bake, the moved instances and their
  previous bounds, and re-solves only the objects whose surface lies within an influence distance of
  the old or new placement. Every texel of every other object is copied from the previous bake, byte
  for byte. When the level no longer packs to the same rectangles it falls back to a full bake and
  says so.
- **Chart-padding check.** After rasterisation the bake measures, per object, the empty-texel gap
  between its distinct charts, and reports each object whose gap is below what bilinear filtering and
  the declared mip levels need. A setting refuses such a level instead.
- **The build producer.** A `cylightmap 1` light line takes an optional trailing
  `static | stationary | movable`.

## Scope

Host-side only: everything here runs in `integration.render_lightmap_bake` without a device. NOT in
this change, and still exempt by name: the frame's use of the shadow mask (the forward shader reading
the mask plane for a stationary light's direct term), the mask's upload, mip levels in the uploaded
atlas, and the editor's bake command and texel-density view. Seam reconciliation of the mask plane is
not done either (see design.md).

# Fix the device loss after `UiRenderer::upload_atlas`: no device frame of its own

## Why

`add-rts-combat-and-minimap` (#146) left a finding open: with the battle's minimap re-made through
`UiRenderer::upload_atlas` every sixth frame, `samples/13-rts-api --battle` on The Frontier lost the
Vulkan device at a fixed frame (NVRM Xid 109, a context-switch timeout) with the GI, ambient
occlusion and the sun's shadows all on, and no validation, synchronisation-validation or
AddressSanitizer finding before it. The battle was moved to `stage_atlas` and the cause was not
found.

`upload_atlas` copied each page in through a render graph of its own, inside a device frame of its
own: `begin_frame`, submit, `wait_idle`, `end_frame`, called between two of the game's frames. That
frame advanced the RHI's frame ring behind the game's back. The game's next frame then ran in the
same frame slot as its previous one, and every per-slot ring and retirement epoch in the engine moved
under it — the frame after such an upload is the one the device was lost in (design.md has the
bisection). It reproduces deterministically on this host: the frame after the eighteenth upload,
every run, with and without the validation layer.

## What changes

- **`upload_atlas` begins no device frame and submits nothing.** It makes the page's texture and
  view and stages the texels; the next frame's own graph copies them in, as `stage_atlas`'s are,
  importing a page no frame has copied into yet as undefined. The page's staging buffer is made per
  frame slot as `stage_atlas`'s is, and dropped when the page is re-made at another size. The page is
  usable from the next frame; calling it between frames or before the frame declares the interface's
  stage is the contract, which is where every caller calls it.
- **`samples/13-rts-api --minimap-upload`**: the battle re-makes its minimap page with
  `upload_atlas` every time it is composed, the path that lost the device — the control that now runs
  the whole battle. The battle keeps `stage_atlas`: it changes the page it has rather than making a
  texture and a view every sixth frame, and its stills are byte-identical either way.
- **`integration.ui_atlas_upload`**, on the null device on every CI leg: `upload_atlas` (and `create`,
  which makes page zero with it) begins no frame and submits nothing; the game's frames take the ring's
  slots in order however often a page is re-made between them; the copy arrives in the next frame's
  graph, again after a page is re-made at another size.
- The RTS guide's and #146's design's open finding is marked resolved with the cause; the UI
  renderer's README and the fog-of-war guide say where an uploaded page's texels go.

## Impact

`upload_atlas`'s signature and its result are unchanged; a page's texels now reach the device in the
next frame rather than before the call returns, which is when every caller first drew them. A frame
loop that uploads pages between frames no longer gains a device frame per upload, so frame slots,
per-slot rings and retirement epochs stay the game's. Other modules that run a one-shot device frame
of their own (material textures, the LTC table, grading's table, lightmaps, the decal table, the GI's
structure build) do so at creation, before the game's first frame; they are unchanged.

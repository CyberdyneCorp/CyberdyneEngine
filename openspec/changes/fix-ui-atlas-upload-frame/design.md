# Design: `upload_atlas` without a device frame of its own

## The finding

`samples/13-rts-api --battle --render <dir> --map frontier.cypak`, with the minimap's page re-made by
`upload_atlas` at every composition (every sixth frame; `--minimap-upload` selects it), on the RTX
5060 (driver 580.95.05, Vulkan 1.4.312), Development build:

- the device is lost in the frame drawn right after the upload at frame 102 — the eighteenth — on
  every run, with or without the validation layer: `vkDeviceWaitIdle` fails after 5.3 s of waiting
  and the kernel logs `NVRM: Xid 109 … CTX SWITCH TIMEOUT` for the process; the frames before it
  take 8.5–9.9 ms of device time;
- no validation, synchronisation-validation or GPU-assisted-validation message precedes it (under
  GPU-assisted validation, as under AddressSanitizer, it does not occur at all: both change the
  timing and the shaders);
- device-side breadcrumbs, with a full barrier before each so the last one written is the last pass
  the device finished, put it in the forward ("opaque") pass: everything before it completed, the
  sky after it never started. One opaque draw is enough (any one; both halves of the draw list hang
  alone), a trivial fragment shader is not.

## What `upload_atlas` did

```
upload_atlas(page, …):  create texture → begin_frame → graph { copy, residency } → submit
                        → wait_idle → end_frame → destroy the old page
```

Called between two of the game's frames, that `begin_frame`/`end_frame` pair is a device frame the
game did not make. The RHI's frame ring — which frame slot a frame runs in, the slot's command and
descriptor pools, the per-slot rings every system keeps (`FrameBindings`, the UI's rows, particles,
the fog of war's staging), and the retirement epoch that frees what a frame released — advances
once per `begin_frame`, so after an upload the game's next frame runs in the SAME slot as its
previous frame, and its `begin_frame` retires two frames' worth of resources at once.

## The bisection

Each line is a full 1 200-frame battle run on the device with everything else as above; "lost" is
the device lost, "clean" is all frames drawn.

| run | result |
|---|---|
| `upload_atlas` every 6th frame (the finding) | lost after the upload at 102 |
| every 2nd / 4th / 12th frame | lost after the uploads at 406, 508, 612 |
| every 1st / 3rd / 5th / 7th frame | clean |
| `stage_atlas` (the battle since #146) | clean |
| an empty `begin_frame`/`end_frame` every 6th frame, nothing uploaded | clean |
| that empty frame plus a graph with one no-op pass, submitted and waited for | clean |
| a texture made, filled through a graph in its own frame and destroyed at once | clean |
| the same texture kept until the next one, no view | clean |
| the same texture kept, with a view, but never filled | clean |
| the same texture kept, filled, with a view (what `upload_atlas` does) to a page never drawn | lost after the upload at 1 086 |
| `upload_atlas`'s graph submitted WITHOUT its `begin_frame`/`end_frame` | clean |
| that, plus an empty `begin_frame`/`end_frame` before it | lost after the upload at 102 |
| retirements released 2, 5 or 20 frames late, or never | clean |
| retired image views, or retired transient images, never released | clean |
| the atlas page in dedicated memory; transient aliasing off; async compute off | lost after 102 |
| per-frame descriptor pools or command pools rotated independently of the slot | lost after the uploads at 114, 102 |

Every loss is in the frame drawn right after an upload. Two things together lose the device, and
neither alone does:

1. **a device frame the game did not make** — the period has to leave the game's frames out of step
   with the ring (an even period with a ring of two), and taking `begin_frame`/`end_frame` out of
   the upload, keeping its submission, is clean;
2. **the churn an upload makes**: a new image, filled, with a view, and the old one retired.

Instrumentation added for the investigation (not committed) found no engine-level lifetime breach:
no descriptor write named a stale view or buffer handle, no view whose texture was gone, no per-frame
descriptor set bound outside the frame it was allocated in or allocated outside a frame. Below the
API the mechanism is in the driver's reuse of what the doubled retirement frees — leaking any one
class of what it frees, or delaying it by two frames, is enough to keep the device — and is not
reachable further from here. What is reachable is the engine's half, and it is a defect on its own:
**a helper must not advance the frame ring.** The ring is the game's; one device frame per game frame
is what every per-slot ring in the engine is written against.

## The fix

`upload_atlas` keeps its contract — a page made, its texels the next thing the interface draws — and
loses its frame:

- it makes the texture and its view (`make_page`), retires the old page, and stages the texels exactly
  as `stage_atlas` does: kept on the host, copied into the frame slot's staging buffer when the next
  frame declares the interface's stage, and moved into the page by a transfer pass every interface
  pass of that frame reads after (`declare_copy`);
- a page no frame has copied into yet is imported `Undefined` (`AtlasPage::copied`), a page copied
  before `SampledRead`, so the graph derives the first transition as it did inside the old graph;
- the staging buffers are made per slot on first use, as before, and dropped when the page is re-made
  at another size (`StagedPage::staging_bytes`), so a larger page never copies out of a smaller buffer.

`create` makes page zero (the white texel) through `upload_atlas`, so a renderer now begins no device
frame from creation to its first drawn frame either.

The alternative — a one-shot submission outside the ring (its own command pool and fence) — would
keep the ring the game's but still stall the device on every upload, which #146 measured at 4 ms; and
it would keep a second way of moving texels that `stage_atlas` already is. Staging is both.

## Why the battle keeps `stage_atlas`

The minimap changes every sixth frame at the same size. `stage_atlas` writes the page it has;
`upload_atlas` makes a texture and a view each time and retires the old ones. Both now cost the frame
one copy and no stall, and the stills are byte-identical, so the battle keeps the cheaper path;
`--minimap-upload` keeps the other one exercised.

## Verification

- `integration.ui_atlas_upload` (null device, every leg): red on the old code — `create` and each
  `upload_atlas` began a frame and submitted, and frames took slots 1, 0, 1, 0 where 0, 1, 0, 1 is the
  ring (evidence/falsification.txt) — green on the fix.
- On the device: `--battle --render --map frontier.cypak --minimap-upload` draws all 1 200 frames with
  the validation layer on (no error) and off; its four stills are byte-identical to the
  `stage_atlas` run's. `render.ui`, `render.ui_toolkit`, `render.upscale`, `render.upscale_off`,
  `render.rts_api_hud`, `smoke.rts_api_map`, `smoke.rts_api_fog`, `smoke.rts_api_gi`,
  `integration.rts_api_sample` pass, one at a time.

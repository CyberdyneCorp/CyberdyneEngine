# `samples/13-rts-selection` — a strategy game's selection, drawn by the engine

```
build/<profile>/samples/13-rts-selection/cy_sample_rts_selection --out docs/design/images [--validate]
```

No CTest entry: the picture needs a graphics device, and on a machine without one the program says
which device it did not find and writes nothing. `render.selection_outlines` is the suite that holds
the stage to its requirement.

## What it runs

| | |
|---|---|
| **the field** | `pipeline_test::FrameScene` — the scene the pipeline suites render — with its ring arranged as a squad of four player units, a building, three enemy units and a neutral one far back. Each box's draws carry its ENTITY's identity (`FrameSceneHooks::stable_id`), as a real entity's draws do after the extract stage |
| **the input** | a drag box and a cursor position, in pixels. The game logic is what a strategy game's is: the player's units whose screen centre falls inside the box are selected; the nearest enemy whose screen rectangle holds the cursor is hovered |
| **the marks** | a `SelectionHighlight` component on each marked unit's entity — `selected` in the team's blue, `hovered` in a warm red — through `ecs::World::add`, the call a Swift module reaches as `world.highlight(unit, .selected(...))` |
| **the frame** | `gather_highlights` turns the components into the `HighlightSet` that `selection::OutlinePass` draws at the frame's `SelectionOutlines` stage, after the tone curve and before the interface |
| **the pictures** | the frame before the input and after it, the building and the unit behind it enlarged four times, and the frame with the HUD and the console (`rts-hud.png`, and `rts-hud-2x.png` at twice the size) |

| Before | After |
|---|---|
| ![](../../docs/design/images/rts-selection-before.png) | ![](../../docs/design/images/rts-selection-after.png) |

![The building and the unit behind it, four times](../../docs/design/images/rts-selection-detail.png)

The unit behind the building keeps its solid blue outline where it is on screen; beside the part
the building hides the outline is dimmed and dashed, and the hidden part itself is tinted, so the
player still sees where the unit is. The hovered enemy glows rather than being outlined.

## The HUD and the console

The third frame draws the game's interface over the selection: a resource bar, the minimap with the
units and the camera's rectangle (clipped where the camera looks past the map), a panel listing the
units the drag box caught, and the engine's developer console, where the player has typed
`selection` and is typing `spawn tank`. `hud.h` is game code over CyberUI's `ElementStore`; the
console is `ui::DevConsole`; both are laid out and flattened together and drawn by
`ui::render::UiRenderer` at the frame's interface stage, after the outlines. `render.ui` photographs
the same HUD against a golden image.

![The HUD and the console, twice the size](../../docs/design/images/rts-hud-2x.png)

## What it does not claim

The field is boxes: the scene is the pipeline suites' own, whose geometry is a cube. The camera is
the scene's, level with the ground rather than a strategy game's high angle. The marks are made
once, from a fixed input, rather than by a live cursor in a window.

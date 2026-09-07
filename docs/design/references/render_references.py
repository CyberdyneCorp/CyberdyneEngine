#!/usr/bin/env python3
"""Draw `docs/design/images/`'s three editor reference images — M6 task 10.5.

--- WHY THE REFERENCES ARE DRAWN RATHER THAN PAINTED ---------------------------------------------

`editor-visual-language` requires that *"a reference that no longer reflects the intended language
SHALL be replaced rather than left to decay"*, and M5.5's artefact compared the built editor against
the previous three images difference by difference and found three of them wrong in the same way
every time — all three faults were in the CHROME and none in the scene:

  1. **The publisher was branded as the product.** The headers read `CYBERDYNE · ARTIFICIAL
     INTELLIGENCE`, `CYBERDYNE EDITOR` and `CYBERDYNE ENGINE`. The product is CyberEngine and
     Cyberdyne is the publisher; `docs/design/editor-visual-language.md` says so under "The
     identity" and the built editor's header already does it correctly.
  2. **The console carried another engine's vocabulary** — a `Blueprint Log` tab, `BP_` prefixes,
     `2,341 actors`, `Static Mesh`. The vocabulary table forbids every one of them and
     `cy-editor-interface`'s own gate rejects them in a registered command's prose, so a reference
     image was the last place in the project still using them.
  3. **The header said nothing about the runtime.** Whether a runtime is attached is load-bearing
     from M5.5 onward — the editor is a separate process that survives the runtime's death — and it
     belongs where the eye already goes.

So the chrome is DRAWN, from this file, and the scene behind it is a plate. An image nobody can
regenerate is an image that decays, which is the argument M5.5's own two screenshots make; a
reference whose every label comes from a table in a script cannot drift into another engine's words
without somebody editing the table.

--- WHAT IS A PLATE AND WHAT IS NOT ---------------------------------------------------------------

`scene-*.jpg` beside this file are the viewport contents and nothing else: no interface, no text, no
mark. They were produced for this repository as concept art — the same standing as the paintings the
previous references were — and they are held at the viewport's own resolution because that is all
they are ever scaled into. They stand for what a renderer produces, which is exactly what a reference image is NOT
normative about — `editor-visual-language`'s own "What a reference image is — and is not" says the
imagery states hierarchy, density, colour, chrome and composition, and that none of the volumetric
clouds and instanced armies in it exist before M7–M10. Everything this file draws over them IS
normative, and every string it draws is in the tables below.

    python3 docs/design/references/render_references.py            # all three, into docs/design/images/
    python3 docs/design/references/render_references.py --only rts

Needs Pillow. Nothing else, and nothing from the engine: this is a document's own tool.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
IMAGES = HERE.parent / "images"

# The dark theme's surfaces and semantic roles, from `cy_editor_visual::colour`. Restated as numbers
# because this script may not depend on the editor's crates, and because a reference image that
# picked its own greys would be arguing with the surface system it is meant to establish.
INK = {
    "window": (0x0E, 0x10, 0x12),
    "panel": (0x16, 0x19, 0x1C),
    "raised": (0x1E, 0x22, 0x27),
    "line": (0x2A, 0x2F, 0x35),
    "primary": (0xE6, 0xE9, 0xEC),
    "secondary": (0xA2, 0xAB, 0xB4),
    "muted": (0x6E, 0x78, 0x82),
    "live": (0x35, 0xC0, 0x7C),
    "warning": (0xF0, 0x91, 0x3A),
    "selection": (0xE5, 0xB9, 0x5C),
    "active": (0x4C, 0x9A, 0xFF),
    "x": (0xE0, 0x5A, 0x5A),
    "y": (0x5A, 0xD0, 0x7A),
    "z": (0x5A, 0x9A, 0xE0),
}

WIDTH, HEIGHT = 1536, 1024
HEADER, TOOLBAR, FOOTER = 34, 28, 22
LEFT, RIGHT = 288, 300
GAP = 6

MENUS = ("Project", "File", "Edit", "Scene", "Play", "Source", "Transform", "Viewport", "View",
         "Help")
TOOLS = ("Move", "Rotate", "Scale", "Universal")
ACTIONS = ("Undo", "Redo", "Save")
# THE CONSOLE'S TABS, AND FAULT 2 IS THAT THIS LIST USED TO READ "Blueprint Log". These are the
# editor's own three, and they are the three the built editor draws.
CONSOLE_TABS = ("Console", "Profiler", "Problems")


@dataclass
class Scene:
    """One reference: its plate, its fiction, and every string drawn over it."""

    name: str
    plate: str
    project: str
    document: str
    runtime: str
    live: bool
    hierarchy: list[tuple[int, str]]
    content: list[str]
    inspector: list[tuple[str, list[tuple[str, str]]]]
    selection: str
    console: list[tuple[str, str]]
    status: str
    # Where the transform gizmo sits in the viewport, in fractions of it, and which handles.
    gizmo: tuple[float, float]
    gizmo_mode: str = "translate"
    # The selection outline, in fractions of the viewport.
    outline: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)
    minimap: bool = False
    rail: bool = False
    # A second dock under the viewport, which is where the graph lives when it is open.
    lower_dock: tuple[str, list[str]] | None = None
    view_state: str = "Perspective · Lit · translate · World · Pivot · Snap 0.25 m"
    performance: list[str] = field(default_factory=list)
    # "columns" is the shipped default: the hierarchy upper-left over the content browser, the
    # viewport centre, the inspector down the right. "bottom-docks" is the THIRD VARIANT the scene
    # view establishes — a full-width viewport with the hierarchy and the content browser docked
    # along the bottom — and it exists to show that the composition requirement fixes REGIONS
    # rather than pixel positions. `editor-visual-language.md` says which is the default and why.
    layout: str = "columns"
    # The rotation read-out the variant floats in the viewport, bottom right.
    read_out: list[tuple[str, str]] = field(default_factory=list)


def font(size: int, bold: bool = False, mono: bool = False):
    from PIL import ImageFont

    families = {
        (False, False): ("DejaVuSans.ttf", "LiberationSans-Regular.ttf"),
        (True, False): ("DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf"),
        (False, True): ("DejaVuSansMono.ttf", "LiberationMono-Regular.ttf"),
        (True, True): ("DejaVuSansMono-Bold.ttf", "LiberationMono-Bold.ttf"),
    }[(bold, mono)]
    roots = ("/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/truetype/liberation/",
             "/usr/share/fonts/TTF/")
    for root in roots:
        for family in families:
            candidate = Path(root) / family
            if candidate.is_file():
                return ImageFont.truetype(str(candidate), size)
    return ImageFont.load_default()


def panel(draw, box, fill="panel") -> None:
    draw.rectangle(box, fill=INK[fill])


def tabs(draw, x: int, y: int, names, active: int = 0) -> int:
    """A dock's tab strip. The active tab carries the selection hue and nothing glows."""
    for index, name in enumerate(names):
        colour = INK["primary"] if index == active else INK["muted"]
        draw.text((x, y), name, font=font(12), fill=colour)
        width = int(draw.textlength(name, font=font(12)))
        if index == active:
            draw.line([x, y + 17, x + width, y + 17], fill=INK["selection"], width=2)
        draw.text((x + width + 6, y + 1), "×", font=font(11), fill=INK["muted"])
        x += width + 26
    return x


# --- The regions ----------------------------------------------------------------------------------


def draw_header(draw, scene: Scene) -> None:
    """The mark, the menus, and — fault 3 — what document is open and whether a runtime is attached."""
    panel(draw, [0, 0, WIDTH, HEADER], "panel")

    # THE MARK IDENTIFIES AND DOES NOT DOMINATE. A small hexagonal glyph and the product's name, in
    # the interface's own flat charcoal: the logo's metallic gradients and blue emissive core belong
    # to the logo and not to the chrome, which is the one thing the identity section says does not
    # travel.
    cx, cy = 18, HEADER // 2
    draw.regular_polygon((cx, cy, 9), 6, rotation=90, outline=INK["active"], fill=INK["raised"])
    draw.regular_polygon((cx, cy, 4), 6, rotation=90, fill=INK["active"])
    draw.text((32, cy - 7), "CyberEngine", font=font(13, bold=True), fill=INK["primary"])

    x = 132
    for name in MENUS:
        draw.text((x, cy - 7), name, font=font(12), fill=INK["secondary"])
        x += int(draw.textlength(name, font=font(12))) + 16

    # The centre-right block: project, document, runtime. Right-aligned, so a long project name
    # grows leftwards into space rather than into the menus.
    state = f"{scene.project} · {scene.document} · {scene.runtime}"
    width = int(draw.textlength(state, font=font(12)))
    pill = "live" if scene.live else "no runtime"
    pill_width = int(draw.textlength(pill, font=font(11))) + 18
    right = WIDTH - 12 - pill_width - 12
    draw.text((right - width, cy - 7), state, font=font(12), fill=INK["secondary"])
    draw.rounded_rectangle([WIDTH - 12 - pill_width, cy - 9, WIDTH - 12, cy + 9], radius=9,
                           fill=INK["raised"])
    dot = INK["live"] if scene.live else INK["muted"]
    draw.ellipse([WIDTH - 6 - pill_width, cy - 3, WIDTH - pill_width, cy + 3], fill=dot)
    draw.text((WIDTH - 12 - pill_width + 14, cy - 7), pill, font=font(11), fill=INK["secondary"])


def draw_toolbar(draw, scene: Scene) -> None:
    top = HEADER
    panel(draw, [0, top, WIDTH, top + TOOLBAR], "window")
    y = top + 7
    x = 12
    for name in TOOLS:
        selected = name.lower().startswith(scene.gizmo_mode[:4])
        width = int(draw.textlength(name, font=font(12))) + 14
        if selected:
            draw.rounded_rectangle([x - 6, y - 4, x + width - 6, y + 17], radius=4,
                                   fill=INK["raised"])
        draw.text((x, y), name, font=font(12),
                  fill=INK["primary"] if selected else INK["secondary"])
        x += width + 4
    x += 12
    for name in ACTIONS:
        draw.text((x, y), name, font=font(12), fill=INK["secondary"])
        x += int(draw.textlength(name, font=font(12))) + 18
    draw.text((x + 12, y), "Play", font=font(12), fill=INK["live"])
    draw.text((x + 52, y), "Snap 0.25 m", font=font(12), fill=INK["muted"])


def draw_hierarchy(draw, scene: Scene, box) -> None:
    """The scene tree. Named Hierarchy, holding Nodes — never Actors, never a World Outliner."""
    left, top, right, bottom = box
    panel(draw, box, "panel")
    tabs(draw, left + 10, top + 6, ("Hierarchy", "Layers"))
    draw.rounded_rectangle([left + 10, top + 30, right - 10, top + 50], radius=4, fill=INK["raised"])
    draw.text((left + 18, top + 34), "Search the scene", font=font(12), fill=INK["muted"])
    draw.text((left + 10, top + 56), f"{len(scene.hierarchy)} nodes · 1 selected", font=font(11),
              fill=INK["muted"])

    y = top + 76
    for depth, label in scene.hierarchy:
        # Clipped to the dock. A tree drawn past its panel is not density, it is a bug in a picture.
        if y + 19 > bottom - 6:
            draw.text((left + 16, y), "…", font=font(12), fill=INK["muted"])
            break
        selected = label == scene.selection
        if selected:
            draw.rounded_rectangle([left + 8, y - 3, right - 10, y + 15], radius=3,
                                   fill=INK["raised"])
        colour = INK["selection"] if selected else INK["primary"]
        draw.text((left + 16 + depth * 14, y), label, font=font(12), fill=colour)
        # The per-row visibility toggle the previous references had and the built editor does not.
        # It stays in the reference: `NodeState` gaining a visibility field is a change to the
        # editor, and a reference that dropped it would quietly turn a gap into a decision.
        draw.ellipse([right - 26, y + 4, right - 18, y + 12], outline=INK["muted"])
        y += 19


def draw_content(draw, image, plate, box, scene: Scene) -> None:
    """The content browser, with RENDERED thumbnails — the row of the M5.5 table the reference won.

    Every thumbnail is a crop of the plate, because "every unit is distinguishable BY ITS THUMBNAIL"
    is what makes a large content library navigable and a grid of file icons is not that.
    """
    left, top, right, bottom = box
    panel(draw, box, "panel")
    tabs(draw, left + 10, top + 6, ("Content browser", "Favourites"))
    draw.text((left + 10, top + 32), f"{len(scene.content)} items · 1 selected", font=font(11),
              fill=INK["muted"])

    size = 62
    x, y = left + 12, top + 52
    for index, label in enumerate(scene.content):
        # Crops from the lower two thirds, where a scene's content is: a thumbnail of the sky is a
        # thumbnail nobody can tell from another thumbnail of the sky.
        cx = (137 * index + 90) % max(1, plate.width - 300)
        cy = int(plate.height * 0.45) + (61 * index) % max(1, int(plate.height * 0.4) - 300)
        crop = plate.crop((cx, cy, cx + 300, cy + 300))
        image.paste(crop.resize((size, size)), (x, y))
        if index == 0:
            draw.rounded_rectangle([x - 3, y - 3, x + size + 2, y + size + 2], radius=3,
                                   outline=INK["selection"], width=2)
        draw.text((x, y + size + 4), label[:10], font=font(10), fill=INK["secondary"])
        x += size + 12
        if x + size > right - 10:
            x = left + 12
            y += size + 24


def draw_inspector(draw, scene: Scene, box) -> None:
    """Sections visible at once, and one axis language: X red, Y green, Z blue."""
    left, top, right, bottom = box
    panel(draw, box, "panel")
    tabs(draw, left + 10, top + 6, ("Inspector", "Diagnostics"))
    draw.text((left + 10, top + 32), f"1 selected · {scene.selection}", font=font(12),
              fill=INK["primary"])

    y = top + 58
    for title, rows in scene.inspector:
        draw.text((left + 10, y), title, font=font(12, bold=True), fill=INK["secondary"])
        draw.line([left + 10, y + 18, right - 10, y + 18], fill=INK["line"])
        y += 26
        for label, value in rows:
            draw.text((left + 14, y), label, font=font(11), fill=INK["muted"])
            if value.startswith("xyz:"):
                parts = value[4:].split()
                fx = left + 104
                for axis, part in zip("XYZ", parts):
                    draw.text((fx, y), axis, font=font(10, bold=True), fill=INK[axis.lower()])
                    draw.rounded_rectangle([fx + 9, y - 2, fx + 58, y + 15], radius=3,
                                           fill=INK["raised"])
                    draw.text((fx + 13, y), part, font=font(10, mono=True), fill=INK["primary"])
                    fx += 62
            else:
                draw.rounded_rectangle([left + 108, y - 2, right - 12, y + 15], radius=3,
                                       fill=INK["raised"])
                draw.text((left + 114, y), value, font=font(11), fill=INK["primary"])
            y += 21
        y += 10


def draw_console(draw, scene: Scene, box) -> None:
    """Console · Profiler · Problems — fault 2, and the tab strip is where it used to be visible."""
    left, top, right, bottom = box
    panel(draw, box, "panel")
    tabs(draw, left + 10, top + 6, CONSOLE_TABS)
    y = top + 32
    for level, text in scene.console:
        colour = {"info": INK["secondary"], "ok": INK["live"], "warn": INK["warning"]}[level]
        draw.text((left + 10, y), f"[{level}]", font=font(11, mono=True), fill=colour)
        # Clipped to the panel rather than allowed to run under the window's edge: a console line
        # that overflows its dock is the one thing a reference must not teach.
        message = text
        while message and draw.textlength(message, font=font(11)) > (right - left - 78):
            message = message[:-2] + "…"
        draw.text((left + 56, y), message, font=font(11), fill=INK["secondary"])
        y += 18
    draw.rounded_rectangle([left + 10, bottom - 30, right - 10, bottom - 8], radius=4,
                           fill=INK["raised"])
    draw.text((left + 18, bottom - 27), "Command", font=font(11), fill=INK["muted"])


def draw_axes(draw, centre, length: int, mode: str) -> None:
    """The transform gizmo. Three axes, X red, Y green, Z blue, separated by SHAPE per mode."""
    cx, cy = centre
    arms = {"x": (length, 0), "y": (0, -length), "z": (-length * 0.7, length * 0.5)}
    for axis, (dx, dy) in arms.items():
        end = (cx + dx, cy + dy)
        draw.line([cx, cy, end[0], end[1]], fill=INK[axis], width=4)
        if mode == "translate":
            draw.regular_polygon((end[0], end[1], 7), 3,
                                 rotation={"x": -90, "y": 0, "z": 145}[axis], fill=INK[axis])
        else:
            draw.rectangle([end[0] - 5, end[1] - 5, end[0] + 5, end[1] + 5], fill=INK[axis])
    if mode == "universal":
        for axis, radius in (("y", length), ("x", int(length * 0.86)), ("z", int(length * 0.72))):
            draw.ellipse([cx - radius, cy - radius, cx + radius, cy + radius], outline=INK[axis],
                         width=2)
    draw.ellipse([cx - 4, cy - 4, cx + 4, cy + 4], fill=INK["primary"])


def draw_orientation(draw, box) -> None:
    """The orientation widget: three axes and NOTHING ELSE, so it is never confused with a gizmo.

    It is quieter than the transform gizmo a few hundred pixels away by construction — thinner
    lines, no arrowheads, no rings, no boxes — which is the separation
    `the_two_gizmos_are_unmistakable.rs` protects in the built editor.
    """
    left, top, right, bottom = box
    cx, cy = (left + right) // 2, (top + bottom) // 2
    draw.rounded_rectangle(box, radius=6, fill=(0x14, 0x17, 0x1A))
    for axis, (dx, dy), label in (("x", (26, 8), "X"), ("y", (0, -28), "Y"), ("z", (-24, 12), "Z")):
        draw.line([cx, cy, cx + dx, cy + dy], fill=INK[axis], width=2)
        draw.ellipse([cx + dx - 5, cy + dy - 5, cx + dx + 5, cy + dy + 5], fill=INK[axis])
        draw.text((cx + dx * 1.5 - 3, cy + dy * 1.45 - 6), label, font=font(9),
                  fill=INK["secondary"])
    draw.text((left + 8, bottom - 16), "‹ Persp ›", font=font(10), fill=INK["muted"])


def draw_viewport(draw, image, plate, box, scene: Scene) -> None:
    """The renderer's image, and the chrome that floats IN it rather than around it."""
    left, top, right, bottom = box
    width, height = right - left, bottom - top
    ratio = max(width / plate.width, height / plate.height)
    scaled = plate.resize((int(plate.width * ratio) + 1, int(plate.height * ratio) + 1))
    image.paste(scaled.crop((0, 0, width, height)), (left, top))

    # The view-state strip and, under it, the performance overlay — TOP-LEFT, which is where this
    # document's region table puts it and where the built editor does not yet: M5.5 recorded that
    # difference with the reference as the winner, so the reference keeps saying so.
    draw.rounded_rectangle([left + 12, top + 10, left + 12 + 330, top + 30], radius=4,
                           fill=(0x12, 0x14, 0x17))
    draw.text((left + 20, top + 13), scene.view_state, font=font(11), fill=INK["secondary"])
    if scene.performance:
        overlay_height = 16 * len(scene.performance) + 10
        draw.rounded_rectangle([left + 12, top + 38, left + 12 + 150, top + 38 + overlay_height],
                               radius=4, fill=(0x12, 0x14, 0x17))
        y = top + 43
        for line in scene.performance:
            name, value = line.split("=")
            draw.text((left + 20, y), name, font=font(10, mono=True), fill=INK["muted"])
            draw.text((left + 92, y), value, font=font(10, mono=True), fill=INK["secondary"])
            y += 16

    draw_orientation(draw, [right - 108, top + 10, right - 12, top + 96])

    if scene.outline != (0.0, 0.0, 0.0, 0.0):
        x0, y0, x1, y1 = scene.outline
        # A THIN OUTLINE THAT DOES NOT GLOW, and the material underneath is still judgeable.
        draw.rounded_rectangle([left + x0 * width, top + y0 * height,
                                left + x1 * width, top + y1 * height],
                               radius=4, outline=INK["selection"], width=2)
    draw_axes(draw, (left + scene.gizmo[0] * width, top + scene.gizmo[1] * height), 62,
              scene.gizmo_mode)

    if scene.minimap:
        map_box = [right - 210, bottom - 150, right - 12, bottom - 12]
        draw.rounded_rectangle(map_box, radius=4, fill=(0x10, 0x13, 0x16))
        draw.rectangle([map_box[0] + 8, map_box[1] + 8, map_box[2] - 8, map_box[3] - 8],
                       outline=INK["line"])
        for index in range(9):
            colour = INK["active"] if index % 3 else INK["x"]
            cx = map_box[0] + 26 + (index * 19) % 150
            cy = map_box[1] + 30 + (index * 31) % 90
            draw.ellipse([cx, cy, cx + 5, cy + 5], fill=colour)
    if scene.read_out:
        height = 22 * len(scene.read_out) + 34
        box = [right - 210, bottom - 46 - height, right - 12, bottom - 46]
        draw.rounded_rectangle(box, radius=4, fill=(0x12, 0x14, 0x17))
        draw.text((box[0] + 12, box[1] + 8), "Current rotation", font=font(11),
                  fill=INK["secondary"])
        y = box[1] + 30
        for axis, value in scene.read_out:
            draw.text((box[0] + 14, y), axis, font=font(11, bold=True), fill=INK[axis.lower()])
            draw.text((box[0] + 34, y), value, font=font(11, mono=True), fill=INK["primary"])
            y += 22
    draw.rounded_rectangle([right - 250, bottom - 34, right - 12, bottom - 12], radius=4,
                           fill=(0x12, 0x14, 0x17))
    draw.text((right - 240, bottom - 31), "Camera · Snap · Transform", font=font(11),
              fill=INK["muted"])


def draw_rail(draw, box) -> None:
    """The adventure reference's vertical tool rail: mode-level tools without toolbar width."""
    left, top, right, bottom = box
    panel(draw, box, "panel")
    y = top + 12
    for index in range(7):
        colour = INK["primary"] if index == 1 else INK["muted"]
        draw.rounded_rectangle([left + 6, y, right - 6, y + 26], radius=4,
                               fill=INK["raised"] if index == 1 else INK["panel"])
        draw.regular_polygon((( left + right) // 2, y + 13, 6), 4 + (index % 3),
                             rotation=index * 12, outline=colour)
        y += 34


def draw_lower_dock(draw, box, dock) -> None:
    """The graph, docked in the workspace rather than in a window of its own."""
    title, nodes = dock
    left, top, right, bottom = box
    panel(draw, box, "panel")
    tabs(draw, left + 10, top + 6, (title, "Animation"))
    x, y = left + 24, top + 40
    previous = None
    for index, label in enumerate(nodes):
        width = int(draw.textlength(label, font=font(11))) + 26
        draw.rounded_rectangle([x, y, x + width, y + 34], radius=5, fill=INK["raised"],
                               outline=INK["line"])
        draw.rectangle([x, y, x + width, y + 12], fill=(0x24, 0x2A, 0x31))
        draw.text((x + 8, y + 15), label, font=font(11), fill=INK["primary"])
        draw.ellipse([x - 4, y + 20, x + 4, y + 28], fill=INK["active"])
        if previous is not None:
            draw.line([previous, y + 24, x - 4, y + 24], fill=INK["active"], width=2)
        previous = x + width + 4
        draw.ellipse([x + width - 4, y + 20, x + width + 4, y + 28], fill=INK["active"])
        x += width + 46
        y += 22 if index % 2 else -8


def draw_footer(draw, scene: Scene) -> None:
    top = HEIGHT - FOOTER
    panel(draw, [0, top, WIDTH, HEIGHT], "panel")
    draw.text((12, top + 4), scene.status, font=font(11), fill=INK["live"])
    right_text = "Scene · Compact"
    draw.text((WIDTH - 12 - draw.textlength(right_text, font=font(11)), top + 4), right_text,
              font=font(11), fill=INK["muted"])


# --- One reference --------------------------------------------------------------------------------


def render(scene: Scene, out: Path) -> None:
    from PIL import Image, ImageDraw

    plate = Image.open(HERE / scene.plate).convert("RGB")
    image = Image.new("RGB", (WIDTH, HEIGHT), INK["window"])
    draw = ImageDraw.Draw(image)

    rail = 40 if scene.rail else 0
    body_top = HEADER + TOOLBAR
    body_bottom = HEIGHT - FOOTER
    left_box = (rail, body_top, rail + LEFT, body_bottom)
    right_box = (WIDTH - RIGHT, body_top, WIDTH, body_bottom)
    centre_left = rail + LEFT + GAP
    centre_right = WIDTH - RIGHT - GAP

    draw_header(draw, scene)
    draw_toolbar(draw, scene)
    if scene.rail:
        draw_rail(draw, (0, body_top, rail, body_bottom))

    if scene.layout == "bottom-docks":
        dock_top = body_bottom - 236
        draw_hierarchy(draw, scene, (rail, dock_top, rail + 300, body_bottom))
        draw_content(draw, image, plate,
                     (rail + 300 + GAP, dock_top, centre_right, body_bottom), scene)
        console_top = body_bottom - 236
        draw_inspector(draw, scene, (right_box[0], right_box[1], right_box[2], console_top - GAP))
        draw_console(draw, scene, (right_box[0], console_top, right_box[2], body_bottom))
        draw_viewport(draw, image, plate, (rail, body_top, centre_right, dock_top - GAP), scene)
        draw_footer(draw, scene)
        out.parent.mkdir(parents=True, exist_ok=True)
        image.save(out, optimize=True)
        print(f"  {out}")
        return

    # The left column is the hierarchy over the content browser; the proportion is the reference's
    # own claim about density — about thirty rows visible without crowding.
    split = body_top + 560
    draw_hierarchy(draw, scene, (left_box[0], left_box[1], left_box[2], split - GAP))
    draw_content(draw, image, plate, (left_box[0], split, left_box[2], body_bottom), scene)

    # The right column is the inspector over the console. Every section visible at once.
    console_top = body_bottom - 300
    draw_inspector(draw, scene, (right_box[0], right_box[1], right_box[2], console_top - GAP))
    draw_console(draw, scene, (right_box[0], console_top, right_box[2], body_bottom))

    viewport_bottom = body_bottom - (260 if scene.lower_dock else 0)
    draw_viewport(draw, image, plate, (centre_left, body_top, centre_right, viewport_bottom - GAP),
                  scene)
    if scene.lower_dock:
        draw_lower_dock(draw, (centre_left, viewport_bottom, centre_right, body_bottom),
                        scene.lower_dock)
    draw_footer(draw, scene)

    out.parent.mkdir(parents=True, exist_ok=True)
    image.save(out, optimize=True)
    print(f"  {out}")


# --- The three ------------------------------------------------------------------------------------
#
# Every string below is the reference's own content, and each one is checked against the vocabulary
# table in docs/design/editor-visual-language.md: Node and entity, mesh and mesh instance, graph and
# script graph, content browser, prefab, world and scene and cell. A word from another engine here
# is the fault that made the previous three images wrong.

SCENES = {
    "rts": Scene(
        name="rts",
        plate="scene-rts.jpg",
        project="DesertFrontier",
        document="worlds/basin.cyworld",
        runtime="Live runtime",
        live=True,
        hierarchy=[(0, "DesertFrontier"), (1, "Environment"), (2, "Landscape"),
                   (2, "RockFormations"), (2, "SandDunes"), (2, "PollutedLake_01"),
                   (2, "DeadTrees"), (1, "Lighting"), (2, "DirectionalLight"), (2, "SkyAtmosphere"),
                   (2, "HeightFog"), (1, "Structures"), (2, "Player_Base"), (3, "Command_Hub"),
                   (3, "Power_Generator"), (3, "Barracks"), (1, "Units"), (2, "Harvester_01"),
                   (2, "Harvester_02"), (2, "Harvester_03"), (1, "Enemies"), (2, "Insectoid_Hive"),
                   (2, "Insectoid_01"), (2, "Insectoid_02")],
        content=["Harvester", "Scout", "Worker", "Command", "Refinery", "Turret", "Insectoid",
                 "Queen"],
        inspector=[
            ("Transform", [("Position", "xyz:1250.6 328.4 -842.7"),
                           ("Rotation", "xyz:0.0 90.0 0.0"),
                           ("Scale", "xyz:1.0 1.0 1.0")]),
            ("Unit", [("Team", "Player"), ("Health", "150 / 150"), ("Armour", "25.0"),
                      ("Cargo", "125 / 500")]),
            ("Behaviour", [("Controller", "HarvesterBehaviour"), ("State", "Harvesting"),
                           ("Auto gather", "on")]),
            ("Mesh instance", [("Mesh", "harvester_a"), ("Material", "m_harvester_worn")]),
            ("Rendering", [("Visible", "on"), ("Cast shadow", "on")]),
        ],
        selection="Harvester_01",
        console=[("info", "World loaded: worlds/basin.cyworld"),
                 ("info", "1,245 nodes activated in 12 cells (3.62 s)"),
                 ("info", "Navigation built (2.13 s)"),
                 ("ok", "Streaming: 12 cells resident, 4 prefetching"),
                 ("info", "Shader pipelines compiled (184)"),
                 ("warn", "Cell 14,12 exceeded its activation budget once"),
                 ("info", "Play in editor")],
        status="All saved · No problems · Live",
        gizmo=(0.32, 0.46),
        outline=(0.21, 0.30, 0.44, 0.55),
        minimap=True,
        performance=["Frame=16.1 ms", "Draw=12.3 ms", "GPU=11.7 ms", "Cells=12", "Mem=6.2 GB"],
    ),
    "adventure": Scene(
        name="adventure",
        plate="scene-adventure.jpg",
        project="AncientFrontier",
        document="worlds/valley.cyworld",
        runtime="Live runtime",
        live=True,
        hierarchy=[(0, "AncientFrontier"), (1, "World"), (2, "Terrain"), (2, "WaterSystem"),
                   (2, "OakForest"), (1, "Lighting"), (2, "DirectionalLight"),
                   (2, "SkyAtmosphere"), (2, "VolumetricClouds"), (1, "CameraRig"),
                   (2, "ThirdPersonCamera"), (2, "SpringArm"), (1, "Player"),
                   (2, "ThirdPersonCharacter"), (3, "Mesh"), (3, "InteractionProbe"),
                   (1, "Props"), (2, "Ruins_Column_A"), (2, "Ruins_Column_B"),
                   (2, "Ruins_Statue"), (2, "Wooden_Crate")],
        content=["m_rock_cliff", "t_grass", "m_cliff", "s_oak", "t_rock", "m_stone", "t_cliff",
                 "s_pine"],
        inspector=[
            ("Selection", [("Nodes", "3 selected"), ("Kind", "mesh instance")]),
            ("Transform", [("Position", "xyz:1250.6 328.4 -842.7"),
                           ("Rotation", "xyz:15.0 -10.0 5.0"),
                           ("Scale", "xyz:1.2 1.2 1.2")]),
            ("Mesh instance", [("Mesh", "ruins_column_a"), ("Mesh", "wooden_crate"),
                               ("Mesh", "ruins_statue")]),
            ("Materials", [("Element 0", "m_stone_ruins"), ("Element 1", "m_wood_old")]),
            ("Rendering", [("Visible", "on"), ("Cast shadow", "on"),
                           ("Global illumination", "on")]),
        ],
        selection="Ruins_Column_A",
        console=[("info", "World loaded: worlds/valley.cyworld"),
                 ("info", "Cells: 1,024 authored, 18 resident, 6 prefetching"),
                 ("info", "Script graph 'ThirdPersonCharacter' compiled in 86 ms"),
                 ("ok", "Virtual texture: mip tail resident, 0 missing samples"),
                 ("info", "Shader pipelines compiled (120)"),
                 ("info", "Play in editor")],
        status="All saved · No problems · Live",
        gizmo=(0.53, 0.46),
        gizmo_mode="universal",
        outline=(0.45, 0.24, 0.62, 0.72),
        rail=True,
        lower_dock=("Script graph", ["Event tick", "Get input axis", "Move", "Break vector",
                                     "Add movement"]),
        performance=["Frame=16.1 ms", "Draw=12.3 ms", "GPU=11.3 ms", "Cells=18", "Mem=6.2 GB"],
    ),
    "scene-view": Scene(
        name="scene-view",
        plate="scene-courtyard.jpg",
        project="Terranova",
        document="worlds/courtyard.cyworld",
        runtime="No runtime",
        live=False,
        hierarchy=[(0, "Terranova"), (1, "Environment"), (2, "Courtyard"), (2, "Columns"),
                   (2, "Rubble"), (1, "Props"), (2, "Dragon_Statue"), (2, "Crate"),
                   (1, "Lights"), (2, "SunShaft"), (1, "Cameras"), (2, "SceneCamera")],
        content=["Materials", "Meshes", "Textures", "Worlds", "Characters", "Effects"],
        inspector=[
            ("Transform", [("Position", "xyz:0.000 0.000 0.000"),
                           ("Rotation", "xyz:15.000 42.500 0.000"),
                           ("Scale", "xyz:1.000 1.000 1.000")]),
            ("Mesh instance", [("Mesh", "dragon_statue"), ("Material", "m_stone_carved")]),
            ("Rendering", [("Visible", "on"), ("Cast shadow", "on")]),
        ],
        selection="Dragon_Statue",
        console=[("info", "World loaded: worlds/courtyard.cyworld"),
                 ("info", "3 nodes · 1 selected"),
                 ("ok", "Transform mode: translate · pivot · world space"),
                 ("info", "Hold Ctrl to snap · Shift for precision · Alt to duplicate")],
        status="All saved · No problems · Idle",
        gizmo=(0.20, 0.40),
        outline=(0.05, 0.06, 0.36, 0.72),
        rail=True,
        layout="bottom-docks",
        read_out=[("X", "15.0°"), ("Y", "42.5°"), ("Z", "0.0°")],
        view_state="Perspective · Lit · translate · World · Pivot · Snap 0.25 m",
        performance=["Frame=0.00 ms", "Draw=0", "GPU=—", "Cells=0", "Mem=0.4 GB"],
    ),
}

OUTPUTS = {
    "rts": "editor-rts-desertfrontier.png",
    "adventure": "editor-adventure-ancientfrontier.png",
    "scene-view": "editor-scene-view.png",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", choices=sorted(SCENES), help="draw one of the three")
    parser.add_argument("--out", default=str(IMAGES), help="where the images go")
    arguments = parser.parse_args()

    try:
        import PIL  # noqa: F401
    except ImportError:
        print("render_references: needs Pillow (pip install Pillow)", file=sys.stderr)
        return 2

    print("==> references  docs/design/images/")
    for name in ([arguments.only] if arguments.only else sorted(SCENES)):
        render(SCENES[name], Path(arguments.out) / OUTPUTS[name])
    return 0


if __name__ == "__main__":
    sys.exit(main())

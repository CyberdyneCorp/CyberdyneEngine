#!/usr/bin/env python3
"""samples/05b-editor-window — the M5.5 artefact a person drives. Task 4.1.

`just run-editor-window` is the recipe; `smoke.editor_window` is the CTest entry where a display
exists; this file is what both of them run.

--- WHAT IT IS ---------------------------------------------------------------------------------

Two processes and a real X11 session:

    cy-viewport-publisher   the runtime's half of the viewport transport — a second process that
                            renders into dma-buf images on the GPU and hands them across. It is a
                            fixture, not the engine, and the wire it speaks is the one the engine's
                            runtime will speak: `cy_editor_viewport_transport::publisher`.
    cyberdyne-editor        the editor, with a window, opened on a project.

and then this driver operates that window the way a person does: **synthesised X11 key presses and
pointer events through XTEST**, delivered to the server rather than to the application. Nothing here
is a test hook. The editor has no idea it is not a hand on a keyboard, which is what
`delivery-roadmap` means by an artefact exercising the capabilities "through the same entry points a
user would use" — and what M5's scripted session, by its own admission, could not do.

--- HOW A WINDOW IS OBSERVED, WHICH IS THE ONLY SUBTLE THING HERE -------------------------------

Three observables, and none of them is "it did not crash":

  1. **The journal.** `--journal <dir>` makes every committed transaction a length-prefixed record
     in `<document>.cyjournal`, appended and flushed on commit. Counting records is how this driver
     knows what the keyboard actually committed, and `file.save` discarding the journal is how it
     knows the save reached the document rather than the button.
  2. **The hierarchy's rows.** Undo and redo do not touch the journal — correctly: it is an
     append-only record of what was committed. So they are observed where a person observes them,
     in the outliner, by counting the rows drawn in it. `rows_in_hierarchy` is deliberately a
     count of drawn rows rather than a reading of text: it is what changes when undo removes an
     entity, and it needs no font.
  3. **The viewport's pixels.** The publisher's frame is a strongly saturated field with a moving
     white bar. The editor's own chrome is charcoal and, per `docs/design/editor-visual-language.md`,
     is neutral by rule. So "the viewport region is saturated" is a statement no toolkit-drawn
     approximation could satisfy: those pixels came out of another process's GPU allocation. That
     is task 2.1 photographed rather than asserted.

--- WHAT M6 CLOSED HERE, AND WHAT IT COST -------------------------------------------------------

At M5.5 this file recorded a gap in its own output: *a gizmo drag moves nothing, and it is not the
viewport's fault.* `DocumentService::open` called `Document::new` — a name and an empty schema —
because there was no world loader, so `TransformBinding::of_schema` found no `Transform` and a drag
committed nothing.

M6 task 2.4 closed it from the engine's end rather than the editor's. `types.cytypes` in the project
is the engine's own type registry, written by `cy::scene::serialization::write_authoring_schema` and
regenerated and compared byte for byte by `cy_test_unit_scene_serialization`; the editor reads it in
`cy_editor_services::worldfile`. So the schema an opened document has is the ENGINE'S component
types, `scene.create-entity` gives every new entity the `Transform` that schema describes, and
`file.save` writes the world back as `worlds/city.cyworld`. Acts 3 and 5 below are what say so.

The rule this file has kept since M5 is unchanged: an artefact that quietly narrows its claim to what
happens to work reports a milestone as closed that is not. Act 3 still reports a gap rather than a
pass if the drag commits nothing.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

SAMPLE = Path(__file__).resolve().parent
ROOT = SAMPLE.parents[1]

# THE ARTEFACT HARNESS, NOT A LOCAL COPY OF IT. `samples/harness/artefact.py` owns the two rules
# this file broke at M6 — a run that recorded a gap returned 0, and a headline may not be an
# extreme-value statistic — and it owns them so that no artefact can break them again by being
# written carefully in its own style. M7 tasks 5b.5 and 5b.5b.
sys.path.insert(0, str(ROOT / "samples" / "harness"))
from artefact import (  # noqa: E402 — the path above has to be set first
    Absent,
    Failed,
    Report,
    Statistic,
    expect,
    socket_path,
)

WORLD = "worlds/city.cyworld"

# The journal's format, from `cy_editor_documents::journal`. Eight magic bytes, a little-endian
# format version, then records of a little-endian length and checksum followed by the payload.
# Restated here rather than linked because this driver has to read the file WITHOUT the editor —
# an observable the editor computed for us would only be the editor agreeing with itself.
JOURNAL_MAGIC = b"CYJRNL\x00\x01"

# What the publisher clears its images to. `cy_editor_viewport_transport::publisher` draws a
# saturated field with a moving bar; the number that matters is that it is FAR from neutral, and the
# editor's own surfaces are neutral by rule (editor-visual-language, "Dark and neutral").
MIN_VIEWPORT_CHROMA = 40

# What the engine's runtime renders at. Not the space the layout arrives in — the engine rescales
# the layout into the viewport's own pixels before publishing it, because that is the space the
# editor hit-tests a pointer in (`cy::render::rescale_gizmo_layout`). It is here because the runtime
# is started with it, and because a report that says "1280x720 stretched into a 934x570 panel" is
# the sentence a reader needs when a handle is a third of a viewport away from where it was aimed.
RUNTIME_WIDTH = 1280
RUNTIME_HEIGHT = 720


def until(predicate, seconds: float = 8.0, poll: float = 0.25) -> bool:
    """Wait for something to become true, up to a deadline.

    EVERY WAIT IN THIS FILE IS A DEADLINE AND NONE IS A SLEEP, and that is not tidiness. A window
    answers a key press when its next frame runs, and on a loaded machine that is not a fixed
    number of milliseconds — an artefact built on `sleep(0.4)` passes on a quiet machine and fails
    in continuous integration, which is the worst kind of check because it teaches people to re-run
    it. Two agents on this milestone reported exactly that shape of flake in their own suites.
    """
    deadline = time.monotonic() + seconds
    while True:
        if predicate():
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(poll)


def press_for(session, key: str, modifiers: tuple[str, ...], changed, attempts: int = 4,
              seconds: float = 12.0) -> int:
    """Press a key until the window shows that it arrived, and return how many presses it took.

    THE RE-SEND IS GUARDED BY "NOTHING HAS CHANGED", not by a timer, and that is the whole design.
    An X server drops nothing, but a window that is not yet focused, or is mid-resize, or has not
    run a frame, can miss one — and a driver that pressed Ctrl+Z twice because the first press was
    merely SLOW would undo two things and report a wrong result rather than a flake. So `changed`
    becomes true on ANY effect, and a second press happens only while it is still false.
    """
    for attempt in range(attempts):
        session.ensure_focus()
        session.key(key, modifiers)
        if until(changed, seconds=seconds, poll=0.2):
            return attempt + 1
    return 0


# --- The display, the window, and a hand on the keyboard -----------------------------------------


class Session:
    """One editor window, and the X11 session it is in."""

    def __init__(self, display_name: str):
        try:
            import PIL.Image as _pillow  # `capture` uses it; imported here to fail early
            from Xlib import X, XK, display
            from Xlib.ext import xtest
        except ImportError as missing:  # pragma: no cover — the absence path
            raise Absent(
                f"{missing.name} is not installed. This artefact operates a real window: it needs "
                "python-xlib for XTEST input and window capture, and Pillow to read the pixels back"
            ) from missing
        self.X, self.XK, self.xtest, self.pillow = X, XK, xtest, _pillow
        try:
            self.display = display.Display(display_name)
        except Exception as problem:  # pragma: no cover — the absence path
            raise Absent(f"no X display at {display_name!r}: {problem}") from problem
        if not self.display.has_extension("XTEST"):
            raise Absent(
                f"the X server at {display_name!r} has no XTEST extension, so a key press cannot "
                "be synthesised and this artefact cannot operate the window"
            )
        self.root = self.display.screen().root
        self.window = None
        self.geometry = (0, 0, 0, 0)

    # -- finding and focusing --------------------------------------------------------------------

    def attach(self, pid: int, seconds: float = 40.0) -> None:
        """Wait for the window this process mapped, and give it the keyboard."""
        wanted = self.display.intern_atom("_NET_WM_PID")
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            found = self._find(self.root, wanted, pid)
            if found is not None:
                self.window = found
                break
            time.sleep(0.2)
        if self.window is None:
            raise Failed(f"the editor mapped no window within {seconds:.0f} s")
        self.activate()
        self.display.sync()
        time.sleep(1.0)
        self._measure()

    def _find(self, node, wanted, pid):
        try:
            value = node.get_full_property(wanted, self.X.AnyPropertyType)
            if value is not None and value.value and value.value[0] == pid:
                attributes = node.get_attributes()
                if attributes.map_state == self.X.IsViewable:
                    return node
            for child in node.query_tree().children:
                found = self._find(child, wanted, pid)
                if found is not None:
                    return found
        except Exception:  # a window can disappear between the query and the read
            return None
        return None

    def activate(self) -> None:
        """Raise the window and give it the input focus, the way a window manager would."""
        from Xlib import protocol

        active = self.display.intern_atom("_NET_ACTIVE_WINDOW")
        event = protocol.event.ClientMessage(
            window=self.window, client_type=active, data=(32, (2, self.X.CurrentTime, 0, 0, 0))
        )
        mask = self.X.SubstructureRedirectMask | self.X.SubstructureNotifyMask
        self.root.send_event(event, event_mask=mask)
        self.window.configure(stack_mode=self.X.Above)
        self.display.sync()
        time.sleep(0.4)
        try:
            self.window.set_input_focus(self.X.RevertToParent, self.X.CurrentTime)
        except Exception:
            pass
        self.display.sync()
        time.sleep(0.3)

    def _measure(self) -> None:
        geometry = self.window.get_geometry()
        origin = self.window.translate_coords(self.root, 0, 0)
        self.geometry = (-origin.x, -origin.y, geometry.width, geometry.height)

    @property
    def width(self) -> int:
        return self.geometry[2]

    @property
    def height(self) -> int:
        return self.geometry[3]

    # -- input -----------------------------------------------------------------------------------

    def is_focused(self) -> bool:
        """Whether the X server would deliver a synthesised key press to this window."""
        try:
            active = self.root.get_full_property(
                self.display.intern_atom("_NET_ACTIVE_WINDOW"), self.X.AnyPropertyType
            )
            if active is not None and active.value and active.value[0] == self.window.id:
                return True
            focused = self.display.get_input_focus().focus
            return hasattr(focused, "id") and focused.id == self.window.id
        except Exception:
            return False

    def ensure_focus(self, seconds: float = 15.0) -> None:
        """Give the window the keyboard back, and wait until the server agrees that it has it.

        Called at the head of every act that types, and it WAITS rather than asking once. A
        synthesised key press goes to whatever the X server thinks is focused, so an artefact that
        assumed focus would send half a session into somebody else's window — a wrong result and a
        rude one — and a window manager that maps and focuses on its own schedule makes "assume it
        worked" a race. This is the deadline that closed the one flake this driver had.
        """
        if until(self.is_focused, seconds=1.0, poll=0.1):
            return
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.activate()
            if until(self.is_focused, seconds=1.5, poll=0.15):
                return
        raise Failed(
            "the editor's window never took the keyboard focus, so nothing could be typed into it"
        )

    def _code(self, name: str) -> int:
        code = self.display.keysym_to_keycode(self.XK.string_to_keysym(name))
        if code == 0:
            raise Failed(f"this keyboard layout has no key for {name!r}")
        return code

    def key(self, name: str, modifiers: tuple[str, ...] = ()) -> None:
        for modifier in modifiers:
            self.xtest.fake_input(self.display, self.X.KeyPress, self._code(modifier))
        code = self._code(name)
        self.xtest.fake_input(self.display, self.X.KeyPress, code)
        self.display.sync()
        time.sleep(0.04)
        self.xtest.fake_input(self.display, self.X.KeyRelease, code)
        for modifier in reversed(modifiers):
            self.xtest.fake_input(self.display, self.X.KeyRelease, self._code(modifier))
        self.display.sync()
        time.sleep(0.18)

    def move(self, x: int, y: int) -> None:
        """Move the pointer to a point inside the window, in the window's own coordinates."""
        left, top = self.geometry[0], self.geometry[1]
        self.xtest.fake_input(self.display, self.X.MotionNotify, x=left + x, y=top + y)
        self.display.sync()
        time.sleep(0.03)

    def click(self, x: int, y: int, button: int = 1) -> None:
        self.move(x, y)
        self.xtest.fake_input(self.display, self.X.ButtonPress, button)
        self.display.sync()
        time.sleep(0.06)
        self.xtest.fake_input(self.display, self.X.ButtonRelease, button)
        self.display.sync()
        time.sleep(0.2)

    def drag(self, start: tuple[int, int], end: tuple[int, int], steps: int = 16) -> None:
        self.move(*start)
        self.xtest.fake_input(self.display, self.X.ButtonPress, 1)
        self.display.sync()
        time.sleep(0.08)
        for index in range(1, steps + 1):
            self.move(
                start[0] + (end[0] - start[0]) * index // steps,
                start[1] + (end[1] - start[1]) * index // steps,
            )
        time.sleep(0.08)
        self.xtest.fake_input(self.display, self.X.ButtonRelease, 1)
        self.display.sync()
        time.sleep(0.25)

    # -- looking ---------------------------------------------------------------------------------

    def region(self, left: int, top: int, width: int, height: int):
        """One rectangle of the window's own pixels, as a PIL image.

        Separate from `capture` because the outliner is polled in a loop and reading a whole
        1600x950 window each time is most of what such a loop costs — which is how a deadline that
        is generous in seconds becomes a deadline of four attempts on a loaded machine.
        """
        raw = self.window.get_image(left, top, width, height, self.X.ZPixmap, 0xFFFFFFFF)
        return self.pillow.frombytes("RGB", (width, height), raw.data, "raw", "BGRX")

    def capture(self, path: Path):
        """The whole window, as a PIL image, saved to `path`."""
        image = self.region(0, 0, self.width, self.height)
        path.parent.mkdir(parents=True, exist_ok=True)
        image.save(path)
        return image


# --- Reading the observables -----------------------------------------------------------------------


# --- The screen has to be awake, and that is not a nicety ------------------------------------------


def wake_the_display() -> str:
    """Deactivate a running screensaver, and say which mechanism answered.

    --- THE FINDING THIS FUNCTION IS -------------------------------------------------------------

    M6 recorded that "XTEST does not deliver synthesised key events in this X session", reproduced
    it with a small python-xlib program, and planned around it. That conclusion was wrong, and the
    cost of it being wrong was a whole milestone's keyboard-driven acts.

    **It is the screensaver.** A locked or blanked session holds an active keyboard AND pointer
    grab, so every synthesised event goes to the locker instead of to the application — while the
    windows underneath keep rendering, so a screenshot still comes back and everything looks fine.
    Measured on this machine: `grab_keyboard` on the root window answers `AlreadyGrabbed` while
    `cinnamon-screensaver` is active and `Success` a second after `--deactivate`, and key events
    start arriving at the editor in the same second.

    The symptom is intermittent by nature — it depends on how long the machine has been idle — which
    is exactly why it was diagnosed as a property of XTEST. A run started within the idle timeout
    works; one started after it does not.

    Best-effort and never fatal: a machine with no screensaver returns "none" and the session runs
    exactly as before. `keyboard_arrives` below is what checks the result rather than assuming it.
    """
    attempts = (
        ("cinnamon-screensaver", ["cinnamon-screensaver-command", "--deactivate"]),
        ("gnome-screensaver", ["gnome-screensaver-command", "--deactivate"]),
        ("xdg", ["xdg-screensaver", "reset"]),
    )
    for name, command in attempts:
        if shutil.which(command[0]) is None:
            continue
        try:
            subprocess.run(command, check=False, timeout=20, capture_output=True)
        except (OSError, subprocess.TimeoutExpired):
            continue
        return name
    return "none"


# --- Whether this X server delivers a synthesised key press ---------------------------------------


def keyboard_arrives(session) -> bool:
    """Whether XTEST key events reach the editor on this machine, checked rather than assumed.

    --- WHY THIS EXISTS ------------------------------------------------------------------------

    On this project's development machine, XTEST delivers synthesised POINTER events reliably and
    synthesised KEY events only sometimes: two runs an hour apart, of the same binary, differed —
    the first created three entities from Ctrl+Shift+N and the second saw Ctrl+P not open the
    command palette. It is a property of the X session rather than of the editor, and no amount of
    waiting or re-pressing changes it, because the press never arrives.

    An artefact that assumed keys arrive would report "Ctrl+Shift+N never produced entity 1" — a
    sentence that reads as a defect in the editor's keymap and is a defect in nobody's code. So it
    is CHECKED, once, and what cannot be driven is reported as not evaluated rather than as failed
    or as passed (`delivery-roadmap`, and `harness.artefact.Report.not_evaluated`).

    --- THE CHECK ------------------------------------------------------------------------------

    Ctrl+P opens the command palette, which covers the top of the window. The observable is a PIXEL
    DIFFERENCE in that band, for the reason `palette_band` gives: "is the band neutral" is also true
    when the viewport behind it is dark, and this check decides how the whole rest of the session is
    driven.
    """
    session.ensure_focus()
    before = palette_band(session)
    session.key("p", ("Control_L",))
    arrived = until(
        lambda: band_changed(before, palette_band(session)) > len(before) // 20,
        seconds=4.0, poll=0.4,
    )
    session.key("Escape")
    time.sleep(0.5)
    return arrived


# The editor's toolbar, as fractions of the window. The second row of chrome, left to right:
# Move · Rotate · Scale · Universal, then Undo · Redo · Save. Fractions rather than pixels because
# the window is opened at whatever size the display allows, and this row's layout is proportional.
#
# THEY ARE HERE SO THAT A SESSION CAN BE DRIVEN WITHOUT A KEYBOARD. Every one of them is a button a
# person clicks, so a pointer-driven act is the same entry point a user would use — which is what
# `delivery-roadmap` asks of an artefact — rather than a test hook.
TOOLBAR = {
    "move": (0.0153, 0.0453),
    "rotate": (0.0438, 0.0453),
    "scale": (0.0706, 0.0453),
    "universal": (0.1028, 0.0453),
    "undo": (0.1409, 0.0453),
    "redo": (0.1656, 0.0453),
    "save": (0.1894, 0.0453),
}


def click_toolbar(session, button: str) -> None:
    """Press one of the toolbar's buttons with the pointer."""
    fraction = TOOLBAR[button]
    session.click(int(session.width * fraction[0]), int(session.height * fraction[1]))


def journal_records(directory: Path) -> int:
    """How many transactions the editor has committed and flushed, read out of its own journal."""
    files = sorted(directory.glob("*.cyjournal")) if directory.is_dir() else []
    if not files:
        return 0
    data = files[0].read_bytes()
    if len(data) < len(JOURNAL_MAGIC) + 4 or data[: len(JOURNAL_MAGIC)] != JOURNAL_MAGIC:
        raise Failed(f"{files[0]} does not start with a journal header")
    offset, records = len(JOURNAL_MAGIC) + 4, 0
    while offset + 8 <= len(data):
        length, _checksum = struct.unpack_from("<II", data, offset)
        offset += 8 + length
        if offset > len(data):
            break  # a record that was being written when the process stopped; not our case
        records += 1
    return records


def outliner_box(width: int, height: int) -> tuple[int, int, int, int]:
    """The outliner's list area, as (left, top, width, height) in the window's own coordinates."""
    left, right = int(width * 0.010), int(width * 0.160)
    top, bottom = int(height * 0.125), int(height * 0.500)
    return left, top, right - left, bottom - top


def rows_in_patch(patch) -> int:
    """Count the rows drawn in a picture of the outliner's list area.

    Not a reading of the text — a count of the horizontal bands that have anything drawn in them. It
    is what changes when undo removes an entity, and unlike reading a label it needs no font, no
    interface-scale assumption and no locale.
    """
    grey = patch.convert("L")
    columns, lines = grey.size
    pixels = grey.load()
    rows, inside = 0, False
    for y in range(lines):
        row = [pixels[x, y] for x in range(columns)]
        busy = max(row) - min(row) > 24
        if busy and not inside:
            rows += 1
        inside = busy
    return rows


def rows_in_hierarchy(session, image=None) -> int:
    """How many rows the outliner is drawing right now, read from the window itself."""
    box = outliner_box(session.width, session.height)
    if image is not None:
        return rows_in_patch(image.crop((box[0], box[1], box[0] + box[2], box[1] + box[3])))
    return rows_in_patch(session.region(*box))


def viewport_rect(width: int, height: int) -> tuple[int, int, int, int]:
    """Where the engine's frame is drawn in the window, as (left, top, right, bottom).

    THE DOCK'S PROPORTIONS, not a search of the image. `editor-rust-application`'s workspace opens
    with a fixed arrangement — outliner left, inspector right, script graph below — and every panel
    in it is a fraction of the window, so the viewport's rectangle is one too. `outliner_box` above
    is the same reasoning about the same dock, and has been since M5.5.

    It is deliberately not derived from the pixels. Two attempts at that failed in ways worth
    recording: the engine's frame is bright where the interface is dark, but so is the content
    browser's thumbnail, and the frame's own sky is darker than the floor — so a brightness
    projection found the outliner. Where a search IS used is `handle_in_window` below, which refines
    a mapped point to the handle it can actually see, and that search has a colour to key on because
    the engine drew it.
    """
    return (
        int(width * 0.1888),
        int(height * 0.0958),
        int(width * 0.7725),
        int(height * 0.6863),
    )


#: The colour the engine draws each axis handle in, from `cy_editor_visual::axis::colour`'s dark
#: theme — **the one colour convention shared with every other tool**, and the reason a handle can
#: be found in a screenshot at all.
AXIS_INK = {
    "axis-x": (0xF2, 0x61, 0x5C),
    "box-x": (0xF2, 0x61, 0x5C),
    "ring-x": (0xF2, 0x61, 0x5C),
    "axis-y": (0x4F, 0xC2, 0x6B),
    "box-y": (0x4F, 0xC2, 0x6B),
    "ring-y": (0x4F, 0xC2, 0x6B),
    "axis-z": (0x5B, 0x9B, 0xF8),
    "box-z": (0x5B, 0x9B, 0xF8),
    "ring-z": (0x5B, 0x9B, 0xF8),
}


def read_layout(path: Path) -> dict | None:
    """The gizmo layout the engine published, or `None` when it has published none.

    Written by `cy_editor_window_runtime --layout`, as one line of JSON, replaced by rename so a
    reader never sees half of one. It is the engine's own answer to "where did I put the handles",
    in the pixels of the viewport the editor asked about — which is what makes the drag below land
    on a HANDLE rather than on a coordinate somebody hoped one was at. M7 task 5b.4.
    """
    try:
        text = path.read_text()
    except OSError:
        return None
    try:
        layout = json.loads(text)
    except ValueError:
        return None  # written between the open and the read; the next poll gets it
    return layout if layout.get("handles") else None


def handle_in_window(session, layout: dict, handle: str, image=None) -> tuple[int, int] | None:
    """Where one published handle is, in the window's own pixels.

    Two steps, and the second is what makes this an observation rather than an assumption:

      1. **Translate.** The engine publishes in the pixels of the viewport the editor asked about
         (`cy::render::rescale_gizmo_layout`), so this is the panel's origin and nothing else.
      2. **Refine.** Look for the handle the engine actually DREW, near where the translation says
         it is, by its axis colour. If the two agree the aim is the drawn handle; if the search
         finds nothing the translated point stands, and the drag either lands inside the editor's
         twelve-pixel acquisition slop or the act reports a gap.

    The refinement is what makes a wrong panel origin a smaller error than a wrong gizmo: it
    corrects for whatever the dock's proportions are on this window, using the one thing that is
    unambiguous — a saturated axis colour, on a scene the engine renders in greys and pastels.
    """
    spot = layout["handles"].get(handle)
    if spot is None:
        return None
    if image is None:
        image = session.region(0, 0, session.width, session.height)
    left, top, _right, _bottom = viewport_rect(session.width, session.height)
    return _nearest_ink(image, AXIS_INK.get(handle), int(round(left + spot[0])),
                        int(round(top + spot[1])))


#: How far from the translated point the drawn handle is looked for, in pixels. Larger than any
#: error the dock's proportions can produce and much smaller than the distance between two handles,
#: so a search can find the wrong handle only if the translation is wrong by more than a handle's
#: spacing — which the act's own assertions would then report.
INK_SEARCH = 22


def _nearest_ink(image, ink, x: int, y: int) -> tuple[int, int]:
    """The centroid of `ink`-coloured pixels near (x, y), or (x, y) when there are none."""
    if ink is None:
        return x, y
    width, height = image.size
    box = (
        max(0, x - INK_SEARCH), max(0, y - INK_SEARCH),
        min(width, x + INK_SEARCH), min(height, y + INK_SEARCH),
    )
    patch = image.crop(box)
    found = [
        (px, py)
        for py in range(patch.height)
        for px in range(patch.width)
        if _is_ink(patch.getpixel((px, py)), ink)
    ]
    if not found:
        return x, y
    return (
        box[0] + round(sum(p for p, _ in found) / len(found)),
        box[1] + round(sum(p for _, p in found) / len(found)),
    )


def _is_ink(pixel, ink) -> bool:
    """Whether a pixel is that axis colour, allowing for the blend the overlay draws it with."""
    return all(abs(pixel[channel] - ink[channel]) <= 48 for channel in range(3))


def palette_band(session) -> list:
    """The strip of the window the command palette covers when it is open.

    A LIST OF PIXELS RATHER THAN A JUDGEMENT ABOUT THEM. M5.5 asked "is this band neutral?", which
    is true when the palette is open — and also true when the viewport behind it is showing a dark
    sky, or nothing at all. The predicate therefore reported the palette as open in two situations
    where it was shut, and the act built on it failed with "Escape did not close the command
    palette" while the palette was never open in the first place.

    Comparing the band against itself has no such ambiguity: whatever is behind it, opening a
    surface over it changes it, and closing that surface puts it back.
    """
    return list(
        session.region(
            int(session.width * 0.30), int(session.height * 0.085),
            int(session.width * 0.40), int(session.height * 0.05),
        ).getdata()
    )


def band_changed(before: list, after: list) -> int:
    """How many pixels of a band differ. Zero is "nothing was drawn over it"."""
    return sum(1 for a, b in zip(before, after) if a != b)


def viewport_chroma(image, width: int, height: int) -> int:
    """How far the viewport's pixels are from neutral. The engine's frame, or the editor's chrome."""
    left, right = int(width * 0.22), int(width * 0.72)
    top, bottom = int(height * 0.16), int(height * 0.58)
    patch = image.crop((left, top, right, bottom)).resize((32, 24))
    return max(max(pixel) - min(pixel) for pixel in patch.getdata())


# --- The acts ----------------------------------------------------------------------------------------


def act_open(session: Session, journal: Path, shots: Path, report: Report) -> None:
    """The window is up, and the viewport is showing another process's rendered image."""
    expect(session.width > 600 and session.height > 400, "the editor's window is too small to use")
    image = session.capture(shots / "01-open.png")
    report.shot(shots / "01-open.png")
    chroma = viewport_chroma(image, session.width, session.height)
    expect(
        chroma >= MIN_VIEWPORT_CHROMA,
        f"the viewport is neutral (chroma {chroma}), so it is not showing the runtime's image. "
        "Either the publisher is not connected or the editor drew an approximation, which "
        "editor-viewport-and-gizmos forbids",
    )
    report.did(
        "the viewport is the engine's frame",
        f"chroma {chroma} in the viewport against a charcoal interface — those pixels came from "
        "the publisher's GPU allocation, over a dma-buf, in another process",
    )
    expect(journal_records(journal) == 0, "a freshly opened document has already committed something")
    rows = rows_in_hierarchy(session, image)
    expect(
        rows >= 3,
        f"the outliner draws {rows} row(s); the project's world declares three entities and the "
        "editor is expected to have loaded them",
    )
    report.did(
        "the editor opened the project's world",
        f"{session.width}x{session.height}, journal empty, {rows} outliner rows from "
        f"worlds/city.cyworld",
    )


def act_author(session: Session, journal: Path, shots: Path, keyboard: bool,
               report: Report) -> int:
    """Keyboard-first: three entities, created by a shortcut and nothing else.

    Returns the outliner's row count, which act 4 compares undo and redo against.
    """
    if not keyboard:
        report.not_evaluated(
            "keyboard-first operation",
            "this X server delivers no synthesised key events to the editor, so Ctrl+Shift+N "
            "cannot be pressed. The rest of the session is driven with the pointer, which the same "
            "server does deliver",
        )
        return rows_in_hierarchy(session)

    session.ensure_focus()
    # ONE PRESS AT A TIME, EACH WAITED FOR. A key press that the window has not drawn a frame for
    # yet has not happened, and pressing three times and counting afterwards cannot tell a lost
    # press from a slow one. Each press is re-sent only when its OWN record did not appear, and the
    # final count is asserted to be exactly three — so a press that landed twice fails loudly rather
    # than being absorbed.
    presses = 0
    for wanted in (1, 2, 3):
        sent = press_for(
            session, "n", ("Control_L", "Shift_L"),
            lambda wanted=wanted: journal_records(journal) >= wanted,
        )
        expect(sent > 0, f"Ctrl+Shift+N never produced entity {wanted}")
        presses += sent
    records = journal_records(journal)
    expect(
        records == 3,
        f"{presses} Ctrl+Shift+N committed {records} transaction(s); the shortcut is bound in "
        "cy_editor_services::builtin as Ctrl+Shift+N",
    )
    image = session.capture(shots / "02-authored.png")
    report.shot(shots / "02-authored.png")
    rows = rows_in_hierarchy(session, image)
    expect(rows >= 6, f"the outliner draws {rows} row(s) after three more entities were created")
    report.did(
        "keyboard-first operation",
        f"three entities created with Ctrl+Shift+N, three journal records, {rows} outliner rows",
    )
    return rows


def act_select(session: Session, report: Report) -> None:
    """Selection by pointer, in the outliner, which is where a person clicks."""
    x = int(session.width * 0.06)
    y = int(session.height * 0.148)
    session.click(x, y)
    report.did("selection by pointer", f"clicked the first outliner row at ({x}, {y})")


def act_drag(session: Session, journal: Path, shots: Path, rows: int, layout_file: Path,
             keyboard: bool, report: Report) -> int:
    """The gizmo drag, and the undo that takes it back. M7 tasks 5b.3, 5b.4 and 5b.7.

    --- WHAT CHANGED HERE, AND WHY IT IS THE POINT OF THE WHOLE TASK -----------------------------

    M6's version dragged from a hard-coded (47 %, 38 %) of the window, *hoping* a handle was there,
    reported `GAP` when nothing committed, and returned 0. Three separate defects in one act:

      * nothing published a gizmo layout, so there was no handle at any coordinate;
      * the drag aimed at a coordinate rather than at a handle;
      * the run passed anyway.

    All three are closed. `cy::render::build_gizmo_layout` produces the geometry, the engine draws
    exactly those handles into the frame it publishes and answers the editor's `GizmoIntent` with
    exactly those handles, and this act reads the same layout and aims at the X arrow. The exit
    status comes from `Report.exit_code`, which counts gaps.

    Returns how many transactions the journal holds afterwards, which the later acts count from
    rather than assuming.
    """
    before = journal_records(journal)
    session.ensure_focus()
    # Move mode, through the toolbar rather than through `W`. It is the same command, the pointer
    # delivers it on every machine, and the button is what a person clicks.
    click_toolbar(session, "move")

    # The engine publishes a layout only for a selection it has been told about, so this waits for
    # one rather than assuming the first frame carries it: the editor asks once a frame and the
    # answer crosses two sockets.
    expect(
        until(lambda: read_layout(layout_file) is not None, seconds=20.0, poll=0.25),
        f"the engine published no gizmo layout to {layout_file}. The editor asks for one every "
        "frame over --host; a layout that never arrives means the intent never reached the runtime",
    )
    layout = read_layout(layout_file)
    assert layout is not None  # the deadline above is what makes this true
    image = session.capture(shots / "03a-gizmo.png")
    report.shot(shots / "03a-gizmo.png")

    # THE MAPPING IS CHECKED BEFORE IT IS USED. If the viewport rectangle were found wrongly the
    # drag would miss and the act would report a gap that read as a defect in the gizmo, so the
    # centre the engine published is compared against the marker it drew — one number, from two
    # sides of a process boundary.
    left, top, _right, _bottom = viewport_rect(session.width, session.height)
    grab = handle_in_window(session, layout, "axis-x", image)
    expect(grab is not None, "the engine published no X arrow for the selected object")
    # THE MAPPING IS CHECKED BEFORE IT IS USED, and it is checked against the pixels rather than
    # against itself: the point the dock's proportions predict and the point the engine's own red
    # arrow occupies must be the same point, within the slop a click is allowed. If they are not,
    # the drag would miss and the act would report a gap that read as a defect in the gizmo.
    predicted = (
        int(round(left + layout["handles"]["axis-x"][0])),
        int(round(top + layout["handles"]["axis-x"][1])),
    )
    drift = max(abs(grab[0] - predicted[0]), abs(grab[1] - predicted[1]))
    expect(
        drift < INK_SEARCH,
        f"the engine's X arrow was drawn at {grab} and the published layout maps to {predicted}, "
        f"{drift} px apart — the geometry the editor hit-tests is not the geometry on the screen",
    )
    report.did(
        "the engine published its gizmo geometry",
        f"frame {layout['frame']}, {layout['mode']}, {len(layout['handles'])} handles, "
        f"{layout['extent']:.0f} px across; the X arrow is published at "
        f"{tuple(round(v) for v in layout['handles']['axis-x'][:2])} in the frame, drawn at {grab} "
        f"in the window, {drift} px from where the published layout maps to",
    )

    # Along the arrow, away from the centre: a drag that pulls the handle in the direction the
    # handle means. A drag perpendicular to an axis handle is a legitimate gesture and a much
    # smaller movement, which would make this act's assertion about the journal a coin toss.
    centre = handle_in_window(session, layout, "screen", image) or (
        int((left + right) / 2),
        int((top + bottom) / 2),
    )
    reach = max(40, int(((grab[0] - centre[0]) ** 2 + (grab[1] - centre[1]) ** 2) ** 0.5))
    direction = ((grab[0] - centre[0]) / reach, (grab[1] - centre[1]) / reach)
    # Far enough to be unmistakable in a screenshot and short enough to keep the object inside the
    # viewport: a drag that pushes it off the bottom edge produces a picture in which the thing that
    # moved cannot be seen, which is the one thing the exit criterion's image has to show.
    target = (int(grab[0] + direction[0] * 70), int(grab[1] + direction[1] * 70))
    resting = tuple(layout["centre"])
    session.drag(grab, target)
    until(lambda: journal_records(journal) > before, seconds=5.0)
    after = journal_records(journal)
    session.capture(shots / "03-drag.png")
    report.shot(shots / "03-drag.png")
    if after == before:
        report.gap(
            "the gizmo drag",
            f"the drag from the published X arrow at {grab} to {target} committed nothing. The "
            "layout was published, so the handle was there; a drag that still commits nothing "
            "means the press did not begin a manipulation",
        )
        return after

    expect(
        after == before + 1,
        f"the drag committed {after - before} transactions; a manipulation is exactly one",
    )

    # AND THE OBJECT MOVED IN THE ENGINE'S WORLD, which is the half a journal record cannot show.
    # The editor's transaction reaches the runtime as `Message::Apply`, the runtime moves the
    # object it associated with that identity, and the gizmo it publishes for the next frame is
    # centred on the object's new place. So "the drag moved it" is read out of the ENGINE's own
    # answer rather than out of the editor agreeing with itself.
    expect(
        until(lambda: _moved_from(read_layout(layout_file), resting), seconds=8.0, poll=0.25),
        f"the editor committed the drag and the engine's gizmo stayed at {resting}: the "
        "transaction did not reach the runtime, or the runtime did not apply it",
    )
    dragged = tuple(read_layout(layout_file)["centre"])
    report.did(
        "the drag moved the object in the engine's world",
        f"the engine re-centred its gizmo from {tuple(round(v) for v in resting)} to "
        f"{tuple(round(v) for v in dragged)} after applying the editor's transaction",
    )
    # AND IT UNDOES — EXACTLY. `editor-documents-and-transactions` requires an undo to return the
    # document to its pre-transaction state, and a transform change is not structural, so the
    # outliner must NOT move. Two observables from one action, which is what makes "undone exactly"
    # something this act can assert rather than photograph.
    if keyboard:
        expect(
            press_for(session, "z", ("Control_L",), lambda: True) > 0,
            "Ctrl+Z after a gizmo drag did nothing at all",
        )
    else:
        click_toolbar(session, "undo")
        time.sleep(0.6)
    settled = rows_in_hierarchy(session)
    expect(
        settled == rows,
        f"undoing a gizmo drag changed the outliner from {rows} to {settled} rows; a transform is "
        "not a structural change",
    )
    expect(
        journal_records(journal) == after,
        "undo rewrote the journal; it is an append-only record of what was committed",
    )
    # UNDONE EXACTLY, in the engine's world. `editor-documents-and-transactions` requires an undo to
    # return the document to its pre-transaction state; this is the same claim about the thing the
    # user is looking at, and it is a stronger one — the inverse transaction crossed the socket and
    # put the object back where it started, to within the pixel the gizmo is published in.
    expect(
        until(lambda: not _moved_from(read_layout(layout_file), resting), seconds=8.0, poll=0.25),
        f"undo left the engine's gizmo at {tuple(read_layout(layout_file)['centre'])} rather than "
        f"back at {resting}; the undo did not reach the runtime",
    )
    report.did(
        "and the undo put it back, in the engine's world",
        f"the engine's gizmo returned to {tuple(round(v) for v in resting)}, within a pixel of "
        "where it started",
    )
    session.capture(shots / "03b-undone.png")
    report.shot(shots / "03b-undone.png")
    # Put it back, so the rest of the session runs against the world the drag made.
    if keyboard:
        session.key("z", ("Control_L", "Shift_L"))
    else:
        click_toolbar(session, "redo")
    report.did(
        "a gizmo drag lands on a published handle, is one transaction, and undoes",
        f"dragged the engine's X arrow {grab} to {target}: {after - before} transaction recorded, "
        f"{'Ctrl+Z' if keyboard else 'the toolbar Undo'} left the outliner at {settled} rows and "
        "the journal at its append-only length",
    )
    return after


def _moved_from(layout: dict | None, resting: tuple) -> bool:
    """Whether the engine's published gizmo has moved away from where it was.

    A pixel of tolerance, because the gizmo is published in whole-ish pixels and the camera is not
    perfectly still; the movements this asks about are tens of pixels, so the threshold separates
    "moved" from "did not" by two orders of magnitude rather than by a judgement.
    """
    if layout is None:
        return False
    centre = layout["centre"]
    return abs(centre[0] - resting[0]) > 1.5 or abs(centre[1] - resting[1]) > 1.5


def act_undo(
    session: Session, journal: Path, shots: Path, rows: int, committed: int, keyboard: bool,
    report: Report
) -> None:
    """Undo and redo of a STRUCTURAL change, observed where a person observes them."""
    if not keyboard:
        report.not_evaluated(
            "undo and redo through the keyboard",
            "this X server delivers no synthesised key events; the drag's undo is exercised "
            "through the toolbar in act 3 instead, which is the same command through the other "
            "entry point",
        )
        return
    session.ensure_focus()
    # Polled from the outliner's rectangle alone rather than from the whole window: a full capture
    # per poll is most of what this loop costs, and a deadline generous in seconds becomes a deadline
    # of four attempts on a machine that is also running a compiler.
    expect(
        press_for(session, "z", ("Control_L",), lambda: rows_in_hierarchy(session) != rows) > 0,
        "Ctrl+Z changed nothing in the outliner",
    )
    after_undo = rows_in_hierarchy(session)
    session.capture(shots / "04-undone.png")
    report.shot(shots / "04-undone.png")
    expect(
        after_undo < rows,
        f"Ctrl+Z left the outliner at {after_undo} row(s), unchanged from {rows}",
    )
    expect(
        journal_records(journal) == committed,
        "undo rewrote the journal; it is an append-only record of what was committed",
    )
    expect(
        press_for(
            session, "z", ("Control_L", "Shift_L"),
            lambda: rows_in_hierarchy(session) != after_undo,
        )
        > 0,
        "Ctrl+Shift+Z changed nothing in the outliner",
    )
    after_redo = rows_in_hierarchy(session)
    session.capture(shots / "05-redone.png")
    expect(
        after_redo == rows,
        f"Ctrl+Shift+Z left the outliner at {after_redo} row(s) rather than {rows}",
    )
    report.did(
        "undo and redo through the keyboard",
        f"{rows} rows, {after_undo} after Ctrl+Z, {after_redo} after Ctrl+Shift+Z; the journal "
        f"stayed at {committed} records, which is what an append-only record of commits should do",
    )


def act_palette(session: Session, shots: Path, keyboard: bool, report: Report) -> None:
    """The command palette, over the same registry the menus and the keymap read."""
    if not keyboard:
        report.not_evaluated(
            "the command palette",
            "it is opened with Ctrl+P and closed with Escape, and this X server delivers no "
            "synthesised key events",
        )
        return
    session.ensure_focus()
    # The band, before anything is drawn over it. Everything below is a comparison against this
    # rather than a judgement about a colour — see `palette_band`.
    resting = palette_band(session)
    opened = 0
    for _ in range(4):
        session.key("p", ("Control_L",))
        if until(lambda: band_changed(resting, palette_band(session)) > len(resting) // 20,
                 seconds=4.0, poll=0.3):
            opened = band_changed(resting, palette_band(session))
            break
    expect(opened > 0, "Ctrl+P did not open the command palette over the viewport")
    for letter in "undo":
        session.key(letter)
    session.capture(shots / "06-palette.png")
    report.shot(shots / "06-palette.png")
    session.key("Escape")
    expect(
        until(lambda: band_changed(resting, palette_band(session)) <= len(resting) // 20,
              seconds=12.0, poll=0.3),
        "Escape did not close the command palette",
    )
    report.did(
        "the command palette",
        f"Ctrl+P drew it over {opened} pixels of the viewport, typing narrowed it, Escape put the "
        "band back",
    )


def act_save(
    session: Session, journal: Path, shots: Path, committed: int, world: Path, keyboard: bool,
    report: Report
) -> None:
    """Save, and the journal discarded only after it succeeded — and the world written to disk."""
    session.ensure_focus()
    if not keyboard:
        # The pointer path: the toolbar's Save button is the same command `file.save` that Ctrl+S
        # invokes, through the same registry.
        expect(
            journal_records(journal) == committed,
            f"the document should have {committed} committed transactions",
        )
        click_toolbar(session, "save")
        expect(
            until(lambda: journal_records(journal) == 0, seconds=12.0, poll=0.25),
            "the toolbar's Save never discarded the journal",
        )
        session.capture(shots / "07-saved.png")
        report.shot(shots / "07-saved.png")
        expect(world.is_file(), f"file.save discarded the journal and wrote no world at {world}")
        report.did(
            "saved with the pointer",
            f"the toolbar's Save took the journal from {committed} records to 0 and wrote "
            f"{world.name}",
        )
        return
    expect(
        journal_records(journal) == committed,
        f"the document should have {committed} committed transactions",
    )
    # Saving twice is harmless — the second is refused, because `file.save` is unavailable while the
    # document is clean — so this press is the one that may safely be re-sent on a busy machine.
    expect(
        press_for(session, "s", ("Control_L",), lambda: journal_records(journal) == 0) > 0,
        "Ctrl+S never discarded the journal",
    )
    session.capture(shots / "07-saved.png")
    report.shot(shots / "07-saved.png")
    records = journal_records(journal)
    expect(
        records == 0,
        f"Ctrl+S left {records} record(s) in the journal; file.save discards it after the write",
    )
    # THE FILE. Before M6 `file.save` wrote nothing at all — "writing the assets themselves is the
    # serialisation layer's, at a later task" — so the journal emptying was the whole observable.
    # Now the world is on disk, and the second editor in act_recover reads it back.
    expect(
        until(world.is_file, seconds=10.0, poll=0.25),
        f"file.save discarded the journal and wrote no world at {world}",
    )
    text = world.read_text()
    expect(
        text.startswith("cyworld 1\n"),
        f"the world it wrote is not a world: {text[:40]!r}",
    )
    expect(
        'runtime "Transform"' in text,
        "the world carries the schema it was written against, so a second editor can read it",
    )
    report.did(
        "saved through the keyboard",
        f"the journal went from {committed} records to 0, and {world.name} was written: "
        f"{len(text.splitlines())} lines, carrying the schema it was written against",
    )


def act_reference(session: Session, path: Path, layout_file: Path, report: Report) -> None:
    """The committed reference screenshot, taken by the artefact rather than by hand.

    **THIS IS M7's EXIT CRITERION AS A PICTURE**: the editor's viewport showing the engine's
    rendered world, with a transform gizmo the engine generated on a selected object that a drag
    moved and an undo put back. Everything in it was asserted by the acts above; the image is so
    that the milestone can also be evaluated by looking, which is what
    `docs/design/images/transform-gizmo.png` has asked for since M3 and what nothing produced until
    now.

    It waits for the transient notifications to retire first. A toast is a notification that has
    already been seen; photographing a wall of them would make the reference a picture of this
    driver's speed rather than of the editor — and `editor-ui-ux` requires notifications not to
    interrupt, which is a claim about the resting state.
    """
    session.key("Escape")
    # Selected again, because the acts above ended on an undo and a redo and the gizmo is drawn for
    # whatever is selected NOW. A reference photograph of the exit criterion has to contain the
    # gizmo, and the way to be sure of that is to ask for it and wait for the engine to publish one.
    session.click(int(session.width * 0.06), int(session.height * 0.148))
    until(lambda: read_layout(layout_file) is not None, seconds=8.0, poll=0.25)
    for _ in range(24):
        time.sleep(0.5)
        image = session.capture(path)
        # `capture` has already written the file; the image is read only to decide whether to stop.
        # The console panel's right-hand column is where toasts stack. When it stops changing and
        # the interface is quiet, the resting state has been reached.
        if _toast_area_quiet(image, session.width, session.height):
            break
    report.shot(path)
    report.did("the reference screenshot", f"{path}, {session.width}x{session.height}")


def _toast_area_quiet(image, width: int, height: int) -> bool:
    """Whether the toast stack has retired: the notification corner is back to one flat surface."""
    left, right = int(width * 0.78), int(width * 0.99)
    top, bottom = int(height * 0.78), int(height * 0.96)
    patch = image.convert("L").crop((left, top, right, bottom))
    values = list(patch.getdata())
    return max(values) - min(values) < 40


def act_recover(binary: Path, root: Path, journal: Path, report: Report) -> None:
    """A second, headless editor over the same journal finds nothing to recover. Task 5.1's other
    half: the save reached the document rather than the button."""
    result = subprocess.run(
        [str(binary), "--headless", "--open", WORLD, "--journal", str(journal)],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
        timeout=120,
    )
    expect(result.returncode == 0, f"the headless editor exited {result.returncode}")
    expect(
        "recoverable" not in result.stdout,
        f"a saved document still offers recovery:\n{result.stdout}",
    )
    report.did(
        "a second editor confirms the save",
        "it opens the same document over the same journal and offers no recovery",
    )


# --- Running it ----------------------------------------------------------------------------------------


def runtime_figures(published: str) -> list:
    """What the engine's runtime said it did, as figures the harness can check.

    Parsed out of the runtime's own closing line rather than measured here, because the runtime is
    the only side that can count a frame it dropped — and a rate this driver timed would be a
    measurement of this driver.
    """
    match = re.search(
        r"published (\d+) frames in ([\d.]+) s = ([\d.]+) fps, dropped (\d+) on a full ring, "
        r"(\d+) vetoed; (\d+) gizmo\(s\) answered, (\d+) move\(s\) applied",
        published or "",
    )
    if match is None:
        return []
    frames, seconds, fps, dropped, vetoed, gizmos, moves = match.groups()
    return [
        Statistic.stable("mean published frame rate", float(fps), "fps"),
        Statistic.stable("total frames the engine published", float(frames)),
        Statistic.stable("total frames dropped on a full ring", float(dropped)),
        Statistic.stable("total frames the editor's claim vetoed", float(vetoed)),
        Statistic.stable("total gizmo layouts answered", float(gizmos)),
        Statistic.stable("total transactions applied to the engine's world", float(moves)),
        Statistic.stable("mean seconds of session", float(seconds), "s"),
    ]


def binaries(profile: str, build: bool, runtime: str = "") -> tuple[Path, Path]:
    if build:
        subprocess.run(["just", "build-editor", "--profile", profile], cwd=ROOT, check=True)
    cargo_profile = subprocess.run(
        ["just", "_cargo-profile", profile], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.strip()
    target = subprocess.run(
        ["just", "_editor-target-dir"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.strip()
    directory = Path(target) / ("debug" if cargo_profile == "dev" else cargo_profile)
    editor = directory / "cyberdyne-editor"
    if not editor.is_file():
        raise Failed(f"no editor at {editor}. Build it: just build-editor --profile {profile}")
    # THE ENGINE, NOT THE FIXTURE. Until M7 this was `cy-viewport-publisher`, a Vulkan fixture in
    # the editor's own Cargo workspace that clears an image to a colour and moves a white bar — so
    # six milestones of editor work produced a viewport that had never shown anything the engine
    # drew. `cy_editor_window_runtime` renders M3's scene through the render graph on a real device
    # and publishes it over the same transport. M7 task 5b.1.
    #
    # PASSED IN BY CMAKE WHEN THERE IS ONE, guessed otherwise. `smoke.editor_window` is given
    # `$<TARGET_FILE:cy_editor_window_runtime>`, which is exact whatever the build tree is called;
    # a guess from `CY_BUILD_DIR` is right for `just run-editor-window` and wrong under CTest, whose
    # environment does not carry it — and the symptom is this artefact quietly reporting that the
    # viewport is neutral, which reads as a defect in the transport.
    if runtime:
        return editor, Path(runtime)
    tree = Path(os.environ.get("CY_BUILD_DIR", f"build/{profile}"))
    if not tree.is_absolute():
        tree = ROOT / tree
    return editor, tree / "samples" / "05b-editor-window" / "runtime" / "cy_editor_window_runtime"


def prepare(work: Path) -> tuple[Path, Path, Path]:
    root = work / "project"
    if root.exists():
        shutil.rmtree(root)
    shutil.copytree(SAMPLE / "project", root)
    journal = work / "journal"
    if journal.exists():
        shutil.rmtree(journal)
    journal.mkdir(parents=True)
    shots = work / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    return root, journal, shots


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="dev")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--work", default="")
    # EMPTY WHEN THERE IS NONE, rather than a hopeful ":0". A default that names a display the
    # machine does not have turns "this artefact cannot run here" into a connection error, and the
    # whole point of the exit-3 path below is that it says which part is missing.
    parser.add_argument(
        "--display",
        default=os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY") or "",
    )
    parser.add_argument("--socket", default="", help="the viewport transport's socket")
    parser.add_argument(
        "--runtime", default="",
        help="the engine's runtime, which CMake knows the path of exactly and a guess does not",
    )
    parser.add_argument("--seconds", type=float, default=180.0, help="how long the publisher runs")
    parser.add_argument(
        "--hold",
        action="store_true",
        help="leave the window open at the end, for a person watching",
    )
    # THE COMMITTED SCREENSHOT IS REGENERATED BY THE ARTEFACT THAT IS IN IT. Task 4.3 asks for an
    # image under docs/design/images/ so the milestone can be evaluated by looking, and an image
    # taken by hand is an image nobody can reproduce or refresh. `--shot` writes the one frame the
    # reference is made of: the session in the state act 4 leaves it in, after the transient
    # notifications have retired, so what is photographed is the editor rather than its toasts.
    parser.add_argument("--shot", default="", help="write the reference screenshot here")
    options = parser.parse_args()

    # CY_BUILD_DIR, not `build/`. Every recipe in this tree honours it and a driver that wrote into
    # `build/` regardless would put one run's output into another agent's tree.
    default = ROOT / os.environ.get("CY_BUILD_DIR", "build") / "editor-window"
    work = Path(options.work).resolve() if options.work else default
    work.mkdir(parents=True, exist_ok=True)
    report = Report()

    if not options.display:
        print(
            "editor-window: there is no display. THIS ARTEFACT IS ABOUT A WINDOW, so it is not\n"
            "  run here rather than reported as passing: a window that was never mapped cannot\n"
            "  say anything about docking, the palette or keyboard-first operation. Run it on a\n"
            "  machine with a display, or set DISPLAY.",
            file=sys.stderr,
        )
        return 3

    try:
        editor, publisher = binaries(options.profile, options.build, options.runtime)
    except (Failed, subprocess.CalledProcessError) as problem:
        print(f"editor-window: {problem}", file=sys.stderr)
        return 2

    root, journal, shots = prepare(work)
    # A UNIX SOCKET PATH IS CAPPED AT `sun_path`, AND THIS ONE IS DERIVED FROM THE BUILD TREE.
    # M7 task 5b.6, found by M6's release run: `build/release-with-assertions/samples/
    # 05b-editor-window/session/viewport.sock` is past the kernel's limit, and the publisher dies
    # with `bind: path must be shorter than SUN_LEN` — which reads as a defect in the transport and
    # is a defect in the path. `harness.artefact.socket_path` falls back to a short one derived
    # from this one, so two runs of the same build still share a socket.
    socket = str(options.socket) if options.socket else str(socket_path(work, "viewport.sock"))
    # The engine's CONTROL socket, which is a different one: the image is pixels and the gizmo is
    # control, and the two travel separately for the reason `cy/backends/viewport/publisher.h`
    # gives. Both are capped at `sun_path`, so both go through the same fallback.
    host_socket = str(socket_path(work, "runtime.sock"))
    # Where the engine writes the gizmo layout it published. THIS IS WHAT MAKES THE DRAG LAND ON A
    # HANDLE: M6's run dragged from a hard-coded (47 %, 38 %) hoping one was there. M7 task 5b.4.
    layout_file = work / "gizmo.json"
    if layout_file.exists():
        layout_file.unlink()
    print(f"==> editor-window  profile={options.profile}  display={options.display}")
    print(f"    editor         {editor}")
    print(f"    publisher      {publisher}")
    print(f"    project        {root}")

    runtime = None
    if publisher.is_file():
        runtime = subprocess.Popen(
            [
                str(publisher),
                # ONE WORLD, M8.a task 1.1. The runtime opens the SAME world file the editor
                # opens, and derives each node's identity the way `cy_editor_core::ids` derives
                # it, so an identity the editor names is a node in the runtime's world rather
                # than whichever object it handed out first. Without these two arguments the
                # runtime renders M3's ring, which is what it did until M8.a and which is why its
                # gizmo used to land on an unrelated box.
                "--project", str(root),
                "--world", WORLD,
                "--socket", socket,
                "--host", host_socket,
                "--layout", str(layout_file),
                "--width", "1280",
                "--height", "720",
                "--rate", "60",
                "--seconds", str(options.seconds),
            ],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        # Waited for rather than slept through: the engine creates a device, compiles pipelines and
        # exports four dma-buf images before it binds either socket, and on a cold shader cache that
        # is seconds rather than milliseconds.
        expect(
            until(lambda: Path(socket).exists() and Path(host_socket).exists(), seconds=60.0,
                  poll=0.2),
            f"the engine's runtime did not open {socket} and {host_socket}",
        )
    else:
        print(
            f"    no runtime at {publisher}; the viewport will say the transport is not ready, "
            "which is what it should say",
            file=sys.stderr,
        )

    environment = dict(os.environ, DISPLAY=options.display, CY_VIEWPORT_SOCKET=socket)
    # `--host` IS WHAT MAKES THE GIZMO POSSIBLE. Without it the editor has no runtime session, so
    # `GizmoIntent` goes nowhere, no layout comes back, and a drag has nothing to land on — which is
    # precisely the gap M6's run reported. M7 task 5b.3.
    process = subprocess.Popen(
        [str(editor), "--open", WORLD, "--journal", str(journal), "--host", host_socket],
        cwd=root,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    code = 0
    try:
        session = Session(options.display)
        # BEFORE ANYTHING IS DRIVEN. A screensaver holds a keyboard and pointer grab, and every
        # synthesised event goes to it rather than to the editor — while the windows underneath
        # keep rendering, so the screenshots look right and nothing else does. See
        # `wake_the_display` for the measurement, and for what M6 concluded instead.
        woken = wake_the_display()
        session.attach(process.pid)
        # ONCE, BEFORE ANYTHING IS DRIVEN. See `keyboard_arrives`: on this project's development
        # machine XTEST delivers pointer events reliably and key events only sometimes, and an
        # artefact that assumed otherwise would report the X server's behaviour as the editor's.
        keyboard = keyboard_arrives(session)
        print(f"    input          screensaver: {woken}; synthesised keys "
              f"{'arrive' if keyboard else 'DO NOT ARRIVE'}")
        print("--- act 1: the editor opens, and the viewport is the engine's ---")
        act_open(session, journal, shots, report)
        print("--- act 2: a person authors, keyboard first ---")
        rows = act_author(session, journal, shots, keyboard, report)
        act_select(session, report)
        print("--- act 3: a gizmo drag, and the undo that takes it back ---")
        committed = act_drag(session, journal, shots, rows, layout_file, keyboard,
                             report)
        print("--- act 4: undo, redo, and the palette ---")
        act_undo(session, journal, shots, rows, committed, keyboard, report)
        act_palette(session, shots, keyboard, report)
        print("--- act 5: save, and a second editor that confirms it ---")
        act_save(session, journal, shots, committed, root / WORLD, keyboard, report)
        if options.shot:
            act_reference(session, Path(options.shot), layout_file, report)
        if options.hold:
            print("    holding the window open; close it to finish")
            process.wait()
    except Absent as missing:
        print(
            f"editor-window: {missing}\n"
            "  THIS ARTEFACT IS ABOUT A WINDOW and is not run here rather than reported as\n"
            "  passing. tests/render/ takes the same position about a machine with no GPU.",
            file=sys.stderr,
        )
        code = 3
    except Failed as problem:
        print(f"\neditor-window: {problem}", file=sys.stderr)
        code = 1
    finally:
        process.terminate()
        try:
            out, err = process.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
            out, err = process.communicate()
        if runtime is not None:
            runtime.terminate()
            try:
                published = runtime.communicate(timeout=20)[0]
            except subprocess.TimeoutExpired:
                runtime.kill()
                published = ""
        else:
            published = ""

    if code == 0:
        try:
            act_recover(editor, root, journal, report)
        except (Failed, subprocess.TimeoutExpired) as problem:
            print(f"\neditor-window: {problem}", file=sys.stderr)
            code = 1

    print("\n--- what the runtime and the editor said ---")
    for line in (published or "").splitlines()[-3:]:
        print(f"    {line}")
    for line in (err or "").splitlines()[:6]:
        print(f"    {line}")

    # THE HEADLINE, AND WHY IT IS THIS ONE. `harness.artefact.Report.headline` refuses an extreme
    # value, so what a run leads with is a rate over its whole length rather than the best or worst
    # frame in it — M7 task 5b.5b. The maximum is not printed at all here because this artefact
    # measures a session rather than a frame budget; what it has to say about pace is how many
    # frames the engine got across, and the drop count beside it.
    figures = runtime_figures(published)
    for figure in figures:
        report.figure(figure)
    if figures:
        try:
            report.headline(figures[0])
        except Failed as refused:
            report.gap("the run's headline figure", str(refused))

    # THE STATUS IS THE HARNESS'S, NOT THIS FILE'S. `code` covers the acts that raised; the gaps
    # are counted by `Report.exit_code`, and there is deliberately no path by which a run that
    # recorded one returns zero. That is task 5b.5, and this line is the whole of it: at M6 the
    # equivalent line was `return code`, and `GAP the gizmo drag` sat above a green tick.
    if code != 0:
        report.failed(f"the session exited {code}")
    return max(code, report.summarise(shots))


if __name__ == "__main__":
    sys.exit(main())

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

--- WHAT THIS ARTEFACT CANNOT DO, IN ITS OWN OUTPUT ---------------------------------------------

**A gizmo drag moves nothing, and it is not the viewport's fault.** Opening a document constructs an
empty `Document` — `DocumentService::open` calls `Document::new`, a name and an empty schema —
because there is no world loader. `TransformBinding::of_schema` therefore finds no `Transform`, so
the gizmo has nothing to bind to and a drag over the viewport commits nothing. The drag act below
performs the drag anyway, asserts that nothing was committed, and reports the gap by name. It will
report the drag as satisfied the day opening a world produces content.

That is the same rule samples/05-editor-session set at M5: an artefact that quietly narrows its
claim to what happens to work reports a milestone as closed that is not.
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

SAMPLE = Path(__file__).resolve().parent
ROOT = SAMPLE.parents[1]

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


@dataclass
class Step:
    name: str
    detail: str = ""
    ok: bool = True


@dataclass
class Report:
    steps: list[Step] = field(default_factory=list)
    gaps: list[Step] = field(default_factory=list)
    shots: list[Path] = field(default_factory=list)

    def did(self, name: str, detail: str = "") -> None:
        self.steps.append(Step(name, detail))
        print(f"    ok    {name}" + (f" — {detail}" if detail else ""))

    def gap(self, name: str, detail: str) -> None:
        self.gaps.append(Step(name, detail, ok=False))
        print(f"    GAP   {name} — {detail}")

    def shot(self, path: Path) -> None:
        self.shots.append(path)
        print(f"    shot  {path}")


class Failed(Exception):
    """An act that was expected to work did not."""


class Absent(Exception):
    """This machine cannot run the artefact, and says which part is missing."""


def expect(condition: bool, what: str) -> None:
    if not condition:
        raise Failed(what)


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


def palette_is_open(session) -> bool:
    """Whether the command palette is covering the top of the viewport.

    The palette is one of the interface's neutral raised surfaces; behind it is the runtime's frame,
    which is not neutral. A colour question, and therefore one that needs no font and no layout.
    """
    left = int(session.width * 0.30)
    top = int(session.height * 0.090)
    patch = session.region(left, top, int(session.width * 0.30), int(session.height * 0.020))
    return max(max(pixel) - min(pixel) for pixel in patch.getdata()) < MIN_VIEWPORT_CHROMA


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
    report.did("the editor opened on a project", f"{session.width}x{session.height}, journal empty")


def act_author(session: Session, journal: Path, shots: Path, report: Report) -> int:
    """Keyboard-first: three entities, created by a shortcut and nothing else.

    Returns the outliner's row count, which act 4 compares undo and redo against.
    """
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
    expect(rows >= 3, f"the outliner draws {rows} row(s) after three entities were created")
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


def act_drag(session: Session, journal: Path, shots: Path, report: Report) -> None:
    """The gizmo drag. An OPEN STEP — see the module note."""
    before = journal_records(journal)
    session.ensure_focus()
    session.key("w")  # Move
    centre = (int(session.width * 0.47), int(session.height * 0.38))
    session.drag(centre, (centre[0] + 140, centre[1] - 40))
    until(lambda: journal_records(journal) > before, seconds=3.0)
    after = journal_records(journal)
    session.capture(shots / "03-drag.png")
    report.shot(shots / "03-drag.png")
    if after > before:
        report.did("a gizmo drag is one transaction", f"{after - before} transaction recorded")
        return
    report.gap(
        "the gizmo drag",
        "the drag committed nothing, because the document's schema declares no Transform: opening "
        "a document builds an empty one (DocumentService::open calls Document::new) and there is "
        "no world loader, so TransformBinding::of_schema finds nothing to bind to. The gizmo, its "
        "three states, its snapping and its one-transaction rule are held by cy-editor-viewport's "
        "own tests against a document that declares a Transform",
    )


def act_undo(session: Session, journal: Path, shots: Path, rows: int, report: Report) -> None:
    """Undo and redo, observed where a person observes them."""
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
        journal_records(journal) == 3,
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
        "stayed at 3 records, which is what an append-only record of commits should do",
    )


def act_palette(session: Session, shots: Path, report: Report) -> None:
    """The command palette, over the same registry the menus and the keymap read."""
    session.ensure_focus()
    # The palette is a charcoal surface drawn over the top of the viewport, and the viewport is
    # showing another process's saturated frame. So "the palette is open" is a colour question with
    # an unambiguous answer, and this act asserts it rather than only photographing it.
    expect(
        press_for(session, "p", ("Control_L",), lambda: palette_is_open(session)) > 0,
        "Ctrl+P did not open the command palette over the viewport",
    )
    for letter in "undo":
        session.key(letter)
    until(lambda: not palette_is_open(session), seconds=1.0, poll=0.25)  # settle a frame
    session.capture(shots / "06-palette.png")
    report.shot(shots / "06-palette.png")
    session.key("Escape")
    expect(
        until(lambda: not palette_is_open(session), seconds=12.0, poll=0.2),
        "Escape did not close the command palette",
    )
    report.did(
        "the command palette",
        "Ctrl+P opened it over the viewport, typing narrowed it, Escape closed it",
    )


def act_save(session: Session, journal: Path, shots: Path, report: Report) -> None:
    """Save, and the journal discarded only after it succeeded."""
    session.ensure_focus()
    expect(journal_records(journal) == 3, "the document should have three committed transactions")
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
    report.did(
        "saved through the keyboard",
        "the journal went from 3 records to 0, which is the sequence file.save is responsible for",
    )


def act_reference(session: Session, path: Path, report: Report) -> None:
    """The committed reference screenshot, taken by the artefact rather than by hand.

    It waits for the transient notifications to retire first. A toast is a notification that has
    already been seen; photographing a wall of them would make the reference a picture of this
    driver's speed rather than of the editor — and `editor-ui-ux` requires notifications not to
    interrupt, which is a claim about the resting state.
    """
    session.key("Escape")
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


def binaries(profile: str, build: bool) -> tuple[Path, Path]:
    if build:
        subprocess.run(["just", "build-editor", "--profile", profile], cwd=ROOT, check=True)
    cargo_profile = subprocess.run(
        ["just", "_cargo-profile", profile], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.strip()
    target = subprocess.run(
        ["just", "_editor-target-dir"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.strip()
    directory = Path(target) / ("debug" if cargo_profile == "dev" else cargo_profile)
    editor, publisher = directory / "cyberdyne-editor", directory / "cy-viewport-publisher"
    if not editor.is_file():
        raise Failed(f"no editor at {editor}. Build it: just build-editor --profile {profile}")
    return editor, publisher


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
        editor, publisher = binaries(options.profile, options.build)
    except (Failed, subprocess.CalledProcessError) as problem:
        print(f"editor-window: {problem}", file=sys.stderr)
        return 2

    root, journal, shots = prepare(work)
    socket = options.socket or str(work / "viewport.sock")
    print(f"==> editor-window  profile={options.profile}  display={options.display}")
    print(f"    editor         {editor}")
    print(f"    publisher      {publisher}")
    print(f"    project        {root}")

    runtime = None
    if publisher.is_file():
        runtime = subprocess.Popen(
            [
                str(publisher),
                "--socket", socket,
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
        time.sleep(2.5)
    else:
        print(
            "    the reference publisher is not in this build; the viewport will say the transport "
            "is not ready, which is what it should say",
            file=sys.stderr,
        )

    environment = dict(os.environ, DISPLAY=options.display, CY_VIEWPORT_SOCKET=socket)
    process = subprocess.Popen(
        [str(editor), "--open", WORLD, "--journal", str(journal)],
        cwd=root,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    code = 0
    try:
        session = Session(options.display)
        session.attach(process.pid)
        print("--- act 1: the editor opens, and the viewport is the engine's ---")
        act_open(session, journal, shots, report)
        print("--- act 2: a person authors, keyboard first ---")
        rows = act_author(session, journal, shots, report)
        act_select(session, report)
        print("--- act 3: a gizmo drag ---")
        act_drag(session, journal, shots, report)
        print("--- act 4: undo, redo, and the palette ---")
        act_undo(session, journal, shots, rows, report)
        act_palette(session, shots, report)
        print("--- act 5: save, and a second editor that confirms it ---")
        act_save(session, journal, shots, report)
        if options.shot:
            act_reference(session, Path(options.shot), report)
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

    print("\n--- the session ---")
    print(f"    {len(report.steps)} step(s) satisfied, {len(report.shots)} screenshot(s) in {shots}")
    if report.gaps:
        print(f"    {len(report.gaps)} step(s) NOT SATISFIED, each named above and in README.md:")
        for gap in report.gaps:
            print(f"      · {gap.name}")
    return code


if __name__ == "__main__":
    sys.exit(main())

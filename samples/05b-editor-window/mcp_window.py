#!/usr/bin/env python3
"""samples/05b-editor-window — the viewport proved through MCP, without touching the keyboard.

`smoke.editor_window_mcp` is the CTest entry; this file is what it runs.

`window.py` drives the editor with synthesised X11 input, which is the only way to prove keyboard
and pointer operation — and which takes the machine's input for the length of the run, so a person
using the same desktop breaks it (issue #18's verification lost four of twelve runs to a Caps Lock
pressed elsewhere). This driver asks the questions that need no input through the editor's own MCP
interface instead: it reads `editor:window?panel=viewport`, the window as the editor presented it,
and moves an entity with the registered commands. It imports no X11 or XTEST library, so it cannot
send input even by mistake.

Four acts, each able to fail:

  0. With no runtime, the viewport shows the editor's own sunken fill. The control: without it,
     act 1's "exact engine black" could not be told apart from the editor drawing nothing.
  1. An EMPTY world. PR #14 draws no stand-in geometry, so the engine's frame is black — and the
     viewport must show exactly that black, not the editor's fill.
  2. A DRAWABLE world (`worlds/city-blocks.cyworld`). The viewport carries colour, `scene.translate`
     changes it, and `edit.undo` puts it back.
  3. The whole window: the capture is the window's size and holds the hierarchy panel too.
"""

from __future__ import annotations

import argparse
import base64
import io
import json
import os
import subprocess
import sys
import time
from pathlib import Path

SAMPLE = Path(__file__).resolve().parent
ROOT = SAMPLE.parents[1]
sys.path.insert(0, str(ROOT / "samples" / "harness"))
sys.path.insert(0, str(SAMPLE))
from artefact import Failed, Report, expect, socket_path  # noqa: E402
from window import MIN_VIEWPORT_CHROMA, binaries, prepare, until  # noqa: E402

DRAWABLE = "worlds/city-blocks.cyworld"
EMPTY = "worlds/empty.cyworld"

#: What the editor fills the viewport with when no engine frame is shown: `Surface::Sunken` in the
#: dark theme, `cy_editor_visual::colour`.
SUNKEN = (6, 7, 8)
#: What the engine's authored frame shows where nothing is drawn, measured from its published frame.
ENGINE_EMPTY = (0, 0, 0)
#: The share of the viewport's centre that must be one exact colour to call it that colour.
UNIFORM = 0.9
#: Mean absolute channel difference that counts as "the picture changed", and as "it came back".
CHANGED = 2.0
RESTORED = 0.5


class Mcp:
    """A JSON-RPC client over the editor's standard input and output."""

    def __init__(self, process: subprocess.Popen) -> None:
        self.process = process
        self.next_id = 0

    def call(self, method: str, params: dict | None = None, seconds: float = 30.0) -> dict:
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            message["params"] = params
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            line = self.process.stdout.readline()
            if not line:
                raise Failed(f"the editor closed its MCP stream during {method}")
            try:
                reply = json.loads(line)
            except json.JSONDecodeError:
                continue  # the editor's own log lines share the stream's terminal, not its protocol
            if reply.get("id") == self.next_id:
                if "error" in reply:
                    raise Failed(f"{method} failed: {reply['error']}")
                return reply["result"]
        raise Failed(f"{method} had no answer within {seconds:.0f} s")

    def initialise(self) -> None:
        self.call("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "smoke.editor_window_mcp", "version": "1"},
        })
        self.process.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        self.process.stdin.flush()

    def text(self, uri: str) -> str:
        result = self.call("resources/read", {"uri": uri})
        return result["contents"][0]["text"]

    def tool(self, name: str, arguments: dict) -> str:
        result = self.call("tools/call", {"name": name, "arguments": arguments})
        text = "\n".join(item.get("text", "") for item in result.get("content", []))
        expect(not result.get("isError"), f"{name} was refused: {text}")
        return text

    def capture(self, uri: str):
        """Read a window address and decode it. Retries a budget refusal, which is a wait."""
        from PIL import Image

        for _ in range(20):
            result = self.call("resources/read", {"uri": uri})
            if "contents" in result:
                entry = result["contents"][0]
                expect(entry.get("mimeType") == "image/png", f"{uri} answered {entry}")
                image = Image.open(io.BytesIO(base64.b64decode(entry["blob"]))).convert("RGB")
                return image, result["contents"][1]["text"]
            text = result["content"][0]["text"]
            expect("render" in text, f"{uri} was refused: {text}")
            time.sleep(0.5)
        raise Failed(f"{uri} stayed over this connection's render budget")


def centre(image):
    """The middle of the viewport panel, clear of its toolbar, stats and orientation gizmo."""
    width, height = image.size
    return image.crop((int(width * 0.2), int(height * 0.25), int(width * 0.8), int(height * 0.75)))


def share_of(image, colour) -> float:
    pixels = list(image.getdata())
    return sum(1 for pixel in pixels if pixel == colour) / len(pixels)


def chroma(image) -> int:
    return max(max(pixel) - min(pixel) for pixel in image.resize((32, 24)).getdata())


def difference(a, b) -> float:
    if a.size != b.size:
        return 255.0
    left, right = list(a.getdata()), list(b.getdata())
    total = sum(abs(x - y) for p, q in zip(left, right) for x, y in zip(p, q))
    return total / (len(left) * 3)


class Session:
    """One runtime (or none) and one editor with `--mcp`, on the prepared project copy."""

    def __init__(self, editor: Path, runtime: Path | None, root: Path, work: Path, world: str,
                 display: str) -> None:
        self.runtime = None
        viewport = str(socket_path(work, "mcp-viewport.sock"))
        host = str(socket_path(work, "mcp-runtime.sock"))
        for path in (viewport, host):
            Path(path).unlink(missing_ok=True)
        if runtime is not None:
            self.log = open(work / f"runtime-{Path(world).stem}.log", "w")  # noqa: SIM115
            self.runtime = subprocess.Popen(
                [str(runtime), "--project", str(root), "--world", world, "--socket", viewport,
                 "--host", host, "--width", "1280", "--height", "720", "--rate", "60",
                 "--seconds", "600", "--no-validation"],
                cwd=ROOT, stdout=self.log, stderr=subprocess.STDOUT, text=True,
            )
            expect(
                until(lambda: Path(viewport).exists() and Path(host).exists(), seconds=60.0,
                      poll=0.2),
                f"the runtime did not open {viewport} and {host}",
            )
        arguments = [str(editor), "--open", world, "--mcp", "--agent-scope", "author"]
        if runtime is not None:
            arguments += ["--host", host]
        self.editor = subprocess.Popen(
            arguments, cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True,
            env=dict(os.environ, DISPLAY=display, CY_VIEWPORT_SOCKET=viewport),
        )
        self.mcp = Mcp(self.editor)
        self.mcp.initialise()

    def viewport(self):
        image, described = self.mcp.capture("editor:window?panel=viewport")
        return centre(image), described

    def close(self) -> None:
        for process in (self.editor, self.runtime):
            if process is None:
                continue
            process.terminate()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if self.runtime is not None:
            self.log.close()


def settle(read, predicate, seconds: float = 30.0):
    """Read until `predicate` holds, returning the last reading either way."""
    deadline = time.monotonic() + seconds
    reading = read()
    while not predicate(reading) and time.monotonic() < deadline:
        time.sleep(0.4)
        reading = read()
    return reading


def steady(session: Session, seconds: float = 15.0):
    """The viewport once two consecutive captures agree."""
    previous, _ = session.viewport()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        time.sleep(0.4)
        current, _ = session.viewport()
        if difference(previous, current) < RESTORED / 5:
            return current
        previous = current
    raise Failed("the viewport never settled after the selection")


def act_control(session: Session, shots: Path, report: Report) -> None:
    image, _ = settle(session.viewport, lambda r: share_of(r[0], SUNKEN) >= UNIFORM, 15.0)
    image.save(shots / "mcp-0-no-runtime.png")
    share = share_of(image, SUNKEN)
    expect(
        share >= UNIFORM,
        f"with no runtime the viewport's centre is {share:.0%} the editor's sunken fill "
        f"{SUNKEN}; the control that makes act 1 meaningful does not hold",
    )
    report.did("control: no runtime shows the editor's own fill",
               f"{share:.0%} of the viewport centre is {SUNKEN}")


def act_empty(session: Session, shots: Path, report: Report) -> None:
    image, described = settle(session.viewport,
                              lambda r: share_of(r[0], ENGINE_EMPTY) >= UNIFORM)
    image.save(shots / "mcp-1-empty-world.png")
    engine, fill = share_of(image, ENGINE_EMPTY), share_of(image, SUNKEN)
    expect(
        engine >= UNIFORM,
        f"the empty world's viewport centre is {engine:.0%} the engine's empty frame "
        f"{ENGINE_EMPTY} and {fill:.0%} the editor's fill {SUNKEN}: the editor is not showing the "
        f"runtime's frame ({described})",
    )
    report.did("an empty world shows the engine's frame, not the editor's fill",
               f"{engine:.0%} of the centre is exactly {ENGINE_EMPTY}; {described}")


def act_drawable(session: Session, shots: Path, report: Report) -> None:
    image, described = settle(session.viewport, lambda r: chroma(r[0]) >= MIN_VIEWPORT_CHROMA)
    image.save(shots / "mcp-2-drawable.png")
    measured = chroma(image)
    expect(
        measured >= MIN_VIEWPORT_CHROMA,
        f"the drawable world's viewport is neutral (chroma {measured}); {described}",
    )
    report.did("a drawable world shows the engine's colours",
               f"chroma {measured} in the viewport panel; {described}")

    pillar = session.mcp.text("hierarchy:").split()[0]
    session.mcp.tool("edit.select", {"entity": pillar})
    # The reference is taken once the selection's gizmo has arrived from the engine, which is a
    # round trip or two after the selection; otherwise the undo is compared with a picture that
    # never had a gizmo in it.
    before = steady(session)
    before.save(shots / "mcp-2b-selected.png")
    session.mcp.tool("scene.translate", {"amount": [1.5, 0, 0]})
    moved, _ = settle(session.viewport, lambda r: difference(before, r[0]) > CHANGED)
    moved.save(shots / "mcp-3-translated.png")
    changed = difference(before, moved)
    expect(changed > CHANGED,
           f"scene.translate on {pillar} changed the viewport by {changed:.2f}; nothing moved")
    session.mcp.tool("edit.undo", {})
    restored, _ = settle(session.viewport, lambda r: difference(before, r[0]) < RESTORED)
    restored.save(shots / "mcp-4-undone.png")
    left = difference(before, restored)
    expect(left < RESTORED,
           f"edit.undo left the viewport {left:.2f} away from before the move")
    report.did("an MCP move is visible in the viewport and undo takes it back",
               f"translate changed the panel by {changed:.2f}, undo left {left:.2f}")


def act_window(session: Session, shots: Path, report: Report) -> None:
    whole, described = session.mcp.capture("editor:window")
    whole.save(shots / "mcp-5-window.png")
    hierarchy, where = session.mcp.capture("editor:window?panel=hierarchy")
    expect(hierarchy.size[0] < whole.size[0] and hierarchy.size[1] < whole.size[1],
           f"the hierarchy panel {hierarchy.size} is not inside the window {whole.size}")
    expect(f"of a {whole.size[0]}x{whole.size[1]} window" in where,
           f"the panel does not name the window it was cut from: {where}")
    report.did("the whole window, and one panel of it",
               f"{described}; hierarchy: {where}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="dev")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--runtime", default="")
    parser.add_argument("--work", default="")
    parser.add_argument("--world", default=DRAWABLE,
                        help="the drawable world for acts 2 and 3; worlds/city.cyworld must fail")
    parser.add_argument(
        "--display", default=os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY") or "")
    options = parser.parse_args()
    if not options.display:
        print("editor-window-mcp: there is no display, and this reads a window; not run here.",
              file=sys.stderr)
        return 3
    try:
        editor, runtime = binaries(options.profile, options.build, options.runtime)
    except (Failed, subprocess.CalledProcessError) as problem:
        print(f"editor-window-mcp: {problem}", file=sys.stderr)
        return 2
    if not runtime.is_file():
        print(f"editor-window-mcp: no runtime at {runtime}; not run here.", file=sys.stderr)
        return 3

    work = Path(options.work).resolve() if options.work else (
        ROOT / os.environ.get("CY_BUILD_DIR", "build") / "editor-window-mcp")
    work.mkdir(parents=True, exist_ok=True)
    root, _journal, shots = prepare(work)
    (root / EMPTY).write_text("cyworld 1\n")
    print(f"==> editor-window-mcp  profile={options.profile}  display={options.display}")
    print(f"    editor   {editor}\n    runtime  {runtime}\n    project  {root}")

    report = Report()
    acts = [
        ("act 0: no runtime, the editor's own fill", None, EMPTY, [act_control]),
        ("act 1: an empty world shows the engine's frame", runtime, EMPTY, [act_empty]),
        ("act 2 and 3: a drawable world, an MCP move and its undo, the whole window",
         runtime, options.world, [act_drawable, act_window]),
    ]
    code = 0
    try:
        for title, engine, world, steps in acts:
            print(f"--- {title} ---")
            session = Session(editor, engine, root, work, world, options.display)
            try:
                for step in steps:
                    step(session, shots, report)
            finally:
                session.close()
    except Failed as problem:
        print(f"\neditor-window-mcp: {problem}", file=sys.stderr)
        code = 1
    return code or report.exit_code


if __name__ == "__main__":
    sys.exit(main())

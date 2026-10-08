#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""samples/05b-editor-window — the animation panel (#29) against the real engine, through MCP only.

The Rust and C++ suites hold the two ends of the animation wire to one set of fixtures. This driver
runs both ends at once, `cy_editor_window_runtime` and `cyberdyne-editor --mcp`, with nothing standing
in for either, and sends no input: it authors with the registered animation commands and reads the
window the editor presented, like `mcp_window.py` and `audio_window.py`.

Acts, each able to fail:

  1. A locomotion graph authored over MCP, with two footsteps on the walk; the engine compiles it.
  2. The walk scrubbed: the viewport shows the preview character the engine posed, a second time
     shows another pose, and the engine's pose digest moves with it. Crossing a footstep fires it.
  3. The state machine with its condition open, then a lengthened transition: the engine is blending
     at the same time where it had finished. Play runs the engine's clock on; Stop takes the character
     out of the viewport.

Needs a display (the window is what is read) and a Vulkan device; exits 3 where it cannot run.
`--shots <directory>` keeps the captures.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

SAMPLE = Path(__file__).resolve().parent
ROOT = SAMPLE.parents[1]
sys.path.insert(0, str(ROOT / "samples" / "harness"))
sys.path.insert(0, str(SAMPLE))
from artefact import Failed, Report, expect  # noqa: E402
from mcp_window import Session, difference, require_pillow, settle  # noqa: E402
from window import binaries, prepare  # noqa: E402

WORLD = "worlds/animation-preview.cyworld"
GRAPH = "game/animation/locomotion.cyanimgraph"


def status(session: Session) -> dict:
    """What the engine last said about the graph and its preview."""
    result = session.mcp.call("tools/call", {"name": "animation.status",
                                             "arguments": {"reference": GRAPH}})
    return result.get("structuredContent", {})


def refreshed(session: Session) -> dict:
    """The engine's state after every request the editor queued has been answered."""
    return settle(lambda: status(session), lambda s: s.get("pending") == "false", 15.0)


def act_author(session: Session, report: Report) -> None:
    tool = session.mcp.tool
    # The first graph needs the engine's vocabulary; asking for it is what fetches it.
    session.mcp.call("tools/call", {"name": "animation.graph.create",
                                    "arguments": {"reference": GRAPH}})
    settle(lambda: status(session), lambda s: s.get("catalogue") == "ready", 20.0)
    tool("animation.graph.create", {"reference": GRAPH})
    tool("animation.node.property.set", {"reference": GRAPH, "node": 1, "property": "duration",
                                         "value": "2"})
    tool("animation.node.add", {"reference": GRAPH, "node_type": "pose.clip", "x": 16, "y": 160})
    tool("animation.node.property.set", {"reference": GRAPH, "node": 3, "property": "clip",
                                         "value": "walk"})
    tool("animation.node.add", {"reference": GRAPH, "node_type": "pose.state", "x": 230, "y": 160})
    tool("animation.node.property.set", {"reference": GRAPH, "node": 4, "property": "name",
                                         "value": "walk"})
    tool("animation.node.connect", {"reference": GRAPH, "from": 3, "from_pin": "pose", "to": 4,
                                    "to_pin": "pose"})
    tool("animation.node.add", {"reference": GRAPH, "node_type": "pose.transition", "x": 444,
                                "y": 16})
    tool("animation.node.property.set", {"reference": GRAPH, "node": 5, "property": "condition",
                                         "value": "moving"})
    tool("animation.node.connect", {"reference": GRAPH, "from": 2, "from_pin": "state", "to": 5,
                                    "to_pin": "from"})
    tool("animation.node.connect", {"reference": GRAPH, "from": 4, "from_pin": "state", "to": 5,
                                    "to_pin": "to"})
    for time_s in (0.25, 0.75):
        tool("animation.event.add", {"reference": GRAPH, "node": 3, "event": "footstep",
                                     "time": time_s})
    tool("animation.graph.compile", {"reference": GRAPH})
    state = settle(lambda: status(session), lambda s: "compiled" in s and s.get("pending") ==
                   "false", 20.0)
    expect(state.get("compiled") == "true", f"the engine did not compile the graph: {state}")
    report.did("a graph authored over MCP compiles in the engine",
               f"states {state.get('states')}, parameters {state.get('parameters')}")


def viewport(session: Session, shots: Path, name: str):
    image, described = session.mcp.capture("editor:window?panel=viewport")
    image.save(shots / name)
    return image, described


def act_scrub(session: Session, shots: Path, report: Report) -> None:
    # Frame the character: the world's second root is an empty node at its hips.
    hierarchy = [line.split()[0] for line in session.mcp.text("hierarchy:").splitlines()
                 if line.strip() and not line.startswith(" ")]
    expect(len(hierarchy) == 2, f"the preview world has two roots, ground and focus: {hierarchy}")
    session.mcp.tool("edit.select", {"entity": hierarchy[1]})
    session.mcp.tool("viewport.frame-selection", {})
    # The ground selected instead, so the gizmo sits at the feet rather than over the hips.
    session.mcp.tool("edit.select", {"entity": hierarchy[0]})
    session.mcp.tool("animation.preview.stop", {})
    refreshed(session)
    time.sleep(1.0)
    empty, _ = viewport(session, shots, "editor-animation-window-empty.png")

    session.mcp.tool("animation.preview.scrub", {"reference": GRAPH, "node": 3, "time": 0.2})
    first = settle(lambda: refreshed(session), lambda s: s.get("previewing") == "true", 15.0)
    time.sleep(1.0)
    contact, described = viewport(session, shots, "editor-animation-window-contact.png")
    drawn = difference(empty, contact)
    expect(drawn > 0.002, f"the viewport did not change when the character appeared ({drawn:.4f}); "
                          f"{described}")
    session.mcp.tool("animation.preview.scrub", {"reference": GRAPH, "node": 3, "time": 0.3})
    second = refreshed(session)
    time.sleep(1.0)
    passing, _ = viewport(session, shots, "editor-animation-window-passing.png")
    moved = difference(contact, passing)
    expect(second.get("pose_digest") != first.get("pose_digest"),
           f"the engine's pose did not change with the scrub: {first} {second}")
    expect(moved > 0.0005, f"the viewport did not show the new pose ({moved:.4f})")
    fired = [value for key, value in second.items() if key.startswith("event.")]
    expect(any(value.startswith("footstep at 0.25") for value in fired),
           f"crossing the footstep did not fire it: {fired}")
    report.did("the viewport shows the pose the engine evaluated, and a scrub moves it",
               f"{drawn:.4f} of the frame drawn, {moved:.4f} moved; {fired}")


def act_machine(session: Session, shots: Path, report: Report) -> None:
    tool = session.mcp.tool
    tool("animation.preview.scrub", {"reference": GRAPH, "node": 0, "time": 0.4})
    tool("animation.preview.parameter", {"name": "moving", "value": 1})
    walking = settle(lambda: refreshed(session), lambda s: s.get("state") == "walk", 15.0)
    expect(walking.get("state") == "walk" and walking.get("target") == "",
           f"the open condition did not take the machine into the walk: {walking}")
    tool("animation.node.property.set", {"reference": GRAPH, "node": 5, "property": "duration",
                                         "value": "0.625"})
    blending = settle(lambda: refreshed(session), lambda s: s.get("target") == "walk", 15.0)
    expect(blending.get("target") == "walk",
           f"the lengthened blend is not what the engine evaluates: {blending}")
    time.sleep(1.0)
    viewport(session, shots, "editor-animation-window-blend.png")
    whole, _ = session.mcp.capture("editor:window")
    whole.save(shots / "editor-animation-window.png")
    report.did("an edited transition changes what the engine evaluates",
               f"at 0.4 s: {walking.get('preview')} before, {blending.get('preview')} after")

    tool("animation.preview.play", {"reference": GRAPH, "node": 0})
    # The engine's clock: a playing preview is polled while it plays, so its time keeps moving (and
    # starts again from the entry state every four seconds).
    times = []
    for _ in range(6):
        time.sleep(0.5)
        times.append(status(session).get("time", ""))
    expect(len(set(times)) >= 4, f"the engine's clock did not run the preview on: {times}")
    tool("animation.preview.pause", {})
    tool("animation.preview.stop", {})
    stopped = settle(lambda: refreshed(session), lambda s: s.get("previewing") == "false", 15.0)
    expect(stopped.get("previewing") == "false", f"Stop left the preview on: {stopped}")
    report.did("Play runs the engine's clock, and Stop ends the preview",
               f"times read while playing: {', '.join(times)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="dev")
    parser.add_argument("--runtime", default="")
    parser.add_argument("--work", default="")
    parser.add_argument("--shots", default="")
    parser.add_argument(
        "--display", default=os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY") or "")
    options = parser.parse_args()
    if not options.display and sys.platform != "darwin":
        print("editor-animation-window: there is no display; not run here.", file=sys.stderr)
        return 3
    try:
        require_pillow()
        editor, runtime = binaries(options.profile, False, options.runtime)
    except Failed as problem:
        print(f"editor-animation-window: {problem}", file=sys.stderr)
        return 2
    if not runtime.is_file():
        print(f"editor-animation-window: no runtime at {runtime}; not run here.", file=sys.stderr)
        return 3
    work = Path(options.work).resolve() if options.work else (
        ROOT / os.environ.get("CY_BUILD_DIR", "build") / "editor-animation-window")
    work.mkdir(parents=True, exist_ok=True)
    root, _journal, shots = prepare(work)
    if options.shots:
        shots = Path(options.shots).resolve()
        shots.mkdir(parents=True, exist_ok=True)
    report = Report()
    session = None
    try:
        session = Session(editor, runtime, root, work, WORLD, options.display)
        act_author(session, report)
        act_scrub(session, shots, report)
        act_machine(session, shots, report)
    except Failed as problem:
        print(f"editor-animation-window: {problem}", file=sys.stderr)
        return 1
    finally:
        if session is not None:
            session.close()
    return report.exit_code


if __name__ == "__main__":
    sys.exit(main())

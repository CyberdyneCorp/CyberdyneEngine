#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""samples/05b-editor-window — the audio tools (#29) against the real engine, through MCP only.

The Rust and C++ suites hold the two ends of the audio wire to one set of fixtures. This driver
runs both ends at once, `cy_editor_window_runtime` and `cyberdyne-editor --mcp`, with nothing
standing in for either, and sends no input: it uses the registered audio commands and reads the
window the editor presented, like `mcp_window.py`.

Acts, each able to fail:

  1. A mixer and a cue authored over MCP; the engine's state names the new bus at the gain set.
  2. A source placed and selected; the Editor view shows its rings (a capture), and a preview heard
     from the viewport camera reports the engine's attenuation.
  3. Play: the engine reports the source sounding; Stop ends it.

Needs a display (the window is what is read) and a Vulkan device; exits 3 where it cannot run.
`--shots <directory>` keeps the viewport captures.
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
from mcp_window import DRAWABLE, Session, require_pillow, settle  # noqa: E402
from window import binaries, prepare  # noqa: E402

CUE = "game/audio/cues/hum.cycue"


def status(session: Session) -> dict:
    """Ask the engine for its audio state, then read what it answered."""
    session.mcp.tool("audio.refresh", {})
    time.sleep(0.5)
    result = session.mcp.call("tools/call", {"name": "audio.status", "arguments": {}})
    return result.get("structuredContent", {})


def act_mixer(session: Session, report: Report) -> None:
    session.mcp.tool("audio.mixer.create", {})
    session.mcp.tool("audio.bus.add", {"name": "Ambience"})
    session.mcp.tool("audio.bus.volume", {"name": "Ambience", "volume": 0.5})
    session.mcp.tool("audio.cue.save", {"reference": CUE, "clip": "tone:220:1", "bus": "Ambience",
                                        "looping": True})
    state = settle(lambda: status(session), lambda s: "bus.Ambience.volume" in s, 15.0)
    expect(abs(float(state.get("bus.Ambience.volume", "nan")) - 0.5) < 1e-6,
           f"the engine does not hold Ambience at 0.5: {state}")
    report.did("a bus authored over MCP is in the engine's graph at its gain",
               f"backend {state.get('backend')}, Ambience {state['bus.Ambience.volume']}")


def act_source(session: Session, shots: Path, report: Report) -> None:
    created = session.mcp.call("tools/call", {"name": "audio.source.create", "arguments": {
        "cue": CUE, "at": [0.0, 0.5, 0.0], "min_distance": 1.5, "max_distance": 4.0}})
    entity = created["structuredContent"]["entity"]
    image, described = session.mcp.capture("editor:window?panel=viewport")
    image.save(shots / "editor-audio-source-viewport.png")
    whole, _ = session.mcp.capture("editor:window")
    whole.save(shots / "editor-audio-source-window.png")
    teal = sum(1 for (r, g, b) in image.getdata() if g > 150 and b > 140 and r < 140)
    expect(teal > 200, f"no source rings in the viewport ({teal} teal pixels); {described}")
    report.did("the Editor view draws the source's rings", f"{teal} teal pixels; {described}")

    session.mcp.tool("audio.source.preview", {"entity": entity})
    state = settle(lambda: status(session), lambda s: "preview.gain" in s, 15.0)
    expect("preview.gain" in state, f"the engine reported no preview: {state}")
    # Heard from beyond the dashed ring the engine's curve is zero; from inside it, not.
    distance, gain = float(state["preview.distance"]), float(state["preview.gain"])
    expect((gain == 0.0) == (distance >= 4.0),
           f"the engine's gain {gain} at {distance} m disagrees with the 4 m silence radius")
    report.did("a source preview is heard from the viewport camera",
               f"distance {state['preview.distance']} m, gain {state['preview.gain']}, "
               f"pan {state['preview.left']}/{state['preview.right']}")


def act_play(session: Session, report: Report) -> None:
    session.mcp.tool("play.enter", {})
    state = settle(lambda: status(session), lambda s: s.get("playing") == "true", 20.0)
    expect(state.get("playing") == "true" and state.get("play_voices") == "1",
           f"Play is not sounding the source: {state}")
    report.did("Play sounds the world's source in the engine",
               f"{state['play_voices']} source(s), {state['active_voices']} voice(s)")
    session.mcp.tool("play.leave", {})
    state = settle(lambda: status(session), lambda s: s.get("playing") == "false", 20.0)
    expect(state.get("playing") == "false", f"Stop left the source sounding: {state}")
    report.did("Stop ends it", f"playing {state['playing']}")


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
        print("editor-audio-window: there is no display; not run here.", file=sys.stderr)
        return 3
    try:
        require_pillow()
        editor, runtime = binaries(options.profile, False, options.runtime)
    except Failed as problem:
        print(f"editor-audio-window: {problem}", file=sys.stderr)
        return 2
    if not runtime.is_file():
        print(f"editor-audio-window: no runtime at {runtime}; not run here.", file=sys.stderr)
        return 3
    work = Path(options.work).resolve() if options.work else (
        ROOT / os.environ.get("CY_BUILD_DIR", "build") / "editor-audio-window")
    work.mkdir(parents=True, exist_ok=True)
    root, _journal, shots = prepare(work)
    if options.shots:
        shots = Path(options.shots).resolve()
        shots.mkdir(parents=True, exist_ok=True)
    report = Report()
    session = None
    try:
        session = Session(editor, runtime, root, work, DRAWABLE, options.display)
        act_mixer(session, report)
        act_source(session, shots, report)
        act_play(session, report)
    except Failed as problem:
        print(f"editor-audio-window: {problem}", file=sys.stderr)
        return 1
    finally:
        if session is not None:
            session.close()
    return report.exit_code


if __name__ == "__main__":
    sys.exit(main())

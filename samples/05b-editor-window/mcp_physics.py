#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""samples/05b-editor-window — the physics tools photographed through MCP. Issue #29.

The editor runs with `--mcp` beside the hosted runtime, exactly as `mcp_window.py` does, and every
step is a registered command over the editor's own MCP interface; no input is synthesised. The world
is written into the prepared project copy: a ground, a post with a motorised door hinged to it, a
bob hanging from the world by a point joint, and a crate that falls.

  1. The door is selected while authoring. The ENGINE draws its authored hinge — the anchor, the
     axis and the limits — into the frame; the capture must differ from the same view unselected.
  2. The collider, contact, joint and sleep layers are asked for with `viewport.physics.*` and play
     is pressed. The engine draws them from the running physics world; the capture must differ from
     the same view with no layers asked for.

Usage:
    python3 samples/05b-editor-window/mcp_physics.py --shots docs/design/images
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

SAMPLE = Path(__file__).resolve().parent
ROOT = SAMPLE.parents[1]
sys.path.insert(0, str(ROOT / "samples" / "harness"))
sys.path.insert(0, str(SAMPLE))
from artefact import Failed, expect  # noqa: E402
from mcp_window import CHANGED, Session, difference, require_pillow, steady  # noqa: E402
from window import binaries, prepare  # noqa: E402

WORLD = "worlds/physics-joints.cyworld"

#: Physics layers the second capture asks for.
LAYERS = ("colliders", "contacts", "constraints", "sleep-state")

HEADER = """cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
type 2 runtime "MeshRenderer"
  field 4 text "mesh" ""
  field 5 text "material" ""
  field 6 vec3 "tint" "Per-instance base colour multiplier."
type 3 runtime "StaticBody"
type 4 runtime "RigidBody"
  field 7 float "mass" ""
  field 8 float "gravity_scale" ""
type 5 runtime "Collider"
  field 9 text "shape" ""
  field 10 vec3 "extent" ""
  field 11 float "radius" ""
  field 12 float "height" ""
type 6 runtime "Joint"
  field 13 text "kind" ""
  field 14 entity "target" ""
  field 15 vec3 "anchor" ""
  field 16 vec3 "axis" ""
  field 17 float "limit_min" ""
  field 18 float "limit_max" ""
  field 19 float "motor_velocity" ""
  field 20 float "motor_max_force" ""
"""


def box(position: int, name: str, at: tuple, scale: tuple, tint: tuple, body: str) -> str:
    """One node: a box mesh, a matching box collider, and a body (`static` or a mass)."""
    extent = " ".join(f"{value / 2:g}" for value in scale)
    lines = [
        f'node {position} - "physics" "{name}"',
        "  component 1",
        "    field 1 0 0 0 1",
        f"    field 2 {' '.join(f'{value:g}' for value in at)}",
        f"    field 3 {' '.join(f'{value:g}' for value in scale)}",
        "  component 2",
        '    field 4 "assets/primitives/Box.cyprim"',
        '    field 5 ""',
        f"    field 6 {' '.join(f'{value:g}' for value in tint)}",
        "  component 5",
        '    field 9 "box"',
        f"    field 10 {extent}",
        "    field 11 0.5",
        "    field 12 1",
    ]
    if body == "static":
        lines.append("  component 3")
    else:
        lines += ["  component 4", f"    field 7 {body}", "    field 8 1"]
    return "\n".join(lines) + "\n"


def joint(kind: str, target: str, anchor: str, axis: str, limits: str = "1 -1",
          motor: str = "0 0") -> str:
    low, high = limits.split()
    velocity, force = motor.split()
    return "\n".join([
        "  component 6",
        f'    field 13 "{kind}"',
        f"    field 14 {target}",
        f"    field 15 {anchor}",
        f"    field 16 {axis}",
        f"    field 17 {low}",
        f"    field 18 {high}",
        f"    field 19 {velocity}",
        f"    field 20 {force}",
    ]) + "\n"


def world_text() -> str:
    return (
        HEADER
        + box(0, "Ground", (0, -0.5, 0), (12, 1, 12), (0.35, 0.37, 0.4), "static")
        + box(1, "Post", (0, 1.5, 0), (0.3, 3, 0.3), (0.72, 0.22, 0.9), "static")
        + box(2, "Door", (1.2, 1.5, 0), (2, 2.6, 0.15), (0.95, 0.68, 0.08), "20")
        + joint("hinge", "1", "-1.05 0 0", "0 1 0", "-1.4 1.4", "1.2 400")
        + box(3, "Bob", (-4.2, 2.2, 0), (0.6, 0.6, 0.6), (0.08, 0.8, 0.78), "2")
        + joint("point", "-", "1.2 1.2 0", "1 0 0")
        + box(4, "Crate", (2.5, 4, 2.5), (1, 1, 1), (0.9, 0.3, 0.25), "5")
    )


def node_id(session: Session, name: str) -> str:
    """The identity the hierarchy resource gives a node, by its name."""
    for line in session.mcp.text("hierarchy:").splitlines():
        words = line.split()
        if name in words and words:
            return words[0]
    raise Failed(f"the hierarchy has no {name}")


def capture_gizmo(session: Session, shots: Path) -> str:
    unselected = steady(session)
    session.mcp.tool("edit.select", {"entity": node_id(session, "Door")})
    selected = steady(session)
    changed = difference(unselected, selected)
    expect(changed > CHANGED / 4,
           f"selecting the hinged door changed the viewport by {changed:.2f}; no gizmo was drawn")
    whole, _ = session.mcp.capture("editor:window")
    whole.save(shots / "editor-physics-joint-gizmo.png")
    return f"selecting the door changed the viewport by {changed:.2f}"


def capture_layers(session: Session, shots: Path) -> str:
    session.mcp.tool("edit.select", {"entity": node_id(session, "Crate")})
    session.mcp.tool("play.enter", {})
    time.sleep(1.5)
    session.mcp.tool("play.pause", {})
    plain = steady(session)
    for layer in LAYERS:
        session.mcp.tool(f"viewport.physics.{layer}", {"state": "on"})
    layered = steady(session)
    changed = difference(plain, layered)
    expect(changed > CHANGED / 4,
           f"asking for the physics layers changed the paused viewport by {changed:.2f}")
    whole, _ = session.mcp.capture("editor:window")
    whole.save(shots / "editor-physics-layers.png")
    session.mcp.tool("play.leave", {})
    return f"the physics layers changed the paused viewport by {changed:.2f}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="dev")
    parser.add_argument("--runtime", default="")
    parser.add_argument("--work", default="")
    parser.add_argument("--shots", default="", help="where the two PNGs go")
    parser.add_argument(
        "--display", default=os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY") or "")
    options = parser.parse_args()
    if not options.display:
        print("editor-physics-mcp: there is no display, and this reads a window; not run here.",
              file=sys.stderr)
        return 3
    try:
        require_pillow()
        editor, runtime = binaries(options.profile, False, options.runtime)
    except (Failed, subprocess.CalledProcessError) as problem:
        print(f"editor-physics-mcp: {problem}", file=sys.stderr)
        return 2
    if not runtime.is_file():
        print(f"editor-physics-mcp: no runtime at {runtime}; not run here.", file=sys.stderr)
        return 3
    work = Path(options.work).resolve() if options.work else (
        ROOT / os.environ.get("CY_BUILD_DIR", "build") / "editor-physics-mcp")
    work.mkdir(parents=True, exist_ok=True)
    root, _journal, shots = prepare(work)
    if options.shots:
        shots = Path(options.shots).resolve()
        shots.mkdir(parents=True, exist_ok=True)
    (root / WORLD).write_text(world_text())
    session = None
    try:
        session = Session(editor, runtime, root, work, WORLD, options.display)
        print(f"editor-physics-mcp: {capture_gizmo(session, shots)}")
        print(f"editor-physics-mcp: {capture_layers(session, shots)}")
    except Failed as problem:
        print(f"editor-physics-mcp: {problem}", file=sys.stderr)
        return 1
    finally:
        if session is not None:
            session.close()
    print(f"editor-physics-mcp: wrote {shots / 'editor-physics-joint-gizmo.png'} and "
          f"{shots / 'editor-physics-layers.png'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

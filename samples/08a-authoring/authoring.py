#!/usr/bin/env python3
"""samples/08a-authoring — M8.a's closing artefact. Section 6.

`just run-authoring` is the recipe; `smoke.authoring` is the CTest entry; this file is what both of
them run.

--- WHAT IT IS -------------------------------------------------------------------------------------

The sentence this milestone exists for, run end to end and measured:

    create a sphere and a box in an empty world
    place the sphere above the box with the gizmo
    add a rigid body to each, in the inspector
    press play — the sphere falls and lands on the box
    stop — the world is exactly as it was authored
    undo back to an empty world, exactly

**It is one session, not six checks.** Every step below happens in one editor process, over one
control socket, against one engine runtime, in the order above; a fixture cannot satisfy any of
them, because each step's evidence is the state the previous one left. That is the whole point of
the artefact: primitive creation, the gizmo's manipulation, the physics ECS bridge, play mode and
the transaction system are exercised TOGETHER, and none of them is worth anything here alone.

Three processes:

| | |
|---|---|
| `cy_editor_window_runtime` | **the engine.** It loads the same `.cyworld` the editor opens, applies the editor's transactions as they commit, and hosts the play session — `cy::gameplay::PlaySession` over `cy::physics::PhysicsBridge` on Jolt. It needs a Vulkan device; where there is none the target does not exist and the acts that need it are reported NOT EVALUATED. |
| `cyberdyne-editor --mcp` | the editor, attached to that runtime with `--host`, driven through its own command registry. No window and no graphics device. |
| `cy_sample_authoring` | the measurement. See below. |

--- WHY THERE IS A SECOND PROGRAM, AND WHAT IT MEASURES ----------------------------------------------

The runtime's report counts sessions, ticks and bodies. It does not count metres, and no message the
editor can ask for carries the simulated placement of a node — the only place it appears is inside
the runtime's own copy of the authored world. So "the sphere falls and lands on the box" would be a
claim with no number behind it.

`cy_sample_authoring` is where that number comes from. It plays THE SAME AUTHORED WORLD: the empty
`.cyworld` this project ships, plus the editor's own committed transactions, replayed with
`cy::scene::serialization::apply_transaction` out of the `.cyjournal` the session above wrote. It
reports where the sphere started, where it came to rest, whether that is the height the two
colliders imply, and whether `stop()` put the file back byte for byte — and it needs no display and
no graphics device, which makes it the half a hosted runner can judge.

--- THE SURFACE THIS DRIVER USES, AND WHY IT IS THE AGENT'S RATHER THAN A SCRIPT ----------------------

`cyberdyne-editor --script` runs commands one per line and is the obvious way to drive a session.
It cannot drive this one: `cy_editor_app::run_script` makes every argument a `Value::Text` and the
registry validates argument kinds, so `scene.translate amount=0,3,0` is refused with

    the parameter "amount" is declared vec3 and a text was supplied

and a world in which nothing can be PLACED is not this milestone's world. The agent surface parses a
typed argument out of the same text (`cy_editor_agent::tool`), so that is the surface this artefact
drives, over the Model Context Protocol, exactly as `samples/05b-agent-authoring` does. The run
below performs that refusal rather than describing it, so the day `run_script` reuses the agent's
parser this note goes stale loudly.

The agent surface costs two things, and the run performs both refusals too, because each is the
scope working rather than a defect:

  * `scene.create-primitive` writes its `.cyprim` source to `assets/primitives/`, and the `author`
    scope grants `game/` and nothing else. So the artefact asks for the one-call command, records
    the refusal, and does the two halves the scope does permit — `asset.write-primitive` to write
    the same source under `game/`, and `asset.import` to put it in the world. The entity is the same
    entity: `cy_editor_services::primitives::create_mesh_instance` is the ONE constructor of a mesh
    instance and both callers use it, which is task 2.3 guaranteed rather than tested for.
  * `file.save` is an irreversible mutation and the `author` scope grants read and reversible
    mutation. The world is therefore never written by this session, which costs nothing here: the
    engine's copy of it is the runtime's, and the measurement's copy is rebuilt from the journal.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

SAMPLE = Path(__file__).resolve().parent
ROOT = SAMPLE.parents[1]
sys.path.insert(0, str(ROOT / "samples" / "harness"))

from artefact import (  # noqa: E402 — the path above has to be set first
    Absent,
    Failed,
    Report,
    Statistic,
    expect,
    socket_path,
)

#: The document every act works in, spelled the same way on both sides of the process boundary: the
#: editor hashes this string into a `DocumentId` and the engine derives the identical number from
#: the identical string. Nothing transmits it.
WORLD = "worlds/authored.cyworld"

#: Where the primitive sources go. `assets/primitives/` is where a person's editor writes them and
#: is what the artefact asks for first; this is the directory the `author` agent scope grants, and
#: the run records the refusal that puts the files here instead.
GROUND_SOURCE = "game/Ground.cyprim"
BALL_SOURCE = "game/Ball.cyprim"

#: The scene: a wide flat box on the ground, and a sphere three metres above it.
GROUND_EXTENT = [4.0, 0.5, 4.0]
BALL_RADIUS = 0.5
DROP = 3.0


def relative(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


# --- The connection ---------------------------------------------------------------------------------


class Agent:
    """One MCP connection, over the editor's standard input and output.

    Newline-delimited JSON-RPC 2.0, hand written and dependency-free for the reason
    `samples/05b-agent-authoring` gives: a client built out of the editor's own types would prove
    nothing about the wire.
    """

    def __init__(self, binary: Path, root: Path, journal: Path, host: Path | None, importer: Path):
        arguments = [
            str(binary), "--open", WORLD, "--mcp",
            "--agent-scope", "author",
            "--agent-intent", "build the milestone's scene and press play",
            "--journal", str(journal),
        ]
        if host is not None:
            arguments += ["--host", str(host)]
        self.process = subprocess.Popen(
            arguments,
            cwd=root,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=dict(os.environ, CY_AGENT="cyberdyne-m8a-artefact", CY_IMPORT_CLI=str(importer)),
        )
        self.next_id = 0

    def call(self, method: str, params: dict | None = None) -> dict:
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            message["params"] = params
        assert self.process.stdin is not None and self.process.stdout is not None
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise Failed(
                f"the editor closed the connection during {method}; "
                f"stderr: {self.process.stderr.read() if self.process.stderr else ''}"
            )
        answer = json.loads(line)
        expect(answer.get("id") == self.next_id, f"{method}: the answer is for another request")
        return answer

    def tool(self, command: str, **arguments) -> tuple[bool, str, dict]:
        """Call a tool. Returns (ok, the text it said, its structured content).

        The command's identifier is a positional parameter called `command` rather than `name`,
        because `name` is an argument several of the editor's own commands take and a keyword
        collision here would be a puzzling error a long way from its cause.
        """
        answer = self.call("tools/call", {"name": command, "arguments": arguments})
        if "error" in answer:
            raise Failed(f"{command}: the protocol refused the call: {answer['error']}")
        result = answer["result"]
        text = "\n".join(part.get("text", "") for part in result.get("content", ()))
        return not result.get("isError", False), text, result.get("structuredContent", {})

    def resource(self, prefix: str) -> str:
        """Read the first resource whose address starts with `prefix`."""
        listed = self.call("resources/list")["result"]["resources"]
        uri = next((entry["uri"] for entry in listed if entry["uri"].startswith(prefix)), None)
        expect(uri is not None, f"the editor offers no {prefix} resource")
        answer = self.call("resources/read", {"uri": uri})
        result = answer.get("result", {})
        expect("contents" in result, f"reading {uri} was refused: {json.dumps(result)[:300]}")
        return "\n".join(part.get("text", "") for part in result["contents"])

    def settle(self, seconds: float = 1.0, polls: int = 5) -> None:
        """Let the editor pump.

        `cy_editor_mcp::serve` calls `Editor::pump` once per request, and `pump` is what forwards a
        committed transaction to the runtime and drains what the runtime sent back. So a driver that
        wants the far end to have caught up asks for something cheap a few times — which is what an
        interface does at frame rate and what this stands in for.
        """
        for _ in range(polls):
            time.sleep(seconds / polls)
            self.call("resources/list")

    def close(self) -> str:
        assert self.process.stdin is not None
        self.process.stdin.close()
        try:
            self.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.process.kill()
        return self.process.stderr.read() if self.process.stderr else ""


class Runtime:
    """The engine on the far end of the editor's control socket."""

    def __init__(self, binary: Path, project: Path, control: Path, viewport: Path, seconds: float):
        self.process = subprocess.Popen(
            [
                str(binary),
                "--host", str(control),
                "--socket", str(viewport),
                "--project", str(project),
                "--world", WORLD,
                "--physics", "jolt",
                "--seconds", f"{seconds:.0f}",
                "--rate", "60",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        deadline = time.monotonic() + 40.0
        while time.monotonic() < deadline:
            if control.exists():
                return
            if self.process.poll() is not None:
                raise Absent(
                    "the engine runtime exited before it opened its control socket:\n"
                    + (self.process.stdout.read() if self.process.stdout else "")
                )
            time.sleep(0.05)
        raise Failed("the engine runtime never opened its control socket")

    def finish(self) -> str:
        self.process.terminate()
        try:
            output, _ = self.process.communicate(timeout=40)
        except subprocess.TimeoutExpired:
            self.process.kill()
            output, _ = self.process.communicate()
        return output or ""


def runtime_line(published: str, name: str) -> str:
    """One `editor-window-runtime: <name> …` line of the runtime's own report.

    The LAST one, deliberately. The runtime prints a start-up line and a closing line under several
    of the same names — `world` names the file it opened and later what became of it, `play` names
    the backend it chose and later what the sessions did — and the closing report is what this
    driver reads. Taking the first match read the start-up line and reported an empty world as
    evidence that the undos had worked, which is a check that could not fail.
    """
    marker = f"editor-window-runtime: {name}"
    found = ""
    for line in published.splitlines():
        if line.startswith(marker) and line[len(marker):len(marker) + 1] in (" ", ""):
            found = line[len(marker):].strip()
    return found


# --- The session ------------------------------------------------------------------------------------


def act_empty(agent: Agent, world: Path, report: Report) -> None:
    """The world the session starts in, and the fact that it is empty."""
    hierarchy = agent.resource("hierarchy:")
    expect(
        "no nodes" in hierarchy,
        f"the world this artefact starts in is not empty:\n{hierarchy}",
    )
    types = sum(1 for line in world.read_text().splitlines() if line.startswith("type "))
    report.did(
        "an empty world, and the schema it is authored against",
        f"{relative(world)} declares {types} types and no nodes",
    )


def create_primitive(agent: Agent, report: Report, shape: str, name: str, source: str,
                     at: list[float], **parameters) -> str:
    """One primitive, through the one-call command where the scope allows it and its two halves
    where it does not.

    The refusal is PERFORMED on every run rather than assumed, and the artefact takes the direct
    path the moment it stops being refused — so this reports the tree as it is rather than as it was
    when the driver was written.
    """
    direct, refusal, values = agent.tool(
        "scene.create-primitive", shape=shape, name=name, at=at, **parameters
    )
    if direct:
        report.did(
            f"created a {shape} with scene.create-primitive",
            refusal.splitlines()[0] if refusal else name,
        )
        return values["entity"]

    expect(
        "does not include it" in refusal,
        f"scene.create-primitive was refused for an unrecorded reason:\n{refusal}",
    )
    report.did(
        f"the agent scope holds when a {shape} is created",
        "scene.create-primitive writes assets/primitives/ and this connection was granted game/; "
        "refused with the scope as the reason, so the source is written under game/ instead",
    )

    ok, text, _ = agent.tool(
        "asset.write-primitive", shape=shape, name=name, asset=source, **parameters
    )
    expect(ok, f"asset.write-primitive was refused: {text}")
    ok, text, values = agent.tool("asset.import", path=source, at=at)
    expect(ok, f"asset.import was refused: {text}")
    expect("entity" in values, f"asset.import placed nothing in the world:\n{text}")
    report.did(
        f"created a {shape}",
        f"{source} written by the editor and imported by {values.get('importer', '?')} "
        f"({values.get('sub-assets', '?')} sub-asset(s), cache {values.get('cache', '?')}) "
        f"at {at}",
    )
    return values["entity"]


def act_create(agent: Agent, project: Path, report: Report) -> tuple[str, str]:
    """Task 6.1 — a sphere and a box, in an empty world, from the editor."""
    ground = create_primitive(
        agent, report, "box", "Ground", GROUND_SOURCE, [0.0, 0.0, 0.0],
        origin="base", extent=GROUND_EXTENT,
    )
    ball = create_primitive(
        agent, report, "sphere", "Ball", BALL_SOURCE, [0.0, 0.0, 0.0], radius=BALL_RADIUS,
    )
    expect(ground != ball, "two creations produced one identity")

    # THE SOURCE IS AN ASSET IN THE PROJECT, and the point of design.md §2 is that it is an ORDINARY
    # one: the same importer registry, the same derivation key and the same cache as a glTF. So the
    # file is on disk and the cook left its record beside every other cooked asset.
    source = project / BALL_SOURCE
    expect(source.is_file(), f"the editor reported writing {BALL_SOURCE} and there is no file")
    cooked = project / ".cy" / "cooked"
    expect(cooked.is_dir(), "the primitive was imported and nothing was cooked")
    report.did(
        "a primitive is a source asset and a cooked one",
        f"{BALL_SOURCE} is {source.stat().st_size} bytes of text and "
        f"{sum(1 for _ in cooked.rglob('*') if _.is_file())} cooked file(s) came out of the two",
    )

    hierarchy = agent.resource("hierarchy:")
    expect(
        hierarchy.count("\n") >= 2,
        f"the document does not hold two entities:\n{hierarchy}",
    )
    return ground, ball


def act_place(agent: Agent, ball: str, report: Report) -> None:
    """Task 6.2 — place the sphere above the box, with the gizmo."""
    ok, text, _ = agent.tool("edit.select", entity=ball)
    expect(ok, f"edit.select was refused: {text}")
    ok, text, _ = agent.tool("viewport.transform-mode-move")
    expect(ok, f"viewport.transform-mode-move was refused: {text}")

    # `scene.translate` IS the gizmo. Its own metadata says so — "through the same manipulation a
    # gizmo drag performs — the same start-state capture, the same pivot, the same space, and the
    # same increments" — and `cy_editor_services::manipulate` builds a `Drag` out of the focused
    # viewport's `gizmo_space` and `gizmo_pivot` and advances it, which is the same call the pointer
    # makes. `samples/05b-editor-window` drags the engine's published handle with a real pointer;
    # this drives the identical code with a stated amount.
    ok, text, values = agent.tool("scene.translate", amount=[0.0, DROP, 0.0])
    expect(ok, f"scene.translate was refused: {text}")
    report.did(
        "placed the sphere above the box with the gizmo",
        f"{text.splitlines()[0]} — through cy_editor_services::manipulate, the implementation a "
        f"pointer drag on the move gizmo uses (amount {values.get('amount', '?')})",
    )


def act_bodies(agent: Agent, ground: str, ball: str, report: Report) -> None:
    """Task 6.3 — a rigid body on each, as the inspector adds one."""
    ok, text, values = agent.tool(
        "scene.add-body", entity=ball, kind="dynamic", mass=2.0,
        shape="sphere", radius=BALL_RADIUS,
    )
    expect(ok, f"scene.add-body was refused for the sphere: {text}")
    expect(values.get("collider") == "sphere", f"the sphere got no sphere collider:\n{text}")

    ok, text, values = agent.tool(
        "scene.add-body", entity=ground, kind="static", shape="box",
        extent=[GROUND_EXTENT[0] / 2, GROUND_EXTENT[1] / 2, GROUND_EXTENT[2] / 2],
    )
    expect(ok, f"scene.add-body was refused for the box: {text}")
    expect(values.get("collider") == "box", f"the box got no box collider:\n{text}")

    history = agent.resource("history:")
    entries = [line for line in history.splitlines() if line[:1].isdigit()]
    expect(
        len(entries) >= 5,
        f"five edits were made and the history holds {len(entries)}:\n{history}",
    )
    report.did(
        "a rigid body on each, one transaction apiece",
        f"a dynamic body and a sphere collider on the sphere, a static body and a box collider on "
        f"the box; {len(entries)} transactions in the history, every one attributed",
    )


def act_play(agent: Agent, hosted: bool, report: Report, seconds: float) -> None:
    """Tasks 6.4 and 6.5 — press play, let it fall, and stop."""
    ok, text, values = agent.tool("play.enter")
    if not hosted:
        report.not_evaluated(
            "pressing play reaches a runtime",
            "there is no engine runtime on this machine to press it against; the editor answered "
            f"{text.splitlines()[0] if text else '(nothing)'}",
        )
        return
    expect(ok, f"play.enter was refused: {text}")
    expect(values.get("play") == "playing", f"play.enter did not enter play:\n{text}")
    report.did("pressed play", text.splitlines()[0])

    agent.settle(seconds, polls=max(4, int(seconds * 4)))

    ok, text, values = agent.tool("play.leave")
    expect(ok, f"play.leave was refused: {text}")
    expect(values.get("play") == "editing", f"play.leave did not leave play:\n{text}")
    report.did("stopped", text.splitlines()[0])


def act_undo(agent: Agent, report: Report) -> int:
    """Task 6.6 — undo back to an empty world, exactly."""
    undone = 0
    for _ in range(32):
        ok, text, _ = agent.tool("edit.undo")
        if not ok:
            expect(
                "nothing has been changed" in text,
                f"edit.undo stopped for an unrecorded reason:\n{text}",
            )
            break
        undone += 1
    else:
        raise Failed("undo never reached the beginning of the session")

    hierarchy = agent.resource("hierarchy:")
    expect(
        "no nodes" in hierarchy,
        f"undo did not empty the world; it still holds:\n{hierarchy}",
    )
    # The transactions the runtime has to hear about are sent by `Editor::pump`, which runs once per
    # request — so the far end is given the same chance to catch up that an interface gives it.
    agent.settle(1.0)
    report.did(
        "undo back to an empty world",
        f"{undone} transactions reversed and the document holds no nodes",
    )
    return undone


def act_runtime_report(published: str, report: Report) -> None:
    """The engine's own account of the session, which is the second witness to all of it."""
    world = runtime_line(published, "world")
    play = runtime_line(published, "play")
    editor = runtime_line(published, "editor")
    expect(bool(world) and bool(play), f"the runtime printed no report:\n{published[-2000:]}")

    expect(
        "0 node(s) presented" in world,
        f"the engine's world is not empty after the undos: {world}",
    )
    expect("2 created" in world, f"the engine's world never gained the two entities: {world}")
    expect("2 deleted" in world, f"the undos did not reach the engine's world: {world}")
    report.did(
        "the engine's world is the editor's, and it empties with it",
        world,
    )

    expect("1 session(s)" in play, f"the runtime hosted no play session: {play}")
    expect("2 bodies" in play, f"the runtime built no bodies from the authored world: {play}")
    expect(
        "document identical after every stop" in play,
        f"stopping did not restore the engine's world exactly: {play}",
    )
    report.did("the runtime's account of play", play)
    report.did("what crossed the control socket", editor)

    same_frame = runtime_line(published, "same-frame")
    if same_frame:
        report.did("a committed edit and the frame that carried it", same_frame)


# --- The measurement -----------------------------------------------------------------------------


def measure(sample: Path, project: Path, journal: Path, write: Path, runs: int,
            physics: str) -> list[dict]:
    """Play the authored world headlessly, `runs` times, and read the numbers back."""
    measured: list[dict] = []
    for index in range(runs):
        arguments = [
            str(sample),
            "--world", str(project / WORLD),
            "--asset", WORLD,
            "--journal", str(journal),
            "--ticks", "600",
            "--physics", physics,
        ]
        if index == 0:
            arguments += ["--write", str(write)]
        finished = subprocess.run(arguments, capture_output=True, text=True, check=False)
        if finished.returncode != 0:
            raise Failed(
                f"cy_sample_authoring exited {finished.returncode}:\n"
                f"{finished.stdout}\n{finished.stderr}"
            )
        values: dict[str, str] = {}
        for line in finished.stdout.splitlines():
            if line.startswith("08a-authoring: ") and " = " in line:
                key, value = line[len("08a-authoring: "):].split(" = ", 1)
                values[key] = value
        expect(bool(values), f"cy_sample_authoring printed no measurements:\n{finished.stdout}")
        measured.append(values)
    return measured


def act_measure(measured: list[dict], report: Report) -> Statistic:
    """Tasks 6.4 to 6.6, with numbers: what fell, where it stopped, and what was put back."""
    first = measured[0]
    report.did(
        "the authored world, rebuilt from the editor's own transactions",
        f"{first['journal-transactions']} journalled transaction(s), "
        f"{first['journal-operations-applied']} operation(s) applied over a world of "
        f"{first['nodes-in-file']} nodes, leaving {first['nodes-after-replay']}",
    )
    report.did(
        "the solver, and the colliders it was given",
        f"{first['physics']}: a {first['dynamic-collider']} body and a "
        f"{first['static-collider']} one, {first['bodies']} bodies from "
        f"{first['entities']} entities",
    )

    # EVERY SESSION, not the first: two per program run, and the run is repeated. A resting height
    # that moved between them would mean a session left something behind.
    # IN MILLIMETRES, and that is not decoration. `artefact.Statistic` renders a figure between
    # 0.01 and 100000 to one decimal place, so a sphere resting at 0.7299 m and a contact height of
    # 0.75 m both print as "0.7 m" — two numbers 20 mm apart, rendered identically, in the line the
    # milestone is judged by. The claim is about centimetres, so the unit is millimetres.
    rests = [float(values["rest-height"]) * 1000.0 for values in measured]
    rests += [float(values["second-rest-height"]) * 1000.0 for values in measured]
    ticks = [float(values["ticks-to-rest"]) for values in measured]
    contact = float(first["contact-height"]) * 1000.0
    start = float(first["start-height"]) * 1000.0
    resting = Statistic.median("median resting height of the sphere", rests, "mm")

    report.figure(Statistic.stable("the height it was authored at", start, "mm"))
    report.figure(
        Statistic.stable("the contact height the two colliders imply", contact, "mm")
    )
    report.figure(resting)
    report.figure(Statistic.median("median ticks to come to rest", ticks))
    report.figure(
        Statistic.extreme("the lowest the sphere reached",
                          float(first["lowest-height"]) * 1000.0, "mm")
    )

    fell = start - resting.value
    expect(fell > 500.0, f"the sphere did not fall: it started at {start} mm and rests at {resting}")
    # A sphere that fell THROUGH the box rests metres below the contact height; one that rests ON it
    # sits within the solver's penetration allowance, which is tens of millimetres — Jolt's own
    # default slop is 20 mm and that is what this measures.
    apart = abs(resting.value - contact)
    if apart > 50.0:
        report.gap(
            "the sphere lands on the box",
            f"it came to rest at {resting.value:.1f} mm and the two colliders put contact at "
            f"{contact:.1f} mm, which is {apart:.0f} mm out — far enough that it went through",
        )
    else:
        report.did(
            "the sphere falls and lands on the box",
            f"{fell:.0f} mm in {ticks[0]:.0f} ticks, resting {apart:.0f} mm inside the contact "
            f"height the two colliders imply, which is the solver's penetration allowance",
        )

    spread = max(rests) - min(rests)
    expect(
        spread < 0.001,
        f"{len(rests)} sessions over one world came to rest {spread:.4f} mm apart, so one of them "
        "left something behind",
    )
    report.did(
        "a session leaves nothing behind",
        f"{len(rests)} sessions over one world, every one resting within "
        f"{spread * 1000:.0f} micrometres of the others",
    )

    for index, values in enumerate(measured):
        for key in ("restored-exactly", "session-claimed-exact", "second-restored-exactly"):
            expect(
                values[key] == "yes",
                f"run {index}: {key} is {values[key]} — stopping did not put the world back "
                f"(first difference at byte {values['restored-difference']})",
            )
        expect(
            values["total-restore"] == "no",
            f"run {index}: the session's own byte comparison failed and it re-read its snapshot",
        )
        expect(
            abs(float(values["height-after-stop"]) * 1000.0 - start) < 1e-3,
            f"run {index}: the sphere is at {values['height-after-stop']} after stop and was "
            f"authored at {start}",
        )
    report.did(
        "stop restores exactly, and the report agrees with the bytes",
        f"{len(measured)} run(s): the file's bytes at stop() equal its bytes at enter(), the "
        "session's own claim says the same, and no total restore was needed",
    )
    return resting


# --- The picture ---------------------------------------------------------------------------------


def act_photograph(binaries: dict, project: Path, authored: Path, work: Path, shot: Path,
                   report: Report) -> None:
    """Task 6.8 — the scene this session authored, in the editor, drawn by the engine.

    Deliberately small beside `samples/05b-editor-window`, which is the artefact ABOUT the window.
    This one wants the one thing the terminal cannot give: a picture of the world the acts above
    built, in the editor, with the engine's own frame in the viewport and the gizmo on a selected
    object — which is what "what you see is what ships" looks like.

    --- IT USES THE POINTER AND NOT THE KEYBOARD, AND THAT IS A FINDING ---------------------------

    `play.enter` is bound to `F5`, so pressing play in the window would be one synthesised key. On
    the X session this was built on, XTEST key events do not reach this editor's window at all —
    `Ctrl+P` opened no palette and `F5` reached no runtime, while the pointer works and every
    process involved was healthy. `samples/05b-editor-window` succeeds with keys on the same machine
    and carries about two hundred lines of focus, retry and screensaver machinery to do it, which is
    the difference. Rather than copy that here, this act does what the pointer can and says so:
    pressing play is proven in the session above, over the editor's own control socket, and how far
    the sphere fell is measured by `cy_sample_authoring`.
    """
    try:
        import PIL.Image  # noqa: F401 — `_capture` needs it, and this says which import is missing
        from Xlib import X, display
    except ImportError as missing:
        raise Absent(
            f"{missing.name} is not installed; the photograph needs python-xlib and Pillow"
        ) from missing

    name = os.environ.get("DISPLAY", "")
    if not name:
        raise Absent("there is no DISPLAY, so no window can be opened or photographed")
    if not binaries["runtime"].exists():
        raise Absent(
            "the engine runtime is not in this build, so the viewport would photograph its own "
            "refusal rather than the engine's frame"
        )
    try:
        server = display.Display(name)
    except Exception as problem:  # pragma: no cover — the absence path
        raise Absent(f"no X display at {name!r}: {problem}") from problem
    if not server.has_extension("XTEST"):
        raise Absent(f"the X server at {name!r} has no XTEST extension")

    # THE WORLD THE PHOTOGRAPH IS OF IS THE ONE THE SESSION AUTHORED, put on disk by the engine's
    # own writer. The session could not save it — `file.save` is an irreversible mutation and the
    # `author` agent scope does not grant one — so `cy_sample_authoring --write` wrote out the world
    # it rebuilt from the editor's journal, and this is where it becomes the project's.
    expect(authored.is_file(), "there is no authored world to photograph")
    (project / WORLD).write_bytes(authored.read_bytes())

    control = socket_path(work, "photograph.sock")
    viewport = socket_path(work, "photograph-viewport.sock")
    for path in (control, viewport):
        path.unlink(missing_ok=True)
    runtime = Runtime(binaries["runtime"], project, control, viewport, seconds=90.0)
    editor = subprocess.Popen(
        [str(binaries["editor"]), "--open", WORLD, "--host", str(control)],
        cwd=project,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        # CY_VIEWPORT_SOCKET is how the editor's window finds the frames the runtime publishes.
        # Without it the viewport reports the default path and DRAWS THE REASON, which is a
        # photograph of a disconnected transport rather than of the engine's world — and it looks
        # enough like a working editor to be committed by mistake.
        env=dict(os.environ, CY_IMPORT_CLI=str(binaries["importer"]),
                 CY_VIEWPORT_SOCKET=str(viewport)),
    )
    try:
        window = _await_window(server, X, editor.pid)
        _activate(server, X, window)
        # Long enough for the transport to hand over its first frames rather than its first
        # complaint: the editor draws the reason it has no image, and that is a photograph too.
        time.sleep(6.0)
        after = _capture(server, X, PIL.Image, window, shot)
        report.shot(shot)

        viewport, chrome = _lit(after, _VIEWPORT), _lit(after, _CHROME)
        expect(
            viewport > 0.10 and chrome < 0.02,
            "the viewport is not showing another process's frame: "
            f"{viewport * 100:.0f}% of it is lit and {chrome * 100:.0f}% of the editor's own "
            "chrome is, and those two numbers should be far apart",
        )
        report.did(
            "the editor's window, showing the world this session authored",
            f"the engine's frame in the viewport: {viewport * 100:.0f}% of it is lit against "
            f"{chrome * 100:.0f}% of the editor's charcoal chrome, so those pixels came out of "
            f"another process's GPU allocation. {relative(shot)} is the picture the milestone is "
            "judged by",
        )
        report.did(
            "and what the picture does NOT show",
            "the two entities are drawn as unit boxes. `samples/05b-editor-window/runtime` presents "
            "a world through M3's fixed scene slots and its mesh reference reaches no renderer yet "
            "(cy::render::MeshRenderer is a declared name with no reflected type), so the sphere "
            "above the box is a box above a box. What is real in the picture is the placement, the "
            "hierarchy and the transport",
        )
    finally:
        editor.terminate()
        try:
            editor.wait(timeout=20)
        except subprocess.TimeoutExpired:
            editor.kill()
        runtime.finish()


def _await_window(server, X, pid: int, seconds: float = 40.0):
    wanted = server.intern_atom("_NET_WM_PID")

    def search(node):
        try:
            value = node.get_full_property(wanted, X.AnyPropertyType)
            if value is not None and value.value and value.value[0] == pid:
                if node.get_attributes().map_state == X.IsViewable:
                    return node
            for child in node.query_tree().children:
                found = search(child)
                if found is not None:
                    return found
        except Exception:  # a window can disappear between the query and the read
            return None
        return None

    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        found = search(server.screen().root)
        if found is not None:
            return found
        time.sleep(0.2)
    raise Failed(f"the editor mapped no window within {seconds:.0f} s")


def _activate(server, X, window) -> None:
    from Xlib import protocol

    event = protocol.event.ClientMessage(
        window=window,
        client_type=server.intern_atom("_NET_ACTIVE_WINDOW"),
        data=(32, (2, X.CurrentTime, 0, 0, 0)),
    )
    server.screen().root.send_event(
        event, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask
    )
    window.configure(stack_mode=X.Above)
    server.sync()
    time.sleep(0.5)
    try:
        window.set_input_focus(X.RevertToParent, X.CurrentTime)
    except Exception:  # some window managers refuse, and the activate above has already done it
        pass
    server.sync()
    time.sleep(0.5)


def _capture(server, X, pillow, window, path: Path):
    geometry = window.get_geometry()
    raw = window.get_image(0, 0, geometry.width, geometry.height, X.ZPixmap, 0xFFFFFFFF)
    image = pillow.frombytes("RGB", (geometry.width, geometry.height), raw.data, "raw", "BGRX")
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)
    return image


#: The viewport and a slab of the editor's own chrome, as fractions of the window. Two rectangles
#: rather than one, because the claim is a COMPARISON: the viewport is bright and varied and the
#: chrome beside it is a flat charcoal, so the check fails both when the frame stops arriving and
#: when something makes the whole capture bright.
_VIEWPORT = (0.20, 0.09, 0.77, 0.68)
_CHROME = (0.00, 0.20, 0.17, 0.50)


def _lit(image, box: tuple[float, float, float, float], threshold: int = 96) -> float:
    """The share of sampled pixels in `box` brighter than `threshold`.

    `samples/05b-editor-window` counts SATURATED pixels for the same purpose; this counts lit ones,
    because M3's scene as this world view presents it is a grey checkerboard under a white light and
    is barely saturated at all — 0.1% of the window, which is not a number anything can be judged
    by. Brightness separates the two surfaces completely: 43% of the viewport is above this
    threshold and 0% of the editor's chrome is, because the chrome is a flat 22 out of 255.
    """
    width, height = image.size
    region = image.crop((
        int(width * box[0]), int(height * box[1]),
        int(width * box[2]), int(height * box[3]),
    ))
    pixels = region.tobytes()
    total = len(pixels) // 3
    step = max(1, total // 20000)
    seen = 0
    lit = 0
    for index in range(0, total, step):
        seen += 1
        if max(pixels[index * 3], pixels[index * 3 + 1], pixels[index * 3 + 2]) > threshold:
            lit += 1
    return lit / max(1, seen)


# --- Putting the run together -----------------------------------------------------------------------


def prepare(work: Path) -> Path:
    """A project of this artefact's own, in the build tree.

    Copied rather than used in place, for two reasons that are both about repeatability: the run
    writes `.cyprim` sources, cooked assets and a journal, and a source tree that accumulated them
    would make the second run of the artefact a different run from the first.
    """
    project = work / "project"
    if project.exists():
        shutil.rmtree(project)
    shutil.copytree(SAMPLE / "project", project)
    (project / ".cy" / "journal").mkdir(parents=True, exist_ok=True)
    return project


def locate(profile: str, build: bool, build_dir: str, sample: str) -> dict:
    """The four binaries this artefact drives, built first unless the caller supplied them."""
    if build:
        for recipe in ("build-editor", "build-engine"):
            subprocess.run(["just", recipe, "--profile", profile], cwd=ROOT, check=True)
    tree = Path(build_dir) if build_dir else ROOT / "build" / profile
    if not tree.is_absolute():
        tree = ROOT / tree
    cargo = {"dev": "development"}.get(profile, profile)
    editor = tree / "editor" / cargo / "cyberdyne-editor"
    if not editor.exists():
        found = sorted(tree.glob("editor/*/cyberdyne-editor"))
        editor = found[0] if found else editor
    found_sample = Path(sample) if sample else tree / "samples/08a-authoring/cy_sample_authoring"
    return {
        "editor": editor,
        "runtime": tree / "samples/05b-editor-window/runtime/cy_editor_window_runtime",
        "importer": tree / "tools/import/cy_import_cli",
        "sample": found_sample,
    }


def act_script_surface(binary: Path, project: Path, work: Path, report: Report) -> None:
    """The refusal that decided this driver's shape, performed rather than described.

    `cyberdyne-editor --script` is the obvious way to drive a session and cannot place anything: it
    makes every argument a `Value::Text` and the registry validates kinds. The agent surface parses
    the identical text into the declared kind (`cy_editor_agent::tool`), which is the one-line
    difference and the reason every act above goes over MCP.
    """
    script = work / "placement.cyscript"
    script.write_text("scene.create-entity\nscene.translate amount=0,3,0\n", encoding="utf-8")
    finished = subprocess.run(
        [str(binary), "--open", WORLD, "--script", str(script)],
        cwd=project, capture_output=True, text=True, check=False,
    )
    said = finished.stdout + finished.stderr
    if "declared vec3 and a text was supplied" not in said:
        report.gap(
            "the script surface refuses a typed argument",
            "this driver exists in its present shape because `cyberdyne-editor --script` could not "
            f"express a vec3, and this run did not get that refusal:\n{said.strip()[:400]}",
        )
        return
    report.did(
        "a .cyscript cannot place anything, and the agent surface can",
        "`scene.translate amount=0,3,0` from a script is refused — the parameter is declared vec3 "
        "and cy_editor_app::run_script makes every argument text — while the identical text over "
        "MCP is parsed into the declared kind by cy_editor_agent::tool. That one line is why this "
        "artefact drives the editor as an agent",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="dev")
    parser.add_argument("--work", default="", help="where the run's project and shots go")
    parser.add_argument("--build-dir", default="", help="the build tree to take binaries from")
    parser.add_argument("--sample", default="", help="cy_sample_authoring, when it is already built")
    parser.add_argument("--binaries", action="store_true",
                        help="the binaries are already built; do not build")
    parser.add_argument("--no-window", action="store_true", help="skip the photograph")
    parser.add_argument("--shot", default="", help="where the milestone's picture goes")
    parser.add_argument("--seconds", type=float, default=2.0, help="how long play runs for")
    parser.add_argument("--runs", type=int, default=3, help="how many times the fall is measured")
    # THE NEGATIVE CONTROL, and it is a real backend rather than a switch that fakes a failure.
    # `cy::physics::reference` declares `Capabilities::contact_resolution = false`: it integrates
    # motion and resolves nothing, so the sphere falls THROUGH the box. The run then records a gap
    # and returns 1, which is task 6.7's "a recorded gap exits non-zero" demonstrated rather than
    # asserted — and it is also the check that `physics`'s "swapping the backend changes no
    # gameplay" is being taken seriously rather than assumed.
    parser.add_argument("--physics", default="jolt", choices=("jolt", "reference"),
                        help="which solver the fall is measured in; reference is the negative "
                             "control and is expected to record a gap")
    options = parser.parse_args()

    work = Path(options.work) if options.work else ROOT / "build" / options.profile / "authoring"
    work = work if work.is_absolute() else ROOT / work
    work.mkdir(parents=True, exist_ok=True)
    shot = Path(options.shot) if options.shot else work / "shots" / "authoring-m8a.png"
    shot = shot if shot.is_absolute() else ROOT / shot

    report = Report()
    print("=== samples/08a-authoring — a scene a person builds by hand, and a game they can press "
          "play on ===\n")
    try:
        binaries = locate(options.profile, not options.binaries, options.build_dir, options.sample)
        for name in ("editor", "importer", "sample"):
            if not binaries[name].exists():
                raise Absent(f"{binaries[name]} was not built; there is nothing to drive")
        project = prepare(work)

        hosted = binaries["runtime"].exists()
        control = socket_path(work, "authoring.sock")
        viewport = socket_path(work, "authoring-viewport.sock")
        for path in (control, viewport):
            path.unlink(missing_ok=True)

        print("--- the session: one editor, one engine, six steps ---")
        # `--seconds` is the runtime's own lifetime, and it is generous on purpose: the session
        # below takes about four seconds, but this artefact also runs inside `just test-all` beside
        # every other suite, and a runtime that exited under load would take the editor's
        # transactions with it and report itself as a defect in the bridge.
        runtime = Runtime(binaries["runtime"], project, control, viewport,
                          seconds=180.0) if hosted else None
        if runtime is None:
            report.not_evaluated(
                "the engine on the far end",
                f"{relative(binaries['runtime'])} is not in this build — it needs a Vulkan device "
                "that can export a dma-buf — so the acts that need a runtime are not evaluated",
            )
        journal = project / ".cy" / "journal"
        agent = Agent(binaries["editor"], project, journal,
                      control if hosted else None, binaries["importer"])
        published = ""
        try:
            agent.call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                      "clientInfo": {"name": "cyberdyne-m8a-artefact",
                                                     "version": "1"}})
            act_empty(agent, project / WORLD, report)
            ground, ball = act_create(agent, project, report)
            act_place(agent, ball, report)
            act_bodies(agent, ground, ball, report)
            act_play(agent, hosted, report, options.seconds)
            act_undo(agent, report)
        finally:
            agent.close()
            if runtime is not None:
                published = runtime.finish()
        if runtime is not None:
            act_runtime_report(published, report)

        print("\n--- what fell, measured ---")
        journal_file = next(iter(sorted(journal.glob("*.cyjournal"))), None)
        expect(journal_file is not None, "the session wrote no journal, so nothing can replay it")
        authored = work / "authored.cyworld"
        measured = measure(binaries["sample"], project, journal_file, authored,
                           max(1, options.runs), options.physics)
        resting = act_measure(measured, report)
        expect(authored.is_file(), "the authored world was not written out")
        report.did(
            "the authored world, written back out by the engine",
            f"{relative(authored)}, {authored.stat().st_size} bytes",
        )

        print("\n--- the surfaces, and which of them can express a placement ---")
        act_script_surface(binaries["editor"], project, work, report)

        if not options.no_window:
            print("\n--- the picture ---")
            try:
                act_photograph(binaries, project, authored, work, shot, report)
            except Absent as absence:
                report.not_evaluated("the milestone's picture", str(absence))
        report.headline(resting)
    except Absent as absence:
        print(f"\n    n/a   {absence}")
        report.summarise(work)
        return 3
    except Failed as failure:
        report.failed(str(failure))
    return report.summarise(work)


if __name__ == "__main__":
    sys.exit(main())

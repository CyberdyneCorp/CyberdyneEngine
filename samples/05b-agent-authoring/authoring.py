#!/usr/bin/env python3
"""samples/05b-agent-authoring — the M5.5 artefact an agent drives. Task 4.2.

`just run-agent-authoring` is the recipe; `smoke.agent_authoring` is the CTest entry; this file is
what both of them run. It needs no display and no graphics device, which is why it is the half of
this milestone that runs in continuous integration.

--- WHAT IT IS ---------------------------------------------------------------------------------

One editor process, started with `--mcp`, and this driver on the other end of its standard input
speaking JSON-RPC 2.0 as an agent would. Nothing here reaches into the editor: every act below is a
`tools/call` or a `resources/read` over the wire, and the editor is the same binary a person opens a
window with. `design.md` §3 states the loop the milestone exists for:

    compose a scene  ->  write a gameplay script  ->  build and reload  ->  play  ->  LOOK  ->  decide

The acts follow it in that order, and the steps this tree cannot yet close are attempted and
reported by name rather than skipped, faked, or left out of the artefact.

--- WHY THE UNSATISFIED STEPS ARE IN THE OUTPUT RATHER THAN IN A FOOTNOTE -----------------------

samples/05-editor-session established the rule at M5 and it is the right one: an artefact that
quietly narrows its claim to what happens to work reports a milestone as closed that is not. So each
`report.gap` below names the call it wants, the refusal it currently gets, and the one change that
would close it — and every run makes the call. The day the change lands, the step SUCCEEDS and this
driver says so. Until then it fails the run if the refusal is still there but the REASON has moved,
because a gap whose reason changed is a different gap and the note recording it has gone stale.

An act that is expected to work and does not fails the run, always.

--- THE THREE STEPS THAT DO NOT CLOSE, AS OF THIS COMMIT ----------------------------------------

  1. `scene.translate` — "this document's schema declares no Transform component". Opening a
     document constructs an empty `Document`: `cy_editor_services::documents::DocumentService::open`
     calls `Document::new`, which is a name and an empty schema, because there is no world loader.
     Nothing in the tree outside a test ever calls `DocumentSchema::declare_type`. So a scene can be
     COMPOSED (entities are real, and undo restores them exactly) but nothing in it can be PLACED.

  2. `project.build` — "the scope \"author\" does not grant the effect class external-effect". That
     refusal is CORRECT and this driver asserts it: running a compiler is an effect undo cannot
     reach, and `editor-agent-interface` requires it to be granted deliberately. What is missing is
     a scope that grants it: `cy-editor-app/src/main.rs` offers `read` and `author` and no third.
     Until there is one, no agent connection can start a build, and `project.reload` therefore
     answers "nothing has been built".

  3. `viewport:` — "no frame has arrived from the runtime for this viewport". The viewport transport
     is real and measured, and this milestone's other artefact photographs the engine's own frame
     inside the editor's window. But the code that claims frames from it — `cy-editor-shell`'s
     `ViewportLink` — belongs to the window, and `--mcp` runs without one. So the LOOK step of the
     loop above has an implementation and no host in this configuration.

None of the three is a defect in this driver, and each is a few lines away in a file this artefact
does not own. They are reported by name, with the refusal, on every run.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

SAMPLE = Path(__file__).resolve().parent
ROOT = SAMPLE.parents[1]

# The document every act works in. An ASSET PATH rather than a file, for the reason
# samples/05-editor-session records: a document's identity is the asset it is the authoring form of,
# and writing that asset is `file.save`'s job at a later task.
WORLD = "worlds/opening.cyworld"

# The script the agent writes. Under `game/`, which is the one directory `--agent-scope author`
# grants — so a write outside it is refused with the scope as the reason, which act 5 requires.
SCRIPT_PATH = "game/Beacon.swift"
SCRIPT_SOURCE = """import CyberdyneEngine

/// A beacon that pulses. Written by an agent, through `source.write`, as one undoable transaction.
///
/// Deliberately small: what this artefact demonstrates is that an agent can put gameplay source
/// into a project through the same command a person uses, and take it back out again with undo.
@CyBehaviour
final class Beacon {
    var height: Float = 4.0
    var period: Float = 1.5

    func tick(_ context: CyTickContext) {
        // M4's model: gameplay is Swift, over the C ABI, reloaded per generation.
        height = 4.0 + sin(Float(context.elapsed) / period)
    }
}
"""


# --- What the driver reports ----------------------------------------------------------------------


@dataclass
class Step:
    """One thing the artefact claims, and whether this run saw it."""

    name: str
    detail: str = ""
    ok: bool = True


@dataclass
class Report:
    steps: list[Step] = field(default_factory=list)
    gaps: list[Step] = field(default_factory=list)
    # What the transcript screenshot is drawn from, so the image and the terminal cannot disagree.
    lines: list[tuple[str, str]] = field(default_factory=list)

    def act(self, title: str) -> None:
        self.lines.append(("act", f"— {title}"))
        print(f"--- {title} ---")

    def did(self, name: str, detail: str = "") -> None:
        self.steps.append(Step(name, detail))
        self.lines.append(("ok", f"  ok   {name}"))
        if detail:
            self.lines.append(("note", f"         {detail[:120]}"))
        print(f"    ok    {name}" + (f" — {detail}" if detail else ""))

    def gap(self, name: str, detail: str) -> None:
        self.gaps.append(Step(name, detail, ok=False))
        self.lines.append(("gap", f"  GAP  {name}"))
        self.lines.append(("note", f"         {detail.split('.')[0][:120]}"))
        print(f"    GAP   {name} — {detail}")

    @property
    def failed(self) -> bool:
        return any(not step.ok for step in self.steps)


class Failed(Exception):
    """An act that was expected to work did not."""


def expect(condition: bool, what: str) -> None:
    if not condition:
        raise Failed(what)


# --- The connection --------------------------------------------------------------------------------


class Agent:
    """One MCP connection, over the editor's standard input and output.

    Newline-delimited JSON-RPC 2.0, which is what `cy_editor_mcp::rpc` speaks. Deliberately hand
    written and dependency-free: this driver has to be a client the editor has never seen, and a
    client built out of the editor's own types would prove nothing about the wire.
    """

    def __init__(self, binary: Path, root: Path, scope: str, intent: str, journal: Path | None):
        arguments = [
            str(binary),
            "--open",
            WORLD,
            "--mcp",
            "--agent-scope",
            scope,
            "--agent-intent",
            intent,
        ]
        if journal is not None:
            arguments += ["--journal", str(journal)]
        self.scope = scope
        self.intent = intent
        self.process = subprocess.Popen(
            arguments,
            cwd=root,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=dict(os.environ, CY_AGENT="cyberdyne-sample-agent"),
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

    def tool(self, name: str, **arguments: str) -> tuple[bool, str, dict]:
        """Call a tool. Returns (ok, the text it said, its structured content)."""
        answer = self.call("tools/call", {"name": name, "arguments": arguments})
        if "error" in answer:
            raise Failed(f"{name}: the protocol refused the call: {answer['error']}")
        result = answer["result"]
        text = "\n".join(part.get("text", "") for part in result.get("content", ()))
        return not result.get("isError", False), text, result.get("structuredContent", {})

    def read(self, uri: str) -> tuple[bool, str]:
        """Read a resource. Returns (ok, its text) — a refusal comes back as a tool error would."""
        answer = self.call("resources/read", {"uri": uri})
        result = answer.get("result", {})
        if "contents" in result:
            return True, "\n".join(part.get("text", "") for part in result["contents"])
        text = "\n".join(part.get("text", "") for part in result.get("content", ()))
        return False, text

    def close(self) -> str:
        assert self.process.stdin is not None
        self.process.stdin.close()
        try:
            self.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.process.kill()
        return self.process.stderr.read() if self.process.stderr else ""


# --- The acts ---------------------------------------------------------------------------------------


def act_connect(agent: Agent, binary: Path, root: Path, report: Report) -> list[str]:
    """The agent connects, and every tool it is offered is one of the editor's own commands."""
    answer = agent.call(
        "initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {
            "name": "cyberdyne-sample-agent", "version": "1"}}
    )
    result = answer["result"]
    expect("protocolVersion" in result, "initialize answered no protocol version")
    expect(bool(result.get("instructions")), "initialize answered no instructions")
    report.did("initialize", f"protocol {result['protocolVersion']}, {result['serverInfo']['name']}")

    tools = [tool["name"] for tool in agent.call("tools/list")["result"]["tools"]]
    expect(bool(tools), "tools/list offered nothing")

    # THE CHECK THAT MATTERS, and it is why the same binary is run twice. `--list-commands` prints
    # the registry's own projection; `tools/list` is a map over it. A tool that is in one and not the
    # other is a hand-maintained entry, which is the first of the twelve patterns
    # `editor-agent-interface` calls forbidden — and it would be invisible from either side alone.
    listing = subprocess.run(
        [str(binary), "--list-commands"], cwd=root, capture_output=True, text=True, check=True
    ).stdout
    # `edit.select(entity: text) [read] — …`: the identifier is what precedes the parameter list.
    registered = {
        line.split("(", 1)[0].strip() for line in listing.splitlines() if line.strip()
    }
    invented = sorted(set(tools) - registered)
    expect(not invented, f"tools/list offers what the registry does not: {invented}")
    excluded = sorted(registered - set(tools))
    report.did(
        "tools/list is the registry",
        f"{len(tools)} tool(s), none invented, {len(excluded)} excluded by declaration",
    )

    resources = [entry["uri"] for entry in agent.call("resources/list")["result"]["resources"]]
    for wanted in ("selection:", "documents:", "history:", "budget:", "sources:", "viewport:"):
        expect(
            any(uri == wanted or uri.startswith(wanted) for uri in resources),
            f"resources/list offers no {wanted}",
        )
    report.did("resources/list", f"{len(resources)} resource(s)")
    return resources


def act_compose(agent: Agent, report: Report) -> list[str]:
    """The agent composes a scene from an empty project."""
    created: list[str] = []
    for _ in range(3):
        ok, text, values = agent.tool("scene.create-entity")
        expect(ok, f"scene.create-entity was refused: {text}")
        expect("entity" in values, "scene.create-entity named no entity")
        created.append(values["entity"])
    expect(len(set(created)) == 3, "three creations produced fewer than three identities")
    report.did("composed a scene", f"{len(created)} entities in an empty project")

    ok, text, _ = agent.tool("edit.select", entity=created[-1])
    expect(ok, f"edit.select was refused: {text}")

    ok, hierarchy = agent.read(
        next(uri for uri in agent.call("resources/list")["result"]["resources"]
             if uri["uri"].startswith("hierarchy:"))["uri"]
    )
    expect(ok, f"hierarchy: was refused: {hierarchy}")
    expect(hierarchy.count("\n") >= 3, f"the hierarchy does not show three entities:\n{hierarchy}")
    report.did("read the scene back", f"{hierarchy.count(chr(10))} line(s) of hierarchy")

    ok, selection = agent.read("selection:")
    expect(ok, f"selection: was refused: {selection}")
    expect(created[-1] in selection, "the selection does not name the entity that was selected")
    report.did("selection is one service", "the entity the agent selected is the editor's selection")
    return created


def act_place(agent: Agent, report: Report) -> None:
    """Place what was composed. An OPEN STEP — see the module note, gap 1."""
    ok, text, _ = agent.tool("scene.translate", amount="3 0 0")
    if ok:
        report.did("placed the selection", text.splitlines()[0])
        return
    expect(
        "declares no Transform component" in text,
        f"scene.translate was refused for an unrecorded reason:\n{text}",
    )
    report.gap(
        "scene.translate",
        "the document's schema declares no Transform, because opening a document builds an empty "
        "one — DocumentService::open calls Document::new and there is no world loader. The gizmo "
        "and the numeric entry are held by their own tests against a document that declares one",
    )


def act_author(agent: Agent, root: Path, report: Report) -> None:
    """The agent writes a gameplay script, and undo takes it back out."""
    path = root / SCRIPT_PATH
    if path.exists():
        path.unlink()

    ok, text, values = agent.tool("source.write", path=SCRIPT_PATH, contents=SCRIPT_SOURCE)
    expect(ok, f"source.write was refused: {text}")
    expect(path.is_file(), f"source.write reported success and wrote no file at {path}")
    expect(
        path.read_text(encoding="utf-8") == SCRIPT_SOURCE,
        "the file on disk is not what the agent wrote",
    )
    expect(values.get("created") == "true", "source.write did not report that it created the file")
    report.did("wrote a gameplay script", f"{SCRIPT_PATH}, {len(SCRIPT_SOURCE)} bytes")

    ok, sources = agent.read("sources:")
    expect(ok, f"sources: was refused: {sources}")
    expect(SCRIPT_PATH in sources, f"sources: does not list {SCRIPT_PATH}:\n{sources}")

    # `design.md` §4: a source edit "is a transaction, it carries an actor and an intent". This is
    # the half of that claim a document test cannot make — the FILE has to come back.
    ok, text, _ = agent.tool("edit.undo")
    expect(ok, f"edit.undo was refused: {text}")
    expect(not path.exists(), "undo left the file the agent created on disk")
    ok, text, _ = agent.tool("edit.redo")
    expect(ok, f"edit.redo was refused: {text}")
    expect(path.is_file(), "redo did not put the file back")
    expect(
        path.read_text(encoding="utf-8") == SCRIPT_SOURCE, "redo put back different contents"
    )
    report.did("a source edit is a transaction", "undo removed the file and redo restored it byte "
                                                 "for byte")


def act_attribute(agent: Agent, report: Report) -> None:
    """Every change the agent made carries the actor, the session and the intent. Task 5.5."""
    uri = next(
        entry["uri"]
        for entry in agent.call("resources/list")["result"]["resources"]
        if entry["uri"].startswith("history:")
    )
    ok, history = agent.read(uri)
    expect(ok, f"history: was refused: {history}")
    entries = [line for line in history.splitlines() if line[:1].isdigit()]
    expect(bool(entries), f"the history is empty after the agent changed things:\n{history}")
    for line in entries:
        expect("[agent]" in line, f"a history entry does not say an agent made it: {line}")
        expect(agent.intent in line, f"a history entry carries no intent: {line}")
        expect("session" in line, f"a history entry names no session: {line}")
    report.did(
        "history carries actor, session and intent",
        f"{len(entries)} entry/entries, every one attributed",
    )


def act_scope(agent: Agent, report: Report) -> None:
    """The two refusals that are the point rather than a limitation. Tasks 5.6 and 3.10."""
    ok, text, _ = agent.tool("source.write", path="src/engine/main.cpp", contents="// no")
    expect(not ok, "a write outside the connection's directories was allowed")
    expect(
        "does not include it" in text or "scope" in text,
        f"the refusal does not name the scope:\n{text}",
    )
    report.did("a write outside the scope is refused", "with the scope as the reason")

    ok, text, _ = agent.tool("project.build")
    if ok:
        report.did("the agent started a build", text.splitlines()[0])
        return
    expect(
        "external-effect" in text,
        f"project.build was refused for an unrecorded reason:\n{text}",
    )
    report.did(
        "an effect undo cannot reach is refused",
        'the scope "author" does not grant external-effect, which is the rule working',
    )
    report.gap(
        "project.build",
        "no --agent-scope grants external-effect, so no agent connection can start a build and "
        "project.reload therefore answers \"nothing has been built\". cy-editor-app/src/main.rs "
        "declares read and author and no third scope",
    )


def act_reload(agent: Agent, report: Report) -> None:
    """Reload what was built. An OPEN STEP that follows from the one above."""
    ok, text, _ = agent.tool("project.reload")
    if ok:
        report.did("reloaded the script module", text.splitlines()[0])
        return
    expect(
        "nothing has been built" in text,
        f"project.reload was refused for an unrecorded reason:\n{text}",
    )
    report.gap(
        "project.reload",
        "nothing has been built, because the step above is refused. The reload path itself is real: "
        "cy_editor_protocol carries Reload/Reloaded and RuntimeSession::reload sends them",
    )


def act_play(agent: Agent, report: Report) -> None:
    """Play mode entered and left. Task 3.8."""
    ok, text, values = agent.tool("play.enter")
    expect(ok, f"play.enter was refused: {text}")
    expect(values.get("play") == "playing", f"play.enter did not report playing: {text}")
    ok, state = agent.read("play:")
    expect(ok, f"play: was refused: {state}")
    ok, text, values = agent.tool("play.leave")
    expect(ok, f"play.leave was refused: {text}")
    expect(values.get("play") == "editing", f"play.leave did not report editing: {text}")
    report.did("entered and left play", state.replace("\n", " ").strip())


def act_observe(agent: Agent, out: Path, report: Report) -> None:
    """LOOK — the step that makes it authoring rather than data entry. An OPEN STEP; gap 3."""
    answer = agent.call("resources/read", {"uri": "viewport:"})
    result = answer.get("result", {})
    blobs = [part for part in result.get("contents", ()) if "blob" in part]
    if blobs:
        import base64

        image = base64.b64decode(blobs[0]["blob"])
        expect(len(image) > 8, "viewport: answered an empty image")
        out.write_bytes(image)
        report.did("looked at what it built", f"{len(image)} bytes, {blobs[0].get('mimeType')}")
        return
    text = "\n".join(part.get("text", "") for part in result.get("content", ()))
    expect(
        "no frame has arrived" in text,
        f"viewport: was refused for an unrecorded reason:\n{text}",
    )
    report.gap(
        "viewport:",
        "no frame has arrived: --mcp runs without a ViewportLink, which lives in cy-editor-shell "
        "with the window. The transport itself is real and samples/05b-editor-window photographs "
        "the engine's own frame inside the editor",
    )


def act_budget(agent: Agent, report: Report) -> None:
    """What the connection has spent, reported to the agent. Task 3.12."""
    ok, budget = agent.read("budget:")
    expect(ok, f"budget: was refused: {budget}")
    expect("invocations" in budget, f"budget: reports no invocation count:\n{budget}")
    expect(agent.scope in budget, "budget: does not name the connection's scope")
    report.did("budget is reported", budget.splitlines()[0])


# --- The committed screenshot ---------------------------------------------------------------------------
#
# TASK 4.3 ASKS FOR A SCREENSHOT OF EACH ARTEFACT, AND THIS ONE HAS NO WINDOW. What an agent sees of
# the editor is a JSON-RPC conversation, so what is photographed is the conversation — the driver's
# own transcript, rendered in the editor's own palette so the two images in docs/design/images/ read
# as one product rather than as a screenshot and a terminal dump.
#
# Rendered by the artefact rather than captured by hand, for the same reason the window's reference
# is: an image nobody can regenerate is an image that decays. Re-running this with `--shot` refreshes
# it, and a step that started or stopped working changes the picture.

# The dark theme's surfaces and semantic roles, from `cy_editor_visual::colour`. Restated as numbers
# because this driver may not depend on the editor's crates — it is a client, and a client that
# linked the editor's types would be the editor agreeing with itself.
INK = {
    "window": (0x0E, 0x10, 0x12),
    "panel": (0x16, 0x19, 0x1C),
    "raised": (0x1E, 0x22, 0x27),
    "primary": (0xE6, 0xE9, 0xEC),
    "secondary": (0xA2, 0xAB, 0xB4),
    "live": (0x35, 0xC0, 0x7C),
    "warning": (0xF0, 0x91, 0x3A),
    "selection": (0xE5, 0xB9, 0x5C),
    "active": (0x4C, 0x9A, 0xFF),
}


def render_transcript(report: Report, lines: list[tuple[str, str]], path: Path) -> None:
    """Draw the session as an image, in the editor's palette."""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print(f"    (no Pillow, so no screenshot at {path})", file=sys.stderr)
        return

    font = _monospace(15)
    bold = _monospace(15)
    pitch, margin, top = 22, 28, 74
    width = 1280
    height = top + pitch * (len(lines) + 3) + margin
    image = Image.new("RGB", (width, height), INK["window"])
    draw = ImageDraw.Draw(image)

    draw.rectangle([0, 0, width, 46], fill=INK["panel"])
    draw.text((margin, 14), "CyberEngine", font=bold, fill=INK["primary"])
    draw.text(
        (margin + 120, 15),
        "samples/05b-agent-authoring — one agent, over MCP, on the editor's standard input",
        font=font,
        fill=INK["secondary"],
    )
    draw.rectangle([margin - 12, top - 12, width - margin + 12, height - margin + 4],
                   fill=INK["panel"])

    y = top
    for kind, text in lines:
        colour = {
            "ok": INK["live"],
            "gap": INK["warning"],
            "act": INK["selection"],
            "note": INK["secondary"],
        }.get(kind, INK["primary"])
        draw.text((margin, y), text[:132], font=font, fill=colour)
        y += pitch

    y += pitch
    draw.text(
        (margin, y),
        f"{len(report.steps)} step(s) satisfied · {len(report.gaps)} not satisfied, each named",
        font=font,
        fill=INK["active"],
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)
    print(f"    shot  {path}")


def _monospace(size: int):
    from PIL import ImageFont

    for candidate in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    ):
        if Path(candidate).is_file():
            return ImageFont.truetype(candidate, size)
    return ImageFont.load_default()


# --- Running it --------------------------------------------------------------------------------------


def editor_binary(profile: str, build: bool) -> Path:
    """The editor built in `profile`, building it first for the reason every run recipe does."""
    if build:
        subprocess.run(["just", "build-editor", "--profile", profile], cwd=ROOT, check=True)
    cargo_profile = subprocess.run(
        ["just", "_cargo-profile", profile], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.strip()
    target = subprocess.run(
        ["just", "_editor-target-dir"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.strip()
    directory = "debug" if cargo_profile == "dev" else cargo_profile
    binary = Path(target) / directory / "cyberdyne-editor"
    if not binary.is_file():
        raise Failed(
            f"no editor at {binary}. Build it with: just build-editor --profile {profile}"
        )
    return binary


def prepare(work: Path) -> Path:
    """A copy of the empty project, so a run never edits the committed one.

    The agent WRITES into this tree — that is the milestone — and an artefact that left its output
    in the source directory would make the second run start from the first run's result.
    """
    root = work / "project"
    if root.exists():
        shutil.rmtree(root)
    shutil.copytree(SAMPLE / "project", root)
    (root / "worlds").mkdir(exist_ok=True)
    return root


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="dev")
    parser.add_argument("--build", action="store_true", help="build the editor first")
    parser.add_argument("--work", default="", help="where the run's copy of the project goes")
    # The work tree is never removed: it is under the build directory, it holds the project the
    # agent wrote into, and it is the first thing a person looks at after a failure. The flag is kept
    # so a caller that passes it is not refused, and it says so.
    parser.add_argument("--keep", action="store_true",
                        help="accepted and always true: the work directory is never removed")
    # See "The committed screenshot" above: task 4.3's image for an artefact that has no window.
    parser.add_argument("--shot", default="", help="render the transcript here, as an image")
    options = parser.parse_args()

    # CY_BUILD_DIR, not `build/`. Every recipe in this tree honours it and a driver that wrote into
    # `build/` regardless would put one run's output into another agent's tree.
    default = ROOT / os.environ.get("CY_BUILD_DIR", "build") / "agent-authoring"
    work = Path(options.work).resolve() if options.work else default
    work.mkdir(parents=True, exist_ok=True)
    report = Report()

    try:
        binary = editor_binary(options.profile, options.build)
    except (Failed, subprocess.CalledProcessError) as problem:
        print(f"agent-authoring: {problem}", file=sys.stderr)
        return 2

    root = prepare(work)
    print(f"==> agent-authoring  profile={options.profile}  project={root}")
    print(f"    editor           {binary}")

    agent = Agent(
        binary,
        root,
        scope="author",
        intent="compose the opening scene and give it a beacon",
        journal=work / "journal",
    )
    started = time.monotonic()
    try:
        report.act("act 1: the agent connects, and its tools are the editor's commands")
        act_connect(agent, binary, root, report)
        report.act("act 2: it composes a scene from an empty project")
        act_compose(agent, report)
        act_place(agent, report)
        report.act("act 3: it writes a gameplay script, and undo takes it back out")
        act_author(agent, root, report)
        report.act("act 4: every change it made is attributed")
        act_attribute(agent, report)
        report.act("act 5: scope and effect class")
        act_scope(agent, report)
        act_reload(agent, report)
        report.act("act 6: play, and what it can see of the result")
        act_play(agent, report)
        act_observe(agent, work / "agent-viewport.png", report)
        act_budget(agent, report)
    except Failed as problem:
        print(f"\nagent-authoring: {problem}", file=sys.stderr)
        stderr = agent.close()
        if stderr.strip():
            print(f"the editor said:\n{stderr}", file=sys.stderr)
        return 1

    stderr = agent.close()
    seconds = time.monotonic() - started

    print(f"\n--- the loop, in {seconds:.2f} s ---")
    print(f"    {len(report.steps)} step(s) satisfied")
    if report.gaps:
        print(f"    {len(report.gaps)} step(s) NOT SATISFIED, each named above and in README.md:")
        for gap in report.gaps:
            print(f"      · {gap.name}")
        print(
            "    They are not defects in this artefact. Each is re-attempted on every run and will\n"
            "    report as satisfied the day the change it names lands."
        )
    if stderr.strip():
        print(f"    the editor said: {stderr.strip().splitlines()[0]}")
    if options.shot:
        render_transcript(report, report.lines, Path(options.shot))
    return 1 if report.failed else 0


if __name__ == "__main__":
    sys.exit(main())

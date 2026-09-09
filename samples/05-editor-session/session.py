#!/usr/bin/env python3
"""samples/05-editor-session — the M5 artefact, as a scripted session across three processes.

Tasks 6.1 and 6.2. `just run-editor-session` is the recipe; `smoke.editor_session` is the CTest
entry; this file is what both of them run.

--- WHY THE ARTEFACT IS A DRIVER AND NOT A `cy_sample_*` BINARY ---------------------------------

Every earlier milestone's artefact is one executable, because every earlier milestone's subject was
the engine. M5's subject is the boundary between two processes written in two languages, and the
claim being demonstrated — "kill the hosted runtime mid-session and the editor survives with the
document intact" — is a statement about processes. A C++ program that started an editor and a
runtime and killed one of them would link no engine code and prove nothing about the engine; what it
would add is a build dependency that says nothing. So the artefact is the orchestration itself, in
the language this tree already orchestrates multi-toolchain artefacts in — `bindings/swift/tools/`
and `samples/04-character/tools/` are the same shape.

The three processes, and none of them is a stand-in for another:

    cy_import_cli        the engine's importer (tools/import/), which cooks the glTF.
    cy-runtime-stub      the hosted runtime, over a Unix domain socket. A stub: it speaks the live
                         bridge's message set and holds no world. The engine's own hosted runtime is
                         `live-editing`'s and arrives over the same encoding, so the editor's half of
                         this session does not change when it does.
    cyberdyne-editor     the editor, which reaches the engine only over the C ABI and this socket.

--- HOW THE KILL IS TIMED, WHICH IS THE ONLY SUBTLE THING HERE ----------------------------------

A scripted editor session is over in about three milliseconds, so "kill the runtime while the editor
is mid-session" cannot be timed by sleeping — the first attempt at this raced and killed the runtime
after the editor had already exited, which reports the claim as proven having proven nothing.

The editor reads its script with `std::fs::read_to_string`, and `--script` is a path. So the act is
delivered through a FIFO: opening a FIFO for reading blocks until a writer opens it, and reading it
blocks until the writer closes. `cyberdyne-editor` attaches its runtime BEFORE it reads the script
(see its `main.rs`), so the ordering below is decided rather than raced:

    1. the editor connects to the runtime            — observed, not assumed: the runtime's open
                                                       file descriptors go up when it accepts
    2. the runtime is killed with SIGKILL            — the editor is blocked on the FIFO, connected
    3. the act is written to the FIFO and closed     — the whole session now runs with a dead runtime
    4. the editor finishes it, surfaces the loss, and exits zero

The alternative would have been a `--pause-here` flag in the editor, which is a test hook in
shipping code. A FIFO is a property of the operating system, and the editor has no idea it is being
held.

--- WHAT THIS SESSION DOES NOT DO, STATED HERE RATHER THAN IN A FOOTNOTE -------------------------

The M5 task list asks the session to manipulate the asset with gizmos and to enter play mode.
Neither is on the editor's command surface at the time of writing: `cy_editor_services::builtin`
registers six commands and none of them transforms anything or starts a simulation, although the
types behind both exist and are tested (`cy_editor_viewport::gizmo`, `::play`). Rather than fake it,
`OPTIONAL_STEPS` below names the commands the artefact wants, checks the registry for each one, runs
it when it is there, and prints it as missing when it is not — so the gap is visible in the output of
every run and closes itself the day somebody registers them. README.md says the same thing at
greater length.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

SAMPLE = Path(__file__).resolve().parent
ROOT = SAMPLE.parents[1]

# The document every act opens. It is an ASSET PATH and not a file: a document's identity is the
# asset it is the authoring form of, and writing that asset is `file.save`'s job at a later task —
# the command's own source says it writes nothing yet. Naming a file that does not exist would be
# less honest than naming the identity that does.
WORLD = "worlds/lamppost.cyworld"

# What `cy_import` must produce out of project/assets/lamppost.gltf, by name. The ids are assigned
# once and remembered, so they are not checked here; the NAMES are what acts/01-place.cyscript
# assumes, and a change to either side that does not change the other should fail this run.
EXPECTED_SUB_ASSETS = (
    "collision/Lamppost_collision",
    "material/PaintedIron",
    "mesh/Collider",
    "mesh/Lantern",
    "mesh/Post",
    "prefab",
)

# The commands the session would use if the editor had them, with what each would demonstrate. Every
# run prints which are present and which are not; see the module note.
#
# A `None` invocation means REPORT ITS PRESENCE AND DO NOT RUN IT HERE. `asset.import` landed at M8.a
# and is the first entry to need that: this session runs the editor with the REPOSITORY as its
# working directory, so the editor has no project open and `assets/lamppost.gltf` is not a path it
# can resolve — act 2 already cooks that file, by running `cy_import_cli` against the sample's own
# project. Where the command itself is exercised is
# `editor/crates/cy-editor-services/tests/importing_from_inside_the_editor.rs`, which drives it
# through the registry and asserts the entity it creates.
OPTIONAL_STEPS = (
    ("viewport.transform", "scene.create-entity", "a gizmo drag, as a transform transaction"),
    ("runtime.play", "runtime.play", "entering play mode"),
    ("asset.import", None, "importing from inside the editor"),
)


class SessionError(RuntimeError):
    """An assertion this session makes about the editor, the importer or the runtime."""


@dataclass
class Report:
    """What the session observed, printed as one JSON line so a machine caller can read it."""

    acts: dict = field(default_factory=dict)
    missing_commands: list = field(default_factory=list)
    #: The command identifiers the editor's registry actually has, read from `--list-commands`.
    present: set = field(default_factory=set)

    def record(self, act: str, **facts: object) -> None:
        self.acts[act] = facts


def expect(condition: object, what: str, detail: str = "") -> None:
    """One assertion. The message names what was expected, because a failure here is read by
    somebody who was not watching the run."""
    if not condition:
        raise SessionError(what + (f"\n    {detail}" if detail else ""))


def say(line: str) -> None:
    print(line, flush=True)


# --- Finding the three binaries -------------------------------------------------------------------


@dataclass(frozen=True)
class Binaries:
    editor: Path
    runtime: Path
    importer: Path


def cargo_directory(profile: str) -> Path:
    """Where `just build-editor --profile <p>` put its output.

    The workflow-profile-to-Cargo-profile mapping is read out of the justfile through the recipe
    that owns it rather than copied here, because a second copy of that table is a second thing to
    keep in step — and getting it wrong runs a binary from a different profile than the one that was
    just built, which is the class of mistake that makes a measurement meaningless.
    """
    cargo_profile = run(["just", "_cargo-profile", profile], capture=True).strip()
    target = Path(os.environ.get("CY_BUILD_DIR", "build")) / "editor"
    # Cargo's own `dev` profile writes to `debug/`; every other profile writes to its own name.
    return ROOT / target / ("debug" if cargo_profile == "dev" else cargo_profile)


def locate(arguments: argparse.Namespace) -> Binaries:
    if arguments.build:
        # The EDITOR only, and deliberately not the engine's tools. Cargo has a build tree of its
        # own, so building it here touches nothing CMake owns; running `just build-tools` from
        # inside a CTest run would reconfigure the very tree ctest is reading its test list out of.
        # `smoke.editor_session` is handed `--import-cli $<TARGET_FILE:cy_import_cli>` instead, so
        # the importer is built by the build, like every other test's dependencies.
        say(f"==> build       the editor, profile={arguments.profile}")
        run(["just", "build-editor", "--profile", arguments.profile])

    directory = Path(arguments.editor_dir) if arguments.editor_dir else cargo_directory(
        arguments.profile
    )
    editor, runtime = directory / "cyberdyne-editor", directory / "cy-runtime-stub"
    importer = Path(arguments.import_cli) if arguments.import_cli else (
        ROOT / os.environ.get("CY_BUILD_DIR", f"build/{arguments.profile}")
        / "tools/import/cy_import_cli"
    )
    for path, recipe in ((editor, "just build-editor"), (runtime, "just build-editor"),
                         (importer, "just build-tools")):
        expect(path.is_file(), f"{path.name} is not at {path}",
               f"Build it with: {recipe} --profile {arguments.profile}")
    return Binaries(editor=editor, runtime=runtime, importer=importer)


def run(command: list, capture: bool = False, cwd: Path | None = None) -> str:
    completed = subprocess.run(command, cwd=cwd or ROOT, text=True, check=False,
                               stdout=subprocess.PIPE if capture else None)
    expect(completed.returncode == 0, f"`{' '.join(str(part) for part in command)}` exited "
                                      f"{completed.returncode}")
    return completed.stdout or ""


# --- Act 1: the project ---------------------------------------------------------------------------


def act_project(work: Path, report: Report) -> Path:
    """Copy the project somewhere writable and check its manifest with the engine's own validator.

    THE COPY IS NOT TIDINESS. `cy_import` writes `<source>.meta` and `<source>.import` beside the
    source it imported, which is correct — they are committed project files — and would mean that
    running the artefact dirtied this repository. A session works on a copy for the same reason a
    build works in a build directory.
    """
    project = work / "project"
    shutil.copytree(SAMPLE / "project", project)
    say("==> act 1       the project")
    manifest = project / "project.json"
    line = run([sys.executable, str(ROOT / "tools/project/project.py"), "validate",
                "--manifest", str(manifest)], capture=True).strip()
    say(f"    {line}")
    expect("the project graph is valid" in line, "the sample's project manifest validates")
    report.record("project", manifest=str(manifest.relative_to(work)), valid=True)
    return project


# --- Act 2: the import ----------------------------------------------------------------------------


def import_once(binaries: Binaries, project: Path, work: Path) -> str:
    return run([str(binaries.importer), "--project", str(project), "--out", str(work / "cooked"),
                "--cache", str(work / "cache"), "assets/lamppost.gltf"], capture=True)


def sidecar_values(path: Path, prefix: str) -> dict:
    """The `key = "value"` lines of a sidecar. Small on purpose: the sidecar's own reader is C++,
    and a second full parser here would be a second thing to keep in agreement with it."""
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#"):
            continue
        if line.startswith(prefix) and "=" in line:
            key, _, value = line.partition("=")
            values[key.strip().removeprefix(prefix).strip('."')] = value.strip().strip('"')
    return values


def act_import(binaries: Binaries, project: Path, work: Path, report: Report) -> None:
    """Import the glTF, then import it again. The second run must be a cache hit."""
    say("==> act 2       import assets/lamppost.gltf")
    cold, warm = import_once(binaries, project, work), import_once(binaries, project, work)
    say("    " + cold.splitlines()[0])
    expect("0 error(s)" in cold, "the import reports no errors", cold)
    expect("0 hit, 1 miss" in cold, "a first import is a cache miss", cold)
    expect("1 hit, 0 miss" in warm, "importing an unchanged source again is a cache hit", warm)

    source = project / "assets/lamppost.gltf"
    meta, record = Path(f"{source}.meta"), Path(f"{source}.import")
    expect(meta.is_file(), "the engine's identity sidecar was written", str(meta))
    expect(record.is_file(), "the importer's own record was written", str(record))

    identity = sidecar_values(meta, "")
    cooked_hash = identity.get("cooked_hash", "")
    # `AssetMeta::cooked_hash` was zero in every sidecar this project had ever written, because
    # nothing cooked before M5. This is the artefact's check that the import pipeline is what fills
    # it in — a zero here means the asset was registered and never cooked.
    expect(cooked_hash and set(cooked_hash) != {"0"}, "the identity sidecar carries a cooked hash",
           f"cooked_hash = {cooked_hash!r}")

    names = tuple(sorted(sidecar_values(record, "sub_asset")))
    expect(names == EXPECTED_SUB_ASSETS, "the import produced the sub-assets act 1 places",
           f"got {names}\n    want {EXPECTED_SUB_ASSETS}")
    say(f"    {len(names)} sub-asset(s): {', '.join(names)}")
    report.record("import", sub_assets=list(names), cooked_hash=cooked_hash, cache_hit=True)


# --- The editor ------------------------------------------------------------------------------------


@dataclass
class EditorRun:
    exit_code: int
    output: str

    @property
    def summaries(self) -> list:
        """The one-line result of each command that ran.

        The editor prints one per command, then its notifications, then one closing status line;
        this is the first group. Counting it is how an act asserts that the session did not stop
        early — `run_script` stops at the first command that fails, so a short list is a failure
        that would otherwise be invisible behind a zero exit code.
        """
        return [line for line in self.output.splitlines()
                if line and not line.startswith(("Error:", "Warning:", "Info:", "    "))
                and "document(s) open" not in line]

    def says(self, text: str) -> bool:
        return text in self.output


def editor_command(binaries: Binaries, work: Path, script: Path, socket: Path | None) -> list:
    command = [str(binaries.editor), "--open", WORLD, "--journal", str(work / "journal"),
               "--script", str(script)]
    if socket is not None:
        command += ["--host", str(socket)]
    return command


def run_editor(binaries: Binaries, work: Path, script: Path,
               socket: Path | None = None) -> EditorRun:
    completed = subprocess.run(editor_command(binaries, work, script, socket), cwd=ROOT, text=True,
                               check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return EditorRun(exit_code=completed.returncode, output=completed.stdout)


def registered_commands(binaries: Binaries) -> set:
    listing = run([str(binaries.editor), "--list-commands"], capture=True)
    return {line.split("(", 1)[0] for line in listing.splitlines() if "(" in line}


def act_from(script: Path, extra: list) -> str:
    return script.read_text(encoding="utf-8") + "".join(f"{line}\n" for line in extra)


# --- The hosted runtime, and killing it -------------------------------------------------------------


_SOCKETS: Path | None = None


def socket_path(name: str) -> Path:
    """A Unix domain socket path short enough to bind.

    `sun_path` holds 108 bytes on Linux and 104 on macOS, and a scratch directory under a build tree
    or a continuous-integration workspace is easily longer than that. The failure is the runtime
    refusing to listen — `path must be shorter than SUN_LEN` — several acts into a run, which is a
    confusing way to discover that a path was long. So the sockets live in their own short temporary
    directory rather than beside the work they belong to.
    """
    global _SOCKETS  # noqa: PLW0603 — one directory per process, created on first use
    if _SOCKETS is None:
        _SOCKETS = Path(tempfile.mkdtemp(prefix="cy-session-"))
    return _SOCKETS / name


def discard_sockets() -> None:
    if _SOCKETS is not None:
        shutil.rmtree(_SOCKETS, ignore_errors=True)


def start_runtime(binaries: Binaries, socket: Path) -> subprocess.Popen:
    """Start the runtime and wait for it to say it is listening, rather than sleeping."""
    runtime = subprocess.Popen([str(binaries.runtime), str(socket)], stdout=subprocess.PIPE,
                               text=True)
    ready = runtime.stdout.readline().strip()
    expect(ready == "listening", "the hosted runtime is listening", f"it said {ready!r}")
    return runtime


def wait_until_connected(runtime: subprocess.Popen, timeout_s: float = 10.0) -> bool:
    """Whether the editor has reached the runtime, observed rather than assumed.

    A runtime that has accepted a connection holds two more file descriptors than one that has not.
    Reading that costs nothing and needs no cooperation from either process. Where there is no
    procfs — a host that is not Linux — this returns False and the caller kills anyway: the editor is
    blocked on the FIFO either way, and the assertions that follow are what actually decide the run.
    """
    descriptors = Path(f"/proc/{runtime.pid}/fd")
    try:
        base = len(list(descriptors.iterdir()))
    except OSError:
        return False
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            if len(list(descriptors.iterdir())) > base:
                return True
        except OSError:
            return False
        time.sleep(0.001)
    return False


def kill(runtime: subprocess.Popen) -> None:
    """SIGKILL, because a crash is what is being tested and a crash runs no shutdown path."""
    runtime.send_signal(signal.SIGKILL)
    runtime.wait(timeout=30)


# --- Act 3: authoring with a runtime attached ---------------------------------------------------------


def act_author(binaries: Binaries, work: Path, report: Report,
               with_runtime: bool = True) -> None:
    """`with_runtime` is False only in this file's selftest, where the editor is pointed at a socket
    nothing is listening on and the assertions below must refuse the run."""
    say("==> act 3       author the scene, with a hosted runtime attached")
    socket = socket_path("author.sock")
    runtime = start_runtime(binaries, socket) if with_runtime else None
    try:
        session = run_editor(binaries, work, SAMPLE / "acts/01-place.cyscript", socket)
    finally:
        if runtime is not None:
            kill(runtime)
    for line in session.output.splitlines():
        say(f"    {line}")
    expect(session.exit_code == 0, "the editor exits zero", session.output)
    expect(session.says("engine: hosted"), "the session ran against the hosted runtime",
           session.output)
    expect(len(session.summaries) == 3, "every line of act 1 ran", session.output)
    expect(session.says("1 unsaved"), "the document carries the act's unsaved work", session.output)
    report.record("author", entities=3, hosting="hosted", exit_code=0)


# --- Act 4: the runtime dies mid-session. Task 6.2 ------------------------------------------------------


def act_survive(binaries: Binaries, work: Path, report: Report, kill_runtime: bool = True) -> None:
    """The milestone's claim, demonstrated rather than asserted.

    `kill_runtime` is False only in this file's own selftest, which requires the assertions below to
    fail when the runtime is left alive — a check that has never been seen to fire is a check nobody
    should trust.
    """
    say("==> act 4       the hosted runtime is killed mid-session")
    socket, fifo = socket_path("survive.sock"), work / "act-02.cyscript"
    os.mkfifo(fifo)
    runtime = start_runtime(binaries, socket)
    editor = subprocess.Popen(editor_command(binaries, work, fifo, socket), cwd=ROOT, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        connected = wait_until_connected(runtime)
        say(f"    the editor reached the runtime: {'observed' if connected else 'not observable'}")
        if kill_runtime:
            kill(runtime)
            say(f"    SIGKILL to the runtime (pid {runtime.pid}); the editor is still running: "
                f"{editor.poll() is None}")
            expect(editor.poll() is None, "the editor outlives the runtime")
        # Only now does the act reach the editor, so every command in it runs after the kill.
        extra = [line for identifier, line, _ in OPTIONAL_STEPS
                 if line is not None and identifier in report.present]
        fifo.write_text(act_from(SAMPLE / "acts/02-manipulate.cyscript", extra), encoding="utf-8")
        session = EditorRun(exit_code=editor.wait(timeout=120), output=editor.stdout.read())
    finally:
        editor.stdout.close()
        if runtime.poll() is None:
            kill(runtime)
    for line in session.output.splitlines():
        say(f"    {line}")

    expect(session.exit_code == 0, "the editor exits zero with its runtime dead", session.output)
    expect(len(session.summaries) == 6 + len(extra), "every line of act 2 ran after the kill",
           session.output)
    expect(session.says("The hosted runtime stopped"),
           "the loss is surfaced rather than silent", session.output)
    expect(session.says("restart the runtime"),
           "and it is surfaced with something to act on", session.output)
    expect(session.says("1 document(s) open, 1 unsaved"),
           "the document is intact and still unsaved", session.output)
    report.record("survive", killed=kill_runtime, connection_observed=connected,
                  commands=len(session.summaries), exit_code=0)


# --- Act 5: recovery, and the save that clears the journal -------------------------------------------------


def act_recover(binaries: Binaries, work: Path, report: Report) -> None:
    say("==> act 5       a new editor over the same journal: recovery is offered, then saved")
    session = run_editor(binaries, work, SAMPLE / "acts/03-recover-and-save.cyscript")
    for line in session.output.splitlines():
        say(f"    {line}")
    expect(session.exit_code == 0, "the editor exits zero", session.output)
    expect(session.says("recoverable transaction(s) from a previous session"),
           "the unsaved work of the previous acts is offered back", session.output)
    expect(session.says("0 unsaved"), "and the session ends saved", session.output)

    settled = run_editor(binaries, work, work / "empty.cyscript")
    expect(not settled.says("recoverable"),
           "saving discarded the journal, so there is nothing left to recover", settled.output)
    say("    reopened: nothing left to recover")
    report.record("recover", offered=True, saved=True, journal_cleared=True)


# --- The whole session -------------------------------------------------------------------------------------


def report_optional(binaries: Binaries, report: Report) -> None:
    """What the artefact would do if the command surface had it. See the module note."""
    report.present = registered_commands(binaries)
    for identifier, _, demonstrates in OPTIONAL_STEPS:
        if identifier in report.present:
            say(f"    {identifier:<20} present — {demonstrates}")
        else:
            say(f"    {identifier:<20} NOT ON THE COMMAND SURFACE — {demonstrates}")
            report.missing_commands.append(identifier)


def session(binaries: Binaries, work: Path, only: str | None) -> Report:
    report = Report()
    say(f"==> act 0       the editor's command surface, {len(registered_commands(binaries))} "
        f"command(s)")
    report_optional(binaries, report)
    (work / "empty.cyscript").write_text("# nothing; this run only opens the document\n",
                                         encoding="utf-8")
    project = act_project(work, report)
    acts = {"import": lambda: act_import(binaries, project, work, report),
            "author": lambda: act_author(binaries, work, report),
            "survive": lambda: act_survive(binaries, work, report),
            "recover": lambda: act_recover(binaries, work, report)}
    for name, act in acts.items():
        if only in (None, name):
            act()
    return report


def selftest(binaries: Binaries, work: Path) -> int:
    """The driver's own negative cases: each assertion this session makes must be seen to fail.

    Case 1 leaves the runtime alive and requires act 4 to reject the run, because act 4's whole
    value is in what it asserts AFTER the kill — an act that passed either way would be a green tick
    over nothing. Case 2 points the session at a runtime that is not there and requires act 3 to
    notice that the session was not hosted.
    """
    cases = [
        ("act 4 accepts a runtime that was never killed",
         lambda scratch: act_survive(binaries, scratch, make_report(binaries), kill_runtime=False)),
        ("act 3 accepts a session whose runtime never answered",
         lambda scratch: act_author(binaries, scratch, make_report(binaries), with_runtime=False)),
    ]
    rejected = 0
    for index, (name, case) in enumerate(cases):
        scratch = work / f"selftest-{index}"
        scratch.mkdir(parents=True)
        try:
            case(scratch)
        except SessionError as refused:
            say(f"ok   rejected: {name}\n       {str(refused).splitlines()[0]}")
            rejected += 1
            continue
        say(f"fail {name} was accepted")
    say(f"session selftest: {rejected}/{len(cases)} cases rejected")
    return 0 if rejected == len(cases) else 1


def make_report(binaries: Binaries) -> Report:
    return Report(present=registered_commands(binaries))


def prepare(arguments: argparse.Namespace) -> Path:
    work = Path(arguments.work) if arguments.work else (
        ROOT / os.environ.get("CY_BUILD_DIR", "build") / "editor-session")
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    return work


def parse(argv: list) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--profile", default="dev", help="the workflow profile (default: dev)")
    parser.add_argument("--build", action="store_true",
                        help="build the editor first (not the engine, whose tree may be in use)")
    parser.add_argument("--work", help="the scratch directory (default: <build>/editor-session)")
    parser.add_argument("--editor-dir", help="where cyberdyne-editor and cy-runtime-stub are")
    parser.add_argument("--import-cli", help="the cy_import_cli binary")
    parser.add_argument("--only", choices=("import", "author", "survive", "recover"),
                        help="run one act instead of the session")
    parser.add_argument("--keep", action="store_true", help="keep the scratch directory")
    parser.add_argument("--selftest", action="store_true",
                        help="run this driver's own negative cases")
    return parser.parse_args(argv)


def main(argv: list) -> int:
    arguments = parse(argv)
    if os.name != "posix":
        say("session: the live bridge is a Unix domain socket; this host has none")
        return 2
    binaries = locate(arguments)
    work = prepare(arguments)
    try:
        if arguments.selftest:
            return selftest(binaries, work)
        report = session(binaries, work, arguments.only)
    except SessionError as failure:
        print(f"\nsession: {failure}", file=sys.stderr)
        print(f"  the scratch directory is {work}", file=sys.stderr)
        return 1
    finally:
        discard_sockets()
        if not arguments.keep:
            shutil.rmtree(work, ignore_errors=True)
    if arguments.only is None:
        say("\nsession: the editor opened a project, imported a glTF asset, authored a scene "
            "against a hosted runtime,\n         outlived that runtime being killed, and recovered "
            "and saved the work.")
    say("RESULT " + json.dumps({"acts": report.acts, "missing_commands": report.missing_commands}))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

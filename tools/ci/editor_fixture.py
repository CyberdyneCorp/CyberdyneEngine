#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Hold every CTest entry that drives the editor to the `cy_editor` fixture.

The editor is Cargo's, so no CMake target builds it. Three smoke entries used to build it from
inside their own run (`--build`), inside the 300 s smoke budget: a cold build of the workspace on a
hosted runner spent all of it, and `agent` and `authorable` timed out compiling crates before the
session started. A fourth, `smoke.authoring`, built nothing and failed on every leg that had not
happened to build the editor first. samples/editor_fixture.cmake is the repair: one entry,
`smoke.editor_build`, builds it as the setup of the `cy_editor` fixture, and every entry that drives
the editor requires that fixture and builds nothing itself.

This reads the configured tree's own record (`ctest --show-only=json-v1`) and fails when:

  * there is no single `cy_editor` setup, or it does not run `just build-editor`;
  * an entry that runs one of the editor drivers does not require `cy_editor`;
  * such an entry passes `--build`, which puts the build back inside the timed session.

`--selftest` runs the same check against hand-made records that must fail, so a rule that stopped
firing is caught rather than read as a tree with nothing wrong in it.

Usage: editor_fixture.py --build-dir <tree> [--ctest <ctest>]   |   editor_fixture.py --selftest
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys

FIXTURE = "cy_editor"
SETUP = "smoke.editor_build"

# The drivers that start `cyberdyne-editor`. A new one is added here and to editor_fixture.cmake's
# callers together; the check below also fails when none of them is registered, so a rename that
# silently empties this list is not a pass.
DRIVERS = (
    "samples/05-editor-session/session.py",
    "samples/05b-agent-authoring/authoring.py",
    "samples/05b-editor-window/window.py",
    "samples/05b-editor-window/mcp_window.py",
    "samples/08a-authoring/authoring.py",
)

# Entries that run a driver above in a mode that starts no editor, and why. Named rather than
# inferred from the arguments, so that an entry which starts needing the editor is a visible edit.
STARTS_NO_EDITOR = {
    # window.py --selftest checks the viewport mapping against recorded geometry: "no runtime, no
    # editor and no display" (samples/05b-editor-window/CMakeLists.txt).
    "integration.editor_window_selftest",
}


def properties(test: dict) -> dict:
    return {entry["name"]: entry["value"] for entry in test.get("properties", [])}


def as_list(value) -> list[str]:
    if value is None:
        return []
    return list(value) if isinstance(value, list) else [value]


def violations(record: dict) -> list[str]:
    tests = record.get("tests", [])
    found = []

    setups = [t for t in tests if FIXTURE in as_list(properties(t).get("FIXTURES_SETUP"))]
    if len(setups) != 1:
        found.append(f"{len(setups)} tests set up the {FIXTURE!r} fixture; exactly one must")
    for setup in setups:
        command = " ".join(setup.get("command", []))
        if setup["name"] != SETUP:
            found.append(f"the {FIXTURE!r} setup is {setup['name']!r}, expected {SETUP!r}")
        if "build-editor" not in command:
            found.append(f"{setup['name']} does not run `just build-editor`: {command}")

    drivers = 0
    for test in tests:
        command = [str(part) for part in test.get("command", [])]
        if not any(part.replace("\\", "/").endswith(DRIVERS) for part in command):
            continue
        name = test["name"]
        if name in STARTS_NO_EDITOR:
            continue
        drivers += 1
        if FIXTURE not in as_list(properties(test).get("FIXTURES_REQUIRED")):
            found.append(f"{name} drives the editor and does not require the {FIXTURE!r} fixture")
        if "--build" in command:
            found.append(
                f"{name} passes --build, so the editor is compiled inside its own timeout; "
                f"the {FIXTURE!r} fixture builds it"
            )
    if drivers == 0:
        found.append("no registered test runs an editor driver; the driver list is out of date")
    return found


def entry(name: str, command: list[str], **props) -> dict:
    return {
        "name": name,
        "command": command,
        "properties": [{"name": key, "value": value} for key, value in props.items()],
    }


def selftest() -> int:
    setup = entry(SETUP, ["just", "build-editor", "--profile", "dev"], FIXTURES_SETUP=[FIXTURE])
    driver = ["python3", "/src/samples/05b-agent-authoring/authoring.py", "--profile", "dev"]
    good = entry("smoke.agent_authoring", driver, FIXTURES_REQUIRED=[FIXTURE])
    cases = {
        "the shape the tree must have": ({"tests": [setup, good]}, False),
        "the build inside the session (the defect)": (
            {"tests": [setup, entry("smoke.agent_authoring", driver + ["--build"],
                                    FIXTURES_REQUIRED=[FIXTURE])]}, True),
        "a driver that does not require the fixture": (
            {"tests": [setup, entry("smoke.agent_authoring", driver)]}, True),
        "no setup at all (main before the fixture)": (
            {"tests": [entry("smoke.agent_authoring", driver + ["--build"])]}, True),
        "a setup that builds something else": (
            {"tests": [entry(SETUP, ["just", "build-engine"], FIXTURES_SETUP=[FIXTURE]), good]},
            True),
        "no driver registered at all": ({"tests": [setup]}, True),
        "an exempt entry that starts no editor": (
            {"tests": [setup, good, entry("integration.editor_window_selftest",
                                          ["python3", "/src/samples/05b-editor-window/window.py",
                                           "--selftest"])]}, False),
    }
    failed = 0
    for name, (record, must_fail) in cases.items():
        did_fail = bool(violations(record))
        if did_fail != must_fail:
            failed += 1
            print(f"fail {name}: expected {'a violation' if must_fail else 'none'}", file=sys.stderr)
        else:
            print(f"ok   {name}")
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", help="the configured tree to read")
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--selftest", action="store_true")
    options = parser.parse_args()

    if options.selftest:
        return selftest()
    if not options.build_dir:
        parser.error("--build-dir is required unless --selftest is given")
    # The rule's own negative cases first: a check that stopped firing reads as a clean tree.
    if selftest() != 0:
        return 1

    shown = subprocess.run(
        [options.ctest, "--test-dir", options.build_dir, "--show-only=json-v1"],
        capture_output=True, text=True, check=True,
    )
    found = violations(json.loads(shown.stdout))
    for problem in found:
        print(f"editor-fixture: {problem}", file=sys.stderr)
    if found:
        return 1
    print(f"editor-fixture: every editor driver requires {FIXTURE!r}, and none builds the editor")
    return 0


if __name__ == "__main__":
    sys.exit(main())

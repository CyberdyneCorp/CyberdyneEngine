#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Did adding a platform backend change an engine layer? M11.d task 4.2.

`core-platform-abstraction` states the exit criterion as a property of a CHANGESET, not of a test:

    A port implements Platform, DisplayServer, an input backend, an audio backend and a graphics
    surface provider, "with no changes required in src/core/ or above".

`platform/README.md` puts the consequence plainly — "if it does, the abstraction is wrong" — and
ROADMAP.md's M11 exit criteria name the four directories: src/core/, src/ecs/, src/servers/ and
src/scene/. No suite can see that. A diff can, and this reads one.

WHY THIS IS A SCRIPT AND NOT A SENTENCE IN A REPORT. The claim is checked the same way twice: here,
by whoever is writing the port, against the working tree before anything is committed; and in
`tools/roadmap/milestones/m11d.toml`, by the milestone gate, against the commits that landed. A
claim that is only ever checked by the person making it is a claim nobody checked.

AND A FINDING IS NOT A FAILURE OF THIS SCRIPT. If the port did touch one of the four, the right
response is to report the diff — the abstraction leaked, and that is worth more than a green tick —
rather than to quietly make the change and say nothing. The exit status says which happened; the
output says what.

    tools/ci/port_engine_layer_diff.py                       the working tree, staged and unstaged
    tools/ci/port_engine_layer_diff.py --since <commit>      every change since that commit
    tools/ci/port_engine_layer_diff.py --commits <marker>    the commits whose subject names <marker>

Exit status is 0 when no engine layer was touched and 1 when one was, with every offending path
printed. A range that names no commit at all is status 2: "nothing to judge" is not a pass.

ONE CAVEAT, AND IT IS WHY THE GATE USES --commits. The working-tree mode sees every change in the
tree, not only the port's. Where several people (or several agents) share a checkout, a file another
change touched is reported here and is not this port's doing — so the working-tree mode is a
developer's own check while writing, and the claim that the gate records is read from the commits
that carry the port.
"""

from __future__ import annotations

import argparse
import subprocess
import sys

# ROADMAP.md's M11 exit criteria, verbatim as directories. Layer 3 — platform/ and src/backends/ —
# is where a port lives and is deliberately absent; layers 4 and above are absent because the
# criterion names these four and a check that widened it on its own would be checking something
# nobody agreed to.
ENGINE_LAYERS = ("src/core/", "src/ecs/", "src/servers/", "src/scene/")


def run(arguments: list[str]) -> str:
    result = subprocess.run(["git", *arguments], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        print(f"port-diff: git {' '.join(arguments)} failed: {result.stderr.strip()}",
              file=sys.stderr)
        sys.exit(2)
    return result.stdout


def working_tree_files() -> tuple[list[str], str]:
    """Every path the working tree changes: staged, unstaged and untracked."""
    names = set()
    for arguments in (["diff", "--name-only", "HEAD"],
                      ["ls-files", "--others", "--exclude-standard"]):
        names.update(line for line in run(arguments).split() if line)
    return sorted(names), "the working tree (staged, unstaged and untracked)"


def range_files(since: str) -> tuple[list[str], str]:
    names = run(["diff", "--name-only", f"{since}..HEAD"]).split()
    return sorted(set(names)), f"{since}..HEAD"


def marker_files(marker: str, depth: int) -> tuple[list[str], str]:
    log = run(["log", "--format=%H %s", f"-{depth}"])
    commits = [line.split(" ", 1)[0] for line in log.splitlines() if marker in line.lower()]
    if not commits:
        print(f"port-diff: no commit naming '{marker}' in the last {depth}.")
        print("  The port has not landed, so the claim that it touched no engine layer cannot be")
        print("  evaluated — and NOT EVALUATED is never a pass.")
        sys.exit(2)
    names: set[str] = set()
    for commit in commits:
        names.update(run(["show", "--name-only", "--format=", commit]).split())
    return sorted(names), f"{len(commits)} commit(s) naming '{marker}'"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--since", help="compare HEAD against this commit")
    source.add_argument("--commits", help="read the commits whose subject contains this marker")
    parser.add_argument("--depth", type=int, default=40,
                        help="how many commits --commits searches (default: 40)")
    arguments = parser.parse_args(argv)

    if arguments.since:
        files, described = range_files(arguments.since)
    elif arguments.commits:
        files, described = marker_files(arguments.commits, arguments.depth)
    else:
        files, described = working_tree_files()

    offenders = [name for name in files if name.startswith(ENGINE_LAYERS)]
    print(f"port-diff: {len(files)} file(s) in {described}")
    for name in sorted(offenders):
        print(f"  ENGINE LAYER TOUCHED: {name}")

    if offenders:
        print(f"port-diff: {len(offenders)} file(s) under {', '.join(ENGINE_LAYERS)} changed.")
        print("  THIS IS THE FINDING. A port that needs a change above layer 3 has found a place")
        print("  where the abstraction leaks, and the change plus its reason is worth more than a")
        print("  green tick — platform/README.md: 'if it does, the abstraction is wrong'.")
        return 1

    print("port-diff: no engine layer changed — the port is entirely beneath the interfaces.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

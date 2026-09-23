#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Target-platform selection, and the refusal that explains itself. M11.d task 7.7.

`developer-workflow-and-just`:

    The workflow SHALL support **selecting a target platform** for build, test, deploy, and package
    recipes, and SHALL report clearly when a target cannot be built on the current host and why.

    #### Scenario: An impossible target is explained
    - **WHEN** a target cannot be built on the current host
    - **THEN** the workflow SHALL say so and state what is required

--- WHAT THIS REPLACES -------------------------------------------------------------------------------

`just/build.just`'s `_resolve-target` compared the request against the host and, when they differed,
printed one sentence:

    This milestone builds for the host only. Cross-compilation needs a CMake toolchain file and a
    platform SDK, and the first of those arrive with the mobile targets at M9.

Three things are wrong with that and only one of them is the stale milestone number. It said the
same thing about `macos` (which the engine supports and this host cannot produce), about `android`
(which the engine does not support at all), and about `macoss` (which is a typo) — so a developer
could not tell "wrong machine" from "not written yet" from "you misspelled it". **A sentence with
one answer is not a table**, and a second desktop is the first time the requirement's own scenario
has a second answer.

`just/targets.toml` is the table. This file reads it, and every recipe that takes `--platform`
resolves through here, so what `just build-engine --platform ios` says and what `just env-targets`
prints cannot disagree.

--- THE THREE REFUSALS, WHICH ARE DELIBERATELY DIFFERENT ---------------------------------------------

  unknown       no such target. Names the ones that exist. Exit 3.
  needs-host    the engine supports it and THIS HOST cannot produce it. Names the hosts that can and
                what a cross-build would require. Exit 2.
  planned       no port exists in this tree. Names the RUNG that writes one, and that rung is checked
                against `record.MILESTONES` — a refusal naming a milestone off the end of the ladder
                is exactly the defect `just/release.just`'s four recipes carry today, which task 7.8
                exists to stop this rung claiming over.

Exit statuses differ because a caller should be able to tell a typo from a machine limit without
parsing prose.
"""

from __future__ import annotations

import argparse
import platform
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
TABLE = REPOSITORY / "just/targets.toml"

STATES = ("buildable", "needs-host", "planned")
KINDS = ("os", "port")


@dataclass(frozen=True)
class Target:
    id: str
    kind: str
    describe: str
    hosts: tuple[str, ...]
    state: str
    owner: str
    requires: str


def host_platform() -> str:
    """linux, macos, windows or unknown — the same four words `just _host-platform` prints."""
    system = platform.system()
    if system == "Linux":
        return "linux"
    if system == "Darwin":
        return "macos"
    if system == "Windows" or system.startswith(("MINGW", "MSYS", "CYGWIN")):
        return "windows"
    return "unknown"


def ladder() -> tuple[str, ...]:
    """The rungs that exist, so a refusal cannot name a milestone off the end of the plan."""
    sys.path.insert(0, str(REPOSITORY / "tools/roadmap"))
    import record  # noqa: E402  (after the path insert, deliberately)

    return record.MILESTONES


def load(path: Path = TABLE) -> list[Target]:
    document = tomllib.loads(path.read_text(encoding="utf-8"))
    rungs = ladder()
    targets = []
    for entry in document.get("target", []):
        target = Target(
            id=entry["id"],
            kind=entry["kind"],
            describe=entry["describe"],
            hosts=tuple(entry.get("hosts", ())),
            state=entry["state"],
            owner=entry["owner"],
            requires=entry["requires"],
        )
        if target.state not in STATES:
            raise ValueError(f"{target.id}: state '{target.state}' is not one of {STATES}")
        if target.kind not in KINDS:
            raise ValueError(f"{target.id}: kind '{target.kind}' is not one of {KINDS}")
        # The check that stops this table from becoming release.just's four recipes: a refusal that
        # names a milestone the ladder does not have tells a developer to wait for nothing.
        if target.owner not in rungs:
            raise ValueError(
                f"{target.id}: owner '{target.owner}' is not a rung on the ladder {rungs}. "
                "A refusal that names a milestone that does not exist is worse than no refusal."
            )
        if target.state == "buildable" and not target.hosts:
            raise ValueError(f"{target.id}: buildable on no host is not buildable")
        targets.append(target)
    if not targets:
        raise ValueError(f"{path} declares no targets; nothing could be resolved against it")
    return targets


def verdict(target: Target, host: str) -> str:
    """What this host can do with this target: `here`, `elsewhere` or `unwritten`."""
    if target.state == "planned":
        return "unwritten"
    return "here" if host in target.hosts else "elsewhere"


def resolve(name: str, host: str, targets: list[Target], out=sys.stderr) -> tuple[str | None, int]:
    """The target's id when this host can build it, or a refusal that states what is required."""
    if not name:
        return host, 0

    found = next((target for target in targets if target.id == name), None)
    if found is None:
        print(f"there is no target '{name}'.", file=out)
        print(f"  The targets this workflow knows: {', '.join(t.id for t in targets)}", file=out)
        print("  `just env-targets` prints what each one needs and which this host can build.",
              file=out)
        return None, 3

    state = verdict(found, host)
    if state == "here":
        return found.id, 0

    if state == "unwritten":
        print(f"cannot build for '{found.id}': no port for it exists in this tree.", file=out)
        print(f"  {found.describe}", file=out)
        print(f"  What it needs: {found.requires}", file=out)
        print(f"  The rung that writes it: {found.owner.upper()}.", file=out)
        return None, 2

    print(f"cannot build for '{found.id}' on a {host} host.", file=out)
    print(f"  {found.describe}", file=out)
    print(f"  What it needs: {found.requires}", file=out)
    print(f"  Hosts that can build it: {', '.join(found.hosts) or '(none declared)'}.", file=out)
    return None, 2


def render(targets: list[Target], host: str) -> str:
    """The table, with this host's verdict on each row."""
    mark = {"here": "yes", "elsewhere": "not here", "unwritten": "not written"}
    width = max(len(target.id) for target in targets)
    lines = [f"target platforms, on a {host} host", ""]
    for kind, heading in (("os", "operating systems"), ("port", "ports of the porting surface")):
        rows = [target for target in targets if target.kind == kind]
        if not rows:
            continue
        lines.append(f"  {heading}")
        for target in rows:
            state = verdict(target, host)
            lines.append(f"    {target.id:<{width}}  {mark[state]:<12}"
                         f"{'' if state == 'here' else target.owner.upper() + '  '}{target.describe}")
            if state != "here":
                lines.append(f"    {'':<{width}}  {'':<12}needs: {target.requires}")
        lines.append("")
    lines.append("  Select one with --platform on build, test, package and deploy recipes,")
    lines.append("  or for a whole shell with CY_TARGET_PLATFORM.")
    return "\n".join(lines)


def selftest() -> int:
    """Prove the refusals still discriminate. A refusal that says one thing about everything is the
    check this table replaced."""
    targets = load()
    host = "linux"
    import io

    cases: list[tuple[str, str, int, str]] = [
        # (what, requested, expected status, text the refusal must contain)
        ("a target this host builds", "linux", 0, ""),
        ("a port every host builds", "stub", 0, ""),
        ("a target that needs another host", "macos", 2, "macOS SDK is not redistributable"),
        ("a target nobody has written", "android", 2, "Android NDK"),
        ("iOS needs its supported host", "ios", 2, "Xcode 27"),
        ("a misspelling", "macoss", 3, "there is no target"),
        ("an empty request is the host", "", 0, ""),
    ]

    failures = []
    for about, requested, expected, phrase in cases:
        buffer = io.StringIO()
        _, status = resolve(requested, host, targets, out=buffer)
        text = buffer.getvalue()
        if status != expected:
            failures.append(f"{about}: '{requested}' exited {status}, expected {expected}")
        elif phrase and phrase not in text:
            failures.append(f"{about}: the refusal for '{requested}' does not say {phrase!r}\n{text}")
        else:
            print(f"    ok       {about}")

    # The three refusals must not be one refusal wearing three exit codes.
    messages = set()
    for requested in ("macos", "android", "macoss"):
        buffer = io.StringIO()
        resolve(requested, host, targets, out=buffer)
        messages.add(buffer.getvalue())
    if len(messages) != 3:
        failures.append("the three refusals are not three distinct messages")
    else:
        print("    ok       the three refusals say three different things")

    # A table that declared an owner off the end of the ladder must be refused at load.
    import tempfile

    with tempfile.NamedTemporaryFile("w", suffix=".toml", delete=False) as handle:
        handle.write('schema = 1\n[[target]]\nid = "x"\nkind = "os"\ndescribe = "d"\n'
                     'hosts = []\nstate = "planned"\nowner = "not-a-rung"\nrequires = "r"\n')
        bogus = Path(handle.name)
    try:
        load(bogus)
        failures.append("a target owned by a milestone that is not on the ladder was accepted")
    except ValueError:
        print("    ok       an owner that is not a rung on the ladder is refused at load")
    finally:
        bogus.unlink()

    if failures:
        print("\ntargets selftest: " + "\n  ".join(failures), file=sys.stderr)
        return 1
    print(f"\ntargets selftest: {len(cases) + 2} cases passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=("list", "resolve", "selftest"))
    parser.add_argument("name", nargs="?", default="")
    parser.add_argument("--host", default=None, help="override the detected host, for the selftest")
    arguments = parser.parse_args()

    if arguments.command == "selftest":
        print("==> targets     the refusals' own negative cases")
        return selftest()

    targets = load()
    host = arguments.host or host_platform()

    if arguments.command == "list":
        print(render(targets, host))
        return 0

    resolved, status = resolve(arguments.name, host, targets)
    if resolved is not None:
        print(resolved)
    return status


if __name__ == "__main__":
    sys.exit(main())

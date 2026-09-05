#!/usr/bin/env python3
"""The sample contains no C++ gameplay code. Task 5.3.

    check_no_cpp_gameplay.py --sample samples/04-character
    check_no_cpp_gameplay.py --selftest

--- WHAT THIS CHECKS, AND WHAT IT DELIBERATELY DOES NOT --------------------------------------------

Task 5.3 asks for "a check that the sample contains no C++ gameplay code", and the honest reading of
that is not a grep for the word "jump". A grep can only fail on the words somebody thought to forbid,
and a host that decided how fast the character walks would pass every one of them.

So this is the STATIC half of a two-part check, and it is deliberately the weaker half:

  A. THE PARTITION. Every file under game/ is Swift, and no Swift file exists anywhere else in the
     sample. A C++ file that appeared under game/ would be a game written in C++ whatever it did.

  B. ONE CALL INTO THE GAME. The host's C++ calls `fixed_update` exactly once and defines no
     behaviour class of its own. A second entry point into gameplay is the shape a bypass takes.

  C. THE DIRECTION OF EVERY COMPONENT ACCESS.  **This is the one that means something.** The boundary
     in game/Contract.swift declares which way each component flows: the host WRITES what the engine
     tells the game (`PlayerInput`, `CharacterState`) and READS what the game asks the engine to do
     (`CharacterSpec`, `CharacterDrive`, `CameraSpec`, `CameraIntent`, `AudioCue`, `LevelBox`). A host
     that wrote into `CharacterDrive` would be choosing the character's velocity; a host that wrote
     into `CameraSpec` would be choosing the framing. Neither is possible to do accidentally and
     neither is possible to do without failing here.

  D. THE DISTINCTIVE TUNABLES. Every `@Export`ed default in the Swift with two or more digits after
     the decimal point — 0.35, 1.65, 0.12, 1.0472 — must not appear as a literal in the host's C++.
     Two digits, because a check on 0.5 or 1.0 would fire on arithmetic and be turned off within a
     week, and a check that is turned off is worse than one that was never written. It catches a
     tunable that was copied; it does not catch one that was rounded.

WHAT IT CANNOT CATCH is a decision expressed in numbers this file does not know about — and that is
why the REAL check for task 5.3 is a run, not a grep. `--no-behaviours` runs the identical host over
the identical level with the identical input and creates neither deciding behaviour; the character
does not move, does not jump, makes no sound, and the camera does not turn.
tests/smoke/test_character_sample.cpp asserts exactly that, against a positive run of the same
binary, so the two are the same measurement with one thing changed.

--- WHY THE SELFTEST EDITS THE REAL FILES RATHER THAN A FIXTURE DIRECTORY --------------------------

The same argument src/abi/tests/ makes about the ABI gate. A copied fixture goes stale, and worse: if
this parser ever stopped recognising a `write_f32` call, a hand-written "violating" fixture and a
hand-written "clean" one would both parse as nothing, and comparing nothing to nothing succeeds. So
every case below edits the live tree IN MEMORY and runs the real check over it, and case 0 — the
unedited tree passes — is the control that makes the other four mean anything.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
DEFAULT_SAMPLE = REPOSITORY / "samples" / "04-character"

CPP_SUFFIXES = {".cpp", ".h", ".hpp", ".cc", ".cxx"}

# The direction each component flows, as game/Contract.swift declares it. The names are the members
# of `sample::Contract` in host/contract.h, which is the only place the host can reach them through.
HOST_WRITES = {"input", "state"}
HOST_READS = {"spec", "drive", "camera_spec", "camera_intent", "cue", "level"}

ACCESS = re.compile(r"\b(read|write)_(?:f32|vec3)\(\s*world_\s*,\s*\w+\s*,\s*contract_\.(\w+)")
EXPORT = re.compile(r"@Export(?:\([^)]*\))?\s+var\s+(\w+)\s*:\s*\w+\s*=\s*(-?\d+\.\d+)")
BEHAVIOUR_CLASS = re.compile(r"\bclass\s+\w+\s*:\s*(?:public\s+)?\w*Behaviour\b")
FIXED_UPDATE = re.compile(r"\bfixed_update\s*\(")


class Finding(list):
    """The failures, in the order they were found. A list with a name, so a caller reads intent."""


def read_tree(sample: pathlib.Path) -> dict[str, str]:
    """Every file of the sample that this check looks at, by path relative to the sample."""
    files: dict[str, str] = {}
    for path in sorted(sample.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix in CPP_SUFFIXES or path.suffix == ".swift":
            files[path.relative_to(sample).as_posix()] = path.read_text(encoding="utf-8")
    return files


def check_partition(files: dict[str, str], findings: Finding) -> None:
    for name in files:
        if name.endswith(".swift") and not name.startswith("game/"):
            findings.append(f"A: {name} is Swift but is not under game/")
        if not name.endswith(".swift") and not name.startswith("host/"):
            findings.append(f"A: {name} is C++ but is not under host/")
    if not any(name.startswith("game/") for name in files):
        findings.append("A: game/ holds no Swift at all, so there is no game to be hosting")


def check_single_entry(files: dict[str, str], findings: Finding) -> None:
    calls = 0
    for name, text in files.items():
        if name.endswith(".swift"):
            continue
        calls += len(FIXED_UPDATE.findall(text))
        if BEHAVIOUR_CLASS.search(text):
            findings.append(f"B: {name} defines a behaviour class in C++")
    if calls != 1:
        findings.append(
            f"B: the host calls fixed_update {calls} times; exactly one call into the game is the "
            "whole of its per-tick contact with gameplay"
        )


def check_directions(files: dict[str, str], findings: Finding) -> None:
    seen = False
    for name, text in files.items():
        if name.endswith(".swift"):
            continue
        for verb, component in ACCESS.findall(text):
            seen = True
            allowed = HOST_WRITES if verb == "write" else HOST_READS
            if component not in allowed:
                findings.append(
                    f"C: {name} calls {verb} on contract_.{component}, which flows the other way — "
                    "the host would be deciding a value the game declares"
                )
    if not seen:
        findings.append("C: no component access was recognised, so this check verified nothing")


def check_tunables(files: dict[str, str], findings: Finding) -> None:
    tunables: dict[str, str] = {}
    for name, text in files.items():
        if not name.endswith(".swift"):
            continue
        for field, value in EXPORT.findall(text):
            decimals = value.split(".")[1]
            if len(decimals) >= 2:
                tunables[value] = field
    if not tunables:
        findings.append("D: no exported tunable was recognised, so this check verified nothing")
    for name, text in files.items():
        if name.endswith(".swift"):
            continue
        for value, field in tunables.items():
            # The `F` suffix or a word boundary, so 0.35 does not match inside 10.357.
            if re.search(rf"(?<![\d.]){re.escape(value)}(?![\d])", text):
                findings.append(
                    f"D: {name} contains the literal {value}, which is {field}'s value in Swift"
                )


def check(files: dict[str, str]) -> Finding:
    findings = Finding()
    check_partition(files, findings)
    check_single_entry(files, findings)
    check_directions(files, findings)
    check_tunables(files, findings)
    return findings


def selftest(sample: pathlib.Path) -> int:
    """Five cases: the tree as it stands, then one deliberate violation of each rule."""
    base = read_tree(sample)
    host = next(name for name in base if name.startswith("host/") and name.endswith(".cpp"))

    def with_edit(name: str, text: str) -> dict[str, str]:
        edited = dict(base)
        edited[name] = text
        return edited

    cases: list[tuple[str, dict[str, str], bool]] = [
        ("0 the sample as it stands passes", base, True),
        (
            "A a Swift file outside game/ is rejected",
            with_edit("host/Sneaky.swift", "// gameplay in the wrong place\n"),
            False,
        ),
        (
            "B a second call into the game is rejected",
            with_edit(host, base[host] + "\nvoid extra() { runtime_.fixed_update(0.0F); }\n"),
            False,
        ),
        (
            "C the host writing what the game declares is rejected",
            with_edit(
                host,
                base[host] + "\nvoid decide() { write_f32(world_, player_, contract_.drive, "
                "contract_.drive.jump_speed, 6.2F); }\n",
            ),
            False,
        ),
        (
            "D a tunable copied into the host is rejected",
            with_edit(host, base[host] + "\nconstexpr float kStride = 1.65F;\n"),
            False,
        ),
    ]

    failures = 0
    for label, files, expected in cases:
        findings = check(files)
        passed = not findings
        verdict = "ok  " if passed == expected else "FAIL"
        if passed != expected:
            failures += 1
        print(f"  {verdict} {label}")
        if passed != expected and findings:
            for finding in findings:
                print(f"         {finding}")
    print(f"selftest: {len(cases) - failures}/{len(cases)} passed")
    return 1 if failures else 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sample", type=pathlib.Path, default=DEFAULT_SAMPLE)
    parser.add_argument("--selftest", action="store_true",
                        help="run the check's own negative cases and exit")
    arguments = parser.parse_args(argv)

    sample = arguments.sample.resolve()
    if not sample.is_dir():
        sys.stderr.write(f"no such sample directory: {sample}\n")
        return 2
    if arguments.selftest:
        return selftest(sample)

    findings = check(read_tree(sample))
    if findings:
        sys.stderr.write(f"{sample.name}: the sample contains C++ gameplay code.\n")
        for finding in findings:
            sys.stderr.write(f"  {finding}\n")
        sys.stderr.write(
            "\nThe rules are A partition, B one entry, C access direction, D tunables; the "
            "docstring at the top of this file says what each is for.\n"
        )
        return 1
    print(f"{sample.name}: no C++ gameplay code — partition, entry, direction and tunables all hold")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Negative fixtures for the cross-leg comparator. M11.a section 4.

THE COMPARATOR IS THE ONLY THING STANDING BETWEEN "TWO ARCHITECTURES AGREED" AND A GREEN TICK OVER
NOTHING, so its refusals are checked here rather than reviewed. `m9:lockstep-cross-platform` was
declared a gap rather than a `where = "ci"` criterion for one reason, written into m9.toml: a
`where = "ci"` criterion would have PASSED in continuous integration, satisfied by a single-leg
suite. A comparator that accepted one leg, or four legs of one architecture, or two digests of an
empty world, would reintroduce exactly that defect one layer down.

Every case below builds digest files on disk, runs `cross_leg_digests.py` over them, and asserts the
EXIT CODE and a phrase from the message. Three exit codes, and they are three different facts:

    0   compared, and every requested digest is identical across at least two architectures
    1   compared, and they DISAGREE — a finding, with every leg's number printed
    2   REFUSED: the comparison could not be made, and must not be read as one that passed

Run through `just ci-check`, beside `check_workflows.py --selftest`, for the same reason that one
exists: a rule that stopped firing looks exactly like a tree with nothing wrong in it.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

COMPARATOR = pathlib.Path(__file__).resolve().parent / "cross_leg_digests.py"

#: A well-formed digest, as the fields `determinism.cross_leg` publishes them. Each case below
#: overrides exactly what it is about, so a refusal that fired for a second reason would show up as
#: the wrong message rather than as a pass.
BASE = {
    "schema": "1",
    "label": "a-leg",
    "os": "linux",
    "arch": "x86_64",
    "compiler": "gcc-13.3",
    "pointer-bits": "64",
    "endian": "little",
    "sim-ticks": "120",
    "sim-state-digest": "44d0feadcb7cd95d",
    "sim-final-hash": "9bfb1196310fbe35",
    "pcg-seed": "00c0ffee12345678",
    "pcg-regions": "64",
    "pcg-world-digest": "7bc377fd2fc0872b",
    "pcg-identity-digest": "e40fc43a7d8059bf",
    "pcg-gpu-domain": "none",
}


def leg(**overrides: str) -> dict[str, str]:
    """One leg's fields: the well-formed set with `overrides` applied, and `None` deleting a key."""
    fields = dict(BASE)
    for key, value in overrides.items():
        name = key.replace("_", "-")
        if value is None:
            fields.pop(name, None)
        else:
            fields[name] = value
    return fields


def run(legs: list[dict[str, str]], *arguments: str) -> tuple[int, str]:
    """The comparator over `legs`, written into a throwaway directory."""
    with tempfile.TemporaryDirectory(prefix="cy-cross-leg-") as directory:
        root = pathlib.Path(directory)
        for index, fields in enumerate(legs):
            # A subdirectory each, because `actions/download-artifact` unpacks several artefacts
            # that way and a comparator that only looked at the top level would find nothing.
            nested = root / f"cross-leg-digest-{index}"
            nested.mkdir()
            body = "".join(f"{key} {value}\n" for key, value in fields.items())
            (nested / f"{fields.get('label', index)}.digest").write_text(body, encoding="utf-8")
        finished = subprocess.run(
            [sys.executable, str(COMPARATOR), "--dir", str(root), *arguments],
            capture_output=True, text=True, check=False)
    return finished.returncode, finished.stdout + finished.stderr


#: (name, legs, extra arguments, expected exit, a phrase the output must contain)
CASES = (
    ("one leg agrees with itself, and that is not a comparison",
     [leg()], (), 2, "one leg agrees with itself"),

    ("no leg at all is a refusal, never an empty pass",
     [], (), 2, "0 leg(s) published"),

    ("four legs of ONE architecture are still one architecture",
     [leg(label="a"), leg(label="b"), leg(label="c"), leg(label="d")], (), 2,
     "all of them are x86_64"),

    ("two runners named differently are not two architectures",
     [leg(label="linux-x86_64"), leg(label="linux-arm64")], (), 2, "all of them are x86_64"),

    ("two architectures that agree is the pass, and the only one",
     [leg(label="a"), leg(label="b", arch="arm64")], (), 0, "identical across"),

    ("a disagreeing simulation hash is a FINDING, reported with both numbers",
     [leg(label="a"), leg(label="b", arch="arm64", sim_state_digest="0123456789abcdef")],
     (), 1, "sim-state-digest"),

    ("a disagreeing generated world is a finding too, and only under --pcg",
     [leg(label="a"), leg(label="b", arch="arm64", pcg_world_digest="0123456789abcdef")],
     ("--pcg",), 1, "pcg-world-digest"),

    ("...and without --pcg the same pair passes, so the two claims are separable",
     [leg(label="a"), leg(label="b", arch="arm64", pcg_world_digest="0123456789abcdef")],
     (), 0, "identical across"),

    ("a zero digest is agreement produced by absence",
     [leg(label="a", sim_state_digest="0000000000000000"),
      leg(label="b", arch="arm64", sim_state_digest="0000000000000000")], (), 2,
     "Two legs that both computed nothing"),

    ("a digest over an empty workload, likewise",
     [leg(label="a", pcg_regions="0"), leg(label="b", arch="arm64", pcg_regions="0")],
     ("--pcg",), 2, "agrees with every other digest of an empty workload"),

    ("a leg that cannot name its own architecture is not counted as one",
     [leg(label="a", arch="unknown"), leg(label="b", arch="arm64")], (), 2,
     "could not name its own architecture"),

    ("a schema this comparator does not understand is refused, not guessed at",
     [leg(label="a", schema="2"), leg(label="b", arch="arm64")], (), 2, "and this comparator "
     "understands 1"),

    ("a digest missing a field the claim needs is refused by name",
     [leg(label="a", sim_state_digest=None), leg(label="b", arch="arm64")], (), 2,
     "no 'sim-state-digest'"),

    ("a key with no value is a damaged file rather than an empty answer",
     [leg(label="a", endian=""), leg(label="b", arch="arm64")], (), 2, "has no value"),

    ("--gpu-domain refuses on every leg this tree can produce, and says which two things are absent",
     [leg(label="a"), leg(label="b", arch="arm64")], ("--pcg", "--gpu-domain"), 2,
     "no leg published a GPU execution domain"),
)


def main() -> int:
    failures = 0
    for name, legs, arguments, expected_exit, phrase in CASES:
        code, output = run(list(legs), *arguments)
        wrong = []
        if code != expected_exit:
            wrong.append(f"exit {code}, expected {expected_exit}")
        if phrase not in output:
            wrong.append(f"the message does not say {phrase!r}")
        if wrong:
            failures += 1
            print(f"FAIL {name}\n     {'; '.join(wrong)}")
            print("     ---\n" + "\n".join("     " + line for line in output.splitlines()))
        else:
            print(f"ok   {name}")
    print(f"cross-leg comparator selftest: {len(CASES) - failures}/{len(CASES)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Compare the state digests two continuous-integration legs published. M11.a section 4.

THE ONE JOB THREE CRITERIA LOOK FOR. `ci.yml`'s build and test matrices carry linux-x86_64,
linux-arm64, macos-arm64 and windows-x86_64 and they run INDEPENDENTLY: nothing publishes a digest
and nothing downloads one to compare. Three criteria in three ledgers fail or report NOT EVALUATED
for want of that one job —

    m9:lockstep-cross-platform            a simulation state hash compared between two architectures
    m10:pcg-regeneration-cross-platform   a generated region's digest, likewise
    m10:pcg-gpu-domain-agreement          the GPU execution domain against the CPU one, on more than
                                          one vendor's driver

— and this is the comparator half of it. `determinism.cross_leg` (tests/determinism/) is the
publisher half; `just test-determinism --publish-digest <path>` runs one leg's publication and
`just test-determinism --compare-legs [--pcg]` runs this.

WHAT THIS REFUSES TO CALL AN AGREEMENT, which is the whole of its value. `m9:lockstep-cross-platform`
was declared a gap rather than a `where = "ci"` criterion for one stated reason: `where = "ci"` would
have made it PASS in continuous integration, satisfied by a SINGLE-LEG suite. So every one of these
exits non-zero rather than reporting an agreement:

  * fewer than two legs published                       (exit 2) — one leg agrees with itself
  * every leg reports the same architecture             (exit 2) — "between two architectures" is
                                                                   made of the architecture each
                                                                   BINARY detected, never of the
                                                                   workflow's label for the leg
  * a leg could not name its own architecture           (exit 2)
  * a compared digest is zero, or its workload is empty (exit 2) — the shape of an agreement
                                                                   produced by absence
  * a schema this comparator does not understand        (exit 2)
  * the digests differ                                  (exit 1) — a FINDING, reported with every
                                                                   leg's number, never hidden

Exit 0 means: at least two architectures published, each published a non-empty workload, and every
digest of every requested claim is identical across all of them.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

SCHEMA = 1

#: One claim: the fields compared, and the criterion that asks for it.
CLAIMS = {
    "lockstep": (
        ("sim-state-digest", "sim-final-hash"),
        "sim-ticks",
        "m9:lockstep-cross-platform — a simulation state hash between two architectures",
    ),
    "pcg": (
        ("pcg-world-digest", "pcg-identity-digest"),
        "pcg-regions",
        "m10:pcg-regeneration-cross-platform — a generated region's digest between two architectures",
    ),
}

#: Fields a digest file must carry before it is compared at all. A file missing one is a publisher
#: this comparator cannot read, never a leg that agreed.
REQUIRED = ("schema", "os", "arch", "compiler", "pointer-bits", "endian")


class Refusal(Exception):
    """A precondition of the comparison that does not hold. Never a disagreement."""


def read_digest(path: pathlib.Path) -> dict[str, str]:
    """One published file, as its key/value lines. Comments and blank lines are skipped."""
    fields: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        key, _, value = stripped.partition(" ")
        if not value.strip():
            raise Refusal(f"{path}: '{key}' has no value")
        fields[key] = value.strip()
    fields["source"] = str(path)
    return fields


def collect(directory: pathlib.Path) -> list[dict[str, str]]:
    """Every digest under `directory`, at any depth.

    `actions/download-artifact` unpacks each artefact into its own subdirectory when several are
    downloaded at once, so the search is recursive — a comparator that looked only at the top level
    would find nothing and report it as "no legs published", which is a refusal for the wrong
    reason.
    """
    if not directory.is_dir():
        raise Refusal(f"{directory}: no such directory — no leg published a digest")
    legs = [read_digest(path) for path in sorted(directory.rglob("*.digest"))]
    for leg in legs:
        missing = [key for key in REQUIRED if key not in leg]
        if missing:
            raise Refusal(f"{leg['source']}: not a digest this comparator can read — "
                          f"no {', '.join(missing)}")
        if leg["schema"] != str(SCHEMA):
            raise Refusal(f"{leg['source']}: schema {leg['schema']}, and this comparator "
                          f"understands {SCHEMA}. A field whose meaning moved must not be compared "
                          f"as though it had not")
        if leg["arch"] == "unknown":
            raise Refusal(f"{leg['source']}: the leg could not name its own architecture. It is not "
                          f"counted as one, because the two-architecture rule is made of this field")
    return legs


def describe(leg: dict[str, str]) -> str:
    return (f"{leg.get('label', 'unlabelled')} [{leg['os']}/{leg['arch']}/{leg['compiler']}, "
            f"{leg['endian']}-endian]")


def check_preconditions(legs: list[dict[str, str]]) -> None:
    """Two legs, and two ARCHITECTURES. The second is the one that cannot be satisfied by a label."""
    if len(legs) < 2:
        raise Refusal(f"{len(legs)} leg(s) published a digest. A comparison between two "
                      f"architectures needs two legs; one leg agrees with itself, which is the "
                      f"single-leg pass m9:lockstep-cross-platform was declared a gap to avoid")
    architectures = sorted({leg["arch"] for leg in legs})
    if len(architectures) < 2:
        raise Refusal(f"{len(legs)} legs published and all of them are {architectures[0]}. The "
                      f"architecture compared is the one each BINARY detected, never the workflow's "
                      f"label for the leg, so this cannot be satisfied by naming two runners "
                      f"differently")


def compare(legs: list[dict[str, str]], claim: str) -> list[str]:
    """One claim across every leg. Returns the disagreements, empty when they agree."""
    fields, workload, asked_by = CLAIMS[claim]
    print(f"\n{claim}: {asked_by}")

    for leg in legs:
        for key in (*fields, workload):
            if key not in leg:
                raise Refusal(f"{leg['source']}: no '{key}', so the {claim} claim cannot be "
                              f"compared against it")
        if leg[workload].strip("0") == "":
            raise Refusal(f"{describe(leg)}: {workload} is {leg[workload]}. A digest of an empty "
                          f"workload agrees with every other digest of an empty workload")
        for key in fields:
            if leg[key].strip("0") == "":
                raise Refusal(f"{describe(leg)}: {key} is {leg[key]}. Two legs that both computed "
                              f"nothing are not two legs that agreed")

    disagreements: list[str] = []
    for key in fields:
        values = {leg[key] for leg in legs}
        verdict = "identical on every leg" if len(values) == 1 else f"{len(values)} DIFFERENT VALUES"
        print(f"  {key}: {verdict}")
        for leg in legs:
            print(f"    {leg[key]}  {describe(leg)}")
        if len(values) > 1:
            disagreements.append(key)
    return disagreements


def report_gpu_domain(legs: list[dict[str, str]]) -> None:
    """`m10:pcg-gpu-domain-agreement`, and why this job does not answer it.

    The criterion asks for a GPU execution domain compared against the CPU one on more than one
    vendor's driver. TWO THINGS ARE ABSENT AND ONLY ONE OF THEM IS A RUNNER QUESTION:
    `cy::pcg::ExecutionDomain` is Editor, Cook, Runtime, Streaming and Dynamic, so there is no GPU
    domain in this tree to run at all — `determinism.cross_leg`'s last case asserts exactly that and
    goes red on the day one is added — and no hosted runner in `ci.yml`'s matrices has a device.
    So this prints the state and REFUSES; it does not report an agreement it did not measure.
    """
    print("\ngpu-domain: m10:pcg-gpu-domain-agreement — the GPU execution domain against the CPU one")
    for leg in legs:
        print(f"  {leg.get('pcg-gpu-domain', 'unpublished'):>11}  {describe(leg)}")
    raise Refusal(
        "no leg published a GPU execution domain. `cy::pcg::ExecutionDomain` has no GPU enumerator "
        "— the domain does not exist in this tree — and no runner in ci.yml's matrices has a "
        "device, so there is nothing to compare and nothing here may be reported as agreeing. "
        "m10:pcg-gpu-domain-agreement stays open")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", default="cross-leg-digests",
                        help="where the downloaded digests are (default: cross-leg-digests)")
    parser.add_argument("--pcg", action="store_true",
                        help="also compare the generated world's digest between the legs")
    parser.add_argument("--gpu-domain", action="store_true",
                        help="report m10:pcg-gpu-domain-agreement's state and refuse")
    arguments = parser.parse_args()

    try:
        legs = collect(pathlib.Path(arguments.dir))
        print(f"{len(legs)} leg(s) published a digest into {arguments.dir}:")
        for leg in legs:
            print(f"  {describe(leg)}  <- {leg['source']}")
        check_preconditions(legs)

        claims = ["lockstep"] + (["pcg"] if arguments.pcg else [])
        disagreements = {claim: compare(legs, claim) for claim in claims}
        if arguments.gpu_domain:
            report_gpu_domain(legs)
    except Refusal as refusal:
        print(f"\nREFUSED: {refusal}", file=sys.stderr)
        print("This is not a disagreement between legs. It is a comparison that could not be made, "
              "and it must not be read as one that passed.", file=sys.stderr)
        return 2

    failed = {claim: keys for claim, keys in disagreements.items() if keys}
    if failed:
        print("\nDISAGREEMENT — and this is a FINDING, recorded with the workloads and the digests "
              "above rather than hidden:", file=sys.stderr)
        for claim, keys in failed.items():
            print(f"  {claim}: {', '.join(keys)} differ between architectures", file=sys.stderr)
        return 1

    architectures = sorted({leg["arch"] for leg in legs})
    print(f"\nEvery digest of {', '.join(disagreements)} is identical across "
          f"{len(legs)} legs and {len(architectures)} architectures ({', '.join(architectures)}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run one milestone's ledger sequentially and in parallel, and compare the two verdict for verdict.

A SPEEDUP THAT CHANGES ONE VERDICT IS A REGRESSION, and this is the check that says so. It runs
`roadmap.py milestone <id>` twice against the same tree — once with `--jobs 1`, which is the loop the
scheduler replaced, and once with `--jobs n` — and then compares:

  * the SEQUENCE of criterion labels, so that parallel execution has not reordered the report;
  * each criterion's VERDICT — ok, FAILED, not evaluated — label by label;
  * the ledger's OWN TEXT with the durations masked, because the ledger is read by eye against the
    previous run and a report that says the same things in different words is not the same report.
    What a failing criterion's SUBPROCESS printed is excluded, and the first run of this tool is why:
    `just quality-lint` fans clang-tidy out over twenty-four processes and prints a progress line per
    file as each finishes, and `ctest` schedules its suites; neither emits those lines in the same
    order twice, on this scheduler or the old one. Eight hundred and forty-two lines of that is not
    a disagreement about a verdict, and treating it as one would make this tool unable to pass.

It is deliberately not a unit test: `selftest.py` holds the scheduler's rules against fixtures in
milliseconds, and this holds the real ledger against the real tree in hours. Both are needed —
fixtures cannot show that the corpus's own bodies were classified correctly, and the corpus cannot
be run on every pull request.

Usage: python3 tools/roadmap/ledger_equivalence.py <milestone> [--jobs n] [--out <dir>]
"""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LEDGER = REPO_ROOT / "tools" / "roadmap" / "roadmap.py"

CRITERION = re.compile(r"^==> (\S+)\s")
VERDICT = re.compile(r"^    (ok|FAILED|not evaluated)\b")
#: `(12.3 s)` differs between any two runs of anything and is the one thing that may.
DURATION = re.compile(r"\(\d+\.\d+ s\)")
#: A line the failing criterion's own SUBPROCESS printed, quoted back by the ledger. It is the
#: criterion's output, not the ledger's text, and nothing makes it reproducible: clang-tidy prints a
#: progress line per file across twenty-four processes and ctest schedules its suites. The VERDICT
#: those lines explain is compared; the lines themselves are not.
CAPTURED = re.compile(r"^      \| ")


def corpus_digest(milestone: str) -> str:
    """A digest of the ledger DATA THIS RUN READS, so a comparison across a change to it is not made.

    This repository is worked on by several people and agents at once, and a criterion edited between
    the two runs would make them differ for a reason that has nothing to do with the scheduler —
    which is the one wrong conclusion this tool could reach. So the data is digested before the first
    run and after the second, and a change is reported as what it is: not a disagreement, but a
    comparison that was never valid.

    Only the ledgers in THIS milestone's plan, which is the second thing the first run of this tool
    found: a peer edited `m8c.toml` and `m9.toml` while M0's ledger was running, and digesting every
    file under milestones/ reported that as invalidating a comparison it cannot touch.
    """
    sys.path.insert(0, str(REPO_ROOT / "tools" / "roadmap"))
    import criteria as criteria_module
    import gates as gates_module

    gate_set = gates_module.load()
    plan = criteria_module.build_plan(milestone, gates_module.permanent_milestones(gate_set))
    directory = REPO_ROOT / "tools" / "roadmap"
    paths = [directory / "milestones" / f"{name}.toml" for name in plan.ledgers]
    digest = hashlib.sha256()
    for path in paths + [directory / "gates.toml"]:
        digest.update(path.name.encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()[:16]


def run_ledger(milestone: str, jobs: int, log: Path) -> tuple[int, str, float]:
    started = time.monotonic()
    with log.open("w") as handle:
        completed = subprocess.run(
            [sys.executable, str(LEDGER), "milestone", milestone, "--jobs", str(jobs)],
            cwd=REPO_ROOT, stdout=handle, stderr=subprocess.DEVNULL, check=False)
    return completed.returncode, log.read_text(), time.monotonic() - started


def verdicts(report: str) -> list[tuple[str, str]]:
    """Every criterion in the report, in the order printed, with the verdict it was given."""
    found: list[tuple[str, str]] = []
    label = None
    for line in report.splitlines():
        heading = CRITERION.match(line)
        if heading:
            label = heading.group(1)
            continue
        verdict = VERDICT.match(line)
        if verdict and label is not None:
            found.append((label, verdict.group(1)))
            label = None
    return found


def masked(report: str) -> list[str]:
    """The report from its first criterion onwards, with durations masked.

    The HEADING is excluded on purpose and it is the only thing that is: it says how the run was
    configured — "8 at a time, 81 run alone" — which is exactly the difference between these two
    runs. Everything from the first `==>` to the last line of the summary is the verdict, and that
    is what may not differ.
    """
    lines = report.splitlines()
    first = next((index for index, line in enumerate(lines) if line.startswith("==> ")), 0)
    return [DURATION.sub("(t)", line) for line in lines[first:]
            if not CAPTURED.match(line)]


def _finished(name: str, report: str, found: list) -> list[str]:
    """A COMPARISON OF TWO EMPTY REPORTS AGREES PERFECTLY, and a run that died after printing its
    heading produces exactly that. So the shape of each report is checked before its contents."""
    problems = []
    announced = sum(1 for line in report.splitlines() if line.startswith("==> "))
    if announced != len(found):
        problems.append(f"the {name} run announced {announced} criteria and reported a verdict for "
                        f"{len(found)}: it did not finish")
    if len(found) < 10:
        problems.append(f"the {name} run reported {len(found)} criteria, which is too few for this "
                        "comparison to mean anything")
    return problems


def _out_of_order(one: list, other: list) -> list[str]:
    if [label for label, _ in one] == [label for label, _ in other]:
        return []
    first = next((index for index, pair in enumerate(zip(one, other))
                  if pair[0][0] != pair[1][0]), min(len(one), len(other)))
    return [f"the report ORDER differs: {len(one)} criteria sequentially, {len(other)} in parallel",
            f"  first difference at position {first}"]


def _disagreeing_verdicts(one: list, other: list) -> list[str]:
    return [f"  {label}: sequentially '{left}', in parallel '{right}'"
            for (label, left), (_, right) in zip(one, other) if left != right]


def _differing_text(sequential: str, parallel: str) -> list[str]:
    left_lines, right_lines = masked(sequential), masked(parallel)
    if left_lines == right_lines:
        return []
    differing = [index for index, pair in enumerate(zip(left_lines, right_lines))
                 if pair[0] != pair[1]]
    problems = [f"the ledger's own TEXT differs on {len(differing)} line(s) (durations masked); "
                f"first at line {differing[0] if differing else '?'}"]
    for index in differing[:10]:
        problems.append(f"    - {left_lines[index]}")
        problems.append(f"    + {right_lines[index]}")
    return problems


def compare(sequential: str, parallel: str) -> list[str]:
    """Every way the two runs disagree. An empty list is the claim this tool exists to support."""
    one, other = verdicts(sequential), verdicts(parallel)
    shape = (_finished("sequential", sequential, one) + _finished("parallel", parallel, other))
    if shape:
        return shape
    order = _out_of_order(one, other)
    if order:
        return order
    return _disagreeing_verdicts(one, other) + _differing_text(sequential, parallel)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("milestone")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--out", type=Path, default=REPO_ROOT / "build" / "ledger-parallel")
    arguments = parser.parse_args(argv)

    arguments.out.mkdir(parents=True, exist_ok=True)
    sequential_log = arguments.out / f"{arguments.milestone}-sequential.txt"
    parallel_log = arguments.out / f"{arguments.milestone}-parallel-{arguments.jobs}.txt"

    before = corpus_digest(arguments.milestone)
    print(f"[equivalence] ledger data {before}", flush=True)
    print(f"[equivalence] {arguments.milestone}: sequential run", flush=True)
    sequential_exit, sequential, sequential_s = run_ledger(arguments.milestone, 1, sequential_log)
    print(f"[equivalence] sequential: exit {sequential_exit} in {sequential_s / 60:.1f} min "
          f"({len(verdicts(sequential))} criteria)", flush=True)

    print(f"[equivalence] {arguments.milestone}: parallel run, {arguments.jobs} at a time",
          flush=True)
    parallel_exit, parallel, parallel_s = run_ledger(arguments.milestone, arguments.jobs,
                                                     parallel_log)
    print(f"[equivalence] parallel: exit {parallel_exit} in {parallel_s / 60:.1f} min "
          f"({len(verdicts(parallel))} criteria)", flush=True)

    after = corpus_digest(arguments.milestone)
    problems = compare(sequential, parallel)
    if after != before:
        problems.insert(0, f"the ledger DATA changed between the two runs ({before} -> {after}); "
                           "somebody edited a milestone's criteria while this was running, so the "
                           "two runs were not of the same ledger and nothing can be concluded")
    if sequential_exit != parallel_exit:
        problems.insert(0, f"the ledger's own EXIT CODE differs: {sequential_exit} sequentially, "
                           f"{parallel_exit} in parallel")

    counts: dict[str, int] = {}
    for _, verdict in verdicts(sequential):
        counts[verdict] = counts.get(verdict, 0) + 1
    print()
    print(f"sequential  {sequential_s / 60:7.1f} min   exit {sequential_exit}   "
          + "  ".join(f"{verdict}: {count}" for verdict, count in sorted(counts.items())))
    print(f"parallel    {parallel_s / 60:7.1f} min   exit {parallel_exit}   "
          f"jobs {arguments.jobs}")
    if sequential_s and parallel_s:
        print(f"speedup     {sequential_s / parallel_s:7.2f}x")
    print()
    if problems:
        print(f"VERDICT: THE TWO RUNS DISAGREE — {len(problems)} difference(s):")
        for problem in problems:
            print(problem)
        return 1
    print(f"VERDICT: identical. {len(verdicts(sequential))} criteria, same order, same verdicts, "
          "same text.")
    print(f"  sequential report: {sequential_log}")
    print(f"  parallel report:   {parallel_log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

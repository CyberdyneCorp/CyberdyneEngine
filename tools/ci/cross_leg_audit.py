#!/usr/bin/env python3
"""Run the cross-leg job's OWN comparison against digests this module controls. M11.a, repair 1.

WHAT WENT WRONG, AND IT IS THE EIGHTH OF ITS KIND. Three criteria in three ledgers —

    m9:lockstep-cross-platform            a simulation state hash compared between architectures
    m10:pcg-regeneration-cross-platform   a generated region's digest, likewise
    m11a:cross-leg-digest-job             the job that does both

— were written as a search over `.github/workflows/*.yml` for a job block that contained the words
`download-artifact`, `digest`, `lockstep` or `pcg`. M11.a then built the job those searches were
waiting for, they turned green, and the gate that read them said the true thing about all three:
they are word-greps a dummy job satisfies. This is what a dummy job looks like, and every one of
those three checks passes on it:

    dummy:
      steps:
        - uses: actions/upload-artifact@v4       # publishes nothing
        - uses: actions/download-artifact@v4     # downloads nothing
        - run: echo "lockstep and pcg digests compared"

THE REPAIR IS NOT A BETTER SEARCH. A search over a workflow can only ever measure spelling, and the
job's claim is about BEHAVIOUR: that when two legs disagree, the job goes red. So this module reads
the workflow only far enough to find the comparison — which job publishes a digest from a matrix of
legs, which job downloads those artefacts, and what command that second job RUNS over them — and
then it EXECUTES THAT COMMAND, verbatim, against digest files it wrote itself:

    two architectures, identical digests        the command must exit 0      (or it compares nothing
                                                                              and refuses everything)
    two architectures, one digest changed       the command must exit non-0
    one leg alone                               the command must exit non-0
    two legs, one architecture                  the command must exit non-0
    a digest of zero, an empty workload         the command must exit non-0

The dummy above fails the second case: `echo` exits 0 whatever the legs say. So does a job that
downloads the artefacts and runs a linter, and so does a job whose comparison was deleted — which is
the mutation `just roadmap-falsify` applies to prove these three criteria can still go red.

WHAT THIS DOES NOT CLAIM. It does not run the engine, so it says nothing about whether the digests a
real leg publishes are digests of anything; `m11a:cross-leg-digest-published` builds the tree and
runs `determinism.cross_leg`'s five cases for that, and the fixtures here are cross-checked against
that publisher's own field list so the two cannot drift apart silently. And it does not say the
architectures AGREE — only continuous integration, with two architectures in it, can answer that,
which is why `m11a:lockstep-agrees-across-architectures` carries `where = "ci"`.

Governed by: delivery-roadmap (Milestone exit criteria are executable, Forbidden roadmap patterns),
simulation-and-determinism (CrossPlatform), procedural-content-generation (Deterministic derivation).
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field

HERE = pathlib.Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent

sys.path.insert(0, str(HERE))

import cross_leg_digests as comparator  # noqa: E402  — the fields it reads, from the source
import test_cross_leg_digests as digests  # noqa: E402  — one definition of a well-formed leg


# --- Reading the workflow, as far as the comparison and no further --------------------------------
#
# A YAML library would be one more thing a hosted runner has to have installed, and this module is
# read by three permanent gates on four operating systems. What is needed is a small subset — nested
# mappings, sequences of mappings, flow mappings in a matrix, and block scalars for `run:` — so it is
# read here rather than depended upon. `--selftest` parses this repository's own ci.yml and asserts
# what it found, so a reader that stopped understanding the file is a failure rather than a silence.


def _indentation(line: str) -> int:
    return len(line) - len(line.lstrip(" "))


def _skippable(line: str) -> bool:
    stripped = line.strip()
    return not stripped or stripped.startswith("#")


def _scalar(raw: str) -> str:
    """One scalar value: quotes removed, and a trailing `# comment` on an unquoted value dropped."""
    raw = raw.strip()
    if raw[:1] in ("'", '"'):
        quote = raw[0]
        end = raw.find(quote, 1)
        return raw[1:end] if end > 0 else raw[1:]
    return re.sub(r"\s+#.*$", "", raw).strip()


def _flow_mapping(text: str) -> dict:
    """`{ label: linux-x86_64, os: ubuntu-24.04 }` — the shape a matrix `include:` entry uses."""
    inner = text.strip().lstrip("{").rstrip("}")
    entry = {}
    for piece in inner.split(","):
        key, separator, value = piece.partition(":")
        if separator and key.strip():
            entry[key.strip()] = _scalar(value)
    return entry


_BLOCK_SCALAR = ("|", "|-", "|+", ">", ">-", ">+")


class _Reader:
    """The workflow file, one logical line at a time."""

    def __init__(self, text: str) -> None:
        self.lines = text.expandtabs(8).splitlines()
        self.at = 0

    def peek(self) -> tuple[int, str] | None:
        while self.at < len(self.lines) and _skippable(self.lines[self.at]):
            self.at += 1
        if self.at >= len(self.lines):
            return None
        return _indentation(self.lines[self.at]), self.lines[self.at].strip()

    def block(self, indent: int):
        """Whatever begins at `indent`: a mapping, a sequence, or nothing."""
        head = self.peek()
        if head is None or head[0] < indent:
            return {}
        return self.sequence(indent) if head[1].startswith("- ") else self.mapping(indent)

    def mapping(self, indent: int) -> dict:
        result: dict = {}
        while True:
            head = self.peek()
            if head is None or head[0] != indent or head[1].startswith("- ") or ":" not in head[1]:
                return result
            self.at += 1
            key, value = self.pair(head[1], indent)
            result[key] = value

    def sequence(self, indent: int) -> list:
        items: list = []
        while True:
            head = self.peek()
            if head is None or head[0] != indent or not head[1].startswith("- "):
                return items
            self.at += 1
            items.append(self.item(head[1][2:].strip(), indent + 2))

    def item(self, content: str, indent: int):
        if content.startswith("{"):
            return _flow_mapping(content)
        if ":" not in content:
            return _scalar(content)
        key, value = self.pair(content, indent)
        entry = {key: value}
        entry.update(self.mapping(indent))
        return entry

    def pair(self, content: str, indent: int) -> tuple[str, object]:
        """One `key: value`, having already consumed its line."""
        key, _, raw = content.partition(":")
        raw = raw.strip()
        if raw in _BLOCK_SCALAR:
            return _scalar(key), self.scalar_block(indent)
        if raw:
            return _scalar(key), (_flow_mapping(raw) if raw.startswith("{") else _scalar(raw))
        nested = self.peek()
        if nested is None or nested[0] <= indent:
            return _scalar(key), ""
        return _scalar(key), self.block(nested[0])

    def scalar_block(self, indent: int) -> str:
        body: list[str] = []
        while self.at < len(self.lines):
            line = self.lines[self.at]
            if line.strip() and _indentation(line) <= indent:
                break
            body.append(line)
            self.at += 1
        while body and not body[-1].strip():
            body.pop()
        pad = min((_indentation(line) for line in body if line.strip()), default=0)
        return "\n".join(line[pad:] if line.strip() else "" for line in body)


def parse_workflow(text: str) -> dict:
    return _Reader(text).block(0)


@dataclass(frozen=True)
class Job:
    """One workflow job, in the terms this audit needs."""

    workflow: str
    name: str
    needs: tuple[str, ...]
    runs_on: str
    legs: tuple[dict, ...]
    steps: tuple[dict, ...]

    @property
    def label(self) -> str:
        return f"{self.workflow}:{self.name}"

    def using(self, action: str) -> list[dict]:
        return [step for step in self.steps if action in str(step.get("uses", ""))]

    @property
    def runners(self) -> tuple[str, ...]:
        """The distinct runner images this job's legs ask for.

        `runs-on: ${{ matrix.os }}` is answered by the matrix; a literal is one image however many
        legs there are, which is what makes "two legs" and "two runners" different questions.
        """
        key = re.search(r"matrix\.([A-Za-z0-9_-]+)", self.runs_on)
        if not key:
            return (self.runs_on,) if self.runs_on else ()
        return tuple(sorted({str(leg[key.group(1)]) for leg in self.legs if key.group(1) in leg}))


def _as_list(value) -> tuple[str, ...]:
    if isinstance(value, list):
        return tuple(str(item) for item in value)
    return (str(value),) if value else ()


def _legs(job: dict) -> tuple[dict, ...]:
    matrix = job.get("strategy", {})
    matrix = matrix.get("matrix", {}) if isinstance(matrix, dict) else {}
    if not isinstance(matrix, dict):
        return ()
    include = matrix.get("include", [])
    legs = [entry for entry in include if isinstance(entry, dict)] if isinstance(include, list) else []
    for key, value in matrix.items():
        if key != "include" and isinstance(value, list):
            legs.extend({key: str(item)} for item in value)
    return tuple(legs)


def jobs_of(path: pathlib.Path) -> list[Job]:
    document = parse_workflow(path.read_text(encoding="utf-8"))
    declared = document.get("jobs", {})
    found = []
    for name, body in declared.items() if isinstance(declared, dict) else ():
        if not isinstance(body, dict):
            continue
        steps = body.get("steps", [])
        found.append(Job(workflow=path.name, name=name, needs=_as_list(body.get("needs")),
                         runs_on=str(body.get("runs-on", "")), legs=_legs(body),
                         steps=tuple(step for step in steps if isinstance(step, dict))))
    return found


def workflows_in(directory: pathlib.Path) -> list[Job]:
    return [job for path in sorted(directory.glob("*.y*ml")) for job in jobs_of(path)]


# --- Finding the comparison ------------------------------------------------------------------------


@dataclass(frozen=True)
class Comparison:
    """A publishing job, the job that downloads what it published, and the command that compares."""

    publisher: Job
    comparer: Job
    artefact: str
    directory: str
    command: str


def _stem(name: str) -> str:
    """The constant part of an artefact name: `cross-leg-digest-${{ matrix.label }}` -> the prefix."""
    return re.split(r"\$\{\{|\*", str(name))[0].strip()


def _uploads_a_digest(step: dict) -> bool:
    with_ = step.get("with", {})
    if not isinstance(with_, dict):
        return False
    return "digest" in f"{with_.get('name', '')} {with_.get('path', '')}".lower()


def publishers(jobs: list[Job]) -> list[Job]:
    return [job for job in jobs if any(_uploads_a_digest(step) for step in job.using("upload-artifact"))]


def _downloads_from(job: Job, publisher: Job) -> tuple[str, str] | None:
    """The artefact pattern and the directory this job downloads the publisher's uploads into."""
    uploaded = [step.get("with", {}) for step in publisher.using("upload-artifact")
                if _uploads_a_digest(step)]
    for step in job.using("download-artifact"):
        with_ = step.get("with", {})
        if not isinstance(with_, dict):
            continue
        wanted = _stem(with_.get("pattern") or with_.get("name") or "")
        directory = str(with_.get("path", "")).strip()
        for published in uploaded:
            name = _stem(published.get("name", ""))
            if wanted and name and (wanted.startswith(name) or name.startswith(wanted)) and directory:
                return f"{wanted}*", directory
    return None


def _pair(publisher: Job, job: Job) -> tuple[Comparison | None, str]:
    """Whether `job` compares what `publisher` published, or why it does not."""
    downloaded = _downloads_from(job, publisher)
    if downloaded is None:
        return None, ""
    artefact, directory = downloaded
    if publisher.name not in job.needs:
        return None, (f"{job.label} downloads {artefact} and does not `needs:` {publisher.name}, "
                      f"so it can run before anything published")
    commands = [str(step["run"]) for step in job.steps
                if "run" in step and directory in str(step["run"])]
    if not commands:
        return None, (f"{job.label} downloads {artefact} into {directory}/ and no `run:` step of it "
                      f"reads {directory}/: it collects the digests and compares nothing")
    return Comparison(publisher, job, artefact, directory, "\n".join(commands)), ""


def comparisons(jobs: list[Job]) -> tuple[list[Comparison], list[str]]:
    """Every publish-then-compare pair in these workflows, and why the near misses are not pairs."""
    found: list[Comparison] = []
    rejected: list[str] = []
    for publisher in publishers(jobs):
        for job in jobs:
            pair, refusal = _pair(publisher, job)
            if pair is not None:
                found.append(pair)
            elif refusal:
                rejected.append(refusal)
    return found, rejected


# --- The fixtures the comparison is run against ---------------------------------------------------
#
# `digests.leg()` is the well-formed leg `tools/ci/test_cross_leg_digests.py` already defines against
# the publisher's own field list, so there is ONE definition of what a leg publishes rather than two
# that drift.


def _leg(**overrides: str) -> dict:
    return digests.leg(**overrides)


AGREEING = ("two architectures published identical digests", 0,
            [_leg(label="alpha"), _leg(label="beta", arch="arm64")],
            "a command that cannot pass when the legs agree is refusing, not comparing")

CASES = {
    "lockstep": (
        AGREEING,
        ("the simulation state hash differs between the two architectures", 1,
         [_leg(label="alpha"), _leg(label="beta", arch="arm64", sim_state_digest="0123456789abcdef")],
         "THE CASE THE WORD-GREP COULD NOT SEE: a job that compares nothing passes this"),
        ("one leg published, and one leg agrees with itself", 1, [_leg(label="alpha")],
         "the single-leg pass m9:lockstep-cross-platform was declared a gap to avoid"),
        ("two legs of ONE architecture, named differently", 1,
         [_leg(label="linux-x86_64"), _leg(label="linux-arm64")],
         "the architecture compared is the one the binary detected, never the runner's label"),
        ("both legs published a digest of zero", 1,
         [_leg(label="alpha", sim_state_digest="0000000000000000"),
          _leg(label="beta", arch="arm64", sim_state_digest="0000000000000000")],
         "two legs that both computed nothing are not two legs that agreed"),
    ),
    "pcg": (
        AGREEING,
        ("the generated world's digest differs between the two architectures", 1,
         [_leg(label="alpha"), _leg(label="beta", arch="arm64", pcg_world_digest="0123456789abcdef")],
         "THE CASE THE WORD-GREP COULD NOT SEE, and the one that needs the comparison to ask for "
         "the generation claim at all rather than the simulation claim alone"),
        ("one leg published, and one leg agrees with itself", 1, [_leg(label="alpha")],
         "a region that reproduces on one architecture has not reproduced on a second"),
        ("both legs generated an empty world", 1,
         [_leg(label="alpha", pcg_regions="0"), _leg(label="beta", arch="arm64", pcg_regions="0")],
         "a digest of an empty workload agrees with every other digest of an empty workload"),
    ),
}

#: `m11a:cross-leg-digest-job` claims the job itself, so it is answerable only by every case at once.
CASES["job"] = tuple({case[0]: case for case in CASES["lockstep"] + CASES["pcg"]}.values())


def _write(directory: pathlib.Path, legs: list[dict]) -> None:
    """The legs, laid out as `actions/download-artifact` unpacks several artefacts: one each."""
    for index, fields in enumerate(legs):
        nested = directory / f"cross-leg-digest-{index}"
        nested.mkdir(parents=True)
        body = "".join(f"{key} {value}\n" for key, value in fields.items())
        (nested / f"{fields.get('label', index)}.digest").write_text(body, encoding="utf-8")


def run_against(comparison: Comparison, legs: list[dict]) -> tuple[int, str]:
    """The comparison job's own command, over digests written into the directory it downloads into."""
    with tempfile.TemporaryDirectory(prefix="cy-cross-leg-audit-") as directory:
        root = pathlib.Path(directory) / "digests"
        root.mkdir()
        _write(root, legs)
        command = comparison.command.replace(comparison.directory, str(root))
        finished = subprocess.run(  # noqa: S602 — the command is this repository's own workflow
            ["bash", "-c", command], cwd=REPO_ROOT, capture_output=True, text=True, check=False,
            env={**os.environ, "CY_CROSS_LEG_AUDIT": "1"}, timeout=600)
    return finished.returncode, finished.stdout + finished.stderr


# --- The audit ------------------------------------------------------------------------------------


@dataclass
class Audit:
    claim: str
    findings: list[str] = field(default_factory=list)
    passed: int = 0

    def failed(self, detail: str) -> None:
        self.findings.append(detail)


def _check_fixture_agrees_with_the_publisher(audit: Audit) -> None:
    """Every field the comparator reads is one the engine's publisher emits, and one a fixture has.

    Without this the fixtures could drift into a private format that the comparator accepts and no
    leg produces — a green audit over a comparison no real leg could enter.
    """
    source = REPO_ROOT / "tests" / "determinism" / "test_cross_leg.cpp"
    if not source.is_file():
        audit.failed(f"{source.relative_to(REPO_ROOT)} is missing: the publisher these fixtures "
                     "imitate is not in this tree")
        return
    published = source.read_text(encoding="utf-8")
    fields = set(comparator.REQUIRED)
    for keys, workload, _asked_by in comparator.CLAIMS.values():
        fields.update((*keys, workload))
    for name in sorted(fields):
        if f'"{name}"' not in published:
            audit.failed(f"the comparator reads '{name}' and {source.name} never publishes it")
        if name not in digests.BASE:
            audit.failed(f"the comparator reads '{name}' and this audit's fixtures omit it, so a "
                         f"comparison that dropped it would still be exercised")


def _check_the_legs(audit: Audit, comparison: Comparison) -> None:
    publisher = comparison.publisher
    if len(publisher.legs) < 2:
        audit.failed(f"{publisher.label} publishes {comparison.artefact} from "
                     f"{len(publisher.legs)} leg(s): one leg cannot disagree with anything")
    if len(publisher.runners) < 2:
        audit.failed(f"{publisher.label} runs every leg on {publisher.runners or ('nothing',)}: "
                     "one runner image is one architecture, whatever the legs are named")


def _check_the_comparison(audit: Audit, comparison: Comparison) -> None:
    for name, expected, legs, why in CASES[audit.claim]:
        code, output = run_against(comparison, list(legs))
        red = "exit 0" if expected == 0 else "a non-zero exit"
        verdict = (code == 0) if expected == 0 else (code != 0)
        print(f"  {'ok  ' if verdict else 'FAIL'}  {name}: expected {red}, got exit {code}")
        if verdict:
            audit.passed += 1
            continue
        audit.failed(f"{comparison.comparer.label} exits {code} when {name} — expected {red}. {why}")
        for line in output.splitlines()[-12:]:
            print(f"        | {line}")


def audit_claim(claim: str, directory: pathlib.Path) -> Audit:
    audit = Audit(claim)
    print(f"claim: {claim}\nworkflows: {directory}")
    _check_fixture_agrees_with_the_publisher(audit)
    jobs = workflows_in(directory)
    found, rejected = comparisons(jobs)
    print(f"{len(jobs)} job(s) read; {len(found)} publish a digest from a matrix and compare what "
          f"another job downloaded: {[pair.comparer.label for pair in found] or 'none'}")
    for detail in rejected:
        print(f"  not a comparison: {detail}")
    if not found:
        audit.failed("no job in these workflows publishes a digest per leg and runs a command over "
                     "the artefacts a second job downloaded. The legs exist and run independently; "
                     "the comparison between them does not.")
        for detail in rejected:
            audit.failed(detail)
        return audit
    for comparison in found:
        print(f"\n{comparison.publisher.label} publishes {comparison.artefact} from "
              f"{len(comparison.publisher.legs)} legs on {', '.join(comparison.publisher.runners)}")
        print(f"{comparison.comparer.label} downloads them into {comparison.directory}/ and runs:")
        for line in comparison.command.splitlines():
            print(f"    {line}")
        _check_the_legs(audit, comparison)
        _check_the_comparison(audit, comparison)
    return audit


# --- The audit's own negative fixtures ------------------------------------------------------------
#
# A rule that stopped firing looks exactly like a workflow with nothing wrong in it, which is why
# `check_workflows.py` and `test_cross_leg_digests.py` both carry fixtures. THE FIRST FIXTURE IS THE
# DUMMY JOB THE GATE DESCRIBED: it says every word the three word-greps looked for and compares
# nothing, and it must be refused here.

_REAL_COMPARISON = """
      - name: One leg's digest against another's
        run: just test-determinism --compare-legs --pcg --digests cross-leg-digests
"""

_FIXTURES = {
    "dummy": ("""
jobs:
  publish:
    strategy:
      matrix:
        include:
          - { label: linux-x86_64, os: ubuntu-24.04 }
          - { label: linux-arm64, os: ubuntu-24.04-arm }
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/upload-artifact@v4
        with:
          name: cross-leg-digest-${{ matrix.label }}
          path: cross-leg-digests/${{ matrix.label }}.digest
  compare:
    needs: publish
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/download-artifact@v4
        with:
          pattern: cross-leg-digest-*
          path: cross-leg-digests
      - name: lockstep and pcg digests compared between architectures
        run: echo "cross-leg-digests compared: lockstep and pcg agree"
""", "the dummy job: every word the three greps looked for, and no comparison"),

    "publication-only": ("""
jobs:
  publish:
    strategy:
      matrix:
        include:
          - { label: linux-x86_64, os: ubuntu-24.04 }
          - { label: linux-arm64, os: ubuntu-24.04-arm }
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/upload-artifact@v4
        with:
          name: cross-leg-digest-${{ matrix.label }}
          path: cross-leg-digests/${{ matrix.label }}.digest
""", "a publication is not a comparison"),

    "one-leg": ("""
jobs:
  publish:
    strategy:
      matrix:
        include:
          - { label: linux-x86_64, os: ubuntu-24.04 }
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/upload-artifact@v4
        with:
          name: cross-leg-digest-${{ matrix.label }}
          path: cross-leg-digests/${{ matrix.label }}.digest
  compare:
    needs: publish
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/download-artifact@v4
        with:
          pattern: cross-leg-digest-*
          path: cross-leg-digests
""" + _REAL_COMPARISON, "one leg publishing is one leg agreeing with itself"),

    "unordered": ("""
jobs:
  publish:
    strategy:
      matrix:
        include:
          - { label: linux-x86_64, os: ubuntu-24.04 }
          - { label: linux-arm64, os: ubuntu-24.04-arm }
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/upload-artifact@v4
        with:
          name: cross-leg-digest-${{ matrix.label }}
          path: cross-leg-digests/${{ matrix.label }}.digest
  compare:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/download-artifact@v4
        with:
          pattern: cross-leg-digest-*
          path: cross-leg-digests
""" + _REAL_COMPARISON, "a comparison that does not `needs:` the publication can run before it"),

    "refuses-everything": ("""
jobs:
  publish:
    strategy:
      matrix:
        include:
          - { label: linux-x86_64, os: ubuntu-24.04 }
          - { label: linux-arm64, os: ubuntu-24.04-arm }
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/upload-artifact@v4
        with:
          name: cross-leg-digest-${{ matrix.label }}
          path: cross-leg-digests/${{ matrix.label }}.digest
  compare:
    needs: publish
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/download-artifact@v4
        with:
          pattern: cross-leg-digest-*
          path: cross-leg-digests
      - run: test -d cross-leg-digests && exit 1
""", "a job that fails whatever the legs published has measured nothing either"),

    "one-runner": ("""
jobs:
  publish:
    strategy:
      matrix:
        include:
          - { label: leg-one, os: ubuntu-24.04 }
          - { label: leg-two, os: ubuntu-24.04 }
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/upload-artifact@v4
        with:
          name: cross-leg-digest-${{ matrix.label }}
          path: cross-leg-digests/${{ matrix.label }}.digest
  compare:
    needs: publish
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/download-artifact@v4
        with:
          pattern: cross-leg-digest-*
          path: cross-leg-digests
""" + _REAL_COMPARISON, "two legs of one runner image are not two architectures"),
}


def _reader_agrees_with_check_workflows() -> list[str]:
    """The job names this module reads are the job names `check_workflows.py` reads.

    A hand-written reader over a workflow is a thing that can quietly stop understanding the file,
    and a reader that found no jobs would report "no comparison" — a red for the wrong reason, which
    is a false green upside down. `check_workflows.py` finds jobs by an unrelated method (a regex
    over `^  <name>:`), so the two disagreeing is a finding rather than a matter of taste.
    """
    import check_workflows  # noqa: PLC0415 — only the selftest needs it

    complaints = []
    for path in sorted((REPO_ROOT / ".github" / "workflows").glob("*.y*ml")):
        mine = {job.name for job in jobs_of(path)}
        theirs = {command.job for command in check_workflows.commands_in(path)} - {"?"}
        missing = sorted(theirs - mine)
        if missing:
            complaints.append(f"{path.name}: check_workflows.py sees {', '.join(missing)} and this "
                              f"reader does not — it has stopped understanding the file")
    return complaints


def selftest() -> int:
    """Every fixture above must be REFUSED, and this repository's own workflows must pass."""
    failures = 0
    complaints = _reader_agrees_with_check_workflows()
    for complaint in complaints:
        print(f"FAIL {complaint}")
    failures += len(complaints)
    if not complaints:
        print("ok   the workflow reader finds every job check_workflows.py finds\n")
    for name, (text, why) in _FIXTURES.items():
        with tempfile.TemporaryDirectory(prefix="cy-audit-fixture-") as directory:
            path = pathlib.Path(directory)
            (path / "fixture.yml").write_text(text, encoding="utf-8")
            audit = audit_claim("job", path)
        if audit.findings:
            print(f"ok   {name}: refused — {why}\n")
            continue
        failures += 1
        print(f"FAIL {name}: ACCEPTED, and it must not be — {why}\n")

    audit = audit_claim("job", REPO_ROOT / ".github" / "workflows")
    if audit.findings:
        failures += 1
        print("FAIL this repository's own workflows do not carry a comparison this audit accepts")
        for finding in audit.findings:
            print(f"     {finding}")
    else:
        print("ok   this repository's own workflows carry a comparison that discriminates")
    total = len(_FIXTURES) + 2
    print(f"\ncross-leg audit selftest: {total - failures}/{total} passed")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--claim", choices=sorted(CASES), default="job",
                        help="which ledger's claim to audit (default: the job itself)")
    parser.add_argument("--workflows", default=str(REPO_ROOT / ".github" / "workflows"),
                        help="the workflow directory to read")
    parser.add_argument("--selftest", action="store_true",
                        help="run the audit against fixtures it must refuse")
    arguments = parser.parse_args(argv)
    if arguments.selftest:
        return selftest()

    audit = audit_claim(arguments.claim, pathlib.Path(arguments.workflows))
    print(f"\n{audit.passed} of {len(CASES[audit.claim])} case(s) behaved as the claim requires")
    if audit.findings:
        print("\nTHE COMPARISON DOES NOT ANSWER THIS CLAIM:", file=sys.stderr)
        for finding in audit.findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print("the job publishes a digest from two runner images, a second job downloads them, and the "
          "command it runs passes when the legs agree and goes RED on every way of not comparing.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

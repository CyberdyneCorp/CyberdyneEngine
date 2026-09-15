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

AND A COMMAND IS NOT A JOB, WHICH IS WHAT THE REPAIR-2 GATE PROVED BY BREAKING THE WORKFLOW THREE
WAYS AND WATCHING ALL THREE CRITERIA STAY GREEN. `if: false` on the comparison job: nothing is ever
compared, and because GitHub scores a SKIPPED job as success the whole pipeline stays green too. The
matrix leg's publishing step deleted: no leg computes a digest, and four legs upload a path nothing
wrote. That step's command replaced by `echo`, or its flag renamed: the leg runs, exits, and routes
nothing to the path it uploads. So two more questions are asked here, and both fail CLOSED —

    IS THE COMPARISON SCHEDULED?   every `if:` on the publishing job, the comparison job, the chain
                                   of `needs:` between them and each step the comparison is made of,
                                   evaluated against the workflow's OWN `on:` triggers. A condition
                                   this reader cannot evaluate is a finding, never a pass.
    DOES A LEG PUBLISH ANYTHING?   the leg's own publishing command is RUN, with the path it uploads
                                   redirected into a directory this check has deliberately not
                                   created and with the build made impossible. A command that routes
                                   that path creates it before it fails; `echo` exits 0 and creates
                                   nothing; a renamed flag fails having touched nothing at all.

WHAT THIS DOES NOT CLAIM, AND THE SECOND OF THESE IS A HOLE LEFT OPEN DELIBERATELY. A publishing
step that creates the directory and then fails on every run would satisfy the routing check — and it
would also be RED in the pipeline on every run, which is the state this whole module exists to tell
apart from green. The line drawn here is between a step that is green and publishes nothing and a
step that is red; the first is invisible and is what is checked for, the second announces itself.
And it does not run the engine, so it says nothing about whether the digests a
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
    #: The job's own `if:`, and the triggers of the file it lives in. Together they answer whether
    #: this job is ever SCHEDULED, which is a different question from what its steps say.
    condition: str = ""
    events: tuple[Event, ...] = ()

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
    events = events_of(document)
    found = []
    for name, body in declared.items() if isinstance(declared, dict) else ():
        if not isinstance(body, dict):
            continue
        steps = body.get("steps", [])
        found.append(Job(workflow=path.name, name=name, needs=_as_list(body.get("needs")),
                         runs_on=str(body.get("runs-on", "")), legs=_legs(body),
                         steps=tuple(step for step in steps if isinstance(step, dict)),
                         condition=str(body.get("if", "")), events=events))
    return found


def workflows_in(directory: pathlib.Path) -> list[Job]:
    return [job for path in sorted(directory.glob("*.y*ml")) for job in jobs_of(path)]


# --- Does the comparison RUN AT ALL? --------------------------------------------------------------
#
# THE NINTH UNFALSIFIABLE CHECK, AND IT IS THIS MODULE'S OWN. Everything above proves things about a
# COMMAND. The claim is about a JOB, and M11.a's repair-2 gate showed the difference by breaking the
# workflow three ways and watching all three criteria stay GREEN:
#
#   `if: always()` -> `if: false` on the comparison job   the comparison never runs. GitHub scores a
#                                                         skipped job as SUCCESS, so the pipeline is
#                                                         green too, and nothing is compared.
#   the publishing step deleted from the matrix leg       no leg computes a digest; the legs upload a
#                                                         path nothing wrote.
#   that step's command replaced, or its flag renamed     the leg runs, exits, and routes nothing to
#                                                         the path it uploads.
#
# A command that discriminates, inside a job that never runs, over an artefact nobody wrote, is the
# dummy job again with more lines in it. So the reader below answers two more questions, and both of
# them fail CLOSED — a condition this evaluator cannot read is a FINDING, never a pass, because "I
# could not tell whether the comparison runs" and "the comparison runs" must never print the same.
#
# The subset evaluated is the subset a job's `if:` uses: `always()`, `success()`, `failure()`,
# `cancelled()`, `github.event_name`, `github.ref`, string and boolean literals, `==`, `!=`, `!`,
# `&&`, `||` and parentheses. Anything else raises. The contexts are the workflow's OWN `on:`
# triggers, one per event, with `needs:` assumed to have succeeded — so a job that is unreachable
# here is unreachable on every event the workflow declares, which is the only way to be sure a
# comparison that is scheduled by nothing is not read as a comparison that agreed.


class Unreadable(Exception):
    """A condition this reader cannot evaluate. It is reported as a finding, never as a pass."""


@dataclass(frozen=True)
class Event:
    """One trigger of a workflow, as far as a job's `if:` can see it."""

    name: str
    ref: str

    def __str__(self) -> str:
        return self.name


#: The ref each trigger arrives with. `pull_request` is the one that is not a branch, and the one a
#: condition restricting a job to `refs/heads/main` excludes.
_REF_OF = {"pull_request": "refs/pull/7/merge"}


def events_of(document: dict) -> tuple[Event, ...]:
    triggers = document.get("on", "")
    names = tuple(triggers) if isinstance(triggers, dict) else _as_list(triggers)
    return tuple(Event(name, _REF_OF.get(name, "refs/heads/main")) for name in names if name)


_TOKEN = re.compile(r"(\|\||&&|==|!=|[()!,]|'(?:[^']|'')*'|[A-Za-z_][A-Za-z0-9_.-]*|[0-9]+)")


def _tokens(text: str) -> list[str]:
    text = text.replace("${{", " ").replace("}}", " ")
    found, at = [], 0
    while at < len(text):
        if text[at].isspace():
            at += 1
            continue
        match = _TOKEN.match(text, at)
        if not match:
            raise Unreadable(f"cannot read {text[at]!r} in `{' '.join(text.split())}`")
        found.append(match.group(1))
        at = match.end()
    return found


def _truth(value) -> bool:
    return bool(value)


def _same(left, right) -> bool:
    if isinstance(left, bool) or isinstance(right, bool):
        return _truth(left) == _truth(right)
    return str(left) == str(right)


class _Condition:
    """One `if:` expression, evaluated for one event. Every unknown raises."""

    #: `needs:` is assumed to have succeeded, which is the nominal run. `failure()` and `cancelled()`
    #: are therefore false: a job that runs ONLY when something failed does not run on a green
    #: pipeline, and a comparison nobody reaches on a green pipeline has not compared anything.
    FUNCTIONS = {"always": True, "success": True, "failure": False, "cancelled": False}

    def __init__(self, tokens: list[str], event: Event) -> None:
        self.tokens, self.at, self.event = tokens, 0, event

    # --- the grammar -----------------------------------------------------------------------------

    def value(self):
        result = self.disjunction()
        if self.at != len(self.tokens):
            raise Unreadable(f"unexpected `{self.tokens[self.at]}`")
        return result

    def disjunction(self):
        result = self.conjunction()
        while self.take("||"):
            result = _truth(self.conjunction()) or _truth(result)
        return result

    def conjunction(self):
        result = self.comparison()
        while self.take("&&"):
            result = _truth(self.comparison()) and _truth(result)
        return result

    def comparison(self):
        left = self.unary()
        for operator in ("==", "!="):
            if self.take(operator):
                agree = _same(left, self.unary())
                return agree if operator == "==" else not agree
        return left

    def unary(self):
        if self.take("!"):
            return not _truth(self.unary())
        return self.primary()

    def primary(self):
        token = self.next()
        if token == "(":
            inner = self.disjunction()
            if not self.take(")"):
                raise Unreadable("unbalanced parentheses")
            return inner
        if token.startswith("'"):
            return token[1:-1].replace("''", "'")
        if token in ("true", "false"):
            return token == "true"
        if token.isdigit():
            return int(token)
        if self.peek() == "(":
            return self.call(token)
        return self.context(token)

    def call(self, name: str):
        self.take("(")
        depth, arguments = 1, 0
        while depth:
            token = self.next()
            arguments += token not in ("(", ")")
            depth += {"(": 1, ")": -1}.get(token, 0)
        if name not in self.FUNCTIONS or arguments:
            raise Unreadable(f"`{name}(...)` is a function this reader does not evaluate")
        return self.FUNCTIONS[name]

    def context(self, name: str):
        if name == "github.event_name":
            return self.event.name
        if name == "github.ref":
            return self.event.ref
        raise Unreadable(f"`{name}` is a context this reader does not evaluate")

    # --- the cursor ------------------------------------------------------------------------------

    def peek(self) -> str:
        return self.tokens[self.at] if self.at < len(self.tokens) else ""

    def next(self) -> str:
        token = self.peek()
        if not token:
            raise Unreadable("the condition ends where a value was expected")
        self.at += 1
        return token

    def take(self, token: str) -> bool:
        if self.peek() != token:
            return False
        self.at += 1
        return True


def runs_on_event(condition: str, event: Event) -> bool:
    """Whether a job or step carrying this `if:` runs on this event, its `needs:` having succeeded."""
    text = str(condition).strip()
    if not text:
        return True
    return _truth(_Condition(_tokens(text), event).value())


def blocked_on(job: Job, event: Event, index: dict[str, Job], seen: tuple[str, ...] = ()) -> str:
    """Empty when the job runs on this event; otherwise the link in the chain that stops it."""
    if not runs_on_event(job.condition, event):
        return f"{job.label} carries `if: {' '.join(job.condition.split())}`, false on {event}"
    for need in job.needs:
        upstream = index.get(f"{job.workflow}:{need}")
        if upstream is None:
            return f"{job.label} needs `{need}`, which is not a job in {job.workflow}"
        if need in seen:
            return f"{job.label} and `{need}` need each other, so neither is ever scheduled"
        stopped = blocked_on(upstream, event, index, (*seen, job.name))
        if stopped:
            return f"{stopped} — and {job.label} needs it"
    return ""


def index_of(jobs: list[Job]) -> dict[str, Job]:
    return {job.label: job for job in jobs}


# --- Finding the comparison ------------------------------------------------------------------------


@dataclass(frozen=True)
class Comparison:
    """A publishing job, the job that downloads what it published, and the command that compares.

    IT REMEMBERS THE STEPS AND NOT ONLY THE COMMAND, because the three mutations that killed the
    first repair were all steps: the one that computes the digest, deleted; the one that compares,
    conditioned away; the job around them, `if: false`. A comparison is the chain, and a chain is
    checked link by link.
    """

    publisher: Job
    comparer: Job
    artefact: str
    directory: str
    command: str
    #: The steps the chain is made of: what computes the digest, what uploads it, what downloads it,
    #: and what compares. `produced` is empty when no command in the publishing job writes the path
    #: that job uploads, which is the deletion this audit exists to notice.
    produced: tuple[dict, ...] = ()
    compared: tuple[dict, ...] = ()
    uploaded: dict = field(default_factory=dict)
    downloaded: dict = field(default_factory=dict)
    #: The constant part of the uploaded path — `cross-leg-digests/` — which is what a publishing
    #: command has to name and what this audit redirects when it runs one.
    stem: str = ""


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


def _downloads_from(job: Job, publisher: Job) -> tuple[str, str, dict, dict] | None:
    """The artefact, the directory it lands in, and the two steps that move it."""
    uploads = [step for step in publisher.using("upload-artifact") if _uploads_a_digest(step)]
    for step in job.using("download-artifact"):
        with_ = step.get("with", {})
        if not isinstance(with_, dict):
            continue
        wanted = _stem(with_.get("pattern") or with_.get("name") or "")
        directory = str(with_.get("path", "")).strip()
        for upload in uploads:
            name = _stem(upload.get("with", {}).get("name", ""))
            if wanted and name and (wanted.startswith(name) or name.startswith(wanted)) and directory:
                return f"{wanted}*", directory, upload, step
    return None


def _pair(publisher: Job, job: Job) -> tuple[Comparison | None, str]:
    """Whether `job` compares what `publisher` published, or why it does not."""
    downloaded = _downloads_from(job, publisher)
    if downloaded is None:
        return None, ""
    artefact, directory, upload, download = downloaded
    if publisher.name not in job.needs:
        return None, (f"{job.label} downloads {artefact} and does not `needs:` {publisher.name}, "
                      f"so it can run before anything published")
    compared = [step for step in job.steps if "run" in step and directory in str(step["run"])]
    if not compared:
        return None, (f"{job.label} downloads {artefact} into {directory}/ and no `run:` step of it "
                      f"reads {directory}/: it collects the digests and compares nothing")
    # THE PUBLISHING SIDE, FOUND THE SAME WAY THE COMPARING SIDE IS: by the path, not by the name of
    # a step or a word in it. A step whose command never mentions the path this job uploads cannot
    # be what writes it.
    stem = _stem(str(upload.get("with", {}).get("path", "")))
    produced = [step for step in publisher.steps
                if "run" in step and stem and stem in str(step["run"])]
    return Comparison(publisher=publisher, comparer=job, artefact=artefact, directory=directory,
                      command="\n".join(str(step["run"]) for step in compared),
                      produced=tuple(produced), compared=tuple(compared), uploaded=upload,
                      downloaded=download, stem=stem), ""


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


def write_legs(directory: pathlib.Path, claim: str) -> int:
    """The agreeing pair, on disk, exactly as the comparison job finds them after downloading.

    WHAT THIS IS FOR, AND WHAT IT IS NOT. `m11a:lockstep-agrees-across-architectures` and
    `m11a:pcg-regenerates-across-architectures` carry `where = "ci"`: this host has one architecture,
    so the comparison cannot be MADE here and the ledger reports them NOT EVALUATED rather than
    passed. `falsify.prove` then refused to judge them at all, and M11's repair gate was right that a
    criterion nobody has shown can fail is not a criterion that has been shown to fail.

    The environment is the part that was missing, not the criterion. This writes it: the same
    `digests.leg()` fixtures the audit's own AGREEING case uses, from the publisher's field list, in
    the nested layout `actions/download-artifact` produces. `falsify` then runs the criterion's own
    command over it — green — mutates one leg's digest — red — and restores. That proves the
    criterion can go red; it does NOT claim two architectures agreed, which only continuous
    integration, with two architectures in it, can say.
    """
    if directory.exists() and any(directory.iterdir()):
        print(f"{directory} already has something in it; refusing to write legs over it",
              file=sys.stderr)
        return 1
    directory.mkdir(parents=True, exist_ok=True)
    legs = AGREEING[2]
    _write(directory, legs)
    print(f"wrote {len(legs)} agreeing leg(s) into {directory} for the {claim} claim:")
    for leg in legs:
        print(f"  {leg['label']}  {leg['os']}/{leg['arch']}")
    return 0


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


# --- Did any leg PUBLISH anything? ----------------------------------------------------------------
#
# THE MUTATION THIS EXISTS TO CATCH deletes the one step of the matrix leg that computes a digest.
# Every leg then runs, every leg then uploads the path named in its `upload-artifact` step, the
# comparison job downloads what it can find, and the whole pipeline is green over an empty
# comparison. A reader of the workflow cannot tell the difference, because the difference is a step
# that is not there.
#
# HOW THIS IS ANSWERED WITHOUT BUILDING THE ENGINE, and why that is not a compromise. The leg's own
# command is RUN — with the path it uploads redirected into a directory this check owns and has
# deliberately NOT created, and with `CY_BUILD_DIR` pointed inside a regular file so that no build
# can possibly succeed. That makes one narrow behaviour observable in sixty milliseconds and on any
# host: DOES THE COMMAND ROUTE THE PATH IT UPLOADS, before anything else it might do?
#
#   the publication, unmutated   fails at the impossible build, HAVING CREATED the directory    ok
#   the step deleted             there is no command at all                                     RED
#   the command replaced by      exits 0 and writes nothing — a green tick over no digest       RED
#   `echo`
#   its flag renamed or dropped  fails, and never touched the path: the argument went nowhere   RED
#   a command that writes        exits 0 with a file the comparator cannot read                 RED
#   something else there
#
# WHAT THIS DOES NOT CLAIM, because the distinction is the whole reason the build is made impossible
# rather than attempted: it does not say the digest a real leg writes is a digest OF ANYTHING. That
# is `m11a:cross-leg-digest-published`, which builds the tree and runs `determinism.cross_leg`'s five
# cases over the numbers themselves. This says only that the path the comparison eventually reads is
# a path this leg's command actually writes to — which is exactly what the deletion removed.


def _resolve_matrix(text: str, leg: dict) -> str:
    """`${{ matrix.label }}` answered by one leg of the matrix; every other expression left alone."""
    return re.sub(r"\$\{\{\s*matrix\.([A-Za-z0-9_-]+)\s*\}\}",
                  lambda found: str(leg.get(found.group(1), found.group(0))), text)


def _last_line(text: str) -> str:
    lines = [line for line in text.splitlines() if line.strip()]
    return lines[-1] if lines else "(it printed nothing)"


def run_the_publication(comparison: Comparison) -> tuple[bool, str]:
    """Run the leg's own publishing command with its output redirected and the build made impossible."""
    leg = comparison.publisher.legs[0] if comparison.publisher.legs else {}
    command = _resolve_matrix("\n".join(str(step["run"]) for step in comparison.produced), leg)
    with tempfile.TemporaryDirectory(prefix="cy-cross-leg-publish-") as directory:
        root = pathlib.Path(directory)
        wall = root / "not-a-directory"
        wall.write_text("", encoding="utf-8")   # a FILE, so `mkdir -p` under it fails on every host
        into = root / "published"               # NOT created here: the command has to create it
        environment = {**os.environ, "CY_CROSS_LEG_AUDIT": "1", "CY_BUILD_DIR": str(wall / "build")}
        for step in comparison.produced:
            for key, value in (step.get("env") or {}).items():
                environment[str(key)] = _resolve_matrix(str(value), leg)
        try:
            finished = subprocess.run(  # noqa: S602 — the command is this repository's own workflow
                ["bash", "-c", command.replace(comparison.stem, f"{into}{os.sep}")], cwd=REPO_ROOT,
                capture_output=True, text=True, check=False, env=environment, timeout=300)
        except subprocess.TimeoutExpired:
            return False, ("did not finish in five minutes with the build made impossible, so this "
                           "check cannot say what it routed")
        routed = into.is_dir()
        written = {path.name: path.read_text(encoding="utf-8", errors="replace")
                   for path in sorted(into.rglob("*")) if path.is_file()} if routed else {}
    return _judge_the_publication(finished, routed, written)


def _judge_the_publication(finished, routed: bool, written: dict[str, str]) -> tuple[bool, str]:
    """What the leg's command did with the path it uploads. Four outcomes, and two of them are red."""
    if finished.returncode != 0:
        if routed:
            return True, ("routed its output into the directory it uploads and then failed at the "
                          "build this check made impossible — the publication doing its work")
        return False, ("failed WITHOUT EVER TOUCHING the path its job uploads, so the argument "
                       "naming that path went nowhere and the artefact is empty however green the "
                       f"leg is. Last line: {_last_line(finished.stdout + finished.stderr)}")
    if not written:
        return False, ("exited 0 AND WROTE NO DIGEST AT ALL. A step that reports success without "
                       "publishing is the publication deleted, wearing a green tick: every leg then "
                       "uploads a path nothing computed and the comparison agrees with the empty set")
    unreadable = [name for name in sorted(comparator.REQUIRED)
                  if not all(body.startswith(f"{name} ") or f"\n{name} " in body
                             for body in written.values())]
    if unreadable:
        return False, (f"exited 0 and wrote {', '.join(written)}, which the comparison cannot read: "
                       f"no {', '.join(unreadable)} in it")
    return True, f"wrote {', '.join(written)}, carrying every field the comparison reads"


def _check_it_runs(audit: Audit, comparison: Comparison, index: dict[str, Job]) -> None:
    """The comparison must be SCHEDULED. A skipped job is scored as success and compares nothing."""
    events = comparison.comparer.events
    if not events:
        audit.failed(f"{comparison.comparer.workflow} declares no `on:` trigger, so nothing in it "
                     f"says when {comparison.comparer.name} would ever run")
        return
    runs, stopped = [], []
    for event in events:
        try:
            why = (blocked_on(comparison.publisher, event, index)
                   or blocked_on(comparison.comparer, event, index)
                   or _steps_blocked(comparison, event))
        except Unreadable as unreadable:
            audit.failed(f"{comparison.comparer.label} is scheduled by a condition this audit "
                         f"cannot evaluate, and 'I could not tell whether the comparison runs' must "
                         f"not print the same as 'the comparison runs': {unreadable}")
            return
        (runs if not why else stopped).append(event.name if not why else f"on {event}: {why}")
    if runs:
        print(f"  ok    the publication and the comparison are both scheduled on: {', '.join(runs)}")
        return
    audit.failed(f"{comparison.comparer.label} NEVER RUNS on any of this workflow's own triggers "
                 f"({', '.join(event.name for event in events)}), so nothing is ever compared — and "
                 f"a job that is skipped is scored as SUCCESS, which leaves the pipeline green too. "
                 + "; ".join(stopped))


def _steps_blocked(comparison: Comparison, event: Event) -> str:
    """Whether every step the comparison is made of runs on this event, the jobs having run."""
    named = (("publishes the digest", comparison.produced),
             ("uploads it", (comparison.uploaded,)),
             ("downloads it", (comparison.downloaded,)),
             ("compares", comparison.compared))
    for what, steps in named:
        for step in steps:
            if step and not runs_on_event(str(step.get("if", "")), event):
                return (f"the step that {what} carries `if: "
                        f"{' '.join(str(step.get('if')).split())}`, which is false")
    return ""


def _check_the_publication(audit: Audit, comparison: Comparison) -> None:
    publisher = comparison.publisher
    if not comparison.stem:
        audit.failed(f"{publisher.label} uploads a path with no constant part, so this audit cannot "
                     f"tell which of its commands would have to write it")
        return
    if not comparison.produced:
        audit.failed(f"{publisher.label} uploads {comparison.stem}… and NO `run:` step of it writes "
                     f"{comparison.stem}: every leg runs, every leg uploads a path nothing computed, "
                     f"and the comparison downloads and compares the empty set")
        return
    verdict, detail = run_the_publication(comparison)
    print(f"  {'ok  ' if verdict else 'FAIL'}  the leg's own publishing command {detail}")
    if not verdict:
        audit.failed(f"{publisher.label}'s publishing command {detail}")


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
    index = index_of(jobs)
    for comparison in found:
        print(f"\n{comparison.publisher.label} publishes {comparison.artefact} from "
              f"{len(comparison.publisher.legs)} legs on {', '.join(comparison.publisher.runners)}")
        for step in comparison.produced:
            for line in str(step["run"]).splitlines():
                print(f"    {line}")
        print(f"{comparison.comparer.label} downloads them into {comparison.directory}/ and runs:")
        for line in comparison.command.splitlines():
            print(f"    {line}")
        _check_it_runs(audit, comparison, index)
        _check_the_legs(audit, comparison)
        _check_the_publication(audit, comparison)
        _check_the_comparison(audit, comparison)
    return audit


# --- The audit's own negative fixtures ------------------------------------------------------------
#
# A rule that stopped firing looks exactly like a workflow with nothing wrong in it, which is why
# `check_workflows.py` and `test_cross_leg_digests.py` both carry fixtures. Each fixture below is
# THE WORKING WORKFLOW WITH ONE THING CHANGED, built from one template, so that what a fixture is
# about is the line that differs and not a second copy of the file that drifted.
#
# AND EACH FIXTURE NAMES THE FINDING IT MUST PROVOKE. "Refused" is not enough: a rule that began
# refusing everything — an unreadable template, a missing `on:`, a reader that stopped parsing —
# would refuse all twelve and look exactly like twelve discriminating rules. The selftest requires
# the refusal to SAY the thing the fixture is about, so a blanket refusal is a failure here.
#
# THE FIRST FIXTURE IS THE DUMMY JOB THE GATE DESCRIBED: it says every word the three word-greps
# looked for and compares nothing. The next three are the repair-2 gate's own mutations.

_TRIGGERS = """on:
  push:
    branches: [main]
  pull_request:
"""

_TWO_LEGS = """          - { label: linux-x86_64, os: ubuntu-24.04 }
          - { label: linux-arm64, os: ubuntu-24.04-arm }
"""

_REAL_PUBLICATION = """      - name: Publish this leg's simulation and generation digests
        env:
          CY_CROSS_LEG_LABEL: ${{ matrix.label }}
        run: just test-determinism --publish-digest cross-leg-digests/${{ matrix.label }}.digest
"""

_UPLOAD = """      - uses: actions/upload-artifact@v4
        with:
          name: cross-leg-digest-${{ matrix.label }}
          path: cross-leg-digests/${{ matrix.label }}.digest
"""

_DOWNLOAD = """      - uses: actions/download-artifact@v4
        with:
          pattern: cross-leg-digest-*
          path: cross-leg-digests
"""

_REAL_COMPARISON = """      - name: One leg's digest against another's
        run: just test-determinism --compare-legs --pcg --digests cross-leg-digests
"""


def _workflow(*, triggers: str = _TRIGGERS, legs: str = _TWO_LEGS, publish_if: str = "",
              publication: str = _REAL_PUBLICATION, upload: str = _UPLOAD, compare: bool = True,
              needs: str = "    needs: publish\n", compare_if: str = "", download: str = _DOWNLOAD,
              comparison: str = _REAL_COMPARISON) -> str:
    """The working workflow, with one thing changed. Every fixture is a call to this."""
    text = triggers + "jobs:\n  publish:\n" + publish_if + (
        "    strategy:\n      fail-fast: false\n      matrix:\n        include:\n" + legs +
        "    runs-on: ${{ matrix.os }}\n    steps:\n      - uses: actions/checkout@v4\n" +
        publication + upload)
    if compare:
        text += ("  compare:\n" + needs + compare_if + "    runs-on: ubuntu-24.04\n    steps:\n"
                 "      - uses: actions/checkout@v4\n" + download + comparison)
    return text


#: name -> (the workflow, why it must be refused, a phrase the refusal must contain).
_FIXTURES = {
    "dummy": (
        _workflow(publication="", comparison='      - name: lockstep and pcg digests compared\n'
                                             '        run: echo "cross-leg-digests compared"\n'),
        "the dummy job: every word the three greps looked for, and no comparison",
        "when the simulation state hash differs"),

    # --- the three the repair-2 gate applied to the real ci.yml ----------------------------------
    "never-scheduled": (
        _workflow(compare_if="    if: false\n"),
        "`if: false` on the comparison job. It never runs, and GitHub scores a skipped job as "
        "SUCCESS, so the pipeline is green and nothing was compared",
        "NEVER RUNS"),

    "scheduled-by-an-event-that-never-fires": (
        _workflow(compare_if="    if: github.event_name == 'release'\n"),
        "a condition that is false on every trigger this workflow declares",
        "NEVER RUNS"),

    "publication-deleted": (
        _workflow(publication=""),
        "the matrix leg's publishing step deleted: four legs upload a path nothing computed",
        "NO `run:` step of it writes"),

    "publication-replaced-by-an-echo": (
        _workflow(publication='      - name: Publish this leg\'s digests\n'
                              '        run: echo "published cross-leg-digests/x.digest"\n'),
        "a publishing step that reports success and writes nothing",
        "WROTE NO DIGEST AT ALL"),

    "publication-routes-nothing": (
        _workflow(publication="      - name: Publish this leg's digests\n"
                              "        run: just test-determinism --publish-digest-typo "
                              "cross-leg-digests/${{ matrix.label }}.digest\n"),
        "the publishing flag misspelt: the leg runs, fails, and never routes the path it uploads",
        "WITHOUT EVER TOUCHING the path"),

    "scheduled-by-a-condition-nobody-can-read": (
        _workflow(compare_if="    if: ${{ vars.CY_COMPARE_LEGS == 'true' }}\n"),
        "a condition this reader cannot evaluate. 'I could not tell whether it runs' must not "
        "print the same as 'it runs'",
        "cannot evaluate"),

    # --- and the shapes the first repair already refused -----------------------------------------
    "publication-only": (
        _workflow(compare=False),
        "a publication is not a comparison",
        "publishes a digest per leg and runs a command"),

    "one-leg": (
        _workflow(legs="          - { label: linux-x86_64, os: ubuntu-24.04 }\n"),
        "one leg publishing is one leg agreeing with itself",
        "1 leg(s): one leg cannot disagree"),

    "unordered": (
        _workflow(needs=""),
        "a comparison that does not `needs:` its publication can run before it",
        "does not `needs:`"),

    "refuses-everything": (
        _workflow(comparison="      - run: test -d cross-leg-digests && exit 1\n"),
        "a job that fails whatever the legs published has measured nothing either",
        "a command that cannot pass when the legs agree"),

    "one-runner": (
        _workflow(legs="          - { label: leg-one, os: ubuntu-24.04 }\n"
                       "          - { label: leg-two, os: ubuntu-24.04 }\n"),
        "two legs of one runner image are not two architectures",
        "one runner image is one architecture"),
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
    """Every fixture above must be REFUSED, and refused for the reason it is about."""
    failures = 0
    complaints = _reader_agrees_with_check_workflows()
    for complaint in complaints:
        print(f"FAIL {complaint}")
    failures += len(complaints)
    if not complaints:
        print("ok   the workflow reader finds every job check_workflows.py finds\n")
    for name, (text, why, expected) in _FIXTURES.items():
        with tempfile.TemporaryDirectory(prefix="cy-audit-fixture-") as directory:
            path = pathlib.Path(directory)
            (path / "fixture.yml").write_text(text, encoding="utf-8")
            audit = audit_claim("job", path)
        if not audit.findings:
            failures += 1
            print(f"FAIL {name}: ACCEPTED, and it must not be — {why}\n")
            continue
        # REFUSED IS NOT ENOUGH. A rule that began refusing every workflow would refuse these too,
        # and twelve blanket refusals read exactly like twelve discriminating ones.
        if not any(expected in finding for finding in audit.findings):
            failures += 1
            print(f"FAIL {name}: refused, and NOT for the reason it is about — nothing said "
                  f"{expected!r}. It is about: {why}")
            for finding in audit.findings:
                print(f"     said instead: {finding}")
            print()
            continue
        print(f"ok   {name}: refused, saying {expected!r} — {why}\n")

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
    parser.add_argument("--write-legs", metavar="DIR",
                        help="write the AGREEING pair of legs into DIR, laid out as "
                             "actions/download-artifact unpacks them, and exit — this is the "
                             "environment the comparison job runs in, and it is what lets "
                             "`just roadmap-falsify` judge a where = \"ci\" criterion on a host "
                             "with one architecture")
    arguments = parser.parse_args(argv)
    if arguments.selftest:
        return selftest()
    if arguments.write_legs:
        return write_legs(pathlib.Path(arguments.write_legs), arguments.claim)

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

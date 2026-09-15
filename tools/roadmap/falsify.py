#!/usr/bin/env python3
"""Falsifiability: the ledger refuses a criterion nobody has shown can fail.

THE DEFECT THIS EXISTS TO END, AND IT HAS NOW BEEN FOUND SEVEN TIMES.

  1. M4  a determinism test whose scene never contended, so the race it named could not occur.
  2. M7  a sky test asserting against the producer's own statistics instead of the store.
  3. M8  a criterion running through a recipe that passes `--no-tests=ignore`, so an empty
         selection is a pass.
  4-5. M9 two criteria whose grep matched only the ledger file doing the grepping.
  6. M10 a dependency check parsing backticks out of a table written in bold, so it parsed nothing.
  7. M10 a four-profiles criterion that passed only because the runner supplied a build step it
         lacked.
  and then, at M11.a/M11.b's gate, five claims at once: three criteria the gate called
  "word-greps a dummy job satisfies", a record audit that "cannot detect the deletion of two of the
  four evaluators", and editor rows recorded off a tier the record does not carry.

Every one of them was green. Every one of them had been read by somebody. At seven this is not seven
bugs, it is a defect in HOW A CRITERION IS WRITTEN, and the fix cannot be another comment asking for
care — six comments-worth of intent have already failed. A criterion is a claim about the world, and
a claim that no state of the world contradicts is not a check. So:

    A CRITERION IS NOT ADMITTED TO A LEDGER UNTIL THE TOOLING HAS BROKEN WHAT IT NAMES
    AND WATCHED IT GO RED.

WHY THE MUTATION IS DERIVED AND NOT DESCRIBED. The options were a `falsifies` field naming in prose
what to break, a recorded negative-control run, and a mutation the tooling applies itself. The first
is prose, and prose is what the seven were written in — a person satisfies it by typing a sentence.
The second is a recorded number, and a recording nothing reproduces decays into a number nobody
re-earns. This module takes the third and pushes it one step further: for the shapes that carry the
defect — a text search, a committed artefact, a tier claim — IT DERIVES THE MUTATION FROM THE
CRITERION'S OWN TEXT, so the author writes nothing at all and there is nothing to write falsely. A
criterion whose mutation cannot be derived declares one, and what it declares is a verb and a target
this module executes, never a sentence (`criteria._check_falsifies`).

WHAT A PROOF IS. Three runs against a sandbox that is a copy of the tracked tree, never the tree
itself:

    positive control   unmutated, in the sandbox            must PASS
    mutation           the derived or declared mutation     must FAIL
    ledger-blind       tools/roadmap/milestones/ removed    must PASS

A DECLARED GAP IS JUDGED THE OTHER WAY ROUND (`_prove_a_declared_gap`). A criterion its ledger
declares as an expected failure is already red on the unmutated tree, in the open, on every run of
that ledger: "show that it can go red" asks for what is in front of the reader. What it has not
shown is that it is not PERMANENTLY red, so its declared mutation is the gap's own closing act made
small, and it must take the criterion GREEN.

AND A PROOF COMES IN THREE SHAPES, for the same reason one level down: a criterion that cannot pass
the positive control is not thereby unjudged.

    proven              it passes, and the mutation turns it RED
    red in the tree     it FAILS as written, in the sandbox AND in the repository — the tooling has
                        watched it go red, which is the whole of what a mutation stands in for
    red against a       the same, for a criterion a source-only sandbox cannot run at all: observed
    built tree          against a real build named on the command line (`prove --build-dir`)

WHY `red in the tree` IS A RATCHET AND NOT A HOLE, since it is the shape that could become one. The
defect this module exists to end runs in ONE direction: a criterion that is green and that nothing
can turn red. A criterion that is red is not that. And the day it goes green — which is what closing
a rung means — the recorded verdict stops matching the observed one, `reconcile` says so by name,
and the criterion must earn an ordinary mutation proof before the ladder will take it again. A rung
cannot close by turning its red criteria green quietly.

WHAT KEEPS THAT HONEST IS THE TREE CONTROL. Red in the SANDBOX is not enough: the sandbox is a copy
of the TRACKED tree, so a criterion can be red in it for a reason that has nothing to do with its
subject — a generated header nobody commits, a `.git` that is not there. It has to be red in the
repository as well, which is what makes the redness a property of the repository rather than of the
copy. That control runs the criterion UNMUTATED and nothing else: this module never mutates the
working tree, which is the entire reason the sandbox exists.

The third is there because two of the seven were a grep that found its own ledger. A criterion whose
verdict changes when the ledgers are deleted is reading the roadmap instead of the repository, and no
amount of reading the regex catches that reliably — deleting the ledgers catches it every time.

The positive control is not a formality either: it is what stops this module from reporting a proof
for a criterion that fails in the sandbox for an unrelated reason — a missing build tree, a network
fetch — where "it went red under mutation" would be true and meaningless. Such a criterion is
reported `not provable here`, with the reason, and is NOT counted as proven.

WHAT IS ENFORCED, AND WHERE. `selftest.test_falsifiability` — `just roadmap-test`, which is the
`plan-consistency` criterion of every ledger on the ladder — requires that every criterion is either
PROVEN by a fresh proof or named in `falsifiability.toml`, the inventory of what the ladder has not
yet shown can fail. The inventory only ever shrinks: a criterion that is neither proven nor listed
fails the roadmap's own gate, which is what stops the eighth. An entry whose criterion has been
edited fails too — the digest is over what the criterion CHECKS, so re-wording its prose is free and
changing its command costs a re-proof.

Governed by: delivery-roadmap (Milestone exit criteria are executable, Forbidden roadmap patterns),
testing-and-quality (Quality gates for merge).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shlex
import subprocess
import sys
import tarfile
import tempfile
import time
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import criteria as criteria_module  # noqa: E402
from record import REPO_ROOT  # noqa: E402

HERE = Path(__file__).resolve().parent
INVENTORY = HERE / "falsifiability.toml"
LEDGER_DIR_NAME = "tools/roadmap/milestones"

#: How long one criterion gets in the sandbox. A proof is a fast check by construction — anything
#: that needs a build tree is `not provable here` and says so — so this is a stall detector, not a
#: budget to be tuned.
PROOF_TIMEOUT_S = 240

PROVEN = "proven"
#: THE SECOND SHAPE A PROOF COMES IN, and it is the strongest evidence there is: the tooling ran the
#: criterion, unmutated, and WATCHED IT FAIL. A criterion that is red today has not been argued to be
#: falsifiable — it has been observed falsified, by the repository as it stands.
#:
#: WHY THIS IS A RATCHET AND NOT A HOLE. The defect this module exists to end is one-directional: a
#: criterion that is GREEN and that nothing can turn red. A red criterion is not that. And the moment
#: it goes green — which is what closing the rung means — the recorded verdict stops matching what is
#: observed, `reconcile` says so, and the criterion has to earn an ordinary mutation proof before the
#: ledger accepts it again. So the rung cannot close by turning these green quietly.
#:
#: WHAT KEEPS IT HONEST is the tree control below: red in the SANDBOX is not enough, because the
#: sandbox is source-only and a criterion can be red there for want of a generated or untracked file.
#: The criterion has to be red in the repository as well, which is what makes the redness a property
#: of the repository rather than of the copy.
RED_IN_THE_TREE = "red in the tree"
#: The same shape for a criterion the source-only sandbox cannot run at all: observed red against a
#: real build tree, named on the command line. Recorded apart because a source-only run cannot
#: re-earn it — see `reconcile`.
RED_WITH_A_BUILD = "red against a built tree"
REFUTED = "refuted"
UNPROVABLE = "not provable here"
NO_MUTATION = "no mutation"

#: Every verdict that counts as a proof. `reconcile` requires the recorded one to be the observed
#: one, so a criterion that changes proof shape is re-judged rather than carried.
PROOF_VERDICTS = (PROVEN, RED_IN_THE_TREE, RED_WITH_A_BUILD)


# --- Reading a criterion's shell ------------------------------------------------------------------


#: Separators that end one simple command and begin the next. Deliberately crude: this is a reader
#: over committed data, not a shell, and every rule below is written to be conservative when it
#: cannot tell.
_SEPARATORS = frozenset({"&&", "||", "|", ";", "(", ")", "&", ">", ">>", "<", "<<", "|&",
                         "do", "done", "then", "else", "elif", "fi", "{", "}", "if", "while",
                         "for", "case", "esac", "!"})

GREP_FAMILY = frozenset({"grep", "egrep", "fgrep", "rg", "ag", "ack"})

#: grep options that consume the next token, so that it is not mistaken for a path to search.
_GREP_TAKES_A_VALUE = frozenset({"-e", "--regexp", "-f", "--file", "-m", "--max-count", "-A", "-B",
                                 "-C", "--after-context", "--before-context", "--context", "-d",
                                 "--directories", "--binary-files", "--label", "--devices",
                                 "--include", "--exclude", "--exclude-dir", "--glob", "-g",
                                 "--type", "-t", "--color", "--colour"})

#: Path operands that mean "the whole repository". A search over one of these reaches this file, the
#: ledger that declares the criterion, and the tasks document that asks for it.
ROOT_OPERANDS = frozenset({".", "./", "..", "../", "/", "$PWD", "${PWD}", "*"})


#: `python3 - <<\'EOF\'`. A heredoc body is data for another interpreter, not shell, and reading it
#: as shell is how the first draft of this module reported nine criteria as having no assertion when
#: what they had was an embedded Python program.
_HEREDOC = re.compile(r"<<-?\s*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\1")

#: `CY_BUILD_DIR=build/x just build-engine` — the command is `just`, not the assignment. Missing this
#: was the other half of the same false report.
_ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")


#: A backslash at the end of a line continues it. Not joining them put a bare newline into a search's
#: path operands and the derived mutation then named a file called "\n".
_CONTINUATION = re.compile(r"\\\n\s*")


def without_heredocs(run: str) -> str:
    """The criterion body with every heredoc body removed, leaving the command that opened it."""
    lines = _CONTINUATION.sub(" ", run).splitlines()
    kept: list[str] = []
    index = 0
    while index < len(lines):
        line = lines[index]
        kept.append(line)
        match = _HEREDOC.search(line)
        index += 1
        if not match:
            continue
        terminator = match.group(2)
        while index < len(lines) and lines[index].strip() != terminator:
            index += 1
        index += 1  # the terminator itself
    return "\n".join(kept)


def simple_commands(run: str) -> list[list[str]]:
    """Every simple command in a criterion body, as argv.

    A NEWLINE ENDS A COMMAND, and forgetting that is why the first draft of this module reported
    `set -eo pipefail` as the whole of nine criteria: `shlex` in whitespace-splitting mode treats a
    newline as ordinary space, so every line of a body ran together into one argv whose command name
    was `set`. Each line is lexed on its own, and a line that will not lex — an opening quote that
    closes further down — is joined to the next until it does.
    """
    commands: list[list[str]] = []
    pending = ""
    for line in without_heredocs(run).splitlines():
        pending = f"{pending}\n{line}" if pending else line
        tokens = _tokenise(pending)
        if tokens is None:
            continue
        pending = ""
        commands.extend(_split_on_separators(tokens))
    return [argv for argv in commands if argv]


def _tokenise(text: str) -> list[str] | None:
    """The shell words of one logical line, or None while a quote is still open."""
    lexer = shlex.shlex(text, posix=True, punctuation_chars=True)
    lexer.whitespace_split = True
    try:
        return list(lexer)
    except ValueError:
        return None


def _split_on_separators(tokens: list[str]) -> list[list[str]]:
    commands: list[list[str]] = []
    current: list[str] = []
    for token in tokens:
        if token in _SEPARATORS:
            commands.append(_strip_assignments(current))
            current = []
        else:
            current.append(token)
    commands.append(_strip_assignments(current))
    return commands


def _strip_assignments(argv: list[str]) -> list[str]:
    index = 0
    while index < len(argv) and _ASSIGNMENT.match(argv[index]):
        index += 1
    return argv[index:]


@dataclass(frozen=True)
class Search:
    """One text search a criterion performs: what it looks for, and where."""

    tool: str
    pattern: str
    paths: tuple[str, ...]
    recursive: bool
    #: `--include='*.cpp'`. A search restricted to source extensions cannot read the ledger that
    #: declares it however far up the tree it starts, and `m9:replay-one-record` — which greps
    #: `tools/` for a token its own ledger contains — is correct for exactly that reason.
    includes: tuple[str, ...] = ()
    #: `grep -v`: the pattern is what must NOT be found. Inverting changes what a mutation means, so
    #: no mutation is derived from one.
    inverted: bool = False

    @property
    def reaches_the_root(self) -> bool:
        return not self.paths or any(path in ROOT_OPERANDS for path in self.paths)

    @property
    def reaches_the_ledgers(self) -> bool:
        """Whether this search can read tools/roadmap/, which is where the criterion itself lives."""
        if self.includes and not any(
                _matches_a_ledger_file(pattern) for pattern in self.includes):
            return False
        if self.reaches_the_root:
            return True
        return any(_within("tools/roadmap", path) for path in self.paths)


#: What is in tools/roadmap/: the ledgers, this tooling, and its README. A search that cannot open
#: one of these cannot match itself.
_LEDGER_SUFFIXES = (".toml", ".py", ".md")


def _matches_a_ledger_file(include: str) -> bool:
    from fnmatch import fnmatch
    return any(fnmatch("x" + suffix, include) for suffix in _LEDGER_SUFFIXES)


def _within(directory: str, operand: str) -> bool:
    """Whether a search of `operand` can reach `directory` — either above it or inside it."""
    operand = operand.rstrip("/")
    return directory.startswith(operand + "/") or operand.startswith(directory) or operand == directory


def searches(run: str) -> list[Search]:
    """Every grep-family invocation in a criterion body."""
    found = []
    for argv in simple_commands(run):
        if not argv or Path(argv[0]).name not in GREP_FAMILY:
            continue
        found.append(_search(argv))
    return [search for search in found if search is not None]


@dataclass
class _Options:
    """What a grep-family command line says about itself, accumulated one token at a time."""

    recursive: bool = False
    inverted: bool = False
    pattern: str = ""
    includes: list[str] = field(default_factory=list)


def _search(argv: list[str]) -> Search | None:
    tool = Path(argv[0]).name
    options = _Options(recursive=tool in ("rg", "ag", "ack"))
    operands: list[str] = []
    index = 1
    while index < len(argv):
        consumed = _read_option(argv, index, options)
        if consumed:
            index += consumed
            continue
        operands.append(argv[index])
        index += 1
    pattern = options.pattern or (operands.pop(0) if operands else "")
    if not pattern:
        return None
    return Search(tool=tool, pattern=pattern, paths=tuple(operands), recursive=options.recursive,
                  includes=tuple(options.includes), inverted=options.inverted)


def _read_option(argv: list[str], index: int, options: _Options) -> int:
    """How many tokens the option at `index` consumed, or 0 when it is not an option at all."""
    token = argv[index]
    if token in _GREP_TAKES_A_VALUE:
        value = argv[index + 1] if index + 1 < len(argv) else ""
        if token in ("-e", "--regexp"):
            options.pattern = options.pattern or value
        if token in ("--include", "--glob", "-g"):
            options.includes.append(value.lstrip("!"))
        return 2
    if token.startswith(("--include=", "--glob=")):
        options.includes.append(token.split("=", 1)[1].lstrip("!"))
        return 1
    if token.startswith("-") and len(token) > 1:
        _read_flags(token, options)
        return 1
    return 0


def _read_flags(token: str, options: _Options) -> None:
    if token.startswith("--"):
        options.recursive = options.recursive or token.startswith("--recursive")
        options.inverted = options.inverted or token == "--invert-match"
        return
    letters = token[1:]
    options.recursive = options.recursive or bool(re.search(r"[rR]", letters))
    options.inverted = options.inverted or "v" in letters


# --- The static rules: shapes that are known not to be able to go red -----------------------------


@dataclass(frozen=True)
class Finding:
    rule: str
    detail: str


#: Every rule this module can report, and what a reader should do about it. The keys are the audit's
#: vocabulary, so they are stable identifiers rather than sentences.
RULES = {
    "searches-the-repository-root": (
        "a recursive search over the whole tree: the mere presence of the token ANYWHERE — in a "
        "ledger, a task list, a specification, a comment — satisfies it"),
    "self-match": (
        "the search reaches tools/roadmap/, so the ledger declaring the criterion is one of the "
        "files searched and the criterion matches itself"),
    "vacuous-suite": (
        "a test selection that is a pass when it selects nothing: `just test-render` and "
        "`just test-unclassified` run ctest with --no-tests=ignore, and a ctest -R that matches no "
        "test exits zero unless the count is asserted"),
    "no-assertion": (
        "nothing in the body can produce a non-zero exit: no test, no comparison, no suite, no "
        "recipe — it reports and returns"),
    "swallowed-verdict": (
        "the verdict-bearing command's exit code is discarded by `|| true` and cannot reach the "
        "criterion"),
    "presence-only": (
        "the whole verdict is that some text exists somewhere: nothing the engine builds is run, no "
        "suite is executed, no two observations are compared"),
    "artefact-presence": (
        "a `path` criterion: it is satisfied by a file of the right name, whatever is in it"),
}

#: Rules 1-5 say the criterion CANNOT GO RED — no state of the repository makes it fail. `presence-
#: only` is weaker and is reported apart: such a criterion can be turned red (delete the token) but
#: it is satisfied by typing the token, so it measures spelling rather than capability.
CANNOT_GO_RED = ("searches-the-repository-root", "self-match", "vacuous-suite", "no-assertion",
                 "swallowed-verdict")

#: Commands that cannot carry a verdict: they report, they move bytes, and in a body under `set -e`
#: they are what "a dummy job satisfies it" looks like. The rule is a SAFE LIST rather than a list of
#: verdict-bearing commands, because the second is open-ended — `cargo`, `ctest`, `cmake`, a sample
#: binary, an embedded Python program — and every name missing from it is a false accusation.
_INERT = frozenset({"echo", "printf", "ls", "cat", "true", ":", "set", "cd", "export", "trap",
                    "mkdir", "touch", "pwd", "date", "sleep", "shift", "local", "declare", "unset",
                    "sort", "tr", "head", "tail", "cut", "uniq", "tee", "read", "eval", "source",
                    "."})

#: Recipes whose ctest invocation passes `--no-tests=ignore` (just/test.just `_ctest`), so a
#: selection that matches nothing is a pass rather than an error.
_IGNORES_AN_EMPTY_SELECTION = ("test-render", "test-unclassified")

_SELECTS_TESTS = ("-R", "--tests-regex", "-L", "--label-regex")


def inspect(criterion: criteria_module.Criterion) -> tuple[Finding, ...]:
    """Every shape in this criterion that is known to be unable to fail."""
    if criterion.kind in ("path", "tiers"):
        return ()
    body = criterion.run
    findings = [*_inspect_searches(body), *_inspect_suites(body), *_inspect_verdict(body)]
    return tuple(findings)


#: How a criterion says "and not my own ledger". Recognising only these spellings matters: the first
#: draft accepted the mere STRING `tools/roadmap` anywhere in the body, which a criterion that
#: SEARCHES tools/roadmap satisfies by naming the path it is searching — the check excusing exactly
#: the criterion it exists to catch.
_EXCLUDES_THE_LEDGERS = re.compile(
    r"""grep\s+-[A-Za-z]*v[A-Za-z]*\s+["']?\^?(\./)?tools/roadmap"""
    r"""|--exclude-dir[= ]["']?(tools/)?roadmap"""
    r"""|:!\s*tools/roadmap"""
    r"""|--glob[= ]["']?!\*?/?tools/roadmap""")


def _inspect_searches(body: str) -> list[Finding]:
    findings = []
    excluded = bool(_EXCLUDES_THE_LEDGERS.search(body))
    for search in searches(body):
        if not search.recursive:
            continue
        if search.reaches_the_root:
            findings.append(Finding("searches-the-repository-root",
                                    f"{search.tool} {search.pattern!r} over "
                                    f"{' '.join(search.paths) or 'the working directory'}"))
        if search.reaches_the_ledgers and not excluded:
            findings.append(Finding("self-match",
                                    f"{search.tool} {search.pattern!r} reaches tools/roadmap/ and "
                                    "nothing filters it out"))
    return findings


def _inspect_suites(body: str) -> list[Finding]:
    """A test selection that is green when it selects nothing."""
    # `ctest -N` LISTS the selection without running it, and a criterion that pipes that list into an
    # assertion has answered the question this rule asks — "and what if it selected nothing?". Both
    # spellings the ledgers use count: `| grep -q <suite>`, and `| grep -c | test -ge`.
    asserts_a_count = bool(re.search(r"\B-N\b", body)) and bool(
        re.search(r"grep -q", body)
        or (re.search(r"\b(grep -c|wc -l)", body) and re.search(r"\btest\s", body)))
    if asserts_a_count:
        return []
    return [finding for argv in simple_commands(body)
            if (finding := _vacuous_selection(argv)) is not None]


def _vacuous_selection(argv: list[str]) -> Finding | None:
    name = Path(argv[0]).name
    if not any(token in _SELECTS_TESTS for token in argv):
        return None
    if name == "ctest" and "--no-tests=error" not in argv:
        return Finding("vacuous-suite", f"ctest {' '.join(argv[1:])[:60]}")
    if name == "just" and len(argv) > 1 and argv[1] in _IGNORES_AN_EMPTY_SELECTION:
        return Finding("vacuous-suite", f"just {argv[1]} selects with --no-tests=ignore behind it")
    return None


def _inspect_verdict(body: str) -> list[Finding]:
    """Whether anything in the body can return non-zero at all, and whether it is thrown away."""
    findings = []
    argvs = [argv for argv in simple_commands(body) if argv]
    names = [Path(argv[0]).name for argv in argvs]
    verdicts = [name for name in names if name not in _INERT]
    if not verdicts and not re.search(r"\bexit\s+[1-9]", body):
        findings.append(Finding("no-assertion", f"commands: {' '.join(sorted(set(names))) or 'none'}"))
    if re.search(r"\|\|\s*true\s*$", body.strip()) or re.search(r"\|\|\s*true\s*$",
                                                                body.strip(), re.MULTILINE):
        last = body.strip().splitlines()[-1].strip()
        if re.search(r"\|\|\s*true$", last):
            findings.append(Finding("swallowed-verdict", last[:80]))
    return findings


#: Anything that makes the repository DO something, as opposed to reading what is written in it. A
#: criterion that runs one of these and then greps the result is measuring behaviour; one that only
#: greps the tree is measuring spelling.
_EXERCISES_THE_TREE = re.compile(
    r"\bctest\b|\bcmake\b|\bcargo\b|just (test|build|run|content|release|generate|ci)"
    r"|\$\{?CY_BUILD_DIR|\bbuild/|(^|\s)\./|cy_sample|python3\s|\bdiff\b|\bcmp\b",
    re.MULTILINE)


def weakness(criterion: criteria_module.Criterion) -> tuple[Finding, ...]:
    """Criteria that CAN go red but only for a word: reported apart, because they are not the same
    defect.

    Deleting the file turns an artefact criterion red and renaming the symbol turns a word-grep red,
    so neither is one of the seven. What they share is that typing the word satisfies them, which is
    what M11's gate meant by "word-greps a dummy job satisfies" — a weaker complaint than "it cannot
    fail", and a real one.
    """
    if criterion.kind == "path":
        return (Finding("artefact-presence", f"a file matching {criterion.path} exists"),)
    if criterion.kind not in ("command", "recipe"):
        return ()
    body = without_comments(criterion.run)
    if not searches(body) or _EXERCISES_THE_TREE.search(body):
        return ()
    patterns = ", ".join(sorted({search.pattern for search in searches(body)}))[:90]
    return (Finding("presence-only", f"the verdict is that this text exists: {patterns}"),)


# --- The mutation the tooling applies itself ------------------------------------------------------


@dataclass(frozen=True)
class Mutation:
    """A change to the sandbox that MUST turn the criterion red."""

    verb: str
    target: str = ""
    token: str = ""
    derived: bool = True
    tiers: tuple[str, ...] = ()

    def describe(self) -> str:
        where = f" in {self.target}" if self.target else ""
        subject = f" {self.token!r}" if self.token else ""
        origin = "derived" if self.derived else "declared"
        return f"{self.verb}{subject}{where} ({origin})"


#: Regex metacharacters that make a searched pattern something other than a literal. A pattern is
#: mutated by its LITERAL alternatives, so `FieldImageHeader\|field_image` renames both.
_ALTERNATION = re.compile(r"\\\||\|")
_NOT_LITERAL = re.compile(r"[\\^$.\[\]()*+?{}]")


def literals(pattern: str) -> tuple[str, ...]:
    """The literal alternatives of a searched pattern, or nothing unless EVERY alternative is one.

    All-or-nothing, because renaming some alternatives of `class .*DisplayServer\\|DisplayServer
    final` leaves the regex branch matching and the criterion green — which this module would then
    report as "it cannot go red" about a criterion that can. A partial mutation is not a weaker
    proof, it is a false accusation.
    """
    pieces = [piece.strip() for piece in _ALTERNATION.split(pattern)]
    if not pieces or any(len(piece) < 3 or _NOT_LITERAL.search(piece) for piece in pieces):
        return ()
    return tuple(pieces)


def derive(criterion: criteria_module.Criterion) -> Mutation | None:
    """The mutation this criterion's own text implies, or None when it implies none.

    THE AUTHOR WRITES NOTHING. That is the whole design: a field an author fills in is a field an
    author can fill in falsely, and the three shapes that carry the defect all say out loud what
    would break them — a `path` criterion names the artefact, a `tiers` criterion names the rows, a
    text search names the token and the files. Deriving it leaves nothing to be written falsely.
    """
    if criterion.falsifies:
        declared = criterion.falsifies
        return Mutation(verb=str(declared["mutate"]), target=str(declared.get("target", "")),
                        token=str(declared.get("token", "")), derived=False)
    if criterion.kind == "path":
        return Mutation(verb="delete-path", target=criterion.path)
    if criterion.kind == "tiers":
        return Mutation(verb="lower-tiers", tiers=tuple(sorted(criterion.expect_tiers)))
    return _derive_from_searches(criterion.run)


#: `module=src/rendering/shaders/cy/field.slang` — a literal assignment, which several criteria make
#: before searching `"$module"`. Resolving those recovers a derivable mutation for criteria that
#: would otherwise have to declare one; anything with a `$`, a backtick or a `$(` on the right-hand
#: side is left alone, because guessing at a command substitution is how a mutation ends up naming a
#: file that is not there and the criterion is then accused of not being able to fail.
_LITERAL_ASSIGNMENT = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)=([^\s$`\"\';|&]+)\s*$", re.MULTILINE)


def literal_assignments(body: str) -> dict[str, str]:
    return {name: value for name, value in _LITERAL_ASSIGNMENT.findall(without_heredocs(body))}


def _resolve(text: str, assignments: dict[str, str]) -> str:
    for name, value in assignments.items():
        text = text.replace("${" + name + "}", value).replace("$" + name, value)
    return text


def _derive_from_searches(body: str) -> Mutation | None:
    """Rename every literal a text search looks for, inside the paths that same search reads.

    THREE THINGS ARE REFUSED HERE RATHER THAN GUESSED AT, and each of them produced a false
    accusation in this module's first run over the ladder:

      an inverted search (`grep -v`)   its pattern is what must NOT appear, so renaming it proves
                                       the opposite of what the criterion claims — this is how
                                       `m11b:play-modes-exist` came to be mutated on `/target/`;
      a pattern that is not a literal  `$mode`, `^tools/roadmap/`, a character class: there is no
                                       string to rename;
      a path this module cannot resolve  `"$module"` after a command substitution, or none at all.

    A criterion whose mutation cannot be derived is NOT thereby unfalsifiable. It is undetermined,
    and it says so: it declares a `[criterion.falsifies]`, or it sits in the inventory until it does.
    """
    assignments = literal_assignments(body)
    tokens: list[str] = []
    paths: list[str] = []
    for search in searches(body):
        if search.inverted:
            continue
        found = literals(_resolve(search.pattern, assignments))
        resolved = [_resolve(path, assignments) for path in search.paths]
        concrete = [path for path in resolved if "$" not in path and path not in ROOT_OPERANDS]
        if not found or len(concrete) != len(resolved) or not concrete:
            continue
        tokens.extend(found)
        paths.extend(concrete)
    if not tokens or not paths:
        return None
    return Mutation(verb="rename-token", target=",".join(sorted(set(paths))),
                    token="\x1f".join(sorted(set(tokens))))


# --- The sandbox ----------------------------------------------------------------------------------


class Sandbox:
    """A copy of the tracked tree, in the scratch directory, that a mutation may be applied to.

    Never the repository itself. A tool that proves a criterion can fail by breaking the working
    tree and putting it back is one crash away from leaving it broken, and this one runs hundreds of
    times.
    """

    def __init__(self, root: Path) -> None:
        self.root = root
        self._saved: dict[Path, bytes | None] = {}

    @classmethod
    def materialise(cls, into: Path) -> Sandbox:
        listed = subprocess.run(["git", "ls-files", "-z"], cwd=REPO_ROOT, check=True,
                                capture_output=True)
        names = [name for name in listed.stdout.decode().split("\0") if name]
        into.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as handle:
            handle.write("\n".join(names))
            manifest = handle.name
        try:
            stream = subprocess.Popen(["tar", "-cf", "-", "-T", manifest], cwd=REPO_ROOT,
                                      stdout=subprocess.PIPE)
            with tarfile.open(fileobj=stream.stdout, mode="r|") as archive:
                archive.extractall(into, filter="data")  # noqa: S202 — this repository's own files
            stream.wait()
        finally:
            os.unlink(manifest)
        return cls(into)

    # --- applying and undoing --------------------------------------------------------------------

    def _remember(self, path: Path) -> None:
        if path in self._saved:
            return
        self._saved[path] = path.read_bytes() if path.is_file() else None

    def forget(self) -> None:
        """Drop what is remembered without restoring it — for a file the caller wrote itself."""
        self._saved.clear()

    def restore(self) -> None:
        for path, content in self._saved.items():
            if content is None:
                path.unlink(missing_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
        self._saved.clear()

    def apply(self, mutation: Mutation) -> int:
        """Apply the mutation; return how many files it changed. Zero is itself a finding."""
        if mutation.verb == "delete-path":
            return self._delete(mutation.target)
        if mutation.verb == "truncate":
            return self._edit(mutation.target, lambda _text: "")
        if mutation.verb == "lower-tiers":
            return self._lower_tiers(mutation.tiers)
        if mutation.verb == "delete-lines":
            return self._delete_lines(mutation.target, mutation.token)
        if mutation.verb == "rename-token":
            return self._rename(mutation.target, mutation.token)
        raise ValueError(f"no such mutation: {mutation.verb}")

    def _matching(self, target: str) -> list[Path]:
        found: list[Path] = []
        for piece in target.split(","):
            piece = piece.strip()
            if not piece:
                continue
            candidate = self.root / piece
            if candidate.is_file():
                found.append(candidate)
            elif candidate.is_dir():
                found.extend(path for path in candidate.rglob("*") if path.is_file())
            else:
                found.extend(path for path in self.root.glob(piece) if path.is_file())
        return found

    def _delete(self, target: str) -> int:
        changed = 0
        for path in self._matching(target):
            self._remember(path)
            path.unlink()
            changed += 1
        return changed

    def _edit(self, target: str, rewrite) -> int:
        changed = 0
        for path in self._matching(target):
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            replacement = rewrite(text)
            if replacement == text:
                continue
            self._remember(path)
            path.write_text(replacement, encoding="utf-8")
            changed += 1
        return changed

    def _delete_lines(self, target: str, token: str) -> int:
        def rewrite(text: str) -> str:
            return "\n".join(line for line in text.splitlines() if token not in line) + "\n"
        return self._edit(target, rewrite)

    def _rename(self, target: str, tokens: str) -> int:
        wanted = [token for token in tokens.split("\x1f") if token]

        def rewrite(text: str) -> str:
            for token in wanted:
                text = text.replace(token, "cyFalsified" + hashlib.sha1(
                    token.encode()).hexdigest()[:8])  # noqa: S324 — a nonce, not a signature
            return text
        return self._edit(target, rewrite)

    def _lower_tiers(self, capabilities: tuple[str, ...]) -> int:
        """Demote the rows a `tiers` criterion names to `none`, record's own rules included.

        `none` cannot name a milestone or a change (`record._entry`), so the demotion clears those
        two fields as well. A mutation that leaves the record unreadable would make the criterion go
        red for the wrong reason, which is the same false green upside down.
        """
        record = self.root / "docs" / "roadmap" / "status.yaml"
        if not record.is_file():
            return 0
        wanted = set(capabilities)
        self._remember(record)
        before = record.read_text(encoding="utf-8")
        lines = before.splitlines()
        current = ""
        changed = 0
        for index, line in enumerate(lines):
            name = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", line)
            if name:
                current = name.group(1)
                continue
            if current not in wanted:
                continue
            if re.match(r"^\s+tier:\s", line):
                lines[index] = re.sub(r"(tier:\s*)\S+", r"\1none", line)
                changed += 1
            elif re.match(r"^\s+(milestone|change):\s", line):
                lines[index] = re.sub(r"((milestone|change):\s*).*", r"\1null", line)
        after = "\n".join(lines) + "\n"
        if after == before:
            return 0
        record.write_text(after, encoding="utf-8")
        return changed

    def hide_the_ledgers(self) -> int:
        """Remove tools/roadmap/milestones/ — the ledger-blind control."""
        return self._delete(LEDGER_DIR_NAME)

    # --- running ---------------------------------------------------------------------------------

    def run(self, criterion: criteria_module.Criterion) -> tuple[int, str]:
        if criterion.kind == "path":
            matched = sorted(self.root.glob(criterion.path))
            return (0 if matched else 1), "\n".join(str(path) for path in matched)
        if criterion.kind == "tiers":
            return self._run_tiers(criterion)
        # Every CY_* override is dropped: a sandbox run that inherited CY_BUILD_DIR would read —
        # or write — the real build tree, and a proof that touches the repository it is protecting
        # is not a proof.
        environment = {key: value for key, value in os.environ.items() if not key.startswith("CY_")}
        environment["CY_FALSIFY"] = "1"
        try:
            completed = subprocess.run(  # noqa: S603 — the command is committed data
                ["bash", "-c", criterion.run], cwd=self.root, capture_output=True, text=True,
                timeout=PROOF_TIMEOUT_S, check=False, env=environment)
        except subprocess.TimeoutExpired:
            return 124, f"no result within {PROOF_TIMEOUT_S} s"
        return completed.returncode, (completed.stdout or "") + (completed.stderr or "")

    def _run_tiers(self, criterion: criteria_module.Criterion) -> tuple[int, str]:
        import record as record_module
        try:
            entries = record_module.load(self.root / "docs" / "roadmap" / "status.yaml")
        except record_module.RecordError as error:
            return 1, str(error)
        status, _detail, output = criteria_module._check_tiers(criterion, entries)
        return (0 if status == criteria_module.OK else 1), output


# --- A proof ---------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Proof:
    ledger: str
    criterion: str
    digest: str
    verdict: str
    mutation: str
    detail: str
    seconds: float = 0.0
    #: THIS RUN COULD NOT JUDGE THE CRITERION AT ALL — as opposed to judging it and finding nothing.
    #: A source-only run cannot re-earn a proof taken against a build tree, and a prover running
    #: inside another prover cannot run its own tree control; in both cases the honest report is "not
    #: here", and a standing proof is neither confirmed nor destroyed by it. `reconcile` and `_record`
    #: read this rather than pattern-matching the detail string, because a sentence is not a flag.
    #: It is never written to the inventory: it is a property of the RUN, not of the criterion.
    unjudged: bool = False


def digest(criterion: criteria_module.Criterion) -> str:
    """A fingerprint of what the criterion CHECKS — never of what it says about itself.

    Re-wording a `describe` is free; changing the command, the artefact, the expected tiers or the
    declared mutation costs a re-proof, because that is the part a proof was about.
    """
    material = "\n".join([
        criterion.kind, criterion.run, criterion.path,
        repr(sorted(criterion.expect_tiers.items())),
        repr(sorted((criterion.falsifies or {}).items())),
    ])
    return hashlib.sha256(material.encode()).hexdigest()[:16]


#: What a criterion reads or writes that a sandbox of the TRACKED tree does not have. The sandbox is
#: source, not a build: a criterion that needs a compiled engine is reported `not provable here` and
#: named, rather than being handed four hours and a timeout. Continuous integration, which has the
#: build, is where those are proven.
#: `just ci-check` is EXCLUDED from the `ci` alternative on purpose. It is `tools/ci/check_workflows
#: .py`, which reads .github/workflows/, gates.toml and the justfile and compiles nothing — and the
#: regex matched it only because "just ci" is a prefix of it. A criterion reported as needing a build
#: it does not need is a criterion nothing ever proves, which is the same silence one door along.
_NEEDS_A_BUILD = re.compile(
    r"\bCY_BUILD_DIR\b|\bbuild/|\bctest\b|\bcmake\b|\bcargo\b|\bninja\b|"
    r"just ci(?!-check)|"
    r"just (build|test|run|content|release|generate|quality-lint|quality-tidy)")


#: A whole-line shell comment. Stripped before asking whether a body needs a build, because a
#: criterion that EXPLAINS in a comment why it no longer runs `ctest` was being reported as needing
#: a build over the sentence saying it does not — `m11b:specialised-editors` is that criterion, and
#: it is source-only in every command it actually runs.
_COMMENT_LINE = re.compile(r"^[ \t]*#.*$", re.MULTILINE)


def without_comments(run: str) -> str:
    return _COMMENT_LINE.sub("", run)


def unsandboxable(criterion: criteria_module.Criterion) -> str:
    """Why a sandbox of the tracked tree cannot judge this criterion, or an empty string."""
    if criterion.kind in ("path", "tiers"):
        return ""
    found = _NEEDS_A_BUILD.search(without_comments(criterion.run))
    if found:
        return f"it needs a built tree ({found.group(0)!r}); the sandbox is source only"
    # AND THE SECOND WAY A CRITERION SAYS SO, WHICH IS ALSO ITS OWN DATA. A ledger's default budget
    # is half an hour; a criterion that asked for MORE than that has said, in the only field there is
    # for saying it, that what it runs is not reading files. `m11a:thirdparty-dependencies-at-working`
    # is the case that forced this: it invokes a script whose name carries no build word, and the leg
    # inside configures CMake twice and fetches every gated dependency — about 1.5 GB on a cold
    # cache, which is why its ledger entry says 7200. Against the 240 s stall detector that is a
    # timeout, and a timeout is not a verdict: it was reported `not provable here` with a reason that
    # named the prover's clock rather than the criterion. Routed to the build-backed proof instead,
    # where a real tree answers the question.
    if criterion.timeout_s > criteria_module.DEFAULT_TIMEOUT_S:
        return (f"its ledger asks for {criterion.timeout_s} s, past the {criteria_module.DEFAULT_TIMEOUT_S} s "
                "default: it fetches or builds, and the sandbox is source only")
    return ""


#: A criterion whose body runs this module, or the recipe that runs it. The tree control below
#: executes a criterion in the repository, and executing one of THESE in the repository starts a
#: second prover inside the first: `just roadmap-test` runs `falsify check`, which proves the whole
#: ladder, which reaches this criterion again. The cost is not the objection — the objection is that
#: a nested run sees `CY_FALSIFY` set, refuses its own tree controls, and so returns DIFFERENT
#: verdicts from the outer one, which would make the outer run record "red in the tree" about a
#: criterion that is red only because it was run inside itself. Refused by name instead.
_RUNS_THE_ROADMAP_TOOLING = re.compile(r"just roadmap-(test|falsify)\b|falsify\.py|selftest\.py")

#: How long the tree control gets. It runs the body the sandbox has just run, so it is the same work;
#: the cap is there so that a criterion which is cheap over source and expensive against a real tree
#: is reported unjudged rather than hanging the prover.
TREE_CONTROL_TIMEOUT_S = 300

#: How long a criterion gets against a real build tree (`prove --build-dir`). These are the suites,
#: the samples and the four-profile builds — minutes rather than seconds.
BUILD_PROOF_TIMEOUT_S = 3600

#: `--budget`, in seconds, when a run wants a tighter cap than a criterion's own. A criterion that
#: runs out of it is reported unjudged rather than red: a clock is not a verdict.
_BUILD_BUDGET: list[int] = []


def build_budget() -> int:
    return _BUILD_BUDGET[0] if _BUILD_BUDGET else BUILD_PROOF_TIMEOUT_S


def run_in_the_repository(criterion: criteria_module.Criterion, build_dir: str,
                         timeout_s: int) -> tuple[int, str]:
    """Run the criterion, UNMUTATED, in the repository itself.

    THE WORKING TREE IS NEVER MUTATED HERE, and that is the whole licence for this function. The
    sandbox exists so that a mutation cannot escape into the repository and nothing about that
    changes: this runs the criterion exactly as `just roadmap-milestone` runs it, reads the exit
    code, and stops. What it buys is the one question a source-only copy cannot answer — whether a
    criterion that is red in the copy is red in the original.
    """
    if os.environ.get("CY_FALSIFY"):
        return -1, "refusing to nest: a prover is already running"
    if criterion.kind in ("path", "tiers"):
        # NOT `bash -c criterion.run`: these kinds carry no shell at all, and an empty command exits
        # ZERO. Reading the repository through the same evaluator the sandbox uses IS the control;
        # running an empty string reported every artefact and every tier claim as green in the tree,
        # which is this module's own defect committed inside the control that exists to catch it.
        return Sandbox(REPO_ROOT).run(criterion)
    if _RUNS_THE_ROADMAP_TOOLING.search(without_comments(criterion.run)):
        return -1, "a criterion that runs this module cannot be controlled by running it again"
    environment = {key: value for key, value in os.environ.items() if not key.startswith("CY_")}
    environment["CY_FALSIFY"] = "1"
    if build_dir:
        environment["CY_BUILD_DIR"] = build_dir
    try:
        completed = subprocess.run(  # noqa: S603 — the command is committed data
            ["bash", "-c", criterion.run], cwd=REPO_ROOT, capture_output=True, text=True,
            timeout=timeout_s, check=False, env=environment)
    except subprocess.TimeoutExpired:
        return 124, f"no result within {timeout_s} s"
    return completed.returncode, (completed.stdout or "") + (completed.stderr or "")


def _red_in_the_tree(criterion: criteria_module.Criterion, code: int, output: str, finished) -> Proof:
    """A criterion that failed unmutated: watched going red, or red for the sandbox's own reasons.

    THE TREE CONTROL IS WHAT SEPARATES THOSE TWO. The sandbox is a copy of the TRACKED tree, so a
    criterion can be red in it for a reason that has nothing to do with its subject — a generated
    header nobody commits, an untracked fixture. Red in the sandbox AND red in the repository is a
    property of the repository; red only in the sandbox is a property of the copy, and is reported
    `not provable here` with that said in as many words.
    """
    if code == 124:
        return finished(UNPROVABLE, "-", f"the unmutated run did not finish: {_first_line(output)}")
    tree_code, tree_output = run_in_the_repository(criterion, "", TREE_CONTROL_TIMEOUT_S)
    if tree_code < 0:
        return finished(UNPROVABLE, "-", f"red in the sandbox, and the tree control cannot run: "
                                         f"{tree_output}", unjudged=True)
    if tree_code == 124:
        return finished(UNPROVABLE, "-", "red in the sandbox, and the tree control did not finish "
                                         f"within {TREE_CONTROL_TIMEOUT_S} s")
    if tree_code == 0:
        return finished(UNPROVABLE, "-",
                        "it is red in the sandbox and GREEN in the repository, so the copy is what "
                        f"made it red, not the tree: {_first_line(output)}")
    return finished(RED_IN_THE_TREE, "-",
                    f"red unmutated, in the sandbox and in the repository (exit {code}): "
                    f"{_first_line(output)}")


def _prove_against_a_build(criterion: criteria_module.Criterion, build_dir: str, blocked: str,
                           mutation: Mutation | None, finished) -> Proof:
    """A criterion the source-only sandbox cannot run at all, judged against a real build tree.

    WITHOUT `--build-dir` NOTHING IS CLAIMED: it is `not provable here`, named, with the reason. With
    one, the criterion is RUN — unmutated, in the repository, against that tree — and if it fails it
    has been watched going red, which is the same shape as `_red_in_the_tree` and is recorded apart
    only because a source-only run cannot re-earn it.

    A criterion that PASSES against a build is NOT proven here, and this is the honest edge of the
    tool: turning it red needs its source mutated and the tree rebuilt, which this prover does not do
    — mutating the repository is exactly what the sandbox exists to prevent. Those are named, with
    the mutation their own text implies, so that whoever builds the job that proves them knows what
    to break.
    """
    if not build_dir:
        return finished(UNPROVABLE, mutation.describe() if mutation else "-", blocked, unjudged=True)
    budget = min(build_budget(), criterion.timeout_s or BUILD_PROOF_TIMEOUT_S)
    code, output = run_in_the_repository(criterion, build_dir, budget)
    if code < 0:
        return finished(UNPROVABLE, "-", f"{blocked}; and {output}", unjudged=True)
    if code == 124:
        return finished(UNPROVABLE, "-", f"against {build_dir} it did not finish within {budget} s")
    if code != 0:
        return finished(RED_WITH_A_BUILD, "-",
                        f"red unmutated against the build tree {build_dir} (exit {code}): "
                        f"{_first_line(output)}")
    return finished(UNPROVABLE, mutation.describe() if mutation else "-",
                    f"it PASSES against the build tree {build_dir}; turning it red needs its source "
                    "mutated and the tree rebuilt, which this prover does not do")


def prove(sandbox: Sandbox, ledger: str, criterion: criteria_module.Criterion,
          build_dir: str = "") -> Proof:
    """Positive control, mutation, ledger-blind control. Anything short of all three is not a proof.

    UNLESS THE UNMUTATED RUN IS ITSELF THE PROOF. A criterion that FAILS as written has been watched
    going red, which is the claim a mutation exists to demonstrate; `_red_in_the_tree` is that shape
    and `_prove_a_declared_gap` is its sibling for the criteria a ledger declares expecting to fail.
    Both are checked before the mutation, and they have to be: a criterion whose subject does not
    exist yet — a game nobody has written, a benchmark nobody has committed — has no file for a
    mutation to break, and the older order reported "the mutation changed nothing" about criteria
    that were red for precisely the reason they were declared.
    """
    started = time.monotonic()

    def finished(verdict: str, mutation: str, detail: str, unjudged: bool = False) -> Proof:
        return Proof(ledger, criterion.id, digest(criterion), verdict, mutation, detail,
                     round(time.monotonic() - started, 2), unjudged)

    # THE STATIC RULES DECIDE FIRST, and they have to, because the shapes they name are exactly the
    # ones a sandbox cannot reach: `just test-render -R <suite>` selects nothing and passes, and the
    # sandbox has no build to select from, so a mutation run would report `not provable here` about a
    # criterion that is already known not to be able to fail.
    blocking = [finding for finding in inspect(criterion) if finding.rule in CANNOT_GO_RED]
    if blocking:
        return finished(REFUTED, "-", "; ".join(
            f"{finding.rule}: {finding.detail}" for finding in blocking))

    # A CRITERION THIS HOST CANNOT EVALUATE MAY NOT BE JUDGED HERE EITHER, and getting this wrong
    # would have been this module committing the defect it polices. `just run-editor --smoke` fails
    # on a machine with no display and `just test-determinism --compare-legs` fails on a machine with
    # one architecture — and a build-backed run would have read both as "watched going red", which is
    # a verdict about this laptop rather than about the repository. `criteria.unmet_requirement` is
    # the same probe the ledger uses to report NOT EVALUATED rather than passed.
    unmet = criteria_module.unmet_requirement(criterion)
    if unmet:
        return finished(UNPROVABLE, "-", f"this host cannot evaluate it, so it cannot judge it "
                                         f"either: {unmet} — CI job '{criterion.ci_job}'",
                        unjudged=True)

    mutation = derive(criterion)
    blocked = unsandboxable(criterion)
    if blocked:
        return _prove_against_a_build(criterion, build_dir, blocked, mutation, finished)

    code, output = sandbox.run(criterion)
    if code != 0:
        # A DECLARED GAP IS JUDGED THE OTHER WAY ROUND and keeps its own route: its ledger says it is
        # expected to fail, so what it owes is a mutation that makes it GREEN. Only a criterion that
        # is red WITHOUT having declared itself so falls through to the tree control.
        if criterion.is_declared_gap:
            if mutation is None:
                return finished(NO_MUTATION, "-",
                                "a declared gap owes a mutation that makes it GREEN, and none can be "
                                "derived from its text: it must declare a [criterion.falsifies]")
            return _prove_a_declared_gap(sandbox, criterion, mutation, output, finished)
        return _red_in_the_tree(criterion, code, output, finished)

    if mutation is None:
        return finished(NO_MUTATION, "-", "no mutation can be derived from this criterion's text; "
                                          "it must declare a [criterion.falsifies]")

    try:
        changed = sandbox.apply(mutation)
    except (OSError, ValueError) as error:
        sandbox.restore()
        return finished(UNPROVABLE, mutation.describe(), f"the mutation could not be applied: {error}")
    if not changed:
        sandbox.restore()
        return finished(REFUTED, mutation.describe(),
                        "the mutation changed nothing — it names a file or a token that is not there")
    code, output = sandbox.run(criterion)
    sandbox.restore()
    if code == 0:
        return finished(REFUTED, mutation.describe(),
                        f"the criterion still PASSES with {changed} file(s) mutated: it cannot go red")

    blind = _ledger_blind(sandbox, criterion)
    if blind:
        return finished(REFUTED, mutation.describe(), blind)
    return finished(PROVEN, mutation.describe(), f"red under mutation of {changed} file(s)")


def _prove_a_declared_gap(sandbox: Sandbox, criterion: criteria_module.Criterion,
                          mutation: Mutation, red: str, finished) -> Proof:
    """A criterion that is RED on the unmutated tree, judged the other way round.

    THE POSITIVE CONTROL CANNOT APPLY TO IT, and demanding one would be the wrong question twice
    over. A declared gap is a criterion its own ledger says is expected to fail — it is failing, in
    the open, on every run of that ledger — so "show that it can go red" is a demonstration of
    something already demonstrated, and reporting it `not provable here` puts a check that is
    visibly working on the list of checks nobody has judged.

    What such a criterion has NOT shown is that it is not PERMANENTLY red, and that is the whole
    difference between a gap that is a deadline and a criterion that can never pass — the shape
    `_check_known_gap` exists to keep honest at the other end, where a gap that starts passing is a
    ledger failure. So the mutation is required to make it GREEN: it is the gap's own closing act,
    made small, executed by the tooling rather than described. A mutation that leaves it red proves
    nothing about what it measures, and says so rather than counting.

    The ledger-blind control is not run here: it asks whether a PASS survives the ledgers being
    deleted, and this criterion does not pass.
    """
    try:
        changed = sandbox.apply(mutation)
    except (OSError, ValueError) as error:
        sandbox.restore()
        return finished(UNPROVABLE, mutation.describe(), f"the mutation could not be applied: {error}")
    if not changed:
        sandbox.restore()
        return finished(REFUTED, mutation.describe(),
                        "the mutation changed nothing — it names a file or a token that is not there")
    code, output = sandbox.run(criterion)
    sandbox.restore()
    if code != 0:
        return finished(UNPROVABLE, mutation.describe(),
                        "this criterion is a declared gap and is RED on the unmutated tree, and the "
                        "declared mutation does not make it green, so what it measures is "
                        f"undetermined: {_last_line(output)}")
    return finished(PROVEN, mutation.describe(),
                    f"a declared gap: RED unmutated ({_last_line(red)}), GREEN under the mutation of "
                    f"{changed} file(s) — the gap is a deadline rather than a permanent red")


def _ledger_blind(sandbox: Sandbox, criterion: criteria_module.Criterion) -> str:
    """Whether the criterion's verdict comes from tools/roadmap/ rather than from the repository."""
    if criterion.kind != "command" and criterion.kind != "recipe":
        return ""
    if not searches(criterion.run):
        return ""
    if "roadmap" in criterion.run and "tools/roadmap" in criterion.run:
        pass  # it names the directory deliberately; the control below still decides
    removed = sandbox.hide_the_ledgers()
    code, output = sandbox.run(criterion)
    sandbox.restore()
    if removed and code != 0:
        return ("the criterion goes RED when tools/roadmap/milestones/ is deleted, so its verdict "
                f"comes from its own ledger rather than from the repository: {_first_line(output)}")
    return ""


def _first_line(output: str) -> str:
    for line in output.splitlines():
        if line.strip():
            return line.strip()[:160]
    return "(no output)"


def _last_line(output: str) -> str:
    """The LAST thing a check said, which is where a report that prints its legs puts the verdict."""
    for line in reversed(output.splitlines()):
        if line.strip():
            return line.strip()[:160]
    return "(no output)"


# --- The inventory: what the ladder has not yet shown can fail ------------------------------------


@dataclass
class Inventory:
    """What the ladder has shown can fail, and what it has not. Read from falsifiability.toml."""

    proofs: dict[tuple[str, str], Proof] = field(default_factory=dict)
    unproven: dict[tuple[str, str], Proof] = field(default_factory=dict)

    def entry(self, key: tuple[str, str]) -> Proof | None:
        return self.proofs.get(key) or self.unproven.get(key)


def read_inventory(path: Path = INVENTORY) -> Inventory:
    if not path.is_file():
        return Inventory()
    document = tomllib.loads(path.read_text(encoding="utf-8"))
    inventory = Inventory()
    for table, verdict in (("proof", PROVEN), ("unproven", "")):
        for entry in document.get(table, ()):
            key = (entry["ledger"], entry["criterion"])
            target = inventory.proofs if table == "proof" else inventory.unproven
            target[key] = Proof(entry["ledger"], entry["criterion"], entry["digest"],
                                entry.get("verdict", verdict), entry.get("mutation", ""),
                                entry.get("detail", ""))
    return inventory


def _quote(value: str) -> str:
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"').replace("\n", " ") + '"'


def _entry_lines(table: str, proof: Proof, with_verdict: bool) -> list[str]:
    lines = [f"[[{table}]]",
             f"ledger = {_quote(proof.ledger)}",
             f"criterion = {_quote(proof.criterion)}",
             f"digest = {_quote(proof.digest)}"]
    if with_verdict:
        lines.append(f"verdict = {_quote(proof.verdict)}")
    lines.append(f"mutation = {_quote(proof.mutation)}")
    lines.append(f"detail = {_quote(proof.detail)}")
    lines.append("")
    return lines


def write_inventory(inventory: Inventory, path: Path = INVENTORY) -> None:
    lines = [_INVENTORY_HEADER,
             f"# {len(inventory.proofs)} proven, {len(inventory.unproven)} not yet shown able to "
             f"fail.", ""]
    for _key, proof in sorted(inventory.proofs.items()):
        # THE VERDICT IS WRITTEN OUT for proofs as well as for unproven entries, because a proof now
        # comes in more than one shape and `reconcile` requires the recorded shape to be the observed
        # one. An entry written before this carried no `verdict` key and reads back as `proven`,
        # which is what it was.
        lines.extend(_entry_lines("proof", proof, with_verdict=True))
    for _key, proof in sorted(inventory.unproven.items()):
        lines.extend(_entry_lines("unproven", proof, with_verdict=True))
    path.write_text("\n".join(lines), encoding="utf-8")


_INVENTORY_HEADER = '''# What the ladder has shown can FAIL, and what it has not.
#
# GENERATED by `just roadmap-falsify --record`. Do not hand-edit: every entry carries a digest of
# what its criterion checks, and `just roadmap-test` re-runs the proofs rather than believing this
# file — an entry that disagrees with what the tooling observes is refused exactly as a missing one
# is, in both directions.
#
# THE LIST OF UNPROVEN ENTRIES ONLY EVER SHRINKS, and that is the mechanism. A criterion that is
# neither proven nor listed here fails `just roadmap-test`, which is the `plan-consistency` criterion
# of every ledger on the ladder — so a NEW criterion arrives with no entry and must come with a
# proof. An entry whose criterion has been edited fails too: the digest is over the command, the
# artefact, the expected tiers and the declared mutation, so re-wording a description is free and
# changing a check costs a re-proof. And an entry that has become provable fails as well, with
# "delete the entry" — a marker that outlives its gap is the next thing nobody notices.
#
# A `[[proof]]` is three runs against a sandboxed copy of the tracked tree: the criterion passes
# unmutated, goes RED when the tooling breaks what it names, and still passes when
# tools/roadmap/milestones/ is deleted. See tools/roadmap/falsify.py.
'''


# --- Reconciling what is observed with what is recorded -------------------------------------------


#: How `check` — and therefore `just roadmap-test` — is told that this machine has a build tree the
#: criteria needing one can be judged against. A flag would not reach it: `check` is run by a recipe
#: inside a self-test, so the environment is the only channel that survives the three layers.
BUILD_DIR_VARIABLE = "CY_FALSIFY_BUILD_DIR"


def prove_the_ladder(ledgers=(), build_dir: str | None = None) -> list[Proof]:
    """Run every proof, in one sandbox. Two seconds for the whole ladder, so a gate can afford it."""
    if build_dir is None:
        build_dir = os.environ.get(BUILD_DIR_VARIABLE, "")
    with tempfile.TemporaryDirectory(prefix="cy-falsify-", ignore_cleanup_errors=True) as directory:
        sandbox = Sandbox.materialise(Path(directory) / "tree")
        return _prove_all(sandbox, tuple(ledgers), "", build_dir)


def reconcile(observed: list[Proof], inventory: Inventory) -> list[str]:
    """Everything the observed proofs and the recorded inventory disagree about.

    FOUR DIRECTIONS, and the third and fourth are the ones that keep the mechanism from rotting the
    way six comments did:

      a criterion nothing has judged      it is new, or it has been edited since; it arrives with a
                                          proof or it does not arrive;
      an entry whose digest has moved     the check changed, so the judgement about it lapsed;
      an entry that has become provable   "DELETE THE ENTRY" — the debt was paid and the marker
                                          outlived it;
      a proof that has stopped proving    a criterion that used to go red under mutation and no
                                          longer does has quietly become one of the seven.
    """
    findings: list[str] = []
    seen: set[tuple[str, str]] = set()
    for proof in observed:
        key = (proof.ledger, proof.criterion)
        label = f"{proof.ledger}:{proof.criterion}"
        seen.add(key)
        recorded = inventory.entry(key)
        if recorded is None:
            findings.append(
                f"{label}: nothing in falsifiability.toml has judged this criterion. A criterion "
                f"joins a ledger with a proof that it can go red — run `just roadmap-falsify "
                f"--record`. (This run says: {proof.verdict} — {proof.detail})")
            continue
        if recorded.digest != proof.digest:
            findings.append(
                f"{label}: what this criterion CHECKS has changed since it was last judged "
                f"({recorded.digest} -> {proof.digest}). Re-run `just roadmap-falsify --record`.")
            continue
        if proof.verdict in PROOF_VERDICTS and key in inventory.unproven:
            findings.append(
                f"{label}: is listed as not yet shown able to fail, and it has now been shown to go "
                f"red ({proof.verdict}: {proof.detail}). DELETE THE ENTRY from falsifiability.toml.")
        if proof.verdict not in PROOF_VERDICTS and key in inventory.proofs:
            # A BUILD-BACKED PROOF IS NOT RE-EARNED BY A SOURCE-ONLY RUN, and pretending otherwise
            # would turn every laptop's `just roadmap-test` red over a proof that is not in question.
            # The run that CAN re-earn it is the one with a build: `prove --build-dir`, or
            # `CY_FALSIFY_BUILD_DIR` in the environment of `check`. Anything else is a finding.
            if proof.unjudged:
                continue
            findings.append(
                f"{label}: is recorded as {recorded.verdict} and no longer proves — "
                f"{proof.verdict}: {proof.detail}")
            continue
        if (proof.verdict in PROOF_VERDICTS and key in inventory.proofs
                and recorded.verdict != proof.verdict):
            # THE SHAPE OF THE PROOF CHANGED, which is the direction that matters: a criterion
            # recorded `red in the tree` has gone GREEN, so the evidence that it can fail is now the
            # evidence nobody has — it owes an ordinary mutation proof. This is what stops a rung
            # closing by quietly turning its red criteria green.
            findings.append(
                f"{label}: was recorded as '{recorded.verdict}' and is now '{proof.verdict}' "
                f"({proof.detail}). Re-run `just roadmap-falsify --record`.")
    for key in sorted(set(inventory.proofs) | set(inventory.unproven)):
        if key not in seen:
            findings.append(f"{key[0]}:{key[1]}: is in falsifiability.toml and in no ledger. "
                            "Delete the entry.")
    return findings


# --- The audit ------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Row:
    ledger: str
    criterion: criteria_module.Criterion
    findings: tuple[Finding, ...]
    weak: tuple[Finding, ...]

    @property
    def label(self) -> str:
        return f"{self.ledger}:{self.criterion.id}"

    @property
    def cannot_go_red(self) -> tuple[Finding, ...]:
        return tuple(finding for finding in self.findings if finding.rule in CANNOT_GO_RED)


def audit(ledgers=()) -> list[Row]:
    """Every criterion on the ladder, with the shapes in it that are known not to be able to fail."""
    rows = []
    for identifier in (ledgers or criteria_module.available()):
        for criterion in criteria_module.load(identifier).criteria:
            rows.append(Row(identifier, criterion, inspect(criterion), weakness(criterion)))
    return rows


# --- The command line -----------------------------------------------------------------------------


def command_audit(arguments: argparse.Namespace) -> int:
    rows = audit(tuple(arguments.milestone))
    broken = [row for row in rows if row.cannot_go_red]
    weak = [row for row in rows if row.weak and not row.cannot_go_red]
    for row in broken:
        print(f"CANNOT GO RED  {row.label}")
        for finding in row.cannot_go_red:
            print(f"                 {finding.rule}: {finding.detail}")
    if arguments.weak:
        for row in weak:
            print(f"weak           {row.label}")
            for finding in row.weak:
                print(f"                 {finding.rule}: {finding.detail}")
    print(f"\n{len(rows)} criterion declaration(s): {len(broken)} cannot go red, "
          f"{len(weak)} measure only that some text exists")
    return 1 if broken else 0


def command_prove(arguments: argparse.Namespace) -> int:
    build_dir = arguments.build_dir or os.environ.get(BUILD_DIR_VARIABLE, "")
    if arguments.budget:
        _BUILD_BUDGET[:] = [arguments.budget]
    if build_dir and not (REPO_ROOT / build_dir).is_dir():
        print(f"falsify: no build tree at {build_dir}", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="cy-falsify-", ignore_cleanup_errors=True) as directory:
        sandbox = Sandbox.materialise(Path(directory) / "tree")
        proofs = _prove_all(sandbox, tuple(arguments.milestone), arguments.only, build_dir)
    counts: dict[str, int] = {}
    for proof in proofs:
        counts[proof.verdict] = counts.get(proof.verdict, 0) + 1
        if proof.verdict != PROVEN or arguments.verbose:
            print(f"{proof.verdict.upper():<18} {proof.ledger}:{proof.criterion}  "
                  f"[{proof.mutation}] {proof.detail}")
    print("\n" + ", ".join(f"{count} {name}" for name, count in sorted(counts.items())))
    if arguments.record:
        refused = _record(proofs, tuple(arguments.milestone), arguments.baseline)
        for line in refused:
            print(f"REFUSED TO RECORD  {line}")
        if refused:
            return 1
        print(f"recorded in {INVENTORY.relative_to(REPO_ROOT)}")
    return 1 if any(proof.verdict == REFUTED for proof in proofs) else 0


def _prove_all(sandbox: Sandbox, ledgers: tuple[str, ...], only: str,
               build_dir: str = "") -> list[Proof]:
    proofs = []
    for identifier in (ledgers or criteria_module.available()):
        for criterion in criteria_module.load(identifier).criteria:
            if only and only not in criterion.id:
                continue
            proof = prove(sandbox, identifier, criterion, build_dir)
            sandbox.restore()
            proofs.append(proof)
    return proofs


def _record(proofs: list[Proof], ledgers: tuple[str, ...], baseline: bool = False) -> list[str]:
    """Write what was observed. The unproven list only ever SHRINKS, and this is where that holds.

    A PROOF MAY ALWAYS BE RECORDED. An entry saying "this one has not been shown able to fail" may
    not, unless it is already there with the same digest — otherwise `--record` would be the escape
    hatch that makes the whole mechanism advisory: write the criterion, run the recorder, and the
    eighth unfalsifiable criterion is on the ladder with the tooling's blessing. The one exception is
    `--baseline`, which is how the 584 criteria this ladder already carries were first written down;
    it is a flag on the command line rather than a field in a file, so using it is a deliberate act
    that shows up in a shell history and in a review.
    """
    inventory = read_inventory()
    refused: list[str] = []
    for proof in proofs:
        key = (proof.ledger, proof.criterion)
        if proof.verdict in PROOF_VERDICTS:
            inventory.proofs[key] = proof
            inventory.unproven.pop(key, None)
            continue
        # A BUILD-BACKED PROOF SURVIVES A SOURCE-ONLY RE-RECORD for the same reason `reconcile` does
        # not flag it: this run had no build and did not judge it. It is re-earned, or contradicted,
        # by a run that has one.
        standing_proof = inventory.proofs.get(key)
        if (proof.unjudged and standing_proof is not None
                and standing_proof.digest == proof.digest):
            continue
        standing = inventory.unproven.get(key)
        if not baseline and (standing is None or standing.digest != proof.digest):
            refused.append(
                f"{proof.ledger}:{proof.criterion} has not been shown able to fail "
                f"({proof.verdict}: {proof.detail}) and is not already on the list. The list only "
                "shrinks: prove it, or fix the criterion.")
            continue
        inventory.unproven[key] = proof
        inventory.proofs.pop(key, None)
    if refused:
        return refused
    if not ledgers:
        _forget_deleted(inventory)
    write_inventory(inventory)
    return []


def _forget_deleted(inventory: Inventory) -> None:
    live = {(identifier, criterion.id)
            for identifier in criteria_module.available()
            for criterion in criteria_module.load(identifier).criteria}
    for key in [key for key in inventory.proofs if key not in live]:
        del inventory.proofs[key]
    for key in [key for key in inventory.unproven if key not in live]:
        del inventory.unproven[key]


def command_check(arguments: argparse.Namespace) -> int:
    del arguments
    observed = prove_the_ladder()
    inventory = read_inventory()
    findings = reconcile(observed, inventory)
    for finding in findings:
        print(f"  {finding}")
    # SAY OUT LOUD WHAT THIS RUN DID NOT JUDGE. A proof taken against a build tree is carried by a
    # source-only run rather than re-earned, and a carried proof nobody can see is the beginning of
    # a recorded number nobody re-earns — which is the decay this whole module was written against.
    carried = [proof for proof in observed
               if proof.unjudged and (proof.ledger, proof.criterion) in inventory.proofs]
    if carried:
        print(f"{len(carried)} recorded proof(s) this run could not re-earn — it has no build tree. "
              f"Set {BUILD_DIR_VARIABLE} to one, or run "
              "`just roadmap-falsify prove --build-dir <dir>`:")
        for proof in carried[:10]:
            print(f"    {proof.ledger}:{proof.criterion}")
    print(f"{len(findings)} disagreement(s) between the ladder and falsifiability.toml")
    return 1 if findings else 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="falsify", description=__doc__.splitlines()[0])
    subcommands = parser.add_subparsers(dest="command", required=True)

    audit_command = subcommands.add_parser("audit", help="which criteria cannot go red")
    audit_command.add_argument("milestone", nargs="*", help="ledgers to audit (default: all)")
    audit_command.add_argument("--weak", action="store_true",
                               help="also list criteria whose only verdict is that text exists")
    audit_command.set_defaults(handler=command_audit)

    prove_command = subcommands.add_parser(
        "prove", help="break what each criterion names, in a sandbox, and require it to go red")
    prove_command.add_argument("milestone", nargs="*", help="ledgers to prove (default: all)")
    prove_command.add_argument("--only", default="", help="only criteria whose id contains this")
    prove_command.add_argument("--record", action="store_true", help="write the inventory")
    prove_command.add_argument(
        "--baseline", action="store_true",
        help="allow NEW unproven entries. The one-time flag that wrote this ladder's existing debt; "
             "after that the list only shrinks")
    prove_command.add_argument(
        "--build-dir", default="",
        help="a BUILT tree to judge the criteria a source-only sandbox cannot run against. They are "
             "run unmutated, in the repository, and a failure is recorded as observed rather than "
             f"argued. Also read from {BUILD_DIR_VARIABLE}")
    prove_command.add_argument(
        "--budget", type=int, default=0,
        help="seconds a build-backed criterion gets, when that is tighter than its own timeout_s")
    prove_command.add_argument("--verbose", action="store_true", help="print the proven ones too")
    prove_command.set_defaults(handler=command_prove)

    check_command = subcommands.add_parser(
        "check", help="re-run every proof and reconcile it with falsifiability.toml")
    check_command.set_defaults(handler=command_check)
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        return arguments.handler(arguments)
    except criteria_module.CriteriaError as error:
        print(f"falsify: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

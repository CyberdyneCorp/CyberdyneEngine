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

AND A PROOF COMES IN FOUR SHAPES, for the same reason one level down: a criterion that cannot pass
the positive control is not thereby unjudged.

    proven              it passes, and the mutation turns it RED
    red in the tree     it FAILS as written, in the sandbox AND in the repository — the tooling has
                        watched it go red, which is the whole of what a mutation stands in for
    red against a       the same, for a criterion a source-only sandbox cannot run at all: observed
    built tree          against a real build named on the command line (`prove --build-dir`)
    proven against a    it PASSES against that build, and the mutation applied to the WORKING TREE,
    built tree          rebuilt over by the criterion's own body, turns it RED — and restoring the
                        tree turns it green again (`prove --build-dir --mutate-the-tree`)

WHY THE FOURTH EXISTS, AND IT IS THE HOLE THE OTHER THREE LEFT. A criterion whose subject is a
COMPILED artefact cannot be judged by a copy of the tracked tree. With `--build-dir` it was run
unmutated against a real one, and when it PASSED this module had nothing left to say: "turning it red
needs its source mutated and the tree rebuilt, which this prover does not do." That sentence named
SIXTEEN of M11.a's and M11.b's seventy criteria, and it is a description of the seven — green, with
nothing in the tooling able to turn it red. The rule that produced it ("never mutate the working
tree") is therefore narrowed rather than kept: `WorkingTree` mutates the repository under a flag,
under a guard that refuses to run inside another prover, remembers every byte it overwrites, restores
on every exit path including a signal, and afterwards requires `git status` — a witness that took no
part in the bookkeeping — to report exactly what it reported before. A tree that does not come back,
or a HEAD that moved while the tree was mutated, raises and stops the run.

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
import functools
import hashlib
import json
import os
import re
import shlex
import shutil
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
#: THE FOURTH SHAPE, AND THE ONE THAT CLOSES THE HOLE THE OTHER THREE LEFT. A criterion that needs a
#: build and PASSES against one was, until M11's repair round, `not provable here` — named, with the
#: mutation its own text implies, and nothing more. That is the shape the seven had: green, and
#: nothing in the tooling able to turn it red. Sixteen of M11.a's and M11.b's seventy sat in it.
#:
#: So the mutation is applied to the WORKING TREE, under `--mutate-the-tree`, the criterion's own
#: body rebuilds (every one of them opens with `just build-engine`), and the criterion must go red.
#: Then the tree is restored and the criterion must come back GREEN — which is what separates "the
#: mutation made it red" from "something about this run made it red", and is also how the restore is
#: verified by something other than the restorer's own bookkeeping.
#:
#: `WorkingTree` is what makes that safe: it refuses to start against a tree `git status` calls
#: dirty, remembers every byte it overwrites, restores on every exit path including a signal, and
#: verifies afterwards that git reports the tree clean again. A run that cannot put the tree back
#: does not return a verdict — it aborts, loudly, naming the files.
PROVEN_BY_REBUILD = "proven against a built tree"
#: THE FIFTH SHAPE, FOR THE CRITERIA THIS HOST CANNOT EVALUATE AT ALL. A `where = "ci"` criterion is
#: skipped by the ledger — `just test-determinism --compare-legs` cannot compare two architectures on
#: a machine that has one — and this module refused to judge it, correctly, because a red produced by
#: the laptop is a verdict about the laptop. Two of M11.a's seventy sat there, and M11's repair gate
#: called that dispositive: nobody had shown they could fail.
#:
#: What was missing was the ENVIRONMENT, not the criterion. `[criterion.ci_proof]` declares the
#: command that CONSTRUCTS what continuous integration hands the criterion — for the cross-leg pair,
#: a directory of digests published by several legs, which `tools/ci/cross_leg_audit.py --write-legs`
#: writes from the publisher's own field list — and the mutation of that environment which must turn
#: the criterion red. The criterion's own body then runs verbatim, three times, exactly as the
#: sandbox and the build-backed shapes run theirs: green against the environment, red under the
#: mutation, green again once it is restored.
#:
#: IT CLAIMS NOTHING ABOUT THE ANSWER. The criterion still reports NOT EVALUATED on this host and is
#: still answered only in CI. What is earned here is the thing every other criterion in the ladder
#: had and these two did not: a demonstration that the check is capable of saying no.
PROVEN_IN_THE_CI_ENVIRONMENT = "proven in the environment CI supplies"
REFUTED = "refuted"
UNPROVABLE = "not provable here"
NO_MUTATION = "no mutation"

#: The sentence `_gap_no_mutation_can_close` opens with, and the string `command_check` counts by.
#: A declared gap owes a mutation that makes it GREEN and every verb this module has SUBTRACTS, so a
#: gap that is red because the thing it names does not exist yet cannot be judged here at all. It is
#: said in one place and printed under its own heading rather than mixed into the build-backed carry,
#: because the two are unjudged for opposite reasons: one for want of a build on this host, this one
#: for want of a verb that could exist.
GAP_IS_ADDITIVE = "THIS GAP CLOSES BY AN ADDITION AND NO MUTATION VERB ADDS:"

#: Every verdict that counts as a proof. `reconcile` requires the recorded one to be the observed
#: one, so a criterion that changes proof shape is re-judged rather than carried.
PROOF_VERDICTS = (PROVEN, RED_IN_THE_TREE, RED_WITH_A_BUILD, PROVEN_BY_REBUILD,
                  PROVEN_IN_THE_CI_ENVIRONMENT)


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


#: The separators that are REDIRECTIONS. A bare file descriptor number written in front of one —
#: `grep -rl token src/ 2>/dev/null` — is part of the redirection and not a path to search, and
#: reading it as one is how `m11b:source-control-provider` came to declare a mutation against a file
#: called "2". Harmless in that it matched nothing, which is exactly what made it worth fixing: a
#: recorded proof naming a path that does not exist reads like a proof about that path.
_REDIRECTIONS = frozenset({">", ">>", "<", "<<", "|&"})
_FILE_DESCRIPTOR = re.compile(r"^[0-9]$")


def _split_on_separators(tokens: list[str]) -> list[list[str]]:
    commands: list[list[str]] = []
    current: list[str] = []
    for token in tokens:
        if token in _SEPARATORS:
            if token in _REDIRECTIONS and current and _FILE_DESCRIPTOR.match(current[-1]):
                current.pop()
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
    "absent-recipe": (
        "the body invokes a `just` recipe that does not exist, so `just` aborts at argument parsing "
        "and NOTHING the criterion names is ever measured — the red it produces is the name failing "
        "to resolve, not the subject failing"),
    "absent-sample": (
        "the body runs `just run-sample <name>` for a sample no samples/*/CMakeLists.txt declares, "
        "so run.just prints `no sample '<name>'` and exits 2 having run nothing — `absent-recipe` "
        "one level down, and the red it produces is again the name failing to resolve"),
}

#: Rules 1-5 say the criterion CANNOT GO RED — no state of the repository makes it fail. `presence-
#: only` is weaker and is reported apart: such a criterion can be turned red (delete the token) but
#: it is satisfied by typing the token, so it measures spelling rather than capability.
CANNOT_GO_RED = ("searches-the-repository-root", "self-match", "vacuous-suite", "no-assertion",
                 "swallowed-verdict", "absent-recipe", "absent-sample")

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
    findings = [*_inspect_searches(body), *_inspect_suites(body), *_inspect_verdict(body),
                *_inspect_recipes(body), *_inspect_samples(body)]
    return tuple(findings)


#: `just` options that consume the next token, so the token after one is a value rather than the
#: recipe name. `--set` consumes two. Anything else beginning with `-` consumes only itself.
_JUST_TAKES_A_VALUE = frozenset({"-f", "--justfile", "-d", "--working-directory", "--color",
                                 "--shell", "--shell-arg", "--chooser", "--command-color",
                                 "--dotenv-filename", "--dotenv-path", "--list-heading",
                                 "--list-prefix", "--timestamp-format"})


def _invocation(argv: list[str]) -> tuple[str, list[str]] | None:
    """The recipe a `just ...` command runs and the arguments it hands it.

    None when this is not a `just` command, or when the name is assembled at run time — guessing at
    `$recipe` is how a rule that exists to catch a name that does not resolve starts accusing names
    it could not see.
    """
    if not argv or Path(argv[0]).name != "just":
        return None
    index = 1
    while index < len(argv):
        token = argv[index]
        if token == "--set":
            index += 3
            continue
        if token in _JUST_TAKES_A_VALUE:
            index += 2
            continue
        if token.startswith("-") and len(token) > 1:
            index += 1
            continue
        if re.search(r"[$`*?]", token):
            return None
        return token, argv[index + 1:]
    return None


def _invoked_recipe(argv: list[str]) -> str | None:
    """The recipe name a `just ...` command runs, or None when this is not one or cannot be read."""
    invocation = _invocation(argv)
    return invocation[0] if invocation is not None else None


@functools.lru_cache(maxsize=1)
def recipe_names() -> frozenset[str]:
    """Every recipe this repository's justfile defines, private ones included.

    `just --summary` lists only the public ones and several criteria invoke `just _ctest`, so the
    JSON dump is what is read. An EMPTY set means "this could not be determined", and the rule below
    is skipped entirely rather than accusing every criterion in the ladder of naming a recipe that
    is not there — a rule that cannot see is a rule that must not speak.
    """
    try:
        dumped = subprocess.run(["just", "--dump", "--dump-format", "json"], cwd=REPO_ROOT,
                                capture_output=True, text=True, check=False, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return frozenset()
    if dumped.returncode != 0:
        return frozenset()
    try:
        return frozenset(json.loads(dumped.stdout).get("recipes", {}))
    except (ValueError, AttributeError):
        return frozenset()


def _inspect_recipes(body: str) -> list[Finding]:
    """A `just` recipe a criterion names that the justfile does not define.

    THE EIGHTH OF THE SEVEN, and the one that made the mechanism itself complicit. Three criteria —
    `m11a:network-at-complete-grade`, `m11b:gameplay-at-complete-grade`,
    `m11b:editor-at-complete-grade` — ran `just quality-requirements <rows...>`, and there is no such
    recipe. `just` stops at argument parsing with `error: Justfile does not contain recipes`, exit 1,
    having run nothing. The criterion is therefore red, unmutated, in the sandbox AND in the
    repository, which is precisely the shape `_red_in_the_tree` records as a PROOF — so a command
    that never executed was counted as a check that had been watched going red.

    A red that comes from a name failing to resolve says nothing about the subject, and it cannot be
    told apart from a real one by looking at the exit code. So it is refused here, before any run.
    """
    known = recipe_names()
    if not known:
        return []
    findings = []
    for argv in simple_commands(without_comments(body)):
        name = _invoked_recipe(argv)
        if name is not None and name not in known:
            findings.append(Finding("absent-recipe", f"just {name}: no such recipe"))
    return findings


@functools.lru_cache(maxsize=1)
def sample_names() -> frozenset[str]:
    """Every sample `just run-sample` can find, read from the declarations that build them.

    `just/run.just`'s own comment fixes the mapping: "samples/<nn>-<name>/cy_sample_<name>: the
    sample's number orders the directory listing and is not part of its name, so the binary is found
    by pattern rather than by a table kept in step." So the name is the one in `cy_add_module(NAME
    cy_sample_<name>)` and NOT the directory — which is exactly the difference the rule below
    catches.

    An EMPTY set means "this could not be determined", and the rule is skipped rather than accusing
    every criterion that runs a sample: a rule that cannot see is a rule that must not speak, which
    is `recipe_names()`'s rule above and the same one here.
    """
    found = set()
    for declaration in sorted((REPO_ROOT / "samples").glob("*/CMakeLists.txt")):
        try:
            text = declaration.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        found.update(re.findall(r"NAME\s+cy_sample_([A-Za-z0-9_-]+)", text))
    return frozenset(found)


def _inspect_samples(body: str) -> list[Finding]:
    """A sample a criterion runs by a name `just run-sample` cannot resolve.

    THE NINTH, AND IT IS `absent-recipe` ONE LEVEL DOWN — the same defect, in the same mechanism,
    caught by the same reasoning and missed because the rule above reads the recipe's name and stops
    there. `m11a:world-budget-headless` and `m11a:world-budget-on-a-device` ran
    `just run-sample 10-world ... --cycle --budget-ms 16.7`. The recipe exists; the sample is called
    `world` and not `10-world` — `10-world` is the DIRECTORY — so run.just printed
    `run-sample: no sample '10-world' in <build>/samples.` and exited 2, having run nothing at all.

    Both criteria were then recorded by this very module as `red against a built tree`, with the
    exit-2 line quoted in the record as though it were a measurement. A criterion that cannot run is
    indistinguishable, by exit code, from one that ran and failed — which is the whole defect this
    module exists to end, committed by the module. So a name `run-sample` cannot resolve is refused
    here, before any run, exactly as an absent recipe is.

    NOT AN ARGUMENT CHECK, and deliberately not: `--cycle` and `--budget-ms` did not exist either
    when those two criteria were written, and a static reader that tried to validate a sample's own
    command line would need every sample's parser. What it CAN read with certainty is which binaries
    `samples/*/CMakeLists.txt` declares, because that is the same list `run-sample` globs for.
    """
    known = sample_names()
    if not known:
        return []
    findings = []
    for argv in simple_commands(without_comments(body)):
        invocation = _invocation(argv)
        if invocation is None or invocation[0] != "run-sample":
            continue
        # `just run-sample --headless` with no name at all means `empty`, which run.just names as
        # its default. A leading flag is therefore not a sample name and not an accusation.
        names = [token for token in invocation[1] if not token.startswith("-")]
        if not names or re.search(r"[$`*?]", names[0]):
            continue
        if names[0] not in known:
            findings.append(Finding(
                "absent-sample",
                f"just run-sample {names[0]}: no samples/*/CMakeLists.txt declares "
                f"cy_sample_{names[0]}"))
    return findings


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


# --- The working tree, for the criteria only a build can judge -------------------------------------


class TreeNotRestored(RuntimeError):
    """The repository was mutated and could not be put back. Nothing continues after this."""


class WorkingTree:
    """The repository itself, mutated under a guard and put back — the one thing the sandbox cannot be.

    WHY THIS EXISTS, HAVING BEEN REFUSED ONCE. The rule above this module's first eight hundred lines
    is that a mutation never reaches the working tree, and it is the right rule: a tool that breaks
    the tree and puts it back is one crash away from leaving it broken, and this one runs hundreds of
    times. What that rule bought was a sandbox; what it cost was every criterion whose subject is a
    COMPILED artefact. A source-only copy has no build, so such a criterion is either red there for
    want of one — which the tree control correctly refuses to call a proof — or, with `--build-dir`,
    run unmutated against a real tree and found GREEN, at which point the module had nothing left to
    say but "turning it red needs its source mutated and the tree rebuilt, which this prover does not
    do". That sentence names sixteen of M11.a's and M11.b's criteria, and it describes the seven
    exactly: green, and nothing able to turn them red.

    So the rule is narrowed rather than kept: a mutation never reaches the working tree EXCEPT under
    `--mutate-the-tree`, which is a flag on a command line rather than a default, and which is
    answerable for putting the tree back exactly as it found it:

      * every byte it overwrites is remembered before it is overwritten (`Sandbox._remember`);
      * the restore runs on every exit path — normal, exception, SIGINT, SIGTERM, interpreter exit;
      * afterwards, `git status --porcelain -uno` must report EXACTLY what it reported before the
        mutation. That is a byte-for-byte comparison against the index of every tracked file, made
        by something that took no part in the bookkeeping, so a restore this class believes it made
        and did not is caught by a witness rather than by the witness's employer;
      * a tree that does not come back raises `TreeNotRestored`, which is caught nowhere: the run
        stops and names the files. A prover that carried on would be writing proofs about a tree it
        had already broken.

    THE BASELINE IS "AS THIS RUN FOUND IT", NOT "AS HEAD HAS IT", and that distinction is load-
    bearing rather than a convenience. Requiring a pristine checkout was the first draft, and it
    makes the tool unusable in the one situation it is for: a phase that is editing this very module
    cannot prove anything with it. So the state at construction is recorded, and what is required at
    the end is that state — a file already modified when the run began is restored to the bytes it
    had then. The recovery path is narrowed to match: `git checkout --` is offered only to paths that
    were CLEAN at the baseline, so it can never discard work that was in flight before this ran.

    It is refused outright inside another prover (`CY_FALSIFY`), where two runs would mutate one tree.
    """

    def __init__(self, root: Path = REPO_ROOT) -> None:
        self.root = root
        self._files = Sandbox(root)
        self._armed = False
        self._previous: dict = {}
        self._baseline = self._status()
        self._head = self._revision()
        #: Every path a mutation in this window has written to. Kept by this class rather than read
        #: off the sandbox, because `Sandbox.forget()` exists and clears the sandbox's copy — and
        #: this set is what bounds the recovery below.
        self._touched: set[str] = set()

    # --- what git says --------------------------------------------------------------------------

    def _status(self) -> tuple[str, ...]:
        """What `git status` reports as modified. Untracked files are not it.

        `-uno`: this prover's own runs leave untracked artefacts behind — a capture at the repository
        root, a generated header — and refusing to work in a tree that has any would refuse to work
        in this repository at all. What must come back is the TRACKED content, because that is what a
        mutation touches and what a restore has to reproduce.
        """
        reported = subprocess.run(["git", "status", "--porcelain", "-uno"], cwd=self.root,
                                  capture_output=True, text=True, check=False)
        if reported.returncode != 0:
            return (f"git could not report the state of {self.root}: {reported.stderr.strip()}",)
        return tuple(sorted(line for line in reported.stdout.splitlines() if line.strip()))

    def _revision(self) -> str:
        """What HEAD is. Read before the mutation and again after, for the reason in `restore`."""
        reported = subprocess.run(["git", "rev-parse", "HEAD"], cwd=self.root,
                                  capture_output=True, text=True, check=False)
        return reported.stdout.strip() if reported.returncode == 0 else ""

    def drift(self) -> tuple[str, ...]:
        """Everything git reports now that it did not report when this object was made."""
        return tuple(line for line in self._status() if line not in self._baseline)

    def modified_at_the_baseline(self) -> tuple[str, ...]:
        return self._baseline

    def dirty(self) -> str:
        """Drift from the baseline, as one line for a message. Empty when the tree is as it was."""
        return "\n".join(self.drift())

    def unavailable(self) -> str:
        """Why this tree may not be mutated at all, or an empty string."""
        if os.environ.get("CY_FALSIFY"):
            return "a prover is already running: two of them must not mutate one tree"
        if not (self.root / ".git").exists():
            return f"{self.root} is not a git checkout, so a restore could not be verified"
        return ""

    # --- the mutation window --------------------------------------------------------------------

    def apply(self, mutation: Mutation) -> int:
        """Apply the mutation to the repository, arming the restore FIRST."""
        self._arm()
        self._head = self._revision()
        changed = self._files.apply(mutation)
        self._touched.update(str(path.relative_to(self.root)) for path in self._files._saved)
        return changed

    def restore(self) -> None:
        """Put every remembered byte back, and require git to agree the tree is as it was.

        RAISES rather than returns a verdict. A caller that could handle this would be a caller that
        continues with a broken tree.
        """
        try:
            self._files.restore()
        finally:
            self._disarm()
        # THE HAZARD A RESTORE CANNOT UNDO, AND IT IS CHECKED FIRST, WHICH THIS REPOSITORY LEARNED
        # THE EXPENSIVE WAY ON THIS TOOL'S FIRST RUN. The working tree is restored; a COMMIT taken
        # while it was mutated is not. An orchestrator snapshotting between phases caught
        # `src/rendering/sky/tests/test_cloud_shadows.cpp` with a renamed token in it and committed
        # it — and afterwards the file on disk was right and HEAD was wrong, which is the one
        # direction `git status` reads as a stray modification to be tidied away. That is why this
        # is checked BEFORE the recovery below: `git checkout --` restores from the index, so
        # against a commit that contains the mutation it would faithfully put the mutation back.
        after = self._revision()
        if after != self._head:
            raise TreeNotRestored(
                f"the working tree was restored, but HEAD MOVED while it was mutated "
                f"({self._head[:12] or '(none)'} -> {after[:12] or '(none)'}). Whatever was "
                "committed in that window contains the mutation — check that commit and correct it "
                "before trusting the history. The tree on disk is correct; the commit is not.")
        left = self.drift()
        if left:
            # ONE RECOVERY ATTEMPT, AND IT IS GIT'S — `_remember` can only put back what it was asked
            # to change, and a criterion's own body may have written to a tracked file. It is
            # `git checkout`, which discards, so it is aimed at as little as it can be and not at
            # "whatever git is complaining about":
            #
            #   * not a path that was ALREADY modified when this run began. That is somebody's work
            #     in flight — this module's own source, while the phase that wrote this was editing
            #     it — and throwing it away would be a worse outcome than the one being recovered
            #     from;
            #   * not a path this run never wrote to. A prover takes minutes and an editor does not
            #     stop for it; a file that changed underneath belongs to whoever changed it, and the
            #     most this class may do about one is say so.
            already = {line[3:] for line in self._baseline}
            recoverable = [line[3:] for line in left
                           if line[3:] not in already and line[3:] in self._touched]
            if recoverable:
                subprocess.run(["git", "checkout", "--", *recoverable], cwd=self.root,
                               capture_output=True, check=False)
            left = self.drift()
        if left:
            mine = [line for line in left if line[3:] in self._touched]
            raise TreeNotRestored(
                "the working tree was mutated and has NOT been put back. Restore it before anything "
                "else is done in it — `git status` reports this, which it did not before:\n"
                + "\n".join(left)
                + ("\n(none of those is a file this mutation wrote to, so they are somebody else's "
                   "and were deliberately left alone)" if not mine else ""))

    def _arm(self) -> None:
        if self._armed:
            return
        self._armed = True
        import atexit
        import signal
        atexit.register(self._emergency)
        for number in (signal.SIGINT, signal.SIGTERM):
            try:
                self._previous[number] = signal.signal(number, self._on_signal)
            except (ValueError, OSError):  # not the main thread, or no such signal here
                pass

    def _disarm(self) -> None:
        if not self._armed:
            return
        self._armed = False
        import atexit
        import signal
        atexit.unregister(self._emergency)
        for number, handler in self._previous.items():
            try:
                signal.signal(number, handler)
            except (ValueError, OSError):
                pass
        self._previous.clear()

    def _emergency(self) -> None:
        """The last-resort restore: the interpreter is going away with the tree still mutated."""
        self._files.restore()
        print("falsify: the working tree was restored on the way out; git reports "
              f"{len(self.drift())} file(s) it did not report before this run", file=sys.stderr)

    def _on_signal(self, number, frame) -> None:  # noqa: ANN001 — a signal handler's signature
        del frame
        self._emergency()
        self._disarm()
        raise KeyboardInterrupt(f"interrupted by signal {number}; the working tree was restored")


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
    # THE DECLARED CI ENVIRONMENT IS PART OF WHAT THE PROOF WAS ABOUT, exactly as the declared
    # mutation is. A `where = "ci"` criterion is judged against what its `provide` builds, so changing
    # that command — or the mutation of it — changes what was demonstrated, and the standing proof
    # lapses rather than being carried over a different experiment.
    #
    # APPENDED ONLY WHEN THERE IS ONE, and that is not cosmetic. Adding an unconditional line to the
    # material moves the digest of every criterion on the ladder, which invalidates all 641 recorded
    # proofs at once — this module's own ratchet, broken by an edit to this module. That happened
    # once, in this function, and `check` reported 633 disagreements a moment later.
    if criterion.ci_proof:
        material += "\n" + repr(sorted(criterion.ci_proof.items()))
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
    r"just (build|test|run|content|release|generate|quality-lint|quality-tidy|quality-identity)")


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
                         timeout_s: int, root: Path = REPO_ROOT) -> tuple[int, str]:
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
        return Sandbox(root).run(criterion)
    if _RUNS_THE_ROADMAP_TOOLING.search(without_comments(criterion.run)):
        return -1, "a criterion that runs this module cannot be controlled by running it again"
    environment = {key: value for key, value in os.environ.items() if not key.startswith("CY_")}
    environment["CY_FALSIFY"] = "1"
    if build_dir:
        environment["CY_BUILD_DIR"] = build_dir
    try:
        completed = subprocess.run(  # noqa: S603 — the command is committed data
            ["bash", "-c", criterion.run], cwd=root, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=timeout_s, check=False, env=environment)
    except subprocess.TimeoutExpired:
        return 124, f"no result within {timeout_s} s"
    # ONE STREAM, IN ORDER. Concatenating stdout after stderr put a criterion's own verdict line in
    # the middle of the capture and a build tool's chatter at the end, so the recorded detail of a
    # proof read "Interprocedural optimizations are turned on" about a criterion that had said
    # exactly why it was red four lines earlier.
    return completed.returncode, completed.stdout or ""


def _red_in_the_tree(criterion: criteria_module.Criterion, code: int, output: str, finished,
                     build_dir: str = "") -> Proof:
    """A criterion that failed unmutated: watched going red, or red for the sandbox's own reasons.

    THE TREE CONTROL IS WHAT SEPARATES THOSE TWO. The sandbox is a copy of the TRACKED tree, so a
    criterion can be red in it for a reason that has nothing to do with its subject — a generated
    header nobody commits, an untracked fixture. Red in the sandbox AND red in the repository is a
    property of the repository; red only in the sandbox is a property of the copy, and is reported
    `not provable here` with that said in as many words.
    """
    if code == 124:
        return finished(UNPROVABLE, "-", f"the unmutated run did not finish: {_first_line(output)}")
    # THE BUILD DIRECTORY IS PASSED IN when the run has one, and that matters: `m1:identity-manifest`
    # is `just quality-identity`, whose build requirement is inside the recipe where no regex over
    # the criterion's text can see it. It is red on a machine with no build/dev and green against a
    # real tree — so a tree control that did not hand over the build would have recorded "watched
    # going red" about a criterion that was only missing a build, which is the defect this module
    # polices, committed by the module.
    tree_code, tree_output = run_in_the_repository(criterion, build_dir, TREE_CONTROL_TIMEOUT_S)
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
    # THE DETAIL COMES FROM THE TREE RUN, not the sandbox's. They are red for the same criterion but
    # not always at the same line — a sandbox lacks untracked inputs, so its first line can be an
    # import error over a check that fails in the repository for its own stated reason, and the
    # recorded sentence is what a reader has.
    return finished(RED_IN_THE_TREE, "-",
                    f"red unmutated, in the sandbox and in the repository (exit {tree_code}): "
                    f"{_why_it_is_red(tree_output)}")


def _prove_by_mutating_the_tree(criterion: criteria_module.Criterion, build_dir: str,
                                mutation: Mutation, tree: WorkingTree, finished) -> Proof:
    """The criterion passes against a real build. Break what it names, rebuild, and require RED.

    THREE RUNS, exactly as the sandbox has three, with the build standing in for the copy:

      positive control  already taken by the caller: the criterion passes, unmutated, against the
                        build tree named on the command line;
      the mutation      applied to the working tree. The criterion's own body rebuilds — every one
                        of the sixteen this exists for opens with `just build-engine` — so what is
                        judged is a COMPILED tree that carries the mutation, not a text file;
      the restore       the tree is put back and the criterion must come back GREEN. This is the run
                        that makes the middle one mean something: a criterion red under the mutation
                        AND red afterwards was red for some reason of its own, and a restore that
                        the tooling believes it made but did not would show up here as well.

    THE LEDGER-BLIND CONTROL IS NOT REPEATED HERE, and that is a rule rather than an omission. It
    exists because two of the seven were a grep that matched the ledger declaring it — and that
    shape is refused before any run reaches this function: `self-match` is in `CANNOT_GO_RED`, so a
    criterion whose search can reach `tools/roadmap/` is REFUTED by `inspect` at the top of `prove`,
    whatever it would do against a build.
    """
    budget = min(build_budget(), criterion.timeout_s or BUILD_PROOF_TIMEOUT_S)
    unavailable = tree.unavailable()
    if unavailable:
        return finished(UNPROVABLE, mutation.describe(),
                        f"the working tree cannot be mutated: {unavailable}", unjudged=True)
    dirty = tree.dirty()
    if dirty:
        return finished(UNPROVABLE, mutation.describe(),
                        "the working tree is not clean, so a mutation applied to it could not be "
                        f"told from what is already there: {dirty.splitlines()[0]}", unjudged=True)
    try:
        changed = tree.apply(mutation)
        if not changed:
            return finished(REFUTED, mutation.describe(),
                            "the mutation changed nothing — it names a file or a token that is not "
                            "there")
        code, output = run_in_the_repository(criterion, build_dir, budget, tree.root)
    finally:
        tree.restore()
    if code < 0:
        return finished(UNPROVABLE, mutation.describe(), output, unjudged=True)
    if code == 124:
        return finished(UNPROVABLE, mutation.describe(),
                        f"mutated, it did not finish within {budget} s", unjudged=True)
    if code == 0:
        return finished(REFUTED, mutation.describe(),
                        f"the criterion still PASSES with {changed} file(s) mutated and "
                        f"{build_dir} rebuilt over them: it cannot go red")
    back, restored_output = run_in_the_repository(criterion, build_dir, budget, tree.root)
    if back != 0:
        return finished(UNPROVABLE, mutation.describe(),
                        f"it went red with {changed} file(s) mutated and did NOT come back green "
                        f"when the tree was restored (exit {back}: {_first_line(restored_output)}), "
                        "so the redness cannot be laid at the mutation's door", unjudged=True)
    return finished(PROVEN_BY_REBUILD, mutation.describe(),
                    f"red under mutation of {changed} file(s), rebuilt against {build_dir} "
                    f"(exit {code}: {_last_line(output)}), and green again once restored")


def _prove_against_a_build(criterion: criteria_module.Criterion, build_dir: str, blocked: str,
                           mutation: Mutation | None, finished,
                           tree: WorkingTree | None = None) -> Proof:
    """A criterion the source-only sandbox cannot run at all, judged against a real build tree.

    WITHOUT `--build-dir` NOTHING IS CLAIMED: it is `not provable here`, named, with the reason. With
    one, the criterion is RUN — unmutated, in the repository, against that tree — and if it fails it
    has been watched going red, which is the same shape as `_red_in_the_tree` and is recorded apart
    only because a source-only run cannot re-earn it.

    A criterion that PASSES against a build IS the positive control of a fourth proof shape, and
    `_prove_by_mutating_the_tree` takes it from here when `--mutate-the-tree` has been given: the
    mutation goes into the working tree, the criterion's own body rebuilds over it, and the criterion
    has to go red and then come back. Without that flag the answer is the one this tool gave until
    M11's repair round — `not provable here`, with the mutation the criterion's own text implies, so
    that whoever runs the proving job knows what to break. That answer is UNJUDGED and not a finding:
    a run with no licence to mutate has not contradicted a standing proof, it has declined to re-earn
    one.
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
                        f"{_why_it_is_red(output)}")
    if tree is not None and mutation is not None:
        return _prove_by_mutating_the_tree(criterion, build_dir, mutation, tree, finished)
    if mutation is None:
        return finished(NO_MUTATION, "-",
                        f"it PASSES against the build tree {build_dir}, and no mutation can be "
                        "derived from its text: it must declare a [criterion.falsifies] before a "
                        "build-backed run can turn it red")
    return finished(UNPROVABLE, mutation.describe(),
                    f"it PASSES against the build tree {build_dir}; turning it red needs its source "
                    "mutated and the tree rebuilt, which needs `prove --mutate-the-tree`",
                    unjudged=True)


def prove(sandbox: Sandbox, ledger: str, criterion: criteria_module.Criterion,
          build_dir: str = "", tree: WorkingTree | None = None) -> Proof:
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
        if criterion.where == "ci" and criterion.ci_proof:
            return _prove_in_the_ci_environment(sandbox, criterion, finished)
        return finished(UNPROVABLE, "-", f"this host cannot evaluate it, so it cannot judge it "
                                         f"either: {unmet} — CI job '{criterion.ci_job}'",
                        unjudged=True)

    mutation = derive(criterion)
    blocked = unsandboxable(criterion)
    if blocked:
        return _prove_against_a_build(criterion, build_dir, blocked, mutation, finished, tree)

    code, output = sandbox.run(criterion)
    if code != 0:
        # A DECLARED GAP IS JUDGED THE OTHER WAY ROUND and keeps its own route: its ledger says it is
        # expected to fail, so what it owes is a mutation that makes it GREEN. Only a criterion that
        # is red WITHOUT having declared itself so falls through to the tree control.
        if criterion.is_declared_gap:
            attempted = ""
            if mutation is not None:
                proof = _prove_a_declared_gap(sandbox, criterion, mutation, output, finished)
                if proof.verdict == PROVEN:
                    return proof
                attempted = proof.detail
            return _gap_no_mutation_can_close(mutation, attempted, output, finished)
        return _red_in_the_tree(criterion, code, output, finished, build_dir)

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


#: How long the command that builds a CI-shaped environment may take. It writes files; it does not
#: build anything. A `provide` that needs longer than this is doing something other than laying out
#: what a job downloads, and the timeout is where that is noticed.
CI_ENVIRONMENT_TIMEOUT_S = 300


def _prove_in_the_ci_environment(sandbox: Sandbox, criterion: criteria_module.Criterion,
                                 finished) -> Proof:
    """A `where = "ci"` criterion, judged against the environment continuous integration gives it.

    THREE RUNS, the same three every other shape here is held to, with the declared environment
    standing in for the machine this host is not:

      positive control  `provide` is EXECUTED — never read — and the criterion's own body must then
                        pass against what it produced. A `provide` that supplied nothing leaves the
                        criterion red here and the verdict is `not provable here`, which is exactly
                        the answer this shape replaced, so the field cannot be filled in falsely;
      the mutation      applied to the environment, not to the criterion. One leg's digest changed,
                        and the comparison has to report the disagreement;
      the restore       the environment is put back and the criterion must come back GREEN, which is
                        what separates "the mutation made it red" from "this run made it red".

    THE VERDICT IS ABOUT THE CHECK, NOT ABOUT THE SUBJECT. Nothing here says two architectures agree
    — this host has one, which is the whole reason the criterion carries `where = "ci"` — and the
    ledger still reports it NOT EVALUATED. What is established is that the check can say no.
    """
    declared = criterion.ci_proof
    provide = str(declared.get("provide", ""))
    mutation = Mutation(verb=str(declared.get("mutate", "")), target=str(declared.get("target", "")),
                        token=str(declared.get("token", "")), derived=False)

    # ONE SANDBOX SERVES A WHOLE LEDGER, so an environment left behind by one criterion is an input
    # to the next. The first run of this shape proved the lockstep criterion and then reported the
    # pcg criterion `not provable here`, because `--write-legs` correctly refused to write over the
    # directory its neighbour had just created. What a proof leaves behind is therefore removed:
    # everything at the sandbox root that was not there before `provide` ran.
    before = {entry.name for entry in sandbox.root.iterdir()}
    environment = {key: value for key, value in os.environ.items() if not key.startswith("CY_")}
    environment["CY_FALSIFY"] = "1"
    try:
        built = subprocess.run(  # noqa: S603 — the command is committed data
            ["bash", "-c", provide], cwd=sandbox.root, capture_output=True, text=True,
            timeout=CI_ENVIRONMENT_TIMEOUT_S, check=False, env=environment)
    except (OSError, subprocess.TimeoutExpired) as error:
        _discard_the_ci_environment(sandbox, before)
        return finished(UNPROVABLE, mutation.describe(),
                        f"the environment CI supplies could not be built: {error}", unjudged=True)
    try:
        return _judge_in_the_ci_environment(sandbox, criterion, mutation, provide, built, finished)
    finally:
        _discard_the_ci_environment(sandbox, before)


def _discard_the_ci_environment(sandbox: Sandbox, before: set[str]) -> None:
    """Everything `provide` left at the sandbox root, removed. Nothing tracked is ever in this set."""
    for entry in sandbox.root.iterdir():
        if entry.name in before:
            continue
        if entry.is_dir() and not entry.is_symlink():
            shutil.rmtree(entry, ignore_errors=True)
        else:
            entry.unlink(missing_ok=True)


def _judge_in_the_ci_environment(sandbox: Sandbox, criterion: criteria_module.Criterion,
                                 mutation: Mutation, provide: str, built, finished) -> Proof:
    """The three runs, once `provide` has been executed. Separated so the cleanup is a `finally`."""
    if built.returncode != 0:
        return finished(UNPROVABLE, mutation.describe(),
                        f"`{provide}` exited {built.returncode}, so there is no environment to "
                        f"judge against: {_first_line(built.stdout + built.stderr)}", unjudged=True)

    code, output = sandbox.run(criterion)
    if code != 0:
        return finished(UNPROVABLE, mutation.describe(),
                        f"it is RED against the environment its own ci_proof built (exit {code}): "
                        f"{_first_line(output)} — a positive control that fails judges nothing",
                        unjudged=True)

    try:
        changed = sandbox.apply(mutation)
    except (OSError, ValueError) as error:
        sandbox.restore()
        return finished(UNPROVABLE, mutation.describe(), f"the mutation could not be applied: {error}")
    if not changed:
        sandbox.restore()
        return finished(REFUTED, mutation.describe(),
                        "the mutation changed nothing — it names a file or a token that is not "
                        "there in the environment ci_proof built")
    mutated_code, mutated_output = sandbox.run(criterion)
    sandbox.restore()
    if mutated_code == 0:
        return finished(REFUTED, mutation.describe(),
                        f"the criterion still PASSES with {changed} file(s) of its CI environment "
                        "mutated: it cannot go red")

    back, restored_output = sandbox.run(criterion)
    if back != 0:
        return finished(UNPROVABLE, mutation.describe(),
                        f"it went red under the mutation and did NOT come back green when the "
                        f"environment was restored (exit {back}: {_first_line(restored_output)}), "
                        "so the redness cannot be laid at the mutation's door", unjudged=True)
    return finished(PROVEN_IN_THE_CI_ENVIRONMENT, mutation.describe(),
                    f"green against the environment `{provide}` builds, RED under the mutation of "
                    f"{changed} file(s) of it (exit {mutated_code}: {_last_line(mutated_output)}), "
                    "and green again once restored")


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


def _gap_no_mutation_can_close(mutation: Mutation | None, attempted: str, output: str,
                               finished) -> Proof:
    """A declared gap whose closing act is an ADDITION, which no mutation verb performs.

    EVERY VERB THIS MODULE HAS TAKES SOMETHING AWAY: `delete-path`, `delete-lines`, `rename-token`,
    `truncate`, `lower-tiers`. That is right for the question the tool was built to ask — break what
    a green criterion names and require it to go red — and it is the mirror of the question a
    declared gap asks, which is: make the red one GREEN. Eleven of this ladder's gaps are red
    because something is ABSENT — a forbidden-pattern checker nobody has written, a benchmark nobody
    has committed, a game project that does not exist, a screenshot nobody has captured,
    requirements nobody has mapped, a tier nobody has raised — and no subtraction performs an
    addition. So the author cannot write a `[criterion.falsifies]` that would work, and an author
    who writes one anyway has written a mutation that makes a check stop looking rather than one
    that closes the gap. THE TOOLING BEING UNABLE TO JUDGE A GAP IS A FINDING ABOUT THE TOOLING, and
    this is where it is said, per gap, in the prover's own words.

    THE VERDICT IS UNCHANGED AND THE FLAG IS WHAT IS NEW. `no mutation` and `not provable here` are
    what this module already returned here, and both still say what they said; what they did not say
    is that a run which could not ASK a question has not thereby contradicted the answer to a
    different one. Declaring a gap does not change what a criterion checks — `digest` does not read
    `known_gap` — so a standing `red in the tree` record is neither stale nor re-earned falsely by
    this run, and marking the verdict UNJUDGED carries it exactly as a source-only run carries a
    build-backed proof.

    IT IS NOT A WAY IN. `_record` carries an unjudged verdict only for a key already in the
    inventory; a changed digest on an existing red-in-the-tree proof is refreshed only after this
    run observes the gap still red. A NEW declared gap with no workable mutation is refused as it
    always was. The other direction is not this function's to hold and does not need to be: a
    declared gap that starts PASSING never reaches here — `prove` only calls it on a criterion that
    failed unmutated — and the ledger fails it by name with "THE GAP IS CLOSED, DELETE THE
    DECLARATION".
    """
    if mutation is not None:
        # WHAT THE ATTEMPT SAID IS CARRIED VERBATIM and not summarised away. "the mutation changed
        # nothing — it names a file or a token that is not there" is the commonest sentence here and
        # it is the additive shape spelled out: the closing act would CREATE that file. A reader who
        # thinks the declaration is simply wrong can see the same sentence a finding would have
        # shown them; what changed is that it no longer contradicts a record it does not contradict.
        return finished(UNPROVABLE, mutation.describe(),
                        f"{GAP_IS_ADDITIVE} no verb here could do better — this gap closes by an "
                        f"addition and every verb subtracts. The attempt said: {attempted}. Its "
                        "recorded redness stands; what is unjudged is whether the gap is a deadline "
                        f"rather than a permanent red. Red unmutated: {_last_line(output)}",
                        unjudged=True)
    return finished(NO_MUTATION, "-",
                    f"{GAP_IS_ADDITIVE} a declared gap owes a mutation that makes it GREEN, none can "
                    "be derived from its text, and none can be declared either: the thing it names "
                    "is ABSENT and every verb here subtracts. Its recorded redness stands; what is "
                    "unjudged is whether the gap is a deadline rather than a permanent red. Red "
                    f"unmutated: {_last_line(output)}",
                    unjudged=True)


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


#: The sandbox's own path, which several `just` recipes echo as `cd "<root>"`. It is a fresh
#: temporary directory on every run, so leaving it in a recorded detail would make the inventory
#: churn on every `--record` for no change anyone made.
_SANDBOX_PATH = re.compile(r"/tmp/cy-\w+-\w+/tree")


def _first_line(output: str) -> str:
    for line in output.splitlines():
        if line.strip():
            return _SANDBOX_PATH.sub("<sandbox>", line.strip())[:160]
    return "(no output)"


def _last_line(output: str) -> str:
    """The LAST thing a check said, which is where a report that prints its legs puts the verdict."""
    for line in reversed(output.splitlines()):
        if line.strip():
            return _SANDBOX_PATH.sub("<sandbox>", line.strip())[:160]
    return "(no output)"


def _why_it_is_red(output: str) -> str:
    """Both ends of what a red criterion said, because one end is usually boilerplate.

    `m11a:world-budget-headless` was recorded as red with the detail
    `==> configure   profile=dev  configuration=Development  platform=linux` — the FIRST line a
    `just` recipe prints, and a line every build-backed criterion in the ladder prints whether it
    then measured anything or not. What that criterion actually said was
    `run-sample: no sample '10-world' in build/…/samples.`, three lines further down, and the record
    carried no trace of it. A reader of `falsifiability.toml` could not tell a criterion that ran and
    failed from one that never resolved its own command, which is the distinction this whole module
    exists to keep.

    So the recorded sentence is the first line AND the last, when they differ: the first is where a
    harness refusal lands and the last is where a check that ran puts its verdict.
    """
    first, last = _first_line(output), _last_line(_without_just_epilogue(output))
    return first if first == last else f"{first} … {last}"


#: `just`'s own last word when a recipe fails. It is not the check's verdict — it is the same
#: sentence for every failing recipe in the ladder — so it is stepped over when looking for what the
#: criterion actually said. Recognised narrowly: anything else `just` prints is kept.
_JUST_EPILOGUE = re.compile(r"^error: Recipe `[^`]+` failed with exit code \d+\s*$")


def _without_just_epilogue(output: str) -> str:
    return "\n".join(line for line in output.splitlines() if not _JUST_EPILOGUE.match(line.strip()))


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
    tree: WorkingTree | None = None
    if arguments.mutate_the_tree:
        # THE FLAG IS REFUSED WITHOUT A BUILD, rather than quietly ignored. Its whole subject is the
        # criteria a source-only sandbox cannot run, and without a tree to run them against it would
        # be a licence to mutate the repository in exchange for nothing.
        if not build_dir:
            print("falsify: --mutate-the-tree needs --build-dir: the criteria it exists for are the "
                  "ones a source-only sandbox cannot run at all", file=sys.stderr)
            return 2
        tree = WorkingTree()
        unavailable = tree.unavailable()
        if unavailable:
            print(f"falsify: --mutate-the-tree: {unavailable}", file=sys.stderr)
            return 2
        print("falsify: --mutate-the-tree — the repository itself is mutated, one criterion at a "
              "time, and restored before the next. What the restore has to reproduce is the tree as "
              "this run found it, and `git status` is asked after every one.")
        already = tree.modified_at_the_baseline()
        if already:
            print(f"falsify: {len(already)} tracked file(s) were already modified when this started. "
                  "They are the baseline, they are restored to the bytes they have now, and they are "
                  "the ones `git checkout` is never offered:")
            for line in already[:10]:
                print(f"    {line}")
    with tempfile.TemporaryDirectory(prefix="cy-falsify-", ignore_cleanup_errors=True) as directory:
        sandbox = Sandbox.materialise(Path(directory) / "tree")
        proofs = _prove_all(sandbox, tuple(arguments.milestone), arguments.only, build_dir, tree)
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
               build_dir: str = "", tree: WorkingTree | None = None) -> list[Proof]:
    proofs = []
    for identifier in (ledgers or criteria_module.available()):
        for criterion in criteria_module.load(identifier).criteria:
            if only and only not in criterion.id:
                continue
            proof = prove(sandbox, identifier, criterion, build_dir, tree)
            sandbox.restore()
            proofs.append(proof)
    return proofs


def _record(proofs: list[Proof], ledgers: tuple[str, ...], baseline: bool = False) -> list[str]:
    """Write what was observed. The unproven list only ever SHRINKS, and this is where that holds.

    A PROOF MAY ALWAYS BE RECORDED. An entry saying "this one has not been shown able to fail" may
    not, unless it is already there with the same digest. An existing red-in-the-tree proof can
    refresh its digest when a declared additive gap is still observed red in both trees. Otherwise
    `--record` would be the escape hatch that makes the whole mechanism advisory: write the
    criterion, run the recorder, and the eighth unfalsifiable criterion is on the ladder with the
    tooling's blessing. The one exception is
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
        # A declared additive gap can still be observed failing in both trees. If that
        # same criterion already had a red-in-the-tree proof, refresh its digest when
        # the check changes while keeping the proof's limited claim: it goes red.
        # This does not admit a new unproved criterion or claim the gap can close.
        if (proof.unjudged and standing_proof is not None
                and standing_proof.verdict == RED_IN_THE_TREE
                and GAP_IS_ADDITIVE in proof.detail):
            criterion = next(item for item in criteria_module.load(proof.ledger).criteria
                             if item.id == proof.criterion)
            tree_code, tree_output = run_in_the_repository(criterion, "", TREE_CONTROL_TIMEOUT_S)
            if tree_code > 0 and tree_code != 124:
                inventory.proofs[key] = Proof(
                    proof.ledger, proof.criterion, proof.digest, RED_IN_THE_TREE, "-",
                    "red unmutated, in the sandbox and in the repository "
                    f"(exit {tree_code}): {_last_line(tree_output)}", proof.seconds)
                continue
            refused.append(
                f"{proof.ledger}:{proof.criterion} is red in the sandbox, but the repository "
                f"control did not confirm it (exit {tree_code}: {_last_line(tree_output)})")
            continue
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
    additive = [proof for proof in carried if proof.detail.startswith(GAP_IS_ADDITIVE)]
    if additive:
        # SAID OUT LOUD, AND NOT AS A BUILD THAT IS MISSING. These are declared gaps whose closing
        # act is to WRITE something, and a prover whose every verb deletes cannot perform one. The
        # criterion is still red on every run of its ledger and its recorded redness still stands;
        # what nobody has shown is that the gap is a deadline rather than a permanent red.
        print(f"{len(additive)} declared gap(s) this prover cannot judge: each closes by an "
              "ADDITION and every mutation verb here subtracts. Their recorded redness stands; "
              "what is unjudged is whether they are deadlines rather than permanent reds:")
        for proof in additive:
            print(f"    {proof.ledger}:{proof.criterion}")
    rest = [proof for proof in carried if proof not in additive]
    if rest:
        print(f"{len(rest)} recorded proof(s) this run could not re-earn — it has no build tree. "
              f"Set {BUILD_DIR_VARIABLE} to one, or run "
              "`just roadmap-falsify prove --build-dir <dir>`:")
        for proof in rest[:10]:
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
    prove_command.add_argument(
        "--mutate-the-tree", action="store_true",
        help="for the criteria that PASS against --build-dir and can only be judged by a rebuild: "
             "apply the mutation to the WORKING TREE, let the criterion's own body rebuild over it, "
             "require RED, then restore and require GREEN again. Refuses a tree git calls dirty, "
             "restores on every exit path, and verifies with `git status` that it did")
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
    except TreeNotRestored as error:
        # NOT A VERDICT AND NOT A FINDING. `--mutate-the-tree` broke the repository and could not put
        # it back, so nothing this run would go on to say about falsifiability is worth reading.
        print(f"falsify: THE WORKING TREE IS STILL MUTATED.\n{error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())

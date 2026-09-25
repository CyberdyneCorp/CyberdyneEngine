#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Which criteria run a timing-sensitive suite, and whether ALL of it runs on a checked-quiet host.

M11.c's seventh close decided that a suite whose stall ceiling is a wall-clock assertion runs on a
quiet host, through `just test-quiet-host -- <command>` (tools/quiet-host/), rather than excusing a
loaded host's share of the wait from inside the case. The eighth close's gate then found the
premise enforced over HALF of one criterion: `m6:culling` was

    just test-quiet-host -- just test-unit -R unit.render_gpu_culling && just test-integration ...

and `criteria.py` runs a body with `bash -c`, so the `&&` is that shell's and the teardown suite ran
bare, after the wrapper had exited. `m1:four-profiles` ran `just test-all` bare in each of its four
matrix rows. Neither is visible from the criterion's text, which said MEASURED ON A QUIET HOST in
capitals both times. So the shape is checked rather than read:

  1. On a line that invokes `just test-quiet-host`, nothing may follow the wrapped command but a
     status guard, `|| exit <n>`, which runs nothing. A command after `&&`, `||`, `;`, `|` or `&`
     runs outside the wrapper. Several suites under one premise are ONE command:
     `just test-quiet-host -- just test-suites <kind>:<regex>...`.
  2. A command that runs one of `SUITES` — `just test-all`, `just test-<kind>` whose `-R` matches
     one (or that has no `-R` at all), `just test-suites`, `ctest -R` — is the wrapped command
     of a `just test-quiet-host` on its line, or it is a finding.
  3. A criterion that runs through the wrapper declares `needs = ["exclusive"]`: the premise is that
     nothing runs beside it, and the scheduler would otherwise be free to put something there.

`where = "ci"` criteria are not judged: they are never evaluated on a host this wrapper can read
(`three-platforms` is Windows and macOS as much as Linux, and `cy_quiet_host` is Linux-only).

NOT A SHELL PARSER. It tokenises one logical line at a time with `shlex` in punctuation mode, which
keeps quoted words whole and splits the control operators out; `schedule._decommented` has already
removed comments, heredocs and continuations. A body it cannot tokenise is a finding, so the check
errs towards a human reading the body, never towards a silent pass.
"""

from __future__ import annotations

import re
import shlex

import schedule as schedule_module

#: THE SUITES THE OWNER'S DECISION IS ABOUT, by kind: the ones whose flakes under the ledger's own
#: load it was taken over (M8.c's gate, M11.c's fourth to seventh closes), plus the harness's own.
#: Every harness suite carries a stall ceiling; widening this set is a decision, not a refactor.
SUITES: dict[str, tuple[str, ...]] = {
    "unit": ("unit.determinism", "unit.render_gpu_culling", "unit.harness"),
    "integration": ("integration.render_gpu_culling_teardown", "integration.harness"),
    "smoke": ("smoke.editor_window",),
}

WRAPPER = ("just", "test-quiet-host")
_OPERATOR = re.compile(r"^[;&|()]+$")
_EXIT_STATUS = re.compile(r"^\d+$")


def _tokens(line: str) -> list[str] | None:
    lexer = shlex.shlex(line, posix=True, punctuation_chars=";&|()")
    lexer.whitespace_split = True
    lexer.commenters = ""
    try:
        return list(lexer)
    except ValueError:
        return None


def _segments(tokens: list[str]) -> list[tuple[str, list[str]]]:
    """(the operator before it, its words) for each simple command on the line."""
    segments: list[tuple[str, list[str]]] = [("", [])]
    for token in tokens:
        if _OPERATOR.match(token):
            segments.append((token, []))
        else:
            segments[-1][1].append(token)
    return [(operator, words) for operator, words in segments if words or operator]


def _command(words: list[str]) -> list[str]:
    """The words from the command name on: keywords (`do`, `then`) and assignments dropped."""
    index = 0
    while index < len(words) and (words[index] in schedule_module._KEYWORDS
                                  or schedule_module._ASSIGNMENT.match(words[index])):
        index += 1
    return words[index:]


def _option(words: list[str], flags: tuple[str, ...] = ("-R", "--tests-regex")) -> str | None:
    """The value a command gives one of these flags (`-R x`, `-Rx`, `-R=x`), or None."""
    for index, word in enumerate(words):
        if word in flags and index + 1 < len(words):
            return words[index + 1]
        for flag in flags:
            if len(flag) == 2 and word.startswith(flag) and len(word) > 2:
                return word[2:].lstrip("=")
            if word.startswith(flag + "="):
                return word[len(flag) + 1:]
    return None


def _matching(kinds: tuple[str, ...], pattern: str | None) -> list[str]:
    names = [name for kind in kinds for name in SUITES.get(kind, ())]
    if pattern is None:
        return names
    try:
        expression = re.compile(pattern)
    except re.error:
        return names
    return [name for name in names if expression.search(name)]


def _ctest_suites(command: list[str]) -> list[str]:
    """`ctest -N` lists and runs nothing; `-L <label>` narrows the kinds as the recipes do."""
    if "-N" in command or "--show-only" in command:
        return []
    kinds = tuple(SUITES)
    label = _option(command, ("-L", "--label-regex"))
    if label is not None:
        try:
            kinds = tuple(kind for kind in SUITES if re.search(label, kind))
        except re.error:
            pass
    return _matching(kinds, _option(command))


def suites_run(command: list[str]) -> list[str]:
    """The timing-sensitive suites this one command runs, as far as its words say."""
    if not command:
        return []
    if command[0] == "ctest":
        return _ctest_suites(command)
    if command[0] != "just" or len(command) < 2:
        return []
    recipe = command[1]
    if recipe == "test-all":
        return _matching(tuple(SUITES), None)
    if recipe == "test-suites":
        found: list[str] = []
        for spec in command[2:]:
            kind, _, regex = spec.partition(":")
            if regex:
                found += _matching((kind,), regex)
        return found
    kind = recipe.removeprefix("test-")
    if recipe.startswith("test-") and kind in SUITES:
        return _matching((kind,), _option(command))
    return []


def _is_status_guard(rest: list[tuple[str, list[str]]]) -> bool:
    return (len(rest) == 1 and rest[0][0] == "||" and len(rest[0][1]) == 2
            and rest[0][1][0] == "exit" and bool(_EXIT_STATUS.match(rest[0][1][1])))


def _line_findings(line: str) -> tuple[list[str], bool]:
    """What is wrong with one logical line, and whether it runs the wrapper."""
    tokens = _tokens(line)
    if tokens is None:
        if "test-quiet-host" in line or "test-" in line:
            return [f"cannot tokenise `{line.strip()}`; a human has to read it"], False
        return [], False
    segments = _segments(tokens)
    findings: list[str] = []
    for index, (_operator, words) in enumerate(segments):
        command = _command(words)
        if tuple(command[:2]) == WRAPPER:
            rest = segments[index + 1:]
            if rest and not _is_status_guard(rest):
                outside = " ".join(" ".join([operator, *words]) for operator, words in rest)
                findings.append(
                    f"`{outside.strip()}` follows the wrapped command on its line, so the "
                    f"body's own shell runs it after `cy_quiet_host` has exited, outside the "
                    f"premise; run several suites as ONE wrapped command (`just test-suites`)")
            return findings, True
        named = suites_run(command)
        if named:
            findings.append(f"`{' '.join(command)}` runs {', '.join(named)} outside "
                            f"`just test-quiet-host`")
    return findings, False


def findings(criterion) -> list[str]:
    """Every way this criterion runs a timing-sensitive suite outside a checked-quiet host."""
    if criterion.kind not in ("recipe", "command") or criterion.where == "ci":
        return []
    body = schedule_module._decommented(criterion.run or "")
    found: list[str] = []
    wrapped = False
    for line in body.splitlines():
        if not line.strip():
            continue
        line_found, line_wrapped = _line_findings(line)
        found += line_found
        wrapped = wrapped or line_wrapped
    if wrapped and schedule_module.EXCLUSIVE not in (criterion.needs or ()):
        found.append("runs through `just test-quiet-host` without declaring "
                     "needs = [\"exclusive\"], so the scheduler may run another criterion "
                     "beside it")
    return found

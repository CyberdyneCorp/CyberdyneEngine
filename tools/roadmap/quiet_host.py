#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Whether a criterion that runs through `just test-quiet-host` keeps its whole line inside it.

WHAT THIS RULE NO LONGER CLAIMS. Until M11.c's ninth close this module was the guarantee that a
timing-sensitive suite never ran outside `cy_quiet_host` (tools/quiet-host/): it named six suites
and failed any body that ran one bare. Three gates in a row then found a route it could not see —
a `&&` after the wrapper (`m6:culling`), a matrix row's bare `test-all` (`four-profiles`), and a
`for t in ...; do just test-sanitize --tests "$t"` loop (`m3:sanitizers-render`, `m2:asan-world`)
— and a reading of a body's text can never see every route: loop variables, command
substitution, a recipe that runs ctest on its own. The owner's option B moved the premise into the
harness instead: `tests/harness/` enforces the wall-clock stall ceiling only when it verifies,
through /proc, that the run descends from a live `cy_quiet_host` that passed its pre-run check,
and anywhere else REPORTS a stall ("not enforced: not on a quiet host") without failing the case.
A suite run bare is therefore no longer a hidden hole in the premise — it is a run whose stalls
are honestly reported and not enforced — so the list of suites, and the finding for running one
bare, were removed rather than taught another route.

WHAT IS STILL TRUE, AND CHECKED:

  1. On a line that invokes `just test-quiet-host`, nothing may follow the wrapped command but a
     status guard, `|| exit <n>`, which runs nothing. A command after `&&`, `||`, `;`, `|` or `&`
     runs after `cy_quiet_host` has exited, and its stalls would silently be reported rather than
     enforced while the criterion's own text wraps it. Several suites under one premise are ONE
     command: `just test-quiet-host -- just test-suites <kind>:<regex>...`.
  2. A criterion that runs through the wrapper declares `needs = ["exclusive"]`: the premise is that
     nothing runs beside it, and the scheduler would otherwise be free to put something there
     (the wrapper would then refuse or fail the run rather than pass it, but the criterion should
     not be scheduled into a refusal).

`where = "ci"` criteria are not judged: they are never evaluated on a host this wrapper can read
(`three-platforms` is Windows and macOS as much as Linux, and `cy_quiet_host` is Linux-only).

NOT A SHELL PARSER. It tokenises one logical line at a time with `shlex` in punctuation mode, which
keeps quoted words whole and splits the control operators out; `schedule._decommented` has already
removed comments, heredocs and continuations. A wrapped line it cannot tokenise is a finding, so
the check errs towards a human reading the body, never towards a silent pass.
"""

from __future__ import annotations

import re
import shlex

import schedule as schedule_module

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


def _is_status_guard(rest: list[tuple[str, list[str]]]) -> bool:
    return (len(rest) == 1 and rest[0][0] == "||" and len(rest[0][1]) == 2
            and rest[0][1][0] == "exit" and bool(_EXIT_STATUS.match(rest[0][1][1])))


def _line_findings(line: str) -> tuple[list[str], bool]:
    """What is wrong with one logical line, and whether it runs the wrapper."""
    tokens = _tokens(line)
    if tokens is None:
        if "test-quiet-host" in line:
            return [f"cannot tokenise `{line.strip()}`; a human has to read it"], True
        return [], False
    segments = _segments(tokens)
    for index, (_operator, words) in enumerate(segments):
        if tuple(_command(words)[:2]) != WRAPPER:
            continue
        rest = segments[index + 1:]
        if rest and not _is_status_guard(rest):
            outside = " ".join(" ".join([operator, *words]) for operator, words in rest)
            return [f"`{outside.strip()}` follows the wrapped command on its line, so the body's "
                    f"own shell runs it after `cy_quiet_host` has exited and its stalls are "
                    f"reported, not enforced; run several suites as ONE wrapped command "
                    f"(`just test-suites`)"], True
        return [], True
    return [], False


def findings(criterion) -> list[str]:
    """Every way a criterion that runs through the wrapper lets part of its line escape it."""
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

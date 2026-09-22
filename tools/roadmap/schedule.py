# SPDX-License-Identifier: MIT
"""What one criterion needs to run, and the scheduler that runs independent ones at the same time.

THE LEDGER RAN ON ONE CORE OF TWENTY-FOUR. `criteria.evaluate` runs one criterion per
`subprocess.run` and `roadmap.command_milestone` called it in a plain loop, so M11.c's 440 distinct
checks were strictly sequential: a hundred and eleven of them finish in under a second and every one
of those waited behind a twenty-five-minute rebuild it has nothing to do with.

RUNNING THEM CONCURRENTLY IS NOT THE HARD PART; KNOWING WHICH ARE INDEPENDENT IS. Some criteria
configure and build a CMake tree, some run ctest in one, some drive Cargo, some need the one
graphics device or the one display, some bind a port, some write into the working tree. Two that
share any of those are not independent, and running them together produces a FLAKE — which in this
repository is strictly worse than being slow, because a flake is a verdict nobody can act on. Every
mechanism here is therefore biased one way: when in doubt, DO NOT run it concurrently.

So a criterion DECLARES WHAT IT NEEDS, as data, and the scheduler holds each declared resource
exclusively for as long as the criterion holds it.

  * `needs = ["build:@", "gpu"]` in the criterion's ledger entry is the declaration.
  * Where the criterion does not declare, `derive` reads the body — but only through facts it can
    see WITH CERTAINTY: the `just` recipes invoked (each one's needs are in `RECIPE_NEEDS`, a table
    checked against the justfile by `selftest.py`), the build-directory redirections it performs
    (each has to match a recognised idiom), and the criterion's own `requires`.
  * ANYTHING ELSE IS UNKNOWN, AND AN UNKNOWN CRITERION RUNS ALONE. A shell function, a command not
    in the read-only table, a heredoc fed to an interpreter, a redirection into the working tree, a
    build directory built out of a loop variable — each of those makes the body underivable, and an
    underivable body is a barrier that runs with nothing beside it. Guessing here is what produces
    the flake, so nothing here guesses.

THE ORDER OF RESULTS DOES NOT CHANGE. `run` returns one result per entry in the entries' own order,
whatever order they finished in, and reports each one through `report` in that same order too: the
ledger is read by eye against a previous run, so parallel execution that reprinted the same
criteria in completion order would be a regression even with identical verdicts.

Governed by: delivery-roadmap (Milestone exit criteria are executable), developer-workflow-and-just.
"""

from __future__ import annotations

import os
import re
from concurrent.futures import FIRST_COMPLETED, Future, ThreadPoolExecutor, wait
from dataclasses import dataclass

#: A criterion whose needs could not be derived and were not declared. It runs alone: nothing else
#: starts while it runs, and it does not start until everything running has finished.
EXCLUSIVE = "exclusive"

#: The classes of resource a criterion can hold. A token is a class, or `class:instance` where two
#: instances of the same class are genuinely separate things — two CMake build trees, say.
RESOURCE_CLASSES = ("build", "cargo", "gpu", "display", "net", "tree")

#: The build tree the ledger itself was pointed at — `CY_BUILD_DIR`, or the default. A body that
#: redirects `CY_BUILD_DIR` to a named subdirectory gets `build:<that name>` instead and is
#: therefore independent of this one.
DEFAULT_BUILD = "build:@"
#: `just test-sanitize` configures a tree of its own under `${CY_BUILD_DIR:-build}/sanitize-<san>`.
#: One token for all sanitizers: telling them apart would buy two criteria and cost a guess.
SANITIZE_BUILD = "build:sanitize"

TOKEN = re.compile(r"^(?:%s)(?::[A-Za-z0-9._@/+-]+)?$" % "|".join(RESOURCE_CLASSES))

DEFAULT_JOBS = 8


class ScheduleError(Exception):
    """A declaration the scheduler cannot honour."""


# --- What each recipe and program needs -------------------------------------------------------------
#
# These two tables are the whole of what `derive` knows. A recipe or program that is not in them
# makes the body underivable — which is safe, and is why adding a row is a deliberate act: a wrong
# row here is exactly the flake this module exists to prevent. `selftest.py` checks every name in
# RECIPE_NEEDS against the recipes the justfile actually declares, so a renamed recipe cannot leave
# a stale row behind that silently stops matching.

#: `just <recipe>` → the resources it holds. `EXCLUSIVE` means the recipe can reach anything.
RECIPE_NEEDS: dict[str, tuple[str, ...]] = {
    # Read the tree and nothing else.
    "env-doctor": (), "env-targets": (), "env-overrides": (),
    "roadmap-status": (), "roadmap-gates": (),
    "quality-specs": (), "quality-layers": (), "quality-format-check": (), "quality-abi": (),
    "quality-editor-contract": (), "quality-requirements": (), "quality-docs": (),
    "quality-licence": (), "quality-spelling": (), "quality-gates-selftest": (),
    "quality-swift-format": (),
    "maintenance-deps-check": (), "maintenance-deps-test": (),
    "diagnose-trace": (), "release-version": (), "release-changelog": (),
    "_editor-target-dir": (), "_cargo-profile": (), "_host-platform": (),
    "_host-compiler": (), "_host-architecture": (), "_split": (),
    "_report-override": (), "_report-overrides": (), "_profile-column": (),

    # Configure, build or test one CMake tree. Every test recipe builds first — see just/test.just —
    # so none of them is a mere reader of a tree somebody else owns.
    "build-engine": (DEFAULT_BUILD,), "build-tools": (DEFAULT_BUILD,),
    "build-shaders": (DEFAULT_BUILD,),
    "test-unit": (DEFAULT_BUILD,), "test-integration": (DEFAULT_BUILD,),
    "test-smoke": (DEFAULT_BUILD,), "test-render": (DEFAULT_BUILD,),
    "test-all": (DEFAULT_BUILD,), "test-determinism": (DEFAULT_BUILD,),
    "test-bench": (DEFAULT_BUILD,), "test-bench-jobs": (DEFAULT_BUILD,),
    "quality-lint": (DEFAULT_BUILD,), "quality-identity": (DEFAULT_BUILD,),
    "generate-check": (DEFAULT_BUILD,), "generate-test": (DEFAULT_BUILD,),
    "run-sample": (DEFAULT_BUILD,), "run-headless": (DEFAULT_BUILD,),
    "run-fidelity": (DEFAULT_BUILD,), "run-open-world": (DEFAULT_BUILD,),
    "run-ship": (DEFAULT_BUILD,), "run-vertical-slice": (DEFAULT_BUILD,),
    "build-all": (DEFAULT_BUILD, "cargo"),

    # A sanitized build is not interchangeable with an ordinary one and never reuses build/<profile>.
    "test-sanitize": (SANITIZE_BUILD,),

    # Cargo, which serialises on its own target directory anyway; one token keeps the two target
    # directories in the corpus — `_editor-target-dir` and the workspace default — from racing.
    "build-editor": ("cargo",), "build-editor-check": ("cargo",),
    "build-editor-format": ("cargo",), "build-editor-toolchain": ("cargo",),
    "run-editor-session": ("cargo", DEFAULT_BUILD),
    "run-agent-authoring": ("cargo", DEFAULT_BUILD),
    "run-authoring": ("cargo", DEFAULT_BUILD),
    "run-editor-runtime": ("cargo", DEFAULT_BUILD),
    "run-editor": ("cargo", "display"),
    "run-editor-window": ("cargo", DEFAULT_BUILD, "display"),

    # Binds a port as well as building.
    "run-multiplayer": (DEFAULT_BUILD, "net"),

    # Writes generated sources back into the tree, or runs recipes this table cannot see through.
    "generate-headers": (EXCLUSIVE,), "generate-swift": (EXCLUSIVE,),
    "maintenance-deps": (EXCLUSIVE,), "maintenance-deps-rust": (EXCLUSIVE,),
    "maintenance-clean": (EXCLUSIVE,), "build-clean": (EXCLUSIVE,),
    "quality-format": (EXCLUSIVE,), "build-reap": (EXCLUSIVE,),
    "ci-check": (EXCLUSIVE,), "ci-jobs": (EXCLUSIVE,),
    "roadmap-milestone": (EXCLUSIVE,), "roadmap-falsify": (EXCLUSIVE,),
    "content-cook": (EXCLUSIVE,), "content-import": (EXCLUSIVE,),
    "content-validate": (EXCLUSIVE,), "content-audit": (EXCLUSIVE,),
    "content-package": (EXCLUSIVE,), "content-patch": (EXCLUSIVE,),
    # `roadmap-debts` REWRITES docs/roadmap/open-debts.md every time it is run — found the way
    # everything in this module was found, by running it and then reading `git status`. And
    # `roadmap-test` is the whole prover: it materialises sandboxes, builds in them, and runs
    # criterion bodies in the repository itself, so there is no tree it can be said not to touch.
    "roadmap-debts": (EXCLUSIVE,), "roadmap-test": (EXCLUSIVE,),
}

#: AN ARGUMENT CAN TURN A READER INTO A WRITER, and a table keyed on the recipe name alone cannot
#: see it. `just roadmap-status` reads the record and `just roadmap-status --write-lists` rewrites
#: the capability matrix; `just quality-abi` checks the ABI baseline and `--update` replaces it;
#: `just test-bench --record` rewrites the committed thresholds. An invocation carrying one of these
#: is not the recipe this table classified, so the body is underivable and the criterion runs alone.
WRITING_ARGUMENTS: dict[str, frozenset[str]] = {
    "roadmap-status": frozenset({"--write-lists"}),
    "quality-abi": frozenset({"--update"}),
    "test-bench": frozenset({"--record"}),
    "test-bench-jobs": frozenset({"--record"}),
}

#: Programs a body may invoke directly. A program NOT here makes the body underivable; a program
#: here holds exactly what it is mapped to. The shell builtins and text tools hold nothing because
#: they only read the tree and write to their own stdout.
PROGRAM_NEEDS: dict[str, tuple[str, ...]] = {
    name: () for name in (
        "echo", "printf", "exit", "return", "continue", "break", "true", "false", "test", "[",
        "set", "shift", "read", "cd", "pwd", "local", "declare", "export", "unset", "trap",
        "cat", "head", "tail", "grep", "egrep", "fgrep", "rg", "sed", "awk", "cut", "tr", "sort",
        "uniq", "wc", "comm", "diff", "cmp", "join", "paste", "rev", "nl", "od", "xxd", "strings",
        "find", "ls", "basename", "dirname", "realpath", "readlink", "stat", "file",
        "sha256sum", "shasum", "md5sum", "cksum", "date", "seq", "sleep", "which",
        "jq", "mktemp",
    )
}
PROGRAM_NEEDS.update({
    "ctest": (DEFAULT_BUILD,),
    "cmake": (DEFAULT_BUILD,),
    "ninja": (DEFAULT_BUILD,),
    "cargo": ("cargo",),
})

#: `python3 <script>` → the script's needs. An interpreter with no script path — `python3 -`, or a
#: heredoc — is arbitrary code and makes the body underivable, which is the point: a heredoc is
#: where a criterion writes whatever it likes.
SCRIPT_NEEDS: dict[str, tuple[str, ...]] = {
    "tools/roadmap/row_evidence.py": (),
    "tools/roadmap/roadmap.py": (),
    "tools/roadmap/debts.py": (),
    "tools/roadmap/selftest.py": (),
    "tools/editor/play_contract.py": (),
    "tools/editor/selftest.py": (),
    "tools/ci/cross_leg_audit.py": (),
    "tools/ci/check_workflows.py": (),
    "tools/layercheck/layercheck.py": (),
    "tools/abi/selftest.py": (),
}

#: Programs that write where they are pointed. A criterion invoking one of these is underivable —
#: not because writing is forbidden, but because WHERE it writes is an argument, and an argument is
#: exactly what this module refuses to guess at. `rm -rf "$d"` on the criterion's own build tree is
#: harmless and `rm -rf docs/` is not, and nothing here can tell those apart with certainty.
#:
#: `xargs`, `env`, `command` and `git` are absent from PROGRAM_NEEDS for the same reason one level
#: up: each runs a program named in an argument, so admitting them would admit everything.
TREE_WRITERS = ("rm", "mkdir", "rmdir", "cp", "mv", "touch", "install", "chmod", "ln", "truncate",
                "tee", "dd", "patch", "sponge")


# --- Reading a body ---------------------------------------------------------------------------------

_HEREDOC = re.compile(r"<<-?\s*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\1(.*?)^\s*\2\s*$",
                      re.DOTALL | re.MULTILINE)
_CONTINUATION = re.compile(r"\\\n")
_COMMENT = re.compile(r"(?m)(?:^|(?<=\s))#[^\n]*")
_STRING = re.compile(r"'[^']*'|\"(?:[^\"\\]|\\.)*\"")
_SPLIT = re.compile(r"\|\||&&|;;|[;\n|&()`]|\$\(|\{|\}")
_ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*\+?=")
_REDIRECTION = re.compile(r"^\d*[<>]")
#: `2>&1` and `&>log`. Removed before splitting, because `&` is a separator and `2>&1` left behind a
#: fragment whose only word was `1`.
_FD_REDIRECT = re.compile(r"\d*>&\d*|&>>?")
#: A loop or case HEADER is not a command: `for t in ecs scene` names a variable and a word list.
_COMPOUND_HEADERS = frozenset({"for", "select", "case"})
_KEYWORDS = frozenset({
    "if", "then", "else", "elif", "fi", "for", "do", "done", "while", "until", "case", "esac",
    "in", "!", "time", "exec", "eval", "function", "select", "[[", "]]", "&&", "||", "coproc",
})

_HEREDOC_MARK = "\x00heredoc\x00"
_STRING_MARK = "\x00string\x00"

#: `> <target>` where the target is neither absolute nor a variable is a write into the working
#: tree: a criterion's body runs with the repository root as its working directory. Read against the
#: body with its STRINGS INTACT, because `> "docs/out.png"` is a tree write and `> "$work/out.png"`
#: is not, and masking the strings would make the two indistinguishable.
_TREE_REDIRECT = re.compile(r"""(?<![0-9<>])>>?\s*["']?(?!/|\$|&)([A-Za-z0-9_.][^\s;|&)"']*)""")

#: `d="${CY_BUILD_DIR:-build/dev}"` and friends. The whole point is that these are the ONLY shapes
#: accepted: a redirection built out of a loop variable — `CY_BUILD_DIR="$base/agree-$p"` — matches
#: none of them and makes the body underivable, which is correct, because it is several trees.
_BUILD_DIR_IDIOMS = (
    # ${CY_BUILD_DIR:+${CY_BUILD_DIR}/<literal>} — the ledger's tree with a named subdirectory.
    (re.compile(r"\$\{CY_BUILD_DIR:\+\$\{?CY_BUILD_DIR\}?/([A-Za-z0-9._-]+)\}"), "suffix"),
    # ${CY_BUILD_DIR:-<anything>}/<literal> — likewise.
    (re.compile(r"\$\{CY_BUILD_DIR:-[^}]*\}/([A-Za-z0-9._-]+)"), "suffix"),
    # ${CY_BUILD_DIR:-<anything>} on its own, and a plain expansion: the ledger's own tree.
    (re.compile(r"\$\{CY_BUILD_DIR:?[-+][^}]*\}"), "default"),
    (re.compile(r"\$\{?CY_BUILD_DIR\}?"), "default"),
)
#: `CY_BUILD_DIR="$d"` — handing a tree this function has already resolved to the recipe that uses
#: it. `CY_BUILD_DIR="$base/agree-$p"` is deliberately NOT this shape: it builds a path out of a loop
#: variable, which is several trees, so a body written that way is underivable and runs alone.
#: `m9:multiplayer-profiles-agree` was written that way; it now builds through
#: `tools/roadmap/matrix.py` and DECLARES the two matrix rows it holds instead.
_HANDS_ON_A_TREE = re.compile(
    r"""CY_BUILD_DIR=(?:"\$\{?[A-Za-z_][A-Za-z0-9_]*\}?"|\$\{?[A-Za-z_][A-Za-z0-9_]*\}?)""")
_CARGO_TARGET_DIR = re.compile(r"CARGO_TARGET_DIR")


def _unquote(match: re.Match) -> str:
    """A quoted string is prose — UNLESS IT SUBSTITUTES A COMMAND, and then it is a command.

    `out="$(cargo test ...)"` is how four of this repository's criteria invoke Cargo, and replacing
    it with a marker the way `echo "..."` is replaced would hide the one invocation that decides
    whether the criterion may run beside another Cargo criterion. So a string carrying `$(` or a
    backtick keeps its contents and loses only its quotes; everything else becomes the marker.
    """
    text = match.group(0)
    if "$(" in text or "`" in text:
        return f" {text[1:-1]} "
    # NO SPACES AROUND THE MARKER. `d="${CY_BUILD_DIR:-build/dev}"` is ONE word, an assignment, and
    # spacing the marker off split it into `d=` and a bare marker that read as a command name — which
    # made eighty-seven perfectly ordinary bodies underivable, every one of them for a reason that
    # was an artefact of this substitution rather than anything in the criterion.
    return _STRING_MARK


def _decommented(body: str) -> str:
    """The body with only its comments, heredocs and line continuations gone.

    `build_trees` needs this rather than `_strip`: a build directory lives inside a quoted string,
    so masking the strings is exactly what hides it.
    """
    body = _CONTINUATION.sub(" ", body)
    body = _HEREDOC.sub(f" {_HEREDOC_MARK} ", body)
    return _COMMENT.sub(" ", body)


def _strip(body: str) -> str:
    """The body with everything an analyser must not read as a command removed.

    Heredocs, comments and quoted prose all contain words and regular expressions that look like
    command names. They are replaced by a marker rather than deleted so that their PRESENCE is still
    visible: a heredoc is what makes a body underivable.
    """
    body = _FD_REDIRECT.sub(" ", _decommented(body))
    return _STRING.sub(_unquote, body)


def commands(body: str) -> list[list[str]]:
    """Every command in the body, as its own word list, in order.

    Not a shell parser and not pretending to be one: it splits on the separators, drops leading
    keywords, assignments and redirections, and takes what is left. A word it gets wrong is a word
    that is not in the tables, and a word not in the tables makes the body underivable — so the
    failure mode of this function is 'runs alone', never 'runs beside something it shares a tree
    with'.
    """
    found: list[list[str]] = []
    for fragment in _SPLIT.split(_strip(body)):
        words = [word for word in fragment.strip().split()
                 if word and not _REDIRECTION.match(word)]
        if words and words[0] in _COMPOUND_HEADERS:
            continue
        index = 0
        while index < len(words) and (words[index] in _KEYWORDS
                                      or _ASSIGNMENT.match(words[index])):
            index += 1
        if index < len(words):
            found.append(words[index:])
    return found


def _writes_into_the_tree(body: str) -> bool:
    if _TREE_REDIRECT.search(_decommented(body)):
        return True
    return any(command[0] in TREE_WRITERS for command in commands(body))


def build_trees(body: str) -> tuple[str, ...] | None:
    """Which build trees the body names, or None when it names one this module cannot resolve.

    It reads the body with its QUOTED STRINGS INTACT, unlike everything else here, because a build
    directory is almost always written inside them — `d="${CY_BUILD_DIR:+${CY_BUILD_DIR}/off-ml}"`
    is one word — and reading it with the strings masked out found no redirection at all in the
    fourteen bodies that have one.

    The rule is subtractive, which is what makes it safe: every recognised shape is REMOVED from the
    text, and if the name `CY_BUILD_DIR` still appears anywhere afterwards then this body does
    something with it that these idioms do not describe, and the body is not derivable.
    """
    text = _decommented(body)
    if "CY_BUILD_DIR" not in text:
        return ()
    trees: set[str] = set()

    def take(match: re.Match, kind: str) -> str:
        trees.add(f"build:{match.group(1)}" if kind == "suffix" else DEFAULT_BUILD)
        return " "

    for pattern, kind in _BUILD_DIR_IDIOMS:
        text = pattern.sub(lambda match, kind=kind: take(match, kind), text)
    text = _HANDS_ON_A_TREE.sub(" ", text)
    return None if "CY_BUILD_DIR" in text else tuple(sorted(trees))


def _resolve(needs: tuple[str, ...], trees: tuple[str, ...]) -> tuple[str, ...]:
    """Point every `build:@` in a recipe's needs at the trees this body actually named."""
    resolved: list[str] = []
    for need in needs:
        if need != DEFAULT_BUILD:
            resolved.append(need)
        else:
            resolved.extend(trees or (DEFAULT_BUILD,))
    return tuple(resolved)


def _body_needs(body: str) -> tuple[str, ...] | None:
    """What the body holds, or None when any part of it is beyond these tables."""
    trees = build_trees(body)
    if trees is None or _writes_into_the_tree(body) or _HEREDOC_MARK in _strip(body):
        return None
    needs: set[str] = set()
    for command in commands(body):
        one = _word_needs(command[0], command[1:])
        if one is None:
            return None
        needs.update(_resolve(one, trees))
    if _CARGO_TARGET_DIR.search(_decommented(body)):
        needs.add("cargo")
    return tuple(sorted(needs))


def _word_needs(word: str, rest: list[str]) -> tuple[str, ...] | None:
    if word == "just":
        if not rest:
            return None
        if WRITING_ARGUMENTS.get(rest[0], frozenset()).intersection(rest[1:]):
            return None
        return RECIPE_NEEDS.get(rest[0])
    if word in ("python3", "python"):
        return SCRIPT_NEEDS.get(rest[0]) if rest else None
    return PROGRAM_NEEDS.get(word)


# --- The declaration --------------------------------------------------------------------------------


def check_declaration(declared, where: str) -> None:
    """`needs` is a list of resource tokens this scheduler knows, or it is not a declaration."""
    if not isinstance(declared, list):
        raise ScheduleError(f"{where}: 'needs' is a list of resource tokens, not "
                            f"{type(declared).__name__}")
    seen: set[str] = set()
    for token in declared:
        if not isinstance(token, str) or not token.strip():
            raise ScheduleError(f"{where}: 'needs' holds resource tokens, not {token!r}")
        if token != EXCLUSIVE and not TOKEN.match(token):
            raise ScheduleError(
                f"{where}: 'needs' names {token!r}, which is not {EXCLUSIVE!r} nor one of "
                f"{', '.join(RESOURCE_CLASSES)} (optionally as '<class>:<instance>')")
        if token in seen:
            raise ScheduleError(f"{where}: 'needs' names {token!r} twice")
        seen.add(token)


#: There is one graphics device and one window system on a machine, and `requires` is where a
#: criterion already says it wants them. Nothing else in the ledger declares either.
_DEVICE = {"gpu": ("gpu",), "display": ("display",)}


def derive(criterion) -> tuple[str, ...] | None:
    """What the criterion's own text shows it needs, or None when the text does not show it.

    A `path` criterion globs the tree and a `tiers` criterion compares a record already in memory:
    neither runs a subprocess, so neither holds anything. Everything else is its body, plus the
    device its `requires` names.
    """
    if criterion.kind in ("path", "tiers"):
        body: tuple[str, ...] | None = ()
    else:
        body = _body_needs(criterion.run)
        if body is None:
            return None
    return tuple(sorted(set(body) | set(_DEVICE.get(criterion.requires, ()))))


def needs(criterion) -> tuple[str, ...]:
    """The resources this criterion holds while it runs. `(EXCLUSIVE,)` means it runs alone.

    A declaration in the ledger wins over derivation, always: derivation exists so that 435 criteria
    need not be annotated by hand, not so that it can overrule somebody who knows better.
    """
    declared = tuple(getattr(criterion, "needs", ()) or ())
    resolved = declared if declared else derive(criterion)
    if resolved is None or EXCLUSIVE in resolved:
        # `exclusive` beside anything else says nothing more than `exclusive`: it already means that
        # nothing runs alongside. Normalising it here keeps one shape for every caller to read.
        return (EXCLUSIVE,)
    return resolved


def independent(left, right) -> bool:
    """Whether these two criteria may run at the same time."""
    one, other = set(needs(left)), set(needs(right))
    if EXCLUSIVE in one or EXCLUSIVE in other:
        return False
    return not (one & other)


# --- The scheduler ----------------------------------------------------------------------------------


@dataclass
class _Slot:
    index: int
    entry: object
    tokens: frozenset[str]
    exclusive: bool


def default_jobs() -> int:
    """How many criteria run at once unless told otherwise. ONE, and the default is the finding.

    Concurrency is OPT-IN because running two criteria at once CHANGED VERDICTS, which is the one
    outcome a speedup may not have. M11.c's verification run, at `min(8, cores)`, reported five
    failures the sequential run before it did not have: `m2:nodes`, `m7:arbiter`, `m7:sky`,
    `m8b:animation` and `m8b:navigation`. Every one of them exits 0 when re-run alone.

    THE CAUSE IS NOT FLAKINESS, IT IS A WATCHDOG MEASURING THE WRONG THING. `tests/harness/src/
    budget.cpp`'s `stalled:` check compares WALL CLOCK against a CPU-derived ceiling, and it
    subtracts time spent waiting for a core — but not time spent waiting for I/O or a page fault.
    So a case that spent 0.702 ms of CPU and 0.000 ms on the runqueue was failed for holding
    2053.091 ms of wall clock against a 227.106 ms ceiling, because the ledger's own scheduler was
    compiling beside it. Under `--jobs 1` that case passes.

    So this default returns to 1 until the harness subtracts I/O and page-fault waiting the way it
    already subtracts runqueue contention, or the scheduler refuses to co-schedule a budget-
    sensitive suite with a build. `--jobs n` and `CY_LEDGER_JOBS` still work for anyone who wants
    the pool; what changed is that nobody gets it without asking, because a ledger that reports a
    failure the tree does not have is worse than a slow one.
    """
    override = os.environ.get("CY_LEDGER_JOBS")
    if override and override.strip().isdigit() and int(override) > 0:
        return int(override)
    return 1


def _slots(entries) -> list[_Slot]:
    slots = []
    for index, entry in enumerate(entries):
        tokens = needs(entry.criterion)
        slots.append(_Slot(index, entry, frozenset(tokens) - {EXCLUSIVE},
                           EXCLUSIVE in tokens))
    return slots


def _startable(waiting: list[_Slot], held: set[str], running: int, jobs: int) -> list[_Slot]:
    """The waiting criteria that may start now, in ledger order.

    AN UNKNOWN CRITERION IS A BARRIER, and that is what stops it starving. Scanning past a blocked
    criterion is what keeps the pool busy, but a criterion that runs alone would never reach the
    front of a pool that is always busy — so once it is the first thing waiting that cannot start,
    nothing behind it starts either, and it goes as soon as the pool drains.
    """
    chosen: list[_Slot] = []
    claimed = set(held)
    for slot in waiting:
        if running + len(chosen) >= jobs:
            break
        if slot.exclusive:
            if running or chosen:
                break
            return [slot]
        if slot.tokens & claimed:
            continue
        claimed |= slot.tokens
        chosen.append(slot)
    return chosen


def run(entries, evaluate, jobs: int, started=None, finished=None) -> list:
    """Evaluate every entry, honouring what each one needs, and return the results IN ENTRY ORDER.

    `evaluate(entry)` returns that entry's result. `started(entry)` and `finished(index, entry,
    result)` are notifications: `finished` is called in COMPLETION order and is where a caller
    releases whatever it wants to report as soon as it can, while the returned list is always in
    entry order. See `roadmap._evaluate_plan` for the reporting rule that depends on both.
    """
    slots = _slots(entries)
    results: list = [None] * len(slots)
    if jobs <= 1:
        return _run_in_one_thread(slots, evaluate, results, started, finished)

    waiting, held, live = list(slots), set(), {}
    with ThreadPoolExecutor(max_workers=jobs, thread_name_prefix="ledger") as pool:
        while waiting or live:
            for slot in _startable(waiting, held, len(live), jobs):
                waiting.remove(slot)
                held |= slot.tokens
                if started:
                    started(slot.entry)
                live[pool.submit(evaluate, slot.entry)] = slot
            future = _next_done(live)
            slot = live.pop(future)
            held -= slot.tokens
            results[slot.index] = future.result()
            if finished:
                finished(slot.index, slot.entry, results[slot.index])
    return results


def _next_done(live: dict[Future, _Slot]) -> Future:
    done, _ = wait(list(live), return_when=FIRST_COMPLETED)
    return next(iter(done))


def _run_in_one_thread(slots, evaluate, results, started, finished) -> list:
    """`--jobs 1`: the loop this module replaced, kept verbatim so a run can be compared against it."""
    for slot in slots:
        if started:
            started(slot.entry)
        results[slot.index] = evaluate(slot.entry)
        if finished:
            finished(slot.index, slot.entry, results[slot.index])
    return results

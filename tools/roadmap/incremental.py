# SPDX-License-Identifier: MIT
"""Incremental ledger closes: which criteria a change since a green ledger can have moved.

M11.d. A full `just roadmap-milestone <rung>` evaluates every criterion of the permanent set plus the
rung's own — over 470 at M11.d, hours at a close — and most of them read nothing the change
under review touched. `--incremental` evaluates three sets instead, and prints which criterion fell in
which and why:

  1. the rung's OWN criteria, always;
  2. every earlier criterion that is NEW or EDITED since the base commit (its falsifiability digest
     moved, or it was not in the base's plan at all), or whose INPUTS a changed file belongs to;
  3. a fixed SMOKE set — build, format, lint, test-all — always, because a broken build or a broken
     suite anywhere is not something an input set can rule out.

**FAIL SAFE IS THE RULE THIS MODULE IS BUILT AROUND.** A criterion is skipped only when its inputs are
KNOWN and none of them changed. A criterion whose inputs cannot be read — a multi-line shell body, a
recipe that declares nothing, a test that runs a Python script, a build tree that is not current — is
selected, and the reason is printed. There is no heuristic that guesses a criterion is unaffected.

What "known" means, per kind, all of it read from data the ledger or the build already has:

  tiers     the status record and its parser.
  path      the criterion's own glob: a changed path matching it, or under a directory matching it.
  recipe /  only a body that is a chain of `just <recipe>` invocations joined by `&&`, where every
  command   recipe is a TEST recipe given `-R <regex>` (or `test-suites kind:regex`, or either behind
            `test-quiet-host --`). Its inputs are then:
              - the files the selected tests' executables are BUILT FROM: `ninja -t inputs` for the
                objects and sources, and `ninja -t deps` for every header each object included;
              - every repository path those tests NAME at run time: the compile definitions of their
                objects, their command line and environment, and their working directory when it is
                in the source tree — a path naming a build output adds that output's own inputs;
              - the CMake files of every directory above an input, `cmake/` and `CMakePresets.json`;
              - the recipe text itself: `justfile`, `just/*.just`, and the directory of every
                repository path the recipe's closure names (`just --show`, followed transitively).
            Any other body is unknown and selected.

Every criterion also reads `tools/roadmap/criteria.py`, the evaluator, and the criterion's own ledger
entry — the second is the digest check in set 2. A compile definition naming the repository ROOT is
a read of anything, unless `incremental.toml` carries a reviewed exemption for it, pinned by a digest
of the files that use it (`exempt_definitions`).

**THE FULL LEDGER STAYS THE DEFAULT AND STAYS THE CLOSE.** `just roadmap-milestone <rung>` is
unchanged; this mode is an explicit flag. The full flattened ledger runs nightly and at M11.e, and a
green full run is what records the commit an incremental run defaults to comparing against — an
incremental run never records one, so the baseline is always a full ledger's.

Governed by: delivery-roadmap (Milestone exit criteria are executable), testing-and-quality.
"""

from __future__ import annotations

import datetime
import hashlib
import json
import os
import re
import shlex
import subprocess
import tempfile
import tomllib
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

import criteria as criteria_module
import gates as gates_module
from record import REPO_ROOT

#: The fixed smoke set, by ledger label. Always evaluated, and a plan missing one is refused rather
#: than run without it: an incremental close with no build or no test-all would be a close of nothing.
SMOKE = ("m0:build", "m0:format", "m0:lint", "m0:test")

#: The recipes whose selection this module can read, and the ctest label each one runs.
TEST_RECIPES = {"test-unit": "unit", "test-integration": "integration", "test-smoke": "smoke",
                "test-determinism": "determinism", "test-render": "render"}
SUITE_KINDS = frozenset(TEST_RECIPES.values())
QUIET_HOST = "test-quiet-host"

EVALUATOR = ("tools/roadmap/criteria.py",)
RECORD_INPUTS = ("docs/roadmap/status.yaml", "tools/roadmap/record.py")
JUST_FILES = ("justfile", "just/*.just")
CMAKE_EVERYWHERE = ("cmake", "CMakePresets.json")

#: Why a criterion is in the incremental run. Printed, and in the JSON document.
OWN = "own"
SMOKE_SET = "smoke"
EDITED = "new or edited"
CHANGED = "inputs changed"
UNKNOWN = "inputs unknown"
UNCHANGED = "inputs unchanged"

#: A body is read only when it is ONE line of words and `&&`. Anything a shell would expand,
#: redirect, pipe or substitute makes what the body runs a question this reader does not answer.
_UNREADABLE_BODY = re.compile(r"[\n;|&`<>*?\[\]#\\]|\$[({A-Za-z_0-9]")
_NO_WORK = "no work to do"

#: The reviewed exemptions for compile definitions that name the repository root. See
#: `exempt_definitions`.
EXEMPTIONS_FILE = Path(__file__).resolve().parent / "incremental.toml"
EXEMPT = "exempt"


class IncrementalError(Exception):
    """An incremental run that cannot be set up as asked: no baseline, or a commit that is not one."""


# --- What a criterion reads -----------------------------------------------------------------------


@dataclass(frozen=True)
class Inputs:
    """What a criterion reads, as repository paths — or the reason nobody can say.

    `paths` are repository-relative files OR directories; a changed path matches one when it is that
    path or lies under it. `cmake_dirs` are the directories whose CMake files configure an input.
    `account` says where the answer came from, so that a skipped criterion's line can be checked.
    """

    paths: frozenset[str] = frozenset()
    globs: tuple[str, ...] = ()
    cmake_dirs: frozenset[str] = frozenset()
    account: tuple[str, ...] = ()
    unknown: str = ""

    @staticmethod
    def unknowable(reason: str) -> Inputs:
        return Inputs(unknown=reason)

    def union(self, other: Inputs) -> Inputs:
        return Inputs(paths=self.paths | other.paths,
                      globs=tuple(dict.fromkeys(self.globs + other.globs)),
                      cmake_dirs=self.cmake_dirs | other.cmake_dirs,
                      account=tuple(dict.fromkeys(self.account + other.account)),
                      unknown=self.unknown or other.unknown)

    def matches(self, changed: Iterable[str]) -> list[str]:
        return [path for path in changed if self._matches(path)]

    def _matches(self, path: str) -> bool:
        if any(path == known or path.startswith(known + "/") for known in self.paths):
            return True
        if any(_glob_matches(path, pattern) for pattern in self.globs):
            return True
        return _is_cmake_file(path) and _parent(path) in self.cmake_dirs


def _glob_matches(path: str, pattern: str) -> bool:
    """A path criterion is satisfied by a FILE or a DIRECTORY matching its glob, so a change to a file
    under a matching directory is a change to what it reads."""
    candidate = PurePosixPath(path)
    return any(ancestor.full_match(pattern) for ancestor in (candidate, *candidate.parents)
               if str(ancestor) != ".")


def _is_cmake_file(path: str) -> bool:
    name = PurePosixPath(path).name
    return name == "CMakeLists.txt" or name.endswith(".cmake")


def _parent(path: str) -> str:
    parent = str(PurePosixPath(path).parent)
    return "" if parent == "." else parent


def _ancestors(path: str) -> set[str]:
    """Every directory above a repository path, the root spelled as the empty string."""
    return {"" if str(parent) == "." else str(parent) for parent in PurePosixPath(path).parents}


def _with_cmake(paths: Iterable[str]) -> Inputs:
    """Source files, and the CMake files that decide how they are built."""
    files = frozenset(paths)
    directories: set[str] = set()
    for path in files:
        directories |= _ancestors(path)
    return Inputs(paths=files | frozenset(CMAKE_EVERYWHERE), cmake_dirs=frozenset(directories))


# --- The build graph ------------------------------------------------------------------------------


Runner = Callable[[list[str]], tuple[int, str]]


def _subprocess_runner(argv: list[str]) -> tuple[int, str]:
    try:
        completed = subprocess.run(argv, cwd=REPO_ROOT, capture_output=True, text=True,  # noqa: S603
                                   check=False, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as error:
        return 1, str(error)
    return completed.returncode, completed.stdout


@dataclass(frozen=True)
class Test:
    name: str
    labels: tuple[str, ...]
    command: tuple[str, ...]
    working_directory: str = ""
    environment: tuple[str, ...] = ()


@dataclass
class BuildGraph:
    """The ninja graph and the CTest registrations of ONE build tree, read and never written.

    Every question it answers comes from the tree's own records: `ctest --show-only=json-v1` for the
    tests, `ninja -t inputs` for what a target is built from, `ninja -t deps` for the headers each
    object included, `compile_commands.json` for the paths an object was compiled to read at run time.
    A tree that is not current answers "unknown", because its dependency log describes an older
    tree. Current means three things, each read without writing to the tree: `build.ninja` is newer
    than every CMake file it was generated from; the `CONFIGURE_DEPENDS` globs still match what they
    matched (a COPY of `VerifyGlobs.cmake`, its stamp touch pointed at a temporary flag); and the
    target has no work to do. The last is a dry run through a wrapper manifest that `include`s
    `build.ninja`: `VerifyGlobs.cmake_force` makes `build.ninja` itself dirty on every run, and a dry
    run that must regenerate its manifest stops there without looking at the target at all.
    """

    build_dir: Path
    repo_root: Path = REPO_ROOT
    run: Runner = _subprocess_runner
    read_text: Callable[[Path], str | None] = field(default=lambda path: _read_text(path))
    mtime: Callable[[Path], float | None] = field(default=lambda path: _mtime(path))
    _tests: list[Test] | None = field(default=None, init=False)
    _deps: dict[str, tuple[bool, tuple[str, ...]]] | None = field(default=None, init=False)
    _defines: dict[str, tuple[tuple[str, str], ...]] | None = field(default=None, init=False)
    _unavailable: str | None = field(default=None, init=False)
    _targets: dict[str, Inputs] = field(default_factory=dict, init=False)
    _classified: dict[str, tuple[str, str]] = field(default_factory=dict, init=False)
    #: Definitions that name the repository root and are READ AS TEXT, never opened: see
    #: `exempt_definitions`. Each maps to the reason it is exempt, or to why its exemption lapsed.
    exemptions: Callable[[], dict[str, str]] = field(default=lambda: {})

    def __post_init__(self) -> None:
        # Every path a tree reports is absolute or relative to it, so both roots must be absolute
        # for `_classify` to tell a build output from a source.
        self.build_dir = (self.repo_root / self.build_dir).resolve()
        self.repo_root = self.repo_root.resolve()

    def tests(self, kind: str, regex: str) -> list[Test] | str:
        """The registered tests `_ctest kind -R regex` runs, or why that cannot be read."""
        problem = self.unavailable()
        if problem:
            return problem
        try:
            pattern = re.compile(regex)
        except re.error as error:
            return f"-R {regex!r} is not a regular expression this reader can apply ({error})"
        assert self._tests is not None
        return [test for test in self._tests if kind in test.labels and pattern.search(test.name)]

    def unavailable(self) -> str:
        """Why this tree cannot answer, or an empty string. Read once."""
        if self._unavailable is None:
            self._unavailable = self._load()
        return self._unavailable

    def _load(self) -> str:
        cache = self.read_text(self.build_dir / "CMakeCache.txt")
        if cache is None:
            return f"{self._display(self.build_dir)} is not a configured build tree"
        home = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$", cache, re.MULTILINE)
        if not home or Path(home.group(1)).resolve() != self.repo_root.resolve():
            return f"{self._display(self.build_dir)} was configured from another source tree"
        stale = self._manifest_stale() or self._globs_stale()
        if stale:
            return stale
        status, text = self.run(["ctest", "--test-dir", str(self.build_dir), "--show-only=json-v1"])
        if status != 0:
            return f"ctest could not list the tests of {self._display(self.build_dir)}"
        self._tests = [_test(entry) for entry in json.loads(text).get("tests", ())]
        return ""

    def _manifest_stale(self) -> str:
        """Why `build.ninja` is older than what it was generated from, or an empty string."""
        built = self.mtime(self.build_dir / "build.ninja")
        status, text = self.run(["ninja", "-C", str(self.build_dir), "-t", "query", "build.ninja"])
        if built is None or status != 0:
            return f"{self._display(self.build_dir)} has no readable build.ninja"
        for source in _query_inputs(text):
            if source.endswith("cmake.verify_globs"):
                continue
            changed = self.mtime(Path(source) if os.path.isabs(source) else self.build_dir / source)
            if changed is None or changed > built:
                return (f"{self._display(self.build_dir)} was configured before {source} last "
                        "changed; configure and build it first")
        return ""

    def _globs_stale(self) -> str:
        """Why a CONFIGURE_DEPENDS glob matches something else now, or an empty string."""
        script = self.read_text(self.build_dir / "CMakeFiles" / "VerifyGlobs.cmake")
        if script is None:
            return ""
        with tempfile.TemporaryDirectory(prefix="cy-incremental-globs-") as directory:
            flag = Path(directory) / "mismatch"
            copy = Path(directory) / "VerifyGlobs.cmake"
            copy.write_text(re.sub(r"file\(TOUCH_NOCREATE [^)]*\)",
                                   f'file(WRITE "{flag.as_posix()}" "1")', script), encoding="utf-8")
            status, _ = self.run(["cmake", "-P", str(copy)])
            if status != 0:
                return f"the glob check of {self._display(self.build_dir)} could not be run"
            if flag.exists():
                return (f"a CONFIGURE_DEPENDS glob matches different files than when "
                        f"{self._display(self.build_dir)} was configured; build it first")
        return ""

    def test_inputs(self, test: Test) -> Inputs:
        """What one registered test is built from and what it names at run time."""
        if not test.command:
            return Inputs.unknowable(f"`{test.name}` has no command in this tree — it is not built")
        where, executable = self._classify(test.command[0])
        if where != "build":
            return Inputs.unknowable(
                f"`{test.name}` runs {test.command[0]}, which this tree does not build, so what it "
                "reads is not in the build graph")
        inputs = self.target_inputs(executable)
        for value in (*test.command[1:], *_environment_values(test.environment)):
            inputs = inputs.union(self._named(value, f"`{test.name}`'s command line"))
        return inputs.union(self._working_directory(test))

    def target_inputs(self, target: str) -> Inputs:
        """Everything one ninja target is built from, headers included. Cached per target."""
        if target not in self._targets:
            # A cycle guard: a target reached again while its own inputs are read is unknown.
            self._targets[target] = Inputs.unknowable(f"{target} depends on itself")
            self._targets[target] = self._target_inputs(target)
        return self._targets[target]

    def _target_inputs(self, target: str) -> Inputs:
        with tempfile.TemporaryDirectory(prefix="cy-incremental-ninja-") as directory:
            wrapper = Path(directory) / "wrapper.ninja"
            wrapper.write_text(f"include {self.build_dir.as_posix()}/build.ninja\n",
                               encoding="utf-8")
            status, dry_run = self.run(["ninja", "-C", str(self.build_dir), "-f", str(wrapper),
                                        "-n", target])
        if status != 0:
            return Inputs.unknowable(f"{target} is not a target of {self._display(self.build_dir)}")
        if _NO_WORK not in dry_run:
            return Inputs.unknowable(
                f"{self._display(self.build_dir)} is not current for {target} (`ninja -n` has work "
                "to do), so its dependency log describes an older tree; build it first")
        status, listing = self.run(["ninja", "-C", str(self.build_dir), "-t", "inputs", target])
        if status != 0:
            return Inputs.unknowable(f"`ninja -t inputs {target}` failed")
        files: set[str] = set()
        inputs = Inputs(account=(f"the build graph of {target}",))
        for line in filter(None, (line.strip() for line in listing.splitlines())):
            where, path = self._classify(line)
            if where == "repo":
                files.add(path)
            elif where == "build" and path.endswith((".o", ".obj")):
                inputs = inputs.union(self._object_inputs(path, files))
        return inputs.union(_with_cmake(files))

    def _object_inputs(self, obj: str, files: set[str]) -> Inputs:
        """An object's headers, from the dependency log, and the paths its definitions name."""
        deps = self._dependency_log()
        if obj not in deps:
            return Inputs.unknowable(f"{obj} has no recorded header dependencies")
        valid, headers = deps[obj]
        if not valid:
            return Inputs.unknowable(f"{obj}'s recorded header dependencies are STALE")
        files.update(self._repository_paths(headers))
        inputs = Inputs()
        for name, value in self._definitions().get(obj, ()):
            inputs = inputs.union(self._defined(name, value, obj))
        return inputs

    def _defined(self, name: str, value: str, obj: str) -> Inputs:
        """A path an object was compiled with. A definition naming the repository ROOT would make
        every test linking the object unknown; one declared in incremental.toml, whose using files
        are unchanged since it was reviewed, is a string and not a read."""
        named = self._named(value, f"{name}, a compile definition of {obj},")
        if not named.unknown or self._classify(value) != ("repo", ""):
            return named
        exemption = self.exemptions().get(name, "")
        if exemption == EXEMPT:
            return Inputs()
        return Inputs.unknowable(exemption).union(named) if exemption else named

    def _repository_paths(self, paths: Iterable[str]) -> set[str]:
        """The source-tree files among a build's paths; build outputs and system headers are not."""
        classified = (self._classify(path) for path in paths)
        return {path for where, path in classified if where == "repo" and path}

    def _named(self, value: str, where: str) -> Inputs:
        """A path a test reads at run time: in the source tree it is an input, in the build tree it
        is a build output and its own inputs are, and the root of either is anything at all."""
        if not os.path.isabs(value):
            return Inputs()
        kind, path = self._classify(value)
        if kind == "repo":
            if not path:
                return Inputs.unknowable(f"{where} names the repository root, so it can read "
                                         "anything in it")
            return _with_cmake((path,))
        if kind == "build":
            if not path:
                return Inputs.unknowable(f"{where} names the build tree's root")
            return self.target_inputs(path)
        return Inputs()

    def _working_directory(self, test: Test) -> Inputs:
        """A working directory in the SOURCE tree is read by relative path; one in the build tree is
        where the test writes, and what it reads there is a build output already in its graph."""
        if not test.working_directory:
            return Inputs()
        kind, path = self._classify(test.working_directory)
        if kind != "repo":
            return Inputs()
        if not path:
            return Inputs.unknowable(f"`{test.name}` runs in the repository root and may read any "
                                     "file in it by a relative path")
        return _with_cmake((path,))

    def _dependency_log(self) -> dict[str, tuple[bool, tuple[str, ...]]]:
        if self._deps is None:
            status, text = self.run(["ninja", "-C", str(self.build_dir), "-t", "deps"])
            self._deps = _parse_deps(text) if status == 0 else {}
        return self._deps

    def _definitions(self) -> dict[str, tuple[tuple[str, str], ...]]:
        if self._defines is None:
            text = self.read_text(self.build_dir / "compile_commands.json")
            self._defines = _parse_definitions(json.loads(text), self.build_dir) if text else {}
        return self._defines

    def _classify(self, path: str) -> tuple[str, str]:
        """('build' | 'repo' | 'outside', the path relative to that tree). The build tree is inside
        the repository, so it is asked first. Memoised: a test's closure names each header once
        per object that includes it, tens of thousands of times over the ladder."""
        known = self._classified.get(path)
        if known is None:
            known = self._classified[path] = self._classify_once(path)
        return known

    def _classify_once(self, path: str) -> tuple[str, str]:
        absolute = os.path.normpath(os.path.join(self.build_dir, path))
        for kind, root in (("build", str(self.build_dir)), ("repo", str(self.repo_root))):
            if absolute == root:
                return kind, ""
            if absolute.startswith(root + os.sep):
                return kind, Path(absolute[len(root) + 1:]).as_posix()
        return "outside", ""

    def _display(self, path: Path) -> str:
        try:
            return path.relative_to(self.repo_root).as_posix()
        except ValueError:
            return str(path)


def _mtime(path: Path) -> float | None:
    try:
        return path.stat().st_mtime
    except OSError:
        return None


def _query_inputs(text: str) -> list[str]:
    """The inputs `ninja -t query` lists for one output: the indented lines between `input:` and
    `outputs:`, with the `|` and `||` markers of implicit and order-only inputs removed."""
    inputs: list[str] = []
    reading = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith(("input:", "outputs:")):
            reading = stripped.startswith("input:")
            continue
        if reading and stripped:
            inputs.append(stripped.lstrip("|").strip())
    return inputs


def _read_text(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8")
    except OSError:
        return None


def _test(entry: dict) -> Test:
    properties = {item.get("name"): item.get("value") for item in entry.get("properties", ())}
    return Test(name=entry.get("name", ""),
                labels=tuple(properties.get("LABELS") or ()),
                command=tuple(entry.get("command") or ()),
                working_directory=str(properties.get("WORKING_DIRECTORY") or ""),
                environment=tuple(properties.get("ENVIRONMENT") or ()))


def _environment_values(environment: Iterable[str]) -> list[str]:
    values: list[str] = []
    for assignment in environment:
        values.extend(assignment.partition("=")[2].split(os.pathsep))
    return values


def _parse_deps(text: str) -> dict[str, tuple[bool, tuple[str, ...]]]:
    """`ninja -t deps`: an unindented `<output>: #deps N, deps mtime M (VALID|STALE)` line, then one
    indented line per dependency."""
    parsed: dict[str, tuple[bool, tuple[str, ...]]] = {}
    current, valid, headers = "", False, []
    for line in text.splitlines():
        if line and not line[0].isspace():
            if current:
                parsed[current] = (valid, tuple(headers))
            output, _, rest = line.partition(": #deps")
            current = os.path.normpath(output)
            valid, headers = rest.rstrip().endswith("(VALID)"), []
        elif line.strip():
            headers.append(line.strip())
    if current:
        parsed[current] = (valid, tuple(headers))
    return parsed


_DEFINITION = re.compile(r"""-D(\w+)=(?:\\?["'])?([^\s"'\\]+)""")


def _parse_definitions(entries: list[dict],
                       build_dir: Path) -> dict[str, tuple[tuple[str, str], ...]]:
    """Each object's `-D NAME=value` pairs, keyed as ninja names the object."""
    parsed: dict[str, tuple[tuple[str, str], ...]] = {}
    for entry in entries:
        output = entry.get("output", "")
        command = entry.get("command") or " ".join(entry.get("arguments", ()))
        if not output:
            continue
        relative = os.path.relpath(output, build_dir) if os.path.isabs(output) else output
        key = os.path.normpath(relative)
        parsed[key] = tuple(_DEFINITION.findall(command))
    return parsed


# --- Definitions that name the root and are not reads ------------------------------------------


def exempt_definitions(source: Path = EXEMPTIONS_FILE, repo_root: Path = REPO_ROOT,
                       run: Runner | None = None) -> dict[str, str]:
    """Each declared definition, mapped to EXEMPT or to the reason its exemption has LAPSED.

    A definition naming the repository root makes every object compiled with it able to read any
    file, as far as a reader of the build graph can tell — `CY_DIAG_SOURCE_ROOT`, compiled into the
    diagnostics library nearly every test links, would make almost every test criterion unknown.
    That one is a prefix STRIPPED from reported source paths and never opened, and a person can
    see that; a tool cannot. So the exemption is declared, and pinned the way a falsifiability proof
    is: it names every file allowed to use the macro and a digest of their content. It lapses —
    the definition is a read again, and the criteria are unknown — when any of those files changes,
    or when any other file names the macro, until somebody re-reads them and updates the digest.
    """
    run = run or _subprocess_runner
    try:
        with source.open("rb") as handle:
            document = tomllib.load(handle)
    except (OSError, tomllib.TOMLDecodeError):
        return {}
    return {str(entry.get("name", "")): _exemption_state(entry, repo_root, run)
            for entry in document.get("definition", ())}


def _exemption_state(entry: dict, repo_root: Path, run: Runner) -> str:
    name, used_by = str(entry.get("name", "")), sorted(entry.get("used_by", ()))
    current = definition_digest(used_by, repo_root)
    if current != entry.get("digest"):
        return (f"the exemption for {name} in incremental.toml lapsed: {', '.join(used_by)} changed "
                f"since it was reviewed (digest now {current}); re-read them and update it")
    users: set[str] = set()
    for token in entry.get("tokens", (name,)):
        status, listing = run(["git", "-C", str(repo_root), "grep", "-l", "-F", token, "--",
                               *entry.get("pathspec", ())])
        if status not in (0, 1):
            return f"the exemption for {name} in incremental.toml lapsed: git grep failed"
        users |= {path for path in listing.split() if not _is_cmake_file(path)}
    strays = sorted(users - set(used_by))
    if strays:
        return (f"the exemption for {name} in incremental.toml lapsed: {', '.join(strays)} names "
                "it or the function that returns it, and is not among the files it was reviewed "
                "over")
    return EXEMPT


def definition_digest(paths: Iterable[str], repo_root: Path = REPO_ROOT) -> str:
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(path.encode() + b"\0")
        try:
            digest.update((repo_root / path).read_bytes())
        except OSError:
            digest.update(b"<absent>")
    return digest.hexdigest()[:16]


# --- What a recipe reads --------------------------------------------------------------------------


@dataclass
class Recipes:
    """What a `just` recipe's own text reads: the just files, and the directory of every repository
    path its closure names. Read from `just --show`, followed through every recipe it invokes."""

    run: Runner = _subprocess_runner
    repo_root: Path = REPO_ROOT
    _names: frozenset[str] | None = field(default=None, init=False)
    _tops: str | None = field(default=None, init=False)
    _cache: dict[str, Inputs] = field(default_factory=dict, init=False)

    def inputs(self, recipe: str) -> Inputs:
        if recipe not in self._cache:
            self._cache[recipe] = self._closure(recipe)
        return self._cache[recipe]

    def _closure(self, recipe: str) -> Inputs:
        if recipe not in self._recipe_names():
            return Inputs.unknowable(f"`just {recipe}` is not a recipe")
        seen, waiting, named = {recipe}, [recipe], set()
        while waiting:
            status, body = self.run(["just", "--show", waiting.pop()])
            if status != 0:
                return Inputs.unknowable(f"`just --show` could not print the closure of {recipe}")
            body = re.sub(r"(?m)^\s*#.*$", "", body)
            named |= self._paths(body)
            for invoked in re.findall(r"\bjust\s+([A-Za-z0-9_-]+)", body):
                if invoked in self._recipe_names() and invoked not in seen:
                    seen.add(invoked)
                    waiting.append(invoked)
        return Inputs(paths=frozenset(named) | {"justfile"}, globs=(JUST_FILES[1],),
                      account=(f"the text of `just {recipe}` and the {len(seen)} recipe(s) it runs",))

    def _paths(self, body: str) -> set[str]:
        """The directory of every repository path the text names: a script's siblings are what it
        imports, so the directory is the input and not the file."""
        found = set()
        for path in re.findall(rf"(?<![\w.-])((?:{self._top_level()})/[\w.+/-]*)", body):
            path = path.rstrip("/.")
            candidate = self.repo_root / path
            found.add(path if candidate.is_dir() else _parent(path) or path)
        return found

    def _recipe_names(self) -> frozenset[str]:
        """Every recipe, PRIVATE ONES INCLUDED: `just --summary` hides `_ctest`, which is where every
        test recipe's work is, so a closure read from it stopped one step short."""
        if self._names is None:
            status, text = self.run(["just", "--dump", "--dump-format", "json"])
            try:
                names = json.loads(text).get("recipes", {}) if status == 0 else {}
            except ValueError:
                names = {}
            self._names = frozenset(names)
        return self._names

    def _top_level(self) -> str:
        if self._tops is None:
            names = sorted(entry.name for entry in self.repo_root.iterdir()
                           if entry.is_dir() and entry.name not in ("build", ".git")
                           and not entry.name.startswith("."))
            self._tops = "|".join(re.escape(name) for name in names) or "(?!)"
        return self._tops


# --- The resolver: a criterion's inputs, or why they are unknown ----------------------------------


@dataclass
class Resolver:
    graph: BuildGraph
    recipes: Recipes

    def inputs(self, criterion: criteria_module.Criterion) -> Inputs:
        evaluator = Inputs(paths=frozenset(EVALUATOR), account=("the evaluator",))
        if criterion.kind == "tiers":
            return evaluator.union(Inputs(paths=frozenset(RECORD_INPUTS),
                                          account=("the status record",)))
        if criterion.kind == "path":
            return evaluator.union(Inputs(globs=(criterion.path,),
                                          account=(f"the glob {criterion.path}",)))
        return evaluator.union(self._body(criterion.run))

    def _body(self, body: str) -> Inputs:
        segments = parse_body(body)
        if segments is None:
            return Inputs.unknowable("its body is not a one-line chain of `just` invocations, so "
                                     "what it runs is not something this reader decides")
        inputs = Inputs()
        for words in segments:
            inputs = inputs.union(self._segment(words))
        return inputs

    def _segment(self, words: list[str]) -> Inputs:
        if len(words) < 2 or words[0] != "just":
            return Inputs.unknowable(f"`{' '.join(words)}` is not a `just` invocation")
        recipe, arguments = words[1], words[2:]
        if recipe == QUIET_HOST and arguments[:1] == ["--"]:
            return self.recipes.inputs(recipe).union(self._segment(arguments[1:]))
        selections = _suite_selections(recipe, arguments)
        if selections is None:
            return Inputs.unknowable(
                f"`just {recipe}` declares no inputs: only a test recipe given `-R <regex>` names "
                "what it reads")
        inputs = self.recipes.inputs(recipe)
        for kind, regex in selections:
            inputs = inputs.union(self._suite(kind, regex))
        return inputs

    def _suite(self, kind: str, regex: str) -> Inputs:
        tests = self.graph.tests(kind, regex)
        if isinstance(tests, str):
            return Inputs.unknowable(tests)
        if not tests:
            return Inputs.unknowable(f"`{kind}` -R {regex!r} selects no test registered in "
                                     f"{self.graph._display(self.graph.build_dir)}")
        inputs = Inputs(account=(f"{len(tests)} {kind} test(s) matching {regex!r}",))
        for test in tests:
            inputs = inputs.union(self.graph.test_inputs(test))
        return inputs


def parse_body(body: str) -> list[list[str]] | None:
    """The `&&`-joined commands of a one-line body, or None when a shell would do more with it."""
    text = body.strip()
    if not text or _UNREADABLE_BODY.search(text.replace("&&", " ")):
        return None
    try:
        words = shlex.split(text)
    except ValueError:
        return None
    segments: list[list[str]] = [[]]
    for word in words:
        if word == "&&":
            segments.append([])
        else:
            segments[-1].append(word)
    return segments if all(segments) else None


def _suite_selections(recipe: str, arguments: list[str]) -> list[tuple[str, str]] | None:
    """The (label, regex) pairs a test recipe runs, or None when the arguments say anything else."""
    if recipe in TEST_RECIPES:
        readable = len(arguments) == 2 and arguments[0] == "-R"
        return [(TEST_RECIPES[recipe], arguments[1])] if readable else None
    if recipe == "test-suites":
        return _kind_regex_pairs(arguments)
    return None


def _kind_regex_pairs(arguments: list[str]) -> list[tuple[str, str]] | None:
    """`test-suites unit:^a$ integration:b`, each argument one suite kind and one regex."""
    pairs = [argument.partition(":") for argument in arguments]
    if not pairs or not all(kind in SUITE_KINDS and regex for kind, _, regex in pairs):
        return None
    return [(kind, regex) for kind, _, regex in pairs]


# --- The selection --------------------------------------------------------------------------------


@dataclass(frozen=True)
class Choice:
    entry: criteria_module.PlanEntry
    selected: bool
    reasons: tuple[str, ...]


@dataclass(frozen=True)
class Selection:
    base: str
    changed: tuple[str, ...]
    choices: tuple[Choice, ...]

    @property
    def selected(self) -> tuple[criteria_module.PlanEntry, ...]:
        return tuple(choice.entry for choice in self.choices if choice.selected)

    def count(self, reason: str) -> int:
        return sum(1 for choice in self.choices if choice.reasons[0].startswith(reason))


def select(plan: criteria_module.Plan, changed: Iterable[str], base: str,
           base_digests: dict[str, str] | None, resolve: Callable) -> Selection:
    """Every plan entry with the verdict and its reason. `base_digests` maps a label to the digest
    it had in the base's plan; None means the base's ledgers could not be read, so every criterion is
    treated as edited."""
    labels = {entry.label for entry in plan.entries}
    missing = [label for label in SMOKE if label not in labels]
    if missing:
        raise IncrementalError(f"the smoke set is not in this plan: {', '.join(missing)} — an "
                               "incremental run without it would close on nothing")
    changed = tuple(sorted(set(changed)))
    choices = tuple(_choose(entry, changed, base, base_digests, resolve) for entry in plan.entries)
    return Selection(base=base, changed=changed, choices=choices)


def _choose(entry, changed, base, base_digests, resolve) -> Choice:
    if not entry.permanent:
        return Choice(entry, True, (f"{OWN}: this rung's own criterion",))
    if entry.label in SMOKE:
        return Choice(entry, True, (f"{SMOKE_SET}: build, format, lint and test-all always run",))
    edited = _edited(entry, base, base_digests)
    if edited:
        return Choice(entry, True, (edited,))
    inputs = resolve(entry.criterion)
    if inputs.unknown:
        return Choice(entry, True, (f"{UNKNOWN}: {inputs.unknown}",))
    hits = inputs.matches(changed)
    if hits:
        more = f" (+{len(hits) - 3} more)" if len(hits) > 3 else ""
        return Choice(entry, True, (f"{CHANGED}: {', '.join(hits[:3])}{more}",
                                    f"read from {'; '.join(inputs.account)}"))
    return Choice(entry, False, (f"{UNCHANGED} since {base[:12]}: read from "
                                 f"{'; '.join(inputs.account)}",))


def _edited(entry, base: str, base_digests: dict[str, str] | None) -> str:
    import falsify  # noqa: PLC0415 — only this mode needs the prover's digest, and it is heavy

    if base_digests is None:
        return f"{EDITED}: the ledgers at {base[:12]} could not be read with today's tooling"
    if entry.label not in base_digests:
        return f"{EDITED}: not in this rung's plan at {base[:12]}"
    if base_digests[entry.label] != falsify.digest(entry.criterion):
        return f"{EDITED}: what it checks changed since {base[:12]} (its digest moved)"
    return ""


# --- The repository: the base commit, what changed, and the plan it had ---------------------------


def git(*arguments: str, check: bool = True) -> str:
    completed = subprocess.run(["git", *arguments],  # noqa: S603, S607 — fixed argv, no shell
                               cwd=REPO_ROOT, capture_output=True, text=True, check=False)
    if check and completed.returncode != 0:
        raise IncrementalError(f"git {' '.join(arguments)}: {completed.stderr.strip()}")
    return completed.stdout


def resolve_commit(commit: str) -> str:
    return git("rev-parse", "--verify", "--quiet", f"{commit}^{{commit}}").strip()


def changed_files(base: str) -> tuple[str, ...]:
    """Every path whose content differs between the base and the WORKING TREE, both sides of a
    rename, and every untracked file that is not ignored."""
    tracked = git("diff", "--name-only", "--no-renames", base, "--").splitlines()
    untracked = git("ls-files", "--others", "--exclude-standard").splitlines()
    return tuple(sorted(set(filter(None, tracked + untracked))))


def base_digests(rung: str, base: str) -> dict[str, str] | None:
    """The digest of every criterion in this rung's plan AS IT WAS at the base commit: its ledgers
    and its gate set, both read from the commit. None when they do not load."""
    import falsify  # noqa: PLC0415

    listing = git("ls-tree", "--name-only", f"{base}:tools/roadmap/milestones", check=False)
    with tempfile.TemporaryDirectory(prefix="cy-incremental-") as directory:
        root = Path(directory)
        for name in filter(lambda name: name.endswith(".toml"), listing.split()):
            (root / name).write_text(git("show", f"{base}:tools/roadmap/milestones/{name}"),
                                     encoding="utf-8")
        gates_file = root / "gates.toml.base"
        gates_file.write_text(git("show", f"{base}:tools/roadmap/gates.toml", check=False),
                              encoding="utf-8")
        try:
            permanent = gates_module.permanent_milestones(gates_module.load(gates_file))
            plan = criteria_module.build_plan(rung, permanent, directory=root)
        except (criteria_module.CriteriaError, gates_module.GateError, ValueError, KeyError):
            return None
    return {entry.label: falsify.digest(entry.criterion) for entry in plan.entries}


# --- The baseline a green full ledger records -----------------------------------------------------


def baseline_file() -> Path:
    """Beside the repository's object store, so every worktree of one clone shares it and nothing
    commits or reaps it."""
    common = Path(git("rev-parse", "--git-common-dir").strip())
    root = common if common.is_absolute() else REPO_ROOT / common
    return root / "cy-roadmap" / "last-green.json"


def last_green(rung: str) -> str:
    try:
        document = json.loads(baseline_file().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return ""
    return str(document.get(rung, {}).get("commit", ""))


def tree_state() -> tuple[str, bool]:
    """HEAD, and whether the working tree is exactly HEAD: no edit, no untracked file."""
    return git("rev-parse", "HEAD").strip(), not git("status", "--porcelain").strip()


def record_green(rung: str, before: tuple[str, bool]) -> str:
    """Record HEAD as this rung's baseline, but only for a run that evaluated one commit and nothing
    else: clean and unmoved from its first criterion to its last."""
    after = tree_state()
    if not (before[1] and after[1] and before[0] == after[0]):
        return ("not recorded as the incremental baseline: the tree was edited, or HEAD moved, "
                "while the ledger ran")
    path = baseline_file()
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        document = {}
    document[rung] = {"commit": after[0],
                      "recorded": datetime.datetime.now(datetime.UTC).isoformat(timespec="seconds")}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return f"recorded {after[0][:12]} as {rung}'s incremental baseline ({display(path)})"


def display(path: Path) -> str:
    try:
        return path.relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return str(path)


def default_build_dir() -> Path:
    """The tree the test recipes use: CY_BUILD_DIR, or the default profile's."""
    return (REPO_ROOT / os.environ.get("CY_BUILD_DIR", "build/dev")).resolve()


def selection_for(plan: criteria_module.Plan, since: str, build_dir: Path) -> Selection:
    """The whole mode: resolve the base, list what changed, and choose."""
    requested = since or last_green(plan.milestone.id)
    if not requested:
        raise IncrementalError(
            f"no green full {plan.milestone.id} ledger is recorded to compare against; pass "
            "--changed-since <commit>, or run the full ledger on a clean tree first")
    base = resolve_commit(requested)
    if not base:
        raise IncrementalError(f"--changed-since {requested!r} is not a commit")
    exemptions: dict[str, str] = {}

    def reviewed() -> dict[str, str]:
        if not exemptions:
            exemptions.update(exempt_definitions() or {"": ""})
        return exemptions
    resolver = Resolver(BuildGraph(build_dir, exemptions=reviewed), Recipes())
    return select(plan, changed_files(base), base, base_digests(plan.milestone.id, base),
                  resolver.inputs)

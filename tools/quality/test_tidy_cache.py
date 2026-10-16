#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The lint cache's refusals: it remembers only a pass, only at a clean commit, only for one entry.

`tools/quality/tidy_cache.py` lets a second job skip files the first one linted green at the same
commit. Every way that could turn a red lint green is a case here, against a real git repository and
a stand-in linter whose verdict each case chooses:

  * a file with findings is never remembered, so the next run lints it again;
  * a dirty working tree — an edit, or an untracked file — reads and writes nothing;
  * a different compile-database entry for the same file is a different key;
  * a different commit is a different key.

Run through `just quality-gates-selftest`.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
CACHE_TOOL = HERE / "tidy_cache.py"

_failures: list[str] = []
_cases = 0


def check(name: str, condition: bool, detail: str = "") -> None:
    global _cases
    _cases += 1
    print(f"{'ok  ' if condition else 'FAIL'} {name}")
    if not condition:
        _failures.append(name)
        if detail:
            print(f"     | {detail}", file=sys.stderr)


def git(repository: Path, *arguments: str) -> None:
    subprocess.run(["git", *arguments], cwd=repository, check=True, capture_output=True)


def tool(repository: Path, *arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(CACHE_TOOL), *arguments], cwd=repository,
                          capture_output=True, text=True, check=False)


def linter(directory: Path, verdict: int) -> Path:
    """A stand-in clang-tidy: answers --version, and otherwise logs the file and exits `verdict`."""
    path = directory / f"tidy-{verdict}"
    path.write_text(
        "#!/bin/sh\n"
        'if [ "$1" = "--version" ]; then echo "stand-in tidy 1.0"; exit 0; fi\n'
        f'echo "$@" >> "{directory}/linted"\n'
        f"exit {verdict}\n", encoding="utf-8")
    path.chmod(0o755)
    return path


def linted(directory: Path) -> int:
    log = directory / "linted"
    return len(log.read_text(encoding="utf-8").splitlines()) if log.exists() else 0


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="cy-tidy-cache-") as scratch:
        root = Path(scratch)
        repository, tools, cache = root / "repo", root / "tools", root / "cache"
        repository.mkdir()
        tools.mkdir()
        git(repository, "init", "-q")
        git(repository, "config", "user.email", "fixture@example.invalid")
        git(repository, "config", "user.name", "fixture")
        (repository / "a.cpp").write_text("int a;\n", encoding="utf-8")
        build = repository / "build"
        build.mkdir()
        database = build / "compile_commands.json"

        def entries(flags: str) -> None:
            database.write_text(json.dumps([{"directory": str(build), "file": "../a.cpp",
                                             "command": f"c++ {flags} -c ../a.cpp"}]),
                                encoding="utf-8")

        entries("-O2")
        git(repository, "add", "-A")
        git(repository, "commit", "-q", "-m", "fixture")
        green, red = linter(tools, 0), linter(tools, 1)

        def run(the_linter: Path) -> tuple[str, int]:
            key = tool(repository, "prepare", str(cache), str(database), str(the_linter))
            outcome = tool(repository, "lint", str(cache), key.stdout.strip(), "--",
                           str(the_linter), "-p", "build", "--quiet", "a.cpp")
            return key.stdout.strip(), outcome.returncode

        key, code = run(red)
        check("a clean commit has a key", bool(key) and code == 1, key)
        before = linted(tools)
        _, code = run(red)
        check("a file with findings is NEVER remembered: it is linted again, and red again",
              code == 1 and linted(tools) == before + 1)
        _, code = run(green)
        check("a pass at this commit is linted once", code == 0)
        before = linted(tools)
        _, code = run(red)
        check("and remembered: the same file at the same commit is not asked again",
              code == 0 and linted(tools) == before)

        entries("-O0")
        git(repository, "commit", "-q", "-am", "flags")  # the database is tracked here, by design
        before = linted(tools)
        key_after, code = run(red)
        check("another commit is another key", key_after != key and code == 1
              and linted(tools) == before + 1)
        _, code = run(green)
        entries("-O3")
        before = linted(tools)
        key_dirty, code = run(red)
        check("a DIRTY tree has no key, and every file is linted",
              key_dirty == "" and code == 1 and linted(tools) == before + 1)
        git(repository, "checkout", "-q", "--", ".")
        (repository / "untracked.h").write_text("#define X 1\n", encoding="utf-8")
        key_untracked, code = run(red)
        check("an untracked file makes the tree dirty too", key_untracked == "" and code == 1)
        (repository / "untracked.h").unlink()

        # The same commit, a different compile entry: the cache was written for `-O0` above.
        index = cache / key_after / "entries.json"
        recorded = json.loads(index.read_text(encoding="utf-8"))
        recorded["a.cpp"] = "0" * 64
        index.write_text(json.dumps(recorded), encoding="utf-8")
        before = linted(tools)
        outcome = tool(repository, "lint", str(cache), key_after, "--", str(red), "-p", "build",
                       "--quiet", "a.cpp")
        check("a different compile-database entry for the file is a different key",
              outcome.returncode == 1 and linted(tools) == before + 1)
        outcome = tool(repository, "lint", str(cache), "", "--", str(red), "a.cpp")
        check("an empty run key lints, and caches nothing", outcome.returncode == 1)

    print(f"\ntidy-cache selftest: {_cases - len(_failures)}/{_cases} passed")
    return 1 if _failures else 0


if __name__ == "__main__":
    sys.exit(main())

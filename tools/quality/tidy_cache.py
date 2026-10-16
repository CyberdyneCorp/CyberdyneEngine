#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Remember which files clang-tidy passed AT ONE COMMIT, so a second job linting it need not ask again.

THE LINT CRITERION CANNOT FINISH ON A HOSTED RUNNER. `m0:lint` runs `just quality-lint`, which lints
every engine translation unit; on a four-core ubuntu-24.04 runner that is 44 minutes (the `quality`
job's `Static analysis` step, run 38776285025), and the ledger gives a criterion 1800 s. The nightly
ledger therefore reported `no result within 1800 s` for a lint that was green — and the `quality`
job of the SAME RUN had just linted the same commit, file by file, with the same tool.

So with `CY_TIDY_CACHE=<dir>` set, `just quality-lint` records each file clang-tidy PASSED, under a
key made of everything the answer is a function of, and skips a file whose key is already recorded:

  * the tree: `HEAD^{tree}`, and only when the working tree is CLEAN — no modified, staged or
    untracked file. A dirty tree is not a commit, so nothing is read from or written to the cache
    and every file is linted; a mutation of the tree (the falsifiability prover's, or a developer's)
    can therefore never be answered from it;
  * the file, and its compile-database entry: the flags, the include paths, the working directory;
  * the linter's `--version`, the compiler's `--version` (its headers are the system headers every
    file includes), and the runner image's version when there is one;
  * the `.clang-tidy` files, which are in the tree and so already in its id.

ONLY A PASS IS RECORDED. A file with findings is linted every time, so a red lint is red in every job
that runs it; what the cache can save is the time of re-confirming a green one. It is off unless the
variable is set: a developer's lint is unchanged, and continuous integration opts in where one job
hands its results to another (`.github/workflows/ci.yml`, `quality` to the ledger's shards).

Usage, from just/quality.just:
    tidy_cache.py prepare <cache> <database> <linter>   prints the run key, or nothing (no caching)
    tidy_cache.py lint <cache> <run-key> -- <command...> <file>    an empty run key lints
"""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

INDEX = "entries.json"


def _output(*command: str) -> str:
    try:
        completed = subprocess.run(command, capture_output=True, text=True, check=False)
    except OSError:
        return ""
    return (completed.stdout + completed.stderr).strip() if completed.returncode == 0 else ""


def _clean_tree() -> str:
    """`HEAD^{tree}` when the working tree is exactly that tree, else an empty string."""
    tree = _output("git", "rev-parse", "HEAD^{tree}")
    if not tree:
        return ""
    status = subprocess.run(["git", "status", "--porcelain", "--untracked-files=all"],
                            capture_output=True, text=True, check=False)
    if status.returncode != 0 or status.stdout.strip():
        return ""
    return tree


def _entry_digests(database: Path) -> dict[str, str]:
    """Each file's compile-database entry, digested, keyed by the path `quality-lint` passes."""
    repository = Path.cwd()
    digests: dict[str, str] = {}
    for entry in json.loads(database.read_text(encoding="utf-8")):
        source = os.path.join(entry.get("directory", ""), entry["file"])
        path = os.path.relpath(os.path.realpath(source), repository).replace(os.sep, "/")
        canonical = json.dumps(entry, sort_keys=True).encode("utf-8")
        digests[path] = hashlib.sha256(canonical).hexdigest()
    return digests


def prepare(cache: Path, database: Path, linter: str) -> int:
    """Print the key every file's key extends, or nothing when this run may not use the cache."""
    tree = _clean_tree()
    if not tree:
        print("tidy-cache: the working tree is not a clean commit; linting every file",
              file=sys.stderr)
        return 0
    compiler = os.environ.get("CXX") or "c++"
    parts = {
        "tree": tree,
        "linter": _output(linter, "--version"),
        "compiler": _output(compiler, "--version"),
        "image": os.environ.get("ImageOS", "") + " " + os.environ.get("ImageVersion", ""),
    }
    if not parts["linter"]:
        print("tidy-cache: the linter does not answer --version; linting every file",
              file=sys.stderr)
        return 0
    key = hashlib.sha256(json.dumps(parts, sort_keys=True).encode("utf-8")).hexdigest()
    run = cache / key
    run.mkdir(parents=True, exist_ok=True)
    recorded = len(list((run / "passed").glob("*"))) if (run / "passed").is_dir() else 0
    print(f"tidy-cache: {recorded} file(s) already passed at tree {tree[:12]}; the rest are linted",
          file=sys.stderr)
    (run / INDEX).write_text(json.dumps(_entry_digests(database), sort_keys=True),
                             encoding="utf-8")
    print(key)
    return 0


def lint(cache: Path, key: str, command: list[str]) -> int:
    """Run the linter on one file unless this run's key already records it passing."""
    if not command:
        print("tidy-cache: no command to run", file=sys.stderr)
        return 2
    if not key:
        return subprocess.run(command, check=False).returncode
    run = cache / key
    file = command[-1]
    entries = json.loads((run / INDEX).read_text(encoding="utf-8"))
    entry = entries.get(file)
    if entry is None:
        return subprocess.run(command, check=False).returncode
    marker = run / "passed" / hashlib.sha256(f"{file}\0{entry}".encode("utf-8")).hexdigest()
    if marker.exists():
        return 0
    code = subprocess.run(command, check=False).returncode
    if code == 0:
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_text(f"{file}\n", encoding="utf-8")
    return code


def main(argv: list[str]) -> int:
    if len(argv) == 4 and argv[0] == "prepare":
        return prepare(Path(argv[1]), Path(argv[2]), argv[3])
    if len(argv) >= 4 and argv[0] == "lint" and argv[3] == "--":
        return lint(Path(argv[1]), argv[2], argv[4:])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

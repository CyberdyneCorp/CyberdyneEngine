# SPDX-License-Identifier: MIT
"""Delete a build tree the moment no later criterion of this ledger run needs it.

THE NIGHTLY LEDGER DIED OF A FULL DISK, TWICE, WITH NO LOG. `milestone — the closed milestones'
exit criteria` ran `just roadmap-milestone m11d5 --ci` on a hosted ubuntu-24.04 runner on
2026-10-13 and 2026-10-14 and both nights the runner itself crashed after about five hours with
`No space left on device`, taking the job's log with it. Nothing in the ledger ever deleted a tree:
every criterion that builds a configuration of its own leaves that tree behind for the rest of the
run. On this project's workstation one full run leaves `build/ledger-matrix/` at 155 GB (thirteen
rows of 1.6 to 20 GB) and the run's own `CY_BUILD_DIR` at 244 GB with its sanitizer, benchmark and
option-off trees inside; the runner image has 87 GB free on `/` before the job installs anything.
The run cannot fit, and it filled the disk within the first thirty criteria: `m1:four-profiles`
builds four matrix rows (55 GB) beside the default tree and two sanitizer trees.

So with `--reclaim-builds` the ledger knows, from the plan, which trees each criterion uses, and
after each criterion finishes it deletes every tree whose users have ALL finished. A tree a later
criterion still needs is kept — `m9:multiplayer-profiles-agree` reads the `dev-default` and
`debug-default` rows `four-profiles` built, 250 criteria later — so this costs no rebuild at all.

DELETING HERE CAN COST TIME AND NEVER A VERDICT. Every criterion builds what it reads: the matrix
runs `just build-engine` for every row it is asked for, every time, and `just test-sanitize` builds
its own tree. A tree reclaimed too early — a body that reaches one by a path this module did not
see — is rebuilt by that criterion, which is slower and still correct. What is never deleted is the
ledger's own default tree (`build:@`), and nothing outside the build root or the matrix root.

OFF BY DEFAULT. A developer's machine keeps its trees warm between two runs on purpose (M11.c's
fifth close took 7 h 28 m because its trees were cold); the hosted runner is the caller with a
disk smaller than one run, and it opts in.

Governed by: delivery-roadmap (Milestone exit criteria are executable), developer-workflow-and-just.
"""

from __future__ import annotations

import os
import re
import shutil
import sys
from pathlib import Path

import matrix as matrix_module
import schedule as schedule_module

#: `python3 tools/roadmap/matrix.py build a b` and `... path a` — rows a body names by hand.
_MATRIX_WORDS = re.compile(r"matrix\.py\s+(?:build|path)\s+([^\n;|&)]*)")
_MATRIX_TOKEN = "ledger-matrix/"
#: `just test-sanitize` builds `${CY_BUILD_DIR:-build}/sanitize-<sanitizers>` (just/test.just).
_SANITIZE_GLOB = "sanitize-*"
#: With CY_BUILD_DIR unset the recipes build in `build/<profile>`: those ARE default trees, whatever
#: a token happens to call them, and a later criterion would pay a full build for each.
_DEFAULT_TREES = frozenset({"dev", "debug", "profile", "release"})


def build_root() -> Path:
    """`${CY_BUILD_DIR:-build}`: where a body's `${CY_BUILD_DIR:-build}/<name>` tree lands."""
    return Path(os.environ.get("CY_BUILD_DIR", "").strip() or "build")


def _labels(entry) -> set[str]:
    return {f"{ledger}:{entry.criterion.id}" for ledger in entry.declared_by}


def _tokens(criterion) -> set[str]:
    """Every `build:<tree>` the criterion declares, derives, or spells in its body.

    The DECLARED list is read as well as `schedule.needs`, because `needs` reduces a declaration
    carrying `exclusive` to `exclusive` alone — and `four-profiles` declares its four rows that way.
    """
    tokens = set(getattr(criterion, "needs", ()) or ())
    tokens |= set(schedule_module.needs(criterion))
    if criterion.kind not in ("path", "tiers"):
        tokens |= set(schedule_module.build_trees(criterion.run or "") or ())
    return {token for token in tokens if token.startswith("build:")}


def _matrix_rows(entry) -> set[str]:
    labels = _labels(entry)
    rows = {row.id for row in matrix_module.CONFIGURATIONS if labels & set(row.needed_by)}
    for match in _MATRIX_WORDS.finditer(entry.criterion.run or ""):
        rows |= {word for word in match.group(1).split() if word in matrix_module.BY_ID}
    return rows


def trees_of(entry, root: Path, matrix_root: Path) -> set[Path]:
    """The trees this entry builds or reads, under `root` or `matrix_root`; a name may be a glob.

    The ledger's default tree (`build:@`) is never among them: every other criterion uses it.
    """
    trees = {matrix_root / row for row in _matrix_rows(entry)}
    for token in _tokens(entry.criterion):
        name = token.split(":", 1)[1]
        if token == schedule_module.DEFAULT_BUILD:
            continue
        if token == schedule_module.SANITIZE_BUILD:
            trees.add(root / _SANITIZE_GLOB)
        elif name.startswith(_MATRIX_TOKEN):
            trees.add(matrix_root / name[len(_MATRIX_TOKEN):])
        else:
            trees.add(root / name)
    return trees


def _inside(path: Path, parent: Path) -> bool:
    try:
        resolved, base = path.resolve(), parent.resolve()
    except OSError:
        return False
    return resolved != base and base in resolved.parents


class Reclaimer:
    """Tracks which criteria are still to finish, and deletes the trees none of them needs."""

    def __init__(self, entries, root: Path | None = None, matrix_root: Path | None = None,
                 log=None) -> None:
        self.root = root if root is not None else build_root()
        self.matrix_root = matrix_root if matrix_root is not None else matrix_module.root()
        self.log = log if log is not None else sys.stderr
        self.users: dict[Path, set[int]] = {}
        for index, entry in enumerate(entries):
            for tree in trees_of(entry, self.root, self.matrix_root):
                self.users.setdefault(tree, set()).add(index)

    def finished(self, index: int) -> list[Path]:
        """Criterion `index` is done: delete each tree it was the last user of. Returns them."""
        removed: list[Path] = []
        for pattern, users in self.users.items():
            users.discard(index)
            if users:
                continue
            for tree in self._existing(pattern):
                if self._remove(tree):
                    removed.append(tree)
        for pattern in [pattern for pattern, users in self.users.items() if not users]:
            del self.users[pattern]
        return removed

    def _existing(self, pattern: Path) -> list[Path]:
        if any(character in pattern.name for character in "*?["):
            return sorted(path for path in pattern.parent.glob(pattern.name) if path.is_dir())
        return [pattern] if pattern.is_dir() else []

    def _remove(self, tree: Path) -> bool:
        if tree.is_symlink() or not (_inside(tree, self.root) or _inside(tree, self.matrix_root)):
            print(f"[ledger] not reclaiming {tree}: outside {self.root} and {self.matrix_root}",
                  file=self.log, flush=True)
            return False
        if tree.parent.resolve() == self.root.resolve() and tree.name in _DEFAULT_TREES:
            print(f"[ledger] not reclaiming {tree}: a profile's default tree", file=self.log,
                  flush=True)
            return False
        shutil.rmtree(tree, ignore_errors=True)
        print(f"[ledger] reclaimed {tree}: no later criterion needs it; {free_space(tree.parent)}",
              file=self.log, flush=True)
        return True


def free_space(path: Path) -> str:
    """`N.N GB free of M.M GB` on the filesystem holding `path` — the line the full disk lacked."""
    probe = path
    while not probe.exists() and probe != probe.parent:
        probe = probe.parent
    try:
        usage = shutil.disk_usage(probe)
    except OSError as error:
        return f"free space unreadable ({error})"
    return f"{usage.free / 1e9:.1f} GB free of {usage.total / 1e9:.1f} GB"

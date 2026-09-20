#!/usr/bin/env python3
"""Keep Editor feature work out of renderer/backend ownership areas."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
FORBIDDEN = {
    "editor/crates/cy-editor-viewport-transport/": "native viewport transport",
    "src/backends/rhi/": "shared RHI backend interface",
    "src/backends/rhi-metal/": "native Metal backend",
    "openspec/changes/implement-macos-metal/": "macOS/Metal OpenSpec change",
    "openspec/changes/implement-m11d5-backends/": "backend OpenSpec change",
}


def violations(paths: list[str]) -> list[tuple[str, str]]:
    """Return paths owned by another active workstream."""
    found: list[tuple[str, str]] = []
    for path in paths:
        normalized = path.strip().replace("\\", "/").removeprefix("./")
        for prefix, owner in FORBIDDEN.items():
            if normalized.startswith(prefix):
                found.append((normalized, owner))
                break
    return found


def changed_paths() -> list[str]:
    """Read tracked and untracked worktree paths from porcelain status."""
    result = subprocess.run(
        ["git", "status", "--porcelain=v1", "-z"],
        cwd=REPOSITORY,
        check=True,
        capture_output=True,
    )
    entries = result.stdout.decode("utf-8", errors="strict").split("\0")
    paths: list[str] = []
    skip_rename_source = False
    for entry in entries:
        if not entry:
            continue
        if skip_rename_source:
            skip_rename_source = False
            continue
        status = entry[:2]
        paths.append(entry[3:])
        if "R" in status or "C" in status:
            skip_rename_source = True
    return paths


def selftest() -> int:
    fixtures = [
        "editor/crates/cy-editor-viewport-transport/src/session.rs",
        "src/backends/rhi/include/cy/backends/rhi/device.h",
        "src/backends/rhi-metal/src/device.mm",
        "openspec/changes/implement-macos-metal/tasks.md",
        "openspec/changes/implement-m11d5-backends/tasks.md",
    ]
    for fixture in fixtures:
        if len(violations([fixture])) != 1:
            print(f"feature-scope selftest: did not reject {fixture}", file=sys.stderr)
            return 1
    if violations(["editor/crates/cy-editor-shell/src/app.rs"]):
        print("feature-scope selftest: rejected an Editor shell change", file=sys.stderr)
        return 1
    print("feature-scope selftest: all ownership fixtures rejected")
    return 0


def main(arguments: list[str]) -> int:
    if arguments == ["--selftest"]:
        return selftest()
    found = violations(arguments or changed_paths())
    for path, owner in found:
        print(f"feature-scope: {path} belongs to the parallel {owner} workstream", file=sys.stderr)
    return 1 if found else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

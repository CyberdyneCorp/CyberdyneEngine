#!/usr/bin/env python3
"""Hold the workflow's own recipes to the invariants a gate depends on.

`tools/ci/check_workflows.py` checks that continuous integration invokes recipes rather than
reimplementing them, and `tools/ci/test_env_doctor.py` checks that the doctor is right when the
environment is wrong. This file is the third of those: a recipe whose *choice of build tree* is
wrong corrupts every gate that runs after it, and no amount of checking the workflow catches that.

The case that put this file here — found while closing M1 — is `just test-sanitize`. It read
CY_BUILD_DIR as the tree to build **in**, so a run with the override set configured the developer's
ordinary build directory with `-D CY_SANITIZE=address,undefined` and left it that way. Everything
downstream then ran against it: `just quality-lint` linted a compile database carrying sanitizer
flags and failed on findings an ordinary database does not produce, and `just run-sample` ran an
instrumented binary and failed on LeakSanitizer reports from inside SDL3. The visible symptom was
`just roadmap-milestone m0` going red on the very tree `just roadmap-milestone m1` had just proved
green — the exact regression the milestone ledger exists to catch, produced by the ledger itself.

Run through `just ci-check`. Exits 0 when every case passes, 1 naming the ones that did not.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys


def recipe(root: pathlib.Path, arguments: list[str], build_dir: str | None) -> str:
    """Run a private recipe and return its single line of output."""
    environment = dict(os.environ)
    environment.pop("CY_BUILD_DIR", None)
    if build_dir is not None:
        environment["CY_BUILD_DIR"] = build_dir
    result = subprocess.run(
        ["just", *arguments],
        cwd=root,
        env=environment,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def sanitized_tree_is_never_the_build_tree(root: pathlib.Path) -> list[str]:
    """A sanitized tree is a tree of its own, whether or not CY_BUILD_DIR is set."""
    failures = []

    default = recipe(root, ["_sanitize-dir", "thread"], None)
    if default != "build/sanitize-thread":
        failures.append(f"the default sanitized tree is {default!r}, expected 'build/sanitize-thread'")

    for override in ("build/dev", "build/somewhere-else", "/tmp/cy-build"):
        for sanitizer in ("thread", "address,undefined"):
            chosen = recipe(root, ["_sanitize-dir", sanitizer], override)
            if chosen == override:
                failures.append(
                    f"CY_BUILD_DIR={override} with --sanitizer {sanitizer} builds in the ordinary "
                    f"tree; a sanitized tree must never be one"
                )
            elif not chosen.startswith(override + "/"):
                failures.append(
                    f"CY_BUILD_DIR={override} with --sanitizer {sanitizer} chose {chosen!r}, which "
                    f"ignores the override instead of nesting under it"
                )
            elif sanitizer.replace(",", "-") not in chosen:
                failures.append(
                    f"{chosen!r} does not name the sanitizer, so two sanitizers would share a tree"
                )
    return failures


def sanitized_tree_survives_wl(root: pathlib.Path) -> list[str]:
    """No comma in a build path: `-Wl,` splits its argument on commas.

    `--sanitizer address,undefined` named `build/sanitize-address,undefined`, and SDL3 probes the
    linker with `-Wl,--version-script=<path>.sym`. The linker received the path truncated at the
    comma, could not open it, and SDL3 reported that this platform does not support version
    scripts — so the whole configure failed. It passed locally only because CY_BUILD_DIR was set to
    a comma-free tree, which is the other rule being broken; continuous integration sets no
    CY_BUILD_DIR, so the sanitizer gate could not have passed there.
    """
    failures = []
    for sanitizer in ("thread", "address", "undefined", "address,undefined"):
        for override in (None, "build/dev"):
            chosen = recipe(root, ["_sanitize-dir", sanitizer], override)
            for character in ",;: ":
                if character in chosen:
                    failures.append(
                        f"--sanitizer {sanitizer} chose {chosen!r}, which contains {character!r}; "
                        f"a build path with one of ',;: ' is mangled by -Wl, or by a shell"
                    )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[2],
        help="repository root (default: this file's repository)",
    )
    arguments = parser.parse_args()
    root = arguments.root.resolve()

    cases = {
        "a sanitized build tree is never the ordinary build tree": (
            sanitized_tree_is_never_the_build_tree
        ),
        "a sanitized build tree's path survives -Wl,": sanitized_tree_survives_wl,
    }

    failed = 0
    for name, case in cases.items():
        failures = case(root)
        if failures:
            failed += 1
            print(f"fail {name}", file=sys.stderr)
            for failure in failures:
                print(f"       {failure}", file=sys.stderr)
        else:
            print(f"ok   {name}")

    if failed:
        print(f"recipe selftest: {failed} of {len(cases)} cases failed", file=sys.stderr)
        return 1
    print(f"recipe selftest: {len(cases)}/{len(cases)} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

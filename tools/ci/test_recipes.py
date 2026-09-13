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
import re
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


def editor_target_dir_honours_the_override(root: pathlib.Path) -> list[str]:
    """The editor's cargo target directory is the override, not the override glued to the root.

    `_editor-target-dir` joined `{{root}}/${CY_BUILD_DIR:-build}/editor` unconditionally, so an
    ABSOLUTE override became `<root>/<root>/<override>/editor`: three gigabytes of editor written
    inside the source tree while every consumer — `samples/05-editor-session/session.py`,
    `samples/08a-authoring/authoring.py` and the `--build-dir` CTest passes — looked under the
    override itself. `just build-editor` reported success and the editor suites then failed with
    "cyberdyne-editor is not at <override>/editor/<cargo>/cyberdyne-editor. Build it with: just
    build-editor --profile debug", naming the recipe that had just run. Found closing M10.
    """
    failures = []

    default = recipe(root, ["_editor-target-dir"], None)
    if default != f"{root}/build/editor":
        failures.append(f"the default editor tree is {default!r}, expected {str(root) + '/build/editor'!r}")

    for override in ("build/dev", "build/gate-profiles", "/tmp/cy-build", str(root) + "/build/dev"):
        chosen = recipe(root, ["_editor-target-dir"], override)
        expected = (
            f"{override}/editor" if override.startswith("/") else f"{root}/{override}/editor"
        )
        if chosen != expected:
            failures.append(
                f"CY_BUILD_DIR={override} puts the editor at {chosen!r}, expected {expected!r}"
            )
        if pathlib.PurePosixPath(chosen).is_absolute() is False:
            failures.append(f"{chosen!r} is relative; CARGO_TARGET_DIR is read from cargo's own cwd")
        # The engine's tree and the editor's must be the same directory, whatever the override's
        # shape: `--build-dir <tree>` is how the CTest entries find the binary cargo just wrote.
        if not chosen.startswith(
            (override if override.startswith("/") else f"{root}/{override}") + "/"
        ):
            failures.append(
                f"CY_BUILD_DIR={override} puts the editor outside the build tree, at {chosen!r}"
            )
    return failures


def a_recipe_that_parses_flags_binds_them(root: pathlib.Path) -> list[str]:
    """`just` interpolates `{{args}}` as TEXT; it does not set `$@`.

    A recipe that parses flags with a `while (($#))` loop and never runs `set -- {{args}}` reads an
    EMPTY positional list, silently ignores every flag it was handed, and reports success for work it
    did not do. `build-reap` shipped with exactly that: `--keep`, `--older-than` and `--apply` were all
    dropped, so the one flag that deletes anything did nothing while the recipe claimed a dry run.

    That is a silent wrong answer rather than a crash, which is the class a self-test has to catch —
    the recipe LOOKED like it worked, and only exercising each flag showed it did not.
    """
    text = (root / "just" / "build.just").read_text(encoding="utf-8")
    # Split on RECIPE HEADERS, not on blank lines: a recipe body contains blank lines of its own, so
    # splitting on those separates a header from the body it introduces and the check silently finds
    # nothing. That is how the first version of this case passed with the defect reinstated.
    headers = [
        (m.start(), m.group(1))
        for m in re.finditer(r"^([a-z_][\w-]*) \*args:$", text, re.MULTILINE)
    ]
    offenders: list[str] = []
    for index, (start, name) in enumerate(headers):
        end = headers[index + 1][0] if index + 1 < len(headers) else len(text)
        body = text[start:end]
        parses = "$#" in body or re.search(r"^\s+shift\b", body, re.MULTILINE) is not None
        if parses and "set -- {{args}}" not in body:
            offenders.append(name)
    return [
        f"{name}: parses positional arguments but never runs `set -- {{{{args}}}}`, so every flag it "
        "is given is silently ignored"
        for name in offenders
    ]


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
        "a recipe that parses flags binds them to $@": a_recipe_that_parses_flags_binds_them,
        "the editor is built into the build tree the override names": (
            editor_target_dir_honours_the_override
        ),
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

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regression tests for the release assembler."""

from __future__ import annotations

import hashlib
import json
import sys
import tarfile
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release  # noqa: E402


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def version_updates_both_toolchains() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        (root / "editor").mkdir()
        (root / "CMakeLists.txt").write_text("project(CyberdyneEngine\n VERSION 0.0.0)\n")
        (root / "editor" / "Cargo.toml").write_text(
            '[workspace.package]\nversion = "0.1.0"\n')
        release.set_source_version("1.2.3", root)
        check(release.source_versions(root) == ("1.2.3", "1.2.3"),
              "one version command must update CMake and Cargo")


def archives_are_reproducible_and_normalized() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        source = root / "stage"
        source.mkdir()
        (source / "payload.txt").write_text("same bytes\n")
        first, second = root / "first.tar.gz", root / "second.tar.gz"
        release.deterministic_archive(source, first, "release")
        (source / "payload.txt").touch()
        release.deterministic_archive(source, second, "release")
        check(hashlib.sha256(first.read_bytes()).digest() ==
              hashlib.sha256(second.read_bytes()).digest(),
              "archive bytes changed when only the source timestamp changed")
        with tarfile.open(first) as archive:
            check(all(member.mtime == 0 and member.uid == 0 and member.gid == 0
                      for member in archive.getmembers()), "archive metadata is not normalized")


def headers_land_directly_under_include() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        header = root / "src" / "example" / "include" / "cy" / "example.h"
        header.parent.mkdir(parents=True)
        header.write_text("#pragma once\n")
        build = root / "build"
        (build / "generated" / "include").mkdir(parents=True)
        (build / "generated" / "project" / "include").mkdir(parents=True)
        stage = root / "stage"
        previous = release.ROOT
        try:
            release.ROOT = root
            release.include_headers(stage, build)
        finally:
            release.ROOT = previous
        check((stage / "include" / "cy" / "example.h").is_file(),
              "public header was not packaged below include/")
        check(not (stage / "include" / "include").exists(),
              "public header gained a duplicate include/include prefix")


def missing_and_changed_artifacts_are_refused() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        artifact = root / "component.tar.gz"
        artifact.write_bytes(b"original")
        manifest = root / "manifest.json"
        manifest.write_text(json.dumps({"artifacts": [{"file": artifact.name,
            "sha256": release.sha256(artifact)}]}))
        release.verify_manifest(manifest)
        artifact.write_bytes(b"changed")
        try:
            release.verify_manifest(manifest)
        except release.ReleaseError:
            return
        raise AssertionError("publication accepted an artifact changed after assembly")


def publication_defaults_to_a_reviewable_dry_run() -> None:
    arguments = release.parser().parse_args(["publish", "--manifest", "missing.json"])
    check(not arguments.execute, "publication must require an explicit execution flag")


def version_rejects_non_semantic_input() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        (root / "editor").mkdir()
        (root / "CMakeLists.txt").write_text("project(CyberdyneEngine VERSION 0.0.0)\n")
        (root / "editor" / "Cargo.toml").write_text(
            '[workspace.package]\nversion = "0.0.0"\n')
        try:
            release.set_source_version("next", root)
        except release.ReleaseError:
            return
        raise AssertionError("a non-semantic release version was accepted")


def main() -> int:
    cases = [version_updates_both_toolchains, archives_are_reproducible_and_normalized,
             headers_land_directly_under_include,
             missing_and_changed_artifacts_are_refused,
             publication_defaults_to_a_reviewable_dry_run,
             version_rejects_non_semantic_input]
    for case in cases:
        case()
        print(f"PASS {case.__name__}")
    print(f"release assembler: {len(cases)}/{len(cases)} cases pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

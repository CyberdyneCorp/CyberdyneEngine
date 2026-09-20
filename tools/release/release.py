#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reproducible CyberdyneEngine release assembly.

This tool is deliberately an assembler. CMake and Cargo build the binaries, while
``cy_build`` remains the package engine for game content. The release layer selects the
documented public outputs, records their provenance, and writes deterministic archives.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:[-+][0-9A-Za-z.-]+)?$")
COMPONENTS = ("editor", "runtime", "sdk", "tools", "templates")


class ReleaseError(RuntimeError):
    """A release input or output is incomplete."""


@dataclass(frozen=True)
class Identity:
    version: str
    system: str
    architecture: str
    configuration: str
    revision: str
    dirty: bool

    @property
    def stem(self) -> str:
        fields = ("CyberdyneEngine", self.version, self.system, self.architecture,
                  self.configuration)
        return "-".join(safe_name(field) for field in fields)


def run(command: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    if check and result.returncode != 0:
        raise ReleaseError(f"{' '.join(command)} failed:\n{result.stdout}{result.stderr}")
    return result


def run_live(command: list[str]) -> None:
    environment = os.environ.copy()
    if sys.version_info < (3, 11):
        modern = (shutil.which("python3.13") or shutil.which("python3.12") or
                  shutil.which("python3.11"))
        if modern is None:
            raise ReleaseError("the build workflow requires Python 3.11 or newer (tomllib)")
        with tempfile.TemporaryDirectory(prefix="cy-python-") as temporary:
            shim = Path(temporary) / executable("python3")
            shim.symlink_to(Path(modern).resolve())
            environment["PATH"] = f"{temporary}{os.pathsep}{environment.get('PATH', '')}"
            result = subprocess.run(command, cwd=ROOT, env=environment)
    else:
        result = subprocess.run(command, cwd=ROOT, env=environment)
    if result.returncode != 0:
        raise ReleaseError(f"{' '.join(command)} failed with exit {result.returncode}")


def safe_name(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "-", value.strip()).strip("-").lower()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_versions(root: Path = ROOT) -> tuple[str, str]:
    cmake = (root / "CMakeLists.txt").read_text()
    cargo = (root / "editor" / "Cargo.toml").read_text()
    engine = re.search(r"project\(CyberdyneEngine\s+VERSION\s+([^\s)]+)", cmake)
    editor = re.search(r"\[workspace\.package\].*?^version\s*=\s*\"([^\"]+)\"",
                       cargo, re.MULTILINE | re.DOTALL)
    if engine is None or editor is None:
        raise ReleaseError("the engine or editor version declaration is missing")
    return engine.group(1), editor.group(1)


def set_source_version(version: str, root: Path = ROOT) -> None:
    if SEMVER.fullmatch(version) is None:
        raise ReleaseError(f"{version!r} is not a semantic version")
    replacements = (
        (root / "CMakeLists.txt", r"(project\(CyberdyneEngine\s+VERSION\s+)[^\s)]+"),
        (root / "editor" / "Cargo.toml",
         r"(\[workspace\.package\].*?^version\s*=\s*\")[^\"]+"),
    )
    for path, pattern in replacements:
        text = path.read_text()
        changed, count = re.subn(pattern, rf"\g<1>{version}", text, count=1,
                                 flags=re.MULTILINE | re.DOTALL)
        if count != 1:
            raise ReleaseError(f"could not update the version in {path}")
        path.write_text(changed)


def git_value(*arguments: str, fallback: str = "unknown") -> str:
    result = run(["git", *arguments], check=False)
    return result.stdout.strip() if result.returncode == 0 and result.stdout.strip() else fallback


def identity(version: str, configuration: str) -> Identity:
    if SEMVER.fullmatch(version) is None:
        raise ReleaseError(f"{version!r} is not a semantic version")
    status = git_value("status", "--porcelain", fallback="")
    return Identity(version, platform.system().lower(), platform.machine().lower(), configuration,
                    git_value("rev-parse", "HEAD"), bool(status))


def copy_file(source: Path, destination: Path) -> None:
    if not source.is_file():
        raise ReleaseError(f"required release input is missing: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and sha256(source) != sha256(destination):
        raise ReleaseError(f"two release inputs map to different content at {destination}")
    shutil.copy2(source, destination)


def copy_tree(source: Path, destination: Path, ignored: set[str] | None = None) -> None:
    ignored = ignored or set()
    if not source.is_dir():
        raise ReleaseError(f"required release input is missing: {source}")
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source)
        if path.is_file() and not any(part in ignored for part in relative.parts):
            copy_file(path, destination / relative)


def common_files(stage: Path) -> None:
    for name in ("LICENSE", "THIRD_PARTY.md", "README.md"):
        copy_file(ROOT / name, stage / name)


def stage_editor(stage: Path, editor_dir: Path) -> None:
    copy_file(editor_dir / executable("cyberdyne-editor"),
              stage / "bin" / executable("cyberdyne-editor"))
    copy_file(ROOT / "editor" / "README.md", stage / "docs" / "editor.md")


def include_headers(stage: Path, build_dir: Path) -> None:
    for base in (ROOT / "src", ROOT / "platform"):
        for header in sorted(base.glob("**/include/**/*")):
            if header.is_file():
                relative = Path(*header.parts[header.parts.index("include") + 1:])
                copy_file(header, stage / "include" / relative)
    copy_tree(build_dir / "generated" / "include", stage / "include")
    copy_tree(build_dir / "generated" / "project" / "include", stage / "include")


def stage_runtime(stage: Path, build_dir: Path) -> None:
    libraries = sorted(build_dir.glob("src/**/libcy_*.a"))
    libraries += sorted(build_dir.glob("src/**/cy_*.lib"))
    libraries += sorted(build_dir.glob("platform/**/libcy_*.a"))
    if not libraries:
        raise ReleaseError(f"no runtime libraries found under {build_dir}")
    for library in libraries:
        copy_file(library, stage / "lib" / library.name)
    include_headers(stage, build_dir)
    copy_file(ROOT / "src" / "abi" / "abi_baseline.json", stage / "share" / "abi.json")


def stage_sdk(stage: Path) -> None:
    copy_tree(ROOT / "bindings" / "swift", stage / "CyberdyneKit",
              ignored={".build", ".swiftpm"})
    copy_tree(ROOT / "src" / "abi" / "include", stage / "include")
    copy_file(ROOT / "src" / "abi" / "abi_baseline.json", stage / "abi_baseline.json")


def first_existing(paths: list[Path]) -> Path:
    for path in paths:
        if path.is_file():
            return path
    raise ReleaseError("required release input is missing; tried:\n  " +
                       "\n  ".join(str(path) for path in paths))


def stage_tools(stage: Path, build_dir: Path, configuration: str) -> None:
    candidates = {
        "cy_build": [build_dir / "tools/build/cy_build"],
        "cy_cook": [build_dir / "tools/cook/cy_cook"],
        "cy_import": [build_dir / "tools/import/cy_import_cli"],
        "cy_material": [build_dir / "tools/material/cy_material"],
        "cy_shaderc": [build_dir / "tools/shaders/cy_shaderc"],
        "slangc": [build_dir / configuration / "bin/slangc"],
    }
    for installed_name, paths in candidates.items():
        copy_file(first_existing([path.with_name(executable(path.name)) for path in paths]),
                  stage / "bin" / executable(installed_name))
    library_dir = build_dir / configuration / "lib"
    if library_dir.is_dir():
        for library in sorted(library_dir.iterdir()):
            if library.is_file() and library.suffix in {".dll", ".dylib", ".so"}:
                copy_file(library, stage / "lib" / library.name)


def stage_templates(stage: Path, build_dir: Path) -> None:
    runtime = first_existing([build_dir / "samples/00-empty" / executable("cy_sample_empty")])
    copy_file(runtime, stage / "bin" / executable("cyberdyne-runtime"))
    copy_tree(ROOT / "samples" / "11-ship" / "project", stage / "project")
    copy_file(ROOT / "CMakePresets.json", stage / "CMakePresets.json")


def executable(name: str) -> str:
    return name if os.name != "nt" or name.endswith(".exe") else f"{name}.exe"


def payload_manifest(stage: Path, release: Identity, component: str) -> dict[str, object]:
    files = []
    for path in sorted(stage.rglob("*")):
        if path.is_file() and path.name != "provenance.json":
            files.append({"path": path.relative_to(stage).as_posix(), "bytes": path.stat().st_size,
                          "sha256": sha256(path)})
    return {"schema": 1, "component": component, "version": release.version,
            "platform": release.system, "architecture": release.architecture,
            "configuration": release.configuration, "revision": release.revision,
            "dirty": release.dirty, "files": files}


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def deterministic_archive(source: Path, destination: Path, root_name: str) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for path in sorted(source.rglob("*")):
                    relative = path.relative_to(source)
                    info = archive.gettarinfo(str(path), arcname=f"{root_name}/{relative.as_posix()}")
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    info.mtime = 0
                    if path.is_file():
                        with path.open("rb") as payload:
                            archive.addfile(info, payload)
                    else:
                        archive.addfile(info)


def build_release() -> None:
    run_live(["just", "build-engine", "--profile", "release", "-D", "CY_BUILD_TOOLS=ON",
              "-D", "CY_SHADER_SLANG=ON"])
    run_live(["just", "build-editor", "--profile", "release"])


def assemble(arguments: argparse.Namespace) -> Path:
    release = identity(arguments.version, arguments.configuration)
    build_dir = (ROOT / arguments.build_dir).resolve()
    editor_dir = (ROOT / arguments.editor_dir).resolve()
    output = (ROOT / arguments.output / release.version).resolve()
    if not arguments.skip_build:
        build_release()
    output.mkdir(parents=True, exist_ok=True)
    archives = []
    staging = {
        "editor": lambda path: stage_editor(path, editor_dir),
        "runtime": lambda path: stage_runtime(path, build_dir),
        "sdk": stage_sdk,
        "tools": lambda path: stage_tools(path, build_dir, arguments.configuration),
        "templates": lambda path: stage_templates(path, build_dir),
    }
    with tempfile.TemporaryDirectory(prefix="cy-release-") as temporary:
        for component in COMPONENTS:
            stage = Path(temporary) / component
            stage.mkdir()
            common_files(stage)
            staging[component](stage)
            write_json(stage / "provenance.json", payload_manifest(stage, release, component))
            filename = f"{release.stem}-{component}.tar.gz"
            archive = output / filename
            deterministic_archive(stage, archive, filename.removesuffix(".tar.gz"))
            archives.append({"component": component, "file": filename,
                             "bytes": archive.stat().st_size, "sha256": sha256(archive)})
    manifest = {"schema": 1, "version": release.version, "platform": release.system,
                "architecture": release.architecture, "configuration": release.configuration,
                "revision": release.revision, "dirty": release.dirty, "artifacts": archives}
    manifest_path = output / f"{release.stem}-manifest.json"
    write_json(manifest_path, manifest)
    print(manifest_path)
    return manifest_path


def changelog(arguments: argparse.Namespace) -> Path:
    version = arguments.version
    if SEMVER.fullmatch(version) is None:
        raise ReleaseError(f"{version!r} is not a semantic version")
    previous = arguments.since or git_value("describe", "--tags", "--abbrev=0", fallback="")
    revision_range = f"{previous}..HEAD" if previous else "HEAD"
    result = run(["git", "log", "--first-parent", "--pretty=format:%h%x09%s", revision_range])
    lines = [f"# CyberdyneEngine {version}", "", f"Revision range: `{revision_range}`", ""]
    commits = [line.split("\t", 1) for line in result.stdout.splitlines() if "\t" in line]
    commits = [(commit, subject) for commit, subject in commits
               if not subject.startswith(("WIP snapshot:", "Merge pull request"))]
    if commits:
        lines += ["## Changes", ""] + [f"- {subject} (`{commit}`)" for commit, subject in commits]
    else:
        lines += ["No changes since the previous release."]
    output = (ROOT / arguments.output / version / f"CHANGELOG-{version}.md").resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n")
    print(output)
    return output


def verify_manifest(path: Path) -> dict[str, object]:
    manifest = json.loads(path.read_text())
    for entry in manifest.get("artifacts", []):
        artifact = path.parent / entry["file"]
        if not artifact.is_file() or sha256(artifact) != entry["sha256"]:
            raise ReleaseError(f"release artifact failed verification: {artifact}")
    return manifest


def publish(arguments: argparse.Namespace) -> None:
    manifest_path = (ROOT / arguments.manifest).resolve()
    manifest = verify_manifest(manifest_path)
    tag = f"v{manifest['version']}"
    changelog_path = manifest_path.parent / f"CHANGELOG-{manifest['version']}.md"
    if not changelog_path.is_file():
        raise ReleaseError(f"release changelog is missing: {changelog_path}")
    artifacts = [str(manifest_path), str(changelog_path)]
    artifacts += [str(manifest_path.parent / entry["file"]) for entry in manifest["artifacts"]]
    command = ["gh", "release", "create", tag, *artifacts, "--title", tag,
               "--notes-file", str(changelog_path), "--draft"]
    if not arguments.execute:
        print("verified; publication command (not executed):")
        print(" ".join(command))
        return
    if arguments.confirm != tag:
        raise ReleaseError(f"publication requires --confirm {tag}")
    result = run(command)
    print(result.stdout.strip())


def version_command(arguments: argparse.Namespace) -> None:
    if arguments.set:
        set_source_version(arguments.set)
    engine, editor = source_versions()
    print(f"engine {engine}\neditor {editor}")
    if engine != editor:
        raise ReleaseError("engine and editor versions differ; run release-version --set <semver>")


def check_release(_arguments: argparse.Namespace) -> None:
    engine, editor = source_versions()
    if engine != editor:
        raise ReleaseError("engine and editor versions differ; run release-version --set <semver>")
    with tempfile.TemporaryDirectory(prefix="cy-release-check-") as temporary:
        common = {"version": engine, "output": temporary}
        changelog(argparse.Namespace(**common, since=None))
        manifest = assemble(argparse.Namespace(**common, configuration="Shipping",
                            build_dir="build/release", editor_dir="build/editor/shipping",
                            skip_build=False))
        publish(argparse.Namespace(manifest=str(manifest), execute=False, confirm=None))


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    version = commands.add_parser("version")
    version.add_argument("--set")
    version.set_defaults(function=version_command)
    notes = commands.add_parser("changelog")
    notes.add_argument("--version", required=True)
    notes.add_argument("--since")
    notes.add_argument("--output", default="dist/releases")
    notes.set_defaults(function=changelog)
    artifacts = commands.add_parser("artifacts")
    artifacts.add_argument("--version", required=True)
    artifacts.add_argument("--output", default="dist/releases")
    artifacts.add_argument("--build-dir", default="build/release")
    artifacts.add_argument("--editor-dir", default="build/editor/shipping")
    artifacts.add_argument("--configuration", default="Shipping")
    artifacts.add_argument("--skip-build", action="store_true")
    artifacts.set_defaults(function=assemble)
    upload = commands.add_parser("publish")
    upload.add_argument("--manifest", required=True)
    upload.add_argument("--execute", action="store_true")
    upload.add_argument("--confirm")
    upload.set_defaults(function=publish)
    check = commands.add_parser("check")
    check.set_defaults(function=check_release)
    return result


def main(argv: list[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    try:
        arguments.function(arguments)
    except ReleaseError as error:
        print(f"release: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Package a game into a distributable artefact for a desktop target. Issue #87.

    package.py --game rts --target linux --build-dir build/release [--out dist/packages]
    package.py --game all --target host  --build-dir build/dev

`just package <game> <target>` is the recipe; this is what it runs. One artefact per game and
target, self-contained — the executable, the libraries it loads that the target does not provide,
the cooked content cut into chunks, a signed manifest listing every file, the licences, a version
and a changelog produced by tools/release/release.py:

    linux    <Name>-<version>-linux-<arch>.tar.gz   bin/, lib/ (RUNPATH $ORIGIN), content/;
                                                    stripped, its symbols in a -symbols.tar.gz
    windows  <Name>-<version>-windows-<arch>.zip    bin/ with the executable and its DLLs
    macos    <Name>-<version>-macos-<arch>.zip      <Name>.app, ad-hoc signed; the manifest in
                                                    Contents/Resources, the executable sealed by
                                                    the bundle's code signature

THE BINARIES COME FROM ONE BUILD TREE AND ONE HOST. Nothing here cross-compiles: a target is
packaged on the operating system it runs on, out of the tree `just build-engine` wrote there, and
asking for another is refused by name. Continuous integration packages each desktop target on its
own runner from the build it already made (.github/workflows/ci.yml, `package`).

WHAT IT REFUSES. A game this tree did not build (no Swift toolchain, no renderer) is refused naming
the missing file rather than packaged without it. `--require-shipping` refuses a build tree that is
not the release profile, which is how `just release-games` keeps a development build out of a
release: a development profile links assertions and the shader compiler, and says so in the
manifest's `profile` line either way.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "release"))
sys.path.insert(0, str(ROOT / "tools" / "build"))

import release  # noqa: E402  — tools/release/release.py: the version and the changelog
import symbols  # noqa: E402  — tools/build/symbols.py: stripping, and the symbols archived

MODULE = "libCyGame_g0.so"
SYSTEM_LIBRARY_DIRS = ("/lib", "/lib64", "/usr/lib", "/usr/lib64", "/usr/local/lib")


class PackageError(RuntimeError):
    """A game cannot be packaged for this target, and why."""


@dataclass(frozen=True)
class Game:
    """What one game ships: its executable, whether it has a Swift module, and its content."""

    name: str
    title: str
    application: str
    executable: str
    directory: str
    binary: str
    swift: bool
    # Content entry name -> (where it is: "build" or "source", path below that root, required).
    content: dict[str, tuple[str, str, bool]] = field(default_factory=dict)
    blurb: str = ""


GAMES = {
    "rts": Game(
        "rts", "CyberdyneEngine RTS", "CyberdyneRTS", "cyberdyne-rts", "13-rts-api",
        "cy_sample_rts-api", True,
        {"frontier.cypak": ("build", "samples/13-rts-api/frontier.cypak", True),
         "module.toml": ("source", "samples/13-rts-api/module.toml", True)},
        "The RTS battle on The Frontier: blue's army is yours, red's is the opponent's."),
    "tps": Game(
        "tps", "CyberdyneEngine Third Person", "CyberdyneTPS", "cyberdyne-tps",
        "14-third-person", "cy_sample_third-person", True,
        {"hillside.cypak": ("build", "samples/14-third-person/hillside.cypak", True),
         "hillside_wind.cyaudio": ("build", "samples/14-third-person/hillside_wind.cyaudio", False),
         "module.toml": ("source", "samples/14-third-person/module.toml", True)},
        "The third-person hillside: run, jump and climb with a camera behind you."),
    "fps": Game(
        "fps", "CyberdyneEngine First Person", "CyberdyneFPS", "cyberdyne-fps",
        "15-first-person", "cy_sample_first-person", True,
        {"module.toml": ("source", "samples/15-first-person/module.toml", True),
         **{f"content/beauty/textures/{name}_albedo.png":
            ("source", f"content/beauty/textures/{name}_albedo.png", True)
            for name in ("stone", "gravel", "copper")}},
        "The first-person arena: targets to shoot down, a magazine to manage."),
    "ship": Game(
        "ship", "CyberdyneEngine Ship Card", "CyberdyneShip", "cyberdyne-ship", "11-ship",
        "cy_sample_ship", False, {},
        "samples/11-ship's card: a packaged project drawn in a window. Not a game."),
}

TARGETS = {"linux": "Linux", "windows": "Windows", "macos": "Darwin"}


def host_target() -> str:
    for name, system in TARGETS.items():
        if platform.system() == system:
            return name
    raise PackageError(f"this host ({platform.system()}) is not a desktop target")


def architecture() -> str:
    machine = platform.machine().lower()
    return {"amd64": "x86_64", "aarch64": "arm64"}.get(machine, machine)


def exe(name: str, target: str) -> str:
    return f"{name}.exe" if target == "windows" else name


def run(command: list, **kwargs) -> subprocess.CompletedProcess:
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True,
                            **kwargs)
    if result.returncode != 0:
        raise PackageError(f"{Path(str(command[0])).name} failed:\n{result.stdout}{result.stderr}")
    return result


def cmake_value(build_dir: Path, name: str) -> str:
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        match = re.search(rf"^{re.escape(name)}:[A-Z]+=(.*)$", cache.read_text(), re.MULTILINE)
        if match:
            return match.group(1)
    return ""


@dataclass
class Build:
    """The tree the binaries come from, and what it says about itself."""

    directory: Path
    profile: str
    version: str
    revision: str

    @staticmethod
    def of(directory: Path) -> "Build":
        if not (directory / "CMakeCache.txt").is_file():
            raise PackageError(f"{directory} is not a configured build tree")
        engine, _editor = release.source_versions()
        revision = release.git_value("rev-parse", "HEAD")
        return Build(directory, cmake_value(directory, "CY_PROFILE") or "unknown", engine,
                     revision)

    def tool(self, target: str) -> Path:
        found = self.directory / "tools" / "ship" / exe("cy_ship", target)
        if not found.is_file():
            raise PackageError(f"{found} was not built (CY_BUILD_TOOLS)")
        return found


@dataclass
class Staged:
    """Where a game is laid out: the installation root, and its content outside it."""

    root: Path
    content: Path
    manifest: str
    sealed: list[str]
    bundle: Path | None = None


def stage_documents(game: Game, build: Build, target: str, documents: Path) -> None:
    documents.mkdir(parents=True, exist_ok=True)
    for name in ("LICENSE", "THIRD_PARTY.md"):
        shutil.copyfile(ROOT / name, documents / name)
    with tempfile.TemporaryDirectory(prefix="cy-changelog-") as scratch:
        written = release.changelog(argparse.Namespace(version=build.version, since=None,
                                                       output=scratch))
        shutil.copyfile(written, documents / "CHANGELOG.md")
    (documents / "README.txt").write_text(readme(game, build, target))


def readme(game: Game, build: Build, target: str) -> str:
    run_line = {"linux": f"./bin/{game.executable}",
                "windows": f"bin\\{game.executable}.exe",
                "macos": f"open {game.application}.app"}[target]
    vulkan = {
        "linux": "It draws with Vulkan 1.3: it loads the system's Vulkan loader (libvulkan.so.1, "
                 "package `libvulkan1`) and your GPU driver's ICD, neither of which it carries. "
                 "`vulkaninfo --summary` says whether the machine has both.",
        "windows": "It draws with Vulkan 1.3 through the loader your GPU driver installs "
                   "(vulkan-1.dll).",
        "macos": "This build has no Metal picture yet: it plays headless or reports the window "
                 "it could not draw.",
    }[target]
    return (f"{game.title} {build.version} ({target}-{architecture()}, {build.profile} profile)\n"
            f"\n{game.blurb}\n\nRun it: {run_line}\n\n{vulkan}\n\n"
            "At start it verifies every file against the signed manifest (ship.manifest) and "
            "refuses to run, naming the file, if one is missing, modified or not listed. Its "
            "settings file, crash reports and unpacked content live in your user directories; "
            "the first line it prints names them.\n\nCHANGELOG.md is what changed. LICENSE and "
            "THIRD_PARTY.md are the licences of the engine and of what it is built from.\n")


def stage_content(game: Game, build: Build, content: Path, target: str) -> None:
    for name, (where, relative, required) in game.content.items():
        source = (build.directory if where == "build" else ROOT) / relative
        if not source.is_file():
            if required:
                raise PackageError(f"{game.name}: {source} was not built")
            continue
        (content / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, content / name)
    if game.name == "ship":
        install_ship_card(build, content / "install", target)


def install_ship_card(build: Build, out: Path, target: str) -> None:
    """samples/11-ship's card, built, packaged and installed by cy_build — its content."""
    cy_build = build.directory / "tools" / "build" / exe("cy_build", target)
    if not cy_build.is_file():
        raise PackageError(f"{cy_build} was not built")
    with tempfile.TemporaryDirectory(prefix="cy-ship-card-") as scratch:
        work = Path(scratch)
        project = work / "project"
        shutil.copytree(ROOT / "samples" / "11-ship" / "project", project)
        package = work / "base.cypackage"
        run([cy_build, "build", "--description", project / "build" / "ship.cybuild",
             "--project", project, "--out", work / "artefacts", "--cache", work / "cache",
             "--package", package, "--platform", "host", "--profile", "client",
             "--revision", build.revision, "--engine-revision", build.revision])
        run([cy_build, "install", "--package", package, "--artefacts", work / "artefacts",
             "--install", out])
    for path in out.rglob("*"):
        if path.is_file():
            path.chmod(0o644)


def strip_into(binary: Path, shipped: Path, symbol_store: Path, build: Build) -> None:
    """ELF: stripped, its symbols archived by build-id. Elsewhere: copied as built."""
    if symbols.binary_format(binary) == "ELF":
        try:
            symbols.split(binary, shipped, symbol_store, build.revision)
            return
        except symbols.SymbolError as refusal:
            print(f"package: {binary.name} is shipped unstripped: {refusal}")
    shipped.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binary, shipped)


def linux_runtime_libraries(module: Path) -> list[Path]:
    """What the module needs that a Linux system does not provide: the Swift runtime."""
    listed = run(["ldd", module]).stdout
    found = []
    for line in listed.splitlines():
        match = re.match(r"\s*(\S+) => (/\S+)", line)
        if match and not match.group(2).startswith(SYSTEM_LIBRARY_DIRS):
            found.append(Path(match.group(2)))
        elif "not found" in line:
            raise PackageError(f"{module.name} needs {line.split()[0]}, which ldd cannot find")
    return found


def stage_linux(game: Game, build: Build, root: Path, symbol_store: Path) -> None:
    sample = build.directory / "samples" / game.directory
    strip_into(sample / game.binary, root / "bin" / game.executable, symbol_store, build)
    if not game.swift:
        return
    module = sample / "module" / MODULE
    if not module.is_file():
        raise PackageError(f"{game.name}: {module} was not built (it needs Swift)")
    lib = root / "lib"
    strip_into(module, lib / MODULE, symbol_store, build)
    # A Shipping module carries its Swift runtime (samples/CMakeLists.txt) and needs nothing here.
    # A development one loads the toolchain's, which travel with it and are found beside it.
    runtime = linux_runtime_libraries(module)
    if not runtime:
        return
    if shutil.which("patchelf") is None:
        raise PackageError(f"{module.name} loads the Swift runtime dynamically, and patchelf is "
                           "required to point it beside the module (apt install patchelf); a "
                           "release-profile module carries its runtime and needs neither")
    for library in runtime:
        shutil.copyfile(library, lib / library.name)
    for library in lib.iterdir():
        library.chmod(0o755)
        run(["patchelf", "--set-rpath", "$ORIGIN", library])


def stage_windows(game: Game, build: Build, root: Path) -> None:
    sample = build.directory / "samples" / game.directory
    built = sample / exe(game.binary, "windows")
    if not built.is_file():
        raise PackageError(f"{game.name}: {built} was not built")
    bin_dir = root / "bin"
    bin_dir.mkdir(parents=True)
    shutil.copy2(built, bin_dir / exe(game.executable, "windows"))
    for library in sample.glob("*.dll"):
        shutil.copy2(library, bin_dir / library.name)
    if game.swift:
        module = sample / "module" / MODULE
        if not module.is_file():
            raise PackageError(f"{game.name}: {module} was not built (it needs Swift)")
        shutil.copy2(module, bin_dir / MODULE)


INFO_PLIST = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>{executable}</string>
  <key>CFBundleIdentifier</key><string>com.cyberdyne.engine.{game}</string>
  <key>CFBundleName</key><string>{application}</string>
  <key>CFBundleDisplayName</key><string>{title}</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>{version}</string>
  <key>CFBundleVersion</key><string>{version}</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
"""


def codesign(path: Path) -> None:
    run(["codesign", "--force", "--sign", "-", "--timestamp=none", path])


def stage_macos(game: Game, build: Build, bundle: Path) -> None:
    sample = build.directory / "samples" / game.directory
    contents = bundle / "Contents"
    built = sample / game.binary
    if not built.is_file():
        raise PackageError(f"{game.name}: {built} was not built")
    (contents / "MacOS").mkdir(parents=True)
    shutil.copy2(built, contents / "MacOS" / game.executable)
    (contents / "Info.plist").write_text(INFO_PLIST.format(
        executable=game.executable, game=game.name, application=game.application,
        title=game.title, version=build.version))
    if game.swift:
        module = sample / "module" / MODULE
        if not module.is_file():
            raise PackageError(f"{game.name}: {module} was not built (it needs Swift)")
        (contents / "Frameworks").mkdir()
        shutil.copy2(module, contents / "Frameworks" / MODULE)
        # Signed BEFORE the manifest is written: signing rewrites the file, and the manifest pins
        # the bytes that ship. The bundle's own seal comes after (package_macos).
        codesign(contents / "Frameworks" / MODULE)


def stage(game: Game, build: Build, target: str, work: Path, name: str,
          symbol_store: Path) -> Staged:
    content = work / "content"
    content.mkdir(parents=True)
    stage_content(game, build, content, target)
    if target == "macos":
        bundle = work / f"{game.application}.app"
        stage_macos(game, build, bundle)
        root = bundle / "Contents"
        stage_documents(game, build, target, root / "Resources")
        return Staged(root, content, "Resources/ship.manifest",
                      [f"MacOS/{game.executable}"], bundle)
    root = work / name
    if target == "windows":
        stage_windows(game, build, root)
    else:
        stage_linux(game, build, root, symbol_store)
    stage_documents(game, build, target, root)
    return Staged(root, content, "ship.manifest", [])


def pack(staged: Staged, game: Game, build: Build, target: str, key: str | None) -> str:
    command = [build.tool(host_target()), "pack", "--root", staged.root,
               "--content", staged.content, "--manifest", staged.manifest,
               "--game", game.name, "--title", game.title, "--version", build.version,
               "--revision", build.revision, "--target", f"{target}-{architecture()}",
               "--profile", build.profile]
    for sealed in staged.sealed:
        command += ["--sealed", sealed]
    if key is not None:
        command += ["--key", key]
    return run(command).stdout.strip()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def deterministic_zip(source: Path, destination: Path, root_name: str) -> None:
    with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(source.rglob("*")):
            if path.is_file():
                info = zipfile.ZipInfo(f"{root_name}/{path.relative_to(source).as_posix()}",
                                       date_time=(1980, 1, 1, 0, 0, 0))
                info.external_attr = (0o755 if os.access(path, os.X_OK) else 0o644) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, path.read_bytes())


def archive(staged: Staged, target: str, out: Path, name: str) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    if target == "linux":
        artefact = out / f"{name}.tar.gz"
        release.deterministic_archive(staged.root, artefact, name)
    elif target == "windows":
        artefact = out / f"{name}.zip"
        deterministic_zip(staged.root, artefact, name)
    else:
        assert staged.bundle is not None
        codesign(staged.bundle)
        run(["codesign", "--verify", "--deep", "--strict", staged.bundle])
        artefact = out / f"{name}.zip"
        artefact.unlink(missing_ok=True)
        run(["ditto", "-c", "-k", "--keepParent", staged.bundle, artefact])
    return artefact


def package(game: Game, target: str, build: Build, out: Path, key: str | None) -> Path:
    name = f"{game.application}-{build.version}-{target}-{architecture()}"
    with tempfile.TemporaryDirectory(prefix=f"cy-package-{game.name}-") as scratch:
        work = Path(scratch)
        symbol_store = work / "symbols"
        staged = stage(game, build, target, work, name, symbol_store)
        packed = pack(staged, game, build, target, key)
        artefact = archive(staged, target, out, name)
        if symbol_store.is_dir():
            release.deterministic_archive(symbol_store, out / f"{name}-symbols.tar.gz",
                                          f"{name}-symbols")
    (out / f"{artefact.name}.sha256").write_text(f"{sha256(artefact)}  {artefact.name}\n")
    print(f"package {game.name} {target}-{architecture()} profile={build.profile} "
          f"bytes={artefact.stat().st_size} {packed.removeprefix('cy_ship pack ')} "
          f"artefact={artefact}")
    return artefact


def games_for(name: str, build: Build) -> list[Game]:
    """`all` is every game this tree built; a named one must have been."""
    if name != "all":
        if name not in GAMES:
            raise PackageError(f"no game '{name}'; the games are {', '.join(GAMES)} and all")
        return [GAMES[name]]
    built = [game for game in GAMES.values()
             if (build.directory / "samples" / game.directory /
                 exe(game.binary, host_target())).is_file()]
    if not built:
        raise PackageError(f"{build.directory} built none of the games")
    return built


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", required=True, help=f"{', '.join(GAMES)} or all")
    parser.add_argument("--target", default="host", help="linux, windows, macos or host")
    parser.add_argument("--build-dir", default="build/release")
    parser.add_argument("--out", default="dist/packages")
    parser.add_argument("--key", help="the secret key file; default CY_SHIP_SIGNING_KEY or the "
                                      "development key")
    parser.add_argument("--require-shipping", action="store_true",
                        help="refuse a build tree that is not the release profile")
    arguments = parser.parse_args(argv)
    try:
        target = host_target() if arguments.target == "host" else arguments.target
        if target not in TARGETS:
            raise PackageError(f"no target '{target}'; the targets are {', '.join(TARGETS)}")
        if target != host_target():
            raise PackageError(f"a {target} artefact is packaged on {target}: this host is "
                               f"{host_target()}, and its binaries are {host_target()} binaries")
        build = Build.of((ROOT / arguments.build_dir).resolve())
        if arguments.require_shipping and build.profile != "release":
            raise PackageError(f"{build.directory} is the {build.profile} profile, and a release "
                               "ships the release profile")
        for game in games_for(arguments.game, build):
            package(game, target, build, (ROOT / arguments.out).resolve(), arguments.key)
    except (PackageError, release.ReleaseError) as refusal:
        print(f"package: {refusal}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

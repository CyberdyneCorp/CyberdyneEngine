#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""smoke.ship_games — every game this tree built, packaged, unpacked somewhere else and launched.

`just package-test` runs it by itself; CTest runs it as smoke.ship_games. For each game:

  1. PACKAGE     `tools/ship/package.py` writes the host target's artefact.
  2. A CLEAN DIRECTORY. The artefact is unpacked into a fresh system temporary directory — no
                 source tree above it, no build tree beside it — and run there with an environment
                 holding nothing but PATH and the user directory, so a run that reached back into
                 this checkout for a module, a map or a texture has nothing to find. Where `strace`
                 exists the run is traced, and any file it opens under the checkout is a failure
                 naming the file: the binaries still carry the build tree's paths, and only a trace
                 can say none of them was used.
  3. THE SAME DIGESTS. The scripted run is made once from the build tree and once from the
                 unpacked artefact, and every `name=<16 hex digits>` the two print — the lockstep
                 battle's, the fog's, the map's, the session's — must agree, in order.
     The first-person game reads its textures only when it draws, so on a machine with a device
                 it also draws twenty frames, traced the same way.
     Played headless, each game writes its settings file and accepts every flag it becomes.
  4. EVERY FILE LISTED. Every file the archive holds is in the signed manifest, or is the manifest.
  5. TAMPERED    one byte of a content chunk flipped: the game does not start, exits 3, and names
                 the chunk. A deleted file is named the same way, and so is a file added beside the
                 executable.
  6. A CRASH     `--ship-crash` takes the packaged game down with SIGSEGV after its handler is
                 installed, and a report lands in the user's crash directory (POSIX hosts).

It reports through samples/harness/artefact.py, so a step it cannot evaluate here is not a pass.
Exit 77 (CTest's skip) when this tree built none of the games.
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "samples" / "harness"))
sys.path.insert(0, str(ROOT / "tools" / "ship"))

import artefact  # noqa: E402
import package  # noqa: E402
from artefact import Failed, Report, Statistic, expect  # noqa: E402

DIGEST = re.compile(r"\b([a-z_]+)=([0-9a-f]{16})\b")
LAUNCH = re.compile(r"^ship launch .*\bstartup_ms=([0-9.]+)", re.MULTILINE)
SKIP = 77


@dataclass(frozen=True)
class Scripted:
    """The same deterministic run, spelled for the build tree and for the artefact."""

    packaged: tuple[str, ...]
    development: tuple[str, ...]
    seconds: int = 240


def scripted(game: package.Game, build_dir: Path) -> Scripted:
    if game.name == "rts":
        # Packaged, the map is the game's own; from the build tree it is named.
        return Scripted(("--battle",),
                        ("--battle", "--map",
                         str(build_dir / "samples" / "13-rts-api" / "frontier.cypak")))
    if game.name == "tps":
        return Scripted(("--frames", "1200"), ("--frames", "1200"))
    if game.name == "fps":
        return Scripted(("--headless",), ("--headless",))
    return Scripted(("--platform", "headless", "--frames", "1"), ())


def strace() -> str | None:
    """Linux's strace. Git for Windows ships an `strace` of its own that traces nothing here."""
    return shutil.which("strace") if platform.system() == "Linux" else None


def clean_environment(user: Path, traced: bool = False) -> dict[str, str]:
    """The environment a player's machine gives the game; `traced` for a run under strace.

    LeakSanitizer stops the process's threads with ptrace at exit, and a process strace already
    traces cannot be attached to again: a sanitized tree's traced run ended in "LeakSanitizer has
    encountered a fatal error" and a non-zero exit, whatever the game did. So a traced run checks
    no leaks — the same binary's untraced runs still do. The variable is inert in a tree with no
    sanitizer.
    """
    environment = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "CY_SHIP_USER_DIR": str(user),
                   "SDL_VIDEO_DRIVER": "dummy", "HOME": str(user)}
    for name in ("SYSTEMROOT", "TEMP", "TMP", "TMPDIR"):
        if name in os.environ:
            environment[name] = os.environ[name]
    if traced:
        environment["ASAN_OPTIONS"] = "detect_leaks=0"
    return environment


def unpack(artefact_path: Path, into: Path) -> Path:
    if artefact_path.name.endswith(".tar.gz"):
        with tarfile.open(artefact_path) as archive:
            if sys.version_info >= (3, 12):
                archive.extractall(into, filter="tar")
            else:
                archive.extractall(into)
    else:
        with zipfile.ZipFile(artefact_path) as archive:
            for member in archive.infolist():
                extracted = Path(archive.extract(member, into))
                mode = member.external_attr >> 16
                if mode:
                    extracted.chmod(mode)
    roots = [path for path in into.iterdir()]
    expect(len(roots) == 1, f"{artefact_path.name} does not unpack to one directory: {roots}")
    return roots[0]


def executable_of(root: Path, game: package.Game) -> Path:
    if root.suffix == ".app":
        return root / "Contents" / "MacOS" / game.executable
    return root / "bin" / package.exe(game.executable, package.host_target())


def installation_root(root: Path) -> tuple[Path, str]:
    if root.suffix == ".app":
        return root / "Contents", "Resources/ship.manifest"
    return root, "ship.manifest"


def run(command: list, environment: dict[str, str] | None, cwd: Path,
        seconds: int) -> subprocess.CompletedProcess:
    return subprocess.run([str(part) for part in command], capture_output=True, text=True,
                          env=environment, cwd=str(cwd), timeout=seconds)


def digests(text: str) -> list[tuple[str, str]]:
    """The game's digests: every line but the launch's own, which names the signing key."""
    return [pair for line in text.splitlines() if not line.startswith("ship launch ")
            for pair in DIGEST.findall(line)]


class GameRun:
    """One game's acts, against one artefact."""

    def __init__(self, game: package.Game, build_dir: Path, artefact_path: Path, report: Report):
        self.game = game
        self.build_dir = build_dir
        self.artefact = artefact_path
        self.report = report
        self.scripted = scripted(game, build_dir)

    def development_run(self, cwd: Path) -> str:
        binary = self.build_dir / "samples" / self.game.directory / package.exe(
            self.game.binary, package.host_target())
        arguments = list(self.scripted.development)
        if self.game.name == "ship":
            return ""
        result = run([binary, *arguments], None, cwd, self.scripted.seconds)
        expect(result.returncode == 0,
               f"{self.game.name}: the build tree's run failed ({result.returncode}):\n"
               f"{result.stderr[-2000:]}")
        return result.stdout

    def packaged_run(self, root: Path, user: Path, extra: tuple[str, ...] = (),
                     trace: Path | None = None) -> subprocess.CompletedProcess:
        command = [executable_of(root, self.game), *self.scripted.packaged, *extra]
        if trace is not None:
            command = ["strace", "-f", "-qq", "-e", "trace=open,openat,execve", "-o", trace,
                       *command]
        return run(command, clean_environment(user, traced=trace is not None), root.parent,
                   self.scripted.seconds)

    def act_digests(self, root: Path, user: Path, scratch: Path) -> float:
        trace = scratch / "trace.txt" if strace() else None
        packaged = self.packaged_run(root, user, trace=trace)
        expect(packaged.returncode == 0,
               f"{self.game.name}: the packaged run failed ({packaged.returncode}):\n"
               f"{packaged.stdout[-1500:]}{packaged.stderr[-1500:]}")
        launch = LAUNCH.search(packaged.stdout)
        expect(launch is not None, f"{self.game.name}: the packaged run printed no `ship launch`")
        self.check_trace(trace)
        reference = self.development_run(scratch)
        if self.game.name == "ship":
            card = [line for line in packaged.stdout.splitlines() if "verified" in line]
            expect(card, "the packaged card did not verify its own installation")
            self.report.did("ship: the packaged card opens and verifies the installation it "
                            "carries", card[0].strip())
        else:
            ours, theirs = digests(packaged.stdout), digests(reference)
            expect(ours, f"{self.game.name}: the scripted run printed no digest")
            expect(ours == theirs,
                   f"{self.game.name}: the packaged run's digests differ from the build tree's:\n"
                   f"  packaged {ours}\n  tree     {theirs}")
            self.report.did(f"{self.game.name}: the packaged scripted run ends in the build tree's "
                            f"digests", f"{len(ours)} digests, e.g. {ours[0][0]}={ours[0][1]}")
        return float(launch.group(1))

    def check_trace(self, trace: Path | None) -> None:
        if trace is None:
            self.report.not_evaluated(f"{self.game.name}: no file under the checkout is opened",
                                      "Linux's strace is not on this machine")
            return
        opened = re.findall(r'(?:open|openat|execve)\([^"]*"([^"]+)"', trace.read_text())
        reached = sorted({path for path in opened if path.startswith(str(ROOT) + os.sep)})
        expect(not reached, f"{self.game.name}: the packaged run opened files in the checkout:\n  "
               + "\n  ".join(reached[:10]))
        self.report.did(f"{self.game.name}: the packaged run opens nothing under the checkout",
                        f"{len(opened)} opens traced")

    def act_drawn(self, root: Path, user: Path, scratch: Path) -> None:
        """The first-person game reads its textures only when it draws: twenty frames on a device,
        traced, so the textures it opens are the artefact's and none of them the checkout's."""
        if self.game.name != "fps":
            return
        if strace() is None:
            self.report.not_evaluated("fps: a drawn run reads its textures from the artefact",
                                      "Linux's strace is not on this machine")
            return
        trace = scratch / "drawn-trace.txt"
        frames = scratch / "drawn"
        frames.mkdir()
        command = ["strace", "-f", "-qq", "-e", "trace=open,openat", "-o", trace,
                   executable_of(root, self.game), "--render", frames, "--frames", "20",
                   "--width", "320", "--height", "180", "--no-validation"]
        environment = clean_environment(user, traced=True)
        environment.pop("SDL_VIDEO_DRIVER")
        result = run(command, environment, root.parent, self.scripted.seconds)
        if "fps render" not in result.stdout:
            self.report.not_evaluated("fps: a drawn run reads its textures from the artefact",
                                      "no graphics device drew the frames")
            return
        expect(result.returncode == 0, f"fps: the drawn run failed:\n{result.stderr[-1500:]}")
        opened = re.findall(r'open(?:at)?\([^"]*"([^"]+_albedo\.png)"', trace.read_text())
        expect(opened, "fps: the drawn run opened no texture")
        outside = [path for path in opened if path.startswith(str(ROOT) + os.sep)]
        expect(not outside, f"fps: the drawn run read textures from the checkout: {outside}")
        self.report.did("fps: a drawn run reads its textures from the artefact's content",
                        f"{len(opened)} textures, none under the checkout")

    def act_played(self, root: Path, scratch: Path) -> None:
        """Played headless (SDL's events, no window): the settings file is written on the first
        launch and its every flag is one the game itself accepts — a setting the game refused would
        stop a double-clicked game from starting."""
        if self.game.name == "ship":
            return
        user = scratch / "played-user"
        command = [executable_of(root, self.game), "--play", "--headless", "--frames", "30"]
        result = run(command, clean_environment(user), root.parent, self.scripted.seconds)
        expect(result.returncode == 0, f"{self.game.name}: played with its settings, it failed "
               f"({result.returncode}):\n{result.stderr[-1500:]}")
        settings = next(user.rglob("settings.cfg"), None)
        expect(settings is not None, f"{self.game.name}: the first launch wrote no settings file")
        keys = [line.split("=")[0].strip() for line in settings.read_text().splitlines()
                if "=" in line and not line.startswith("#")]
        self.report.did(f"{self.game.name}: the settings file is written, and the game takes "
                        "every flag it becomes", ", ".join(keys))

    def act_listing(self, root: Path) -> None:
        installation, manifest = installation_root(root)
        text = (installation / manifest).read_text()
        listed = {line.split(" ", 1)[1] if line.startswith("sealed ") else line.split(" ", 3)[3]
                  for line in text.splitlines() if line.startswith(("file ", "chunk ", "sealed "))}
        listed |= {manifest, manifest + ".sig"}
        present = {path.relative_to(installation).as_posix() for path in installation.rglob("*")
                   if path.is_file() and not path.relative_to(installation).as_posix()
                   .startswith("_CodeSignature/")}
        expect(present == listed, f"{self.game.name}: the manifest and the artefact disagree:\n"
               f"  not listed {sorted(present - listed)}\n  absent {sorted(listed - present)}")
        self.report.did(f"{self.game.name}: the signed manifest lists every file the artefact "
                        "holds", f"{len(present)} files")

    def refused(self, root: Path, user: Path, needle: str, what: str) -> None:
        result = self.packaged_run(root, user)
        expect(result.returncode == 3, f"{self.game.name}: {what}, and the game started "
               f"(exit {result.returncode})")
        expect(needle in result.stderr,
               f"{self.game.name}: {what}, and the refusal does not name it:\n{result.stderr}")
        self.report.did(f"{self.game.name}: {what}, and it is refused by name",
                        result.stderr.strip().splitlines()[-1].removeprefix("ship:   "))

    def act_tampering(self, root: Path, scratch: Path) -> None:
        user = scratch / "tampered-user"
        installation, _ = installation_root(root)
        chunks = sorted(installation.rglob("chunk-*.cychunk"))
        expect(chunks, f"{self.game.name}: the artefact has no content chunks")
        chunk = chunks[0]
        original = chunk.read_bytes()
        chunk.write_bytes(original[:100] + bytes([original[100] ^ 0x20]) + original[101:])
        relative = chunk.relative_to(installation).as_posix()
        self.refused(root, user, f"{relative} was modified", "a chunk is tampered with")
        chunk.unlink()
        self.refused(root, user, f"{relative} is missing", "a chunk is missing")
        chunk.write_bytes(original)
        planted = executable_of(root, self.game).parent / "libplanted.so"
        planted.write_bytes(b"not ours")
        self.refused(root, user, "libplanted.so is not listed", "a library is planted beside it")
        planted.unlink()

    def act_crash(self, root: Path, scratch: Path) -> None:
        if os.name != "posix":
            self.report.not_evaluated(f"{self.game.name}: a crash is reported",
                                      "the forced crash is a POSIX signal")
            return
        user = scratch / "crash-user"
        result = self.packaged_run(root, user, extra=("--ship-crash",))
        reports = list(user.rglob("crash-*.txt"))
        expect(result.returncode != 0, f"{self.game.name}: --ship-crash did not crash")
        expect(reports, f"{self.game.name}: the crash wrote no report under {user}")
        text = reports[0].read_text(errors="replace")
        expect("SIGSEGV" in text, f"{self.game.name}: the report does not name the signal")
        self.report.did(f"{self.game.name}: a crash writes a report to the user's crash directory",
                        f"{reports[0].relative_to(user)}")


def act_shipping_check(build_dir: Path, report: Report) -> None:
    """`--require-shipping`'s symbol check, against a binary that does link the Slang compiler."""
    shaderc = build_dir / "tools" / "shaders" / package.exe("cy_shaderc", package.host_target())
    if os.name != "posix" or not shaderc.is_file() or shutil.which("nm") is None:
        report.not_evaluated("the shipping check refuses a binary that links the shader compiler",
                             "no cy_shaderc in this tree, or no nm")
        return
    found = package.development_code(shaderc)
    expect(found is not None, "the shipping check passed cy_shaderc, which links Slang")
    report.did("the shipping check refuses a binary that links the shader compiler",
               f"cy_shaderc: {found!r}")


def package_games(build_dir: Path, out: Path, report: Report) -> list[tuple[package.Game, Path]]:
    build = package.Build.of(build_dir)
    made = []
    for game in package.games_for("all", build):
        try:
            made.append((game, package.package(game, package.host_target(), build, out, None)))
        except package.PackageError as refusal:
            report.gap(f"{game.name}: packaged", str(refusal))
    return made


def act_game(game: package.Game, artefact_path: Path, build_dir: Path, report: Report) -> float:
    print(f"\n==> {game.name} — {artefact_path.name}, {artefact_path.stat().st_size} bytes")
    with tempfile.TemporaryDirectory(prefix=f"cy-shipped-{game.name}-") as scratch_name:
        scratch = Path(scratch_name)
        root = unpack(artefact_path, scratch / "unpacked")
        acts = GameRun(game, build_dir, artefact_path, report)
        acts.act_listing(root)
        startup = acts.act_digests(root, scratch / "user", scratch)
        acts.act_drawn(root, scratch / "user", scratch)
        acts.act_played(root, scratch)
        acts.act_tampering(root, scratch)
        acts.act_crash(root, scratch)
        return startup


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--work", required=True)
    arguments = parser.parse_args(argv)
    build_dir = Path(arguments.build_dir).resolve()
    work = Path(arguments.work).resolve()
    shutil.rmtree(work, ignore_errors=True)
    report = Report()
    try:
        act_shipping_check(build_dir, report)
        made = package_games(build_dir, work / "dist", report)
        if not made:
            print("smoke.ship_games: this tree built none of the games")
            return SKIP
        startups = []
        for game, artefact_path in made:
            startups.append(act_game(game, artefact_path, build_dir, report))
            report.figure(Statistic.stable(f"{game.name} artefact total",
                                           artefact_path.stat().st_size / 1e6, "MB"))
        report.headline(Statistic.median("median packaged startup", startups, "ms"))
    except (Failed, package.PackageError, subprocess.TimeoutExpired) as failure:
        print(f"\nFAILED  {failure}")
        report.failed(str(failure))
    return report.summarise(work)


if __name__ == "__main__":
    raise SystemExit(main())

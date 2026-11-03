#!/usr/bin/env python3
"""Tests for the dependency gates: the licence policy, and the record's agreement with Cargo.lock.

WHY THIS FILE EXISTS AT ALL. `just maintenance-deps-check` reported green through the whole of M5.5
while 386 third-party crates entered the tree undeclared, because the check compared THIRD_PARTY.md
against the two C and C++ manifests and the crates were in neither. A gate that cannot fail is
indistinguishable from a tree with nothing wrong in it, and the only way to tell them apart is to
show the gate failing on purpose. So each check below is exercised against a fixture that MUST be
rejected — the undeclared crate first, because that is the one that actually happened.

It is the same arrangement as `tools/layercheck/selftest.py`, and it runs from the same recipe as
the gate it tests, in a fraction of a second, so there is no configuration in which the gate ships
without its fixtures.

Run directly, or through `just maintenance-deps-check`.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import manifest as manifest_module  # noqa: E402
import rust_crates  # noqa: E402

_cases: list[str] = []
_failures: list[str] = []


def check(name: str, condition: bool, detail: str = "") -> None:
    print(f"{'ok  ' if condition else 'FAIL'} {name}")
    _cases.append(name)
    if not condition:
        _failures.append(name)
        for line in detail.strip().splitlines():
            print(f"     | {line}", file=sys.stderr)


def crate(name: str = "left-pad", version: str = "1.0.0", licence: str = "MIT",
          checksum: str = "abc", direct: bool = False, used_by=(), roots=("eframe",),
          justification: str = "reached through eframe") -> rust_crates.Crate:
    return rust_crates.Crate(name=name, version=version, licence=licence, checksum=checksum,
                             direct=direct, used_by=tuple(used_by), roots=tuple(roots),
                             justification=justification)


def locked(name: str = "left-pad", version: str = "1.0.0",
           checksum: str = "abc") -> rust_crates.Locked:
    return rust_crates.Locked(name=name, version=version, checksum=checksum,
                              source="registry+https://github.com/rust-lang/crates.io-index")


def from_git(name: str = "left-pad", version: str = "1.0.0") -> rust_crates.Locked:
    return rust_crates.Locked(name=name, version=version, checksum="",
                              source="git+https://example.invalid/left-pad#deadbeef")


def named(complaints: list[str], fragment: str) -> bool:
    return any(fragment in complaint for complaint in complaints)


# --- The failure that happened --------------------------------------------------------------------


def test_lockfile_agreement() -> None:
    """A crate Cargo acquires and nobody declared fails the gate, by name."""
    check("a declared crate that the lockfile pins is accepted",
          not rust_crates.check([crate()], [locked()]))

    undeclared = rust_crates.check([], [locked(name="serde", version="1.0.0")])
    check("THE M5.5 FAILURE: a crate in Cargo.lock and in no record fails, naming it",
          named(undeclared, "serde 1.0.0"), "\n".join(undeclared) or "no complaint was made")

    stale = rust_crates.check([crate(name="serde")], [])
    check("a declaration the lockfile no longer pins fails, so a removal is noticed too",
          named(stale, "serde"), "\n".join(stale))

    moved = rust_crates.check([crate(version="1.0.0")], [locked(version="1.0.1")])
    check("a version the lockfile has moved past fails as one undeclared and one stale entry",
          len(moved) == 2, "\n".join(moved))

    forked = rust_crates.check([crate(checksum="0000")], [locked(checksum="ffff")])
    check("a checksum that disagrees with the lockfile fails, so the pin cannot be forked",
          named(forked, "not a pin"), "\n".join(forked))

    # A git source has no registry checksum, so a record could attest to nothing about it — and a
    # branch is not a pin, which is the same rule deps/manifest.toml states for a fetched C++
    # dependency's commit.
    git = rust_crates.check([crate(checksum="")], [from_git()])
    check("a crate from a git source rather than crates.io fails, naming the source",
          named(git, "rather than crates.io"), "\n".join(git))


def test_policy() -> None:
    """The two facts Cargo does not record: a licence identifier, and why the crate is there."""
    check("a crate with no licence identifier fails",
          named(rust_crates.check([crate(licence="")], [locked()]), "no licence identifier"))
    check("a crate with no justification fails",
          named(rust_crates.check([crate(justification="")], [locked()]), "no justification"))
    check("a transitive crate that names no chooser fails, since nothing says why it is here",
          named(rust_crates.check([crate(roots=())], [locked()]), "reaches it"))
    check("a direct crate that names no editor crate using it fails",
          named(rust_crates.check([crate(direct=True, roots=(), used_by=())], [locked()]),
                "names no editor crate"))


def test_licence_expressions() -> None:
    """The policy is 'permissive, and no copyleft in anything that links'. Applied to real forms."""
    for expression in ("MIT", "MIT OR Apache-2.0", "Apache-2.0/MIT", "MIT / Apache-2.0",
                       "Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT",
                       "(MIT OR Apache-2.0) AND Unicode-3.0",
                       "(MIT OR Apache-2.0) AND OFL-1.1 AND Ubuntu-font-1.0",
                       "Apache-2.0 OR GPL-2.0-only", "MIT OR Apache-2.0 OR LGPL-2.1-or-later"):
        check(f"permissive: {expression}", rust_crates.permissive(expression))

    # An OR with a copyleft branch passes because the project takes the other branch. An AND does
    # not, because every part of a conjunction applies — that is the whole difference, and getting
    # it backwards would let a GPL crate in through a dual-licence expression.
    for expression in ("GPL-3.0-only", "AGPL-3.0", "MIT AND GPL-2.0-only",
                       "(MIT OR Apache-2.0) AND GPL-3.0-only", "", "LGPL-2.1-or-later"):
        check(f"not permissive: {expression or '(no licence at all)'}",
              not rust_crates.permissive(expression))

    copyleft = rust_crates.check([crate(licence="GPL-3.0-only")], [locked()])
    check("a copyleft-only crate fails the gate with the policy named",
          named(copyleft, "permissive set"), "\n".join(copyleft))


# --- The records in this repository ---------------------------------------------------------------


def test_this_repository() -> None:
    """The gate over the tree as it stands, so a broken record fails here as well as in CI."""
    crates = rust_crates.load()
    complaints = rust_crates.check(crates, rust_crates.load_lockfile())
    check("deps/rust-crates.toml agrees with editor/Cargo.lock", not complaints,
          "\n".join(complaints[:20]))
    check("every crate in the record carries a licence and a justification",
          all(entry.licence and entry.justification for entry in crates))
    check("the interface toolkit and the graphics device are declared, which they were not at M5.5",
          {"egui", "wgpu", "eframe"} <= {entry.name for entry in crates})

    tools = manifest_module.load_host_tools()
    check("the Rust toolchain is a declared host prerequisite",
          any(tool.name == "rust" and tool.version == "1.95.0" for tool in tools),
          f"declared: {', '.join(tool.name for tool in tools)}")
    check("every host prerequisite's declared version appears in the file that loads it",
          not manifest_module.check_host_tool_pins(tools),
          "\n".join(manifest_module.check_host_tool_pins(tools)))


def test_record_rules(root: Path) -> None:
    """The record's own shape: a malformed entry is refused rather than half-read."""
    def load(text: str) -> Path:
        path = root / "record.toml"
        path.write_text(text, encoding="utf-8")
        return path

    head = 'schema = 1\n\n[[crate]]\nname = "a"\nversion = "1"\n'
    tail = ('licence = "MIT"\nchecksum = "x"\ndirect = false\nused_by = []\nroots = ["eframe"]\n'
            'justification = "j"\n')
    for name, text, fragment in (
        ("a record at the wrong schema is refused", 'schema = 99\n', "schema"),
        ("a record with no crates is refused", "schema = 1\n", "no [[crate]] tables"),
        ("an entry missing a field is refused, naming the field", head, "licence"),
        ("an entry with a field this tool does not know is refused",
         head + tail + 'unsafe_lines = "42"\n', "unknown key"),
        ("the same crate and version declared twice is refused",
         head + tail + "\n[[crate]]\n" + 'name = "a"\nversion = "1"\n' + tail, "twice"),
    ):
        try:
            rust_crates.load(load(text))
        except rust_crates.RustCratesError as error:
            check(name, fragment in str(error), f"raised: {error}")
            continue
        check(name, False, "no error was raised")

    try:
        rust_crates.load_lockfile(root / "absent.lock")
    except rust_crates.RustCratesError as error:
        check("a missing lockfile is an error rather than an empty dependency set",
              "does not exist" in str(error))


# --- Which pins are fetched shallowly ----------------------------------------------------------

SHALLOW_PROBE = """\
include("${MODULE}")
foreach(entry IN LISTS ENTRIES)
    string(REPLACE "|" ";" fields "${entry}")
    list(GET fields 0 name)
    list(GET fields 1 repository)
    list(GET fields 2 tag)
    cy_dependency_fetch_is_shallow(shallow "${repository}" "${tag}")
    message(STATUS "shallow ${name}=${shallow}")
endforeach()
"""


def shallow_decisions(root: Path, entries: list[tuple[str, str, str]]) -> dict[str, bool]:
    """Ask cmake/dependency_fetch.cmake itself, in script mode, rather than a copy of its rule."""
    probe = root / "shallow_probe.cmake"
    probe.write_text(SHALLOW_PROBE, encoding="utf-8")
    module = manifest_module.REPO_ROOT / "cmake" / "dependency_fetch.cmake"
    listed = ";".join("|".join(entry) for entry in entries)
    result = subprocess.run(
        ["cmake", f"-DMODULE={module.as_posix()}", f"-DENTRIES={listed}", "-P", str(probe)],
        capture_output=True, text=True, check=False)
    decisions: dict[str, bool] = {}
    for line in (result.stdout + result.stderr).splitlines():
        if "shallow " in line and "=" in line:
            name, _, value = line.split("shallow ", 1)[1].partition("=")
            decisions[name] = value.strip() == "TRUE"
    return decisions


def test_shallow_fetch(root: Path) -> None:
    """A shallow clone holds branch tips and tags, so a pin with no upstream tag is fetched in full.

    THE FAILURE THAT HAPPENED: stb_vorbis is pinned at a commit on nothings/stb's master with no
    tag (`master@003501f`). The pin was master's tip when it was taken, so the shallow clone held
    it; master moved on 2026-11-01 and every configure without a populated cache, CI's included,
    failed with "Failed to checkout tag".
    """
    if shutil.which("cmake") is None:
        check("cmake is on PATH to ask cmake/dependency_fetch.cmake", False,
              "the shallow-fetch rule lives in CMake and cannot be checked without it")
        return
    fixtures = [
        ("fixture_branch_pin", "https://github.com/example/branch", "master@0123456"),
        ("fixture_unreleased", "https://github.com/example/unreleased", "unreleased-0123abcd"),
        ("fixture_release", "https://github.com/example/release", "v1.2.3"),
        ("fixture_gitlab", "https://gitlab.com/example/release", "v1.2.3"),
    ]
    pins = [(entry.name, entry.repository, entry.tag) for entry in manifest_module.load()]
    decided = shallow_decisions(root, fixtures + pins)
    check("the CMake rule answered for every pin and fixture",
          len(decided) == len(fixtures) + len(pins),
          f"answered {len(decided)} of {len(fixtures) + len(pins)}")
    check("stb_vorbis, pinned at an untagged master commit, is fetched in full",
          decided.get("stb_vorbis") is False, f"decided: {decided.get('stb_vorbis')}")
    check("a `<branch>@<abbrev>` pin is fetched in full", decided.get("fixture_branch_pin") is False)
    check("an `unreleased-<abbrev>` pin is fetched in full",
          decided.get("fixture_unreleased") is False)
    check("a pin at a release tag on GitHub is still fetched shallowly",
          decided.get("fixture_release") is True and decided.get("sdl3") is True,
          f"fixture: {decided.get('fixture_release')}, sdl3: {decided.get('sdl3')}")
    check("a pin on a host other than GitHub is fetched in full",
          decided.get("fixture_gitlab") is False and decided.get("eigen") is False)
    untagged = [name for name, _, tag in pins if "@" in tag or tag.startswith("unreleased-")]
    check("every manifest pin with no upstream tag is fetched in full",
          not [name for name in untagged if decided.get(name) is not False],
          f"untagged: {', '.join(untagged)}")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="cy-deps-selftest-") as directory:
        test_lockfile_agreement()
        test_policy()
        test_licence_expressions()
        test_this_repository()
        test_record_rules(Path(directory))
        test_shallow_fetch(Path(directory))
    passed = len(_cases) - len(_failures)
    print(f"\ndeps selftest: {passed}/{len(_cases)} passed")
    return 1 if _failures else 0


if __name__ == "__main__":
    sys.exit(main())

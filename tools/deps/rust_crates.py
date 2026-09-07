#!/usr/bin/env python3
"""The editor's Rust dependency record: deps/rust-crates.toml, and its agreement with Cargo.lock.

THE GAP THIS CLOSES. M5.5 gave the editor a window, and with it 386 third-party crates. None of
them appeared in deps/manifest.toml, in deps/host-tools.toml or in THIRD_PARTY.md, and
`just maintenance-deps-check` compared the attribution document against the two C and C++ manifests
alone — so the gate that exists to catch an undeclared dependency reported green while
`grep -ci 'egui|wgpu|winit' THIRD_PARTY.md` answered 0. `thirdparty-dependencies` requires every
dependency in a single machine-readable manifest carrying a licence identifier and a one-line
justification, and requires the editor's Rust set to be governed by the same policy as the engine's
C++ set. This module and deps/rust-crates.toml are that record for the Rust half.

WHY A THIRD FILE. deps/manifest.toml is parsed by cmake/dependencies.cmake without a TOML library
and every table it reads becomes a `FetchContent_Declare` needing a git repository and a
40-character commit; a crates.io release has neither, so an entry for one would carry fabricated
fields and would then be *fetched*. deps/host-tools.toml records software the machine must already
provide and the build never links, which a crate is not: Cargo acquires these and they link into
the editor binary. A crate is a third kind of entry and gets a third file, for the same reason the
second one exists.

WHAT IS RECORDED HERE AND WHAT IS RECORDED BY CARGO. Cargo.lock is the pin — the exact version and
the checksum of every crate in the graph — and it is generated, complete and already reviewed on
every change. Duplicating a pin is how two records come to disagree, so this file does not replace
it: it carries the two facts Cargo does not record, the LICENCE IDENTIFIER and the JUSTIFICATION,
against a name and version that must match the lockfile exactly. The check below is what makes
"must match" true rather than intended.

WHY THE CHECK READS Cargo.lock RATHER THAN RUNNING CARGO. `just maintenance-deps-check` runs on
every pull request, on three platforms, in a job that configures nothing; the existing attribution
gate is deliberately Python and a file read for that reason. Cargo.lock is TOML, so the whole check
is `tomllib`. `--sync` — which regenerates this record — is the half that needs `cargo metadata`,
because licences live in each crate's own manifest, and it is a recipe a person runs when the
lockfile changes rather than a gate.

WHY 375 GENERATED JUSTIFICATIONS ARE NOT A LOOPHOLE. Eleven crates were chosen; the rest are what
those eleven pull in. `thirdparty-dependencies` says the transitive set "SHALL be part of the
evaluation, not only the crate itself", and the honest way to record that is to name, for each
transitive crate, which chosen crate reaches it — which is what makes the cost of a choice legible
and is exactly what a hand-written sentence per crate would obscure. The eleven carry justifications
written by a person, and `--sync` preserves them.

    tools/deps/rust_crates.py --check    the gate: the record agrees with editor/Cargo.lock
    tools/deps/rust_crates.py --sync     regenerate the record from the lockfile and cargo metadata

Governed by: thirdparty-dependencies (Dependency manifest, Rust editor dependencies, Attribution).
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
RECORD = REPO_ROOT / "deps" / "rust-crates.toml"
LOCKFILE = REPO_ROOT / "editor" / "Cargo.lock"
WORKSPACE = REPO_ROOT / "editor" / "Cargo.toml"

SCHEMA = 1
# The only source a crate may come from. A git or path source is a dependency on a moving branch or
# on somebody's working copy: it carries no registry checksum, so the pin below would be empty and
# `--check` would compare nothing against nothing. Adopting one is a decision `thirdparty-
# dependencies` wants argued in a change, so the gate refuses it by default rather than recording it.
REGISTRY = "registry+https://github.com/rust-lang/crates.io-index"
CRATE_FIELDS = ("name", "version", "licence", "checksum", "direct", "used_by", "roots",
                "justification")

# Every licence the policy accepts, as SPDX identifiers. `thirdparty-dependencies` sets the rule —
# "MIT, BSD, Apache 2.0, Zlib, or equivalent", and no copyleft for anything that links into shipped
# code — and this tuple is that rule in a form a gate can apply. A crate whose expression cannot be
# satisfied from this set fails the check by NAME, rather than being noticed at release.
#
# The three that are not obvious, all reached through the interface toolkit:
#   OFL-1.1, Ubuntu-font-1.0   the default fonts epaint embeds. Font licences, permissive for use
#                              and redistribution; neither imposes terms on the program.
#   Unicode-3.0                the Unicode character tables `unicode-ident` derives its data from.
#   BSL-1.0                    the Boost licence, permissive and without an attribution requirement.
#
# A GPL or LGPL identifier is deliberately absent. Three crates offer one as an ALTERNATIVE — the
# `r-efi` pair and `self_cell` — and the expression evaluator below is what makes that acceptable
# rather than a judgement call: a disjunction passes when any branch does, so those crates are taken
# under Apache-2.0 or MIT and the copyleft branch is one the project does not exercise.
PERMISSIVE = (
    "0BSD",
    "Apache-2.0",
    "BSD-2-Clause",
    "BSD-3-Clause",
    "BSL-1.0",
    "CC0-1.0",
    "ISC",
    "MIT",
    "MIT-0",
    "OFL-1.1",
    "Ubuntu-font-1.0",
    "Unicode-3.0",
    "Unicode-DFS-2016",
    "Unlicense",
    "Zlib",
)


class RustCratesError(Exception):
    """A Rust dependency record that cannot be trusted to answer 'what does the editor link'."""


@dataclass(frozen=True)
class Crate:
    """One [[crate]] table. Field meanings are documented in deps/rust-crates.toml."""

    name: str
    version: str
    licence: str
    checksum: str
    direct: bool
    used_by: tuple[str, ...]
    roots: tuple[str, ...]
    justification: str

    @property
    def key(self) -> tuple[str, str]:
        return (self.name, self.version)

    @property
    def upstream(self) -> str:
        return f"https://crates.io/crates/{self.name}"


# --- The licence expression -----------------------------------------------------------------------


def _leaf_is_permissive(token: str) -> bool:
    """One identifier, with any `WITH <exception>` clause removed.

    An exception only ever grants more than the licence it qualifies — `Apache-2.0 WITH
    LLVM-exception` is Apache 2.0 with the static-linking attribution requirement lifted — so the
    identifier decides, and keeping the clause would mean listing every exception a crate might use.
    """
    return token.split(" WITH ")[0].strip() in PERMISSIVE


def _tokenise(expression: str) -> list[str]:
    """The expression as tokens: identifiers, operators and brackets.

    Cargo carries pre-SPDX expressions from older crates — `MIT/Apache-2.0` and `MIT / Apache-2.0`
    both appear in this lockfile — and the slash means OR. It is normalised here rather than in each
    entry so that the record holds what the crate actually declares.
    """
    normalised = expression.replace("/", " OR ").replace("(", " ( ").replace(")", " ) ")
    tokens: list[str] = []
    for word in normalised.split():
        if word in ("OR", "AND", "(", ")") or not tokens or tokens[-1] in ("OR", "AND", "(", ")"):
            tokens.append(word)
        else:
            tokens[-1] = f"{tokens[-1]} {word}"  # `Apache-2.0 WITH LLVM-exception` is one leaf
    return tokens


def _parse_or(tokens: list[str], position: int) -> tuple[bool, int]:
    value, position = _parse_and(tokens, position)
    while position < len(tokens) and tokens[position] == "OR":
        right, position = _parse_and(tokens, position + 1)
        value = value or right
    return value, position


def _parse_and(tokens: list[str], position: int) -> tuple[bool, int]:
    value, position = _parse_term(tokens, position)
    while position < len(tokens) and tokens[position] == "AND":
        right, position = _parse_term(tokens, position + 1)
        value = value and right
    return value, position


def _parse_term(tokens: list[str], position: int) -> tuple[bool, int]:
    if position >= len(tokens):
        raise RustCratesError("licence expression ends where an identifier was expected")
    if tokens[position] == "(":
        value, position = _parse_or(tokens, position + 1)
        if position >= len(tokens) or tokens[position] != ")":
            raise RustCratesError("licence expression has an unclosed bracket")
        return value, position + 1
    return _leaf_is_permissive(tokens[position]), position + 1


def permissive(expression: str) -> bool:
    """Whether an SPDX expression can be satisfied entirely from PERMISSIVE.

    A disjunction passes when any branch does — the project takes the permissive option — and a
    conjunction only when every part does, because an AND is a licence whose terms all apply.
    """
    if not expression.strip():
        return False
    tokens = _tokenise(expression)
    value, position = _parse_or(tokens, 0)
    if position != len(tokens):
        raise RustCratesError(f"licence expression {expression!r} does not parse")
    return value


# --- The lockfile ---------------------------------------------------------------------------------


@dataclass(frozen=True)
class Locked:
    """One `[[package]]` in Cargo.lock that Cargo acquires: name, version, and the pin."""

    name: str
    version: str
    source: str
    checksum: str

    @property
    def key(self) -> tuple[str, str]:
        return (self.name, self.version)


def load_lockfile(path: Path = LOCKFILE) -> list[Locked]:
    """Every third-party crate the editor's lockfile pins, in name order.

    Workspace members carry no `source` and are excluded: `cy-editor-core` is the editor, not a
    dependency of it.
    """
    if not path.is_file():
        raise RustCratesError(f"{_display(path)}: the editor lockfile does not exist")
    document = tomllib.loads(path.read_text(encoding="utf-8"))
    crates = [
        Locked(name=package["name"], version=package["version"], source=package["source"],
               checksum=package.get("checksum", ""))
        for package in document.get("package", ())
        if package.get("source")
    ]
    if not crates:
        raise RustCratesError(f"{_display(path)}: pins no third-party crates")
    return sorted(crates, key=lambda crate: crate.key)


# --- The record -----------------------------------------------------------------------------------


def load(path: Path = RECORD) -> list[Crate]:
    """Read and validate deps/rust-crates.toml. Raises RustCratesError on anything wrong."""
    if not path.is_file():
        raise RustCratesError(f"{_display(path)}: the Rust dependency record does not exist")
    document = tomllib.loads(path.read_text(encoding="utf-8"))
    if document.get("schema") != SCHEMA:
        raise RustCratesError(
            f"{_display(path)}: schema is {document.get('schema')!r}, this tool reads {SCHEMA}")
    entries = document.get("crate", [])
    if not entries:
        raise RustCratesError(f"{_display(path)}: declares no [[crate]] tables")

    seen: set[tuple[str, str]] = set()
    crates = []
    for index, entry in enumerate(entries):
        crate = _crate(entry, index)
        if crate.key in seen:
            raise RustCratesError(f"{crate.name} {crate.version}: declared twice")
        seen.add(crate.key)
        crates.append(crate)
    return sorted(crates, key=lambda crate: crate.key)


def _crate(entry: dict, index: int) -> Crate:
    where = entry.get("name") or f"entry {index + 1}"
    for field in CRATE_FIELDS:
        if field not in entry:
            raise RustCratesError(f"{where}: does not declare `{field}`")
    unknown = sorted(set(entry) - set(CRATE_FIELDS))
    if unknown:
        raise RustCratesError(f"{where}: unknown key(s) {', '.join(unknown)}")
    return Crate(
        name=entry["name"], version=entry["version"], licence=entry["licence"],
        checksum=entry["checksum"], direct=bool(entry["direct"]),
        used_by=tuple(entry["used_by"]), roots=tuple(entry["roots"]),
        justification=entry["justification"],
    )


# --- The check ------------------------------------------------------------------------------------


def check(crates: list[Crate], locked: list[Locked]) -> list[str]:
    """Everything wrong with the record, as complaints. Empty when it is trustworthy.

    Five failures, and each is one somebody would otherwise discover late: a crate the lockfile
    acquires and the record does not declare (the failure this whole file exists for), a declaration
    the lockfile no longer pins, a pin that disagrees, a crate acquired from somewhere other than
    crates.io, and an entry whose licence or justification is missing or whose licence the policy
    does not accept.
    """
    declared = {crate.key: crate for crate in crates}
    pinned = {crate.key: crate for crate in locked}
    complaints = [
        f"{name} {version}: editor/Cargo.lock acquires it and deps/rust-crates.toml does not "
        f"declare it. Run `just maintenance-deps-rust`."
        for name, version in sorted(set(pinned) - set(declared))
    ]
    complaints += [
        f"{name} {version}: declared, but editor/Cargo.lock no longer pins it. Run "
        f"`just maintenance-deps-rust`."
        for name, version in sorted(set(declared) - set(pinned))
    ]
    for key in sorted(set(declared) & set(pinned)):
        complaints += _compare(declared[key], pinned[key])
    complaints += [
        f"{pin.name} {pin.version}: comes from `{pin.source}` rather than crates.io. A git or path "
        f"source has no registry checksum to pin, so this record could not attest to it."
        for pin in locked if pin.source != REGISTRY
    ]
    complaints += [complaint for crate in crates for complaint in _policy(crate)]
    return complaints


def _compare(crate: Crate, locked: Locked) -> list[str]:
    if crate.checksum == locked.checksum:
        return []
    return [
        f"{crate.name} {crate.version}: the record's checksum is "
        f"`{crate.checksum or '(none)'}` and editor/Cargo.lock pins "
        f"`{locked.checksum or '(none)'}`. A pin that disagrees with the "
        f"lockfile is not a pin."
    ]


def _policy(crate: Crate) -> list[str]:
    complaints = []
    if not crate.licence.strip():
        complaints.append(f"{crate.name} {crate.version}: declares no licence identifier")
    elif not permissive(crate.licence):
        complaints.append(
            f"{crate.name} {crate.version}: licence `{crate.licence}` cannot be satisfied from the "
            f"permissive set. thirdparty-dependencies forbids copyleft for code that links into a "
            f"shipped artefact; adopting it needs a change that argues the case.")
    if not crate.justification.strip():
        complaints.append(f"{crate.name} {crate.version}: declares no justification")
    if crate.direct and not crate.used_by:
        complaints.append(
            f"{crate.name} {crate.version}: is a direct dependency and names no editor crate that "
            f"uses it")
    if not crate.direct and not crate.roots:
        complaints.append(
            f"{crate.name} {crate.version}: is transitive and names no direct dependency that "
            f"reaches it, so nothing says why the editor carries it")
    return complaints


def _display(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


# --- Regeneration ---------------------------------------------------------------------------------

HEADER = """\
# CyberEditor — the Rust dependency record.
#
# GENERATED from editor/Cargo.lock and `cargo metadata`, except for the justifications of the
# direct dependencies, which are written by a person and preserved across regeneration. Regenerate
# it with `just maintenance-deps-rust`; `just maintenance-deps-check` fails when it and the lockfile
# disagree, and CI runs that on every pull request.
#
# WHY THIS FILE EXISTS. `thirdparty-dependencies` requires every dependency in a single
# machine-readable manifest with a licence identifier and a one-line justification, and requires the
# editor's Rust dependencies to be "declared, pinned, licence-reviewed, vendored or reproducibly
# acquired, and justified" — the same policy as the engine's C++ set. M5.5 brought the editor a
# window and, with it, {total} crates that appeared in no manifest at all; the attribution gate
# compared THIRD_PARTY.md against the C and C++ manifests only, so it stayed green while every one
# of them was undeclared. tools/deps/rust_crates.py's header records the rest of the reasoning,
# including why this is a third file rather than more tables in deps/manifest.toml.
#
# WHAT A HUMAN DECIDES, AND WHAT IS DERIVED. {direct} of these crates were chosen: they are named in
# editor/Cargo.toml's `[workspace.dependencies]`, they carry `direct = true`, they say which editor
# crate may use them, and their justification is an argument. The other {transitive} are what those
# choices pull in. Their justification names the chosen crates that reach them, because that — not a
# sentence invented per crate — is the fact `thirdparty-dependencies` asks for when it says the
# transitive set "SHALL be part of the evaluation, not only the crate itself". The number of them is
# the cost of the eleven, and it is written down here so that adding a twelfth is a visible edit.
#
# THE PIN IS Cargo.lock'S. `version` and `checksum` are copied from it and checked against it; they
# are here so this file alone answers a security advisory's "which version, and is it in the tree".
# Nothing is pinned twice: a disagreement is a gate failure rather than a fork.
#
# Fields
#   name           the crate, as crates.io names it.
#   version        the exact version editor/Cargo.lock resolves.
#   licence        SPDX expression, as the crate declares it. A `WITH` exception is kept. It must be
#                  satisfiable from tools/deps/rust_crates.py's PERMISSIVE set — an expression with
#                  a copyleft ALTERNATIVE passes, one that only offers copyleft does not.
#   checksum       the registry checksum editor/Cargo.lock pins.
#   direct         true when editor/Cargo.toml names it; false when it arrived through
#                  another crate.
#   used_by        the editor crates that name it. Direct dependencies only, and never empty
#                  for one of them.
#   roots          the direct dependencies whose graph reaches it. Transitive crates only.
#   justification  one line. Written by a person for a direct dependency; derived for the rest.

schema = {schema}
"""

DIRECT_JUSTIFICATIONS = {
    "ash": "The Vulkan API where wgpu does not expose it — timeline semaphore import and export, "
           "the bounded host wait, and the DRM format-modifier queries the cross-process viewport "
           "needs. Behind cy-editor-viewport-transport, which is the only crate that names it.",
    "eframe": "The window, the event loop and the wgpu renderer beneath the interface toolkit, "
              "with AccessKit — which editor-ui-ux requires and which decided the toolkit. Behind "
              "cy-editor-shell, and a containment test keeps it there.",
    "egui": "The immediate-mode interface toolkit, selected on measurement per "
            "editor-rust-application and kept replaceable behind the editor's own view models: no "
            "toolkit type appears in the document, command, protocol or plugin-facing layers.",
    "egui-wgpu": "egui's wgpu paint backend, which is what lets the interface and the imported "
                 "engine texture share one device rather than two.",
    "egui_dock": "The dockable panel layout editor-ui-ux requires. Small, focused, and above the "
                 "toolkit rather than beside it.",
    "image": "PNG decoding for the editor's identity assets, with every other format and every "
             "encoder switched off. Decoding a published format is not differentiating work.",
    "libc": "memfd_create, mmap and SCM_RIGHTS descriptor passing — the POSIX calls the viewport "
            "transport is built on, which the Rust standard library does not expose.",
    "pollster": "Three lines of executor for wgpu's async adapter enumeration. The editor has no "
                "async runtime and wants none; this is how it avoids acquiring one.",
    "wgpu": "The editor's graphics device: one portable API for the panels, and the object the "
            "engine's texture is imported into. Writing a second RHI for the editor would be the "
            "renderer's work done twice.",
    "wgpu-hal": "wgpu's supported escape hatch. Adding VK_KHR_external_semaphore_fd before "
                "vkCreateDevice is only reachable through Adapter::open_with_callback, and the "
                "cross-process viewport cannot be built without it.",
    "wgpu-types": "The shared type definitions wgpu and wgpu-hal both speak, named directly so the "
                  "transport does not reach them through a re-export that may move.",
}


def _metadata() -> dict:
    """`cargo metadata` for the editor workspace, resolved offline against the lockfile."""
    completed = subprocess.run(  # noqa: S603 — a fixed argument vector, no shell, no input
        ["cargo", "metadata", "--format-version", "1", "--locked", "--offline",
         "--manifest-path", str(WORKSPACE)],
        capture_output=True, text=True, check=False,
    )
    if completed.returncode != 0:
        raise RustCratesError(
            f"`cargo metadata` failed with exit {completed.returncode}. It is needed only to "
            f"regenerate this record — the check reads editor/Cargo.lock alone.\n"
            f"{completed.stderr.strip()}")
    return json.loads(completed.stdout)


def _resolve_graph(metadata: dict) -> tuple[dict, dict, set[str]]:
    """The package table, the resolved dependency edges, and the workspace's own package ids."""
    packages = {package["id"]: package for package in metadata["packages"]}
    edges = {node["id"]: tuple(node["dependencies"]) for node in metadata["resolve"]["nodes"]}
    return packages, edges, set(metadata["workspace_members"])


def _reachable(start: str, edges: dict, members: set[str]) -> set[str]:
    """Every package reachable from one crate, excluding the workspace's own members."""
    seen: set[str] = set()
    frontier = [start]
    while frontier:
        for identifier in edges.get(frontier.pop(), ()):
            if identifier not in seen and identifier not in members:
                seen.add(identifier)
                frontier.append(identifier)
    return seen


def _direct_ids(packages: dict, edges: dict, members: set[str]) -> dict[str, str]:
    """The chosen crates, by name: a workspace member's dependency that is not itself a member."""
    chosen = {}
    for member in members:
        for identifier in edges.get(member, ()):
            if identifier not in members:
                chosen[packages[identifier]["name"]] = identifier
    return chosen


def _users(packages: dict, edges: dict, members: set[str], target: str) -> tuple[str, ...]:
    """The editor crates naming one direct dependency, which is what `used_by` records."""
    return tuple(sorted(packages[member]["name"] for member in members
                        if target in edges.get(member, ())))


def _roots_by_crate(packages: dict, edges: dict, members: set[str],
                    direct: dict[str, str]) -> dict[tuple[str, str], tuple[str, ...]]:
    """For every crate in the graph, the chosen crates whose subtree reaches it.

    Keyed by name AND version, because a lockfile holds several versions of the same crate — this
    one pins two `r-efi`s and three `hashbrown`s — and attributing one version's cost to the other's
    chooser would be a quiet lie about which choice is expensive.
    """
    reached: dict[tuple[str, str], set[str]] = {}
    for name, identifier in sorted(direct.items()):
        for found in _reachable(identifier, edges, members):
            package = packages[found]
            reached.setdefault((package["name"], package["version"]), set()).add(name)
    return {key: tuple(sorted(names)) for key, names in reached.items()}


def _rebuild(metadata: dict, locked: list[Locked], existing: dict[str, str]) -> list[Crate]:
    """One entry per locked crate, with the licence and the graph position cargo metadata knows."""
    packages, edges, members = _resolve_graph(metadata)
    direct = _direct_ids(packages, edges, members)
    roots = _roots_by_crate(packages, edges, members, direct)
    licences = {(package["name"], package["version"]): (package.get("license") or "").strip()
                for package in packages.values()}

    crates = []
    for pin in locked:
        is_direct = direct.get(pin.name) is not None and packages[direct[pin.name]][
            "version"] == pin.version
        reached_by = () if is_direct else roots.get(pin.key, ())
        crates.append(Crate(
            name=pin.name, version=pin.version, licence=licences.get(pin.key, ""),
            checksum=pin.checksum, direct=is_direct,
            used_by=_users(packages, edges, members, direct[pin.name]) if is_direct else (),
            roots=reached_by,
            justification=_justification(pin.name, is_direct, reached_by, existing),
        ))
    return crates


def _justification(name: str, direct: bool, roots: tuple[str, ...],
                   existing: dict[str, str]) -> str:
    if direct:
        return existing.get(name) or DIRECT_JUSTIFICATIONS.get(name, "")
    if not roots:
        return ""
    reached = ", ".join(roots)
    verb = "reach" if len(roots) > 1 else "reaches"
    return (f"Not chosen: {name} is in the editor's graph because {reached} {verb} it. Its cost is "
            f"part of the case for {'those crates' if len(roots) > 1 else reached}.")


def _render(crates: list[Crate]) -> str:
    direct = [crate for crate in crates if crate.direct]
    parts = [HEADER.format(schema=SCHEMA, total=len(crates), direct=len(direct),
                           transitive=len(crates) - len(direct))]
    parts.append("\n# --- Chosen, and argued for ---------------------------------------------"
                 "-------------------------\n")
    parts.extend(_table(crate) for crate in direct)
    parts.append("\n# --- Reached through the crates above, and recorded so the cost of choosing "
                 "them is legible ---\n")
    parts.extend(_table(crate) for crate in crates if not crate.direct)
    return "".join(parts)


def _array(names: tuple[str, ...]) -> str:
    """A TOML array of strings. Written out rather than dumped: this file has no TOML writer, and a
    dependency added to acquire one would be a joke at this file's expense."""
    return "[" + ", ".join('"' + name + '"' for name in names) + "]"


def _table(crate: Crate) -> str:
    lines = [
        "\n[[crate]]",
        f'name = "{crate.name}"',
        f'version = "{crate.version}"',
        f'licence = "{crate.licence}"',
        f'checksum = "{crate.checksum}"',
        f"direct = {'true' if crate.direct else 'false'}",
        f"used_by = {_array(crate.used_by)}",
        f"roots = {_array(crate.roots)}",
        f'justification = "{crate.justification}"',
    ]
    return "\n".join(lines) + "\n"


def sync(record: Path = RECORD, lockfile: Path = LOCKFILE) -> tuple[int, int]:
    """Rewrite the record from the lockfile and cargo metadata. Returns (total, direct)."""
    locked = load_lockfile(lockfile)
    existing = {}
    if record.is_file():
        existing = {crate.name: crate.justification for crate in load(record) if crate.direct}
    crates = _rebuild(_metadata(), locked, existing)
    missing = [crate.name for crate in crates if not crate.justification.strip()]
    if missing:
        raise RustCratesError(
            f"no justification for the direct dependenc{'y' if len(missing) == 1 else 'ies'} "
            f"{', '.join(missing)}. A crate is added by writing one — in DIRECT_JUSTIFICATIONS in "
            f"tools/deps/rust_crates.py, or in the entry this regenerates from.")
    record.write_text(_render(crates), encoding="utf-8")
    return len(crates), sum(1 for crate in crates if crate.direct)


# --- Attribution ----------------------------------------------------------------------------------

CRATES_HEADER = """
## The editor's Rust crates

The editor is a separate binary and a separate dependency set: none of this is linked into a game,
and a packaged game contains no part of it. It is listed here because `thirdparty-dependencies`
governs the editor's Rust dependencies under the same policy as the engine's C++ ones, and because
{total} crates that appear in no attribution document are {total} crates nobody has reviewed.

{direct} were chosen; each is named in `editor/Cargo.toml`, sits behind the editor's own
abstractions, and carries the argument for its adoption below. The other {transitive} arrived
through those {direct}, which is the transitive cost `thirdparty-dependencies` requires to be part
of the decision rather than a discovery. Every one of them is pinned by `editor/Cargo.lock` — an
exact version and a registry checksum — and recorded with its licence in `deps/rust-crates.toml`.

Where a crate offers a choice of licences, the project takes a permissive one; the record is the
expression its author published.

| Crate | Version | Licence | In the tree because |
|---|---|---|---|
"""


def _crate_row(crate: Crate) -> str:
    reason = ("chosen — used by " + ", ".join(f"`{name}`" for name in crate.used_by)
              if crate.direct else "reached through " + ", ".join(crate.roots))
    return (f"| [{crate.name}]({crate.upstream}) | {crate.version} | {crate.licence} "
            f"| {reason} |")


def _crate_entry(crate: Crate) -> str:
    return f"""\
### {crate.name} {crate.version}

- **Upstream**: {crate.upstream}
- **Pinned at**: `{crate.version}`, checksum `{crate.checksum}` in `editor/Cargo.lock`
- **Licence**: {crate.licence}
- **Named by**: {', '.join(f'`{name}`' for name in crate.used_by)}
- **Linked into**: the editor only — no part of it enters a packaged game
- **Why integrated rather than built**: {crate.justification}
"""


def attribution(crates: list[Crate]) -> str:
    """The Rust half of THIRD_PARTY.md: every crate in a table, and a notice per chosen crate."""
    direct = [crate for crate in crates if crate.direct]
    parts = [CRATES_HEADER.format(total=len(crates), direct=len(direct),
                                  transitive=len(crates) - len(direct))]
    parts.extend(f"{_crate_row(crate)}\n" for crate in crates)
    parts.append("\n## Notices — the editor crates that were chosen\n\n")
    parts.append("\n".join(_crate_entry(crate) for crate in direct))
    return "".join(parts)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true",
                       help="fail if the record and editor/Cargo.lock disagree")
    group.add_argument("--sync", action="store_true",
                       help="regenerate the record; needs cargo and a populated registry cache")
    arguments = parser.parse_args(argv)

    try:
        if arguments.sync:
            total, direct = sync()
            print(f"wrote {_display(RECORD)} — {total} crates, {direct} of them chosen directly")
            return 0
        complaints = check(load(), load_lockfile())
    except RustCratesError as error:
        print(f"rust-crates: {error}", file=sys.stderr)
        return 2

    if complaints:
        print(f"{_display(RECORD)} does not agree with {_display(LOCKFILE)}:", file=sys.stderr)
        for complaint in complaints:
            print(f"  {complaint}", file=sys.stderr)
        return 1
    print(f"{_display(RECORD)} agrees with {_display(LOCKFILE)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

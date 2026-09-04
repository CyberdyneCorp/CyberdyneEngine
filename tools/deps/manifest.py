"""Reading and validating the two dependency records: deps/manifest.toml and deps/host-tools.toml.

deps/manifest.toml is parsed twice in this repository: here, and by cmake/dependencies.cmake, which
cannot use a TOML library. The rules the two enforce are the same ones, and they are stated in the
manifest's own header; when one changes, both change. This module is the reference: a manifest that
fails here is malformed, whatever CMake made of it.

deps/host-tools.toml is parsed *only* here. It records software that must already be on the machine
and that the build never fetches or links — today, the reflection generator's C++ frontend. Its
header says why it is a second file rather than more tables in the first one; the short version is
that cmake/dependencies.cmake turns every table it reads into a FetchContent_Declare, and a PyPI
wheel has no commit to pin.
"""

from __future__ import annotations

import tomllib
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = REPO_ROOT / "deps" / "manifest.toml"
HOST_TOOLS = REPO_ROOT / "deps" / "host-tools.toml"

REQUIRED_FIELDS = (
    "name",
    "version",
    "tag",
    "commit",
    "repository",
    "licence",
    "licence_file",
    "optional",
    "feature",
    "system_package",
    "cmake_target",
    "source_subdir",
    "interface",
    "scope",
    "justification",
)

# Required only where a system copy is acceptable, because finding one needs both its package name
# and the target it exports, and neither is reliably the name the fetched build uses.
SYSTEM_FIELDS = ("system_find_package", "system_target")

SCOPES = ("runtime", "tool-time", "test", "development")

# deps/host-tools.toml. Deliberately a different field set from a fetched dependency's: there is no
# commit to pin, no CMake target to name and no source subdirectory, and inventing those three would
# make an entry look fetchable when it is not.
HOST_TOOL_FIELDS = (
    "name",
    "version",
    "kind",
    "package",
    "upstream",
    "licence",
    "licence_file",
    "required",
    "consumer",
    "checked_by",
    "install_linux",
    "install_macos",
    "install_windows",
    "justification",
)

HOST_TOOL_KINDS = ("python-package", "system-library")


class ManifestError(Exception):
    """A manifest that cannot be trusted to drive a build."""


@dataclass(frozen=True)
class Dependency:
    """One [[dependency]] table. Field meanings are documented in deps/manifest.toml."""

    name: str
    version: str
    tag: str
    commit: str
    repository: str
    licence: str
    licence_file: str
    optional: bool
    feature: str
    system_package: bool
    cmake_target: str
    source_subdir: str
    interface: str
    scope: str
    justification: str

    @property
    def licence_url(self) -> str:
        """The full licence text at the exact commit that is pinned.

        Derived rather than recorded: a stored URL is one more thing that can disagree with the
        commit beside it.
        """
        return f"{self.repository}/blob/{self.commit}/{self.licence_file}"

    @property
    def gate(self) -> str:
        return self.feature if self.optional else ""


def _require_fields(where: str, entry: dict) -> None:
    for field in REQUIRED_FIELDS:
        if field not in entry:
            raise ManifestError(f"{where}: does not declare `{field}`")


def _check_gate(where: str, entry: dict) -> None:
    if entry["optional"] and not entry["feature"]:
        raise ManifestError(
            f"{where}: is optional but names no gating feature. An optional dependency that "
            f"nothing gates cannot be excluded."
        )
    if not entry["optional"] and entry["feature"]:
        raise ManifestError(
            f"{where}: is not optional but names the gating feature `{entry['feature']}`. "
            f"A gate on a dependency that is always built is a gate that does nothing."
        )


def _check_system(where: str, entry: dict) -> None:
    if not entry["system_package"]:
        return
    for field in SYSTEM_FIELDS:
        if field not in entry:
            raise ManifestError(
                f"{where}: says a system copy is acceptable but does not declare `{field}`"
            )


def _check_scope(where: str, entry: dict) -> None:
    if entry["scope"] not in SCOPES:
        raise ManifestError(f"{where}: scope `{entry['scope']}` is not one of {', '.join(SCOPES)}")


def _check_pin(where: str, entry: dict) -> None:
    commit = entry["commit"]
    if len(commit) != 40 or not all(character in "0123456789abcdef" for character in commit):
        raise ManifestError(
            f"{where}: `commit` is not a full lowercase 40-character SHA. A tag or an abbreviation "
            f"is not a pin: a tag can be moved and an abbreviation can become ambiguous."
        )


# Order matters only in that the field check comes first: the rest read fields it has proved present.
_RULES = (_check_gate, _check_system, _check_scope, _check_pin)


def _validate(entry: dict, index: int) -> None:
    where = entry.get("name") or f"entry {index + 1}"
    _require_fields(where, entry)
    for rule in _RULES:
        rule(where, entry)


def load(path: Path = MANIFEST) -> list[Dependency]:
    """Every dependency in the manifest, in manifest order. Raises ManifestError if invalid."""
    document = tomllib.loads(path.read_text(encoding="utf-8"))
    entries = document.get("dependency", [])
    if not entries:
        raise ManifestError(f"{path}: declares no [[dependency]] tables")

    names: set[str] = set()
    dependencies = []
    for index, entry in enumerate(entries):
        _validate(entry, index)
        if entry["name"] in names:
            raise ManifestError(f"{entry['name']}: declared twice")
        names.add(entry["name"])
        dependencies.append(
            Dependency(**{field: entry[field] for field in REQUIRED_FIELDS})
        )
    return dependencies


@dataclass(frozen=True)
class HostTool:
    """One [[host_tool]] table. Field meanings are documented in deps/host-tools.toml."""

    name: str
    version: str
    kind: str
    package: str
    upstream: str
    licence: str
    licence_file: str
    required: bool
    consumer: str
    checked_by: str
    install_linux: str
    install_macos: str
    install_windows: str
    justification: str

    def install(self, platform: str) -> str:
        """The correction for this host, or an empty string where there is none to give."""
        return {
            "linux": self.install_linux,
            "macos": self.install_macos,
            "windows": self.install_windows,
        }.get(platform, "")


def _validate_host_tool(entry: dict, index: int) -> None:
    where = entry.get("name") or f"entry {index + 1}"
    for field in HOST_TOOL_FIELDS:
        if field not in entry:
            raise ManifestError(f"{where}: does not declare `{field}`")
    if entry["kind"] not in HOST_TOOL_KINDS:
        raise ManifestError(
            f"{where}: kind `{entry['kind']}` is not one of {', '.join(HOST_TOOL_KINDS)}"
        )
    if not entry["consumer"]:
        raise ManifestError(
            f"{where}: names no consumer. A host prerequisite nothing loads is a prerequisite "
            f"nobody can remove, because nobody can find out whether it is still needed."
        )
    if not (REPO_ROOT / entry["consumer"]).exists():
        raise ManifestError(
            f"{where}: names the consumer `{entry['consumer']}`, which does not exist. The "
            f"declaration and the code that loads it move together or they do not move at all."
        )


def load_host_tools(path: Path = HOST_TOOLS) -> list[HostTool]:
    """Every host prerequisite, in file order. Raises ManifestError if invalid."""
    document = tomllib.loads(path.read_text(encoding="utf-8"))
    entries = document.get("host_tool", [])
    if not entries:
        raise ManifestError(f"{path}: declares no [[host_tool]] tables")

    names: set[str] = set()
    tools = []
    for index, entry in enumerate(entries):
        _validate_host_tool(entry, index)
        if entry["name"] in names:
            raise ManifestError(f"{entry['name']}: declared twice")
        names.add(entry["name"])
        tools.append(HostTool(**{field: entry[field] for field in HOST_TOOL_FIELDS}))
    return tools


def check_host_tool_pins(tools: list[HostTool]) -> list[str]:
    """Every declared version must appear in the file that loads it.

    A grep rather than an import, because the consumer is a module that only runs where the
    prerequisite is installed and this check has to work on a clone where it is not. It is crude and
    it catches the failure that matters in both directions: a version bumped in the declaration and
    forgotten in the code, or bumped in the code and forgotten in the declaration. That is exactly
    the drift that left these two undeclared until M2 — the pins lived in tools/gen/reflect/parse.py
    and in .github/workflows/ci.yml, and in no table anybody audited.

    Returns a list of complaints, empty when the two agree.
    """
    complaints = []
    for tool in tools:
        source = (REPO_ROOT / tool.consumer).read_text(encoding="utf-8")
        if tool.version not in source:
            complaints.append(
                f"{tool.name}: deps/host-tools.toml declares version `{tool.version}`, which does "
                f"not appear anywhere in its consumer {tool.consumer}. One of the two was bumped "
                f"without the other."
            )
    return complaints

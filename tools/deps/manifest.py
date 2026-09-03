"""Reading and validating deps/manifest.toml.

The manifest is parsed twice in this repository: here, and by cmake/dependencies.cmake, which
cannot use a TOML library. The rules the two enforce are the same ones, and they are stated in the
manifest's own header; when one changes, both change. This module is the reference: a manifest that
fails here is malformed, whatever CMake made of it.
"""

from __future__ import annotations

import tomllib
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = REPO_ROOT / "deps" / "manifest.toml"

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

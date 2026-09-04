"""The project manifest's schema, and the reader that turns one file into records.

Task 4.1. `project-and-plugins` states the rule this file implements: **the project graph is
authoritative**. A project is what its manifest says it is — its modules, its plugins, its content
roots, its build targets and its per-platform overrides — and "unknown or malformed entries SHALL be
reported rather than ignored".

Two things this file deliberately does *not* do:

  * It does not look at the filesystem to decide what a project contains. A directory that is not in
    the manifest is not in the project, however much it looks like a module. `graph.py` reads files
    only to check declared modules against what they actually include, never to discover one.
  * It does not stop at the first error. A manifest with four typos should report four typos: a
    reader who has to configure four times to find them will stop reading the manifest and start
    guessing.

JSON rather than TOML, for the same reason cmake/modules.cmake reads module.json: CMake can parse it
with `string(JSON)` and needs no dependency, and one file format across the project graph means one
parser to trust.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

SCHEMA_VERSION = 1

# `engine-architecture`'s layer table, mirrored from cmake/module.cmake. Two spellings share index 3
# there and share it here; a second order that disagreed would be worse than none.
LAYER_INDEX: dict[str, int] = {
    "core": 0,
    "ecs": 1,
    "servers": 2,
    "backends": 3,
    "platform": 3,
    "scene": 4,
    "runtime": 5,
    "abi": 6,
    "editor": 7,
    "tools": 7,
}

# Registration levels order initialisation; layers constrain dependencies. They are distinct, and a
# module carries both — see the header comment of cmake/modules.cmake.
REGISTRATION_LEVELS = ("Core", "Servers", "Scene", "Editor")

MODULE_TYPES = ("runtime", "editor", "developer", "server", "tool", "third-party")
PLATFORMS = ("linux", "windows", "macos", "ios", "android", "visionos", "web")
TARGET_KINDS = ("client", "server", "editor", "tool")

# Module types that are editor code whatever layer they sit at. `project-and-plugins` requires a
# shipping target to exclude editor code *from the graph*, not from a build flag, so the graph has to
# know which modules those are.
EDITOR_TYPES = ("editor",)
EDITOR_LAYERS = ("editor", "tools")

ROOT_KEYS = {
    "schema_version", "project", "content_roots", "modules", "plugins", "targets",
    "settings", "platform_overrides",
}
ROOT_REQUIRED = ("schema_version", "project", "modules")

PROJECT_KEYS = {"name", "version", "engine_version", "description"}
PROJECT_REQUIRED = ("name", "version", "engine_version")

MODULE_KEYS = {
    "name", "path", "layer", "type", "registration_level",
    "public_dependencies", "private_dependencies",
    "platforms", "hot_reload", "enabled", "description",
}
MODULE_REQUIRED = ("name", "layer", "type", "registration_level")

PLUGIN_KEYS = {"id", "version", "engine_api", "enabled", "description"}
PLUGIN_REQUIRED = ("id", "version", "engine_api")

TARGET_KEYS = {"name", "kind", "shipping", "modules", "description"}
TARGET_REQUIRED = ("name", "kind", "modules")

PLATFORM_OVERRIDE_KEYS = {"modules", "settings"}
MODULE_OVERRIDE_KEYS = {"enabled"}


@dataclass(frozen=True)
class Diagnostic:
    """One rejected entry. `where` is the manifest path to it, so the message names what to fix."""

    where: str
    message: str

    def __str__(self) -> str:
        return f"{self.where}: {self.message}"


@dataclass
class Module:
    name: str
    layer: str
    type: str
    registration_level: str
    path: str = ""
    public_dependencies: tuple[str, ...] = ()
    private_dependencies: tuple[str, ...] = ()
    platforms: tuple[str, ...] = PLATFORMS
    hot_reload: bool = False
    enabled: bool = True
    description: str = ""

    @property
    def layer_index(self) -> int:
        return LAYER_INDEX[self.layer]

    @property
    def level_index(self) -> int:
        return REGISTRATION_LEVELS.index(self.registration_level)

    @property
    def dependencies(self) -> tuple[str, ...]:
        return tuple(self.public_dependencies) + tuple(self.private_dependencies)

    @property
    def is_editor_code(self) -> bool:
        return self.type in EDITOR_TYPES or self.layer in EDITOR_LAYERS


@dataclass
class Plugin:
    id: str
    version: str
    engine_api: str
    enabled: bool = True
    description: str = ""


@dataclass
class Target:
    name: str
    kind: str
    modules: tuple[str, ...]
    shipping: bool = False
    description: str = ""


@dataclass
class Manifest:
    """A manifest that parsed. Whether it is *valid* is `graph.py`'s question, not this file's."""

    source: Path
    root: Path
    name: str = ""
    version: str = ""
    engine_version: str = ""
    description: str = ""
    content_roots: tuple[str, ...] = ()
    modules: list[Module] = field(default_factory=list)
    plugins: list[Plugin] = field(default_factory=list)
    targets: list[Target] = field(default_factory=list)
    settings: dict[str, object] = field(default_factory=dict)
    platform_overrides: dict[str, dict] = field(default_factory=dict)

    def module(self, name: str) -> Module | None:
        for entry in self.modules:
            if entry.name == name:
                return entry
        return None

    def enabled_modules(self) -> list[Module]:
        return [entry for entry in self.modules if entry.enabled]

    def modules_in_registration_order(self) -> list[Module]:
        """Registration level, then name. The order the runtime initialises them in."""
        return sorted(self.enabled_modules(), key=lambda entry: (entry.level_index, entry.name))


# --- Typed access, with the diagnostic instead of an exception -----------------------------------
#
# Every accessor takes the collector, so a wrong type is a reported entry and the read continues with
# a default. The alternative — raising — reports one error per run, which is the behaviour the
# specification's "reported rather than ignored" is written against.

class Reader:
    def __init__(self) -> None:
        self.diagnostics: list[Diagnostic] = []

    def report(self, where: str, message: str) -> None:
        self.diagnostics.append(Diagnostic(where, message))

    @property
    def ok(self) -> bool:
        return not self.diagnostics

    def object_at(self, value: object, where: str) -> dict:
        if not isinstance(value, dict):
            self.report(where, f"expected an object, found {_kind(value)}")
            return {}
        return value

    def keys(self, value: dict, where: str, allowed: set[str], required: tuple[str, ...]) -> None:
        for key in sorted(value):
            if key not in allowed:
                near = _nearest(key, allowed)
                hint = f"; did you mean '{near}'?" if near else ""
                self.report(f"{where}.{key}", f"unknown key '{key}'{hint}")
        for key in required:
            if key not in value:
                self.report(where, f"missing required key '{key}'")

    def string(self, value: dict, where: str, key: str, default: str = "") -> str:
        if key not in value:
            return default
        found = value[key]
        if not isinstance(found, str):
            self.report(f"{where}.{key}", f"expected a string, found {_kind(found)}")
            return default
        return found

    def boolean(self, value: dict, where: str, key: str, default: bool) -> bool:
        if key not in value:
            return default
        found = value[key]
        if not isinstance(found, bool):
            self.report(f"{where}.{key}", f"expected true or false, found {_kind(found)}")
            return default
        return found

    def string_list(self, value: dict, where: str, key: str,
                    default: tuple[str, ...] = ()) -> tuple[str, ...]:
        if key not in value:
            return default
        found = value[key]
        if not isinstance(found, list):
            self.report(f"{where}.{key}", f"expected an array, found {_kind(found)}")
            return default
        items: list[str] = []
        for index, element in enumerate(found):
            if not isinstance(element, str):
                self.report(f"{where}.{key}[{index}]",
                            f"expected a string, found {_kind(element)}")
                continue
            items.append(element)
        return tuple(items)

    def enumerated(self, value: str, allowed: tuple[str, ...] | dict, where: str) -> str:
        names = tuple(allowed)
        if value and value not in names:
            self.report(where, f"'{value}' is not one of: {', '.join(sorted(names))}")
        return value


def _kind(value: object) -> str:
    if isinstance(value, bool):
        return "true/false"
    if value is None:
        return "null"
    return {dict: "an object", list: "an array", str: "a string",
            int: "a number", float: "a number"}.get(type(value), type(value).__name__)


def _nearest(key: str, allowed: set[str]) -> str:
    """A one-edit neighbour, which is the typo class worth naming. No fuzzy matching beyond that."""
    for candidate in sorted(allowed):
        if abs(len(candidate) - len(key)) > 1:
            continue
        if candidate.replace("_", "") == key.replace("_", "").replace("-", ""):
            return candidate
        differences = sum(1 for a, b in zip(candidate, key) if a != b)
        if len(candidate) != len(key):
            continue
        # One substitution, or one transposition: the two typos worth naming. Anything looser
        # starts suggesting 'targets' for 'plugins', which is noise rather than help.
        if differences <= 1 or (differences == 2 and sorted(candidate) == sorted(key)):
            return candidate
    return ""


# --- Reading -------------------------------------------------------------------------------------

def read(path: Path) -> tuple[Manifest | None, list[Diagnostic]]:
    """Read one manifest. Returns the records and every entry that was rejected."""
    reader = Reader()
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        return None, [Diagnostic(str(path), f"cannot be read: {error.strerror}")]
    try:
        document = json.loads(text)
    except json.JSONDecodeError as error:
        return None, [Diagnostic(f"{path}:{error.lineno}:{error.colno}",
                                 f"is not valid JSON: {error.msg}")]

    root = reader.object_at(document, "the manifest")
    if not root:
        return None, reader.diagnostics

    reader.keys(root, "manifest", ROOT_KEYS, ROOT_REQUIRED)
    _check_schema_version(reader, root)

    manifest = Manifest(source=path, root=path.parent)
    _read_project(reader, root, manifest)
    manifest.content_roots = reader.string_list(root, "manifest", "content_roots")
    manifest.modules = _read_modules(reader, root)
    manifest.plugins = _read_plugins(reader, root)
    manifest.targets = _read_targets(reader, root)
    manifest.settings = _read_settings(reader, root, "manifest.settings")
    manifest.platform_overrides = _read_platform_overrides(reader, root)

    return manifest, reader.diagnostics


def _check_schema_version(reader: Reader, root: dict) -> None:
    version = root.get("schema_version")
    if version is None:
        return
    if not isinstance(version, int) or isinstance(version, bool):
        reader.report("manifest.schema_version", f"expected a number, found {_kind(version)}")
        return
    if version != SCHEMA_VERSION:
        reader.report("manifest.schema_version",
                      f"is {version}; this engine reads schema {SCHEMA_VERSION}")


def _read_project(reader: Reader, root: dict, manifest: Manifest) -> None:
    section = reader.object_at(root.get("project", {}), "manifest.project")
    reader.keys(section, "manifest.project", PROJECT_KEYS, PROJECT_REQUIRED)
    manifest.name = reader.string(section, "manifest.project", "name")
    manifest.version = reader.string(section, "manifest.project", "version")
    manifest.engine_version = reader.string(section, "manifest.project", "engine_version")
    manifest.description = reader.string(section, "manifest.project", "description")


def _read_modules(reader: Reader, root: dict) -> list[Module]:
    entries = root.get("modules", [])
    if not isinstance(entries, list):
        reader.report("manifest.modules", f"expected an array, found {_kind(entries)}")
        return []
    modules: list[Module] = []
    for index, element in enumerate(entries):
        where = f"manifest.modules[{index}]"
        value = reader.object_at(element, where)
        if not value:
            continue
        reader.keys(value, where, MODULE_KEYS, MODULE_REQUIRED)
        name = reader.string(value, where, "name")
        modules.append(Module(
            name=name,
            layer=reader.enumerated(reader.string(value, where, "layer"), LAYER_INDEX,
                                    f"{where}.layer"),
            type=reader.enumerated(reader.string(value, where, "type"), MODULE_TYPES,
                                   f"{where}.type"),
            registration_level=reader.enumerated(
                reader.string(value, where, "registration_level"), REGISTRATION_LEVELS,
                f"{where}.registration_level"),
            path=reader.string(value, where, "path"),
            public_dependencies=reader.string_list(value, where, "public_dependencies"),
            private_dependencies=reader.string_list(value, where, "private_dependencies"),
            platforms=_read_platforms(reader, value, where),
            hot_reload=reader.boolean(value, where, "hot_reload", False),
            enabled=reader.boolean(value, where, "enabled", True),
            description=reader.string(value, where, "description"),
        ))
    return modules


def _read_platforms(reader: Reader, value: dict, where: str) -> tuple[str, ...]:
    platforms = reader.string_list(value, where, "platforms", PLATFORMS)
    for platform in platforms:
        reader.enumerated(platform, PLATFORMS, f"{where}.platforms")
    return platforms


def _read_plugins(reader: Reader, root: dict) -> list[Plugin]:
    entries = root.get("plugins", [])
    if not isinstance(entries, list):
        reader.report("manifest.plugins", f"expected an array, found {_kind(entries)}")
        return []
    plugins: list[Plugin] = []
    for index, element in enumerate(entries):
        where = f"manifest.plugins[{index}]"
        value = reader.object_at(element, where)
        if not value:
            continue
        reader.keys(value, where, PLUGIN_KEYS, PLUGIN_REQUIRED)
        plugins.append(Plugin(
            id=reader.string(value, where, "id"),
            version=reader.string(value, where, "version"),
            engine_api=reader.string(value, where, "engine_api"),
            enabled=reader.boolean(value, where, "enabled", True),
            description=reader.string(value, where, "description"),
        ))
    return plugins


def _read_targets(reader: Reader, root: dict) -> list[Target]:
    entries = root.get("targets", [])
    if not isinstance(entries, list):
        reader.report("manifest.targets", f"expected an array, found {_kind(entries)}")
        return []
    targets: list[Target] = []
    for index, element in enumerate(entries):
        where = f"manifest.targets[{index}]"
        value = reader.object_at(element, where)
        if not value:
            continue
        reader.keys(value, where, TARGET_KEYS, TARGET_REQUIRED)
        targets.append(Target(
            name=reader.string(value, where, "name"),
            kind=reader.enumerated(reader.string(value, where, "kind"), TARGET_KINDS,
                                   f"{where}.kind"),
            modules=reader.string_list(value, where, "modules"),
            shipping=reader.boolean(value, where, "shipping", False),
            description=reader.string(value, where, "description"),
        ))
    return targets


def _read_settings(reader: Reader, container: dict, where: str) -> dict[str, object]:
    """Settings are typed by the engine's schema, not by the manifest, so only the shape is checked
    here: a flat object of scalars. `core-config`'s typed layer rejects a value the schema forbids —
    see src/core/config/include/cy/core/config/settings.h."""
    section = container.get("settings", {})
    if not isinstance(section, dict):
        reader.report(where, f"expected an object, found {_kind(section)}")
        return {}
    values: dict[str, object] = {}
    for key in sorted(section):
        value = section[key]
        if isinstance(value, (str, bool, int, float)):
            values[key] = value
        else:
            reader.report(f"{where}.{key}",
                          f"expected a string, number or true/false, found {_kind(value)}")
    return values


def _read_platform_overrides(reader: Reader, root: dict) -> dict[str, dict]:
    section = reader.object_at(root.get("platform_overrides", {}), "manifest.platform_overrides")
    overrides: dict[str, dict] = {}
    for platform in sorted(section):
        where = f"manifest.platform_overrides.{platform}"
        reader.enumerated(platform, PLATFORMS, where)
        value = reader.object_at(section[platform], where)
        reader.keys(value, where, PLATFORM_OVERRIDE_KEYS, ())
        overrides[platform] = {
            "modules": _read_module_overrides(reader, value, where),
            "settings": _read_settings(reader, value, f"{where}.settings"),
        }
    return overrides


def _read_module_overrides(reader: Reader, container: dict, where: str) -> dict[str, dict]:
    section = container.get("modules", {})
    if not isinstance(section, dict):
        reader.report(f"{where}.modules", f"expected an object, found {_kind(section)}")
        return {}
    overrides: dict[str, dict] = {}
    for name in sorted(section):
        entry = reader.object_at(section[name], f"{where}.modules.{name}")
        reader.keys(entry, f"{where}.modules.{name}", MODULE_OVERRIDE_KEYS, ())
        overrides[name] = {
            "enabled": reader.boolean(entry, f"{where}.modules.{name}", "enabled", True),
        }
    return overrides

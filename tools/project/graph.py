"""The project graph, and the four ways it is rejected.

Tasks 4.1, 4.2 and 4.4. `schema.py` decides whether a manifest *parses*; this file decides whether
what it declares is a project. Four failures, each a build error rather than a warning, each naming
both ends of what collided:

  * an **undeclared dependency** — a module using an interface from a module it does not declare.
    Checked twice, because they catch different mistakes: the declared graph catches a dependency
    named nowhere, and the source scan catches one that is used anyway. The specification is explicit
    that this is "a build error, not a link-time accident".
  * a **cycle** — reported as the path that closed it, because the single edge noticed last is
    rarely the one to remove.
  * a **layer violation** — a dependency pointing upward. This is task 4.2: cmake/module.cmake
    enforces the rule between *targets*, and the same rule holds between the project graph's modules,
    which exist before any target does.
  * a **private dependency that leaks** — a public header reaching a privately declared module.
    "Private dependencies SHALL NOT be transitively exposed to dependents."

Plus the graph-level consequence the specification asks to follow from the graph rather than from a
build flag: a **shipping target SHALL NOT reach editor code**, transitively, through any dependency.

Every check appends to a diagnostic list and continues. A project with a cycle and three typos should
report all four.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from schema import (
    EDITOR_LAYERS,
    EDITOR_TYPES,
    Diagnostic,
    Manifest,
    Module,
)

SOURCE_SUFFIXES = (".h", ".hpp", ".hh", ".inl", ".c", ".cc", ".cpp", ".cxx")
INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
VERSION_PATTERN = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:[-+].*)?$")
CLAUSE_PATTERN = re.compile(r"^(>=|<=|==|>|<|\^|~)?\s*(.+)$")


@dataclass
class Graph:
    manifest: Manifest
    diagnostics: list[Diagnostic]

    @property
    def ok(self) -> bool:
        return not self.diagnostics


def validate(manifest: Manifest, *, engine_version: str = "", scan_sources: bool = True) -> Graph:
    """Every check, in the order a reader would want them: names, then edges, then reachability."""
    found: list[Diagnostic] = []
    known = _check_names_are_unique(manifest, found)
    _check_dependencies_exist(manifest, known, found)
    _check_dependency_layers(manifest, known, found)
    _check_cycles(manifest, known, found)
    _check_targets(manifest, known, found)
    _check_content_roots(manifest, found)
    _check_plugins(manifest, engine_version, found)
    _check_platform_overrides(manifest, known, found)
    if scan_sources:
        _check_source_dependencies(manifest, found)
    return Graph(manifest=manifest, diagnostics=found)


# --- Names ---------------------------------------------------------------------------------------

def _check_names_are_unique(manifest: Manifest, found: list[Diagnostic]) -> dict[str, Module]:
    known: dict[str, Module] = {}
    for index, module in enumerate(manifest.modules):
        where = f"manifest.modules[{index}]"
        if not module.name:
            continue
        if module.name in known:
            found.append(Diagnostic(where,
                                    f"a second module is called '{module.name}'. Module names are "
                                    "the project graph's identifiers and must be unique."))
            continue
        known[module.name] = module
    seen: set[str] = set()
    for index, plugin in enumerate(manifest.plugins):
        if plugin.id in seen:
            found.append(Diagnostic(f"manifest.plugins[{index}]",
                                    f"a second plugin declares the identifier '{plugin.id}'. A "
                                    "plugin's identity is its identifier, not its name."))
        seen.add(plugin.id)
    return known


# --- Edges ---------------------------------------------------------------------------------------

def _check_dependencies_exist(manifest: Manifest, known: dict[str, Module],
                              found: list[Diagnostic]) -> None:
    for module in manifest.modules:
        for kind in ("public_dependencies", "private_dependencies"):
            for dependency in getattr(module, kind):
                if dependency not in known:
                    found.append(Diagnostic(
                        f"manifest.modules['{module.name}'].{kind}",
                        f"module '{module.name}' declares a dependency on '{dependency}', which is "
                        f"not a module in this project. Known modules: "
                        f"{', '.join(sorted(known)) or '(none)'}."))
                elif dependency == module.name:
                    found.append(Diagnostic(
                        f"manifest.modules['{module.name}'].{kind}",
                        f"module '{module.name}' depends on itself."))


def _check_dependency_layers(manifest: Manifest, known: dict[str, Module],
                             found: list[Diagnostic]) -> None:
    for module in manifest.modules:
        if module.layer not in _known_layers():
            continue
        for dependency_name in module.dependencies:
            dependency = known.get(dependency_name)
            if dependency is None or dependency.layer not in _known_layers():
                continue
            if dependency.layer_index > module.layer_index:
                found.append(Diagnostic(
                    f"manifest.modules['{module.name}']",
                    f"layering violation: module '{module.name}' (layer {module.layer}, "
                    f"{module.layer_index}) depends on module '{dependency_name}' (layer "
                    f"{dependency.layer}, {dependency.layer_index}). Dependencies point downward "
                    "or within a layer, never upward."))


def _known_layers() -> set[str]:
    from schema import LAYER_INDEX
    return set(LAYER_INDEX)


def _report_cycle(name: str, stack: list[str], reported: set[tuple[str, ...]],
                  found: list[Diagnostic]) -> None:
    """The path that closed the cycle, once per distinct cycle. The path rather than the edge,
    because the edge noticed last is rarely the one to remove."""
    cycle = stack[stack.index(name):] + [name]
    key = tuple(cycle)
    if key in reported:
        return
    reported.add(key)
    found.append(Diagnostic(
        f"manifest.modules['{name}']",
        "dependency cycle between modules: " + " -> ".join(cycle) +
        ". Cycles are rejected: they have no initialisation order."))


def _check_cycles(manifest: Manifest, known: dict[str, Module],
                  found: list[Diagnostic]) -> None:
    """Three-colour depth-first search, with the stack carried down so a cycle can be reported as
    the path that closed it."""
    state: dict[str, str] = {}
    reported: set[tuple[str, ...]] = set()

    def visit(name: str, stack: list[str]) -> None:
        colour = state.get(name, "")
        if colour == "done":
            return
        if colour == "open":
            _report_cycle(name, stack, reported, found)
            return
        state[name] = "open"
        stack.append(name)
        for dependency in known[name].dependencies:
            if dependency in known:
                visit(dependency, stack)
        stack.pop()
        state[name] = "done"

    for name in sorted(known):
        visit(name, [])


# --- Reachability --------------------------------------------------------------------------------

def reachable_modules(known: dict[str, Module], roots: tuple[str, ...]) -> list[str]:
    """Every module reachable from `roots`, including the roots. Sorted, so the answer does not
    depend on traversal order."""
    seen: set[str] = set()
    pending = [name for name in roots if name in known]
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        pending.extend(dep for dep in known[name].dependencies if dep in known)
    return sorted(seen)


def _check_targets(manifest: Manifest, known: dict[str, Module],
                   found: list[Diagnostic]) -> None:
    for index, target in enumerate(manifest.targets):
        where = f"manifest.targets[{index}]"
        for name in target.modules:
            if name not in known:
                found.append(Diagnostic(
                    f"{where}.modules",
                    f"build target '{target.name}' names the module '{name}', which is not a "
                    "module in this project."))
        if not target.shipping:
            continue
        for name in reachable_modules(known, target.modules):
            module = known[name]
            if not module.is_editor_code:
                continue
            path = _dependency_path(known, target.modules, name)
            found.append(Diagnostic(
                where,
                f"shipping target '{target.name}' reaches editor code: module '{name}' is type "
                f"'{module.type}' at layer '{module.layer}', through {' -> '.join(path)}. A "
                "shipping build excludes editor code by the shape of the graph, not by a build "
                "flag."))


def _dependency_path(known: dict[str, Module], roots: tuple[str, ...], goal: str) -> list[str]:
    """The shortest declared path from a target's modules to `goal`, for the diagnostic."""
    queue: list[list[str]] = [[name] for name in roots if name in known]
    seen = {name for name in roots if name in known}
    while queue:
        path = queue.pop(0)
        if path[-1] == goal:
            return path
        for dependency in known[path[-1]].dependencies:
            if dependency in known and dependency not in seen:
                seen.add(dependency)
                queue.append(path + [dependency])
    return [goal]


# --- Content, plugins, overrides -----------------------------------------------------------------

def _check_content_roots(manifest: Manifest, found: list[Diagnostic]) -> None:
    for index, root in enumerate(manifest.content_roots):
        if not (manifest.root / root).is_dir():
            found.append(Diagnostic(f"manifest.content_roots[{index}]",
                                    f"content root '{root}' is not a directory under "
                                    f"{manifest.root}."))


def _check_plugins(manifest: Manifest, engine_version: str, found: list[Diagnostic]) -> None:
    for index, plugin in enumerate(manifest.plugins):
        where = f"manifest.plugins[{index}]"
        if plugin.version and not VERSION_PATTERN.match(plugin.version):
            found.append(Diagnostic(f"{where}.version",
                                    f"'{plugin.version}' is not a major.minor.patch version."))
        satisfied = _range_satisfied(plugin.engine_api, engine_version)
        if satisfied is None:
            found.append(Diagnostic(f"{where}.engine_api",
                                    f"'{plugin.engine_api}' is not a version range. A range is "
                                    "comma-separated clauses such as '>=0.1.0, <0.3.0'."))
        elif engine_version and not satisfied:
            found.append(Diagnostic(
                f"{where}.engine_api",
                f"plugin '{plugin.id}' supports engine API '{plugin.engine_api}', which excludes "
                f"this engine ({engine_version}). It is incompatible and is not loaded."))


def _check_platform_overrides(manifest: Manifest, known: dict[str, Module],
                              found: list[Diagnostic]) -> None:
    for platform, override in sorted(manifest.platform_overrides.items()):
        for name in sorted(override.get("modules", {})):
            if name not in known:
                found.append(Diagnostic(
                    f"manifest.platform_overrides.{platform}.modules.{name}",
                    f"overrides the module '{name}', which is not a module in this project."))


def _parse_version(text: str) -> tuple[int, int, int] | None:
    match = VERSION_PATTERN.match(text.strip())
    if match is None:
        return None
    return int(match.group(1)), int(match.group(2)), int(match.group(3))


def _range_satisfied(spec: str, version: str) -> bool | None:
    """True, False, or None when the range itself is malformed. An empty engine version means the
    range is only checked for shape — which is what a validation with no engine to compare against
    can honestly claim."""
    clauses = [clause.strip() for clause in spec.split(",") if clause.strip()]
    if not clauses:
        return None
    subject = _parse_version(version) if version else None
    satisfied = True
    for clause in clauses:
        match = CLAUSE_PATTERN.match(clause)
        if match is None:
            return None
        operator = match.group(1) or "=="
        bound = _parse_version(match.group(2))
        if bound is None:
            return None
        if subject is None:
            continue
        satisfied = satisfied and _clause_holds(operator, subject, bound)
    return satisfied


def _clause_holds(operator: str, subject: tuple[int, int, int],
                  bound: tuple[int, int, int]) -> bool:
    if operator == "==":
        return subject == bound
    if operator == ">=":
        return subject >= bound
    if operator == ">":
        return subject > bound
    if operator == "<=":
        return subject <= bound
    if operator == "<":
        return subject < bound
    if operator == "^":
        # Compatible-with: the leading non-zero component may not change. 0.x is its own major.
        upper = (bound[0] + 1, 0, 0) if bound[0] else (0, bound[1] + 1, 0)
        return bound <= subject < upper
    if operator == "~":
        return bound <= subject < (bound[0], bound[1] + 1, 0)
    return False


# --- The source scan -----------------------------------------------------------------------------
#
# The declared graph says what a module may use. This says what it does use. A module whose sources
# include a header owned by a module it did not declare is the "undeclared use" the specification
# makes a build error, and it is invisible to the declared graph because nothing in the manifest
# mentions it.
#
# Header ownership is by directory, not by naming convention: everything under <module path>/include
# belongs to that module, keyed by its path relative to that include root, which is exactly the
# spelling a dependent writes in its #include.

def _include_root(manifest: Manifest, module: Module) -> Path | None:
    if not module.path:
        return None
    root = manifest.root / module.path / "include"
    return root if root.is_dir() else None


def _header_owners(manifest: Manifest) -> dict[str, str]:
    owners: dict[str, str] = {}
    for module in manifest.modules:
        root = _include_root(manifest, module)
        if root is None:
            continue
        for header in sorted(root.rglob("*")):
            if header.is_file() and header.suffix in SOURCE_SUFFIXES:
                owners[header.relative_to(root).as_posix()] = module.name
    return owners


def _module_files(manifest: Manifest, module: Module) -> list[tuple[Path, bool]]:
    """Every source file the module owns, paired with whether it is a *public* header."""
    if not module.path:
        return []
    directory = manifest.root / module.path
    if not directory.is_dir():
        return []
    include_root = _include_root(manifest, module)
    files: list[tuple[Path, bool]] = []
    for path in sorted(directory.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        public = include_root is not None and include_root in path.parents
        files.append((path, public))
    return files


def _check_source_dependencies(manifest: Manifest, found: list[Diagnostic]) -> None:
    owners = _header_owners(manifest)
    for module in manifest.modules:
        if not module.path:
            continue
        if not (manifest.root / module.path).is_dir():
            found.append(Diagnostic(
                f"manifest.modules['{module.name}'].path",
                f"'{module.path}' is not a directory under {manifest.root}."))
            continue
        _check_module_sources(manifest, module, owners, found)


def _check_module_sources(manifest: Manifest, module: Module, owners: dict[str, str],
                          found: list[Diagnostic]) -> None:
    public = set(module.public_dependencies)
    declared = set(module.dependencies)
    for path, is_public_header in _module_files(manifest, module):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            found.append(Diagnostic(str(path), f"cannot be read: {error.strerror}"))
            continue
        relative = path.relative_to(manifest.root).as_posix()
        for spelling in INCLUDE_PATTERN.findall(text):
            owner = owners.get(spelling)
            if owner is None or owner == module.name:
                continue
            if owner not in declared:
                found.append(Diagnostic(relative, _undeclared_message(module, owner, spelling)))
            elif is_public_header and owner not in public:
                found.append(Diagnostic(relative, _leak_message(module, owner, spelling)))


def _undeclared_message(module: Module, owner: str, spelling: str) -> str:
    return (f"module '{module.name}' includes <{spelling}>, which module '{owner}' owns, but "
            f"'{module.name}' does not declare a dependency on '{owner}'. Add '{owner}' to "
            f"public_dependencies or private_dependencies, or stop including it. A module only "
            f"uses interfaces from its declared dependencies.")


def _leak_message(module: Module, owner: str, spelling: str) -> str:
    return (f"the public header includes <{spelling}>, which module '{owner}' owns, and "
            f"'{module.name}' declares '{owner}' as a private dependency. A private dependency is "
            f"not transitively exposed to dependents: move the include into the module's sources, "
            f"or declare '{owner}' in public_dependencies.")


def editor_modules(manifest: Manifest) -> list[str]:
    """Modules that are editor code, by type or by layer. Used by the shipping check and reported by
    the CLI's `describe`."""
    return sorted(module.name for module in manifest.modules
                  if module.type in EDITOR_TYPES or module.layer in EDITOR_LAYERS)

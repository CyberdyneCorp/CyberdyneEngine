#!/usr/bin/env python3
"""The project graph's command line: validate a manifest, describe it, render it as C++.

Tasks 4.1 and 4.2. Three subcommands, and one rule between them — the *same* graph code answers all
three, so `validate` cannot pass something `emit-header` would render differently:

    project.py validate     --manifest <path> [--engine-version <v>] [--no-source-scan]
    project.py describe     --manifest <path> [--json]
    project.py emit-header  --output <file> [--manifest <path>] [--module <record>]...

`emit-header` writes `cy_project.h`, the runtime's view of the graph. It is generated rather than
parsed at run time for the reason cy_modules.h is: one manifest parser in the tree, in a language
that has one, instead of a second in C++ that has to agree with it. It is also what lets the engine
answer "what is this project" with no file I/O and no allocation.

Output is reproducible: sorted, no timestamp, no absolute path, and an unchanged file is not
rewritten, so a configure does not invalidate every translation unit that includes it.

`--module <record>` is the shape cmake/modules.cmake already records for cy_modules.h:

    name|layer|layer_index|level|level_index|type|hot_reload|enabled

It is how the engine's own tree renders a project header when it carries no project manifest of its
own: the records still come from module manifests, so the graph is still declared rather than
inferred from the filesystem.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import graph as graph_module  # noqa: E402
import schema  # noqa: E402

MODULE_RECORD_FIELDS = ("name", "layer", "layer_index", "level", "level_index", "type",
                        "hot_reload", "enabled")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="project.py", description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)

    validate = commands.add_parser("validate", help="check a manifest and its graph")
    _add_manifest_arguments(validate)
    validate.add_argument("--no-source-scan", action="store_true",
                          help="skip the include scan; the declared graph is still checked")

    describe = commands.add_parser("describe", help="print the resolved graph")
    _add_manifest_arguments(describe)
    describe.add_argument("--json", action="store_true", help="machine-readable output")

    emit = commands.add_parser("emit-header", help="render cy_project.h")
    _add_manifest_arguments(emit, required=False)
    emit.add_argument("--output", required=True, type=Path)
    emit.add_argument("--project-name", default="")
    emit.add_argument("--project-version", default="")
    emit.add_argument("--module", dest="modules", action="append", default=[],
                      metavar="|".join(MODULE_RECORD_FIELDS),
                      help="a module record, used when there is no project manifest")

    arguments = parser.parse_args(argv)
    return {
        "validate": _validate,
        "describe": _describe,
        "emit-header": _emit_header,
    }[arguments.command](arguments)


def _add_manifest_arguments(parser: argparse.ArgumentParser, *, required: bool = True) -> None:
    parser.add_argument("--manifest", required=required, type=Path,
                        help="the project manifest (project.json)")
    parser.add_argument("--engine-version", default="",
                        help="the engine's version, for plugin API range checks")
    parser.add_argument("--platform", default="",
                        help="the platform whose overrides apply")


# --- validate ------------------------------------------------------------------------------------

def _load(arguments: argparse.Namespace, *, scan_sources: bool = True):
    manifest, diagnostics = schema.read(arguments.manifest)
    if manifest is None:
        return None, diagnostics
    resolved = graph_module.validate(manifest, engine_version=arguments.engine_version,
                                     scan_sources=scan_sources)
    return manifest, diagnostics + resolved.diagnostics


def _validate(arguments: argparse.Namespace) -> int:
    manifest, diagnostics = _load(arguments, scan_sources=not arguments.no_source_scan)
    if diagnostics:
        _report(arguments.manifest, diagnostics)
        return 1
    assert manifest is not None
    print(f"{arguments.manifest}: {manifest.name} {manifest.version} — "
          f"{len(manifest.modules)} module(s), {len(manifest.plugins)} plugin(s), "
          f"{len(manifest.targets)} target(s): the project graph is valid")
    return 0


def _report(path: Path, diagnostics: list[schema.Diagnostic]) -> None:
    print(f"{path}: the project graph was rejected, {len(diagnostics)} entr"
          f"{'y' if len(diagnostics) == 1 else 'ies'}:", file=sys.stderr)
    for diagnostic in diagnostics:
        print(f"  {diagnostic}", file=sys.stderr)


# --- describe ------------------------------------------------------------------------------------

def _describe(arguments: argparse.Namespace) -> int:
    manifest, diagnostics = _load(arguments)
    if manifest is None:
        _report(arguments.manifest, diagnostics)
        return 1

    render = _describe_as_json if arguments.json else _describe_as_text
    render(manifest, diagnostics)
    if diagnostics:
        _report(arguments.manifest, diagnostics)
        return 1
    return 0


def _describe_as_json(manifest: schema.Manifest,
                      diagnostics: list[schema.Diagnostic]) -> None:
    print(json.dumps({
        "project": manifest.name,
        "version": manifest.version,
        "engine_version": manifest.engine_version,
        "modules": [{"name": entry.name, "layer": entry.layer,
                     "registration_level": entry.registration_level,
                     "type": entry.type,
                     "dependencies": list(entry.dependencies)}
                    for entry in manifest.modules_in_registration_order()],
        "plugins": [{"id": entry.id, "version": entry.version} for entry in manifest.plugins],
        "targets": [{"name": entry.name, "kind": entry.kind, "shipping": entry.shipping,
                     "modules": list(entry.modules)} for entry in manifest.targets],
        "diagnostics": [str(entry) for entry in diagnostics],
    }, indent=2, sort_keys=True))


def _describe_as_text(manifest: schema.Manifest,
                      diagnostics: list[schema.Diagnostic]) -> None:
    del diagnostics  # reported separately, so that stdout stays the description
    print(f"{manifest.name} {manifest.version} (engine {manifest.engine_version})")
    print("modules, in registration order:")
    for entry in manifest.modules_in_registration_order():
        arrow = "  -> " + ", ".join(entry.dependencies) if entry.dependencies else ""
        print(f"  {entry.registration_level:<8} {entry.layer:<9} {entry.name}{arrow}")

    known = {module.name: module for module in manifest.modules}
    for entry in manifest.targets:
        reach = ", ".join(graph_module.reachable_modules(known, entry.modules))
        shipping = ", shipping" if entry.shipping else ""
        print(f"target {entry.name} ({entry.kind}{shipping}): {reach}")


# --- emit-header ---------------------------------------------------------------------------------

def _emit_header(arguments: argparse.Namespace) -> int:
    if arguments.manifest is not None:
        manifest, diagnostics = _load(arguments)
        if diagnostics:
            _report(arguments.manifest, diagnostics)
            return 1
        assert manifest is not None
        text = _render_from_manifest(manifest, arguments.platform)
    else:
        text = _render_from_records(arguments)
    _write_if_changed(arguments.output, text)
    return 0


def _render_from_manifest(manifest: schema.Manifest, platform: str) -> str:
    override = manifest.platform_overrides.get(platform, {})
    module_overrides = override.get("modules", {})

    modules = []
    for entry in manifest.modules_in_registration_order():
        enabled = module_overrides.get(entry.name, {}).get("enabled", entry.enabled)
        if not enabled:
            continue
        modules.append((entry.name, entry.layer, entry.layer_index, entry.registration_level,
                        entry.level_index, entry.type, entry.hot_reload))

    settings = [("Project", key, value) for key, value in sorted(manifest.settings.items())]
    settings += [("Platform", key, value)
                 for key, value in sorted(override.get("settings", {}).items())]

    return _render(
        name=manifest.name,
        version=manifest.version,
        engine_version=manifest.engine_version,
        manifest_present=True,
        platform=platform,
        modules=modules,
        content_roots=list(manifest.content_roots),
        plugins=[(entry.id, entry.version, entry.engine_api, entry.enabled)
                 for entry in manifest.plugins],
        targets=[(entry.name, entry.kind, entry.shipping) for entry in manifest.targets],
        settings=settings,
    )


def _render_from_records(arguments: argparse.Namespace) -> str:
    modules = []
    for record in arguments.modules:
        fields = record.split("|")
        if len(fields) != len(MODULE_RECORD_FIELDS):
            print(f"project.py emit-header: --module '{record}' has {len(fields)} fields; the "
                  f"record is {'|'.join(MODULE_RECORD_FIELDS)}", file=sys.stderr)
            raise SystemExit(2)
        name, layer, layer_index, level, level_index, kind, hot_reload, enabled = fields
        if enabled.strip().lower() not in ("1", "on", "true"):
            continue
        modules.append((name, layer, int(layer_index), level, int(level_index), kind,
                        hot_reload.strip().lower() in ("1", "on", "true")))
    modules.sort(key=lambda entry: (entry[4], entry[0]))
    return _render(
        name=arguments.project_name,
        version=arguments.project_version,
        engine_version=arguments.engine_version,
        manifest_present=False,
        platform=arguments.platform,
        modules=modules,
        content_roots=[],
        plugins=[],
        targets=[],
        settings=[],
    )


def _quote(text: object) -> str:
    return '"' + str(text).replace("\\", "\\\\").replace('"', '\\"') + '"'


def _boolean(value: object) -> str:
    return "1" if value else "0"


def _setting_text(value: object) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def _table(macro: str, comment: str, rows: list[str]) -> list[str]:
    lines = [f"// {comment}"]
    if not rows:
        lines += [f"#define {macro}(X)"]
        return lines
    lines.append(f"#define {macro}(X) \\")
    for index, row in enumerate(rows):
        suffix = "" if index + 1 == len(rows) else " \\"
        lines.append(f"    X({row}){suffix}")
    return lines


def _render(*, name: str, version: str, engine_version: str, manifest_present: bool,
            platform: str, modules: list, content_roots: list[str], plugins: list,
            targets: list, settings: list) -> str:
    lines = [
        "// Generated by tools/project/project.py. Do not edit.",
        "//",
        "// The project graph, as the runtime sees it. cmake/project.cmake decides what is in it,",
        "// from the project manifest when there is one and from the module manifests otherwise;",
        "// `project-and-plugins` makes the manifest — never the folder layout — authoritative.",
        "//",
        "// Read it through <cy/core/config/project.h>, which gives these tables names and types.",
        "",
        "#ifndef CY_PROJECT_H",
        "#define CY_PROJECT_H",
        "",
        f"#define CY_PROJECT_NAME {_quote(name)}",
        f"#define CY_PROJECT_VERSION {_quote(version)}",
        f"#define CY_PROJECT_ENGINE_VERSION {_quote(engine_version)}",
        f"#define CY_PROJECT_PLATFORM {_quote(platform)}",
        f"#define CY_PROJECT_MANIFEST_PRESENT {_boolean(manifest_present)}",
        "",
    ]

    lines += _table(
        "CY_PROJECT_MODULE_TABLE",
        "X(name, layer, layer_index, level, level_index, type, hot_reload) — "
        "registration order.",
        [f"{_quote(entry[0])}, {_quote(entry[1])}, {entry[2]}, {_quote(entry[3])}, {entry[4]}, "
         f"{_quote(entry[5])}, {_boolean(entry[6])}" for entry in modules])
    lines += [f"#define CY_PROJECT_MODULE_COUNT {len(modules)}", ""]

    lines += _table("CY_PROJECT_CONTENT_ROOT_TABLE", "X(path)",
                    [_quote(root) for root in content_roots])
    lines += [f"#define CY_PROJECT_CONTENT_ROOT_COUNT {len(content_roots)}", ""]

    lines += _table("CY_PROJECT_PLUGIN_TABLE", "X(id, version, engine_api, enabled)",
                    [f"{_quote(entry[0])}, {_quote(entry[1])}, {_quote(entry[2])}, "
                     f"{_boolean(entry[3])}" for entry in plugins])
    lines += [f"#define CY_PROJECT_PLUGIN_COUNT {len(plugins)}", ""]

    lines += _table("CY_PROJECT_TARGET_TABLE", "X(name, kind, shipping)",
                    [f"{_quote(entry[0])}, {_quote(entry[1])}, {_boolean(entry[2])}"
                     for entry in targets])
    lines += [f"#define CY_PROJECT_TARGET_COUNT {len(targets)}", ""]

    lines += _table("CY_PROJECT_SETTING_TABLE", "X(layer, key, value) — value is text, parsed "
                    "against the setting's declared type.",
                    [f"{_quote(entry[0])}, {_quote(entry[1])}, {_quote(_setting_text(entry[2]))}"
                     for entry in settings])
    lines += [f"#define CY_PROJECT_SETTING_COUNT {len(settings)}", ""]

    lines += ["#endif  // CY_PROJECT_H", ""]
    return "\n".join(lines)


def _write_if_changed(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == text:
        print(f"project header current: {path.name}")
        return
    path.write_text(text, encoding="utf-8")
    print(f"project header written: {path.name}")


if __name__ == "__main__":
    raise SystemExit(main())

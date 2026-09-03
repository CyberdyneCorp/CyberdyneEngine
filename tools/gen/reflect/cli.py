"""The reflection generator's command line. Tasks 1.1.2, 1.1.5 and 1.2.x.

Four things it does, and they share one code path so that a check cannot disagree with the generator
it checks:

    (default)      parse, assign any new identifiers, append to the manifest, write the generated C++
    --check        the same, writing nothing; fails naming what is stale or unrecorded
    --gate         the identity gate alone: drift, internal consistency, and a diff against the
                   committed manifest. This is `just quality-identity`.
    --probe        report whether the frontend is available, for the build to decide with

and two explicit, reviewed manifest edits — `--rename` and `--tombstone` — which are the two answers
to the one question the generator refuses to guess.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import tempfile

import subprocess
import sys
from pathlib import Path

from . import emit, manifest as identity_manifest, parse as frontend
from .attrspec import AttributeError_, load_schemas
from .cache import Cache, digest_of, digest_of_text
from .manifest import IdentityError


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="reflect_gen.py",
        description="Generate reflection metadata from annotated C++ (task 1.1.2).",
    )
    parser.add_argument("--source-root", type=Path, default=Path.cwd(),
                        help="the repository root; every recorded path is relative to it")
    parser.add_argument("--manifest", type=Path, default=Path("identity/manifest.toml"))
    parser.add_argument("--output-dir", type=Path, default=Path("src/core/reflect/generated"))
    parser.add_argument("--header", type=Path, action="append", default=[],
                        help="an annotated header; repeat for each. Never a glob.")
    parser.add_argument("--include", type=Path, action="append", default=[],
                        help="an include directory, passed to the frontend and used to derive a "
                             "header's include path")
    parser.add_argument("--attributes", type=Path, action="append", default=[],
                        help="a module attribute schema (TOML)")
    parser.add_argument("--define", action="append", default=[],
                        help="a preprocessor definition for the parse, without -D")
    parser.add_argument("--cache", type=Path, default=None,
                        help="the incremental cache; belongs in the build directory")
    parser.add_argument("--stamp", type=Path, default=None,
                        help="a file to touch on success, for a build system's dependency edge")
    parser.add_argument("--engine-version", default="0.0.0",
                        help="recorded on a tombstone as the version that removed the declaration")
    parser.add_argument("--check", action="store_true",
                        help="write nothing; fail if the output is stale or an identifier is "
                             "unrecorded")
    parser.add_argument("--gate", action="store_true",
                        help="the identity gate: drift, consistency, and a diff of the committed "
                             "manifest")
    parser.add_argument("--git-ref", default="HEAD",
                        help="the revision the gate diffs the manifest against")
    parser.add_argument("--baseline-manifest", type=Path, default=None,
                        help="diff against this manifest instead of a revision; for a checkout "
                             "with no repository, and for testing the gate itself")
    parser.add_argument("--probe", action="store_true",
                        help="report whether the frontend is available and exit")
    parser.add_argument("--rename", action="append", default=[], metavar="SELECTOR=NEW",
                        help="record a rename; the identifier is unchanged")
    parser.add_argument("--tombstone", action="append", default=[], metavar="SELECTOR",
                        help="retire a declaration; its identifier is never issued again")
    parser.add_argument("--quiet", action="store_true")
    return parser


class Failure(Exception):
    """A reported failure. Its message is the whole diagnostic."""


def main(argv: list[str]) -> int:
    arguments = build_parser().parse_args(argv)
    try:
        return _run(arguments)
    except (Failure, IdentityError, AttributeError_, frontend.ParseError) as error:
        print(f"\nreflect_gen: {error}\n", file=sys.stderr)
        return 1


def _run(arguments) -> int:
    if arguments.probe:
        print(frontend.probe())
        return 0

    if not arguments.header:
        raise Failure(
            "no annotated headers were named.\n"
            "  Every mode of this generator is a gate, and a gate that examined nothing would "
            "report success. Pass --header for each annotated header, or run it through "
            "`just generate-headers`, which reads the list the build declares."
        )

    source_root = arguments.source_root.resolve()
    manifest_path = _absolute(arguments.manifest, source_root)

    # One writer at a time. Four build directories configured from one checkout will each run this,
    # and they share the manifest and the generated tree.
    with _exclusive(manifest_path):
        return _generate(arguments, source_root, manifest_path)


def _generate(arguments, source_root: Path, manifest_path: Path) -> int:
    schemas = load_schemas([_absolute(p, source_root) for p in arguments.attributes], source_root)
    manifest = identity_manifest.load(manifest_path)

    edits = _apply_edits(arguments, manifest)
    if edits and not (arguments.check or arguments.gate):
        _write_manifest(manifest_path, manifest)
        for line in edits:
            _say(arguments, f"identity: {line}")

    headers = _parse_all(arguments, source_root, schemas)
    parsed_types = [parsed_type for header in headers for parsed_type in header.types]

    assign = not (arguments.check or arguments.gate)
    reconciliation = identity_manifest.reconcile(manifest, parsed_types, assign=assign)

    if reconciliation.drift:
        raise Failure(identity_manifest.drift_message(reconciliation.drift))

    if arguments.gate:
        return _gate(arguments, source_root, manifest_path, manifest, reconciliation)

    if arguments.check and reconciliation.appended:
        raise Failure(
            "identity: these declarations have no identifier in the manifest:\n"
            + "".join(f"  {name}\n" for name in reconciliation.appended)
            + "\nRun `just generate-headers` and commit identity/manifest.toml with the change."
        )

    outputs = _render(headers, reconciliation.identity, schemas)
    output_dir = _absolute(arguments.output_dir, source_root)

    if arguments.check:
        return _check_outputs(arguments, output_dir, outputs, source_root)

    if reconciliation.changed:
        _write_manifest(manifest_path, manifest)
        for name in reconciliation.appended:
            _say(arguments, f"identity: assigned an identifier to {name}")

    written = _write_outputs(output_dir, outputs)
    _say(arguments, f"reflect: {len(parsed_types)} type(s) from {len(headers)} header(s); "
                    f"{written} generated file(s) updated")
    _touch(arguments.stamp)
    return 0


# --- Parsing -----------------------------------------------------------------------------------------


def _parse_all(arguments, source_root: Path, schemas):
    include_dirs = [_absolute(directory, source_root) for directory in arguments.include]
    engine = frontend.Frontend(source_root, include_dirs, arguments.define)
    cache = Cache(_absolute(arguments.cache, source_root) if arguments.cache else None)

    schema_digest = digest_of_text(
        "|".join(f"{name}:{schema.struct}:{[p.name for p in schema.parameters]}"
                 for name, schema in sorted(schemas.items()))
    )
    manifest_digest = digest_of(_absolute(arguments.manifest, source_root))

    headers = []
    for header in sorted(arguments.header, key=lambda p: p.as_posix()):
        absolute = _absolute(header, source_root)
        if not absolute.exists():
            raise Failure(f"{header}: no such header. Source lists are explicit and never globbed, "
                          f"so a header named here and absent is a mistake in a CMakeLists.txt.")
        relative = _relative(absolute, source_root)
        include_path = _include_path(absolute, include_dirs, relative)
        stamp = "|".join([engine.toolchain_stamp, emit.GENERATOR_VERSION, schema_digest,
                          manifest_digest])
        cached = cache.lookup(relative, source_root, stamp, schemas)
        if cached is not None:
            headers.append(cached)
            continue
        parsed, _ = engine.parse(absolute, include_path, schemas)
        cache.store(parsed, source_root, stamp)
        headers.append(parsed)
    cache.save()
    return headers


def _include_path(header: Path, include_dirs: list[Path], relative: str) -> str:
    for directory in include_dirs:
        try:
            return header.relative_to(directory).as_posix()
        except ValueError:
            continue
    raise Failure(
        f"{relative} is not under any directory given with --include, so it has no include path "
        f"and generated code could not include it. Add its module's include/ directory."
    )


# --- Output ------------------------------------------------------------------------------------------


def _render(headers, identity, schemas) -> list[emit.Output]:
    outputs: list[emit.Output] = []
    for header in headers:
        used = _schemas_used(header, schemas)
        outputs.append(emit.render_header(header, identity, used))
        outputs.append(emit.render_source(header, identity))
    outputs.append(emit.render_aggregate(headers))
    return sorted(outputs, key=lambda output: output.path)


def _schemas_used(header, schemas):
    names = {
        value.schema.name
        for parsed_type in header.types
        for parsed_field in parsed_type.fields
        for value in parsed_field.attributes.custom
    }
    return [schemas[name] for name in sorted(names) if name in schemas]


def _write_outputs(output_dir: Path, outputs: list[emit.Output]) -> int:
    written = 0
    for output in outputs:
        path = output_dir / output.path
        if path.exists() and path.read_text(encoding="utf-8") == output.text:
            continue  # identical: leave the modification time alone so nothing rebuilds
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_text(output.text, encoding="utf-8")
        temporary.replace(path)
        written += 1
    return written


def _check_outputs(arguments, output_dir: Path, outputs, source_root: Path) -> int:
    stale = []
    for output in outputs:
        path = output_dir / output.path
        if not path.exists():
            stale.append(f"  {_relative(path, source_root)} — missing")
        elif path.read_text(encoding="utf-8") != output.text:
            stale.append(f"  {_relative(path, source_root)} — differs from what the source says")
    if stale:
        raise Failure(
            "generated reflection metadata is stale:\n" + "\n".join(stale) +
            "\n\nRegenerate and commit it:\n      just generate-headers"
        )
    _say(arguments, f"reflect: {len(outputs)} generated file(s) are current")
    return 0


# --- The gate ------------------------------------------------------------------------------------------


def _gate(arguments, source_root: Path, manifest_path: Path, manifest, reconciliation) -> int:
    problems: list[str] = []
    if reconciliation.appended:
        problems.append(
            "these declarations have no identifier in the manifest:\n"
            + "".join(f"      {name}\n" for name in reconciliation.appended)
            + "  Run `just generate-headers` and commit identity/manifest.toml."
        )

    if arguments.baseline_manifest is not None:
        committed = identity_manifest.load(_absolute(arguments.baseline_manifest, source_root))
        against = str(arguments.baseline_manifest)
    else:
        committed = _committed_manifest(source_root, manifest_path, arguments.git_ref)
        against = arguments.git_ref
    if committed is not None:
        problems += [
            f"{problem}" for problem in
            identity_manifest.compare_with_committed(manifest, committed)
        ]

    if problems:
        raise Failure(
            "the identity gate failed.\n\n"
            + "\n".join(f"  {problem}" for problem in problems)
            + "\n\nAn identifier is assigned once and never changes. A deliberate change is a\n"
              "reviewed edit of identity/manifest.toml; an accidental one is this failure."
        )

    if committed is None:
        print(f"identity: no baseline to diff against — identity/manifest.toml is not in "
              f"{arguments.git_ref} yet. The tree and the manifest agree.")
        return 0

    live = sum(1 for entry in manifest.types if entry.status == identity_manifest.LIVE)
    tombstones = sum(
        1 for entry in manifest.types
        for sub in entry.fields if sub.status == identity_manifest.REMOVED
    ) + sum(1 for entry in manifest.types if entry.status == identity_manifest.REMOVED)
    print(f"identity: {live} live type(s), {tombstones} tombstone(s); "
          f"no identifier has moved since {against}")
    return 0


def _committed_manifest(source_root: Path, manifest_path: Path, ref: str):
    """The manifest as of a revision, or None when there is no repository or no such file yet."""
    relative = _relative(manifest_path, source_root)
    try:
        result = subprocess.run(
            ["git", "show", f"{ref}:{relative}"],
            cwd=source_root, capture_output=True, text=True, check=False,
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    return identity_manifest.loads(result.stdout)


# --- The explicit edits ------------------------------------------------------------------------------


def _apply_edits(arguments, manifest) -> list[str]:
    lines: list[str] = []
    for entry in arguments.rename:
        selector, separator, new_name = entry.partition("=")
        if not separator or not new_name:
            raise Failure(f"--rename takes SELECTOR=NEW_NAME; '{entry}' has no '='.")
        lines.append(identity_manifest.rename(manifest, selector.strip(), new_name.strip()))
    for selector in arguments.tombstone:
        lines.append(
            identity_manifest.tombstone(manifest, selector.strip(), arguments.engine_version)
        )
    return lines


# --- Shared -------------------------------------------------------------------------------------------


def _write_manifest(path: Path, manifest) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = identity_manifest.dumps(manifest)
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return
    path.write_text(text, encoding="utf-8")


class _exclusive:
    """A lock around the manifest, held for the run.

    Four build directories configured from one checkout each run this generator, and they share the
    committed manifest and the committed generated tree. The lock is keyed on the manifest's
    absolute path and lives in the system temporary directory rather than beside the manifest: a
    lock file inside a source directory is an artefact somebody eventually commits.
    """

    def __init__(self, manifest_path: Path) -> None:
        key = hashlib.sha256(str(manifest_path.resolve()).encode("utf-8")).hexdigest()[:16]
        self._path = Path(tempfile.gettempdir()) / f"cy-reflect-{key}.lock"
        self._handle = None

    def __enter__(self):
        self._handle = open(self._path, "w", encoding="utf-8")  # noqa: SIM115 - held by the context
        fcntl.flock(self._handle.fileno(), fcntl.LOCK_EX)
        return self

    def __exit__(self, *unused) -> None:
        if self._handle is not None:
            fcntl.flock(self._handle.fileno(), fcntl.LOCK_UN)
            self._handle.close()


def _absolute(path: Path, source_root: Path) -> Path:
    return path if path.is_absolute() else (source_root / path)


def _relative(path: Path, source_root: Path) -> str:
    try:
        return path.resolve().relative_to(source_root).as_posix()
    except ValueError:
        return path.as_posix()


def _touch(path: Path | None) -> None:
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("", encoding="utf-8")


def _say(arguments, message: str) -> None:
    if not arguments.quiet:
        print(message)



"""libclang: the frontend, the pruned descent, and the include digests. Task 1.1.2.

`core-type-system` requires the generator consume "annotated C++ declarations, parsed with a real
C++ frontend rather than a bespoke text scanner". This is that frontend. Three things in here cost
the M1 spike real time and are written down so they are not rediscovered:

**`cursor.kind` is a landmine.** The PyPI `clang` bindings carry a CursorKind table that is
incomplete relative to the libclang they load: reading `.kind` on a `requires` expression inside a
libstdc++ header raises `ValueError: Unknown template argument kind 154` and kills the run. Every
comparison here is against `cursor._kind_id`, so an unrecognised construct fails to match rather
than raising. It only shows up once real standard-library headers are in the parse, so a generator
tested on toy headers passes and then breaks on the first real component.

**Traverse by pruned descent, never `walk_preorder()`.** Recurse from the translation unit, skip any
child whose file is not the header being generated, and descend only into namespaces and records.
That was a 3.5x cold speed-up on its own in the spike, and it is also what keeps output per header
rather than per translation unit — so an included header's types are not emitted twice depending on
parse order.

**Reproducibility is a property of the arguments.** Include directories are passed relative, with
the source root as the working directory, so the argument vector is identical in every build
directory and on every checkout. Diagnostics have the source-root prefix stripped before they are
reported.
"""

from __future__ import annotations

import os
from pathlib import Path

from .attrspec import AttributeError_, CustomSchema, validate
from .annotations import AnnotationError, parse as parse_annotation
from .model import ParsedField, ParsedHeader, ParsedType

# The libclang shared object. The bindings are pinned; the library is the platform's, which is the
# split deps/manifest.toml records. Named explicitly by CY_LIBCLANG when a host keeps it elsewhere.
LIBRARY_CANDIDATES = (
    "/usr/lib/llvm-18/lib/libclang-18.so.1",
    "/usr/lib/x86_64-linux-gnu/libclang-18.so.1",
    "/usr/lib/llvm-18/lib/libclang.so",
    "/opt/homebrew/opt/llvm@18/lib/libclang.dylib",
    "/usr/local/opt/llvm@18/lib/libclang.dylib",
)

BINDINGS_VERSION = "18.1.8"


class ParseError(Exception):
    """A parse that cannot produce metadata. The message is the diagnostic a contributor reads."""


class Frontend:
    """One libclang index and the argument vector every header is parsed with."""

    def __init__(self, source_root: Path, include_dirs: list[Path], definitions: list[str],
                 standard: str = "c++20") -> None:
        self._clang = _load_bindings()
        bind_kinds()
        self._index = self._clang.Index.create()
        self.source_root = source_root.resolve()

        relative_includes = [_relative(directory, self.source_root) for directory in include_dirs]
        self.arguments = [
            "-x", "c++",
            f"-std={standard}",
            "-fno-exceptions",
            "-fno-rtti",
            "-DCY_REFLECT_GENERATOR=1",
        ]
        for definition in definitions:
            self.arguments.append(f"-D{definition}")
        for directory in relative_includes:
            self.arguments += ["-I", directory]

    @property
    def toolchain_stamp(self) -> str:
        """What, other than the source, decides the output. Part of every cache key."""
        return "|".join([BINDINGS_VERSION, _library_path(), *self.arguments])

    def parse(self, header: Path, include_path: str, schemas: dict[str, CustomSchema]
              ) -> tuple[ParsedHeader, list[str]]:
        """Parse one header. Returns its types and the in-tree files it included."""
        relative = _relative(header, self.source_root)
        previous = os.getcwd()
        os.chdir(self.source_root)
        try:
            unit = self._index.parse(relative, args=self.arguments,
                                     options=self._clang.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES)
            self._raise_on_errors(unit, relative)
            result = ParsedHeader(path=relative, include_path=include_path)
            self._descend(unit.cursor, relative, result, schemas)
            includes = _in_tree_includes(unit, self.source_root)
        finally:
            os.chdir(previous)
        result.types.sort(key=lambda parsed: (parsed.line, parsed.name))
        result.includes = includes
        return result, includes

    def _raise_on_errors(self, unit, relative: str) -> None:
        errors = [
            _strip_root(diagnostic.format(), self.source_root)
            for diagnostic in unit.diagnostics
            if diagnostic.severity >= self._clang.Diagnostic.Error
        ]
        if errors:
            joined = "\n  ".join(errors)
            raise ParseError(f"{relative}: the reflection generator could not parse it:\n  {joined}")

    # --- The pruned descent ----------------------------------------------------------------------

    def _descend(self, cursor, relative: str, into: ParsedHeader,
                 schemas: dict[str, CustomSchema]) -> None:
        for child in cursor.get_children():
            if not _declared_in(child, relative):
                continue
            kind = child._kind_id
            if kind == _KIND.namespace:
                self._descend(child, relative, into, schemas)
            elif kind in _KIND.records:
                self._record(child, relative, into, schemas)

    def _record(self, cursor, relative: str, into: ParsedHeader,
                schemas: dict[str, CustomSchema]) -> None:
        annotation = _annotation_of(cursor, "cy.type:")
        if annotation is None:
            # Not reflected. Descend anyway: a reflected type may be nested inside a plain one.
            self._descend(cursor, relative, into, schemas)
            return

        name = _qualified_name(cursor)
        where = f"{relative}:{cursor.location.line}: type '{name}'"
        attributes = _validate(annotation, where, on_type=True, schemas=schemas)
        parsed = ParsedType(name=name, attributes=attributes, header=relative,
                            module=module_of(relative), line=cursor.location.line)
        for child in cursor.get_children():
            if child._kind_id in _KIND.records and _declared_in(child, relative):
                self._record(child, relative, into, schemas)
            elif child._kind_id == _KIND.field:
                field = _field(child, relative, name, schemas)
                if field is not None:
                    parsed.fields.append(field)
        into.types.append(parsed)


def _field(cursor, relative: str, type_name: str,
           schemas: dict[str, CustomSchema]) -> ParsedField | None:
    annotation = _annotation_of(cursor, "cy.field:")
    if annotation is None:
        return None
    where = f"{relative}:{cursor.location.line}: field '{type_name}::{cursor.spelling}'"
    attributes = _validate(annotation, where, on_type=False, schemas=schemas)
    kind = _field_kind(cursor.type, attributes)
    if kind is None:
        raise ParseError(
            f"{where} has type '{cursor.type.spelling}', which reflection cannot carry.\n"
            f"  A reflected field is a fixed-width scalar, a bool, or an enumeration. Strings, "
            f"pointers and containers cross the boundary as a cy::Var, which the values module "
            f"introduces at task 1.3.1.\n"
            f"  Remove CY_REFLECT_FIELD from it, or mark it Transient and give the type a "
            f"reflected field that carries the same information."
        )
    return ParsedField(name=cursor.spelling, kind=kind, attributes=attributes,
                       line=cursor.location.line)


def _validate(annotation: str, where: str, *, on_type: bool,
              schemas: dict[str, CustomSchema]):
    try:
        return validate(parse_annotation(annotation), on_type=on_type, schemas=schemas)
    except (AnnotationError, AttributeError_) as error:
        raise ParseError(f"{where}: {error}") from None


# --- Cursor and type inspection -------------------------------------------------------------------


def _declared_in(cursor, relative: str) -> bool:
    location = cursor.location
    if location is None or location.file is None:
        return False
    return location.file.name == relative


def _annotation_of(cursor, prefix: str) -> str | None:
    """The argument list of the engine's annotation on this cursor, or None."""
    for child in cursor.get_children():
        if child._kind_id != _KIND.annotate:
            continue
        spelling = child.spelling or ""
        if spelling.startswith(prefix):
            return spelling[len(prefix):]
    return None


def _qualified_name(cursor) -> str:
    parts: list[str] = []
    node = cursor
    while node is not None and node._kind_id != _KIND.translation_unit:
        if node.spelling:
            parts.append(node.spelling)
        node = node.semantic_parent
    return "::".join(reversed(parts))


def _field_kind(type_, attributes) -> str | None:
    canonical = type_.get_canonical()
    kind_id = canonical._kind_id
    if kind_id == _KIND.enum_type:
        return "Flags" if attributes.declares("Flags") else "Enum"
    return _SCALARS.get(kind_id)


def _in_tree_includes(unit, source_root: Path) -> list[str]:
    """The repository's own files this translation unit pulled in, source-root relative.

    System headers are deliberately absent: digesting libstdc++ per translation unit costs more than
    the parse it would save, and the toolchain stamp already covers a compiler change.
    """
    found: set[str] = set()
    for inclusion in unit.get_includes():
        name = inclusion.include.name
        path = Path(name)
        if not path.is_absolute():
            path = source_root / path
        try:
            found.add(path.resolve().relative_to(source_root).as_posix())
        except ValueError:
            continue
    return sorted(found)


def module_of(relative: str) -> str:
    """The module a header belongs to, derived from where it is rather than declared.

    A declared module name is one more thing to keep in step with the tree; the path already says
    it, and cmake/module.cmake's own naming is the same derivation.
    """
    parts = relative.split("/")
    if len(parts) >= 3 and parts[0] == "src":
        return f"{parts[1]}-{parts[2]}"
    if len(parts) >= 2 and parts[0] in ("modules", "platform"):
        return parts[1]
    return parts[0] if parts else "unknown"


# --- Bindings and library loading -------------------------------------------------------------------

_clang_module = None
_library_used = ""


def _load_bindings():
    global _clang_module, _library_used  # noqa: PLW0603 - one process-wide libclang, by design
    if _clang_module is not None:
        return _clang_module
    try:
        import clang.cindex as cindex  # noqa: PLC0415 - imported late so --probe can report on it
    except ImportError as error:
        raise ParseError(
            "the libclang Python bindings are not installed.\n"
            f"  The reflection generator parses annotated C++ with a real frontend, and needs the "
            f"pinned bindings ({BINDINGS_VERSION}) and a libclang 18 shared library.\n"
            "      pip install --user clang==18.1.8\n"
            "      sudo apt install libclang1-18        # Debian and Ubuntu\n"
            "      brew install llvm@18                 # macOS\n"
            "  Then rebuild, or run `just generate-headers`."
        ) from error

    library = _pick_library()
    if library:
        cindex.Config.set_library_file(library)
        _library_used = library
    _clang_module = cindex
    return cindex


def _pick_library() -> str:
    named = os.environ.get("CY_LIBCLANG", "")
    if named:
        if not Path(named).exists():
            raise ParseError(f"CY_LIBCLANG names '{named}', which does not exist.")
        return named
    for candidate in LIBRARY_CANDIDATES:
        if Path(candidate).exists():
            return candidate
    return ""  # let the bindings find it themselves; they raise a clear error if they cannot


def _library_path() -> str:
    """The library the stamp records. Its basename only: an absolute path would put a machine's
    layout into the cache key and make the key differ between two identical checkouts."""
    return Path(_library_used).name if _library_used else "bindings-default"


class _Kinds:
    """Cursor and type kind numbers, read from the bindings once.

    Read rather than hard-coded so a bindings upgrade that renumbers something is a clean failure,
    and compared as integers so that an unknown kind on some *other* cursor never raises.
    """

    def __init__(self) -> None:
        self.namespace = 0
        self.records = frozenset()
        self.field = 0
        self.annotate = 0
        self.translation_unit = 0
        self.enum_type = 0
        self.ready = False

    def bind(self, cindex) -> None:
        if self.ready:
            return
        cursor = cindex.CursorKind
        self.namespace = cursor.NAMESPACE.value
        self.records = frozenset({cursor.STRUCT_DECL.value, cursor.CLASS_DECL.value})
        self.field = cursor.FIELD_DECL.value
        self.annotate = cursor.ANNOTATE_ATTR.value
        self.translation_unit = cursor.TRANSLATION_UNIT.value
        self.enum_type = cindex.TypeKind.ENUM.value
        self.ready = True


_KIND = _Kinds()

# The C++ types a reflected field may hold, as canonical libclang type kinds. Filled in when the
# bindings load, because the numbers belong to the bindings and not to this file.
_SCALARS: dict[int, str] = {}

_SCALAR_NAMES = (
    ("BOOL", "Bool"),
    ("SCHAR", "I8"), ("CHAR_S", "I8"),
    ("SHORT", "I16"),
    ("INT", "I32"),
    ("LONG", "I64"), ("LONGLONG", "I64"),
    ("UCHAR", "U8"), ("CHAR_U", "U8"),
    ("USHORT", "U16"),
    ("UINT", "U32"),
    ("ULONG", "U64"), ("ULONGLONG", "U64"),
    ("FLOAT", "F32"),
    ("DOUBLE", "F64"),
)


def bind_kinds() -> None:
    """Resolve the kind numbers. Called once the bindings are known to be importable."""
    cindex = _load_bindings()
    _KIND.bind(cindex)
    if _SCALARS:
        return
    for name, field_kind in _SCALAR_NAMES:
        kind = getattr(cindex.TypeKind, name, None)
        if kind is not None:
            _SCALARS[kind.value] = field_kind
    # long is 64-bit on the LP64 targets the engine builds for and 32-bit on Windows; the width is
    # emitted as sizeof() in generated code either way, so the kind is corrected here rather than
    # producing an I64 descriptor for a 32-bit field.
    if os.name == "nt":
        long_kind = getattr(cindex.TypeKind, "LONG", None)
        ulong_kind = getattr(cindex.TypeKind, "ULONG", None)
        if long_kind is not None:
            _SCALARS[long_kind.value] = "I32"
        if ulong_kind is not None:
            _SCALARS[ulong_kind.value] = "U32"


def probe() -> str:
    """One line naming the bindings and the library, or a ParseError explaining what is missing."""
    bind_kinds()
    return f"clang bindings {BINDINGS_VERSION}, library {_library_used or 'found by the bindings'}"


def _relative(path: Path, source_root: Path) -> str:
    try:
        return path.resolve().relative_to(source_root).as_posix()
    except ValueError:
        return path.resolve().as_posix()


def _strip_root(text: str, source_root: Path) -> str:
    return text.replace(f"{source_root.as_posix()}/", "")

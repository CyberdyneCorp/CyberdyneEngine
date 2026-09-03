"""The attribute table, module-declared attributes, and the validation of both. Task 1.1.3.

`core-type-system` fixes two things this module implements. The first is the table: Range, Enum,
Flags, Hidden, ReadOnly, Category, Tooltip, Transient, Replicated, AssetRef, Unit and Persistence,
each carrying named, typed values rather than a string. The second is the rule that "an unknown or
malformed attribute SHALL be a build error naming the field, rather than a value silently ignored at
runtime" — so every path out of here is either a checked value or an AttributeError naming what was
wrong.

A module declares its own attributes in a TOML schema (see src/core/reflect/reflect_attributes.toml)
and the generator emits them as typed structs exactly like the built-in ones, which is the
specification's last requirement on attributes.
"""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from .annotations import Annotation, AnnotationError

# The unit vocabulary of cy::reflect::UnitKind, and the classification of
# `serialization-and-prefabs`. Both are spelled here exactly as the C++ enumerators are, because the
# generator emits the spelling straight through and a mismatch would be a compile error in generated
# code rather than a diagnostic naming the field.
UNITS = ("None", "Metres", "Radians", "Degrees", "Seconds", "Milliseconds", "Kilograms", "Newtons",
         "Percent")
PERSISTENCE = ("Authoring", "RuntimeState", "PersistentState", "Derived")

# The parameter types a module's own attribute may declare, and the C++ they become.
PARAMETER_TYPES = {
    "bool": "bool",
    "i8": "::cy::i8",
    "i16": "::cy::i16",
    "i32": "::cy::i32",
    "i64": "::cy::i64",
    "u8": "::cy::u8",
    "u16": "::cy::u16",
    "u32": "::cy::u32",
    "u64": "::cy::u64",
    "f32": "::cy::f32",
    "f64": "::cy::f64",
    "string": "const char*",
}


class AttributeError_(Exception):
    """A malformed attribute. The caller prefixes the declaration it was written on."""


@dataclass(frozen=True)
class Parameter:
    name: str
    type: str
    default: object | None = None
    required: bool = True


@dataclass(frozen=True)
class CustomSchema:
    """One attribute a module declared for itself."""

    name: str
    struct: str
    namespace: str
    doc: str
    parameters: tuple[Parameter, ...]

    @property
    def qualified_struct(self) -> str:
        return f"::{self.namespace}::{self.struct}" if self.namespace else f"::{self.struct}"


@dataclass
class CustomValue:
    schema: CustomSchema
    values: dict[str, object]


@dataclass
class Attributes:
    """One declaration's attributes, checked and ready to emit."""

    declared: list[str] = field(default_factory=list)
    range: tuple[float, float, float] | None = None
    enumerators: list[tuple[str, int]] | None = None
    category: str | None = None
    tooltip: str | None = None
    replicated: tuple[str, str, str] | None = None
    asset_ref: str | None = None
    unit: str = "None"
    persistence: str = "Authoring"
    custom: list[CustomValue] = field(default_factory=list)

    def declares(self, name: str) -> bool:
        return name in self.declared


def load_schemas(paths: list[Path], source_root: Path) -> dict[str, CustomSchema]:
    """Read every module attribute schema. Fails naming the file and the attribute."""
    schemas: dict[str, CustomSchema] = {}
    for path in sorted(paths, key=lambda p: p.resolve().as_posix()):
        where = _relative(path, source_root)
        document = tomllib.loads(path.read_text(encoding="utf-8"))
        for entry in document.get("attribute", []):
            schema = _schema_from(entry, where)
            if schema.name in BUILTIN_NAMES:
                raise AttributeError_(
                    f"{where}: attribute '{schema.name}' has the same name as a built-in one. "
                    f"The built-in table is closed; give the module's own attribute another name."
                )
            if schema.name in schemas:
                raise AttributeError_(f"{where}: attribute '{schema.name}' is declared twice.")
            schemas[schema.name] = schema
    return schemas


def _schema_from(entry: dict, where: str) -> CustomSchema:
    for key in ("name", "struct"):
        if key not in entry:
            raise AttributeError_(f"{where}: an [[attribute]] table has no '{key}'.")
    parameters: list[Parameter] = []
    for parameter in entry.get("parameter", []):
        name = parameter.get("name")
        kind = parameter.get("type")
        if not name or not kind:
            raise AttributeError_(
                f"{where}: a parameter of attribute '{entry['name']}' has no name or no type."
            )
        if kind not in PARAMETER_TYPES:
            raise AttributeError_(
                f"{where}: parameter '{name}' of attribute '{entry['name']}' has type '{kind}', "
                f"which is not one of: {', '.join(sorted(PARAMETER_TYPES))}."
            )
        has_default = "default" in parameter
        parameters.append(
            Parameter(name=name, type=kind, default=parameter.get("default"),
                      required=not has_default)
        )
    return CustomSchema(
        name=entry["name"],
        struct=entry["struct"],
        namespace=entry.get("namespace", ""),
        doc=entry.get("doc", ""),
        parameters=tuple(parameters),
    )


# --- The built-in table -------------------------------------------------------------------------
#
# Each entry says how many arguments the attribute takes and what they mean. The shapes are few
# enough to name rather than to encode: a flag carries nothing, a text attribute carries one string,
# an enumeration attribute carries name/value pairs, and the rest are handled one by one below.

BUILTIN_NAMES = frozenset(
    {"Range", "Enum", "Flags", "Hidden", "ReadOnly", "Category", "Tooltip", "Transient",
     "Replicated", "AssetRef", "Unit", "Persistence"}
)

# What a type declaration may carry. A field's table is the whole of the above; a type's is
# presentation only, because the rest describe a value and a type is not one.
TYPE_ATTRIBUTES = frozenset({"Category", "Tooltip"})

_FLAGS = frozenset({"Hidden", "ReadOnly", "Transient"})


def validate(annotations: list[Annotation], *, on_type: bool,
             schemas: dict[str, CustomSchema]) -> Attributes:
    """Check one declaration's attributes and return them ready to emit."""
    result = Attributes()
    seen: set[str] = set()
    allowed = TYPE_ATTRIBUTES if on_type else BUILTIN_NAMES
    for annotation in annotations:
        name = annotation.name
        if name in seen:
            raise AttributeError_(f"attribute '{name}' is declared twice.")
        seen.add(name)
        if name in schemas:
            result.custom.append(_custom(annotation, schemas[name]))
            continue
        if name not in allowed:
            raise AttributeError_(_unknown_message(name, on_type, schemas))
        _apply_builtin(annotation, result)
        result.declared.append(name)
    result.custom.sort(key=lambda value: value.schema.name)
    return result


def _unknown_message(name: str, on_type: bool, schemas: dict[str, CustomSchema]) -> str:
    if not on_type and name in BUILTIN_NAMES:
        return f"attribute '{name}' describes a value and cannot be written on a type."
    if on_type and name in BUILTIN_NAMES:
        return (
            f"attribute '{name}' describes a value and cannot be written on a type. "
            f"A type may carry: {', '.join(sorted(TYPE_ATTRIBUTES))}."
        )
    known = sorted(BUILTIN_NAMES | set(schemas))
    return (
        f"unknown attribute '{name}'. The attributes this build knows are: {', '.join(known)}. "
        f"A module adds its own by declaring it in an attribute schema."
    )


def _apply_builtin(annotation: Annotation, result: Attributes) -> None:
    name = annotation.name
    if name in _FLAGS:
        _expect_no_arguments(annotation)
        return
    if name in ("Category", "Tooltip"):
        text = _one_string(annotation)
        if name == "Category":
            result.category = text
        else:
            result.tooltip = text
        return
    if name == "Range":
        result.range = _range(annotation)
        return
    if name in ("Enum", "Flags"):
        result.enumerators = _enumerators(annotation)
        return
    if name == "Replicated":
        result.replicated = _replicated(annotation)
        return
    if name == "AssetRef":
        result.asset_ref = _one_string(annotation)
        return
    if name == "Unit":
        result.unit = _choice(annotation, UNITS)
        return
    result.persistence = _choice(annotation, PERSISTENCE)  # Persistence


def _expect_no_arguments(annotation: Annotation) -> None:
    if annotation.arguments:
        raise AttributeError_(f"attribute '{annotation.name}' takes no arguments.")


def _positional(annotation: Annotation) -> list[object]:
    for argument in annotation.arguments:
        if argument.key:
            raise AttributeError_(
                f"attribute '{annotation.name}' takes positional arguments, "
                f"but '{argument.key} = ...' was written."
            )
    return [argument.value for argument in annotation.arguments]


def _one_string(annotation: Annotation) -> str:
    values = _positional(annotation)
    if len(values) != 1 or not isinstance(values[0], str):
        raise AttributeError_(f"attribute '{annotation.name}' takes exactly one string.")
    return values[0]


def _range(annotation: Annotation) -> tuple[float, float, float]:
    values = _positional(annotation)
    if len(values) not in (2, 3):
        raise AttributeError_("attribute 'Range' takes a minimum, a maximum, and an optional step.")
    numbers: list[float] = []
    for value in values:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise AttributeError_("every argument of 'Range' is a number.")
        numbers.append(float(value))
    minimum, maximum = numbers[0], numbers[1]
    step = numbers[2] if len(numbers) == 3 else 0.0
    if minimum > maximum:
        raise AttributeError_(
            f"attribute 'Range' has a minimum ({minimum:g}) greater than its maximum "
            f"({maximum:g}), which describes a control that cannot be used."
        )
    if step < 0.0:
        raise AttributeError_(f"attribute 'Range' has a negative step ({step:g}).")
    return minimum, maximum, step


def _enumerators(annotation: Annotation) -> list[tuple[str, int]]:
    if not annotation.arguments:
        raise AttributeError_(
            f"attribute '{annotation.name}' takes at least one 'Name = value' pair."
        )
    pairs: list[tuple[str, int]] = []
    used: dict[int, str] = {}
    for argument in annotation.arguments:
        if not argument.key:
            raise AttributeError_(
                f"every argument of '{annotation.name}' is written 'Name = value'; the value is "
                f"persistent and appears in serialized data, so it cannot be positional."
            )
        if isinstance(argument.value, bool) or not isinstance(argument.value, int):
            raise AttributeError_(
                f"enumerator '{argument.key}' of '{annotation.name}' must have an integer value."
            )
        if argument.value in used:
            raise AttributeError_(
                f"enumerators '{used[argument.value]}' and '{argument.key}' of "
                f"'{annotation.name}' share the value {argument.value}."
            )
        used[argument.value] = argument.key
        pairs.append((argument.key, argument.value))
    return pairs


def _replicated(annotation: Annotation) -> tuple[str, str, str]:
    values = _positional(annotation)
    if not 1 <= len(values) <= 3:
        raise AttributeError_(
            "attribute 'Replicated' takes an encoder, and optionally its parameters and a send "
            "condition."
        )
    for value in values:
        if not isinstance(value, str):
            raise AttributeError_("every argument of 'Replicated' is a string.")
    padded = list(values) + [""] * (3 - len(values))
    return padded[0], padded[1], padded[2]


def _choice(annotation: Annotation, choices: tuple[str, ...]) -> str:
    values = _positional(annotation)
    if len(values) != 1 or not isinstance(values[0], str):
        raise AttributeError_(
            f"attribute '{annotation.name}' takes one of: {', '.join(choices)}."
        )
    if values[0] not in choices:
        raise AttributeError_(
            f"attribute '{annotation.name}' was given '{values[0]}', which is not one of: "
            f"{', '.join(choices)}."
        )
    return values[0]


def _custom(annotation: Annotation, schema: CustomSchema) -> CustomValue:
    given: dict[str, object] = {}
    for index, argument in enumerate(annotation.arguments):
        if argument.key:
            name = argument.key
        elif index < len(schema.parameters):
            name = schema.parameters[index].name
        else:
            raise AttributeError_(
                f"attribute '{schema.name}' was given more arguments than it declares."
            )
        given[name] = argument.value

    declared = {parameter.name for parameter in schema.parameters}
    for name in given:
        if name not in declared:
            raise AttributeError_(
                f"attribute '{schema.name}' has no parameter '{name}'. It declares: "
                f"{', '.join(parameter.name for parameter in schema.parameters)}."
            )

    values: dict[str, object] = {}
    for parameter in schema.parameters:
        if parameter.name in given:
            values[parameter.name] = _coerce(schema.name, parameter, given[parameter.name])
        elif parameter.required:
            raise AttributeError_(
                f"attribute '{schema.name}' requires a value for '{parameter.name}'."
            )
        else:
            values[parameter.name] = parameter.default
    return CustomValue(schema=schema, values=values)


def _coerce(attribute: str, parameter: Parameter, value: object) -> object:
    where = f"parameter '{parameter.name}' of attribute '{attribute}'"
    if parameter.type == "bool":
        if not isinstance(value, bool):
            raise AttributeError_(f"{where} is a bool; 'true' or 'false'.")
        return value
    if parameter.type == "string":
        if not isinstance(value, str):
            raise AttributeError_(f"{where} is a string.")
        return value
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise AttributeError_(f"{where} is a number.")
    if parameter.type.startswith(("i", "u")):
        if isinstance(value, float) and not value.is_integer():
            raise AttributeError_(f"{where} is an integer, but {value!r} was written.")
        return int(value)
    return float(value)


def _relative(path: Path, source_root: Path) -> str:
    try:
        return path.resolve().relative_to(source_root.resolve()).as_posix()
    except ValueError:
        return path.name

"""C spellings to Swift spellings. Task 3.1.

Every rule here is total and mechanical: given a name from the ABI description it either produces a
Swift name or raises. Nothing falls through to "leave it as it was", because a name the generator
does not understand is a name whose Swift spelling nobody chose, and choosing it silently is how a
generated API acquires an entry called `CY_RESULT_OK` sitting beside one called `ok`.
"""

from __future__ import annotations


class NamingError(Exception):
    """A name this generator has no rule for. Always names the offending spelling."""


def _camel(words: list[str]) -> str:
    """['schema', 'too', 'new'] -> 'schemaTooNew'."""
    return words[0] + "".join(word.capitalize() for word in words[1:])


def enum_case(constant: str, prefix: str) -> str:
    """`CY_RESULT_SCHEMA_TOO_NEW`, prefix `CY_RESULT_` -> `schemaTooNew`.

    Swift keywords are back-ticked rather than renamed: `CY_VAR_NIL` becomes `` `nil` ``, which is
    what a reader looking for the nil case expects to type.
    """
    if not constant.startswith(prefix):
        raise NamingError(f"enum constant {constant!r} does not start with {prefix!r}")
    tail = constant[len(prefix) :]
    if not tail:
        raise NamingError(f"enum constant {constant!r} is nothing but its prefix")
    name = _camel([word.lower() for word in tail.split("_")])
    return f"`{name}`" if name in SWIFT_KEYWORDS else name


def member(name: str) -> str:
    """`struct_size` -> `structSize`. A C member name is always lower_snake in this header."""
    if not name or not name.replace("_", "").isalnum():
        raise NamingError(f"member name {name!r} is not lower_snake_case")
    swift = _camel(name.split("_"))
    return f"`{swift}`" if swift in SWIFT_KEYWORDS else swift


def type_name(name: str) -> str:
    """`CyComponentTypeDesc` -> `ComponentTypeDesc`. The `Cy` prefix is the C namespace; Swift has
    modules, so `CyberdyneCore.ComponentTypeDesc` says the same thing without the stutter."""
    if not name.startswith("Cy"):
        raise NamingError(f"ABI type {name!r} does not carry the Cy prefix")
    return name[2:]


def entry_method(name: str) -> str:
    """A table entry's C name to its Swift method name: `world_create_entity` -> `worldCreateEntity`."""
    return member(name)


# Swift's reserved words, restricted to the ones a C identifier from this header can collide with.
# A longer list would be dead weight: every name that reaches these functions is lower_snake ASCII.
SWIFT_KEYWORDS = frozenset(
    {
        "as", "associatedtype", "break", "case", "catch", "class", "continue", "default", "defer",
        "deinit", "do", "else", "enum", "extension", "fallthrough", "false", "fileprivate", "for",
        "func", "guard", "if", "import", "in", "init", "inout", "internal", "is", "let", "nil",
        "operator", "private", "protocol", "public", "repeat", "rethrows", "return", "self",
        "static", "struct", "subscript", "super", "switch", "throw", "throws", "true", "try",
        "typealias", "var", "where", "while",
    }
)

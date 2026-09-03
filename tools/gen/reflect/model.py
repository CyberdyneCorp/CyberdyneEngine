"""What one parse produced, with no libclang in it. Task 1.1.2.

Keeping the parse result free of frontend types is what lets the emitter, the manifest and the cache
be tested without libclang installed, and it is what would make replacing the frontend a change to
parse.py alone.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from .attrspec import Attributes


@dataclass
class ParsedField:
    name: str
    kind: str  # a cy::reflect::FieldKind enumerator, spelled as C++ spells it
    attributes: Attributes
    line: int


@dataclass
class ParsedType:
    name: str  # fully qualified, without a leading "::"
    attributes: Attributes
    fields: list[ParsedField] = field(default_factory=list)
    header: str = ""  # relative to the source root, so the output does not depend on the checkout
    module: str = ""
    line: int = 0

    @property
    def qualified(self) -> str:
        return f"::{self.name}"


@dataclass
class ParsedHeader:
    path: str  # relative to the source root
    include_path: str  # relative to the include directory it was found under
    types: list[ParsedType] = field(default_factory=list)
    includes: list[str] = field(default_factory=list)  # in-tree includes, source-root relative

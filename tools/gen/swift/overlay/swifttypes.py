"""C type spellings to the Swift the importer presents, and to the mirror structs. Task 3.1.

`imported()` is what Swift's C importer already calls a type once `CyberdyneABI` is imported. The
generated `Interface` wrapper is written in these spellings, so its bodies are calls into the
imported function pointers with no conversion at all — a wrapper that converted would be a second
layout model, and the whole reason this overlay is generated is that there is only one.

WHY THERE IS NO SECOND MAPPING FOR "SWIFT MIRRORS OF THE C STRUCTS". Because the C importer already
produces them, and its `CyVar` is the same 32 bytes the engine writes by construction. A generated
`struct Var` beside it would be a hand-modelled copy of a layout that is already exact — one more
declaration that can drift, which is the failure this generator exists to prevent. The types that
genuinely have no C counterpart are the vectors (the ABI carries them as four floats inside a
union), and those ARE generated, with their layout asserted. See emit.math().

`imported()` raises on a type it has no rule for. A mapping that fell back to `Int` for an unknown
spelling would satisfy the compiler and nothing else.
"""

from __future__ import annotations


class TypeError_(Exception):
    """A C type spelling this generator has no Swift rule for."""


# The scalars, spelled as the importer spells them. `bool` is Swift's `Bool` because the header
# includes <stdbool.h>; `char` appears only behind a pointer and is `CChar` there.
_SCALARS = {
    "void": "Void",
    "bool": "Bool",
    "int8_t": "Int8",
    "uint8_t": "UInt8",
    "int16_t": "Int16",
    "uint16_t": "UInt16",
    "int32_t": "Int32",
    "uint32_t": "UInt32",
    "int64_t": "Int64",
    "uint64_t": "UInt64",
    "float": "Float",
    "double": "Double",
    "size_t": "Int",
}

# Pointers whose pointee is not a named ABI struct.
_POINTERS = {
    "const char*": "UnsafePointer<CChar>",
    "char*": "UnsafeMutablePointer<CChar>",
    "const void*": "UnsafeRawPointer",
    "void*": "UnsafeMutableRawPointer",
    "const float*": "UnsafePointer<Float>",
    "float*": "UnsafeMutablePointer<Float>",
    "const uint8_t*": "UnsafePointer<UInt8>",
    "uint8_t*": "UnsafeMutablePointer<UInt8>",
}


def _named_pointer(spelling: str) -> str | None:
    """`const CyVar*` -> `UnsafePointer<CyVar>`, `CyVar*` -> `UnsafeMutablePointer<CyVar>`."""
    if not spelling.endswith("*"):
        return None
    pointee = spelling[:-1].strip()
    mutable = True
    if pointee.startswith("const "):
        pointee = pointee[len("const ") :].strip()
        mutable = False
    if not pointee.startswith("Cy"):
        return None
    kind = "UnsafeMutablePointer" if mutable else "UnsafePointer"
    return f"{kind}<{pointee}>"


def imported(spelling: str, *, optional: bool = False) -> str:
    """The Swift type the C importer gives `spelling`.

    `optional` adds the `?` the importer puts on a nullable pointer. It is a caller's decision
    rather than something derivable, because C has no nullability annotations: the header says which
    entries may return null in prose, and `entries.py` records that per entry.
    """
    spelling = spelling.strip()
    if spelling in _SCALARS:
        return _SCALARS[spelling]
    if spelling in _POINTERS:
        return _POINTERS[spelling] + ("?" if optional else "")
    named = _named_pointer(spelling)
    if named is not None:
        return named + ("?" if optional else "")
    if spelling.startswith("Cy"):
        # A typedef the importer carries through by name: an opaque handle (CyEngine), an integer
        # alias (CyEntity), an enum (CyResult) or a POD struct (CyVar).
        return spelling + ("?" if optional else "")
    raise TypeError_(f"no Swift spelling for the C type {spelling!r}")

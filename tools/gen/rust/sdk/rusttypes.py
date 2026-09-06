"""C type spellings to the Rust the SDK declares. Task 2.3.

`ffi()` is the Rust spelling of a C type inside `#[repr(C)]` declarations and `extern "C"` function
pointers. It is a total function over the ABI's vocabulary and it RAISES on anything else: a mapping
that fell back to `usize` for an unknown spelling would satisfy `rustc` and nothing else, and the
mistake would surface as a corrupted struct rather than as a build failure.

WHY `char` IS `c_char` AND NOT `i8`. They are the same type on every platform this engine targets,
but `c_char` is signed on x86-64 Linux and unsigned on aarch64 Linux, and writing `i8` would make
the SDK's `CStr` conversions stop compiling on an ARM host for a reason nobody would connect to this
file. `std::ffi::c_char` is the spelling that is right on both.

WHY THERE IS NO NULLABILITY RULE HERE. C cannot say whether a returned pointer may be null, so the
generated FFI declares every pointer as a raw pointer and the safe layer decides. Rust's own
nullable-pointer optimisation is used in exactly one place — a table entry is
`Option<unsafe extern "C" fn(..)>`, which is pointer-sized and null-representable by guarantee — and
that is what lets the SDK detect a runtime that handed back a table with a hole in it.
"""

from __future__ import annotations


class RustTypeError(Exception):
    """A C type spelling this generator has no Rust rule for."""


# The scalars. Every one is fixed-width by the C standard or by the platform ABI the engine targets,
# which is the same table `tools/abi/abi_describe.py` computes layout from.
_SCALARS = {
    "void": "()",
    "bool": "bool",
    "char": "::std::ffi::c_char",
    "int8_t": "i8",
    "uint8_t": "u8",
    "int16_t": "i16",
    "uint16_t": "u16",
    "int32_t": "i32",
    "uint32_t": "u32",
    "int64_t": "i64",
    "uint64_t": "u64",
    "float": "f32",
    "double": "f64",
    "size_t": "usize",
}

# A C enum in a signature is an `int`. The header keeps every enumerator inside that range on
# purpose and no enum is ever a struct member, precisely so that its width is never part of a
# layout — see abi_describe.PRIMITIVES for the same rule stated from the layout side. The FFI
# therefore carries the raw `i32`, and `enums.rs` is what turns one into a Rust enum.
_ENUM_REPR = "i32"


def _pointer(spelling: str, enums: frozenset[str], prefix: str) -> str | None:
    """`const CyVar*` -> `*const CyVar`, `void*` -> `*mut ::std::ffi::c_void`."""
    if not spelling.endswith("*"):
        return None
    pointee = spelling[:-1].strip()
    mutability = "*mut "
    if pointee.startswith("const "):
        pointee = pointee[len("const ") :].strip()
        mutability = "*const "
    if pointee == "void":
        return mutability + "::std::ffi::c_void"
    return mutability + ffi(pointee, enums, prefix)


def ffi(spelling: str, enums: frozenset[str], prefix: str = "") -> str:
    """The Rust type an `extern "C"` declaration uses for the C type `spelling`.

    `enums` is the set of enum names in the description, passed in rather than hard-coded so that
    appending an enum to the header needs no edit here.

    `prefix` is what a name declared in `ffi.rs` is reached by from the file being written — empty
    inside `ffi.rs` itself, `"ffi::"` from `interface.rs`. Passed rather than assumed, because a
    generator that emitted bare names into a module that did not declare them would produce a file
    whose errors point at the output rather than at the rule that produced it.
    """
    spelling = spelling.strip()
    if spelling.startswith("struct "):
        spelling = spelling[len("struct ") :].strip()
    if spelling in _SCALARS:
        return _SCALARS[spelling]
    pointer = _pointer(spelling, enums, prefix)
    if pointer is not None:
        return pointer
    if spelling in enums:
        return _ENUM_REPR
    if spelling.startswith("Cy"):
        # A typedef the description declares: an opaque handle, an integer alias, or a POD struct.
        # Each of those becomes a Rust item of the same name in `ffi.rs`.
        return prefix + spelling
    raise RustTypeError(f"no Rust spelling for the C type {spelling!r}")


def array(element: str, count: int, enums: frozenset[str], prefix: str = "") -> str:
    """`float[4]` -> `[f32; 4]`. Split out so a member's type is one call either way."""
    return f"[{ffi(element, enums, prefix)}; {count}]"


def member(spelling: str, enums: frozenset[str], prefix: str = "") -> str:
    """The Rust type of a struct or union member, whose spelling may be an array or a signature."""
    spelling = spelling.strip()
    if spelling.endswith("]") and "[" in spelling:
        element, _, count = spelling[:-1].rpartition("[")
        return array(element.strip(), int(count), enums, prefix)
    if "(*)(" in spelling:
        return function_pointer(spelling, enums, prefix)
    return ffi(spelling, enums, prefix)


def function_pointer(spelling: str, enums: frozenset[str], prefix: str = "") -> str:
    """`void(*)(CyEngine, uint32_t)` -> `Option<unsafe extern "C" fn(CyEngine, u32)>`.

    `Option` rather than a bare `fn` pointer for one reason that matters at run time: a bare
    `extern "C" fn` is a non-null type, so a table entry the engine left as a null pointer would be
    instant undefined behaviour on read. Wrapped in `Option`, the null IS the `None`, at no cost —
    the layout is identical by the null-pointer-optimisation guarantee — and the SDK can report
    "this runtime's table has no `world_chunks`" instead of jumping to zero.
    """
    marker = "(*)("
    index = spelling.find(marker)
    if index < 0 or not spelling.endswith(")"):
        raise RustTypeError(f"{spelling!r} is not a function-pointer spelling")
    returns = spelling[:index].strip()
    inside = spelling[index + len(marker) : -1].strip()
    parameters = [ffi(item, enums, prefix) for item in inside.split(",")] if inside else []
    rendered = ", ".join(parameters)
    suffix = "" if returns == "void" else f" -> {ffi(returns, enums, prefix)}"
    return f'Option<unsafe extern "C" fn({rendered}){suffix}>'

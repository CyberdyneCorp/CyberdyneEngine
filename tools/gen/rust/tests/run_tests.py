#!/usr/bin/env python3
"""The Rust SDK generator's selftest. Task 2.3.

    python3 tools/gen/rust/tests/run_tests.py

--- NO FIXTURE DIRECTORY, FOR THE REASON THE ABI GATE'S SELFTEST GIVES ---------------------------

Every case here edits the LIVE header — src/abi/include/cy/abi/cy_abi.h — in memory and runs the
real generator over the result. A committed "broken header" fixture goes stale, and worse: if the
parser ever stopped recognising the table, a hand-written broken fixture and a hand-written correct
one would both describe nothing, and comparing nothing to nothing succeeds.

CASE 0 IS THE CONTROL. The unedited header generates without error and produces exactly what is
committed. It is what makes the other cases mean anything: a generator that raised on everything
would pass every negative case here.
"""

from __future__ import annotations

import contextlib
import io
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools" / "gen" / "rust"))

from sdk import cli, emit, entries, rusttypes  # noqa: E402

HEADER = ROOT / "src" / "abi" / "include" / "cy" / "abi" / "cy_abi.h"
CRATE = ROOT / "editor" / "crates" / "cy-editor-sdk"

failures: list[str] = []
passes = 0


def case(name: str, body) -> None:
    """Run one case. A case is a function that raises AssertionError to fail."""
    global passes
    try:
        body()
    except AssertionError as error:
        failures.append(f"{name}: {error}")
    except Exception as error:  # noqa: BLE001 — an unexpected exception IS the failure
        failures.append(f"{name}: unexpected {type(error).__name__}: {error}")
    else:
        passes += 1


def describe_edited(replacement: tuple[str, str] | None = None) -> dict:
    """The ABI description, optionally with one edit applied to the header first."""
    text = HEADER.read_text()
    if replacement is not None:
        old, new = replacement
        assert old in text, f"the header no longer contains {old!r}; this case needs rewriting"
        text = text.replace(old, new, 1)
    with tempfile.TemporaryDirectory() as scratch:
        edited = pathlib.Path(scratch) / "cy_abi.h"
        edited.write_text(text)
        return cli.load_description(edited)


def raises(body, fragment: str) -> None:
    """Assert that `body` raises and that its message names `fragment`."""
    try:
        body()
    except Exception as error:  # noqa: BLE001 — any refusal counts; the message is what matters
        assert fragment in str(error), f"expected {fragment!r} in: {error}"
        return
    raise AssertionError(f"expected a refusal naming {fragment!r}, and nothing was raised")


# --- Case 0: the control -------------------------------------------------------------------------


def control() -> None:
    description = describe_edited()
    files = cli.outputs(description)
    assert len(files) == 6, f"six generated files, got {len(files)}"
    with contextlib.redirect_stdout(io.StringIO()):
        status = cli._check(CRATE, files)  # noqa: SLF001 — this is the module's own test
    assert status == 0, "the committed bindings are not what the generator produces"


# --- The entry table cannot drift from the description --------------------------------------------


def appended_entry_without_a_record() -> None:
    """Appending to CyInterface stops generation until somebody names its parameters."""
    description = describe_edited((
        "    CyResult (*world_chunks)(CyWorld world, CyComponentTypeId component, CyChunk* out_chunks,\n"
        "                             uint32_t capacity, uint32_t* out_count);",
        "    CyResult (*world_chunks)(CyWorld world, CyComponentTypeId component, CyChunk* out_chunks,\n"
        "                             uint32_t capacity, uint32_t* out_count);\n"
        "    uint32_t (*world_layer_count)(CyWorld world);"))
    raises(lambda: entries.validate(description), "world_layer_count")


def record_naming_an_entry_the_abi_does_not_have() -> None:
    description = describe_edited()
    entries.ENTRIES["world_invented"] = entries.Entry(("world",), doc="not in the ABI")
    try:
        raises(lambda: entries.validate(description), "world_invented")
    finally:
        del entries.ENTRIES["world_invented"]


def a_record_with_the_wrong_arity() -> None:
    description = describe_edited()
    original = entries.ENTRIES["world_epoch"]
    entries.ENTRIES["world_epoch"] = entries.Entry(("world", "extra"), doc=original.doc)
    try:
        raises(lambda: entries.validate(description), "world_epoch")
    finally:
        entries.ENTRIES["world_epoch"] = original


def a_record_with_no_doc_line() -> None:
    description = describe_edited()
    original = entries.ENTRIES["world_epoch"]
    entries.ENTRIES["world_epoch"] = entries.Entry(original.parameters)
    try:
        raises(lambda: entries.validate(description), "no doc line")
    finally:
        entries.ENTRIES["world_epoch"] = original


def a_record_marked_fallible_that_does_not_return_cyresult() -> None:
    description = describe_edited()
    original = entries.ENTRIES["world_epoch"]
    entries.ENTRIES["world_epoch"] = entries.Entry(
        original.parameters, result="fallible", doc=original.doc)
    try:
        raises(lambda: entries.validate(description), "marked fallible")
    finally:
        entries.ENTRIES["world_epoch"] = original


# --- Enums --------------------------------------------------------------------------------------


def an_enum_with_no_rust_name() -> None:
    """Adding an enum to the header stops generation until somebody chooses its Rust spelling."""
    description = describe_edited((
        "typedef enum CyInitLevel {",
        "typedef enum CyPickMode {\n"
        "    CY_PICK_MODE_NEAREST = 0,\n"
        "    CY_PICK_MODE_ALL = 1\n"
        "} CyPickMode;\n\n"
        "typedef enum CyInitLevel {"))
    raises(lambda: emit.enums(description), "CyPickMode")


def a_cyresult_enumerator_with_no_message() -> None:
    description = describe_edited((
        "    CY_RESULT_MODULE_LOAD_FAILED = 103",
        "    CY_RESULT_MODULE_LOAD_FAILED = 103,\n"
        "    CY_RESULT_SHADER_COMPILE_FAILED = 104"))
    raises(lambda: emit.enums(description), "CY_RESULT_SHADER_COMPILE_FAILED")


def a_variant_name_is_derived_and_not_tabulated() -> None:
    assert emit.variant("CY_RESULT_OUT_OF_RANGE", "CY_RESULT_") == "OutOfRange"
    assert emit.variant("CY_VAR_VEC2", "CY_VAR_") == "Vec2"
    assert emit.variant("CY_VAR_I64", "CY_VAR_") == "I64"
    assert emit.variant("CY_RESULT_IO", "CY_RESULT_") == "Io"
    raises(lambda: emit.variant("CY_VAR_NIL", "CY_RESULT_"), "does not start with")


# --- Types ---------------------------------------------------------------------------------------


def an_unknown_c_type_is_refused_rather_than_guessed() -> None:
    raises(lambda: rusttypes.ffi("long double", frozenset()), "no Rust spelling")


def a_keyword_member_becomes_a_raw_identifier() -> None:
    assert emit.identifier("type") == "r#type"
    assert emit.identifier("name") == "name"
    raises(lambda: emit.identifier("self"), "cannot be spelled")


def a_pointer_carries_its_module_prefix() -> None:
    enums = frozenset({"CyResult"})
    assert rusttypes.ffi("const CyVar*", enums, "ffi::") == "*const ffi::CyVar"
    assert rusttypes.ffi("CyResult", enums, "ffi::") == "i32", "a C enum in a signature is an int"
    assert rusttypes.ffi("const char*", enums) == "*const ::std::ffi::c_char"


def a_table_entry_is_optional_so_a_null_is_reportable() -> None:
    rendered = rusttypes.function_pointer("void(*)(CyEngine, uint32_t)", frozenset(), "ffi::")
    assert rendered.startswith("Option<unsafe extern \"C\" fn("), rendered
    assert "ffi::CyEngine" in rendered


# --- Determinism and the layout model --------------------------------------------------------------


def generation_is_deterministic() -> None:
    description = describe_edited()
    assert cli.outputs(description) == cli.outputs(description)


def the_layout_assertions_cover_every_record() -> None:
    description = describe_edited()
    layout = emit.layout(description)
    records = [item["name"] for item in description["types"]
               if item["kind"] in ("struct", "union")]
    for name in records:
        assert f"size_of::<ffi::{name}>()" in layout, f"{name} has no size assertion"


def the_interface_declares_one_method_per_entry() -> None:
    description = describe_edited()
    interface = emit.interface(description)
    for entry in entries.function_entries(description):
        assert f"pub unsafe fn {entry['name']}(" in interface, f"{entry['name']} has no method"


def the_empty_table_names_every_entry() -> None:
    description = describe_edited()
    ffi = emit.ffi(description)
    for entry in entries.function_entries(description):
        assert f"        {entry['name']}: None," in ffi, f"{entry['name']} is not in EMPTY"


CASES = [
    ("0. control: the unedited header produces exactly what is committed", control),
    ("1. an appended entry with no record stops generation", appended_entry_without_a_record),
    ("2. a record naming an entry the ABI does not have is refused",
     record_naming_an_entry_the_abi_does_not_have),
    ("3. a record whose arity differs from the signature is refused", a_record_with_the_wrong_arity),
    ("4. a record with no doc line is refused", a_record_with_no_doc_line),
    ("5. a record marked fallible that does not return CyResult is refused",
     a_record_marked_fallible_that_does_not_return_cyresult),
    ("6. an enum with no Rust name stops generation", an_enum_with_no_rust_name),
    ("7. a CyResult enumerator with no message stops generation",
     a_cyresult_enumerator_with_no_message),
    ("8. variant names are derived rather than tabulated", a_variant_name_is_derived_and_not_tabulated),
    ("9. an unknown C type is refused rather than guessed",
     an_unknown_c_type_is_refused_rather_than_guessed),
    ("10. a keyword member becomes a raw identifier", a_keyword_member_becomes_a_raw_identifier),
    ("11. a pointer carries its module prefix", a_pointer_carries_its_module_prefix),
    ("12. a table entry is Option, so a null is reportable",
     a_table_entry_is_optional_so_a_null_is_reportable),
    ("13. generation is deterministic", generation_is_deterministic),
    ("14. every record has a layout assertion", the_layout_assertions_cover_every_record),
    ("15. every table entry has a method", the_interface_declares_one_method_per_entry),
    ("16. every table entry is in CyInterface::EMPTY", the_empty_table_names_every_entry),
]


def main() -> int:
    for name, body in CASES:
        case(name, body)
    if failures:
        print(f"\nrust sdk generator selftest: {len(failures)} of {len(CASES)} cases failed\n",
              file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(f"rust sdk generator selftest: {passes}/{len(CASES)} cases pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())

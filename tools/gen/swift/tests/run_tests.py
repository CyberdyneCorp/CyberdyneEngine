#!/usr/bin/env python3
"""The Swift overlay generator's selftest. Task 3.1.

    python3 tools/gen/swift/tests/run_tests.py

--- NO FIXTURE DIRECTORY, FOR THE REASON THE ABI GATE'S SELFTEST GIVES ---------------------------

Every case here edits the LIVE header — src/abi/include/cy/abi/cy_abi.h — in memory and runs the
real generator over the result. A committed "reordered header" fixture goes stale, and worse: if the
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
import shutil
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools" / "gen" / "swift"))

from overlay import cli, cnames, emit, entries, swifttypes  # noqa: E402

HEADER = ROOT / "src" / "abi" / "include" / "cy" / "abi" / "cy_abi.h"
PACKAGE = ROOT / "bindings" / "swift"

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


def expect_raises(exception, body, containing: str) -> None:
    try:
        body()
    except exception as error:
        assert containing in str(error), f"the message did not mention {containing!r}: {error}"
        return
    raise AssertionError(f"expected {exception.__name__} mentioning {containing!r}, nothing raised")


# --- case 0: the control ---------------------------------------------------------------------------


def unedited_header_generates_and_matches_what_is_committed() -> None:
    description = cli.load_description(HEADER)
    files = cli.outputs(description, HEADER)
    assert files, "the generator produced no files at all"
    for name, text in files.items():
        committed = PACKAGE / name
        assert committed.exists(), f"{name} is not committed"
        assert committed.read_text() == text, f"{name} differs from what regeneration produces"


# --- determinism -----------------------------------------------------------------------------------


def two_runs_produce_identical_bytes() -> None:
    description = cli.load_description(HEADER)
    first = cli.outputs(description, HEADER)
    second = cli.outputs(cli.load_description(HEADER), HEADER)
    assert first == second, "two runs over the same header disagreed"


def nothing_generated_carries_a_path_or_a_time() -> None:
    files = cli.outputs(cli.load_description(HEADER), HEADER)
    for name, text in files.items():
        assert str(ROOT) not in text, f"{name} carries an absolute path"
        for marker in ("Generated on", "20", "T00:"):
            if marker == "20":
                continue  # a year prefix is too common a substring to test for usefully
            assert marker not in text, f"{name} carries {marker!r}"


# --- the entry table must cover the ABI exactly ------------------------------------------------------


def an_appended_entry_with_no_label_record_is_refused() -> None:
    description = describe_edited((
        "    /* --- Append new entries below this line. Never above it, never between. ------------------ */",
        "    uint32_t (*world_new_thing)(CyWorld world, uint32_t value);\n"
        "    /* --- Append new entries below this line. ------------------ */"))
    expect_raises(entries.EntryError, lambda: entries.validate(description), "world_new_thing")


def a_record_naming_an_absent_entry_is_refused() -> None:
    description = cli.load_description(HEADER)
    original = dict(entries.ENTRIES)
    entries.ENTRIES["never_existed"] = entries.Entry(("a",))
    try:
        expect_raises(entries.EntryError, lambda: entries.validate(description), "never_existed")
    finally:
        entries.ENTRIES.clear()
        entries.ENTRIES.update(original)


def a_label_count_that_does_not_match_the_signature_is_refused() -> None:
    description = cli.load_description(HEADER)
    original = entries.ENTRIES["world_epoch"]
    entries.ENTRIES["world_epoch"] = entries.Entry(("world", "spurious"))
    try:
        expect_raises(entries.EntryError, lambda: entries.validate(description), "world_epoch")
    finally:
        entries.ENTRIES["world_epoch"] = original


def every_problem_is_reported_not_only_the_first() -> None:
    description = cli.load_description(HEADER)
    original = dict(entries.ENTRIES)
    del entries.ENTRIES["world_epoch"]
    del entries.ENTRIES["borrow_valid"]
    try:
        try:
            entries.validate(description)
        except entries.EntryError as error:
            assert "world_epoch" in str(error) and "borrow_valid" in str(error), \
                f"only one of the two problems was reported: {error}"
        else:
            raise AssertionError("two missing records were accepted")
    finally:
        entries.ENTRIES.clear()
        entries.ENTRIES.update(original)


# --- the emitters refuse what they cannot name --------------------------------------------------------


def an_enum_with_no_swift_name_is_refused() -> None:
    description = describe_edited((
        "typedef enum CyInitLevel {",
        "typedef enum CyBrandNew { CY_BRAND_NEW_ZERO = 0 } CyBrandNew;\n\ntypedef enum CyInitLevel {"))
    expect_raises(entries.EntryError, lambda: emit.enums(description), "CyBrandNew")


def a_removed_vector_kind_is_refused_rather_than_silently_skipped() -> None:
    description = describe_edited(("    CY_VAR_VEC3 = 6,\n", ""))
    expect_raises(entries.EntryError, lambda: emit.math(description), "CY_VAR_VEC3")


def a_c_type_with_no_swift_spelling_is_refused() -> None:
    expect_raises(swifttypes.TypeError_, lambda: swifttypes.imported("struct timespec"), "timespec")


def a_receiver_word_collision_is_refused() -> None:
    """Two entries that would become the same method on one wrapper must be an error.

    The rule that strips `world_` from `world_create_entity` is mechanical, so it CAN collide — an
    appended `world_epoch` beside an existing `epoch` would. Being told is the difference between a
    generated API with two `epoch()` methods that does not compile and one where the second silently
    replaced the first.
    """
    group = [{"name": "world_epoch", "type": "uint64_t(*)(CyWorld)"},
             {"name": "epoch", "type": "uint64_t(*)(CyWorld)"}]
    expect_raises(entries.EntryError,
                  lambda: emit._check_no_collisions("World", group), "epoch")


# --- naming ---------------------------------------------------------------------------------------


def enum_cases_are_camel_cased_and_keywords_are_back_ticked() -> None:
    assert cnames.enum_case("CY_RESULT_SCHEMA_TOO_NEW", "CY_RESULT_") == "schemaTooNew"
    assert cnames.enum_case("CY_VAR_NIL", "CY_VAR_") == "`nil`"
    expect_raises(cnames.NamingError,
                  lambda: cnames.enum_case("WRONG_PREFIX", "CY_VAR_"), "WRONG_PREFIX")


# --- the currency check --------------------------------------------------------------------------


def a_stale_committed_file_is_detected_and_named() -> None:
    # The check prints a diff and a correction, which is what a developer needs and what a passing
    # selftest does not: captured so that this file's output is its verdicts and nothing else.
    with tempfile.TemporaryDirectory() as scratch, \
            contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        copy = pathlib.Path(scratch) / "package"
        shutil.copytree(PACKAGE, copy,
                        ignore=shutil.ignore_patterns(".build", "*.so", "modules"))
        target = copy / "Sources" / "CyberdyneCore" / "Generated" / "Enums.swift"
        target.write_text(target.read_text() + "\n// an edit nobody regenerated\n")
        files = cli.outputs(cli.load_description(HEADER), HEADER)
        assert cli._check(copy, files) == 1, "an edited generated file was reported as current"
        # And the control: the unedited copy is current.
        shutil.rmtree(copy)
        shutil.copytree(PACKAGE, copy,
                        ignore=shutil.ignore_patterns(".build", "*.so", "modules"))
        assert cli._check(copy, files) == 0, "an unedited copy was reported as stale"


CASES = [
    ("case 0 control: the unedited header generates what is committed",
     unedited_header_generates_and_matches_what_is_committed),
    ("determinism: two runs produce identical bytes", two_runs_produce_identical_bytes),
    ("determinism: nothing generated carries a path or a time",
     nothing_generated_carries_a_path_or_a_time),
    ("an appended entry with no label record is refused",
     an_appended_entry_with_no_label_record_is_refused),
    ("a record naming an absent entry is refused", a_record_naming_an_absent_entry_is_refused),
    ("a label count that does not match the signature is refused",
     a_label_count_that_does_not_match_the_signature_is_refused),
    ("every problem is reported, not only the first", every_problem_is_reported_not_only_the_first),
    ("an enum with no Swift name is refused", an_enum_with_no_swift_name_is_refused),
    ("a removed vector kind is refused", a_removed_vector_kind_is_refused_rather_than_silently_skipped),
    ("a C type with no Swift spelling is refused", a_c_type_with_no_swift_spelling_is_refused),
    ("a receiver-word collision is refused", a_receiver_word_collision_is_refused),
    ("enum cases are camel-cased and keywords back-ticked",
     enum_cases_are_camel_cased_and_keywords_are_back_ticked),
    ("a stale committed file is detected and named", a_stale_committed_file_is_detected_and_named),
]


def main() -> int:
    for name, body in CASES:
        case(name, body)
    total = len(CASES)
    if failures:
        for failure in failures:
            print(f"  FAIL {failure}", file=sys.stderr)
        print(f"swift overlay generator selftest: {len(failures)} of {total} cases failed",
              file=sys.stderr)
        return 1
    print(f"swift overlay generator selftest: {passes}/{total} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

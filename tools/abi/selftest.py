#!/usr/bin/env python3
"""Prove the ABI gate, by breaking the ABI on purpose. Tasks 2.3 and 6.2.

design.md §1: "Test it the way M1's identity gate was tested: reorder an entry and watch the build
stop, remove one and watch it stop, append one and watch it pass. A GATE THAT HAS NEVER FAILED IS A
GATE NOBODY KNOWS WORKS."

--- THE FIXTURES ARE DERIVED FROM THE REAL HEADER, NOT COPIED FROM IT ---------------------------

Each case below takes cy/abi/cy_abi.h as it is right now, applies one edit in memory, and runs the
gate against the real committed baseline. Nothing is stored under tools/abi/fixtures/, and that is
the design rather than a shortcut:

  * A copied fixture goes stale. It would be a snapshot of the ABI as it was on the day it was
    written, and every case here would keep passing months after it stopped describing anything.
  * A copied fixture can pass while the gate is broken. If the parser stopped recognising the
    interface table, a hand-written "reordered" fixture and a hand-written "correct" fixture would
    both describe nothing, and comparing nothing to nothing succeeds. Deriving from the live header
    means case 0 — the unedited header matches the baseline — fails loudly first.

Case 0 is therefore load-bearing: it is the control, and every other case is only meaningful because
it passes.

Run through `just quality-abi --selftest`, and by CI through `integration.abi_gate`.
"""

from __future__ import annotations

import io
import json
import pathlib
import re
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import abi_gate  # noqa: E402  (after the path insert, deliberately)

REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
HEADER = REPOSITORY / "src" / "abi" / "include" / "cy" / "abi" / "cy_abi.h"
BASELINE = REPOSITORY / "src" / "abi" / "abi_baseline.json"

# The marker the table's entries end at. Everything above it is the ABI's committed order.
APPEND_MARKER = "/* --- Append new entries below this line."


def table_entry_lines(text: str) -> tuple[int, int]:
    """The half-open line range of the interface table's body."""
    lines = text.splitlines(keepends=True)
    start = next(i for i, line in enumerate(lines) if "typedef struct CyInterface {" in line)
    end = next(i for i, line in enumerate(lines) if APPEND_MARKER in line)
    return start, end


def find_entry(text: str, name: str) -> tuple[int, int]:
    """The line range of one table entry's declaration, comments excluded."""
    lines = text.splitlines(keepends=True)
    start, end = table_entry_lines(text)
    pattern = re.compile(rf"\(\*\s*{re.escape(name)}\s*\)\s*\(")
    for index in range(start, end):
        if pattern.search(lines[index]):
            stop = index
            while ";" not in lines[stop]:
                stop += 1
            return index, stop + 1
    raise AssertionError(f"the table has no entry named {name}")


def swap_entries(text: str, first: str, second: str) -> str:
    """Exchange two entries. The signatures stay; only the ORDER changes, which is the whole point:
    this edit compiles perfectly and calls the wrong function."""
    lines = text.splitlines(keepends=True)
    a_start, a_end = find_entry(text, first)
    b_start, b_end = find_entry(text, second)
    if a_start > b_start:
        a_start, a_end, b_start, b_end = b_start, b_end, a_start, a_end
    return "".join(
        lines[:a_start]
        + lines[b_start:b_end]
        + lines[a_end:b_start]
        + lines[a_start:a_end]
        + lines[b_end:]
    )


def remove_entry(text: str, name: str) -> str:
    lines = text.splitlines(keepends=True)
    start, end = find_entry(text, name)
    return "".join(lines[:start] + lines[end:])


def append_entry(text: str, declaration: str) -> str:
    """Add an entry in the one place an entry may be added."""
    return text.replace(APPEND_MARKER, f"{declaration}\n\n    {APPEND_MARKER}", 1)


def bump_minor(text: str) -> str:
    match = re.search(r"^#define CY_ABI_MINOR (\d+)u$", text, re.MULTILINE)
    assert match, "CY_ABI_MINOR is not declared the way this edit expects"
    return text.replace(match.group(0), f"#define CY_ABI_MINOR {int(match.group(1)) + 1}u", 1)


def run_gate(header_text: str, extra: list[str] | None = None) -> tuple[int, str]:
    """Run the gate over a header held in memory. Returns its exit status and everything it said.

    The committed baseline is used read-only; the one case that needs to write a baseline copies it
    into its own directory first, because a selftest that rewrote the tree's baseline would be a
    test that destroyed the thing it was checking.
    """
    with tempfile.TemporaryDirectory() as directory:
        header = pathlib.Path(directory) / "cy_abi.h"
        header.write_text(header_text, encoding="utf-8")
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            status = abi_gate.main(
                ["--header", str(header), "--baseline", str(BASELINE)] + (extra or [])
            )
        return status, output.getvalue()


def run_gate_with_own_baseline(header_text: str) -> tuple[int, str]:
    """Append, accept, and check again — the whole workflow an additive change goes through."""
    with tempfile.TemporaryDirectory() as directory:
        header = pathlib.Path(directory) / "cy_abi.h"
        header.write_text(header_text, encoding="utf-8")
        baseline = pathlib.Path(directory) / "abi_baseline.json"
        baseline.write_text(BASELINE.read_text(encoding="utf-8"), encoding="utf-8")
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            accepted = abi_gate.main(
                ["--header", str(header), "--baseline", str(baseline), "--update"]
            )
            rechecked = abi_gate.main(["--header", str(header), "--baseline", str(baseline)])
        return (accepted if accepted != 0 else rechecked), output.getvalue()


class Selftest:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0
        self.original = HEADER.read_text(encoding="utf-8")

    def expect(self, label: str, condition: bool, detail: str = "") -> None:
        if condition:
            self.passed += 1
            print(f"  ok   {label}")
        else:
            self.failed += 1
            print(f"  FAIL {label}")
            if detail:
                print("".join(f"         {line}\n" for line in detail.splitlines()))

    def case_unedited_matches(self) -> None:
        status, output = run_gate(self.original)
        self.expect(
            "the header as committed matches the baseline (the control)", status == 0, output
        )

    def case_reordered(self) -> None:
        # Two entries with DIFFERENT signatures, so that the swap is a genuine mis-call rather than
        # two interchangeable slots.
        status, output = run_gate(swap_entries(self.original, "get_last_error", "set_last_error"))
        self.expect("a reordered entry is refused", status == 1, output)
        self.expect(
            "the refusal names the reorder and the entry",
            "reordered" in output and "get_last_error" in output,
            output,
        )

    def case_removed(self) -> None:
        status, output = run_gate(remove_entry(self.original, "var_live_count"))
        self.expect("a removed entry is refused", status == 1, output)
        self.expect(
            "the refusal names the removed entry",
            "var_live_count" in output and "removed" in output,
            output,
        )

    def case_appended(self) -> None:
        declaration = (
            "\n    /* Appended by tools/abi/selftest.py, in memory only. */\n"
            "    uint32_t (*selftest_appended_entry)(CyEngine engine);"
        )
        edited = bump_minor(append_entry(self.original, declaration))
        status, output = run_gate(edited)
        # It is compatible, so it is not refused; it IS stale, because the committed description no
        # longer matches — which is the correct second half of an accepted append.
        self.expect(
            "an appended entry with a minor bump is not an incompatible change",
            "incompatible change" not in output,
            output,
        )
        self.expect(
            "an accepted append asks for the description to be refreshed",
            status == 1 and "stale" in output,
            output,
        )
        # And the whole workflow: append, accept, check again — which is the "watch it pass" half of
        # design.md §1's instruction, and the only one of the three edits that ends green.
        accepted, accepted_output = run_gate_with_own_baseline(edited)
        self.expect(
            "an appended entry passes once the baseline is updated",
            accepted == 0 and "matches the baseline" in accepted_output,
            accepted_output,
        )

    def case_appended_without_bump(self) -> None:
        declaration = "\n    uint32_t (*selftest_appended_entry)(CyEngine engine);"
        status, output = run_gate(append_entry(self.original, declaration))
        self.expect(
            "an append without a minor bump is refused",
            status == 1 and "CY_ABI_MINOR" in output,
            output,
        )

    def case_changed_signature(self) -> None:
        edited = self.original.replace(
            "uint64_t (*var_live_count)(CyEngine engine);",
            "uint32_t (*var_live_count)(CyEngine engine);",
            1,
        )
        assert edited != self.original, "the signature edit did not apply"
        status, output = run_gate(edited)
        self.expect(
            "a changed return type is refused", status == 1 and "var_live_count" in output, output
        )

    def case_inserted_struct_member(self) -> None:
        edited = self.original.replace(
            "    uint64_t length; /* string and bytes: the byte count.",
            "    uint32_t inserted_member;\n    uint64_t length; /* string and bytes: the byte count.",
            1,
        )
        assert edited != self.original, "the struct edit did not apply"
        status, output = run_gate(edited)
        self.expect(
            "a member inserted into a POD struct is refused",
            status == 1 and "CyVar" in output,
            output,
        )

    def case_changed_enum_value(self) -> None:
        edited = self.original.replace("CY_RESULT_NOT_FOUND = 4,", "CY_RESULT_NOT_FOUND = 40,", 1)
        assert edited != self.original, "the enum edit did not apply"
        status, output = run_gate(edited)
        self.expect(
            "a changed enum value is refused",
            status == 1 and "CY_RESULT_NOT_FOUND" in output,
            output,
        )

    def case_approval_is_named(self) -> None:
        _, output = run_gate(remove_entry(self.original, "var_live_count"))
        self.expect(
            "the refusal prints the approval stanza to paste",
            "[[approval]]" in output and "rationale" in output,
            output,
        )

    def case_baseline_is_current(self) -> None:
        # The committed baseline is a real description of a real table, not an empty document that
        # would make every comparison above vacuous.
        baseline = json.loads(BASELINE.read_text(encoding="utf-8"))
        self.expect(
            "the committed baseline describes a non-empty table",
            len(baseline["table"]["entries"]) > 1 and baseline["abi"]["major"] >= 1,
        )

    def run(self) -> int:
        print("abi-gate selftest: breaking the ABI on purpose, against the committed baseline")
        self.case_unedited_matches()
        self.case_reordered()
        self.case_removed()
        self.case_appended()
        self.case_appended_without_bump()
        self.case_changed_signature()
        self.case_inserted_struct_member()
        self.case_changed_enum_value()
        self.case_approval_is_named()
        self.case_baseline_is_current()
        total = self.passed + self.failed
        print(f"selftest: {self.passed}/{total} passed")
        return 1 if self.failed else 0


if __name__ == "__main__":
    raise SystemExit(Selftest().run())

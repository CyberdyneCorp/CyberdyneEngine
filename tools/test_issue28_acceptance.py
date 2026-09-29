#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regression checks for the issue #28 acceptance runner's false-green guards."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import issue28_acceptance as ledger  # noqa: E402


def completed(stdout: str, returncode: int = 0) -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess(args=("unused",), returncode=returncode, stdout=stdout, stderr="")


class AcceptanceLedgerTests(unittest.TestCase):
    def test_every_issue_criterion_has_a_stable_entry_with_a_red_mutation(self) -> None:
        self.assertEqual(
            [item.key for item in ledger.CRITERIA],
            ["bake", "overlay", "obstacle", "history", "stale", "docs"],
        )
        for item in ledger.CRITERIA:
            self.assertTrue(item.probes, item.key)
            self.assertIsNotNone(item.red_mutation, item.key)
            self.assertIsNone(item.gap, item.key)

    def test_native_probes_skip_nothing_and_carry_an_assertion_floor(self) -> None:
        for item in ledger.CRITERIA:
            for probe in item.probes:
                if probe.command[0].startswith("build/dev/"):
                    self.assertIn("--no-skip", probe.command, probe.name)
                    self.assertGreater(probe.min_assertions, 0, probe.name)
                    self.assertNotIn(",", probe.command[1], "doctest splits filters on commas")

    def test_a_native_probe_below_its_assertion_floor_is_not_passed(self) -> None:
        probe = ledger.Probe("native bake", ("build/dev/x",), min_assertions=10)
        device_check_only = completed(
            "[doctest] test cases: 1 | 1 passed | 0 failed\n"
            "[doctest] assertions: 2 | 2 passed | 0 failed\n"
        )
        self.assertEqual(
            ledger.probe_result(probe, device_check_only),
            (False, "only 2 assertions; needs 10"),
        )

    def test_a_native_probe_needs_one_executed_case(self) -> None:
        probe = ledger.Probe("native bake", ("build/dev/x",), min_assertions=10)
        skipped = completed(
            "[doctest] test cases: 0 | 0 passed | 0 failed\n"
            "[doctest] assertions: 0 | 0 passed | 0 failed\n"
        )
        passed = completed(
            "[doctest] test cases: 1 | 1 passed | 0 failed\n"
            "[doctest] assertions: 24 | 24 passed | 0 failed\n"
        )
        self.assertFalse(ledger.probe_result(probe, skipped)[0])
        self.assertEqual(ledger.probe_result(probe, passed), (True, "passed"))

    def test_an_empty_cargo_filter_is_not_passed(self) -> None:
        probe = ledger.Probe("editor history", ("cargo", "test", "missing_case"))
        empty = completed("test result: ok. 0 passed; 0 failed; 8 filtered out\n")
        passed = completed(
            "test result: ok. 0 passed; 0 failed; 8 filtered out\n"
            "test result: ok. 1 passed; 0 failed; 0 filtered out\n"
        )
        self.assertEqual(
            ledger.probe_result(probe, empty),
            (False, "Cargo filter selected no tests"),
        )
        self.assertEqual(ledger.probe_result(probe, passed), (True, "passed"))

    def test_a_failing_command_is_not_passed(self) -> None:
        probe = ledger.Probe("strict OpenSpec", ("openspec", "validate"))
        self.assertEqual(ledger.probe_result(probe, completed("", 1)), (False, "exit 1"))

    def test_the_docs_check_names_each_missing_line(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            for path in ledger.REQUIRED_FILES:
                (root / path).parent.mkdir(parents=True, exist_ok=True)
                (root / path).write_text("", encoding="utf-8")
            for path, snippets in ledger.REQUIRED_GUIDANCE.items():
                (root / path).parent.mkdir(parents=True, exist_ok=True)
                (root / path).write_text("\n".join(snippets), encoding="utf-8")
            self.assertEqual(ledger.missing_docs(root), [])
            readme = root / "editor/README.md"
            readme.write_text(
                readme.read_text(encoding="utf-8").replace("### The Navigation panel", ""),
                encoding="utf-8",
            )
            self.assertEqual(
                ledger.missing_docs(root), ["editor/README.md: ### The Navigation panel"]
            )

    def test_the_shipped_docs_are_complete(self) -> None:
        self.assertEqual(ledger.missing_docs(ledger.ROOT), [])


if __name__ == "__main__":
    unittest.main()

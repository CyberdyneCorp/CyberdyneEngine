#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regression checks for the issue #15 acceptance runner's false-green guard."""

from __future__ import annotations

import subprocess
import unittest

import issue15_acceptance as ledger


class AcceptanceLedgerTests(unittest.TestCase):
    def test_every_issue_criterion_has_a_stable_entry(self) -> None:
        self.assertEqual(
            [item.key for item in ledger.CRITERIA],
            [
                "palette", "sample", "recook", "diagnostic", "displacement",
                "unsupported", "history", "regressions", "docs",
            ],
        )
        open_proofs = {item.key for item in ledger.CRITERIA if item.gap}
        self.assertEqual(
            open_proofs,
            {"displacement", "unsupported", "history", "regressions", "docs"},
        )

    def test_native_availability_check_does_not_count_as_pixel_evidence(self) -> None:
        probe = ledger.Probe("native pixels", ("unused",), min_assertions=10)
        result = subprocess.CompletedProcess(
            args=probe.command,
            returncode=0,
            stdout="[doctest] test cases: 1 | 1 passed | 0 failed\n"
            "[doctest] assertions: 2 | 2 passed | 0 failed\n",
            stderr="no metal device",
        )
        self.assertEqual(
            ledger.probe_result(probe, result),
            (False, "only 2 assertions; needs 10"),
        )

    def test_native_case_needs_one_executed_case_and_enough_assertions(self) -> None:
        probe = ledger.Probe("native pixels", ("unused",), min_assertions=10)
        skipped = subprocess.CompletedProcess(
            args=probe.command,
            returncode=0,
            stdout="[doctest] test cases: 0 | 0 passed | 0 failed\n"
            "[doctest] assertions: 0 | 0 passed | 0 failed\n",
            stderr="",
        )
        passed = subprocess.CompletedProcess(
            args=probe.command,
            returncode=0,
            stdout="[doctest] test cases: 1 | 1 passed | 0 failed\n"
            "[doctest] assertions: 24 | 24 passed | 0 failed\n",
            stderr="",
        )
        self.assertFalse(ledger.probe_result(probe, skipped)[0])
        self.assertEqual(ledger.probe_result(probe, passed), (True, "passed"))

    def test_cargo_filter_must_execute_a_test(self) -> None:
        probe = ledger.Probe("editor history", ("cargo", "test", "missing_case"))
        empty = subprocess.CompletedProcess(
            args=probe.command,
            returncode=0,
            stdout="test result: ok. 0 passed; 0 failed; 8 filtered out\n",
            stderr="",
        )
        passed = subprocess.CompletedProcess(
            args=probe.command,
            returncode=0,
            stdout="test result: ok. 0 passed; 0 failed; 8 filtered out\n"
            "test result: ok. 1 passed; 0 failed; 0 filtered out\n",
            stderr="",
        )
        self.assertEqual(
            ledger.probe_result(probe, empty),
            (False, "Cargo filter selected no tests"),
        )
        self.assertEqual(ledger.probe_result(probe, passed), (True, "passed"))


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Ensure the CI-only roadmap criteria receive the real cross-leg artifacts."""

from __future__ import annotations

import pathlib

from cross_leg_audit import parse_workflow

WORKFLOW = pathlib.Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml"


def change_milestone(text: str, old: str, new: str) -> str:
    before, marker, milestone = text.partition("\n  milestone:\n")
    if not marker or old not in milestone:
        raise ValueError("milestone fixture no longer matches the workflow")
    return before + marker + milestone.replace(old, new, 1)


def check(text: str) -> list[str]:
    jobs = parse_workflow(text).get("jobs", {})
    milestone = jobs.get("milestone", {})
    publisher = jobs.get("cross-leg-publish", {})
    problems = []

    needs = milestone.get("needs", [])
    if isinstance(needs, str):
        needs = [needs]
    if "cross-leg-publish" not in needs:
        problems.append("milestone must wait for cross-leg-publish")
    if "!cancelled()" not in str(milestone.get("if", "")):
        problems.append("milestone must run after a failed publisher leg")

    uploads = [step.get("with", {}) for step in publisher.get("steps", [])
               if "upload-artifact" in str(step.get("uses", ""))]
    downloads = [step.get("with", {}) for step in milestone.get("steps", [])
                 if "download-artifact" in str(step.get("uses", ""))]
    if not uploads or not downloads:
        problems.append("milestone must download the cross-leg publisher artifacts")
    elif not any(item.get("pattern") == "cross-leg-digest-*" and
                 item.get("path") == "cross-leg-digests" for item in downloads):
        problems.append("milestone must put all cross-leg digests where the criteria read them")

    if not any("roadmap-milestone" in str(step.get("run", "")) and
               "--ci" in str(step.get("run", "")) for step in milestone.get("steps", [])):
        problems.append("milestone must evaluate its CI-only criteria")
    return problems


def main() -> int:
    text = WORKFLOW.read_text(encoding="utf-8")
    cases = (
        ("valid workflow", text, False),
        ("publisher dependency deleted", change_milestone(
            text, "needs:\n      - build\n      - cross-leg-publish", "needs: build"), True),
        ("failed publisher skips the ledger", change_milestone(
            text, " && !cancelled()", ""), True),
        ("download deleted", change_milestone(text,
            "      - uses: actions/download-artifact@v5\n"
            "        with:\n"
            "          pattern: cross-leg-digest-*\n"
            "          path: cross-leg-digests\n", ""), True),
        ("download routed elsewhere", change_milestone(text,
            "          pattern: cross-leg-digest-*\n"
            "          path: cross-leg-digests\n"
            "      - name: Install the generator",
            "          pattern: unrelated-*\n"
            "          path: cross-leg-digests\n"
            "      - name: Install the generator"), True),
    )
    for name, candidate, must_fail in cases:
        problems = check(candidate)
        if bool(problems) != must_fail:
            print(f"FAIL {name}: {problems}")
            return 1
    print("cross-leg roadmap artifact routing: green and four mutations red")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

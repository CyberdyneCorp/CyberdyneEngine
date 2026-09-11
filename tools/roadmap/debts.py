#!/usr/bin/env python3
"""Generate `docs/roadmap/open-debts.md`: everything this engine has not finished.

WHY THIS IS GENERATED AND NOT WRITTEN. A hand-written list of unfinished work goes stale exactly
when it matters most — at the moment a milestone closes and somebody adds to it. Every source below
is already maintained for its own reasons, so the list is a view over them rather than a fourth
place to update:

  * the DECLARED GAPS in `tools/roadmap/milestones/*.toml` — criteria that run, fail, and carry the
    rung that must close them. The strongest form, because the ledger enforces both halves: a gap
    that is not closed keeps failing, and a gap that starts PASSING fails the ledger with "THE GAP IS
    CLOSED, DELETE THE DECLARATION".
  * the TIERS in `docs/roadmap/status.yaml` against the plan in `docs/roadmap/capability-matrix.md` —
    which is where a demotion shows up: a capability the plan said would be Complete by now and the
    record holds lower.
  * the PROSE in each archived change's "What this milestone did NOT close" section. Weakest, because
    nothing checks it, and the only place M0 to M8.b's unfinished work is recorded at all — the
    declared-gap mechanism did not exist until M8.c.

Run by `just roadmap-debts`. `--check` regenerates and fails if the committed file differs, which is
what keeps it honest when a milestone closes.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import criteria as criteria_module  # noqa: E402
import plan as plan_module  # noqa: E402
import record as record_module  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
OUTPUT = REPO_ROOT / "docs" / "roadmap" / "open-debts.md"
ARCHIVE = REPO_ROOT / "openspec" / "changes" / "archive"

#: The heading each closing commit writes into its change's task list. Matched as a prefix because
#: the wording drifted — M6's reads "…did NOT close, and why" — and a prefix match keeps an older
#: milestone's account in the document rather than silently dropping it.
NOT_CLOSED = "## What this milestone did NOT close"


def milestone_label(identifier: str) -> str:
    """`m8a` reads as `M8.a` everywhere a person sees it."""
    if identifier == "m5b":
        return "M5.5"
    text = identifier.upper()
    for suffix in ("A", "B", "C"):
        if text.endswith(suffix) and text[:-1].rstrip("0123456789") == "M":
            return f"{text[:-1]}.{suffix.lower()}"
    return text


def declared_gaps() -> list[dict[str, str]]:
    """Every criterion that runs, fails, and names the rung that closes it."""
    gaps: list[dict[str, str]] = []
    for identifier in criteria_module.available():
        for criterion in criteria_module.load(identifier).criteria:
            if not getattr(criterion, "known_gap", None):
                continue
            gaps.append(
                {
                    "from": identifier,
                    "id": criterion.id,
                    "closes": getattr(criterion, "known_gap_closes", "") or "unstated",
                    "why": " ".join(str(criterion.known_gap).split()),
                    "describe": " ".join(str(criterion.describe).split()),
                }
            )
    gaps.sort(key=lambda g: (criteria_module.rung(g["closes"]), g["from"], g["id"]))
    return gaps


def behind_plan() -> list[dict[str, str]]:
    """Capabilities the plan has already promised and the record holds lower.

    A capability whose planned tier for a CLOSED milestone is above its recorded tier is work that
    was scheduled and did not happen. That is a different thing from a capability simply not started
    yet, which is why the comparison is against closed milestones only.
    """
    matrix = plan_module.read_matrix()
    entries = {entry.capability: entry for entry in record_module.load()}
    closed = closed_milestones()
    if not closed:
        return []
    newest = max(closed, key=criteria_module.rung)

    behind: list[dict[str, str]] = []
    for capability, cells in sorted(matrix.cells.items()):
        planned = matrix.tier_at(capability, newest)
        if planned is None:
            continue
        entry = entries.get(capability)
        actual = entry.tier if entry is not None else "none"
        if plan_module.tier_rank(actual) >= plan_module.tier_rank(planned):
            continue
        reached = next(
            (rung for rung in matrix.milestones
             if cells.get(rung) == planned and rung in closed),
            newest,
        )
        behind.append(
            {
                "capability": capability,
                "planned": planned,
                "actual": actual,
                "by": reached,
            }
        )
    return behind


def closed_milestones() -> set[str]:
    """The milestones whose gate is green, which is what `closed` means here."""
    import gates as gates_module

    return {
        gate.milestone
        for gate in gates_module.load().gates
        if gate.klass == "milestone" and gate.state == "green" and gate.milestone
    }


def prose_sections() -> list[tuple[str, str]]:
    """Each archived change's own account of what it did not finish."""
    found: list[tuple[str, str]] = []
    if not ARCHIVE.is_dir():
        return found
    for tasks in sorted(ARCHIVE.glob("*/tasks.md")):
        text = tasks.read_text(encoding="utf-8")
        if NOT_CLOSED not in text:
            continue
        body = text.split(NOT_CLOSED, 1)[1]
        # Drop the remainder of the heading line — "…, and why" — before taking the paragraph.
        body = body.split("\n", 1)[1].strip() if "\n" in body else body.strip()
        # The first paragraph is the summary the closing commit wrote; the rest is the detail, and
        # linking to it beats copying it — a copy is a fourth place to go stale.
        summary = body.split("\n\n", 1)[0].strip()
        found.append((tasks.parent.name, " ".join(summary.split())))
    return found


def render() -> str:
    gaps = declared_gaps()
    behind = behind_plan()
    prose = prose_sections()
    closed = sorted(closed_milestones(), key=criteria_module.rung)

    lines: list[str] = []
    add = lines.append

    add("# Open debts")
    add("")
    add("**Generated by `just roadmap-debts`. Do not edit by hand — your edit will be overwritten,")
    add("and `just roadmap-debts --check` fails when this file and its sources disagree.**")
    add("")
    add("Everything this engine has not finished, from three sources of decreasing strength. It is")
    add("a view over records that are already maintained for their own reasons rather than a fourth")
    add("place to keep up to date, because a hand-written list of unfinished work goes stale exactly")
    add("when it matters most — at the moment a milestone closes and somebody adds to it.")
    add("")
    add(f"Closed milestones: {', '.join(milestone_label(m) for m in closed)}.")
    add("")

    add("## 1. Declared gaps")
    add("")
    add("A criterion that **runs, fails, and names the rung that must close it**. The strongest form,")
    add("because the ledger enforces both halves: an open gap keeps failing, and a gap that starts")
    add("*passing* fails the ledger with \"THE GAP IS CLOSED, DELETE THE DECLARATION\" — so it cannot")
    add("rot into something quietly fixed that nobody noticed.")
    add("")
    add("The mechanism arrived at M8.c. Anything earlier is in section 3, unchecked.")
    add("")
    if gaps:
        add("| Declared at | Gap | Closes at | Why it is open |")
        add("|---|---|---|---|")
        for gap in gaps:
            why = gap["why"]
            why = why if len(why) <= 200 else why[:197] + "…"
            add(f"| {milestone_label(gap['from'])} | `{gap['id']}` | "
                f"**{milestone_label(gap['closes'])}** | {why} |")
    else:
        add("None declared.")
    add("")

    add("## 2. Behind the plan")
    add("")
    add("A capability the plan promised by a milestone that has **closed**, which the status record")
    add("holds lower. This is where a demotion shows up: seven consecutive gates have demoted a row")
    add("rather than accept a claim, and each one is a piece of work the plan still expects.")
    add("")
    if behind:
        add("| Capability | Planned | Recorded | Promised by |")
        add("|---|---|---|---|")
        for row in behind:
            add(f"| [`{row['capability']}`](../../openspec/specs/{row['capability']}/spec.md) | "
                f"{row['planned']} | **{row['actual']}** | {milestone_label(row['by'])} |")
    else:
        add("Nothing behind plan: every closed milestone's promised tiers are met.")
    add("")

    add("## 3. What each milestone said it did not close")
    add("")
    add("Written prose in each archived change, and **nothing checks it**. This is the only record of")
    add("unfinished work before M8.c, because the declared-gap mechanism did not exist yet. Follow the")
    add("link for the detail rather than trusting the summary.")
    add("")
    if prose:
        for name, summary in prose:
            summary = summary if len(summary) <= 320 else summary[:317] + "…"
            add(f"- **[{name}](../../openspec/changes/archive/{name}/tasks.md)** — {summary}")
    else:
        add("No archived change carries that section.")
    add("")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="fail if the committed file differs from what would be generated")
    arguments = parser.parse_args()

    generated = render()
    if not arguments.check:
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        OUTPUT.write_text(generated, encoding="utf-8")
        print(f"roadmap-debts: wrote {OUTPUT.relative_to(REPO_ROOT)}")
        return 0

    if not OUTPUT.exists():
        print(f"roadmap-debts: {OUTPUT.relative_to(REPO_ROOT)} does not exist; "
              "run `just roadmap-debts`", file=sys.stderr)
        return 1
    if OUTPUT.read_text(encoding="utf-8") != generated:
        print(f"roadmap-debts: {OUTPUT.relative_to(REPO_ROOT)} is stale — a debt was declared, "
              "closed or demoted and the document was not regenerated.\n"
              "  Run `just roadmap-debts`.", file=sys.stderr)
        return 1
    print("roadmap-debts: the open-debts document matches its sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())

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
  * the FALSIFIABILITY RECORD in `tools/roadmap/falsifiability.toml` read against the gate states in
    `tools/roadmap/gates.toml` — which is where a criterion that was FAILING when its rung's gate was
    flipped green shows up. Added by M11.c's gate-findings phase, because the other three sources
    cannot see that shape: section 1 only sees a failure somebody DECLARED, section 2 only sees the
    tier, and section 3 only sees what a closing commit chose to write down. See section 4.

Run by `just roadmap-debts`. `--check` regenerates and fails if the committed file differs, which is
what keeps it honest when a milestone closes.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import tomllib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import criteria as criteria_module  # noqa: E402
import plan as plan_module  # noqa: E402
import record as record_module  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
OUTPUT = REPO_ROOT / "docs" / "roadmap" / "open-debts.md"
ARCHIVE = REPO_ROOT / "openspec" / "changes" / "archive"
FALSIFIABILITY = REPO_ROOT / "tools" / "roadmap" / "falsifiability.toml"

#: The heading each closing commit writes into its change's task list. Matched as a prefix because
#: the wording drifted — M6's reads "…did NOT close, and why" — and a prefix match keeps an older
#: milestone's account in the document rather than silently dropping it.
NOT_CLOSED = "## What this milestone did NOT close"


def milestone_label(identifier: str) -> str:
    """`m8a` reads as `M8.a` everywhere a person sees it.

    THE SUFFIX LIST USED TO BE ("A", "B", "C") AND M11's SPLIT CAUGHT IT RENDERING `M11E`. Three
    rungs were enough for M8 and the fourth reader of a milestone identifier was written the same
    way as the three that dropped M8's columns. It is a letter range now, and the rule is the one
    the ladder already uses: a rung identifier is a number with an optional single-letter suffix.
    """
    if identifier == "m5b":
        return "M5.5"
    text = identifier.upper()
    suffix = text[-1:]
    if suffix.isalpha() and text[:-1].rstrip("0123456789") == "M" and text[:-1] != "M":
        return f"{text[:-1]}.{suffix.lower()}"
    return text


def declared_gaps() -> list[dict[str, str]]:
    """Every criterion that runs, fails, and names the rung that closes it.

    `from` IS THE RUNG THAT WROTE THE DECLARATION, WHICH IS NOT ALWAYS THE FILE IT LIVES IN. A gap
    a later rung adds over a gate that is already green carries `known_gap_declared_by`, and reading
    "declared at" off the ledger file instead would credit the declaration to the rung whose gate it
    is a finding ABOUT — printing "declared at M11.a" for a row whose whole content is that M11.a
    closed without declaring it. `owner` keeps the file, because the criterion is still that rung's.
    """
    gaps: list[dict[str, str]] = []
    for identifier in criteria_module.available():
        for criterion in criteria_module.load(identifier).criteria:
            if not getattr(criterion, "known_gap", None):
                continue
            author = getattr(criterion, "known_gap_declared_by", "") or identifier
            gaps.append(
                {
                    "from": author,
                    "owner": identifier,
                    "retroactive": "yes" if author != identifier else "",
                    "id": criterion.id,
                    "closes": getattr(criterion, "known_gap_closes", "") or "unstated",
                    "why": " ".join(str(criterion.known_gap).split()),
                    "describe": " ".join(str(criterion.describe).split()),
                }
            )
    gaps.sort(key=lambda g: (criteria_module.rung(g["closes"]), g["owner"], g["id"]))
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


#: The two proof verdicts `falsify.py` writes when a criterion did not need breaking to be red,
#: because it was already failing. `falsify.py`'s own header: "red in the tree — it FAILS as
#: written, in the sandbox AND in the repository", and "red against a built tree — the same, for a
#: criterion a source-only sandbox cannot run at all". Neither verdict has ever been recorded for a
#: criterion the prover watched PASS, which is what makes the pair readable as "never seen green".
RED_UNMUTATED = ("red in the tree", "red against a built tree")


def never_seen_green() -> list[dict[str, str]]:
    """Criteria of CLOSED milestones that the prover has only ever watched FAIL.

    THE SHAPE THIS SECTION EXISTS FOR, and it is not the one sections 1 to 3 can see. A rung closes
    by flipping `state = "green"` on its gate in `gates.toml`. Nothing in the tooling compares that
    flip against the rung's own ledger verdict, and `roadmap.py`'s arithmetic is explicit that it
    should: `selftest.test_declared_gaps` asserts that "an ordinary failure beside a declared gap
    still fails the milestone". So a criterion that is red and is NOT a declared gap is a criterion
    whose milestone is, by the ledger's own rule, not closed — however green the gate reads.

    `falsifiability.toml` is where that becomes computable without running anything for an hour.
    `falsify.py` records the verdict it observed: a criterion it had to MUTATE to make fail is
    `proven`, and one that was already failing is recorded with one of `RED_UNMUTATED`. For a rung
    that has closed, the second is a statement that the check was failing when the gate was flipped.

    A DECLARED GAP IS EXCLUDED and is section 1's, which is the whole difference between the two
    sections: a gap is a failure the rung declared, dated and pointed at a later rung, and the
    ledger keeps running it. What is listed here is a failure nobody declared at all.

    A RETROACTIVE DECLARATION IS NOT AN EXCLUSION, and this is the half the field
    `known_gap_declared_by` exists for. A gap a LATER rung writes over a gate that is already green
    does not un-flip that gate: the fact the row records is that the gate went green over a failing
    check, and that fact is exactly as true after somebody writes the declaration as before. So a
    criterion carrying `known_gap_declared_by` STAYS HERE as well as appearing in section 1 — with
    the rung that wrote it named, so the two readings cannot be confused. Excluding it would have
    turned M11.c's finding that twenty-one criteria of two closed rungs were red into twenty-one
    tidy rows in the table of debts somebody planned, which is the opposite of what was found.
    """
    if not FALSIFIABILITY.is_file():
        return []
    inventory = tomllib.loads(FALSIFIABILITY.read_text(encoding="utf-8"))
    closed = closed_milestones()
    gaps = {
        (identifier, criterion.id)
        for identifier in criteria_module.available()
        for criterion in criteria_module.load(identifier).criteria
        if getattr(criterion, "known_gap", None)
        and not getattr(criterion, "known_gap_declared_by", "")
    }
    declared_later = {
        (identifier, criterion.id): getattr(criterion, "known_gap_declared_by", "")
        for identifier in criteria_module.available()
        for criterion in criteria_module.load(identifier).criteria
        if getattr(criterion, "known_gap_declared_by", "")
    }
    described = {
        (identifier, criterion.id): " ".join(str(criterion.describe).split())
        for identifier in criteria_module.available()
        for criterion in criteria_module.load(identifier).criteria
    }

    found: list[dict[str, str]] = []
    for entry in inventory.get("proof", ()):
        key = (entry.get("ledger", ""), entry.get("criterion", ""))
        if key[0] not in closed or key in gaps:
            continue
        if entry.get("verdict", "") not in RED_UNMUTATED:
            continue
        found.append(
            {
                "from": key[0],
                "id": key[1],
                "verdict": entry.get("verdict", ""),
                "detail": " ".join(str(entry.get("detail", "")).split()),
                "describe": described.get(key, ""),
                "declared_later_by": declared_later.get(key, ""),
            }
        )
    found.sort(key=lambda row: (criteria_module.rung(row["from"]), row["id"]))
    return found


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
    red = never_seen_green()
    closed = sorted(closed_milestones(), key=criteria_module.rung)

    lines: list[str] = []
    add = lines.append

    add("# Open debts")
    add("")
    add("**Generated by `just roadmap-debts`. Do not edit by hand — your edit will be overwritten,")
    add("and `just roadmap-debts --check` fails when this file and its sources disagree.**")
    add("")
    add("Everything this engine has not finished, from four sources of decreasing strength. It is")
    add("a view over records that are already maintained for their own reasons rather than a fifth")
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
    add("The mechanism arrived at M8.c. Anything earlier is in section 4, unchecked.")
    add("")
    add("**A row marked *retroactive* was written by a LATER rung over a gate that was already")
    add("green.** It is a declaration and a finding at once: the work is owed, and the rung whose")
    add("ledger carries the criterion closed without declaring it. A retroactive row therefore")
    add("appears in section 2 as well, because writing the declaration does not change what the")
    add("gate did.")
    add("")
    if gaps:
        add("| Declared at | Gap | Closes at | Why it is open |")
        add("|---|---|---|---|")
        for gap in gaps:
            why = gap["why"]
            why = why if len(why) <= 200 else why[:197] + "…"
            where = milestone_label(gap["from"])
            if gap["retroactive"]:
                where = (f"{where} <br> *retroactive, over "
                         f"{milestone_label(gap['owner'])}'s green gate*")
            add(f"| {where} | `{gap['id']}` | "
                f"**{milestone_label(gap['closes'])}** | {why} |")
    else:
        add("None declared.")
    add("")

    add("## 2. Red criteria under a green gate")
    add("")
    add("**A criterion of a milestone that has CLOSED, which the falsifiability record says was")
    add("FAILING — and which nobody declared as a gap.** The ledger's own arithmetic is that such a")
    add("criterion blocks: `selftest.test_declared_gaps` asserts in as many words that \"an ordinary")
    add("failure beside a declared gap still fails the milestone\". So every row below is a milestone")
    add("whose gate reads green over a check its own ledger fails, and it is not a margin — a rung")
    add("closes by a person flipping `state = \"green\"` in `gates.toml`, and **nothing in the tooling")
    add("compares that flip against the rung's own verdict.**")
    add("")
    add("How it is read without an hour of running: `falsify.py` records `proven` for a criterion it")
    add("had to BREAK to make fail, and `red in the tree` / `red against a built tree` for one that")
    add("was already failing when it looked. For a rung that has closed, the second pair means the")
    add("check was red when the gate was flipped. A declared gap is excluded — that is section 1,")
    add("and the difference is the whole point: a gap is a failure somebody declared, dated and")
    add("pointed at a later rung, and what is here is a failure nobody declared at all.")
    add("")
    add("**The ledger is the authority on what is red TODAY**; this is what the prover recorded, and")
    add("a row that has since gone green is a row whose recorded verdict `just roadmap-falsify check`")
    add("will report as stale.")
    add("")
    add("**A row whose *Declared later by* cell is filled is a row a LATER rung declared as a gap.**")
    add("It stays here. The declaration says the work is owed and names the rung that owes it; it")
    add("does not reach back and make the closing flip honest, and the two statements are separate.")
    add("")
    later = [row for row in red if row["declared_later_by"]]
    if later:
        gates = sorted({milestone_label(row["from"]) for row in later})
        add(f"**THE GATES OF {' AND '.join(gates)} SHOULD NOT STAND AS THEY ARE READ TODAY.**")
        add(f"{len(later)} criteria below were red when those gates were flipped to")
        add("`state = \"green\"` in `tools/roadmap/gates.toml`, and by the ledger's own arithmetic —")
        add("\"an ordinary failure beside a declared gap still fails the milestone\" — a rung with a")
        add("red criterion nobody declared is not closed. The declarations written over them since")
        add("record the debt; they do not re-run the gate. Either those gates return to")
        add("`joins-on-close` until their ledgers pass, or `gates.toml` states in writing that they")
        add("were closed over named failures and which ones. **Only a Close phase may move a gate**,")
        add("so this document states the finding and moves nothing.")
        add("")
    if red:
        add("| Rung | Criterion | Declared later by | Prover's verdict | What it said |")
        add("|---|---|---|---|---|")
        for row in red:
            detail = row["detail"]
            detail = detail if len(detail) <= 200 else detail[:197] + "…"
            # A recorded detail can quote the criterion's own command, and `determinism-suites`
            # quotes a ctest alternation — so a raw `|` would end the table cell it is describing.
            detail = detail.replace("|", "\\|")
            later_by = (milestone_label(row["declared_later_by"])
                        if row["declared_later_by"] else "— nobody")
            add(f"| {milestone_label(row['from'])} | `{row['id']}` | {later_by} | "
                f"{row['verdict']} | {detail} |")
    else:
        add("None: no closed milestone carries a criterion the prover recorded red and nobody "
            "declared.")
    add("")

    add("## 3. Behind the plan")
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

    add("## 4. What each milestone said it did not close")
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

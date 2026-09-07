#!/usr/bin/env python3
"""Roadmap tooling: what is implemented today, and what closes the current milestone.

Tasks 4.3.1 to 4.3.4 and 4.4.1. Three subcommands, one behind each recipe in just/roadmap.just:

  status              every capability's tier, the milestone that last advanced it, and the change
                      that did so, from docs/roadmap/status.yaml. Exits non-zero when the record and
                      openspec/specs/ disagree — a capability added, renamed or removed without a
                      record entry is drift, and drift fails the build.
  milestone <id>      a milestone's full exit criteria: the permanent set — every closed
                      milestone's criteria, deduplicated — plus the ones this milestone adds, each
                      evaluated exactly once. Exits non-zero if any criterion this host can
                      evaluate fails. It invokes no other ledger; `criteria.build_plan` is the rule
                      and the change that flattened it records what chaining cost.
  gates               the permanent merge-gate set and any recorded override.

Nothing here decides anything: the record, the criteria and the gates are data, and the milestones
after M0 add their own files without touching this one.

Governed by: delivery-roadmap, testing-and-quality (Quality gates for merge).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import criteria as criteria_module  # noqa: E402
import gates as gates_module  # noqa: E402
import record as record_module  # noqa: E402

OK_EXIT, FAILED_EXIT, DATA_EXIT = 0, 1, 2
FAILURE_OUTPUT_LINES = 30


# --- status ---------------------------------------------------------------------------------------


def command_status(arguments: argparse.Namespace) -> int:
    entries = record_module.load(arguments.record)
    capabilities = record_module.specified(arguments.specs)
    drift = record_module.drift(entries, capabilities)

    if arguments.json:
        print(json.dumps(_status_document(entries, drift), indent=2))
        return FAILED_EXIT if drift else OK_EXIT

    _print_status(entries, arguments.all)
    if drift:
        _print_drift(drift, arguments.record)
        return FAILED_EXIT
    print(f"record: {record_module.display(arguments.record)} — in step with "
          f"{record_module.display(arguments.specs)}/ ({len(capabilities)} capabilities)")
    return OK_EXIT


def _print_status(entries: tuple[record_module.Entry, ...], show_all: bool) -> None:
    counts = {tier: sum(1 for entry in entries if entry.tier == tier) for tier in record_module.TIERS}
    print(f"CyberdyneEngine — capability status{'':8}{len(entries)} capabilities")
    print()
    for tier in reversed(record_module.TIERS):
        print(f"  {record_module.TIER_LABEL[tier]:<12}{counts[tier]:>4}")
    print()

    shown = [entry for entry in entries if entry.started or show_all]
    if not shown:
        print("  No capability has left 'not started'. The engine is specified and unimplemented.")
        print()
        return

    width = max(len(entry.capability) for entry in shown)
    print(f"  {'capability':<{width}}  {'tier':<9} {'at':<4} advanced by")
    for entry in _ordered(shown):
        milestone = (entry.milestone or "").upper()
        print(f"  {entry.capability:<{width}}  {entry.tier:<9} {milestone:<4} {entry.change or ''}")
    print()
    hidden = len(entries) - len(shown)
    if hidden:
        print(f"  {hidden} capabilities not started; `just roadmap-status --all` lists them.")
        print()


def _ordered(entries: list[record_module.Entry]) -> list[record_module.Entry]:
    """Most advanced first, then alphabetically: the answer to 'what is implemented' reads top-down."""
    rank = {tier: index for index, tier in enumerate(reversed(record_module.TIERS))}
    return sorted(entries, key=lambda entry: (rank[entry.tier], entry.capability))


def _print_drift(drift: record_module.Drift, record: Path) -> None:
    print(f"roadmap-status: {record_module.display(record)} and the specification set disagree.",
          file=sys.stderr)
    if drift.unrecorded:
        print(f"\n  {len(drift.unrecorded)} capability(ies) have a specification and no entry:",
              file=sys.stderr)
        for capability in drift.unrecorded:
            print(f"      {capability}", file=sys.stderr)
        print("    Add each with tier 'none', or with the tier, milestone and change that "
              "advanced it.", file=sys.stderr)
    if drift.unspecified:
        print(f"\n  {len(drift.unspecified)} entry(ies) name a capability with no specification:",
              file=sys.stderr)
        for capability in drift.unspecified:
            print(f"      {capability}", file=sys.stderr)
        print("    A renamed capability keeps its history: rename the entry rather than adding a "
              "second one.", file=sys.stderr)
    print("\n  The record is the one authoritative answer to what is implemented; it may not lag "
          "the specifications.", file=sys.stderr)


def _status_document(entries: tuple[record_module.Entry, ...], drift: record_module.Drift) -> dict:
    return {
        "capabilities": [
            {"capability": entry.capability, "tier": entry.tier,
             "milestone": entry.milestone, "change": entry.change}
            for entry in entries
        ],
        "drift": {"unrecorded": list(drift.unrecorded), "unspecified": list(drift.unspecified)},
        "in_step": not drift,
    }


# --- milestone ------------------------------------------------------------------------------------


def command_milestone(arguments: argparse.Namespace) -> int:
    gate_set = gates_module.load()
    plan = criteria_module.build_plan(arguments.id, gates_module.permanent_milestones(gate_set))
    _check_criteria_are_gated(plan, gate_set)
    if arguments.list:
        return _list_criteria(plan, arguments.json)

    entries = record_module.load(arguments.record)
    _print_plan(plan)
    results = [_evaluate_and_report(entry, entries, arguments.ci) for entry in plan.entries]
    print()
    return _summarise(plan, results, arguments.json)


def _print_plan(plan: criteria_module.Plan) -> None:
    """What is about to run, and — the point of the flattening — what is NOT about to run twice."""
    print(f"{plan.milestone.id.upper()} — {plan.milestone.name}: "
          f"{len(plan.entries)} exit criteria, each evaluated once")
    if plan.inherited:
        inherited = ", ".join(name.upper() for name in plan.ledgers[:-1])
        print(f"  {len(plan.inherited):>3} from the permanent set ({inherited})")
        print(f"  {len(plan.own):>3} new in {plan.milestone.id.upper()}")
        print(f"  {plan.deduplicated:>3} of {plan.declarations} declarations deduplicated; "
              f"no ledger runs another")
    if plan.milestone.artefact:
        print(f"artefact: {plan.milestone.artefact}")
    print()


def _check_criteria_are_gated(plan: criteria_module.Plan, gate_set: gates_module.GateSet) -> None:
    """Every criterion names a gate in gates.toml. A criterion no gate runs is a criterion in prose."""
    declared = {gate.id for gate in gate_set.gates}
    for entry in plan.entries:
        if entry.criterion.ci_job not in declared:
            raise criteria_module.CriteriaError(
                f"{entry.declared_by[0]}.toml: criterion '{entry.criterion.id}' names CI job "
                f"'{entry.criterion.ci_job}', which is not a gate in tools/roadmap/gates.toml"
            )


def _evaluate_and_report(entry: criteria_module.PlanEntry, entries,
                         force_ci: bool) -> criteria_module.Result:
    criterion = entry.criterion
    print(f"==> {entry.label:<24} {criterion.command}", flush=True)
    result = criteria_module.evaluate(criterion, entries, force_ci)
    if result.status == criteria_module.OK:
        print(f"    ok               {criterion.describe}  ({result.seconds:.1f} s)")
    elif result.status == criteria_module.NOT_EVALUATED:
        print(f"    not evaluated    {result.detail}")
    else:
        _print_failure(entry, result)
    return result


def _print_failure(entry: criteria_module.PlanEntry, result: criteria_module.Result) -> None:
    print(f"    FAILED           {result.detail}  ({result.seconds:.1f} s)")
    print(f"    {result.criterion.describe}")
    if len(entry.declared_by) > 1:
        print(f"    declared by {', '.join(name.upper() for name in entry.declared_by)} — "
              f"one failure, not one per milestone")
    lines = [line for line in result.output.splitlines() if line.strip()]
    # THE LAST LINES ARE NOT WHERE THE FAILURE IS NAMED, and printing only those cost M5.5's gate a
    # day: `just test-all` deliberately runs every suite after a failing one and prints its verdict
    # at the end, so a `four-profiles` failure arrived with thirty lines of passing summary and no
    # test name. The gate re-ran the suite 433 times without reproducing it, because it never knew
    # what to re-run. Lines that NAME a failure are kept wherever they occur, in order, alongside the
    # tail.
    named = [index for index, line in enumerate(lines) if _names_a_failure(line)]
    tail = range(max(0, len(lines) - FAILURE_OUTPUT_LINES), len(lines))
    keep = sorted(set(named) | set(tail))
    previous: int | None = None
    for index in keep:
        if previous is not None and index > previous + 1:
            print(f"      | ... {index - previous - 1} line(s)")
        print(f"      | {lines[index]}")
        previous = index
    if keep and keep[0] > 0:
        print(f"      | reproduce with: {result.criterion.command}")


#: Substrings that mean "this line names what failed". Deliberately small: a wider net would bury the
#: tail it is meant to supplement, and every entry here is a marker some runner in this repository
#: actually prints.
FAILURE_MARKERS = ("FAILED", "FAIL:", "error:", "Errors while running", "SIGSEGV", "SIGTRAP",
                   "Assertion", "panicked at")


def _names_a_failure(line: str) -> bool:
    return any(marker in line for marker in FAILURE_MARKERS)


def _list_criteria(plan: criteria_module.Plan, as_json: bool) -> int:
    if as_json:
        print(json.dumps(_milestone_document(plan), indent=2))
        return OK_EXIT
    print(f"{plan.milestone.id.upper()} — {plan.milestone.name}: {len(plan.entries)} criteria, "
          f"{len(plan.inherited)} from the permanent set and {len(plan.own)} new here")
    for entry in plan.entries:
        criterion = entry.criterion
        where = "CI only" if criterion.where == "ci" else criterion.requires or "here"
        print(f"  {entry.label:<24} {where:<9} {criterion.ci_job:<16} {criterion.source}")
        print(f"  {'':<24} {criterion.describe}")
    return OK_EXIT


def _milestone_document(plan: criteria_module.Plan, results=()) -> dict:
    return {
        "milestone": plan.milestone.id,
        "name": plan.milestone.name,
        "artefact": plan.milestone.artefact,
        "notes": list(plan.milestone.notes),
        "ledgers": list(plan.ledgers),
        "declarations": plan.declarations,
        "deduplicated": plan.deduplicated,
        "criteria": [
            _criterion_document(entry, result)
            for entry, result in zip(plan.entries, results or (None,) * len(plan.entries))
        ],
    }


def _criterion_document(entry: criteria_module.PlanEntry, result) -> dict:
    criterion = entry.criterion
    return {
        "id": criterion.id,
        "label": entry.label,
        "declared_by": list(entry.declared_by),
        "permanent": entry.permanent,
        "describe": criterion.describe,
        "source": criterion.source,
        "kind": criterion.kind,
        "command": criterion.command,
        "where": criterion.where,
        "ci_job": criterion.ci_job,
        "status": result.status if result else None,
        "detail": result.detail if result else "",
    }


def _summarise(plan: criteria_module.Plan, results, as_json: bool) -> int:
    paired = tuple(zip(plan.entries, results))
    failed = [pair for pair in paired if pair[1].status == criteria_module.FAILED]
    skipped = [pair for pair in paired if pair[1].status == criteria_module.NOT_EVALUATED]
    passed = len(paired) - len(failed) - len(skipped)

    if as_json:
        print(json.dumps(_milestone_document(plan, results), indent=2))
        return FAILED_EXIT if failed else OK_EXIT

    identifier = plan.milestone.id.upper()
    if failed:
        print(f"{identifier} is not closed: {len(failed)} of {len(paired) - len(skipped)} "
              f"evaluated criteria failed.")
        for entry, result in failed:
            print(f"  {entry.label:<24} {result.criterion.describe}  "
                  f"[{result.criterion.source}]")
    else:
        print(f"{identifier}: {passed} criteria pass.")
    for entry, result in skipped:
        print(f"  not evaluated here: {entry.label} — {result.detail}")
    for note in plan.milestone.notes:
        print(f"  note: {note}")
    return FAILED_EXIT if failed else OK_EXIT


# --- gates ----------------------------------------------------------------------------------------


def command_gates(arguments: argparse.Namespace) -> int:
    gate_set = gates_module.load()
    if arguments.commands:
        for command in gates_module.commands(gate_set):
            print(command)
        return OK_EXIT
    if arguments.json:
        print(json.dumps(_gates_document(gate_set), indent=2))
        return OK_EXIT

    print(f"Merge gates — {len(gate_set.gates)} declared in tools/roadmap/gates.toml")
    print()
    for gate in gate_set.gates:
        state = "" if gate.klass == "permanent" else f"  [{gate.state}]"
        print(f"  {gate.id:<16} {','.join(gate.platforms):<22} since {gate.since.upper()}{state}")
        print(f"  {'':<16} {gate.describe}")
        for command in gate.runs:
            print(f"  {'':<16}   $ {command}")
        print()
    _print_overrides(gate_set)
    return OK_EXIT


def _print_overrides(gate_set: gates_module.GateSet) -> None:
    if not gate_set.overrides:
        print("No override is recorded. A failing gate is fixed, or an override is written into "
              "gates.toml\nwith its reason, its approver, the change that records it, and an "
              "expiry — never taken quietly.")
        return
    print(f"{len(gate_set.overrides)} recorded override(s):")
    for override in gate_set.overrides:
        print(f"  {override.gate:<16} until {override.expires}  {override.approved_by}  "
              f"({override.change})")
        print(f"  {'':<16} {override.reason}")


def _gates_document(gate_set: gates_module.GateSet) -> dict:
    return {
        "gates": [
            {"id": gate.id, "describe": gate.describe, "runs": list(gate.runs),
             "platforms": list(gate.platforms), "since": gate.since, "class": gate.klass,
             "milestone": gate.milestone, "state": gate.state}
            for gate in gate_set.gates
        ],
        "overrides": [
            {"gate": override.gate, "reason": override.reason,
             "approved_by": override.approved_by, "change": override.change,
             "expires": str(override.expires)}
            for override in gate_set.overrides
        ],
    }


# --- entry point ----------------------------------------------------------------------------------


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="roadmap", description=__doc__.splitlines()[0])
    subcommands = parser.add_subparsers(dest="command", required=True)

    status = subcommands.add_parser("status", help="per-capability implementation status")
    status.add_argument("--all", action="store_true", help="list capabilities that are not started")
    status.add_argument("--json", action="store_true", help="machine-readable output")
    status.add_argument("--record", type=Path, default=record_module.DEFAULT_RECORD,
                        help="the status record to read (default: docs/roadmap/status.yaml)")
    status.add_argument("--specs", type=Path, default=record_module.DEFAULT_SPECS,
                        help="the specification directory to check it against")
    status.set_defaults(handler=command_status)

    milestone = subcommands.add_parser("milestone", help="run a milestone's exit criteria")
    milestone.add_argument("id", help=f"milestone id ({', '.join(criteria_module.available())})")
    milestone.add_argument("--list", action="store_true", help="list the criteria without running")
    milestone.add_argument("--ci", action="store_true",
                           help="also run criteria marked as requiring continuous integration")
    milestone.add_argument("--json", action="store_true", help="machine-readable output")
    milestone.add_argument("--record", type=Path, default=record_module.DEFAULT_RECORD)
    milestone.set_defaults(handler=command_milestone)

    gate_command = subcommands.add_parser("gates", help="the permanent merge-gate set")
    gate_command.add_argument("--commands", action="store_true",
                              help="print only the commands, one per line, for a workflow file")
    gate_command.add_argument("--json", action="store_true", help="machine-readable output")
    gate_command.set_defaults(handler=command_gates)
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        return arguments.handler(arguments)
    except (record_module.RecordError, criteria_module.CriteriaError, gates_module.GateError) as error:
        print(f"roadmap {arguments.command}: {error}", file=sys.stderr)
        return DATA_EXIT


if __name__ == "__main__":
    sys.exit(main())

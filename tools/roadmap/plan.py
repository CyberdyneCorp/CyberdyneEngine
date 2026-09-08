"""The plan, as data: the matrix, the roadmap's work tables, the load summary and the dependencies.

M7 tasks 12.6 and 12.7. `delivery-roadmap` says every forbidden roadmap pattern "SHALL be
checkable", and two of them were not:

  * **A capability scheduled to reach Working before a prerequisite reaches Seed** — and its
    sibling, Complete before a prerequisite reaches Working. M6's plan had two rows in exactly that
    state and both were found at its closing gate BY READING: `core-assets-and-io` and
    `core-memory-and-containers` were planned Complete at M6, `asset-import-pipeline`,
    `serialization-and-prefabs` and `rendering-geometry-and-resources` likewise, and the fix was to
    move each **C** to the milestone at which its unmet prerequisite arrives. A rule that is only
    ever applied by a person reading two documents side by side is a rule that holds until somebody
    is busy.

  * **The four plan documents agreeing.** `docs/ROADMAP.md`'s per-milestone work table, the column
    of `docs/roadmap/capability-matrix.md`, that file's Milestone load table, and the
    `[criterion.expect_tiers]` of `tools/roadmap/milestones/<id>.toml` are four statements of one
    thing. Three of the four disagreed about M5's scope — the load table said fourteen advanced
    where the matrix showed sixteen, and thirty-six completing at M11 where the matrix showed
    thirty-eight — and two disagreed about M6's. The matrix itself records the repair and says
    plainly: "It is still two documents that can drift; what has changed is that they do not
    disagree today." This module is what makes that sentence false in the good direction.

--- WHAT IS PARSED, AND WHY IT IS PARSED RATHER THAN DUPLICATED -----------------------------------

Nothing here is a second copy of the plan. Every fact is read out of the document that owns it:

  `docs/roadmap/capability-matrix.md`   the tier each capability reaches at each milestone, and the
                                       Milestone load table derived from it
  `docs/ROADMAP.md`                    each milestone's **Work** table
  `docs/roadmap/dependencies.md`       the dependency edges, out of the mermaid graphs — which are
                                       already the machine-readable form of that document
  `tools/roadmap/milestones/*.toml`    each ledger's `[criterion.expect_tiers]`

A checker that carried its own table of dependencies would be a fifth document to keep in step, and
the fifth would drift the same way the first four did.

Governed by: delivery-roadmap (Forbidden roadmap patterns, The capability matrix).
"""

from __future__ import annotations

import re
import tomllib
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MATRIX = REPO_ROOT / "docs" / "roadmap" / "capability-matrix.md"
ROADMAP = REPO_ROOT / "docs" / "ROADMAP.md"
DEPENDENCIES = REPO_ROOT / "docs" / "roadmap" / "dependencies.md"
MILESTONES_DIR = Path(__file__).resolve().parent / "milestones"

#: The tiers, weakest first. A milestone cell carrying one of these advances the capability to it.
TIERS = ("seed", "working", "complete")
TIER_OF_SYMBOL = {"S": "seed", "W": "working", "C": "complete"}
#: `◇` is a deferral with its prerequisites verified, not an advance. It is read and ignored.
DEFERRED = "◇"


class PlanError(Exception):
    """A plan document that cannot be read. Distinguished from a plan that disagrees with itself."""


def milestone_id(column: str) -> str:
    """A matrix column heading as the identifier a file name and a record value use.

    `M5.5` is `m5b`, and `M8.a`/`M8.b` are `m8a`/`m8b`: a milestone id is also a file name under
    `tools/roadmap/milestones/`, and a dot in one is a needless special case.

    The mapping is a table rather than a chain of `replace` calls because the chain was one:
    `.replace(".5", "b")` turned `M5.5` into `m5b` correctly and turned `M8.a` into `m8.a`, which
    matched no milestone at all — so the split columns parsed as nothing, every M8 cell vanished
    from the matrix, and the Complete column of nine capabilities pointed at a milestone the matrix
    no longer contained. The checks below caught it, which is what they are for.
    """
    text = column.strip().lower().replace(" ", "")
    return {"m5.5": "m5b", "m8.a": "m8a", "m8.b": "m8b"}.get(text, text)


def tier_rank(tier: str) -> int:
    return TIERS.index(tier)


# --- The matrix -----------------------------------------------------------------------------------


@dataclass(frozen=True)
class Matrix:
    """The capability matrix: what each capability reaches at each milestone."""

    #: In column order, as milestone ids: m0, m1, ..., m5b, m6, ...
    milestones: tuple[str, ...]
    #: capability -> milestone id -> tier
    cells: dict[str, dict[str, str]]
    #: capability -> the milestone its `Complete` column names
    completes_at: dict[str, str]

    def tier_at(self, capability: str, milestone: str) -> str | None:
        """The tier the capability has REACHED by this milestone, cumulatively."""
        best: str | None = None
        for rung in self.milestones:
            tier = self.cells.get(capability, {}).get(rung)
            if tier is not None and (best is None or tier_rank(tier) > tier_rank(best)):
                best = tier
            if rung == milestone:
                return best
        return best

    def reaches(self, capability: str, tier: str) -> str | None:
        """The first milestone at which the capability is at or above `tier`."""
        for rung in self.milestones:
            reached = self.cells.get(capability, {}).get(rung)
            if reached is not None and tier_rank(reached) >= tier_rank(tier):
                return rung
        return None


def _split_row(line: str) -> list[str]:
    stripped = line.strip()
    if not stripped.startswith("|"):
        return []
    return [cell.strip() for cell in stripped.strip("|").split("|")]


_CAPABILITY = re.compile(r"`([a-z0-9-]+)`")


def _cell_tier(cell: str) -> str | None:
    text = cell.replace("*", "").strip()
    if not text or text == DEFERRED:
        return None
    return TIER_OF_SYMBOL.get(text)


def read_matrix(path: Path = MATRIX) -> Matrix:
    """Read the matrix table. The header row decides which column is which milestone."""
    columns: dict[int, str] = {}
    cells: dict[str, dict[str, str]] = {}
    completes: dict[str, str] = {}
    complete_column = -1

    for line in path.read_text(encoding="utf-8").splitlines():
        row = _split_row(line)
        if not row:
            continue
        if not columns:
            if row[0] != "Capability":
                continue
            for index, header in enumerate(row):
                # `M5.5` and `M8.a`/`M8.b` are insertions: a milestone heading is a number with an
                # optional `.5` or `.a`/`.b` suffix. A pattern that admitted only `.5` silently
                # dropped the split columns and took every M8 cell with them.
                if re.fullmatch(r"M\d+(\.5|\.[ab])?", header):
                    columns[index] = milestone_id(header)
                elif header == "Complete":
                    complete_column = index
            if not columns:
                raise PlanError(f"{path.name}: the matrix header names no milestone columns")
            continue
        name = _CAPABILITY.search(row[0])
        if name is None or len(row) <= max(columns):
            continue
        capability = name.group(1)
        cells[capability] = {
            columns[index]: tier
            for index in columns
            if (tier := _cell_tier(row[index])) is not None
        }
        if 0 <= complete_column < len(row):
            completes[capability] = milestone_id(row[complete_column])

    if not cells:
        raise PlanError(f"{path.name}: no capability rows were parsed")
    return Matrix(tuple(columns[index] for index in sorted(columns)), cells, completes)


# --- The Milestone load table ----------------------------------------------------------------------


@dataclass(frozen=True)
class Load:
    advanced: int
    completing: int
    names: tuple[str, ...]


def read_load_summary(path: Path = MATRIX) -> dict[str, Load]:
    """The `## Milestone load` table, as claimed. `Which` is `—` where nothing completes."""
    summary: dict[str, Load] = {}
    inside = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("## "):
            inside = line.startswith("## Milestone load")
            continue
        if not inside:
            continue
        row = _split_row(line)
        if len(row) < 4:
            continue
        # The third place the insertion suffix has to be admitted, and the last: header columns,
        # section headings, and this table's row labels all name a milestone, and all three read it
        # with a pattern of their own. Three patterns for one grammar is why the split had to be
        # made three times before the checks went quiet.
        name = re.match(r"\*\*(M\d+(?:\.5|\.[ab])?)\*\*", row[0])
        if name is None:
            continue
        which = row[3].replace("—", "").strip()
        summary[milestone_id(name.group(1))] = Load(
            advanced=int(row[1]),
            completing=int(row[2]),
            names=tuple(sorted(_CAPABILITY.findall(which))),
        )
    if not summary:
        raise PlanError(f"{path.name}: the Milestone load table was not found")
    return summary


def load_from_matrix(matrix: Matrix) -> dict[str, Load]:
    """The same table, recomputed from the columns it is supposed to be derived from."""
    computed: dict[str, Load] = {}
    for rung in matrix.milestones:
        advancing = [name for name, row in matrix.cells.items() if rung in row]
        completing = [name for name in advancing if matrix.cells[name][rung] == "complete"]
        computed[rung] = Load(len(advancing), len(completing), tuple(sorted(completing)))
    return computed


# --- The roadmap's per-milestone work tables --------------------------------------------------------


@dataclass(frozen=True)
class Section:
    """One milestone's section of `docs/ROADMAP.md`."""

    #: capability -> the tier its **Work** row claims. A `—` row claims no tier and is not here.
    work: dict[str, str]
    #: Every capability the section names OUTSIDE the work table — in the exit criteria, or in the
    #: paragraph a closed milestone writes when the record and the plan diverged.
    named_in_prose: frozenset[str]
    #: The M11 table's `| Everything else | C |` row, which is a claim about every remaining row.
    catch_all: bool


def read_sections(path: Path = ROADMAP) -> dict[str, Section]:
    """Each milestone's section: its **Work** table, and every capability its prose names.

    TWO ROW SHAPES THAT ARE NOT MISTAKES AND MUST NOT BE READ AS ONE.

    A `—` in the arrow column is a row that advances no tier — M3's "camera-relative rendering and
    reversed-Z proven by test", M7's Metal seed, M11's held-open XR prerequisites. It is a piece of
    work rather than a tier claim, so it contributes nothing to `work` and would be a false finding
    if it did.

    A row may name SEVERAL capabilities: `core-math` / `rendering-architecture` is one line of work
    belonging to two rows of the matrix. Every name on the row takes the row's tier.
    """
    sections: dict[str, Section] = {}
    current: str | None = None
    in_table = False
    work: dict[str, str] = {}
    mentioned: set[str] = set()
    catch_all = False

    def close() -> None:
        if current is not None:
            sections[current] = Section(dict(work), frozenset(mentioned), catch_all)

    for line in path.read_text(encoding="utf-8").splitlines():
        # Insertions again: `## M5.5 — Operable`, `## M8.a — Authorable`, `## M8.b — Systems`. A
        # pattern that admits only `.5` reads a split milestone's section as a continuation of the
        # one above it, so its whole work table is attributed to the previous milestone — which is
        # how thirty-six of M8.b's tier claims briefly became M7's.
        heading = re.match(r"^## (M\d+(?:\.5|\.[ab])?) ", line)
        if heading is not None:
            close()
            current = milestone_id(heading.group(1))
            work, mentioned, in_table, catch_all = {}, set(), False, False
            continue
        if current is None:
            continue
        row = _split_row(line)
        if not row:
            mentioned.update(_CAPABILITY.findall(line))
        if not row:
            in_table = in_table and not line.strip().startswith("**")
            continue
        if row[0] == "Capability":
            in_table = True
            continue
        if not in_table or len(row) < 2:
            continue
        if row[0].strip().lower().startswith("everything else"):
            catch_all = True
            continue
        tier = _cell_tier(row[1])
        if tier is not None:
            work.update({name: tier for name in _CAPABILITY.findall(row[0])})
    close()
    if not sections:
        raise PlanError(f"{path.name}: no milestone work tables were found")
    return sections


# --- The dependency graph ---------------------------------------------------------------------------


_NODE = re.compile(r'([A-Za-z0-9_]+)\["([^"]+)"\]')
_EDGE = re.compile(r"([A-Za-z0-9_]+)(?:\[\"[^\"]*\"\])?\s*-->\s*([A-Za-z0-9_]+)")


def _label_capability(label: str, known: set[str]) -> str | None:
    """The capability a mermaid label names, or None for a pseudo-node like `GPU scene · M3`."""
    text = re.split(r"<br/>| · | → ", label)[0].strip().strip("`")
    return text if text in known else None


def read_dependencies(known: set[str], path: Path = DEPENDENCIES) -> set[tuple[str, str]]:
    """The edges of the mermaid graphs, as `(prerequisite, dependent)` capability pairs.

    A node whose label is not a specified capability — `GPU scene · M3`, `Metal · D3D12`, `1.0` —
    is a signpost in the drawing rather than a capability, and its edges are dropped. That is why
    `known` is passed in rather than inferred: an unknown label must be ignorable without being
    silently mistaken for a capability that no longer exists.
    """
    text = path.read_text(encoding="utf-8")
    labels = {node: label for node, label in _NODE.findall(text)}
    edges: set[tuple[str, str]] = set()
    for source, target in _EDGE.findall(text):
        first = _label_capability(labels.get(source, source), known)
        second = _label_capability(labels.get(target, target), known)
        if first is not None and second is not None and first != second:
            edges.add((first, second))
    if not edges:
        raise PlanError(f"{path.name}: no dependency edges were parsed")
    return edges


# --- The ledgers ------------------------------------------------------------------------------------


def read_expected_tiers(directory: Path = MILESTONES_DIR) -> dict[str, dict[str, str]]:
    """Every ledger's `[criterion.expect_tiers]`, by milestone id."""
    expected: dict[str, dict[str, str]] = {}
    for path in sorted(directory.glob("*.toml")):
        with path.open("rb") as handle:
            document = tomllib.load(handle)
        for criterion in document.get("criterion", ()):
            if criterion.get("kind") == "tiers":
                expected.setdefault(path.stem, {}).update(criterion.get("expect_tiers", {}))
    return expected


# --- The two checks ---------------------------------------------------------------------------------


def check_dependency_rules(matrix: Matrix, edges: set[tuple[str, str]]) -> list[str]:
    """Task 12.6. Every forbidden ordering the dependency rule names, as findings.

    "A capability may not reach Working before every capability it depends on has reached Seed, and
    may not reach Complete before its dependencies have reached Working."

    The same milestone is allowed: the rule says *before*, and two capabilities landing together is
    the ordinary case for a seam built in one change.
    """
    order = {rung: index for index, rung in enumerate(matrix.milestones)}
    findings: list[str] = []
    for required, dependent in sorted(edges):
        for tier, prerequisite in (("working", "seed"), ("complete", "working")):
            at = matrix.reaches(dependent, tier)
            needed = matrix.reaches(required, prerequisite)
            if at is None:
                continue
            if needed is None:
                findings.append(
                    f"{dependent} reaches {tier} at {at.upper()}, and {required} — which it "
                    f"depends on — never reaches {prerequisite} on this ladder"
                )
            elif order[needed] > order[at]:
                findings.append(
                    f"{dependent} reaches {tier} at {at.upper()} but {required} does not reach "
                    f"{prerequisite} until {needed.upper()}"
                )
    return findings


def check_documents_agree(
    matrix: Matrix,
    sections: dict[str, Section],
    claimed: dict[str, Load],
    expected: dict[str, dict[str, str]],
) -> list[str]:
    """Task 12.7. The four plan documents, against each other."""
    findings: list[str] = []
    findings += _check_work_against_matrix(matrix, sections)
    findings += _check_load_against_matrix(matrix, claimed)
    findings += _check_ledgers_against_matrix(matrix, expected)
    findings += _check_complete_column(matrix)
    return findings


def _check_work_against_matrix(matrix: Matrix, sections: dict[str, Section]) -> list[str]:
    """Every tier a **Work** row claims is the tier the matrix column carries — or is corrected.

    THE WORK TABLE IS ALLOWED TO GO STALE, AND THE ROADMAP SAYS SO IN SO MANY WORDS. M5's section
    reads: "The table above is the plan M5 was written against. The record of what the milestone
    actually reached is status.yaml, and where the two differ the record wins", and then names the
    four rows that differ. That is the right way to keep a milestone's page readable — the plan it
    was written against is worth preserving — and it is why this check is not a plain equality.

    So a work row may disagree with the matrix column ONLY where the section names that capability
    again outside the table. The correction then has to be written down for the check to pass, which
    is the whole difference between a documented divergence and drift.
    """
    findings: list[str] = []
    for rung, section in sections.items():
        if rung not in matrix.milestones:
            continue
        for capability, tier in sorted(section.work.items()):
            if capability not in matrix.cells:
                findings.append(
                    f"ROADMAP.md's {rung.upper()} work table names `{capability}`, which is not a "
                    "row of the capability matrix"
                )
                continue
            cell = matrix.cells[capability].get(rung)
            if cell != tier and capability not in section.named_in_prose:
                findings.append(
                    f"{rung.upper()} `{capability}`: ROADMAP.md's work table says {tier}, the "
                    f"matrix column says {cell or 'nothing'}, and the {rung.upper()} section says "
                    "nothing about the difference"
                )
    return findings


def _check_load_against_matrix(matrix: Matrix, claimed: dict[str, Load]) -> list[str]:
    computed = load_from_matrix(matrix)
    findings: list[str] = []
    for rung, derived in computed.items():
        stated = claimed.get(rung)
        if stated is None:
            findings.append(f"the Milestone load table has no row for {rung.upper()}")
            continue
        if stated.advanced != derived.advanced:
            findings.append(
                f"{rung.upper()}: the Milestone load table says {stated.advanced} advanced and the "
                f"matrix column has {derived.advanced}"
            )
        # A `Which` cell that lists no capability — M11's "everything remaining" — is a count
        # claim and not a list, so only the count is compared against it.
        if stated.completing != derived.completing or (stated.names
                                                       and stated.names != derived.names):
            findings.append(
                f"{rung.upper()}: the Milestone load table says {stated.completing} complete "
                f"({', '.join(stated.names) or 'none'}) and the matrix column has "
                f"{derived.completing} ({', '.join(derived.names) or 'none'})"
            )
    return findings


def _check_ledgers_against_matrix(matrix: Matrix,
                                  expected: dict[str, dict[str, str]]) -> list[str]:
    """A ledger's expected tier is a FLOOR, so it may not exceed what the plan schedules by then."""
    findings: list[str] = []
    for rung, tiers in expected.items():
        if rung not in matrix.milestones:
            findings.append(f"{rung}.toml declares expected tiers and is not a matrix column")
            continue
        for capability, tier in sorted(tiers.items()):
            planned = matrix.tier_at(capability, rung)
            if planned is None:
                findings.append(
                    f"{rung}.toml expects `{capability}` at {tier} and the matrix plans nothing "
                    f"for it by {rung.upper()}"
                )
            elif tier_rank(tier) > tier_rank(planned):
                findings.append(
                    f"{rung}.toml expects `{capability}` at {tier} and the matrix plans it at "
                    f"{planned} by {rung.upper()}"
                )
    return findings


def _check_complete_column(matrix: Matrix) -> list[str]:
    """The matrix's own `Complete` column against the milestone its row actually completes at."""
    findings: list[str] = []
    for capability, stated in sorted(matrix.completes_at.items()):
        actual = matrix.reaches(capability, "complete")
        if actual is None and stated in ("", "—", "never", "deferred"):
            continue
        if actual != stated:
            findings.append(
                f"`{capability}`: the matrix's Complete column says {stated.upper() or 'nothing'} "
                f"and its row reaches complete at {actual.upper() if actual else 'no milestone'}"
            )
    return findings

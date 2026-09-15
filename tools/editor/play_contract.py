#!/usr/bin/env python3
"""The play-mode and live-edit-policy contracts, derived from the engine and compared.

WHY THIS FILE EXISTS, AND IT IS NOT "ANOTHER LINTER".

`editor-architecture` and `live-editing` sat at **Seed** from M5 to M11 for one reason, written into
`tools/roadmap/milestones/m11b.toml` at the time: two greps returned nothing.

    grep -rniI 'SeparateProcess\\|RemoteDevice\\|InEditor'                    src/ editor/ tools/
    grep -rniI 'LiveEditPolicy\\|ReinitializeComponent\\|RecreateEntity\\|RestartWorld' src/ editor/ tools/

M11.b built both features, and the two criteria that were supposed to establish it were those same
greps INVERTED — "the token is now found somewhere". M11's gate refused them in one sentence:
**word-greps a dummy job satisfies.** It is right, and it is the eighth instance of the defect
`tools/roadmap/falsify.py` enumerates: a check that no state of the repository a person would
plausibly reach can turn red. One file containing the six words, in a comment, closes both greps.

So the greps are replaced by this. It does not ask whether a word appears. It DERIVES each side's
table from the declaration that fixes it, and requires the sides to agree:

    play-modes          the engine      src/gameplay/play/{include/cy/gameplay/play/mode.h,src/mode.cpp}
                        the editor      editor/crates/cy-editor-viewport/src/play.rs
                        the requirement openspec/specs/live-editing/spec.md
                                        openspec/specs/editor-architecture/spec.md

    live-edit-policy    the engine      src/gameplay/live/{include/cy/gameplay/live/policy.h,src/policy.cpp}
                        its input       src/core/reflect/include/cy/core/reflect/attributes.h
                        the requirement openspec/specs/live-editing/spec.md

A word typed into a comment satisfies none of it. Renaming a mode on one side of the boundary and
not the other is red; adding a mode to the enumeration without a capability row is red; deriving
`RecreateEntity` from a field's classification — which `policy.h` says at length must never happen,
and which `m11b:live-edit-applies-without-a-restart` rests on — is red; adding a fifth
`PersistenceKind` with no policy for it is red; a mode declared unavailable without naming the rung
it is due at is red.

--- WHY THE TABLES ARE DERIVED RATHER THAN SAMPLED -------------------------------------------------

The same argument `tools/abi/README.md` makes about struct layouts, and it is why this needs no
build: the three spellings are fixed BY THE DECLARATION. `kNames` is `constexpr`, the Rust `name()`
is a `const fn` over a closed match, and both are exhaustive over an enumeration. Reading the
declarations states that rule. Running a binary would only observe one instance of it — and a
criterion that needs a compiled engine cannot be proven by `just roadmap-falsify`, whose sandbox is
the tracked tree.

The BEHAVIOURAL half is not replaced by this and must not be: `m11b:play-mode-round-trip` drives a
world through all three modes in the compiled engine and `m11b:live-edit-applies-without-a-restart`
edits a field of each policy class in a running world. This gate is what makes those two suites'
subject match the specification's, which is the half a passing suite cannot establish about itself.

--- WHY A PARSER THAT DOES NOT UNDERSTAND SOMETHING IS AN ERROR ------------------------------------

The sixth of the seven unfalsifiable checks this repository has found was a dependency gate that
parsed backticks out of a table written in bold, so it parsed nothing and compared nothing to
nothing. Every reader below raises `ContractError` when it finds nothing, and every comparison is
against a list that is required to be non-empty first. `tools/editor/selftest.py` breaks each input
on purpose and requires this gate to notice.

    python3 tools/editor/play_contract.py play-modes
    python3 tools/editor/play_contract.py live-edit-policy
    python3 tools/editor/play_contract.py --list

Standard library only, like `tools/abi/` and `tools/roadmap/`: this runs on every pull request on
three platforms, and a gate may not depend on a package that happens to be installed.

Governed by: editor-architecture (Play mode), live-editing (Play modes, Live edit policy),
delivery-roadmap (Milestone exit criteria are executable, Forbidden roadmap patterns).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]

MODE_HEADER = "src/gameplay/play/include/cy/gameplay/play/mode.h"
MODE_SOURCE = "src/gameplay/play/src/mode.cpp"
EDITOR_PLAY = "editor/crates/cy-editor-viewport/src/play.rs"
POLICY_HEADER = "src/gameplay/live/include/cy/gameplay/live/policy.h"
POLICY_SOURCE = "src/gameplay/live/src/policy.cpp"
ATTRIBUTES = "src/core/reflect/include/cy/core/reflect/attributes.h"
LIVE_EDITING = "openspec/specs/live-editing/spec.md"
EDITOR_ARCHITECTURE = "openspec/specs/editor-architecture/spec.md"


class ContractError(Exception):
    """A file this gate reads no longer has the shape it reads. Never a pass: see the header."""


# --- Reading ---------------------------------------------------------------------------------------

_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
_LINE_COMMENT = re.compile(r"//[^\n]*")
_RUST_ATTRIBUTE = re.compile(r"^[ \t]*#\[[^\]]*\][ \t]*$\n?", re.M)


def read(relative: str, root: Path = REPOSITORY) -> str:
    path = root / relative
    if not path.is_file():
        raise ContractError(f"{relative}: not in the tree. This gate compares what the engine "
                            "declares; it cannot compare a file that is gone.")
    return path.read_text(encoding="utf-8")


def uncommented(text: str) -> str:
    """Comments removed, so that a word in prose can never be mistaken for a declaration."""
    return _LINE_COMMENT.sub("", _BLOCK_COMMENT.sub("", text))


def braced(text: str, opening: int, where: str) -> str:
    """The balanced `{...}` body beginning at or after `opening`, without its outer braces."""
    start = text.find("{", opening)
    if start < 0:
        raise ContractError(f"{where}: no '{{' after the declaration")
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:index]
    raise ContractError(f"{where}: the block beginning at {start} is never closed")


def enumerators(text: str, name: str, where: str) -> tuple[str, ...]:
    """The enumerators of a C++ `enum class` or a Rust `enum`, in declaration order."""
    match = re.search(rf"\benum\b(?:\s+class)?\s+{re.escape(name)}\b[^{{;]*{{", uncommented(text))
    if match is None:
        raise ContractError(f"{where}: no enumeration named {name}")
    body = _RUST_ATTRIBUTE.sub("", braced(uncommented(text), match.start(), where))
    found: list[str] = []
    for piece in body.split(","):
        piece = piece.strip()
        if not piece:
            continue
        leading = re.match(r"^([A-Za-z_]\w*)\s*(=\s*.+)?$", piece, re.S)
        if leading is None:
            raise ContractError(f"{where}: {name} has an enumerator this gate cannot read: "
                                f"{piece[:60]!r}")
        found.append(leading.group(1))
    if not found:
        raise ContractError(f"{where}: {name} declares no enumerators")
    return tuple(found)


def string_array(text: str, name: str, where: str) -> tuple[str, ...]:
    """The string literals of a C++ array initialiser, in order."""
    match = re.search(rf"\b{re.escape(name)}\s*\[[^\]]*\]\s*=\s*{{", uncommented(text))
    if match is None:
        raise ContractError(f"{where}: no array named {name}")
    body = braced(uncommented(text), match.start(), where)
    values = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
    if not values:
        raise ContractError(f"{where}: {name} holds no string literals")
    return tuple(values)


def constant(text: str, name: str, where: str) -> int:
    match = re.search(rf"\b{re.escape(name)}\s*=\s*(\d+)\s*;", uncommented(text))
    if match is None:
        raise ContractError(f"{where}: no integer constant named {name}")
    return int(match.group(1))


def function_body(text: str, name: str, where: str) -> str:
    """The body of the definition of `name` — the one followed by a block, not a declaration."""
    stripped = uncommented(text)
    for match in re.finditer(rf"\b{re.escape(name)}\s*\(", stripped):
        tail = stripped[match.end():]
        closing = tail.find(")")
        if closing < 0:
            continue
        after = tail[closing + 1:closing + 80]
        if re.match(r"^[^;{]*{", after):
            return braced(stripped, match.end() + closing, f"{where}: {name}")
    raise ContractError(f"{where}: no definition of {name}()")


def rust_impl(text: str, type_name: str, where: str) -> str:
    match = re.search(rf"\bimpl\s+{re.escape(type_name)}\s*{{", uncommented(text))
    if match is None:
        raise ContractError(f"{where}: no `impl {type_name}` block")
    return braced(uncommented(text), match.start(), f"{where}: impl {type_name}")


def switch_arms(body: str, enum_name: str, where: str) -> dict[str, str]:
    """Each `case Enum::X:` of a C++ switch, and the text from it to the next case."""
    marks = list(re.finditer(rf"case\s+(?:\w+::)*{re.escape(enum_name)}::(\w+)\s*:", body))
    if not marks:
        raise ContractError(f"{where}: no `case {enum_name}::...` arms")
    arms: dict[str, str] = {}
    for index, mark in enumerate(marks):
        end = marks[index + 1].start() if index + 1 < len(marks) else len(body)
        arms[mark.group(1)] = body[mark.end():end]
    return arms


def assignments(text: str, receiver: str) -> dict[str, str]:
    """`receiver.field = value;` — the LAST value assigned to each field, which is the one in force."""
    found: dict[str, str] = {}
    for match in re.finditer(rf"\b{re.escape(receiver)}\.(\w+)\s*=\s*([^;]+);", text):
        found[match.group(1)] = match.group(2).strip()
    return found


def matches_predicate(body: str, enum_name: str, every: tuple[str, ...], where: str) -> frozenset:
    """The variants a Rust `matches!(self, ...)` predicate answers TRUE for."""
    match = re.search(r"(!?)\s*matches!\s*\(\s*self\s*,([^)]*)\)", body)
    if match is None:
        raise ContractError(f"{where}: no `matches!(self, ...)` to read the predicate from")
    listed = set(re.findall(rf"{re.escape(enum_name)}::(\w+)", match.group(2)))
    if not listed:
        raise ContractError(f"{where}: the predicate names no {enum_name} variant")
    return frozenset(set(every) - listed) if match.group(1) == "!" else frozenset(listed)


def requirement_section(text: str, requirement: str, where: str) -> str:
    """The body of `### Requirement: <name>`, up to the next heading of the same level or above."""
    match = re.search(rf"^###\s+Requirement:\s*{re.escape(requirement)}\s*$", text, re.M)
    if match is None:
        raise ContractError(f"{where}: no `### Requirement: {requirement}`")
    tail = text[match.end():]
    end = re.search(r"^(?:#{1,3})\s", tail, re.M)
    return tail[:end.start()] if end else tail


def table_keys(section: str, where: str) -> tuple[str, ...]:
    """The backticked first column of a markdown table, in order.

    The guard is the point: the sixth unfalsifiable check this repository found parsed backticks out
    of a table whose cells were bold, matched nothing, and compared nothing to nothing. Fewer than
    two rows is an ERROR here rather than an empty comparison.
    """
    keys = []
    for line in section.splitlines():
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) < 2 or not line.strip().startswith("|"):
            continue
        backticked = re.fullmatch(r"`([A-Za-z_]\w*)`", cells[0])
        if backticked:
            keys.append(backticked.group(1))
    if len(keys) < 2:
        raise ContractError(f"{where}: the table has {len(keys)} row(s) whose first cell is a "
                            "backticked identifier. It is not the table this gate reads.")
    return tuple(keys)


def kebab(identifier: str) -> str:
    """`ReinitializeComponent` -> `reinitialize-component`. The rule both name tables follow."""
    return re.sub(r"(?<!^)(?=[A-Z])", "-", identifier).lower()


# --- Reporting -------------------------------------------------------------------------------------


class Report:
    """What one contract came to. Every leg prints; the shortfalls decide the exit status."""

    def __init__(self, contract: str, subject: str) -> None:
        self.contract = contract
        self.subject = subject
        self.short: list[str] = []
        print(f"{contract}: {subject}")

    def leg(self, holds: bool, claim: str, detail: str = "") -> bool:
        print(f"  {'ok  ' if holds else 'RED '}  {claim}")
        for line in (detail or "").splitlines():
            print(f"          {line}")
        if not holds:
            self.short.append(claim)
        return holds

    def same(self, claim: str, left_name: str, left, right_name: str, right) -> bool:
        holds = list(left) == list(right)
        detail = "" if holds else f"{left_name}: {list(left)}\n{right_name}: {list(right)}"
        return self.leg(holds, claim, detail)

    def finish(self) -> int:
        print()
        if self.short:
            print(f"{self.contract}: {len(self.short)} leg(s) of the contract DO NOT HOLD:")
            for claim in self.short:
                print(f"    {claim}")
            return 1
        print(f"{self.contract}: every leg holds.")
        return 0


# --- play-modes ------------------------------------------------------------------------------------


def check_play_modes(root: Path) -> int:
    report = Report("play-modes",
                    "the three play modes, as the engine declares them, as the editor declares "
                    "them, and as the requirement names them")
    engine = _engine_modes(root)
    editor = _editor_modes(root, engine)
    _compare_mode_tables(report, engine, editor)
    _check_mode_requirement(report, root, engine)
    _check_no_fallback(report, root)
    return report.finish()


def _engine_modes(root: Path) -> dict:
    header = read(MODE_HEADER, root)
    source = read(MODE_SOURCE, root)
    variants = enumerators(header, "PlayMode", MODE_HEADER)
    capabilities = _capability_table(source, variants)
    return {
        "variants": variants,
        "declared_count": constant(header, "kPlayModeCount", MODE_HEADER),
        "names": string_array(source, "kNames", MODE_SOURCE),
        "step_frame": frozenset(name for name in variants
                                if capabilities[name].get("step_frame") == "true"),
        "isolated": frozenset(name for name in variants
                              if capabilities[name].get("isolated_state") == "true"),
        "availability": switch_arms(function_body(source, "availability_of", MODE_SOURCE),
                                    "PlayMode", f"{MODE_SOURCE}: availability_of"),
    }


def _capability_table(source: str, variants: tuple[str, ...]) -> dict[str, dict[str, str]]:
    """Each mode's capability row: the defaults set before the switch, overridden by its own arm."""
    body = function_body(source, "capabilities_of", MODE_SOURCE)
    first_case = body.find("case ")
    if first_case < 0:
        raise ContractError(f"{MODE_SOURCE}: capabilities_of has no switch arms")
    defaults = assignments(body[:first_case], "capabilities")
    arms = switch_arms(body, "PlayMode", f"{MODE_SOURCE}: capabilities_of")
    missing = [name for name in variants if name not in arms]
    if missing:
        raise ContractError(f"{MODE_SOURCE}: capabilities_of has no arm for {', '.join(missing)}")
    return {name: {**defaults, **assignments(arms[name], "capabilities")} for name in variants}


def _editor_modes(root: Path, engine: dict) -> dict:
    text = read(EDITOR_PLAY, root)
    variants = enumerators(text, "PlayMode", EDITOR_PLAY)
    block = rust_impl(text, "PlayMode", EDITOR_PLAY)
    arms = dict(re.findall(r'PlayMode::(\w+)\s*=>\s*"([^"]*)"',
                           function_body(block, "name", f"{EDITOR_PLAY}: impl PlayMode")))
    listed = re.findall(r"PlayMode::(\w+)", _all_initialiser(block))
    del engine
    return {
        "variants": variants,
        "names": tuple(arms.get(name, "<no arm>") for name in variants),
        "all": tuple(listed),
        "step_frame": matches_predicate(
            function_body(block, "can_step_frame", f"{EDITOR_PLAY}: can_step_frame"),
            "PlayMode", variants, f"{EDITOR_PLAY}: can_step_frame"),
        "isolated": matches_predicate(
            function_body(block, "isolates_editor_state", f"{EDITOR_PLAY}: isolates_editor_state"),
            "PlayMode", variants, f"{EDITOR_PLAY}: isolates_editor_state"),
    }


def _all_initialiser(block: str) -> str:
    match = re.search(r"\bALL\s*:\s*\[\s*PlayMode\s*;\s*\d+\s*\]\s*=\s*\[", block)
    if match is None:
        raise ContractError(f"{EDITOR_PLAY}: `PlayMode::ALL` is not declared as "
                            "`[PlayMode; N] = [ ... ]`")
    end = block.find("]", match.end())
    if end < 0:
        raise ContractError(f"{EDITOR_PLAY}: `PlayMode::ALL` is never closed")
    return block[match.end():end]


def _compare_mode_tables(report: Report, engine: dict, editor: dict) -> None:
    report.same("the engine and the editor enumerate the same modes, in the same order",
                MODE_HEADER, engine["variants"], EDITOR_PLAY, editor["variants"])
    report.leg(engine["declared_count"] == len(engine["variants"]),
               f"kPlayModeCount is the number of enumerators ({engine['declared_count']} declared, "
               f"{len(engine['variants'])} enumerated)")
    report.same("the engine and the editor spell the modes the same way, in the same order",
                MODE_SOURCE, engine["names"], EDITOR_PLAY, editor["names"])
    report.same("each spelling is its enumerator in kebab case",
                "derived", tuple(kebab(name) for name in engine["variants"]),
                MODE_SOURCE, engine["names"])
    report.same("PlayMode::ALL lists every mode, in the same order",
                EDITOR_PLAY, editor["all"], EDITOR_PLAY, editor["variants"])
    report.same("the two sides agree on which modes can step a frame",
                f"{MODE_SOURCE} capabilities_of", sorted(engine["step_frame"]),
                f"{EDITOR_PLAY} can_step_frame", sorted(editor["step_frame"]))
    report.same("the two sides agree on which modes isolate editor state",
                f"{MODE_SOURCE} capabilities_of", sorted(engine["isolated"]),
                f"{EDITOR_PLAY} isolates_editor_state", sorted(editor["isolated"]))
    _check_due(report, engine)


def _check_due(report: Report, engine: dict) -> None:
    """`live-editing`: a mode declared absent SHALL name the rung at which it is due."""
    silent: list[str] = []
    for mode, arm in engine["availability"].items():
        for path in arm.split("return availability;")[:-1]:
            if "available = true" in path.replace(" ", " "):
                continue
            if "availability.due" not in path:
                silent.append(mode)
    report.leg(not silent,
               "every refusal in availability_of names the rung the mode is due at",
               "" if not silent else f"modes with a refusal that names no rung: {sorted(set(silent))}")
    report.same("availability_of has an arm for every mode",
                MODE_HEADER, engine["variants"], f"{MODE_SOURCE} availability_of",
                sorted(engine["availability"], key=engine["variants"].index))


def _check_mode_requirement(report: Report, root: Path, engine: dict) -> None:
    section = requirement_section(read(LIVE_EDITING, root), "Play modes", LIVE_EDITING)
    report.same("`live-editing` (Play modes) names exactly the modes the engine declares",
                LIVE_EDITING, table_keys(section, f"{LIVE_EDITING}: Play modes"),
                MODE_HEADER, engine["variants"])
    prose = requirement_section(read(EDITOR_ARCHITECTURE, root), "Play mode", EDITOR_ARCHITECTURE)
    flattened = re.sub(r"[*_`\s]+", " ", prose).lower()
    absent = [spelling for spelling in engine["names"]
              if spelling not in flattened and spelling.replace("-", " ") not in flattened]
    report.leg(not absent,
               "`editor-architecture` (Play mode) names every mode the engine spells",
               "" if not absent else f"spellings the requirement does not name: {absent}")


def _check_no_fallback(report: Report, root: Path) -> None:
    """`live-editing`: selecting a mode that is not available SHALL NOT fall back to another mode."""
    engine = function_body(read(MODE_SOURCE, root), "play_mode_of", MODE_SOURCE)
    report.leg("fail(" in engine and "return static_cast<PlayMode>" in engine,
               "play_mode_of answers an unknown word with a failure rather than a nearest mode",
               "" if "fail(" in engine else "no `fail(` on any path out of play_mode_of")
    editor = read(EDITOR_PLAY, root)
    signature = re.search(r"fn\s+from_name\s*\([^)]*\)\s*->\s*Option<PlayMode>", uncommented(editor))
    report.leg(signature is not None,
               "PlayMode::from_name returns Option<PlayMode> rather than a mode",
               "" if signature else f"{EDITOR_PLAY}: from_name does not return Option<PlayMode>")


# --- live-edit-policy --------------------------------------------------------------------------------


def check_live_edit_policy(root: Path) -> int:
    report = Report("live-edit-policy",
                    "the six policies, the order of what they disturb, the classifications they "
                    "are derived from, and the two that may never be derived")
    header = read(POLICY_HEADER, root)
    source = read(POLICY_SOURCE, root)
    variants = enumerators(header, "LiveEditPolicy", POLICY_HEADER)

    report.leg(constant(header, "kLiveEditPolicyCount", POLICY_HEADER) == len(variants),
               f"kLiveEditPolicyCount is the number of enumerators ({len(variants)} enumerated)")
    report.same("each spelling is its enumerator in kebab case",
                "derived", tuple(kebab(name) for name in variants),
                POLICY_SOURCE, string_array(source, "kNames", POLICY_SOURCE))
    section = requirement_section(read(LIVE_EDITING, root), "Live edit policy", LIVE_EDITING)
    report.same("`live-editing` (Live edit policy) names exactly the policies the engine declares",
                LIVE_EDITING, table_keys(section, f"{LIVE_EDITING}: Live edit policy"),
                POLICY_HEADER, variants)
    _check_disturbance(report, source, variants)
    _check_derivation(report, root, source, variants)
    return report.finish()


def _check_disturbance(report: Report, source: str, variants: tuple[str, ...]) -> None:
    """A transaction is applied at the STRONGEST policy any of its fields declares, so the scale has
    to be a total order over all six — two policies sharing a rank would make that choice a coin."""
    arms = switch_arms(function_body(source, "live_edit_disturbance", POLICY_SOURCE),
                       "LiveEditPolicy", f"{POLICY_SOURCE}: live_edit_disturbance")
    missing = [name for name in variants if name not in arms]
    if not report.leg(not missing, "live_edit_disturbance ranks every policy",
                      "" if not missing else f"policies with no arm: {missing}"):
        return
    ranks: dict[str, int] = {}
    for name in variants:
        returned = re.search(r"return\s+(\d+)\s*;", arms[name])
        if returned is None:
            report.leg(False, f"live_edit_disturbance returns a rank for {name}")
            return
        ranks[name] = int(returned.group(1))
    report.leg(len(set(ranks.values())) == len(variants),
               "the six ranks are distinct, so 'the stronger of two' is never a coin toss",
               f"ranks: {ranks}" if len(set(ranks.values())) != len(variants) else "")
    order = [name for name, _rank in sorted(ranks.items(), key=lambda item: item[1])]
    report.leg(order[0] == "Immediate" and order[-1] == "Unsupported",
               "Immediate disturbs least and Unsupported most", f"by rank: {order}")
    escalating = ["ReloadAsset", "ReinitializeComponent", "RecreateEntity", "RestartWorld"]
    present = [name for name in escalating if name in ranks]
    report.leg(present == escalating and [ranks[name] for name in present]
               == sorted(ranks[name] for name in present),
               "a rebind disturbs less than a component rebuild, which disturbs less than an entity "
               "recreate, which disturbs less than a restart", f"by rank: {order}")


def _check_derivation(report: Report, root: Path, source: str, variants: tuple[str, ...]) -> None:
    """`policy.h`: only Immediate, ReloadAsset, ReinitializeComponent and Unsupported are ever
    DERIVED. `RecreateEntity` and `RestartWorld` have to be declared per field, and
    `m11b:live-edit-applies-without-a-restart` rests on exactly that distinction."""
    body = function_body(source, "derived_policy_for", POLICY_SOURCE)
    classifications = enumerators(read(ATTRIBUTES, root), "PersistenceKind", ATTRIBUTES)
    arms = switch_arms(body, "PersistenceKind", f"{POLICY_SOURCE}: derived_policy_for")
    report.same("every field classification the engine has is derived from",
                ATTRIBUTES, classifications,
                f"{POLICY_SOURCE} derived_policy_for", sorted(arms, key=classifications.index))
    empty = [name for name, arm in arms.items() if "LiveEditPolicy::" not in arm]
    report.leg(not empty, "every classification arm reaches a policy",
               "" if not empty else f"arms that name no policy: {empty}")
    derived = set(re.findall(r"LiveEditPolicy::(\w+)", body))
    unknown = sorted(derived - set(variants))
    report.leg(not unknown, "every policy derived_policy_for names is one of the six",
               "" if not unknown else f"not in LiveEditPolicy: {unknown}")
    declared_only = sorted({"RecreateEntity", "RestartWorld"} & derived)
    report.leg(not declared_only,
               "RecreateEntity and RestartWorld are never DERIVED — they are declared per field",
               "" if not declared_only else
               f"derived_policy_for can return {declared_only}, which makes the per-field "
               "declaration unnecessary and `live-edit-applies-without-a-restart` vacuous")
    table = read(POLICY_HEADER, root)
    report.leg("LiveEditPolicyTable" in table and "declare(" in table,
               "a per-field declaration table exists to override the derived default")


# --- The command line --------------------------------------------------------------------------------


CONTRACTS = {
    "play-modes": check_play_modes,
    "live-edit-policy": check_live_edit_policy,
}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="play_contract", description=__doc__.splitlines()[0])
    parser.add_argument("contract", nargs="?", choices=sorted(CONTRACTS),
                        help="which contract to check")
    parser.add_argument("--list", action="store_true", help="list the contracts and exit")
    parser.add_argument("--root", default=str(REPOSITORY),
                        help="the tree to read (default: this repository)")
    arguments = parser.parse_args(argv)
    if arguments.list:
        for name in sorted(CONTRACTS):
            print(name)
        return 0
    if not arguments.contract:
        parser.error("name a contract, or pass --list")
    try:
        return CONTRACTS[arguments.contract](Path(arguments.root))
    except ContractError as error:
        print(f"play_contract: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

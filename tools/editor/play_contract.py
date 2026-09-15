#!/usr/bin/env python3
"""The play-mode, live-edit-policy and specialised-editor contracts, derived and compared.

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

    specialised-editors the editor      editor/crates/cy-editor-interface/src/specialised/{mod,timeline}.rs
                        the workspace   editor/crates/cy-editor-visual/src/chrome.rs
                        the engine      src/graph/src/lower_{script,behaviour,pose}.cpp, locomotion.cpp
                                        src/sequencing/{include/cy/sequencing/source.h,src/source.cpp}
                        the requirement openspec/specs/editor-architecture/spec.md (Specialised editors)

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
    python3 tools/editor/play_contract.py specialised-editors
    python3 tools/editor/play_contract.py --list

Standard library only, like `tools/abi/` and `tools/roadmap/`: this runs on every pull request on
three platforms, and a gate may not depend on a package that happens to be installed.

THE THIRD CONTRACT REPLACES A CRITERION THAT CALLED ITSELF A PLACEHOLDER. `m11b:specialised-editors`
was `grep -rniIl CentreLower editor/crates/` with a count of three, and its own body said *"three
files naming a region is satisfied by three comments ... whoever writes section 3.3 owns the
replacement"*. The requirement it stands for forbids two things by name — a sixth bespoke graph
editor, and a second curve surface — so those prohibitions are what the contract reads, off the
requirement's own description of each editor rather than off a table kept here.

Governed by: editor-architecture (Play mode, Specialised editors), live-editing (Play modes, Live
edit policy), delivery-roadmap (Milestone exit criteria are executable, Forbidden roadmap patterns).
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
SPECIALISED = "editor/crates/cy-editor-interface/src/specialised/mod.rs"
SPECIALISED_TIMELINE = "editor/crates/cy-editor-interface/src/specialised/timeline.rs"
CHROME = "editor/crates/cy-editor-visual/src/chrome.rs"
SEQUENCING_HEADER = "src/sequencing/include/cy/sequencing/source.h"
SEQUENCING_SOURCE = "src/sequencing/src/source.cpp"

#: Which engine source declares each domain's authoring vocabulary, and by which name prefixes. The
#: editor's palette is required to offer exactly what these register: a node type the engine gains
#: and the palette does not is a node an author cannot place, and one the palette gains and the
#: engine does not is a node that fails at cook time.
VOCABULARIES = {
    "GameplayAndUtilityGraphs": ("ai", "script"),
    "AbilitiesAndEffects": ("ability", "script"),
    "AnimationGraphsAndClips": ("pose",),
}

#: Where each prefix's node types are registered. Read as string literals rather than as calls,
#: because `lower_script.cpp` registers some of its types through a table and some through a
#: function, and a reader that understood only one of those would compare half a vocabulary.
LOWERINGS = {
    "script": ("src/graph/src/lower_script.cpp",),
    "ability": ("src/graph/src/lower_script.cpp",),
    "ai": ("src/graph/src/lower_behaviour.cpp",),
    "pose": ("src/graph/src/lower_pose.cpp", "src/graph/src/locomotion.cpp"),
}


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


def rust_string_arms(body: str, enum_name: str, where: str) -> dict[str, str]:
    """`Enum::Variant => "text",` — every arm of a Rust match that answers with a literal."""
    arms = dict(re.findall(rf"{re.escape(enum_name)}::(\w+)\s*=>\s*\"((?:[^\"\\]|\\.)*)\"",
                           body))
    if not arms:
        raise ContractError(f"{where}: no `{enum_name}::... => \"...\"` arms to read")
    return arms


def rust_grouped_arms(body: str, enum_name: str, answer: str, where: str) -> dict[str, tuple]:
    """A Rust match whose arms may list several variants and answer with a set of names.

        Domain::Materials | Domain::VfxGraph => &[Surface::Graph],
        Domain::Terrain | Domain::Foliage => {
            &[Surface::Painting]
        }

    Reads to `{"Materials": ("Graph",), ...}`. The alternation is the point: an arm that collects
    four domains is still four claims, and a gate that read only the first would stop noticing
    three of them. Both arm bodies above are read, because rustfmt chooses between them on line
    length and a gate whose result depended on that would be a gate that depended on formatting.
    """
    pattern = (rf"({re.escape(enum_name)}::\w+(?:\s*\|\s*{re.escape(enum_name)}::\w+)*)"
               r"\s*=>\s*(\{[^{}]*\}|&\[[^\]]*\]|[^,\n]+)")
    found: dict[str, tuple] = {}
    for arm in re.finditer(pattern, body, re.S):
        answers = tuple(re.findall(rf"{re.escape(answer)}::(\w+)", arm.group(2)))
        for variant in re.findall(rf"{re.escape(enum_name)}::(\w+)", arm.group(1)):
            found[variant] = answers
    if not found:
        raise ContractError(f"{where}: no `{enum_name}::...` arms to read")
    return found


def rust_named_arms(body: str, enum_name: str, where: str) -> dict[str, str]:
    """A Rust match whose arms answer with an identifier — `Domain::X => SOME_CONST,`."""
    pattern = (rf"({re.escape(enum_name)}::\w+(?:\s*\|\s*{re.escape(enum_name)}::\w+)*)"
               r"\s*=>\s*([A-Z_][A-Z0-9_]*)\s*,")
    found: dict[str, str] = {}
    for arm in re.finditer(pattern, body, re.S):
        for variant in re.findall(rf"{re.escape(enum_name)}::(\w+)", arm.group(1)):
            found[variant] = arm.group(2)
    if not found:
        raise ContractError(f"{where}: no `{enum_name}::... => IDENTIFIER` arms to read")
    return found


def rust_const_strings(text: str, name: str, where: str) -> tuple[str, ...]:
    """The string literals of a Rust `const NAME: &[&str] = &[...];`."""
    match = re.search(rf"\bconst\s+{re.escape(name)}\s*:[^=]*=\s*&\[", uncommented(text))
    if match is None:
        raise ContractError(f"{where}: no `const {name}` to read the palette from")
    start = uncommented(text).index("[", match.end() - 1)
    depth = 0
    body = ""
    stripped = uncommented(text)
    for index in range(start, len(stripped)):
        if stripped[index] == "[":
            depth += 1
        elif stripped[index] == "]":
            depth -= 1
            if depth == 0:
                body = stripped[start + 1:index]
                break
    values = tuple(re.findall(r'"((?:[^"\\]|\\.)*)"', body))
    if not values:
        raise ContractError(f"{where}: {name} holds no node type names")
    return values


def engine_node_types(root: Path, prefix: str) -> tuple[str, ...]:
    """Every `prefix.node` name a lowering registers, read out of the engine's own sources.

    String literals rather than calls, and comments stripped first, so that a node type named in a
    comment cannot enter the engine's half of the comparison. An empty result is an ERROR: the
    seventh unfalsifiable check this project found compared nothing to nothing.
    """
    found: set[str] = set()
    for relative in LOWERINGS[prefix]:
        found.update(re.findall(rf'"({re.escape(prefix)}\.\w+)"', uncommented(read(relative, root))))
    if not found:
        raise ContractError(f"{', '.join(LOWERINGS[prefix])}: no `{prefix}.*` node type literals. "
                            "These are not the files this gate reads.")
    return tuple(sorted(found))


def specification_editors(root: Path) -> tuple[tuple[str, str], ...]:
    """The specialised editors the requirement enumerates: `(term, the segment it was written as)`.

    The requirement's first paragraph is a prose list — *"dedicated editors for: materials
    (including the node graph), animation graphs and clips (a timeline with curve editing), ..."* —
    and it is the authority on which editors exist. The segment is kept beside the term because the
    parenthetical is what says which shared surface an editor is built on.
    """
    section = requirement_section(read(EDITOR_ARCHITECTURE, root), "Specialised editors",
                                  EDITOR_ARCHITECTURE)
    match = re.search(r"dedicated editors for:(.+?)\.\s*\n\s*\n", section, re.S)
    if match is None:
        raise ContractError(f"{EDITOR_ARCHITECTURE}: the Specialised editors requirement no longer "
                            "opens with a `dedicated editors for: ...` list. It is not the "
                            "paragraph this gate reads.")
    listed = []
    for segment in match.group(1).replace("\n", " ").split(","):
        raw = segment.strip()
        term = re.sub(r"\([^)]*\)", " ", raw)
        term = term.replace("**", " ").replace("`", " ")
        term = re.sub(r"^\s*and\s+", "", term.strip(), flags=re.I)
        term = re.sub(r"^\s*the\s+", "", term.strip(), flags=re.I)
        term = re.sub(r"\s+", " ", term).strip().lower()
        if term:
            listed.append((term, raw.lower()))
    if len(listed) < 10:
        raise ContractError(f"{EDITOR_ARCHITECTURE}: the editors list parsed to {len(listed)} "
                            "entries. It is not the list this gate reads.")
    return tuple(listed)


# --- specialised-editors ---------------------------------------------------------------------------


def check_specialised_editors(root: Path) -> int:
    report = Report("specialised-editors",
                    "the editors the requirement enumerates, the ONE canvas and the ONE timeline "
                    "surface they are built on, and the vocabularies the engine can actually lower")
    source = read(SPECIALISED, root)
    impl = rust_impl(source, "Domain", SPECIALISED)
    listed = specification_editors(root)
    terms = rust_string_arms(function_body(impl, "spec_term", SPECIALISED), "Domain", SPECIALISED)
    order = enumerators(source, "Domain", SPECIALISED)

    report.same("the editor registers exactly the editors the requirement enumerates, in its order",
                EDITOR_ARCHITECTURE, [term for term, _raw in listed],
                f"{SPECIALISED} Domain::spec_term", [terms.get(name, "?") for name in order])
    _check_shared_surfaces(report, root, impl, listed, terms, order)
    _check_owning_rows(report, root, impl, order)
    _check_palettes(report, root, source, impl)
    _check_track_kinds(report, root)
    _check_region(report, root, source)
    return report.finish()


def _check_shared_surfaces(report: Report, root: Path, impl: str, listed, terms, order) -> None:
    """The two prohibitions, read off the requirement's own words rather than off a table here.

    An editor the requirement describes as a graph must be built on `Surface::Graph`, and one it
    describes with a timeline, a curve or a sequence must be built on `Surface::Timeline`. The
    direction matters: the requirement compels the code, and an editor that ALSO uses a shared
    surface the requirement did not ask for is not a defect.
    """
    surfaces = rust_grouped_arms(function_body(impl, "surfaces", SPECIALISED), "Domain", "Surface",
                                 SPECIALISED)
    by_term = {terms[name]: surfaces.get(name, ()) for name in order if name in terms}
    for keyword, surface, what in (("graph", "Graph", "a node-graph canvas"),
                                   ("timeline|curve|sequence|cinematic", "Timeline",
                                    "a curve editing surface")):
        wanted = [term for term, raw in listed if re.search(keyword, raw)]
        report.leg(bool(wanted), f"the requirement names at least one editor built on {what}")
        missing = [term for term in wanted if surface not in by_term.get(term, ())]
        report.leg(not missing,
                   f"every editor the requirement describes with {what} is built on the ONE shared "
                   f"Surface::{surface}",
                   "" if not missing else
                   f"declares no Surface::{surface}: {missing}\n"
                   "which is the bespoke editor the requirement forbids by name")
    bespoke = sorted(name for name, answers in surfaces.items() if not answers)
    report.leg(not bespoke, "every editor is built on a shared surface",
               "" if not bespoke else f"built on nothing shared: {bespoke}")


def _check_owning_rows(report: Report, root: Path, impl: str, order) -> None:
    """A refusal names the row that owes the vocabulary, and that row has to exist.

    `open()` refuses thirteen of the sixteen by naming a capability row. A refusal that named a row
    no specification declares would send a reader to a directory that is not there, which is the
    same defect as a diagnostic that names a graph and not a pin.
    """
    rows = rust_string_arms(function_body(impl, "owning_row", SPECIALISED), "Domain", SPECIALISED)
    report.leg(sorted(rows) == sorted(order),
               "every registered editor names the capability row that owes its subject",
               "" if sorted(rows) == sorted(order) else
               f"no row: {sorted(set(order) - set(rows))}")
    absent = sorted(row for row in set(rows.values())
                    if not (root / "openspec" / "specs" / row / "spec.md").is_file())
    report.leg(not absent, "every row a refusal names is a specification in this tree",
               "" if not absent else f"named and absent: {absent}")


def _check_palettes(report: Report, root: Path, source: str, impl: str) -> None:
    """The palette offers exactly what the engine can lower, per domain.

    This is the leg a comment cannot satisfy and a rename cannot survive: both sides are lists of
    node type names, one read out of `Domain::node_types`' constants and one out of the engine's
    lowerings, and they are required to be equal.
    """
    constants = rust_named_arms(function_body(impl, "node_types", SPECIALISED), "Domain",
                                SPECIALISED)
    report.leg(sorted(constants) == sorted(VOCABULARIES),
               "the editor declares a palette for exactly the domains this tree has a vocabulary for",
               "" if sorted(constants) == sorted(VOCABULARIES) else
               f"{SPECIALISED}: {sorted(constants)}\nthis gate: {sorted(VOCABULARIES)}")
    for domain, prefixes in sorted(VOCABULARIES.items()):
        if domain not in constants:
            continue
        engine: list[str] = []
        for prefix in prefixes:
            engine.extend(engine_node_types(root, prefix))
        report.same(f"{domain}'s palette is the engine's own {'/'.join(prefixes)} vocabulary",
                    "+".join(sum((LOWERINGS[prefix] for prefix in prefixes), ())), sorted(engine),
                    f"{SPECIALISED} {constants[domain]}",
                    sorted(rust_const_strings(source, constants[domain], SPECIALISED)))


def _check_track_kinds(report: Report, root: Path) -> None:
    """The timeline offers exactly the track kinds the engine can dispatch.

    `cy::sequencing::TrackKind` is what the sequence compiler switches on. A timeline that offered a
    kind the compiler has no arm for would let an author build a sequence that fails at cook time,
    and one that hid a kind the engine has would make a track unreachable from the editor.
    """
    engine = [name for name in enumerators(read(SEQUENCING_HEADER, root), "TrackKind",
                                           SEQUENCING_HEADER) if name != "Count"]
    editor_source = read(SPECIALISED_TIMELINE, root)
    editor = list(enumerators(editor_source, "TrackKind", SPECIALISED_TIMELINE))
    report.same("the editor's timeline declares the engine's track kinds, in the engine's order",
                SEQUENCING_HEADER, engine, SPECIALISED_TIMELINE, editor)

    engine_names = switch_arms(function_body(read(SEQUENCING_SOURCE, root), "track_kind_name",
                                             SEQUENCING_SOURCE),
                               "TrackKind", f"{SEQUENCING_SOURCE}: track_kind_name")
    spelled = {}
    for name, arm in engine_names.items():
        returned = re.search(r'return\s+"([^"]*)"', arm)
        if returned:
            spelled[name] = returned.group(1)
    editor_names = rust_string_arms(
        function_body(rust_impl(editor_source, "TrackKind", SPECIALISED_TIMELINE), "name",
                      SPECIALISED_TIMELINE),
        "TrackKind", SPECIALISED_TIMELINE)
    report.same("both sides spell every track kind the same way",
                SEQUENCING_SOURCE, [spelled.get(name, "?") for name in engine],
                SPECIALISED_TIMELINE, [editor_names.get(name, "?") for name in engine])


def _check_region(report: Report, root: Path, source: str) -> None:
    """The active editor is drawn in the region `chrome.rs` reserved for it, and nowhere else.

    `Region::CentreLower` was reserved in M5.5 and read by ONE file — the one reserving it — until
    this module existed. `m11b:specialised-editors` used to be `grep -l CentreLower` with a count of
    three, which three comments satisfy. This is the same claim, checked where it is decided.
    """
    chrome = read(CHROME, root)
    reserved = rust_string_arms(function_body(rust_impl(chrome, "Region", CHROME), "contents",
                                              CHROME), "Region", CHROME)
    held = reserved.get("CentreLower", "")
    report.leg("specialised editor" in held,
               "chrome.rs still reserves a region for the active specialised editor",
               "" if "specialised editor" in held else
               f"Region::CentreLower now holds \"{held}\"; the regions are {sorted(reserved)}")
    bound = re.search(r"const\s+REGION\s*:\s*Region\s*=\s*Region::(\w+)\s*;", uncommented(source))
    report.leg(bound is not None, "the specialised editors declare which region they are drawn in",
               "" if bound else f"{SPECIALISED}: no `const REGION: Region = Region::...;`")
    if bound is None:
        return
    report.leg(bound.group(1) in reserved and bound.group(1) == "CentreLower",
               "they are drawn in the region chrome.rs reserved for them",
               "" if bound.group(1) == "CentreLower" else
               f"{SPECIALISED} draws them in Region::{bound.group(1)}, and chrome.rs reserves "
               f"CentreLower for \"{reserved.get('CentreLower', '')}\"")


# --- The command line --------------------------------------------------------------------------------


CONTRACTS = {
    "play-modes": check_play_modes,
    "live-edit-policy": check_live_edit_policy,
    "specialised-editors": check_specialised_editors,
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

#!/usr/bin/env python3
"""Prove the play-mode and live-edit-policy contracts, by breaking them on purpose.

`tools/abi/selftest.py` states the rule this file follows: **a gate that has never failed is a gate
nobody knows works.** The two criteria this gate replaces — `m11b:play-modes-exist` and
`m11b:live-edit-policy-exists` — were greps that no state of the repository a person would plausibly
reach could turn red, which is the eighth instance of the defect `tools/roadmap/falsify.py`
enumerates. Replacing them with a gate nobody had watched fail would be the ninth.

--- THE FIXTURES ARE DERIVED FROM THE LIVE TREE, NEVER COPIED FROM IT -------------------------------

Each case takes the files the gate reads AS THEY ARE RIGHT NOW, applies one edit into a temporary
tree, and runs the gate against that tree with `--root`. Nothing is stored under a fixtures
directory, for the two reasons `tools/abi/selftest.py` gives: a copied fixture goes stale, and a
copied fixture can pass while the gate is broken — if a reader stopped recognising the enumeration,
a hand-written "renamed" fixture and a hand-written "correct" one would both describe nothing, and
comparing nothing to nothing succeeds.

Case 0 is therefore load-bearing: the UNEDITED copy must pass. Every other case is only meaningful
because it does.

--- THE LAST CASE IS ABOUT THE PARSER RATHER THAN THE CONTRACT --------------------------------------

`bold-table` writes the specification's policy table with bold cells instead of backticked ones —
which is exactly the shape that made this repository's sixth unfalsifiable check parse nothing and
compare nothing to nothing. The gate must ERROR naming the table, not report a contract that holds.

Run through `just quality-editor-contract --selftest`, and by CI through `integration.editor_contract_gate`.
"""

from __future__ import annotations

import io
import shutil
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import play_contract  # noqa: E402  (after the path insert, deliberately)

REPOSITORY = play_contract.REPOSITORY

#: Every file any contract reads. A tree holding these is a tree the gate can judge.
INPUTS = (
    play_contract.MODE_HEADER,
    play_contract.MODE_SOURCE,
    play_contract.EDITOR_PLAY,
    play_contract.POLICY_HEADER,
    play_contract.POLICY_SOURCE,
    play_contract.ATTRIBUTES,
    play_contract.LIVE_EDITING,
    play_contract.EDITOR_ARCHITECTURE,
    play_contract.SPECIALISED,
    play_contract.SPECIALISED_TIMELINE,
    play_contract.CHROME,
    play_contract.SEQUENCING_HEADER,
    play_contract.SEQUENCING_SOURCE,
) + tuple(sorted({source for sources in play_contract.LOWERINGS.values() for source in sources}))

#: `specialised-editors` asks whether every row a refusal names is a specification in this tree, so
#: the sandbox needs the specification set's *names*. Copied as whole files rather than as empty
#: directories, so that a case which points a refusal at a row that does not exist is distinguished
#: from a sandbox that simply has no rows — which is the shape that makes a check compare nothing to
#: nothing and pass.
SPEC_ROWS = "openspec/specs"


@dataclass(frozen=True)
class Case:
    """One edit, the contract it should break, and the words the gate must say about it."""

    name: str
    contract: str
    path: str
    #: The exact text to replace, and what to replace it with. Empty `find` means no edit at all.
    find: str
    replace: str
    #: 0 for the control, 1 for a contract that does not hold, 2 for a file the gate cannot read.
    expect: int
    says: str


CASES = (
    Case("control", "play-modes", play_contract.MODE_SOURCE, "", "", 0,
         "every leg holds"),
    Case("control-policy", "live-edit-policy", play_contract.POLICY_SOURCE, "", "", 0,
         "every leg holds"),

    # --- play-modes -----------------------------------------------------------------------------
    Case("engine-renames-a-mode", "play-modes", play_contract.MODE_SOURCE,
         '"separate-process"', '"second-process"', 1,
         "spell the modes the same way"),
    Case("editor-renames-a-mode", "play-modes", play_contract.EDITOR_PLAY,
         'PlayMode::RemoteDevice => "remote-device"', 'PlayMode::RemoteDevice => "remote"', 1,
         "spell the modes the same way"),
    Case("editor-gains-a-fourth-mode", "play-modes", play_contract.EDITOR_PLAY,
         "    RemoteDevice,\n}", "    RemoteDevice,\n    CloudWorker,\n}", 1,
         "enumerate the same modes"),
    Case("the-sides-disagree-about-stepping", "play-modes", play_contract.EDITOR_PLAY,
         "!matches!(self, PlayMode::RemoteDevice)\n    }\n\n    /// Whether this mode can be asked "
         "to advance exactly one simulation tick",
         "!matches!(self, PlayMode::InEditor)\n    }\n\n    /// Whether this mode can be asked "
         "to advance exactly one simulation tick", 1,
         "which modes can step a frame"),
    Case("a-refusal-names-no-rung", "play-modes", play_contract.MODE_SOURCE,
         "availability.due = kRemoteDeviceDue;\n                return availability;\n            }"
         "\n            if (!support.remote_runtime)",
         "return availability;\n            }\n            if (!support.remote_runtime)", 1,
         "names the rung the mode is due at"),
    Case("the-requirement-drops-a-mode", "play-modes", play_contract.LIVE_EDITING,
         "| `RemoteDevice` | The game runs on a console", "| x | The game runs on a console", 1,
         "names exactly the modes the engine declares"),
    Case("editor-architecture-forgets-a-mode", "play-modes", play_contract.EDITOR_ARCHITECTURE,
         "**remote device**", "**some other machine**", 1,
         "names every mode the engine spells"),
    Case("an-unknown-word-falls-back", "play-modes", play_contract.MODE_SOURCE,
         "return fail(ErrorCode::InvalidArgument,\n                \"play mode:",
         "return PlayMode::InEditor;  // fall back\n    return fallback(ErrorCode::InvalidArgument,"
         "\n                \"play mode:", 1,
         "rather than a nearest mode"),
    Case("from_name-stops-returning-an-option", "play-modes", play_contract.EDITOR_PLAY,
         "pub fn from_name(name: &str) -> Option<PlayMode>",
         "pub fn from_name(name: &str) -> PlayMode", 1,
         "returns Option<PlayMode>"),

    # --- live-edit-policy -----------------------------------------------------------------------
    Case("the-engine-misspells-a-policy", "live-edit-policy", play_contract.POLICY_SOURCE,
         '"recreate-entity"', '"recreate-the-entity"', 1,
         "kebab case"),
    Case("a-policy-is-dropped-from-the-enum", "live-edit-policy", play_contract.POLICY_HEADER,
         "    /// The referenced asset is reloaded and rebound.\n    ReloadAsset,\n", "", 1,
         "names exactly the policies the engine declares"),
    Case("two-policies-share-a-rank", "live-edit-policy", play_contract.POLICY_SOURCE,
         "case LiveEditPolicy::RecreateEntity:\n            return 3;",
         "case LiveEditPolicy::RecreateEntity:\n            return 2;", 1,
         "ranks are distinct"),
    Case("a-restart-becomes-derivable", "live-edit-policy", play_contract.POLICY_SOURCE,
         "        case reflect::PersistenceKind::Derived:\n"
         "            decision.policy = LiveEditPolicy::ReinitializeComponent;",
         "        case reflect::PersistenceKind::Derived:\n"
         "            decision.policy = LiveEditPolicy::RestartWorld;", 1,
         "never DERIVED"),
    Case("a-classification-gains-no-policy", "live-edit-policy", play_contract.ATTRIBUTES,
         "    Derived,          ///< Computed", "    Ephemeral,\n    Derived,          ///< Computed",
         1, "every field classification the engine has is derived from"),
    Case("the-requirement-drops-a-policy", "live-edit-policy", play_contract.LIVE_EDITING,
         "| `ReloadAsset` | The referenced asset", "| x | The referenced asset", 1,
         "names exactly the policies the engine declares"),

    # --- specialised-editors --------------------------------------------------------------------
    Case("control-specialised", "specialised-editors", play_contract.SPECIALISED, "", "", 0,
         "every leg holds"),
    Case("the-requirement-gains-an-editor", "specialised-editors",
         play_contract.EDITOR_ARCHITECTURE,
         "and localisation tables.\n", "and localisation tables, and shader permutations.\n", 1,
         "exactly the editors the requirement enumerates"),
    Case("the-editor-renames-one", "specialised-editors", play_contract.SPECIALISED,
         'Domain::Tilemaps => "tilemaps",', 'Domain::Tilemaps => "tile maps",', 1,
         "exactly the editors the requirement enumerates"),
    Case("a-graph-editor-goes-bespoke", "specialised-editors", play_contract.SPECIALISED,
         "Domain::Materials\n            | Domain::VfxGraph",
         "Domain::Materials => &[Surface::Canvas2D],\n            Domain::VfxGraph", 1,
         "built on the ONE shared Surface::Graph"),
    Case("a-timeline-editor-goes-bespoke", "specialised-editors", play_contract.SPECIALISED,
         "Domain::SequencesAndCinematics => &[Surface::Timeline],",
         "Domain::SequencesAndCinematics => &[Surface::Table],", 1,
         "built on the ONE shared Surface::Timeline"),
    Case("the-engine-gains-a-node-type", "specialised-editors",
         "src/graph/src/lower_pose.cpp",
         'register_node(registry, "pose.ik"', 'register_node(registry, "pose.mirror"', 1,
         "palette is the engine's own pose vocabulary"),
    Case("the-engine-respells-a-track-kind", "specialised-editors",
         play_contract.SEQUENCING_SOURCE, 'return "CameraCut";', 'return "Camera-Cut";', 1,
         "spell every track kind the same way"),
    Case("the-editors-leave-the-reserved-region", "specialised-editors",
         play_contract.SPECIALISED,
         "pub const REGION: Region = Region::CentreLower;",
         "pub const REGION: Region = Region::Right;", 1,
         "drawn in the region chrome.rs reserved for them"),
    Case("chrome-stops-reserving-the-region", "specialised-editors", play_contract.CHROME,
         'Region::CentreLower => "the active specialised editor",',
         'Region::CentreLower => "a second content browser",', 1,
         "still reserves a region for the active specialised editor"),
    Case("a-refusal-names-a-row-that-is-not-there", "specialised-editors",
         play_contract.SPECIALISED,
         'Domain::Tilemaps => "rendering-2d",', 'Domain::Tilemaps => "tilemaps",', 1,
         "every row a refusal names is a specification in this tree"),

    # --- the parser, rather than the contract ----------------------------------------------------
    Case("prose-list", "specialised-editors", play_contract.EDITOR_ARCHITECTURE,
         "The editor SHALL provide dedicated editors for: materials",
         "The editor SHALL provide these dedicated editors. Materials", 2,
         "It is not the paragraph this gate reads"),
    Case("bold-table", "live-edit-policy", play_contract.LIVE_EDITING,
         "| `Immediate` | Applied to running instances directly |\n"
         "| `ReinitializeComponent` | The component is torn down and rebuilt from new data |\n"
         "| `RecreateEntity` | The entity is recreated, preserving identity |\n"
         "| `ReloadAsset` | The referenced asset is reloaded and rebound |\n"
         "| `RestartWorld` | Requires ending and restarting play |\n"
         "| `Unsupported` | Cannot be live edited; the change applies on the next run |",
         "| **Immediate** | Applied to running instances directly |\n"
         "| **ReinitializeComponent** | The component is torn down and rebuilt from new data |\n"
         "| **RecreateEntity** | The entity is recreated, preserving identity |\n"
         "| **ReloadAsset** | The referenced asset is reloaded and rebound |\n"
         "| **RestartWorld** | Requires ending and restarting play |\n"
         "| **Unsupported** | Cannot be live edited; the change applies on the next run |", 2,
         "It is not the table this gate reads"),
)


def materialise(into: Path) -> None:
    for relative in INPUTS:
        destination = into / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPOSITORY / relative, destination)
    for row in sorted((REPOSITORY / SPEC_ROWS).glob("*/spec.md")):
        destination = into / SPEC_ROWS / row.parent.name / "spec.md"
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(row, destination)


def apply(root: Path, case: Case) -> None:
    if not case.find:
        return
    path = root / case.path
    text = path.read_text(encoding="utf-8")
    if text.count(case.find) < 1:
        raise SystemExit(f"selftest: case '{case.name}' edits {case.path}, which no longer contains "
                         f"the text it edits:\n  {case.find[:120]!r}\n"
                         "The case is stale — correct it rather than deleting it.")
    path.write_text(text.replace(case.find, case.replace), encoding="utf-8")


def run(case: Case, root: Path) -> tuple[int, str]:
    captured = io.StringIO()
    with redirect_stdout(captured), redirect_stderr(captured):
        code = play_contract.main([case.contract, "--root", str(root)])
    return code, captured.getvalue()


def main() -> int:
    failures = 0
    for case in CASES:
        with tempfile.TemporaryDirectory(prefix="cy-editor-contract-") as directory:
            root = Path(directory)
            materialise(root)
            apply(root, case)
            code, output = run(case, root)
        wrong = []
        if code != case.expect:
            wrong.append(f"exit {code}, expected {case.expect}")
        if case.says not in output:
            wrong.append(f"the output never says {case.says!r}")
        if wrong:
            failures += 1
            print(f"FAILED  {case.name}: {'; '.join(wrong)}")
            for line in output.splitlines():
                print(f"          {line}")
        else:
            print(f"ok      {case.name}: {case.contract} exits {code} and says {case.says!r}")
    print()
    if failures:
        print(f"{failures} of {len(CASES)} case(s) FAILED: the gate no longer notices what it must.")
        return 1
    print(f"{len(CASES)} case(s): the gate accepts the tree and refuses every break of it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

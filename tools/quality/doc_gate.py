#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The undocumented-symbol gate — `testing-and-quality`'s "Documentation as a gate".

    Every public API — C++ headers, the C ABI, and the Swift overlay — SHALL carry documentation
    comments, and CI SHALL fail when a newly exported symbol is undocumented.

    #### Scenario: Undocumented public API
    - **WHEN** a new public function is added without documentation
    - **THEN** CI SHALL fail naming the symbol

This is the "documentation gate" the M11 exit criteria name, and until M11.d it did not exist.

--- WHY THERE IS A BASELINE, AND WHY IT IS NOT AN EXCUSE ---------------------------------------------

The requirement's own verb is **newly**: CI fails when a *newly* exported symbol is undocumented.
That is not the same sentence as "every symbol in the tree is documented today", and writing the
second one would have produced a gate that goes red on its first run and is therefore switched off —
which is worse than no gate, because it is a gate nobody reads.

So the shape is the one `src/abi/abi_baseline.json` already uses in this repository: the symbols that
were undocumented when the gate landed are recorded, by name, in `doc_baseline.txt`, and

  * a public symbol **not** in the baseline and **not** documented fails the gate, naming it;
  * a baseline symbol that has since been documented, renamed or deleted **also** fails, telling the
    author to remove the line — so the backlog can only shrink, and a stale baseline is a red build
    rather than a quiet exemption.

The second rule is what stops this from being an allowlist. An allowlist that is never re-checked
grows; a baseline that fails when it goes stale cannot.

--- WHAT COUNTS AS PUBLIC, AND WHAT COUNTS AS DOCUMENTED ---------------------------------------------

**Public** is `src/<module>/include/cy/**/*.h` — the directories CMake puts on a consumer's include
path — plus the C ABI header and the Swift overlay's `public` declarations. A header under `src/*/src/`
is private by construction and is not read here.

**A symbol** is a namespace-scope declaration: a type (`class`, `struct`, `enum class`, `union`), a
free function, a type alias, or a namespace-scope constant. Class *members* are deliberately not
checked — a rule that demanded a doc comment on every accessor would be satisfied by noise, and the
scenario the specification writes is about a public *function* being added.

**Documented** is a comment immediately above the declaration: `///`, `/** … */`, or a `//` block.
The comment must be non-empty after its marker, so `///` on its own line is not documentation.

--- THE PARSER IS REGEX, AND THAT IS A DECISION WITH A GUARD -----------------------------------------

libclang is available in this repository (the reflection generator uses it) and is not used here, for
the reason `just quality-abi` gives for computing the ABI description from declarations rather than
from a build: this gate must run on a bare checkout, in seconds, before anything is configured.

The cost is that a regex can stop recognising the tree and report nothing, which is precisely this
project's most-repeated defect — a check that compares nothing to nothing and passes. Two guards:

  * `--summary` prints the symbol count per root, and `selftest.py` case 0 requires it to stay within
    a recorded band, so a parser that fell silent is a red build;
  * a header that yields **zero** namespace-scope symbols while containing the text `namespace cy`
    is reported as a parse failure rather than as a clean file.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]

#: The public C++ surface: a module's `include/cy/` tree is what its consumers compile against.
CPP_GLOB = "src/*/include/cy/**/*.h"

#: The C ABI. `native-abi` makes this the one header whose every entry is an exported symbol.
ABI_HEADER = "src/abi/include/cy/abi/cy_abi.h"

#: The Swift overlay. `swift-scripting`'s public surface, the third of the three the requirement names.
SWIFT_ROOTS = ("bindings/swift/Sources/CyberdyneKit", "bindings/swift/Sources/CyberdyneMacros")

#: Generated code is checked where it is generated, not where it lands: `just generate-check` already
#: fails when these drift from their generator, and a doc comment hand-added here would be erased by
#: the next run. Named rather than globbed so that adding a generated directory is a visible act.
GENERATED = ("bindings/swift/Sources/CyberdyneCore/Generated",)

#: A namespace whose contents are not API. The engine spells its private corners this way.
PRIVATE_NAMESPACES = ("detail", "internal", "impl")

BASELINE = Path(__file__).resolve().parent / "doc_baseline.txt"

# --- Recognising a declaration ---------------------------------------------------------------------
#
# Each pattern captures the symbol's name in group "name". They are applied to a single logical line
# at namespace scope, after comments and preprocessor directives have been removed.

TYPE = re.compile(
    r"^(?:template\s*<[^>]*>\s*)?"
    r"(?P<kind>class|struct|union|enum\s+class|enum\s+struct)\s+"
    r"(?:\[\[[^\]]*\]\]\s*)?(?P<name>[A-Za-z_]\w*)\b"
)
ALIAS = re.compile(r"^(?:template\s*<[^>]*>\s*)?using\s+(?P<name>[A-Za-z_]\w*)\s*=")
CONSTANT = re.compile(
    r"^(?:inline\s+|constexpr\s+|const\s+|static\s+|extern\s+)+"
    r"[\w:<>,\s*&]+?\b(?P<name>[A-Za-z_]\w*)\s*(?:=|\{)"
)
FUNCTION = re.compile(
    r"^(?:template\s*<[^>]*>\s*)?"
    r"(?:\[\[[^\]]*\]\]\s*)*"
    r"(?:inline\s+|constexpr\s+|consteval\s+|static\s+|extern\s+|friend\s+|virtual\s+)*"
    r"(?:[\w:]+(?:\s*<[^;{]*>)?(?:\s*[*&]+|\s+))+"
    r"(?P<name>[A-Za-z_]\w*)\s*\("
)
SWIFT = re.compile(
    r"^public\s+(?:final\s+|static\s+|class\s+|open\s+)*"
    r"(?P<kind>func|var|let|struct|class|enum|protocol|typealias|actor|macro|init|subscript)"
    r"(?:\s+(?P<name>[A-Za-z_]\w*))?"
)
ABI_FUNCTION = re.compile(r"^(?:CY_ABI_API\s+)?[\w ]+\**\s*\(?\*?(?P<name>cy_[a-z0-9_]+)\s*\)?\s*\(")

#: `struct X;` and `class X;` — a forward declaration names nothing new.
FORWARD = re.compile(r"^(?:template\s*<[^>]*>\s*)?(?:class|struct|union)\s+\w+\s*;\s*$")

#: What a doc comment looks like once its marker is stripped. An empty one is not documentation.
DOC_MARKER = re.compile(r"^\s*(///<?|//!|\*|/\*\*|//)\s*(?P<text>.*?)\s*(\*/)?$")


@dataclass(frozen=True)
class Symbol:
    """One public declaration, and whether the line above it explains what it is."""

    path: str
    line: int
    name: str
    documented: bool

    @property
    def key(self) -> str:
        """The baseline's identity for a symbol: the file and the name, never the line number.

        A line number would make the baseline go stale on every unrelated edit above it, and a
        baseline that has to be regenerated constantly is a baseline nobody reads.
        """
        return f"{self.path}::{self.name}"


def _strip_block_comments(text: str) -> str:
    """Replace `/* … */` with spaces, preserving line structure so line numbers survive."""
    out = []
    index = 0
    while True:
        start = text.find("/*", index)
        if start < 0:
            out.append(text[index:])
            return "".join(out)
        end = text.find("*/", start + 2)
        if end < 0:
            out.append(text[index:start])
            out.append("\n" * text.count("\n", start))
            return "".join(out)
        out.append(text[index:start])
        out.append("\n" * text.count("\n", start, end))
        index = end + 2


def _documented(lines: list[str], index: int) -> bool:
    """Is the declaration at `index` preceded by a non-empty comment block?"""
    cursor = index - 1
    while cursor >= 0:
        stripped = lines[cursor].strip()
        if not stripped:
            return False
        if stripped.startswith("#"):  # an `#if` between the comment and the declaration is fine
            cursor -= 1
            continue
        if stripped.endswith("*/") or stripped.startswith("//"):
            match = DOC_MARKER.match(stripped)
            return bool(match and match.group("text"))
        return False
    return False


def _depth_before(line: str) -> int:
    """Braces opened minus braces closed, ignoring those inside character and string literals."""
    cleaned = re.sub(r"'(?:\\.|[^'])*'|\"(?:\\.|[^\"])*\"", "", line)
    return cleaned.count("{") - cleaned.count("}")


def scan_cpp(path: Path, text: str) -> tuple[list[Symbol], bool]:
    """Every namespace-scope declaration in a C++ header, and whether the file parsed at all."""
    stripped = _strip_block_comments(text)
    raw = text.splitlines()
    lines = stripped.splitlines()

    symbols: list[Symbol] = []
    depth = 0
    namespaces: list[str] = []
    relative = str(path.relative_to(REPOSITORY))

    for index, line in enumerate(lines):
        code = line.split("//", 1)[0].strip()
        if not code or code.startswith("#"):
            depth += 0 if code.startswith("#") else _depth_before(line)
            continue

        namespace = re.match(r"^namespace\s+(?P<name>[\w:]+)?", code)
        if namespace:
            namespaces.append(namespace.group("name") or "")
            depth += _depth_before(line)
            continue

        at_namespace_scope = depth == len(namespaces)
        private = any(part in PRIVATE_NAMESPACES for name in namespaces for part in name.split("::"))

        if at_namespace_scope and not private and not FORWARD.match(code):
            for pattern in (TYPE, ALIAS, FUNCTION, CONSTANT):
                match = pattern.match(code)
                if match and match.group("name"):
                    symbols.append(
                        Symbol(relative, index + 1, match.group("name"), _documented(raw, index))
                    )
                    break

        before = depth
        depth += _depth_before(line)
        if depth < len(namespaces) and before >= len(namespaces):
            namespaces = namespaces[: max(depth, 0)]

    # The parse guard: a header that declares a cy namespace and yields nothing has not been read.
    parsed = bool(symbols) or "namespace cy" not in text
    return symbols, parsed


def scan_swift(path: Path, text: str) -> list[Symbol]:
    """Every `public` declaration in the overlay."""
    relative = str(path.relative_to(REPOSITORY))
    lines = text.splitlines()
    symbols = []
    # A macro's expansion is Swift inside a `"""` literal — text this file writes, not API it
    # exports. Counting it would put a `\(raw: name)` placeholder in the baseline forever.
    in_literal = False
    for index, line in enumerate(lines):
        if line.count('"""') % 2 == 1:
            in_literal = not in_literal
            continue
        if in_literal:
            continue
        match = SWIFT.match(line.strip())
        if match:
            name = match.group("name") or match.group("kind")
            symbols.append(Symbol(relative, index + 1, name, _documented(lines, index)))
    return symbols


def scan_abi(path: Path, text: str) -> list[Symbol]:
    """Every `cy_`-prefixed entry point and table slot in the C ABI header."""
    relative = str(path.relative_to(REPOSITORY))
    stripped = _strip_block_comments(text)
    raw = text.splitlines()
    symbols = []
    for index, line in enumerate(stripped.splitlines()):
        code = line.split("//", 1)[0].strip()
        match = ABI_FUNCTION.match(code)
        if match:
            symbols.append(Symbol(relative, index + 1, match.group("name"), _documented(raw, index)))
    return symbols


def collect(root: Path) -> tuple[list[Symbol], list[str]]:
    """Every public symbol under `root`, and the headers whose parse looks wrong."""
    symbols: list[Symbol] = []
    unparsed: list[str] = []

    for path in sorted(root.glob(CPP_GLOB)):
        found, parsed = scan_cpp(path, path.read_text(encoding="utf-8"))
        symbols += found
        if not parsed:
            unparsed.append(str(path.relative_to(root)))

    abi = root / ABI_HEADER
    if abi.is_file():
        symbols = [symbol for symbol in symbols if symbol.path != ABI_HEADER]
        symbols += scan_abi(abi, abi.read_text(encoding="utf-8"))

    for swift_root in SWIFT_ROOTS:
        for path in sorted((root / swift_root).rglob("*.swift")):
            if any(part in str(path) for part in GENERATED):
                continue
            symbols += scan_swift(path, path.read_text(encoding="utf-8"))

    return symbols, unparsed


def read_baseline(path: Path) -> set[str]:
    if not path.is_file():
        return set()
    return {
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.startswith("#")
    }


def write_baseline(path: Path, keys: set[str], total: int) -> None:
    header = (
        "# The public symbols that were undocumented when the undocumented-symbol gate landed,\n"
        "# at M11.d. tools/quality/doc_gate.py explains why this file exists and why it can only\n"
        "# shrink: a line here that has since been documented, renamed or deleted FAILS the gate.\n"
        "#\n"
        "# Regenerate with `just quality-docs --update`, and only when you have just documented\n"
        f"# something. Backlog at last write: {len(keys)} of {total} public symbols.\n"
    )
    path.write_text(header + "\n".join(sorted(keys)) + "\n", encoding="utf-8")


def main() -> int:
    global REPOSITORY  # the scanners report paths relative to the tree being judged

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=None)
    parser.add_argument("--baseline", type=Path, default=None)
    parser.add_argument("--summary", action="store_true", help="counts only, for the selftest")
    parser.add_argument("--update", action="store_true", help="record today's backlog as the baseline")
    arguments = parser.parse_args()

    REPOSITORY = (arguments.root or REPOSITORY).resolve()
    root = REPOSITORY
    baseline_path = arguments.baseline or (root / "tools/quality/doc_baseline.txt")

    symbols, unparsed = collect(root)
    undocumented = {symbol.key for symbol in symbols if not symbol.documented}

    if arguments.summary:
        print(f"symbols {len(symbols)}")
        print(f"undocumented {len(undocumented)}")
        print(f"unparsed {len(unparsed)}")
        return 0

    if arguments.update:
        write_baseline(baseline_path, undocumented, len(symbols))
        print(f"doc-gate: baseline written — {len(undocumented)} of {len(symbols)} undocumented")
        return 0

    baseline = read_baseline(baseline_path)
    new = sorted(undocumented - baseline)
    stale = sorted(baseline - undocumented)

    if unparsed:
        print("doc-gate: these public headers declare a cy namespace and parsed to no symbol.",
              file=sys.stderr)
        print("  That is the gate failing to read the tree, not the tree being clean:",
              file=sys.stderr)
        for path in unparsed:
            print(f"    {path}", file=sys.stderr)
        return 1

    if new:
        print(f"doc-gate: {len(new)} public symbol(s) exported without a documentation comment.",
              file=sys.stderr)
        print("  `testing-and-quality`: every public API SHALL carry documentation comments.",
              file=sys.stderr)
        by_key = {symbol.key: symbol for symbol in symbols if not symbol.documented}
        for key in new:
            symbol = by_key[key]
            print(f"    {symbol.path}:{symbol.line}  {symbol.name}", file=sys.stderr)
        print("  Write a /// comment above each, saying what it is for rather than what it is.",
              file=sys.stderr)
        return 1

    if stale:
        print(f"doc-gate: {len(stale)} baseline entr{'y' if len(stale) == 1 else 'ies'} "
              "no longer name an undocumented symbol.", file=sys.stderr)
        print("  The backlog only shrinks, so this is a red build rather than a quiet exemption.",
              file=sys.stderr)
        for key in stale:
            print(f"    {key}", file=sys.stderr)
        print("  Remove those lines: `just quality-docs --update`.", file=sys.stderr)
        return 1

    documented = len(symbols) - len(undocumented)
    print(f"doc-gate: {documented} of {len(symbols)} public symbols documented; "
          f"{len(baseline)} in the declared backlog, 0 new")
    return 0


if __name__ == "__main__":
    sys.exit(main())

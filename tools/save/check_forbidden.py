#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the ten forbidden save patterns `save-and-persistence` says SHALL be checkable.

The requirement "Forbidden save patterns" lists ten things that SHALL NOT appear and says "each
SHALL be checkable". Until this file they were a review: a sentence nobody could run. Each one is
now a check, and each check is one of two kinds, chosen by where the pattern would live:

  static   the pattern is a shape of CODE — a memory copy into a save sink, a file opened for
           writing in place, a load that returns `bool`. This script reads every production C++
           file that is save code (under src/save/) or includes a `<cy/save/...>` header, with
           comments and string literals removed, and reports each hit with its file and line.
  runtime  the pattern is a property of DATA — whether the bytes of a commit mix scopes, whether a
           load that cannot recover invents a value. No grep can see that, so the check is a case
           in `unit.save` named `forbidden save pattern <id>: ...`, and this script runs the suite
           binary once per pattern with that filter and requires at least one selected case and a
           pass. A renamed or deleted case is a failure here, not a silent loss.

Seven of the ten carry both kinds; the three whose subject is only data (a monolithic blob,
invented state) or only code (runtime identity) carry the one that can see them.

    python3 tools/save/check_forbidden.py --list            # the ten, one per line
    python3 tools/save/check_forbidden.py --binary <path>   # every check; exit 1 on any finding

Without `--binary` the runtime checks cannot run, and the script says so and FAILS rather than
reporting the static half as the whole: a check that is skipped is not a check that passed.

`tools/save/selftest.py` proves every static check red by planting its pattern in a copy of the
real tree, and proves the checker refuses an empty tree.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable

SOURCE_SUFFIXES = (".h", ".hpp", ".cpp", ".cc", ".inl")
SCAN_ROOTS = ("src", "samples", "tools")
SAVE_INCLUDE = re.compile(r"#\s*include\s*<cy/save/")


@dataclass
class Source:
    path: str  # repository-relative, forward slashes
    code: str  # comments and string literals blanked, line structure kept
    raw: str

    def lines(self) -> Iterable[tuple[int, str]]:
        return enumerate(self.code.splitlines(), start=1)


@dataclass
class Finding:
    pattern: str
    path: str
    line: int
    text: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: [{self.pattern}] {self.text.strip()}"


def _skip_literal(text: str, i: int) -> int:
    """The index just past the string or character literal opening at `i` (or at its line's end)."""
    quote = text[i]
    j = i + 1
    while j < len(text) and text[j] not in (quote, "\n"):
        j += 2 if text[j] == "\\" else 1
    return min(j + 1, len(text)) if j < len(text) and text[j] == quote else j


def strip_code(text: str) -> str:
    """Blank comments and string/char literals, keeping every newline so line numbers hold."""
    out: list[str] = []
    i = 0
    while i < len(text):
        two = text[i:i + 2]
        if two == "//":
            end = text.find("\n", i)
            end = len(text) if end < 0 else end
        elif two == "/*":
            end = text.find("*/", i + 2)
            end = len(text) if end < 0 else end + 2
        elif text[i] in "\"'":
            end = _skip_literal(text, i)
        else:
            out.append(text[i])
            i += 1
            continue
        out.append(re.sub(r"[^\n]", " ", text[i:end]))
        i = end
    return "".join(out)


def is_production(path: str) -> bool:
    parts = path.split("/")
    return not any(part in ("tests", "test", "generated") for part in parts) and not parts[
        -1
    ].startswith("test_")


def _candidates(root: Path) -> Iterable[tuple[str, Path]]:
    for top in SCAN_ROOTS:
        base = root / top
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            rel = path.relative_to(root).as_posix()
            if path.suffix in SOURCE_SUFFIXES and path.is_file() and is_production(rel):
                yield rel, path


def collect_sources(root: Path) -> list[Source]:
    sources: list[Source] = []
    for rel, path in _candidates(root):
        raw = path.read_text(encoding="utf-8", errors="replace")
        if rel.startswith("src/save/") or SAVE_INCLUDE.search(raw):
            sources.append(Source(rel, strip_code(raw), raw))
    return sources


# --- The static checks --------------------------------------------------------------------------
#
# Each takes every save source and returns its findings. They are regular expressions over code with
# comments and literals blanked, which is deliberately crude: the patterns they look for are ones a
# reviewer names in one line, and a check a reviewer cannot read is one nobody trusts.

# A call that puts bytes into a save: a backend write, a value-record field, the overlay's recorders.
SINK = re.compile(r"\.\s*(write|set|set_scalar|record_component|record_fragment|append)\s*\(")
CLASS_SIZEOF = re.compile(r"sizeof\s*\(\s*\*?\s*(this|[A-Z][A-Za-z0-9_]+|[a-z_][\w:]*::[A-Z]\w*)\s*\)")
ADDRESS_AS_BYTES = re.compile(r"reinterpret_cast\s*<\s*const\s+(u8|std::byte|char|unsigned\s+char)\s*\*\s*>\s*\(\s*&")


def statements(source: Source) -> Iterable[tuple[int, str]]:
    """Code split at ';', each with the line it starts on — a sink call spans lines. Braces do not
    split, because brace initialisation is exactly where an identity is built; `line_of` places a
    match within its statement."""
    line = 1
    start_line = 1
    current: list[str] = []
    for ch in source.code:
        if not current and ch.isspace():
            if ch == "\n":
                line += 1
            continue
        if not current:
            start_line = line
        current.append(ch)
        if ch == "\n":
            line += 1
        if ch == ";":
            yield start_line, "".join(current)
            current = []
    if current:
        yield start_line, "".join(current)


def line_of(start_line: int, statement: str, match: re.Match[str]) -> int:
    return start_line + statement.count("\n", 0, match.start())


def check_raw_memory(sources: list[Source]) -> list[Finding]:
    findings = []
    for source in sources:
        for line, statement in statements(source):
            sink = SINK.search(statement)
            if sink is None:
                continue
            tail = statement[sink.start():]
            if CLASS_SIZEOF.search(tail) or ADDRESS_AS_BYTES.search(tail):
                findings.append(Finding("raw-memory", source.path, line_of(line, statement, sink),
                                        " ".join(tail.split())[:160]))
    return findings


RUNTIME_ID_SOURCE = re.compile(
    r"PersistentId\s*[({][^;]*?(\.index\b|\.slot\b|archetype|\.row\b|uintptr_t|reinterpret_cast|"
    r"\bthis\b|\.generation\b|entity_index|ecs::Entity\b)"
)
ECS_INCLUDE = re.compile(r"#\s*include\s*<cy/ecs/")
POINTER_INTEGER = re.compile(r"\b(uintptr_t|intptr_t)\b")


def _save_code_holds_runtime_identity(source: Source) -> list[Finding]:
    """Inside src/save/ only: no ECS, whose entity is a runtime index, and no address as a number."""
    findings = []
    for number, text in source.lines():
        if ECS_INCLUDE.search(text):
            findings.append(Finding("runtime-identity", source.path, number,
                                    "save code includes the ECS, whose entity is a runtime index"))
        if POINTER_INTEGER.search(text):
            findings.append(Finding("runtime-identity", source.path, number,
                                    "save code holds an address as an integer"))
    return findings


def check_runtime_identity(sources: list[Source]) -> list[Finding]:
    findings = []
    for source in sources:
        for line, statement in statements(source):
            match = RUNTIME_ID_SOURCE.search(statement)
            if match:
                findings.append(Finding("runtime-identity", source.path,
                                        line_of(line, statement, match),
                                        "a persistent identity built from " + match.group(1)))
        if source.path.startswith("src/save/"):
            findings.extend(_save_code_holds_runtime_identity(source))
    return findings


RECORD_FROM_OBJECT = re.compile(r"record_from_object\s*\(([^;]*)")
WRITE_OBJECT = re.compile(r"write_object\s*\(([^;]*)")


def check_derived_saved(sources: list[Source]) -> list[Finding]:
    """A save builds records for `Purpose::Persistence`, whose column holds no derived field."""
    findings = []
    for source in sources:
        for line, statement in statements(source):
            for regex in (RECORD_FROM_OBJECT, WRITE_OBJECT):
                match = regex.search(statement)
                if match and "Purpose::Persistence" not in match.group(1) and "purpose" not in match.group(1):
                    findings.append(Finding("derived-saved", source.path,
                                            line_of(line, statement, match),
                                            "a save record built for a purpose other than Persistence"))
    return findings


RESIDENCY_READ = re.compile(r"\b(residency_of|resident_region_count)\s*\(|\.\s*residency\b")
# Files allowed to read residency: the overlay that stores it and the inspector that reports it.
RESIDENCY_READERS = ("src/save/src/overlay.cpp", "src/save/src/inspect.cpp",
                     "src/save/src/inspect_text.cpp", "src/save/include/cy/save/overlay.h")


def check_whole_world(sources: list[Source]) -> list[Finding]:
    """Nothing in save code that WRITES a save may depend on what is resident."""
    findings = []
    for source in sources:
        if not source.path.startswith("src/save/") or source.path in RESIDENCY_READERS:
            continue
        for line, text in source.lines():
            if RESIDENCY_READ.search(text):
                findings.append(Finding("whole-world-resident", source.path, line,
                                        "save code reads residency"))
    return findings


IN_PLACE_WRITE = re.compile(
    r"FileMode::(Write|ReadWrite)\b|\bfopen\s*\(|std::ofstream|std::fstream|\bO_TRUNC\b|\bO_WRONLY\b|"
    r"\bO_RDWR\b|fs::copy_file\s*\(|fs::move_file\s*\(|\bfreopen\s*\("
)


def check_in_place(sources: list[Source]) -> list[Finding]:
    """Every byte a save writes to a file goes through `fs::write_atomic`: write, flush, rename."""
    findings = []
    for source in sources:
        for line, text in source.lines():
            match = IN_PLACE_WRITE.search(text)
            if match:
                findings.append(Finding("destructive-in-place", source.path, line,
                                        f"a file written with {match.group(0)} rather than write_atomic"))
    return findings


COOKED_INCLUDE = re.compile(r"#\s*include\s*<cy/(core/assets/package|cook/|build/)")
COOKED_MUTATION = re.compile(r"\b(write_package|rewrite_package|patch_package|migrate_package|"
                             r"migrate_cooked|recook)\s*\(")


def check_cooked_migration(sources: list[Source]) -> list[Finding]:
    """Save code reads no cooked package and writes none: a mismatch is refused (MissingContent)."""
    findings = []
    for source in sources:
        if source.path.startswith("src/save/"):
            for number, text in source.lines():
                if COOKED_INCLUDE.search(text):
                    findings.append(Finding("cooked-migration", source.path, number,
                                            "save code includes the cooked-package or cook writer"))
        for line, text in source.lines():
            match = COOKED_MUTATION.search(text)
            if match:
                findings.append(Finding("cooked-migration", source.path, line,
                                        f"save code calls {match.group(1)}"))
    return findings


CRYPTO_WORD = re.compile(r"\b\w*(encrypt|decrypt|cipher|obfuscat|xor_?key|chacha|salsa20|rc4|aes)\w*\b",
                         re.IGNORECASE)
XOR_WITH_KEY = re.compile(r"\^=?\s*\w*key\w*", re.IGNORECASE)
ESTABLISHED_CRYPTO = re.compile(r"#\s*include\s*<(mbedtls|psa)/")
INTEGRITY_CLAIM = re.compile(
    r"((en|de)crypt|cipher)\w*[^;\n]*(verif|integrit|tamper|authentic_?check)|"
    r"(verif|integrit|tamper)\w*[^;\n]*((en|de)crypt|cipher)", re.IGNORECASE)


def check_crypto(sources: list[Source]) -> list[Finding]:
    """No cryptography written here, and no encryption result used as an integrity verdict."""
    findings = []
    for source in sources:
        established = bool(ESTABLISHED_CRYPTO.search(source.raw))
        for line, text in source.lines():
            if XOR_WITH_KEY.search(text):
                findings.append(Finding("bespoke-crypto", source.path, line,
                                        "bytes mixed with a key by hand"))
            if INTEGRITY_CLAIM.search(text):
                findings.append(Finding("bespoke-crypto", source.path, line,
                                        "an encryption result used as an integrity check"))
            elif not established and CRYPTO_WORD.search(text):
                findings.append(Finding("bespoke-crypto", source.path, line,
                                        "cryptography without an established library (mbedTLS)"))
    return findings


BOOL_LOAD = re.compile(
    r"\bbool\s+(\w+::)?(load\w*|restore\w*|decode\w*|read_manifest|inspect\w*|check_compatibility|"
    r"resume\w*|from_save\w*)\s*\("
)


def check_boolean_load(sources: list[Source]) -> list[Finding]:
    """A load answers with a Status and a structured LoadReport, never a bool."""
    findings = []
    for source in sources:
        for line, text in source.lines():
            match = BOOL_LOAD.search(text)
            if match:
                findings.append(Finding("boolean-load", source.path, line,
                                        f"{match.group(2)} returns bool"))
    return findings


@dataclass
class Pattern:
    id: str
    title: str
    static: Callable[[list[Source]], list[Finding]] | None = None
    runtime: bool = False
    extra: list[str] = field(default_factory=list)

    @property
    def how(self) -> str:
        kinds = [k for k, on in (("static", self.static is not None), ("runtime", self.runtime)) if on]
        return "+".join(kinds)


# The specification's order and wording. The ids are what the unit.save case names carry.
PATTERNS: list[Pattern] = [
    Pattern("raw-memory", "Raw runtime memory serialised as a save format", check_raw_memory, True),
    Pattern("runtime-identity",
            "Runtime entity indices, pointers, or archetype positions used as persistent identity",
            check_runtime_identity),
    Pattern("derived-saved", "Derived caches saved rather than reconstructed", check_derived_saved, True),
    Pattern("whole-world-resident", "A save requiring the whole world to be resident", check_whole_world,
            True),
    Pattern("destructive-in-place", "Save writes performed destructively in place", check_in_place, True),
    Pattern("monolithic-blob", "One monolithic save blob mixing profile, campaign, and session state",
            None, True),
    Pattern("cooked-migration", "Runtime migration of cooked content", check_cooked_migration, True),
    Pattern("bespoke-crypto", "Bespoke cryptography, or encryption presented as integrity", check_crypto,
            True),
    Pattern("boolean-load", "A boolean returned as the result of a failed load", check_boolean_load, True),
    Pattern("invented-state", "Inventing authoritative state to replace unrecoverable data", None, True),
]


# --- The runtime checks -------------------------------------------------------------------------

CASES = re.compile(r"test cases:\s*(\d+)\s*\|\s*(\d+)\s*passed\s*\|\s*(\d+)\s*failed")


def run_runtime(pattern: Pattern, binary: Path) -> tuple[bool, str]:
    case_filter = f"forbidden save pattern {pattern.id}:*"
    try:
        result = subprocess.run([str(binary), f"-tc={case_filter}"], capture_output=True, text=True,
                                timeout=120)
    except (OSError, subprocess.TimeoutExpired) as error:
        return False, f"could not run {binary}: {error}"
    output = result.stdout + result.stderr
    match = CASES.search(output)
    selected = int(match.group(1)) if match else 0
    if selected == 0:
        return False, f"no case matches '{case_filter}' in {binary.name} — renamed, deleted, or never written"
    if result.returncode != 0:
        tail = "\n".join(output.strip().splitlines()[-15:])
        return False, f"{selected} case(s) ran and FAILED:\n{tail}"
    return True, f"{selected} case(s) passed"


def check_pattern(pattern: Pattern, sources: list[Source], binary: Path | None,
                  static_only: bool) -> tuple[bool, list[str]]:
    """Every check one pattern carries, printing each static finding. A runtime check that cannot
    run is a failure, never a skip, unless the caller asked for the static half by name."""
    notes: list[str] = []
    ok = True
    if pattern.static is not None:
        findings = pattern.static(sources)
        ok = not findings
        notes.append(f"static: {len(findings)} finding(s) over {len(sources)} files")
        for finding in findings:
            print(f"  {finding}")
    if not pattern.runtime:
        return ok, notes
    if static_only:
        notes.append("runtime: skipped (--static-only)")
    elif binary is None:
        ok = False
        notes.append("runtime: NOT RUN — pass --binary <cy_test_unit_save>")
    else:
        passed, note = run_runtime(pattern, binary)
        ok = ok and passed
        notes.append("runtime: " + note)
    return ok, notes


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--binary", type=Path, help="the unit.save suite binary (cy_test_unit_save)")
    parser.add_argument("--list", action="store_true", help="print the ten patterns and how each is checked")
    parser.add_argument("--static-only", action="store_true",
                        help="run the static half alone and say it was partial (for the selftest)")
    args = parser.parse_args(argv)

    if args.list:
        for pattern in PATTERNS:
            print(f"{pattern.id}\t{pattern.how}\t{pattern.title}")
        return 0

    sources = collect_sources(args.root)
    if not any(source.path.startswith("src/save/src/") for source in sources):
        print(f"check_forbidden: no save sources under {args.root}/src/save/src — refusing to report a "
              "clean result over nothing", file=sys.stderr)
        return 1

    failures = 0
    for pattern in PATTERNS:
        ok, notes = check_pattern(pattern, sources, args.binary, args.static_only)
        print(f"{'ok  ' if ok else 'FAIL'} {pattern.id}: {'; '.join(notes)}")
        failures += 0 if ok else 1

    scope = " (STATIC HALF ONLY — the runtime cases did not run)" if args.static_only else ""
    print(f"{len(PATTERNS) - failures} of {len(PATTERNS)} forbidden save patterns checked clean{scope}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""A source location may not be registered as a trace name.

`diagnostics-profiling-and-crash`, "Privacy classification":

    **A source location is classified data, not a name.** Any value carrying a filesystem path —
    including one the compiler injects through `__FILE__` or an equivalent — SHALL be a classified
    field, reachable by the writer's redaction. Registering such a value as an event or scope
    *name* places it structurally beyond redaction, and SHALL NOT be done.

M0's gate found one instance of this and the repair was a compiler flag. A flag repairs the
instance; this lint is what closes the class. It runs over the whole tree, not over
`src/core/diagnostics/`, because the forbidden shape is available to every subsystem that includes
`field.h` — and the next one to write it will not be the module that declared the rule.

The rule, stated as the four things a name may not be built from:

  1. `__FILE__`, `__FILE_NAME__` or `std::source_location::file_name()` anywhere inside a
     `register_name()`, `register_category()`, `CY_TRACE_NAME()` or `CY_TRACE_CATEGORY()` call;
  2. a `.file` or `->file` member — which is how `cy::AssertionFailure` hands a path to a handler;
  3. a string literal that looks like a path to a source file;
  4. `CY_BREADCRUMB` with any of the above, since a breadcrumb's phase is a name too.

`register_source_location()` is the permitted destination, and it is what the lint tells an author
to use.

Exit status is 0 when nothing is found and 1 when something is, so it is a gate rather than a
report. `--self-test` proves it can fail by running it over a snippet that contains the shape.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# The calls that intern a string into a table the writer cannot redact.
NAME_CALLS = ("register_name", "register_category", "CY_TRACE_NAME", "CY_TRACE_CATEGORY",
              "CY_BREADCRUMB", "CY_LOG_CATEGORY")

# What may never appear inside one of them.
PATH_SOURCES = (
    (re.compile(r"__FILE__|__FILE_NAME__"), "__FILE__ is a filesystem path the compiler injected"),
    (re.compile(r"\bfile_name\s*\(\s*\)"), "std::source_location::file_name() is a path"),
    (re.compile(r"(?:\.|->)\s*file\b"), "a `.file` member carries a path (cy::AssertionFailure)"),
    (re.compile(r'"[^"\n]*[/\\][^"\n]*\.(?:cpp|cc|cxx|h|hpp|inl)"'),
     "a literal that names a source file under a directory is a path"),
)

SUFFIXES = (".h", ".hpp", ".inl", ".cpp", ".cc", ".cxx")

SKIP_PARTS = ("build", ".git", "_deps", "third_party", "thirdparty")


def call_spans(text: str) -> list[tuple[int, int, str]]:
    """Every `<call>( ... )` region, balanced, as (start, end, call name)."""
    spans: list[tuple[int, int, str]] = []
    for call in NAME_CALLS:
        for match in re.finditer(rf"\b{call}\s*\(", text):
            depth = 0
            index = match.end() - 1
            while index < len(text):
                if text[index] == "(":
                    depth += 1
                elif text[index] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                index += 1
            spans.append((match.start(), min(index + 1, len(text)), call))
    return spans


def findings_in(path: Path, text: str) -> list[str]:
    """Every violation in one file, as a rendered line."""
    found: list[str] = []
    for start, end, call in call_spans(text):
        region = text[start:end]
        for pattern, why in PATH_SOURCES:
            if not pattern.search(region):
                continue
            line = text.count("\n", 0, start) + 1
            found.append(
                f"{path}:{line}: {call}() is given a source path — {why}.\n"
                f"    A name is interned into the metadata table unredacted, so a path placed there "
                f"is beyond the writer's reach.\n"
                f"    Use cy::diag::register_source_location(file, line) and pass the LocationId; "
                f"the writer sanitises and classifies it.")
            break
    return found


def walk(roots: list[Path]) -> list[Path]:
    files: list[Path] = []
    for root in roots:
        if root.is_file():
            files.append(root)
            continue
        for path in sorted(root.rglob("*")):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            if any(part in SKIP_PARTS for part in path.parts):
                continue
            files.append(path)
    return files


SELF_TEST_SNIPPET = """
void offender() {
    const NameId site = register_name(__FILE__ ":" LINE);
    (void)site;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("roots", nargs="*", type=Path, help="directories or files to scan")
    parser.add_argument("--self-test", action="store_true",
                        help="prove the lint can fail: run it over a snippet that violates the rule")
    arguments = parser.parse_args()

    if arguments.self_test:
        found = findings_in(Path("<self-test>"), SELF_TEST_SNIPPET)
        if not found:
            print("lint_source_locations: SELF-TEST FAILED — the lint did not find the shape it "
                  "exists to find, so it would pass a tree that contains it.", file=sys.stderr)
            return 1
        print(f"lint_source_locations: self-test found the shape ({len(found)} finding)")
        return 0

    roots = arguments.roots or [Path("src")]
    findings: list[str] = []
    scanned = 0
    for path in walk(roots):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:  # unreadable is a finding about the tree, not a pass
            print(f"lint_source_locations: cannot read {path}: {error}", file=sys.stderr)
            return 1
        scanned += 1
        findings.extend(findings_in(path, text))

    if findings:
        for finding in findings:
            print(finding, file=sys.stderr)
        print(f"lint_source_locations: {len(findings)} violation(s) in {scanned} files",
              file=sys.stderr)
        return 1
    print(f"lint_source_locations: {scanned} files, no source location registered as a name")
    return 0


if __name__ == "__main__":
    sys.exit(main())

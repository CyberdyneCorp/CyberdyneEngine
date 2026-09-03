#!/usr/bin/env python3
"""The generator, the manifest and the identity gate, exercised as programs. Sections 1.1 and 1.2.

Most of what this milestone promises about identity is a *build failure*, and a build failure cannot
be asserted from inside the build it fails. So each check here writes a small annotated tree into a
temporary directory, runs tools/gen/reflect_gen.py over it, and reads what it did.

The one that matters is `test_a_renamed_field_fails_the_build`. `core-type-system` requires that an
accidental identity change be "a red build rather than a corrupted save months later", and design.md
§1 says to test it by renaming a field. That is exactly what happens below: a field is renamed, the
generator refuses and names the field, its identifier and the two fixes, and then both fixes are
applied and checked — the tombstone route, which retires the number, and the recorded-rename route,
which keeps it. Both are in the specification, and they answer different questions.

Run:  ctest -R integration.reflect_generator      (or)  python3 test_generator.py [name-fragment...]
"""

from __future__ import annotations

import argparse
import filecmp
import shutil
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]


class Failure(Exception):
    pass


def check(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def check_in(needle: str, haystack: str, message: str) -> None:
    if needle not in haystack:
        raise Failure(f"{message}\n  expected to find: {needle!r}\n  in:\n{_indent(haystack)}")


def _indent(text: str) -> str:
    return "".join(f"      {line}\n" for line in text.splitlines())


# --- The fixture tree ---------------------------------------------------------------------------

FIXTURE = """\
// A fixture for tools/gen/reflect_gen.py. Not part of the engine.
#pragma once

#include <cy/core/reflect/annotations.h>

namespace fx {{

struct CY_REFLECT_TYPE(Category("Fixture")) Widget {{
    CY_REFLECT_FIELD(Range(0.0, 10.0), Category("Size")) float {first} = 0.0F;
    CY_REFLECT_FIELD(Tooltip("how many")) unsigned int count = 0;
    CY_REFLECT_FIELD(Budget(priority = 3, label = "tiles", eager = true)) unsigned int budget = 0;
{extra}}};

}}  // namespace fx
"""


# A module attribute schema, so that the fixture exercises the typed custom-attribute path as well
# as the built-in table. Its parameters are deliberately not in alphabetical order: the incremental
# cache round-trips the parsed values through sorted JSON, and emitting them in dictionary order
# rather than in the schema's would reorder the initialiser between a cold run and a warm one.
SCHEMA = """\
[[attribute]]
name = "Budget"
struct = "BudgetAttribute"
namespace = "fx"

[[attribute.parameter]]
name = "priority"
type = "i32"

[[attribute.parameter]]
name = "label"
type = "string"

[[attribute.parameter]]
name = "eager"
type = "bool"
default = false
"""


class Tree:
    """One temporary annotated tree, and the generator runs over it."""

    def __init__(self, root: Path, source_root: Path) -> None:
        self.root = root
        self.source_root = source_root
        self.header = root / "include" / "fx" / "widget.h"
        self.manifest = root / "identity" / "manifest.toml"
        self.generated = root / "generated"
        self.cache = root / "cache.json"
        self.schema = root / "attributes.toml"
        self.header.parent.mkdir(parents=True, exist_ok=True)
        self.schema.write_text(SCHEMA, encoding="utf-8")
        self.write_fixture()

    def write_fixture(self, first: str = "size", extra: str = "") -> None:
        self.header.write_text(FIXTURE.format(first=first, extra=extra), encoding="utf-8")

    def run(self, *extra: str, output_dir: Path | None = None,
            cache: Path | None = None) -> subprocess.CompletedProcess:
        command = [
            sys.executable, str(self.source_root / "tools" / "gen" / "reflect_gen.py"),
            "--source-root", str(self.root),
            "--manifest", str(self.manifest),
            "--output-dir", str(output_dir or self.generated),
            "--include", str(self.source_root / "src" / "core" / "reflect" / "include"),
            "--include", str(self.root / "include"),
            "--attributes", str(self.schema),
            "--header", str(self.header),
            "--cache", str(cache or self.cache),
            *extra,
        ]
        return subprocess.run(command, capture_output=True, text=True, check=False)

    def succeed(self, *extra: str, **keywords) -> subprocess.CompletedProcess:
        result = self.run(*extra, **keywords)
        if result.returncode != 0:
            raise Failure(f"expected success, got exit {result.returncode}:\n"
                          f"{_indent(result.stdout + result.stderr)}")
        return result

    def fail(self, *extra: str, **keywords) -> str:
        result = self.run(*extra, **keywords)
        if result.returncode == 0:
            raise Failure(f"expected a failure, got success:\n{_indent(result.stdout)}")
        return result.stdout + result.stderr

    def identity(self) -> dict:
        if not self.manifest.exists():
            return {}  # nothing has been assigned, which is itself a result worth asserting
        return tomllib.loads(self.manifest.read_text(encoding="utf-8"))

    def field_id(self, type_name: str, field_name: str, status: str = "live") -> int | None:
        for entry in self.identity().get("type", []):
            if entry["name"] != type_name:
                continue
            for sub in entry.get("field", []):
                if sub["name"] == field_name and sub.get("status", "live") == status:
                    return sub["id"]
        return None


# --- Identity ------------------------------------------------------------------------------------


def test_identifiers_are_assigned_once_and_appended(tree: Tree) -> None:
    tree.succeed()
    first = tree.identity()
    check(first["next_type_id"] == 2, "one type should have taken one identifier")
    widget = first["type"][0]
    check(widget["id"] == 1, f"the first type gets identifier 1, not {widget['id']}")
    check([f["id"] for f in widget["field"]] == [1, 2, 3],
          "fields are numbered from one, in declaration order")

    # A second run over unchanged input changes nothing at all.
    tree.succeed()
    check(tree.identity() == first, "a re-run must not touch the manifest")

    # A field added later takes the next number; the ones already issued do not move.
    tree.write_fixture(extra='    CY_REFLECT_FIELD(Hidden) bool ready = false;\n')
    tree.succeed()
    check(tree.field_id("fx::Widget", "size") == 1, "an existing identifier moved")
    check(tree.field_id("fx::Widget", "ready") == 4, "a new field must take the next number")


def test_an_identifier_is_never_derived_from_a_name(tree: Tree) -> None:
    """Two trees, two different names, the same numbers: identity is a counter, not a hash."""
    tree.succeed()
    with tempfile.TemporaryDirectory() as other_root:
        other = Tree(Path(other_root), tree.source_root)
        other.write_fixture(first="a_completely_different_name")
        other.succeed()
        check(other.field_id("fx::Widget", "a_completely_different_name") == 1,
              "the first field of the first type is 1 whatever it is called")


def test_a_renamed_field_fails_the_build(tree: Tree) -> None:
    """The gate this milestone is for. design.md §1: prove it by renaming a field."""
    tree.succeed()
    original = tree.field_id("fx::Widget", "size")
    check(original == 1, "precondition: the field being renamed has FieldId 1")

    tree.write_fixture(first="magnitude")
    output = tree.fail()

    check_in("fx::Widget::size", output, "the failure must name the field")
    check_in("FieldId 1", output, "the failure must name the identifier")
    check_in("--rename", output, "the failure must offer the rename fix")
    check_in("--tombstone", output, "the failure must offer the removal fix")
    check(tree.field_id("fx::Widget", "magnitude") is None,
          "nothing may be assigned while the build is failing")


def test_a_tombstone_lets_the_build_through_and_retires_the_number(tree: Tree) -> None:
    """The removal answer: the old number is retired, the new field gets a fresh one."""
    tree.succeed()
    tree.write_fixture(first="magnitude")
    tree.fail()

    tree.succeed("--tombstone", "fx::Widget::size")
    check(tree.field_id("fx::Widget", "size", status="removed") == 1,
          "the removed field must remain in the manifest as a tombstone")
    check(tree.field_id("fx::Widget", "size") is None, "a tombstoned field is not live")
    fresh = tree.field_id("fx::Widget", "magnitude")
    check(fresh is not None and fresh != 1,
          f"a new field must not recycle a retired identifier (got {fresh})")

    # And the retired number stays retired across every later run.
    tree.write_fixture(first="magnitude", extra='    CY_REFLECT_FIELD() bool ready = false;\n')
    tree.succeed()
    check(tree.field_id("fx::Widget", "ready") not in (1, fresh),
          "every later field also avoids the retired number")


def test_a_recorded_rename_keeps_the_identifier(tree: Tree) -> None:
    """The rename answer, which is what `core-type-system` requires of a rename."""
    tree.succeed()
    tree.write_fixture(first="magnitude")
    tree.fail()

    tree.succeed("--rename", "fx::Widget::size=magnitude")
    check(tree.field_id("fx::Widget", "magnitude") == 1,
          "renaming a field must leave its FieldId alone")
    check(tree.field_id("fx::Widget", "size", status="removed") is None,
          "a rename is not a removal and writes no tombstone")


def test_a_renamed_type_keeps_its_identifier(tree: Tree) -> None:
    tree.succeed()
    tree.header.write_text(
        tree.header.read_text(encoding="utf-8").replace("Widget", "Gadget"), encoding="utf-8"
    )
    output = tree.fail()
    check_in("fx::Widget", output, "the failure must name the type")
    check_in("TypeId  1", output, "the failure must name the type's identifier")

    tree.succeed("--rename", "fx::Widget=fx::Gadget")
    entry = tree.identity()["type"][0]
    check(entry["name"] == "fx::Gadget" and entry["id"] == 1,
          "moving or renaming a type must not change its TypeId")


def test_the_gate_fails_when_an_identifier_moved(tree: Tree) -> None:
    """`core-type-system`: the gate diffs the manifest and names the previous identifier."""
    tree.succeed()
    baseline = tree.root / "baseline.toml"
    shutil.copyfile(tree.manifest, baseline)

    tree.succeed("--gate", "--baseline-manifest", str(baseline))

    # Somebody edits an identifier by hand, which is the accident the gate exists for.
    tree.manifest.write_text(
        tree.manifest.read_text(encoding="utf-8")
        .replace("next_type_id = 2", "next_type_id = 8")
        .replace('id = 1\nname = "fx::Widget"', 'id = 7\nname = "fx::Widget"'),
        encoding="utf-8",
    )
    output = tree.fail("--gate", "--baseline-manifest", str(baseline))
    check_in("fx::Widget", output, "the gate must name the entry")
    check_in("TypeId 1", output, "the gate must name the previous identifier")


def test_the_gate_rejects_a_recycled_number(tree: Tree) -> None:
    tree.succeed()
    text = tree.manifest.read_text(encoding="utf-8")
    recycled = text.replace('id = 2\nname = "count"', 'id = 1\nname = "count"')
    tree.manifest.write_text(recycled, encoding="utf-8")
    output = tree.fail("--gate")
    check_in("FieldId 1", output, "a duplicated identifier must be named")
    check_in("never reused", output, "the diagnostic must say why it is refused")


def test_the_gate_rejects_a_lowered_counter(tree: Tree) -> None:
    tree.succeed()
    tree.manifest.write_text(
        tree.manifest.read_text(encoding="utf-8").replace("next_type_id = 2", "next_type_id = 1"),
        encoding="utf-8",
    )
    output = tree.fail("--gate")
    check_in("next_type_id", output, "a counter below an issued identifier must be refused")


# --- Generation ------------------------------------------------------------------------------------


def test_output_is_reproducible_across_build_directories(tree: Tree) -> None:
    """Byte-identical output from two runs in two build directories — task 0.3, kept as a gate."""
    first = tree.root / "out-a"
    second = tree.root / "out-b"
    tree.succeed(output_dir=first, cache=tree.root / "cache-a.json")
    tree.succeed(output_dir=second, cache=tree.root / "cache-b.json")

    comparison = filecmp.dircmp(first, second)
    differences = _all_differences(comparison)
    check(not differences, f"generated output differs between runs: {differences}")

    for path in sorted(first.rglob("*.cpp")) + sorted(first.rglob("*.h")):
        text = path.read_text(encoding="utf-8")
        check(str(tree.root) not in text, f"{path.name} carries an absolute path")
        check(str(first) not in text, f"{path.name} carries its build directory")


def _all_differences(comparison) -> list[str]:
    found = list(comparison.diff_files) + list(comparison.left_only) + list(comparison.right_only)
    for sub in comparison.subdirs.values():
        found += _all_differences(sub)
    return found


def test_a_cached_parse_emits_what_a_cold_one_emits(tree: Tree) -> None:
    """A regression: the cache stores the parse as sorted JSON, and a custom attribute's arguments
    are emitted in the schema's declaration order rather than the dictionary's. Emitting the
    dictionary's order compiled as `{true, 2}` for `{priority, eager}` on the second run only —
    a narrowing error that a cold build never sees."""
    cache = tree.root / "shared-cache.json"
    tree.succeed(output_dir=tree.root / "cold", cache=cache)
    tree.succeed(output_dir=tree.root / "warm", cache=cache)

    differences = _all_differences(filecmp.dircmp(tree.root / "cold", tree.root / "warm"))
    check(not differences, f"a cached parse emitted something different: {differences}")

    emitted = (tree.root / "warm" / "fx" / "widget.reflect.cpp").read_text(encoding="utf-8")
    check_in('fx::BudgetAttribute custom_0_2_0_value{3, "tiles", true}', emitted,
             "a custom attribute's arguments must follow the schema's declaration order")


def test_check_detects_stale_output(tree: Tree) -> None:
    tree.succeed()
    tree.succeed("--check")

    generated = next(tree.generated.rglob("*.reflect.cpp"))
    generated.write_text("// stale\n", encoding="utf-8")
    output = tree.fail("--check")
    check_in("stale", output, "a stale generated file must be reported")
    check_in("just generate-headers", output, "the report must name the fix")


def test_check_refuses_to_assign_an_identifier(tree: Tree) -> None:
    output = tree.fail("--check")
    check_in("fx::Widget", output, "a declaration with no identifier must be named")
    check_in("generate-headers", output, "the report must name the fix")
    check(not tree.manifest.exists() or "fx::Widget" not in tree.manifest.read_text("utf-8"),
          "--check must not write to the manifest")


def test_the_cache_skips_an_unchanged_header(tree: Tree) -> None:
    tree.succeed()
    stamp = next(tree.generated.rglob("*.reflect.cpp")).stat().st_mtime_ns
    tree.header.touch()  # a new modification time, identical content
    tree.succeed()
    check(next(tree.generated.rglob("*.reflect.cpp")).stat().st_mtime_ns == stamp,
          "regenerating identical content must not rewrite the file")


# --- Attribute validation -----------------------------------------------------------------------------


def test_a_malformed_range_fails_naming_the_field(tree: Tree) -> None:
    tree.header.write_text(
        FIXTURE.format(first="size", extra="").replace("Range(0.0, 10.0)", "Range(10.0, 0.0)"),
        encoding="utf-8",
    )
    output = tree.fail()
    check_in("fx::Widget::size", output, "the failure must name the field")
    check_in("minimum", output, "the failure must say what is wrong with it")


def test_an_unknown_attribute_fails_naming_the_field(tree: Tree) -> None:
    tree.header.write_text(
        FIXTURE.format(first="size", extra="").replace("Tooltip(\"how many\")", "Sparkly"),
        encoding="utf-8",
    )
    output = tree.fail()
    check_in("fx::Widget::count", output, "the failure must name the field")
    check_in("unknown attribute 'Sparkly'", output, "the failure must name the attribute")


def test_an_unsupported_field_type_fails_naming_the_field(tree: Tree) -> None:
    tree.write_fixture(extra='    CY_REFLECT_FIELD() const char* label = nullptr;\n')
    output = tree.fail()
    check_in("fx::Widget::label", output, "the failure must name the field")
    check_in("cannot carry", output, "the failure must say why")


def test_an_attribute_that_belongs_on_a_field_is_refused_on_a_type(tree: Tree) -> None:
    tree.header.write_text(
        FIXTURE.format(first="size", extra="").replace(
            'CY_REFLECT_TYPE(Category("Fixture"))', "CY_REFLECT_TYPE(Transient)"
        ),
        encoding="utf-8",
    )
    output = tree.fail()
    check_in("fx::Widget", output, "the failure must name the type")
    check_in("cannot be written on a type", output, "the failure must say why")


def test_nothing_is_reflected_without_an_annotation(tree: Tree) -> None:
    tree.header.write_text(
        FIXTURE.format(first="size", extra="")
        .replace('CY_REFLECT_TYPE(Category("Fixture")) ', "")
        .replace("CY_REFLECT_FIELD(Range(0.0, 10.0), Category(\"Size\")) ", ""),
        encoding="utf-8",
    )
    tree.succeed()
    check(not tree.identity().get("type"), "an unannotated type must not be reflected")


# --- Harness ---------------------------------------------------------------------------------------


def collect() -> list:
    return [value for name, value in sorted(globals().items()) if name.startswith("test_")]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=REPO)
    parser.add_argument("filters", nargs="*")
    arguments = parser.parse_args(argv)

    selected = [
        test for test in collect()
        if not arguments.filters or any(f in test.__name__ for f in arguments.filters)
    ]
    failures = 0
    for test in selected:
        with tempfile.TemporaryDirectory(prefix="cy-reflect-") as workspace:
            tree = Tree(Path(workspace), arguments.source_root.resolve())
            try:
                test(tree)
            except Failure as failure:
                failures += 1
                print(f"FAIL  {test.__name__}\n{_indent(str(failure))}")
            except Exception as error:  # noqa: BLE001 - one broken check must not hide the rest
                failures += 1
                print(f"ERROR {test.__name__}\n{_indent(f'{type(error).__name__}: {error}')}")
            else:
                print(f"ok    {test.__name__}")
    print(f"\n{len(selected) - failures}/{len(selected)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

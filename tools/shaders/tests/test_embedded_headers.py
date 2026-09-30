#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The embedded-shader header check's own cases: it passes a current tree and refuses a stale one.

Every case but the last runs the real tool against a sandbox git repository whose "slangc" copies its
source to its output, so a header is a pure function of a `.slang` file and a stale header can be
made on purpose. The last case reads the real manifest, which needs no compiler: every header in
the tree must be declared, and every declared header and script must exist.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import embedded_headers  # noqa: E402

FAKE_SLANGC = """\
import sys
arguments = sys.argv[1:]
output = arguments[arguments.index("-o") + 1]
entry = arguments[arguments.index("-entry") + 1]
source = open(arguments[0], encoding="utf-8").read()
open(output, "w", encoding="utf-8").write(entry + ":" + source)
"""

EMBED = """\
import sys
body = "".join(open(path, encoding="utf-8").read() for path in sys.argv[2:])
open(sys.argv[1], "w", encoding="utf-8").write("// GENERATED\\n// " + body.strip() + "\\n")
"""

MANIFEST = """\
patterns = ["src/**/*_spirv.h"]

[[group]]
name = "probe"
headers = ["src/probe/probe_spirv.h"]
source = "src/probe/probe.slang"
compile = [["spirv", "compute", "main", "probe.spv"]]
run = [["{python}", "src/probe/embed.py", "src/probe/probe_spirv.h", "{scratch}/probe.spv"]]

[[excluded]]
header = "src/pinned/pinned_spirv.h"
reason = "pinned"
"""


def write(path: pathlib.Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class Sandbox:
    """A throwaway repository holding one group and one exclusion, committed and current."""

    def __init__(self, directory: pathlib.Path) -> None:
        self.root = directory / "tree"
        self.tools = directory / "tools"
        write(self.tools / "slangc.py", FAKE_SLANGC)
        write(self.tools / "noop_format.py", "")
        write(self.root / "tools/shaders/embedded_headers.toml", MANIFEST)
        write(self.root / "src/probe/probe.slang", "float probe;")
        write(self.root / "src/probe/embed.py", EMBED)
        write(self.root / "src/pinned/pinned_spirv.h", "// pinned\n")
        write(self.root / "src/probe/probe_spirv.h", "// GENERATED\n// main:float probe;\n")
        self.git("init", "-q")
        self.git("add", "-A")

    def git(self, *arguments: str) -> None:
        subprocess.run(["git", *arguments], cwd=self.root, check=True, capture_output=True)

    def slangc(self) -> str:
        wrapper = self.tools / "slangc"
        write(wrapper, f"#!/bin/sh\nexec {sys.executable} {self.tools / 'slangc.py'} \"$@\"\n")
        wrapper.chmod(0o755)
        return str(wrapper)

    def formatter(self) -> str:
        wrapper = self.tools / "clang-format"
        write(wrapper, "#!/bin/sh\nexit 0\n")
        wrapper.chmod(0o755)
        return str(wrapper)

    def check(self, *extra: str) -> tuple[int, str]:
        command = [sys.executable, str(HERE.parent / "embedded_headers.py"), "check",
                   "--slangc", self.slangc(), "--clang-format", self.formatter(),
                   "--root", str(self.root), *extra]
        result = subprocess.run(command, capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr


class CheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.sandbox = Sandbox(pathlib.Path(self.directory.name))

    def tearDown(self) -> None:
        self.directory.cleanup()

    def test_a_current_tree_passes(self) -> None:
        status, output = self.check()
        self.assertEqual(status, 0, output)

    def test_a_changed_source_makes_its_header_stale(self) -> None:
        write(self.sandbox.root / "src/probe/probe.slang", "float probe; float grown;")
        status, output = self.check()
        self.assertEqual(status, 1, output)
        self.assertIn("stale: src/probe/probe_spirv.h (group probe)", output)

    def test_a_hand_edited_header_is_stale(self) -> None:
        write(self.sandbox.root / "src/probe/probe_spirv.h", "// GENERATED\n// edited\n")
        status, output = self.check()
        self.assertEqual(status, 1, output)
        self.assertIn("stale: src/probe/probe_spirv.h", output)

    def test_the_check_does_not_write_the_tree(self) -> None:
        header = self.sandbox.root / "src/probe/probe_spirv.h"
        before = header.read_bytes()
        write(self.sandbox.root / "src/probe/probe.slang", "float changed;")
        self.check()
        self.assertEqual(header.read_bytes(), before)

    def test_an_undeclared_header_fails(self) -> None:
        write(self.sandbox.root / "src/other/other_spirv.h", "// GENERATED\n")
        status, output = self.check()
        self.assertEqual(status, 1, output)
        self.assertIn("not in tools/shaders/embedded_headers.toml: src/other/other_spirv.h", output)

    def test_a_declared_header_that_is_gone_fails(self) -> None:
        (self.sandbox.root / "src/pinned/pinned_spirv.h").unlink()
        status, output = self.check()
        self.assertEqual(status, 1, output)
        self.assertIn("declared but absent: src/pinned/pinned_spirv.h", output)

    def test_artifacts_hold_the_regenerated_text(self) -> None:
        write(self.sandbox.root / "src/probe/probe.slang", "float grown;")
        artifacts = pathlib.Path(self.directory.name) / "artifacts"
        status, _ = self.check("--artifacts", str(artifacts))
        self.assertEqual(status, 1)
        self.assertEqual((artifacts / "src/probe/probe_spirv.h").read_text(encoding="utf-8"),
                         "// GENERATED\n// main:float grown;\n")

    def check(self, *extra: str) -> tuple[int, str]:
        return self.sandbox.check(*extra)


class ManifestTest(unittest.TestCase):
    """The real manifest, read without a compiler."""

    root = HERE.parents[2]

    def setUp(self) -> None:
        self.manifest = embedded_headers.load_manifest(self.root)

    def test_every_header_in_the_tree_is_declared(self) -> None:
        self.assertEqual(embedded_headers.unlisted_headers(self.manifest, self.root), [])

    def test_every_declared_header_exists(self) -> None:
        self.assertEqual(embedded_headers.missing_headers(self.manifest, self.root), [])

    def test_every_script_and_source_a_group_names_exists(self) -> None:
        for group in self.manifest.groups:
            scripts = [argument for command in group.run for argument in command
                       if argument.endswith(".py")]
            sources = [row[4] if len(row) > 4 else group.source for row in group.compile]
            for path in scripts + [pathlib.Path(group.cwd) / source for source in sources]:
                self.assertTrue((self.root / path).is_file(), f"{group.name}: {path}")

    def test_group_names_are_unique(self) -> None:
        names = [group.name for group in self.manifest.groups]
        self.assertEqual(len(names), len(set(names)))


if __name__ == "__main__":
    unittest.main(verbosity=1)

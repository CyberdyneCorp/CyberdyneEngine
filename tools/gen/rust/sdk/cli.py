"""Write the SDK bindings, or fail naming what is stale. Task 2.3.

The write and the check are the SAME code path — the file set is built once, in memory, and then
either written or compared. `tools/gen/README.md` gives the reason for the whole directory: a
currency check that regenerated through a different path could disagree with the generator it
checks, and then nobody would know which of the two was right.
"""

from __future__ import annotations

import argparse
import difflib
import importlib.util
import pathlib
import re
import shutil
import subprocess
import sys

from . import emit

REPOSITORY = pathlib.Path(__file__).resolve().parents[4]
DEFAULT_HEADER = REPOSITORY / "src" / "abi" / "include" / "cy" / "abi" / "cy_abi.h"
DEFAULT_CRATE = REPOSITORY / "editor" / "crates" / "cy-editor-sdk"


def load_description(header: pathlib.Path) -> dict:
    """Run the ABI description generator in process.

    Imported by path rather than as a package: `tools/abi/` is a directory of programs, not an
    installed module, and the alternative — shelling out and parsing JSON back — would put the
    generator's correctness at the mercy of a subprocess's working directory.
    """
    location = REPOSITORY / "tools" / "abi" / "abi_describe.py"
    spec = importlib.util.spec_from_file_location("cy_abi_describe", location)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load the ABI description generator at {location}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.describe(header)


# The formatter the generated files are written through, and the reason they are.
#
# WHY THE GENERATOR FORMATS ITS OWN OUTPUT. `rustfmt` formats a crate by walking its module tree
# from the crate root, and there is no stable way to tell it to skip a module — `--skip-children`
# is not a recognised option on a stable toolchain. So a repository that runs `rustfmt --check` over
# the editor and also checks that the generated bindings are current has two gates that disagree by
# construction: the formatter rewrites the generated files, and the currency check then reports them
# as stale.
#
# Both are gates worth having, so the generator writes what the formatter would produce. The
# committed bindings are rustfmt's output, `--check` compares against rustfmt's output, and the two
# agree because there is one pipeline.
#
# THE COST, STATED. Generation now depends on `rustfmt`, and on its version: a different rustfmt
# would produce different bytes and the currency check would report the bindings as stale, with the
# diff saying so. That is the same shape as the clang-format pin `justfile` documents at length, and
# the same remedy — one version everywhere. `rustfmt` ships with every Rust toolchain, so the
# dependency costs nothing to satisfy; what it costs is a pin, and this repository does not have one
# for Rust yet. See tools/gen/rust/README.md.
RUSTFMT_EDITION = "2024"


def format_rust(text: str, name: str) -> str:
    """Return `text` as `rustfmt` would write it.

    Fails loudly rather than silently emitting unformatted Rust: a generator that quietly skipped
    formatting on a machine without `rustfmt` would write bindings that the formatting gate then
    rejects, which is a worse failure than not generating at all because it happens somewhere else.
    """
    formatter = shutil.which("rustfmt")
    if formatter is None:
        raise RuntimeError(
            "rustfmt is not on PATH, and the generated bindings are written through it so that the "
            "formatting gate and the currency gate agree.\n"
            "  It ships with every Rust toolchain: `rustup component add rustfmt`.")
    result = subprocess.run(
        [formatter, "--edition", RUSTFMT_EDITION, "--emit", "stdout"],
        input=text, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"rustfmt refused the generated {name}:\n{result.stderr}")
    return result.stdout


def tidy(text: str) -> str:
    """One blank line between sections, exactly one newline at the end.

    The emitters compose a file out of independently written blocks, and a block that begins with
    its own blank line then meets a separator that supplies another. Collapsing runs here rather
    than trimming every block keeps each emitter readable and keeps the output stable — which is
    what `--check` compares, so it has to be a function of the description and nothing else.
    """
    return re.sub(r"\n{3,}", "\n\n", text).rstrip("\n") + "\n"


def outputs(description: dict) -> dict[str, str]:
    """Every generated file, keyed by its path relative to the SDK crate root."""
    files = {
        "src/generated/mod.rs": emit.module(description),
        "src/generated/abi.rs": emit.abi_version(description),
        "src/generated/enums.rs": emit.enums(description),
        "src/generated/ffi.rs": emit.ffi(description),
        "src/generated/interface.rs": emit.interface(description),
        "src/generated/layout.rs": emit.layout(description),
    }
    return {name: format_rust(tidy(text), name) for name, text in files.items()}


def _write(crate: pathlib.Path, files: dict[str, str], quiet: bool) -> int:
    for name, text in sorted(files.items()):
        target = crate / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists() and target.read_text() == text:
            continue
        target.write_text(text)
        if not quiet:
            print(f"    wrote {name}")
    if not quiet:
        print(f"==> rust sdk    {len(files)} file(s) in {crate}")
    return 0


def _check(crate: pathlib.Path, files: dict[str, str]) -> int:
    stale: list[str] = []
    for name, text in sorted(files.items()):
        target = crate / name
        current = target.read_text() if target.exists() else ""
        if current == text:
            continue
        stale.append(name)
        diff = difflib.unified_diff(current.splitlines(True), text.splitlines(True),
                                    fromfile=f"committed/{name}", tofile=f"generated/{name}")
        sys.stderr.writelines(diff)
    if not stale:
        print(f"==> rust sdk    {len(files)} file(s) are current")
        return 0
    print(f"\nthe committed Rust SDK bindings are stale: {', '.join(stale)}", file=sys.stderr)
    print("  Regenerate and commit the result:", file=sys.stderr)
    print("      just build-editor --generate", file=sys.stderr)
    return 1


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Generate the editor's Rust SDK bindings from the C ABI description.")
    parser.add_argument("--header", type=pathlib.Path, default=DEFAULT_HEADER)
    parser.add_argument("--crate", type=pathlib.Path, default=DEFAULT_CRATE,
                        help="the SDK crate root (default: editor/crates/cy-editor-sdk)")
    parser.add_argument("--check", action="store_true",
                        help="fail if the committed bindings are not what regeneration produces")
    parser.add_argument("--quiet", action="store_true")
    arguments = parser.parse_args(argv)

    description = load_description(arguments.header)
    files = outputs(description)
    if arguments.check:
        return _check(arguments.crate, files)
    return _write(arguments.crate, files, arguments.quiet)

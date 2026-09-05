"""Write the overlay, or fail naming what is stale. Task 3.1.

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
import sys

from . import emit

REPOSITORY = pathlib.Path(__file__).resolve().parents[4]
DEFAULT_HEADER = REPOSITORY / "src" / "abi" / "include" / "cy" / "abi" / "cy_abi.h"
DEFAULT_PACKAGE = REPOSITORY / "bindings" / "swift"

# The C header is copied into the Swift package rather than reached with an include path outside it.
# A Swift package is a self-contained unit — `swift build` in bindings/swift must work in a checkout
# with no CMake tree and no configured build directory, and SwiftPM refuses a header search path
# that leaves the package. The copy is generated, so `--check` catches an edit to it exactly as it
# catches an edit to the Swift; it cannot drift from src/abi/ any more than the overlay can.
HEADER_BANNER = """\
/* GENERATED COPY — DO NOT EDIT.
 *
 * `tools/gen/swift/overlay_gen.py` copies src/abi/include/cy/abi/cy_abi.h here so that the Swift
 * package is self-contained: `swift build` in bindings/swift works in a checkout with no configured
 * CMake tree, and SwiftPM will not accept a header search path that leaves the package.
 *
 * Edit the original. `just generate-swift --check` fails when this copy is stale, which is the same
 * gate that catches a stale overlay.
 */
"""

MODULE_MAP = """\
// GENERATED FILE — DO NOT EDIT. See tools/gen/swift/overlay_gen.py.
//
// Written out rather than left to SwiftPM's inference, so that what `import CyberdyneABI` brings in
// is stated in one place: exactly the C ABI header, and nothing that happens to be beside it.
module CyberdyneABI {
    header "cy/abi/cy_abi.h"
    export *
}
"""


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


def outputs(description: dict, header: pathlib.Path) -> dict[str, str]:
    """Every generated file, keyed by its path relative to the Swift package root."""
    return {
        "Sources/CyberdyneABI/include/cy/abi/cy_abi.h": HEADER_BANNER + header.read_text(),
        "Sources/CyberdyneABI/include/module.modulemap": MODULE_MAP,
        "Sources/CyberdyneCore/Generated/ABI.swift": emit.abi_version(description),
        "Sources/CyberdyneCore/Generated/Enums.swift": emit.enums(description),
        "Sources/CyberdyneCore/Generated/Math.swift": emit.math(description),
        "Sources/CyberdyneCore/Generated/Interface.swift": emit.interface(description),
        "Sources/CyberdyneCore/Generated/Handles.swift": emit.handles(description),
        "Tests/CyberdyneCoreTests/Generated/LayoutTests.swift": emit.layout_tests(description),
    }


def _write(package: pathlib.Path, files: dict[str, str], quiet: bool) -> int:
    for name, text in sorted(files.items()):
        target = package / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists() and target.read_text() == text:
            continue
        target.write_text(text)
        if not quiet:
            print(f"    wrote {name}")
    if not quiet:
        print(f"==> swift overlay  {len(files)} file(s) in {package}")
    return 0


def _check(package: pathlib.Path, files: dict[str, str]) -> int:
    stale: list[str] = []
    for name, text in sorted(files.items()):
        target = package / name
        current = target.read_text() if target.exists() else ""
        if current == text:
            continue
        stale.append(name)
        diff = difflib.unified_diff(current.splitlines(True), text.splitlines(True),
                                    fromfile=f"committed/{name}", tofile=f"generated/{name}")
        sys.stderr.writelines(diff)
    if not stale:
        print(f"==> swift overlay  {len(files)} file(s) are current")
        return 0
    print(f"\nthe committed Swift overlay is stale: {', '.join(stale)}", file=sys.stderr)
    print("  Regenerate and commit the result:", file=sys.stderr)
    print("      just generate-swift", file=sys.stderr)
    return 1


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Generate the CyberdyneKit Swift overlay from the C ABI description.")
    parser.add_argument("--header", type=pathlib.Path, default=DEFAULT_HEADER)
    parser.add_argument("--package", type=pathlib.Path, default=DEFAULT_PACKAGE,
                        help="the Swift package root (default: bindings/swift)")
    parser.add_argument("--check", action="store_true",
                        help="fail if the committed overlay is not what regeneration produces")
    parser.add_argument("--quiet", action="store_true")
    arguments = parser.parse_args(argv)

    description = load_description(arguments.header)
    files = outputs(description, arguments.header)
    if arguments.check:
        return _check(arguments.package, files)
    return _write(arguments.package, files, arguments.quiet)

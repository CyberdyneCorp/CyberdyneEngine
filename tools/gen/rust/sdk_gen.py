#!/usr/bin/env python3
"""Generate the editor's Rust SDK bindings from the C ABI description. Task 2.3.

    python3 tools/gen/rust/sdk_gen.py [--check] [--crate DIR] [--header PATH]

A thin entry point over the `sdk/` package, the way `overlay_gen.py` is over `overlay/` and
`reflect_gen.py` is over `reflect/`. The reasoning lives in the package's module docstrings; this
file exists so that the recipe and the tests have one command to name.
"""

from __future__ import annotations

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from sdk import cli  # noqa: E402  (the path above has to be set first)

if __name__ == "__main__":
    sys.exit(cli.main(sys.argv[1:]))

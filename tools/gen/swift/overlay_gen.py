#!/usr/bin/env python3
"""Generate the CyberdyneKit Swift overlay from the C ABI description. Task 3.1.

    python3 tools/gen/swift/overlay_gen.py [--check] [--package DIR] [--header PATH]

A thin entry point over the `overlay/` package, the way `reflect_gen.py` is over `reflect/`. The
reasoning lives in the package's module docstrings; this file exists so that the recipe and the
tests have one command to name.
"""

from __future__ import annotations

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from overlay import cli  # noqa: E402  (the path above has to be set first)

if __name__ == "__main__":
    sys.exit(cli.main(sys.argv[1:]))

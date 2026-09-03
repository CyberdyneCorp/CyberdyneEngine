#!/usr/bin/env python3
"""The reflection generator — task 1.1.2. See tools/gen/reflect/ for the implementation.

Governed by `core-type-system`: reflection metadata is produced by a generator run as a build step,
parsed with a real C++ frontend rather than a text scanner, and every identifier it emits is read
from the committed identity manifest rather than derived from a name.

    reflect_gen.py --source-root . --manifest identity/manifest.toml \
                   --output-dir src/core/reflect/generated \
                   --include src/core/base/include --include src/core/reflect/include \
                   --header src/core/reflect/include/cy/core/reflect/demo/types.h

Run it through `just generate-headers`, `just generate-check` or `just quality-identity` rather than
by hand; those recipes are what continuous integration invokes.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from reflect.cli import main  # noqa: E402 - the path has to be set before the package is importable

if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

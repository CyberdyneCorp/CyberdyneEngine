"""The Swift overlay generator. Task 3.1, `swift-scripting`.

`swift-scripting`: "The overlay in `CyberdyneCore` SHALL be **generated** from the same ABI
description that produces the C headers, so the two cannot drift."

The description is the one `tools/abi/abi_describe.py` produces and `tools/abi/abi_gate.py` diffs
against `src/abi/abi_baseline.json`. There is exactly one reader of the C header in this repository
and it is that parser; this package consumes its output and never opens `cy_abi.h` itself, except to
copy it verbatim into the package's C target.

WHY GENERATED, IN THE PROJECT'S OWN WORDS. `core-type-system` already carries the argument for the
reflection generator: a declaration that can drift from the thing it describes will. Hand-writing
the overlay would reproduce that drift in a second language, and the failure mode is worse here than
in C++ — a Swift `struct` whose member order silently disagrees with the C struct it is passed as
does not fail to compile, it reads the wrong bytes.
"""

# `src/abi/` — layer 6

The stable flat C ABI exported to scripting languages and extensions. Symbols are `cy_*`; the ABI is
the boundary at which C++ types stop.

**What belongs here**: the export table, the C headers generated from the ABI definition, and the
machine-readable ABI description that the Swift overlay and the Rust SDK are generated from.

**What does not belong here**: anything that assumes the caller is C++, and any function that can
throw — the engine is built `-fno-exceptions`, and the ABI reports failure by value.

**Governed by**: `native-abi`. Arrives at M4.

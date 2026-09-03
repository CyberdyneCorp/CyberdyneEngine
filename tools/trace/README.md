# `tools/trace/`

Readers for the two artefacts `src/core/diagnostics/` writes. Layer 7; they depend on nothing in the
engine, because their whole purpose is to be usable where the engine is not.

| Tool | Reads | Recipe |
|---|---|---|
| `trace_inspect.py` | a `.cytrace` capture | `just diagnose-trace <file>` |
| `crash_inspect.py` | a crash report | `just diagnose-crash [file]` |

**Why Python, and why a second implementation.** `diagnostics-profiling-and-crash` requires that a
capture is "openable without the game runtime" and that its identifiers "resolve to names from the
capture's metadata". A reader that shares code with the writer proves neither. These parse
`src/core/diagnostics/include/cy/core/diagnostics/format.h` independently, as does the reader the
diagnostics tests use, so a format change that breaks one breaks all three loudly.

`trace_inspect.py` reads the chunk index at the file's tail and loads only the chunks it needs — the
"a long capture opens quickly" scenario — and falls back to a forward scan for a capture a crash
truncated before its index was written.

**Governed by**: `diagnostics-profiling-and-crash`, `developer-workflow-and-just`.

# `src/core/diagnostics/` — layer 0

One trace, one timeline, one clock. Every subsystem the engine will ever have emits into what is
declared here, and the reason it is in M0 rather than M4 is that a diagnostic field's **privacy
classification** cannot be added retroactively — it would be an audit of every field ever written,
performed by someone who did not write them.

**Governed by**: `diagnostics-profiling-and-crash`. Decisions: `design.md` §2. Tasks: 3.5.1–3.5.9.

## The invariant

A field's classification is a required argument of the macro that declares it. There is no overload
that omits it, no default, and no second way to obtain a `FieldId`.

```cpp
CY_TRACE_FIELD(frame_index, u64,    cy::Privacy::Public)
CY_TRACE_FIELD(user_path,   string, cy::Privacy::Sensitive)
```

- Two arguments is `error: macro "CY_TRACE_FIELD" requires 3 arguments, but only 2 given`.
- A classification computed at run time does not compile: `require_classification()` is `consteval`.
- A field type that does not exist names itself: `CY_DIAG_FIELD_TYPE_uint64` is undefined.
- An id that was never registered carries no classification, so the writer redacts it and counts it.

`tests/test_field_macro.py` compiles four declarations and requires exactly one of them to succeed.

**Redaction is the writer's**, not the producer's. A producer says what a value *is* by declaring its
field; the writer decides what may be written by comparing that declaration against the artefact's
declared ceiling. A capture carries the ceiling it was written under, and a redacted field keeps its
entry and loses its value, so the gap is visible rather than silently misleading. No policy admits
`Secret` at any ceiling: credentials, tokens, private communications and personal files have no path
into any artefact. `ExportPolicy` can be tightened and cannot be widened.

## What is here

| Header | What it declares |
|---|---|
| `privacy.h` | `cy::Privacy`, `cy::ExportPolicy` |
| `field.h` | compiled identifiers: `CY_TRACE_FIELD`, `CY_TRACE_NAME`, `CY_TRACE_CATEGORY`, the registry |
| `trace.h` | the timeline: event kinds, channels, emission, lifecycle, `EmissionCost` |
| `log.h` | `CY_LOG`, levels, category floors — records on the same timeline |
| `breadcrumb.h` | the bounded ring that survives when the trace does not |
| `crash.h` | the crash artefact and the handler that writes it |
| `bridge.h` | the two seams `src/core/base/` declares, filled in by this module |
| `format.h` | the capture's wire format, shared with `tools/trace/trace_inspect.py` |

The emission path, in order: one relaxed load of "is a trace open", one thread-local pointer, one
monotonic clock read, one bounds check in the producer's own ring, and a `memcpy` of a record it
composed on its stack. No allocation, no string formatting, no hashing, no lock, no call into
another subsystem. A producer refused by the loss policy increments one relaxed counter and returns.

**Loss is recorded, never silent.** Per-thread rings, and a channel is admitted only while the buffer
is below its share: critical 100%, important 85%, verbose 50%, sampled 25%. What is refused is
counted per channel, emitted as a `Loss` record on the timeline where the gap is, and totalled in the
artefact's `LOSS` chunk. The same chunk carries the fields the export policy removed and the
registrations the fixed-capacity metadata table refused.

## Overhead — measured, not claimed

`diagnostics-profiling-and-crash` requires overhead to be bounded and declared. These are from
`cy_diag_overhead`, 400 000 samples per measurement, Linux, gcc 13.3.0, `--profile dev`, on the
machine M0 was implemented on. Re-run it rather than trusting the table:

```
build/<dir>/src/core/diagnostics/tests/cy_diag_overhead 400000 [trace-path]
```

| State | ns per instant | +2 fields | scope pair |
|---|---:|---:|---:|
| compiled in, no trace open | 4–5 | 4–5 | 5–9 |
| log record below the level floor | 0.6 | | |
| open, recording, artefact to `/dev/null` | 30–37 | 32–39 | 60–73 |
| open, recording, artefact to disk (77 MB written) | ~43 | ~46 | ~85 |
| open, background consumer draining | 32–43 | 34–46 | 64–90 |
| `CY_PROFILING=ON`, Tracy republishing every record | 95–320 | 85–160 | 200+ |

Against a 16.67 ms frame:

- **shipping, minimal telemetry** — 200 events a frame: 6–9 µs, **0.04–0.05% of the frame**. The
  requirement is "well under one per cent".
- **development, normal tracing** — 5 000 events a frame: 150–215 µs, **0.9–1.3%**. The requirement
  is "a small single-digit percentage".
- **full instrumentation** — Tracy on: explicitly higher, which is what the requirement allows.

The dominant term in the recording figures is the monotonic clock read, not the buffer write.
`measure_emission_cost()` is public, so the engine can report the cost of its own diagnostics.

## Artefacts

A capture is `CYTRACE\0`, then chunks — `META` identifier tables and build identity, `EVTS` one
thread's records, `LOSS`, `ENDX` the chunk index — and the file's last eight bytes are the index's
own offset, so a long capture opens by reading its index and loading regions on demand. `META` is
written at open as well as at close, so a capture a crash truncated still resolves what was
registered before it started. Compression is `None` at M0; the chunk header already carries both
lengths, so turning on zstd changes the writer and the readers, not the format.

A crash report is text, written by a path that assumes the process is damaged: no allocation, no
lock, no variadic formatter, no subsystem re-entry. The path, the identity strings and the report
buffer are prepared when the handler is installed. It carries the build identity, the declared
classification ceiling, the signal or exception, the last frame the process reached, the breadcrumb
ring, and the backtrace with module identities and offsets — symbol-independent, symbolicated later
against the archived symbols. A report already written by a fatal assertion is not overwritten by the
`SIGABRT` that assertion raised.

```
just diagnose-trace <capture> [--events N] [--kind counter] [--json]
just diagnose-crash [report] [--directory <dir>] [--symbolicate]
```

## Seams, and what is deliberately not here

- **`src/core/base/`** owns the type aliases, `cy::Expected<T, Error>`, the assertion macros and
  their behaviour per configuration. `trace_open()` installs this module into base's assertion
  handler and its diagnostic sink; `trace_close()` restores them. A warning a layer above emits
  becomes a classified record on this timeline instead of a line on standard error.
- **Tracy** is a backend of this trace, behind `CY_PROFILING`, never a second timeline. With the
  option off there is no Tracy target, the sink compiles to nothing, and the trace is complete.
- **The crash handler's operating-system half** is two translation units selected by the build, not
  an `#ifdef` in a shared file. When `Platform::install_crash_handler()` lands (task 3.2.1), this
  becomes its implementation. The Windows unit is written and **unverified** — M0 was implemented on
  Linux and CI does not exist yet.
- **Not here, and named in the specification**: the health model, the always-on rolling buffer and
  automatic capture, profiler views, remote and dedicated-server transport, telemetry export,
  reproduction artefacts, graphics-device and shader diagnostics. Each needs a subsystem that does
  not exist yet — a renderer, a job system, a replay — and each lands on this transport rather than
  beside it. M0 is the transport, the classification, the loss policy and the artefacts.

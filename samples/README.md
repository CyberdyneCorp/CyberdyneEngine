# `samples/`

Small, complete programs that exercise the engine through its public surface. Each milestone's
closing artefact is a sample, which is what keeps the milestone gate executable rather than
narrative.

| Sample | Milestone | What it proves |
|---|---|---|
| `00-empty/` | M0 | Opens a window, runs an empty loop through `Runtime::tick()`, writes a trace, exits cleanly |

Run one with `just run-sample <name>`; `--headless` runs it under the headless display server, which
is how the smoke test runs it in CI.

```
just run-sample empty                      a window on the desktop, until you close it
just run-sample empty --headless           no window system at all
just run-sample empty --frames 120         stop after 120 frames — what makes it testable
just run-sample empty --trace /tmp/a.cytrace
just run-headless --frames 120             the same thing, spelled as CI runs it
just diagnose-trace <path>                 read the trace it wrote, without linking the engine
```

With no `--trace`, the capture is written to the platform's user data directory — the only place the
engine may write to — and the sample prints the path it chose.

## What a sample looks like

`00-empty` is deliberately the first thing a contributor reads. Four objects and one loop: the
host — `main.cpp` — owns the platform, the display server, the runtime and the loop; the runtime
owns the frame. Nothing owns the other direction, which is what lets a platform that drives frames
itself replace `run_host_loop()` and change nothing else (`design.md` §3).

A run ends in one of three ways, and all three end the same way — `Platform::request_exit()` records
the intent, the loop observes it, `main()` returns: the window is closed, the process is interrupted
(`SIGINT`, `SIGTERM`), or the frame limit is reached.

## Adding one

A directory, a `cy_add_module(NAME cy_sample_<name> LAYER tools TYPE EXECUTABLE ...)`, and one
`add_subdirectory()` line in `samples/CMakeLists.txt`. `just run-sample` finds the binary as
`samples/*/cy_sample_<name>`, so there is no manifest to keep in step. Samples sit at the top of the
layer stack: they consume the whole engine and nothing consumes them.

**Governed by**: `delivery-roadmap` (milestone artefacts), `testing-and-quality` (smoke tests).

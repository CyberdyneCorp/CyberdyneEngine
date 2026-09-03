# just/diagnose.just — Diagnose: trace inspection, crash artefacts, log collection.
#
# A developer reporting a defect should be able to produce a complete reproduction bundle with one
# command. Everything in this category serves that.
#
# Both recipes read an artefact with a tool that does not link the engine, because
# `diagnostics-profiling-and-crash` requires a capture to be readable without the game runtime and a
# crash report to be usable on a machine with no symbols. Neither builds anything.

# Inspect a trace file written by the engine. Pass --events N to list events, --json for a machine.
diagnose-trace *args:
    @cd "{{root}}" && python3 tools/trace/trace_inspect.py {{args}}

# Read a crash report; with no argument, the newest in the user mount's crash directory.
diagnose-crash *args:
    @cd "{{root}}" && python3 tools/trace/crash_inspect.py {{args}}

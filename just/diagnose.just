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

# Inspect a save directory. `show <dir> [--why]` prints what is in it and why each field is there;
# `diff <dir>[@n] <dir>[@n]` the semantic difference between two generations. Unlike the two above
# it links the engine: a save is read through the same loader a game uses, so the inspector cannot
# disagree with it about what the save contains. Build it with `just build-engine` first.
diagnose-save *args:
    @cd "{{root}}" && "${CY_BUILD_DIR:-build/{{default_profile}}}/tools/save/cy_save_inspect" {{args}}

# The ten forbidden save patterns, checked: the static half over the tree, the runtime half through
# the `forbidden save pattern` cases in unit.save, and the checker's own selftest.
diagnose-save-patterns:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    python3 tools/save/selftest.py
    python3 tools/save/check_forbidden.py --binary "${CY_BUILD_DIR:-build/{{default_profile}}}/cy_test_unit_save"

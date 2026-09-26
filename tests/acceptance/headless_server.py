#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""`smoke.acceptance_headless_server` — run the dedicated server, then observe it from outside.

M11.d task 7.2. The property this scenario is named for is OBSERVABILITY with nothing that draws
present: `diagnostics-profiling-and-crash` — "WHEN a dedicated server runs THEN its diagnostics
SHALL be available with no rendering or interface code present". So this driver does not trust the
server's own summary. It reads the trace the server wrote with `tools/trace/trace_inspect.py` — a
reader that does not link the engine — and requires every counter a dedicated server owes, at every
tick, with values that mean the simulation actually ran:

    server.tick_rate_hz, server.tick_work_us, server.overruns, server.entities,
    server.memory_live_bytes, ai.thought, ai.starved, commands.committed, commands.refused,
    physics.bodies, physics.active_bodies, server.ai_us / commands_us / physics_us (where the tick
    went), and a state hash per tick for divergence.

The server is run PACED at its fixed rate, for two seconds of simulation, over 100 000 entities.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COUNTERS = ("server.tick_rate_hz", "server.tick_work_us", "server.overruns", "server.entities",
            "server.memory_live_bytes", "ai.thought", "ai.starved", "commands.committed",
            "commands.refused", "physics.bodies", "physics.active_bodies", "server.ai_us",
            "server.commands_us", "server.physics_us")
TICKS = 60
RATE = 30
ENTITIES = 100_000


def fail(message: str) -> int:
    print(f"smoke.acceptance_headless_server: FAILED: {message}", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    arguments = parser.parse_args()
    arguments.work.mkdir(parents=True, exist_ok=True)
    trace = arguments.work / "headless-server.cytrace"
    trace.unlink(missing_ok=True)

    ran = subprocess.run([str(arguments.server), "--entities", str(ENTITIES), "--ticks",
                          str(TICKS), "--rate", str(RATE), "--trace", str(trace)],
                         capture_output=True, text=True, timeout=240)
    print(ran.stdout, end="")
    if ran.returncode != 0:
        return fail(f"the server exited {ran.returncode}:\n{ran.stderr}")
    if not trace.is_file():
        return fail(f"the server wrote no trace at {trace}")

    inspected = subprocess.run([sys.executable, str(ROOT / "tools" / "trace" / "trace_inspect.py"),
                                str(trace), "--events", "1000000", "--json"],
                               capture_output=True, text=True, timeout=120)
    if inspected.returncode != 0:
        return fail(f"the trace could not be read without the engine:\n{inspected.stderr}")
    capture = json.loads(inspected.stdout)
    events = capture.get("events", [])

    ticks = [event for event in events if event.get("kind") == "tick-end"]
    hashes = [event for event in events if event.get("kind") == "state-hash"]
    if len(ticks) != TICKS:
        return fail(f"the trace carries {len(ticks)} tick(s), not the {TICKS} the server ran")
    # THE FIXED RATE IS A FIXED STEP: the ticks the trace carries are 1..N with none skipped or
    # repeated, whatever the wall clock did. The achieved wall rate and the overruns are REPORTED —
    # they are observability, and a loaded machine moves them — not asserted.
    if [event.get("a") for event in ticks] != list(range(1, TICKS + 1)):
        return fail("the trace's ticks are not one fixed step each, in order")
    if len(hashes) != TICKS:
        return fail(f"the trace carries {len(hashes)} state hash(es) for {TICKS} ticks")

    counters: dict[str, list[int]] = {}
    for event in events:
        if event.get("kind") == "counter":
            counters.setdefault(event.get("name", ""), []).append(int(event.get("a", 0)))
    missing = [name for name in COUNTERS if len(counters.get(name, [])) != TICKS]
    if missing:
        return fail("counters absent or not written every tick: " + ", ".join(
            f"{name} ({len(counters.get(name, []))})" for name in missing))

    # Values that mean it ran — a trace of zeros is a trace of nothing.
    checks = {
        "every tick reports 100 000 entities": all(v == ENTITIES for v in counters["server.entities"]),
        "every tick reports the bodies in the world":
            all(v >= ENTITIES for v in counters["physics.bodies"]),
        "the AI thought on every tick": all(v > 0 for v in counters["ai.thought"]),
        "no agent starved": all(v == 0 for v in counters["ai.starved"]),
        "commands were committed": sum(counters["commands.committed"]) > 0,
        "no command was refused": all(v == 0 for v in counters["commands.refused"]),
        "bodies became active as orders arrived": max(counters["physics.active_bodies"]) > 0,
        "memory in use is reported": all(v > 0 for v in counters["server.memory_live_bytes"]),
        "the achieved tick rate is reported": counters["server.tick_rate_hz"][-1] > 0,
    }
    failed = [name for name, held in checks.items() if not held]
    if failed:
        return fail("; ".join(failed))
    for name in checks:
        print(f"  ok   {name}")
    print(f"smoke.acceptance_headless_server: {len(events)} events read back without the engine; "
          f"achieved {counters['server.tick_rate_hz'][-1]} Hz against {RATE} Hz fixed, "
          f"{counters['server.overruns'][-1]} overrun(s), worst tick "
          f"{max(counters['server.tick_work_us'])} us")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""`cy_quiet_host`, run against a host this test makes busy on purpose.

A ledger criterion that runs a timing-sensitive suite runs it through `cy_quiet_host -- <command>`
and says so: when the host is busy the run FAILS with a "host too busy:" reason, and it neither
passes nor skips. This test is what goes red if that check is removed or stops looking.

  * LEG 1, BEFORE THE COMMAND. Spinners are started first, then the wrapper around a command that
    writes a marker file. The wrapper has to refuse to run it: exit 1, "host too busy:" and "was
    not run" on stderr, and no marker file. Removing the pre-run check makes the command run, and
    this leg goes red. Judged on every host: a busy host is exactly what it needs.
  * LEG 2, ACROSS THE RUN. The wrapper starts on whatever host there is around a command that
    sleeps. Once it prints that the run has started, the spinners start. The check across the run
    has to fail with "was not quiet while". Removing that check makes the run exit 0, and this leg
    goes red. It needs the pre-run check to PASS first, which needs a quiet host; on a busy one it
    is reported NOT EVALUATED with the reason, and the exit code says so (3).
  * LEG 3, THE COMMAND'S OWN LOAD. The wrapped command itself burns four cores for two seconds —
    twice the two cores the check allows everyone else. Its own process tree is subtracted, so on a
    quiet host the run passes and the command's exit status (7, chosen) is the wrapper's. A wrapper
    that counted the command's own children against the host would fail this leg. It needs a
    quiet host, and exits 3 on a busy one — including a host that turned busy during the run and
    is busy still, which this leg cannot tell from its own children being counted.
  * LEG 4, A NESTED SESSION. The same four-core command, but each worker calls `setsid` first,
    which is what this wrapper itself does to a command it runs — so this is the shape of the
    wrapper inside a suite it wraps (`just test-all` runs leg 3). A census by session id alone
    counted those workers against the host and failed `m0:test` on an idle machine; the census
    follows the parent chain too, and this leg is what goes red if it stops. Judged as leg 3 is.

  * LEG 5, I/O PRESSURE BEFORE THE COMMAND. A writer that fsyncs 4 MiB at a time into a bounded
    file, in pulses of two seconds on and three off, is started first — the shape of the eighth
    close's writeback burst, which read `full 70%` with no compiler running and was judged quiet by
    a check that read CPU alone. The wrapper has to refuse the command with "host too busy: io" (the
    I/O clause leads when it decided the verdict). Removing the I/O check makes it run, and so does
    settling on one quiet second instead of five, since every pause holds one; either way this
    half goes red. Judged on every host.
    Then the writer is killed by its PID, and the same wrapper around the same command must PASS
    once its fsyncs are over: a limit the idle host trips would fail every wrapped criterion. That
    half needs a quiet host to begin with; a host still busy for any reason but I/O exits 3.

THE SPINNERS ARE NICED TO 19 AND THERE ARE FOUR OF THEM: two more than the two cores' worth the
check allows everyone else, so the verdict is not borderline, and niced so that they yield to any
real work on the machine rather than disturbing it. They are killed by their own PIDs.

EACH LEG IS ITS OWN CTest ENTRY (`--leg before|across|own|nested|io`), so that a leg being
unjudgeable on a busy host never hides another leg's verdict inside one skipped test.

Exit codes: 0 the leg held; 1 it failed; 3 (legs 2 to 5) it could not be evaluated.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SPINNERS = 4
SPIN = "while True:\n    pass\n"
NOT_EVALUATED = 3
#: What the wrapper prints, flushed, once the pre-run check passed and the command is starting.
RUN_STARTED = "=== running:"
#: Leg 3's command: four processes spinning for two seconds, then a chosen exit status.
OWN_LOAD = (
    "import multiprocessing, sys, time\n"
    "def spin(until):\n"
    "    while time.monotonic() < until:\n"
    "        pass\n"
    "until = time.monotonic() + 2.0\n"
    "workers = [multiprocessing.Process(target=spin, args=(until,)) for _ in range(4)]\n"
    "for worker in workers:\n"
    "    worker.start()\n"
    "for worker in workers:\n"
    "    worker.join()\n"
    "sys.exit(7)\n"
)
OWN_LOAD_EXIT = 7
#: Leg 4's command: leg 3's workers, each in a session of its own, as a nested wrapper's would be.
NESTED_LOAD = OWN_LOAD.replace("def spin(until):\n", "def spin(until):\n    os.setsid()\n").replace(
    "import multiprocessing, sys, time\n", "import multiprocessing, os, sys, time\n")


#: Leg 5's writer: 4 MiB written and fsynced at a time, into a file that wraps at 256 MiB so the
#: disk it takes is bounded, for at most 120 s whatever happens to the test. Every write is flushed
#: before the next, so once it is killed there is at most one chunk of dirty pages left to drain —
#: nothing that outlives the leg and lands on the suite after it, which is what the eighth close's
#: burst did.
#:
#: IT WRITES IN PULSES, two seconds on and three off, because that is what a writeback burst looks
#: like a second at a time (the flusher wakes every five seconds) and because a steady writer would
#: not tell a wrapper that settles on five quiet seconds from one that took the first quiet second
#: it saw: a pause, with the second of writeback that trails it, still holds a whole quiet second,
#: and none holds five.
WRITER = (
    "import os, sys, time\n"
    "chunk = os.urandom(4 << 20)\n"
    "until = time.monotonic() + 120.0\n"
    "fd = os.open(sys.argv[1], os.O_WRONLY | os.O_CREAT, 0o600)\n"
    "offset = 0\n"
    "while time.monotonic() < until:\n"
    "    pulse = time.monotonic() + 2.0\n"
    "    while time.monotonic() < pulse:\n"
    "        os.pwrite(fd, chunk, offset)\n"
    "        os.fsync(fd)\n"
    "        offset = (offset + len(chunk)) % (256 << 20)\n"
    "    time.sleep(3.0)\n"
)
#: What the wrapper's reason says when I/O pressure decided it.
IO_REASON = "host too busy: io pressure"


def start_writer(path: Path) -> subprocess.Popen:
    return subprocess.Popen([sys.executable, "-c", WRITER, str(path)])


def stop_writer(writer: subprocess.Popen) -> None:
    writer.kill()
    writer.wait()


def start_spinners() -> list[subprocess.Popen]:
    return [subprocess.Popen([sys.executable, "-c", SPIN], preexec_fn=lambda: os.nice(19))
            for _ in range(SPINNERS)]


def stop_spinners(spinners: list[subprocess.Popen]) -> None:
    for spinner in spinners:
        spinner.kill()
    for spinner in spinners:
        spinner.wait()


def leg_before_the_run(wrapper: str) -> list[str]:
    spinners = start_spinners()
    with tempfile.TemporaryDirectory(prefix="quiet-host-") as scratch:
        marker = Path(scratch) / "ran"
        try:
            time.sleep(0.5)
            run = subprocess.run([wrapper, "--wait-s", "2", "--", sys.executable, "-c",
                                  f"open({str(marker)!r}, 'w').write('ran')"],
                                 capture_output=True, text=True, timeout=120, check=False)
        finally:
            stop_spinners(spinners)
        ran = marker.exists()
    problems = []
    if run.returncode != 1:
        problems.append(f"exit {run.returncode}, expected 1")
    if "host too busy:" not in run.stderr:
        problems.append("no 'host too busy:' reason on stderr")
    if "was not run" not in run.stderr:
        problems.append("the pre-run check did not refuse the command")
    if ran or RUN_STARTED in run.stdout:
        problems.append("the command ran on a host the check was told was busy")
    if problems:
        sys.stdout.write(run.stdout[-3000:])
        sys.stderr.write(run.stderr[-3000:])
    return problems


def leg_across_the_run(wrapper: str) -> tuple[list[str], str]:
    """Problems found, or an empty list and a reason the leg could not be judged."""
    process = subprocess.Popen([wrapper, "--wait-s", "20", "--", sys.executable, "-c",
                                "import time; time.sleep(4)"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    spinners: list[subprocess.Popen] = []
    seen: list[str] = []
    try:
        assert process.stdout is not None
        for line in process.stdout:
            seen.append(line)
            if line.startswith(RUN_STARTED):
                spinners = start_spinners()
                break
        rest, errors = process.communicate(timeout=120)
    finally:
        stop_spinners(spinners)
        if process.poll() is None:
            process.kill()
            process.wait()
    output = "".join(seen) + rest
    if not spinners:
        if "host too busy:" in errors:
            return [], "the host was not quiet before the run, so the command never started"
        return [f"the command never started (exit {process.returncode})"], ""
    problems = []
    if process.returncode != 1:
        problems.append(f"exit {process.returncode}, expected 1")
    if "was not quiet while" not in errors:
        problems.append("the check across the run did not fail the run")
    if problems:
        sys.stdout.write(output[-3000:])
        sys.stderr.write(errors[-3000:])
    return problems, ""


def host_quiet_now(wrapper: str) -> bool:
    """Whether the wrapper's own pre-run check passes right now, around a command that does nothing."""
    probe = subprocess.run([wrapper, "--wait-s", "2", "--", "true"], capture_output=True, text=True,
                           timeout=60, check=False)
    return probe.returncode == 0


def leg_own_load(wrapper: str, load: str = OWN_LOAD) -> tuple[list[str], str]:
    """Problems found, or an empty list and a reason the leg could not be judged."""
    run = subprocess.run([wrapper, "--wait-s", "20", "--", sys.executable, "-c", load],
                         capture_output=True, text=True, timeout=120, check=False)
    if RUN_STARTED not in run.stdout:
        if "host too busy:" in run.stderr:
            return [], "the host was not quiet before the run, so the command never started"
        return [f"the command never started (exit {run.returncode})"], ""
    if "host too busy:" in run.stderr and not host_quiet_now(wrapper):
        # The host was quiet before the run and busy across it, and it is still busy now: something
        # else started while the command ran, and this leg cannot tell that from its own children
        # being counted. Said so, rather than reported as either verdict.
        return [], "the host became busy while the command ran, and is busy still"
    problems = []
    if run.returncode != OWN_LOAD_EXIT:
        problems.append(f"exit {run.returncode}, expected the command's own {OWN_LOAD_EXIT}")
    if "host too busy:" in run.stderr:
        problems.append("the command's own four spinning children were counted against the host")
    if "host quiet:" not in run.stdout.split(RUN_STARTED, 1)[1]:
        problems.append("no 'host quiet:' verdict across the run")
    if problems:
        sys.stdout.write(run.stdout[-3000:])
        sys.stderr.write(run.stderr[-3000:])
    return problems, ""


def leg_io_pressure(wrapper: str) -> tuple[list[str], str]:
    """Problems found, or an empty list and a reason the idle half could not be judged."""
    problems: list[str] = []
    with tempfile.TemporaryDirectory(prefix="quiet-host-io-") as scratch:
        marker = Path(scratch) / "ran"
        command = [sys.executable, "-c", f"open({str(marker)!r}, 'w').write('ran')"]
        writer = start_writer(Path(scratch) / "burst")
        try:
            time.sleep(1.0)
            # Eight seconds, so that the wait sees two pauses: a wrapper that settled on one quiet
            # second would take one of them and run the command.
            busy = subprocess.run([wrapper, "--wait-s", "8", "--", *command], capture_output=True,
                                  text=True, timeout=120, check=False)
        finally:
            stop_writer(writer)
        if busy.returncode != 1:
            problems.append(f"under the writer: exit {busy.returncode}, expected 1")
        if IO_REASON not in busy.stderr:
            problems.append(f"under the writer: no '{IO_REASON}' reason on stderr")
        if marker.exists() or RUN_STARTED in busy.stdout:
            problems.append("under the writer: the command ran on a host busy on I/O")
        if problems:
            sys.stdout.write(busy.stdout[-3000:])
            sys.stderr.write(busy.stderr[-3000:])
            return problems, ""
        idle = subprocess.run([wrapper, "--wait-s", "30", "--", *command], capture_output=True,
                              text=True, timeout=120, check=False)
        if idle.returncode == 0 and marker.exists():
            return [], ""
        if "host too busy:" in idle.stderr and IO_REASON not in idle.stderr:
            return [], ("the host was busy on CPU after the writer stopped, so the idle half "
                        "is unjudged")
        sys.stdout.write(idle.stdout[-3000:])
        sys.stderr.write(idle.stderr[-3000:])
        return [f"with the writer killed: exit {idle.returncode}, expected 0 and the command "
                f"run"], ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--wrapper", required=True, help="the cy_quiet_host binary")
    parser.add_argument("--leg", required=True, choices=("before", "across", "own", "nested", "io"),
                        help="which check to load the host against")
    arguments = parser.parse_args()

    if arguments.leg == "before":
        before = leg_before_the_run(arguments.wrapper)
        print(f"leg 1, before the run: {'FAILED: ' + '; '.join(before) if before else 'held'}")
        return 1 if before else 0

    legs = {"across": ("leg 2, across the run", leg_across_the_run),
            "own": ("leg 3, the command's own load", leg_own_load),
            "nested": ("leg 4, the command's own load in a nested session",
                       lambda wrapper: leg_own_load(wrapper, NESTED_LOAD)),
            "io": ("leg 5, I/O pressure before the run", leg_io_pressure)}
    label, leg = legs[arguments.leg]
    problems, unjudged = leg(arguments.wrapper)
    if unjudged:
        print(f"{label}: NOT EVALUATED: {unjudged}")
        return NOT_EVALUATED
    print(f"{label}: {'FAILED: ' + '; '.join(problems) if problems else 'held'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

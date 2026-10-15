#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The world sample's `--quiet-host` check, run against a host this test makes busy on purpose.

`m11a:world-budget-on-a-device` measures on a quiet host and says so: when the host is busy the run
FAILS with a "host too busy:" reason, and it neither passes nor skips. This test is what goes red if
that check is removed or stops looking.

  * LEG 1, BEFORE THE TAKE. Spinners are started first, then the sample. The check has to refuse
    to measure: exit 1, "host too busy:" and "The take was not measured" on stderr, and no take on
    stdout. Removing the pre-take check makes the take run, and this leg goes red. The world must
    not have been built either: the check precedes all work, so moving it back to just before the
    take turns this leg red too.
  * LEG 2, ACROSS THE TAKE. The sample starts on whatever host there is. Once it prints that the
    take has started, the spinners start. The check across the take has to fail the run with
    "was not quiet while the take was measured". Removing that check makes the run exit 0, and this
    leg goes red. This leg needs the pre-take check to PASS first, which needs a quiet host; on a
    busy one it is reported NOT EVALUATED with the reason, and the exit code says so (3). So is a
    run whose take passed while the spinners got no more cores than the check allows everyone
    else: the host was never made busy, so the pass does not say the check looked.

Both legs run a small world (`--regions 4`): what is under test is the check, not the frame, and the
check's verdict does not depend on how many regions were generated before it looked. Leg 2's take is
2400 frames, several seconds even on a small world on a quiet host (about 3 ms a frame), so that
the spinners it starts at the take's first line fill most of the window the check judges rather
than a sliver of it.

THE SPINNERS ARE NICED TO 19 AND THERE ARE FOUR OF THEM: two more than the two cores' worth the
check allows everyone else, so the verdict is not borderline, and niced so that they yield to any
real work on the machine rather than disturbing it. They are killed by their own PIDs.

EACH LEG IS ITS OWN CTest ENTRY (`--leg before`, `--leg across`), so that leg 2 being unjudgeable on
a busy host never hides leg 1's verdict inside one skipped test.

Exit codes: 0 the leg held; 1 it failed; 3 (leg 2 only) it could not be evaluated.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

SPINNERS = 4
SPIN = "while True:\n    pass\n"
NOT_EVALUATED = 3
#: What the sample prints once its world is built (`main.cpp`, after `world.build`).
WORLD_BUILT = "to generate, cook, claim and place"
#: The cores everyone else may use before the check calls the host busy: `max_other_cores` in
#: tools/quiet-host/host_load.h, which the sample's check uses unchanged.
OTHERS_LIMIT_CORES = 2.0


def sample_argv(sample: str, seconds: int, wait_s: int) -> list[str]:
    """A headless take of `seconds` at 8 fps on a 4x4 world, behind the quiet-host check."""
    return [sample, "--headless", "--regions", "4", "--seconds", str(seconds), "--fps", "8",
            "--quiet-host", "--quiet-wait-s", str(wait_s)]


def start_spinners() -> list[subprocess.Popen]:
    return [subprocess.Popen([sys.executable, "-c", SPIN], preexec_fn=lambda: os.nice(19))
            for _ in range(SPINNERS)]


def spinner_cpu_seconds(spinners: list[subprocess.Popen]) -> float:
    """The CPU the spinners were given, read from /proc before they are killed."""
    ticks = 0
    for spinner in spinners:
        try:
            with open(f"/proc/{spinner.pid}/stat", encoding="ascii") as stat:
                # Fields 14 and 15, user and system time; the name in field 2 may hold spaces, so
                # count from the closing parenthesis.
                fields = stat.read().rsplit(")", 1)[1].split()
            ticks += int(fields[11]) + int(fields[12])
        except (OSError, IndexError, ValueError):
            continue
    return ticks / os.sysconf("SC_CLK_TCK")


def stop_spinners(spinners: list[subprocess.Popen]) -> None:
    for spinner in spinners:
        spinner.kill()
    for spinner in spinners:
        spinner.wait()


def leg_before_the_take(sample: str) -> list[str]:
    spinners = start_spinners()
    try:
        time.sleep(0.5)
        run = subprocess.run(sample_argv(sample, seconds=1, wait_s=2), capture_output=True,
                             text=True, timeout=240, check=False)
    finally:
        stop_spinners(spinners)
    problems = []
    if run.returncode != 1:
        problems.append(f"exit {run.returncode}, expected 1")
    if "host too busy:" not in run.stderr:
        problems.append("no 'host too busy:' reason on stderr")
    if "The take was not measured" not in run.stderr:
        problems.append("the pre-take check did not refuse the take")
    if "=== the take:" in run.stdout:
        problems.append("the take ran on a host the check was told was busy")
    # THE CHECK COMES BEFORE ANY WORK. Placed just before the take, its idle second let the workers
    # park and the caches go cold, and frame 0 cost about twice what it costs without the check on
    # the same quiet host. A refusal after the world was built is that ordering come back.
    if WORLD_BUILT in run.stdout:
        problems.append("the world was built before the host was judged; the check must precede "
                        "all work, so that nothing idles between the work and the take")
    if problems:
        sys.stdout.write(run.stdout[-3000:])
        sys.stderr.write(run.stderr[-3000:])
    return problems


def leg_across_the_take(sample: str) -> tuple[list[str], str]:
    """Problems found, or an empty list and a reason the leg could not be judged."""
    process = subprocess.Popen(sample_argv(sample, seconds=300, wait_s=20), stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    spinners: list[subprocess.Popen] = []
    seen: list[str] = []
    loaded_s = 0.0
    spinner_s = 0.0
    try:
        assert process.stdout is not None
        for line in process.stdout:
            seen.append(line)
            if line.startswith("=== the take:"):
                spinners = start_spinners()
                started = time.monotonic()
                break
        rest, errors = process.communicate(timeout=240)
        if spinners:
            loaded_s = time.monotonic() - started
            spinner_s = spinner_cpu_seconds(spinners)
    finally:
        stop_spinners(spinners)
        if process.poll() is None:
            process.kill()
            process.wait()
    output = "".join(seen) + rest
    if not spinners:
        if "host too busy:" in errors:
            return [], "the host was not quiet before the take, so the take never started"
        return [f"the take never started (exit {process.returncode})"], ""
    # THE PREMISE, MEASURED: the spinners must have made the host busy. They are niced to 19, so
    # they only get the cores the sample leaves idle, and a sample that saturates every core itself
    # — an instrumented build under ThreadSanitizer is five to fifteen times heavier — starves them
    # below the limit they are there to exceed. The check then rightly calls the host quiet, and
    # its passing says nothing about whether it looks. A check that FAILED the run is judged
    # whatever the spinners got.
    spinner_cores = spinner_s / loaded_s if loaded_s > 0.0 else 0.0
    if process.returncode == 0 and spinner_cores <= OTHERS_LIMIT_CORES:
        return [], (f"the take itself left the niced spinners {spinner_cores:.2f} cores, not the "
                    f"more than {OTHERS_LIMIT_CORES:.2f} that make the host busy")
    problems = []
    if process.returncode != 1:
        problems.append(f"exit {process.returncode}, expected 1")
    if "was not quiet while the take was measured" not in errors:
        problems.append("the check across the take did not fail the run")
    if problems:
        sys.stdout.write(output[-3000:])
        sys.stderr.write(errors[-3000:])
    return problems, ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sample", required=True, help="the cy_sample_world binary")
    parser.add_argument("--leg", required=True, choices=("before", "across"),
                        help="which of the two checks to load the host against")
    arguments = parser.parse_args()

    if arguments.leg == "before":
        before = leg_before_the_take(arguments.sample)
        print(f"leg 1, before the take: {'FAILED: ' + '; '.join(before) if before else 'held'}")
        return 1 if before else 0

    across, unjudged = leg_across_the_take(arguments.sample)
    if unjudged:
        print(f"leg 2, across the take: NOT EVALUATED: {unjudged}")
        return NOT_EVALUATED
    print(f"leg 2, across the take: {'FAILED: ' + '; '.join(across) if across else 'held'}")
    return 1 if across else 0


if __name__ == "__main__":
    sys.exit(main())

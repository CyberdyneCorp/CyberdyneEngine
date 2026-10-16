#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The world sample's budget CSV keeps the shape its readers depend on, on a machine with no device.

`m10:world-frame-budget` runs the sample headless with `--budget` and reads EVERY column whose name
ends `_ms` as a processor band, summing it over the frames. A column of that name that is blank on a
frame — the device's own times, which nothing measures headless — made the criterion fail with
`could not convert string to float: ''` on every machine that runs it without a device (nightly run
38833426854). This runs the same take, short, and requires:

  * every cell of every `_ms` column to be a number, on every frame;
  * `frame_ms` to be there, because it is the column the criterion judges;
  * the device's columns to be there under their own names, blank when nothing timed them, so the
    distinction between "not measured" and "zero" is kept rather than papered over.
"""

from __future__ import annotations

import argparse
import csv
import subprocess
import sys
import tempfile
from pathlib import Path

DEVICE_COLUMNS = ("gpu_milliseconds", "water_gpu_milliseconds")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sample", required=True)
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cy-world-budget-") as scratch:
        budget = Path(scratch) / "budget.csv"
        run = subprocess.run([arguments.sample, "--headless", "--regions", "4", "--seconds", "2",
                              "--fps", "8", "--seed", "0x5EED", "--budget", str(budget)],
                             capture_output=True, text=True, check=False)
        if run.returncode != 0:
            print(f"the take failed (exit {run.returncode}):\n{run.stdout[-2000:]}{run.stderr[-2000:]}")
            return 1
        rows = list(csv.DictReader(budget.open(newline="", encoding="utf-8")))
    if not rows:
        print("the budget CSV has no frames")
        return 1
    problems = []
    columns = list(rows[0])
    if "frame_ms" not in columns:
        problems.append(f"no `frame_ms` column: {columns}")
    for name in DEVICE_COLUMNS:
        if name not in columns:
            problems.append(f"the device column `{name}` is missing: {columns}")
    for index, row in enumerate(rows):
        for name in (column for column in columns if column.endswith("_ms")):
            try:
                float(row[name])
            except ValueError:
                problems.append(f"frame {index}: `{name}` is {row[name]!r}, not a number — a "
                                "column ending `_ms` is a processor band every reader sums")
    for problem in problems[:20]:
        print(problem)
    if problems:
        return 1
    print(f"{len(rows)} frames, {sum(name.endswith('_ms') for name in columns)} processor bands, "
          "every cell a number")
    return 0


if __name__ == "__main__":
    sys.exit(main())

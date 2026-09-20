#!/usr/bin/env python3
"""Install and smoke-test the iOS Metal sample on an already booted simulator."""

from __future__ import annotations

import argparse
import os
import subprocess
import time
from pathlib import Path

from run_device import parse_measurement

if Path("/Applications/Xcode.app/Contents/Developer").is_dir():
    os.environ.setdefault("DEVELOPER_DIR", "/Applications/Xcode.app/Contents/Developer")


def run(*arguments: str, capture: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["xcrun", "simctl", *arguments],
        check=True,
        text=True,
        capture_output=capture,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default="booted")
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--bundle-id", default="com.cyberdyne.engine.ship.simulator")
    parser.add_argument("--screenshot", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()

    if not args.app.is_dir():
        parser.error(f"app bundle does not exist: {args.app}")
    args.screenshot.parent.mkdir(parents=True, exist_ok=True)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    stderr_log = args.log.with_suffix(".stderr.log")
    args.log.unlink(missing_ok=True)
    stderr_log.unlink(missing_ok=True)

    run("install", args.device, str(args.app))
    subprocess.run(
        ["xcrun", "simctl", "terminate", args.device, args.bundle_id],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    launched = run(
        "launch",
        "--terminate-running-process",
        f"--stdout={args.log.resolve()}",
        f"--stderr={stderr_log.resolve()}",
        args.device,
        args.bundle_id,
        capture=True,
    )
    time.sleep(5)
    services = run("spawn", args.device, "launchctl", "list", capture=True).stdout
    process_label = f"UIKitApplication:{args.bundle_id}"
    if process_label not in services:
        raise RuntimeError(
            f"{args.bundle_id} exited after launch; simulator smoke test failed"
        )
    lines = []
    for path in (args.log, stderr_log):
        if path.is_file():
            lines.extend(path.read_text(encoding="utf-8", errors="replace").splitlines())
    measurement = parse_measurement(lines, 3)
    run("io", args.device, "screenshot", str(args.screenshot))
    if args.screenshot.stat().st_size < 10_000:
        raise RuntimeError("simulator screenshot is missing or implausibly small")
    print(
        f"iOS simulator PASS: {launched.stdout.strip()}, "
        f"{len(measurement.fps)} Metal frames-per-second samples"
    )
    print(f"log: {args.log}")
    print(f"screenshot: {args.screenshot}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Install, run, measure, and capture the iOS Metal sample on physical hardware."""

from __future__ import annotations

import argparse
import json
import os
import queue
import re
import statistics
import subprocess
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

if Path("/Applications/Xcode.app/Contents/Developer").is_dir():
    os.environ.setdefault("DEVELOPER_DIR", "/Applications/Xcode.app/Contents/Developer")


CONTRACT = (
    "CY_IOS_CONTRACT platform=ios window=single desktop_ops=unsupported "
    "surface=metal touch_events=4"
)
READY = re.compile(r"CY_IOS_READY backend=(\S+) device=(.+) size=(\d+)x(\d+)")
FPS = re.compile(r"CY_IOS_FPS fps=([0-9]+(?:\.[0-9]+)?) frames=(\d+) seconds=([0-9]+(?:\.[0-9]+)?)")


@dataclass(frozen=True)
class Device:
    selector: str
    name: str
    model: str
    os_version: str
    udid: str


@dataclass(frozen=True)
class Measurement:
    backend: str
    gpu: str
    width: int
    height: int
    fps: tuple[float, ...]


def _nested(value: dict[str, Any], *keys: str, default: Any = "") -> Any:
    current: Any = value
    for key in keys:
        if not isinstance(current, dict):
            return default
        current = current.get(key, default)
    return current


def select_physical_iphone(document: dict[str, Any], requested: str) -> Device:
    """Find the requested connected physical iPhone and reject simulators loudly."""
    candidates = _nested(document, "result", "devices", default=[])
    for candidate in candidates:
        hardware = _nested(candidate, "properties", "hardware", default={})
        state = _nested(candidate, "properties", "state", default={})
        connection = _nested(candidate, "properties", "connection", default={})
        legacy_hardware = candidate.get("hardwareProperties", {})
        legacy_device = candidate.get("deviceProperties", {})
        udid = str(hardware.get("udid") or legacy_hardware.get("udid") or "")
        name = str(state.get("name") or legacy_device.get("name") or "")
        aliases = {str(candidate.get("identifier", "")), udid, name}
        if requested not in aliases:
            continue
        reality = str(hardware.get("reality") or legacy_hardware.get("reality") or "")
        platform = str(hardware.get("platform") or legacy_hardware.get("platform") or "")
        connected = str(connection.get("state") or "")
        if reality != "physical" or platform != "iOS":
            raise RuntimeError(f"{requested!r} is {reality or 'unknown'} {platform or 'device'}, not a physical iPhone")
        if connected != "connected":
            raise RuntimeError(f"{name or requested} is paired but {connected or 'not connected'}; unlock and connect it")
        software = _nested(candidate, "properties", "software", "osVersionNumber", default={})
        os_version = str(software.get("stringValue") or legacy_device.get("osVersionNumber") or "unknown")
        model = str(hardware.get("marketingName") or legacy_hardware.get("marketingName") or "iPhone")
        return Device(requested, name, model, os_version, udid)
    raise RuntimeError(f"no device matches {requested!r}; check `xcrun devicectl list devices`")


def parse_measurement(lines: list[str], minimum_samples: int) -> Measurement:
    """Require the full platform contract, native Metal selection, and enough measured frames."""
    if not any(CONTRACT in line for line in lines):
        raise RuntimeError("the app never emitted the iOS platform contract marker")
    ready = next((match for line in lines if (match := READY.search(line))), None)
    if ready is None:
        raise RuntimeError("the app never emitted CY_IOS_READY after creating its Metal swapchain")
    if ready.group(1) != "metal":
        raise RuntimeError(f"the app selected {ready.group(1)!r}, expected the native Metal backend")
    samples = tuple(float(match.group(1)) for line in lines if (match := FPS.search(line)))
    if len(samples) < minimum_samples:
        raise RuntimeError(f"only {len(samples)} FPS samples arrived; expected at least {minimum_samples}")
    if any(sample <= 0.0 for sample in samples):
        raise RuntimeError("an FPS sample was zero; frames were not continuously presented")
    return Measurement(
        backend=ready.group(1),
        gpu=ready.group(2),
        width=int(ready.group(3)),
        height=int(ready.group(4)),
        fps=samples,
    )


def devicectl(*arguments: str, capture: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["xcrun", "devicectl", *arguments],
        check=True,
        text=True,
        capture_output=capture,
    )


def discover(requested: str) -> Device:
    listed = devicectl("list", "devices", "--json-output", "-", capture=True)
    return select_physical_iphone(json.loads(listed.stdout), requested)


def measure(
    device: Device,
    bundle_id: str,
    seconds: float,
    minimum_samples: int,
    screenshot: Path,
    log: Path,
) -> Measurement:
    command = [
        "xcrun", "devicectl", "device", "process", "launch",
        "--device", device.selector,
        "--console", "--terminate-existing", bundle_id,
    ]
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    received: queue.Queue[Optional[str]] = queue.Queue()
    lines: list[str] = []

    def read_console() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            received.put(line.rstrip())
        received.put(None)

    reader = threading.Thread(target=read_console, daemon=True)
    reader.start()
    deadline = time.monotonic() + seconds
    try:
        while time.monotonic() < deadline:
            try:
                line = received.get(timeout=min(0.25, max(0.01, deadline - time.monotonic())))
            except queue.Empty:
                if process.poll() is not None:
                    break
                continue
            if line is None:
                break
            print(line, flush=True)
            lines.append(line)
        while True:
            try:
                line = received.get_nowait()
            except queue.Empty:
                break
            if line is not None:
                print(line, flush=True)
                lines.append(line)
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text("\n".join(lines) + "\n", encoding="utf-8")
        measurement = parse_measurement(lines, minimum_samples)
        if process.poll() is not None:
            raise RuntimeError("the app exited before its physical display could be captured")
        screenshot.parent.mkdir(parents=True, exist_ok=True)
        devicectl(
            "device", "capture", "screenshot", "--device", device.selector,
            "--destination", str(screenshot.resolve()),
        )
        if not screenshot.is_file() or screenshot.stat().st_size < 10_000:
            raise RuntimeError("physical-device screenshot is missing or implausibly small")
        return measurement
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        reader.join(timeout=1)


def write_evidence(path: Path, screenshot: Path, device: Device, measurement: Measurement) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    relative_screenshot = os.path.relpath(screenshot, path.parent)
    samples = ", ".join(f"{sample:.2f}" for sample in measurement.fps)
    path.write_text(
        "\n".join(
            [
                "# Physical iPhone Metal validation",
                "",
                f"- Captured: {datetime.now(timezone.utc).isoformat(timespec='seconds')}",
                f"- Hardware: {device.model}",
                f"- OS: iOS {device.os_version}",
                f"- Backend: `{measurement.backend}`",
                f"- GPU reported by RHI: `{measurement.gpu}`",
                f"- Drawable: {measurement.width} × {measurement.height}",
                f"- FPS samples: {samples}",
                f"- Median FPS: {statistics.median(measurement.fps):.2f}",
                f"- Range: {min(measurement.fps):.2f}–{max(measurement.fps):.2f} FPS",
                "- Contract: platform `ios`, one fullscreen window, desktop operations rejected, "
                "Metal surface, four touch events",
                "",
                f"![Open-world terrain and day/night cycle on {device.model}]({relative_screenshot})",
                "",
            ]
        ),
        encoding="utf-8",
    )


def self_test() -> int:
    physical = {
        "result": {"devices": [{
            "identifier": "core-device-id",
            "properties": {
                "connection": {"state": "connected"},
                "hardware": {"reality": "physical", "platform": "iOS", "udid": "PHONE", "marketingName": "iPhone 16"},
                "software": {"osVersionNumber": {"stringValue": "27.0"}},
                "state": {"name": "Test iPhone"},
            },
        }]},
    }
    assert select_physical_iphone(physical, "PHONE").model == "iPhone 16"
    simulated = json.loads(json.dumps(physical))
    simulated["result"]["devices"][0]["properties"]["hardware"]["reality"] = "simulated"
    try:
        select_physical_iphone(simulated, "PHONE")
        raise AssertionError("a simulator was accepted as hardware evidence")
    except RuntimeError as error:
        assert "not a physical iPhone" in str(error)
    good = [
        CONTRACT,
        "CY_IOS_READY backend=metal device=Apple A18 GPU size=2556x1179",
        "CY_IOS_FPS fps=59.80 frames=60 seconds=1.003",
        "CY_IOS_FPS fps=60.00 frames=60 seconds=1.000",
        "CY_IOS_FPS fps=59.90 frames=60 seconds=1.002",
    ]
    assert parse_measurement(good, 3).fps == (59.8, 60.0, 59.9)
    for broken in (good[1:], [*good[:2], good[2]], [line.replace("backend=metal", "backend=null") for line in good]):
        try:
            parse_measurement(broken, 3)
            raise AssertionError("invalid device evidence was accepted")
        except RuntimeError:
            pass
    print("iOS physical-device runner self-test PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--device", help="physical iPhone name, CoreDevice identifier, or UDID")
    parser.add_argument("--app", type=Path)
    parser.add_argument("--bundle-id")
    parser.add_argument("--seconds", type=float, default=8.0)
    parser.add_argument("--minimum-samples", type=int, default=3)
    parser.add_argument("--screenshot", type=Path)
    parser.add_argument("--log", type=Path)
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    required = ("device", "app", "bundle_id", "screenshot", "log", "evidence")
    missing = [name for name in required if getattr(args, name) is None]
    if missing:
        parser.error("required for a device run: " + ", ".join(f"--{name.replace('_', '-')}" for name in missing))
    if args.seconds < 3.0 or args.minimum_samples < 1:
        parser.error("--seconds must be at least 3 and --minimum-samples must be positive")
    if not args.app.is_dir():
        parser.error(f"app bundle does not exist: {args.app}")

    device = discover(args.device)
    print(f"physical device: {device.name} — {device.model}, iOS {device.os_version}")
    devicectl("device", "install", "app", "--device", args.device, str(args.app))
    measurement = measure(
        device,
        args.bundle_id,
        args.seconds,
        args.minimum_samples,
        args.screenshot,
        args.log,
    )
    write_evidence(args.evidence, args.screenshot, device, measurement)
    print(f"iOS physical-device PASS: median {statistics.median(measurement.fps):.2f} FPS")
    print(f"screenshot: {args.screenshot}")
    print(f"evidence: {args.evidence}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
